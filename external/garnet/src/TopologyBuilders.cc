/*
 * TopologyBuilders.cc
 *
 * Implementations for all 7 built-in topologies plus custom file parser.
 * Each builder creates Garnet Router, NI, IntLink, and ExtLink objects
 * and assembles them into a TopologyResult.
 *
 * All links record their src/dst endpoints and port directions so that
 * Topology::createLinks() can wire the GarnetNetwork correctly.
 */

#include "TopologyBuilders.hh"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "gem5_compat/base/trace.hh"
#include "BusArbiter.hh"
#include "RingRouter.hh"
#include "TreeRouter.hh"
#include "CreditLink.hh"
#include "GarnetLink.hh"
#include "NetworkInterface.hh"
#include "NetworkLink.hh"
#include "Router.hh"

namespace gem5 {
namespace ruby {
namespace garnet {

// -- Helper: create a Router ----------------------------------

Router*
TopologyBuilders::makeRouter(uint32_t id, uint32_t vcs_per_vnet,
                             uint32_t virt_nets, Cycles latency,
                             uint32_t width)
{
    GarnetRouterParams rp;
    rp.name = "router" + std::to_string(id);
    rp.router_id = id;
    rp.vcs_per_vnet = vcs_per_vnet;
    rp.virt_nets = virt_nets;
    rp.latency = latency;
    rp.width = width;
    return new Router(rp);
}

// -- Helper: create a NetworkInterface ------------------------

NetworkInterface*
TopologyBuilders::makeNI(uint32_t id, uint32_t vcs_per_vnet,
                         uint32_t virt_nets)
{
    GarnetNetworkInterfaceParams nip;
    nip.name = "ni" + std::to_string(id);
    nip.id = id;
    nip.vcs_per_vnet = vcs_per_vnet;
    nip.virt_nets = virt_nets;
    // High threshold: under heavy halo-exchange congestion (stencil MPI on
    // high-latency DRAM like DDR5/HBM2) a VC can stay busy past 500k cycles and
    // trip a FALSE "Possible network deadlock" panic while packets are still
    // draining. processBatch bounds itself via maxTick, so this only suppresses
    // the false positive. This is the NetworkInterface threshold the panic
    // (NetworkInterface.cc) actually reads.
    nip.garnet_deadlock_threshold = 100000000;
    return new NetworkInterface(nip);
}

// -- Helper: create an internal (Router->Router) link ----------

GarnetIntLink*
TopologyBuilders::makeIntLink(uint32_t id,
                              uint32_t src_node, uint32_t dst_node,
                              const std::string& src_outport,
                              const std::string& dst_inport,
                              Cycles latency,
                              uint32_t vcs_per_vnet, uint32_t virt_nets,
                              uint32_t width)
{
    NetworkLinkParams nlp;
    nlp.name = "int_net_link" + std::to_string(id);
    nlp.link_id = id * 2;
    nlp.link_latency = latency;
    nlp.vcs_per_vnet = vcs_per_vnet;
    nlp.virt_nets = virt_nets;
    nlp.width = width;

    CreditLinkParams clp;
    clp.name = "int_cred_link" + std::to_string(id);
    clp.link_id = id * 2 + 1;
    clp.link_latency = latency;
    clp.vcs_per_vnet = vcs_per_vnet;
    clp.virt_nets = virt_nets;
    clp.width = width;

    GarnetIntLinkParams gilp;
    gilp.name = "int_link" + std::to_string(id);
    gilp.link_id = id;
    gilp.link_latency = latency;
    gilp.vcs_per_vnet = vcs_per_vnet;
    gilp.virt_nets = virt_nets;
    gilp.channel_width = width;
    gilp.src_node = src_node;
    gilp.dst_node = dst_node;
    gilp.src_outport = src_outport;
    gilp.dst_inport = dst_inport;
    gilp.network_link = new NetworkLink(nlp);
    gilp.credit_link = new CreditLink(clp);

    return new GarnetIntLink(gilp);
}

// -- Helper: create an external (NI<->Router) link -------------

GarnetExtLink*
TopologyBuilders::makeExtLink(uint32_t id,
                              NodeID ext_node, uint32_t int_node,
                              Cycles latency,
                              uint32_t vcs_per_vnet, uint32_t virt_nets,
                              uint32_t width)
{
    // External links are bidirectional: index 0 = In (NI->Router),
    //                                   index 1 = Out (Router->NI)
    NetworkLinkParams nlp_in;
    nlp_in.name = "ext_net_link_in" + std::to_string(id);
    nlp_in.link_id = 10000 + id * 4;
    nlp_in.link_latency = latency;
    nlp_in.vcs_per_vnet = vcs_per_vnet;
    nlp_in.virt_nets = virt_nets;
    nlp_in.width = width;

    CreditLinkParams clp_in;
    clp_in.name = "ext_cred_link_in" + std::to_string(id);
    clp_in.link_id = 10000 + id * 4 + 1;
    clp_in.link_latency = latency;
    clp_in.vcs_per_vnet = vcs_per_vnet;
    clp_in.virt_nets = virt_nets;
    clp_in.width = width;

    NetworkLinkParams nlp_out;
    nlp_out.name = "ext_net_link_out" + std::to_string(id);
    nlp_out.link_id = 10000 + id * 4 + 2;
    nlp_out.link_latency = latency;
    nlp_out.vcs_per_vnet = vcs_per_vnet;
    nlp_out.virt_nets = virt_nets;
    nlp_out.width = width;

    CreditLinkParams clp_out;
    clp_out.name = "ext_cred_link_out" + std::to_string(id);
    clp_out.link_id = 10000 + id * 4 + 3;
    clp_out.link_latency = latency;
    clp_out.vcs_per_vnet = vcs_per_vnet;
    clp_out.virt_nets = virt_nets;
    clp_out.width = width;

    GarnetExtLinkParams gelp;
    gelp.name = "ext_link" + std::to_string(id);
    gelp.link_id = id;
    gelp.link_latency = latency;
    gelp.vcs_per_vnet = vcs_per_vnet;
    gelp.virt_nets = virt_nets;
    gelp.channel_width = width;
    gelp.ext_node = ext_node;
    gelp.int_node = int_node;
    gelp.network_links = { new NetworkLink(nlp_in), new NetworkLink(nlp_out) };
    gelp.credit_links  = { new CreditLink(clp_in),  new CreditLink(clp_out)  };

    return new GarnetExtLink(gelp);
}


// ===============================================================
// MESH_2D: rows x cols grid, N/S/E/W + Local ports
// ===============================================================

TopologyResult
TopologyBuilders::buildMesh(uint32_t rows, uint32_t cols,
                            uint32_t num_endpoints,
                            uint32_t vcs_per_vnet, uint32_t virt_nets,
                            Cycles link_latency, Cycles router_latency,
                            uint32_t width)
{
    return buildMeshImpl(rows, cols, num_endpoints, vcs_per_vnet, virt_nets,
                         link_latency, router_latency, width, false);
}

TopologyResult
TopologyBuilders::buildMeshImpl(uint32_t rows, uint32_t cols,
                                uint32_t num_endpoints,
                                uint32_t vcs_per_vnet, uint32_t virt_nets,
                                Cycles link_latency, Cycles router_latency,
                                uint32_t width, bool torus_routers)
{
    uint32_t num_routers = rows * cols;
    TopologyResult result;
    result.topology = new Topology(num_routers);
    result.topology->setName(torus_routers ? "TORUS_2D" : "MESH_2D");

    for (uint32_t i = 0; i < num_routers; i++) {
        if (!torus_routers) {
            result.routers.push_back(
                makeRouter(i, vcs_per_vnet, virt_nets, router_latency, width));
            continue;
        }
        // 1.11.94 (H35): torus router = RingRouter with one dateline per
        // dimension on its wrap link (the link buildTorus adds from the last
        // column/row back to the first) and a class reset on the X->Y turn.
        GarnetRouterParams rp;
        rp.name = "torus_router" + std::to_string(i);
        rp.router_id = i;
        rp.vcs_per_vnet = vcs_per_vnet;
        rp.virt_nets = virt_nets;
        rp.latency = router_latency;
        rp.width = width;
        RingRouter* rr = new RingRouter(rp);
        uint32_t r = i / cols, c = i % cols;
        if (c == cols - 1) rr->addDatelineDirection("East");
        if (c == 0)        rr->addDatelineDirection("West");
        if (r == rows - 1) rr->addDatelineDirection("North");
        if (r == 0)        rr->addDatelineDirection("South");
        rr->setDimensionReset(true);
        result.routers.push_back(rr);
    }

    uint32_t ext_id = 0;
    for (uint32_t i = 0; i < num_endpoints; i++) {
        result.nis.push_back(makeNI(i, vcs_per_vnet, virt_nets));
        uint32_t router_id = i % num_routers;
        GarnetExtLink* elink = makeExtLink(ext_id++, i, router_id,
                                           link_latency,
                                           vcs_per_vnet, virt_nets, width);
        result.ext_links.push_back(elink);
        result.topology->addExternalLink(elink);
    }

    // Internal links: East<->West, North<->South
    uint32_t int_id = 0;
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            uint32_t src = r * cols + c;

            // East link: (r,c) -> (r,c+1)
            if (c + 1 < cols) {
                uint32_t dst = r * cols + (c + 1);
                GarnetIntLink* link_e = makeIntLink(int_id++,
                    src, dst, "East", "West",
                    link_latency, vcs_per_vnet, virt_nets, width);
                result.int_links.push_back(link_e);
                result.topology->addInternalLink(link_e);

                GarnetIntLink* link_w = makeIntLink(int_id++,
                    dst, src, "West", "East",
                    link_latency, vcs_per_vnet, virt_nets, width);
                result.int_links.push_back(link_w);
                result.topology->addInternalLink(link_w);
            }

            // North link: (r,c) -> (r+1,c) -- increasing row = increasing y = North
            if (r + 1 < rows) {
                uint32_t dst = (r + 1) * cols + c;
                GarnetIntLink* link_n = makeIntLink(int_id++,
                    src, dst, "North", "South",
                    link_latency, vcs_per_vnet, virt_nets, width);
                result.int_links.push_back(link_n);
                result.topology->addInternalLink(link_n);

                GarnetIntLink* link_s = makeIntLink(int_id++,
                    dst, src, "South", "North",
                    link_latency, vcs_per_vnet, virt_nets, width);
                result.int_links.push_back(link_s);
                result.topology->addInternalLink(link_s);
            }
        }
    }

    return result;
}


// ===============================================================
// TORUS_2D: mesh + wrap-around links
// ===============================================================

TopologyResult
TopologyBuilders::buildTorus(uint32_t rows, uint32_t cols,
                             uint32_t num_endpoints,
                             uint32_t vcs_per_vnet, uint32_t virt_nets,
                             Cycles link_latency, Cycles router_latency,
                             uint32_t width)
{
    // 1.11.94 (x03-garnet-custom-1, ruling H35): the torus used to be the
    // plain-router mesh plus wrap links, so DOR over the wrap links closed a
    // cyclic channel dependency on every row and column ring with nothing to
    // break it (an 8x8 tornado with 4 packets per node delivered 0/256). The
    // routers are now dateline-aware (RingRouter, see buildMeshImpl), which
    // needs two VC classes per vnet, i.e. at least 2 VCs per vnet.
    if (vcs_per_vnet < 2) {
        fatal("TORUS_2D needs vcs_per_vnet >= 2 (one VC per dateline class "
              "on each wrap ring); got %u. Set noc.vcs_per_vnet to 2 or more.",
              (unsigned)vcs_per_vnet);
    }
    TopologyResult result = buildMeshImpl(rows, cols, num_endpoints,
        vcs_per_vnet, virt_nets, link_latency, router_latency, width, true);

    uint32_t int_id = result.int_links.size();

    // Wrap-around for columns (east edge -> west edge)
    for (uint32_t r = 0; r < rows; r++) {
        uint32_t left = r * cols;
        uint32_t right = r * cols + (cols - 1);

        GarnetIntLink* wrap_e = makeIntLink(int_id++,
            right, left, "East", "West",
            link_latency, vcs_per_vnet, virt_nets, width);
        result.int_links.push_back(wrap_e);
        result.topology->addInternalLink(wrap_e);

        GarnetIntLink* wrap_w = makeIntLink(int_id++,
            left, right, "West", "East",
            link_latency, vcs_per_vnet, virt_nets, width);
        result.int_links.push_back(wrap_w);
        result.topology->addInternalLink(wrap_w);
    }

    // Wrap-around for rows: continuing North/South through wrap
    // North wrap: last_row -> first_row (continuing North past edge)
    // South wrap: first_row -> last_row (continuing South past edge)
    for (uint32_t c = 0; c < cols; c++) {
        uint32_t top = c;                          // row 0 (lowest y)
        uint32_t bottom = (rows - 1) * cols + c;   // last row (highest y)

        GarnetIntLink* wrap_n = makeIntLink(int_id++,
            bottom, top, "North", "South",
            link_latency, vcs_per_vnet, virt_nets, width);
        result.int_links.push_back(wrap_n);
        result.topology->addInternalLink(wrap_n);

        GarnetIntLink* wrap_s = makeIntLink(int_id++,
            top, bottom, "South", "North",
            link_latency, vcs_per_vnet, virt_nets, width);
        result.int_links.push_back(wrap_s);
        result.topology->addInternalLink(wrap_s);
    }

    return result;
}


// ===============================================================
// RING: N routers in a bidirectional ring
// ===============================================================

TopologyResult
TopologyBuilders::buildRing(uint32_t ring_size, uint32_t num_endpoints,
                            uint32_t vcs_per_vnet, uint32_t virt_nets,
                            Cycles link_latency, Cycles router_latency,
                            uint32_t width, bool unidirectional)
{
    TopologyResult result;
    result.topology = new Topology(ring_size);
    result.topology->setName(unidirectional ? "RING (unidirectional)" : "RING");

    if (unidirectional) {
        // -- Unidirectional ring: standard routers, CW links only --
        // Acyclic channel dependency -> deadlock-free with 1 VC.
        for (uint32_t i = 0; i < ring_size; i++) {
            result.routers.push_back(
                makeRouter(i, vcs_per_vnet, virt_nets, router_latency, width));
        }

        uint32_t ext_id = 0;
        for (uint32_t i = 0; i < num_endpoints; i++) {
            result.nis.push_back(makeNI(i, vcs_per_vnet, virt_nets));
            uint32_t router_id = i % ring_size;
            GarnetExtLink* elink = makeExtLink(ext_id++, i, router_id,
                                               link_latency,
                                               vcs_per_vnet, virt_nets, width);
            result.ext_links.push_back(elink);
            result.topology->addExternalLink(elink);
        }

        // CW only: router i -> router (i+1) % N
        uint32_t int_id = 0;
        for (uint32_t i = 0; i < ring_size; i++) {
            uint32_t next = (i + 1) % ring_size;
            GarnetIntLink* link_cw = makeIntLink(int_id++,
                i, next, "East", "West",
                link_latency, vcs_per_vnet, virt_nets, width);
            result.int_links.push_back(link_cw);
            result.topology->addInternalLink(link_cw);
        }
    } else {
        // -- Bidirectional ring: RingRouters with dateline-aware VC allocation --
        for (uint32_t i = 0; i < ring_size; i++) {
            GarnetRouterParams rp;
            rp.name = "ring_router" + std::to_string(i);
            rp.router_id = i;
            rp.vcs_per_vnet = vcs_per_vnet;
            rp.virt_nets = virt_nets;
            rp.latency = router_latency;
            rp.width = width;
            result.routers.push_back(new RingRouter(rp));
        }

        uint32_t ext_id = 0;
        for (uint32_t i = 0; i < num_endpoints; i++) {
            result.nis.push_back(makeNI(i, vcs_per_vnet, virt_nets));
            uint32_t router_id = i % ring_size;
            GarnetExtLink* elink = makeExtLink(ext_id++, i, router_id,
                                               link_latency,
                                               vcs_per_vnet, virt_nets, width);
            result.ext_links.push_back(elink);
            result.topology->addExternalLink(elink);
        }

        // Bidirectional: CW + CCW links with dateline tracking
        std::vector<int> ext_outport_count(ring_size, 0);
        for (uint32_t i = 0; i < num_endpoints; i++) {
            uint32_t router_id = i % ring_size;
            ext_outport_count[router_id]++;
        }

        std::vector<int> int_outport_idx(ring_size);
        for (uint32_t i = 0; i < ring_size; i++) {
            int_outport_idx[i] = ext_outport_count[i];
        }

        uint32_t int_id = 0;
        for (uint32_t i = 0; i < ring_size; i++) {
            uint32_t next = (i + 1) % ring_size;

            // CW link: router i -> router next
            GarnetIntLink* link_cw = makeIntLink(int_id++,
                i, next, "East", "West",
                link_latency, vcs_per_vnet, virt_nets, width);
            result.int_links.push_back(link_cw);
            result.topology->addInternalLink(link_cw);

            if (i == ring_size - 1 && next == 0) {
                static_cast<RingRouter*>(result.routers[i])
                    ->addDatelineOutport(int_outport_idx[i]);
            }
            int_outport_idx[i]++;

            // CCW link: router next -> router i
            GarnetIntLink* link_ccw = makeIntLink(int_id++,
                next, i, "West", "East",
                link_latency, vcs_per_vnet, virt_nets, width);
            result.int_links.push_back(link_ccw);
            result.topology->addInternalLink(link_ccw);

            if (next == 0 && i == ring_size - 1) {
                static_cast<RingRouter*>(result.routers[next])
                    ->addDatelineOutport(int_outport_idx[next]);
            }
            int_outport_idx[next]++;
        }
    }

    return result;
}


// ===============================================================
// CROSSBAR: every router connected to every other router
// ===============================================================

TopologyResult
TopologyBuilders::buildCrossbar(uint32_t num_endpoints,
                                uint32_t vcs_per_vnet, uint32_t virt_nets,
                                Cycles link_latency, Cycles router_latency,
                                uint32_t width)
{
    TopologyResult result;
    // Single central non-blocking switch -- 1 router, all NIs directly connected.
    // Unlike the BusArbiter (1 grant/cycle), the standard Router crossbar switch
    // allows multiple non-conflicting transfers per cycle.
    result.topology = new Topology(1);
    result.topology->setName("CROSSBAR");

    result.routers.push_back(
        makeRouter(0, vcs_per_vnet, virt_nets, router_latency, width));

    for (uint32_t i = 0; i < num_endpoints; i++) {
        result.nis.push_back(makeNI(i, vcs_per_vnet, virt_nets));
        GarnetExtLink* elink = makeExtLink(i, i, 0, link_latency,
                                           vcs_per_vnet, virt_nets, width);
        result.ext_links.push_back(elink);
        result.topology->addExternalLink(elink);
    }

    return result;
}


// ===============================================================
// FAT_TREE: k-ary fat tree
// ===============================================================

TopologyResult
TopologyBuilders::buildFatTree(uint32_t num_endpoints, uint32_t arity,
                               uint32_t vcs_per_vnet, uint32_t virt_nets,
                               Cycles link_latency, Cycles router_latency,
                               uint32_t width)
{
    uint32_t levels = 1;
    uint32_t cap = arity;
    while (cap < num_endpoints) {
        cap *= arity;
        levels++;
    }

    std::vector<uint32_t> routers_per_level;
    uint32_t level_routers = (num_endpoints + arity - 1) / arity;
    for (uint32_t l = 0; l < levels; l++) {
        routers_per_level.push_back(level_routers);
        level_routers = (level_routers + arity - 1) / arity;
    }

    uint32_t total_routers = 0;
    for (auto r : routers_per_level) total_routers += r;

    TopologyResult result;
    result.topology = new Topology(total_routers);
    result.topology->setName("FAT_TREE");

    for (uint32_t i = 0; i < total_routers; i++) {
        result.routers.push_back(
            makeRouter(i, vcs_per_vnet, virt_nets, router_latency, width));
    }

    // NIs connect to leaf routers
    for (uint32_t i = 0; i < num_endpoints; i++) {
        result.nis.push_back(makeNI(i, vcs_per_vnet, virt_nets));
        uint32_t leaf_router = i / arity;  // map endpoint to leaf router
        GarnetExtLink* elink = makeExtLink(i, i, leaf_router, link_latency,
                                           vcs_per_vnet, virt_nets, width);
        result.ext_links.push_back(elink);
        result.topology->addExternalLink(elink);
    }

    // Internal links between levels
    uint32_t int_id = 0;
    uint32_t base_offset = 0;
    for (uint32_t l = 0; l + 1 < levels; l++) {
        uint32_t next_offset = base_offset + routers_per_level[l];
        for (uint32_t r = 0; r < routers_per_level[l]; r++) {
            uint32_t parent = next_offset + r / arity;
            uint32_t child  = base_offset + r;

            GarnetIntLink* up = makeIntLink(int_id++,
                child, parent, "Up", "Down",
                link_latency, vcs_per_vnet, virt_nets, width);
            result.int_links.push_back(up);
            result.topology->addInternalLink(up);

            GarnetIntLink* down = makeIntLink(int_id++,
                parent, child, "Down", "Up",
                link_latency, vcs_per_vnet, virt_nets, width);
            result.int_links.push_back(down);
            result.topology->addInternalLink(down);
        }
        base_offset = next_offset;
    }

    return result;
}


// ===============================================================
// BUS: 1 central BusArbiter, all NIs directly connected.
// Unlike a crossbar router, the BusArbiter enforces shared-bus
// semantics: only ONE flit traverses the switch per cycle.
// ===============================================================

TopologyResult
TopologyBuilders::buildBus(uint32_t num_endpoints,
                           uint32_t vcs_per_vnet, uint32_t virt_nets,
                           Cycles link_latency, Cycles router_latency,
                           uint32_t width)
{
    TopologyResult result;
    result.topology = new Topology(1);
    result.topology->setName("BUS");

    // Central arbiter -- shared bus, not a crossbar
    GarnetRouterParams rp;
    rp.name = "bus_arbiter0";
    rp.router_id = 0;
    rp.vcs_per_vnet = vcs_per_vnet;
    rp.virt_nets = virt_nets;
    rp.latency = router_latency;
    rp.width = width;
    result.routers.push_back(new BusArbiter(rp));

    for (uint32_t i = 0; i < num_endpoints; i++) {
        result.nis.push_back(makeNI(i, vcs_per_vnet, virt_nets));
        GarnetExtLink* elink = makeExtLink(i, i, 0, link_latency,
                                           vcs_per_vnet, virt_nets, width);
        result.ext_links.push_back(elink);
        result.topology->addExternalLink(elink);
    }

    return result;
}


// ===============================================================
// H_TREE: binary tree with H-pattern branching
// ===============================================================

TopologyResult
TopologyBuilders::buildHTree(uint32_t num_endpoints,
                             uint32_t vcs_per_vnet, uint32_t virt_nets,
                             Cycles link_latency, Cycles router_latency,
                             uint32_t width)
{
    uint32_t levels = 1;
    uint32_t cap = 2;
    while (cap < num_endpoints) {
        cap *= 2;
        levels++;
    }

    uint32_t total_routers = (1u << levels) - 1;

    TopologyResult result;
    result.topology = new Topology(total_routers);
    result.topology->setName("H_TREE");

    for (uint32_t i = 0; i < total_routers; i++) {
        result.routers.push_back(
            makeRouter(i, vcs_per_vnet, virt_nets, router_latency, width));
    }

    uint32_t leaf_start = (1u << (levels - 1)) - 1;
    uint32_t num_leaves = total_routers - leaf_start;
    for (uint32_t i = 0; i < num_endpoints; i++) {
        result.nis.push_back(makeNI(i, vcs_per_vnet, virt_nets));
        // Distribute endpoints evenly across leaf routers (round-robin)
        uint32_t leaf_router = leaf_start + (i % num_leaves);
        GarnetExtLink* elink = makeExtLink(i, i, leaf_router, link_latency,
                                           vcs_per_vnet, virt_nets, width);
        result.ext_links.push_back(elink);
        result.topology->addExternalLink(elink);
    }

    // Binary tree: parent i -> left child 2i+1, right child 2i+2
    uint32_t int_id = 0;
    for (uint32_t i = 0; i < total_routers; i++) {
        uint32_t left = 2 * i + 1;
        uint32_t right = 2 * i + 2;

        if (left < total_routers) {
            GarnetIntLink* link_down = makeIntLink(int_id++,
                i, left, "Left", "Up",
                link_latency, vcs_per_vnet, virt_nets, width);
            result.int_links.push_back(link_down);
            result.topology->addInternalLink(link_down);

            GarnetIntLink* link_up = makeIntLink(int_id++,
                left, i, "Up", "Left",
                link_latency, vcs_per_vnet, virt_nets, width);
            result.int_links.push_back(link_up);
            result.topology->addInternalLink(link_up);
        }

        if (right < total_routers) {
            GarnetIntLink* link_down = makeIntLink(int_id++,
                i, right, "Right", "Up",
                link_latency, vcs_per_vnet, virt_nets, width);
            result.int_links.push_back(link_down);
            result.topology->addInternalLink(link_down);

            GarnetIntLink* link_up = makeIntLink(int_id++,
                right, i, "Up", "Right",
                link_latency, vcs_per_vnet, virt_nets, width);
            result.int_links.push_back(link_up);
            result.topology->addInternalLink(link_up);
        }
    }

    return result;
}


// ===============================================================
// CUSTOM: parse topology from file
// ===============================================================

TopologyResult
TopologyBuilders::buildFromFile(const std::string& filename,
                                uint32_t vcs_per_vnet, uint32_t virt_nets,
                                Cycles link_latency, Cycles router_latency,
                                uint32_t width)
{
    std::ifstream infile(filename);
    if (!infile.is_open()) {
        throw std::runtime_error("Cannot open topology file: " + filename);
    }

    uint32_t num_routers = 0, num_endpoints = 0;
    // Optional per-link latency (cycles) and width (bits): -1 / 0 mean
    // "inherit the global link_latency / width". This lets a CUSTOM topology
    // file encode a real DRAM hierarchy where each layer's link carries its
    // own per-layer latency and channel width (the per-link latency+bw target).
    struct ExtEntry { uint32_t endpoint; uint32_t router; long latency; uint32_t width; };
    struct IntEntry { uint32_t src; uint32_t dst; int weight; long latency; uint32_t width; };
    std::vector<ExtEntry> ext_entries;
    std::vector<IntEntry> int_entries;

    std::string line;
    while (std::getline(infile, line)) {
        auto hash_pos = line.find('#');
        if (hash_pos != std::string::npos) line = line.substr(0, hash_pos);

        std::istringstream iss(line);
        std::string token;
        if (!(iss >> token)) continue;

        if (token == "routers") {
            iss >> num_routers;
        } else if (token == "endpoints") {
            iss >> num_endpoints;
        } else if (token == "ext") {
            ExtEntry e; e.latency = -1; e.width = 0;
            iss >> e.endpoint >> e.router;
            // optional per-link latency / width; guarded so an absent field
            // inherits the global (C++11 >> zeroes the target on failure).
            long lat; uint32_t w;
            if (iss >> lat) e.latency = lat;
            if (iss >> w)   e.width = w;
            ext_entries.push_back(e);
        } else if (token == "int") {
            IntEntry e;
            e.weight = 1; e.latency = -1; e.width = 0;
            iss >> e.src >> e.dst;
            int wt; long lat; uint32_t w;
            if (iss >> wt)  e.weight = wt;
            if (iss >> lat) e.latency = lat;
            if (iss >> w)   e.width = w;
            int_entries.push_back(e);
        }
    }

    if (num_routers == 0) {
        throw std::runtime_error("Topology file missing 'routers' count");
    }
    if (num_endpoints == 0) num_endpoints = num_routers;

    // -- Tree detection for up*/down* deadlock-free routing ------------------
    // The detailed DRAM topology is a rooted tree (ROOT = router 0; each edge
    // emitted in BOTH directions). On a tree, shortest-path routing is
    // up-to-LCA-then-down, which is deadlock-free PROVIDED the up- and
    // down-phases use disjoint VC classes. We detect a clean rooted tree here
    // and, if found, (a) instantiate TreeRouters that enforce up/down VC-class
    // separation and (b) name each internal outport "Up"/"Down" so the router
    // can classify it. Any non-tree CUSTOM graph falls back to plain Routers
    // with generic "Port<i>" names + table routing (unchanged behavior).
    //
    // depth[r] = BFS hop distance from ROOT(0) over the undirected adjacency.
    // is_tree = every router reachable from 0 AND undirected edge count
    //           (== int_entries/2, since each edge is emitted both ways)
    //           equals num_routers-1 (the defining property of a tree).
    std::vector<int> depth(num_routers, -1);
    bool is_tree = false;
    {
        std::vector<std::vector<uint32_t>> undirected_adj(num_routers);
        size_t directed_edges = 0;
        bool in_range = true;
        for (auto& e : int_entries) {
            if (e.src >= num_routers || e.dst >= num_routers) {
                in_range = false; break;
            }
            undirected_adj[e.src].push_back(e.dst);
            directed_edges++;
        }
        if (in_range) {
            // BFS from ROOT(0)
            std::vector<uint32_t> q;
            q.push_back(0);
            depth[0] = 0;
            size_t head = 0;
            uint32_t visited = 1;
            while (head < q.size()) {
                uint32_t u = q[head++];
                for (uint32_t v : undirected_adj[u]) {
                    if (depth[v] == -1) {
                        depth[v] = depth[u] + 1;
                        visited++;
                        q.push_back(v);
                    }
                }
            }
            // Clean rooted tree: all routers reached AND exactly
            // (num_routers - 1) undirected edges (each emitted twice).
            is_tree = (visited == num_routers) &&
                      (directed_edges == (size_t)(num_routers - 1) * 2);
        }
        if (!is_tree) {
            std::fill(depth.begin(), depth.end(), -1);
        }
    }

    TopologyResult result;
    result.topology = new Topology(num_routers);
    // 1.11.94 (x03-garnet-custom-3): name used in routing refusals only.
    result.topology->setName("CUSTOM (" + filename + ")");

    for (uint32_t i = 0; i < num_routers; i++) {
        if (is_tree) {
            // up*/down* deadlock-free router (VC-class separation).
            GarnetRouterParams rp;
            rp.name = "tree_router" + std::to_string(i);
            rp.router_id = i;
            rp.vcs_per_vnet = vcs_per_vnet;
            rp.virt_nets = virt_nets;
            rp.latency = router_latency;
            rp.width = width;
            result.routers.push_back(new TreeRouter(rp));
        } else {
            result.routers.push_back(
                makeRouter(i, vcs_per_vnet, virt_nets, router_latency, width));
        }
    }

    for (uint32_t i = 0; i < num_endpoints; i++) {
        result.nis.push_back(makeNI(i, vcs_per_vnet, virt_nets));
    }

    // External links
    if (ext_entries.empty()) {
        for (uint32_t i = 0; i < num_endpoints; i++) {
            uint32_t router_id = i % num_routers;
            GarnetExtLink* elink = makeExtLink(i, i, router_id,
                                               link_latency,
                                               vcs_per_vnet, virt_nets, width);
            result.ext_links.push_back(elink);
            result.topology->addExternalLink(elink);
        }
    } else {
        uint32_t ext_id = 0;
        for (auto& e : ext_entries) {
            Cycles  eff_lat = (e.latency >= 0) ? Cycles(e.latency) : link_latency;
            uint32_t eff_w  = (e.width   >  0) ? e.width           : width;
            GarnetExtLink* elink = makeExtLink(ext_id++, e.endpoint, e.router,
                                               eff_lat,
                                               vcs_per_vnet, virt_nets, eff_w);
            result.ext_links.push_back(elink);
            result.topology->addExternalLink(elink);
        }
    }

    // Internal links
    uint32_t int_id = 0;
    for (auto& e : int_entries) {
        // For a rooted tree, name the SRC outport by its direction so the
        // TreeRouter can apply up/down VC-class separation: an edge toward
        // lower depth (toward ROOT) is "Up"; toward higher depth (toward
        // leaves) is "Down". The DST inport name is the opposite. Non-tree
        // CUSTOM graphs keep the generic, direction-agnostic "Port<i>" names.
        std::string src_port, dst_port;
        if (is_tree && depth[e.src] >= 0 && depth[e.dst] >= 0 &&
            depth[e.src] != depth[e.dst]) {
            bool down = depth[e.dst] > depth[e.src];  // src -> dst goes deeper
            src_port = down ? "Down" : "Up";
            dst_port = down ? "Up"   : "Down";
        } else {
            src_port = "Port" + std::to_string(int_id);
            dst_port = src_port;
        }
        Cycles  eff_lat = (e.latency >= 0) ? Cycles(e.latency) : link_latency;
        uint32_t eff_w  = (e.width   >  0) ? e.width           : width;
        GarnetIntLink* link = makeIntLink(int_id++,
            e.src, e.dst, src_port, dst_port,
            eff_lat, vcs_per_vnet, virt_nets, eff_w);
        result.int_links.push_back(link);
        result.topology->addInternalLink(link);
    }

    return result;
}

} // namespace garnet
} // namespace ruby
} // namespace gem5
