/**
 * @file reram_architecture.h
 * @brief ReRAM (Resistive RAM / Memristor) Architecture Specifications for PIM
 *
 * KEY CHARACTERISTICS:
 * - Non-volatile
 * - Fast writes (5-20ns, much better than PCM!)
 * - Moderate read latency (4-7ns)
 * - Crossbar structure (IDEAL for analog computing!)
 * - Good endurance (10^10-10^12 cycles)
 *
 * SOURCES:
 * - NVSim (external/nvsim/)
 * - ISAAC, PRIME ReRAM papers
 * - INNER_BANK_TIMING_ALL_MEMORY_TECH.md
 *
 * VERSION: 1.0
 */

#ifndef PIMID_RERAM_ARCHITECTURE_H
#define PIMID_RERAM_ARCHITECTURE_H

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

struct ReRAMOrganization {
    int banks_per_chip;
    int bank_rows, bank_cols;
    size_t chip_size_mb, bank_size_kb;
    int mats_per_bank;
    size_t mat_size_kb;
    int crossbar_rows, crossbar_cols;  // Crossbar array size
};

struct ReRAMInnerBankTiming {
    // READ path
    double row_decoder_ns, wordline_ns;
    double bitline_ns;             // Resistance sensing
    double sense_amp_ns;
    double column_mux_ns, subarray_output_drv_ns;
    double local_io_ns;
    double htree_horizontal_ns, htree_vertical_ns;
    double global_io_ns, bank_output_drv_ns;

    // WRITE path (fast!)
    double set_pulse_ns;           // Form conductive filament
    double reset_pulse_ns;         // Rupture filament

    // ANALOG compute (special for ReRAM!)
    double analog_multiply_ns;     // Matrix-vector multiply in crossbar
    double analog_accumulate_ns;   // Column accumulation

    VerificationStatus verification_status;
    std::string source;

    double getTotalReadLatency() const {
        return row_decoder_ns + wordline_ns + bitline_ns + sense_amp_ns +
               column_mux_ns + subarray_output_drv_ns + local_io_ns +
               htree_horizontal_ns + htree_vertical_ns + global_io_ns +
               bank_output_drv_ns;
    }

    double getTotalWriteLatency() const {
        return row_decoder_ns + wordline_ns + set_pulse_ns;  // Fast!
    }

    double getAnalogComputeLatency() const {
        // Crossbar analog compute (very fast!)
        return analog_multiply_ns + analog_accumulate_ns;
    }
};

struct ReRAMTiming {
    double clock_freq_ghz;
    double mat_read_ns, bank_read_ns, chip_read_ns;
    double mat_write_ns, bank_write_ns, chip_write_ns;
    double analog_compute_ns;      // Analog crossbar operation
    ReRAMInnerBankTiming inner_bank;
};

struct ReRAMEnergy {
    double mat_read_energy_pJ, bank_read_energy_pJ, chip_read_energy_pJ;
    double mat_write_energy_pJ, bank_write_energy_pJ, chip_write_energy_pJ;
    double analog_compute_energy_pJ;  // Very low!
    double read_energy_per_byte, write_energy_per_byte;
    double mat_leakage_mw, bank_leakage_mw, chip_leakage_mw;
    std::string energy_source;
};

struct ReRAMEndurance {
    double write_cycles;           // 10^10-10^12 typical
    double retention_years;
    bool mlc_support;              // Multi-level cell
    bool analog_capable;           // Analog computing support
    std::string endurance_source;
};

struct ReRAMDatapath {
    int mat_io_bits, bank_io_bits, chip_io_bits;
    int crossbar_analog_bits;      // Analog compute resolution
    VerificationStatus verification_status;
    std::string source;
};

class ReRAMArchitecture {
public:
    std::string name, technology, process_node;
    ReRAMOrganization organization;
    ReRAMTiming timing;
    ReRAMEnergy energy;
    ReRAMEndurance endurance;
    ReRAMDatapath datapath;

    ReRAMArchitecture(const std::string& name, const std::string& tech)
        : name(name), technology(tech) {}

    bool isSuitableForPIM() const {
        // Excellent for PIM, especially analog compute!
        return endurance.analog_capable;
    }

    bool hasAnalogCompute() const {
        return endurance.analog_capable;
    }
};

// Example: 2MB ReRAM (32nm, analog-capable)

// Example: 8MB ReRAM (22nm, digital)

} // namespace memory
} // namespace pimid

#endif // PIMID_RERAM_ARCHITECTURE_H
