/**
 * @file internal_dram_network.cpp
 * @brief Implementation of In-Memory Network Model
 */

#include "memory/internal_dram_network.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cmath>

namespace pimid {

//=============================================================================
// Free Functions
//=============================================================================

MemoryTechnology parseMemoryTechnology(const std::string& dram_type) {
    if (dram_type == "DDR3") return MemoryTechnology::DDR3;
    if (dram_type == "DDR4" || dram_type == "DDR4-RVRR" || dram_type == "DDR4-VRR")
        return MemoryTechnology::DDR4;
    if (dram_type == "DDR5" || dram_type == "DDR5-RVRR" || dram_type == "DDR5-VRR")
        return MemoryTechnology::DDR5;
    if (dram_type == "LPDDR5") return MemoryTechnology::LPDDR5;
    if (dram_type == "GDDR6") return MemoryTechnology::GDDR6;
    if (dram_type == "HBM2") return MemoryTechnology::HBM2;
    if (dram_type == "HBM3") return MemoryTechnology::HBM3;
    if (dram_type == "SRAM") return MemoryTechnology::SRAM;
    /* 1.11.25: accept the CANONICAL spellings this codebase actually uses.
     * canonicalMemTech() normalises to STT_MRAM / RERAM / PCM, and every
     * config and string comparison in main.cpp uses those -- but this parser
     * accepted only STT-MRAM/STTMRAM/MRAM, so "STT_MRAM" fell through to
     * UNKNOWN. Two vocabularies that disagreed, silently: the memory plugin
     * factory then refused a technology the rest of the simulator handles
     * fine. Caught by gate 1134, which reported the reason rather than
     * substituting a value.
     *
     * Kept minimal ON PURPOSE (user): canonicalMemTech() normalises the alias
     * spellings (STTMRAM/MRAM -> STT_MRAM, PCRAM/3DXPOINT -> PCM,
     * RESISTIVE/MEMRISTOR -> RERAM) before anything reaches here, so this
     * parser needs the CANONICAL forms only. Duplicating the alias list in a
     * second place is how the two vocabularies drifted apart to begin with. */
    if (dram_type == "STT_MRAM" || dram_type == "STT-MRAM")
        return MemoryTechnology::STT_MRAM;
    if (dram_type == "PCM") return MemoryTechnology::PCM;
    if (dram_type == "ReRAM" || dram_type == "RERAM") return MemoryTechnology::ReRAM;
    return MemoryTechnology::UNKNOWN;
}

bool isHBM(MemoryTechnology tech) {
    return tech == MemoryTechnology::HBM2 ||
           tech == MemoryTechnology::HBM3;
}

bool isDRAM(MemoryTechnology tech) {
    return tech == MemoryTechnology::DDR3 ||
           tech == MemoryTechnology::DDR4 ||
           tech == MemoryTechnology::DDR5 ||
           tech == MemoryTechnology::LPDDR5 ||
           tech == MemoryTechnology::GDDR6 ||
           tech == MemoryTechnology::HBM2 ||
           tech == MemoryTechnology::HBM3;
}

std::string SwitchHierarchyConfig::levelName(int idx) const {
    if (idx < 0 || idx >= NUM_HIERARCHY_LEVELS) return "Unknown";

    if (isHBM(technology)) {
        switch (idx) {
            case 0: return "Subarray";
            case 1: return "Bank";
            case 2: return "BankGroup";
            case 3: return "DieLayer";
            case 4: return "LogicDie";
            case 5: return "Channel";
            case 6: return "System";
            default: return "Unknown";
        }
    } else if (!isDRAM(technology)) {
        // NVM/SRAM: L0-L1 active, L2-L6 passthrough
        switch (idx) {
            case 0: return "Subarray";
            case 1: return "Bank";
            default: return "Unused";
        }
    } else {
        switch (idx) {
            case 0: return "Subarray";
            case 1: return "Bank";
            case 2: return "BankGroup";
            case 3: return "Chip";
            case 4: return "Rank";
            case 5: return "Channel";
            case 6: return "System";
            default: return "Unknown";
        }
    }
}

//=============================================================================
// Topology-Aware Hop Count
//=============================================================================

/**
 * @brief Average hop count for a topology with num_nodes endpoints.
 *
 * Formulas from interconnection network theory:
 *   crossbar/bus:        1 (single switch/medium)
 *   point-to-point:      1 (direct link)
 *   ring:                N/4 (bidirectional ring average)
 *   mesh_2d:             (2/3)(sqrt(N)-1) (Manhattan distance average)
 *   torus_2d:            sqrt(N)/2 (wrap-around halves distance)
 *   fat_tree/h_tree:     2*log2(N) (up to root + down)
 *   unknown:             1 (conservative fallback)
 */
static double avgHops(const std::string& topology, int num_nodes) {
    if (num_nodes <= 1) return 0.0;
    if (num_nodes == 2) return 1.0;
    int N = num_nodes;

    if (topology == "crossbar" || topology == "bus")
        return 0.0;  // single router, 0 internal hops
    if (topology == "point-to-point" || topology == "point_to_point")
        return 1.0;
    if (topology == "ring") {
        if (N % 2 == 0)
            return static_cast<double>(N * N) / (4.0 * (N - 1));
        else
            return (N + 1) / 4.0;
    }
    if (topology == "mesh" || topology == "mesh_2d") {
        double k = std::ceil(std::sqrt(static_cast<double>(N)));
        return 2.0 * (k * k - 1.0) / (3.0 * k);
    }
    if (topology == "torus" || topology == "torus_2d") {
        double k = std::ceil(std::sqrt(static_cast<double>(N)));
        if (static_cast<int>(k) % 2 == 0)
            return 2.0 * (k / 4.0);
        else
            return 2.0 * (k * k - 1.0) / (4.0 * k);
    }
    if (topology == "fat_tree" || topology == "h_tree") {
        int a = 2;
        int L = static_cast<int>(std::ceil(std::log(static_cast<double>(N)) / std::log(static_cast<double>(a))));
        double totalHops = 0.0;
        int totalPairs = 0;
        int subtreeSize = a;
        for (int d = L; d >= 1; d--) {
            int prevSubtreeSize = (d == L) ? 1 : subtreeSize / a;
            int newPeers = subtreeSize - prevSubtreeSize;
            int hopCount = 2 * (L - d);
            totalHops += static_cast<double>(newPeers) * hopCount;
            totalPairs += newPeers;
            subtreeSize *= a;
        }
        return (totalPairs > 0) ? totalHops / totalPairs : 0.0;
    }
    return 0.0;
}

/**
 * @brief Total unidirectional channels in the network.
 *
 * Each channel carries 1 flit/cycle.  This determines the average
 * per-channel load when computing M/M/1 contention.
 */
static int totalChannels(const std::string& topology, int N) {
    if (N <= 1) return 1;
    if (topology == "bus") return 1;       // shared bus: 1 flit/cycle total
    if (topology == "crossbar") return N;  // N output ports
    if (topology == "ring") return 2 * N;  // bidirectional: N CW + N CCW
    if (topology == "mesh" || topology == "mesh_2d") {
        int k = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(N))));
        return 4 * k * (k - 1);  // 2 dir x 2 dim x k rows x (k-1) links
    }
    if (topology == "torus" || topology == "torus_2d") {
        int k = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(N))));
        return 4 * k * k;  // 2 dir x 2 dim x k rows x k links (wrap-around)
    }
    if (topology == "fat_tree" || topology == "h_tree") {
        // Binary tree: (N_routers - 1) parent-child links x 2 directions
        int levels = static_cast<int>(std::ceil(std::log2(static_cast<double>(N))));
        int total_routers = (1 << levels) - 1;
        return 2 * std::max(1, total_routers - 1);
    }
    return N;
}

/**
 * @brief Hotspot factor: ratio of max-loaded channel to average.
 *
 * Accounts for non-uniform link loading in different topologies.
 * XY-routed mesh: center links carry ~2x average.
 * Trees: root link carries much more than leaves.
 */
static double hotspotFactor(const std::string& topology) {
    if (topology == "bus" || topology == "crossbar") return 1.0;  // N/A (switch models)
    if (topology == "ring") return 1.0;        // bidirectional ring: fairly uniform
    if (topology == "mesh" || topology == "mesh_2d") return 2.0;  // center links ~2x average (XY routing)
    if (topology == "torus" || topology == "torus_2d") return 3.5; // DOR dateline VC class separation
    if (topology == "fat_tree" || topology == "h_tree") return 1.5; // root ~1.5x average (uniform BW)
    return 1.0;
}

//=============================================================================
// Constructor
//=============================================================================

InternalDRAMNetwork::InternalDRAMNetwork(
    const std::string& dram_type,
    std::shared_ptr<NetworkModel> network_model)
    : dram_type_(dram_type),
      technology_(parseMemoryTechnology(dram_type)),
      num_subarrays_per_bank_(0),
      num_banks_per_bg_(0),
      num_bg_per_chip_(0),
      num_chips_per_rank_(0),
      use_custom_switch_config_(false),
      external_network_model_(network_model),
      use_garnet_models_(false),
      current_cycle_(0),
      total_packets_sent_(0),
      total_packets_completed_(0),
      total_bytes_transferred_(0),
      total_network_latency_(0) {
    queue_limits_.fill(DEFAULT_QUEUE_DEPTH_LIMIT);
    garnet_networks_.fill(nullptr);
    bridge_garnet_networks_.fill(nullptr);
    network_accesses_.fill(0);
    level_models_.fill(NetworkModelType::SIMPLE);
}

//=============================================================================
// Static Helper
//=============================================================================

int InternalDRAMNetwork::levelToIndex(NetworkLevel level) {
    switch (level) {
        case NetworkLevel::SUBARRAY_NETWORK:    return 0;
        case NetworkLevel::BANK_NETWORK:        return 1;
        case NetworkLevel::BANK_GROUP_NETWORK:  return 2;
        case NetworkLevel::CHIP_NETWORK:        return 3;
        case NetworkLevel::RANK_NETWORK:        return 4;
        case NetworkLevel::CHANNEL_NETWORK:     return 5;
        case NetworkLevel::SYSTEM_NETWORK:      return 6;
        default:                                return 0;
    }
}

//=============================================================================
// Initialize
//=============================================================================

void InternalDRAMNetwork::initialize(int num_subarrays_per_bank,
                                     int num_banks_per_bg,
                                     int num_bg_per_chip,
                                     int num_chips_per_rank) {
    num_subarrays_per_bank_ = num_subarrays_per_bank;
    num_banks_per_bg_ = num_banks_per_bg;
    num_bg_per_chip_ = num_bg_per_chip;
    num_chips_per_rank_ = num_chips_per_rank;

    // Configure network based on memory type (DRAM and NVM). Accept both
    // hyphen and underscore spellings of the NVM technologies -- the README,
    // YAML_REFERENCE, and Ramulator2's tech tables use STT_MRAM (underscore),
    // RERAM, etc.; earlier branches only handled the hyphen forms, so
    // underscore configs silently fell through to "Unknown, using DDR4
    // defaults" and were simulated with a DDR4-shaped 7-level hierarchy
    // instead of the intended SRAM/NVM L0-L1 layout.
    if (dram_type_ == "SRAM") {
        configureSRAMNetwork();
    } else if (dram_type_ == "STT-MRAM" || dram_type_ == "STTMRAM" ||
               dram_type_ == "STT_MRAM" || dram_type_ == "STTRAM" ||
               dram_type_ == "MRAM") {
        configureSTTMRAMNetwork();
    } else if (dram_type_ == "PCM" || dram_type_ == "PRAM") {
        configurePCMNetwork();
    } else if (dram_type_ == "ReRAM" || dram_type_ == "RERAM" ||
               dram_type_ == "RRAM") {
        configureReRAMNetwork();
    } else if (dram_type_ == "DDR3") {
        configureDDR3Network();
    } else if (dram_type_ == "DDR4" || dram_type_ == "DDR4-RVRR" || dram_type_ == "DDR4-VRR") {
        configureDDR4Network();
    } else if (dram_type_ == "DDR5" || dram_type_ == "DDR5-RVRR" || dram_type_ == "DDR5-VRR") {
        configureDDR5Network();
    } else if (dram_type_ == "LPDDR5") {
        configureLPDDR5Network();
    } else if (dram_type_ == "GDDR6") {
        configureGDDR6Network();
    } else if (dram_type_ == "HBM2") {
        configureHBM2Network();
    } else if (dram_type_ == "HBM3") {
        configureHBM3Network();
    } else {
        std::cerr << "WARNING: Unknown memory type " << dram_type_
                  << ", using DDR4 defaults\n";
        configureDDR4Network();
    }

    // Populate num_nodes per level from DRAM organization
    network_configs_[0].num_nodes = num_subarrays_per_bank_;
    network_configs_[1].num_nodes = num_banks_per_bg_;
    network_configs_[2].num_nodes = num_bg_per_chip_;
    network_configs_[3].num_nodes = num_chips_per_rank_;
    /* 1.11.57 (audit C006): the top two counts are the LAST two literals in
     * this ladder, and they are the only ones with no configuration surface at
     * all -- initialize() carries subarrays, banks, bank-groups and chips, and
     * overrideLevelConfig() sets width, frequency, latency, topology and router
     * fields but never num_nodes. So a 16-channel HBM3 stack was modelled with
     * a 2-node channel network and memory.ranks_per_channel changed nothing at
     * L4, while the same release taught the PLACEMENT side to resolve both
     * counts correctly. num_nodes is not decorative: it drives avgHops() in
     * both transfer-latency paths and in the contention model, and it gates
     * detailed-Garnet router construction.
     *
     * setTopLevelNodeCounts() is the surface that was missing. Until a caller
     * uses it these stay at 2, which is a DIMM-shaped assumption and is now
     * announced as one rather than passing for an organisation. */
    network_configs_[4].num_nodes = 2;  // ranks per channel -- ASSUMED, see above
    network_configs_[5].num_nodes = 2;  // channels -- ASSUMED, see above
    network_configs_[6].num_nodes = 1;  // system root
    if (!top_level_nodes_set_) {
        static bool announced_top_nodes = false;
        if (!announced_top_nodes) {
            announced_top_nodes = true;
            std::cerr << "[hierarchy] NOTE: the rank (L4) and channel (L5) node "
                         "counts are the assumed 2 and 2 -- a DIMM-shaped "
                         "default, not this device's organisation. They set the "
                         "hop term at those two rungs and gate detailed-router "
                         "construction there. Call setTopLevelNodeCounts() with "
                         "the run's ranks_per_channel and channel count to "
                         "replace them." << std::endl;
        }
    }

    // Set router defaults for all levels (MINIMAL pipeline for internal DRAM)
    for (int i = 0; i < NUM_HIERARCHY_LEVELS; ++i) {
        network_configs_[i].router_latency = 1;
        network_configs_[i].router_pipeline = 3;  // MINIMAL
        network_configs_[i].router_bypass = true;
        network_configs_[i].virtual_networks = 2;  // read + write
        network_configs_[i].virtual_channels_per_vn = 1;
        network_configs_[i].input_buffer_depth = 2;
        network_configs_[i].output_buffer_depth = 2;
    }

    // Initialize bridge defaults from technology
    initializeBridgeDefaults();

    resetStats();

    // Build a temporary SwitchHierarchyConfig to get level names
    SwitchHierarchyConfig tmp_cfg;
    tmp_cfg.technology = technology_;

    /* 1.11.71 (the last piece of audit B006): SAY THAT THIS BLOCK IS
     * PROVISIONAL, at the place that prints it.
     *
     * These widths are the per-technology table defaults applied in this
     * constructor, which runs BEFORE main.cpp decides whether the run's
     * architecture object reconciles with its preset. When it does -- and
     * since 1.11.70 that is every technology in the lineup -- buildHierarchy
     * replaces every width below with the sourced ladder and prints it as
     * "Hierarchy link ladder from the <tech> architecture object". 1.11.57
     * added that superseding line but left this block unlabelled, so a reader
     * or a log parser still met two different fabrics under one technology
     * name and no indication of which one ran. */
    std::cout << "In-Memory Network Initialized (" << dram_type_ << ") "
                 "-- PROVISIONAL per-technology table defaults, printed at "
                 "construction. If a sourced ladder is adopted below "
                 "(\"Hierarchy link ladder from the " << dram_type_
              << " architecture object\"), THOSE widths are the ones "
                 "simulated and these are superseded:\n";
    for (int i = 0; i < NUM_HIERARCHY_LEVELS; ++i) {
        std::string name = tmp_cfg.levelName(i);
        if (name == "Unused") {
            std::cout << "  L" << i << " " << std::setw(12) << std::left << "Unused:"
                      << " passthrough\n";
        } else {
            std::cout << "  L" << i << " " << std::setw(12) << std::left << (name + " network:")
                      << " " << network_configs_[i].link_width_bits
                      << " bits, " << network_configs_[i].bandwidth_GBs << " GB/s";
            if (i < NUM_TIER_BOUNDARIES) {
                std::cout << "  [bridge: " << bridges_[i].count
                          << "x " << bridges_[i].lower_width_bits << "b->"
                          << bridges_[i].upper_width_bits << "b, "
                          << bridges_[i].latency_ns << "ns]";
            }
            std::cout << "\n";
        }
    }
}

//=============================================================================
// Network Configuration Methods
//=============================================================================

void InternalDRAMNetwork::configureDDR4Network() {
    // NOTE: Width defaults are design-specific placeholders.
    // L0 = post-column-mux subarray output (narrow), L1 = bank H-tree root (wider).
    // Current values may be inverted. Correct values should come from Ramulator2
    // (DRAM), CACTI (SRAM), or NVSim (NVM) characterization, or user YAML overrides
    // (noc.levels.*). See TODO for proper integration.

    // DDR4 x8 device: 8n prefetch -> 64-bit internal data bus
    // Internal paths (L0-L2) are all at prefetch width.
    // The bottleneck is L3 (DQ pins): x8 = 8-bit per chip.
    // L4 (rank bus) = 8 chips x 8 DQ = 64-bit channel.

    // L0: Subarray network (within bank): column mux -> prefetch buffer
    network_configs_[0].link_width_bits = 512;  // L0 subarray: wide on-die (~4x channel)
    network_configs_[0].frequency_GHz = 1.2;    // DDR4-2400 core clock
    network_configs_[0].bandwidth_GBs =
        (network_configs_[0].link_width_bits / 8.0) *
        network_configs_[0].frequency_GHz;
    network_configs_[0].latency_cycles = 5;    // Column decode + sense amp
    network_configs_[0].topology = "crossbar";

    // L1: Bank network (banks within bank group): shared internal data bus
    network_configs_[1].link_width_bits = 256;     // L1 bank: ~2x channel
    network_configs_[1].frequency_GHz = 1.2;
    network_configs_[1].bandwidth_GBs =
        (network_configs_[1].link_width_bits / 8.0) *
        network_configs_[1].frequency_GHz;
    network_configs_[1].latency_cycles = 2;        // Mux arbitration only
    network_configs_[1].topology = "bus";

    // L2: Bank group network (BGs within chip): internal mux to I/O gating
    network_configs_[2].link_width_bits = 256;     // L2 bankgroup: ~2x channel
    network_configs_[2].frequency_GHz = 1.2;
    network_configs_[2].bandwidth_GBs =
        (network_configs_[2].link_width_bits / 8.0) *
        network_configs_[2].frequency_GHz;
    network_configs_[2].latency_cycles = 2;        // BG mux + tCCD_S penalty
    network_configs_[2].topology = "bus";

    // L3: Chip network (chip DQ pins -> rank bus)
    network_configs_[3].link_width_bits = 192;     // channel-derived width, no device-width bottleneck
    network_configs_[3].frequency_GHz = 1.2;
    network_configs_[3].bandwidth_GBs =
        (network_configs_[3].link_width_bits / 8.0) *
        network_configs_[3].frequency_GHz;
    network_configs_[3].latency_cycles = 5;        // I/O driver + package delay
    network_configs_[3].topology = "point-to-point";

    // L4: Rank network (ranks within channel): = channel (64-bit DDR ch @ data rate)
    network_configs_[4].link_width_bits = 128;
    network_configs_[4].frequency_GHz = 1.2;
    network_configs_[4].bandwidth_GBs =
        (network_configs_[4].link_width_bits / 8.0) *
        network_configs_[4].frequency_GHz;
    network_configs_[4].latency_cycles = 8;        // Rank-to-rank switching (tRR)
    network_configs_[4].topology = "bus";

    // L5: Channel network (channel to MC): JEDEC 19.2 GB/s @ 1.2 GHz
    network_configs_[5].link_width_bits = 128;
    network_configs_[5].frequency_GHz = 1.2;
    network_configs_[5].bandwidth_GBs =
        (network_configs_[5].link_width_bits / 8.0) *
        network_configs_[5].frequency_GHz;
    network_configs_[5].latency_cycles = 5;        // PCB trace to MC
    network_configs_[5].topology = "crossbar";

    // L6: System network (root): 1 channel aggregate
    network_configs_[6].link_width_bits = 128;
    network_configs_[6].frequency_GHz = 1.2;
    network_configs_[6].bandwidth_GBs =
        (network_configs_[6].link_width_bits / 8.0) *
        network_configs_[6].frequency_GHz;
    network_configs_[6].latency_cycles = 2;
    network_configs_[6].topology = "crossbar";
}

void InternalDRAMNetwork::configureDDR5Network() {
    // NOTE: Width defaults are design-specific placeholders.
    // L0 = post-column-mux subarray output (narrow), L1 = bank H-tree root (wider).
    // Current values may be inverted. Correct values should come from Ramulator2
    // (DRAM), CACTI (SRAM), or NVSim (NVM) characterization, or user YAML overrides
    // (noc.levels.*). See TODO for proper integration.

    // DDR5 x8 device: 16n prefetch -> 128-bit internal data bus
    // Two independent 32-bit subchannels per DIMM (modeled as channels).
    // Internal paths (L0-L2) at prefetch width; L3 = x8 DQ pin bottleneck.
    // L4 (rank bus) = 4 chips x x8 = 32b per subchannel, 64b total.

    // L0: Subarray network: 16n prefetch buffer
    network_configs_[0].link_width_bits = 512;  // ~153 GB/s
    network_configs_[0].frequency_GHz = 2.4;    // DDR5-4800 core clock
    network_configs_[0].bandwidth_GBs =
        (network_configs_[0].link_width_bits / 8.0) *
        network_configs_[0].frequency_GHz;
    network_configs_[0].latency_cycles = 5;
    network_configs_[0].topology = "crossbar";

    // L1: Bank network (banks within BG): internal prefetch-width bus
    network_configs_[1].link_width_bits = 256;     // ~76.8
    network_configs_[1].frequency_GHz = 2.4;
    network_configs_[1].bandwidth_GBs =
        (network_configs_[1].link_width_bits / 8.0) *
        network_configs_[1].frequency_GHz;
    network_configs_[1].latency_cycles = 2;        // Mux arbitration
    network_configs_[1].topology = "bus";

    // L2: Bank group network (BGs within chip): internal mux
    network_configs_[2].link_width_bits = 256;     // ~76.8
    network_configs_[2].frequency_GHz = 2.4;
    network_configs_[2].bandwidth_GBs =
        (network_configs_[2].link_width_bits / 8.0) *
        network_configs_[2].frequency_GHz;
    network_configs_[2].latency_cycles = 2;
    network_configs_[2].topology = "bus";

    // L3: Chip DQ pins -> rank bus
    network_configs_[3].link_width_bits = 128;     // channel-derived width, no device-width bottleneck (~38.4)
    network_configs_[3].frequency_GHz = 2.4;
    network_configs_[3].bandwidth_GBs =
        (network_configs_[3].link_width_bits / 8.0) *
        network_configs_[3].frequency_GHz;
    network_configs_[3].latency_cycles = 5;        // I/O driver + package
    network_configs_[3].topology = "point-to-point";

    // L4: Rank network (ranks within channel): >= channel
    network_configs_[4].link_width_bits = 96;      // ~28.8 (>= channel)
    network_configs_[4].frequency_GHz = 2.4;
    network_configs_[4].bandwidth_GBs =
        (network_configs_[4].link_width_bits / 8.0) *
        network_configs_[4].frequency_GHz;
    network_configs_[4].latency_cycles = 8;        // Rank switching
    network_configs_[4].topology = "bus";

    // L5: Channel network (channel to MC): JEDEC 25.6 GB/s @ 2.4 GHz
    network_configs_[5].link_width_bits = 88;      // ~26.4 (~ JEDEC 25.6)
    network_configs_[5].frequency_GHz = 2.4;
    network_configs_[5].bandwidth_GBs =
        (network_configs_[5].link_width_bits / 8.0) *
        network_configs_[5].frequency_GHz;
    network_configs_[5].latency_cycles = 5;
    network_configs_[5].topology = "crossbar";

    // L6: System network (root): 1 channel aggregate
    network_configs_[6].link_width_bits = 88;
    network_configs_[6].frequency_GHz = 2.4;
    network_configs_[6].bandwidth_GBs =
        (network_configs_[6].link_width_bits / 8.0) *
        network_configs_[6].frequency_GHz;
    network_configs_[6].latency_cycles = 2;
    network_configs_[6].topology = "crossbar";
}

void InternalDRAMNetwork::configureHBM2Network() {
    // HBM2: 3D-stacked DRAM (TSVs), 8 channels. Per-level link widths form a
    // MONOTONE ladder -- widest at the near-data subarray, narrowing toward the
    // shared channel/system root -- at freq = 1.2 GHz (the 2.4 GT/s I/O clock / 2).
    //
    // FIXED (was inverted): the old table used freq = 1.0 -> L0 = 256/8*1.0 = 32
    // GB/s, BELOW HBM2's 38.4 GB/s per-channel rate, AND a non-monotone L1 (64 bits
    // = 8 GB/s, dipping under both L0 and L2). Both are corrected so this analytical
    // tier AGREES with the detailed L[] (emitDramCustomTopology): L0 = 38.4 GB/s
    // (= per-channel egress, BW-neutral) and the leaf ladder mirrors 256/192/160/128.
    // HBM2's near-data win is the 8-channel parallelism, not a wide subarray link,
    // so the per-level gradient is intentionally gentle.
    //
    // Physical hierarchy:
    //   L0 subarray   L1 bank       L2 bank_group  L3 die_layer
    //   L4 logic_die  L5 channel    L6 system root
    struct Lv { int w; int lat; };
    const Lv hbm2[7] = {
        {256,  3},   // L0 subarray   : 38.4 GB/s (wide column I/O, TSV)
        {192,  5},   // L1 bank       : 28.8
        {160,  8},   // L2 bank_group : 24.0
        {128, 10},   // L3 die_layer  : 19.2 (vertical TSV)
        {128,  8},   // L4 logic_die  : 19.2
        {128,  5},   // L5 channel    : 19.2 (pseudo-channel)
        {128, 15},   // L6 system     : 19.2 (interposer root)
    };
    for (int i = 0; i < 7; ++i) {
        network_configs_[i].link_width_bits = hbm2[i].w;
        network_configs_[i].frequency_GHz   = 1.2;   // 2.4 GT/s I/O clock / 2
        network_configs_[i].bandwidth_GBs   =
            (network_configs_[i].link_width_bits / 8.0) *
            network_configs_[i].frequency_GHz;
        network_configs_[i].latency_cycles  = hbm2[i].lat;
        network_configs_[i].topology        = "crossbar";
    }
}

void InternalDRAMNetwork::configureHBM3Network() {
    // NOTE: Width defaults are design-specific placeholders.
    // L0 = post-column-mux subarray output (narrow), L1 = bank H-tree root (wider).
    // Current values may be inverted. Correct values should come from Ramulator2
    // (DRAM), CACTI (SRAM), or NVSim (NVM) characterization, or user YAML overrides
    // (noc.levels.*). See TODO for proper integration.

    // HBM3: faster TSVs, wider internal paths, pseudo-channels
    //
    // Same physical hierarchy as HBM2, but with 2x bandwidth at most levels:
    //   - Higher frequency (1.8 GHz vs 1.0 GHz)
    //   - Wider internal datapaths (512-bit subarray, 128-bit bank)
    //   - 16 pseudo-channels (vs HBM2's 8 channels)

    network_configs_[0].link_width_bits = 512;  // 2x HBM2
    network_configs_[0].frequency_GHz = 1.8;    // HBM3 @ 1.8 GHz
    network_configs_[0].bandwidth_GBs =
        (network_configs_[0].link_width_bits / 8.0) *
        network_configs_[0].frequency_GHz;
    network_configs_[0].latency_cycles = 3;
    network_configs_[0].topology = "crossbar";

    // L1: Banks within BG -- 128-bit TSV paths (2x HBM2)
    network_configs_[1].link_width_bits = 128;
    network_configs_[1].frequency_GHz = 1.8;
    network_configs_[1].bandwidth_GBs =
        (network_configs_[1].link_width_bits / 8.0) *
        network_configs_[1].frequency_GHz;
    network_configs_[1].latency_cycles = 5;
    network_configs_[1].topology = "crossbar";

    // L2: BGs within die layer
    network_configs_[2].link_width_bits = 256;
    network_configs_[2].frequency_GHz = 1.8;
    network_configs_[2].bandwidth_GBs =
        (network_configs_[2].link_width_bits / 8.0) *
        network_configs_[2].frequency_GHz;
    network_configs_[2].latency_cycles = 8;
    network_configs_[2].topology = "crossbar";

    // L3: Die layers within stack -- improved TSV
    network_configs_[3].link_width_bits = 128;
    network_configs_[3].frequency_GHz = 1.8;
    network_configs_[3].bandwidth_GBs =
        (network_configs_[3].link_width_bits / 8.0) *
        network_configs_[3].frequency_GHz;
    network_configs_[3].latency_cycles = 10;
    network_configs_[3].topology = "crossbar";

    // L4: Logic die -- improved TSV
    network_configs_[4].link_width_bits = 256;
    network_configs_[4].frequency_GHz = 1.8;
    network_configs_[4].bandwidth_GBs =
        (network_configs_[4].link_width_bits / 8.0) *
        network_configs_[4].frequency_GHz;
    network_configs_[4].latency_cycles = 6;
    network_configs_[4].topology = "crossbar";

    // L5: Pseudo-channels within stack
    network_configs_[5].link_width_bits = 256;
    network_configs_[5].frequency_GHz = 1.8;
    network_configs_[5].bandwidth_GBs =
        (network_configs_[5].link_width_bits / 8.0) *
        network_configs_[5].frequency_GHz;
    network_configs_[5].latency_cycles = 5;
    network_configs_[5].topology = "crossbar";

    // L6: System root (multi-stack, interposer)
    network_configs_[6].link_width_bits = 256;
    network_configs_[6].frequency_GHz = 1.8;
    network_configs_[6].bandwidth_GBs =
        (network_configs_[6].link_width_bits / 8.0) *
        network_configs_[6].frequency_GHz;
    network_configs_[6].latency_cycles = 12;
    network_configs_[6].topology = "crossbar";
}

void InternalDRAMNetwork::configureDDR3Network() {
    // NOTE: Width defaults are design-specific placeholders.
    // L0 = post-column-mux subarray output (narrow), L1 = bank H-tree root (wider).
    // Current values may be inverted. Correct values should come from Ramulator2
    // (DRAM), CACTI (SRAM), or NVSim (NVM) characterization, or user YAML overrides
    // (noc.levels.*). See TODO for proper integration.

    // DDR3 x8 device: 8n prefetch -> 64-bit internal data bus
    // No bank groups in DDR3 (L2 = passthrough-like, same internal width).
    // L3 = x8 DQ pin bottleneck. L4 = 8 chips x x8 = 64b channel.

    // L0: Subarray network: 8n prefetch x x8 = 64b internal
    network_configs_[0].link_width_bits = 512;  // L0 subarray: wide on-die (~4x channel)
    network_configs_[0].frequency_GHz = 0.8;    // DDR3-1600 core clock
    network_configs_[0].bandwidth_GBs =
        (network_configs_[0].link_width_bits / 8.0) *
        network_configs_[0].frequency_GHz;
    network_configs_[0].latency_cycles = 6;    // Older process, slightly slower
    network_configs_[0].topology = "crossbar";

    // L1: Bank network (banks within chip -- no BGs in DDR3): internal bus
    network_configs_[1].link_width_bits = 256;     // L1 bank: ~2x channel
    network_configs_[1].frequency_GHz = 0.8;
    network_configs_[1].bandwidth_GBs =
        (network_configs_[1].link_width_bits / 8.0) *
        network_configs_[1].frequency_GHz;
    network_configs_[1].latency_cycles = 3;        // Mux arbitration
    network_configs_[1].topology = "bus";

    // L2: Bank group network: DDR3 has no bank groups -- passthrough
    network_configs_[2].link_width_bits = 256;     // L2 bankgroup: ~2x channel
    network_configs_[2].frequency_GHz = 0.8;
    network_configs_[2].bandwidth_GBs =
        (network_configs_[2].link_width_bits / 8.0) *
        network_configs_[2].frequency_GHz;
    network_configs_[2].latency_cycles = 1;        // No BG overhead
    network_configs_[2].topology = "bus";

    // L3: Chip DQ pins -> rank bus: x8 bottleneck
    network_configs_[3].link_width_bits = 192;     // channel-derived width, no device-width bottleneck
    network_configs_[3].frequency_GHz = 0.8;
    network_configs_[3].bandwidth_GBs =
        (network_configs_[3].link_width_bits / 8.0) *
        network_configs_[3].frequency_GHz;
    network_configs_[3].latency_cycles = 6;        // I/O driver + package (older tech)
    network_configs_[3].topology = "point-to-point";

    // L4: Rank network: 8 chips x x8 = 64b channel
    network_configs_[4].link_width_bits = 128;
    network_configs_[4].frequency_GHz = 0.8;
    network_configs_[4].bandwidth_GBs =
        (network_configs_[4].link_width_bits / 8.0) *
        network_configs_[4].frequency_GHz;
    network_configs_[4].latency_cycles = 10;       // Rank switching
    network_configs_[4].topology = "bus";

    // L5: Channel network
    network_configs_[5].link_width_bits = 128;
    network_configs_[5].frequency_GHz = 0.8;
    network_configs_[5].bandwidth_GBs =
        (network_configs_[5].link_width_bits / 8.0) *
        network_configs_[5].frequency_GHz;
    network_configs_[5].latency_cycles = 5;
    network_configs_[5].topology = "crossbar";

    // L6: System network
    network_configs_[6].link_width_bits = 128;
    network_configs_[6].frequency_GHz = 0.8;
    network_configs_[6].bandwidth_GBs =
        (network_configs_[6].link_width_bits / 8.0) *
        network_configs_[6].frequency_GHz;
    network_configs_[6].latency_cycles = 2;
    network_configs_[6].topology = "crossbar";
}

void InternalDRAMNetwork::configureLPDDR5Network() {
    // NOTE: Width defaults are design-specific placeholders.
    // L0 = post-column-mux subarray output (narrow), L1 = bank H-tree root (wider).
    // Current values may be inverted. Correct values should come from Ramulator2
    // (DRAM), CACTI (SRAM), or NVSim (NVM) characterization, or user YAML overrides
    // (noc.levels.*). See TODO for proper integration.

    // LPDDR5 x16 device: 16n prefetch -> 256-bit internal data bus
    // Mobile PoP: x16 per die, no rank typically.
    // L3 = x16 DQ pins (wider than DDR). L4 = single die per channel = 16b.

    // L0: Subarray network: 16n prefetch x x16 = 256b internal
    network_configs_[0].link_width_bits = 256;  // Wide internal prefetch
    network_configs_[0].frequency_GHz = 1.6;    // LPDDR5-6400 core clock
    network_configs_[0].bandwidth_GBs =
        (network_configs_[0].link_width_bits / 8.0) *
        network_configs_[0].frequency_GHz;
    network_configs_[0].latency_cycles = 4;     // Optimized for mobile
    network_configs_[0].topology = "crossbar";

    // L1: Bank network (banks within BG): internal prefetch-width bus
    network_configs_[1].link_width_bits = 256;     // Same internal bus
    network_configs_[1].frequency_GHz = 1.6;
    network_configs_[1].bandwidth_GBs =
        (network_configs_[1].link_width_bits / 8.0) *
        network_configs_[1].frequency_GHz;
    network_configs_[1].latency_cycles = 2;        // Mux arbitration
    network_configs_[1].topology = "bus";

    // L2: Bank group network (BGs within chip)
    network_configs_[2].link_width_bits = 256;     // Internal path
    network_configs_[2].frequency_GHz = 1.6;
    network_configs_[2].bandwidth_GBs =
        (network_configs_[2].link_width_bits / 8.0) *
        network_configs_[2].frequency_GHz;
    network_configs_[2].latency_cycles = 2;
    network_configs_[2].topology = "bus";

    // L3: Chip DQ pins: x16 device @ 6.4 Gbps/pin (LPDDR5-6400).
    // Model BW = width/8 * core_freq; width absorbs the per-pin data-rate
    // multiplier (6.4 Gbps / 1.6 GHz core = 4x), like DDR5's 64b*2=128b.
    // x16 * 4 = 64b -> 12.8 GB/s (JEDEC LPDDR5 x16 channel).
    network_configs_[3].link_width_bits = 64;      // x16 DQ @ 6.4Gbps -> 12.8 GB/s
    network_configs_[3].frequency_GHz = 1.6;
    network_configs_[3].bandwidth_GBs =
        (network_configs_[3].link_width_bits / 8.0) *
        network_configs_[3].frequency_GHz;
    network_configs_[3].latency_cycles = 3;        // PoP packaging, short traces
    network_configs_[3].topology = "point-to-point";

    // L4: Rank network (typically 1 rank in mobile): sustains channel BW.
    network_configs_[4].link_width_bits = 64;      // x16 @ 6.4Gbps -> 12.8 GB/s
    network_configs_[4].frequency_GHz = 1.6;
    network_configs_[4].bandwidth_GBs =
        (network_configs_[4].link_width_bits / 8.0) *
        network_configs_[4].frequency_GHz;
    network_configs_[4].latency_cycles = 3;        // Minimal rank overhead
    network_configs_[4].topology = "bus";

    // L5: Channel network (to MC): LPDDR5 x16 channel @ 6.4 Gbps = 12.8 GB/s.
    network_configs_[5].link_width_bits = 64;      // x16 @ 6.4Gbps -> 12.8 GB/s
    network_configs_[5].frequency_GHz = 1.6;
    network_configs_[5].bandwidth_GBs =
        (network_configs_[5].link_width_bits / 8.0) *
        network_configs_[5].frequency_GHz;
    network_configs_[5].latency_cycles = 3;        // Short PoP traces
    network_configs_[5].topology = "crossbar";

    // L6: System network (multi-channel)
    network_configs_[6].link_width_bits = 64;      // 4 x 16b channels combined
    network_configs_[6].frequency_GHz = 1.6;
    network_configs_[6].bandwidth_GBs =
        (network_configs_[6].link_width_bits / 8.0) *
        network_configs_[6].frequency_GHz;
    network_configs_[6].latency_cycles = 2;
    network_configs_[6].topology = "crossbar";
}

void InternalDRAMNetwork::configureGDDR6Network() {
    // NOTE: Width defaults are design-specific placeholders.
    // L0 = post-column-mux subarray output (narrow), L1 = bank H-tree root (wider).
    // Current values may be inverted. Correct values should come from Ramulator2
    // (DRAM), CACTI (SRAM), or NVSim (NVM) characterization, or user YAML overrides
    // (noc.levels.*). See TODO for proper integration.

    // GDDR6 x16 device: 16n prefetch -> 256-bit internal data bus
    // Dual-channel per chip: 2 x 8-bit channels (x8 per channel externally).
    // No rank in GDDR6 -- point-to-point per chip.
    // L3 = 16b per chip (2 x x8 channels). L4+ = PCB level.

    // L0: Subarray network: 16n x x16 = 256b internal
    network_configs_[0].link_width_bits = 256;  // Wide internal prefetch
    network_configs_[0].frequency_GHz = 2.0;    // GDDR6 @ ~2 GHz core
    network_configs_[0].bandwidth_GBs =
        (network_configs_[0].link_width_bits / 8.0) *
        network_configs_[0].frequency_GHz;
    network_configs_[0].latency_cycles = 4;     // Throughput-optimized
    network_configs_[0].topology = "crossbar";

    // L1: Bank network (banks within BG): internal prefetch-width bus
    network_configs_[1].link_width_bits = 256;     // Same internal bus
    network_configs_[1].frequency_GHz = 2.0;
    network_configs_[1].bandwidth_GBs =
        (network_configs_[1].link_width_bits / 8.0) *
        network_configs_[1].frequency_GHz;
    network_configs_[1].latency_cycles = 2;        // Fast internal mux
    network_configs_[1].topology = "bus";

    // L2: Bank group network: internal path
    network_configs_[2].link_width_bits = 256;     // Internal prefetch width
    network_configs_[2].frequency_GHz = 2.0;
    network_configs_[2].bandwidth_GBs =
        (network_configs_[2].link_width_bits / 8.0) *
        network_configs_[2].frequency_GHz;
    network_configs_[2].latency_cycles = 2;
    network_configs_[2].topology = "bus";

    // L3: Chip DQ pins: x16 device @ 16 Gbps/pin (GDDR6-16Gbps).
    // Model BW = width/8 * core_freq; width absorbs the per-pin data-rate
    // multiplier (16 Gbps / 2.0 GHz core = 8x), like DDR5's 64b*2=128b.
    // x16 * 8 = 128b -> 32 GB/s per device (JEDEC GDDR6 x16 @ 16Gbps).
    network_configs_[3].link_width_bits = 128;     // x16 DQ @ 16Gbps -> 32 GB/s
    network_configs_[3].frequency_GHz = 2.0;
    network_configs_[3].bandwidth_GBs =
        (network_configs_[3].link_width_bits / 8.0) *
        network_configs_[3].frequency_GHz;
    network_configs_[3].latency_cycles = 4;        // On-board traces, short
    network_configs_[3].topology = "point-to-point";

    // L4: Rank network (no ranks in GDDR6 -- point-to-point): sustains device BW.
    network_configs_[4].link_width_bits = 128;     // x16 @ 16Gbps -> 32 GB/s
    network_configs_[4].frequency_GHz = 2.0;
    network_configs_[4].bandwidth_GBs =
        (network_configs_[4].link_width_bits / 8.0) *
        network_configs_[4].frequency_GHz;
    network_configs_[4].latency_cycles = 1;        // No rank switching
    network_configs_[4].topology = "point-to-point";

    // L5: Channel network (chip to MC): point-to-point, no bus sharing, so the
    // device DQ BW (32 GB/s) is sustained to the MC. (A 32-bit channel = 2 chips
    // aggregates to 64 GB/s; per-chip access path is gated at 32 GB/s.)
    network_configs_[5].link_width_bits = 128;     // x16 @ 16Gbps -> 32 GB/s
    network_configs_[5].frequency_GHz = 2.0;
    network_configs_[5].bandwidth_GBs =
        (network_configs_[5].link_width_bits / 8.0) *
        network_configs_[5].frequency_GHz;
    network_configs_[5].latency_cycles = 3;
    network_configs_[5].topology = "crossbar";

    // L6: System network (multiple chips to GPU MC)
    network_configs_[6].link_width_bits = 256;     // 16 chips x 16b = 256b aggregate
    network_configs_[6].frequency_GHz = 2.0;
    network_configs_[6].bandwidth_GBs =
        (network_configs_[6].link_width_bits / 8.0) *
        network_configs_[6].frequency_GHz;
    network_configs_[6].latency_cycles = 2;
    network_configs_[6].topology = "crossbar";
}

//=============================================================================
// Non-Volatile Memory (NVM) Configurations
//=============================================================================

void InternalDRAMNetwork::configureSRAMNetwork() {
    // NOTE: Width defaults are design-specific placeholders.
    // L0 = post-column-mux subarray output (narrow), L1 = bank H-tree root (wider).
    // Current values may be inverted. Correct values should come from Ramulator2
    // (DRAM), CACTI (SRAM), or NVSim (NVM) characterization, or user YAML overrides
    // (noc.levels.*). See TODO for proper integration.

    // SRAM: On-chip memory with very fast, wide interconnects

    // L0: Subarray network: Very wide on-chip buses
    network_configs_[0].link_width_bits = 128;  // Wide on-chip paths
    network_configs_[0].frequency_GHz = 2.5;    // High frequency on-chip
    network_configs_[0].bandwidth_GBs =
        (network_configs_[0].link_width_bits / 8.0) *
        network_configs_[0].frequency_GHz;
    network_configs_[0].latency_cycles = 1;     // Very low latency
    network_configs_[0].topology = "crossbar";

    // L1: Bank network: Still on-chip, fast
    network_configs_[1].link_width_bits = 64;       // 64-bit on-chip
    network_configs_[1].frequency_GHz = 2.5;
    network_configs_[1].bandwidth_GBs =
        (network_configs_[1].link_width_bits / 8.0) *
        network_configs_[1].frequency_GHz;
    network_configs_[1].latency_cycles = 2;         // Fast on-chip
    network_configs_[1].topology = "crossbar";

    // L2: Bank group network: On-chip crossbar
    network_configs_[2].link_width_bits = 64;
    network_configs_[2].frequency_GHz = 2.5;
    network_configs_[2].bandwidth_GBs =
        (network_configs_[2].link_width_bits / 8.0) *
        network_configs_[2].frequency_GHz;
    network_configs_[2].latency_cycles = 3;
    network_configs_[2].topology = "crossbar";

    // L3: Chip network: On-chip, very fast
    network_configs_[3].link_width_bits = 32;
    network_configs_[3].frequency_GHz = 2.5;
    network_configs_[3].bandwidth_GBs =
        (network_configs_[3].link_width_bits / 8.0) *
        network_configs_[3].frequency_GHz;
    network_configs_[3].latency_cycles = 4;
    network_configs_[3].topology = "crossbar";

    // L4: Rank network
    network_configs_[4].link_width_bits = 64;
    network_configs_[4].frequency_GHz = 2.5;
    network_configs_[4].bandwidth_GBs =
        (network_configs_[4].link_width_bits / 8.0) *
        network_configs_[4].frequency_GHz;
    network_configs_[4].latency_cycles = 5;
    network_configs_[4].topology = "crossbar";

    // L5: Channel network
    network_configs_[5].link_width_bits = 64;
    network_configs_[5].frequency_GHz = 2.5;
    network_configs_[5].bandwidth_GBs =
        (network_configs_[5].link_width_bits / 8.0) *
        network_configs_[5].frequency_GHz;
    network_configs_[5].latency_cycles = 3;
    network_configs_[5].topology = "crossbar";

    // L6: System network
    network_configs_[6].link_width_bits = 64;
    network_configs_[6].frequency_GHz = 2.5;
    network_configs_[6].bandwidth_GBs =
        (network_configs_[6].link_width_bits / 8.0) *
        network_configs_[6].frequency_GHz;
    network_configs_[6].latency_cycles = 2;
    network_configs_[6].topology = "crossbar";
}

void InternalDRAMNetwork::configureSTTMRAMNetwork() {
    // NOTE: Width defaults are design-specific placeholders.
    // L0 = post-column-mux subarray output (narrow), L1 = bank H-tree root (wider).
    // Current values may be inverted. Correct values should come from Ramulator2
    // (DRAM), CACTI (SRAM), or NVSim (NVM) characterization, or user YAML overrides
    // (noc.levels.*). See TODO for proper integration.

    // STT-MRAM: Non-volatile with asymmetric read/write
    // Write latency >> read latency due to MTJ switching

    // L0: Subarray network: Moderate width
    network_configs_[0].link_width_bits = 64;   // Moderate width
    network_configs_[0].frequency_GHz = 1.5;    // STT-MRAM frequency
    network_configs_[0].bandwidth_GBs =
        (network_configs_[0].link_width_bits / 8.0) *
        network_configs_[0].frequency_GHz;
    network_configs_[0].latency_cycles = 3;     // Read latency
    network_configs_[0].topology = "crossbar";

    // L1: Bank network: Similar to DRAM but with NVM characteristics
    network_configs_[1].link_width_bits = 16;       // 16-bit paths
    network_configs_[1].frequency_GHz = 1.5;
    network_configs_[1].bandwidth_GBs =
        (network_configs_[1].link_width_bits / 8.0) *
        network_configs_[1].frequency_GHz;
    network_configs_[1].latency_cycles = 6;         // Moderate latency
    network_configs_[1].topology = "bus";

    // L2: Bank group network
    network_configs_[2].link_width_bits = 32;
    network_configs_[2].frequency_GHz = 1.5;
    network_configs_[2].bandwidth_GBs =
        (network_configs_[2].link_width_bits / 8.0) *
        network_configs_[2].frequency_GHz;
    network_configs_[2].latency_cycles = 12;
    network_configs_[2].topology = "bus";

    // L3: Chip network
    network_configs_[3].link_width_bits = 16;
    network_configs_[3].frequency_GHz = 1.5;
    network_configs_[3].bandwidth_GBs =
        (network_configs_[3].link_width_bits / 8.0) *
        network_configs_[3].frequency_GHz;
    network_configs_[3].latency_cycles = 25;
    network_configs_[3].topology = "point-to-point";

    // L4: Rank network
    network_configs_[4].link_width_bits = 64;
    network_configs_[4].frequency_GHz = 1.5;
    network_configs_[4].bandwidth_GBs =
        (network_configs_[4].link_width_bits / 8.0) *
        network_configs_[4].frequency_GHz;
    network_configs_[4].latency_cycles = 20;
    network_configs_[4].topology = "bus";

    // L5: Channel network
    network_configs_[5].link_width_bits = 64;
    network_configs_[5].frequency_GHz = 1.5;
    network_configs_[5].bandwidth_GBs =
        (network_configs_[5].link_width_bits / 8.0) *
        network_configs_[5].frequency_GHz;
    network_configs_[5].latency_cycles = 5;
    network_configs_[5].topology = "crossbar";

    // L6: System network
    network_configs_[6].link_width_bits = 64;
    network_configs_[6].frequency_GHz = 1.5;
    network_configs_[6].bandwidth_GBs =
        (network_configs_[6].link_width_bits / 8.0) *
        network_configs_[6].frequency_GHz;
    network_configs_[6].latency_cycles = 2;
    network_configs_[6].topology = "crossbar";
}

void InternalDRAMNetwork::configurePCMNetwork() {
    // NOTE: Width defaults are design-specific placeholders.
    // L0 = post-column-mux subarray output (narrow), L1 = bank H-tree root (wider).
    // Current values may be inverted. Correct values should come from Ramulator2
    // (DRAM), CACTI (SRAM), or NVSim (NVM) characterization, or user YAML overrides
    // (noc.levels.*). See TODO for proper integration.

    // PCM: Phase Change Memory with high write latency
    // Write >> Read due to crystallization/amorphization

    // L0: Subarray network: Moderate width
    network_configs_[0].link_width_bits = 64;   // Moderate width
    network_configs_[0].frequency_GHz = 1.2;    // PCM frequency
    network_configs_[0].bandwidth_GBs =
        (network_configs_[0].link_width_bits / 8.0) *
        network_configs_[0].frequency_GHz;
    network_configs_[0].latency_cycles = 4;     // Read latency
    network_configs_[0].topology = "crossbar";

    // L1: Bank network: Similar organization to DRAM
    network_configs_[1].link_width_bits = 16;       // 16-bit paths
    network_configs_[1].frequency_GHz = 1.2;
    network_configs_[1].bandwidth_GBs =
        (network_configs_[1].link_width_bits / 8.0) *
        network_configs_[1].frequency_GHz;
    network_configs_[1].latency_cycles = 8;         // Moderate latency
    network_configs_[1].topology = "bus";

    // L2: Bank group network
    network_configs_[2].link_width_bits = 32;
    network_configs_[2].frequency_GHz = 1.2;
    network_configs_[2].bandwidth_GBs =
        (network_configs_[2].link_width_bits / 8.0) *
        network_configs_[2].frequency_GHz;
    network_configs_[2].latency_cycles = 16;
    network_configs_[2].topology = "bus";

    // L3: Chip network
    network_configs_[3].link_width_bits = 16;
    network_configs_[3].frequency_GHz = 1.2;
    network_configs_[3].bandwidth_GBs =
        (network_configs_[3].link_width_bits / 8.0) *
        network_configs_[3].frequency_GHz;
    network_configs_[3].latency_cycles = 32;
    network_configs_[3].topology = "point-to-point";

    // L4: Rank network
    network_configs_[4].link_width_bits = 64;
    network_configs_[4].frequency_GHz = 1.2;
    network_configs_[4].bandwidth_GBs =
        (network_configs_[4].link_width_bits / 8.0) *
        network_configs_[4].frequency_GHz;
    network_configs_[4].latency_cycles = 25;
    network_configs_[4].topology = "bus";

    // L5: Channel network
    network_configs_[5].link_width_bits = 64;
    network_configs_[5].frequency_GHz = 1.2;
    network_configs_[5].bandwidth_GBs =
        (network_configs_[5].link_width_bits / 8.0) *
        network_configs_[5].frequency_GHz;
    network_configs_[5].latency_cycles = 5;
    network_configs_[5].topology = "crossbar";

    // L6: System network
    network_configs_[6].link_width_bits = 64;
    network_configs_[6].frequency_GHz = 1.2;
    network_configs_[6].bandwidth_GBs =
        (network_configs_[6].link_width_bits / 8.0) *
        network_configs_[6].frequency_GHz;
    network_configs_[6].latency_cycles = 2;
    network_configs_[6].topology = "crossbar";
}

void InternalDRAMNetwork::configureReRAMNetwork() {
    // NOTE: Width defaults are design-specific placeholders.
    // L0 = post-column-mux subarray output (narrow), L1 = bank H-tree root (wider).
    // Current values may be inverted. Correct values should come from Ramulator2
    // (DRAM), CACTI (SRAM), or NVSim (NVM) characterization, or user YAML overrides
    // (noc.levels.*). See TODO for proper integration.

    // ReRAM: Resistive RAM with moderate asymmetry
    // Faster writes than PCM, but still slower than reads

    // L0: Subarray network: Moderate width
    network_configs_[0].link_width_bits = 64;   // Moderate width
    network_configs_[0].frequency_GHz = 1.4;    // ReRAM frequency
    network_configs_[0].bandwidth_GBs =
        (network_configs_[0].link_width_bits / 8.0) *
        network_configs_[0].frequency_GHz;
    network_configs_[0].latency_cycles = 3;     // Good read latency
    network_configs_[0].topology = "crossbar";

    // L1: Bank network: Similar to STT-MRAM
    network_configs_[1].link_width_bits = 16;       // 16-bit paths
    network_configs_[1].frequency_GHz = 1.4;
    network_configs_[1].bandwidth_GBs =
        (network_configs_[1].link_width_bits / 8.0) *
        network_configs_[1].frequency_GHz;
    network_configs_[1].latency_cycles = 7;         // Moderate latency
    network_configs_[1].topology = "bus";

    // L2: Bank group network
    network_configs_[2].link_width_bits = 32;
    network_configs_[2].frequency_GHz = 1.4;
    network_configs_[2].bandwidth_GBs =
        (network_configs_[2].link_width_bits / 8.0) *
        network_configs_[2].frequency_GHz;
    network_configs_[2].latency_cycles = 14;
    network_configs_[2].topology = "bus";

    // L3: Chip network
    network_configs_[3].link_width_bits = 16;
    network_configs_[3].frequency_GHz = 1.4;
    network_configs_[3].bandwidth_GBs =
        (network_configs_[3].link_width_bits / 8.0) *
        network_configs_[3].frequency_GHz;
    network_configs_[3].latency_cycles = 28;
    network_configs_[3].topology = "point-to-point";

    // L4: Rank network
    network_configs_[4].link_width_bits = 64;
    network_configs_[4].frequency_GHz = 1.4;
    network_configs_[4].bandwidth_GBs =
        (network_configs_[4].link_width_bits / 8.0) *
        network_configs_[4].frequency_GHz;
    network_configs_[4].latency_cycles = 22;
    network_configs_[4].topology = "bus";

    // L5: Channel network
    network_configs_[5].link_width_bits = 64;
    network_configs_[5].frequency_GHz = 1.4;
    network_configs_[5].bandwidth_GBs =
        (network_configs_[5].link_width_bits / 8.0) *
        network_configs_[5].frequency_GHz;
    network_configs_[5].latency_cycles = 5;
    network_configs_[5].topology = "crossbar";

    // L6: System network
    network_configs_[6].link_width_bits = 64;
    network_configs_[6].frequency_GHz = 1.4;
    network_configs_[6].bandwidth_GBs =
        (network_configs_[6].link_width_bits / 8.0) *
        network_configs_[6].frequency_GHz;
    network_configs_[6].latency_cycles = 2;
    network_configs_[6].topology = "crossbar";
}


//=============================================================================
// Packet Sending and Processing
//=============================================================================

bool InternalDRAMNetwork::sendPacket(const InternalNetworkPacket& packet) {
    // Multi-tier up-down traversal via LCA routing
    HierarchicalAddress src = bankToAddress(packet.source_bank, packet.source_subarray);
    HierarchicalAddress dst = bankToAddress(packet.dest_bank, packet.dest_subarray);
    int lca = lowestCommonAncestor(src, dst);

    // Track which levels are traversed for statistics
    for (int level = 0; level <= lca; ++level) {
        network_accesses_[level]++;
    }

    // Check if any Garnet level/bridge on the path can accept the packet
    if (use_garnet_models_) {
        for (int level = 0; level <= lca; ++level) {
            if (garnet_networks_[level]) {
                uint32_t src_node = static_cast<uint32_t>(addressFieldAtLevel(src, level));
                if (!garnet_networks_[level]->canInject(src_node)) {
                    return false;  // Congested
                }
            }
            // Check bridge Garnet congestion
            if (level < lca && bridge_garnet_networks_[level]) {
                if (!bridge_garnet_networks_[level]->canInject(0)) {
                    return false;  // Bridge congested
                }
            }
        }
    }

    // Accumulate total latency across all tiers traversed
    uint64_t total_latency = 0;

    // UP: from source tier (L0) to LCA
    for (int level = 0; level < lca; ++level) {
        total_latency += getTierLatency(level, src, dst, packet.data_bytes);
        total_latency += getBridgeLatency(level, packet.data_bytes);
    }

    // AT LCA: route within LCA tier
    total_latency += getTierLatency(lca, src, dst, packet.data_bytes);

    // DOWN: from LCA to destination tier (L0)
    for (int level = lca - 1; level >= 0; --level) {
        total_latency += getBridgeLatency(level, packet.data_bytes);
        total_latency += getTierLatency(level, src, dst, packet.data_bytes);
    }

    // For Garnet-managed levels/bridges, inject into ALL Garnet networks on
    // the path. Each Garnet handles its own tier/bridge independently, with
    // analytical latency offsets between injections.
    if (use_garnet_models_) {
        uint64_t garnet_offset = 0;
        bool any_garnet = false;
        for (int level = 0; level <= lca; ++level) {
            if (garnet_networks_[level]) {
                uint32_t src_node = static_cast<uint32_t>(addressFieldAtLevel(src, level));
                uint32_t dst_node = static_cast<uint32_t>(addressFieldAtLevel(dst, level));

                NetworkPacket net_packet(
                    src_node, dst_node,
                    PacketType::DATA,
                    static_cast<uint32_t>(packet.data_bytes),
                    packet.packet_id,
                    current_cycle_ + garnet_offset
                );
                garnet_networks_[level]->injectPacket(net_packet);
                any_garnet = true;
            }
            // Inject into bridge Garnet if this boundary is DETAILED
            if (level < lca && bridge_garnet_networks_[level]) {
                NetworkPacket br_packet(
                    0, 1,  // bridge: src=0, dst=1
                    PacketType::DATA,
                    static_cast<uint32_t>(packet.data_bytes),
                    packet.packet_id,
                    current_cycle_ + garnet_offset
                );
                bridge_garnet_networks_[level]->injectPacket(br_packet);
                any_garnet = true;
            }
            // Accumulate bridge latency between this level and next
            if (level < lca) {
                garnet_offset += getBridgeLatency(level, packet.data_bytes);
            }
        }
        if (any_garnet) {
            InternalNetworkPacket timed_packet = packet;
            timed_packet.injection_time = current_cycle_;
            timed_packet.completion_time = 0;  // Last Garnet sets this on arrival
            timed_packet.completed = false;
            inflight_packets_.push_back(timed_packet);

            total_packets_sent_++;
            total_bytes_transferred_ += packet.data_bytes;
            return true;
        }
    }

    // All-analytical: compute completion time from accumulated latency
    uint64_t completion_time = current_cycle_ + total_latency;

    InternalNetworkPacket timed_packet = packet;
    timed_packet.injection_time = current_cycle_;
    timed_packet.completion_time = completion_time;
    timed_packet.completed = false;
    inflight_packets_.push_back(timed_packet);

    total_packets_sent_++;
    total_bytes_transferred_ += packet.data_bytes;

    return true;
}

void InternalDRAMNetwork::tick() {
    current_cycle_++;

    // Tick GARNET networks for DETAILED levels and bridges
    if (use_garnet_models_) {
        for (int i = 0; i < NUM_HIERARCHY_LEVELS; ++i) {
            if (level_models_[i] == NetworkModelType::DETAILED && garnet_networks_[i]) {
                garnet_networks_[i]->tick();
            }
        }
        for (int b = 0; b < NUM_TIER_BOUNDARIES; ++b) {
            if (bridge_garnet_networks_[b]) {
                bridge_garnet_networks_[b]->tick();
            }
        }

        // Check for arrived packets in GARNET networks
        processGarnetArrivedPackets();
    }

    // Process bridge buffers -- release packets whose release_cycle <= current_cycle_
    // (completion is handled by processInflightPackets below).
    for (int b = 0; b < NUM_TIER_BOUNDARIES; ++b) {
        for (auto& buffer : bridge_buffers_[b]) {
            while (!buffer.queue.empty() &&
                   buffer.queue.front().release_cycle <= current_cycle_) {
                buffer.queue.pop();
            }
        }
    }

    processInflightPackets();
}

uint64_t InternalDRAMNetwork::getTransferLatency(NetworkLevel level,
                                                 int source_id, int dest_id,
                                                 uint64_t data_bytes) {
    /* 1.11.60 (audit round 4, B017): ONE implementation, ONE answer.
     *
     * This used to be a second, independent copy of the crossing cost --
     * hops x (router + fixed) + serialisation -- and 1.11.59's B014 change
     * landed on the ns twin only. From that release the two functions gave
     * different answers to the same question: at a zero-hop tier (bus,
     * crossbar or a single node, which for a DDR part is six of the seven
     * levels) the ns form charges the fixed term once and this one charged it
     * not at all, so they differed by the WHOLE fixed term -- DDR4 L0 would
     * return 2 cycles here against the ns form's equivalent of 8.
     *
     * It was invisible, and is LATENT rather than live, because every consumer
     * of the cycles form is currently unreachable: calculateNetworkRequirements
     * and executeScatter have no callers outside this file, and the
     * PIMControllerPlugin path is constructed only from enablePIMSupport(),
     * which ramulator_wrapper.cpp records as having no callers either. Nothing
     * measures the disagreement, so nothing could report it. Rather than
     * re-apply the change twice and leave a third divergence available, the
     * cycles form is now the ns form divided by the clock those cycles belong
     * to -- the level's own frequency, which is the clock the per-hop and
     * serialisation terms were counted in. At a level with no stated frequency
     * the ns form's own 1.0 GHz fallback makes the two numerically identical,
     * which is what this function always assumed. */
    int idx = levelToIndex(level);
    const InternalNetworkLink& link = network_configs_[idx];
    double f_ghz = link.frequency_GHz > 0.0 ? link.frequency_GHz : 1.0;

    double ns = getTransferLatencyNs(level, source_id, dest_id, data_bytes);
    uint64_t base = static_cast<uint64_t>(std::ceil(ns * f_ghz));
    if (base == 0) base = 1;
    return base;
}

double InternalDRAMNetwork::getTransferLatencyNs(NetworkLevel level,
                                                 int source_id, int dest_id,
                                                 uint64_t data_bytes) {
    /* 1.11.56 (audit B051): the cycles version above, divided by the clock
     * those cycles belong to. Both terms are per-level: the router pipeline
     * and the fixed link latency tick at frequency_GHz, and a serialisation
     * beat moves link_width_bits at the same frequency. */
    int idx = levelToIndex(level);
    const InternalNetworkLink& link = network_configs_[idx];
    double f_ghz = link.frequency_GHz > 0.0 ? link.frequency_GHz : 1.0;

    double hops = avgHops(link.topology, link.num_nodes);
    int per_hop_router = link.router_latency;
    if (hops <= 1.0 && link.router_bypass && per_hop_router > 1)
        per_hop_router = std::max(1, per_hop_router - 1);

    /* 1.11.59 (audit round 3, B014): a traversal costs its fixed latency ONCE
     * even when the routing hop count is zero.
     *
     * avgHops() returns 0.0 for "bus" and "crossbar" and for num_nodes <= 1,
     * which for a DDR part covers L0, L1, L2, L4, L5 and L6 -- six of the
     * seven levels. Multiplying the fixed term by that zero deleted it: the
     * sense-amp and column-decode time at L0, the mux arbitration at L1/L2,
     * the rank-switching tRR at L4 and the PCB trace at L5 contributed
     * NOTHING to any traversal that crossed them, at every technology. Six of
     * the seven emitted levelLatency values were pure width serialisation.
     *
     * Zero hops means no ROUTING, not no crossing: a bus still has to be
     * driven and sensed. The fixed cost is therefore charged for one crossing
     * plus one per additional hop. A genuine multi-hop topology (mesh, torus,
     * ring, fat tree) is unchanged where avgHops >= 1. */
    double hops_charged = std::max(1.0, hops);

    double per_hop = static_cast<double>(per_hop_router + link.latency_cycles);
    double width = link.link_width_bits > 0 ? link.link_width_bits : 1;
    double serialization = std::ceil(static_cast<double>(data_bytes) * 8.0 / width);
    return (hops_charged * per_hop + serialization) / f_ghz;
}

double InternalDRAMNetwork::getBridgeLatencyNs(int boundary, uint64_t data_bytes,
                                               int lower_width_override,
                                               int upper_width_override,
                                               double base_ns_override,
                                               int router_latency_override,
                                               int router_bypass_override) const {
    if (boundary < 0 || boundary >= NUM_TIER_BOUNDARIES) return 0.0;
    const BridgeConfig& br = bridges_[boundary];

    int lower_w = lower_width_override > 0 ? lower_width_override : br.lower_width_bits;
    int upper_w = upper_width_override > 0 ? upper_width_override : br.upper_width_bits;
    if (lower_w <= 0) lower_w = 1;
    if (upper_w <= 0) upper_w = 1;
    double base_ns = base_ns_override >= 0.0 ? base_ns_override : br.latency_ns;

    int per_hop_router = router_latency_override >= 0 ? router_latency_override
                                                      : br.router_latency;
    bool bypass = router_bypass_override >= 0 ? (router_bypass_override != 0)
                                              : br.router_bypass;
    if (bypass && per_hop_router > 1) per_hop_router = std::max(1, per_hop_router - 1);

    double f_lo = br.lower_frequency_mhz / 1000.0;   // GHz
    double f_hi = br.upper_frequency_mhz / 1000.0;
    if (f_lo <= 0.0) f_lo = 1.0;
    if (f_hi <= 0.0) f_hi = 1.0;

    /* Store-and-forward: the bridge must both ingest the packet from the
     * lower tier and emit it to the upper one, so the crossing cannot be
     * faster than the slower of the two. The old form added a BEAT COUNT
     * taken at the narrower width straight into a cycle total, with no
     * clock attached to either side. */
    double bits = static_cast<double>(data_bytes) * 8.0;
    double ser_lo_ns = std::ceil(bits / lower_w) / f_lo;
    double ser_hi_ns = std::ceil(bits / upper_w) / f_hi;
    double ser_ns = std::max(ser_lo_ns, ser_hi_ns);

    // The router sits on the ingress side, so its pipeline ticks at f_lo.
    return per_hop_router / f_lo + base_ns + ser_ns;
}

void InternalDRAMNetwork::applySourcedLadder(const int width_bits[7],
                                             const double bandwidth_GBs[7]) {
    /* 1.11.59 (audit round 3, B014, second half): adopting the ladder RE-TIMES
     * every per-level fixed latency, because those constants are counts of
     * cycles at the level's own clock and this function changes that clock.
     * DDR5's L3 "I/O driver + package delay" of 5 cycles was authored against
     * 1.2 GHz (4.17 ns) and becomes 2.08 ns at the 2.4 GHz the sourced ladder
     * back-derives -- with no edit to the constant and, until now, no word to
     * the reader. A package delay is a TIME, not a cycle count, so expressing
     * it in cycles at a clock the ladder is free to change is the underlying
     * defect; the constants are left alone (changing them moves the corpus)
     * and the re-timing is stated instead. */
    for (int i = 0; i < NUM_HIERARCHY_LEVELS && i < 7; ++i) {
        if (width_bits[i] <= 0 || bandwidth_GBs[i] <= 0.0) continue;
        auto& cfg = network_configs_[i];
        cfg.link_width_bits = width_bits[i];
        cfg.bandwidth_GBs   = bandwidth_GBs[i];
        // f = BW * 8 / width reproduces the sourced bandwidth exactly, and
        // lands on the data rate for the DQ tiers and the core clock for the
        // array tiers without this class classifying them.
        /* 1.11.57 (audit C003): that second sentence is only true because the
         * bank and bank-group rungs are now derived as a width times the core
         * clock. While they were frozen literals this line silently invented a
         * clock for them -- HBM3's array tiers came out at 1.0 GHz against a
         * 3.2 GHz part -- and the comment asserted the opposite. */
        cfg.frequency_GHz = bandwidth_GBs[i] * 8.0 / static_cast<double>(width_bits[i]);
    }

    /* 1.11.57 (audit C005): THE BRIDGES FOLLOW THE LEVELS THEY JOIN.
     *
     * What was wrong: bridge[i] spans level i and level i+1, and its ingress
     * and egress links ARE those two levels' links -- that is what a tier
     * boundary is. But the widths and clocks came from initializeBridgeDefaults(),
     * a three-class literal table (one branch for all of DDR3/4/5, LPDDR5 and
     * GDDR6, one for both HBM generations, one for SRAM/NVM) written before the
     * levels had a source. So on HBM3 the sourced levels are 512/128/256/1024/
     * 64/64/1024 bits while every bridge was 128 or 256, and no bridge clock in
     * the table (0.5, 0.8, 1.0, 1.6 GHz) is either of that part's two real
     * clocks (3.2 GHz core, 6.4 GT/s DQ).
     *
     * Why it was invisible: until 1.11.56 the bridge term was a dimensionless
     * number added into a cycle total, so a wrong clock in it did not read as
     * a wrong time. B051 made getBridgeLatencyNs() return TIME, which turned
     * every one of these literals into a divisor of real nanoseconds, and
     * D064 replaced the LEVEL widths with the architecture object's in the same
     * release without touching the BRIDGE widths -- so the two halves of one
     * boundary began describing different buses. On HBM3 the L3<->L4 crossing
     * serialised a 64 B payload at ceil(512/128)/1.0 = 4.0 ns per side against
     * the 1024- and 64-bit levels it actually joins.
     *
     * Only the two link descriptions are taken from the ladder. The base
     * latency, the FIFO depth, the parallel-bridge count and the router
     * parameters stay as the technology table set them: those are not link
     * properties and this class has no source for them, so they are left where
     * a reader can still see them for what they are. A YAML level override
     * applied after this changes the level and not the bridge, which is the
     * same division of authority as before. */
    for (int b = 0; b < NUM_TIER_BOUNDARIES; ++b) {
        const int lo = b, hi = b + 1;
        if (width_bits[lo] <= 0 || bandwidth_GBs[lo] <= 0.0) continue;
        if (width_bits[hi] <= 0 || bandwidth_GBs[hi] <= 0.0) continue;
        bridges_[b].lower_width_bits     = width_bits[lo];
        bridges_[b].upper_width_bits     = width_bits[hi];
        bridges_[b].lower_frequency_mhz  = network_configs_[lo].frequency_GHz * 1000.0;
        bridges_[b].upper_frequency_mhz  = network_configs_[hi].frequency_GHz * 1000.0;
    }
}

/* 1.11.57 (audit C006): the configuration surface the rank and channel rungs
 * never had. Both counts come from the run's own resolved organisation; a
 * non-positive value is ignored rather than turned into a zero-node network,
 * because a level with no nodes has no defined hop count. */
void InternalDRAMNetwork::setTopLevelNodeCounts(int ranks_per_channel, int channels) {
    if (ranks_per_channel > 0) network_configs_[4].num_nodes = ranks_per_channel;
    if (channels > 0)          network_configs_[5].num_nodes = channels;
    if (ranks_per_channel > 0 || channels > 0) {
        top_level_nodes_set_ = true;
        std::cout << "  Hierarchy top-tier node counts: L4 (rank) "
                  << network_configs_[4].num_nodes << ", L5 (channel) "
                  << network_configs_[5].num_nodes << "\n";
    }
}

bool InternalDRAMNetwork::canAcceptPacket(NetworkLevel level) {
    size_t current_depth = getQueueDepth(level);
    size_t limit = getQueueLimit(level);

    // If limit is 0, queue is unlimited
    if (limit == 0) {
        return true;
    }

    return current_depth < limit;
}

void InternalDRAMNetwork::setQueueLimit(NetworkLevel level, size_t limit) {
    int idx = levelToIndex(level);
    queue_limits_[idx] = limit;
}

void InternalDRAMNetwork::setQueueLimits(size_t subarray_limit, size_t bank_limit,
                                          size_t bg_limit, size_t chip_limit) {
    queue_limits_[0] = subarray_limit;
    queue_limits_[1] = bank_limit;
    queue_limits_[2] = bg_limit;
    queue_limits_[3] = chip_limit;
}

size_t InternalDRAMNetwork::getQueueDepth(NetworkLevel level) const {
    int idx = levelToIndex(level);
    return network_queues_[idx].size();
}

size_t InternalDRAMNetwork::getQueueLimit(NetworkLevel level) const {
    int idx = levelToIndex(level);
    return queue_limits_[idx];
}

//=============================================================================
// Hierarchical Address Helpers
//=============================================================================

HierarchicalAddress InternalDRAMNetwork::bankToAddress(int global_bank_id, int subarray_id) const {
    HierarchicalAddress addr;
    addr.system = 0;
    addr.channel = 0;  // Single-channel model; multi-channel handled at system level
    addr.subarray = subarray_id;

    int banks_per_chip = num_banks_per_bg_ * num_bg_per_chip_;

    addr.bank = global_bank_id % num_banks_per_bg_;
    addr.bank_group = (global_bank_id / num_banks_per_bg_) % num_bg_per_chip_;
    addr.chip = (global_bank_id / banks_per_chip) % num_chips_per_rank_;
    addr.rank = global_bank_id / (banks_per_chip * num_chips_per_rank_);

    return addr;
}

int InternalDRAMNetwork::addressFieldAtLevel(const HierarchicalAddress& addr, int level) const {
    switch (level) {
        case 0: return addr.subarray;
        case 1: return addr.bank;
        case 2: return addr.bank_group;
        case 3: return addr.chip;
        case 4: return addr.rank;
        case 5: return addr.channel;
        case 6: return addr.system;
        default: return 0;
    }
}

int InternalDRAMNetwork::lowestCommonAncestor(const HierarchicalAddress& src,
                                               const HierarchicalAddress& dst) const {
    // Compare top-down: first level where they differ is the LCA
    // If all fields match from system down to bank, LCA = 0 (subarray level)
    for (int level = NUM_HIERARCHY_LEVELS - 1; level >= 1; --level) {
        if (addressFieldAtLevel(src, level) != addressFieldAtLevel(dst, level)) {
            return level;
        }
    }
    // Same bank: subarray-level routing
    return 0;
}

uint64_t InternalDRAMNetwork::getTierLatency(int level,
                                              const HierarchicalAddress& src,
                                              const HierarchicalAddress& dst,
                                              uint64_t data_bytes) {
    // For Garnet-managed levels, the detailed model handles timing
    if (level_models_[level] == NetworkModelType::DETAILED &&
        use_garnet_models_ && garnet_networks_[level]) {
        // Garnet tick-based timing -- caller injects packet separately
        return 0;
    }

    // SIMPLE: topology-aware analytical + M/D/1 queuing contention
    const InternalNetworkLink& link = network_configs_[level];

    double hops = avgHops(link.topology, link.num_nodes);

    // Router bypass: if single hop and bypass enabled, skip one router stage
    int per_hop_router = link.router_latency;
    if (hops <= 1.0 && link.router_bypass && per_hop_router > 1)
        per_hop_router = std::max(1, per_hop_router - 1);

    double per_hop = static_cast<double>(per_hop_router + link.latency_cycles);
    double serialization = std::ceil(static_cast<double>(data_bytes) * 8.0 / link.link_width_bits);
    uint64_t base = static_cast<uint64_t>(std::ceil(hops * per_hop + serialization));
    if (base == 0) base = 1;

    // M/D/1 queuing contention model
    auto& state = tier_md1_state_[level];
    state.curWindowAccesses++;

    // Update smoothed arrival rate at window boundaries
    if (current_cycle_ >= state.lastUpdateCycle + MD1_WINDOW_CYCLES) {
        double alpha = 0.5;  // exponential smoothing factor
        double cur_rate = static_cast<double>(state.curWindowAccesses) / MD1_WINDOW_CYCLES;
        state.smoothedArrivalRate = alpha * cur_rate + (1.0 - alpha) * state.smoothedArrivalRate;
        state.curWindowAccesses = 0;
        state.lastUpdateCycle = current_cycle_;
    }

    // -- Topology-aware contention: per-channel M/M/1 --------------
    //
    // Three regimes:
    //  A) BUS: shared medium, M/M/1 with (1-rho)^1.5 steeper divergence
    //  B) CROSSBAR: per-output M/M/1 with HOL factor 1.58
    //  C) Multi-hop: per-channel M/M/1 x avgHops, hotspot-adjusted
    //
    // M/M/1 chosen over M/D/1 because real networks have higher service
    // time variance from wormhole blocking and backpressure cascades.

    double svcTime = serialization;  // per-link service time in flits
    if (svcTime < 1.0) svcTime = 1.0;
    double N = static_cast<double>(link.num_nodes);
    // state.smoothedArrivalRate is the AGGREGATE per-tier access rate
    // (accesses/cycle summed over all injectors), NOT a per-node rate.
    // The branches below must therefore not multiply it by N again --
    // doing so overstated offered load by N/injectors on big tiers
    // (same bug as the zsim-side networkContentionLatency, fixed 2026-06-11).
    double arrivalRate = state.smoothedArrivalRate;

    // VCs reduce head-of-line blocking: 1/sqrt(VCs) factor (Dally & Towles)
    int total_vcs = link.virtual_networks * link.virtual_channels_per_vn;
    double vc_factor = (total_vcs > 1) ? 1.0 / std::sqrt(static_cast<double>(total_vcs)) : 1.0;

    double wait_time = 0.0;

    if (link.topology == "bus") {
        // Shared bus: every access crosses the single shared medium.
        // rho = aggregate rate x svcTime. Steeper divergence for round-robin + HOL.
        double rho = arrivalRate * svcTime;
        if (rho >= 0.90) {
            wait_time = 10.0 * static_cast<double>(base);
        } else if (rho > 0.01) {
            double denom = std::pow(1.0 - rho, 1.5);
            wait_time = rho * svcTime / denom;
        }
    } else if (link.topology == "crossbar") {
        // N output ports, each 1 flit/cycle. Aggregate load spreads
        // uniformly over N outputs; per-output arrival = aggregate/N.
        // Input HOL blocking factor ~1.58 (Karol/Hluchyj/Morgan).
        double rhoPerOutput = (arrivalRate / std::max(N, 1.0)) * svcTime;
        double rhoEff = std::min(rhoPerOutput * 1.58, 0.95);
        if (rhoEff > 0.01) {
            wait_time = rhoEff * svcTime / (1.0 - rhoEff);
        }
    } else {
        // Multi-hop: per-channel M/M/1 x avgHops
        int channels = totalChannels(link.topology, link.num_nodes);
        double hopCount = std::max(hops, 0.01);

        // Aggregate flit-hops per cycle spread over all channels
        double totalFlitHops = arrivalRate * svcTime * hopCount;
        double rhoAvg = totalFlitHops / static_cast<double>(channels);

        // Bottleneck channel (hotspot-adjusted)
        double hotspot = hotspotFactor(link.topology);
        double rhoMax = rhoAvg * hotspot;

        if (rhoMax >= 0.75) {
            wait_time = 10.0 * static_cast<double>(base);
        } else if (rhoMax > 0.01) {
            // M/M/1 per channel: E[W] = rhoxS/(1-rho), with VC factor
            wait_time = vc_factor * rhoMax * svcTime / (1.0 - rhoMax);
            wait_time *= hopCount;  // accumulated over all hops
        }
    }
    return static_cast<uint64_t>(base + wait_time + 0.5);
}

uint64_t InternalDRAMNetwork::getBridgeLatency(int boundary, uint64_t data_bytes) {
    if (boundary < 0 || boundary >= NUM_TIER_BOUNDARIES) return 0;

    const BridgeConfig& br = bridges_[boundary];

    // Determine effective model
    NetworkModelType effective = br.model;
    if (effective == NetworkModelType::AUTO) {
        // All AUTO bridges use SIMPLE analytical model with M/D/1 queuing.
        // Each tier's Garnet (if any) models its own internal network independently.
        // The bridge handles cross-tier serialization at the bottleneck width.
        effective = NetworkModelType::SIMPLE;
    }

    // DETAILED bridge: Garnet handles timing
    if (effective == NetworkModelType::DETAILED && bridge_garnet_networks_[boundary]) {
        return 0;
    }

    // SIMPLE: analytical base + M/D/1 queuing contention
    int per_hop_router = br.router_latency;
    if (br.router_bypass && per_hop_router > 1)
        per_hop_router = std::max(1, per_hop_router - 1);

    // Serialization: bottleneck is the narrower side
    double data_bits = static_cast<double>(data_bytes) * 8.0;
    int bottleneck_width = std::min(br.lower_width_bits, br.upper_width_bits);
    double serialization = std::ceil(data_bits / bottleneck_width);

    // Base latency in ns (for frequency-domain crossing)
    double link_ns = br.latency_ns;
    uint64_t base = static_cast<uint64_t>(std::ceil(
        per_hop_router + link_ns + serialization));
    if (base == 0) base = 1;

    // M/D/1 queuing -- VCs reduce HOL blocking, don't multiply bandwidth
    auto& state = bridge_md1_state_[boundary];
    state.curWindowAccesses++;

    if (current_cycle_ >= state.lastUpdateCycle + MD1_WINDOW_CYCLES) {
        double alpha = 0.5;
        double cur_rate = static_cast<double>(state.curWindowAccesses) / MD1_WINDOW_CYCLES;
        state.smoothedArrivalRate = alpha * cur_rate + (1.0 - alpha) * state.smoothedArrivalRate;
        state.curWindowAccesses = 0;
        state.lastUpdateCycle = current_cycle_;
    }

    double service_time = static_cast<double>(base);
    // Parallel bridges DO scale service rate (independent physical links)
    double effective_service_time = service_time / static_cast<double>(std::max(1, br.count));

    double rho = state.smoothedArrivalRate * effective_service_time;

    // VC HOL-blocking reduction factor
    int total_vcs = br.virtual_networks * br.virtual_channels_per_vn;
    double vc_factor = (total_vcs > 1) ? 1.0 / std::sqrt(static_cast<double>(total_vcs)) : 1.0;

    // M/D/1 with saturation clamp: if rho >= 0.95, clamp to 10x base
    double wait_time = 0.0;
    if (rho >= 0.95) {
        wait_time = 10.0 * static_cast<double>(base);
    } else if (rho > 0.01) {
        wait_time = vc_factor * (rho * effective_service_time) / (2.0 * (1.0 - rho));
    }
    return static_cast<uint64_t>(base + wait_time + 0.5);
}

/* 1.11.57 (audit C005): these are DEFAULTS ONLY, and on a technology whose
 * ladder reconciles they no longer survive: applySourcedLadder() overwrites
 * every ingress and egress width and clock with the two levels each bridge
 * joins. What is left of this table after that is the base latency, the FIFO
 * depth, the parallel-bridge count and the router parameters -- none of them a
 * link property, none of them sourced, and all of them still visible here.
 * The widths and clocks below stand only where no ladder is adopted, which is
 * the same case in which the run states that its levels are not tool-sourced.
 *
 * The three branches are one for all of DDR3/4/5, LPDDR5 and GDDR6, one for
 * both HBM generations and one for SRAM/NVM; no clock in them (0.5, 0.8, 1.0,
 * 1.6 GHz) is any modern part's core clock or data rate. */
void InternalDRAMNetwork::initializeBridgeDefaults() {
    if (isHBM(technology_)) {
        // HBM: wide TSV interconnects, symmetric links
        bridges_[0] = {256, 800, 8, 256, 800, 0.5, 1};   // L0<->L1: GSA internal
        bridges_[1] = {128, 1000, 8, 128, 1000, 0.5, 1};  // L1<->L2: crossbar
        if (technology_ == MemoryTechnology::HBM2 || technology_ == MemoryTechnology::HBM3) {
            bridges_[2] = {128, 1000, 8, 128, 1000, 0.5, 2}; // L2<->L3: TSV (2 pseudo-ch)
        } else {
            bridges_[2] = {128, 1000, 8, 128, 1000, 0.5, 1}; // L2<->L3: TSV (single)
        }
        bridges_[3] = {128, 1000, 8, 128, 500, 1.0, 1};   // L3<->L4: TSV to logic die
        bridges_[4] = {128, 500, 8, 128, 500, 0.5, 1};    // L4<->L5: PHY
        bridges_[5] = {128, 500, 8, 64, 500, 2.0, 1};     // L5<->L6: interposer
    } else if (isDRAM(technology_)) {
        // DDR: internal paths are wide, bottleneck at chip DQ pins (L2<->L3)
        bridges_[0] = {128, 800, 8, 64, 800, 0.5, 1};     // L0<->L1: bank internal
        bridges_[1] = {64, 800, 8, 64, 800, 0.25, 1};     // L1<->L2: internal mux
        bridges_[2] = {64, 800, 8, 8, 1600, 1.0, 1};      // L2<->L3: DQ pin bottleneck
        bridges_[3] = {8, 1600, 8, 64, 1600, 1.5, 1};     // L3<->L4: rank bus (8xx8=64b)
        bridges_[4] = {64, 1600, 8, 64, 1600, 0.5, 1};    // L4<->L5: channel
        bridges_[5] = {64, 1600, 8, 64, 800, 2.0, 1};     // L5<->L6: system bus
    } else {
        // NVM/SRAM: monolithic, only L0<->L1 boundary meaningful
        bridges_[0] = {64, 1000, 4, 64, 1000, 0.25, 1};   // L0<->L1: on-chip
        // L1-L5 boundaries: passthrough (zero additional latency)
        for (int i = 1; i < NUM_TIER_BOUNDARIES; ++i) {
            bridges_[i] = {64, 1000, 1, 64, 1000, 0.0, 1};
        }
    }
}

uint64_t InternalDRAMNetwork::calculateTransferTime(const InternalNetworkLink& link,
                                                    uint64_t data_bytes) {
    // transfer_time = data_bytes / bandwidth_GBs / (1 / freq_GHz)
    //               = data_bytes / bandwidth_GBs * freq_GHz

    // Safety check: prevent division by zero
    if (link.bandwidth_GBs <= 0.0 || link.frequency_GHz <= 0.0) {
        std::cerr << "ERROR: Invalid link parameters - bandwidth: "
                  << link.bandwidth_GBs << " GB/s, frequency: "
                  << link.frequency_GHz << " GHz" << std::endl;
        // Return conservative estimate: 1 cycle per byte
        return std::max(data_bytes, (uint64_t)1);
    }

    double transfer_time_ns = data_bytes / link.bandwidth_GBs;
    uint64_t transfer_cycles = std::ceil(transfer_time_ns * link.frequency_GHz);
    return std::max(transfer_cycles, (uint64_t)1);
}

void InternalDRAMNetwork::processInflightPackets() {
    auto it = inflight_packets_.begin();
    while (it != inflight_packets_.end()) {
        // For GARNET-managed packets, completion_time is 0 until packet arrives
        if (it->completion_time == 0 && use_garnet_models_) {
            // Packet is being managed by GARNET, skip analytical processing
            ++it;
            continue;
        }

        if (current_cycle_ >= it->completion_time && !it->completed) {
            // Packet completed
            it->completed = true;
            total_packets_completed_++;
            total_network_latency_ += (it->completion_time - it->injection_time);

            // Call callback if exists
            if (it->callback) {
                it->callback();
            }

            // Remove from inflight
            it = inflight_packets_.erase(it);
        } else {
            ++it;
        }
    }
}

void InternalDRAMNetwork::processGarnetArrivedPackets() {
    // Helper lambda: map network level index to max_nodes for destination scanning
    auto get_max_nodes = [this](int level_idx) -> uint32_t {
        switch (level_idx) {
            case 0: return static_cast<uint32_t>(num_subarrays_per_bank_);
            case 1: return static_cast<uint32_t>(num_banks_per_bg_);
            case 2: return static_cast<uint32_t>(num_bg_per_chip_);
            case 3: return static_cast<uint32_t>(num_chips_per_rank_);
            case 4: return 2;   // ranks within channel (typical)
            case 5: return 2;   // channels (typical small default)
            case 6: return 1;   // system root
            default: return 0;
        }
    };

    // Process each network level
    for (int i = 0; i < NUM_HIERARCHY_LEVELS; ++i) {
        if (!garnet_networks_[i]) continue;

        auto& network = garnet_networks_[i];
        uint32_t max_nodes = get_max_nodes(i);

        for (uint32_t dst = 0; dst < max_nodes; dst++) {
            while (network->hasArrived(dst)) {
                NetworkPacket arrived = network->extractPacket(dst);

                // Find matching inflight packet and mark as completed
                for (auto& pkt : inflight_packets_) {
                    if (pkt.packet_id == arrived.addr && !pkt.completed && pkt.completion_time == 0) {
                        pkt.completion_time = current_cycle_;
                        pkt.completed = true;
                        total_packets_completed_++;
                        total_network_latency_ += (pkt.completion_time - pkt.injection_time);

                        if (pkt.callback) {
                            pkt.callback();
                        }
                        break;
                    }
                }
            }
        }
    }

    // Process bridge Garnet networks (2-node crossbar, drain at node 1)
    for (int b = 0; b < NUM_TIER_BOUNDARIES; ++b) {
        if (!bridge_garnet_networks_[b]) continue;

        auto& network = bridge_garnet_networks_[b];
        // Bridge destination is always node 1
        while (network->hasArrived(1)) {
            NetworkPacket arrived = network->extractPacket(1);

            for (auto& pkt : inflight_packets_) {
                if (pkt.packet_id == arrived.addr && !pkt.completed && pkt.completion_time == 0) {
                    pkt.completion_time = current_cycle_;
                    pkt.completed = true;
                    total_packets_completed_++;
                    total_network_latency_ += (pkt.completion_time - pkt.injection_time);

                    if (pkt.callback) {
                        pkt.callback();
                    }
                    break;
                }
            }
        }
    }

    // Clean up completed GARNET-managed packets
    inflight_packets_.erase(
        std::remove_if(inflight_packets_.begin(), inflight_packets_.end(),
                      [](const InternalNetworkPacket& pkt) { return pkt.completed; }),
        inflight_packets_.end()
    );
}

//=============================================================================
// Statistics
//=============================================================================

void InternalDRAMNetwork::printStats() const {
    // Build a temporary SwitchHierarchyConfig to get level names
    SwitchHierarchyConfig tmp_cfg;
    tmp_cfg.technology = technology_;

    std::cout << "\n+==================================================================+\n";
    std::cout << "| In-Memory Network Statistics (" << dram_type_ << ")                    |\n";
    std::cout << "+==================================================================+\n\n";

    std::cout << "Total Packets:             " << total_packets_sent_ << "\n";
    std::cout << "Completed Packets:         " << total_packets_completed_ << "\n";
    std::cout << "In-Flight Packets:         " << inflight_packets_.size() << "\n";
    std::cout << "Total Bytes Transferred:   " << total_bytes_transferred_ << " B ("
              << total_bytes_transferred_ / (1024.0 * 1024) << " MB)\n";

    if (total_packets_completed_ > 0) {
        std::cout << "Average Network Latency:   "
                  << static_cast<double>(total_network_latency_) / total_packets_completed_
                  << " cycles\n";
    }

    std::cout << "\nNetwork Level Usage:\n";
    std::cout << "--------------------\n";
    for (int i = 0; i < NUM_HIERARCHY_LEVELS; ++i) {
        std::cout << "  " << std::setw(16) << std::left
                  << (tmp_cfg.levelName(i) + " network:")
                  << " " << network_accesses_[i] << " transfers\n";
    }
    std::cout << "\n";
}

void InternalDRAMNetwork::resetStats() {
    total_packets_sent_ = 0;
    total_packets_completed_ = 0;
    total_bytes_transferred_ = 0;
    total_network_latency_ = 0;
    network_accesses_.fill(0);
    inflight_packets_.clear();
}

//=============================================================================
// Data Movement Operations
//=============================================================================

uint64_t InternalDRAMNetwork::calculateNetworkRequirements(
    int pe_bank, int pe_bg, int pe_chip,
    const std::map<int, uint64_t>& data_distribution,
    std::vector<InternalDRAMTransfer>& transfers) {

    uint64_t total_latency = 0;
    transfers.clear();

    for (const auto& [bank_id, bytes] : data_distribution) {
        if (bank_id == pe_bank) {
            // Local access - no network needed
            continue;
        }

        // Remote access - need network transfer
        InternalDRAMTransfer transfer;
        transfer.source_bank = bank_id;
        transfer.dest_bank = pe_bank;
        transfer.source_subarray = -1; // Not specified
        transfer.dest_subarray = -1;
        transfer.source_bank_group = bank_id / num_banks_per_bg_;
        transfer.dest_bank_group = pe_bg;
        transfer.source_chip = bank_id / (num_banks_per_bg_ * num_bg_per_chip_);
        transfer.dest_chip = pe_chip;
        transfer.transfer_bytes = bytes;
        transfer.requires_network = true;

        // Determine network level and calculate latency
        NetworkLevel level;
        if (transfer.source_bank_group == transfer.dest_bank_group) {
            level = NetworkLevel::BANK_NETWORK;
        } else if (transfer.source_chip == transfer.dest_chip) {
            level = NetworkLevel::BANK_GROUP_NETWORK;
        } else {
            level = NetworkLevel::CHIP_NETWORK;
        }

        uint64_t latency = getTransferLatency(level, bank_id, pe_bank, bytes);
        transfer.network_latency = latency;
        total_latency += latency;

        // Determine locality
        if (transfer.source_bank_group == transfer.dest_bank_group) {
            transfer.locality = DataLocality::REMOTE_SAME_BG;
        } else if (transfer.source_chip == transfer.dest_chip) {
            transfer.locality = DataLocality::REMOTE_SAME_CHIP;
        } else {
            transfer.locality = DataLocality::REMOTE_SAME_RANK;
        }

        transfers.push_back(transfer);
    }

    return total_latency;
}

uint64_t InternalDRAMNetwork::executeGather(
    int pe_bank,
    const std::vector<int>& source_banks,
    uint64_t bytes_per_bank) {

    uint64_t total_latency = 0;

    // Create map of data distribution
    std::map<int, uint64_t> data_dist;
    for (int bank : source_banks) {
        data_dist[bank] = bytes_per_bank;
    }

    // Calculate network requirements
    std::vector<InternalDRAMTransfer> transfers;
    int pe_bg = pe_bank / num_banks_per_bg_;
    int pe_chip = pe_bank / (num_banks_per_bg_ * num_bg_per_chip_);

    total_latency = calculateNetworkRequirements(
        pe_bank, pe_bg, pe_chip, data_dist, transfers);

    return total_latency;
}

uint64_t InternalDRAMNetwork::executeScatter(
    int pe_bank,
    const std::vector<int>& dest_banks,
    uint64_t bytes_per_bank) {

    uint64_t total_latency = 0;

    for (int dest_bank : dest_banks) {
        if (dest_bank == pe_bank) continue; // No transfer needed

        NetworkLevel level;
        int pe_bg = pe_bank / num_banks_per_bg_;
        int dest_bg = dest_bank / num_banks_per_bg_;

        if (pe_bg == dest_bg) {
            level = NetworkLevel::BANK_NETWORK;
        } else {
            level = NetworkLevel::BANK_GROUP_NETWORK;
        }

        uint64_t latency = getTransferLatency(level, pe_bank, dest_bank, bytes_per_bank);
        total_latency += latency;
    }

    return total_latency;
}

uint64_t InternalDRAMNetwork::executeReduce(
    const std::vector<int>& source_banks,
    int dest_bank,
    uint64_t bytes_per_bank) {

    // Similar to gather, but with reduction operation overhead
    uint64_t gather_latency = executeGather(dest_bank, source_banks, bytes_per_bank);

    // Add reduction computation overhead (simplified model)
    uint64_t reduction_overhead = source_banks.size() * 10; // 10 cycles per source

    return gather_latency + reduction_overhead;
}

uint64_t InternalDRAMNetwork::executeBroadcast(
    int source_bank,
    const std::vector<int>& dest_banks,
    uint64_t total_bytes) {

    // Broadcast can potentially use multicast if network supports it
    // For now, model as series of point-to-point transfers
    uint64_t bytes_per_dest = total_bytes; // Full copy to each destination

    return executeScatter(source_bank, dest_banks, bytes_per_dest);
}

double InternalDRAMNetwork::getAvailableBandwidth(NetworkLevel level) const {
    int idx = levelToIndex(level);
    return network_configs_[idx].bandwidth_GBs;
}

bool InternalDRAMNetwork::inSameBankGroup(int bank1, int bank2) const {
    int bg1 = bank1 / num_banks_per_bg_;
    int bg2 = bank2 / num_banks_per_bg_;
    return bg1 == bg2;
}

bool InternalDRAMNetwork::inSameChip(int bank1, int bank2) const {
    int chip1 = bank1 / (num_banks_per_bg_ * num_bg_per_chip_);
    int chip2 = bank2 / (num_banks_per_bg_ * num_bg_per_chip_);
    return chip1 == chip2;
}

//=============================================================================
// GARNET Simulation
//=============================================================================

void InternalDRAMNetwork::enableGarnetSimulation(bool enable) {
    use_garnet_models_ = enable;

    if (enable) {
        std::cout << "\n[InternalDRAMNetwork] Enabling GARNET H-tree simulation" << std::endl;
        std::cout << "  This will provide cycle-accurate NoC modeling with:" << std::endl;
        std::cout << "    - Contention and queuing delays" << std::endl;
        std::cout << "    - Router pipeline simulation" << std::endl;
        std::cout << "    - Accurate power/energy modeling" << std::endl;
        std::cout << "    - Virtual channel flow control\n" << std::endl;

        // Helper: map level index to number of nodes for GARNET topology
        // The number of nodes depends on the DRAM technology and hierarchy
        auto get_nodes_for_level = [this](int level_idx) -> int {
            switch (level_idx) {
                case 0: return num_subarrays_per_bank_;       // L0: subarrays within bank
                case 1: return num_banks_per_bg_;              // L1: banks within BG
                case 2: return num_bg_per_chip_;               // L2: BGs within chip/die layer
                case 3: return num_chips_per_rank_;            // L3: chips within rank / die layers
                case 4: return 2;                              // L4: ranks within channel (typically 1-2)
                case 5: return 2;                              // L5: channels (varies, use small default)
                case 6: return 1;                              // L6: system root (single node)
                default: return 1;
            }
        };

        // Helper: map level index to NetworkLevel enum
        auto index_to_level = [](int idx) -> NetworkLevel {
            switch (idx) {
                case 0: return NetworkLevel::SUBARRAY_NETWORK;
                case 1: return NetworkLevel::BANK_NETWORK;
                case 2: return NetworkLevel::BANK_GROUP_NETWORK;
                case 3: return NetworkLevel::CHIP_NETWORK;
                case 4: return NetworkLevel::RANK_NETWORK;
                case 5: return NetworkLevel::CHANNEL_NETWORK;
                case 6: return NetworkLevel::SYSTEM_NETWORK;
                default: return NetworkLevel::SUBARRAY_NETWORK;
            }
        };

        // Build a temporary config for level names
        SwitchHierarchyConfig tmp_cfg;
        tmp_cfg.technology = technology_;

        // Create GARNET networks for ALL 7 hierarchy levels
        for (int i = 0; i < NUM_HIERARCHY_LEVELS; ++i) {
            int num_nodes = get_nodes_for_level(i);

            // Skip creating GARNET for single-node levels (no routing needed)
            if (num_nodes <= 1) {
                garnet_networks_[i] = nullptr;
                std::cout << "  L" << i << " (" << tmp_cfg.levelName(i)
                          << "): skipped (single node, no routing needed)" << std::endl;
                continue;
            }

            garnet_networks_[i] = createGarnetHTreeForDRAM(
                index_to_level(i),
                num_nodes,
                network_configs_[i].link_width_bits,
                network_configs_[i].latency_cycles,
                network_configs_[i].bandwidth_GBs);
        }

        std::cout << "[InternalDRAMNetwork] GARNET networks created for all "
                  << NUM_HIERARCHY_LEVELS << " hierarchy levels\n" << std::endl;

        // Set all levels to DETAILED
        level_models_.fill(NetworkModelType::DETAILED);
    } else {
        std::cout << "[InternalDRAMNetwork] Using analytical network model (faster)" << std::endl;
        garnet_networks_.fill(nullptr);
        level_models_.fill(NetworkModelType::SIMPLE);
    }
}

void InternalDRAMNetwork::setLevelModel(int level, NetworkModelType model) {
    if (level < 0 || level >= NUM_HIERARCHY_LEVELS) return;

    level_models_[level] = model;

    if (model == NetworkModelType::DETAILED && !garnet_networks_[level]) {
        // Need to create Garnet instance for this level
        auto get_nodes = [this](int idx) -> int {
            switch (idx) {
                case 0: return num_subarrays_per_bank_;
                case 1: return num_banks_per_bg_;
                case 2: return num_bg_per_chip_;
                case 3: return num_chips_per_rank_;
                case 4: return 2;
                case 5: return 2;
                case 6: return 1;
                default: return 1;
            }
        };
        auto index_to_level = [](int idx) -> NetworkLevel {
            switch (idx) {
                case 0: return NetworkLevel::SUBARRAY_NETWORK;
                case 1: return NetworkLevel::BANK_NETWORK;
                case 2: return NetworkLevel::BANK_GROUP_NETWORK;
                case 3: return NetworkLevel::CHIP_NETWORK;
                case 4: return NetworkLevel::RANK_NETWORK;
                case 5: return NetworkLevel::CHANNEL_NETWORK;
                case 6: return NetworkLevel::SYSTEM_NETWORK;
                default: return NetworkLevel::SUBARRAY_NETWORK;
            }
        };

        int num_nodes = get_nodes(level);
        if (num_nodes > 1) {
            garnet_networks_[level] = createGarnetHTreeForDRAM(
                index_to_level(level), num_nodes,
                network_configs_[level].link_width_bits,
                network_configs_[level].latency_cycles,
                network_configs_[level].bandwidth_GBs);
        }
        use_garnet_models_ = true;

        // Initialize bridge buffers at boundaries
        if (level > 0) {
            bridge_buffers_[level - 1].resize(bridges_[level - 1].count);
        }
        if (level < NUM_TIER_BOUNDARIES) {
            bridge_buffers_[level].resize(bridges_[level].count);
        }
    } else if (model == NetworkModelType::SIMPLE) {
        garnet_networks_[level] = nullptr;
        // Check if any level or bridge still uses Garnet
        use_garnet_models_ = false;
        for (int i = 0; i < NUM_HIERARCHY_LEVELS; ++i) {
            if (level_models_[i] == NetworkModelType::DETAILED && garnet_networks_[i]) {
                use_garnet_models_ = true;
                break;
            }
        }
        if (!use_garnet_models_) {
            for (int b = 0; b < NUM_TIER_BOUNDARIES; ++b) {
                if (bridge_garnet_networks_[b]) {
                    use_garnet_models_ = true;
                    break;
                }
            }
        }
    }
}

NetworkModelType InternalDRAMNetwork::getLevelModel(int level) const {
    if (level < 0 || level >= NUM_HIERARCHY_LEVELS) return NetworkModelType::SIMPLE;
    return level_models_[level];
}

void InternalDRAMNetwork::overrideLevelConfig(int level, int link_width_bits,
                                               double frequency_ghz,
                                               int latency_cycles,
                                               const std::string& topology,
                                               int router_latency, int router_pipeline,
                                               int router_bypass, int virtual_networks,
                                               int virtual_channels_per_vn,
                                               int input_buffer_depth,
                                               int output_buffer_depth) {
    if (level < 0 || level >= NUM_HIERARCHY_LEVELS) return;
    auto& cfg = network_configs_[level];
    if (link_width_bits > 0)    cfg.link_width_bits = link_width_bits;
    if (frequency_ghz > 0)      cfg.frequency_GHz = frequency_ghz;
    if (latency_cycles >= 0)    cfg.latency_cycles = latency_cycles;

    // Validate topology against node count at this level
    if (!topology.empty()) {
        int nodes = 1;
        switch (level) {
            case 0: nodes = num_subarrays_per_bank_; break;
            case 1: nodes = num_banks_per_bg_; break;
            case 2: nodes = num_bg_per_chip_; break;
            case 3: nodes = num_chips_per_rank_; break;
            case 4: nodes = 2; break;  // ranks per channel
            case 5: nodes = 2; break;  // channels per system
            case 6: nodes = 1; break;  // system root
        }
        bool valid = true;
        if (topology == "mesh" || topology == "mesh_2d" || topology == "torus" || topology == "torus_2d") {
            // Need at least 4 nodes (2x2) and a perfect square
            int side = static_cast<int>(std::sqrt(nodes));
            if (nodes < 4 || side * side != nodes) {
                std::cerr << "WARNING: L" << level << " topology '" << topology
                          << "' needs a square node count >= 4, but level has "
                          << nodes << " nodes. Keeping default '" << cfg.topology << "'.\n";
                valid = false;
            }
        } else if (topology == "ring") {
            if (nodes < 3) {
                std::cerr << "WARNING: L" << level << " topology '" << topology
                          << "' needs >= 3 nodes, but level has " << nodes
                          << " nodes. Keeping default '" << cfg.topology << "'.\n";
                valid = false;
            }
        } else if (topology == "point-to-point" || topology == "point_to_point") {
            if (nodes != 2) {
                std::cerr << "WARNING: L" << level << " topology '" << topology
                          << "' requires exactly 2 nodes, but level has " << nodes
                          << " nodes. Keeping default '" << cfg.topology << "'.\n";
                valid = false;
            }
        }
        // "bus", "crossbar", "fat_tree", "h_tree" work with any node count >= 1
        if (valid) cfg.topology = topology;
    }

    // Apply router param overrides (negative = keep default)
    if (router_latency >= 0)            cfg.router_latency = router_latency;
    if (router_pipeline >= 0)           cfg.router_pipeline = router_pipeline;
    if (router_bypass >= 0)             cfg.router_bypass = (router_bypass != 0);
    if (virtual_networks > 0)           cfg.virtual_networks = virtual_networks;
    if (virtual_channels_per_vn > 0)    cfg.virtual_channels_per_vn = virtual_channels_per_vn;
    if (input_buffer_depth > 0)         cfg.input_buffer_depth = input_buffer_depth;
    if (output_buffer_depth > 0)        cfg.output_buffer_depth = output_buffer_depth;

    // Recompute bandwidth from (possibly overridden) width x frequency
    cfg.bandwidth_GBs = (cfg.link_width_bits / 8.0) * cfg.frequency_GHz;
}

void InternalDRAMNetwork::setBridgeModel(int boundary, NetworkModelType model) {
    if (boundary < 0 || boundary >= NUM_TIER_BOUNDARIES) return;

    bridges_[boundary].model = model;

    if (model == NetworkModelType::DETAILED && !bridge_garnet_networks_[boundary]) {
        // Create a 2-node CROSSBAR Garnet for this bridge
        bridge_garnet_networks_[boundary] = createGarnetBridgeRouter(boundary, bridges_[boundary]);
        use_garnet_models_ = true;

        // Ensure bridge buffers exist
        bridge_buffers_[boundary].resize(std::max(1, bridges_[boundary].count));
    } else if (model != NetworkModelType::DETAILED) {
        bridge_garnet_networks_[boundary] = nullptr;
        // Check if any level or bridge still uses Garnet
        use_garnet_models_ = false;
        for (int i = 0; i < NUM_HIERARCHY_LEVELS; ++i) {
            if (level_models_[i] == NetworkModelType::DETAILED && garnet_networks_[i]) {
                use_garnet_models_ = true;
                break;
            }
        }
        if (!use_garnet_models_) {
            for (int b = 0; b < NUM_TIER_BOUNDARIES; ++b) {
                if (bridge_garnet_networks_[b]) {
                    use_garnet_models_ = true;
                    break;
                }
            }
        }
    }
}

void InternalDRAMNetwork::overrideBridgeConfig(int boundary, int router_latency,
                                                int router_pipeline, int router_bypass,
                                                int virtual_networks,
                                                int virtual_channels_per_vn,
                                                int input_buffer_depth,
                                                int output_buffer_depth) {
    if (boundary < 0 || boundary >= NUM_TIER_BOUNDARIES) return;
    auto& br = bridges_[boundary];
    if (router_latency >= 0)            br.router_latency = router_latency;
    if (router_pipeline >= 0)           br.router_pipeline = router_pipeline;
    if (router_bypass >= 0)             br.router_bypass = (router_bypass != 0);
    if (virtual_networks > 0)           br.virtual_networks = virtual_networks;
    if (virtual_channels_per_vn > 0)    br.virtual_channels_per_vn = virtual_channels_per_vn;
    if (input_buffer_depth > 0)         br.input_buffer_depth = input_buffer_depth;
    if (output_buffer_depth > 0)        br.output_buffer_depth = output_buffer_depth;
}

std::shared_ptr<NetworkModel> InternalDRAMNetwork::createGarnetBridgeRouter(
    int boundary, const BridgeConfig& br) {

    NetworkConfig config;
    config.topology = NetworkTopology::CROSSBAR;
    config.routing = RoutingAlgorithm::MINIMAL;
    config.flow_control = FlowControl::CREDIT_BASED;
    config.num_rows = 2;
    config.num_cols = 1;
    config.num_layers = 1;
    config.virtual_networks = br.virtual_networks;
    config.virtual_channels_per_vn = br.virtual_channels_per_vn;
    config.virtual_channels = br.virtual_networks * br.virtual_channels_per_vn;
    config.router_pipeline = static_cast<RouterPipelineComplexity>(br.router_pipeline);
    config.router_latency = br.router_latency;
    config.enable_router_bypass = br.router_bypass;
    config.input_buffer_depth = br.input_buffer_depth;
    config.output_buffer_depth = br.output_buffer_depth;
    config.link_width_bytes = std::min(br.lower_width_bits, br.upper_width_bits) / 8;
    config.link_latency = std::max(1, static_cast<int>(std::ceil(br.latency_ns)));

    auto garnet = std::make_shared<GarnetModel>(config);
    garnet->initialize();
    garnet->addNode(NetworkNode(0, PEPlacementLevel::SUBARRAY, 0));
    garnet->addNode(NetworkNode(1, PEPlacementLevel::SUBARRAY, 1));

    std::cout << "[GARNET Bridge] Bridge " << boundary
              << " (L" << boundary << "<->L" << (boundary+1)
              << "): 2-node crossbar, VNs=" << br.virtual_networks
              << ", VCs/VN=" << br.virtual_channels_per_vn
              << ", router_lat=" << br.router_latency << "\n";

    return garnet;
}

//=============================================================================
// Factory Functions (free functions)
//=============================================================================

std::shared_ptr<InternalDRAMNetwork> createInternalDRAMNetwork(
    const std::string& dram_type,
    int num_subarrays_per_bank,
    int num_banks_per_bg,
    int num_bg_per_chip,
    int num_chips_per_rank) {

    auto network = std::make_shared<InternalDRAMNetwork>(dram_type);
    network->initialize(num_subarrays_per_bank, num_banks_per_bg,
                       num_bg_per_chip, num_chips_per_rank);
    return network;
}

std::shared_ptr<NetworkModel> createGarnetHTreeForDRAM(
    NetworkLevel level,
    int num_nodes,
    int link_width_bits,
    int link_latency_cycles,
    double bandwidth_GBs) {

    // ========================================
    // Input Validation
    // ========================================

    // Validate number of nodes
    if (num_nodes <= 0) {
        throw std::invalid_argument("Number of nodes must be positive (got " +
                                   std::to_string(num_nodes) + ")");
    }

    if (num_nodes > 1024) {
        throw std::invalid_argument("Number of nodes exceeds reasonable limit (got " +
                                   std::to_string(num_nodes) + ", max 1024)");
    }

    // Validate link width
    if (link_width_bits <= 0) {
        throw std::invalid_argument("Link width must be positive (got " +
                                   std::to_string(link_width_bits) + " bits)");
    }

    // Link width must be byte-aligned (a multiple of 8 bits) and at least 8 bits.
    // Real DRAM channel/bus widths are NOT necessarily powers of two -- this file
    // deliberately configures 88-, 96-, and 192-bit channel links for various
    // technologies. Requiring a power of two would reject those valid configs and
    // abort the cycle-accurate H-tree at init (simple mode skips this builder).
    if (link_width_bits < 8 || (link_width_bits % 8) != 0) {
        throw std::invalid_argument("Link width must be byte-aligned (multiple of 8) and >= 8 bits (got " +
                                   std::to_string(link_width_bits) + " bits)");
    }

    if (link_width_bits > 1024) {
        throw std::invalid_argument("Link width exceeds reasonable limit (got " +
                                   std::to_string(link_width_bits) + " bits, max 1024)");
    }

    // Validate link latency
    if (link_latency_cycles < 0) {
        throw std::invalid_argument("Link latency cannot be negative (got " +
                                   std::to_string(link_latency_cycles) + " cycles)");
    }

    if (link_latency_cycles > 1000) {
        throw std::invalid_argument("Link latency exceeds reasonable limit (got " +
                                   std::to_string(link_latency_cycles) + " cycles, max 1000)");
    }

    // Validate bandwidth
    if (bandwidth_GBs < 0.0) {
        throw std::invalid_argument("Bandwidth cannot be negative (got " +
                                   std::to_string(bandwidth_GBs) + " GB/s)");
    }

    if (bandwidth_GBs > 10000.0) {
        throw std::invalid_argument("Bandwidth exceeds reasonable limit (got " +
                                   std::to_string(bandwidth_GBs) + " GB/s, max 10000)");
    }

    // Validate bandwidth consistency with link parameters
    if (link_width_bits > 0 && bandwidth_GBs == 0.0) {
        throw std::invalid_argument("Bandwidth is zero despite non-zero link width (" +
                                   std::to_string(link_width_bits) + " bits)");
    }

    std::cout << "[GARNET H-Tree] Creating H-tree network for ";
    switch (level) {
        case NetworkLevel::SUBARRAY_NETWORK:
            std::cout << "SUBARRAY level";
            break;
        case NetworkLevel::BANK_NETWORK:
            std::cout << "BANK level";
            break;
        case NetworkLevel::BANK_GROUP_NETWORK:
            std::cout << "BANK_GROUP level";
            break;
        case NetworkLevel::CHIP_NETWORK:
            std::cout << "CHIP level";
            break;
        case NetworkLevel::RANK_NETWORK:
            std::cout << "RANK level";
            break;
        case NetworkLevel::CHANNEL_NETWORK:
            std::cout << "CHANNEL level";
            break;
        case NetworkLevel::SYSTEM_NETWORK:
            std::cout << "SYSTEM level";
            break;
    }
    std::cout << " (" << num_nodes << " nodes)" << std::endl;

    // Create network configuration for H-tree
    NetworkConfig config;
    config.topology = NetworkTopology::H_TREE;
    config.routing = RoutingAlgorithm::TREE_BASED;
    config.flow_control = FlowControl::CREDIT_BASED;

    // H-tree is a binary tree, so we need log2(num_nodes) levels
    // For simplicity, we'll model it as having num_nodes leaf nodes
    config.num_rows = num_nodes;  // Number of leaf nodes (subarrays/banks)
    config.num_cols = 1;
    config.num_layers = 1;

    // Virtual Networks (VN) and Virtual Channels (VC) for DRAM
    // VN = message classes (read vs write traffic)
    // VC = deadlock avoidance within each VN
    config.virtual_networks = 2;          // VN 0: Reads, VN 1: Writes
    config.virtual_channels_per_vn = 1;   // 1 VC per VN (no deadlock in tree)
    config.virtual_channels = config.virtual_networks * config.virtual_channels_per_vn;

    // Link parameters from DRAM specs
    config.link_width_bytes = link_width_bits / 8;
    config.link_latency = link_latency_cycles;

    // Router pipeline - DRAM uses MINIMAL (just muxes, no complex routing)
    // This is critical for realistic DRAM modeling!
    config.router_pipeline = RouterPipelineComplexity::MINIMAL;
    config.router_latency = 1;            // 1 cycle for mux switching
    config.enable_router_bypass = true;   // Bypass for single-hop

    // Buffer depths - keep small for DRAM (limited buffering)
    // DRAM has minimal buffering (sense amp latches, column latches)
    config.input_buffer_depth = 2;        // Minimal buffering
    config.output_buffer_depth = 2;

    // Create GARNET model
    auto garnet = std::make_shared<GarnetModel>(config);
    garnet->initialize();

    // Build H-tree topology
    // In an H-tree, each leaf node connects to the root through log2(N) intermediate routers
    // For now, we'll create a simplified model where all leaf nodes are connected

    // Add leaf nodes (these represent subarrays/banks)
    for (int i = 0; i < num_nodes; i++) {
        NetworkNode node(i, PEPlacementLevel::SUBARRAY, i);
        garnet->addNode(node);
    }

    std::cout << "[GARNET H-Tree] Configuration:" << std::endl;
    std::cout << "  Leaf nodes: " << num_nodes << std::endl;
    std::cout << "  Link width: " << config.link_width_bytes << " bytes ("
              << link_width_bits << " bits)" << std::endl;
    std::cout << "  Link latency: " << link_latency_cycles << " cycles" << std::endl;
    std::cout << "  Bandwidth: " << bandwidth_GBs << " GB/s" << std::endl;
    std::cout << "  Virtual Networks (VN): " << config.virtual_networks
              << " (VN0=Read, VN1=Write)" << std::endl;
    std::cout << "  Virtual Channels per VN: " << config.virtual_channels_per_vn << std::endl;
    std::cout << "  Total VCs: " << config.virtual_channels << std::endl;
    std::cout << "  Router pipeline: MINIMAL (1-stage mux, lightweight)" << std::endl;
    std::cout << "  Router latency: " << config.router_latency << " cycle" << std::endl;
    std::cout << "  Buffer depth: " << config.input_buffer_depth
              << " (minimal, DRAM-realistic)" << std::endl;

    return garnet;
}

//=============================================================================
// Switch Calculation Functions
//=============================================================================

int InternalDRAMNetwork::getNumberOfSwitchLevels() const {
    /**
     * Network hierarchy based on DRAM organization (7 levels, L0-L6):
     * L0: Subarrays within Bank -> 1 per bank (total_banks)
     * L1: Banks in a BG share one L1 switch -> 1 per BG
     * L2: Each BG has 1 L2 switch connecting all its banks -> 1 per BG
     * L3: Each chip has 1 L3 switch connecting all its BGs -> 1 per chip
     * L4: Each rank has 1 L4 switch connecting all its chips -> 1 per rank
     * L5: Each MC has 1 L5 switch connecting ranks in one channel -> 1 per channel
     * L6: One root switch connecting all channels -> 1 total
     *
     * Total: 7 levels (L0 through L6)
     */
    return 7;
}

int InternalDRAMNetwork::getTotalNumberOfSwitches(int num_channels, int ranks_per_channel) const {
    // Calculate the number of each type of unit in the hierarchy
    int num_ranks = num_channels * ranks_per_channel;
    int num_chips = num_ranks * num_chips_per_rank_;
    int num_bgs = num_chips * num_bg_per_chip_;
    int total_banks = num_bgs * num_banks_per_bg_;

    // Calculate switches at each level
    int l0_switches = total_banks;   // 1 per bank (subarray level)
    int l1_switches = num_bgs;       // 1 per BG (bank level)
    int l2_switches = num_bgs;       // 1 per BG (BG level)
    int l3_switches = num_chips;     // 1 per chip
    int l4_switches = num_ranks;     // 1 per rank
    int l5_switches = num_channels;  // 1 per channel
    int l6_switches = 1;             // 1 root switch

    // Total switches
    int total_switches = l0_switches + l1_switches + l2_switches +
                        l3_switches + l4_switches + l5_switches + l6_switches;

    // Build a temporary config for level names
    SwitchHierarchyConfig tmp_cfg;
    tmp_cfg.technology = technology_;

    std::cout << "[InternalDRAMNetwork] Switch hierarchy calculation:" << std::endl;
    std::cout << "  Configuration:" << std::endl;
    std::cout << "    Channels: " << num_channels << std::endl;
    std::cout << "    Ranks per channel: " << ranks_per_channel << std::endl;
    std::cout << "    Chips per rank: " << num_chips_per_rank_ << std::endl;
    std::cout << "    Bank groups per chip: " << num_bg_per_chip_ << std::endl;
    std::cout << "    Banks per BG: " << num_banks_per_bg_ << std::endl;
    std::cout << std::endl;
    std::cout << "  Hierarchy totals:" << std::endl;
    std::cout << "    Total ranks: " << num_ranks << std::endl;
    std::cout << "    Total chips: " << num_chips << std::endl;
    std::cout << "    Total bank groups: " << num_bgs << std::endl;
    std::cout << "    Total banks: " << total_banks << std::endl;
    std::cout << std::endl;
    std::cout << "  Switch counts by level:" << std::endl;
    std::cout << "    L0 (" << tmp_cfg.levelName(0) << " level): " << l0_switches << " switches" << std::endl;
    std::cout << "    L1 (" << tmp_cfg.levelName(1) << " level): " << l1_switches << " switches" << std::endl;
    std::cout << "    L2 (" << tmp_cfg.levelName(2) << " level): " << l2_switches << " switches" << std::endl;
    std::cout << "    L3 (" << tmp_cfg.levelName(3) << " level): " << l3_switches << " switches" << std::endl;
    std::cout << "    L4 (" << tmp_cfg.levelName(4) << " level): " << l4_switches << " switches" << std::endl;
    std::cout << "    L5 (" << tmp_cfg.levelName(5) << " level): " << l5_switches << " switches" << std::endl;
    std::cout << "    L6 (" << tmp_cfg.levelName(6) << " level): " << l6_switches << " switch" << std::endl;
    std::cout << "    TOTAL: " << total_switches << " switches across "
              << getNumberOfSwitchLevels() << " levels" << std::endl;

    return total_switches;
}

int InternalDRAMNetwork::getNumberOfSwitchesAtLevel(int level, int num_channels, int ranks_per_channel) const {
    if (level < 0 || level > 6) {
        std::cerr << "[InternalDRAMNetwork] ERROR: Invalid switch level " << level
                  << " (valid range: 0-6)" << std::endl;
        return 0;
    }

    // If using custom configuration, return custom values
    if (use_custom_switch_config_) {
        const NetworkLevelConfig& config = custom_switch_config_.levels[level];
        if (!config.use_default) {
            return config.num_switches;
        }
    }

    // Calculate the number of each type of unit in the hierarchy
    int num_ranks = num_channels * ranks_per_channel;
    int num_chips = num_ranks * num_chips_per_rank_;
    int num_bgs = num_chips * num_bg_per_chip_;
    int total_banks = num_bgs * num_banks_per_bg_;

    switch (level) {
        case 0:  // L0: Subarray level (1 per bank)
            return total_banks;
        case 1:  // L1: Bank level (internal to BG)
            return num_bgs;
        case 2:  // L2: BG level (connecting to chip level)
            return num_bgs;
        case 3:  // L3: Chip level
            return num_chips;
        case 4:  // L4: Rank level
            return num_ranks;
        case 5:  // L5: Channel level
            return num_channels;
        case 6:  // L6: System level (root)
            return 1;
        default:
            return 0;
    }
}

//=============================================================================
// Custom Topology Configuration Functions
//=============================================================================

void InternalDRAMNetwork::setCustomSwitchHierarchy(const SwitchHierarchyConfig& config) {
    custom_switch_config_ = config;
    use_custom_switch_config_ = true;

    std::cout << "[InternalDRAMNetwork] Custom switch hierarchy configured:" << std::endl;

    for (int i = 0; i < NUM_HIERARCHY_LEVELS; ++i) {
        const auto& cfg = config.levels[i];
        if (!cfg.use_default) {
            std::cout << "  L" << i << " (" << config.levelName(i) << "):" << std::endl;
            std::cout << "    Topology: " << getTopologyName(cfg.topology) << std::endl;
            std::cout << "    Switches: " << cfg.num_switches << std::endl;
            std::cout << "    Ports per switch: " << cfg.ports_per_switch << std::endl;
            std::cout << "    Endpoints: " << cfg.num_endpoints << std::endl;
        }
    }
}

SwitchHierarchyConfig InternalDRAMNetwork::getSwitchHierarchyConfig(int num_channels, int ranks_per_channel) const {
    SwitchHierarchyConfig config;
    config.technology = technology_;
    config.num_channels = num_channels;
    config.ranks_per_channel = ranks_per_channel;

    // Calculate hierarchy
    int num_ranks = num_channels * ranks_per_channel;
    int num_chips = num_ranks * num_chips_per_rank_;
    int num_bgs = num_chips * num_bg_per_chip_;
    int num_banks = num_bgs * num_banks_per_bg_;

    // L0: Subarray (subarrays within bank)
    config.levels[0].level = 0;
    config.levels[0].topology = TopologyType::CROSSBAR;
    config.levels[0].num_switches = num_banks;
    config.levels[0].num_endpoints = num_subarrays_per_bank_;
    config.levels[0].ports_per_switch = calculatePortsPerSwitch(
        config.levels[0].topology, config.levels[0].num_endpoints, 1);

    // L1: Bank (banks within BG)
    config.levels[1].level = 1;
    config.levels[1].topology = TopologyType::CROSSBAR;
    config.levels[1].num_switches = num_bgs;
    config.levels[1].num_endpoints = num_banks_per_bg_;
    config.levels[1].ports_per_switch = calculatePortsPerSwitch(
        config.levels[1].topology, config.levels[1].num_endpoints, 1);

    // L2: BG (BGs within chip)
    config.levels[2].level = 2;
    config.levels[2].topology = TopologyType::BUS;
    config.levels[2].num_switches = num_bgs;
    config.levels[2].num_endpoints = num_bg_per_chip_;
    config.levels[2].ports_per_switch = calculatePortsPerSwitch(
        config.levels[2].topology, config.levels[2].num_endpoints, 1);

    // L3: Chip (chips within rank)
    config.levels[3].level = 3;
    config.levels[3].topology = TopologyType::BUS;
    config.levels[3].num_switches = num_chips;
    config.levels[3].num_endpoints = num_chips_per_rank_;
    config.levels[3].ports_per_switch = calculatePortsPerSwitch(
        config.levels[3].topology, config.levels[3].num_endpoints, 1);

    // L4: Rank (ranks within channel)
    config.levels[4].level = 4;
    config.levels[4].topology = TopologyType::BUS;
    config.levels[4].num_switches = num_ranks;
    config.levels[4].num_endpoints = ranks_per_channel;
    config.levels[4].ports_per_switch = calculatePortsPerSwitch(
        config.levels[4].topology, config.levels[4].num_endpoints, 1);

    // L5: Channel (channels to system)
    config.levels[5].level = 5;
    config.levels[5].topology = TopologyType::CROSSBAR;
    config.levels[5].num_switches = num_channels;
    config.levels[5].num_endpoints = num_channels;
    config.levels[5].ports_per_switch = calculatePortsPerSwitch(
        config.levels[5].topology, config.levels[5].num_endpoints, 1);

    // L6: System level (root switch)
    config.levels[6].level = 6;
    config.levels[6].topology = TopologyType::CROSSBAR;
    config.levels[6].num_switches = 1;
    config.levels[6].num_endpoints = num_channels;
    config.levels[6].ports_per_switch = calculatePortsPerSwitch(
        config.levels[6].topology, config.levels[6].num_endpoints, 1);

    // If custom config exists, overlay it
    if (use_custom_switch_config_) {
        for (int i = 0; i < NUM_HIERARCHY_LEVELS; ++i) {
            if (!custom_switch_config_.levels[i].use_default) {
                config.levels[i] = custom_switch_config_.levels[i];
            }
        }
    }

    return config;
}

int InternalDRAMNetwork::calculatePortsPerSwitch(TopologyType topology, int num_endpoints, int num_switches) {
    if (num_switches == 0 || num_endpoints == 0) {
        return 0;
    }

    switch (topology) {
        case TopologyType::BUS:
            // Bus: 1 switch connects all endpoints
            return num_endpoints;

        case TopologyType::CROSSBAR:
            // Crossbar: N*N connections
            return num_endpoints;

        case TopologyType::MESH_2D: {
            // 2D Mesh: Each switch has ~4-5 ports (4 directions + local)
            // Total switches = sqrt(N) * sqrt(N)
            int sqrt_n = static_cast<int>(std::ceil(std::sqrt(num_endpoints / num_switches)));
            (void)sqrt_n; // suppress unused warning
            return 5;  // N, S, E, W, Local
        }

        case TopologyType::TORUS_2D: {
            // 2D Torus: Same as mesh but wrap-around
            return 5;  // N, S, E, W, Local
        }

        case TopologyType::FAT_TREE: {
            // Fat-tree: k-port switches, k/2 up, k/2 down
            // For simplicity, assume balanced tree
            int endpoints_per_switch = (num_endpoints + num_switches - 1) / num_switches;
            return endpoints_per_switch + 2;  // Endpoints + up/down links
        }

        case TopologyType::H_TREE: {
            // H-tree: Binary tree, log2(N) levels
            // Each router has 2-4 ports depending on position
            return 4;  // Parent + 2 children + local (average)
        }

        case TopologyType::CUSTOM:
            // User must specify
            return 0;

        default:
            return num_endpoints;
    }
}

int InternalDRAMNetwork::calculateOptimalSwitchCount(int num_endpoints, int max_ports_per_switch, TopologyType topology) {
    if (num_endpoints == 0 || max_ports_per_switch == 0) {
        return 1;
    }

    switch (topology) {
        case TopologyType::BUS:
        case TopologyType::CROSSBAR:
            // For bus/crossbar, calculate how many switches needed
            // to keep each under the port limit
            return (num_endpoints + max_ports_per_switch - 1) / max_ports_per_switch;

        case TopologyType::MESH_2D:
        case TopologyType::TORUS_2D: {
            // Mesh/Torus: sqrt(N) * sqrt(N) switches
            int sqrt_n = static_cast<int>(std::ceil(std::sqrt(num_endpoints)));
            return sqrt_n * sqrt_n;
        }

        case TopologyType::FAT_TREE: {
            // Fat-tree with k-port switches
            // Number of switches ~= 5N/k for k-ary fat tree
            int k = max_ports_per_switch;
            return (5 * num_endpoints + k - 1) / k;
        }

        case TopologyType::H_TREE: {
            // H-tree: Binary tree requires (N-1) internal nodes for N leaves
            return num_endpoints - 1;
        }

        case TopologyType::CUSTOM:
            // Must be specified by user
            return 1;

        default:
            return 1;
    }
}

std::string InternalDRAMNetwork::getTopologyName(TopologyType topology) {
    switch (topology) {
        case TopologyType::BUS:        return "Bus";
        case TopologyType::CROSSBAR:   return "Crossbar";
        case TopologyType::MESH_2D:    return "2D Mesh";
        case TopologyType::TORUS_2D:   return "2D Torus";
        case TopologyType::FAT_TREE:   return "Fat-Tree";
        case TopologyType::H_TREE:     return "H-Tree";
        case TopologyType::CUSTOM:     return "Custom";
        default:                        return "Unknown";
    }
}

//=============================================================================
// Factory Functions for Network Creation at Different Hierarchy Levels
//=============================================================================

namespace {
    // Helper to get network parameters based on DRAM type
    struct NetworkParams {
        int link_width_bits;
        int link_latency_cycles;
        double bandwidth_GBs;
    };

    NetworkParams getSubarrayParams(const std::string& dram_type) {
        if (dram_type == "HBM3") return {512, 3, 115.2};
        if (dram_type == "HBM2") return {256, 3, 32.0};
        if (dram_type == "DDR5") return {128, 5, 25.6};
        if (dram_type == "GDDR6") return {256, 4, 64.0};
        if (dram_type == "LPDDR5") return {128, 4, 25.6};
        if (dram_type == "SRAM") return {128, 1, 40.0};
        if (dram_type == "STT-MRAM" || dram_type == "STTMRAM") return {64, 3, 12.0};
        if (dram_type == "PCM" || dram_type == "PRAM") return {64, 4, 9.6};
        if (dram_type == "ReRAM" || dram_type == "RERAM") return {64, 3, 11.2};
        // Default: DDR4
        return {64, 5, 9.6};
    }

    NetworkParams getBankParams(const std::string& dram_type) {
        if (dram_type == "HBM3") return {128, 5, 28.8};
        if (dram_type == "HBM2") return {64, 5, 8.0};
        if (dram_type == "DDR5") return {16, 10, 3.2};
        if (dram_type == "GDDR6") return {32, 6, 8.0};
        if (dram_type == "LPDDR5") return {16, 8, 3.2};
        if (dram_type == "SRAM") return {64, 2, 20.0};
        if (dram_type == "STT-MRAM" || dram_type == "STTMRAM") return {16, 6, 3.0};
        if (dram_type == "PCM" || dram_type == "PRAM") return {16, 8, 2.4};
        if (dram_type == "ReRAM" || dram_type == "RERAM") return {16, 7, 2.8};
        // Default: DDR4
        return {8, 10, 1.2};
    }

    NetworkParams getBankGroupParams(const std::string& dram_type) {
        if (dram_type == "HBM3") return {256, 8, 57.6};
        if (dram_type == "HBM2") return {128, 8, 16.0};
        if (dram_type == "DDR5") return {32, 20, 6.4};
        if (dram_type == "GDDR6") return {64, 10, 16.0};
        if (dram_type == "LPDDR5") return {32, 15, 6.4};
        if (dram_type == "SRAM") return {64, 3, 20.0};
        if (dram_type == "STT-MRAM" || dram_type == "STTMRAM") return {32, 12, 6.0};
        if (dram_type == "PCM" || dram_type == "PRAM") return {32, 16, 4.8};
        if (dram_type == "ReRAM" || dram_type == "RERAM") return {32, 14, 5.6};
        // Default: DDR4
        return {16, 20, 2.4};
    }

    NetworkParams getChipParams(const std::string& dram_type) {
        if (dram_type == "HBM3") return {128, 10, 28.8};
        if (dram_type == "HBM2") return {128, 10, 16.0};
        if (dram_type == "DDR5") return {8, 50, 1.6};
        if (dram_type == "GDDR6") return {16, 20, 4.0};
        if (dram_type == "LPDDR5") return {16, 30, 3.2};
        if (dram_type == "SRAM") return {32, 4, 10.0};
        if (dram_type == "STT-MRAM" || dram_type == "STTMRAM") return {16, 25, 3.0};
        if (dram_type == "PCM" || dram_type == "PRAM") return {16, 32, 2.4};
        if (dram_type == "ReRAM" || dram_type == "RERAM") return {16, 28, 2.8};
        // Default: DDR4
        return {8, 50, 1.2};
    }
}

std::shared_ptr<NetworkModel> createSubarrayNetwork(
    const std::string& dram_type,
    int num_subarrays,
    bool use_garnet) {

    if (use_garnet) {
        auto params = getSubarrayParams(dram_type);
        return createGarnetHTreeForDRAM(
            NetworkLevel::SUBARRAY_NETWORK,
            num_subarrays,
            params.link_width_bits,
            params.link_latency_cycles,
            params.bandwidth_GBs
        );
    }

    // Create simple analytical model via GarnetModel with H-tree topology
    NetworkConfig config;
    config.topology = NetworkTopology::H_TREE;
    config.routing = RoutingAlgorithm::TREE_BASED;
    config.num_rows = num_subarrays;
    config.num_cols = 1;
    config.router_pipeline = RouterPipelineComplexity::MINIMAL;
    config.router_latency = 1;

    auto params = getSubarrayParams(dram_type);
    config.link_width_bytes = params.link_width_bits / 8;
    config.link_latency = params.link_latency_cycles;

    auto model = std::make_shared<GarnetModel>(config);
    model->initialize();
    return model;
}

std::shared_ptr<NetworkModel> createBankNetwork(
    const std::string& dram_type,
    int num_banks,
    bool use_garnet) {

    if (use_garnet) {
        auto params = getBankParams(dram_type);
        return createGarnetHTreeForDRAM(
            NetworkLevel::BANK_NETWORK,
            num_banks,
            params.link_width_bits,
            params.link_latency_cycles,
            params.bandwidth_GBs
        );
    }

    // Create simple model
    NetworkConfig config;
    config.topology = NetworkTopology::CROSSBAR;  // Banks often use bus/crossbar
    config.routing = RoutingAlgorithm::MINIMAL;
    config.num_rows = num_banks;
    config.num_cols = 1;
    config.router_pipeline = RouterPipelineComplexity::SIMPLE;
    config.router_latency = 2;

    auto params = getBankParams(dram_type);
    config.link_width_bytes = params.link_width_bits / 8;
    config.link_latency = params.link_latency_cycles;

    auto model = std::make_shared<GarnetModel>(config);
    model->initialize();
    return model;
}

std::shared_ptr<NetworkModel> createBankGroupNetwork(
    const std::string& dram_type,
    int num_bank_groups,
    bool use_garnet) {

    if (use_garnet) {
        auto params = getBankGroupParams(dram_type);
        return createGarnetHTreeForDRAM(
            NetworkLevel::BANK_GROUP_NETWORK,
            num_bank_groups,
            params.link_width_bits,
            params.link_latency_cycles,
            params.bandwidth_GBs
        );
    }

    // Create simple model
    NetworkConfig config;
    config.topology = NetworkTopology::CROSSBAR;
    config.routing = RoutingAlgorithm::MINIMAL;
    config.num_rows = num_bank_groups;
    config.num_cols = 1;
    config.router_pipeline = RouterPipelineComplexity::SIMPLE;
    config.router_latency = 2;

    auto params = getBankGroupParams(dram_type);
    config.link_width_bytes = params.link_width_bits / 8;
    config.link_latency = params.link_latency_cycles;

    auto model = std::make_shared<GarnetModel>(config);
    model->initialize();
    return model;
}

std::shared_ptr<NetworkModel> createChipNetwork(
    const std::string& dram_type,
    int num_chips,
    bool use_garnet) {

    if (use_garnet) {
        auto params = getChipParams(dram_type);
        return createGarnetHTreeForDRAM(
            NetworkLevel::CHIP_NETWORK,
            num_chips,
            params.link_width_bits,
            params.link_latency_cycles,
            params.bandwidth_GBs
        );
    }

    // Create simple model - chips typically use point-to-point or crossbar
    NetworkConfig config;
    config.topology = NetworkTopology::CROSSBAR;
    config.routing = RoutingAlgorithm::MINIMAL;
    config.num_rows = num_chips;
    config.num_cols = 1;
    config.router_pipeline = RouterPipelineComplexity::REDUCED;
    config.router_latency = 3;

    auto params = getChipParams(dram_type);
    config.link_width_bytes = params.link_width_bits / 8;
    config.link_latency = params.link_latency_cycles;

    auto model = std::make_shared<GarnetModel>(config);
    model->initialize();
    return model;
}

std::shared_ptr<InternalDRAMNetwork> createHierarchicalNetwork(
    const std::string& dram_type,
    int num_subarrays_per_bank,
    int num_banks_per_bg,
    int num_bg_per_chip,
    int num_chips_per_rank,
    bool use_garnet) {

    std::cout << "\n[Factory] Creating hierarchical network for " << dram_type << ":" << std::endl;
    std::cout << "  Subarrays per bank: " << num_subarrays_per_bank << std::endl;
    std::cout << "  Banks per BG: " << num_banks_per_bg << std::endl;
    std::cout << "  BGs per chip: " << num_bg_per_chip << std::endl;
    std::cout << "  Chips per rank: " << num_chips_per_rank << std::endl;
    std::cout << "  GARNET mode: " << (use_garnet ? "enabled (cycle-accurate)" : "disabled (analytical)") << std::endl;

    auto network = std::make_shared<InternalDRAMNetwork>(dram_type);
    network->initialize(num_subarrays_per_bank, num_banks_per_bg,
                       num_bg_per_chip, num_chips_per_rank);

    if (use_garnet) {
        network->enableGarnetSimulation(true);
    }

    std::cout << "[Factory] Hierarchical network created successfully\n" << std::endl;
    return network;
}

} // namespace pimid
