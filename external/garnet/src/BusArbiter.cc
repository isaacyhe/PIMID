/*
 * BusArbiter -- shared-bus arbiter for Garnet BUS topology.
 *
 * Models a real shared bus: one flit crosses the switch per cycle.
 * Uses round-robin arbitration among all input ports for fairness.
 */

#include "BusArbiter.hh"

#include "gem5_compat/debug/RubyNetwork.hh"
#include "InputUnit.hh"
#include "OutputUnit.hh"

namespace gem5 {
namespace ruby {
namespace garnet {

BusArbiter::BusArbiter(const Params &p)
    : Router(p),
      m_bus_rr(0),
      m_num_inports(0),
      m_num_outports(0),
      m_num_vcs(p.vcs_per_vnet * p.virt_nets),
      m_vc_per_vnet(p.vcs_per_vnet),
      m_arbiter_activity(0)
{
}

void
BusArbiter::init()
{
    Router::init();

    m_num_inports = get_num_inports();
    m_num_outports = get_num_outports();

    m_round_robin_invc.resize(m_num_inports, 0);
    m_port_requests.resize(m_num_inports, -1);
    m_vc_winners.resize(m_num_inports, -1);
}

void
BusArbiter::wakeup()
{
    DPRINTF(RubyNetwork, "BusArbiter %d woke up\n", get_id());
    assert(clockEdge() == curTick());

    // 1. Process incoming flits at input units
    for (int i = 0; i < m_num_inports; i++) {
        getInputUnit(i)->wakeup();
    }

    // 2. Process incoming credits at output units
    for (int o = 0; o < m_num_outports; o++) {
        getOutputUnit(o)->wakeup();
    }

    // 3. Bus arbitration (replaces crossbar SwitchAllocator)
    arbitrate_inports();    // SA-I: each input picks a VC
    arbitrate_bus();        // SA-II: single global grant

    // Clear request state for next cycle
    std::fill(m_port_requests.begin(), m_port_requests.end(), -1);
    std::fill(m_vc_winners.begin(), m_vc_winners.end(), -1);

    // 4. Switch traversal (move granted flit to output)
    crossbarSwitch.wakeup();

    // 5. Reschedule if more flits are pending
    check_for_wakeup();
}

/*
 * SA-I: For each input port, select one ready VC via round-robin.
 * Same logic as the standard SwitchAllocator -- we just collect
 * per-input requests here.
 */
void
BusArbiter::arbitrate_inports()
{
    for (int inport = 0; inport < m_num_inports; inport++) {
        int invc = m_round_robin_invc[inport];

        for (int iter = 0; iter < m_num_vcs; iter++) {
            auto input_unit = getInputUnit(inport);

            if (input_unit->need_stage(invc, SA_, curTick())) {
                int outport = input_unit->get_outport(invc);
                int outvc = input_unit->get_outvc(invc);

                if (send_allowed(inport, invc, outport, outvc)) {
                    m_port_requests[inport] = outport;
                    m_vc_winners[inport] = invc;
                    break;
                }
            }

            invc = (invc + 1) % m_num_vcs;
        }
    }
}

/*
 * SA-II: Bus arbitration -- grant exactly ONE input port per cycle.
 *
 * Round-robin across all input ports. The first port (starting from
 * m_bus_rr) that has a pending request wins the bus for this cycle.
 * All other requesters must wait.
 */
void
BusArbiter::arbitrate_bus()
{
    int winner = -1;

    for (int iter = 0; iter < m_num_inports; iter++) {
        int inport = (m_bus_rr + iter) % m_num_inports;

        if (m_port_requests[inport] < 0)
            continue;  // no request from this input

        int outport = m_port_requests[inport];
        int invc = m_vc_winners[inport];

        auto output_unit = getOutputUnit(outport);
        auto input_unit = getInputUnit(inport);

        int outvc = input_unit->get_outvc(invc);
        if (outvc == -1) {
            outvc = vc_allocate(outport, inport, invc);
        }

        // Remove flit from input VC
        flit *t_flit = input_unit->getTopFlit(invc);

        DPRINTF(RubyNetwork, "BusArbiter %d granted bus to inport %d "
                             "(invc %d) -> outport %d (outvc %d) flit %s "
                             "at cycle %lld\n",
                get_id(), inport, invc, outport, outvc,
                *t_flit, curCycle());

        t_flit->set_outport(outport);
        t_flit->set_vc(outvc);

        // Decrement credit in output VC
        output_unit->decrement_credit(outvc);

        // Advance flit to switch traversal stage
        t_flit->advance_stage(ST_, curTick());
        grant_switch(inport, t_flit);
        m_arbiter_activity++;

        if (t_flit->get_type() == TAIL_ ||
            t_flit->get_type() == HEAD_TAIL_) {
            assert(!(input_unit->isReady(invc, curTick())));
            input_unit->set_vc_idle(invc, curTick());
            input_unit->increment_credit(invc, true, curTick());
        } else {
            input_unit->increment_credit(invc, false, curTick());
        }

        winner = inport;
        break;  // BUS: only one grant per cycle
    }

    // Advance round-robin pointer past the winner
    if (winner >= 0) {
        m_bus_rr = (winner + 1) % m_num_inports;
    }
}

bool
BusArbiter::send_allowed(int inport, int invc, int outport, int outvc)
{
    int vnet = get_vnet(invc);
    bool has_outvc = (outvc != -1);
    bool has_credit = false;

    auto output_unit = getOutputUnit(outport);
    if (!has_outvc) {
        if (output_unit->has_free_vc(vnet)) {
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
        auto input_unit = getInputUnit(inport);
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
BusArbiter::vc_allocate(int outport, int inport, int invc)
{
    int outvc =
        getOutputUnit(outport)->select_free_vc(get_vnet(invc));
    assert(outvc != -1);
    getInputUnit(inport)->grant_outvc(invc, outvc);
    return outvc;
}

int
BusArbiter::get_vnet(int invc)
{
    int vnet = invc / m_vc_per_vnet;
    assert(vnet < (int)get_num_vnets());
    return vnet;
}

void
BusArbiter::check_for_wakeup()
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
BusArbiter::resetStats()
{
    Router::resetStats();
    m_arbiter_activity = 0;
}

} // namespace garnet
} // namespace ruby
} // namespace gem5
