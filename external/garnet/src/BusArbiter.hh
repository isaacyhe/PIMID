/*
 * BusArbiter -- shared-bus arbiter for Garnet BUS topology.
 *
 * Replaces the crossbar-style router at the center of a BUS topology.
 * A real shared bus allows only ONE transfer per cycle across all ports.
 * This arbiter enforces that constraint via single-grant round-robin
 * switch allocation: each cycle, at most one input port wins the bus
 * and its flit traverses the switch.
 *
 * Inherits from Router so it plugs directly into Garnet's topology
 * infrastructure (NIs, Links, routing tables all work unchanged).
 */

#ifndef __MEM_RUBY_NETWORK_GARNET_0_BUSARBITER_HH__
#define __MEM_RUBY_NETWORK_GARNET_0_BUSARBITER_HH__

#include <vector>

#include "Router.hh"

namespace gem5 {
namespace ruby {
namespace garnet {

class BusArbiter : public Router {
  public:
    using Params = GarnetRouterParams;
    BusArbiter(const Params &p);
    ~BusArbiter() = default;

    void wakeup() override;
    void init() override;
    void resetStats();
    /* (1.12.259): a bus inside a CUSTOM tree takes the tree's up/down VC classes (see TreeRouter): a packet
     * injected here starts in the UP class, a DOWN outport promotes it to the DOWN class, and an output VC
     * is drawn only from its class. Off (the BUS topology) = one shared VC pool, as before. */
    void setUpDown(bool on) { m_updown = on; }
  private:
    bool m_updown = false;
    bool isDownOutport(int outport) const;
    int vcClassOf(int vc) const;
    void getClassRange(int vnet, int vc_class, int &lo, int &hi) const;
    int targetClass(int inport, int invc, int outport) const;
    // SA-I: per-input VC selection (same as crossbar SA-I)
    void arbitrate_inports();

    // SA-II: single-grant bus arbitration (replaces crossbar SA-II)
    void arbitrate_bus();

    // Check if a flit from (inport, invc) can send to (outport, outvc)
    bool send_allowed(int inport, int invc, int outport, int outvc);

    // Allocate a free VC on the output port
    int vc_allocate(int outport, int inport, int invc);

    // Which vnet does this VC belong to?
    int get_vnet(int invc);

    // Reschedule if flits are ready next cycle
    void check_for_wakeup();

    // Per-input request state (populated by SA-I, consumed by SA-II)
    std::vector<int> m_port_requests;   // inport -> requested outport (-1 = none)
    std::vector<int> m_vc_winners;      // inport -> winning VC

    // Per-input round-robin for VC selection
    std::vector<int> m_round_robin_invc;

    // Global round-robin pointer for bus grant
    int m_bus_rr;

    int m_num_inports;
    int m_num_outports;
    int m_num_vcs;
    int m_vc_per_vnet;

    double m_arbiter_activity;
};

} // namespace garnet
} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_NETWORK_GARNET_0_BUSARBITER_HH__
