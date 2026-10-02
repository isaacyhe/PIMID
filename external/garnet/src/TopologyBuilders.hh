/*
 * TopologyBuilders.hh
 *
 * Utility for constructing various NoC topologies (mesh, torus, ring,
 * crossbar, fat-tree, bus, H-tree, custom) for the standalone Garnet
 * library.  Each builder creates Router, NetworkInterface, and Link
 * objects, populates a Topology, and returns everything ready for
 * GarnetNetwork.
 *
 * All builders populate TABLE_ routing tables during link creation so
 * TABLE routing works on every topology.
 */

#ifndef __MEM_RUBY_NETWORK_GARNET_0_TOPOLOGYBUILDERS_HH__
#define __MEM_RUBY_NETWORK_GARNET_0_TOPOLOGYBUILDERS_HH__

#include <cstdint>
#include <string>
#include <vector>

#include "CommonTypes.hh"
#include "gem5_compat/mem/ruby/network/Topology.hh"
#include "gem5_compat/params/GarnetNetwork.hh"
#include "gem5_compat/params/GarnetRouter.hh"
#include "gem5_compat/params/GarnetNetworkInterface.hh"

namespace gem5 {
namespace ruby {
namespace garnet {

class Router;
class NetworkInterface;
class GarnetIntLink;
class GarnetExtLink;

// Result of a topology build.  Caller takes ownership of everything.
struct TopologyResult {
    std::vector<Router*> routers;
    std::vector<NetworkInterface*> nis;
    std::vector<GarnetIntLink*> int_links;
    std::vector<GarnetExtLink*> ext_links;
    Topology* topology;
};

class TopologyBuilders {
public:
    // ── Built-in topologies ──────────────────────────────────

    static TopologyResult buildMesh(
        uint32_t rows, uint32_t cols, uint32_t num_endpoints,
        uint32_t vcs_per_vnet, uint32_t virt_nets,
        Cycles link_latency, Cycles router_latency, uint32_t width);

    static TopologyResult buildTorus(
        uint32_t rows, uint32_t cols, uint32_t num_endpoints,
        uint32_t vcs_per_vnet, uint32_t virt_nets,
        Cycles link_latency, Cycles router_latency, uint32_t width);

    static TopologyResult buildRing(
        uint32_t ring_size, uint32_t num_endpoints,
        uint32_t vcs_per_vnet, uint32_t virt_nets,
        Cycles link_latency, Cycles router_latency, uint32_t width,
        bool unidirectional = false);

    static TopologyResult buildCrossbar(
        uint32_t num_endpoints,
        uint32_t vcs_per_vnet, uint32_t virt_nets,
        Cycles link_latency, Cycles router_latency, uint32_t width);

    static TopologyResult buildFatTree(
        uint32_t num_endpoints, uint32_t arity,
        uint32_t vcs_per_vnet, uint32_t virt_nets,
        Cycles link_latency, Cycles router_latency, uint32_t width);

    static TopologyResult buildBus(
        uint32_t num_endpoints,
        uint32_t vcs_per_vnet, uint32_t virt_nets,
        Cycles link_latency, Cycles router_latency, uint32_t width);

    static TopologyResult buildHTree(
        uint32_t num_endpoints,
        uint32_t vcs_per_vnet, uint32_t virt_nets,
        Cycles link_latency, Cycles router_latency, uint32_t width);

    static TopologyResult buildFromFile(
        const std::string& filename,
        uint32_t vcs_per_vnet, uint32_t virt_nets,
        Cycles link_latency, Cycles router_latency, uint32_t width);

private:
    // 1.11.94 (H35): the mesh body, shared by MESH_2D and TORUS_2D. With
    // torus_routers the routers are dateline-aware RingRouters; without it
    // they are the plain makeRouter routers MESH_2D has always used.
    static TopologyResult buildMeshImpl(
        uint32_t rows, uint32_t cols, uint32_t num_endpoints,
        uint32_t vcs_per_vnet, uint32_t virt_nets,
        Cycles link_latency, Cycles router_latency, uint32_t width,
        bool torus_routers);

    // Helper: create a single Router with default params
    static Router* makeRouter(uint32_t id, uint32_t vcs_per_vnet,
                              uint32_t virt_nets, Cycles latency,
                              uint32_t width);

    // Helper: create a single NetworkInterface
    static NetworkInterface* makeNI(uint32_t id, uint32_t vcs_per_vnet,
                                    uint32_t virt_nets);

    // Helper: create an internal link (Router → Router)
    static GarnetIntLink* makeIntLink(uint32_t id,
                                      uint32_t src_node, uint32_t dst_node,
                                      const std::string& src_outport,
                                      const std::string& dst_inport,
                                      Cycles latency,
                                      uint32_t vcs_per_vnet,
                                      uint32_t virt_nets,
                                      uint32_t width);

    // Helper: create an external link (NI ↔ Router, bidirectional)
    static GarnetExtLink* makeExtLink(uint32_t id,
                                      NodeID ext_node, uint32_t int_node,
                                      Cycles latency,
                                      uint32_t vcs_per_vnet,
                                      uint32_t virt_nets,
                                      uint32_t width);
};

} // namespace garnet
} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_NETWORK_GARNET_0_TOPOLOGYBUILDERS_HH__
