#ifndef __NBA_ELEMENT_IPv6_CHECKIP6HEADER_HH__
#define __NBA_ELEMENT_IPv6_CHECKIP6HEADER_HH__

#include <nba/element/element.hh>
#include <vector>
#include <string>

namespace nba {

class CheckIP6Header : public Element {
public:
    CheckIP6Header(): Element()
    {
    }

    ~CheckIP6Header()
    {
    }

    const char *class_name() const { return "CheckIP6Header"; }
    const char *port_count() const { return "1/1"; }

    int initialize();
    int initialize_global() { return 0; };      // per-system configuration
    int initialize_per_node() { return 0; };    // per-node configuration
    int configure(comp_thread_context *ctx, std::vector<std::string> &args);

    int process(int input_port, Packet *pkt);

};

EXPORT_ELEMENT(CheckIP6Header);

}

#endif

// vim: ts=8 sts=4 sw=4 et
