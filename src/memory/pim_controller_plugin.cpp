/**
 * @file pim_controller_plugin.cpp
 * @brief Ramulator Controller Plugin for PIM Support
 *
 * This implements a Ramulator IControllerPlugin that adds PIM support
 * to Ramulator2's memory controller. It integrates:
 * - Bandwidth tracking at each DRAM level
 * - Internal DRAM network modeling
 * - Port bitwidth constraint enforcement
 * - PE placement and contention tracking
 *
 * IMPORTANT: This plugin does NOT modify DRAM correctness!
 * - DRAM timing constraints are still enforced by Ramulator
 * - Commands (ACT, PRE, RD, WR) work identically
 * - We only ADD latency for data movement based on port BW
 */

#include "memory/pim_controller_plugin.h"
#include "common/format_scope.h"  // 1.12.15: scoped stream formats
#include <iostream>
#include <iomanip>

namespace pimid {

PIMControllerPlugin::PIMControllerPlugin(
    std::shared_ptr<pimid::memory::DRAMArchitectureV2> dram_arch,
    const std::string& dram_type)
    : dram_arch_(dram_arch),
      dram_type_(dram_type),
      current_cycle_(0),
      total_pim_requests_(0),
      total_normal_requests_(0) {

    // Create bandwidth tracker
    bandwidth_tracker_ = std::make_shared<PIMBandwidthTracker>(dram_arch_);

    // Internal network will be created after we know DRAM organization
    internal_network_ = nullptr;
}

PIMControllerPlugin::~PIMControllerPlugin() {
    // Print final statistics
    printStats();
}

void PIMControllerPlugin::initialize(int num_channels, int num_ranks,
                                     int num_bank_groups, int num_banks,
                                     int num_subarrays) {
    // Initialize bandwidth tracker
    bandwidth_tracker_->initialize(num_channels, num_ranks,
                                   num_bank_groups, num_banks, num_subarrays);

    /* 1.11.57 (latent D019): the chips-per-rank literal 8 is gone. It was the
     * fourth copy of the DDR-x8 population in the tree, stated as "(typical)"
     * inside a function that has the architecture object in hand -- and it is
     * simply wrong for HBM2/HBM3, which put one die behind each channel, and
     * for any x4 or x16 part. The architecture object resolves it. Invisible
     * because PIMControllerPlugin is never constructed: its only creator is
     * RamulatorWrapper::initializePIMComponents, reached only from
     * enablePIMSupport(), which has no callers. */
    int chips_per_rank = 8;
    if (dram_arch_ && dram_arch_->organization.chips_per_rank > 0) {
        chips_per_rank = static_cast<int>(dram_arch_->organization.chips_per_rank);
    } else {
        std::cerr << "[PIMControllerPlugin] WARNING: no DRAM architecture "
                     "object, so chips-per-rank falls back to the DDR-x8 "
                     "literal 8. That is a SUBSTITUTED organization, wrong for "
                     "HBM (one die per channel) and for x4/x16 parts."
                  << std::endl;
    }

    // Create internal DRAM network
    internal_network_ = createInternalDRAMNetwork(
        dram_type_,
        num_subarrays,      // subarrays per bank
        num_banks / num_bank_groups,  // banks per bank group
        num_bank_groups,    // bank groups per chip
        chips_per_rank      // 1.11.57 (latent D019): was the literal 8
    );

    std::cout << "PIM Controller Plugin initialized for " << dram_type_ << "\n";
    std::cout << "  Channels: " << num_channels << ", Ranks: " << num_ranks << "\n";
    std::cout << "  Bank Groups: " << num_bank_groups << ", Banks: " << num_banks << "\n";
    std::cout << "  Subarrays per bank: " << num_subarrays << "\n";
}

void PIMControllerPlugin::update(bool request_found, Ramulator::ReqBuffer::iterator& req_it) {
    // This is called by Ramulator controller after scheduling
    // We use it to:
    // 1. Track PIM-specific bandwidth usage
    // 2. Model internal network transfers
    // 3. Add data movement latency

    if (!request_found) {
        return;  // No request to process
    }

    // Check if this request has PIM payload
    if (req_it->m_payload == nullptr) {
        // Normal DRAM request
        total_normal_requests_++;
        return;
    }

    // Extract PIM payload
    auto* pim_payload = static_cast<PIMRequestPayload*>(req_it->m_payload);

    // Handle based on operation type
    if (pim_payload->operation == PIMOperationType::NORMAL_READ ||
        pim_payload->operation == PIMOperationType::NORMAL_WRITE) {
        // Normal operation, no special handling
        total_normal_requests_++;
        return;
    }

    // PIM operation detected!
    total_pim_requests_++;

    // Calculate data movement latency based on port bandwidth
    // Safety check: ensure bandwidth_tracker_ is initialized
    uint64_t data_movement_latency = 0;
    if (bandwidth_tracker_) {
        data_movement_latency = bandwidth_tracker_->requestBandwidth(
            *pim_payload, pim_payload->data_bytes);
    } else {
        /* 1.11.57 (latent D068): THREE ERRORS IN ONE FALLBACK.
         *
         * (a) THE MAGNITUDE. The old expression was
         *       (data_bytes / 10.0 GB/s) * 1e9   // "Convert to ns"
         * but bytes divided by GB/s is ALREADY nanoseconds: GB/s is 1e9
         * bytes/s, so bytes/(10 x 1e9 bytes/s) = seconds, and multiplying the
         * whole quotient by 1e9 converts seconds to ns exactly once -- except
         * the division by 10.0 was written as if 10.0 were bytes-per-ns, so
         * the 1e9 was applied a second time. A 64 B transfer came out as
         * 6.4e9 instead of 6.4 ns: a factor of 1e9.
         *
         * (b) THE UNIT. The result, whatever its magnitude, is NANOSECONDS,
         * and it was assigned to pim_payload->data_movement_cycles, which the
         * request-completion path adds to a cycle count
         * (RamulatorWrapper::sendPIM sums depart-arrive with
         * data_movement_cycles and network_cycles). The bandwidth-tracker
         * branch beside it returns CYCLES (calculateTransferLatency multiplies
         * ns by clock_freq_GHz_), so the two branches of one if/else filled
         * the same field with two different quantities.
         *
         * (c) THE INVENTED 10 GB/s. The architecture object this plugin holds
         * carries the bank-level effective bandwidth for the part being
         * simulated; DDR4's is about 1.2 GB/s, not 10, and the whole point of
         * bank-level PIM modelling is that this number is the bottleneck.
         *
         * Invisible because PIMControllerPlugin is never constructed, and
         * within it this branch needs bandwidth_tracker_ to be null, which the
         * constructor makes impossible. */
        double bw_GBps = 0.0;
        double clock_GHz = 0.0;
        if (dram_arch_) {
            // 1.11.57 (audit C003): derived from this object's serialization
            // width and core clock, no longer a stored per-technology literal.
            bw_GBps  = dram_arch_->getBankEffectiveBW();
            clock_GHz = dram_arch_->timing.clock_freq_mhz / 1000.0;
        }
        if (bw_GBps > 0.0 && clock_GHz > 0.0) {
            const double latency_ns = pim_payload->data_bytes / bw_GBps;
            data_movement_latency =
                static_cast<uint64_t>(latency_ns * clock_GHz);   // ns -> cycles
        } else {
            static bool warned_fallback = false;
            if (!warned_fallback) {
                warned_fallback = true;
                std::cerr << "[PIMControllerPlugin] WARNING: no bandwidth "
                             "tracker and no DRAM architecture, so PIM data "
                             "movement cannot be priced. Charging 0 cycles -- "
                             "unmodelled, not free." << std::endl;
            }
            data_movement_latency = 0;
        }
    }

    pim_payload->data_movement_cycles = data_movement_latency;

    // If operation requires internal network, model it
    if (pim_payload->requiresInternalNetwork()) {
        for (const auto& transfer : pim_payload->internal_transfers) {
            if (transfer.requires_network) {
                handleInternalTransfer(transfer, pim_payload);
            }
        }
    }

    // Add total latency to request
    // Note: We don't directly modify Ramulator's timing!
    // Instead, we track it in the payload for PIMID's use
    pim_payload->data_movement_cycles += pim_payload->network_cycles;
}

void PIMControllerPlugin::tick() {
    current_cycle_++;

    // Tick bandwidth tracker
    if (bandwidth_tracker_) {
        bandwidth_tracker_->tick();
    }

    // Tick internal network
    if (internal_network_) {
        internal_network_->tick();
    }
}

void PIMControllerPlugin::registerPE(PIMGranularity granularity,
                                    int pe_id, int target_bank) {
    if (bandwidth_tracker_) {
        bandwidth_tracker_->registerPE(granularity, pe_id, target_bank);
    }

    /* 1.11.57 (latent D069): THE LOG LINE NAMED THE WRONG LEVEL, ALWAYS. It
     * read PIMRequestPayload().getGranularityName() -- a DEFAULT-CONSTRUCTED
     * payload, whose granularity member is initialised to PIMGranularity::CPU
     * -- so every registration, at every level, printed "at CPU level" while
     * the granularity argument sitting in scope was ignored. Registering a
     * bank-level PE and reading back "CPU" in the log is not a cosmetic
     * defect: this line is the only record of where a PE was placed, and CPU
     * placement is the baseline the PIM results are compared against.
     * Invisible because PIMControllerPlugin is never constructed. */
    PIMRequestPayload named;
    named.granularity = granularity;
    std::cout << "Registered PIM PE " << pe_id << " at "
              << named.getGranularityName()
              << " level, bank " << target_bank << "\n";
}

void PIMControllerPlugin::handleInternalTransfer(
    const InternalDRAMTransfer& transfer,
    PIMRequestPayload* payload) {

    if (!internal_network_) {
        std::cerr << "WARNING: Internal network not initialized!\n";
        return;
    }

    // Create network packet
    InternalNetworkPacket packet;
    packet.packet_id = total_pim_requests_;
    packet.source_bank = transfer.source_bank;
    packet.dest_bank = transfer.dest_bank;
    packet.source_subarray = transfer.source_subarray;
    packet.dest_subarray = transfer.dest_subarray;
    packet.data_bytes = transfer.transfer_bytes;
    packet.completed = false;

    // Set callback to update payload when transfer completes
    packet.callback = [payload]() {
        if (payload->pim_completion_callback) {
            payload->pim_completion_callback();
        }
    };

    // Send packet through internal network
    if (internal_network_->sendPacket(packet)) {
        // Estimate network latency (detailed calculation in network model)
        NetworkLevel level = NetworkLevel::BANK_NETWORK;
        if (transfer.source_bank == transfer.dest_bank) {
            level = NetworkLevel::SUBARRAY_NETWORK;
        }

        uint64_t network_latency = internal_network_->getTransferLatency(
            level, transfer.source_bank, transfer.dest_bank,
            transfer.transfer_bytes);

        payload->network_cycles += network_latency;
    } else {
        std::cerr << "WARNING: Internal network congested!\n";
    }
}

double PIMControllerPlugin::getBandwidthLimit(PIMGranularity granularity) const {
    if (bandwidth_tracker_) {
        return bandwidth_tracker_->getBandwidthLimit(granularity);
    }
    return 0.0;
}

int PIMControllerPlugin::getPortBitwidth(PIMGranularity granularity) const {
    if (bandwidth_tracker_) {
        return bandwidth_tracker_->getPortBitwidth(granularity);
    }
    return 0;
}

double PIMControllerPlugin::getEffectiveBandwidthPerPE(PIMGranularity granularity,
                                                       int target_id) const {
    if (bandwidth_tracker_) {
        return bandwidth_tracker_->getEffectiveBandwidthPerPE(granularity, target_id);
    }
    return 0.0;
}

void PIMControllerPlugin::printStats() const {
    std::cout << "\n+==================================================================+\n";
    std::cout << "| PIM Controller Plugin Statistics                                 |\n";
    std::cout << "+==================================================================+\n\n";

    std::cout << "Total Cycles:          " << current_cycle_ << "\n";
    std::cout << "Total PIM Requests:    " << total_pim_requests_ << "\n";
    std::cout << "Total Normal Requests: " << total_normal_requests_ << "\n";

    if (total_pim_requests_ + total_normal_requests_ > 0) {
        double pim_ratio = 100.0 * total_pim_requests_ /
                          (total_pim_requests_ + total_normal_requests_);
        pimid::fmt_scoped(std::cout) << "PIM Request Ratio:     " << std::fixed << std::setprecision(2)
                  << pim_ratio << "%\n";
    }

    // Print bandwidth tracker stats
    if (bandwidth_tracker_) {
        bandwidth_tracker_->printStats();
    }

    // Print internal network stats
    if (internal_network_) {
        internal_network_->printStats();
    }
}

void PIMControllerPlugin::resetStats() {
    current_cycle_ = 0;
    total_pim_requests_ = 0;
    total_normal_requests_ = 0;

    if (bandwidth_tracker_) {
        bandwidth_tracker_->resetStats();
    }

    if (internal_network_) {
        internal_network_->resetStats();
    }
}

} // namespace pimid
