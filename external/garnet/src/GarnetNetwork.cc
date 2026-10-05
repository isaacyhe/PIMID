/*
 * Copyright (c) 2020 Advanced Micro Devices, Inc.
 * Copyright (c) 2008 Princeton University
 * Copyright (c) 2016 Georgia Institute of Technology
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "GarnetNetwork.hh"

#include <array>
#include <cassert>
#include <string>
#include <vector>

#include "gem5_compat/base/cast.hh"
#include "gem5_compat/base/compiler.hh"
#include "gem5_compat/base/trace.hh"
#include "gem5_compat/debug/RubyNetwork.hh"
#include "gem5_compat/mem/ruby/protocol/LinkDirection.hh"
#include "gem5_compat/mem/ruby/common/NetDest.hh"
#include "gem5_compat/mem/ruby/network/MessageBuffer.hh"
#include "CommonTypes.hh"
#include "CreditLink.hh"
#include "GarnetLink.hh"
#include "NetworkInterface.hh"
#include "NetworkLink.hh"
#include "Router.hh"
#include "InputUnit.hh"
#include "OutputUnit.hh"
#include "flit.hh"
#include "gem5_compat/mem/ruby/system/RubySystem.hh"

namespace gem5
{

namespace ruby
{

namespace garnet
{

/*
 * GarnetNetwork sets up the routers and links and collects stats.
 * Default parameters (GarnetNetwork.py) can be overwritten from command line
 * (see configs/network/Network.py)
 */

GarnetNetwork::GarnetNetwork(const Params &p)
    : Network(p)
{
    m_num_rows = p.num_rows;
    m_ni_flit_size = p.ni_flit_size;
    m_max_vcs_per_vnet = 0;
    m_buffers_per_data_vc = p.buffers_per_data_vc;
    m_buffers_per_ctrl_vc = p.buffers_per_ctrl_vc;
    m_routing_algorithm = p.routing_algorithm;
    m_next_packet_id = 0;

    m_enable_fault_model = p.enable_fault_model;
    if (m_enable_fault_model)
        fault_model = p.fault_model;

    // Apply configurable message sizes (0 = keep defaults)
    if (p.control_msg_size_bits > 0)
        m_control_msg_size = p.control_msg_size_bits;
    if (p.data_msg_size_bits > 0)
        m_data_msg_size = p.data_msg_size_bits;

    m_vnet_type.resize(m_virtual_networks);

    for (int i = 0 ; i < m_virtual_networks ; i++) {
        if (m_vnet_type_names[i] == "response")
            m_vnet_type[i] = DATA_VNET_; // carries data (and ctrl) packets
        else
            m_vnet_type[i] = CTRL_VNET_; // carries only ctrl packets
    }

    // record the routers
    for (auto* router : p.routers) {
        m_routers.push_back(router);
        // initialize the router's network pointers
        router->init_net_ptr(this);
    }

    // record the network interfaces
    for (auto* ni : p.netifs) {
        m_nis.push_back(ni);
        ni->init_net_ptr(this);
    }

    // Print Garnet version
    inform("Garnet version %s\n", garnetVersion);
}

void
GarnetNetwork::init()
{
    Network::init();

    for (int i=0; i < m_nodes; i++) {
        m_nis[i]->addNode(m_toNetQueues[i], m_fromNetQueues[i]);
    }

    // The topology pointer should have already been initialized in the
    // parent network constructor
    assert(m_topology_ptr != NULL);
    m_topology_ptr->createRoutingTable(this);
    m_topology_ptr->createLinks(this);

    // Initialize routers (SA, CrossbarSwitch) now that all ports are added.
    // In gem5, SimObject::init() is called by the framework on every object;
    // in our standalone flow we must do it explicitly after createLinks().
    for (int i = 0; i < m_routers.size(); i++) {
        m_routers[i]->init();
    }

    // Initialize topology specific parameters
    if (getNumRows() > 0) {
        // Only for Mesh topology
        // m_num_rows and m_num_cols are only used for
        // implementing XY or custom routing in RoutingUnit.cc
        m_num_rows = getNumRows();
        m_num_cols = m_routers.size() / m_num_rows;
        assert(m_num_rows * m_num_cols == m_routers.size());
    } else {
        m_num_rows = -1;
        m_num_cols = -1;
    }

    // 1.11.94 (x03-garnet-custom-2): a ring whose routers have no "West"
    // outport is the unidirectional ring; SHORTEST then goes clockwise.
    m_ring_cw_only = true;
    for (auto* r : m_routers) {
        for (int o = 0; o < r->get_num_outports(); o++) {
            if (r->getOutportDirection(o) == "West") {
                m_ring_cw_only = false;
                break;
            }
        }
        if (!m_ring_cw_only) break;
    }

    validateDirectionRouting();

    // FaultModel: declare each router to the fault model
    if (isFaultModelEnabled()) {
        for (std::vector<Router*>::const_iterator i= m_routers.begin();
             i != m_routers.end(); ++i) {
            Router* router = safe_cast<Router*>(*i);
            [[maybe_unused]] int router_id =
                fault_model->declare_router(router->get_num_inports(),
                                            router->get_num_outports(),
                                            router->get_vc_per_vnet(),
                                            getBuffersPerDataVC(),
                                            getBuffersPerCtrlVC());
            assert(router_id == router->get_id());
            router->printAggregateFaultProbability(std::cout);
            router->printFaultVector(std::cout);
        }
    }
}

/*
 * This function creates a link from the Network Interface (NI)
 * into the Network.
 * It creates a Network Link from the NI to a Router and a Credit Link from
 * the Router to the NI
*/

void
GarnetNetwork::makeExtInLink(NodeID global_src, SwitchID dest, BasicLink* link,
                             std::vector<NetDest>& routing_table_entry)
{
    NodeID local_src = getLocalNodeID(global_src);
    assert(local_src < m_nodes);

    GarnetExtLink* garnet_link = safe_cast<GarnetExtLink*>(link);

    // GarnetExtLink is bi-directional
    NetworkLink* net_link = garnet_link->m_network_links[LinkDirection_In];
    net_link->setType(EXT_IN_);
    CreditLink* credit_link = garnet_link->m_credit_links[LinkDirection_In];

    m_networklinks.push_back(net_link);
    m_creditlinks.push_back(credit_link);

    PortDirection dst_inport_dirn = "Local";

    m_max_vcs_per_vnet = std::max(m_max_vcs_per_vnet,
                             m_routers[dest]->get_vc_per_vnet());

    /*
     * We check if a bridge was enabled at any end of the link.
     * The bridge is enabled if either of clock domain
     * crossing (CDC) or Serializer-Deserializer(SerDes) unit is
     * enabled for the link at each end. The bridge encapsulates
     * the functionality for both CDC and SerDes and is a Consumer
     * object similiar to a NetworkLink.
     *
     * If a bridge was enabled we connect the NI and Routers to
     * bridge before connecting the link. Example, if an external
     * bridge is enabled, we would connect:
     * NI--->NetworkBridge--->GarnetExtLink---->Router
     */
    if (garnet_link->extBridgeEn) {
        DPRINTF(RubyNetwork, "Enable external bridge for %s\n",
            garnet_link->name());
        NetworkBridge *n_bridge = garnet_link->extNetBridge[LinkDirection_In];
        m_nis[local_src]->
        addOutPort(n_bridge,
                   garnet_link->extCredBridge[LinkDirection_In],
                   dest, m_routers[dest]->get_vc_per_vnet());
        m_networkbridges.push_back(n_bridge);
    } else {
        m_nis[local_src]->addOutPort(net_link, credit_link, dest,
            m_routers[dest]->get_vc_per_vnet());
    }

    if (garnet_link->intBridgeEn) {
        DPRINTF(RubyNetwork, "Enable internal bridge for %s\n",
            garnet_link->name());
        NetworkBridge *n_bridge = garnet_link->intNetBridge[LinkDirection_In];
        m_routers[dest]->
            addInPort(dst_inport_dirn,
                      n_bridge,
                      garnet_link->intCredBridge[LinkDirection_In]);
        m_networkbridges.push_back(n_bridge);
    } else {
        m_routers[dest]->addInPort(dst_inport_dirn, net_link, credit_link);
    }

}

/*
 * This function creates a link from the Network to a NI.
 * It creates a Network Link from a Router to the NI and
 * a Credit Link from NI to the Router
*/

void
GarnetNetwork::makeExtOutLink(SwitchID src, NodeID global_dest,
                              BasicLink* link,
                              std::vector<NetDest>& routing_table_entry)
{
    NodeID local_dest = getLocalNodeID(global_dest);
    assert(local_dest < m_nodes);
    assert(src < m_routers.size());
    assert(m_routers[src] != NULL);

    GarnetExtLink* garnet_link = safe_cast<GarnetExtLink*>(link);

    // GarnetExtLink is bi-directional
    NetworkLink* net_link = garnet_link->m_network_links[LinkDirection_Out];
    net_link->setType(EXT_OUT_);
    CreditLink* credit_link = garnet_link->m_credit_links[LinkDirection_Out];

    m_networklinks.push_back(net_link);
    m_creditlinks.push_back(credit_link);

    PortDirection src_outport_dirn = "Local";

    m_max_vcs_per_vnet = std::max(m_max_vcs_per_vnet,
                             m_routers[src]->get_vc_per_vnet());

    /*
     * We check if a bridge was enabled at any end of the link.
     * The bridge is enabled if either of clock domain
     * crossing (CDC) or Serializer-Deserializer(SerDes) unit is
     * enabled for the link at each end. The bridge encapsulates
     * the functionality for both CDC and SerDes and is a Consumer
     * object similiar to a NetworkLink.
     *
     * If a bridge was enabled we connect the NI and Routers to
     * bridge before connecting the link. Example, if an external
     * bridge is enabled, we would connect:
     * NI<---NetworkBridge<---GarnetExtLink<----Router
     */
    if (garnet_link->extBridgeEn) {
        DPRINTF(RubyNetwork, "Enable external bridge for %s\n",
            garnet_link->name());
        NetworkBridge *n_bridge = garnet_link->extNetBridge[LinkDirection_Out];
        m_nis[local_dest]->
            addInPort(n_bridge, garnet_link->extCredBridge[LinkDirection_Out]);
        m_networkbridges.push_back(n_bridge);
    } else {
        m_nis[local_dest]->addInPort(net_link, credit_link);
    }

    if (garnet_link->intBridgeEn) {
        DPRINTF(RubyNetwork, "Enable internal bridge for %s\n",
            garnet_link->name());
        NetworkBridge *n_bridge = garnet_link->intNetBridge[LinkDirection_Out];
        m_routers[src]->
            addOutPort(src_outport_dirn,
                       n_bridge,
                       routing_table_entry, link->get_weight(),
                       garnet_link->intCredBridge[LinkDirection_Out],
                       m_routers[src]->get_vc_per_vnet());
        m_networkbridges.push_back(n_bridge);
    } else {
        m_routers[src]->
            addOutPort(src_outport_dirn, net_link,
                       routing_table_entry,
                       link->get_weight(), credit_link,
                       m_routers[src]->get_vc_per_vnet());
    }
}

/*
 * This function creates an internal network link between two routers.
 * It adds both the network link and an opposite credit link.
*/

void
GarnetNetwork::makeInternalLink(SwitchID src, SwitchID dest, BasicLink* link,
                                std::vector<NetDest>& routing_table_entry,
                                PortDirection src_outport_dirn,
                                PortDirection dst_inport_dirn)
{
    GarnetIntLink* garnet_link = safe_cast<GarnetIntLink*>(link);

    // GarnetIntLink is unidirectional
    NetworkLink* net_link = garnet_link->m_network_link;
    net_link->setType(INT_);
    CreditLink* credit_link = garnet_link->m_credit_link;

    m_networklinks.push_back(net_link);
    m_creditlinks.push_back(credit_link);

    m_max_vcs_per_vnet = std::max(m_max_vcs_per_vnet,
                             std::max(m_routers[dest]->get_vc_per_vnet(),
                             m_routers[src]->get_vc_per_vnet()));

    /*
     * We check if a bridge was enabled at any end of the link.
     * The bridge is enabled if either of clock domain
     * crossing (CDC) or Serializer-Deserializer(SerDes) unit is
     * enabled for the link at each end. The bridge encapsulates
     * the functionality for both CDC and SerDes and is a Consumer
     * object similiar to a NetworkLink.
     *
     * If a bridge was enabled we connect the NI and Routers to
     * bridge before connecting the link. Example, if a source
     * bridge is enabled, we would connect:
     * Router--->NetworkBridge--->GarnetIntLink---->Router
     */
    if (garnet_link->dstBridgeEn) {
        DPRINTF(RubyNetwork, "Enable destination bridge for %s\n",
            garnet_link->name());
        NetworkBridge *n_bridge = garnet_link->dstNetBridge;
        m_routers[dest]->addInPort(dst_inport_dirn, n_bridge,
                                   garnet_link->dstCredBridge);
        m_networkbridges.push_back(n_bridge);
    } else {
        m_routers[dest]->addInPort(dst_inport_dirn, net_link, credit_link);
    }

    if (garnet_link->srcBridgeEn) {
        DPRINTF(RubyNetwork, "Enable source bridge for %s\n",
            garnet_link->name());
        NetworkBridge *n_bridge = garnet_link->srcNetBridge;
        m_routers[src]->
            addOutPort(src_outport_dirn, n_bridge,
                       routing_table_entry,
                       link->get_weight(), garnet_link->srcCredBridge,
                       m_routers[dest]->get_vc_per_vnet());
        m_networkbridges.push_back(n_bridge);
    } else {
        m_routers[src]->addOutPort(src_outport_dirn, net_link,
                        routing_table_entry,
                        link->get_weight(), credit_link,
                        m_routers[dest]->get_vc_per_vnet());
    }
}

// 1.11.94 (x03-garnet-custom-3, ruling H33): build-time check for the
// direction-based routings. XY, DOR and SHORTEST choose an output by port
// NAME (East/West/North/South) as a pure function of (current router,
// destination router), so walking every (source, destination) pair over the
// topology's links visits every decision any packet can ever take. The
// walk refuses (fatal, naming topology, routing, router and direction) when
// a router lacks the chosen port -- the case that used to eject the packet
// at the wrong NI through map operator[] -> port 0 -- and when a walk does
// not reach its destination within one visit per router (a routing that
// loops on this topology). XY and DOR also need the rows x cols grid; a
// topology built without one (num_rows <= 0, e.g. the CUSTOM DRAM tree
// after a user-set noc.routing XY) is refused before the walk. TABLE,
// CUSTOM, DIRECT and NCA use the routing table and are not checked here.
// On a configuration that routes correctly the walk changes no state.
void
GarnetNetwork::validateDirectionRouting()
{
    int alg = m_routing_algorithm;
    if (alg != XY_ && alg != DOR_ && alg != SHORTEST_)
        return;
    const char* rname = (alg == XY_) ? "XY" : (alg == DOR_) ? "DOR"
                                                             : "SHORTEST";
    int R = (int)m_routers.size();
    std::string tname = getTopologyName();

    if (alg == XY_ || alg == DOR_) {
        if (m_num_rows <= 0 || m_num_cols <= 0 ||
            m_num_rows * m_num_cols != R) {
            fatal("[Garnet] routing %s needs a rows x cols router grid, but "
                  "topology %s has %d routers and no grid (num_rows=%d). "
                  "Use routing TABLE for this topology.",
                  rname, tname.c_str(), R, m_num_rows);
        }
    }
    if (R <= 1)
        return;

    // next[r][k]: router reached from r through its port named kDir[k]
    // (-1: no such port). Later links overwrite earlier ones, as
    // RoutingUnit::addOutDirection does for a repeated name.
    static const char* kDir[4] = {"East", "West", "North", "South"};
    std::vector<std::array<int, 4>> next(R, {{-1, -1, -1, -1}});
    for (auto* l : m_topology_ptr->getInternalLinks()) {
        const std::string& d = l->getSrcOutport();
        for (int k = 0; k < 4; k++) {
            if (d == kDir[k] && (int)l->getSrcNode() < R)
                next[l->getSrcNode()][k] = (int)l->getDstNode();
        }
    }

    for (int src = 0; src < R; src++) {
        for (int dst = 0; dst < R; dst++) {
            if (src == dst) continue;
            int cur = src;
            int steps = 0;
            while (cur != dst) {
                PortDirection dirn = RoutingUnit::directionFor(
                    alg, cur, dst, m_num_rows, m_num_cols, R,
                    m_ring_cw_only);
                int k = -1;
                for (int j = 0; j < 4; j++)
                    if (dirn == kDir[j]) k = j;
                if (k < 0 || next[cur][k] < 0) {
                    fatal("[Garnet] routing %s is refused on topology %s: "
                          "router %d has no '%s' output port, which %s needs "
                          "on the path from router %d to router %d. Use "
                          "routing TABLE for this topology.",
                          rname, tname.c_str(), cur, dirn.c_str(), rname,
                          src, dst);
                }
                cur = next[cur][k];
                if (++steps > R) {
                    fatal("[Garnet] routing %s is refused on topology %s: "
                          "the path from router %d to router %d does not "
                          "reach its destination within %d hops. Use routing "
                          "TABLE for this topology.",
                          rname, tname.c_str(), src, dst, R);
                }
            }
        }
    }
}

// Total routers in the network
int
GarnetNetwork::getNumRouters()
{
    return m_routers.size();
}

// Get ID of router connected to a NI.
int
GarnetNetwork::get_router_id(int global_ni, int vnet)
{
    NodeID local_ni = getLocalNodeID(global_ni);

    return m_nis[local_ni]->get_router_id(vnet);
}

// -- Deadlock-state dump (diagnostic) ----------------------------------------
// For every input VC that is requesting Switch Allocation (i.e. has a flit that
// WANTS to move) but is BLOCKED, print what it is waiting for: its up/down class,
// the outport it wants + that outport's direction, the destination router, the
// required output VC class, and whether a free output VC in that class / credit
// is available. The set of blocked VCs and their wait-for edges reveals the
// deadlock cycle and proves whether up/down VC-class separation is holding.
void
GarnetNetwork::dumpDeadlockState(Tick curTime, const char* tag)
{
    fprintf(stderr, "[Deadlock:%s] tick=%lu routers=%zu\n",
            tag, (unsigned long)curTime, m_routers.size());
    int total_block = 0, total_wantmove = 0;
    for (auto* r : m_routers) {
        int vpv  = r->get_vc_per_vnet();
        int half = vpv / 2; if (half <= 0) half = 1;
        int nin  = r->get_num_inports();
        int rblock = 0;
        for (int ip = 0; ip < nin; ip++) {
            InputUnit* iu = r->getInputUnit(ip);
            PortDirection indir = iu->get_direction();
            for (int vc = 0; vc < (int)r->get_num_vcs(); vc++) {
                if (!iu->need_stage(vc, SA_, curTime)) continue;
                total_wantmove++;
                flit* f      = iu->peekTopFlit(vc);
                int   dest   = f ? f->get_route().dest_router : -1;
                int   ftype  = f ? (int)f->get_type() : -1;
                int   outport= iu->get_outport(vc);
                PortDirection odir = r->getOutportDirection(outport);
                int   incls  = (vc % vpv < half) ? 0 : 1;
                int   tgt    = (odir == "Down") ? 1
                               : (indir == "Local" ? 0 : incls);
                int   base   = (vc / vpv) * vpv;
                int   lo     = base + (tgt == 0 ? 0    : half);
                int   hi     = base + (tgt == 0 ? half : vpv);
                OutputUnit* ou = r->getOutputUnit(outport);
                int   outvc  = iu->get_outvc(vc);
                bool  freeCls= ou->has_free_vc_in_range(lo, hi);
                bool  credit = (outvc != -1) ? ou->has_credit(outvc) : false;
                bool  blocked= (outvc == -1) ? !freeCls : !credit;
                if (!blocked) continue;
                rblock++; total_block++;
                fprintf(stderr,
                    "  R%d in%d(%s) vc%d[c%d] -> out%d(%s) destR%d ft%d "
                    "needCls%d outvc%d freeVCinCls=%d credit=%d\n",
                    r->get_id(), ip, indir.c_str(), vc, incls, outport,
                    odir.c_str(), dest, ftype, tgt, outvc,
                    (int)freeCls, (int)credit);
            }
        }
        if (rblock)
            fprintf(stderr, "  -- R%d: %d blocked VCs (vpv=%d half=%d)\n",
                    r->get_id(), rblock, vpv, half);
    }
    fprintf(stderr, "[Deadlock:%s] end: %d blocked / %d want-move\n",
            tag, total_block, total_wantmove);

    // NI flow-control state -- the sink side of the wait-for graph.
    fprintf(stderr, "[Deadlock:%s] NI state (only NIs with pending state):\n", tag);
    for (auto* ni : m_nis) ni->dumpState();
    fprintf(stderr, "[Deadlock:%s] NI state end\n", tag);
}

void
GarnetNetwork::pokeAllConsumers()
{
    // Force a next-cycle wakeup on EVERY Consumer: NIs, routers, network links,
    // and credit links. This is the ultimate lost-wakeup-vs-structural test --
    // if nothing in the network can be asleep and it STILL wedges, the deadlock
    // is a genuine resource cycle (structural). If poking the LINKS (which the
    // NI/router-only pokes missed) clears it, a link wakeup was being lost.
    for (auto* ni : m_nis)          ni->scheduleEvent(Cycles(1));
    for (auto* r  : m_routers)      r->scheduleEvent(Cycles(1));
    for (auto* l  : m_networklinks) l->scheduleEvent(Cycles(1));
    for (auto* c  : m_creditlinks)  c->scheduleEvent(Cycles(1));
}

void
GarnetNetwork::regStats()
{
    Network::regStats();

    // Packets
    m_packets_received
        .init(m_virtual_networks)
        .name(name() + ".packets_received")
        .flags(statistics::pdf | statistics::total | statistics::nozero |
            statistics::oneline)
        ;

    m_packets_injected
        .init(m_virtual_networks)
        .name(name() + ".packets_injected")
        .flags(statistics::pdf | statistics::total | statistics::nozero |
            statistics::oneline)
        ;

    m_packet_network_latency
        .init(m_virtual_networks)
        .name(name() + ".packet_network_latency")
        .flags(statistics::oneline)
        ;

    m_packet_queueing_latency
        .init(m_virtual_networks)
        .name(name() + ".packet_queueing_latency")
        .flags(statistics::oneline)
        ;

    for (int i = 0; i < m_virtual_networks; i++) {
        m_packets_received.subname(i, csprintf("vnet-%i", i));
        m_packets_injected.subname(i, csprintf("vnet-%i", i));
        m_packet_network_latency.subname(i, csprintf("vnet-%i", i));
        m_packet_queueing_latency.subname(i, csprintf("vnet-%i", i));
    }

    m_avg_packet_vnet_latency
        .name(name() + ".average_packet_vnet_latency")
        .flags(statistics::oneline);
    m_avg_packet_vnet_latency =
        m_packet_network_latency / m_packets_received;

    m_avg_packet_vqueue_latency
        .name(name() + ".average_packet_vqueue_latency")
        .flags(statistics::oneline);
    m_avg_packet_vqueue_latency =
        m_packet_queueing_latency / m_packets_received;

    m_avg_packet_network_latency
        .name(name() + ".average_packet_network_latency");
    m_avg_packet_network_latency =
        sum(m_packet_network_latency) / sum(m_packets_received);

    m_avg_packet_queueing_latency
        .name(name() + ".average_packet_queueing_latency");
    m_avg_packet_queueing_latency
        = sum(m_packet_queueing_latency) / sum(m_packets_received);

    m_avg_packet_latency
        .name(name() + ".average_packet_latency");
    m_avg_packet_latency
        = m_avg_packet_network_latency + m_avg_packet_queueing_latency;

    // Flits
    m_flits_received
        .init(m_virtual_networks)
        .name(name() + ".flits_received")
        .flags(statistics::pdf | statistics::total | statistics::nozero |
            statistics::oneline)
        ;

    m_flits_injected
        .init(m_virtual_networks)
        .name(name() + ".flits_injected")
        .flags(statistics::pdf | statistics::total | statistics::nozero |
            statistics::oneline)
        ;

    m_flit_network_latency
        .init(m_virtual_networks)
        .name(name() + ".flit_network_latency")
        .flags(statistics::oneline)
        ;

    m_flit_queueing_latency
        .init(m_virtual_networks)
        .name(name() + ".flit_queueing_latency")
        .flags(statistics::oneline)
        ;

    for (int i = 0; i < m_virtual_networks; i++) {
        m_flits_received.subname(i, csprintf("vnet-%i", i));
        m_flits_injected.subname(i, csprintf("vnet-%i", i));
        m_flit_network_latency.subname(i, csprintf("vnet-%i", i));
        m_flit_queueing_latency.subname(i, csprintf("vnet-%i", i));
    }

    m_avg_flit_vnet_latency
        .name(name() + ".average_flit_vnet_latency")
        .flags(statistics::oneline);
    m_avg_flit_vnet_latency = m_flit_network_latency / m_flits_received;

    m_avg_flit_vqueue_latency
        .name(name() + ".average_flit_vqueue_latency")
        .flags(statistics::oneline);
    m_avg_flit_vqueue_latency =
        m_flit_queueing_latency / m_flits_received;

    m_avg_flit_network_latency
        .name(name() + ".average_flit_network_latency");
    m_avg_flit_network_latency =
        sum(m_flit_network_latency) / sum(m_flits_received);

    m_avg_flit_queueing_latency
        .name(name() + ".average_flit_queueing_latency");
    m_avg_flit_queueing_latency =
        sum(m_flit_queueing_latency) / sum(m_flits_received);

    m_avg_flit_latency
        .name(name() + ".average_flit_latency");
    m_avg_flit_latency =
        m_avg_flit_network_latency + m_avg_flit_queueing_latency;


    // Hops
    m_avg_hops.name(name() + ".average_hops");
    m_avg_hops = m_total_hops / sum(m_flits_received);

    // Links
    m_total_ext_in_link_utilization
        .name(name() + ".ext_in_link_utilization");
    m_total_ext_out_link_utilization
        .name(name() + ".ext_out_link_utilization");
    m_total_int_link_utilization
        .name(name() + ".int_link_utilization");
    m_average_link_utilization
        .name(name() + ".avg_link_utilization");
    m_average_vc_load
        .init(m_virtual_networks * m_max_vcs_per_vnet)
        .name(name() + ".avg_vc_load")
        .flags(statistics::pdf | statistics::total | statistics::nozero |
            statistics::oneline)
        ;

    // Traffic distribution
    for (int source = 0; source < m_routers.size(); ++source) {
        m_data_traffic_distribution.push_back(
            std::vector<statistics::Scalar *>());
        m_ctrl_traffic_distribution.push_back(
            std::vector<statistics::Scalar *>());

        for (int dest = 0; dest < m_routers.size(); ++dest) {
            statistics::Scalar *data_packets = new statistics::Scalar();
            statistics::Scalar *ctrl_packets = new statistics::Scalar();

            data_packets->name(name() + ".data_traffic_distribution." + "n" +
                    std::to_string(source) + "." + "n" + std::to_string(dest));
            m_data_traffic_distribution[source].push_back(data_packets);

            ctrl_packets->name(name() + ".ctrl_traffic_distribution." + "n" +
                    std::to_string(source) + "." + "n" + std::to_string(dest));
            m_ctrl_traffic_distribution[source].push_back(ctrl_packets);
        }
    }
}

void
GarnetNetwork::collateStats()
{
    RubySystem *rs = rubySystem_;
    double time_delta = double(curCycle() - rs->getStartCycle());

    for (int i = 0; i < m_networklinks.size(); i++) {
        link_type type = m_networklinks[i]->getType();
        int activity = m_networklinks[i]->getLinkUtilization();

        if (type == EXT_IN_)
            m_total_ext_in_link_utilization += activity;
        else if (type == EXT_OUT_)
            m_total_ext_out_link_utilization += activity;
        else if (type == INT_)
            m_total_int_link_utilization += activity;

        m_average_link_utilization +=
            (double(activity) / time_delta);

        std::vector<unsigned int> vc_load = m_networklinks[i]->getVcLoad();
        for (int j = 0; j < vc_load.size(); j++) {
            m_average_vc_load[j] += ((double)vc_load[j] / time_delta);
        }
    }

    // Ask the routers to collate their statistics
    for (int i = 0; i < m_routers.size(); i++) {
        m_routers[i]->collateStats();
    }
}

void
GarnetNetwork::resetStats()
{
    for (int i = 0; i < m_routers.size(); i++) {
        m_routers[i]->resetStats();
    }
    for (int i = 0; i < m_networklinks.size(); i++) {
        m_networklinks[i]->resetStats();
    }
    for (int i = 0; i < m_creditlinks.size(); i++) {
        m_creditlinks[i]->resetStats();
    }
}

void
GarnetNetwork::print(std::ostream& out) const
{
    out << "[GarnetNetwork]";
}

void
GarnetNetwork::update_traffic_distribution(RouteInfo route)
{
    int src_node = route.src_router;
    int dest_node = route.dest_router;
    int vnet = route.vnet;

    if (m_vnet_type[vnet] == DATA_VNET_)
        (*m_data_traffic_distribution[src_node][dest_node])++;
    else
        (*m_ctrl_traffic_distribution[src_node][dest_node])++;
}

bool
GarnetNetwork::functionalRead(Packet *pkt, WriteMask &mask)
{
    bool read = false;
    for (unsigned int i = 0; i < m_routers.size(); i++) {
        if (m_routers[i]->functionalRead(pkt, mask))
            read = true;
    }

    for (unsigned int i = 0; i < m_nis.size(); ++i) {
        if (m_nis[i]->functionalRead(pkt, mask))
            read = true;
    }

    for (unsigned int i = 0; i < m_networklinks.size(); ++i) {
        if (m_networklinks[i]->functionalRead(pkt, mask))
            read = true;
    }

    for (unsigned int i = 0; i < m_networkbridges.size(); ++i) {
        if (m_networkbridges[i]->functionalRead(pkt, mask))
            read = true;
    }

    return read;
}

uint32_t
GarnetNetwork::functionalWrite(Packet *pkt)
{
    uint32_t num_functional_writes = 0;

    for (unsigned int i = 0; i < m_routers.size(); i++) {
        num_functional_writes += m_routers[i]->functionalWrite(pkt);
    }

    for (unsigned int i = 0; i < m_nis.size(); ++i) {
        num_functional_writes += m_nis[i]->functionalWrite(pkt);
    }

    for (unsigned int i = 0; i < m_networklinks.size(); ++i) {
        num_functional_writes += m_networklinks[i]->functionalWrite(pkt);
    }

    return num_functional_writes;
}

void
GarnetNetwork::resetNetworkState()
{
    // 1. Clear the global EventQueue (prevents stale events from firing)
    gem5::EventQueue::instance().clear();

    // 2. Reset all NetworkInterfaces
    for (auto* ni : m_nis) {
        ni->resetState();
    }

    // 3. Reset all Routers (which reset their InputUnits and OutputUnits)
    for (auto* router : m_routers) {
        router->resetState();
    }

    // 4. Reset all data links
    for (auto* link : m_networklinks) {
        link->resetState();
    }

    // 5. Reset all credit links
    for (auto* link : m_creditlinks) {
        link->resetState();
    }

    // 6. Reset all network bridges (if any)
    for (auto* bridge : m_networkbridges) {
        bridge->resetState();
    }

    // 7. Reset packet ID counter
    m_next_packet_id = 0;

    // 8. Reset global tick to 0
    gem5::setCurTick(0);
}

} // namespace garnet
} // namespace ruby
} // namespace gem5
