/**
 * @file garnet_network.h
 * @brief Multi-topology Garnet-based network model for ZSim
 *
 * Supports 7 built-in topologies (MESH_2D, TORUS_2D, RING, CROSSBAR,
 * FAT_TREE, BUS, H_TREE) plus CUSTOM from file.  Two modes:
 *   - Simple: topology-aware hop counts + M/D/1 queuing contention
 *   - Detailed: cycle-level simulation via libgarnet.a
 */

#ifndef GARNET_NETWORK_H_
#define GARNET_NETWORK_H_

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <queue>
#include <sched.h>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "locks.h"
#include "log.h"
#include "network.h"

#ifdef HAVE_GARNET
#include "TopologyBuilders.hh"
#include "GarnetNetwork.hh"
#include "NetworkInterface.hh"
#include "Router.hh"
#include "CommonTypes.hh"
#include "gem5_compat/mem/ruby/network/MessageBuffer.hh"
#include "gem5_compat/mem/ruby/slicc_interface/Message.hh"
#include "gem5_compat/mem/ruby/system/RubySystem.hh"
#include "gem5_compat/sim/clocked_object.hh"
#endif

// ── Topology and routing enumerations ────────────────────────

enum class NoCTopology {
    MESH_2D, TORUS_2D, RING, CROSSBAR, FAT_TREE, BUS, H_TREE, CUSTOM
};

enum class NoCRouting {
    XY, DOR, SHORTEST, DIRECT, NCA, TABLE, CUSTOM
};

// ── String ↔ enum helpers ────────────────────────────────────

inline NoCTopology parseNoCTopology(const std::string& s) {
    if (s == "MESH_2D")   return NoCTopology::MESH_2D;
    if (s == "TORUS_2D")  return NoCTopology::TORUS_2D;
    if (s == "RING")      return NoCTopology::RING;
    if (s == "CROSSBAR")  return NoCTopology::CROSSBAR;
    if (s == "FAT_TREE")  return NoCTopology::FAT_TREE;
    if (s == "BUS")       return NoCTopology::BUS;
    if (s == "H_TREE")    return NoCTopology::H_TREE;
    if (s == "CUSTOM")    return NoCTopology::CUSTOM;
    warn("[GarnetNetwork] Unknown topology '%s', defaulting to MESH_2D", s.c_str());
    return NoCTopology::MESH_2D;
}

inline std::string nocTopologyStr(NoCTopology t) {
    switch (t) {
        case NoCTopology::MESH_2D:  return "MESH_2D";
        case NoCTopology::TORUS_2D: return "TORUS_2D";
        case NoCTopology::RING:     return "RING";
        case NoCTopology::CROSSBAR: return "CROSSBAR";
        case NoCTopology::FAT_TREE: return "FAT_TREE";
        case NoCTopology::BUS:      return "BUS";
        case NoCTopology::H_TREE:   return "H_TREE";
        case NoCTopology::CUSTOM:   return "CUSTOM";
    }
    return "UNKNOWN";
}

inline NoCRouting parseNoCRouting(const std::string& s) {
    if (s == "XY")       return NoCRouting::XY;
    if (s == "DOR")      return NoCRouting::DOR;
    if (s == "SHORTEST") return NoCRouting::SHORTEST;
    if (s == "DIRECT")   return NoCRouting::DIRECT;
    if (s == "NCA")      return NoCRouting::NCA;
    if (s == "TABLE")    return NoCRouting::TABLE;
    if (s == "CUSTOM")   return NoCRouting::CUSTOM;
    warn("[GarnetNetwork] Unknown routing '%s', defaulting to TABLE", s.c_str());
    return NoCRouting::TABLE;
}

inline std::string nocRoutingStr(NoCRouting r) {
    switch (r) {
        case NoCRouting::XY:       return "XY";
        case NoCRouting::DOR:      return "DOR";
        case NoCRouting::SHORTEST: return "SHORTEST";
        case NoCRouting::DIRECT:   return "DIRECT";
        case NoCRouting::NCA:      return "NCA";
        case NoCRouting::TABLE:    return "TABLE";
        case NoCRouting::CUSTOM:   return "CUSTOM";
    }
    return "UNKNOWN";
}

inline NoCRouting getDefaultRouting(NoCTopology topo) {
    switch (topo) {
        case NoCTopology::MESH_2D:  return NoCRouting::XY;
        case NoCTopology::TORUS_2D: return NoCRouting::DOR;
        case NoCTopology::RING:     return NoCRouting::SHORTEST;
        case NoCTopology::CROSSBAR: return NoCRouting::DIRECT;
        case NoCTopology::FAT_TREE: return NoCRouting::NCA;
        case NoCTopology::BUS:      return NoCRouting::DIRECT;
        case NoCTopology::H_TREE:   return NoCRouting::NCA;
        case NoCTopology::CUSTOM:   return NoCRouting::TABLE;
    }
    return NoCRouting::TABLE;
}

/**
 * NoC activity statistics for McPAT power modeling
 */
struct GarnetStats {
    // Traffic statistics
    uint64_t total_packets = 0;
    /* 1.11.92 (F2): TRUE flits -- ceil(message bits / source port width) per
     * packet, the count Garnet's NetworkInterface actually flitises into. It
     * was "+= 1 per packet", so a 576-bit data message over a 128-bit port
     * (5 flits) was counted as one, and every energy built on it was 5x
     * short. */
    uint64_t total_flits = 0;
    /* Router-to-router LINK hops, per packet (BFS link distance on CUSTOM).
     * Kept as the latency / M/D/1 input it has always been. NOT an energy
     * count: a packet between two endpoints on one router is 0 links but
     * crosses 1 router. */
    uint64_t total_hops = 0;
    /* 1.11.92 (F2): what McPAT charges per access -- one flit through one
     * router. Per packet: (routers crossed) x flits, routers crossed =
     * links + 1 (1 on a BUS/CROSSBAR). */
    uint64_t total_router_traversals = 0;
    uint64_t data_packets = 0;      // injected as MessageSizeType::Data
    uint64_t control_packets = 0;   // injected as MessageSizeType::Control

    // Router activity
    uint64_t buffer_reads = 0;
    uint64_t buffer_writes = 0;
    uint64_t crossbar_traversals = 0;
    uint64_t arbiter_events = 0;

    // Link activity
    uint64_t link_traversals = 0;

    // Timing
    uint64_t total_cycles = 0;
    uint64_t total_latency = 0;

    // Configuration (for power model)
    uint32_t num_routers = 0;
    uint32_t num_rows = 0;
    uint32_t num_cols = 0;
    uint32_t flit_size_bits = 128;
    uint32_t vcs_per_vnet = 4;
    double clock_mhz = 1000.0;

    // Topology / routing info
    std::string topology_name;
    std::string routing_name;

    /* 1.11.92 (F4/F1): per-HIERARCHY-LEVEL traversals (index = tree level,
     * 0 = subarray ... 6 = system root), ROI only.
     *   level_flit_traversals: flits through a router of that level.
     *     Detailed: MEASURED -- the routers' own crossbar counters, summed
     *     by the level the tree builder assigned each router. Analytical:
     *     the tier walk's router visits x flits per packet.
     *   level_passthrough_flit_traversals: the subset through pass-through
     *     routers (< 2 router children), kept apart for the F3 wire pricing.
     *   level_packet_traversals: packets crossing a router of that level,
     *     from the path walk (detailed) or the tier walk (analytical); the
     *     detailed value is a cross-check on the measured flits.
     * level_source says which ("measured_crossbar", "tier_walk" or "" when
     * the run has no per-level attribution). */
    uint64_t level_flit_traversals[7] = {0,0,0,0,0,0,0};
    uint64_t level_passthrough_flit_traversals[7] = {0,0,0,0,0,0,0};
    uint64_t level_packet_traversals[7] = {0,0,0,0,0,0,0};
    uint64_t level_buffer_reads[7] = {0,0,0,0,0,0,0};
    uint64_t level_buffer_writes[7] = {0,0,0,0,0,0,0};
    uint32_t level_routers[7] = {0,0,0,0,0,0,0};
    uint32_t level_branch_routers[7] = {0,0,0,0,0,0,0};
    std::string level_source;
    // Measured whole-network totals from the Garnet routers (detailed only).
    uint64_t measured_crossbar_flits = 0;
    uint64_t measured_buffer_reads = 0;
    uint64_t measured_buffer_writes = 0;
    uint32_t data_msg_bits = 576;
    uint32_t control_msg_bits = 64;
};


/**
 * Garnet-based network for ZSim (inherits from Network)
 *
 * Supports 7 built-in topologies + CUSTOM.  Two modes:
 *   1. Simple: pre-computed hop counts + M/D/1 queuing (fast)
 *   2. Detailed: cycle-level simulation via Garnet library
 */
class GarnetNetwork : public Network {
private:
    // ── Topology configuration ───────────────────────────────
    NoCTopology topology_;
    NoCRouting routing_;
    uint32_t numRows_;
    uint32_t numCols_;
    uint32_t numNodes_;

    // ── Latency parameters ───────────────────────────────────
    uint32_t routerLatency_;
    uint32_t linkLatency_;
    uint32_t injectionLatency_;
    uint32_t flitSizeBits_;
    uint32_t deadlockThreshold_ = 500000;  // VC-busy cycles before deadlock panic
    double clockMhz_;

    // ── Garnet detailed-mode parameters ──────────────────────
    uint32_t vcsPerVnet_;
    uint32_t buffersPerVc_;

    // ── Message sizes (bits) ────────────────────────────────
    // Control = request/command messages; Data = response/payload messages.
    // 0 = use defaults (control=64, data=576 for cacheline networks).
    uint32_t controlMsgBits_;
    uint32_t dataMsgBits_;

    // ── Node name → ID mapping ───────────────────────────────
    std::unordered_map<std::string, uint32_t> nodeMap_;

    // ── Latency cache (simple mode only) ─────────────────────
    std::unordered_map<std::string, uint32_t> latencyCache_;

    // ── Mode selection ───────────────────────────────────────
    bool cycleAccurate_;

    // ── Ring directionality ─────────────────────────────────
    bool ringUnidirectional_;

    // ── File paths ───────────────────────────────────────────
    std::string customTopoFile_;
    std::string routingTableFile_;

    // ── Custom topology adjacency list (for BFS hop counts) ──
    std::vector<std::vector<uint32_t>> customAdj_;
    std::vector<uint32_t> epToRouter_;  // 1.9.36: endpoint id -> attached router id
    uint32_t customRouterCount_ = 0;
    /* 1.11.92 (F2/F4): per-endpoint port width (the "ext" line's width field,
     * the width Garnet's NI flitises at), and per-router tree level / branch
     * flag from the "rlevel" lines main.cpp writes. Parent/depth are the
     * rooted-tree view (ROOT = router 0) used by the per-packet path walk. */
    std::vector<uint32_t> epWidth_;
    std::vector<int> routerLevel_;
    std::vector<uint8_t> routerBranch_;
    std::vector<int> routerParent_;
    std::vector<int> routerDepth_;
    bool customIsTree_ = false;

    // ── Statistics ───────────────────────────────────────────
    GarnetStats stats_;
    bool roi_rebased_ = false;   // 1.11.90
    uint64_t roiDroppedPackets_ = 0;   // 1.11.92: packets behind the dropped flits
    bool tierWalkUsed_ = false;        // 1.11.92 (F1): analytical counts recorded

#ifdef HAVE_GARNET
    // ── Cycle-accurate Garnet bridge ──────────────────────────
    gem5::ruby::garnet::GarnetNetwork* garnetNet_ = nullptr;
    gem5::ruby::RubySystem* rubySys_ = nullptr;
    std::vector<gem5::ruby::MessageBuffer*> allMsgBufs_;
    std::vector<std::vector<gem5::ruby::MessageBuffer*>> toNetBufs_;
    std::vector<std::vector<gem5::ruby::MessageBuffer*>> fromNetBufs_;
    bool garnetInitialized_ = false;
    gem5::Tick garnetTick_ = 0;

    // ── Concurrent packet tracking for contention modeling ────
    uint64_t nextTag_ = 1;
    // tag → (dst, injectTime) for packets still in-flight
    std::unordered_map<uint64_t, std::pair<uint32_t, uint64_t>> pendingInfo_;
    // tag → measured latency for packets that arrived while another thread was waiting
    std::unordered_map<uint64_t, uint32_t> completedPackets_;
    // set of destinations with at least one pending packet (avoids scanning all nodes)
    std::unordered_set<uint32_t> activeDsts_;

    /* 1.11.92 (F4): per-router activity, harvested from the Garnet routers'
     * own counters. *Seen_ is the last raw value read (the counters are
     * cumulative -- see Router.hh); *Tot_ is the ROI-window total. */
    std::vector<double> rtrXbarSeen_, rtrBufRdSeen_, rtrBufWrSeen_;
    std::vector<uint64_t> rtrXbarTot_, rtrBufRdTot_, rtrBufWrTot_;
#endif

    // Lock for thread-safe direct Garnet access from multiple PE-MIs
    lock_t garnetLock_;

public:
    /**
     * Full constructor with all topology parameters
     */
    GarnetNetwork(NoCTopology topology, uint32_t rows, uint32_t cols,
                  uint32_t routerLat, uint32_t linkLat,
                  bool cycleAccurate, NoCRouting routing,
                  uint32_t vcsPerVnet, uint32_t buffersPerVc,
                  double clockMhz, uint32_t flitSizeBits,
                  const std::string& customTopoFile = "",
                  const std::string& routingTableFile = "",
                  uint32_t controlMsgBits = 0,
                  uint32_t dataMsgBits = 0,
                  bool ringUnidirectional = false)
        : topology_(topology),
          routing_(routing),
          numRows_(rows), numCols_(cols),
          numNodes_(rows * cols),
          routerLatency_(routerLat), linkLatency_(linkLat),
          injectionLatency_(2),
          flitSizeBits_(flitSizeBits), clockMhz_(clockMhz),
          vcsPerVnet_(vcsPerVnet), buffersPerVc_(buffersPerVc),
          controlMsgBits_(controlMsgBits), dataMsgBits_(dataMsgBits),
          cycleAccurate_(cycleAccurate),
          ringUnidirectional_(ringUnidirectional),
          customTopoFile_(customTopoFile),
          routingTableFile_(routingTableFile)
    {
        // For non-grid topologies, numNodes_ is just num_pes
        // (rows*cols still set for grid topologies)

        // Compute effective num_routers based on topology
        uint32_t num_routers = numNodes_;
        if (topology_ == NoCTopology::BUS) {
            num_routers = 1;
        } else if (topology_ == NoCTopology::FAT_TREE) {
            // k-ary tree: more routers than endpoints
            uint32_t arity = 2;
            uint32_t levels = 1, cap = arity;
            while (cap < numNodes_) { cap *= arity; levels++; }
            num_routers = 0;
            uint32_t lr = (numNodes_ + arity - 1) / arity;
            for (uint32_t l = 0; l < levels; l++) {
                num_routers += lr;
                lr = (lr + arity - 1) / arity;
            }
        } else if (topology_ == NoCTopology::H_TREE) {
            uint32_t levels = 1, cap = 2;
            while (cap < numNodes_) { cap *= 2; levels++; }
            num_routers = (1u << levels) - 1;
        }

        // Initialize stats
        stats_.num_routers = num_routers;
        stats_.num_rows = rows;
        stats_.num_cols = cols;
        stats_.flit_size_bits = flitSizeBits;
        stats_.vcs_per_vnet = vcsPerVnet;
        stats_.clock_mhz = clockMhz;
        stats_.topology_name = nocTopologyStr(topology_);
        stats_.routing_name = nocRoutingStr(routing_);
        // 1.11.92 (F2): the message sizes Garnet flitises (same defaults as
        // gem5_compat Network::m_*_msg_size: control 64 b, data 576 b).
        stats_.data_msg_bits = dataMsgBits_ > 0 ? dataMsgBits_ : 576;
        stats_.control_msg_bits = controlMsgBits_ > 0 ? controlMsgBits_ : 64;

        // Parse custom topology file if needed
        if (topology_ == NoCTopology::CUSTOM && !customTopoFile_.empty()) {
            parseCustomTopologyFile();
        }

        info("[GarnetNetwork] Topology: %s, Routing: %s",
             nocTopologyStr(topology_).c_str(),
             nocRoutingStr(routing_).c_str());
        if (topology_ == NoCTopology::MESH_2D || topology_ == NoCTopology::TORUS_2D) {
            info("  Dimensions: %dx%d", rows, cols);
        }
        info("  Router latency: %d cycles, Link latency: %d cycles", routerLat, linkLat);
        info("  VCs/vnet: %d, Buffers/VC: %d", vcsPerVnet, buffersPerVc);
        info("  Clock: %.1f MHz, Flit size: %d bits", clockMhz, flitSizeBits);
        if (controlMsgBits_ > 0 || dataMsgBits_ > 0)
            info("  Message sizes: control=%db, data=%db",
                 controlMsgBits_ > 0 ? controlMsgBits_ : 64,
                 dataMsgBits_ > 0 ? dataMsgBits_ : 576);
        info("  Mode: %s", cycleAccurate ? "detailed" : "simple");
        futex_init(&garnetLock_);
        futex_init(&batchLock_);
        // 1.9.0 deterministic epoch-frozen feedback (thread-MPI). E_phases is the
        // epoch length in phases; larger = looser barrier / more lag, 1 = per-phase.
        futex_init(&epochLock_);
        {
            const char* e = getenv("PIMID_DET_EPOCH_PHASES");
            if (e && atoi(e) > 0) detEpochPhases_ = (uint32_t)atoi(e);
        }
        info("[EpochFeedback] det-epoch-frozen NoC pricing table init: "
             "E_phases=%u (thread-MPI deterministic measured feedback; "
             "PIMID_MPI_ANALYTICAL_PRICING=1 restores the analytical override)",
             detEpochPhases_);
    }

    /**
     * Legacy constructor (MESH_2D only, backward-compatible)
     */
    GarnetNetwork(uint32_t rows, uint32_t cols,
                  uint32_t routerLat = 1, uint32_t linkLat = 1,
                  bool cycleAccurate = false,
                  double clockMhz = 1000.0, uint32_t flitSizeBits = 128)
        : GarnetNetwork(NoCTopology::MESH_2D, rows, cols,
                        routerLat, linkLat, cycleAccurate,
                        NoCRouting::XY, 4, 4, clockMhz, flitSizeBits)
    {}

    // ── Accessors ────────────────────────────────────────────

    NoCTopology getTopology() const { return topology_; }
    NoCRouting  getRouting()  const { return routing_; }
    void setDeadlockThreshold(uint32_t t) { deadlockThreshold_ = t; }
    uint32_t getVcsPerVnet()  const { return vcsPerVnet_; }
    uint32_t getBuffersPerVc() const { return buffersPerVc_; }
    uint32_t getNumNodes()    const { return numNodes_; }
    bool isRingUnidirectional() const { return ringUnidirectional_; }
    bool isCycleAccurate()    const { return cycleAccurate_; }

#ifdef HAVE_GARNET
    /**
     * Save the global gem5 tick and set it to this network's Garnet tick.
     * Returns the saved tick so it can be restored with restoreTick().
     * Used when multiple GarnetNetwork instances share the gem5 singleton.
     */
    uint64_t saveAndSetTick() {
        uint64_t saved = gem5::curTickRef();
        gem5::curTickRef() = garnetTick_;
        return saved;
    }

    /**
     * Restore the global gem5 tick to a previously saved value.
     * Must be called after saveAndSetTick() when done with this network.
     */
    void restoreTick(uint64_t savedTick) {
        garnetTick_ = gem5::curTickRef();
        gem5::curTickRef() = savedTick;
    }
#endif

    // ── Node registration ────────────────────────────────────

    // Lowest node id not yet bound to any endpoint (for collision-free
    // assignment of non-numeric / auto-named endpoints). Co-located numeric
    // endpoints (l1d-3, l1i-3, l2-3 all at core 3) intentionally share a node,
    // so a "taken" id is fine for numeric names -- we only need a free slot for
    // the hash-free fallback below.
    uint32_t nextFreeNodeId() const {
        uint32_t cap = (numNodes_ > 0) ? numNodes_ : ((uint32_t)nodeMap_.size() + 1);
        std::unordered_set<uint32_t> used;
        for (const auto& kv : nodeMap_) used.insert(kv.second);
        for (uint32_t i = 0; i < cap; i++)
            if (!used.count(i)) return i;
        return cap > 0 ? cap - 1 : 0;   // all slots taken (shouldn't happen)
    }

    void registerNode(const char* name, uint32_t nodeId) {
        if (nodeMap_.find(name) != nodeMap_.end()) return;  // idempotent
        if (numNodes_ > 0 && nodeId >= numNodes_) {
            // Out of range: a real wiring/sizing error. Do NOT silently wrap
            // (that would alias this endpoint onto an unrelated node). Warn
            // loudly and bind to a free slot so the mis-wire is visible.
            uint32_t fixed = nextFreeNodeId();
            warn("[GarnetNetwork] endpoint '%s' node id %u out of range "
                 "(numNodes=%u) -- NoC undersized; bound to free node %u",
                 name, nodeId, numNodes_, fixed);
            nodeId = fixed;
        }
        nodeMap_[name] = nodeId;
    }

    void autoRegisterNode(const char* name) {
        std::string s(name);
        // Accept '-' or '_' before a numeric node index (e.g. "l1d-3", "mem_12").
        size_t pos = s.find_last_of("-_");
        if (pos != std::string::npos) {
            try {
                uint32_t id = std::stoul(s.substr(pos + 1));
                registerNode(name, id);   // numeric suffix names the intended node
                return;
            } catch (const std::exception&) { /* non-numeric -> fall through */ }
        }
        // Non-numeric name (e.g. "pe-mc-splitter"): assign the next FREE node id.
        // The old hash%%N could silently alias two distinct endpoints onto one
        // node; sequential free assignment guarantees a unique, valid mapping.
        registerNode(name, nextFreeNodeId());
    }

    // ── Main interface ───────────────────────────────────────

    uint32_t getRTT(const char* src, const char* dst) override {
        if (nodeMap_.find(src) == nodeMap_.end()) autoRegisterNode(src);
        if (nodeMap_.find(dst) == nodeMap_.end()) autoRegisterNode(dst);

        std::string key = std::string(src) + " " + dst;
        auto it = latencyCache_.find(key);
        if (it != latencyCache_.end()) {
            return it->second;
        }

        uint32_t srcId = nodeMap_[src];
        uint32_t dstId = nodeMap_[dst];

        uint32_t latency;
        if (cycleAccurate_) {
            latency = getCycleAccurateLatency(srcId, dstId);
        } else {
            latency = getAnalyticalLatency(srcId, dstId);
        }

        uint32_t rtt = 2 * latency;

        // Don't cache cycle-accurate results (they depend on dynamic state)
        if (!cycleAccurate_) {
            latencyCache_[key] = rtt;
        }

        stats_.total_packets++;
        return rtt;
    }

    // ── Phase-sync injection/dequeue interface ─────────────────

#ifdef HAVE_GARNET
    /**
     * Ensure Garnet is fully initialized (routers, links, buffers).
     * Must be called before injectMessage/checkMessageReady/dequeueMessage.
     * Safe to call multiple times (no-op if already initialized).
     */
    void ensureInitialized() {
        if (!garnetInitialized_) {
            initGarnetNetwork();
        }
    }

    void injectMessage(uint32_t srcNode, uint32_t vnet,
                        std::shared_ptr<gem5::ruby::Message> msg,
                        uint64_t tick) {
        if (srcNode < toNetBufs_.size() && vnet < toNetBufs_[srcNode].size()) {
            toNetBufs_[srcNode][vnet]->enqueue(msg, tick, uint64_t(1));
        }
    }

    bool checkMessageReady(uint32_t dstNode, uint32_t vnet, uint64_t tick) {
        if (dstNode < fromNetBufs_.size() && vnet < fromNetBufs_[dstNode].size()) {
            return fromNetBufs_[dstNode][vnet]->isReady(tick);
        }
        return false;
    }

    void dequeueMessage(uint32_t dstNode, uint32_t vnet, uint64_t tick) {
        if (dstNode < fromNetBufs_.size() && vnet < fromNetBufs_[dstNode].size()) {
            fromNetBufs_[dstNode][vnet]->dequeue(tick);
        }
    }

    gem5::ruby::MessageBuffer* getFromNetBuf(uint32_t dstNode, uint32_t vnet) {
        if (dstNode < fromNetBufs_.size() && vnet < fromNetBufs_[dstNode].size())
            return fromNetBufs_[dstNode][vnet];
        return nullptr;
    }

    /**
     * Direct per-access Garnet injection from PE-MIs.
     *
     * Thread-safe: acquires garnetLock_.  Does NOT reset network state
     * between calls — residual buffer/credit state from prior in-flight
     * packets carries over, so back-to-back injections see real network
     * state.  One real, tag-matched packet is injected per call; the
     * tag identifies it for dequeue.  Cross-rank contention (the only
     * concurrency MPI's sequential per-rank sends actually have) is
     * modeled by the shared occupancy table in libpimid_mpi, NOT by any
     * within-rank background traffic.
     *
     * Returns one-way latency in Garnet cycles (caller doubles for RTT).
     */
    uint32_t accessNetwork(uint32_t src, uint32_t dst, uint64_t issueTime) {
        if (src == dst) return 0;
        if (src >= numNodes_ || dst >= numNodes_) {
            return getAnalyticalLatency(src, dst);
        }

        futex_lock(&garnetLock_);

        if (!garnetInitialized_) {
            initGarnetNetwork();
        }

        gem5::curTickRef() = garnetTick_;
        uint64_t injectTime = std::max(issueTime, garnetTick_);

        // Advance to injection time (processes events for in-flight packets)
        while (gem5::curTickRef() < injectTime) {
            if (!gem5::EventQueue::instance().processOneEvent()) {
                gem5::curTickRef() = injectTime;
                break;
            }
        }

        // ── Inject real packet with unique tag ───────────────────
        uint64_t tag = nextTag_++;
        auto msg = std::make_shared<gem5::ruby::SimpleMessage>(
            src, dst, gem5::ruby::MessageSizeType::Data, gem5::curTickRef());
        msg->setTag(tag);
        toNetBufs_[src][0]->enqueue(msg, gem5::curTickRef(), uint64_t(1));

        // ── Tick until our tagged packet arrives ─────────────────
        uint64_t maxTick = gem5::curTickRef() + 100000;
        uint32_t latCycles = 0;

        while (gem5::curTickRef() < maxTick) {
            // Check if any message is ready at our destination
            if (fromNetBufs_[dst][0]->isReady(gem5::curTickRef())) {
                auto peekMsg = std::dynamic_pointer_cast<
                    gem5::ruby::SimpleMessage>(
                    fromNetBufs_[dst][0]->peekMsgPtr());
                uint64_t pktTag = peekMsg ? peekMsg->getTag() : 0;
                fromNetBufs_[dst][0]->dequeue(gem5::curTickRef());

                if (pktTag == tag) {
                    // Our packet arrived
                    latCycles = static_cast<uint32_t>(
                        gem5::curTickRef() - injectTime);
                    break;
                }
                // Older real packet still draining — discard and keep ticking
                continue;
            }

            if (!gem5::EventQueue::instance().processOneEvent()) {
                gem5::curTickRef()++;
            }
        }

        if (latCycles == 0) {
            // Timeout — use simple model fallback
            latCycles = getAnalyticalLatency(src, dst);
        }

        // Save garnet time
        garnetTick_ = gem5::curTickRef();

        // Update stats (1.11.92 F2: true flits and router traversals)
        uint32_t hops = getHopCount(src, dst);
        stats_.total_packets++;
        stats_.total_latency += latCycles;
        countPacket_(src, dst, hops, false);

        futex_unlock(&garnetLock_);

        return latCycles;
    }

public:
    // ── Phase-level batch: record all PE remote accesses with their
    //    real ZSim cycle timestamps, then replay through Garnet.
    /* 1.11.92 (F2): `ctrl` selects MessageSizeType::Control (control_msg_bits)
     * over Data (data_msg_bits) for the injection. Every recording site in
     * this tree records a data-bearing access (one packet per access, RTT =
     * 2 x its one-way latency), so all of them pass Data today; the field
     * exists so a control injection is flitised and counted at its own size
     * the moment one is recorded. */
    struct BatchAccess { uint32_t src, dst; uint64_t cycle; uint8_t ctrl = 0; };
private:
    std::vector<BatchAccess> phaseBatch_;
    // Published rolling one-way avg latency (lock-free read by all PE threads).
    std::atomic<uint32_t> batchAvgLatency_{0};
    volatile uint64_t batchLastPhase_ = 0;
    lock_t batchLock_;

    // -- 1.9.0 deterministic epoch-frozen feedback (thread-MPI) --------------
    // Per-epoch FROZEN one-way NoC latency, keyed by epoch = phaseNum / E_phases.
    // Filled by runBatchDrain_ with an INTEGER sum/count reducer over the SET of
    // phase buckets whose phase maps to the epoch (order-free => deterministic,
    // independent of which thread drained which bucket first). Read by PE-MIs via
    // latencyForEpoch(epochOf(access)-1): a keyed table lookup, NOT a rolling-EWMA
    // snapshot -- this is what removes the read-instant nondeterminism (E1).
    struct EpochLatAcc { uint64_t sumLat = 0; uint64_t cnt = 0; };
    std::map<uint64_t, EpochLatAcc> epochAvgLat_;
    lock_t epochLock_;
    uint32_t detEpochPhases_ = 4;   // E_phases (env PIMID_DET_EPOCH_PHASES, def 4)
    // Records accumulated but not yet <= the consistent cut. Drained ONLY by
    // the single-threaded barrier fold (foldCompletePhases), never by racing access
    // threads -- so a phase's whole record set is replayed exactly once, together,
    // giving a deterministic per-epoch sample.
    // Kept ALREADY BUCKETED by roiRel-phase. A fold then touches only the
    // leading buckets (those wholly below the cut) and never walks or copies
    // the retained ones: the retained set is the roiRel spread across ranks,
    // which is a workload property and stays large (order 1e6 records for
    // rendezvous-heavy message-passing kernels), so re-scanning it every phase
    // made each barrier callback cost O(backlog) instead of O(new + folded).
    std::map<uint64_t, std::vector<BatchAccess>> pendingByPhase_;

    // PIMID_INJ_DUMP census: per-source totals, accumulated under batchLock_ so
    // the sums are order-independent and comparable across runs.
    struct InjCensus { uint64_t n = 0, dstSum = 0, cycSum = 0; };
    std::vector<InjCensus> injCensus_ = std::vector<InjCensus>(1024);

public:

    /**
     * Reset the entire Garnet network to a pristine state for a new batch.
     * Clears EventQueue, all Consumer schedules, all VC states/credits,
     * all flit/credit buffers, and all MessageBuffers.  Resets global tick
     * and garnetTick_ to 0.  Topology and routing tables are preserved.
     */
    void resetGarnetState() {
        if (!garnetInitialized_ || !garnetNet_) return;

        /* 1.11.92 (F4): fold the routers' per-flit counters into the
         * per-router totals BEFORE the reset, and re-read them after it.
         * resetNetworkState() does not zero them today (only resetStats()
         * does, and nothing calls it), so they are cumulative and the harvest
         * takes deltas; the re-read keeps that correct if a future reset
         * does zero them. Caller holds garnetLock_. */
        harvestRouterCountersLocked_();

        // 1. Reset the gem5 Garnet network (routers, NIs, links, EventQueue)
        garnetNet_->resetNetworkState();
        resnapRouterCountersLocked_();

        // 2. Clear all MessageBuffers (toNet and fromNet queues)
        for (auto* mb : allMsgBufs_) {
            mb->clear();
        }

        // 3. Reset garnet tick tracking
        garnetTick_ = 0;
    }

    /**
     * Record a remote access for batch processing.
     * Called from PE-MI on every remote access when Garnet is cycle-accurate.
     * Thread-safe (uses batchLock_).
     */
    void recordBatchAccess(uint32_t src, uint32_t dst, uint64_t cycle,
                           bool ctrl = false) {
        futex_lock(&batchLock_);
        phaseBatch_.push_back({src, dst, cycle, (uint8_t)(ctrl ? 1 : 0)});
        // PIMID_INJ_DUMP=<path>: order-independent census of everything injected,
        // to separate "the runs execute different accesses" from "the runs bin the
        // same accesses into different phases". Per source node: count, and sums
        // of dst and cycle. Two runs with identical censuses but divergent
        // per-phase membership are a BINNING problem (the phase a record lands in
        // depends on when a thread happened to record it); differing censuses mean
        // the simulated execution itself diverged.
        static const bool injDbg = (getenv("PIMID_INJ_DUMP") != nullptr);
        if (injDbg && src < injCensus_.size()) {
            injCensus_[src].n++;
            injCensus_[src].dstSum += dst;
            injCensus_[src].cycSum += cycle;
        }
        futex_unlock(&batchLock_);
    }

    /** Write the injection census (PIMID_INJ_DUMP). Called at teardown. */
    void dumpInjectionCensus() {
        const char* p = getenv("PIMID_INJ_DUMP");
        if (!p || !p[0]) return;
        FILE* f = fopen(p, "w");
        if (!f) return;
        uint64_t tn = 0, td = 0, tc = 0;
        for (size_t s = 0; s < injCensus_.size(); s++) {
            if (!injCensus_[s].n) continue;
            fprintf(f, "src=%zu n=%lu dstSum=%lu cycSum=%lu\n", s,
                    (unsigned long)injCensus_[s].n,
                    (unsigned long)injCensus_[s].dstSum,
                    (unsigned long)injCensus_[s].cycSum);
            tn += injCensus_[s].n; td += injCensus_[s].dstSum; tc += injCensus_[s].cycSum;
        }
        fprintf(f, "TOTAL n=%lu dstSum=%lu cycSum=%lu\n",
                (unsigned long)tn, (unsigned long)td, (unsigned long)tc);
        fclose(f);
    }

    /** Returns the smoothed average one-way latency from last Garnet batch.
     *  Lock-free read (atomic); safe to call from any PE thread every access. */
    uint32_t getBatchAvgLatency() const {
        return batchAvgLatency_.load(std::memory_order_relaxed);
    }

    // -- 1.9.0 epoch-frozen feedback API -------------------------------------
    uint32_t detEpochPhases() const {
        return detEpochPhases_ > 0 ? detEpochPhases_ : 1;
    }

    /** Single-threaded BARRIER fold on the per-core roiRel axis (called from
     *  EndOfPhaseActions / atSyncFunc while all cores wait at the phase barrier).
     *  Records are stamped on each core's OWN roiRel (curCycle - its roiBaseCycle),
     *  removing the cross-core startup SKEW (rank 0 did init, others spawned fresh)
     *  that the global-numPhases stamp could not align. The caller computes the
     *  CONSISTENT CUT = min roiRel over cores still making progress (finished/parked
     *  cores excluded), read atomically at the barrier -> no blocking spin, no
     *  deadlock. Every pending record with roiRel <= cut is FINAL (no active core
     *  will inject below it), bucketed by roiRel-phase, replayed EXACTLY ONCE
     *  through runBatchDrain_. ONE folder + roiRel axis => deterministic membership.
     */
    void foldByCut(uint64_t cut, uint32_t pl) {
        futex_lock(&batchLock_);
        size_t dbgIn = phaseBatch_.size();
        for (auto& a : phaseBatch_) {
            uint64_t p = (pl > 0) ? a.cycle / pl : 0;
            pendingByPhase_[p].push_back(a);
        }
        phaseBatch_.clear();
        // PIMID_FOLD_DEBUG=1: per-fold trace of the consistent cut and the pending
        // backlog, to detect a pinned cut (backlog grows without bound => each fold
        // becomes O(backlog) and the run degrades quadratically).
        static const bool foldDbg = (getenv("PIMID_FOLD_DEBUG") != nullptr);
        static uint64_t foldCalls = 0;
        bool dbgNow = foldDbg && ((foldCalls++ % 100) == 0);
        // Fold a bucket ONLY when its WHOLE roiRel-phase is below the cut
        // (cut >= (p+1)*pl): every active core has passed it, so no more records
        // will land in it. Folding at roiRel <= cut alone would fold a bucket
        // PARTIALLY when the cut lands mid-bucket, splitting a phase's records
        // across two fold-barriers -> each partial replay measures a different
        // contention -> nondeterminism (the phase-3 first-divergence).
        // (p+1)*pl increases with p, so the foldable buckets are exactly the
        // leading ones: stop at the first bucket that is not yet final.
        std::map<uint64_t, std::vector<BatchAccess>> buckets;
        while (!pendingByPhase_.empty()) {
            auto it = pendingByPhase_.begin();
            if ((it->first + 1) * (uint64_t)pl > cut) break;
            buckets.emplace(it->first, std::move(it->second));
            pendingByPhase_.erase(it);
        }
        size_t dbgPend = 0, dbgBuckets = buckets.size();
        futex_unlock(&batchLock_);
        if (dbgNow) {
            for (auto& kv : pendingByPhase_) dbgPend += kv.second.size();
            uint64_t oldestKept = pendingByPhase_.empty()
                                ? 0ull : pendingByPhase_.begin()->second.front().cycle;
            info("[FoldDbg] cut=%lu in=%zu pending=%zu keptBuckets=%zu foldedBuckets=%zu oldestKept=%lu",
                 cut, dbgIn, dbgPend, pendingByPhase_.size(), dbgBuckets,
                 (unsigned long)oldestKept);
        }
        for (auto& kv : buckets)
            runBatchDrain_(std::move(kv.second), kv.first);
    }

    /** Frozen one-way latency for a COMPLETED epoch: the integer mean over the
     *  SET of phase buckets that mapped to it (order-free). 0 => no bucket yet,
     *  caller bootstraps with the static analytical latency (as today). */
    uint32_t latencyForEpoch(uint64_t epoch) {
        futex_lock(&epochLock_);
        uint32_t v = 0;
        auto it = epochAvgLat_.find(epoch);
        if (it != epochAvgLat_.end() && it->second.cnt > 0)
            v = (uint32_t)(it->second.sumLat / it->second.cnt);
        futex_unlock(&epochLock_);
        return v;
    }

private:
    /** Fold one drained phase bucket's delivered-latency total into its epoch
     *  (epoch = phaseNum / E_phases) with an integer sum/count reducer. */
    void foldEpochLat_(uint64_t phaseNum, uint64_t totalLat, uint32_t delivered) {
        if (delivered == 0) return;
        // phaseNum is already the roiRel-phase (records carry per-core roiRel).
        uint64_t epoch = phaseNum / (detEpochPhases_ > 0 ? detEpochPhases_ : 1);
        futex_lock(&epochLock_);
        auto& e = epochAvgLat_[epoch];
        e.sumLat += totalLat;
        e.cnt    += delivered;
        futex_unlock(&epochLock_);
    }
public:

    /**
     * Process accumulated remote accesses through Garnet as a concurrent batch.
     * All packets are injected with staggered timing (spread over phaseLength)
     * and routed simultaneously, so contention is physically modeled:
     *   - BUS: 1 link → packets serialize, high latency
     *   - CROSSBAR: N/2 concurrent → low latency
     *   - MESH/TORUS: multi-hop contention at intermediate routers
     *
     * Called at phase boundary from PE-MI.  Idempotent per phase.
     */
    void processBatch(uint64_t phaseNum, uint32_t /*phaseLength*/) {
        futex_lock(&batchLock_);
        if (batchLastPhase_ >= phaseNum || phaseBatch_.empty()) {
            futex_unlock(&batchLock_);
            return;
        }
        batchLastPhase_ = phaseNum;

        auto batch = std::move(phaseBatch_);
        phaseBatch_.clear();
        phaseBatch_.reserve(batch.size());
        futex_unlock(&batchLock_);

        runBatchDrain_(std::move(batch), phaseNum);
    }

    /**
     * Drain an EXPLICIT record set through Garnet (shared detailed-MPI mode:
     * the caller collected the merged multi-rank stream from the shared NoC
     * log up to a consistent cut). Bypasses phaseBatch_ entirely -- in shared
     * mode records live in the shm rings, never in phaseBatch_. Same replay
     * engine as processBatch (reset per drain => a drain is a pure function
     * of the record window, so every rank replaying the identical merged
     * stream IS one logical network). Idempotence is the caller's job (ring
     * cursors advance exactly once per consumed record).
     */
    void processBatchRecords(std::vector<BatchAccess>&& records, uint64_t phaseNum) {
        if (records.empty()) return;
        runBatchDrain_(std::move(records), phaseNum);
    }

private:
    /**
     * Core Garnet drain over an already-claimed batch. Holds garnetLock_ for the
     * duration (the shared gem5 Garnet network is single cycle-accurate engine).
     * Publishes the rolling EWMA into batchAvgLatency_ (atomic) on completion.
     * Callable from any thread (uses thread_local gem5 tick/EventQueue).
     */
    void runBatchDrain_(std::vector<BatchAccess> batch, uint64_t phaseNum) {
        futex_lock(&garnetLock_);

        if (!garnetInitialized_) {
            initGarnetNetwork();
        }

        // Reset Garnet for each phase — clean slate
        resetGarnetState();

        // ── Sort by ZSim cycle timestamp so packets enter Garnet in the order
        //    they'd naturally occur in real hardware. TOTAL order (cycle, src,
        //    dst): equal-cycle ties resolve identically everywhere, so the N
        //    per-rank replicas replaying the same merged multi-rank stream
        //    stay deterministic (equal records are interchangeable). ──
        std::sort(batch.begin(), batch.end(),
                  [](const BatchAccess& a, const BatchAccess& b) {
                      if (a.cycle != b.cycle) return a.cycle < b.cycle;
                      if (a.src != b.src) return a.src < b.src;
                      return a.dst < b.dst;
                  });

        // Normalize timestamps: offset so the first packet is at tick 0
        uint64_t baseTime = batch.empty() ? 0 : batch[0].cycle;
        gem5::curTickRef() = 0;

        // DIAG (PIMID_NOC_DIAG_SPAN=1): a drain's wall cost is dominated by the
        // batch's CYCLE SPAN (the replay ticks through the whole injection
        // window). A span of ~1 phase is healthy; a span of ~the whole run
        // means some records carry stale clocks. Print span + the stale
        // records' sources so the bad clock domain can be identified.
        static const bool diagSpan = (getenv("PIMID_NOC_DIAG_SPAN") != nullptr);
        if (diagSpan && !batch.empty()) {
            uint64_t minC = batch.front().cycle, maxC = batch.back().cycle;
            info("[SpanDiag] phase=%lu n=%zu minCyc=%lu maxCyc=%lu span=%lu",
                 phaseNum, batch.size(), minC, maxC, maxC - minC);
            if (maxC - minC > 1000000) {   // >100 phases: list the stragglers
                uint32_t shown = 0;
                for (auto& a : batch) {
                    if (maxC - a.cycle > 1000000 && shown < 8) {
                        info("[SpanDiag]   stale rec: src=%u dst=%u cyc=%lu "
                             "(%lu behind max)", a.src, a.dst, a.cycle,
                             maxC - a.cycle);
                        shown++;
                    }
                }
            }
        }

        // ── Natural inject-and-drain: inject each packet at its
        //    actual ZSim timestamp, let Garnet route them all
        //    concurrently. The network physically models contention. ──
        uint64_t tag = 1;
        std::unordered_map<uint64_t, uint64_t> tagToInjectTick;
        std::unordered_map<uint64_t, uint32_t> tagToSrc;  // epoch-dump attribution
        std::map<uint32_t, std::pair<uint64_t,uint32_t>> perSrcLat;  // src->{latSum,cnt}
        std::unordered_set<uint32_t> dstSet;
        uint32_t validCount = 0;
        size_t nextInjectIdx = 0;

        // Pre-scan to count valid and build dest set
        for (auto& acc : batch) {
            if (acc.src != acc.dst && acc.src < numNodes_ && acc.dst < numNodes_) {
                validCount++;
                dstSet.insert(acc.dst);
            }
        }

        if (validCount == 0) {
            futex_unlock(&garnetLock_);
            return;
        }

        uint64_t totalLat = 0;
        uint32_t delivered = 0;
        uint64_t lastInjectTime = batch.back().cycle - baseTime;
        // Drain window scaled to the batch: with the in-flight cap below, the
        // network drains gracefully even for slow/narrow-link techs, but a large
        // batch needs proportionally more ticks to fully clear. Generous + safety.
        uint64_t maxTick = lastInjectTime + (uint64_t)validCount * 256 + 1000000;

        // NO artificial in-flight cap. The network's OWN flow control -- bounded
        // VC buffers + credit backpressure -- is the physical limiter on
        // outstanding packets. Under heavy offered load the channel saturates
        // and queues build (bandwidth-bound: the real DRAM regime), and the
        // up/down TreeRouter guarantees forward progress so this no longer
        // collapses into deadlock (the only reason a cap ever existed). Inject
        // every packet whose recorded time has come and let credits throttle it.
        while (delivered < validCount && gem5::curTickRef() < maxTick) {
            // ── Inject: enqueue packets whose recorded time has come ──
            while (nextInjectIdx < batch.size()) {
                auto& acc = batch[nextInjectIdx];
                uint64_t injectTick = acc.cycle - baseTime;
                if (injectTick > gem5::curTickRef()) break;  // not yet

                if (acc.src != acc.dst && acc.src < numNodes_ &&
                    acc.dst < numNodes_) {
                    auto msg = std::make_shared<gem5::ruby::SimpleMessage>(
                        acc.src, acc.dst,
                        acc.ctrl ? gem5::ruby::MessageSizeType::Control
                                 : gem5::ruby::MessageSizeType::Data,
                        gem5::curTickRef());
                    msg->setTag(tag);
                    toNetBufs_[acc.src][0]->enqueue(
                        msg, gem5::curTickRef(), uint64_t(1));
                    tagToInjectTick[tag] = gem5::curTickRef();
                    tagToSrc[tag] = acc.src;
                    tag++;
                }
                nextInjectIdx++;
            }

            // ── Drain: check destinations for arrivals ──
            for (uint32_t d : dstSet) {
                while (fromNetBufs_[d][0]->isReady(gem5::curTickRef())) {
                    auto peekMsg = std::dynamic_pointer_cast<
                        gem5::ruby::SimpleMessage>(
                        fromNetBufs_[d][0]->peekMsgPtr());
                    uint64_t pktTag = peekMsg ? peekMsg->getTag() : 0;
                    fromNetBufs_[d][0]->dequeue(gem5::curTickRef());

                    auto it = tagToInjectTick.find(pktTag);
                    if (it != tagToInjectTick.end()) {
                        uint64_t lat = gem5::curTickRef() - it->second;
                        totalLat += lat;
                        tagToInjectTick.erase(it);
                        delivered++;
                        auto st = tagToSrc.find(pktTag);
                        if (st != tagToSrc.end()) {
                            auto& ps = perSrcLat[st->second];
                            ps.first += lat; ps.second += 1;
                        }
                    }
                }
            }

            if (delivered >= validCount) break;

            // ── Advance Garnet simulation ──
            if (!gem5::EventQueue::instance().processOneEvent()) {
                gem5::curTickRef()++;
            }
        }

        // Update smoothed average latency (atomic publish: PE threads read it
        // lock-free via getBatchAvgLatency()).
        if (delivered > 0) {
            uint32_t newAvg = (uint32_t)(totalLat / delivered);
            uint32_t oldAvg = batchAvgLatency_.load(std::memory_order_relaxed);
            uint32_t next = (oldAvg == 0) ? newAvg : (oldAvg + newAvg) / 2;
            batchAvgLatency_.store(next, std::memory_order_relaxed);
            if (phaseNum <= 5 || phaseNum % 100 == 0) {
                info("[GarnetBatch] phase=%lu batch=%u delivered=%u "
                     "avgLat=%u smooth=%u→%u ticks=%lu",
                     phaseNum, validCount, delivered, newAvg,
                     oldAvg, next,
                     gem5::curTickRef());
            }
        }

        // 1.9.0: fold this phase bucket into its epoch's FROZEN sample (integer
        // sum/count). The EWMA above stays for the OMP/non-MPI live-feedback path;
        // thread-MPI reads latencyForEpoch() instead. Order-free => deterministic.
        foldEpochLat_(phaseNum, totalLat, delivered);

        // Epoch-fold DUMP (PIMID_DET_EPOCH_DUMP=<path>): per fold, append phase,
        // epoch, delivered/total latency, an order-independent INPUT-record checksum
        // (to separate membership divergence from measured-latency divergence), and
        // per-source-node subtotals. Diff across reps to find the FIRST divergent
        // epoch and which source diverged. Serialized by garnetLock_ (held here).
        {
            static const char* dumpPath = getenv("PIMID_DET_EPOCH_DUMP");
            if (dumpPath && dumpPath[0]) {
                static FILE* df = fopen(dumpPath, "w");
                if (df) {
                    uint32_t Ed = detEpochPhases_ > 0 ? detEpochPhases_ : 1;
                    uint64_t epoch = phaseNum / Ed;
                    uint64_t inChk = 0, inCnt = 0;
                    for (auto& a : batch)
                        if (a.src != a.dst && a.src < numNodes_ &&
                            a.dst < numNodes_) {
                            inChk += (uint64_t)a.src * 1000003ull +
                                     (uint64_t)a.dst * 10007ull + a.cycle;
                            inCnt++;
                        }
                    fprintf(df, "phase=%lu epoch=%lu valid=%u delivered=%u "
                                "totalLat=%lu inCnt=%lu inChk=%lu |",
                            (unsigned long)phaseNum, (unsigned long)epoch,
                            validCount, delivered, (unsigned long)totalLat,
                            (unsigned long)inCnt, (unsigned long)inChk);
                    for (auto& kv : perSrcLat)
                        fprintf(df, " s%u:%u/%lu", kv.first, kv.second.second,
                                (unsigned long)kv.second.first);
                    fprintf(df, "\n");
                    fflush(df);
                }
            }
        }

        if (delivered < validCount) {
            warn("[GarnetNetwork] Batch: %u/%u delivered, %u timed out (phase %lu)",
                 delivered, validCount, validCount - delivered, phaseNum);
        }

        // Update stats (1.11.92 F2: true flits and router traversals per
        // packet; the routers' own counters are harvested at the reset)
        stats_.total_packets += delivered;
        stats_.total_latency += totalLat;
        for (auto& acc : batch) {
            if (acc.src == acc.dst || acc.src >= numNodes_ || acc.dst >= numNodes_)
                continue;
            uint32_t hops = getHopCount(acc.src, acc.dst);
            countPacket_(acc.src, acc.dst, hops, acc.ctrl != 0);
        }

        garnetTick_ = gem5::curTickRef();
        futex_unlock(&garnetLock_);
    }

public:
#endif

    // ── Statistics ───────────────────────────────────────────

    void getStats(uint64_t& packets, uint64_t& hops, uint64_t& avgLat) const {
        packets = stats_.total_packets;
        hops = stats_.total_hops;
        avgLat = (stats_.total_packets > 0) ? stats_.total_latency / stats_.total_packets : 0;
    }

    const GarnetStats& getGarnetStats() const { return stats_; }

    /* 1.11.90: drop the pre-ROI traffic. The network is driven by the memory
     * hierarchy from process start (the plugin records by default), so the
     * serial array-init traffic reached these counters and was then priced
     * over the ROI wall clock. Called from the plugin at roi_begin, once per
     * network. Returns the number of flits dropped so the caller can say so.
     *
     * 1.11.92 (F2/F4/F11): the returned count is TRUE flits now (it was
     * packets under the name "flits"), the per-router counters and the
     * per-level arrays are rebased with the rest, and the caller drains the
     * pending pre-ROI records FIRST (drainPendingRecords) so they are
     * replayed and dropped here instead of leaking into the first ROI
     * drain. */
    uint64_t markRoiBegin() {
#ifdef HAVE_GARNET
        futex_lock(&garnetLock_);
        harvestRouterCountersLocked_();
        std::fill(rtrXbarTot_.begin(), rtrXbarTot_.end(), 0);
        std::fill(rtrBufRdTot_.begin(), rtrBufRdTot_.end(), 0);
        std::fill(rtrBufWrTot_.begin(), rtrBufWrTot_.end(), 0);
        futex_unlock(&garnetLock_);
#endif
        const uint64_t dropped = stats_.total_flits;
        roiDroppedPackets_ = stats_.total_packets;
        stats_.total_packets = 0; stats_.total_flits = 0; stats_.total_hops = 0;
        stats_.total_router_traversals = 0;
        stats_.data_packets = 0; stats_.control_packets = 0;
        stats_.buffer_reads = 0; stats_.buffer_writes = 0;
        stats_.crossbar_traversals = 0; stats_.arbiter_events = 0;
        stats_.link_traversals = 0; stats_.total_latency = 0;
        for (int l = 0; l < 7; l++) {
            stats_.level_flit_traversals[l] = 0;
            stats_.level_passthrough_flit_traversals[l] = 0;
            stats_.level_packet_traversals[l] = 0;
            stats_.level_buffer_reads[l] = 0;
            stats_.level_buffer_writes[l] = 0;
        }
        stats_.measured_crossbar_flits = 0;
        stats_.measured_buffer_reads = 0;
        stats_.measured_buffer_writes = 0;
        roi_rebased_ = true;
        return dropped;
    }
    bool roiRebased() const { return roi_rebased_; }
    uint64_t roiDroppedPackets() const { return roiDroppedPackets_; }

    void setTotalCycles(uint64_t cycles) { stats_.total_cycles = cycles; }

    /* 1.11.92 (F11): replay the records still waiting for a drain.
     *
     * Two edges of the ROI window lost or mis-filed traffic. At roi_begin the
     * counters were zeroed but phaseBatch_ was not, so the pre-ROI records of
     * the current phase were replayed by the first ROI drain and counted as
     * ROI traffic. At the end, the records of the phase after the last drain
     * were never replayed at all, so the final phase was missing from the
     * report. The plugin calls this at roi_begin (before markRoiBegin, OMP /
     * single-process only: under thread-MPI nothing is recorded before the
     * ROI baseline, so phaseBatch_ then holds ROI records) and once more
     * before the final stats write (both modes; thread-MPI folds every
     * pending bucket through the same cut logic, with an unbounded cut).
     * The shared-memory MPI path keeps its records in the shm rings, not
     * here, and is not touched. Returns the number of records replayed. */
    uint64_t drainPendingRecords(bool threadMpi, uint32_t phaseLength,
                                 uint64_t phaseNum) {
#ifdef HAVE_GARNET
        if (!cycleAccurate_) return 0;
        if (threadMpi) {
            futex_lock(&batchLock_);
            uint64_t n = phaseBatch_.size();
            for (auto& kv : pendingByPhase_) n += kv.second.size();
            futex_unlock(&batchLock_);
            if (n) foldByCut(~0ull, phaseLength);
            return n;
        }
        futex_lock(&batchLock_);
        std::vector<BatchAccess> batch = std::move(phaseBatch_);
        phaseBatch_.clear();
        futex_unlock(&batchLock_);
        uint64_t n = batch.size();
        if (n) runBatchDrain_(std::move(batch), phaseNum);
        return n;
#else
        (void)threadMpi; (void)phaseLength; (void)phaseNum;
        return 0;
#endif
    }

    /* 1.11.92 (F1): the ANALYTICAL NoC's traversal count. The analytical
     * path charges latency from the hierarchy tier walk and never touched
     * these counters, so every analytical run exported zero traffic and the
     * power model printed "accesses 0 ... 0W dynamic" at every level as if
     * that had been measured. perLevel[l] = routers of tier l the walk
     * visits (up to the LCA and back down: 2 per tier below the LCA, 1 at
     * it -- the same links + 1 as the detailed path). Flits per packet are
     * the message at this network's flit width, as on the detailed path.
     * Lock-free: PE-MI threads call it concurrently. */
    void recordTierWalk(const uint32_t perLevel[7], uint32_t links,
                        bool ctrl = false) {
        const uint32_t fb = flitSizeBits_ > 0 ? flitSizeBits_ : 128;
        const uint32_t bits = ctrl ? stats_.control_msg_bits : stats_.data_msg_bits;
        const uint64_t flits = std::max(1u, (bits + fb - 1) / fb);
        uint64_t routers = 0;
        for (int l = 0; l < 7; l++) {
            if (!perLevel[l]) continue;
            __atomic_fetch_add(&stats_.level_packet_traversals[l],
                               (uint64_t)perLevel[l], __ATOMIC_RELAXED);
            __atomic_fetch_add(&stats_.level_flit_traversals[l],
                               (uint64_t)perLevel[l] * flits, __ATOMIC_RELAXED);
            routers += perLevel[l];
        }
        __atomic_fetch_add(&stats_.total_packets, (uint64_t)1, __ATOMIC_RELAXED);
        __atomic_fetch_add(ctrl ? &stats_.control_packets : &stats_.data_packets,
                           (uint64_t)1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&stats_.total_hops, (uint64_t)links, __ATOMIC_RELAXED);
        __atomic_fetch_add(&stats_.total_flits, flits, __ATOMIC_RELAXED);
        __atomic_fetch_add(&stats_.total_router_traversals, routers * flits,
                           __ATOMIC_RELAXED);
        __atomic_fetch_add(&stats_.link_traversals, (uint64_t)links * flits,
                           __ATOMIC_RELAXED);
        tierWalkUsed_ = true;
    }

    /* 1.11.92 (F4): fold the routers' measured counters into the per-level
     * arrays (detailed) and name the source. Called before the stats are
     * written; takes garnetLock_. */
    void finalizeStats() {
#ifdef HAVE_GARNET
        /* A detailed tree whose Garnet was never built (every access went to
         * the PE's own unit, which does not enter the network) measured zero
         * at every router: that is a measurement, and says so. */
        if (cycleAccurate_ && !routerLevel_.empty() && !(garnetInitialized_ && garnetNet_)) {
            for (size_t r = 0; r < routerLevel_.size(); r++) {
                int lv = routerLevel_[r];
                if (lv < 0 || lv > 6) continue;
                stats_.level_routers[lv]++;
                if (r < routerBranch_.size() && routerBranch_[r])
                    stats_.level_branch_routers[lv]++;
            }
            stats_.level_source = "measured_crossbar";
            return;
        }
        if (cycleAccurate_ && garnetInitialized_ && garnetNet_) {
            futex_lock(&garnetLock_);
            harvestRouterCountersLocked_();
            stats_.measured_crossbar_flits = 0;
            stats_.measured_buffer_reads = 0;
            stats_.measured_buffer_writes = 0;
            for (int l = 0; l < 7; l++) {
                stats_.level_flit_traversals[l] = 0;
                stats_.level_passthrough_flit_traversals[l] = 0;
                stats_.level_buffer_reads[l] = 0;
                stats_.level_buffer_writes[l] = 0;
                stats_.level_routers[l] = 0;
                stats_.level_branch_routers[l] = 0;
            }
            for (size_t r = 0; r < rtrXbarTot_.size(); r++) {
                stats_.measured_crossbar_flits += rtrXbarTot_[r];
                stats_.measured_buffer_reads   += rtrBufRdTot_[r];
                stats_.measured_buffer_writes  += rtrBufWrTot_[r];
                int lv = (r < routerLevel_.size()) ? routerLevel_[r] : -1;
                if (lv < 0 || lv > 6) continue;
                bool br = (r < routerBranch_.size()) && routerBranch_[r];
                stats_.level_flit_traversals[lv] += rtrXbarTot_[r];
                if (!br) stats_.level_passthrough_flit_traversals[lv] += rtrXbarTot_[r];
                stats_.level_buffer_reads[lv]  += rtrBufRdTot_[r];
                stats_.level_buffer_writes[lv] += rtrBufWrTot_[r];
                stats_.level_routers[lv]++;
                if (br) stats_.level_branch_routers[lv]++;
            }
            stats_.level_source = routerLevel_.empty() ? "" : "measured_crossbar";
            futex_unlock(&garnetLock_);
            return;
        }
#endif
        stats_.level_source = tierWalkUsed_ ? "tier_walk" : "";
    }

    void printStats() const {
        static const char* kLvl[7] = { "subarray", "bank", "bankgroup", "chip",
                                       "rank", "channel", "system" };
        info("[GarnetNetwork] Statistics (%s, %s)%s:",
             stats_.topology_name.c_str(), stats_.routing_name.c_str(),
             roi_rebased_ ? " [ROI only: counters rebased at roi_begin, 1.11.90]"
                          : " [whole run: no roi_begin was seen]");
        info("  Total packets: %lu (data %lu, control %lu)", stats_.total_packets,
             stats_.data_packets, stats_.control_packets);
        info("  Total flits: %lu (ceil(message bits / source port width) per packet; "
             "data %u b, control %u b)", stats_.total_flits,
             stats_.data_msg_bits, stats_.control_msg_bits);
        info("  Total hops: %lu (router-to-router links per packet; the latency input)",
             stats_.total_hops);
        info("  Router traversals: %lu (flits x routers crossed, routers = links + 1)",
             stats_.total_router_traversals);
        info("  Link traversals: %lu (flits x router-to-router links)",
             stats_.link_traversals);
        if (!stats_.level_source.empty() && stats_.level_source == "measured_crossbar") {
            info("  Measured by the Garnet routers: crossbar %lu flits, buffer reads %lu, "
                 "buffer writes %lu", stats_.measured_crossbar_flits,
                 stats_.measured_buffer_reads, stats_.measured_buffer_writes);
        }
        for (int l = 0; l < 7; l++) {
            if (!stats_.level_flit_traversals[l] && !stats_.level_packet_traversals[l] &&
                !stats_.level_routers[l])
                continue;
            if (stats_.level_source == "measured_crossbar") {
                info("  Level %d (%s): %lu flit traversals MEASURED at %u router(s) "
                     "(%u branch; %lu of the flits through pass-through routers); "
                     "%lu packet crossings by path walk", l, kLvl[l],
                     stats_.level_flit_traversals[l], stats_.level_routers[l],
                     stats_.level_branch_routers[l],
                     stats_.level_passthrough_flit_traversals[l],
                     stats_.level_packet_traversals[l]);
            } else {
                info("  Level %d (%s): %lu flit traversals, %lu packet crossings "
                     "(analytical tier walk)", l, kLvl[l],
                     stats_.level_flit_traversals[l], stats_.level_packet_traversals[l]);
            }
        }
        if (stats_.total_packets > 0) {
            info("  Avg latency: %lu cycles", stats_.total_latency / stats_.total_packets);
        }
    }

    void writeStatsFile(const char* filename) {
        finalizeStats();
        FILE* f = fopen(filename, "w");
        if (!f) {
            warn("[GarnetNetwork] Failed to write stats to %s", filename);
            return;
        }
        fprintf(f, "# GarnetNetwork Statistics for McPAT\n");
        fprintf(f, "garnet.topology = %s\n", stats_.topology_name.c_str());
        fprintf(f, "garnet.routing = %s\n", stats_.routing_name.c_str());
        fprintf(f, "garnet.total_packets = %lu\n", stats_.total_packets);
        fprintf(f, "garnet.data_packets = %lu\n", stats_.data_packets);
        fprintf(f, "garnet.control_packets = %lu\n", stats_.control_packets);
        fprintf(f, "garnet.total_flits = %lu\n", stats_.total_flits);
        fprintf(f, "garnet.total_hops = %lu\n", stats_.total_hops);
        fprintf(f, "garnet.total_router_traversals = %lu\n", stats_.total_router_traversals);
        fprintf(f, "garnet.buffer_reads = %lu\n", stats_.buffer_reads);
        fprintf(f, "garnet.buffer_writes = %lu\n", stats_.buffer_writes);
        fprintf(f, "garnet.crossbar_traversals = %lu\n", stats_.crossbar_traversals);
        fprintf(f, "garnet.arbiter_events = %lu\n", stats_.arbiter_events);
        fprintf(f, "garnet.link_traversals = %lu\n", stats_.link_traversals);
        fprintf(f, "garnet.total_cycles = %lu\n", stats_.total_cycles);
        fprintf(f, "garnet.total_latency = %lu\n", stats_.total_latency);
        fprintf(f, "garnet.num_routers = %u\n", stats_.num_routers);
        fprintf(f, "garnet.num_rows = %u\n", stats_.num_rows);
        fprintf(f, "garnet.num_cols = %u\n", stats_.num_cols);
        fprintf(f, "garnet.flit_size_bits = %u\n", stats_.flit_size_bits);
        fprintf(f, "garnet.vcs_per_vnet = %u\n", stats_.vcs_per_vnet);
        fprintf(f, "garnet.buffers_per_vc = %u\n", buffersPerVc_);
        {
            // What initGarnetNetwork builds: a VC must hold one data packet.
            uint32_t fb = flitSizeBits_ > 0 ? flitSizeBits_ : 128;
            uint32_t df = (stats_.data_msg_bits + fb - 1) / fb;
            fprintf(f, "garnet.effective_buffers_per_vc = %u\n",
                    std::max(buffersPerVc_, std::max(df, 1u)));
        }
        fprintf(f, "garnet.data_msg_bits = %u\n", stats_.data_msg_bits);
        fprintf(f, "garnet.control_msg_bits = %u\n", stats_.control_msg_bits);
        fprintf(f, "garnet.clock_mhz = %.1f\n", stats_.clock_mhz);
        fprintf(f, "garnet.level_source = %s\n",
                stats_.level_source.empty() ? "none" : stats_.level_source.c_str());
        fprintf(f, "garnet.measured_crossbar_flits = %lu\n", stats_.measured_crossbar_flits);
        fprintf(f, "garnet.measured_buffer_reads = %lu\n", stats_.measured_buffer_reads);
        fprintf(f, "garnet.measured_buffer_writes = %lu\n", stats_.measured_buffer_writes);
        if (!stats_.level_source.empty()) {
            for (int l = 0; l < 7; l++) {
                fprintf(f, "garnet.level%d.router_traversals = %lu\n", l,
                        stats_.level_flit_traversals[l]);
                fprintf(f, "garnet.level%d.passthrough_traversals = %lu\n", l,
                        stats_.level_passthrough_flit_traversals[l]);
                fprintf(f, "garnet.level%d.packet_traversals = %lu\n", l,
                        stats_.level_packet_traversals[l]);
                fprintf(f, "garnet.level%d.buffer_reads = %lu\n", l,
                        stats_.level_buffer_reads[l]);
                fprintf(f, "garnet.level%d.buffer_writes = %lu\n", l,
                        stats_.level_buffer_writes[l]);
                fprintf(f, "garnet.level%d.routers = %u\n", l, stats_.level_routers[l]);
                fprintf(f, "garnet.level%d.branch_routers = %u\n", l,
                        stats_.level_branch_routers[l]);
            }
        }
        fclose(f);
    }

    // ── Synthetic traffic injection ─────────────────────────
    // Traffic patterns (from gem5 Ruby Tester):
    //   0 = Uniform Random, 1 = Bit-Complement, 2 = Tornado,
    //   3 = Neighbor, 4 = Transpose, 5 = Bit-Reverse,
    //   6 = Bit-Rotation, 7 = Shuffle

    struct SyntheticResult {
        uint64_t totalPackets = 0;
        uint64_t totalLatency = 0;
        double avgLatency = 0.0;
        double throughput = 0.0;  // packets/cycle/node
        uint64_t minLatency = UINT64_MAX;
        uint64_t maxLatency = 0;
        uint64_t totalCycles = 0;

        // Network config (for power modeling)
        uint32_t numNodes = 0;
        uint32_t numRouters = 0;
        uint32_t numRows = 0;
        uint32_t numCols = 0;
        uint32_t flitSizeBits = 128;
        double clockMhz = 1000.0;
        double injectionRate = 0.0;
    };

#ifdef HAVE_GARNET
    SyntheticResult runSyntheticTraffic(int trafficPattern, double injectionRate,
                                        uint64_t numPackets, int warmupPackets = 0) {
        if (!cycleAccurate_) {
            warn("[SyntheticTraffic] Requires detailed (cycle-accurate) mode");
            return {};
        }

        ensureInitialized();
        resetGarnetState();

        uint32_t N = numNodes_;
        // Lay the N endpoints out on a RECTANGULAR rows x cols grid with
        // rows*cols == N exactly (rows = largest divisor <= sqrt(N)). For the
        // detailed Garnet's N=128 this is 8 x 16 -- every node is ON-GRID, so
        // the gem5 mesh-coordinate patterns (bit-complement/tornado/neighbor)
        // map cleanly with no off-grid nodes. (The old square radix=sqrt(128)=11
        // left 7 nodes off-grid -> the OOB clamp.) gem5's generatePkt() uses a
        // single square radix; this is its faithful rectangular generalization
        // (rows==cols recovers the square case). Only TRANSPOSE is inherently
        // square-only and still relies on the final safe-modulo for N=128.
        uint32_t gridRows = 1, gridCols = N;
        for (uint32_t r = (uint32_t)std::sqrt((double)N); r >= 1; r--) {
            if (N % r == 0) { gridRows = r; gridCols = N / r; break; }
        }

        // Per-node injection timing
        std::vector<double> nextArrival(N, 1.0);

        // Track ALL in-flight packets (multiple per source)
        struct InFlightPkt {
            uint64_t tag;
            uint32_t src;
            uint32_t dst;
            uint64_t injectTick;
        };
        // Map from tag → in-flight info
        std::unordered_map<uint64_t, InFlightPkt> inFlight;
        // Per-destination set of pending tags (for efficient drain)
        std::vector<std::vector<uint64_t>> dstPending(N);
        // Destination-coverage tracking (diagnostic): which of the N nodes get
        // hit as a destination -- proves the pattern maps across the full grid.
        std::vector<char> dstHit(N, 0);

        uint64_t tag = 1;
        uint64_t injected = 0, delivered = 0;
        uint64_t totalLat = 0, minLat = UINT64_MAX, maxLat = 0;
        uint64_t warmupDelivered = 0;

        double avgInterval = 1.0 / injectionRate;
        double uniformRange = avgInterval * 2.0;  // uniform [0, 2*mean] for mean=avgInterval

        gem5::curTickRef() = 0;

        static const char* patternNames[] = {
            "uniform", "bit-complement", "tornado", "neighbor",
            "transpose", "bit-reverse", "bit-rotation", "shuffle",
            "memory-directed"
        };
        const char* patName = (trafficPattern >= 0 && trafficPattern <= 8)
                              ? patternNames[trafficPattern] : "unknown";
        info("[SyntheticTraffic] Pattern=%s, Rate=%.3f, Packets=%lu, Nodes=%u",
             patName, injectionRate, numPackets, N);

        // Bounded drain window: injection span + a capped drain. A SATURATED
        // network (cannot deliver all packets) then finishes quickly and reports
        // a correctly-LOW throughput, instead of spinning to a ~20M-tick maxTick.
        uint64_t injectSpan = numPackets * (uint64_t)avgInterval;
        uint64_t maxTick = injectSpan + 2000000;
        if (maxTick > 10000000) maxTick = 10000000;
        // Stall guard: only fire on a GENUINE wedge (no delivery for a long
        // stretch). A slow single-channel (low-bandwidth) network drains with
        // deliveries spaced far apart -- a tight guard wrongly cut it off at 1
        // packet -- so keep this generous; maxTick still bounds the wall-clock.
        const uint64_t kStallTicks = 2000000;
        uint64_t lastDelivered = 0, lastProgressTick = 0;

        // CLASSIFICATION mode (env PIMID_SYNTH_NOGUARD=1): disable the stall
        // guard and raise the tick ceiling enormously, so a stalled run is given
        // every chance to drain. If `delivered` keeps climbing -> slow-drain (the
        // guard was just too aggressive); if it FREEZES early while curTick marches
        // to the ceiling -> true deadlock. Read lastProgressTick in the exit diag.
        const bool noGuard = getenv("PIMID_SYNTH_NOGUARD") != nullptr;
        // 30M-tick ceiling: decisive but bounded. Earlier data shows slow-drain
        // delivers ~38 pkt / Mtick on DDR3, so a genuine slow-drain finishes all
        // 512 by ~15M ticks; anything still stuck at 30M is a true deadlock.
        if (noGuard) maxTick = injectSpan + 30000000ULL;
        // Periodic progress trace (classification mode): stream delivered/inFlight
        // every kProbeTicks so the drain TRAJECTORY is visible live -- a steadily
        // climbing `delivered` is slow-drain, a frozen one is deadlock. No need to
        // wait for the run to finish (or be killed by the wall-clock timeout).
        const uint64_t kProbeTicks = 1000000;   // 1M
        uint64_t nextProbe = kProbeTicks;

        // Diagnostic (env PIMID_SYNTH_POKE=1): force-wake every NI+router each
        // iteration. If this clears the wedge -> LOST WAKEUP (a Consumer asleep
        // with returnable credit). If the wedge persists -> STRUCTURAL cycle.
        const bool pokeAll = getenv("PIMID_SYNTH_POKE") != nullptr;
        const bool noSameBank = getenv("PIMID_SYNTH_NOSAMEBANK") != nullptr;
        uint64_t pokeIter = 0;        // throttle: poke every N iterations, not
        const uint64_t kPokeEvery = 2000;  // every cycle (poking all links each
                                           // cycle is an event storm). A static
                                           // wedge only needs occasional pokes.

        while (delivered < numPackets && gem5::curTickRef() < maxTick) {
            if (pokeAll && garnetNet_ && (pokeIter++ % kPokeEvery == 0))
                garnetNet_->pokeAllConsumers();
            if (noGuard && gem5::curTickRef() >= nextProbe) {
                fprintf(stderr, "[SynthProbe] pat=%d rate=%.4f curTick=%lu "
                        "delivered=%lu inFlight=%zu lastDelivTick=%lu\n",
                        trafficPattern, injectionRate,
                        (unsigned long)gem5::curTickRef(),
                        (unsigned long)delivered, inFlight.size(),
                        (unsigned long)lastProgressTick);
                nextProbe = gem5::curTickRef() + kProbeTicks;
            }
            if (delivered > lastDelivered) {
                lastDelivered = delivered; lastProgressTick = gem5::curTickRef();
            } else if (!noGuard && gem5::curTickRef() > lastProgressTick &&
                       gem5::curTickRef() - lastProgressTick > kStallTicks) {
                // NB: curTick is NOT strictly monotonic here -- when the event
                // queue is empty we bump it by hand, and processOneEvent() can
                // then snap it to a clock-aligned event a tick or two EARLIER.
                // The `curTick > lastProgressTick` guard is mandatory: without
                // it the unsigned subtraction underflows to ~2^64 on a backward
                // blip and spuriously trips the stall guard, killing the drain
                // with hundreds of packets still in flight (the DDR3 dropout).
                break;  // stalled (saturated) -- report partial
            }
            // ── Inject phase: try each source node ──
            for (uint32_t src = 0; src < N; src++) {
                if (nextArrival[src] > (double)gem5::curTickRef()) continue;
                if (injected >= numPackets + (uint64_t)warmupPackets) continue;

                // Schedule next arrival (Poisson-like uniform)
                nextArrival[src] += ((double)rand() / RAND_MAX) * uniformRange;

                // Pick destination by traffic pattern. The pattern MATH is the
                // canonical gem5 GarnetSyntheticTraffic::generatePkt() (gem5
                // src/cpu/testers/garnet_synthetic_traffic/), reused VERBATIM --
                // not re-derived -- so our patterns match gem5's definitions
                // exactly. 'radix' is gem5's radix = (int)sqrt(num_destinations).
                // PIMID keeps its own pattern IDs (0..8); each maps to the gem5
                // formula of the same name. Pattern 8 (memory-directed) is a
                // PIMID-specific hotspot, NOT a gem5 pattern (see note below).
                const int cols     = (int)gridCols;  // x-dim (16 for N=128)
                const int rows     = (int)gridRows;  // y-dim ( 8 for N=128)
                const int source   = (int)src;
                const int src_x    = source % cols;
                const int src_y    = source / cols;
                int destination    = source;
                if (trafficPattern == 0) {            // UNIFORM_RANDOM_
                    destination = (int)(rand() % N);
                } else if (trafficPattern == 1) {     // BIT_COMPLEMENT_
                    int dest_x = cols - src_x - 1;
                    int dest_y = rows - src_y - 1;
                    destination = dest_y * cols + dest_x;
                } else if (trafficPattern == 2) {     // TORNADO_
                    int dest_x =
                        (src_x + (int)std::ceil((double)cols / 2) - 1) % cols;
                    int dest_y = src_y;
                    destination = dest_y * cols + dest_x;
                } else if (trafficPattern == 3) {     // NEIGHBOR_
                    int dest_x = (src_x + 1) % cols;
                    int dest_y = src_y;
                    destination = dest_y * cols + dest_x;
                } else if (trafficPattern == 4) {     // TRANSPOSE_
                    // Coordinate transpose. On a rectangular cols x rows grid the
                    // transposed (rows x cols) grid re-flattened onto the same N
                    // nodes is a TRUE bijection: dest = src_x*rows + src_y
                    // (src_x in [0,cols), src_y in [0,rows) -> dest in [0,N),
                    // distinct). For a square grid rows==cols this is exactly
                    // gem5's dest_y*radix+dest_x. Full 128/128 coverage, no clamp.
                    destination = src_x * rows + src_y;
                } else if (trafficPattern == 5) {     // BIT_REVERSE_
                    unsigned int straight = source;
                    unsigned int reverse  = source & 1;          // LSB
                    int num_bits = (int)std::log2((double)N);
                    for (int i = 1; i < num_bits; i++) {
                        reverse  <<= 1;
                        straight >>= 1;
                        reverse  |= (straight & 1);              // LSB
                    }
                    destination = (int)reverse;
                } else if (trafficPattern == 6) {     // BIT_ROTATION_
                    if (source % 2 == 0)
                        destination = source / 2;
                    else
                        destination = (source / 2) + ((int)N / 2);
                } else if (trafficPattern == 7) {     // SHUFFLE_
                    if (source < (int)N / 2)
                        destination = source * 2;
                    else
                        destination = source * 2 - (int)N + 1;
                } else if (trafficPattern == 8) {     // memory-directed (PIMID)
                    // NOT a gem5 pattern. Curve fix 2: the PIM workload is
                    // many-PE -> few-memory-org (a hotspot), not uniform. Direct
                    // all sources at a small mem-node set (nodes 0..kMemNodes-1)
                    // so the probed curve reflects the real contention hotspot.
                    int kMemNodes = (N >= 8) ? 2 : 1;
                    destination = source % kMemNodes;
                } else {                              // unknown -> uniform
                    destination = (int)(rand() % N);
                }

                // Safe-modulo into [0,N) + off the diagonal. With the rectangular
                // rows x cols == N layout above, the coordinate patterns are all
                // on-grid; only TRANSPOSE on a non-square grid (here 8 x 16) can
                // still produce an out-of-range destination. This guard maps that
                // back into range so it can never index dstPending[dst] / the
                // delivery buffers OUT OF BOUNDS (and keeps src != dst).
                uint32_t dst = (uint32_t)(((destination % (int)N) + (int)N)
                                          % (int)N);
                if (dst == src) dst = (dst + 1) % N;

                // Diagnostic (env PIMID_SYNTH_NOSAMEBANK=1): redirect a dst on the
                // SAME bank as src to a different bank. Endpoint k maps to bank
                // router 2+(k % nBanks), so same-bank <=> (src % nBanks) ==
                // (dst % nBanks). Tests whether INTRA-BANK Local->Local traffic
                // (which gets up/down class 0 on the bank's Local outport, mixing
                // with class-1 deliveries) is the structural deadlock cause: if
                // excluding it cures the wedge, that hypothesis holds.
                if (noSameBank) {
                    uint32_t nBanks = (N >= 8) ? (N / 8u) : 1u;  // 8 NIs/bank
                    while ((dst % nBanks) == (src % nBanks)) {
                        dst = (dst + 1) % N;
                        if (dst == src) dst = (dst + 1) % N;
                    }
                }

                // Inject
                auto msg = std::make_shared<gem5::ruby::SimpleMessage>(
                    src, dst, gem5::ruby::MessageSizeType::Data,
                    gem5::curTickRef());
                msg->setTag(tag);
                toNetBufs_[src][0]->enqueue(msg, gem5::curTickRef(), uint64_t(1));

                InFlightPkt pkt{tag, src, dst, gem5::curTickRef()};
                inFlight[tag] = pkt;
                dstPending[dst].push_back(tag);
                dstHit[dst] = 1;
                tag++;
                injected++;
            }

            // ── Drain phase: check ALL destinations for arrivals ──
            for (uint32_t dst = 0; dst < N; dst++) {
                while (fromNetBufs_[dst][0]->isReady(gem5::curTickRef())) {
                    auto peekMsg = std::dynamic_pointer_cast<
                        gem5::ruby::SimpleMessage>(
                        fromNetBufs_[dst][0]->peekMsgPtr());
                    uint64_t pktTag = peekMsg ? peekMsg->getTag() : 0;

                    fromNetBufs_[dst][0]->dequeue(gem5::curTickRef());

                    auto it = inFlight.find(pktTag);
                    if (it != inFlight.end()) {
                        uint64_t lat = gem5::curTickRef() - it->second.injectTick;
                        inFlight.erase(it);

                        if (warmupDelivered < (uint64_t)warmupPackets) {
                            warmupDelivered++;
                        } else {
                            delivered++;
                            totalLat += lat;
                            if (lat < minLat) minLat = lat;
                            if (lat > maxLat) maxLat = lat;
                        }
                    }
                }
            }

            // ── Advance simulation ──
            if (!gem5::EventQueue::instance().processOneEvent()) {
                gem5::curTickRef()++;
            }
        }

        // Optional exit diagnostic (env PIMID_SYNTH_DIAG=1): why the drain loop
        // stopped, plus in-flight/injected counts -- for chasing dropouts.
        if (getenv("PIMID_SYNTH_DIAG")) {
            const char* why =
                (delivered >= numPackets) ? "ALL_DELIVERED" :
                (gem5::curTickRef() >= maxTick) ? "MAXTICK" : "STALL_GUARD";
            uint32_t distinctDst = 0;
            for (uint32_t i = 0; i < N; i++) distinctDst += dstHit[i] ? 1 : 0;
            fprintf(stderr,
                "[SynthDiag] pat=%d rate=%.4f grid=%ux%u exit=%s curTick=%lu "
                "maxTick=%lu lastDelivTick=%lu injected=%lu delivered=%lu "
                "inFlight=%zu distinctDst=%u/%u\n",
                trafficPattern, injectionRate, gridRows, gridCols, why,
                (unsigned long)gem5::curTickRef(), (unsigned long)maxTick,
                (unsigned long)lastProgressTick,
                (unsigned long)injected, (unsigned long)delivered,
                inFlight.size(), distinctDst, N);
        }

        // Deadlock-state dump (env PIMID_SYNTH_DUMP=1): if the run wedged
        // (didn't deliver everything), dump every blocked input VC and what it
        // waits for, exposing the cyclic dependency. Requires the gem5 net.
        if (getenv("PIMID_SYNTH_DUMP") && delivered < numPackets && garnetNet_) {
            garnetNet_->dumpDeadlockState(gem5::curTickRef(), "wedge");
        }

        SyntheticResult result;
        result.totalPackets = delivered;
        result.totalLatency = totalLat;
        result.avgLatency = delivered > 0 ? (double)totalLat / delivered : 0.0;
        result.throughput = gem5::curTickRef() > 0 ?
            (double)delivered / ((double)gem5::curTickRef() * N) : 0.0;
        result.minLatency = delivered > 0 ? minLat : 0;
        result.maxLatency = maxLat;
        result.totalCycles = gem5::curTickRef();
        result.numNodes = N;
        result.numRouters = N;
        result.numRows = gridRows;
        result.numCols = gridCols;
        result.flitSizeBits = flitSizeBits_;
        result.clockMhz = clockMhz_;
        result.injectionRate = injectionRate;

        info("[SyntheticTraffic] Results:");
        info("  Delivered:  %lu / %lu packets", delivered, numPackets);
        info("  Cycles:     %lu", gem5::curTickRef());
        info("  Avg lat:    %.1f cycles", result.avgLatency);
        info("  Min lat:    %lu cycles", result.minLatency);
        info("  Max lat:    %lu cycles", result.maxLatency);
        info("  Throughput: %.6f packets/cycle/node", result.throughput);

        return result;
    }
#endif

private:
    /* 1.11.92 (F2): flits for one message from endpoint `srcEp`. Garnet's
     * NetworkInterface cuts a message into ceil(message bits / port width)
     * flits, at the width of the source endpoint's link (NetworkInterface::
     * flitisizeMessage, oPort->bitWidth()); there is no SerDes on these
     * links, so the count is fixed at injection. */
    uint32_t portWidthOf_(uint32_t ep) const {
        if (ep < epWidth_.size() && epWidth_[ep] > 0) return epWidth_[ep];
        return flitSizeBits_ > 0 ? flitSizeBits_ : 128;
    }
    uint32_t flitsFor_(uint32_t srcEp, bool ctrl) const {
        const uint32_t w = portWidthOf_(srcEp);
        const uint32_t b = ctrl ? stats_.control_msg_bits : stats_.data_msg_bits;
        return std::max(1u, (b + w - 1) / w);
    }

    /* 1.11.92 (F2/F4): the routers a packet crosses on the CUSTOM tree, in
     * order: up from the source's router to the lowest common ancestor and
     * down to the destination's (the only shortest path on a tree, which is
     * what TABLE routing takes). false when the topology is not a rooted
     * tree or an endpoint is unattached. */
    bool pathRouters_(uint32_t srcEp, uint32_t dstEp,
                      std::vector<uint32_t>& out) const {
        out.clear();
        if (!customIsTree_) return false;
        if (srcEp >= epToRouter_.size() || dstEp >= epToRouter_.size()) return false;
        uint32_t a = epToRouter_[srcEp], b = epToRouter_[dstEp];
        if (a == UINT32_MAX || b == UINT32_MAX) return false;
        if (a >= routerDepth_.size() || b >= routerDepth_.size()) return false;
        std::vector<uint32_t> down;
        while (routerDepth_[a] > routerDepth_[b]) {
            out.push_back(a);
            if (routerParent_[a] < 0) return false;
            a = (uint32_t)routerParent_[a];
        }
        while (routerDepth_[b] > routerDepth_[a]) {
            down.push_back(b);
            if (routerParent_[b] < 0) return false;
            b = (uint32_t)routerParent_[b];
        }
        while (a != b) {
            out.push_back(a); down.push_back(b);
            if (routerParent_[a] < 0 || routerParent_[b] < 0) return false;
            a = (uint32_t)routerParent_[a]; b = (uint32_t)routerParent_[b];
        }
        out.push_back(a);
        out.insert(out.end(), down.rbegin(), down.rend());
        return true;
    }

    /* 1.11.92 (F2): ONE place that counts a packet. Replaces five copies of
     * "total_flits += 1; buffer_reads += hops; ...", which counted a packet
     * as one flit and a router traversal as a link hop. */
    void countPacket_(uint32_t src, uint32_t dst, uint32_t hops, bool ctrl) {
        const uint64_t flits = flitsFor_(src, ctrl);
        uint64_t routers;
        thread_local std::vector<uint32_t> path;
        if (topology_ == NoCTopology::CUSTOM && pathRouters_(src, dst, path)) {
            routers = path.size();
            for (uint32_t r : path) {
                int lv = (r < routerLevel_.size()) ? routerLevel_[r] : -1;
                if (lv >= 0 && lv <= 6) stats_.level_packet_traversals[lv]++;
            }
        } else if (topology_ == NoCTopology::BUS || topology_ == NoCTopology::CROSSBAR) {
            routers = 1;      // one arbiter / one crossbar, hop count is 1
        } else {
            routers = (uint64_t)hops + 1;
        }
        stats_.total_hops += hops;
        stats_.total_flits += flits;
        stats_.total_router_traversals += routers * flits;
        if (ctrl) stats_.control_packets++; else stats_.data_packets++;
        stats_.buffer_reads += routers * flits;
        stats_.buffer_writes += routers * flits;
        stats_.crossbar_traversals += routers * flits;
        stats_.arbiter_events += routers;          // one allocation per packet per router
        stats_.link_traversals += (uint64_t)hops * flits;
    }

#ifdef HAVE_GARNET
    /* 1.11.92 (F4): fold each router's cumulative counters into its ROI
     * total. Caller holds garnetLock_. A counter that went DOWN was reset
     * underneath us: count from zero. */
    static void harvestOne_(double now, double& seen, uint64_t& tot) {
        double d = now - seen;
        if (d < 0.0) d = now;
        tot += (uint64_t)(d + 0.5);
        seen = now;
    }
    void harvestRouterCountersLocked_() {
        if (!garnetInitialized_ || !garnetNet_) return;
        const int n = garnetNet_->getNumRouters();
        if ((int)rtrXbarSeen_.size() != n) {
            rtrXbarSeen_.assign(n, 0.0); rtrBufRdSeen_.assign(n, 0.0);
            rtrBufWrSeen_.assign(n, 0.0);
            rtrXbarTot_.assign(n, 0); rtrBufRdTot_.assign(n, 0);
            rtrBufWrTot_.assign(n, 0);
        }
        for (int i = 0; i < n; i++) {
            gem5::ruby::garnet::Router* r = garnetNet_->getRouterAt(i);
            if (!r) continue;
            harvestOne_(r->getCrossbarActivityCount(), rtrXbarSeen_[i], rtrXbarTot_[i]);
            harvestOne_(r->getBufferReadCount(),  rtrBufRdSeen_[i], rtrBufRdTot_[i]);
            harvestOne_(r->getBufferWriteCount(), rtrBufWrSeen_[i], rtrBufWrTot_[i]);
        }
    }
    void resnapRouterCountersLocked_() {
        if (!garnetInitialized_ || !garnetNet_) return;
        const int n = garnetNet_->getNumRouters();
        if ((int)rtrXbarSeen_.size() != n) return;
        for (int i = 0; i < n; i++) {
            gem5::ruby::garnet::Router* r = garnetNet_->getRouterAt(i);
            if (!r) continue;
            rtrXbarSeen_[i]  = r->getCrossbarActivityCount();
            rtrBufRdSeen_[i] = r->getBufferReadCount();
            rtrBufWrSeen_[i] = r->getBufferWriteCount();
        }
    }
#endif

    // ── Topology-specific hop count functions ────────────────

    uint32_t getHopCount(uint32_t src, uint32_t dst) const {
        if (src == dst) return 0;
        switch (topology_) {
            case NoCTopology::MESH_2D:  return getMeshHops(src, dst);
            case NoCTopology::TORUS_2D: return getTorusHops(src, dst);
            case NoCTopology::RING:     return getRingHops(src, dst);
            case NoCTopology::CROSSBAR: return 1;
            case NoCTopology::FAT_TREE: return getFatTreeHops(src, dst);
            case NoCTopology::BUS:      return 1;
            case NoCTopology::H_TREE:   return getHTreeHops(src, dst);
            case NoCTopology::CUSTOM:   return getCustomHops(src, dst);
        }
        return getMeshHops(src, dst);  // fallback
    }

    // Manhattan distance in mesh
    uint32_t getMeshHops(uint32_t src, uint32_t dst) const {
        if (numCols_ == 0) return 1;
        uint32_t srcRow = src / numCols_, srcCol = src % numCols_;
        uint32_t dstRow = dst / numCols_, dstCol = dst % numCols_;
        uint32_t rowDist = (srcRow > dstRow) ? (srcRow - dstRow) : (dstRow - srcRow);
        uint32_t colDist = (srcCol > dstCol) ? (srcCol - dstCol) : (dstCol - srcCol);
        return rowDist + colDist;
    }

    // Wrapped Manhattan for torus: min(d, N-d) per dimension
    uint32_t getTorusHops(uint32_t src, uint32_t dst) const {
        if (numCols_ == 0 || numRows_ == 0) return 1;
        uint32_t srcRow = src / numCols_, srcCol = src % numCols_;
        uint32_t dstRow = dst / numCols_, dstCol = dst % numCols_;

        uint32_t dx = (srcCol > dstCol) ? (srcCol - dstCol) : (dstCol - srcCol);
        uint32_t dy = (srcRow > dstRow) ? (srcRow - dstRow) : (dstRow - srcRow);

        dx = std::min(dx, numCols_ - dx);
        dy = std::min(dy, numRows_ - dy);
        return dx + dy;
    }

    // Ring: bidirectional = min(|s-d|, N-|s-d|); unidirectional = (dst-src+N)%N
    uint32_t getRingHops(uint32_t src, uint32_t dst) const {
        uint32_t N = numNodes_;
        if (N == 0) return 1;
        if (ringUnidirectional_) {
            return (dst - src + N) % N;
        }
        uint32_t d = (src > dst) ? (src - dst) : (dst - src);
        return std::min(d, N - d);
    }

    // Fat tree: 2 * level_of_LCA(src, dst)
    uint32_t getFatTreeHops(uint32_t src, uint32_t dst) const {
        uint32_t arity = 2;
        uint32_t s = src, d = dst;
        uint32_t level = 0;
        while (s != d) {
            s /= arity;
            d /= arity;
            level++;
        }
        return 2 * level;
    }

    // H-tree: LCA-based hop count in a binary tree.
    // Endpoints are distributed round-robin across leaf routers.
    // Hops = depth(srcLeaf) + depth(dstLeaf) - 2*depth(LCA).
    uint32_t getHTreeHops(uint32_t src, uint32_t dst) const {
        if (src == dst) return 0;
        // Compute tree depth (same formula as TopologyBuilders::buildHTree)
        uint32_t levels = 1, cap = 2;
        while (cap < numNodes_) { cap *= 2; levels++; }
        uint32_t num_leaves = (1u << (levels - 1));
        // Map endpoints to leaf indices (round-robin)
        uint32_t srcLeaf = src % num_leaves;
        uint32_t dstLeaf = dst % num_leaves;
        if (srcLeaf == dstLeaf) return 0;  // same leaf router
        // Walk up from both leaves to their LCA in the binary tree
        uint32_t a = srcLeaf + num_leaves;  // 1-indexed leaf position
        uint32_t b = dstLeaf + num_leaves;
        uint32_t hops = 0;
        while (a != b) {
            if (a > b) { a >>= 1; hops++; }
            else       { b >>= 1; hops++; }
        }
        return hops;
    }

    // Custom: BFS shortest path on adjacency list
    uint32_t getCustomHops(uint32_t src, uint32_t dst) const {
        /* 1.9.36: src and dst arrive as ENDPOINT ids, but customAdj_ is indexed
         * by ROUTER id and holds only router-to-router links. Translating was
         * impossible until now because the topology parser discarded the "ext"
         * lines that carry the attachment; it keeps them as epToRouter_.
         *
         * The absence of that translation was not caught by the bounds guard
         * below, and could not have been: in the sparse tree the endpoints are
         * FEWER than the routers (for the swept 16-PE HBM3 case, 49 against
         * 51), so every endpoint id satisfies "< customAdj_.size()" and the
         * search ran happily between two unrelated routers, returning a
         * plausible wrong hop count rather than falling back. Hop counts feed
         * the interconnect power model, so the error was silent in exactly the
         * way the rest of this release train has been. */
        uint32_t s = (src < epToRouter_.size() && epToRouter_[src] != UINT32_MAX)
                   ? epToRouter_[src] : src;
        uint32_t d = (dst < epToRouter_.size() && epToRouter_[dst] != UINT32_MAX)
                   ? epToRouter_[dst] : dst;
        src = s; dst = d;
        if (customAdj_.empty() || src >= customAdj_.size() || dst >= customAdj_.size()) {
            return 1;  // fallback
        }

        // BFS
        std::vector<int> dist(customAdj_.size(), -1);
        std::queue<uint32_t> q;
        dist[src] = 0;
        q.push(src);
        while (!q.empty()) {
            uint32_t u = q.front(); q.pop();
            if (u == dst) return static_cast<uint32_t>(dist[u]);
            for (uint32_t v : customAdj_[u]) {
                if (dist[v] == -1) {
                    dist[v] = dist[u] + 1;
                    q.push(v);
                }
            }
        }
        return customRouterCount_;  // unreachable → worst case
    }

    // ── Parse custom topology file ───────────────────────────

    void parseCustomTopologyFile() {
        std::ifstream infile(customTopoFile_);
        if (!infile.is_open()) {
            warn("[GarnetNetwork] Cannot open topology file: %s", customTopoFile_.c_str());
            return;
        }

        std::string line;
        while (std::getline(infile, line)) {
            auto hash = line.find('#');
            if (hash != std::string::npos) line = line.substr(0, hash);

            std::istringstream iss(line);
            std::string token;
            if (!(iss >> token)) continue;

            if (token == "routers") {
                iss >> customRouterCount_;
                customAdj_.resize(customRouterCount_);
                stats_.num_routers = customRouterCount_;
            } else if (token == "endpoints") {
                uint32_t ep;
                iss >> ep;
                numNodes_ = ep;
            } else if (token == "int") {
                uint32_t s, d;
                iss >> s >> d;
                if (s < customAdj_.size() && d < customAdj_.size()) {
                    customAdj_[s].push_back(d);
                }
            } else if (token == "ext") {
                /* 1.9.36: endpoint-to-router attachment, "ext <endpoint>
                 * <router> <lat> <width>". These lines were READ AND DISCARDED,
                 * so no endpoint->router mapping existed and getCustomHops()
                 * below indexed endpoint ids straight into the ROUTER adjacency
                 * -- see the note there. */
                uint32_t ep, rt;
                iss >> ep >> rt;
                if (epToRouter_.size() <= ep) epToRouter_.resize(ep + 1, UINT32_MAX);
                epToRouter_[ep] = rt;
                /* 1.11.92 (F2): the port width Garnet flitises at (0 = the
                 * global flit width, as in TopologyBuilders::buildFromFile). */
                long lat; uint32_t w = 0;
                if (iss >> lat) { if (!(iss >> w)) w = 0; }
                if (epWidth_.size() <= ep) epWidth_.resize(ep + 1, 0);
                epWidth_[ep] = w;
            } else if (token == "rlevel") {
                // 1.11.92 (F4): "rlevel <router> <level> <branch 1|0>"
                uint32_t r; int lv = -1, br = 0;
                iss >> r >> lv >> br;
                if (routerLevel_.size() <= r) {
                    routerLevel_.resize(r + 1, -1);
                    routerBranch_.resize(r + 1, 0);
                }
                routerLevel_[r] = lv;
                routerBranch_[r] = (uint8_t)(br ? 1 : 0);
            }
        }

        /* 1.11.92 (F2/F4): the rooted-tree view for the per-packet path walk.
         * Same test as TopologyBuilders::buildFromFile's tree detection: every
         * router reachable from ROOT (0) and exactly routers-1 undirected
         * edges (each "int" line is one direction). */
        const size_t R = customAdj_.size();
        routerParent_.assign(R, -1);
        routerDepth_.assign(R, -1);
        customIsTree_ = false;
        if (R > 0) {
            size_t directed = 0;
            for (const auto& v : customAdj_) directed += v.size();
            std::vector<uint32_t> q; q.push_back(0);
            routerDepth_[0] = 0;
            size_t head = 0, visited = 1;
            while (head < q.size()) {
                uint32_t u = q[head++];
                for (uint32_t v : customAdj_[u]) {
                    if (routerDepth_[v] != -1) continue;
                    routerDepth_[v] = routerDepth_[u] + 1;
                    routerParent_[v] = (int)u;
                    visited++;
                    q.push_back(v);
                }
            }
            customIsTree_ = (visited == R) && (directed == (R - 1) * 2);
        }
    }

    // ── M/D/1 queuing contention model ───────────────────────

    double estimateUtilization() const {
        // ρ = (total_packets × avg_hops) / (num_links × total_cycles)
        if (stats_.total_cycles == 0 || stats_.num_routers == 0) {
            // Before any cycles are recorded, estimate based on packet count
            // Use a conservative model: scale utilization with packet rate
            double pkt_rate = static_cast<double>(stats_.total_packets);
            double capacity = static_cast<double>(stats_.num_routers * 4);  // ~4 links per router
            if (capacity == 0) return 0.0;
            double rho = pkt_rate / (capacity * 1000.0);  // normalize
            return std::min(rho, 0.95);  // cap below 1 for stability
        }

        double avg_hops = (stats_.total_packets > 0) ?
            static_cast<double>(stats_.total_hops) / stats_.total_packets : 1.0;
        double num_links = stats_.num_routers * 4.0;  // approximate
        double rho = (stats_.total_packets * avg_hops) /
                     (num_links * stats_.total_cycles);
        return std::min(rho, 0.95);
    }

    // ── Simple model latency (hop count + M/D/1 queuing) ─────

    uint32_t getAnalyticalLatency(uint32_t src, uint32_t dst) {
        if (src == dst) return 0;

        uint32_t hops = getHopCount(src, dst);

        // Base latency: injection + hops * (router + link) + ejection
        uint32_t baseLat = injectionLatency_ +
                           hops * (routerLatency_ + linkLatency_) +
                           injectionLatency_;

        // M/D/1 queuing: W_q = ρ / (2μ(1-ρ))
        double rho = estimateUtilization();
        double contention = 0.0;
        if (rho > 0.0 && rho < 1.0) {
            contention = rho / (2.0 * (1.0 - rho));
        }
        uint32_t contentionCycles = static_cast<uint32_t>(baseLat * contention);

        uint32_t latency = baseLat + contentionCycles;

        // Update stats (1.11.92 F2: true flits and router traversals)
        stats_.total_latency += latency;
        countPacket_(src, dst, hops, false);

        return latency;
    }

    // ── Cycle-accurate latency (Garnet bridge) ─────────────

    uint32_t getCycleAccurateLatency(uint32_t src, uint32_t dst) {
#ifdef HAVE_GARNET
        if (!garnetInitialized_) {
            initGarnetNetwork();
        }

        if (src == dst) return 0;
        if (src >= numNodes_ || dst >= numNodes_) {
            return getAnalyticalLatency(src, dst);
        }

        // Full network reset before each packet injection.
        // This clears all VC states, credits, buffers, Consumer schedules,
        // and the EventQueue.  Ensures a pristine network for each query.
        resetGarnetState();

        uint64_t injectTime = 0;
        gem5::curTickRef() = injectTime;

        // Create a data message from src to dst
        auto msg = std::make_shared<gem5::ruby::SimpleMessage>(
            static_cast<uint32_t>(src),
            static_cast<uint32_t>(dst),
            gem5::ruby::MessageSizeType::Data,
            injectTime);

        // Inject into source node's toNet queue (vnet 0, 1 cycle delay)
        toNetBufs_[src][0]->enqueue(msg, injectTime, uint64_t(1));

        // Process events until message arrives at destination
        uint64_t maxTime = injectTime + 100000;  // Safety: 100k cycles max
        while (gem5::curTickRef() < maxTime) {
            if (fromNetBufs_[dst][0]->isReady(gem5::curTickRef())) {
                break;
            }
            if (!gem5::EventQueue::instance().processOneEvent()) {
                gem5::curTickRef()++;
            }
        }

        uint32_t latCycles;
        if (fromNetBufs_[dst][0]->isReady(gem5::curTickRef())) {
            fromNetBufs_[dst][0]->dequeue(gem5::curTickRef());
            latCycles = static_cast<uint32_t>(gem5::curTickRef() - injectTime);
        } else {
            warn("[GarnetNetwork] Drain timeout src=%d dst=%d",
                 src, dst);
            latCycles = getAnalyticalLatency(src, dst);
        }

        // Update stats (1.11.92 F2: true flits and router traversals)
        uint32_t hops = getHopCount(src, dst);
        stats_.total_latency += latCycles;
        countPacket_(src, dst, hops, false);

        return latCycles;
#else
        return getAnalyticalLatency(src, dst);
#endif
    }

#ifdef HAVE_GARNET
    // ── Garnet initialization ────────────────────────────────

    uint32_t mapRoutingAlgorithm(NoCRouting r) const {
        switch (r) {
            case NoCRouting::TABLE:    return 0;  // TABLE_
            case NoCRouting::XY:       return 1;  // XY_
            case NoCRouting::CUSTOM:   return 2;  // CUSTOM_
            case NoCRouting::DOR:      return 3;  // DOR_
            case NoCRouting::SHORTEST: return 4;  // SHORTEST_
            case NoCRouting::DIRECT:   return 5;  // DIRECT_
            case NoCRouting::NCA:      return 6;  // NCA_
        }
        return 0;  // TABLE_ fallback
    }

    void initGarnetNetwork() {
        namespace g = gem5;
        namespace gr = gem5::ruby;
        namespace gg = gem5::ruby::garnet;

        info("[GarnetNetwork] Initializing Garnet detailed network...");

        // Reset global tick and clear EventQueue.
        // Previous Garnet instances (e.g. H-tree DRAM hierarchy networks)
        // may have left stale events in the shared singleton EventQueue.
        g::setCurTick(0);
        g::EventQueue::instance().clear();
        garnetTick_ = 0;
        g::EventQueue::instance().clear();

        // Create RubySystem
        gr::RubySystem::Params rsp;
        rsp.name = "ruby_system";
        rubySys_ = new gr::RubySystem(rsp);

        // Build topology
        uint32_t vnet_count = 3;
        g::Cycles linkLat(linkLatency_);
        g::Cycles routerLat(routerLatency_);

        gg::TopologyResult result;
        switch (topology_) {
            case NoCTopology::MESH_2D:
                result = gg::TopologyBuilders::buildMesh(
                    numRows_, numCols_, numNodes_,
                    vcsPerVnet_, vnet_count, linkLat, routerLat, flitSizeBits_);
                break;
            case NoCTopology::TORUS_2D:
                result = gg::TopologyBuilders::buildTorus(
                    numRows_, numCols_, numNodes_,
                    vcsPerVnet_, vnet_count, linkLat, routerLat, flitSizeBits_);
                break;
            case NoCTopology::RING:
                result = gg::TopologyBuilders::buildRing(
                    numNodes_, numNodes_,
                    vcsPerVnet_, vnet_count, linkLat, routerLat, flitSizeBits_,
                    ringUnidirectional_);
                break;
            case NoCTopology::CROSSBAR:
                result = gg::TopologyBuilders::buildCrossbar(
                    numNodes_,
                    vcsPerVnet_, vnet_count, linkLat, routerLat, flitSizeBits_);
                break;
            case NoCTopology::FAT_TREE:
                result = gg::TopologyBuilders::buildFatTree(
                    numNodes_, 2,
                    vcsPerVnet_, vnet_count, linkLat, routerLat, flitSizeBits_);
                break;
            case NoCTopology::BUS:
                result = gg::TopologyBuilders::buildBus(
                    numNodes_,
                    vcsPerVnet_, vnet_count, linkLat, routerLat, flitSizeBits_);
                break;
            case NoCTopology::H_TREE:
                result = gg::TopologyBuilders::buildHTree(
                    numNodes_,
                    vcsPerVnet_, vnet_count, linkLat, routerLat, flitSizeBits_);
                break;
            case NoCTopology::CUSTOM:
                if (!customTopoFile_.empty()) {
                    result = gg::TopologyBuilders::buildFromFile(
                        customTopoFile_,
                        vcsPerVnet_, vnet_count, linkLat, routerLat, flitSizeBits_);
                } else {
                    warn("[GarnetNetwork] CUSTOM topology requires topology_file; "
                         "falling back to MESH_2D");
                    result = gg::TopologyBuilders::buildMesh(
                        numRows_, numCols_, numNodes_,
                        vcsPerVnet_, vnet_count, linkLat, routerLat, flitSizeBits_);
                }
                break;
        }

        // Create MessageBuffers for each node/vnet
        toNetBufs_.resize(numNodes_);
        fromNetBufs_.resize(numNodes_);
        for (uint32_t i = 0; i < numNodes_; i++) {
            toNetBufs_[i].resize(vnet_count);
            fromNetBufs_[i].resize(vnet_count);
            for (uint32_t v = 0; v < vnet_count; v++) {
                gr::MessageBuffer::Params mbp;
                mbp.name = "toNet_" + std::to_string(i) + "_v" + std::to_string(v);
                auto* toNet = new gr::MessageBuffer(mbp);
                toNet->setVnet(v);
                toNetBufs_[i][v] = toNet;
                allMsgBufs_.push_back(toNet);

                gr::MessageBuffer::Params mbp2;
                mbp2.name = "fromNet_" + std::to_string(i) + "_v" + std::to_string(v);
                auto* fromNet = new gr::MessageBuffer(mbp2);
                fromNet->setVnet(v);
                fromNetBufs_[i][v] = fromNet;
                allMsgBufs_.push_back(fromNet);
            }
        }

        // Populate GarnetNetworkParams
        gr::GarnetNetworkParams gnp;
        gnp.name = "garnet_net";
        gnp.num_nodes = numNodes_;
        gnp.vnet_type_names = {"request", "response", "data"};
        gnp.topology = result.topology;
        gnp.ruby_system = rubySys_;

        if (topology_ == NoCTopology::MESH_2D || topology_ == NoCTopology::TORUS_2D) {
            gnp.num_rows = numRows_;
            gnp.num_cols = numCols_;
        } else {
            gnp.num_rows = 0;
            gnp.num_cols = 0;
        }

        gnp.vcs_per_vnet = vcsPerVnet_;
        // VC buffers must hold at least one full packet
        uint32_t effectiveDataBits = dataMsgBits_ > 0 ? dataMsgBits_ : 576;
        uint32_t dataFlits = (effectiveDataBits + flitSizeBits_ - 1) / flitSizeBits_;
        uint32_t minBuf = std::max(buffersPerVc_, std::max(dataFlits, 1u));
        gnp.buffers_per_data_vc = minBuf;
        gnp.buffers_per_ctrl_vc = minBuf;
        gnp.ni_flit_size = flitSizeBits_ / 8;
        gnp.routing_algorithm = mapRoutingAlgorithm(routing_);
        gnp.garnet_deadlock_threshold = deadlockThreshold_;  // configurable (default 500k)
        gnp.control_msg_size_bits = controlMsgBits_;
        gnp.data_msg_size_bits = dataMsgBits_;
        gnp.routers = result.routers;
        gnp.netifs = result.nis;

        // Create GarnetNetwork
        garnetNet_ = new gg::GarnetNetwork(gnp);

        // Set message buffers on the network
        for (uint32_t i = 0; i < numNodes_; i++) {
            for (uint32_t v = 0; v < vnet_count; v++) {
                garnetNet_->setToNetQueue(i, false, v, "request", toNetBufs_[i][v]);
                garnetNet_->setFromNetQueue(i, false, v, "response", fromNetBufs_[i][v]);
            }
        }

        // Initialize: wires links, connects NIs to message buffers
        garnetNet_->init();
        garnetNet_->regStats();

        garnetInitialized_ = true;
        info("[GarnetNetwork] Garnet detailed initialized: %d nodes, %zu routers, routing=%s",
             numNodes_, result.routers.size(), nocRoutingStr(routing_).c_str());
    }
#endif
};


#endif  // GARNET_NETWORK_H_
