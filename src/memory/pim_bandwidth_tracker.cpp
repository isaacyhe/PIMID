/**
 * @file pim_bandwidth_tracker.cpp
 * @brief Implementation of PIM Bandwidth Tracker
 */

#include "memory/pim_bandwidth_tracker.h"
#include "common/format_scope.h"  // 1.12.15: scoped stream formats
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <stdexcept>   // 1.11.57 (latent D070): the refusal below

namespace pimid {

PIMBandwidthTracker::PIMBandwidthTracker(
    std::shared_ptr<pimid::memory::DRAMArchitectureV2> dram_arch)
    : dram_arch_(dram_arch),
      num_channels_(0),
      num_ranks_(0),
      num_bank_groups_(0),
      num_banks_(0),
      num_subarrays_(0),
      current_cycle_(0),
      window_start_cycle_(0),
      total_requests_(0),
      bandwidth_limited_requests_(0),
      total_bytes_transferred_(0) {
    // Initialize default queue depth limits for all granularity levels
    queue_depth_limits_[PIMGranularity::SUBARRAY] = DEFAULT_QUEUE_DEPTH_LIMIT;
    queue_depth_limits_[PIMGranularity::BANK] = DEFAULT_QUEUE_DEPTH_LIMIT;
    queue_depth_limits_[PIMGranularity::BANK_GROUP] = DEFAULT_QUEUE_DEPTH_LIMIT;
    queue_depth_limits_[PIMGranularity::CHIP] = DEFAULT_QUEUE_DEPTH_LIMIT;
    queue_depth_limits_[PIMGranularity::RANK] = DEFAULT_QUEUE_DEPTH_LIMIT;
    queue_depth_limits_[PIMGranularity::MEMORY_CONTROLLER] = DEFAULT_QUEUE_DEPTH_LIMIT;
    queue_depth_limits_[PIMGranularity::CPU] = 0; // Unlimited for CPU
}

void PIMBandwidthTracker::initialize(int num_channels, int num_ranks,
                                     int num_bank_groups, int num_banks,
                                     int num_subarrays) {
    num_channels_ = num_channels;
    num_ranks_ = num_ranks;
    num_bank_groups_ = num_bank_groups;
    num_banks_ = num_banks;
    num_subarrays_ = num_subarrays;

    initializeBandwidthLimits();
    resetStats();

    std::cout << "PIM Bandwidth Tracker Initialized:\n";
    std::cout << "  Subarray port: " << subarray_port_bits_ << " bits -> "
              << subarray_bw_limit_ << " GB/s\n";
    std::cout << "  Bank port:     " << bank_port_bits_ << " bits -> "
              << bank_bw_limit_ << " GB/s (CRITICAL BOTTLENECK!)\n";
    std::cout << "  Rank port:     " << rank_port_bits_ << " bits -> "
              << rank_bw_limit_ << " GB/s\n";
}

void PIMBandwidthTracker::initializeBandwidthLimits() {
    if (!dram_arch_) {
        /* 1.11.57 (latent D070): REFUSE, DO NOT RETURN. This printed an error
         * and returned before assigning ANY of the six bandwidth limits, the
         * six port widths or the clock -- none of which was in the
         * constructor's initializer list, so they were left indeterminate.
         * initialize() then printed three of them, and every later
         * calculateTransferLatency() DIVIDES by a bandwidth limit, so the
         * object continued running on uninitialized memory after announcing
         * its own failure. The members are zero-initialized in the header now,
         * which makes the state defined -- but 0 GB/s is a division by zero,
         * so a defined state is not enough: a tracker with no architecture
         * cannot answer any question it exists to answer, and it says so by
         * failing here rather than by producing infinities later. */
        throw std::runtime_error(
            "[PIMBandwidthTracker] refuses to initialize: no DRAM "
            "architecture object was supplied, so no port width, bandwidth "
            "limit or clock can be sourced. Every bandwidth answer from this "
            "object would be fabricated.");
    }

    // Get clock frequency
    clock_freq_GHz_ = dram_arch_->timing.clock_freq_mhz / 1000.0;

    // CRITICAL: Use our verified internal port bitwidths!

    // Subarray: GSA datapath (256 bits for DDR4, 512 for HBM2)
    subarray_port_bits_ = dram_arch_->datapath.gsa_datapath_bits.value_bits;

    // Bank: CRITICAL BOTTLENECK - bank serialization (8 bits DDR4, 64 bits HBM2)
    bank_port_bits_ = dram_arch_->datapath.bank_serialization_bits.value_bits;

    // Rank: Standard wide interface (64 bits DDR4, 128 bits HBM2)
    rank_port_bits_ = dram_arch_->datapath.rank_databus_bits.value_bits;

    // Chip: External I/O (8 bits for x8 DDR4, 1024 bits for HBM2)
    chip_port_bits_ = dram_arch_->datapath.chip_io_bits.value_bits;

    /* 1.11.57 (latent D071): THE SAME ASSERTION LIVED IN TWO CLASSES.
     * "bank group port = bank port x 2" is also RamulatorWrapper::
     * getBankGroupPortBits(), which since 1.11.56 sets hierarchy level 2's
     * link width in every DRAM run. Two copies of one unsourced multiplier,
     * in two objects built from the same architecture object, is the exact
     * shape of the drift this audit round is chasing -- and here the copies
     * would not even have been comparable, because this one is read by the
     * PIM bandwidth model and that one by the hierarchy builder. Neither is a
     * specification value: a bank group is not an interface boundary and no
     * JEDEC document fixes its port width. Kept at the same value (changing it
     * would move live level-2 link widths through the other copy), named as
     * unsourced in both places, and marked here as the copy to delete when
     * DRAMArchitectureV2 grows a real bank_group_port_bits field. */
    bank_group_port_bits_ = bank_port_bits_ * 2;   // UNSOURCED -- see above

    // Memory controller: Same as rank typically
    mc_port_bits_ = rank_port_bits_;

    // Calculate bandwidth limits
    subarray_bw_limit_ = calculateBandwidth(subarray_port_bits_, clock_freq_GHz_);
    bank_bw_limit_ = calculateBandwidth(bank_port_bits_, clock_freq_GHz_);
    bank_group_bw_limit_ = calculateBandwidth(bank_group_port_bits_, clock_freq_GHz_);
    chip_bw_limit_ = calculateBandwidth(chip_port_bits_, clock_freq_GHz_);
    rank_bw_limit_ = calculateBandwidth(rank_port_bits_, clock_freq_GHz_);
    mc_bw_limit_ = rank_bw_limit_;

    std::cout << "\nVerified Port Bitwidths from DRAM Architecture:\n";
    std::cout << "  Subarray (GSA):        " << subarray_port_bits_
              << " bits (" << dram_arch_->datapath.gsa_datapath_bits.source << ")\n";
    std::cout << "  Bank (serialization):  " << bank_port_bits_
              << " bits (" << dram_arch_->datapath.bank_serialization_bits.source << ")\n";
    std::cout << "  Rank (interface):      " << rank_port_bits_
              << " bits (" << dram_arch_->datapath.rank_databus_bits.source << ")\n";
}

uint64_t PIMBandwidthTracker::requestBandwidth(const PIMRequestPayload& payload,
                                              uint64_t required_bytes) {
    total_requests_++;
    requests_per_level_[payload.granularity]++;
    bytes_per_level_[payload.granularity] += required_bytes;
    total_bytes_transferred_ += required_bytes;

    // Track queue depth
    auto key = std::make_pair(payload.granularity, payload.target_bank);
    active_queue_depths_[key]++;

    // Calculate transfer latency based on granularity and contention
    uint64_t latency = calculateTransferLatency(payload.granularity,
                                                payload.target_bank,
                                                required_bytes);

    // Update usage statistics
    updateUsageStats(payload.granularity, payload.target_bank,
                    required_bytes, latency);

    return latency;
}

bool PIMBandwidthTracker::canAcceptRequest(const PIMRequestPayload& payload,
                                           uint64_t required_bytes) {
    size_t limit = getQueueDepthLimit(payload.granularity);

    // If limit is 0, queue is unlimited
    if (limit == 0) {
        return true;
    }

    size_t current_depth = getCurrentQueueDepth(payload.granularity, payload.target_bank);
    return current_depth < limit;
}

void PIMBandwidthTracker::tick() {
    current_cycle_++;
    checkStatsWindow();
}

double PIMBandwidthTracker::getBandwidthLimit(PIMGranularity granularity) const {
    switch (granularity) {
        case PIMGranularity::SUBARRAY:
            return subarray_bw_limit_;
        case PIMGranularity::BANK:
            return bank_bw_limit_;
        case PIMGranularity::BANK_GROUP:
            return bank_group_bw_limit_;
        case PIMGranularity::CHIP:
            return chip_bw_limit_;
        case PIMGranularity::RANK:
        case PIMGranularity::MEMORY_CONTROLLER:
            return rank_bw_limit_;
        case PIMGranularity::CPU:
        default:
            return mc_bw_limit_;
    }
}

int PIMBandwidthTracker::getPortBitwidth(PIMGranularity granularity) const {
    switch (granularity) {
        case PIMGranularity::SUBARRAY:
            return subarray_port_bits_;
        case PIMGranularity::BANK:
            return bank_port_bits_;
        case PIMGranularity::BANK_GROUP:
            return bank_group_port_bits_;
        case PIMGranularity::CHIP:
            return chip_port_bits_;
        case PIMGranularity::RANK:
        case PIMGranularity::MEMORY_CONTROLLER:
            return rank_port_bits_;
        case PIMGranularity::CPU:
        default:
            return mc_port_bits_;
    }
}

void PIMBandwidthTracker::registerPE(PIMGranularity granularity,
                                    int pe_id, int target_bank) {
    auto key = std::make_pair(granularity, target_bank);
    pe_registry_[key].push_back(pe_id);

    std::cout << "Registered PE " << pe_id << " at "
              << (int)granularity << " level, target bank " << target_bank
              << " (now " << pe_registry_[key].size() << " PEs sharing)\n";
}

int PIMBandwidthTracker::getConcurrentPEs(PIMGranularity granularity,
                                         int target_id) const {
    auto key = std::make_pair(granularity, target_id);
    auto it = pe_registry_.find(key);
    if (it != pe_registry_.end()) {
        return it->second.size();
    }
    return 1;  // Default: 1 PE
}

double PIMBandwidthTracker::getEffectiveBandwidthPerPE(PIMGranularity granularity,
                                                      int target_id) const {
    double total_bw = getBandwidthLimit(granularity);
    int num_pes = getConcurrentPEs(granularity, target_id);

    if (num_pes > 1) {
        return total_bw / num_pes;
    }
    return total_bw;
}

uint64_t PIMBandwidthTracker::calculateTransferLatency(PIMGranularity granularity,
                                                       int target_id,
                                                       uint64_t bytes) {
    // Get effective bandwidth per PE (considering sharing)
    double effective_bw_GBs = getEffectiveBandwidthPerPE(granularity, target_id);

    // Calculate cycles needed
    // latency = (bytes / BW_GB/s) / (cycle_time_ns)
    // cycle_time_ns = 1 / (freq_GHz)
    // So: latency_cycles = (bytes / BW_GB/s) * freq_GHz * 1e9 / 1e9
    //                    = (bytes / BW_GB/s) * freq_GHz

    double latency_ns = (bytes / effective_bw_GBs);  // nanoseconds
    uint64_t latency_cycles = std::ceil(latency_ns * clock_freq_GHz_);

    // Check if bandwidth limited
    int num_pes = getConcurrentPEs(granularity, target_id);
    if (num_pes > 1) {
        bandwidth_limited_requests_++;
    }

    return std::max(latency_cycles, (uint64_t)1);
}

void PIMBandwidthTracker::updateUsageStats(PIMGranularity granularity,
                                          int target_id,
                                          uint64_t bytes,
                                          uint64_t cycles) {
    auto key = std::make_pair(granularity, target_id);

    bandwidth_usage_[key].bytes_transferred += bytes;
    bandwidth_usage_[key].cycles_active += cycles;
    bandwidth_usage_[key].num_concurrent_pes = getConcurrentPEs(granularity, target_id);

    // Calculate utilization
    if (current_cycle_ > window_start_cycle_) {
        uint64_t window_cycles = current_cycle_ - window_start_cycle_;
        bandwidth_usage_[key].utilization =
            static_cast<double>(bandwidth_usage_[key].cycles_active) / window_cycles;
    }
}

void PIMBandwidthTracker::checkStatsWindow() {
    if (current_cycle_ - window_start_cycle_ >= STATS_WINDOW_CYCLES) {
        // Reset window
        window_start_cycle_ = current_cycle_;

        // Clear usage stats for new window
        for (auto& [key, usage] : bandwidth_usage_) {
            usage.bytes_transferred = 0;
            usage.cycles_active = 0;
            usage.utilization = 0.0;
        }
    }
}

void PIMBandwidthTracker::printStats() const {
    std::cout << "\n+==================================================================+\n";
    std::cout << "| PIM Bandwidth Tracker Statistics                                 |\n";
    std::cout << "+==================================================================+\n\n";

    std::cout << "Total Requests:            " << total_requests_ << "\n";
    std::cout << "Bandwidth-Limited:         " << bandwidth_limited_requests_
              << " (" << (total_requests_ > 0 ?
                        100.0 * bandwidth_limited_requests_ / total_requests_ : 0.0)
              << "%)\n";
    std::cout << "Total Bytes Transferred:   " << total_bytes_transferred_ << " B ("
              << total_bytes_transferred_ / (1024.0 * 1024) << " MB)\n\n";

    std::cout << "Requests per Granularity Level:\n";
    std::cout << "--------------------------------\n";
    for (const auto& [granularity, count] : requests_per_level_) {
        PIMRequestPayload dummy;
        dummy.granularity = granularity;
        std::cout << "  " << std::setw(20) << std::left << dummy.getGranularityName()
                  << ": " << count << " requests, "
                  << bytes_per_level_.at(granularity) / (1024.0 * 1024) << " MB\n";
    }

    std::cout << "\nPort Utilization:\n";
    std::cout << "-----------------\n";
    for (const auto& [key, usage] : bandwidth_usage_) {
        PIMRequestPayload dummy;
        dummy.granularity = key.first;
        pimid::fmt_scoped(std::cout) << "  " << std::setw(20) << std::left << dummy.getGranularityName()
                  << " [ID " << key.second << "]: "
                  << std::fixed << std::setprecision(2)
                  << usage.utilization * 100.0 << "% ("
                  << usage.num_concurrent_pes << " PEs)\n";
    }

    /* 1.11.56 (audit D073): the "<-" markers here and the unit arrows above
     * were U+2192/U+2190, against the project's ASCII-only rule. Text only. */
    std::cout << "\nBandwidth Limits:\n";
    std::cout << "-----------------\n";
    std::cout << "  Subarray:      " << subarray_bw_limit_ << " GB/s\n";
    std::cout << "  Bank:          " << bank_bw_limit_ << " GB/s  <- BOTTLENECK in DDR4!\n";
    std::cout << "  Bank Group:    " << bank_group_bw_limit_ << " GB/s\n";
    std::cout << "  Chip:          " << chip_bw_limit_ << " GB/s\n";
    std::cout << "  Rank:          " << rank_bw_limit_ << " GB/s\n";
    std::cout << "\n";
}

void PIMBandwidthTracker::resetStats() {
    total_requests_ = 0;
    bandwidth_limited_requests_ = 0;
    total_bytes_transferred_ = 0;
    requests_per_level_.clear();
    bytes_per_level_.clear();
    bandwidth_usage_.clear();
    active_transfers_.clear();
    window_start_cycle_ = current_cycle_;
}

/* 1.11.57 (latent D072): calculateLocalCapacity() is DELETED.
 *
 * It answered "how much memory can a PE at this level reach" by dividing a
 * hardcoded 8 GB rank -- written out four separate times, plus a fifth
 * "return 8 GB" and a sixth literal "int num_chips = 8; // Typical DDR4" --
 * by the organization counts. The capacity is not a constant: this class
 * holds the architecture object, whose organization block carries the real
 * chip and bank sizes, and the memory system's capacity is configured per run
 * (RamulatorWrapper reports 8 GB for the DDR presets and 4 GB for the HBM
 * ones, so the literal is wrong by 2x for HBM before any hierarchy division).
 * It had no callers anywhere in src/ or include/ -- only its definition and
 * its declaration -- so no reachable number was ever divided by a fabricated
 * rank. Deleted rather than rewritten because "PE reach" is a placement
 * question that the address-translation layer owns, not a bandwidth tracker,
 * and reviving it here would recreate a second answer to it.
 *
 * determineDataLocality() is DELETED with it: same story, no callers, and it
 * exists only to feed the locality question calculateLocalCapacity framed.
 * calculateDataReach() below is kept -- it takes the data distribution as an
 * argument instead of inventing one. */

void PIMBandwidthTracker::calculateDataReach(
    const PIMRequestPayload& payload,
    uint64_t total_data_bytes,
    const std::map<int, uint64_t>& data_distribution,
    uint64_t& local_bytes,
    uint64_t& remote_bytes) const {

    local_bytes = 0;
    remote_bytes = 0;

    int pe_bank = getPELocalBank(payload.granularity, payload.pe_id);
    int pe_bg = getPEBankGroup(payload.pe_id);
    int pe_chip = getPEChip(payload.pe_id);

    for (const auto& [bank_id, bytes] : data_distribution) {
        // Determine bank's BG and chip
        int data_bg = bank_id / (num_banks_ / num_bank_groups_);
        int data_chip = bank_id / (num_banks_ / 8); // Assume 8 chips

        DataLocality locality = PIMRequestPayload::calculateLocality(
            payload.granularity,
            pe_bank, pe_bg, pe_chip,
            bank_id, data_bg, data_chip);

        // Determine what's "local" based on granularity
        bool is_local = false;
        switch (payload.granularity) {
            case PIMGranularity::BANK:
            case PIMGranularity::SUBARRAY:
                // Only same bank is local
                is_local = (locality == DataLocality::LOCAL);
                break;
            case PIMGranularity::BANK_GROUP:
                // Same BG is local
                is_local = (locality == DataLocality::LOCAL ||
                          locality == DataLocality::REMOTE_SAME_BG);
                break;
            case PIMGranularity::CHIP:
                // Same chip is local
                is_local = (locality != DataLocality::REMOTE_SAME_RANK &&
                          locality != DataLocality::REMOTE_EXTERNAL);
                break;
            case PIMGranularity::RANK:
            case PIMGranularity::MEMORY_CONTROLLER:
                // Entire rank is local
                is_local = (locality != DataLocality::REMOTE_EXTERNAL);
                break;
            case PIMGranularity::CPU:
                // All data accessible (though may be slow)
                is_local = true;
                break;
        }

        if (is_local) {
            local_bytes += bytes;
        } else {
            remote_bytes += bytes;
        }
    }
}

int PIMBandwidthTracker::getPELocalBank(PIMGranularity granularity, int pe_id) const {
    // Find which bank this PE is registered to
    for (const auto& [key, pe_list] : pe_registry_) {
        if (key.first == granularity) {
            for (int id : pe_list) {
                if (id == pe_id) {
                    return key.second; // target_id is the bank
                }
            }
        }
    }
    // Default: distribute PEs across banks
    return pe_id % num_banks_;
}

int PIMBandwidthTracker::getPEBankGroup(int pe_id) const {
    int pe_bank = getPELocalBank(PIMGranularity::BANK, pe_id);
    // Assuming banks_per_BG = num_banks / num_bank_groups
    int banks_per_bg = num_banks_ / num_bank_groups_;
    return pe_bank / banks_per_bg;
}

int PIMBandwidthTracker::getPEChip(int pe_id) const {
    int pe_bank = getPELocalBank(PIMGranularity::BANK, pe_id);
    // Assuming 8 chips, banks distributed evenly
    int banks_per_chip = num_banks_ / 8;
    return pe_bank / banks_per_chip;
}

void PIMBandwidthTracker::setQueueDepthLimit(PIMGranularity granularity, size_t limit) {
    queue_depth_limits_[granularity] = limit;
}

void PIMBandwidthTracker::setGlobalQueueDepthLimit(size_t limit) {
    queue_depth_limits_[PIMGranularity::SUBARRAY] = limit;
    queue_depth_limits_[PIMGranularity::BANK] = limit;
    queue_depth_limits_[PIMGranularity::BANK_GROUP] = limit;
    queue_depth_limits_[PIMGranularity::CHIP] = limit;
    queue_depth_limits_[PIMGranularity::RANK] = limit;
    queue_depth_limits_[PIMGranularity::MEMORY_CONTROLLER] = limit;
    queue_depth_limits_[PIMGranularity::CPU] = limit;
}

size_t PIMBandwidthTracker::getQueueDepthLimit(PIMGranularity granularity) const {
    auto it = queue_depth_limits_.find(granularity);
    if (it != queue_depth_limits_.end()) {
        return it->second;
    }
    return DEFAULT_QUEUE_DEPTH_LIMIT;
}

size_t PIMBandwidthTracker::getCurrentQueueDepth(PIMGranularity granularity, int target_id) const {
    auto key = std::make_pair(granularity, target_id);
    auto it = active_queue_depths_.find(key);
    if (it != active_queue_depths_.end()) {
        return it->second;
    }
    return 0;
}

void PIMBandwidthTracker::completeRequest(const PIMRequestPayload& payload) {
    auto key = std::make_pair(payload.granularity, payload.target_bank);
    auto it = active_queue_depths_.find(key);
    if (it != active_queue_depths_.end() && it->second > 0) {
        it->second--;
    }
}

} // namespace pimid
