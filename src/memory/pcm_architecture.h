/**
 * @file pcm_architecture.h  
 * @brief PCM (Phase-Change Memory) Architecture Specifications for PIM
 *
 * KEY CHARACTERISTICS:
 * - Non-volatile
 * - VERY slow writes (SET: 50-150ns, RESET: 10-50ns)
 * - Moderate read latency (6-12ns)
 * - 3D stackable (e.g., Intel Optane)
 * - Bus routing (some designs, not always H-tree)
 *
 * SOURCES:
 * - NVSim (external/nvsim/)
 * - JSSC 2007 Samsung PCM paper
 * - INNER_BANK_TIMING_ALL_MEMORY_TECH.md
 *
 * VERSION: 1.0
 */

#ifndef PIMID_PCM_ARCHITECTURE_H
#define PIMID_PCM_ARCHITECTURE_H

#include <string>
#include <memory>
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

struct PCMOrganization {
    int banks_per_chip;
    int bank_rows, bank_cols;
    size_t chip_size_mb, bank_size_kb;
    int mats_per_bank;
    size_t mat_size_kb;
    int wordlines_per_mat, bitlines_per_mat;
};

struct PCMInnerBankTiming {
    // READ path
    double row_decoder_ns, wordline_ns;
    double bitline_read_ns;        // Resistance sensing (SLOW)
    double sense_amp_external_ns;  // External to mat!
    double column_mux_ns, mat_output_drv_ns;
    double bus_horizontal_ns;      // Bus routing (not H-tree)
    double bus_vertical_ns;
    double global_io_ns, bank_output_drv_ns;

    // WRITE path (VERY SLOW!)
    double set_pulse_ns;           // Crystallize (50-150ns)
    double reset_pulse_ns;         // Amorphize (10-50ns)

    VerificationStatus verification_status;
    std::string source;

    double getTotalReadLatency() const {
        return row_decoder_ns + wordline_ns + bitline_read_ns +
               sense_amp_external_ns + column_mux_ns + mat_output_drv_ns +
               bus_horizontal_ns + bus_vertical_ns + global_io_ns +
               bank_output_drv_ns;
    }

    double getTotalSetWriteLatency() const {
        return row_decoder_ns + wordline_ns + set_pulse_ns;
    }

    double getTotalResetWriteLatency() const {
        return row_decoder_ns + wordline_ns + reset_pulse_ns;
    }
};

struct PCMTiming {
    double clock_freq_ghz;
    double mat_read_ns, bank_read_ns, chip_read_ns;
    double mat_set_ns, bank_set_ns, chip_set_ns;
    double mat_reset_ns, bank_reset_ns, chip_reset_ns;
    /* 1.11.56 (audit D062): true when NVSim did not resolve
     * FunctionUnit::setLatency and the *_set_ns fields therefore hold the
     * GENERIC WRITE latency. getSetLatency() has no cached branch, so this is
     * the normal state on a cache hit; without the flag the SET tier was
     * attributed to a tool call that returned nothing. */
    bool set_from_generic_write = false;
    PCMInnerBankTiming inner_bank;
};

struct PCMEnergy {
    double mat_read_energy_pJ, bank_read_energy_pJ, chip_read_energy_pJ;
    double mat_set_energy_pJ, bank_set_energy_pJ, chip_set_energy_pJ;
    double mat_reset_energy_pJ, bank_reset_energy_pJ, chip_reset_energy_pJ;
    double read_energy_per_byte, write_energy_per_byte;
    double mat_leakage_mw, bank_leakage_mw, chip_leakage_mw;
    std::string energy_source;
};

struct PCMEndurance {
    double write_cycles;           // 10^8-10^9 typical
    double retention_years;
    bool mlc_support;              // Multi-level cell
    std::string endurance_source;
};

struct PCMDatapath {
    int mat_io_bits, bank_io_bits, chip_io_bits;
    VerificationStatus verification_status;
    std::string source;
};

class PCMArchitecture {
public:
    std::string name, technology, process_node;
    PCMOrganization organization;
    PCMTiming timing;
    PCMEnergy energy;
    PCMEndurance endurance;
    PCMDatapath datapath;

    PCMArchitecture(const std::string& name, const std::string& tech)
        : name(name), technology(tech) {}

    bool isSuitableForPIM() const {
        // Only for read-heavy workloads!
        return timing.mat_set_ns / timing.mat_read_ns > 10.0;
    }
};

// Example: 16MB PCM (90nm, JSSC 2007 design)

} // namespace memory
} // namespace pimid

#endif // PIMID_PCM_ARCHITECTURE_H
