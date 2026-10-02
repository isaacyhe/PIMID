/*
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


#ifndef __MEM_RUBY_NETWORK_GARNET_0_ROUTINGUNIT_HH__
#define __MEM_RUBY_NETWORK_GARNET_0_ROUTINGUNIT_HH__

#include "gem5_compat/mem/ruby/common/Consumer.hh"
#include "gem5_compat/mem/ruby/common/NetDest.hh"
#include "CommonTypes.hh"
#include "GarnetNetwork.hh"
#include "flit.hh"

namespace gem5
{

namespace ruby
{

namespace garnet
{

class InputUnit;
class Router;

class RoutingUnit
{
  public:
    RoutingUnit(Router *router);
    int outportCompute(RouteInfo route,
                      int inport,
                      PortDirection inport_dirn);

    // Topology-agnostic Routing Table based routing (default)
    void addRoute(std::vector<NetDest>& routing_table_entry);
    void addWeight(int link_weight);

    // get output port from routing table
    int  lookupRoutingTable(int vnet, NetDest net_dest);

    // Topology-specific direction based routing
    void addInDirection(PortDirection inport_dirn, int inport);
    void addOutDirection(PortDirection outport_dirn, int outport);

    // Routing for Mesh
    int outportComputeXY(RouteInfo route,
                         int inport,
                         PortDirection inport_dirn);

    // Custom Routing Algorithm using Port Directions
    int outportComputeCustom(RouteInfo route,
                             int inport,
                             PortDirection inport_dirn);

    // Dimension-order routing with dateline for torus
    int outportComputeDOR(RouteInfo route,
                          int inport,
                          PortDirection inport_dirn);

    // Shortest-path routing for ring
    int outportComputeShortest(RouteInfo route,
                               int inport,
                               PortDirection inport_dirn);

    // Single-hop direct routing for crossbar/bus
    int outportComputeDirect(RouteInfo route,
                             int inport,
                             PortDirection inport_dirn);

    // Nearest Common Ancestor routing for fat-tree/h-tree
    int outportComputeNCA(RouteInfo route,
                          int inport,
                          PortDirection inport_dirn);

    // 1.11.94 (x03-garnet-custom-3, ruling H33): the direction XY, DOR or
    // SHORTEST picks at router my_id for a packet bound to router dest_id.
    // A pure function of the ids and the grid, shared by the per-hop routing
    // below and by GarnetNetwork::validateDirectionRouting(), which walks
    // every (router, destination) pair at setup and refuses a routing that
    // names a port some router lacks. Returns "" for any other algorithm.
    // num_rows/num_cols are the grid (XY, DOR); num_routers and
    // ring_cw_only describe the ring (SHORTEST).
    static PortDirection directionFor(int routing_algorithm,
                                      int my_id, int dest_id,
                                      int num_rows, int num_cols,
                                      int num_routers, bool ring_cw_only);

    // 1.11.94 (H33): non-inserting port lookup. hasOutDirection() answers;
    // outportForDirection() refuses (fatal, naming router, direction and
    // topology) when the router has no port by that name.
    bool hasOutDirection(const PortDirection& dirn) const
    { return m_outports_dirn2idx.count(dirn) != 0; }
    int outportForDirection(const PortDirection& dirn, int dest_id);

    // Returns true if vnet is present in the vector
    // of vnets or if the vector supports all vnets.
    bool supportsVnet(int vnet, std::vector<int> sVnets);


  private:
    Router *m_router;

    // Routing Table
    std::vector<std::vector<NetDest>> m_routing_table;
    std::vector<int> m_weight_table;

    // Inport and Outport direction to idx maps
    std::map<PortDirection, int> m_inports_dirn2idx;
    std::map<int, PortDirection> m_inports_idx2dirn;
    std::map<int, PortDirection> m_outports_idx2dirn;
    std::map<PortDirection, int> m_outports_dirn2idx;
};

} // namespace garnet
} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_NETWORK_GARNET_0_ROUTINGUNIT_HH__
