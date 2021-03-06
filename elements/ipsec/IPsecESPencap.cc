#include "IPsecESPencap.hh"
#include <nba/element/annotation.hh>
#include <nba/framework/threadcontext.hh>
#include <random>
#include <nba/core/checksum.hh>
#include <xmmintrin.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <openssl/aes.h>
#include <openssl/sha.h>
#include <rte_memory.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>

using namespace std;
using namespace nba;

int IPsecESPencap::initialize()
{
    rand = bind(uniform_int_distribution<uint64_t>{}, mt19937_64());

    // TODO: Version of ip pkt (4 or 6), src & dest addr of encapsulated pkt should be delivered from configuation.
    assert(num_tunnels != 0);
    for (int i = 0; i < num_tunnels; i++) {
        struct ipaddr_pair pair;
        pair.src_addr  = 0x0a000001u;
        pair.dest_addr = 0x0a000000u | (i + 1); // (rand() % 0xffffff);
        struct espencap_sa_entry *entry = new struct espencap_sa_entry;
        entry->spi = rand() % 0xffffffffu;
        entry->rpl = rand() % 0xffffffffu;
        entry->gwaddr = 0x0a000001u;
        entry->entry_idx = i;
        auto result = sa_table.insert(make_pair<ipaddr_pair&, espencap_sa_entry*&>(pair, entry));
        assert(result.second == true);
        sa_table_linear[i] = entry;
    }

    return 0;
}

int IPsecESPencap::configure(comp_thread_context *ctx, std::vector<std::string> &args)
{
    Element::configure(ctx, args);
    num_tunnels = 1024;         // TODO: this value must be come from configuration.
    return 0;
}

// Input packet: (pkt_in)
// +----------+---------------+---------+
// | Ethernet | IP(proto=UDP) | payload |
// +----------+---------------+---------+
// ^ethh      ^iph
//
// Input packet when latency measurement: (pkt_in)
// +----------+---------------+-------+-------------------------------+---------+
// | Ethernet | IP(proto=UDP) |  UDP  | 16-bit key + 64-bit timestamp | padding |
// +----------+---------------+-------+-------------------------------+---------+
// ^ethh      ^iph                    ^latency_ptr
// The position of latency_ptr is overwritten after prepending the ESP header.
//
// Output packet: (pkt_out)
// +----------+---------------+--------+----+------------+---------+-------+---------------------+
// | Ethernet | IP(proto=ESP) |  ESP   | IP |  payload   | padding | extra | HMAC-SHA1 signature |
// +----------+---------------+--------+----+------------+---------+-------+---------------------+
//      14            20          16     20                pad_len     2    SHA_DIGEST_LENGTH = 20
// ^ethh      ^iph            ^esph    ^encapped_iph     ^esp_trail
//
int IPsecESPencap::process(int input_port, Packet *pkt)
{
    // Temp: Assumes input packet is always IPv4 packet.
    // TODO: make it to handle IPv6 also.
    // TODO: Set src & dest of encapped pkt to ip addrs from configuration.

    struct ether_hdr *ethh = (struct ether_hdr *) pkt->data();
    if (ntohs(ethh->ether_type) != ETHER_TYPE_IPv4) {
        pkt->kill();
        return 0;
    }
    struct iphdr *iph = (struct iphdr *) (ethh + 1);

    struct ipaddr_pair pair;
    pair.src_addr = ntohl(iph->saddr);
    pair.dest_addr = ntohl(iph->daddr);
    auto sa_item = sa_table.find(pair);
    struct espencap_sa_entry *sa_entry = NULL;
    if (likely(sa_item != sa_table.end())) {
        sa_entry = sa_item->second;
        anno_set(&pkt->anno, NBA_ANNO_IPSEC_FLOW_ID, sa_entry->entry_idx);
        assert(sa_entry->entry_idx < 1024u);
    } else {
        pkt->kill();
        return 0;
        // FIXME: this is to encrypt all traffic regardless sa_entry lookup results.
        //        (just for worst-case performance tests)
        //unsigned f = (tunnel_counter ++) % num_tunnels;
        //sa_entry = sa_table_linear[f];
        //anno_set(&pkt->anno, NBA_ANNO_IPSEC_FLOW_ID, f);
        //assert(f < 1024u);
    }

    int ip_len = ntohs(iph->tot_len);
    int pad_len = AES_BLOCK_SIZE - (ip_len + 2) % AES_BLOCK_SIZE;
    int enc_size = ip_len + pad_len + 2;    // additional two bytes mean the "extra" part.
    int extended_ip_len = (short) (sizeof(struct iphdr) + enc_size
                         + sizeof(struct esphdr) + SHA_DIGEST_LENGTH);
    int length_to_extend = extended_ip_len - ip_len;
    pkt->put(length_to_extend);
    assert(0 == (enc_size % AES_BLOCK_SIZE));

    struct esphdr *esph = (struct esphdr *) (iph + 1);
    uint8_t *encapped_iph = (uint8_t *) esph + sizeof(*esph);
    uint8_t *esp_trail    = encapped_iph + ip_len;

    // Hack for latency measurement experiments.
    uintptr_t latency_ptr = 0;
    constexpr uintptr_t latency_offset = sizeof(struct ether_hdr)
                                         + sizeof(struct ipv4_hdr)
                                         + sizeof(struct udp_hdr);
    static_assert(sizeof(struct udp_hdr) + sizeof(uint16_t) + sizeof(uint64_t)
                  <= sizeof(struct esphdr) + sizeof(ipv4_hdr),
                  "Encryption may overwrite latency!");
    __m128i timestamp;  // actual size: uin16 + uint64
    if (ctx->preserve_latency) {
        // latency data size: 16 bit key + 64 bit timestamp
        latency_ptr = (uintptr_t) pkt->data() + latency_offset;
        timestamp = _mm_loadu_si128((__m128i *) latency_ptr);
    }

    memmove(encapped_iph, iph, ip_len);         // copy the IP header and payload.
    memset(esp_trail, 0, pad_len);              // clear the padding.
    esp_trail[pad_len] = (uint8_t) pad_len;     // store pad_len at the second byte from last.
    esp_trail[pad_len + 1] = 0x04;              // store IP-in-IP protocol id at the last byte.

    // Fill the ESP header.
    esph->esp_spi = sa_entry->spi;
    esph->esp_rpl = sa_entry->rpl;

    // Generate random IV.
    uint64_t iv_first_half = rand();
    uint64_t iv_second_half = rand();
    __m128i new_iv = _mm_set_epi64((__m64) iv_first_half, (__m64) iv_second_half);
    _mm_storeu_si128((__m128i *) esph->esp_iv, new_iv);
    anno_set(&pkt->anno, NBA_ANNO_IPSEC_IV1, iv_first_half);
    anno_set(&pkt->anno, NBA_ANNO_IPSEC_IV2, iv_second_half);

    // Hack for latency measurement experiments.
    if (ctx->preserve_latency) {
        _mm_storeu_si128((__m128i *) latency_ptr, timestamp);
    }

    iph->ihl = (20 >> 2);               // standard IP header size.
    iph->tot_len = htons(extended_ip_len);
    iph->protocol = 0x32;               // mark that this packet contains a secured payload.
    iph->check = 0;                     // ignoring previous checksum.
    iph->check = ip_fast_csum(iph, iph->ihl);
    output(0).push(pkt);
    return 0;
}

// vim: sts=4 sw=4 et ts=8
