#ifndef __NBA_COMPUTECONTEXT_HH__
#define __NBA_COMPUTECONTEXT_HH__

#include <nba/core/intrinsic.hh>
#include <nba/core/offloadtypes.hh>
#include <nba/framework/computedevice.hh>

namespace nba {

class OffloadTask;

typedef unsigned io_base_t;
const io_base_t INVALID_IO_BASE = 0xffffffffu;
const uint32_t  INVALID_TASK_ID = 0xffffffffu;

class ComputeContext {

    /**
     * ComputeContext represents a command queue, such as CUDA streams.
     */
public:
    static const unsigned READY = 0;
    static const unsigned PREPARING = 1;
    static const unsigned RUNNING = 2;
    static const unsigned FINISHED = 3;
    static const unsigned PTR_STORAGE_SIZE = 16;

    ComputeContext(unsigned ctx_id, ComputeDevice *mother_device)
                   : type_name("<invalid>"), state(READY), currently_running_task(NULL),
                     mother_device(mother_device),
                     node_id(mother_device->node_id),
                     device_id(mother_device->device_id),
                     ctx_id(ctx_id)
    {
    }
    virtual ~ComputeContext() {}

    virtual uint32_t alloc_task_id() = 0;
    virtual void release_task_id(uint32_t task_id) = 0;
    virtual io_base_t alloc_io_base() = 0;
    virtual int alloc_input_buffer(io_base_t io_base, size_t size,
                                   host_mem_t &hbuf, dev_mem_t &dbuf) = 0;
    virtual int alloc_inout_buffer(io_base_t io_base, size_t size,
                                   host_mem_t &hbuf, dev_mem_t &dbuf) = 0;
    virtual int alloc_output_buffer(io_base_t io_base, size_t size,
                                    host_mem_t &hbuf, dev_mem_t &dbuf) = 0;
    virtual void get_input_buffer(io_base_t io_base,
                                  host_mem_t &hbuf, dev_mem_t &dbuf) const = 0;
    virtual void get_inout_buffer(io_base_t io_base,
                                  host_mem_t &hbuf, dev_mem_t &dbuf) const = 0;
    virtual void get_output_buffer(io_base_t io_base,
                                   host_mem_t &hbuf, dev_mem_t &dbuf) const = 0;
    virtual void *unwrap_host_buffer(const host_mem_t hbuf) const = 0;
    virtual void *unwrap_device_buffer(const dev_mem_t dbuf) const = 0;
    virtual size_t get_input_size(io_base_t io_base) const = 0;
    virtual size_t get_inout_size(io_base_t io_base) const = 0;
    virtual size_t get_output_size(io_base_t io_base) const = 0;
    virtual void shift_inout_base(io_base_t io_base, size_t len) = 0;
    virtual void clear_io_buffers(io_base_t io_base) = 0;

    virtual void clear_kernel_args() = 0;
    virtual void push_kernel_arg(struct kernel_arg &arg) = 0;
    virtual void push_common_kernel_args() = 0;

    /* All methods below must be implemented using non-blocking APIs provided
     * by device runtimes. */
    virtual int enqueue_memwrite_op(uint32_t task_id,
                                    const host_mem_t host_buf, const dev_mem_t dev_buf,
                                    size_t offset, size_t size) = 0;
    virtual int enqueue_memread_op(uint32_t task_id,
                                   const host_mem_t host_buf, const dev_mem_t dev_buf,
                                   size_t offset, size_t size) = 0;
    virtual int enqueue_kernel_launch(//uint32_t task_id,
                                      dev_kernel_t kernel,
                                      struct resource_param *res) = 0;
    virtual int enqueue_event_callback(uint32_t task_id,
                                       void (*func_ptr)(ComputeContext *ctx, void *user_arg),
                                       void *user_arg) = 0;

    virtual void h2d_done(uint32_t task_id) = 0;
    virtual void d2h_done(uint32_t task_id) = 0;
    virtual bool poll_input_finished(uint32_t task_id) = 0;
    virtual bool poll_kernel_finished(uint32_t task_id) = 0;
    virtual bool poll_output_finished(uint32_t task_id) = 0;

    unsigned get_id()
    {
        return ctx_id;
    }

    ComputeDevice *mother() {
        // FIXME: lift off const_cast
        return const_cast<ComputeDevice *>(mother_device);
    }

public:
    std::string type_name;
    volatile unsigned state __cache_aligned;
    OffloadTask *currently_running_task;

    const ComputeDevice *mother_device;
    const unsigned node_id;
    const unsigned device_id;
    const unsigned ctx_id;
};

}
#endif

// vim: ts=8 sts=4 sw=4 et
