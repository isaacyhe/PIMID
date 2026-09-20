/**
 * @file internal_dram_network.h
 * @brief In-Memory Network Model for PIM Data Movement
 *
 * This file models the network INSIDE a DRAM chip that enables data movement
 * between subarrays, banks, and bank groups when PIM units are placed at
 * fine granularity levels.
 *
 * CRITICAL REQUIREMENT:
 * "bank-wide, subarray-wide, bg-wide and chip-wide PE do always, I mean always,
 *  need our network model to enable data communication within."
 *
 * ARCHITECTURE:
 * - Subarray-to-Subarray: Needs network within bank
 * - Bank-to-Bank: Needs network within bank group or chip
 * - BankGroup-to-BankGroup: Needs network within chip
 * - Chip-to-Chip: Needs network within rank (via TSI for HBM, interposer, etc.)
 *
 * NETWORK TOPOLOGY:
 * We use a hierarchical network matching DRAM hierarchy (7 levels, L0-L6):
 *
 *  DDR4/DDR5:
 *   L0: subarray   -- Subarrays within Bank
 *   L1: bank       -- Banks within Bank Group
 *   L2: bank_group -- Bank Groups within Chip
 *   L3: chip       -- Chips within Rank (PCB traces, narrow, slow)
 *   L4: rank       -- Ranks within Channel
 *   L5: channel    -- Channels to Memory Controller
 *   L6: system     -- System root
 *
 *  HBM2/HBM3:
 *   L0: subarray   -- Subarrays within Bank
 *   L1: bank       -- Banks within Bank Group
 *   L2: bank_group -- Bank Groups within Die Layer
 *   L3: die_layer  -- DRAM Die Layers within Stack (TSVs, wide, fast)
 *   L4: logic_die  -- Logic Die <-> DRAM Dies (TSV-based)
 *   L5: channel    -- Pseudo-channels within Stack
 *   L6: system     -- System root (multi-stack, interposer)
 */

#ifndef PIMID_INTERNAL_DRAM_NETWORK_H
#define PIMID_INTERNAL_DRAM_NETWORK_H

#include <vector>
#include <queue>
#include <memory>
#include <string>
#include <array>
#include "memory/pim_request_payload.h"
#include "network/network_model.h"

namespace pimid {

/** Number of hierarchy levels in the switch network (L0-L6) */
static constexpr int NUM_HIERARCHY_LEVELS = 7;

// MemoryTechnology enum is defined in common/types.h (included via network_model.h)

/**
 * @brief Named level indices for DDR4/DDR5 hierarchy
 */
enum class DDRLevel : int {
    SUBARRAY   = 0,  // Subarrays within Bank
    BANK       = 1,  // Banks within Bank Group
    BANK_GROUP = 2,  // Bank Groups within Chip
    CHIP       = 3,  // Chips within Rank (PCB traces, narrow, slow)
    RANK       = 4,  // Ranks within Channel
    CHANNEL    = 5,  // Channels to Memory Controller
    SYSTEM     = 6   // System root
};

/**
 * @brief Named level indices for HBM2/HBM3 hierarchy
 */
enum class HBMLevel : int {
    SUBARRAY   = 0,  // Subarrays within Bank
    BANK       = 1,  // Banks within Bank Group
    BANK_GROUP = 2,  // Bank Groups within Die Layer
    DIE_LAYER  = 3,  // DRAM Die Layers within Stack (TSVs, wide, fast)
    LOGIC_DIE  = 4,  // Logic Die <-> DRAM Dies (TSV-based)
    CHANNEL    = 5,  // Pseudo-channels within Stack
    SYSTEM     = 6   // System root (multi-stack, interposer)
};

/**
 * @brief Named level indices for NVM/SRAM hierarchy
 *
 * NVM and SRAM are monolithic -- only L0 (subarray) and L1 (bank) are active.
 * L2-L6 exist in the 7-level infrastructure but default to passthrough.
 */
enum class NVMLevel : int {
    SUBARRAY   = 0,  // Subarrays within Bank
    BANK       = 1   // Banks within chip (L2-L6 passthrough)
};

/**
 * @brief Network Link Configuration inside DRAM
 */
struct InternalNetworkLink {
    // Link parameters
    int link_width_bits = 64;       // Link width (e.g., 8, 16, 32, 64 bits)
    double frequency_GHz = 1.0;     // Operating frequency
    double bandwidth_GBs = 8.0;     // Effective bandwidth
    int latency_cycles = 2;         // Per-hop link latency (cycles)

    // Routing information
    int source_id = 0;              // Source unit (bank/subarray ID)
    int dest_id = 0;                // Destination unit
    std::string topology = "crossbar"; // "bus", "crossbar", "mesh_2d", "torus_2d",
                                       // "ring", "fat_tree", "h_tree", "point-to-point"

    // Router parameters (used by both models: SIMPLE, DETAILED)
    int router_latency = 1;           // Per-hop router pipeline latency (cycles)
    int router_pipeline = 3;          // 0=FULL(4), 1=REDUCED(3), 2=SIMPLE(2), 3=MINIMAL(1)
    bool router_bypass = true;        // Bypass for single-hop (reduces latency by 1)
    int virtual_networks = 2;         // VNs (message classes: read + write)
    int virtual_channels_per_vn = 1;  // VCs per VN
    int input_buffer_depth = 2;       // Input VC buffer entries (flits)
    int output_buffer_depth = 2;      // Output VC buffer entries (flits)
    int num_nodes = 2;                // Endpoints at this level (for hop count)
};

/**
 * @brief Network Packet for internal DRAM transfers
 */
struct InternalNetworkPacket {
    uint64_t packet_id;
    int source_bank;
    int dest_bank;
    int source_subarray;
    int dest_subarray;
    uint64_t data_bytes;
    uint64_t injection_time;   // When packet entered network
    uint64_t completion_time;  // When packet left network
    bool completed;

    // Callback when packet completes
    std::function<void()> callback;
};

/**
 * @brief Hierarchical address for multi-tier routing
 *
 * Each field corresponds to a hierarchy level. Packets carry this address
 * and each tier's router reads its relevant field.
 */
struct HierarchicalAddress {
    int subarray = 0;    // L0
    int bank = 0;        // L1
    int bank_group = 0;  // L2
    int chip = 0;        // L3 (or die_layer for HBM)
    int rank = 0;        // L4 (or logic_die for HBM)
    int channel = 0;     // L5
    int system = 0;      // L6 (always 0)
};

/** Number of tier boundaries (between adjacent levels) */
static constexpr int NUM_TIER_BOUNDARIES = NUM_HIERARCHY_LEVELS - 1;  // 6

/**
 * @brief Simulation model type for a network level or bridge
 *
 * Two network models:
 *   SIMPLE:   Topology-aware hop count + M/D/1 queuing contention
 *   DETAILED: Cycle-accurate Garnet simulation
 *   AUTO:     Bridge-only: auto-derive from adjacent tier networks
 *
 * "md1" and "analytical" are accepted as backward-compat aliases for SIMPLE.
 */
enum class NetworkModelType {
    SIMPLE,      // Topology-aware hop count + M/D/1 queuing (VCs as parallel servers)
    DETAILED,    // Cycle-accurate Garnet simulation
    AUTO         // Auto-derive from adjacent tiers (bridges only)
};

/**
 * @brief Bridge configuration for a tier boundary (FIFO + dual-link model)
 *
 * Bridges handle tier crossings between adjacent hierarchy levels.
 * Bridge[i] connects Tier[i] (lower) <-> Tier[i+1] (upper).
 *
 * Physical model:
 *   [lower-tier] --[ingress link]--> [FIFO] --[egress link]--> [upper-tier]
 *
 * Ingress/egress links can have different widths and frequencies,
 * reflecting real cross-tier constraints (e.g., wide internal bus into
 * narrow DQ pins, TSV-to-interposer transitions).
 */
struct BridgeConfig {
    int lower_width_bits = 128;         // Ingress link width (from lower tier)
    double lower_frequency_mhz = 800.0; // Ingress link operating frequency
    int fifo_depth = 8;                 // Buffer depth (entries)
    int upper_width_bits = 64;          // Egress link width (to upper tier)
    double upper_frequency_mhz = 1600.0;// Egress link operating frequency
    double latency_ns = 0.5;            // Base propagation + setup latency
    int count = 1;                      // Parallel bridges at this boundary

    // Independent bridge model (AUTO = derive from adjacent tiers)
    NetworkModelType model = NetworkModelType::AUTO;

    // Router parameters (shared by all 3 models)
    int router_latency = 1;
    int router_pipeline = 3;              // 0=FULL, 1=REDUCED, 2=SIMPLE, 3=MINIMAL
    bool router_bypass = true;
    int virtual_networks = 2;
    int virtual_channels_per_vn = 1;
    int input_buffer_depth = 2;
    int output_buffer_depth = 2;
};

/**
 * @brief Hierarchical Network Level (7 levels, L0-L6)
 *
 * The physical meaning of each level depends on the technology:
 *   DDR: SUBARRAY, BANK, BANK_GROUP, CHIP, RANK, CHANNEL, SYSTEM
 *   HBM: SUBARRAY, BANK, BANK_GROUP, DIE_LAYER, LOGIC_DIE, CHANNEL, SYSTEM
 */
enum class NetworkLevel {
    SUBARRAY_NETWORK,      // L0: Within-bank network (subarray-to-subarray)
    BANK_NETWORK,          // L1: Within-bankgroup network (bank-to-bank)
    BANK_GROUP_NETWORK,    // L2: Within-chip network (bankgroup-to-bankgroup)
    CHIP_NETWORK,          // L3: Within-rank network (chip-to-chip / die layer)
    RANK_NETWORK,          // L4: Within-channel network (rank-to-rank / logic die)
    CHANNEL_NETWORK,       // L5: Channel-to-MC / pseudo-channels
    SYSTEM_NETWORK         // L6: System root
};

/**
 * @brief Topology type for network interconnect
 */
enum class TopologyType {
    BUS,           // Shared bus (1 switch, N ports)
    CROSSBAR,      // Full crossbar (1 switch, N^2 complexity)
    MESH_2D,       // 2D mesh (sqrt(N) x sqrt(N) grid)
    TORUS_2D,      // 2D torus (wrap-around mesh)
    FAT_TREE,      // Fat-tree (hierarchical with increasing BW upward)
    H_TREE,        // H-tree (binary tree structure)
    CUSTOM         // User-defined topology
};

/**
 * @brief Configuration for a specific network level
 */
struct NetworkLevelConfig {
    int level;                    // Switch level (0-6)
    TopologyType topology;        // Topology type for this level
    int num_switches;             // Number of switches at this level
    int ports_per_switch;         // Number of ports per switch
    int num_endpoints;            // Number of endpoints connected at this level
    bool use_default;             // True to use auto-calculated values
    NetworkModelType model;       // Simulation model for this level

    NetworkLevelConfig()
        : level(0), topology(TopologyType::CROSSBAR),
          num_switches(1), ports_per_switch(0),
          num_endpoints(0), use_default(true),
          model(NetworkModelType::SIMPLE) {}
};

/**
 * @brief Complete switch hierarchy configuration for DRAM internal networks.
 *
 * 7 switch levels (L0-L6) map to the DRAM hierarchy.
 * The physical meaning depends on the memory technology:
 *
 *  Level | DDR4/DDR5 (planar)             | HBM2/HBM3 (3D-stacked)
 *  ------+--------------------------------+----------------------------------
 *   L0   | Subarrays within Bank          | Subarrays within Bank
 *   L1   | Banks within Bank Group        | Banks within Bank Group
 *   L2   | Bank Groups within Chip        | Bank Groups within Die Layer
 *   L3   | Chips within Rank (PCB)        | Die Layers within Stack (TSV)
 *   L4   | Ranks within Channel           | Logic Die <-> DRAM Dies (TSV)
 *   L5   | Channels to MC                 | Channels within Stack
 *   L6   | System root                    | System root (multi-stack)
 *
 * Key physical differences:
 *  - DDR4 L3 (chip-to-chip) uses narrow PCB traces (8-bit, ~50-cycle latency)
 *  - HBM  L3 (die-to-die)  uses wide TSVs (128-bit, ~10-cycle latency)
 *  - HBM  L4 models the logic die, which has no DDR4 equivalent
 */
struct SwitchHierarchyConfig {
    // Per-level configurations indexed by level number (0-6)
    std::array<NetworkLevelConfig, NUM_HIERARCHY_LEVELS> levels;

    // DRAM technology for this hierarchy
    MemoryTechnology technology;

    // Global settings
    int num_channels;
    int ranks_per_channel;

    SwitchHierarchyConfig()
        : technology(MemoryTechnology::DDR4),
          num_channels(1), ranks_per_channel(1) {
        for (int i = 0; i < NUM_HIERARCHY_LEVELS; ++i) {
            levels[i].level = i;
        }
    }

    /**
     * @brief Get human-readable name for a level index based on technology
     */
    std::string levelName(int idx) const;
};

/**
 * @brief Parse a DRAM type string into MemoryTechnology enum
 */
MemoryTechnology parseMemoryTechnology(const std::string& dram_type);

/**
 * @brief Check if a MemoryTechnology is an HBM variant
 */
bool isHBM(MemoryTechnology tech);

/**
 * @brief Check if a MemoryTechnology is a DRAM variant (DDR/LPDDR/GDDR/HBM)
 */
bool isDRAM(MemoryTechnology tech);

/**
 * @brief In-Memory Network Model
 *
 * This models the network inside memory chips that enables PIM data movement.
 * Each DRAM hierarchy level has its own network with different topology and BW.
 */
class InternalDRAMNetwork {
public:
    /**
     * @brief Constructor
     * @param dram_type DRAM type ("DDR4", "DDR5", "HBM2", "HBM3")
     * @param network_model Optional external network model for detailed simulation
     */
    InternalDRAMNetwork(const std::string& dram_type,
                       std::shared_ptr<NetworkModel> network_model = nullptr);

    ~InternalDRAMNetwork() = default;

    /**
     * @brief Initialize the internal network based on DRAM architecture
     */
    void initialize(int num_subarrays_per_bank,
                   int num_banks_per_bg,
                   int num_bg_per_chip,
                   int num_chips_per_rank);

    /**
     * @brief Send a packet through the internal network
     * @return true if packet accepted, false if network congested
     */
    bool sendPacket(const InternalNetworkPacket& packet);

    /**
     * @brief Tick the network (advance by one cycle)
     */
    void tick();

    /**
     * @brief Get network latency for a transfer
     * @param level Network level
     * @param source_id Source unit ID
     * @param dest_id Destination unit ID
     * @param data_bytes Amount of data to transfer
     * @return Latency in cycles
     */
    uint64_t getTransferLatency(NetworkLevel level,
                               int source_id, int dest_id,
                               uint64_t data_bytes);

    /**
     * @brief Same traversal, expressed in NANOSECONDS.
     *
     * 1.11.56 (audit B051): every level carries its own frequency_GHz, and
     * DQ-side levels run at the data rate while the array side runs at the
     * core clock. getTransferLatency() returns cycles OF THAT LEVEL, and the
     * caller sums seven of them plus six bridge results into one integer it
     * then hands to the timing model as PE cycles. That sum is only
     * meaningful if every term is in the same clock, which they are not.
     * This returns time, so the caller can convert once, at the PE clock.
     */
    double getTransferLatencyNs(NetworkLevel level,
                                int source_id, int dest_id,
                                uint64_t data_bytes);

    /**
     * @brief Bridge crossing time in NANOSECONDS (audit B051).
     *
     * A bridge spans two clock domains. The crossing is store-and-forward,
     * so the serialisation cost is the SLOWER of ingest and egress, and the
     * router pipeline is charged in the clock of the side it sits on.
     * Pass -1 for any override to take the bridge's own value.
     */
    double getBridgeLatencyNs(int boundary, uint64_t data_bytes,
                              int lower_width_override = -1,
                              int upper_width_override = -1,
                              double base_ns_override = -1.0,
                              int router_latency_override = -1,
                              int router_bypass_override = -1) const;

    /**
     * @brief Replace the per-level link widths and bandwidths with the ones
     *        the DRAM architecture object reports (audit D064/D065).
     *
     * Widths are in bits at each tier's own port; bandwidths are GB/s. The
     * level frequency is back-derived so width x frequency reproduces the
     * given bandwidth exactly, which keeps the DQ-side levels at the data
     * rate and the array side at the core clock without this class needing
     * to know which is which. A non-positive entry leaves that level alone.
     */
    /* 1.11.57 (audit C005): this also re-derives every tier bridge's ingress
     * and egress link from the two levels it joins. A bridge's two links ARE
     * those levels' links; the literal table they used to come from described
     * neither, and since B051 made the bridge return TIME its clocks divide a
     * real nanosecond figure. Base latency, FIFO depth, bridge count and the
     * router parameters are untouched -- they are not link properties. */
    void applySourcedLadder(const int width_bits[7], const double bandwidth_GBs[7]);

    /**
     * @brief Set the rank (L4) and channel (L5) node counts (audit C006).
     *
     * These two are the only rungs of the ladder with no other configuration
     * surface: initialize() carries subarrays/banks/bank-groups/chips and
     * overrideLevelConfig() does not touch num_nodes, so both sat at a literal
     * 2 for every technology. num_nodes sets the hop term at that level and
     * gates detailed-router construction there. Non-positive values are
     * ignored. Until this is called the run announces that both are assumed.
     */
    void setTopLevelNodeCounts(int ranks_per_channel, int channels);

    /**
     * @brief Check if network can accept more packets
     * @param level Network level to check
     * @return true if queue depth is below limit, false if congested
     */
    bool canAcceptPacket(NetworkLevel level);

    /**
     * @brief Set queue depth limit for a specific network level
     * @param level Network level
     * @param limit Maximum queue depth (0 = unlimited)
     */
    void setQueueLimit(NetworkLevel level, size_t limit);

    /**
     * @brief Set queue depth limits for L0-L3 (subarray through chip)
     *
     * Convenience overload for the most commonly configured levels.
     * Levels L4-L6 (rank, channel, system) retain their defaults.
     * Use setQueueLimit(NetworkLevel, size_t) to configure any level.
     */
    void setQueueLimits(size_t subarray_limit, size_t bank_limit,
                        size_t bg_limit, size_t chip_limit);

    /**
     * @brief Get current queue depth for a network level
     * @param level Network level
     * @return Current number of packets in queue
     */
    size_t getQueueDepth(NetworkLevel level) const;

    /**
     * @brief Get queue depth limit for a network level
     * @param level Network level
     * @return Queue depth limit (0 = unlimited)
     */
    size_t getQueueLimit(NetworkLevel level) const;

    /**
     * @brief Get network statistics
     */
    void printStats() const;

    /**
     * @brief Reset network statistics
     */
    void resetStats();

    /**
     * @brief Calculate network requirements for a data access pattern
     */
    uint64_t calculateNetworkRequirements(
        int pe_bank, int pe_bg, int pe_chip,
        const std::map<int, uint64_t>& data_distribution,
        std::vector<InternalDRAMTransfer>& transfers);

    uint64_t executeGather(
        int pe_bank,
        const std::vector<int>& source_banks,
        uint64_t bytes_per_bank);

    uint64_t executeScatter(
        int pe_bank,
        const std::vector<int>& dest_banks,
        uint64_t bytes_per_bank);

    uint64_t executeReduce(
        const std::vector<int>& source_banks,
        int dest_bank,
        uint64_t bytes_per_bank);

    uint64_t executeBroadcast(
        int source_bank,
        const std::vector<int>& dest_banks,
        uint64_t total_bytes);

    /**
     * @brief Get available network bandwidth at a level
     */
    double getAvailableBandwidth(NetworkLevel level) const;

    bool inSameBankGroup(int bank1, int bank2) const;
    bool inSameChip(int bank1, int bank2) const;

    /**
     * @brief Enable GARNET H-tree simulation for accurate NoC modeling
     *
     * Legacy API: sets all levels to DETAILED (enable=true) or ANALYTICAL (enable=false).
     * Prefer setLevelModel() for per-level control.
     */
    void enableGarnetSimulation(bool enable = true);

    /**
     * @brief Set the simulation model for a specific hierarchy level
     */
    void setLevelModel(int level, NetworkModelType model);

    /**
     * @brief Set the simulation model for a specific bridge boundary
     *
     * When set to AUTO, the bridge model is auto-derived from adjacent tiers.
     * When set to SIMPLE/DETAILED, the bridge model is independent.
     */
    void setBridgeModel(int boundary, NetworkModelType model);

    /**
     * @brief Override router parameters for a specific bridge boundary.
     * Negative values = keep existing default.
     */
    void overrideBridgeConfig(int boundary, int router_latency, int router_pipeline,
                              int router_bypass, int virtual_networks,
                              int virtual_channels_per_vn, int input_buffer_depth,
                              int output_buffer_depth);

    /**
     * @brief Get the simulation model for a specific hierarchy level
     */
    NetworkModelType getLevelModel(int level) const;

    /**
     * @brief Override link and router parameters for a specific hierarchy level.
     * Negative values or empty string = keep technology default.
     * Bandwidth is auto-recomputed from width x frequency.
     */
    void overrideLevelConfig(int level, int link_width_bits, double frequency_ghz,
                             int latency_cycles, const std::string& topology,
                             int router_latency = -1, int router_pipeline = -1,
                             int router_bypass = -1, int virtual_networks = -1,
                             int virtual_channels_per_vn = -1,
                             int input_buffer_depth = -1, int output_buffer_depth = -1);

    /**
     * @brief Get the bridge configuration array (read-only)
     */
    const std::array<BridgeConfig, NUM_TIER_BOUNDARIES>& getBridges() const { return bridges_; }

    /**
     * @brief Calculate the number of switch levels in the network hierarchy
     * @return Number of switch levels (7: L0-L6)
     */
    int getNumberOfSwitchLevels() const;

    /**
     * @brief Calculate the total number of switches across all levels
     *
     * Switch count per level (DDR4/DDR5):
     * - L0 subarrays: num_channels * ranks * chips * bgs * banks_per_bg
     * - L1 banks:     num_channels * ranks * chips * bgs
     * - L2 BGs:       num_channels * ranks * chips * bgs
     * - L3 chips:     num_channels * ranks * chips
     * - L4 ranks:     num_channels * ranks
     * - L5 channels:  num_channels
     * - L6 system:    1
     */
    int getTotalNumberOfSwitches(int num_channels, int ranks_per_channel) const;

    /**
     * @brief Get the number of switches at a specific level
     * @param level Switch level (0-6)
     */
    int getNumberOfSwitchesAtLevel(int level, int num_channels, int ranks_per_channel) const;

    /**
     * @brief Set custom switch hierarchy configuration
     */
    void setCustomSwitchHierarchy(const SwitchHierarchyConfig& config);

    /**
     * @brief Get current switch hierarchy configuration
     */
    SwitchHierarchyConfig getSwitchHierarchyConfig(int num_channels, int ranks_per_channel) const;

    static int calculatePortsPerSwitch(TopologyType topology, int num_endpoints, int num_switches);
    static int calculateOptimalSwitchCount(int num_endpoints, int max_ports_per_switch, TopologyType topology);
    static std::string getTopologyName(TopologyType topology);

    /**
     * @brief Get the DRAM technology enum
     */
    MemoryTechnology getTechnology() const { return technology_; }

private:
    // DRAM configuration
    std::string dram_type_;
    MemoryTechnology technology_;
    int num_subarrays_per_bank_;
    int num_banks_per_bg_;
    int num_bg_per_chip_;
    int num_chips_per_rank_;
    /* 1.11.57 (audit C006): has a caller supplied the rank and channel node
     * counts, or are L4 and L5 still on the assumed 2? Governs the notice
     * initialize() prints; it never silently changes a number. */
    bool top_level_nodes_set_ = false;

    // Custom switch hierarchy configuration (optional)
    SwitchHierarchyConfig custom_switch_config_;
    bool use_custom_switch_config_;

    // Network configuration for each level (indexed by NetworkLevel)
    std::array<InternalNetworkLink, NUM_HIERARCHY_LEVELS> network_configs_;

    // Packet queues for each network level
    std::array<std::queue<InternalNetworkPacket>, NUM_HIERARCHY_LEVELS> network_queues_;

    // In-flight packets
    std::vector<InternalNetworkPacket> inflight_packets_;

    // Queue depth limits (0 = unlimited)
    std::array<size_t, NUM_HIERARCHY_LEVELS> queue_limits_;

    // Default queue depth limit
    static constexpr size_t DEFAULT_QUEUE_DEPTH_LIMIT = 64;

    // External network model (optional, for detailed simulation)
    std::shared_ptr<NetworkModel> external_network_model_;

    // GARNET H-tree models for each hierarchy level (optional)
    std::array<std::shared_ptr<NetworkModel>, NUM_HIERARCHY_LEVELS> garnet_networks_;
    // GARNET models for bridge boundaries (independent of tier Garnets)
    std::array<std::shared_ptr<NetworkModel>, NUM_TIER_BOUNDARIES> bridge_garnet_networks_;
    bool use_garnet_models_;  // Legacy: true if ANY level or bridge uses Garnet

    // Per-level simulation model choice
    std::array<NetworkModelType, NUM_HIERARCHY_LEVELS> level_models_;

    // Bridge configurations for each tier boundary (6 boundaries for 7 tiers)
    // bridges_[0] = L0<->L1, bridges_[1] = L1<->L2, ..., bridges_[5] = L5<->L6
    std::array<BridgeConfig, NUM_TIER_BOUNDARIES> bridges_;

    // Bridge buffers for packets crossing tier boundaries
    struct BridgeBuffer {
        struct BufferedPacket {
            InternalNetworkPacket packet;
            uint64_t release_cycle;
            int next_tier;
            int next_bridge_id;
        };
        std::queue<BufferedPacket> queue;
        int max_depth = 64;
    };
    std::array<std::vector<BridgeBuffer>, NUM_TIER_BOUNDARIES> bridge_buffers_;

    // M/D/1 contention state for tier networks
    struct TierMD1State {
        uint64_t lastUpdateCycle = 0;
        double smoothedArrivalRate = 0.0;
        uint32_t curWindowAccesses = 0;
    };
    std::array<TierMD1State, NUM_HIERARCHY_LEVELS> tier_md1_state_;

    // M/D/1 contention state for bridge crossings
    struct BridgeMD1State {
        uint64_t lastUpdateCycle = 0;
        double smoothedArrivalRate = 0.0;
        uint32_t curWindowAccesses = 0;
    };
    std::array<BridgeMD1State, NUM_TIER_BOUNDARIES> bridge_md1_state_;

    // M/D/1 smoothing window (in cycles)
    static constexpr uint64_t MD1_WINDOW_CYCLES = 10000;

    // Current cycle
    uint64_t current_cycle_;

    // Statistics
    uint64_t total_packets_sent_;
    uint64_t total_packets_completed_;
    uint64_t total_bytes_transferred_;
    uint64_t total_network_latency_;
    std::array<uint64_t, NUM_HIERARCHY_LEVELS> network_accesses_;

    /**
     * @brief Configure network based on memory type (DRAM and NVM)
     */
    // SRAM configurations
    void configureSRAMNetwork();

    // NVM configurations
    void configureSTTMRAMNetwork();
    void configurePCMNetwork();
    void configureReRAMNetwork();

    // DRAM configurations
    void configureDDR3Network();
    void configureDDR4Network();
    void configureDDR5Network();
    void configureLPDDR5Network();
    void configureGDDR6Network();
    void configureHBM2Network();
    void configureHBM3Network();

    /**
     * @brief Convert global bank/subarray IDs to a hierarchical address
     */
    HierarchicalAddress bankToAddress(int global_bank_id, int subarray_id) const;

    /**
     * @brief Find the lowest common ancestor level for two addresses
     *
     * Compares address fields top-down. The first level where src and dst
     * differ is the LCA (the level at which routing must occur).
     * Returns 0 if same bank (subarray-level routing only).
     */
    int lowestCommonAncestor(const HierarchicalAddress& src,
                             const HierarchicalAddress& dst) const;

    /**
     * @brief Get the address field value at a given hierarchy level
     */
    int addressFieldAtLevel(const HierarchicalAddress& addr, int level) const;

    /**
     * @brief Get latency for routing within a single tier
     *
     * SIMPLE: avg_hops x (router_latency + link_latency) + serialization + M/D/1 queuing
     * DETAILED: delegates to Garnet (returns 0, Garnet handles timing)
     */
    uint64_t getTierLatency(int level, const HierarchicalAddress& src,
                            const HierarchicalAddress& dst,
                            uint64_t data_bytes);

    /**
     * @brief Get bridge crossing latency at a tier boundary
     *
     * When bridge model is AUTO: auto-derives from adjacent tiers (legacy).
     * When bridge model is SIMPLE/DETAILED: independent of tier models.
     * Bridge = 1-hop, 2-port router.
     */
    uint64_t getBridgeLatency(int boundary, uint64_t data_bytes);

    /**
     * @brief Create a 2-node CROSSBAR Garnet for a bridge boundary
     */
    std::shared_ptr<NetworkModel> createGarnetBridgeRouter(
        int boundary, const BridgeConfig& br);

    /**
     * @brief Auto-populate bridge defaults from technology
     */
    void initializeBridgeDefaults();

    uint64_t calculateTransferTime(const InternalNetworkLink& link,
                                   uint64_t data_bytes);

    void processInflightPackets();
    void processGarnetArrivedPackets();

    /**
     * @brief Convert NetworkLevel enum to integer index (0-6)
     */
    static int levelToIndex(NetworkLevel level);
};

/**
 * @brief Helper function to create internal network configuration
 */
std::shared_ptr<InternalDRAMNetwork> createInternalDRAMNetwork(
    const std::string& dram_type,
    int num_subarrays_per_bank = 16,
    int num_banks_per_bg = 4,
    int num_bg_per_chip = 4,
    int num_chips_per_rank = 8);

/**
 * @brief Create GARNET H-tree network for DRAM internal interconnect
 */
std::shared_ptr<NetworkModel> createGarnetHTreeForDRAM(
    NetworkLevel level,
    int num_nodes,
    int link_width_bits,
    int link_latency_cycles,
    double bandwidth_GBs);

std::shared_ptr<NetworkModel> createLevel0Network(
    const std::string& dram_type,
    int num_subarrays,
    bool use_garnet = false);

std::shared_ptr<NetworkModel> createBankNetwork(
    const std::string& dram_type,
    int num_banks,
    bool use_garnet = false);

std::shared_ptr<NetworkModel> createBankGroupNetwork(
    const std::string& dram_type,
    int num_bank_groups,
    bool use_garnet = false);

std::shared_ptr<NetworkModel> createChipNetwork(
    const std::string& dram_type,
    int num_chips,
    bool use_garnet = false);

std::shared_ptr<InternalDRAMNetwork> createHierarchicalNetwork(
    const std::string& dram_type,
    int num_subarrays_per_bank = 16,
    int num_banks_per_bg = 4,
    int num_bg_per_chip = 4,
    int num_chips_per_rank = 8,
    bool use_garnet = false);

} // namespace pimid

#endif // PIMID_INTERNAL_DRAM_NETWORK_H
