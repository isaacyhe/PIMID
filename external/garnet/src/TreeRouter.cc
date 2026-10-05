/*
 * TreeRouter -- up/down deadlock-free router for a CUSTOM DRAM tree.
 * See TreeRouter.hh for the full rationale.
 */

#include "TreeRouter.hh"

#include "gem5_compat/debug/RubyNetwork.hh"
#include "InputUnit.hh"
#include "OutputUnit.hh"

namespace gem5 {
namespace ruby {
namespace garnet {

TreeRouter::TreeRouter(const Params &p)
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
TreeRouter::init()
{
    Router::init();

    m_num_inports = get_num_inports();
    m_num_outports = get_num_outports();

    m_round_robin_invc.resize(m_num_inports, 0);
    m_round_robin_inport.resize(m_num_outports, 0);
    m_port_requests.resize(m_num_inports, -1);
    m_vc_winners.resize(m_num_inports, -1);
}

void
TreeRouter::wakeup()
{
    DPRINTF(RubyNetwork, "TreeRouter %d woke up\n", get_id());
    assert(clockEdge() == curTick());

    // 1. Process incoming flits at input units
    for (int i = 0; i < m_num_inports; i++) {
        getInputUnit(i)->wakeup();
    }

    // 2. Process incoming credits at output units
    for (int o = 0; o < m_num_outports; o++) {
        getOutputUnit(o)->wakeup();
    }

    // 3. Switch Allocation with up/down-aware VC allocation
    arbitrate_inports();   // SA-I: per-input VC selection
    arbitrate_outports();  // SA-II: per-output port arbitration

    // Clear request state for next cycle
    clear_request_vector();

    // 4. Switch traversal (standard crossbar -- N flits/cycle)
    crossbarSwitch.wakeup();

    // 5. Reschedule if more flits pending
    check_for_wakeup();
}

// --- Up/Down helpers -----------------------------------------

bool
TreeRouter::isDownOutport(int outport) const
{
    // const_cast: getOutportDirection is a non-const accessor in the base
    // class but performs no mutation.
    PortDirection d =
        const_cast<TreeRouter*>(this)->getOutportDirection(outport);
    return d == "Down";
}

int
TreeRouter::vcClassOf(int vc) const
{
    // Within each vnet: lower half = class 0 (UP), upper half = class 1 (DOWN).
    int offset = vc % m_vc_per_vnet;
    int half = m_vc_per_vnet / 2;
    if (half <= 0) half = 1;  // degenerate (1 VC/vnet): single shared class
    return (offset < half) ? 0 : 1;
}

void
TreeRouter::getClassRange(int vnet, int vc_class, int &lo, int &hi) const
{
    int base = vnet * m_vc_per_vnet;
    int half = m_vc_per_vnet / 2;
    if (half <= 0) half = 1;

    if (m_vc_per_vnet <= 1) {
        // Degenerate: only one VC per vnet -- both classes share it.
        // (A 1-VC tree is not deadlock-free in general; provision >= 2.)
        lo = base;
        hi = base + m_vc_per_vnet;
        return;
    }

    if (vc_class == 0) {        // UP phase: lower half
        lo = base;
        hi = base + half;
    } else {                    // DOWN phase: upper half
        lo = base + half;
        hi = base + m_vc_per_vnet;
    }
}

// Required VC class for a flit leaving via `outport`.
//   - DOWN outport  -> class 1 (DOWN phase); promote and stay.
//   - UP outport, NI-injected (Local inport) -> class 0 (start UP phase).
//   - UP outport, otherwise -> keep current class. Shortest-path tree
//     routing only sends a packet UP while it is still in the UP phase
//     (class 0), so this preserves class 0; it can never demote class 1.
int
TreeRouter::targetClass(int inport, int invc, int outport) const
{
    if (isDownOutport(outport)) {
        return 1;  // DOWN phase
    }
    // UP outport (or Local -- Local is only used for terminal delivery, which
    // the routing layer resolves before reaching here; treat as class 0).
    auto input_unit =
        const_cast<TreeRouter*>(this)->getInputUnit(inport);
    if (input_unit->get_direction() == "Local") {
        return 0;  // freshly injected: start in the UP phase
    }
    return vcClassOf(invc);  // stay in current phase (must be 0 for an UP hop)
}

// --- SA-I: per-input VC selection (standard round-robin) -----

void
TreeRouter::arbitrate_inports()
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

// --- SA-II: per-output port arbitration with up/down VC ------

void
TreeRouter::arbitrate_outports()
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
                    // VC Allocation -- up/down-aware
                    outvc = vc_allocate(outport, inport, invc);
                    if (outvc == -1) {
                        // No free VC in the required class -- skip this inport
                        inport = (inport + 1) % m_num_inports;
                        continue;
                    }
                }

                // Remove flit from input VC
                flit *t_flit = input_unit->getTopFlit(invc);

                DPRINTF(RubyNetwork, "TreeRouter %d SA-II: inport %d "
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

// --- Up/Down-aware VC allocation -----------------------------
//
// For HEAD/HEAD_TAIL flits: allocate a free VC from the required class
// (UP=class 0, DOWN=class 1) on the chosen outport.
// For BODY/TAIL flits: outvc was already allocated by HEAD, reuse it.

int
TreeRouter::vc_allocate(int outport, int inport, int invc)
{
    int vnet = get_vnet(invc);
    auto output_unit = getOutputUnit(outport);
    auto input_unit = getInputUnit(inport);

    int target = targetClass(inport, invc, outport);

    int lo, hi;
    getClassRange(vnet, target, lo, hi);
    int outvc = output_unit->select_free_vc_in_range(lo, hi);
    if (outvc != -1) {
        input_unit->grant_outvc(invc, outvc);
    }
    return outvc;
}

bool
TreeRouter::send_allowed(int inport, int invc, int outport, int outvc)
{
    int vnet = get_vnet(invc);
    bool has_outvc = (outvc != -1);
    bool has_credit = false;

    auto output_unit = getOutputUnit(outport);
    auto input_unit = getInputUnit(inport);

    if (!has_outvc) {
        // HEAD flit needs a new VC -- restrict to the required class
        int target = targetClass(inport, invc, outport);
        int lo, hi;
        getClassRange(vnet, target, lo, hi);
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
TreeRouter::get_vnet(int invc)
{
    int vnet = invc / m_vc_per_vnet;
    assert(vnet < (int)get_num_vnets());
    return vnet;
}

void
TreeRouter::clear_request_vector()
{
    std::fill(m_port_requests.begin(), m_port_requests.end(), -1);
    std::fill(m_vc_winners.begin(), m_vc_winners.end(), -1);
}

void
TreeRouter::check_for_wakeup()
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
TreeRouter::resetStats()
{
    Router::resetStats();
    m_input_arbiter_activity = 0;
    m_output_arbiter_activity = 0;
}

} // namespace garnet
} // namespace ruby
} // namespace gem5
