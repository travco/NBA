#include <nba/core/accumidx.hh>
#include <nba/engines/knapp/defs.hh>
#include <nba/engines/knapp/mictypes.hh>
#include <nba/engines/knapp/sharedtypes.hh>
#include <nba/engines/knapp/kernels.hh>
#include "ipv4route.hh"
#include <cstdio>
#include <arpa/inet.h>

namespace nba { namespace knapp {

static void ipv4_route_lookup(
        uint32_t begin_idx,
        uint32_t end_idx,
        struct datablock_kernel_arg **datablocks,
        uint32_t *item_counts,
        uint32_t num_batches,
        size_t num_args,
        void **args);

}} //endns(nba::knapp)

using namespace nba::knapp;

static void nba::knapp::ipv4_route_lookup (
        uint32_t begin_idx,
        uint32_t end_idx,
        struct datablock_kernel_arg **datablocks,
        uint32_t *item_counts,
        uint32_t num_batches,
        size_t num_args,
        void **args)
{
    struct datablock_kernel_arg *db_dest_addrs = datablocks[0];
    struct datablock_kernel_arg *db_results    = datablocks[1];
    uint16_t *TBL24   = static_cast<uint16_t *>(args[0]);
    uint16_t *TBLlong = static_cast<uint16_t *>(args[1]);

    for (uint32_t idx = begin_idx; idx < end_idx; ++ idx) {
        uint32_t batch_idx, item_idx;
        nba::get_accum_idx(item_counts, num_batches,
                           idx, batch_idx, item_idx);
        uint32_t daddr = (static_cast<uint32_t*>(db_dest_addrs->batches[batch_idx].buffer_bases))[item_idx];
        uint16_t &lookup_result = (static_cast<uint16_t*>(db_results->batches[batch_idx].buffer_bases))[item_idx];

        lookup_result = 0xffff;
        uint32_t ip = ntohl(daddr);
        uint16_t temp_dest = TBL24[ip >> 8];
        if (temp_dest & 0x8000u) {
            int idx2 = (((uint32_t)(temp_dest & 0x7fff)) << 8) + (ip & 0xff);
            temp_dest = TBLlong[idx2];
        }
        lookup_result = temp_dest;
    }
}


void __attribute__((constructor, used)) ipv4_route_lookup_register()
{
    worker_funcs[ID_KERNEL_IPV4LOOKUP] = ipv4_route_lookup;
}


// vim: ts=8 sts=4 sw=4 et
