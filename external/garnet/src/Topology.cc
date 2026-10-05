/*
 * Topology.cc -- out-of-line implementation of Topology::createLinks()
 * and Topology::createRoutingTable()
 *
 * The methods must be defined here (not inline in Topology.hh) because
 * Network is only forward-declared when Topology.hh is parsed, due to
 * a circular include between Topology.hh and Network.hh.
 */

#include "gem5_compat/mem/ruby/network/Topology.hh"
#include "gem5_compat/mem/ruby/network/Network.hh"

#include <algorithm>
#include <limits>
#include <queue>
#include <vector>

namespace gem5 {
namespace ruby {

/*
 * Compute the routing table using BFS shortest paths.
 *
 * For each router R and each outport P:
 *   routing_table_entry[vnet] = NetDest of all destination NODES reachable
 *   through outport P as the first hop on a shortest path.
 *
 * External outports (Router -> NI): reachable set = {NI's node ID}
 * Internal outports (Router -> Router_B): reachable set = all nodes whose
 *   shortest path from R passes through Router_B as next hop.
 */
void Topology::createRoutingTable(Network* net) {
    uint32_t numR = numRouters_;
    uint32_t numVnets = net->getNumberOfVirtualNetworks();

    // Step 1: Build router-to-router adjacency.
    // adj[r] = list of (neighbor_router, link_weight)
    std::vector<std::vector<std::pair<uint32_t, int>>> adj(numR);
    for (auto* link : internalLinks_) {
        auto* intLink = static_cast<BasicIntLink*>(link);
        uint32_t src = intLink->getSrcNode();
        uint32_t dst = intLink->getDstNode();
        adj[src].push_back({dst, intLink->get_weight()});
    }

    // Step 2: Map each node ID to its connected router.
    // nodeToRouter[nodeID] = routerID
    std::map<uint32_t, uint32_t> nodeToRouter;
    for (auto* link : externalLinks_) {
        NodeID ext = link->getExtNode();
        uint32_t router = link->getIntNode();
        nodeToRouter[ext] = router;
    }

    // Step 3: BFS/Dijkstra from each router to compute next-hop.
    // nextHop[src][dst] = the next router on shortest path from src to dst.
    // (If src == dst, nextHop is undefined/unused.)
    std::vector<std::vector<int>> nextHop(numR, std::vector<int>(numR, -1));

    for (uint32_t src = 0; src < numR; src++) {
        // Dijkstra from src
        std::vector<int> dist(numR, std::numeric_limits<int>::max());
        std::vector<int> prev(numR, -1);
        dist[src] = 0;

        // Min-heap: (distance, node)
        std::priority_queue<std::pair<int,int>,
                            std::vector<std::pair<int,int>>,
                            std::greater<>> pq;
        pq.push({0, (int)src});

        while (!pq.empty()) {
            auto [d, u] = pq.top();
            pq.pop();
            if (d > dist[u]) continue;

            for (auto& [v, w] : adj[u]) {
                int nd = dist[u] + w;
                if (nd < dist[v]) {
                    dist[v] = nd;
                    prev[v] = u;
                    pq.push({nd, (int)v});
                }
            }
        }

        // Trace back to find next-hop from src to each dst
        for (uint32_t dst = 0; dst < numR; dst++) {
            if (dst == src) continue;
            if (prev[dst] == -1) continue;  // unreachable

            // Walk back from dst to src to find the first hop
            int cur = (int)dst;
            while (prev[cur] != (int)src && prev[cur] != -1) {
                cur = prev[cur];
            }
            if (prev[cur] == (int)src) {
                nextHop[src][dst] = cur;
            }
        }
    }

    // Step 4: For each (src_router, next_hop_router), collect all
    // destination NODES reachable through that next hop.
    // reachableNodes[src_router][next_hop_router] = set of node IDs
    // Also handle local nodes (connected directly to this router).

    // Build the routing table entries and store in routingTable_.
    // Key: (src_router, dst_router_or_NI), Value: vector<NetDest> per vnet.
    routingTable_.clear();

    // For external out links (Router -> NI): the routing entry just contains
    // the node connected by that link.
    for (auto* link : externalLinks_) {
        NodeID ext = link->getExtNode();
        uint32_t router = link->getIntNode();

        std::vector<NetDest> entry(numVnets);
        for (uint32_t v = 0; v < numVnets; v++) {
            entry[v].add(ext);
        }
        // Key is (router, ext_node) -- but we need a way to distinguish
        // ext link entries from int link entries. Use high bit for ext nodes.
        routingTable_[{router, (SwitchID)(ext + numR)}] = entry;
    }

    // For internal out links (Router_A -> Router_B): the routing entry contains
    // all nodes whose shortest path from Router_A goes through Router_B as
    // the next hop.
    for (auto* link : internalLinks_) {
        auto* intLink = static_cast<BasicIntLink*>(link);
        uint32_t srcRouter = intLink->getSrcNode();
        uint32_t dstRouter = intLink->getDstNode();

        std::vector<NetDest> entry(numVnets);
        for (auto& [nodeID, nodeRouter] : nodeToRouter) {
            if (nodeRouter == srcRouter) continue;  // local node, handled by ext link
            if (nextHop[srcRouter][nodeRouter] == (int)dstRouter) {
                for (uint32_t v = 0; v < numVnets; v++) {
                    entry[v].add(nodeID);
                }
            }
        }
        routingTable_[{srcRouter, dstRouter}] = entry;
    }
}

void Topology::createLinks(Network* net) {
    // External links: NI <-> Router (bidirectional)
    for (auto* link : externalLinks_) {
        NodeID ext = link->getExtNode();
        uint32_t router = link->getIntNode();

        // Get routing entry for ext out link (Router -> NI)
        auto key = std::make_pair((SwitchID)router, (SwitchID)(ext + numRouters_));
        auto it = routingTable_.find(key);
        std::vector<NetDest> routing_table_entry;
        if (it != routingTable_.end()) {
            routing_table_entry = it->second;
        }

        net->makeExtInLink(ext, router, link, routing_table_entry);
        net->makeExtOutLink(router, ext, link, routing_table_entry);
    }
    // Internal links: Router -> Router (unidirectional)
    for (auto* link : internalLinks_) {
        auto* intLink = static_cast<BasicIntLink*>(link);
        uint32_t src = intLink->getSrcNode();
        uint32_t dst = intLink->getDstNode();

        auto key = std::make_pair((SwitchID)src, (SwitchID)dst);
        auto it = routingTable_.find(key);
        std::vector<NetDest> routing_table_entry;
        if (it != routingTable_.end()) {
            routing_table_entry = it->second;
        }

        net->makeInternalLink(src, dst, link, routing_table_entry,
                              intLink->getSrcOutport(), intLink->getDstInport());
    }
}

} // namespace ruby
} // namespace gem5
