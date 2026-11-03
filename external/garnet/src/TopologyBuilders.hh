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
    /* (1.12.233): with "buffers rtt", the credits per VC of every router input
     * port (in_depth[router][k], the k-th link into it) -- empty otherwise. */
    std::vector<std::vector<uint32_t>> in_depth;
};

class TopologyBuilders {
public:
    /* (1.12.233): how a build sizes each link's receiving buffer per VC. With rtt, a link without an explicit depth
     * feeds a buffer of its credit round trip (link latency + the receiver's pipe stages + credit latency + 1; a
     * network interface sends one cycle late and receives in one stage), never below floor (the network's depth,
     * one data packet), so an uncontended link carries one flit per cycle. level_in / level_out are explicit
     * depths by hierarchy level (CUSTOM files with rlevel lines): the input buffers of the routers at a level, and
     * the buffers the links leaving a level's routers feed (level_in wins where both apply). Explicit depths are
     * used as given (Garnet's wormhole flow control needs no whole packet per VC). fixed = the network's depth is
     * an explicit setting: a file's "buffers rtt" line does not apply. The wrapper sets it before a build and
     * clears it after. */
    struct DepthPolicy {
        bool rtt = false;
        bool fixed = false;
        uint32_t floor = 0;
        uint32_t packet = 1;
        Cycles stages = Cycles(1);   // a built-in topology's routers (every router alike)
        int level_in[7] = {-1, -1, -1, -1, -1, -1, -1};
        int level_out[7] = {-1, -1, -1, -1, -1, -1, -1};
    };
    static void setDepthPolicy(const DepthPolicy& p);
    // (1.12.233): the router input buffers the last build sized: their count and summed credits per VC (0 = none)
    static void sizedInputs(uint64_t& n, uint64_t& sum);

    // -- Built-in topologies ----------------------------------

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

    // (1.12.230): read and check a CUSTOM file without building it; throws std::runtime_error naming the file and line.
    static void validateFile(const std::string& filename);

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

    // Helper: create an internal link (Router -> Router)
    static GarnetIntLink* makeIntLink(uint32_t id,
                                      uint32_t src_node, uint32_t dst_node,
                                      const std::string& src_outport,
                                      const std::string& dst_inport,
                                      Cycles latency,
                                      uint32_t vcs_per_vnet,
                                      uint32_t virt_nets,
                                      uint32_t width,
                                      long credit_latency = -1,    // (1.12.233): -1 = the link's latency
                                      uint32_t vc_depth = 0,       // (1.12.233): 0 = the network's (or the round trip)
                                      bool vc_depth_exact = false);   // (1.12.233): vc_depth is an explicit setting

    // Helper: create an external link (NI <-> Router, bidirectional)
    static GarnetExtLink* makeExtLink(uint32_t id,
                                      NodeID ext_node, uint32_t int_node,
                                      Cycles latency,
                                      uint32_t vcs_per_vnet,
                                      uint32_t virt_nets,
                                      uint32_t width,
                                      long credit_latency = -1,    // (1.12.233): -1 = the link's latency
                                      uint32_t depth_in = 0,       // (1.12.233): NI -> router buffer, 0 = the network's
                                      uint32_t depth_out = 0,      // (1.12.233): router -> NI buffer
                                      bool in_exact = false,       // (1.12.233): depth_in / depth_out are explicit
                                      bool out_exact = false);
};

} // namespace garnet
} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_NETWORK_GARNET_0_TOPOLOGYBUILDERS_HH__
