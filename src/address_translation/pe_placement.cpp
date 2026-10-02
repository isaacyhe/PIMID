#include "address_translation/pe_placement.h"
#include "memory/dram_architecture_v2.h"
#include <iostream>
#include <algorithm>
#include <iomanip>

namespace pimid {

//=============================================================================
// PEPlacementManager Implementation
//=============================================================================

PEPlacementManager::PEPlacementManager(const MemoryHierarchy& hierarchy,
                                       PEPlacementLevel level,
                                       AddressingMode mode,
                                       std::shared_ptr<memory::DRAMArchitectureV2> dram_arch)
    : hierarchy_(hierarchy)
    , placement_level_(level)
    , addressing_mode_(mode)
    , dram_arch_(dram_arch)
    , last_update_cycle_(0) {
}

void PEPlacementManager::registerPE(const PEDescriptor& pe) {
    PEDescriptor pe_copy = pe;

    // Compute constraints based on placement level
    computeConstraints(pe_copy);

    pes_[pe.pe_id] = pe_copy;

    // Rebuild lookup tables
    buildLookupTables();
    buildBusSharingTables();

    std::cout << "[PEPlacement] Registered PE " << pe.pe_id
              << " at level " << static_cast<int>(pe.level) << std::endl;
    printConstraints(pe.pe_id);
}

void PEPlacementManager::unregisterPE(uint32_t pe_id) {
    pes_.erase(pe_id);
    buildLookupTables();
    buildBusSharingTables();
}

const PEDescriptor* PEPlacementManager::getPE(uint32_t pe_id) const {
    auto it = pes_.find(pe_id);
    if (it != pes_.end()) {
        return &it->second;
    }
    return nullptr;
}

//=============================================================================
// Constraint Computation
//=============================================================================

void PEPlacementManager::computeConstraints(PEDescriptor& pe) const {
    // Calculate bus constraints
    pe.bus_constraints = calculateBusConstraints(pe.level, pe.bank_id);

    // Calculate address constraints
    pe.addr_constraints = calculateAddressConstraints(pe.level, pe);
}

PEBusConstraints PEPlacementManager::calculateBusConstraints(
    PEPlacementLevel level,
    uint32_t location_id) const {

    // If DRAM architecture is available, use it for accurate constraints
    if (dram_arch_) {
        PEBusConstraints constraints = createPEBusConstraintsFromDRAM(*dram_arch_, level);

        // Adjust shared_bus_pes based on hierarchy (not in DRAM arch)
        switch (level) {
            case PEPlacementLevel::SUBARRAY:
                constraints.shared_bus_pes = 1;  // Dedicated per subarray
                break;
            case PEPlacementLevel::BANK:
                constraints.shared_bus_pes = hierarchy_.num_subarrays_per_bank;
                break;
            case PEPlacementLevel::BANK_GROUP:
                // BG = group of banks sharing a BG bus. Approximate as chip-level
                // until a dedicated banks_per_bank_group field is plumbed through.
                constraints.shared_bus_pes = hierarchy_.num_banks_per_chip;
                break;
            case PEPlacementLevel::CHIP:
                constraints.shared_bus_pes = hierarchy_.num_banks_per_chip;
                break;
            case PEPlacementLevel::RANK:
                constraints.shared_bus_pes = hierarchy_.num_banks_per_chip *
                                            hierarchy_.num_chips_per_rank;
                break;
            case PEPlacementLevel::LOGIC_DIE:
                constraints.shared_bus_pes = 1;  // Dedicated logic die
                break;
            case PEPlacementLevel::HOST_MC:
                // PEs share host's memory controller -- host owns the rank bus.
                constraints.shared_bus_pes = hierarchy_.num_banks_per_chip *
                                            hierarchy_.num_chips_per_rank;
                break;
        }

        return constraints;
    }

    // FALLBACK: If no DRAM architecture provided, use legacy hardcoded values
    // NOTE: This path is deprecated and should not be used for accurate simulations
    PEBusConstraints constraints;

    switch (level) {
        case PEPlacementLevel::SUBARRAY:
            // Subarray level: Limited to row buffer width
            constraints.data_bus_width_bits = 8192 * 8;  // 8KB row buffer = 64Kb
            constraints.max_bandwidth_gbps = 10;         // Limited internal bandwidth
            constraints.row_buffer_size_bytes = 8192;
            constraints.shared_bus_pes = 1;              // Dedicated per subarray
            constraints.has_dedicated_bus = true;
            break;

        case PEPlacementLevel::BANK:
            // Bank level: Share bank I/O bus
            constraints.data_bus_width_bits = 64;        // 64-bit bank bus
            constraints.max_bandwidth_gbps = 25;         // Typical DDR4 per-bank
            constraints.row_buffer_size_bytes = 8192;
            constraints.shared_bus_pes = hierarchy_.num_subarrays_per_bank;
            constraints.has_dedicated_bus = false;
            break;

        case PEPlacementLevel::BANK_GROUP:
            // Bank group level: Share BG bus (group of banks)
            constraints.data_bus_width_bits = 64;
            constraints.max_bandwidth_gbps = 25;
            constraints.row_buffer_size_bytes = 0;
            constraints.shared_bus_pes = hierarchy_.num_banks_per_chip;
            constraints.has_dedicated_bus = false;
            break;

        case PEPlacementLevel::CHIP:
            // Chip level: Share chip-level interconnect
            constraints.data_bus_width_bits = 64;        // 64-bit per channel
            constraints.max_bandwidth_gbps = 25;         // Share with all banks
            constraints.row_buffer_size_bytes = 0;       // No direct row buffer access
            constraints.shared_bus_pes = hierarchy_.num_banks_per_chip;
            constraints.has_dedicated_bus = false;
            break;

        case PEPlacementLevel::RANK:
            // Rank level: Share rank-level bus
            constraints.data_bus_width_bits = 64;
            constraints.max_bandwidth_gbps = 25;
            constraints.row_buffer_size_bytes = 0;
            constraints.shared_bus_pes = hierarchy_.num_banks_per_chip *
                                        hierarchy_.num_chips_per_rank;
            constraints.has_dedicated_bus = false;
            break;

        case PEPlacementLevel::LOGIC_DIE:
            // Logic die (HBM/HMC): Wide high-bandwidth interface
            constraints.data_bus_width_bits = 1024;      // Wide bus (128 bytes)
            constraints.max_bandwidth_gbps = 256;        // HBM2 bandwidth
            constraints.row_buffer_size_bytes = 0;
            constraints.shared_bus_pes = 1;              // Dedicated logic die bus
            constraints.has_dedicated_bus = true;
            break;

        case PEPlacementLevel::HOST_MC:
            // Host MC: PEs share host's memory controller -- rank-level visibility
            constraints.data_bus_width_bits = 64;
            constraints.max_bandwidth_gbps = 25;
            constraints.row_buffer_size_bytes = 0;
            constraints.shared_bus_pes = hierarchy_.num_banks_per_chip *
                                        hierarchy_.num_chips_per_rank;
            constraints.has_dedicated_bus = false;
            break;
    }

    return constraints;
}

PEAddressConstraints PEPlacementManager::calculateAddressConstraints(
    PEPlacementLevel level,
    const PEDescriptor& pe) const {

    PEAddressConstraints constraints;

    // Calculate sizes from memory hierarchy configuration
    // These values come from DRAM architecture (see pimid/memory/dram_architecture_v2.h)
    // NOT hard-coded to allow different DRAM types (DDR4, HBM2, etc.)

    // Get sizes from hierarchy, with fallback to safe defaults if not configured
    const uint64_t SUBARRAY_SIZE = hierarchy_.subarray_size_bytes > 0 ?
        hierarchy_.subarray_size_bytes : (128ULL * 1024);  // Default: 128 KB
    const uint64_t BANK_SIZE = hierarchy_.bank_size_bytes > 0 ?
        hierarchy_.bank_size_bytes : (8ULL * 1024 * 1024);  // Default: 8 MB
    const uint64_t CHIP_SIZE = hierarchy_.chip_size_bytes > 0 ?
        hierarchy_.chip_size_bytes : (64ULL * 1024 * 1024);  // Default: 64 MB
    const uint64_t RANK_SIZE = hierarchy_.rank_size_bytes > 0 ?
        hierarchy_.rank_size_bytes : (256ULL * 1024 * 1024);  // Default: 256 MB

    switch (level) {
        case PEPlacementLevel::SUBARRAY:
            // Can only access single subarray
            constraints.accessible_base = pe.rank_id * RANK_SIZE +
                                         pe.chip_id * CHIP_SIZE +
                                         pe.bank_id * BANK_SIZE +
                                         pe.subarray_id * SUBARRAY_SIZE;
            constraints.accessible_size_bytes = SUBARRAY_SIZE;
            constraints.accessible_limit = constraints.accessible_base +
                                          constraints.accessible_size_bytes;
            /* 1.11.94 (sweep-94 row 28 / R3287): a subarray-placed PE DOES reach
             * remote units -- through the tier tree at the tree's cost, which is
             * the locked ladder's meaning and what the PE memory interface
             * models. The old "not allowed" here was a claim nothing enforced. */
            constraints.can_access_remote = true;
            constraints.remote_access_penalty = 0;  // the tree cost is charged by the interface, not here
            break;

        case PEPlacementLevel::BANK:
            // Can access entire bank
            constraints.accessible_base = pe.rank_id * RANK_SIZE +
                                         pe.chip_id * CHIP_SIZE +
                                         pe.bank_id * BANK_SIZE;
            constraints.accessible_size_bytes = BANK_SIZE;
            constraints.accessible_limit = constraints.accessible_base +
                                          constraints.accessible_size_bytes;
            constraints.can_access_remote = true;   // Can access other banks
            constraints.remote_access_penalty = hierarchy_.cross_bank_penalty_cycles;
            break;

        case PEPlacementLevel::CHIP:
            // Can access entire chip
            constraints.accessible_base = pe.rank_id * RANK_SIZE +
                                         pe.chip_id * CHIP_SIZE;
            constraints.accessible_size_bytes = CHIP_SIZE;
            constraints.accessible_limit = constraints.accessible_base +
                                          constraints.accessible_size_bytes;
            constraints.can_access_remote = true;
            constraints.remote_access_penalty = hierarchy_.cross_chip_penalty_cycles;
            break;

        case PEPlacementLevel::RANK:
            // Can access entire rank
            constraints.accessible_base = pe.rank_id * RANK_SIZE;
            constraints.accessible_size_bytes = RANK_SIZE;
            constraints.accessible_limit = constraints.accessible_base +
                                          constraints.accessible_size_bytes;
            constraints.can_access_remote = true;
            constraints.remote_access_penalty = hierarchy_.cross_rank_penalty_cycles;
            break;

        case PEPlacementLevel::LOGIC_DIE:
            // Can access all memory
            constraints.accessible_base = 0;
            constraints.accessible_size_bytes = RANK_SIZE * hierarchy_.num_ranks;
            constraints.accessible_limit = constraints.accessible_size_bytes;
            constraints.can_access_remote = true;
            constraints.remote_access_penalty = hierarchy_.logic_to_dram_penalty_cycles;
            break;

        case PEPlacementLevel::BANK_GROUP: {
            // Access span ~= chip / 4 (typical DDR5 has 4 bank groups per chip).
            const uint64_t BG_SIZE = CHIP_SIZE / 4;
            constraints.accessible_base = pe.rank_id * RANK_SIZE +
                                         pe.chip_id * CHIP_SIZE;
            constraints.accessible_size_bytes = BG_SIZE;
            constraints.accessible_limit = constraints.accessible_base +
                                          constraints.accessible_size_bytes;
            constraints.can_access_remote = true;
            constraints.remote_access_penalty = hierarchy_.cross_bank_penalty_cycles;
            break;
        }

        case PEPlacementLevel::HOST_MC:
            // PEs share host's MC -- host's address visibility is rank-level.
            constraints.accessible_base = pe.rank_id * RANK_SIZE;
            constraints.accessible_size_bytes = RANK_SIZE;
            constraints.accessible_limit = constraints.accessible_base +
                                          constraints.accessible_size_bytes;
            constraints.can_access_remote = true;
            constraints.remote_access_penalty = hierarchy_.cross_rank_penalty_cycles;
            break;
    }

    return constraints;
}

//=============================================================================
// Address Access Validation
//=============================================================================

bool PEPlacementManager::canAccess(uint32_t pe_id, Address addr) const {
    auto pe = getPE(pe_id);
    if (!pe) return false;

    const auto& constraints = pe->addr_constraints;

    // Check if address is in local accessible range
    if (addr >= constraints.accessible_base &&
        addr < constraints.accessible_limit) {
        return true;
    }

    // Check if remote access is allowed
    return constraints.can_access_remote;
}

bool PEPlacementManager::isLocalAddress(uint32_t pe_id, Address addr) const {
    auto pe = getPE(pe_id);
    if (!pe) return false;

    const auto& constraints = pe->addr_constraints;
    return (addr >= constraints.accessible_base &&
            addr < constraints.accessible_limit);
}

bool PEPlacementManager::isRemoteAccess(uint32_t pe_id, Address addr) const {
    return canAccess(pe_id, addr) && !isLocalAddress(pe_id, addr);
}

uint32_t PEPlacementManager::getAccessPenalty(uint32_t pe_id, Address addr) const {
    auto pe = getPE(pe_id);
    if (!pe) return 0;

    if (isLocalAddress(pe_id, addr)) {
        return 0;  // No penalty for local access
    }

    if (isRemoteAccess(pe_id, addr)) {
        return pe->addr_constraints.remote_access_penalty;
    }

    // Address not accessible
    return UINT32_MAX;
}

//=============================================================================
// Bandwidth and Bus Contention
//=============================================================================

uint64_t PEPlacementManager::getAvailableBandwidth(uint32_t pe_id) const {
    auto pe = getPE(pe_id);
    if (!pe) return 0;

    const auto& bus_constraints = pe->bus_constraints;

    // If dedicated bus, return full bandwidth
    if (bus_constraints.has_dedicated_bus) {
        return bus_constraints.max_bandwidth_gbps;
    }

    // Shared bus: divide by number of PEs sharing it
    uint32_t sharing_pes = bus_constraints.shared_bus_pes;
    if (sharing_pes == 0) sharing_pes = 1;

    return bus_constraints.max_bandwidth_gbps / sharing_pes;
}

uint64_t PEPlacementManager::getEffectiveBandwidth(uint32_t pe_id,
                                                    Cycle current_cycle) const {
    auto pe = getPE(pe_id);
    if (!pe) return 0;

    uint64_t available_bw = getAvailableBandwidth(pe_id);

    // Get bus utilization
    uint32_t bus_id = getBusID(*pe);
    auto it = bus_utilization_.find(bus_id);

    if (it == bus_utilization_.end()) {
        return available_bw;  // No contention
    }

    // Reduce bandwidth based on utilization (0-100%)
    uint64_t utilization_pct = it->second;
    uint64_t effective_bw = available_bw * (100 - utilization_pct) / 100;

    return effective_bw;
}

bool PEPlacementManager::hasBusContention(uint32_t pe_id) const {
    auto sharing_pes = getSharingPEs(pe_id);
    return sharing_pes.size() > 1;
}

std::vector<uint32_t> PEPlacementManager::getSharingPEs(uint32_t pe_id) const {
    auto pe = getPE(pe_id);
    if (!pe) return {};

    uint32_t bus_id = getBusID(*pe);

    // Find all PEs sharing the same bus
    std::vector<uint32_t> sharing;

    for (const auto& pair : pes_) {
        if (getBusID(pair.second) == bus_id) {
            sharing.push_back(pair.first);
        }
    }

    return sharing;
}

//=============================================================================
// Helper Functions
//=============================================================================

void PEPlacementManager::buildLookupTables() {
    level_to_pes_.clear();
    bank_to_pes_.clear();
    chip_to_pes_.clear();
    rank_to_pes_.clear();

    for (const auto& pair : pes_) {
        const auto& pe = pair.second;

        level_to_pes_[pe.level].push_back(pe.pe_id);
        bank_to_pes_[pe.bank_id].push_back(pe.pe_id);
        chip_to_pes_[pe.chip_id].push_back(pe.pe_id);
        rank_to_pes_[pe.rank_id].push_back(pe.pe_id);
    }
}

void PEPlacementManager::buildBusSharingTables() {
    subarray_bus_to_pes_.clear();
    bank_bus_to_pes_.clear();
    chip_bus_to_pes_.clear();
    rank_bus_to_pes_.clear();

    for (const auto& pair : pes_) {
        const auto& pe = pair.second;
        uint32_t bus_id = getBusID(pe);

        switch (pe.level) {
            case PEPlacementLevel::SUBARRAY:
                subarray_bus_to_pes_[bus_id].push_back(pe.pe_id);
                break;
            case PEPlacementLevel::BANK:
                bank_bus_to_pes_[bus_id].push_back(pe.pe_id);
                break;
            case PEPlacementLevel::BANK_GROUP:
            case PEPlacementLevel::CHIP:
                chip_bus_to_pes_[bus_id].push_back(pe.pe_id);
                break;
            case PEPlacementLevel::RANK:
            case PEPlacementLevel::LOGIC_DIE:
            case PEPlacementLevel::HOST_MC:
                rank_bus_to_pes_[bus_id].push_back(pe.pe_id);
                break;
        }
    }
}

uint32_t PEPlacementManager::getBusID(const PEDescriptor& pe) const {
    // Generate unique bus ID based on PE location
    switch (pe.level) {
        case PEPlacementLevel::SUBARRAY:
            return (pe.rank_id << 24) | (pe.chip_id << 16) |
                   (pe.bank_id << 8) | pe.subarray_id;
        case PEPlacementLevel::BANK:
            return (pe.rank_id << 16) | (pe.chip_id << 8) | pe.bank_id;
        case PEPlacementLevel::BANK_GROUP:
            // BG ID encoded at chip-level granularity (no dedicated BG field yet)
            return (pe.rank_id << 8) | pe.chip_id;
        case PEPlacementLevel::CHIP:
            return (pe.rank_id << 8) | pe.chip_id;
        case PEPlacementLevel::RANK:
            return pe.rank_id;
        case PEPlacementLevel::LOGIC_DIE:
            return 0;  // Single logic die bus
        case PEPlacementLevel::HOST_MC:
            return pe.rank_id;  // Host MC owns the rank
    }
    return 0;
}

//=============================================================================
// Hierarchy Navigation
//=============================================================================

std::vector<uint32_t> PEPlacementManager::getPEsAtLevel(PEPlacementLevel level) const {
    auto it = level_to_pes_.find(level);
    if (it != level_to_pes_.end()) {
        return it->second;
    }
    return {};
}

std::vector<uint32_t> PEPlacementManager::getPEsInBank(uint32_t bank_id) const {
    auto it = bank_to_pes_.find(bank_id);
    if (it != bank_to_pes_.end()) {
        return it->second;
    }
    return {};
}

//=============================================================================
// Printing and Debugging
//=============================================================================

void PEPlacementManager::printPlacement() const {
    std::cout << "\n=== PE Placement Summary ===" << std::endl;
    std::cout << "Total PEs: " << pes_.size() << std::endl;
    std::cout << "Placement Level: " << static_cast<int>(placement_level_) << std::endl;
    std::cout << "Addressing Mode: " << static_cast<int>(addressing_mode_) << std::endl;

    for (const auto& pair : pes_) {
        const auto& pe = pair.second;
        std::cout << "\nPE " << pe.pe_id << ":" << std::endl;
        std::cout << "  Level: " << static_cast<int>(pe.level) << std::endl;
        std::cout << "  Location: R" << pe.rank_id << "C" << pe.chip_id
                  << "B" << pe.bank_id << "S" << pe.subarray_id << std::endl;
        printConstraints(pe.pe_id);
    }
}

void PEPlacementManager::printConstraints(uint32_t pe_id) const {
    auto pe = getPE(pe_id);
    if (!pe) return;

    const auto& bus = pe->bus_constraints;
    const auto& addr = pe->addr_constraints;

    std::cout << "  Bus Constraints:" << std::endl;
    std::cout << "    Data Bus Width: " << bus.data_bus_width_bits << " bits" << std::endl;
    std::cout << "    Max Bandwidth: " << bus.max_bandwidth_gbps << " GB/s" << std::endl;
    std::cout << "    Shared PEs: " << bus.shared_bus_pes << std::endl;
    std::cout << "    Dedicated Bus: " << (bus.has_dedicated_bus ? "Yes" : "No") << std::endl;

    std::cout << "  Address Constraints:" << std::endl;
    std::cout << "    Accessible Range: 0x" << std::hex << addr.accessible_base
              << " - 0x" << addr.accessible_limit << std::dec << std::endl;
    std::cout << "    Accessible Size: " << (addr.accessible_size_bytes / 1024) << " KB" << std::endl;
    std::cout << "    Remote Access: " << (addr.can_access_remote ? "Yes" : "No") << std::endl;
    std::cout << "    Remote Penalty: " << addr.remote_access_penalty << " cycles" << std::endl;
}

//=============================================================================
// Stub implementations for remaining functions
//=============================================================================

uint32_t PEPlacementManager::getPEForAddress(Address addr) const {
    for (const auto& pair : pes_) {
        if (isLocalAddress(pair.first, addr)) {
            return pair.first;
        }
    }
    return UINT32_MAX;
}

std::vector<uint32_t> PEPlacementManager::getPEsForAddress(Address addr) const {
    std::vector<uint32_t> result;
    for (const auto& pair : pes_) {
        if (canAccess(pair.first, addr)) {
            result.push_back(pair.first);
        }
    }
    return result;
}

Address PEPlacementManager::translateAddress(uint32_t from_pe, uint32_t to_pe,
                                             Address addr) const {
    // For unified addressing, no translation needed
    if (addressing_mode_ == AddressingMode::UNIFIED) {
        return addr;
    }

    // For discrete addressing, translate between PE-local address spaces
    const PEDescriptor* from = getPE(from_pe);
    const PEDescriptor* to = getPE(to_pe);

    if (!from || !to) {
        return addr;  // Cannot translate without PE descriptors
    }

    // Calculate the local offset within the source PE's address space
    // In discrete mode, addresses are relative to each PE's base
    Address local_offset = 0;

    if (addr >= from->addr_base && addr < from->addr_limit) {
        // Address is within source PE's local space
        local_offset = addr - from->addr_base;
    } else if (addr >= from->addr_constraints.accessible_base &&
               addr < from->addr_constraints.accessible_limit) {
        // Address is in source PE's accessible range (relative addressing)
        local_offset = addr - from->addr_constraints.accessible_base;
    } else {
        // Address appears to already be a global offset
        local_offset = addr;
    }

    // If target PE has the same size, direct mapping works
    if (to->addr_constraints.accessible_size_bytes ==
        from->addr_constraints.accessible_size_bytes) {
        // Direct 1:1 mapping
        return to->addr_base + local_offset;
    }

    // Handle size mismatch (e.g., subarray PE to bank PE)
    // Scale the offset based on size ratio
    double scale_factor = static_cast<double>(to->addr_constraints.accessible_size_bytes) /
                         static_cast<double>(from->addr_constraints.accessible_size_bytes);

    // For larger target: offset maps directly
    // For smaller target: offset wraps within target space
    if (scale_factor >= 1.0) {
        // Target is larger or equal - direct mapping
        return to->addr_base + local_offset;
    } else {
        // Target is smaller - wrap within target space
        uint64_t wrapped_offset = local_offset % to->addr_constraints.accessible_size_bytes;
        return to->addr_base + wrapped_offset;
    }
}

std::vector<uint32_t> PEPlacementManager::getPEsInSubarray(uint32_t bank_id,
                                                           uint32_t subarray_id) const {
    std::vector<uint32_t> result;
    for (const auto& pair : pes_) {
        const auto& pe = pair.second;
        if (pe.bank_id == bank_id && pe.subarray_id == subarray_id) {
            result.push_back(pe.pe_id);
        }
    }
    return result;
}

std::vector<uint32_t> PEPlacementManager::getPEsInChip(uint32_t chip_id) const {
    auto it = chip_to_pes_.find(chip_id);
    if (it != chip_to_pes_.end()) {
        return it->second;
    }
    return {};
}

std::vector<uint32_t> PEPlacementManager::getPEsInRank(uint32_t rank_id) const {
    auto it = rank_to_pes_.find(rank_id);
    if (it != rank_to_pes_.end()) {
        return it->second;
    }
    return {};
}

std::vector<uint32_t> PEPlacementManager::getPEsOnLogicDie() const {
    std::vector<uint32_t> result;
    for (const auto& pair : pes_) {
        if (pair.second.on_logic_die) {
            result.push_back(pair.first);
        }
    }
    return result;
}

Address PEPlacementManager::getHierarchyBase(PEPlacementLevel level,
                                             const PEDescriptor& pe) const {
    // Calculate base address based on memory hierarchy organization
    // Uses sizes from hierarchy_, falling back to defaults if not configured

    const uint64_t SUBARRAY_SIZE = hierarchy_.subarray_size_bytes > 0 ?
        hierarchy_.subarray_size_bytes : (128ULL * 1024);  // 128 KB default
    const uint64_t BANK_SIZE = hierarchy_.bank_size_bytes > 0 ?
        hierarchy_.bank_size_bytes : (8ULL * 1024 * 1024);  // 8 MB default
    const uint64_t CHIP_SIZE = hierarchy_.chip_size_bytes > 0 ?
        hierarchy_.chip_size_bytes : (64ULL * 1024 * 1024);  // 64 MB default
    const uint64_t RANK_SIZE = hierarchy_.rank_size_bytes > 0 ?
        hierarchy_.rank_size_bytes : (256ULL * 1024 * 1024);  // 256 MB default

    switch (level) {
        case PEPlacementLevel::SUBARRAY:
            // Base of specific subarray
            return pe.rank_id * RANK_SIZE +
                   pe.chip_id * CHIP_SIZE +
                   pe.bank_id * BANK_SIZE +
                   pe.subarray_id * SUBARRAY_SIZE;

        case PEPlacementLevel::BANK:
            // Base of specific bank
            return pe.rank_id * RANK_SIZE +
                   pe.chip_id * CHIP_SIZE +
                   pe.bank_id * BANK_SIZE;

        case PEPlacementLevel::CHIP:
            // Base of specific chip
            return pe.rank_id * RANK_SIZE +
                   pe.chip_id * CHIP_SIZE;

        case PEPlacementLevel::RANK:
            // Base of specific rank
            return pe.rank_id * RANK_SIZE;

        case PEPlacementLevel::LOGIC_DIE:
            // Logic die has access to all memory - base is 0
            return 0;

        default:
            return 0;
    }
}

Address PEPlacementManager::getHierarchyLimit(PEPlacementLevel level,
                                              const PEDescriptor& pe) const {
    // Calculate limit address (exclusive) based on memory hierarchy organization
    // Uses sizes from hierarchy_, falling back to defaults if not configured

    const uint64_t SUBARRAY_SIZE = hierarchy_.subarray_size_bytes > 0 ?
        hierarchy_.subarray_size_bytes : (128ULL * 1024);  // 128 KB default
    const uint64_t BANK_SIZE = hierarchy_.bank_size_bytes > 0 ?
        hierarchy_.bank_size_bytes : (8ULL * 1024 * 1024);  // 8 MB default
    const uint64_t CHIP_SIZE = hierarchy_.chip_size_bytes > 0 ?
        hierarchy_.chip_size_bytes : (64ULL * 1024 * 1024);  // 64 MB default
    const uint64_t RANK_SIZE = hierarchy_.rank_size_bytes > 0 ?
        hierarchy_.rank_size_bytes : (256ULL * 1024 * 1024);  // 256 MB default

    // Get base address and add size for the level
    Address base = getHierarchyBase(level, pe);

    switch (level) {
        case PEPlacementLevel::SUBARRAY:
            return base + SUBARRAY_SIZE;

        case PEPlacementLevel::BANK:
            return base + BANK_SIZE;

        case PEPlacementLevel::CHIP:
            return base + CHIP_SIZE;

        case PEPlacementLevel::RANK:
            return base + RANK_SIZE;

        case PEPlacementLevel::LOGIC_DIE:
            // Logic die has access to all memory
            return RANK_SIZE * hierarchy_.num_ranks;

        default:
            return 0;
    }
}

bool PEPlacementManager::isAddressInRange(Address addr,
                                          const PEDescriptor& pe) const {
    return addr >= pe.addr_base && addr < pe.addr_limit;
}

//=============================================================================
// Factory Functions for Creating Constraints from DRAM Architecture
//=============================================================================

PEBusConstraints createPEBusConstraintsFromDRAM(
    const memory::DRAMArchitectureV2& dram_arch,
    PEPlacementLevel level) {

    PEBusConstraints constraints;

    // Apply port width scaling factor (allows "what if 2x wider?" studies)
    double scale = dram_arch.port_width_scale;

    switch (level) {
        case PEPlacementLevel::SUBARRAY:
            // Use subarray-level constraints from DRAM architecture
            constraints.data_bus_width_bits = static_cast<uint64_t>(
                dram_arch.pe_bus_constraints.subarray_level.data_bus_width_bits * scale);
            constraints.max_bandwidth_gbps =
                dram_arch.pe_bus_constraints.subarray_level.max_bandwidth_gbps * scale;
            constraints.row_buffer_size_bytes =
                dram_arch.pe_bus_constraints.subarray_level.row_buffer_size_bytes;
            constraints.shared_bus_pes = 1;  // Dedicated per subarray
            constraints.has_dedicated_bus =
                dram_arch.pe_bus_constraints.subarray_level.has_dedicated_bus;
            break;

        case PEPlacementLevel::BANK:
            // Use bank-level constraints from DRAM architecture
            constraints.data_bus_width_bits = static_cast<uint64_t>(
                dram_arch.pe_bus_constraints.bank_level.data_bus_width_bits * scale);
            constraints.max_bandwidth_gbps =
                dram_arch.pe_bus_constraints.bank_level.max_bandwidth_gbps * scale;
            constraints.row_buffer_size_bytes = 0;  // No direct row buffer access
            constraints.shared_bus_pes = dram_arch.organization.subarrays_per_bank;
            constraints.has_dedicated_bus =
                dram_arch.pe_bus_constraints.bank_level.has_dedicated_bus;
            break;

        case PEPlacementLevel::CHIP:
            // Use chip-level constraints from DRAM architecture
            constraints.data_bus_width_bits = static_cast<uint64_t>(
                dram_arch.pe_bus_constraints.chip_level.data_bus_width_bits * scale);
            constraints.max_bandwidth_gbps =
                dram_arch.pe_bus_constraints.chip_level.max_bandwidth_gbps * scale;
            constraints.row_buffer_size_bytes = 0;
            constraints.shared_bus_pes =
                dram_arch.organization.banks_per_bank_group *
                dram_arch.organization.bank_groups_per_chip;
            constraints.has_dedicated_bus =
                dram_arch.pe_bus_constraints.chip_level.has_dedicated_bus;
            break;

        case PEPlacementLevel::RANK:
            // Use rank-level constraints from DRAM architecture
            constraints.data_bus_width_bits = static_cast<uint64_t>(
                dram_arch.pe_bus_constraints.rank_level.data_bus_width_bits * scale);
            constraints.max_bandwidth_gbps =
                dram_arch.pe_bus_constraints.rank_level.max_bandwidth_gbps * scale;
            constraints.row_buffer_size_bytes = 0;
            constraints.shared_bus_pes =
                dram_arch.organization.chips_per_rank *
                dram_arch.organization.banks_per_bank_group *
                dram_arch.organization.bank_groups_per_chip;
            constraints.has_dedicated_bus =
                dram_arch.pe_bus_constraints.rank_level.has_dedicated_bus;
            break;

        case PEPlacementLevel::LOGIC_DIE:
            // Use logic die level constraints (for HBM/HMC)
            constraints.data_bus_width_bits = static_cast<uint64_t>(
                dram_arch.pe_bus_constraints.logic_die_level.data_bus_width_bits * scale);
            constraints.max_bandwidth_gbps =
                dram_arch.pe_bus_constraints.logic_die_level.max_bandwidth_gbps * scale;
            constraints.row_buffer_size_bytes = 0;
            constraints.shared_bus_pes = 1;  // Dedicated logic die
            constraints.has_dedicated_bus =
                dram_arch.pe_bus_constraints.logic_die_level.has_dedicated_bus;
            break;

        default:
            // Return empty constraints for unknown level
            break;
    }

    return constraints;
}

MemoryHierarchy createMemoryHierarchyFromDRAM(
    const memory::DRAMArchitectureV2& dram_arch) {

    MemoryHierarchy hierarchy;

    // Populate organization from DRAM architecture
    hierarchy.num_subarrays_per_bank = dram_arch.organization.subarrays_per_bank;
    hierarchy.num_banks_per_chip =
        dram_arch.organization.banks_per_bank_group *
        dram_arch.organization.bank_groups_per_chip;
    hierarchy.num_chips_per_rank = dram_arch.organization.chips_per_rank;
    hierarchy.num_ranks = dram_arch.organization.ranks_per_channel;
    hierarchy.has_logic_die = (dram_arch.technology == "HBM2" ||
                                dram_arch.technology == "HBM3" ||
                                dram_arch.technology == "HMC");

    // Populate sizes from DRAM architecture
    hierarchy.subarray_size_bytes = dram_arch.organization.subarray_size_kb * 1024;
    hierarchy.bank_size_bytes = dram_arch.organization.bank_size_mb * 1024 * 1024;
    hierarchy.chip_size_bytes = dram_arch.organization.chip_size_mb * 1024 * 1024;
    hierarchy.rank_size_bytes = dram_arch.organization.rank_size_gb * 1024ULL * 1024 * 1024;

    return hierarchy;
}

} // namespace pimid
