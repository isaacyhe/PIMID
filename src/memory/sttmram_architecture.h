/**
 * @file sttmram_architecture.h
 * @brief STT-MRAM Architecture Specifications for PIM
 *
 * This file contains STT-MRAM (Spin-Transfer Torque MRAM) architectural
 * parameters based on NVSim models and research papers.
 *
 * HIERARCHY:
 * Chip -> Bank -> Subarray -> MTJ Cell Array
 *
 * KEY CHARACTERISTICS:
 * - Non-volatile (retains data without power)
 * - Asymmetric R/W (read: 3-7ns, write: 13-27ns)
 * - No refresh needed
 * - Good endurance (10^12-10^15 cycles)
 * - MTJ (Magnetic Tunnel Junction) cells
 *
 * SOURCES:
 * - NVSim (external/nvsim/)
 * - SMART STT-MRAM paper (MEMSYS 2019)
 * - INNER_BANK_TIMING_ALL_MEMORY_TECH.md
 *
 * VERSION: 1.0
 * DATE: November 2024
 */

#ifndef PIMID_STTMRAM_ARCHITECTURE_H
#define PIMID_STTMRAM_ARCHITECTURE_H

#include <string>
#include <memory>
#include <iostream>
#include "sram_architecture.h"  // For VerificationStatus enum

namespace pimid {
namespace memory {

/* 1.11.24: the hand-written create*() default factories that used to
 * live in this header are DELETED. They existed only as a fallback for
 * "tool characterization failed", and that fallback let unsourced specs
 * reach a result indistinguishable from a real CACTI/NVSim read. The
 * model now REFUSES when its tool binding fails. The structs below are
 * the plugin CONTRACT -- what a memory technology must report -- and
 * are filled by the extractor from the tool, never by hand. */


// VerificationStatus is defined in sram_architecture.h

//=============================================================================
// STT-MRAM Hierarchy
//=============================================================================

/**
 * @brief STT-MRAM Hierarchy
 *
 * STT-MRAM can be standalone chip or embedded:
 *
 * Level 1: Chip
 *   - 32-64 banks (organized as grid, e.g., 8x8)
 *   - Global row decoder
 *   - H-tree network
 *
 * Level 2: Bank
 *   - Multiple subarrays
 *   - Local row decoder
 *   - WL (Wordline) drivers
 *   - SL/BL switch matrix
 *
 * Level 3: Subarray
 *   - MTJ cell array
 *   - Sense amplifiers
 *   - Column decoder
 *
 * Note: No bank groups (unlike DRAM)
 */
struct STTMRAMOrganization {
    // Chip level
    int banks_per_chip;           // 32-64 typical
    int bank_rows;                // Grid organization (e.g., 8x8)
    int bank_cols;
    size_t chip_size_mb;

    // Bank level
    int mats_per_bank;       // 4-16 typical
    size_t bank_size_kb;

    // Subarray level (mat)
    int wordlines_per_subarray;   // Rows (e.g., 512-1024)
    int bitlines_per_subarray;    // Columns (e.g., 1024-2048)
    size_t mat_size_kb;

    int getBanksPerChip() const {
        return bank_rows * bank_cols;
    }
};

//=============================================================================
// STT-MRAM Inner-Bank Timing
//=============================================================================

/**
 * @brief STT-MRAM Inner-Bank Datapath Timing
 *
 * Based on NVSim models and SMART STT-MRAM architecture.
 *
 * CRITICAL: Read and write have VERY different latencies!
 * - Read: 3-7ns (sensing MTJ resistance)
 * - Write: 13-27ns (MTJ switching + verify)
 */
struct STTMRAMInnerBankTiming {
    // ===== READ PATH =====

    // Row path
    double row_decoder_ns;        // Row address decode
    double wordline_ns;           // WL activation

    // Column path (READ)
    double bitline_read_ns;       // MTJ resistance sensing
    double sense_amp_ns;          // Amplify small signal
    double column_mux_ns;         // Column selection
    double subarray_output_drv_ns;

    // Inner-bank datapath
    double local_io_ns;           // Local data lines
    double htree_horizontal_ns;   // H-tree routing
    double htree_vertical_ns;
    double global_io_ns;          // Bank-level I/O
    double bank_output_drv_ns;

    // ===== WRITE PATH =====

    double bitline_write_ns;      // Set WL + apply write voltage
    double mtj_switching_ns;      // MTJ state switching (SLOW!)
    double write_verify_ns;       // Read back to verify

    // Verification
    VerificationStatus verification_status;
    std::string source;

    // Derived totals
    double getReadRowPath() const {
        return row_decoder_ns + wordline_ns;
    }

    double getReadColumnPath() const {
        return bitline_read_ns + sense_amp_ns + column_mux_ns +
               subarray_output_drv_ns;
    }

    double getInnerBankDatapath() const {
        return local_io_ns + htree_horizontal_ns + htree_vertical_ns +
               global_io_ns + bank_output_drv_ns;
    }

    double getTotalReadLatency() const {
        return getReadRowPath() + getReadColumnPath() + getInnerBankDatapath();
    }

    double getTotalWriteLatency() const {
        // Write includes: WL activation + write pulse + MTJ switching + verify
        return row_decoder_ns + wordline_ns + bitline_write_ns +
               mtj_switching_ns + write_verify_ns;
    }

    // For PIM
    double getSubarrayToSubarrayHTree() const {
        return 2.0 * (htree_horizontal_ns + htree_vertical_ns);
    }
};

//=============================================================================
// STT-MRAM Access Timing
//=============================================================================

struct STTMRAMTiming {
    double clock_freq_ghz;

    // READ latencies
    double mat_read_ns;
    double bank_read_ns;
    double chip_read_ns;

    // WRITE latencies (much slower!)
    double mat_write_ns;
    double bank_write_ns;
    double chip_write_ns;

    // Inner-bank breakdown
    STTMRAMInnerBankTiming inner_bank;

    // Read/Write asymmetry ratio
    double getWriteToReadRatio() const {
        return mat_write_ns / mat_read_ns;
    }
};

//=============================================================================
// STT-MRAM Energy
//=============================================================================

struct STTMRAMEnergy {
    // Read energy (low)
    double mat_read_energy_pJ;
    double bank_read_energy_pJ;
    double chip_read_energy_pJ;

    // Write energy (HIGH!)
    double mat_write_energy_pJ;
    double bank_write_energy_pJ;
    double chip_write_energy_pJ;

    // Per-byte energy
    double read_energy_per_byte;
    double write_energy_per_byte;

    // Leakage (very low - non-volatile!)
    double mat_leakage_mw;
    double bank_leakage_mw;
    double chip_leakage_mw;

    std::string energy_source;

    double getWriteToReadEnergyRatio() const {
        return mat_write_energy_pJ / mat_read_energy_pJ;
    }
};

//=============================================================================
// STT-MRAM Endurance
//=============================================================================

struct STTMRAMEndurance {
    double write_cycles;          // 10^12 - 10^15 typical
    double retention_years;       // 10+ years
    bool ecc_required;            // Error correction

    std::string endurance_source;
};

//=============================================================================
// STT-MRAM Datapath
//=============================================================================

struct STTMRAMDatapath {
    int mat_io_bits;
    int bank_io_bits;
    int chip_io_bits;             // External interface

    VerificationStatus verification_status;
    std::string source;
};

//=============================================================================
// Complete STT-MRAM Architecture
//=============================================================================

class STTMRAMArchitecture {
public:
    std::string name;
    std::string technology;
    std::string process_node;

    STTMRAMOrganization organization;
    STTMRAMTiming timing;
    STTMRAMEnergy energy;
    STTMRAMEndurance endurance;
    STTMRAMDatapath datapath;

    // Constructor
    STTMRAMArchitecture(const std::string& name, const std::string& tech)
        : name(name), technology(tech) {}

    // Print summary
    void printSummary() const;

    // PIM-specific calculations
    double getMatReadBandwidth() const {
        return (datapath.mat_io_bits / 8.0) *
               (1000.0 / timing.inner_bank.getTotalReadLatency());  // MB/s
    }

    double getMatWriteBandwidth() const {
        return (datapath.mat_io_bits / 8.0) *
               (1000.0 / timing.inner_bank.getTotalWriteLatency());  // MB/s
    }

    bool isSuitableForPIM() const {
        // Good for read-heavy PIM workloads
        return timing.getWriteToReadRatio() < 5.0;  // Write < 5x read time
    }
};

//=============================================================================
// Example: 8MB STT-MRAM Cache (22nm)
//=============================================================================


//=============================================================================
// Example: Everspin 256Mb STT-MRAM (40nm)
//=============================================================================


} // namespace memory
} // namespace pimid

#endif // PIMID_STTMRAM_ARCHITECTURE_H
