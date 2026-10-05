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
 */

#include "RingRouter.hh"

#include "gem5_compat/debug/RubyNetwork.hh"
#include "InputUnit.hh"
#include "OutputUnit.hh"

namespace gem5 {
namespace ruby {
namespace garnet {

RingRouter::RingRouter(const Params &p)
    : Router(p),
      m_num_inports(0),
      m_num_outports(0),
      m_num_vcs(p.vcs_per_vnet * p.virt_nets),
      m_vc_per_vnet(p.vcs_per_vnet),
      m_input_arbiter_activity(0),
      m_output_arbiter_activity(0)
{
}

void
RingRouter::init()
{
    Router::init();

    m_num_inports = get_num_inports();
    m_num_outports = get_num_outports();

    m_round_robin_invc.resize(m_num_inports, 0);
    m_round_robin_inport.resize(m_num_outports, 0);
    m_port_requests.resize(m_num_inports, -1);
    m_vc_winners.resize(m_num_inports, -1);

    // 1.11.94 (H35): resolve dateline directions to outport indices.
    for (int o = 0; o < m_num_outports; o++) {
        PortDirection d = getOutportDirection(o);
        for (const auto& dd : m_dateline_dirns) {
            if (d == dd && !isDatelineCrossing(o)) {
                m_dateline_outports.push_back(o);
            }
        }
    }
}

void
RingRouter::wakeup()
{
    DPRINTF(RubyNetwork, "RingRouter %d woke up\n", get_id());
    assert(clockEdge() == curTick());

    // 1. Process incoming flits at input units
    for (int i = 0; i < m_num_inports; i++) {
        getInputUnit(i)->wakeup();
    }

    // 2. Process incoming credits at output units
    for (int o = 0; o < m_num_outports; o++) {
        getOutputUnit(o)->wakeup();
    }

    // 3. Switch Allocation with dateline-aware VC allocation
    arbitrate_inports();   // SA-I: per-input VC selection
    arbitrate_outports();  // SA-II: per-output port arbitration

    // Clear request state for next cycle
    clear_request_vector();

    // 4. Switch traversal (standard crossbar -- N flits/cycle)
    crossbarSwitch.wakeup();

    // 5. Reschedule if more flits pending
    check_for_wakeup();
}

// --- Dateline helpers ----------------------------------------

bool
RingRouter::isDatelineCrossing(int outport) const
{
    for (int p : m_dateline_outports) {
        if (p == outport) return true;
    }
    return false;
}

int
RingRouter::vcClassOf(int vc) const
{
    // Within each vnet, lower half of VCs = class 0, upper half = class 1
    int offset = vc % m_vc_per_vnet;
    int half = m_vc_per_vnet / 2;
    if (half <= 0) half = 1;  // degenerate: 1 VC per class
    return (offset < half) ? 0 : 1;
}

void
RingRouter::getClassRange(int vnet, int vc_class,
                          int &lo, int &hi) const
{
    int base = vnet * m_vc_per_vnet;
    int half = m_vc_per_vnet / 2;
    if (half <= 0) half = 1;

    if (vc_class == 0) {
        lo = base;
        hi = base + half;
    } else {
        lo = base + half;
        hi = base + m_vc_per_vnet;
    }
}

// 1.11.94 (H35): X = East/West, Y = North/South, -1 = anything else
// (Local, tree ports).
int
RingRouter::dimensionOf(const PortDirection& dirn)
{
    if (dirn == "East" || dirn == "West") return 0;
    if (dirn == "North" || dirn == "South") return 1;
    return -1;
}

// Target VC class for a head flit going inport/invc -> outport.
//   - crossing a dateline: class 1
//   - NI-injected (Local inport): class 0
//   - torus only (m_dimension_reset): turning from one dimension onto the
//     other: class 0 (a new ring, entered before its dateline)
//   - otherwise: stay in the current class
// Without m_dimension_reset this is exactly the rule RING has always used
// (the first two and the last branch), so RING allocation is unchanged.
int
RingRouter::targetClass(int inport, int invc, int outport)
{
    if (isDatelineCrossing(outport))
        return 1;
    PortDirection in_dirn = getInputUnit(inport)->get_direction();
    if (in_dirn == "Local")
        return 0;
    if (m_dimension_reset) {
        int din = dimensionOf(in_dirn);
        int dout = dimensionOf(getOutportDirection(outport));
        if (din >= 0 && dout >= 0 && din != dout)
            return 0;
    }
    return vcClassOf(invc);
}

// --- SA-I: per-input VC selection (standard round-robin) -----

void
RingRouter::arbitrate_inports()
{
    for (int inport = 0; inport < m_num_inports; inport++) {
        int invc = m_round_robin_invc[inport];

        for (int iter = 0; iter < m_num_vcs; iter++) {
            auto input_unit = getInputUnit(inport);

            if (input_unit->need_stage(invc, SA_, curTick())) {
                int outport = input_unit->get_outport(invc);
                int outvc = input_unit->get_outvc(invc);

                if (send_allowed(inport, invc, outport, outvc)) {
                    m_input_arbiter_activity++;
                    m_port_requests[inport] = outport;
                    m_vc_winners[inport] = invc;
                    break;
                }
            }

            invc = (invc + 1) % m_num_vcs;
        }
    }
}

// --- SA-II: per-output port arbitration with dateline VC -----
//
// Same structure as standard SwitchAllocator::arbitrate_outports(),
// but vc_allocate() is dateline-aware: when a HEAD flit crosses the
// dateline, it MUST be allocated to a class-1 VC.  Otherwise it
// stays in its current class.

void
RingRouter::arbitrate_outports()
{
    for (int outport = 0; outport < m_num_outports; outport++) {
        int inport = m_round_robin_inport[outport];

        for (int iter = 0; iter < m_num_inports; iter++) {
            if (m_port_requests[inport] == outport) {
                auto output_unit = getOutputUnit(outport);
                auto input_unit = getInputUnit(inport);

                int invc = m_vc_winners[inport];
                int outvc = input_unit->get_outvc(invc);

                if (outvc == -1) {
                    // VC Allocation -- dateline-aware
                    outvc = vc_allocate(outport, inport, invc);
                    if (outvc == -1) {
                        // No free VC in the required class -- skip
                        inport = (inport + 1) % m_num_inports;
                        continue;
                    }
                }

                // Remove flit from input VC
                flit *t_flit = input_unit->getTopFlit(invc);

                DPRINTF(RubyNetwork, "RingRouter %d SA-II: inport %d "
                        "(invc %d) -> outport %d (outvc %d) flit %s "
                        "at cycle %lld\n",
                        get_id(), inport, invc, outport, outvc,
                        *t_flit, curCycle());

                t_flit->set_outport(outport);
                t_flit->set_vc(outvc);

                output_unit->decrement_credit(outvc);

                t_flit->advance_stage(ST_, curTick());
                grant_switch(inport, t_flit);
                m_output_arbiter_activity++;

                if (t_flit->get_type() == TAIL_ ||
                    t_flit->get_type() == HEAD_TAIL_) {
                    assert(!(input_unit->isReady(invc, curTick())));
                    input_unit->set_vc_idle(invc, curTick());
                    input_unit->increment_credit(invc, true, curTick());
                } else {
                    input_unit->increment_credit(invc, false, curTick());
                }

                // Clear this request
                m_port_requests[inport] = -1;

                // Advance round-robin pointers
                m_round_robin_inport[outport] = (inport + 1) % m_num_inports;
                m_round_robin_invc[inport] = (invc + 1) % m_num_vcs;

                break;  // one winner per output port
            }

            inport = (inport + 1) % m_num_inports;
        }
    }
}

// --- Dateline-aware VC allocation ----------------------------
//
// For HEAD/HEAD_TAIL flits:
//   - NI-injected (Local inport): always allocate class 0
//     (packets must start in class 0 for dateline correctness)
//   - If crossing dateline: allocate from class 1
//   - Otherwise: stay in current class
// For BODY/TAIL flits: outvc was already allocated by HEAD, reuse it.

int
RingRouter::vc_allocate(int outport, int inport, int invc)
{
    int vnet = get_vnet(invc);
    auto output_unit = getOutputUnit(outport);
    auto input_unit = getInputUnit(inport);

    // Determine the target VC class (1.11.94 H35: one rule, targetClass)
    int target_class = targetClass(inport, invc, outport);

    int lo, hi;
    getClassRange(vnet, target_class, lo, hi);
    int outvc = output_unit->select_free_vc_in_range(lo, hi);
    if (outvc != -1) {
        input_unit->grant_outvc(invc, outvc);
    }
    return outvc;
}

bool
RingRouter::send_allowed(int inport, int invc, int outport, int outvc)
{
    int vnet = get_vnet(invc);
    bool has_outvc = (outvc != -1);
    bool has_credit = false;

    auto output_unit = getOutputUnit(outport);
    auto input_unit = getInputUnit(inport);

    if (!has_outvc) {
        // HEAD flit needs a new VC -- determine target class
        int target_class = targetClass(inport, invc, outport);

        int lo, hi;
        getClassRange(vnet, target_class, lo, hi);
        if (output_unit->has_free_vc_in_range(lo, hi)) {
            has_outvc = true;
            has_credit = true;
        }
    } else {
        has_credit = output_unit->has_credit(outvc);
    }

    if (!has_outvc || !has_credit)
        return false;

    // Protocol ordering check
    if (get_net_ptr()->isVNetOrdered(vnet)) {
        Tick t_enqueue_time = input_unit->get_enqueue_time(invc);
        int vc_base = vnet * m_vc_per_vnet;
        for (int vc_offset = 0; vc_offset < m_vc_per_vnet; vc_offset++) {
            int temp_vc = vc_base + vc_offset;
            if (input_unit->need_stage(temp_vc, SA_, curTick()) &&
                (input_unit->get_outport(temp_vc) == outport) &&
                (input_unit->get_enqueue_time(temp_vc) < t_enqueue_time)) {
                return false;
            }
        }
    }

    return true;
}

int
RingRouter::get_vnet(int invc)
{
    int vnet = invc / m_vc_per_vnet;
    assert(vnet < (int)get_num_vnets());
    return vnet;
}

void
RingRouter::clear_request_vector()
{
    std::fill(m_port_requests.begin(), m_port_requests.end(), -1);
    std::fill(m_vc_winners.begin(), m_vc_winners.end(), -1);
}

void
RingRouter::check_for_wakeup()
{
    Tick nextCycle = clockEdge(Cycles(1));

    if (alreadyScheduled(nextCycle))
        return;

    for (int i = 0; i < m_num_inports; i++) {
        for (int j = 0; j < m_num_vcs; j++) {
            if (getInputUnit(i)->need_stage(j, SA_, nextCycle)) {
                schedule_wakeup(Cycles(1));
                return;
            }
        }
    }
}

void
RingRouter::resetStats()
{
    Router::resetStats();
    m_input_arbiter_activity = 0;
    m_output_arbiter_activity = 0;
}

} // namespace garnet
} // namespace ruby
} // namespace gem5
