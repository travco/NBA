#ifndef __NBA_ELEMENT_ETHER_ARPRESPONDER_HH__
#define __NBA_ELEMENT_ETHER_ARPRESPONDER_HH__

#include <nba/element/element.hh>
#include <nba/element/annotation.hh>
#include <vector>
#include <string>
#include <unordered_map>
// TODO: separate EtherAddress definition
#include "util_arptable.hh"

namespace nba {

class ARPResponder : public Element {
public:
    ARPResponder(): Element()
    {
    }

    ~ARPResponder()
    {
    }

    const char *class_name() const { return "ARPResponder"; }
    const char *port_count() const { return "1/1"; }

    int initialize();
    int initialize_global();    // per-system configuration
    int initialize_per_node();  // per-node configuration
    int configure(comp_thread_context *ctx, std::vector<std::string> &args);

    int process(int input_port, Packet *pkt);

private:
    std::vector<std::string> _args;
    std::unordered_map<uint32_t, EtherAddress> _addr_hashmap;
};

EXPORT_ELEMENT(ARPResponder);

}

#endif

// vim: ts=8 sts=4 sw=4 et
