#include <nba/engines/knapp/defs.hh>
#include <nba/engines/knapp/mictypes.hh>
#include <nba/engines/knapp/sharedtypes.hh>
#include <nba/engines/knapp/sharedutils.hh>
#include <nba/engines/knapp/micbarrier.hh>
#include <nba/engines/knapp/micutils.hh>
#include <nba/engines/knapp/ctrl.pb.h>
#include <nba/engines/knapp/pollring.hh>
#include <nba/engines/knapp/rma.hh>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <locale.h>

namespace nba { namespace knapp {

extern char **kernel_paths;
extern char **kernel_names;
std::vector<struct nba::knapp::vdevice *> vdevs;
extern worker_func_t worker_funcs[];

/* MIC daemon consists of 3 types of threads:
 * (1) control_thread_loop: global state mgmt (e.g., "cudaMalloc", "cudaMmecpy")
 * (2) master_thread_loop: vDevice controller
 * (3) worker_thread_loop: vDevice worker
 *
 * Each vDevice has at least one master and zero or more workers.
 */

static uint32_t mic_num_pcores = 0;
static uint32_t mic_num_lcores = 0;
static uint64_t global_vdevice_counter = 0;
static bool pcore_used[KNAPP_NUM_CORES];
static pthread_t control_thread;
static volatile bool exit_flag = false;

void *control_thread_loop(void *arg);
void *master_thread_loop(void *arg);
void *worker_thread_loop(void *arg);
void stop_all();
void handle_signal(int signal);

struct vdevice *create_vdev(
        uint32_t num_pcores,
        uint32_t num_lcores_per_pcore,
        uint32_t pipeline_depth,
        pthread_barrier_t *ready_barrier);
void destroy_vdev(struct vdevice *vdev);

bool create_pollring(
        struct vdevice *vdev, uint32_t ring_id,
        size_t len, off_t peer_ra);

bool destroy_pollring(
        struct vdevice *vdev, uint32_t ring_id);

bool create_rma(
        struct vdevice *vdev, uint32_t buffer_id,
        size_t size, off_t peer_ra);

bool destroy_rma(
        struct vdevice *vdev, uint32_t buffer_id);

}} // endns(nba::knapp)

using namespace nba;
using namespace nba::knapp;

static struct vdevice *nba::knapp::create_vdev(
        uint32_t num_pcores,
        uint32_t num_lcores_per_pcore,
        uint32_t pipeline_depth,
        pthread_barrier_t *ready_barrier)
{
    int rc;
    bool avail = true;
    uint32_t pcore_begin = 0;
    struct vdevice *vdev = nullptr;

    /* Find available slots and allocate among MIC cores. */
    for (uint32_t i = 0; i < mic_num_pcores - num_pcores; i++) {
        avail = true;
        for (uint32_t j = 0; j < num_pcores; j++) {
            if (pcore_used[i + j]) {
                avail = false;
                break;
            }
        }
        if (avail) {
            pcore_begin = i;
            break;
        }
    }
    if (avail) {
        vdev = new struct vdevice();
        vdev->pcores.clear();
        vdev->lcores.clear();
        for (uint32_t i = 0; i < num_pcores; i++) {
            pcore_used[pcore_begin + i] = true;
            vdev->pcores.push_back(pcore_begin + i);
            for (uint32_t j = 0; j < num_lcores_per_pcore; j++) {
                vdev->lcores.push_back(mic_pcore_to_lcore(pcore_begin + i, j));
            }
        }
    } else {
        return nullptr;
    }
    vdev->device_id = (++global_vdevice_counter);
    vdev->pipeline_depth = pipeline_depth;
    vdev->ht_per_core = num_lcores_per_pcore;
    vdev->num_worker_threads = vdev->pcores.size() * num_lcores_per_pcore;
    vdev->master_core = pcore_begin;
    vdev->threads_alive = false;
    log_device(vdev->device_id, "created (pcore_begin=%d, num_workers=%d)\n", pcore_begin, vdev->num_worker_threads);

    /* Initialize barriers. */
    vdev->data_ready_barriers = (Barrier **) _mm_malloc(sizeof(Barrier *) * vdev->pipeline_depth, CACHE_LINE_SIZE);
    vdev->task_done_barriers  = (Barrier **) _mm_malloc(sizeof(Barrier *) * vdev->pipeline_depth, CACHE_LINE_SIZE);
    for (uint32_t i = 0; i < vdev->pipeline_depth; i++) {
        vdev->data_ready_barriers[i] = new Barrier(vdev->num_worker_threads, vdev->device_id, KNAPP_BARRIER_PROFILE_INTERVAL);
        vdev->task_done_barriers[i]  = new Barrier(vdev->num_worker_threads, vdev->device_id, KNAPP_BARRIER_PROFILE_INTERVAL);
    }

    /* Spawn master/worker threads. */
    // TODO: attr
    vdev->threads_alive = true;
    vdev->ready_barrier = ready_barrier;
    rc = pthread_create(&vdev->master_thread, nullptr, master_thread_loop, (void *) vdev);
    assert(0 == rc);
    log_device(vdev->device_id, "spawned master thread.\n");

    return vdev;
}

static void nba::knapp::destroy_vdev(struct vdevice *vdev)
{
    /* Destroy master and worker threads. */
    vdev->exit = true;
    if (vdev->threads_alive) {
        pthread_kill(vdev->master_thread, SIGINT);
        //for (int i = 0; i < vdev->num_worker_threads; i++)
        //    pthread_kill(vdev->worker_threads[i], SIGINT);
        pthread_join(vdev->master_thread, nullptr);
        //for (int i = 0; i < vdev->num_worker_threads; i++)
        //    pthread_join(vdev->worker_threads[i], nullptr);
        vdev->threads_alive = false;
    }

    for (uint32_t i = 0; i < vdev->pipeline_depth; i++) {
        delete vdev->data_ready_barriers[i];
        delete vdev->task_done_barriers[i];
    }
    _mm_free(vdev->data_ready_barriers);
    _mm_free(vdev->task_done_barriers);
    delete vdev;
}

static bool nba::knapp::create_pollring(
        struct vdevice *vdev, uint32_t ring_id,
        size_t len, off_t peer_ra)
{
    PollRing *r = new PollRing(vdev->data_epd, len);
    log_device(vdev->device_id, "Creating PollRing[%u] "
               "(length %u, ra %p, peer_ra %p).\n",
               ring_id, len, r->ra(), peer_ra);
    r->set_peer_ra(peer_ra);
    vdev->poll_rings[ring_id] = r;
    return true;
}

static bool nba::knapp::destroy_pollring(
        struct vdevice *vdev, uint32_t ring_id)
{
    delete vdev->poll_rings[ring_id];
    vdev->poll_rings[ring_id] = nullptr;
    return true;
}

static bool nba::knapp::create_rma(
        struct vdevice *vdev, uint32_t buffer_id,
        size_t size, off_t peer_ra)
{
    RMABuffer *b = new RMABuffer(vdev->data_epd, size);
    log_device(vdev->device_id, "Creating RMABuffer[%u] "
               "(size %'u bytes, ra %p, peer_ra %p).\n",
               buffer_id, size, b->ra(), peer_ra);
    b->set_peer_ra(peer_ra);
    vdev->rma_buffers[buffer_id] = b;
    return true;
}

static bool nba::knapp::destroy_rma(
        struct vdevice *vdev, uint32_t buffer_id)
{
    delete vdev->rma_buffers[buffer_id];
    vdev->rma_buffers[buffer_id] = nullptr;
    return true;
}

static void *nba::knapp::worker_thread_loop(void *arg)
{
    struct worker_thread_info *info = (struct worker_thread_info *) arg;
    int tid = info->thread_id;
    struct vdevice *vdev = info->vdev;

    // TODO: retrieve from per-task info
    //worker_func_t pktproc_func = info->pktproc_func;

    while (!vdev->exit) {
        /* former worker_preproc() */
        {
            struct worker *w =
                    &vdev->per_thread_work_info[vdev->next_task_id][tid];
            if (tid != 0) {
                w->data_ready_barrier->here(tid);
            }
            if (tid == 0) {
                uint32_t task_id = vdev->next_task_id;
                vdev->cur_task_id = task_id;

                vdev->poll_rings[0]->wait(task_id, KNAPP_TASK_READY);

                /* init latency/stat measurement */

                vdev->poll_rings[0]->notify(task_id, KNAPP_COPY_PENDING);

                w->data_ready_barrier->here(0);
                vdev->next_task_id = (task_id + 1) % vdev->poll_rings[0]->len();
            }
        }

        uint32_t task_id = vdev->cur_task_id;
        struct worker *w = &vdev->per_thread_work_info[task_id][tid];

        //TODO: pktproc_func(w);

        w->task_done_barrier->here(tid);

        if (tid == 0) { /* former worker_postproc() */

            /* finalize latency/stat measurement */

            //TODO: vdev->output_rma->write();

            vdev->poll_rings[0]->remote_notify(task_id, KNAPP_OFFLOAD_COMPLETE);
        }
    }
    return nullptr;
}

static void *nba::knapp::master_thread_loop(void *arg)
{
    struct vdevice *vdev = (struct vdevice *) arg;

    int backlog = 1;
    int rc = 0;
    uint16_t data_port = get_mic_data_port(vdev->device_id);

    log_device(vdev->device_id, "Opening data channel (port %u)\n", data_port);
    vdev->data_listen_epd = scif_open();
    assert(SCIF_OPEN_FAILED != vdev->data_listen_epd);
    rc = scif_bind(vdev->data_listen_epd, data_port);
    assert(data_port == (uint16_t) rc);
    rc = scif_listen(vdev->data_listen_epd, backlog);
    assert(0 == rc);

    pthread_barrier_wait(vdev->ready_barrier);
    vdev->ready_barrier = nullptr;

    struct scif_portID temp;
    rc = scif_accept(vdev->data_listen_epd, &temp,
                     &vdev->data_epd, SCIF_ACCEPT_SYNC);
    assert(0 == rc);
    log_device(vdev->device_id, "Connection established between "
               "local dataport (%d, %d) and remote dataport (%d, %d)\n",
               1, data_port,
               temp.node, temp.port);

#if 0
    /* Initialize worker thread info. */
    vdev->worker_threads = (pthread_t *) _mm_malloc(
            sizeof(pthread_t) * vdev->num_worker_threads,
            CACHE_LINE_SIZE);
    assert(nullptr != vdev->worker_threads);
    vdev->thread_info_array = (struct worker_thread_info *) _mm_malloc(
            sizeof(struct worker_thread_info) * vdev->num_worker_threads,
            CACHE_LINE_SIZE);
    assert(nullptr != vdev->thread_info_array);
    for (unsigned i = 0; i < vdev->num_worker_threads; i++) {
        struct worker_thread_info *info = &vdev->thread_info_array[i];
        info->thread_id = i;
        info->vdev = vdev;
    }

    /* Initialize pipelined worker info. */
    vdev->per_thread_work_info = (struct worker **) _mm_malloc(
            sizeof(struct worker *) * vdev->pipeline_depth,
            CACHE_LINE_SIZE);
    assert(nullptr != vdev->per_thread_work_info);
    for (unsigned pd = 0; pd < vdev->pipeline_depth; pd++) {
        vdev->per_thread_work_info[pd] = (struct worker *) _mm_malloc(
                sizeof(struct worker) * vdev->num_worker_threads,
                CACHE_LINE_SIZE);
        assert(nullptr != vdev->per_thread_work_info[pd]);
    }
    log_device(vdev->device_id, "Allocated %d per-worker-thread work info\n", vdev->num_worker_threads);
    for (unsigned pd = 0; pd < vdev->pipeline_depth; pd++) {
        for (unsigned i = 0; i < vdev->num_worker_threads; i++) {
            struct worker &w = vdev->per_thread_work_info[pd][i];
            w.data_ready_barrier = vdev->data_ready_barriers[pd];
            w.task_done_barrier  = vdev->task_done_barriers[pd];
        }
    }

    /* Spawn worker threads for this vDevice. */
    for ( unsigned i = 0; i < vdev->lcores.size(); i++ ) {
        //log_device(vdev->device_id, "Creating thread for lcore %d (%d, %d) and thread %d\n", lcore, pcore, ht, i);
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
        set_cpu_mask(&attr, vdev->lcores[i], mic_num_lcores);
        rc = pthread_create(&vdev->worker_threads[i], &attr,
                            worker_thread_loop,
                            (void *) &vdev->thread_info_array[i]);
        assert(0 == rc);
    }
#endif
    log_device(vdev->device_id, "Running processing daemon...\n");
    uint32_t cur_task_id = 0;
    while (!vdev->exit) {
#if 0
        vdev->poll_rings[0]->wait(cur_task_id, KNAPP_OFFLOAD_COMPLETE);

        // TODO: read taskitem from vdev->input_rma.
        //uint8_t *inputbuf_va = bufarray_get_va(&vdev->inputbuf_array, cur_task_id);
        struct taskitem *ti = (struct taskitem *) nullptr;

        if (ti->task_id != cur_task_id) {
            log_device(vdev->device_id, "offloaded task id (%d) doesn't match pending id (%d)\n", ti->task_id, cur_task_id);
            exit(1);
        }

        /* Split the input items for each worker thread. */
        uint8_t *inputbuf_payload_va = (uint8_t *)(ti + 1);
        int32_t remaining = ti->num_items;
        for (int i = 0; i < vdev->num_worker_threads; i++) {
            struct worker *w = &vdev->per_thread_work_info[cur_task_id][i];
            w->num_items = MIN(remaining, w->max_num_items);
            remaining -= w->num_items;
        }

        /* Now we are ready to run the processing function. */
        vdev->poll_rings[0]->notify(cur_task_id, KNAPP_TASK_READY);

        /* If we are terminating, inform worker threads as well. */
        if (vdev->exit == true) {
            for ( int i = 0; i < vdev->num_worker_threads; i++ ) {
                for ( int pdepth = 0; pdepth < vdev->pipeline_depth; pdepth++ ) {
                    vdev->per_thread_work_info[pdepth][i].exit = true;
                }
            }
            break;
        }

        cur_task_id = (cur_task_id + 1) % (vdev->poll_rings[0]->len());
#endif
        insert_pause();
    }

    log_device(vdev->device_id, "Terminating master thread.\n");
    scif_close(vdev->data_epd);
    scif_close(vdev->data_listen_epd);
    return nullptr;
}

static void *nba::knapp::control_thread_loop(void *arg)
{
    scif_epd_t master_listen_epd, master_epd;
    struct scif_portID accepted_master_port;
    int backlog = 1;
    int rc = 0;
    sigset_t intr_mask, orig_mask;
    sigemptyset(&intr_mask);
    sigaddset(&intr_mask, SIGINT);
    sigaddset(&intr_mask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &intr_mask, &orig_mask);

    uint16_t scif_nodes[32];
    uint16_t local_node;
    size_t num_nodes;
    num_nodes = scif_get_nodeIDs(scif_nodes, 32, &local_node);

    master_listen_epd = scif_open();
    rc = scif_bind(master_listen_epd, KNAPP_MASTER_PORT);
    assert(KNAPP_MASTER_PORT == rc);
    rc = scif_listen(master_listen_epd, backlog);
    assert(0 == rc);

    log_info("Starting the control channel...\n");
    /* For simplicty, we allow only a single concurrent connection. */
    while (!exit_flag) {
        struct pollfd p = {master_listen_epd, POLLIN, 0};
        rc = ppoll(&p, 1, nullptr, &orig_mask);
        if (rc == -1 && errno == EINTR && exit_flag)
            break;
        rc = scif_accept(master_listen_epd, &accepted_master_port,
                         &master_epd, SCIF_ACCEPT_SYNC);
        assert(0 == rc);

        log_info("A control session started.\n");
        CtrlRequest request;
        CtrlResponse resp;

        while (!exit_flag) {
            resp.Clear();
            if (!recv_ctrlmsg(master_epd, request, &orig_mask))
                // usually, EINTR or ECONNRESET.
                break;
            switch (request.type()) {
            case CtrlRequest::PING:
                if (request.has_text()) {
                    const std::string &msg = request.text().msg();
                    log_info("CONTROL: PING with \"%s\"\n", msg.c_str());
                    resp.set_reply(CtrlResponse::SUCCESS);
                    resp.mutable_text()->set_msg(msg);
                } else {
                    resp.set_reply(CtrlResponse::INVALID);
                    resp.mutable_text()->set_msg("Invalid parameter.");
                }
                break;
            case CtrlRequest::MALLOC:
                if (request.has_malloc()) {
                    void *ptr = _mm_malloc(request.malloc().size(), request.malloc().align());
                    if (ptr == nullptr) {
                        resp.set_reply(CtrlResponse::FAILURE);
                        resp.mutable_text()->set_msg("_mm_malloc failed.");
                    } else {
                        resp.set_reply(CtrlResponse::SUCCESS);
                        resp.mutable_resource()->set_handle((uintptr_t) ptr);
                    }
                } else {
                    resp.set_reply(CtrlResponse::INVALID);
                    resp.mutable_text()->set_msg("Invalid parameter.");
                }
                break;
            case CtrlRequest::FREE:
                if (request.has_resource()) {
                    void *ptr = (void *) request.resource().handle();
                    _mm_free(ptr);
                    resp.set_reply(CtrlResponse::SUCCESS);
                } else {
                    resp.set_reply(CtrlResponse::INVALID);
                    resp.mutable_text()->set_msg("Invalid parameter.");
                }
                break;
            case CtrlRequest::CREATE_VDEV:
                if (request.has_vdevinfo()) {
                    pthread_barrier_t ready_barrier;
                    pthread_barrier_init(&ready_barrier, nullptr, 2);
                    struct vdevice *vdev = create_vdev(request.vdevinfo().num_pcores(),
                                                       request.vdevinfo().num_lcores_per_pcore(),
                                                       request.vdevinfo().pipeline_depth(),
                                                       &ready_barrier);
                    pthread_barrier_wait(&ready_barrier);
                    pthread_barrier_destroy(&ready_barrier);
                    if (vdev == nullptr) {
                        resp.set_reply(CtrlResponse::FAILURE);
                        resp.mutable_text()->set_msg("vDevice creation failed.");
                    } else {
                        resp.set_reply(CtrlResponse::SUCCESS);
                        resp.mutable_resource()->set_handle((uintptr_t) vdev);
                        resp.mutable_resource()->set_id(vdev->device_id);
                    }
                } else {
                    resp.set_reply(CtrlResponse::INVALID);
                    resp.mutable_text()->set_msg("Invalid parameter.");
                }
                break;
            case CtrlRequest::DESTROY_VDEV:
                if (request.has_resource()) {
                    struct vdevice *vdev = (struct vdevice *) request.resource().handle();
                    destroy_vdev(vdev);
                    resp.set_reply(CtrlResponse::SUCCESS);
                } else {
                    resp.set_reply(CtrlResponse::INVALID);
                    resp.mutable_text()->set_msg("Invalid parameter.");
                }
                break;
            case CtrlRequest::CREATE_POLLRING:
                if (request.has_pollring()) {
                    struct vdevice *vdev = (struct vdevice *) request.pollring().vdev_handle();
                    uint32_t id = request.pollring().ring_id();
                    if (create_pollring(vdev, id, request.pollring().len(),
                                        request.pollring().local_ra())) {
                        resp.set_reply(CtrlResponse::SUCCESS);
                        resp.mutable_resource()->set_peer_ra((uint64_t) vdev->poll_rings[id]->ra());
                    } else
                        resp.set_reply(CtrlResponse::FAILURE);
                } else {
                    resp.set_reply(CtrlResponse::INVALID);
                    resp.mutable_text()->set_msg("Invalid parameter.");
                }
                break;
            case CtrlRequest::DESTROY_POLLRING:
                if (request.has_pollring_ref()) {
                    struct vdevice *vdev = (struct vdevice *) request.pollring_ref().vdev_handle();
                    if (destroy_pollring(vdev, request.pollring_ref().ring_id())) {
                        resp.set_reply(CtrlResponse::SUCCESS);
                    } else
                        resp.set_reply(CtrlResponse::FAILURE);
                } else {
                    resp.set_reply(CtrlResponse::INVALID);
                    resp.mutable_text()->set_msg("Invalid parameter.");
                }
                break;
            case CtrlRequest::CREATE_RMABUFFER:
                if (request.has_rma()) {
                    struct vdevice *vdev = (struct vdevice *) request.rma().vdev_handle();
                    uint32_t id = request.rma().buffer_id();
                    if (create_rma(vdev, id, request.rma().size(),
                                   request.rma().local_ra())) {
                        resp.mutable_resource()->set_peer_ra((uint64_t) vdev->rma_buffers[id]->ra());
                        resp.set_reply(CtrlResponse::SUCCESS);
                    } else
                        resp.set_reply(CtrlResponse::FAILURE);
                } else {
                    resp.set_reply(CtrlResponse::INVALID);
                    resp.mutable_text()->set_msg("Invalid parameter.");
                }
                break;
            case CtrlRequest::DESTROY_RMABUFFER:
                if (request.has_rma_ref()) {
                    struct vdevice *vdev = (struct vdevice *) request.rma_ref().vdev_handle();
                    if (destroy_rma(vdev, request.rma_ref().buffer_id()))
                        resp.set_reply(CtrlResponse::SUCCESS);
                    else
                        resp.set_reply(CtrlResponse::FAILURE);
                } else {
                    resp.set_reply(CtrlResponse::INVALID);
                    resp.mutable_text()->set_msg("Invalid parameter.");
                }
                break;
            default:
                log_error("CONTROL: Not implemented request type: %d\n", request.type());
                resp.set_reply(CtrlResponse::INVALID);
                resp.mutable_text()->set_msg("Invalid request type.");
                break;
            }
            if (!send_ctrlresp(master_epd, resp))
                break;
        } // endwhile
        scif_close(master_epd);
        log_info("The control session terminated.\n");
    } // endwhile
    log_info("Terminating the control channel...\n");
    scif_close(master_listen_epd);

    return nullptr;
}


static void nba::knapp::stop_all() {
    exit_flag = true;
    for (auto vdev : vdevs)
        vdev->exit = true;

    log_info("Stopping...\n");

    /* Ensure propagation of signals. */
    for (auto vdev : vdevs)
        if (vdev->threads_alive) {
            pthread_kill(vdev->master_thread, SIGINT);
            vdev->threads_alive = false;
        }
    pthread_kill(control_thread, SIGINT);

    /* Wait until all finishes. */
    for (auto vdev : vdevs)
        if (vdev->threads_alive) {
            pthread_join(vdev->master_thread, nullptr);
            vdev->threads_alive = false;
        }
    pthread_join(control_thread, nullptr);
}

static void nba::knapp::handle_signal(int signal) {
    /* Ensure that this is the main thread. */
    if (pthread_self() != control_thread)
        stop_all();
}


#ifdef EMPTY_CYCLES
int num_bubble_cycles = 100;
#endif

int main (int argc, char *argv[])
{
    int rc;
    mic_num_lcores = sysconf(_SC_NPROCESSORS_ONLN);
    mic_num_pcores = sysconf(_SC_NPROCESSORS_ONLN) / KNAPP_MAX_THREADS_PER_CORE;
    memzero(pcore_used, KNAPP_NUM_CORES);
    log_info("%ld lcores (%ld pcores) detected.\n", mic_num_lcores, mic_num_pcores);

    setlocale(LC_NUMERIC, "");
#ifdef EMPTY_CYCLES
    if (argc > 1) {
        num_bubble_cycles = atoi(argv[1]);
        assert(num_bubble_cycles > 0);
        fprintf(stderr, "# of bubbles in kernel set to %d\n", num_bubble_cycles);
    } else {
        fprintf(stderr, "Need extra parameter for # of empty cycles\n");
        exit(EXIT_FAILURE);
    }
#endif
    exit_flag = false;
    pthread_attr_t attr;
    {
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
        set_cpu_mask(&attr, 0, mic_num_lcores);
    }
    rc = pthread_create(&control_thread, &attr, control_thread_loop, nullptr);
    assert(0 == rc);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    pthread_join(control_thread, nullptr);
    return 0;
}

// vim: ts=8 sts=4 sw=4 et
