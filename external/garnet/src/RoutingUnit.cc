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


#include "RoutingUnit.hh"

#include "gem5_compat/base/cast.hh"
#include "gem5_compat/base/compiler.hh"
#include "gem5_compat/debug/RubyNetwork.hh"
#include "InputUnit.hh"
#include "Router.hh"
#include "gem5_compat/mem/ruby/slicc_interface/Message.hh"

namespace gem5
{

namespace ruby
{

namespace garnet
{

RoutingUnit::RoutingUnit(Router *router)
{
    m_router = router;
    m_routing_table.clear();
    m_weight_table.clear();
}

void
RoutingUnit::addRoute(std::vector<NetDest>& routing_table_entry)
{
    if (routing_table_entry.size() > m_routing_table.size()) {
        m_routing_table.resize(routing_table_entry.size());
    }
    for (int v = 0; v < routing_table_entry.size(); v++) {
        m_routing_table[v].push_back(routing_table_entry[v]);
    }
}

void
RoutingUnit::addWeight(int link_weight)
{
    m_weight_table.push_back(link_weight);
}

bool
RoutingUnit::supportsVnet(int vnet, std::vector<int> sVnets)
{
    // If all vnets are supported, return true
    if (sVnets.size() == 0) {
        return true;
    }

    // Find the vnet in the vector, return true
    if (std::find(sVnets.begin(), sVnets.end(), vnet) != sVnets.end()) {
        return true;
    }

    // Not supported vnet
    return false;
}

/*
 * This is the default routing algorithm in garnet.
 * The routing table is populated during topology creation.
 * Routes can be biased via weight assignments in the topology file.
 * Correct weight assignments are critical to provide deadlock avoidance.
 */
int
RoutingUnit::lookupRoutingTable(int vnet, NetDest msg_destination)
{
    // First find all possible output link candidates
    // For ordered vnet, just choose the first
    // (to make sure different packets don't choose different routes)
    // For unordered vnet, randomly choose any of the links
    // To have a strict ordering between links, they should be given
    // different weights in the topology file

    int output_link = -1;
    int min_weight = INFINITE_;
    std::vector<int> output_link_candidates;
    int num_candidates = 0;

    // Identify the minimum weight among the candidate output links
    for (int link = 0; link < m_routing_table[vnet].size(); link++) {
        if (msg_destination.intersectionIsNotEmpty(
            m_routing_table[vnet][link])) {

        if (m_weight_table[link] <= min_weight)
            min_weight = m_weight_table[link];
        }
    }

    // Collect all candidate output links with this minimum weight
    for (int link = 0; link < m_routing_table[vnet].size(); link++) {
        if (msg_destination.intersectionIsNotEmpty(
            m_routing_table[vnet][link])) {

            if (m_weight_table[link] == min_weight) {
                num_candidates++;
                output_link_candidates.push_back(link);
            }
        }
    }

    if (output_link_candidates.size() == 0) {
        fatal("Fatal Error:: No Route exists from this Router.");
        exit(0);
    }

    // Randomly select any candidate output link
    int candidate = 0;
    if (!(m_router->get_net_ptr())->isVNetOrdered(vnet))
        candidate = rand() % num_candidates;

    output_link = output_link_candidates.at(candidate);
    return output_link;
}


void
RoutingUnit::addInDirection(PortDirection inport_dirn, int inport_idx)
{
    m_inports_dirn2idx[inport_dirn] = inport_idx;
    m_inports_idx2dirn[inport_idx]  = inport_dirn;
}

void
RoutingUnit::addOutDirection(PortDirection outport_dirn, int outport_idx)
{
    m_outports_dirn2idx[outport_dirn] = outport_idx;
    m_outports_idx2dirn[outport_idx]  = outport_dirn;
}

// outportCompute() is called by the InputUnit
// It calls the routing table by default.
// A template for adaptive topology-specific routing algorithm
// implementations using port directions rather than a static routing
// table is provided here.

int
RoutingUnit::outportCompute(RouteInfo route, int inport,
                            PortDirection inport_dirn)
{
    int outport = -1;

    if (route.dest_router == m_router->get_id()) {
        // Local delivery: use routing table to find the correct outport.
        // Cannot use direction-based shortcut ("Local") because topologies
        // with multiple NIs per router (BUS, CROSSBAR, H-tree) register
        // multiple outports as "Local" and the direction map only keeps
        // the last one.
        outport = lookupRoutingTable(route.vnet, route.net_dest);
        return outport;
    }

    // Routing Algorithm set in GarnetNetwork.py
    // Can be over-ridden from command line using --routing-algorithm = 1
    RoutingAlgorithm routing_algorithm =
        (RoutingAlgorithm) m_router->get_net_ptr()->getRoutingAlgorithm();

    switch (routing_algorithm) {
        case TABLE_:    outport =
            lookupRoutingTable(route.vnet, route.net_dest); break;
        case XY_:       outport =
            outportComputeXY(route, inport, inport_dirn); break;
        case CUSTOM_:   outport =
            outportComputeCustom(route, inport, inport_dirn); break;
        case DOR_:      outport =
            outportComputeDOR(route, inport, inport_dirn); break;
        case SHORTEST_: outport =
            outportComputeShortest(route, inport, inport_dirn); break;
        case DIRECT_:   outport =
            outportComputeDirect(route, inport, inport_dirn); break;
        case NCA_:      outport =
            outportComputeNCA(route, inport, inport_dirn); break;
        default: outport =
            lookupRoutingTable(route.vnet, route.net_dest); break;
    }

    assert(outport != -1);
    return outport;
}

// 1.11.94 (x03-garnet-custom-3, ruling H33): routing algorithm name for
// messages.
static const char*
routingName(int a)
{
    switch (a) {
        case TABLE_:    return "TABLE";
        case XY_:       return "XY";
        case CUSTOM_:   return "CUSTOM";
        case DOR_:      return "DOR";
        case SHORTEST_: return "SHORTEST";
        case DIRECT_:   return "DIRECT";
        case NCA_:      return "NCA";
        default:        return "UNKNOWN";
    }
}

// 1.11.94 (H33): the three direction-based algorithms ended with
// m_outports_dirn2idx[dirn], a std::map operator[] that INSERTS a missing
// key with value 0 and returns it. Outport 0 is the first external (Local)
// port, so a packet at a router without that direction was ejected to
// whichever NI sits there (DDR4 CUSTOM tree + XY: every packet 0->1..12
// came out at node 0). The only guard was assert(num_rows > 0), compiled
// out with -DNDEBUG. The lookup no longer inserts and a miss is fatal;
// GarnetNetwork::validateDirectionRouting() refuses the configuration at
// setup so the runtime fatal is a backstop that should never fire. When the
// port exists the returned index is the one operator[] returned, so every
// configuration that routed correctly before routes identically now.
int
RoutingUnit::outportForDirection(const PortDirection& dirn, int dest_id)
{
    auto it = m_outports_dirn2idx.find(dirn);
    if (it == m_outports_dirn2idx.end()) {
        GarnetNetwork* net = m_router->get_net_ptr();
        fatal("[Garnet] routing %s at router %d of topology %s chose output "
              "direction '%s' toward router %d, but router %d has no '%s' "
              "port. Use routing TABLE for this topology.",
              routingName(net->getRoutingAlgorithm()), m_router->get_id(),
              net->getTopologyName().c_str(), dirn.c_str(), dest_id,
              m_router->get_id(), dirn.c_str());
    }
    return it->second;
}

PortDirection
RoutingUnit::directionFor(int routing_algorithm, int my_id, int dest_id,
                          int num_rows, int num_cols, int num_routers,
                          bool ring_cw_only)
{
    switch (routing_algorithm) {
    case XY_: {
        // XY routing on a mesh (unchanged arithmetic).
        int my_x = my_id % num_cols;
        int my_y = my_id / num_cols;
        int dest_x = dest_id % num_cols;
        int dest_y = dest_id / num_cols;
        int x_hops = abs(dest_x - my_x);
        int y_hops = abs(dest_y - my_y);
        bool x_dirn = (dest_x >= my_x);
        bool y_dirn = (dest_y >= my_y);
        if (x_hops > 0)
            return x_dirn ? "East" : "West";
        if (y_hops > 0)
            return y_dirn ? "North" : "South";
        panic("x_hops == y_hops == 0");
    }
    case DOR_: {
        // DOR for torus: like XY but picks the shorter direction per
        // dimension (wrap-around). Unchanged arithmetic.
        int my_x = my_id % num_cols;
        int my_y = my_id / num_cols;
        int dest_x = dest_id % num_cols;
        int dest_y = dest_id / num_cols;
        int dx = dest_x - my_x;
        int dy = dest_y - my_y;
        int abs_dx = (dx >= 0) ? dx : -dx;
        int abs_dy = (dy >= 0) ? dy : -dy;
        if (abs_dx > 0) {
            if (abs_dx <= num_cols - abs_dx)
                return (dx > 0) ? "East" : "West";
            return (dx > 0) ? "West" : "East";
        }
        if (abs_dy > 0) {
            if (abs_dy <= num_rows - abs_dy)
                return (dy > 0) ? "North" : "South";
            return (dy > 0) ? "South" : "North";
        }
        panic("DOR: src == dst should not reach here");
    }
    case SHORTEST_: {
        // 1.11.94 (x03-garnet-custom-2, ruling H34): the ring size used to
        // be getNumRows()*getNumCols(), which GarnetNetwork sets to -1 * -1
        // = 1 for every non-grid topology, so cw_dist = ccw_dist = 0 and
        // every packet went clockwise (8-node ring: 0->7 crossed 8 routers
        // instead of 2). The ring size is now the number of routers the
        // ring builder made (buildRing: one router per ring position, ids
        // 0..N-1 in clockwise order), and the shorter direction is taken,
        // ties clockwise. zsim getRingHops counts min(d, N-d) over the same
        // N (numNodes_ = ring_size), so timing and energy now describe the
        // same route. A unidirectional ring has no West port anywhere
        // (ring_cw_only): clockwise is then the only, and so the shortest,
        // direction, matching getRingHops' (dst-src+N)%N for that ring.
        if (ring_cw_only)
            return "East";
        int cw_dist = ((dest_id - my_id) % num_routers + num_routers) %
                      num_routers;
        int ccw_dist = ((my_id - dest_id) % num_routers + num_routers) %
                       num_routers;
        return (cw_dist <= ccw_dist) ? "East" : "West";
    }
    default:
        return "";
    }
}

// XY routing implemented using port directions
// Only for reference purpose in a Mesh
// By default Garnet uses the routing table
int
RoutingUnit::outportComputeXY(RouteInfo route,
                              int inport,
                              PortDirection inport_dirn)
{
    GarnetNetwork* net = m_router->get_net_ptr();
    PortDirection outport_dirn = directionFor(XY_, m_router->get_id(),
        route.dest_router, net->getNumRows(), net->getNumCols(),
        net->getNumRouters(), net->isRingClockwiseOnly());

    // Reference-only inport checks (compiled out in Release).
    if (outport_dirn == "East")
        assert(inport_dirn == "Local" || inport_dirn == "West");
    else if (outport_dirn == "West")
        assert(inport_dirn == "Local" || inport_dirn == "East");
    else if (outport_dirn == "North")
        assert(inport_dirn != "North");
    else if (outport_dirn == "South")
        assert(inport_dirn != "South");

    return outportForDirection(outport_dirn, route.dest_router);
}

// User extension point. Defaults to TABLE routing.
// Modify this function to implement custom per-hop routing.
int
RoutingUnit::outportComputeCustom(RouteInfo route,
                                 int inport,
                                 PortDirection inport_dirn)
{
    return lookupRoutingTable(route.vnet, route.net_dest);
}

// DOR (Dimension-Order Routing) for torus: like XY but picks
// shorter direction per dimension (handles wrap-around)
int
RoutingUnit::outportComputeDOR(RouteInfo route,
                               int inport,
                               PortDirection inport_dirn)
{
    GarnetNetwork* net = m_router->get_net_ptr();
    PortDirection outport_dirn = directionFor(DOR_, m_router->get_id(),
        route.dest_router, net->getNumRows(), net->getNumCols(),
        net->getNumRouters(), net->isRingClockwiseOnly());
    return outportForDirection(outport_dirn, route.dest_router);
}

// SHORTEST routing for ring: pick CW or CCW based on shorter distance
int
RoutingUnit::outportComputeShortest(RouteInfo route,
                                    int inport,
                                    PortDirection inport_dirn)
{
    GarnetNetwork* net = m_router->get_net_ptr();
    PortDirection outport_dirn = directionFor(SHORTEST_, m_router->get_id(),
        route.dest_router, net->getNumRows(), net->getNumCols(),
        net->getNumRouters(), net->isRingClockwiseOnly());
    return outportForDirection(outport_dirn, route.dest_router);
}

// DIRECT routing for crossbar/bus: single-hop via routing table
int
RoutingUnit::outportComputeDirect(RouteInfo route,
                                  int inport,
                                  PortDirection inport_dirn)
{
    // Single-hop guaranteed by topology; use table for port lookup
    return lookupRoutingTable(route.vnet, route.net_dest);
}

// NCA (Nearest Common Ancestor) routing for fat-tree/h-tree:
// Route up toward parent until reaching LCA, then down toward destination
int
RoutingUnit::outportComputeNCA(RouteInfo route,
                               int inport,
                               PortDirection inport_dirn)
{
    // Use the routing table which is pre-populated with correct
    // up/down paths during topology construction. The NCA algorithm
    // is encoded in the table entries by the topology builder.
    return lookupRoutingTable(route.vnet, route.net_dest);
}

} // namespace garnet
} // namespace ruby
} // namespace gem5
