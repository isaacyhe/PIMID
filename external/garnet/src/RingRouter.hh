/*
 * RingRouter -- dateline-aware router for Garnet RING topology.
 *
 * Standard crossbar throughput (N flits/cycle across N output ports),
 * but VC allocation enforces the dateline invariant:
 *
 *   VCs are split into two classes (lower half = class 0, upper = class 1).
 *   Packets start in class 0.  When a packet crosses the dateline link,
 *   it must be promoted to class 1.  Once in class 1, it stays there.
 *   This breaks the circular VC dependency and prevents routing deadlock.
 *
 * The dateline cut is between node (N-1) and node 0.  Each RingRouter
 * is told which of its outports (if any) crosses the dateline.
 *
 * 1.11.94 (x03-garnet-custom-1, ruling H35): the same router also builds
 * TORUS_2D. A torus under dimension-order routing is one ring per row
 * (East/West) and one ring per column (North/South), each with its own
 * dateline on its wrap link. With setDimensionReset(true) a head flit that
 * turns from the X rings onto the Y rings restarts in class 0: DOR never
 * turns back from Y to X, so the X->Y dependency is acyclic and each ring
 * only needs its own dateline (Dally and Towles, "Principles and Practices
 * of Interconnection Networks", sec. 14.2, dateline per dimension). The RING
 * topology does not set the flag, so its allocation is unchanged.
 */

#ifndef __MEM_RUBY_NETWORK_GARNET_0_RINGROUTER_HH__
#define __MEM_RUBY_NETWORK_GARNET_0_RINGROUTER_HH__

#include <vector>

#include "Router.hh"

namespace gem5 {
namespace ruby {
namespace garnet {

class RingRouter : public Router {
  public:
    using Params = GarnetRouterParams;
    RingRouter(const Params &p);
    ~RingRouter() = default;

    void wakeup() override;
    void init() override;
    void resetStats();

    // Mark an outport as crossing the dateline.
    // Called by TopologyBuilders::buildRing() after link creation.
    void addDatelineOutport(int outport) { m_dateline_outports.push_back(outport); }

    // 1.11.94 (H35): mark the outport(s) named `dirn` as crossing the
    // dateline. Resolved to outport indices in init(), once the links exist,
    // so the builder does not have to predict port numbering.
    void addDatelineDirection(const PortDirection& dirn)
    { m_dateline_dirns.push_back(dirn); }

    // 1.11.94 (H35): restart in class 0 when a head flit changes dimension
    // (East/West <-> North/South). Set by the TORUS_2D builder only.
    void setDimensionReset(bool on) { m_dimension_reset = on; }

  private:
    // SA-I: per-input VC selection (standard)
    void arbitrate_inports();

    // SA-II: per-output port arbitration with dateline-aware VC allocation
    void arbitrate_outports();

    bool send_allowed(int inport, int invc, int outport, int outvc);
    int vc_allocate(int outport, int inport, int invc);
    int get_vnet(int invc);
    void clear_request_vector();
    void check_for_wakeup();

    // Dateline helpers
    bool isDatelineCrossing(int outport) const;
    int vcClassOf(int vc) const;                    // 0 or 1
    void getClassRange(int vnet, int vc_class,      // [lo, hi)
                       int &lo, int &hi) const;

    // 1.11.94 (H35): the VC class a head flit must be allocated in.
    int targetClass(int inport, int invc, int outport);
    static int dimensionOf(const PortDirection& dirn);

    // Which outports cross the dateline (typically 1 or 2: CW and/or CCW)
    std::vector<int> m_dateline_outports;
    std::vector<PortDirection> m_dateline_dirns;
    bool m_dimension_reset = false;

    // Per-input request state (SA-I -> SA-II)
    std::vector<int> m_port_requests;
    std::vector<int> m_vc_winners;
    std::vector<int> m_round_robin_invc;
    std::vector<int> m_round_robin_inport;

    int m_num_inports;
    int m_num_outports;
    int m_num_vcs;
    int m_vc_per_vnet;

    double m_input_arbiter_activity;
    double m_output_arbiter_activity;
};

} // namespace garnet
} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_NETWORK_GARNET_0_RINGROUTER_HH__
