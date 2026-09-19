/**
 * @file dram_architecture_v2.h
 * @brief RIGOROUSLY VERIFIED DRAM Architecture Specifications
 *
 * This file contains DRAM architectural parameters with VERIFICATION STATUS.
 * Each parameter is marked as VERIFIED (from specs), INFERRED (from papers),
 * or ESTIMATED (educated guess).
 *
 * CRITICAL METHODOLOGY:
 * We distinguish between different datapath stages inside DRAM:
 * 1. Row buffer (bitline sense amps): 8-16Kb wide
 * 2. Global Sense Amplifiers (GSA): 256-512 bits (per subarray)
 * 3. Prefetch datapath: 64-512 bits (technology dependent)
 * 4. Bank-to-peripheral: UNKNOWN (not publicly documented!)
 * 5. Chip I/O pins: 4-16 bits (DDR) or 1024 bits (HBM) - VERIFIED
 *
 * SOURCES:
 * - JEDEC Standards (DDR4: JESD79-4, DDR5: JESD79-5, HBM2: JESD235, HBM3: JESD238)
 * - Academic Papers (DAS-MICRO15, NVIDIA-HPCA17, Darwin-arXiv23)
 * - Manufacturer Datasheets (Micron, Samsung, SK Hynix)
 *
 * VERSION: 2.0 (Rigorously Verified)
 * DATE: November 2024
 */

#ifndef PIMID_DRAM_ARCHITECTURE_V2_H
#define PIMID_DRAM_ARCHITECTURE_V2_H

#include <string>
#include <memory>
#include <iostream>
#include <iomanip>
#include <vector>
#include <map>

// Use common VerificationStatus enum from sram_architecture.h
#include "sram_architecture.h"

namespace pimid {
namespace memory {

// VerificationStatus is defined in sram_architecture.h

struct VerifiedValue {
    int value_bits;
    VerificationStatus status;
    std::string source;  // Citation or reference
    std::string notes;   // Additional context
};

//=============================================================================
// DRAM Internal Datapath Stages (CRITICAL CLARITY!)
//=============================================================================

/**
 * @brief DRAM Internal Datapath Structure
 *
 * DRAM has multiple datapath stages with DIFFERENT widths:
 *
 * Stage 1: Row Buffer (Bitline Sense Amps)
 *   - DDR4: 8,192 bits (8Kb) per subarray [VERIFIED: typical]
 *   - Activated row sits in bitline sense amplifiers
 *
 * Stage 2: Global Sense Amplifiers (GSA)
 *   - DDR4 x8: 256 bits per subarray [INFERRED: from DAS-MICRO15]
 *   - Reads data from row buffer to peripheral circuitry
 *
 * Stage 3: Prefetch Datapath
 *   - DDR4 x8: 64 bits (8n prefetch x 8-bit I/O) [VERIFIED: JEDEC]
 *   - DDR5 x8: 128 bits (16n prefetch x 8-bit I/O) [VERIFIED: JEDEC]
 *   - HBM2: 256 bits (2n x 128-bit channel) [VERIFIED: JEDEC]
 *   - HBM3: 512 bits (8n x 64-bit channel) [VERIFIED: JEDEC]
 *
 * Stage 4: Bank-to-Peripheral Serialization
 *   - UNKNOWN! Not publicly documented.
 *   - Likely narrow (8-32 bits) to save routing area [ESTIMATED]
 *   - THIS IS THE CRITICAL BOTTLENECK FOR BANK-LEVEL PIM!
 *
 * Stage 5: Chip I/O Pins
 *   - DDR4: x4/x8/x16 (4, 8, or 16 bits) [VERIFIED: JEDEC]
 *   - HBM2: 1024 bits total (8 x 128-bit channels) [VERIFIED: JEDEC]
 *   - HBM3: 1024 bits total (16 x 64-bit channels) [VERIFIED: JEDEC]
 */
struct DRAMDatapathStages {
    // Stage 1: Row Buffer (widest, but not a "port")
    VerifiedValue row_buffer_bits;        // Bitline sense amps

    // Stage 2: Global Sense Amplifiers (column I/O)
    VerifiedValue gsa_datapath_bits;      // Subarray -> peripheral

    // Stage 3: Prefetch Datapath (well-documented)
    VerifiedValue prefetch_datapath_bits; // Prefetch buffer width

    // Stage 4: Bank Serialization (CRITICAL UNKNOWN!)
    VerifiedValue bank_serialization_bits; // Bank -> chip peripheral (BOTTLENECK!)

    // Stage 5: Chip I/O (well-documented)
    VerifiedValue chip_io_bits;           // External package pins

    // Higher-level interfaces
    VerifiedValue rank_databus_bits;      // Rank-level (multiple chips)
    VerifiedValue channel_databus_bits;   // Memory channel
};

//=============================================================================
// Bandwidth-Based Inference (Conservative Approach)
//=============================================================================

/**
 * @brief Infer internal port widths from MEASURED bandwidth
 *
 * Since internal port widths are not documented, we can INFER limits
 * from measured bandwidth characteristics.
 *
 * METHODOLOGY:
 * If Bank-PIM achieves only X GB/s despite Y banks in parallel,
 * we can infer each bank's internal port is limited to ~X/Y GB/s.
 *
 * CONSERVATIVE: We use LOWER BOUNDS (worst case for PIM)
 */
/* 1.11.57 (audit C003): the three STORED bandwidths are GONE from this struct.
 *
 * What was wrong: bank_effective_bw_GBs and bank_group_effective_bw_GBs were
 * frozen literals per technology while every other rung of the same ladder is
 * a width times a clock (getSubarrayBandwidth uses clock_freq_mhz;
 * getChipIOBandwidth / getRankBW / getChannelBandwidth use data_rate_mtps).
 * They matched only the clocks the factories carried before 1.11.56 re-binned
 * HBM2, DDR5 and HBM3, and nothing re-derived them.
 *
 * Why it was invisible: the ladder builder turns a rung into a CLOCK by
 * f = BW * 8 / width, so a stale bandwidth does not show up as a bandwidth --
 * it shows up as a frequency, and the comment at that site claimed the
 * arithmetic "lands on the core clock for the array tiers". Worked through,
 * HBM3's bank tier came out at 16.0 * 8 / 128 = 1.0 GHz against its own
 * 3.2 GHz core clock, so every L1 and L2 crossing on the reference cell was
 * charged 3.2x the time this object's own width and clock imply (DDR5: 0.67x).
 * Only DDR4 was self-consistent, which is exactly why it never looked wrong.
 *
 * The fix derives both rungs the way the neighbours are derived, from fields
 * this object already carries, so a speed-bin change moves them and no
 * per-technology bandwidth number is invented anywhere. chip_internal_bw_GBs
 * went with them: it had no reader at all and its literals (DDR4 4.8 against
 * chip I/O 8 bits x 2400 MT/s = 2.4) were a second authority waiting to be
 * believed. See getBankEffectiveBW() / getBankGroupEffectiveBW(). */
struct InferredBandwidthLimits {
    std::string inference_method;       // How we derived these values
    std::string confidence_level;       // "High", "Medium", "Low"
};

//=============================================================================
// Complete DRAM Architecture (Rigorously Verified)
//=============================================================================

class DRAMArchitectureV2 {
public:
    std::string name;
    std::string technology;
    std::string version;  // "2.0-verified"

    // Datapath stages (detailed breakdown)
    DRAMDatapathStages datapath;

    // Bandwidth limits (inferred from measurements)
    InferredBandwidthLimits bandwidth_limits;

    // Physical organization (well-documented)
    struct {
        int subarrays_per_bank;       // VERIFIED
        int banks_per_bank_group;     // VERIFIED
        int bank_groups_per_chip;     // VERIFIED
        int chips_per_rank;           // VERIFIED
        int ranks_per_channel;        // VERIFIED

        size_t subarray_size_kb;      // TYPICAL

        /* 1.11.61 (ruling R1/R2): THE DENSITY FIELDS AND WHAT THEY COUNT.
         *
         * Both fields describe the part the Ramulator ORG PRESET the wrapper
         * names actually simulates -- not "a typical part", which is what they
         * used to say and why they described devices 4x-32x less dense than
         * every run's own timing model (audit round 4, C005; the evidence is in
         * _1164audit/DENSITY_INVESTIGATION.md).
         *
         * bank_size_mb -- ONE BANK of the simulated preset, every family:
         *   preset device density / the banks that density spans. It is the
         *   single hottest density consumer: main.cpp turns it into
         *   pages_per_unit, hence --pages-per-unit and the emitted
         *   pagesPerUnit, hence the address-to-unit map and PE locality.
         *
         * chip_size_mb -- THE UNIT DIFFERS BY FAMILY, deliberately, because
         * the two families do not have the same thing to count:
         *   DDR3/DDR4/DDR5/LPDDR5/GDDR6: ONE DEVICE. Equal to the preset's
         *     device density (GDDR6 included -- GDDR6.cpp puts the channel
         *     level inside the density product, so 8 Gb is the whole two-
         *     channel device). Ruling R1.
         *   HBM2/HBM3: ONE CORE DIE, which fronts TWO channels. NOT the
         *     preset's per-channel density (HBM2.cpp/HBM3.cpp call it "channel
         *     density") and not the stack. This is the unit the die-area path
         *     needs -- HBM2's 1024 MB is the 8 Gb Sohn ISSCC-2016 core die
         *     exactly, which is why HBM2's 96.00 mm^2/die lands on its
         *     published anchor (1.11.64: a SECOND vendor anchor for the same
         *     8 Gb HBM2 core-die density now exists -- Cho et al., SK hynix,
         *     ISSCC 2018 12.3 Fig.12.3.7, "Chip size 81.8 mm^2" for an 8 Gb
         *     2-channel core die, against Samsung's 96.00 mm^2 at 20 nm.
         *     The two bracket the real spread of an 8 Gb HBM2 core die at
         *     roughly 82-96 mm^2, a 17% band across vendors and process
         *     generations; the 96.00 anchor is retained as the modelled
         *     point because the rest of the HBM2 row is Samsung-derived, and
         *     the band is what a die-area claim should be quoted with) --
         *     and it is deliberately NOT the unit its
         *     partner chips_per_rank counts (that is the CHANNEL count, 8/16).
         *     Ruling R2.
         * RamulatorWrapper::initialize() cross-checks both readings against the
         * preset and warns loudly on a mismatch; see the capacity cross-check
         * beside the 1.11.56 speed-bin check. */
        size_t bank_size_mb;          // VERIFIED: the named Ramulator org preset
        size_t chip_size_mb;          // VERIFIED: device (DDR family) / core die (HBM)
        size_t rank_size_gb;          // DERIVED: chip_size_mb x chips_per_rank
    } organization;

    // Timing (JEDEC-verified)
    struct {
        // JEDEC Standard Parameters (End-to-end, externally observable)
        double clock_freq_mhz;        // VERIFIED
        double data_rate_mtps;        // VERIFIED
        double tRCD_ns;               // VERIFIED: ACTIVATE to READ/WRITE (includes all row ops)
        double tCAS_ns;               // VERIFIED: READ to data at pins (includes column + I/O)
        double tRP_ns;                // VERIFIED: Precharge time
        double tRAS_ns;               // VERIFIED: Row active time
        double tBurst_ns;             // VERIFIED: Burst duration

        // Inner-Bank Datapath Breakdown (INFERRED from CACTI and academic papers)
        // These components are HIDDEN inside tRCD and tCAS, but matter for PIM!
        struct {
            // Column access path (part of tCAS)
            double column_decoder_ns;       // Column address decode
            double column_mux_ns;           // Column multiplexer (CSL)
            double subarray_output_drv_ns;  // Subarray output driver

            // Inner-bank datapath (part of tCAS)
            double local_io_ns;             // Local data lines (LDL) - within subarray
            double htree_horizontal_ns;     // H-tree horizontal segment
            double htree_vertical_ns;       // H-tree vertical segment
            double global_io_ns;            // Global data lines (GDL) - bank-wide

            // Bank I/O (part of tCAS)
            double bank_io_driver_ns;       // Bank output driver to chip I/O

            // Verification metadata
            VerificationStatus verification_status;
            std::string source;             // "CACTI v6.5, DAS-MICRO'15, etc."

            // Derived totals
            double getColumnPathDelay() const {
                return column_decoder_ns + column_mux_ns + subarray_output_drv_ns;
            }

            double getInnerBankDatapathDelay() const {
                return local_io_ns + htree_horizontal_ns + htree_vertical_ns + global_io_ns;
            }

            double getTotalInnerBankDelay() const {
                return getColumnPathDelay() + getInnerBankDatapathDelay() + bank_io_driver_ns;
            }

            // For PIM: Subarray-to-bank-I/O (excluding chip I/O)
            double getSubarrayToBankIO() const {
                return subarray_output_drv_ns + local_io_ns +
                       htree_horizontal_ns + htree_vertical_ns +
                       global_io_ns + bank_io_driver_ns;
            }

            // For PIM: Subarray-to-subarray (within same bank)
            double getSubarrayToSubarrayHTree() const {
                // Egress: subarray A -> bank center
                // Ingress: bank center -> subarray B
                return 2.0 * (htree_horizontal_ns + htree_vertical_ns);
            }
        } inner_bank;

        // Hierarchical (INFERRED from measurements)
        /* 1.11.59 (audit C016): the first two are DERIVED from the JEDEC
         * timings above -- set them with deriveHierarchicalAccessTimes(), not
         * by hand. Written as literals they had drifted 22-37% from their own
         * declared sums on HBM2 and HBM3. */
        double subarray_access_ns;    // DERIVED: tRCD + tCAS
        double bank_access_ns;        // DERIVED: tRP + tRCD + tCAS
        double chip_access_ns;        // ESTIMATED
        double rank_access_ns;        // ESTIMATED
    } timing;

    // Energy (INFERRED from papers)
    struct {
        double subarray_energy_pJ;    // INFERRED
        double bank_energy_pJ;        // INFERRED
        double chip_energy_pJ;        // INFERRED
        double rank_energy_pJ;        // INFERRED
        std::string energy_source;    // Citation
    } energy;

    // PE Bus Constraints (for PE placement at different hierarchy levels)
    // These values define the data bus characteristics when PEs are placed
    // at different levels of the memory hierarchy (subarray, bank, chip, etc.)
    struct {
        // Subarray-level PE constraints
        struct {
            uint64_t data_bus_width_bits;
            double max_bandwidth_gbps;
            uint64_t row_buffer_size_bytes;
            bool has_dedicated_bus;
        } subarray_level;

        // Bank-level PE constraints
        struct {
            uint64_t data_bus_width_bits;
            double max_bandwidth_gbps;
            bool has_dedicated_bus;
        } bank_level;

        // Chip-level PE constraints
        struct {
            uint64_t data_bus_width_bits;
            double max_bandwidth_gbps;
            bool has_dedicated_bus;
        } chip_level;

        // Rank-level PE constraints
        struct {
            uint64_t data_bus_width_bits;
            double max_bandwidth_gbps;
            bool has_dedicated_bus;
        } rank_level;

        // Logic die level (for HBM/HMC)
        struct {
            uint64_t data_bus_width_bits;
            double max_bandwidth_gbps;
            bool has_dedicated_bus;
        } logic_die_level;
    } pe_bus_constraints;

    // Scalability (for hypothetical studies)
    double port_width_scale = 1.0;
    double energy_scale = 1.0;

    // Constructor
    DRAMArchitectureV2(const std::string& name, const std::string& tech)
        : name(name), technology(tech), version("2.0-verified") {}

    // Print verification status
    void printVerificationReport() const;

    /* 1.11.59 (audit C016): the two HIERARCHICAL access times are DERIVED from
     * the JEDEC timings above, not written down a second time beside them.
     *
     * The fields declare their own composition (subarray_access_ns = tRCD +
     * tCAS, bank_access_ns = tRP + tRCD + tCAS) and every factory used to state
     * them as literals as well, so a change to tRCD/tCAS/tRP left the derived
     * pair describing the old part. Measured on the tree before this release:
     *
     *   DDR4  tRCD+tCAS 26.64  literal 26.64      consistent
     *   DDR5  tRCD+tCAS 33.34  literal 33.34      consistent
     *   HBM2  tRCD+tCAS 32.00  literal 25.00      22% low
     *   HBM3  tRCD+tCAS 32.00  literal 20.00      37% low
     *   HBM2  tRP+tRCD+tCAS 44.50  literal 37.50  16% low
     *   HBM3  tRP+tRCD+tCAS 42.00  literal 30.00  29% low
     *
     * WHERE THE DRIFT CAME FROM, since the obvious suspect is not the culprit:
     * it is NOT the 1.11.56 speed-bin correction. That release moved
     * clock_freq_mhz, data_rate_mtps and tBurst_ns only, and said in each
     * factory that "the ns timings are absolute and stay" -- tRCD/tCAS/tRP for
     * HBM2 and HBM3 are byte-identical before and after it. The drift dates to
     * release 1.1.1, which raised HBM2 tRCD/tCAS from 12.5 to the JESD235C
     * spec minimum 16.0 and HBM3 tRCD/tCAS from 10.0 to the JESD238 spec
     * minimum 16.0, and did not move the two derived literals with them: 25.0
     * is 12.5+12.5 and 37.5 is 3 x 12.5, HBM2's pre-1.1.1 arithmetic exactly,
     * and 20.0/30.0 is HBM3's. They were consistent when written and have been
     * inconsistent for every release since.
     *
     * Fix (a) of the two the audit offered -- computed, not corrected-plus-
     * checked -- because nothing needs these two settable independently of the
     * timings. The one path that writes them from outside a factory
     * (extractDRAMArchitecture / updateDRAMArchitectureFromRamulator in
     * architecture_extractor.h) writes them from the wrapper's
     * getSubarrayAccessLatency() / getBankAccessLatency(), which compose the
     * SAME sums from getTRCD()/getTCAS()/getTRP(). So the derivation is the
     * only definition either path ever used; the literals were a second
     * authority that agreed by accident.
     *
     * chip_access_ns and rank_access_ns stay literals: they are marked
     * ESTIMATED and no relation in this file states what they are composed of,
     * so there is nothing to derive them from and inventing one would be worse
     * than leaving them declared as estimates. */
    void deriveHierarchicalAccessTimes() {
        timing.subarray_access_ns = timing.tRCD_ns + timing.tCAS_ns;
        timing.bank_access_ns = timing.tRP_ns + timing.tRCD_ns + timing.tCAS_ns;
    }

    // Calculate effective bandwidths
    /* 1.11.57 (audit C003): DERIVED, like every other rung of the ladder.
     * The bank tier moves the bank serialization width once per CORE clock --
     * it sits inside the array, before the DQ tiers where the data rate
     * applies. This is the same form as getSubarrayBandwidth() (GSA width x
     * core clock) and getRankBW() below (rank bus x data rate); it replaces a
     * per-technology literal that had stopped tracking either clock. */
    double getBankEffectiveBW() const {
        return (datapath.bank_serialization_bits.value_bits / 8.0) *
               (timing.clock_freq_mhz / 1000.0);
    }
    /* 1.11.57 (audit C003 + latent D020): the bank-group tier is the bank tier
     * at the bank-group PORT width, which RamulatorWrapper::getBankGroupPortBits()
     * defines as the bank serialization width x 2 -- an UNSOURCED multiplier
     * that the accessor announces as such, since no specification fixes a
     * bank-group port width. It is written as x2 of the bank figure here, and
     * not as an independent number, precisely so that the width and the
     * bandwidth of that rung cannot disagree: the ladder back-derives its clock
     * as BW * 8 / width, and the two must divide out to the core clock. */
    double getBankGroupEffectiveBW() const {
        return getBankEffectiveBW() * 2.0;
    }
    double getRankBW() const;

    // Get verification confidence
    std::string getOverallConfidence() const;
};

//=============================================================================
// DDR4-2400 (Rigorously Verified)
//=============================================================================

inline std::unique_ptr<DRAMArchitectureV2> createDDR4_2400_Verified() {
    auto arch = std::make_unique<DRAMArchitectureV2>("DDR4-2400", "DDR4");

    // ===== DATAPATH STAGES =====

    // Stage 1: Row Buffer
    arch->datapath.row_buffer_bits = {
        8192,  // 8Kb typical for DDR4
        VerificationStatus::VERIFIED,
        "Typical DDR4 subarray, consistent across manufacturers",
        "Activated row in bitline sense amplifiers"
    };

    // Stage 2: Global Sense Amplifiers (from academic papers)
    arch->datapath.gsa_datapath_bits = {
        256,  // 256 bits per subarray
        VerificationStatus::INFERRED,
        "DAS-MICRO15: 'data is read into 256 global sense-amplifiers'",
        "Column I/O width, not a bottleneck for single access"
    };

    // Stage 3: Prefetch Datapath (JEDEC verified)
    arch->datapath.prefetch_datapath_bits = {
        64,  // 8n prefetch x 8-bit I/O
        VerificationStatus::VERIFIED,
        "JEDEC JESD79-4: DDR4 has 8n prefetch architecture, x8 device",
        "8 bursts x 8 bits = 64 bits prefetch buffer"
    };

    // Stage 4: Bank Serialization (CRITICAL UNKNOWN!)
    arch->datapath.bank_serialization_bits = {
        8,  // ESTIMATED: likely 8-16 bits
        VerificationStatus::ESTIMATED,
        "NOT DOCUMENTED! Estimated from bank performance limits",
        "CRITICAL BOTTLENECK: Banks likely serialize through narrow (8-16 bit) paths "
        "to chip peripheral to save routing area. This is why bank-level PIM is slow in DDR4!"
    };

    // Stage 5: Chip I/O (JEDEC verified)
    arch->datapath.chip_io_bits = {
        8,  // x8 device
        VerificationStatus::VERIFIED,
        "JEDEC JESD79-4: DDR4 x8 configuration",
        "External package pins, well-documented"
    };

    // Rank databus
    arch->datapath.rank_databus_bits = {
        64,  // 8 chips x 8 bits
        VerificationStatus::VERIFIED,
        "JEDEC standard: 64-bit rank interface for DDR4",
        "FIRST WIDE interface in DDR4 hierarchy"
    };

    // Channel
    arch->datapath.channel_databus_bits = {
        64,  // Single channel
        VerificationStatus::VERIFIED,
        "Standard DDR4 memory controller interface",
        "Typical single-channel configuration"
    };

    // ===== BANDWIDTH LIMITS (INFERRED) =====

    /* 1.11.57 (audit C003): the three literals here are gone; the bank and
     * bank-group rungs are derived by getBankEffectiveBW() /
     * getBankGroupEffectiveBW() from this object's own serialization width and
     * core clock. DDR4 is the one technology whose literals AGREED with that
     * derivation (8 bits / 8 x 1.2 GHz = 1.2 GB/s), which is why the drift at
     * the other three went unnoticed for as long as it did. */
    arch->bandwidth_limits.inference_method =
        "DERIVED: bank = (bank_serialization_bits / 8) x clock_freq_GHz; "
        "bank group = bank x 2 (the bank-group port multiplier, UNSOURCED). "
        "CONSERVATIVE: assumes the serialization path is the bottleneck.";
    arch->bandwidth_limits.confidence_level = "Medium - based on estimated internal port widths";

    // ===== ORGANIZATION (VERIFIED) =====

    /* 1.11.61 (ruling R1): THE DENSITY FOLLOWS THE PRESET.
     *
     * The preset this tree simulates for DDR4 is DDR4_8Gb_x8
     * (ramulator_wrapper.cpp / main.cpp writeRamulatorConfigYaml), i.e.
     * {8<<10, 8, {1, 1, 4, 4, 1<<16, 1<<10}} at DDR4.cpp:19 -- an 8 Gb device,
     * 16 banks, 65536 rows x 1024 columns x 8 bits = 1024 B per row.
     *
     * The four lines below used to read 4 / 2 MB / 128 MB / 1 GB, tagged
     * "Typical 1Gb chip" and "VERIFIED (from datasheet)" on a value that
     * matched no preset in this tree and no datasheet part the tree cites.
     * Consequences, both measured (_1164audit/DENSITY_INVESTIGATION.md sec 3):
     * a DDR4 BANK-placement element was given a 2 MB contiguous block where one
     * bank of the simulated part is 64 MB, and the DRAM die area came out
     * 3.38 mm^2 for a die claiming 8 GiB of capacity -- 8x less silicon than
     * the memory it reported. At the preset's density the die lands at
     * 27.03 mm^2, which is the 8 Gb-class part vendorDieDensity()'s DDR4 row
     * was measured from.
     *
     * subarrays_per_bank and rank_size_gb move because the arithmetic forces
     * them, not as separate claims: 64 MB / 512 KB = 128 subarrays, which is
     * also what main.cpp's live count (bank_rows / subarray_height =
     * 65536 / 512) has always been, and 1024 MB x 8 devices = 8 GB, which is
     * the capacity_ the wrapper has always reported for DDR4. Both fields were
     * previously the only ones describing the 1 Gb part.
     *
     * The rate, the timings and every ladder width are untouched: density
     * reaches exactly two consumed numbers, pages_per_unit and the die area. */
    arch->organization.subarrays_per_bank = 128;  // DERIVED: 64 MB bank / 512 KB subarray
    arch->organization.banks_per_bank_group = 4;  // JEDEC
    arch->organization.bank_groups_per_chip = 4;  // JEDEC (x8/x4 configs)
    arch->organization.chips_per_rank = 8;  // x8 organization
    arch->organization.ranks_per_channel = 2;  // Typical DIMM
    arch->organization.subarray_size_kb = 512;  // Typical
    arch->organization.bank_size_mb = 64;   // preset DDR4_8Gb_x8: 8 Gb / 16 banks
    arch->organization.chip_size_mb = 1024; // preset DDR4_8Gb_x8: 8 Gb device
    arch->organization.rank_size_gb = 8;    // DERIVED: 8 x 1024 MB

    // ===== TIMING (JEDEC DDR4-2400 CL17) =====

    arch->timing.clock_freq_mhz = 1200;  // VERIFIED
    arch->timing.data_rate_mtps = 2400;  // VERIFIED
    /* 1.11.63 (R6-5): THE ns TIMINGS FOLLOW THE PRESET'S BIN, and are written
     * as the arithmetic that produces them so they cannot drift from it.
     *
     * DDR4_2400R is nCL/nRCD/nRP 16 and nRAS 39 at tCK = 1E6/(2400/2) = 833 ps
     * (DDR4.cpp timing_presets; the impl overwrites the row's tCK column with
     * that expression, DDR4.cpp:336). RamulatorWrapper::
     * applyPresetTimingsToArchitecture() stamps exactly these four values at
     * runtime; the literals here are the same arithmetic so the stamp is a
     * check rather than a change.
     *
     * WHAT MOVED, stated because it is not zero: 13.32 -> 13.328 is a
     * transcription rounding, and 32.0 -> 32.487 is JEDEC_rounding -- the
     * preset holds ceil(32 ns / 833 ps) = 39 WHOLE CYCLES, so 32.487 ns is how
     * long the model really keeps the row open and 32.0 was the un-quantised
     * spec minimum. getTRC() = tRAS + tRP prices the array ACTIVATE energy, so
     * DDR4's tRC rises 45.32 -> 45.815 ns (1.1%). The label "CL17" that used to
     * sit on the first line was wrong as well: DDR4_2400R is nCL 16; 2400U is
     * the CL17 bin and is not what this tree simulates. */
    arch->timing.tRCD_ns = 16 * 0.833;  // DERIVED: DDR4_2400R nRCD 16 x tCK 833 ps
    arch->timing.tCAS_ns = 16 * 0.833;  // DERIVED: nCL 16
    arch->timing.tRP_ns = 16 * 0.833;   // DERIVED: nRP 16
    arch->timing.tRAS_ns = 39 * 0.833;  // DERIVED: nRAS 39
    arch->timing.tBurst_ns = 3.33;  // VERIFIED: 8 beats @ 2400 MT/s

    // Inner-bank datapath timing (INFERRED from CACTI and academic papers)
    arch->timing.inner_bank.column_decoder_ns = 0.35;       // INFERRED from CACTI
    arch->timing.inner_bank.column_mux_ns = 0.55;           // INFERRED from CACTI
    arch->timing.inner_bank.subarray_output_drv_ns = 0.50;  // INFERRED from CACTI
    arch->timing.inner_bank.local_io_ns = 0.75;             // INFERRED from CACTI
    arch->timing.inner_bank.htree_horizontal_ns = 1.20;     // INFERRED from CACTI
    arch->timing.inner_bank.htree_vertical_ns = 1.20;       // INFERRED from CACTI
    arch->timing.inner_bank.global_io_ns = 1.50;            // ESTIMATED
    arch->timing.inner_bank.bank_io_driver_ns = 0.60;       // INFERRED from CACTI
    arch->timing.inner_bank.verification_status = VerificationStatus::INFERRED;
    arch->timing.inner_bank.source =
        "CACTI v6.5 analytical model (external/mcpat/cacti/), "
        "DAS-MICRO'15 (Shih-Lien Lu et al.), "
        "SALP-ISCA'12 (Yoongu Kim et al.), "
        "Tiered-Latency DRAM HPCA'13 (Donghyuk Lee et al.)";

    // Total inner-bank delay = 6.65ns
    // Remaining for sense amp + row decoder: 13.32 - 6.65 = 6.67ns OK
    // This breakdown is consistent with tCAS = 13.32ns

    /* 1.11.59 (audit C016): DERIVED -- see deriveHierarchicalAccessTimes().
     * These were the literals 26.64 and 39.96, and DDR4 is one of the two
     * technologies whose literals still reproduced their own declared sums
     * (13.32 + 13.32, 3 x 13.32). Nothing moves here; the derivation is what
     * keeps it that way after the next timing edit. */
    arch->deriveHierarchicalAccessTimes();
    arch->timing.chip_access_ns = 60.0;  // ESTIMATED
    arch->timing.rank_access_ns = 80.0;  // ESTIMATED

    // ===== ENERGY (INFERRED FROM LITERATURE) =====

    arch->energy.subarray_energy_pJ = 1.0;  // INFERRED
    arch->energy.bank_energy_pJ = 2.0;  // INFERRED
    arch->energy.chip_energy_pJ = 5.0;  // INFERRED
    arch->energy.rank_energy_pJ = 10.0;  // INFERRED
    arch->energy.energy_source =
        "INFERRED from academic literature: NVIDIA-HPCA17, DAS-MICRO15. "
        "Approximate values, order-of-magnitude correct.";

    // ===== PE BUS CONSTRAINTS (for PE placement) =====

    // Subarray-level PEs: Direct row buffer access
    arch->pe_bus_constraints.subarray_level.data_bus_width_bits = 8192 * 8;  // 8KB row buffer
    arch->pe_bus_constraints.subarray_level.max_bandwidth_gbps = 10.0;
    arch->pe_bus_constraints.subarray_level.row_buffer_size_bytes = 8192;
    arch->pe_bus_constraints.subarray_level.has_dedicated_bus = true;

    // Bank-level PEs: Share bank I/O bus
    arch->pe_bus_constraints.bank_level.data_bus_width_bits = 64;  // 64-bit bank bus
    arch->pe_bus_constraints.bank_level.max_bandwidth_gbps = 25.0;  // DDR4-2400 per-bank BW
    arch->pe_bus_constraints.bank_level.has_dedicated_bus = false;

    // Chip-level PEs: Share chip-level interconnect
    arch->pe_bus_constraints.chip_level.data_bus_width_bits = 64;
    arch->pe_bus_constraints.chip_level.max_bandwidth_gbps = 25.0;
    arch->pe_bus_constraints.chip_level.has_dedicated_bus = false;

    // Rank-level PEs: Share rank-level bus
    arch->pe_bus_constraints.rank_level.data_bus_width_bits = 64;
    arch->pe_bus_constraints.rank_level.max_bandwidth_gbps = 25.0;
    arch->pe_bus_constraints.rank_level.has_dedicated_bus = false;

    // Logic die level (not applicable for DDR4, but set defaults)
    arch->pe_bus_constraints.logic_die_level.data_bus_width_bits = 1024;
    arch->pe_bus_constraints.logic_die_level.max_bandwidth_gbps = 256.0;
    arch->pe_bus_constraints.logic_die_level.has_dedicated_bus = true;

    return arch;
}

/**
 * Create DDR4-2400 with configurable port width scaling
 *
 * @param port_width_scale Scaling factor for all port widths (default 1.0)
 *                         2.0 = 2x wider buses, 0.5 = half-width buses
 * @return Configured DDR4-2400 architecture with scaled port widths
 */
inline std::unique_ptr<DRAMArchitectureV2> createDDR4_2400_Verified(double port_width_scale) {
    auto arch = createDDR4_2400_Verified();
    arch->port_width_scale = port_width_scale;
    return arch;
}

//=============================================================================
// DDR3-1600H (1.11.68): DDR3 gets its OWN architecture object.
//
// WHY THIS EXISTS. Until now RamulatorWrapper handed DDR3 the DDR4-2400
// object from an unannounced else-branch (audit 1.11.57 B001). Every width,
// internal bandwidth and derived hierarchy figure reported for DDR3
// described DDR4, so the 1.11.56 reconciliation check failed and, by the
// B001/B002 rule, the per-level link ladder was NOT adopted: DDR3 ran on the
// per-technology placeholder table and every run said so. 1.11.67 measured
// what an unadopted ladder is worth on DDR5 (up to +102% cycles), which is
// why the three object-less technologies are being given their own.
//
// PROVENANCE. Organisation and page size are transcribed from JEDEC
// JESD79-3D section 2.11.5 (8 Gb, x8: 8 banks, row A0-A15, column A0-A9+A11,
// page size 2 KB) -- the same 8 Gb x8 part the Ramulator preset DDR3_8Gb_x8
// simulates. The ns timings are the DDR3_1600H bin written as the arithmetic
// that produces them, so RamulatorWrapper::applyPresetTimingsToArchitecture()
// stamps a check rather than a change. The internal stages (global sense
// amplifier width, bank serialisation) carry the SAME declared-estimate
// status they carry on DDR4 and DDR5: JEDEC does not document them, and this
// release does not pretend otherwise.
//
// WHAT DDR3 DOES NOT HAVE: bank groups. They arrive with DDR4. The eight
// banks sit directly on the chip, so bank_groups_per_chip is 1 and the
// bank-group rung of the ladder is a pass-through, which is the physical
// truth rather than a borrowed DDR4 shape.
//=============================================================================

inline std::unique_ptr<DRAMArchitectureV2> createDDR3_1600_Verified() {
    auto arch = std::make_unique<DRAMArchitectureV2>("DDR3-1600H", "DDR3");

    // ===== DATAPATH STAGES =====

    arch->datapath.row_buffer_bits = {
        16384,  // 2 KB page x 8 bits
        VerificationStatus::VERIFIED,
        "JEDEC JESD79-3D 2.11.5: 8Gb x8 page size 2 KB; page size = 2^COLBITS x ORG / 8",
        "Activated row in the bitline sense amplifiers. TWICE DDR4's x8 page, "
        "because DDR3 x8 carries 11 column bits to DDR4's 10."
    };
    arch->datapath.gsa_datapath_bits = {
        256,
        VerificationStatus::INFERRED,
        "DAS-MICRO15: 'data is read into 256 global sense-amplifiers'",
        "Column I/O width. Same inference as DDR4: not a documented JEDEC quantity."
    };
    arch->datapath.prefetch_datapath_bits = {
        64,  // 8n prefetch x 8-bit I/O
        VerificationStatus::VERIFIED,
        "JEDEC JESD79-3D 3.1: 'The DDR3 SDRAM uses a 8n prefetch architecture'",
        "8 bursts x 8 bits = 64 bits, the same 8n depth DDR4 keeps"
    };
    arch->datapath.bank_serialization_bits = {
        8,
        VerificationStatus::ESTIMATED,
        "NOT DOCUMENTED by JEDEC for any DDR generation; carried at DDR4's estimate",
        "CRITICAL BOTTLENECK and an admitted unknown: banks are assumed to "
        "serialise through a narrow path to the chip periphery. Held equal to "
        "DDR4's so the DDR3/DDR4 comparison does not turn on an invented "
        "difference between two numbers neither of which is sourced."
    };
    arch->datapath.chip_io_bits = {
        8,  // x8 device
        VerificationStatus::VERIFIED,
        "JEDEC JESD79-3D 2.11.5: x8 configuration",
        "External package pins"
    };
    arch->datapath.rank_databus_bits = {
        64,  // 8 chips x 8 bits
        VerificationStatus::VERIFIED,
        "JEDEC standard 64-bit rank interface (8 x8 devices per rank)",
        "First wide interface in the DDR3 hierarchy"
    };
    arch->datapath.channel_databus_bits = {
        64,
        VerificationStatus::VERIFIED,
        "Standard DDR3 memory-controller interface",
        "Single-channel configuration, matching the preset's channel count"
    };

    arch->bandwidth_limits.inference_method =
        "DERIVED: bank = (bank_serialization_bits / 8) x clock_freq_GHz; "
        "bank group = bank (DDR3 HAS NO BANK GROUPS, so the rung is a "
        "pass-through and carries no multiplier). CONSERVATIVE: assumes the "
        "serialization path is the bottleneck.";
    arch->bandwidth_limits.confidence_level =
        "Medium - external widths are JEDEC-verified, internal port widths are estimates";

    // ===== ORGANIZATION (preset DDR3_8Gb_x8, JESD79-3D 2.11.5) =====
    // 8 Gb / 8 banks = 128 MB per bank; 65536 rows x 2 KB = 128 MB confirms it.
    // A 512-row subarray at a 2 KB page is 1 MB, so 128 MB / 1 MB = 128
    // subarrays, which is also the live count main.cpp derives as
    // bank_rows / subarray_height = 65536 / 512.
    arch->organization.subarrays_per_bank = 128;   // DERIVED: 128 MB bank / 1 MB subarray
    arch->organization.banks_per_bank_group = 8;   // JESD79-3D: 8 banks, no grouping
    arch->organization.bank_groups_per_chip = 1;   // DDR3 HAS NO BANK GROUPS
    arch->organization.chips_per_rank = 8;         // x8 organization
    arch->organization.ranks_per_channel = 2;      // Typical DIMM
    arch->organization.subarray_size_kb = 1024;    // DERIVED: 512 rows x 2 KB page
    arch->organization.bank_size_mb = 128;         // DERIVED: 8 Gb / 8 banks
    arch->organization.chip_size_mb = 1024;        // preset DDR3_8Gb_x8: 8 Gb device
    arch->organization.rank_size_gb = 8;           // DERIVED: 8 x 1024 MB

    // ===== TIMING (preset DDR3_1600H; tCK = 1E6 / (1600/2) = 1250 ps) =====
    // The bin is CL9: nCL/nRCD/nRP 9, nRAS 28. Written as the arithmetic so
    // applyPresetTimingsToArchitecture()'s stamp is a check, not a change.
    arch->timing.clock_freq_mhz = 800;    // 1600 MT/s DDR -> 800 MHz CK
    arch->timing.data_rate_mtps = 1600;
    arch->timing.tRCD_ns = 9 * 1.25;      // DERIVED: DDR3_1600H nRCD 9 x tCK 1.25 ns
    arch->timing.tCAS_ns = 9 * 1.25;      // DERIVED: nCL 9
    arch->timing.tRP_ns  = 9 * 1.25;      // DERIVED: nRP 9
    arch->timing.tRAS_ns = 28 * 1.25;     // DERIVED: nRAS 28
    arch->timing.tBurst_ns = 5.0;         // VERIFIED: 8 beats @ 1600 MT/s

    // Inner-bank stage delays are process-level, not part-level: they come
    // from the same CACTI/academic basis as DDR4's and are held identical
    // rather than re-estimated, so any DDR3-vs-DDR4 difference in this run
    // comes from JEDEC quantities and not from two different guesses.
    arch->timing.inner_bank.column_decoder_ns = 0.35;
    arch->timing.inner_bank.column_mux_ns = 0.55;
    arch->timing.inner_bank.subarray_output_drv_ns = 0.50;
    arch->timing.inner_bank.local_io_ns = 0.75;
    arch->timing.inner_bank.htree_horizontal_ns = 1.20;
    arch->timing.inner_bank.htree_vertical_ns = 1.20;
    arch->timing.inner_bank.global_io_ns = 1.50;
    arch->timing.inner_bank.bank_io_driver_ns = 0.60;
    arch->timing.inner_bank.verification_status = VerificationStatus::INFERRED;
    arch->timing.inner_bank.source =
        "CACTI v6.5 analytical model (external/mcpat/cacti/), "
        "DAS-MICRO'15 (Shih-Lien Lu et al.), "
        "SALP-ISCA'12 (Yoongu Kim et al.), "
        "Tiered-Latency DRAM HPCA'13 (Donghyuk Lee et al.)";

    arch->deriveHierarchicalAccessTimes();   // subarray = tRCD+tCAS; bank = +tRP

    /* 1.11.68: THESE TWO ARE DERIVED, NOT ESTIMATED.
     *
     * DDR4's object carries 60.0 and 80.0 ns tagged ESTIMATED, and the wrapper
     * already ignores the first of them: getChipAccessLatency() returns
     * getBankAccessLatency() + getTBurst(). Only rank_access_ns is read raw
     * (ramulator_wrapper.cpp getRankAccessLatency, dram_model.cpp). Rather
     * than copy two of DDR4's guesses onto a different part, both are set to
     * the honest floor the wrapper itself computes: the bank access plus one
     * burst. Manufacturing a rank hop penalty on top would repeat exactly the
     * shape 1.11.23 and 1.11.57 (D020) removed from getBankGroupAccessLatency
     * and getChannelAccessLatency -- an unsourced multiplier charged
     * uniformly. If a sourced rank-traversal term ever arrives, it is added
     * here and the change is visible.
     * bank_access_ns = tRP + tRCD + tCAS = 3 x 11.25 = 33.75; + tBurst 5.0. */
    arch->timing.chip_access_ns = 33.75 + 5.0;   // DERIVED: bank access + one burst
    arch->timing.rank_access_ns = 33.75 + 5.0;   // DERIVED: no unsourced rank penalty

    // ===== ENERGY (order-of-magnitude, same basis as DDR4) =====
    // NOTE: these are NOT what prices a PIMID run. Array and interface energy
    // come from the Ramulator2 IDD rows and the termination model
    // (pimid_energy.h); these four fields are the architecture object's own
    // coarse figures and are kept at DDR4's basis deliberately.
    arch->energy.subarray_energy_pJ = 1.0;
    arch->energy.bank_energy_pJ = 2.0;
    arch->energy.chip_energy_pJ = 5.0;
    arch->energy.rank_energy_pJ = 10.0;
    arch->energy.energy_source =
        "INFERRED from academic literature: NVIDIA-HPCA17, DAS-MICRO15. "
        "Order-of-magnitude only; the priced energy path uses the IDD rows.";

    // ===== PE BUS CONSTRAINTS =====
    // Rank bandwidth is DERIVED, not quoted: 1600 MT/s x 8 B = 12.8 GB/s.
    arch->pe_bus_constraints.subarray_level.data_bus_width_bits = 16384;  // the 2 KB row
    arch->pe_bus_constraints.subarray_level.max_bandwidth_gbps = 10.0;
    arch->pe_bus_constraints.subarray_level.row_buffer_size_bytes = 2048;
    arch->pe_bus_constraints.subarray_level.has_dedicated_bus = true;
    arch->pe_bus_constraints.bank_level.data_bus_width_bits = 64;
    arch->pe_bus_constraints.bank_level.max_bandwidth_gbps = 12.8;
    arch->pe_bus_constraints.bank_level.has_dedicated_bus = false;
    arch->pe_bus_constraints.chip_level.data_bus_width_bits = 64;
    arch->pe_bus_constraints.chip_level.max_bandwidth_gbps = 12.8;
    arch->pe_bus_constraints.chip_level.has_dedicated_bus = false;
    arch->pe_bus_constraints.rank_level.data_bus_width_bits = 64;
    arch->pe_bus_constraints.rank_level.max_bandwidth_gbps = 12.8;
    arch->pe_bus_constraints.rank_level.has_dedicated_bus = false;
    /* DDR3 HAS NO LOGIC DIE. The field exists for the HBM stacks; mirroring
     * the rank rung here means a configuration that places elements at
     * LOGIC_DIE on DDR3 is not silently handed an HBM-class 1024-bit bus. */
    arch->pe_bus_constraints.logic_die_level.data_bus_width_bits = 64;
    arch->pe_bus_constraints.logic_die_level.max_bandwidth_gbps = 12.8;
    arch->pe_bus_constraints.logic_die_level.has_dedicated_bus = false;

    return arch;
}

inline std::unique_ptr<DRAMArchitectureV2> createDDR3_1600_Verified(double port_width_scale) {
    auto arch = createDDR3_1600_Verified();
    arch->port_width_scale = port_width_scale;
    return arch;
}

//=============================================================================
// LPDDR5-6400 (1.11.69): LPDDR5 gets its OWN architecture object.
//
// Second of the three technologies that were reading DDR4's object (audit
// 1.11.57 B001). See the DDR3 block above for why this matters.
//
// PROVENANCE. JEDEC JESD209-5C section 2.2.4 Table 6 ("LPDDR5 SDRAM x16 Mode
// Addressing for BG Mode, 4 Banks / 4 Bank Groups"), the 8 Gb column: 4 banks
// per group, 4 bank groups, 32768 rows, 2048-byte page, array pre-fetch 256
// bits. That is the same part the Ramulator preset LPDDR5_8Gb_x16 simulates
// ({1 Ch, 1 Ra, 4 Bg, 4 Ba, 1<<15 Ro, 1<<10 Co}).
//
// ON THE COLUMN COUNT, so nobody "fixes" it later: JEDEC Table 6 says 64
// columns and Ramulator's preset says 1024. They do not disagree. JEDEC counts
// FETCH BOUNDARIES of 256 bits (64 x 256 / 8 = 2048 B) and Ramulator counts
// 16-bit device words (1024 x 16 / 8 = 2048 B). Both describe the same 2 KB
// page, which is the quantity this object carries.
//
// WHAT IS BETTER SOURCED HERE THAN ON DDR3/DDR4: the array pre-fetch width.
// JEDEC publishes it for LPDDR5 (Table 6, "Array Pre-Fetch 256"), so the
// global sense-amplifier datapath is VERIFIED rather than inferred from a
// paper, and it is four times the 64-bit DDR figure.
//
// NOT A DIMM. One x16 die fronts the channel: chips_per_rank is 1 and the
// rank bus IS the 16-bit channel, where the DDR parts build a 64-bit rank
// from eight x8 devices. This is what makes the reconciliation check pass --
// 6400 MT/s x 16 bits / 8 = 12.8 GB/s, the bandwidth the rate table derives.
//=============================================================================

inline std::unique_ptr<DRAMArchitectureV2> createLPDDR5_6400_Verified() {
    auto arch = std::make_unique<DRAMArchitectureV2>("LPDDR5-6400", "LPDDR5");

    // ===== DATAPATH STAGES =====

    arch->datapath.row_buffer_bits = {
        16384,  // 2 KB page x 8 bits
        VerificationStatus::VERIFIED,
        "JEDEC JESD209-5C Table 6 (2.2.4), 8 Gb x16 BG mode: page size 2048 bytes",
        "Activated row in the bitline sense amplifiers, per bank"
    };
    arch->datapath.gsa_datapath_bits = {
        256,
        VerificationStatus::VERIFIED,
        "JEDEC JESD209-5C Table 6 (2.2.4): 'Array Pre-Fetch 256' for every density",
        "JEDEC PUBLISHES this for LPDDR5, so unlike the DDR parts it is not an "
        "inference from the literature. Four times the DDR 8n x8 figure."
    };
    arch->datapath.prefetch_datapath_bits = {
        256,  // 16n burst x 16 DQ, equal to the array pre-fetch
        VerificationStatus::VERIFIED,
        "JEDEC JESD209-5C Table 6: burst addresses B0-B3 over x16 = 256 bits, "
        "matching the tabulated array pre-fetch",
        "The two agree, which is the check on reading either one"
    };
    arch->datapath.bank_serialization_bits = {
        8,
        VerificationStatus::ESTIMATED,
        "NOT DOCUMENTED by JEDEC for any LPDDR generation; carried at the DDR estimate",
        "CRITICAL BOTTLENECK and an admitted unknown. Held equal to DDR3/DDR4's "
        "so a cross-technology comparison does not turn on an invented "
        "difference between two unsourced numbers."
    };
    arch->datapath.chip_io_bits = {
        16,  // x16 device
        VerificationStatus::VERIFIED,
        "JEDEC JESD209-5C Table 6: x16 DQ configuration",
        "External package pins"
    };
    arch->datapath.rank_databus_bits = {
        16,  // ONE x16 die fronts the channel -- not a 64-bit DIMM rank
        VerificationStatus::VERIFIED,
        "JEDEC JESD209-5C: a 16-bit channel served by a single x16 die",
        "NO 8-DEVICE RANK. This is the field that reconciles the object with "
        "the rate table: 6400 MT/s x 16 bits / 8 = 12.8 GB/s."
    };
    arch->datapath.channel_databus_bits = {
        16,
        VerificationStatus::VERIFIED,
        "JEDEC JESD209-5C: 16-bit LPDDR5 channel",
        "Matches CactiIOWrapper::dramChannelWidthBits(LPDDR5)"
    };

    arch->bandwidth_limits.inference_method =
        "DERIVED: bank = (bank_serialization_bits / 8) x clock_freq_GHz; "
        "bank group = bank x 2 (the bank-group port multiplier, UNSOURCED). "
        "CONSERVATIVE: assumes the serialization path is the bottleneck.";
    arch->bandwidth_limits.confidence_level =
        "Medium-high - page, prefetch, widths and organisation are all JEDEC-verified; "
        "only the bank serialization width is an estimate";

    // ===== ORGANIZATION (preset LPDDR5_8Gb_x16, JESD209-5C Table 6) =====
    // 8 Gb / 16 banks = 64 MB per bank; 32768 rows x 2 KB = 64 MB confirms it.
    // A 512-row subarray at a 2 KB page is 1 MB, so 64 subarrays per bank,
    // which is also main.cpp's live count (32768 rows / 512).
    arch->organization.subarrays_per_bank = 64;   // DERIVED: 64 MB bank / 1 MB subarray
    arch->organization.banks_per_bank_group = 4;  // JESD209-5C Table 6
    arch->organization.bank_groups_per_chip = 4;  // JESD209-5C Table 6 (BG mode)
    arch->organization.chips_per_rank = 1;        // one x16 die per channel
    arch->organization.ranks_per_channel = 1;     // LPDDR5 is point-to-point
    arch->organization.subarray_size_kb = 1024;   // DERIVED: 512 rows x 2 KB page
    arch->organization.bank_size_mb = 64;         // DERIVED: 8 Gb / 16 banks
    arch->organization.chip_size_mb = 1024;       // preset LPDDR5_8Gb_x16: 8 Gb die
    arch->organization.rank_size_gb = 1;          // DERIVED: 1 die x 1024 MB

    // ===== TIMING (preset LPDDR5_6400; tCK = 8E6 / 6400 = 1250 ps) =====
    // LPDDR5 divides 8E6 rather than the DDR families' 2E6: WCK runs at 4x CK
    // and the bus is DDR on WCK. The bin is nRCD 15, nCL 17, nRP 15, nRAS 34.
    arch->timing.clock_freq_mhz = 800;    // 1000 / 1.25 ns CK
    arch->timing.data_rate_mtps = 6400;
    arch->timing.tRCD_ns = 15 * 1.25;     // DERIVED: nRCD 15 x tCK 1.25 ns
    arch->timing.tCAS_ns = 17 * 1.25;     // DERIVED: nCL 17
    arch->timing.tRP_ns  = 15 * 1.25;     // DERIVED: nRP 15
    arch->timing.tRAS_ns = 34 * 1.25;     // DERIVED: nRAS 34
    arch->timing.tBurst_ns = 2.5;         // DERIVED: nBL 2 x tCK

    // Process-level stage delays, held at the DDR basis for the same reason
    // given in the DDR3 block: a cross-technology difference should come from
    // JEDEC quantities, not from two different guesses.
    arch->timing.inner_bank.column_decoder_ns = 0.35;
    arch->timing.inner_bank.column_mux_ns = 0.55;
    arch->timing.inner_bank.subarray_output_drv_ns = 0.50;
    arch->timing.inner_bank.local_io_ns = 0.75;
    arch->timing.inner_bank.htree_horizontal_ns = 1.20;
    arch->timing.inner_bank.htree_vertical_ns = 1.20;
    arch->timing.inner_bank.global_io_ns = 1.50;
    arch->timing.inner_bank.bank_io_driver_ns = 0.60;
    arch->timing.inner_bank.verification_status = VerificationStatus::INFERRED;
    arch->timing.inner_bank.source =
        "CACTI v6.5 analytical model (external/mcpat/cacti/), "
        "DAS-MICRO'15 (Shih-Lien Lu et al.), "
        "SALP-ISCA'12 (Yoongu Kim et al.), "
        "Tiered-Latency DRAM HPCA'13 (Donghyuk Lee et al.)";

    arch->deriveHierarchicalAccessTimes();

    // DERIVED, per the 1.11.68 note in the DDR3 block: bank access + one
    // burst, with no unsourced rank hop added on top.
    // bank_access_ns = tRP + tRCD + tCAS = 18.75 + 18.75 + 21.25 = 58.75.
    arch->timing.chip_access_ns = 58.75 + 2.5;
    arch->timing.rank_access_ns = 58.75 + 2.5;

    // ===== ENERGY (order-of-magnitude; the priced path uses the IDD rows) ====
    arch->energy.subarray_energy_pJ = 1.0;
    arch->energy.bank_energy_pJ = 2.0;
    arch->energy.chip_energy_pJ = 5.0;
    arch->energy.rank_energy_pJ = 10.0;
    arch->energy.energy_source =
        "INFERRED from academic literature: NVIDIA-HPCA17, DAS-MICRO15. "
        "Order-of-magnitude only; the priced energy path uses the IDD rows.";

    // ===== PE BUS CONSTRAINTS =====
    // Channel bandwidth is DERIVED: 6400 MT/s x 16 bits / 8 = 12.8 GB/s.
    arch->pe_bus_constraints.subarray_level.data_bus_width_bits = 16384;
    arch->pe_bus_constraints.subarray_level.max_bandwidth_gbps = 10.0;
    arch->pe_bus_constraints.subarray_level.row_buffer_size_bytes = 2048;
    arch->pe_bus_constraints.subarray_level.has_dedicated_bus = true;
    arch->pe_bus_constraints.bank_level.data_bus_width_bits = 16;
    arch->pe_bus_constraints.bank_level.max_bandwidth_gbps = 12.8;
    arch->pe_bus_constraints.bank_level.has_dedicated_bus = false;
    arch->pe_bus_constraints.chip_level.data_bus_width_bits = 16;
    arch->pe_bus_constraints.chip_level.max_bandwidth_gbps = 12.8;
    arch->pe_bus_constraints.chip_level.has_dedicated_bus = false;
    arch->pe_bus_constraints.rank_level.data_bus_width_bits = 16;
    arch->pe_bus_constraints.rank_level.max_bandwidth_gbps = 12.8;
    arch->pe_bus_constraints.rank_level.has_dedicated_bus = false;
    /* LPDDR5 HAS NO LOGIC DIE; mirror the channel rung so a LOGIC_DIE
     * placement is not silently handed an HBM-class bus. */
    arch->pe_bus_constraints.logic_die_level.data_bus_width_bits = 16;
    arch->pe_bus_constraints.logic_die_level.max_bandwidth_gbps = 12.8;
    arch->pe_bus_constraints.logic_die_level.has_dedicated_bus = false;

    return arch;
}

inline std::unique_ptr<DRAMArchitectureV2> createLPDDR5_6400_Verified(double port_width_scale) {
    auto arch = createLPDDR5_6400_Verified();
    arch->port_width_scale = port_width_scale;
    return arch;
}

//=============================================================================
// GDDR6-14000 (1.11.70): GDDR6 gets its OWN architecture object.
//
// Last of the three that were reading DDR4's object (audit 1.11.57 B001).
//
// PROVENANCE. JEDEC JESD250D section 4.1 Table 19 ("Addressing Scheme"), the
// 8 Gb x16 column: 2 channels per device, 4 Gb per channel, array pre-fetch
// 256 bits per channel, BA[3:0] = 16 banks per channel, R[13:0] = 16384 rows,
// C[5:0], page size 2 K. Same part as the preset GDDR6_8Gb_x16
// ({2 Ch, 4 Bg, 4 Ba, 1<<14 Ro, 1<<10 Co}). As with LPDDR5, JEDEC's column
// count (64) and Ramulator's (1024) differ only in unit -- 256-bit fetch
// boundaries against 16-bit device words -- and both give the 2 KB page;
// JESD250D states the relation itself in Table 19 note 3, "Page Size =
// 2^COLBITS x (Prefetch_Size/8)".
//
// TWO CHANNELS PER DEVICE is the fact that reconciles this object. GDDR6's
// channel is 16 bits (JESD250D 2.2: "two 16 bit wide fully independent
// channels", the basis of ruling R3), and the aggregate the rate table
// derives is 14000 MT/s x 16 bits / 8 x 2 channels = 56 GB/s. Reading DDR4's
// 64-bit single-channel object gave 38.4 GB/s against that 56, which is the
// mismatch that refused GDDR6's ladder.
//
// THE CLOCK. GDDR6 transfers 8 bits per pin per CK (WCK at 4x CK, DDR on
// WCK), so tCK = 8E6 / rate = 571 ps at 14000 MT/s, not rate/2 as in the DDR
// families. 1.11.66 corrected that in the preset and the transcription; this
// object is written to the same divisor so the stamp is a check.
//=============================================================================

inline std::unique_ptr<DRAMArchitectureV2> createGDDR6_14000_Verified() {
    auto arch = std::make_unique<DRAMArchitectureV2>("GDDR6-14000", "GDDR6");

    // ===== DATAPATH STAGES =====

    arch->datapath.row_buffer_bits = {
        16384,  // 2 KB page x 8 bits
        VerificationStatus::VERIFIED,
        "JEDEC JESD250D Table 19 (4.1), 8 Gb x16: page size 2K per channel",
        "Activated row in the bitline sense amplifiers, per bank, per channel"
    };
    arch->datapath.gsa_datapath_bits = {
        256,
        VerificationStatus::VERIFIED,
        "JEDEC JESD250D Table 19: 'Array Pre-Fetch (bits, per channel) 256' in x16 mode",
        "JEDEC publishes this for GDDR6, as it does for LPDDR5; it is an "
        "inference only on the DDR parts. x8 mode would be 128."
    };
    arch->datapath.prefetch_datapath_bits = {
        256,  // 16n prefetch x 16 DQ
        VerificationStatus::VERIFIED,
        "JEDEC JESD250D 2.1: '16n prefetch architecture: 256 bit per array "
        "read or write access per channel'",
        "Stated twice in the standard: as a feature and as Table 19's column"
    };
    arch->datapath.bank_serialization_bits = {
        8,
        VerificationStatus::ESTIMATED,
        "NOT DOCUMENTED by JEDEC; carried at the DDR/LPDDR estimate",
        "CRITICAL BOTTLENECK and an admitted unknown, held equal across the "
        "lineup so comparisons do not turn on an invented difference."
    };
    arch->datapath.chip_io_bits = {
        16,  // x16 mode, per channel
        VerificationStatus::VERIFIED,
        "JEDEC JESD250D Table 19: x16 mode, 16 DQ per channel",
        "Per CHANNEL. The device carries two of these."
    };
    arch->datapath.rank_databus_bits = {
        16,
        VerificationStatus::VERIFIED,
        "JEDEC JESD250D: point-to-point channel, one device per channel",
        "No multi-device rank exists in GDDR6; the rank bus IS the channel"
    };
    arch->datapath.channel_databus_bits = {
        16,
        VerificationStatus::VERIFIED,
        "JEDEC JESD250D 2.2 p.3: 'two 16 bit wide fully independent channels'; "
        "Table 80 p.177: 'DQ[15:0] ... 16-bit data bus' (ruling R3)",
        "THE field that reconciles: 16 bits x 14000 MT/s / 8 x 2 channels = 56 GB/s"
    };

    arch->bandwidth_limits.inference_method =
        "DERIVED: bank = (bank_serialization_bits / 8) x clock_freq_GHz; "
        "bank group = bank x 2 (the bank-group port multiplier, UNSOURCED). "
        "CONSERVATIVE: assumes the serialization path is the bottleneck.";
    arch->bandwidth_limits.confidence_level =
        "Medium-high - page, prefetch, widths, channel count and organisation are "
        "JEDEC-verified; only the bank serialization width is an estimate";

    // ===== ORGANIZATION (preset GDDR6_8Gb_x16, JESD250D Table 19) =====
    // 4 Gb per channel / 16 banks = 32 MB per bank; 16384 rows x 2 KB = 32 MB
    // confirms it. A 512-row subarray at a 2 KB page is 1 MB -> 32 subarrays,
    // which is main.cpp's live count (16384 rows / 512).
    arch->organization.subarrays_per_bank = 32;   // DERIVED: 32 MB bank / 1 MB subarray
    arch->organization.banks_per_bank_group = 4;  // preset Ba 4; JEDEC BA[3:0] = 16 per channel
    arch->organization.bank_groups_per_chip = 4;  // preset Bg 4
    arch->organization.chips_per_rank = 1;        // point-to-point
    arch->organization.ranks_per_channel = 1;     // GDDR6 has no rank dimension
    arch->organization.subarray_size_kb = 1024;   // DERIVED: 512 rows x 2 KB page
    arch->organization.bank_size_mb = 32;         // DERIVED: 4 Gb channel / 16 banks
    arch->organization.chip_size_mb = 1024;       // the 8 Gb DEVICE, spanning 2 channels
    arch->organization.rank_size_gb = 1;          // DERIVED: one 8 Gb device

    // ===== TIMING (preset GDDR6_2000_1350mV_double; tCK = 8E6 / 14000 = 571 ps) =====
    // nRCD 26, nCL 24, nRP 26, nRAS 53, nBL 2.
    arch->timing.clock_freq_mhz = 1000.0 / 0.571;  // DERIVED: 1000 / tCK_ns
    arch->timing.data_rate_mtps = 14000;
    arch->timing.tRCD_ns = 26 * 0.571;   // DERIVED: nRCD 26 x tCK 571 ps
    arch->timing.tCAS_ns = 24 * 0.571;   // DERIVED: nCL 24
    arch->timing.tRP_ns  = 26 * 0.571;   // DERIVED: nRP 26
    arch->timing.tRAS_ns = 53 * 0.571;   // DERIVED: nRAS 53
    arch->timing.tBurst_ns = 2 * 0.571;  // DERIVED: nBL 2 x tCK

    // Process-level stage delays, held at the lineup's common basis.
    arch->timing.inner_bank.column_decoder_ns = 0.35;
    arch->timing.inner_bank.column_mux_ns = 0.55;
    arch->timing.inner_bank.subarray_output_drv_ns = 0.50;
    arch->timing.inner_bank.local_io_ns = 0.75;
    arch->timing.inner_bank.htree_horizontal_ns = 1.20;
    arch->timing.inner_bank.htree_vertical_ns = 1.20;
    arch->timing.inner_bank.global_io_ns = 1.50;
    arch->timing.inner_bank.bank_io_driver_ns = 0.60;
    arch->timing.inner_bank.verification_status = VerificationStatus::INFERRED;
    arch->timing.inner_bank.source =
        "CACTI v6.5 analytical model (external/mcpat/cacti/), "
        "DAS-MICRO'15 (Shih-Lien Lu et al.), "
        "SALP-ISCA'12 (Yoongu Kim et al.), "
        "Tiered-Latency DRAM HPCA'13 (Donghyuk Lee et al.)";

    arch->deriveHierarchicalAccessTimes();

    // DERIVED (see the 1.11.68 note in the DDR3 block): bank access + one
    // burst, no unsourced rank hop.
    // bank_access_ns = tRP + tRCD + tCAS = (26 + 26 + 24) x 0.571 = 43.396.
    arch->timing.chip_access_ns = 43.396 + 1.142;
    arch->timing.rank_access_ns = 43.396 + 1.142;

    // ===== ENERGY (order-of-magnitude; the priced path uses the IDD rows) ====
    arch->energy.subarray_energy_pJ = 1.0;
    arch->energy.bank_energy_pJ = 2.0;
    arch->energy.chip_energy_pJ = 5.0;
    arch->energy.rank_energy_pJ = 10.0;
    arch->energy.energy_source =
        "INFERRED from academic literature: NVIDIA-HPCA17, DAS-MICRO15. "
        "Order-of-magnitude only; the priced energy path uses the IDD rows.";

    // ===== PE BUS CONSTRAINTS =====
    // Per-channel bandwidth is DERIVED: 14000 MT/s x 16 bits / 8 = 28 GB/s.
    arch->pe_bus_constraints.subarray_level.data_bus_width_bits = 16384;
    arch->pe_bus_constraints.subarray_level.max_bandwidth_gbps = 10.0;
    arch->pe_bus_constraints.subarray_level.row_buffer_size_bytes = 2048;
    arch->pe_bus_constraints.subarray_level.has_dedicated_bus = true;
    arch->pe_bus_constraints.bank_level.data_bus_width_bits = 16;
    arch->pe_bus_constraints.bank_level.max_bandwidth_gbps = 28.0;
    arch->pe_bus_constraints.bank_level.has_dedicated_bus = false;
    arch->pe_bus_constraints.chip_level.data_bus_width_bits = 16;
    arch->pe_bus_constraints.chip_level.max_bandwidth_gbps = 28.0;
    arch->pe_bus_constraints.chip_level.has_dedicated_bus = false;
    arch->pe_bus_constraints.rank_level.data_bus_width_bits = 16;
    arch->pe_bus_constraints.rank_level.max_bandwidth_gbps = 28.0;
    arch->pe_bus_constraints.rank_level.has_dedicated_bus = false;
    /* GDDR6 HAS NO LOGIC DIE; mirror the channel rung. */
    arch->pe_bus_constraints.logic_die_level.data_bus_width_bits = 16;
    arch->pe_bus_constraints.logic_die_level.max_bandwidth_gbps = 28.0;
    arch->pe_bus_constraints.logic_die_level.has_dedicated_bus = false;

    return arch;
}

inline std::unique_ptr<DRAMArchitectureV2> createGDDR6_14000_Verified(double port_width_scale) {
    auto arch = createGDDR6_14000_Verified();
    arch->port_width_scale = port_width_scale;
    return arch;
}

//=============================================================================
// HBM2 (Rigorously Verified)
//=============================================================================

inline std::unique_ptr<DRAMArchitectureV2> createHBM2_Verified() {
    auto arch = std::make_unique<DRAMArchitectureV2>("HBM2", "HBM2");

    // ===== DATAPATH STAGES =====

    // Stage 1: Row Buffer
    arch->datapath.row_buffer_bits = {
        16384,  // 16Kb typical for HBM2
        VerificationStatus::INFERRED,
        "Typical HBM2 subarray, larger than DDR4",
        "Wider rows due to 3D stacked architecture"
    };

    // Stage 2: Global Sense Amplifiers
    arch->datapath.gsa_datapath_bits = {
        512,  // Wider than DDR4
        VerificationStatus::INFERRED,
        "Estimated from HBM2 architecture papers",
        "Wider column I/O than DDR4"
    };

    // Stage 3: Prefetch Datapath (JEDEC verified)
    arch->datapath.prefetch_datapath_bits = {
        256,  // 2n prefetch x 128-bit channel
        VerificationStatus::VERIFIED,
        "JEDEC JESD235A: HBM2 has 2n prefetch, 128-bit channels",
        "2 bursts x 128 bits = 256 bits prefetch buffer"
    };

    // Stage 4: Bank Serialization (BETTER THAN DDR!)
    arch->datapath.bank_serialization_bits = {
        64,  // INFERRED: wider due to TSV
        VerificationStatus::INFERRED,
        "ESTIMATED from TSV architecture: Through-Silicon Vias enable wider paths",
        "TSVs provide vertical connections with less routing constraint than DDR. "
        "ESTIMATED 64-bit based on 8x better bank-level BW vs DDR4. "
        "THIS IS WHY HBM ENABLES BANK-LEVEL PIM!"
    };

    // Stage 5: Chip I/O (JEDEC verified)
    arch->datapath.chip_io_bits = {
        1024,  // 8 channels x 128 bits
        VerificationStatus::VERIFIED,
        "JEDEC JESD235A: HBM2 has 8 independent 128-bit channels per stack",
        "Total 1024-bit interface (128 bytes per cycle)"
    };

    // Rank/Channel (HBM terminology different)
    arch->datapath.rank_databus_bits = {
        128,  // Per-channel
        VerificationStatus::VERIFIED,
        "JEDEC JESD235A: 128-bit channel width",
        "HBM uses 'channel' terminology instead of 'rank'"
    };

    /* 1.11.57 (audit C007): ONE CHANNEL, like every other family's entry in
     * this field. This used to hold 1024 -- the whole stack, all 8 channels --
     * while DDR's entry holds one 64-bit channel, so the field meant two
     * different things depending on the technology reading it. That was
     * invisible until 1.11.56 made this field the hierarchy ladder's L5 rung:
     * the ladder then described a channel 8x wider than the L4 rung directly
     * beneath it (the 128-bit "rank" IS the channel here), and the system root
     * above it multiplied by the channel count a second time. The stack figure
     * is still recoverable, and is now recovered where it belongs -- one
     * channel x the channel count -- rather than stored under a per-channel
     * name. */
    arch->datapath.channel_databus_bits = {
        128,  // one channel; the stack is 8 of these (JESD235A)
        VerificationStatus::VERIFIED,
        "JEDEC JESD235A: 128-bit channel, 8 channels per stack",
        "One channel's data bus; stack width = this x channels"
    };

    // ===== BANDWIDTH LIMITS (INFERRED) =====

    /* 1.11.57 (audit C003): the literals here read "64 bits / 8 x 1 GHz" and
     * this part's core clock is not 1 GHz -- it was 1.0 GHz when the line was
     * written and the object has been re-binned since. Derived now, from the
     * serialization width and clock the object itself carries. */
    arch->bandwidth_limits.inference_method =
        "DERIVED: bank = (bank_serialization_bits / 8) x clock_freq_GHz, on an "
        "estimated 64-bit TSV bank path; bank group = bank x 2 (the bank-group "
        "port multiplier, UNSOURCED). TSV enables wider internal paths than "
        "DDR4 wire routing, which is why HBM's bank tier is the faster one.";
    arch->bandwidth_limits.confidence_level = "Medium - TSV width not publicly specified";

    // ===== ORGANIZATION (VERIFIED) =====

    arch->organization.subarrays_per_bank = 4;
    /* 1.11.64 (JESD235D Table 4 p.6, Table 5 p.8): 8 bank groups PER CHANNEL,
     * not 4 -- 32 banks per channel, not 16.
     *
     * JEDEC gives BA[3:0] = 16 banks PER PSEUDO-CHANNEL at 4 Gb/channel, and
     * a channel is two pseudo-channels, so a channel carries 32 banks in 8
     * groups of 4 (Table 5 assigns 16 banks to four groups within a PC; the
     * channel has two such PCs). This object carried 4 groups x 4 banks = 16
     * per channel, which agreed with the pre-1.11.64 org preset -- both were
     * describing the same wrong part.
     *
     * THIS FIELD MUST MOVE WITH THE PRESET. The preset supplies bank ROWS
     * (via getPresetRowsPerBank -> pages_per_unit) while this object supplies
     * bank COUNT (via effectiveDramBanks -> total_units). Correcting the
     * preset's rows alone halved total_units x pages_per_unit, i.e. halved
     * the modelled capacity -- caught by gate 1174A before landing, and the
     * exact "separate blast radius" the DDR5 entry below warns about. The two
     * authorities are corrected together here.
     * Blast radius, as that note lists it: effectiveDramBanks() (total_units
     * 128 -> 256 at 8 channels), the CACTI area query, and the BankGroup
     * level's fanout in the hierarchy ladder. */
    arch->organization.banks_per_bank_group = 4;   // JESD235D Tbl 5 p.8
    arch->organization.bank_groups_per_chip = 8;   // 4 per pseudo-channel x 2 PC
    arch->organization.chips_per_rank = 8;  // 8 channels (not traditional "chips")
    arch->organization.ranks_per_channel = 1;  // Single stack
    arch->organization.subarray_size_kb = 1024;  // Larger than DDR4
    /* 1.11.63 (R6-2): 32 MB, THE PRESET'S BANK -- not 4 MB, which described no
     * part in this tree.
     *
     * Ruling R1 (1.11.61) re-based the DDR family's bank_size_mb on the preset
     * and left HBM's 4 MB untouched, because R2 was about chip_size_mb and
     * bank_size_mb rode along in the same exclusion. HBM2_4Gb spreads 4 Gb over
     * 16 banks per channel, so one bank is 512/16 = 32 MB. R6 removes the
     * exclusion: bank_size_mb means one bank of the simulated preset for EVERY
     * family, which is what the struct field says it means.
     *
     * THIS MOVES A NUMBER. bank_size_mb is the single hottest density consumer
     * -- main.cpp turns it into pages_per_unit and hence --pages-per-unit and
     * the emitted pagesPerUnit -- so an HBM2 BANK-placement element's
     * contiguous block goes from 4 MB to 32 MB, 8x, and the address-to-unit
     * map, PE locality and hop distance move with it. HBM3's goes 4 -> 16 MB,
     * 4x. Both need re-simulation. RamulatorWrapper::
     * applyPresetDensityToArchitecture() stamps the same value at runtime, so
     * this literal is a check on the derivation. */
    /* 1.11.64: 16 MB, following the corrected bank count. The derivation
     * the block above states is unchanged -- one bank of the simulated
     * preset -- but the divisor is now JEDEC's 32 banks per channel, not 16:
     * 512 MB / 32 = 16 MB. applyPresetDensityToArchitecture() stamps the
     * same value at runtime from the preset, so this literal remains a check
     * on that derivation rather than a second authority. */
    arch->organization.bank_size_mb = 16;  // preset HBM2_4Gb: 4 Gb/channel / 32 banks
    /* 1.11.61 (ruling R2): HBM KEEPS CORE-DIE SEMANTICS, and says so.
     *
     * 1024 MB is ONE CORE DIE, not one channel and not the stack. The preset
     * this tree simulates is HBM2_4Gb (HBM2.cpp:27), whose 4 Gb is a
     * PER-CHANNEL density -- HBM2.cpp's own error text calls it "channel
     * density" -- and a core die fronts TWO channels, so the die is
     * 2 x 4 Gb = 8 Gb = 1024 MB. That is exactly the part the die-area anchor
     * was measured from: K. Sohn et al., ISSCC 2016 paper 18.2 / IEEE JSSC
     * 52(1):250-260 Jan 2017, "each core die has 8 Gb DRAM cell array",
     * "the chip size is 12x8mm2" -- the same paper vendorDieDensity()'s HBM2
     * row cites, which is why HBM2 reports 96.00 mm^2/die exactly.
     *
     * So HBM is NOT re-based on the preset the way ruling R1 re-bases the DDR
     * family: naively following the per-channel density would halve this to
     * 512 MB and break the one HBM area figure that is currently right.
     * The reading is CHECKED rather than trusted -- see the capacity
     * cross-check in RamulatorWrapper::initialize(), which requires
     * chip_size_mb x (dies the preset stacks) to equal the preset's stack
     * capacity: 1024 x 4 = 4096 MB = 8 channels x 512 MB. It holds for HBM2.
     *
     * chips_per_rank above counts CHANNELS (8), not dies (4). The two fields
     * therefore count different units on purpose; see the note at the struct
     * field.
     *
     * 1.11.63 (R6-1): the same VALUE, now DERIVED rather than transcribed.
     * applyPresetDensityToArchitecture() computes it as the preset's
     * per-channel density x the channels one core die fronts, and HBM2
     * reproduces this literal exactly -- which is the check that the unit is
     * the right one. rank_size_gb was the only figure left disagreeing: 8 GB
     * against a stack of 8 channels x 512 MB = 4 GiB, which is also what
     * capacity_ reports. It is a dead field (no getter, no reader) and is
     * corrected only so the object does not hand a reader a fourth capacity. */
    arch->organization.chip_size_mb = 1024;  // ONE CORE DIE (2 channels x 4 Gb)
    arch->organization.rank_size_gb = 4;     // DERIVED: 8 channels x 512 MB stack

    // ===== TIMING (JEDEC HBM2; rate = the simulated HBM2_2.4Gbps bin) =====

    /* 1.11.56 (audit D002's defect, found by the B042 reconciliation check):
     * NAME THE BIN THIS TREE SIMULATES. The Ramulator preset selected for
     * HBM2 is HBM2_2.4Gbps (ramulator_wrapper.cpp), so the cycle counts this
     * simulator produces are a 2.4 GT/s part -- while this object said 2.0
     * GT/s and every bandwidth derived from it (chip I/O, rank, channel, and
     * since 1.11.56 the whole hierarchy link ladder) described a slower one.
     * One part, two speed bins, exactly the split D002 closed for DDR4. The
     * ns timings above are absolute and stay; the RATE follows the preset. */
    arch->timing.clock_freq_mhz = 1200;  // 2.4 GT/s DDR -> 1.2 GHz core
    arch->timing.data_rate_mtps = 2400;  // preset HBM2_2.4Gbps
    arch->timing.tRCD_ns = 16.0;  // JESD235C spec-minimum (was 12.5, pre-audit optimistic)
    arch->timing.tCAS_ns = 16.0;  // JESD235C
    arch->timing.tRP_ns = 12.5;  // VERIFIED
    arch->timing.tRAS_ns = 28.0;  // VERIFIED
    arch->timing.tBurst_ns = 8.0 * 1000.0 / 2400.0;  // 8 beats @ 2400 MT/s = 3.33 ns

    // Inner-bank datapath timing (INFERRED, much faster than DDR4 due to TSV!)
    arch->timing.inner_bank.column_decoder_ns = 0.25;       // Faster process node
    arch->timing.inner_bank.column_mux_ns = 0.35;           // Advanced technology
    arch->timing.inner_bank.subarray_output_drv_ns = 0.30;  // TSV-optimized
    arch->timing.inner_bank.local_io_ns = 0.40;             // Shorter distances
    arch->timing.inner_bank.htree_horizontal_ns = 0.40;     // TSV enables short paths
    arch->timing.inner_bank.htree_vertical_ns = 0.40;       // Vertical stacking
    arch->timing.inner_bank.global_io_ns = 0.60;            // 64-bit wide paths
    arch->timing.inner_bank.bank_io_driver_ns = 0.35;       // TSV-optimized
    arch->timing.inner_bank.verification_status = VerificationStatus::INFERRED;
    arch->timing.inner_bank.source =
        "INFERRED from CACTI with TSV technology assumptions, "
        "HBM architecture papers (shorter wires, 3D stacking, wider internal paths)";

    // Total inner-bank delay = 3.05ns (~2.2x faster than DDR4!)
    // Remaining for sense amp + row decoder: 12.5 - 3.05 = 9.45ns OK
    // TSVs enable much shorter H-tree paths and wider datapaths

    /* 1.11.59 (audit C016): DERIVED -- see deriveHierarchicalAccessTimes().
     * WAS 25.0 and 37.5; IS 32.0 (16.0 + 16.0) and 44.5 (12.5 + 16.0 + 16.0),
     * 22% and 16% higher. WHY they drifted: 25.0 = 12.5 + 12.5 and 37.5 = 3 x
     * 12.5 -- this object's arithmetic before release 1.1.1 raised tRCD and
     * tCAS from 12.5 to the JESD235C spec minimum 16.0 without moving the two
     * derived literals. NOT a 1.11.56 casualty: that release changed the rate,
     * the core clock and tBurst and left every ns timing alone. */
    arch->deriveHierarchicalAccessTimes();
    arch->timing.chip_access_ns = 45.0;  // Lower due to TSV
    arch->timing.rank_access_ns = 50.0;

    // ===== ENERGY (INFERRED) =====

    arch->energy.subarray_energy_pJ = 0.5;
    arch->energy.bank_energy_pJ = 1.0;
    arch->energy.chip_energy_pJ = 2.0;  // TSV is efficient
    arch->energy.rank_energy_pJ = 3.0;
    arch->energy.energy_source =
        "INFERRED from HBM architecture papers. "
        "TSV reduces energy vs long wire routing in DDR4.";

    // ===== PE BUS CONSTRAINTS (for PE placement) =====

    // Subarray-level PEs: Direct row buffer access (similar to DDR4)
    arch->pe_bus_constraints.subarray_level.data_bus_width_bits = 8192 * 8;  // 8KB row buffer
    arch->pe_bus_constraints.subarray_level.max_bandwidth_gbps = 15.0;  // Slightly higher than DDR4
    arch->pe_bus_constraints.subarray_level.row_buffer_size_bytes = 8192;
    arch->pe_bus_constraints.subarray_level.has_dedicated_bus = true;

    // Bank-level PEs: Share bank I/O bus (narrower than DDR4 per bank)
    arch->pe_bus_constraints.bank_level.data_bus_width_bits = 32;  // HBM has narrower per-bank paths
    arch->pe_bus_constraints.bank_level.max_bandwidth_gbps = 32.0;  // HBM2 per-channel BW / banks
    arch->pe_bus_constraints.bank_level.has_dedicated_bus = false;

    // Chip-level PEs: Share chip-level interconnect (via TSV)
    arch->pe_bus_constraints.chip_level.data_bus_width_bits = 128;  // Per-channel width
    arch->pe_bus_constraints.chip_level.max_bandwidth_gbps = 128.0;  // HBM2 per-channel (256 GB/s / 2 stacks)
    arch->pe_bus_constraints.chip_level.has_dedicated_bus = false;

    // Rank-level PEs: HBM uses stacks, not traditional ranks
    arch->pe_bus_constraints.rank_level.data_bus_width_bits = 1024;  // 8 channels x 128 bits
    arch->pe_bus_constraints.rank_level.max_bandwidth_gbps = 256.0;  // Full HBM2 bandwidth
    arch->pe_bus_constraints.rank_level.has_dedicated_bus = false;

    // Logic die level: Wide high-bandwidth interface (HBM advantage!)
    arch->pe_bus_constraints.logic_die_level.data_bus_width_bits = 1024;  // Full interface
    arch->pe_bus_constraints.logic_die_level.max_bandwidth_gbps = 256.0;  // HBM2 peak BW
    arch->pe_bus_constraints.logic_die_level.has_dedicated_bus = true;

    return arch;
}

/**
 * Create HBM2 with configurable port width scaling
 *
 * @param port_width_scale Scaling factor for all port widths (default 1.0)
 *                         2.0 = 2x wider buses, 0.5 = half-width buses
 * @return Configured HBM2 architecture with scaled port widths
 */
inline std::unique_ptr<DRAMArchitectureV2> createHBM2_Verified(double port_width_scale) {
    auto arch = createHBM2_Verified();
    arch->port_width_scale = port_width_scale;
    return arch;
}

//=============================================================================
// DDR5-4800 (Rigorously Verified)
//=============================================================================

inline std::unique_ptr<DRAMArchitectureV2> createDDR5_4800_Verified() {
    auto arch = std::make_unique<DRAMArchitectureV2>("DDR5-4800", "DDR5");

    // ===== DATAPATH STAGES =====

    // Stage 1: Row Buffer (same as DDR4)
    arch->datapath.row_buffer_bits = {
        8192,  // 8Kb typical
        VerificationStatus::VERIFIED,
        "JEDEC JESD79-5: DDR5 maintains similar subarray structure",
        "Row buffer size unchanged from DDR4"
    };

    // Stage 2: Global Sense Amplifiers
    arch->datapath.gsa_datapath_bits = {
        256,  // Similar to DDR4
        VerificationStatus::INFERRED,
        "Estimated similar to DDR4 architecture",
        "Column I/O width maintained"
    };

    // Stage 3: Prefetch Datapath (JEDEC verified - 16n prefetch!)
    arch->datapath.prefetch_datapath_bits = {
        128,  // 16n prefetch x 8-bit I/O
        VerificationStatus::VERIFIED,
        "JEDEC JESD79-5: DDR5 has 16n prefetch architecture, x8 device",
        "16 bursts x 8 bits = 128 bits prefetch buffer (2x DDR4!)"
    };

    // Stage 4: Bank Serialization (estimated, likely similar to DDR4)
    arch->datapath.bank_serialization_bits = {
        8,  // Estimated, same as DDR4
        VerificationStatus::ESTIMATED,
        "NOT DOCUMENTED! Estimated similar to DDR4",
        "Despite higher speed, internal paths likely remain narrow"
    };

    // Stage 5: Chip I/O (JEDEC verified)
    arch->datapath.chip_io_bits = {
        8,  // x8 device
        VerificationStatus::VERIFIED,
        "JEDEC JESD79-5: DDR5 x8 configuration",
        "External package pins, same as DDR4"
    };

    // Rank databus - DDR5 has two independent 32-bit subchannels!
    arch->datapath.rank_databus_bits = {
        64,  // 8 chips x 8 bits (or 2 subchannels x 32 bits)
        VerificationStatus::VERIFIED,
        "JEDEC JESD79-5: DDR5 introduces dual 32-bit subchannels",
        "TWO independent 32-bit channels per DIMM (key DDR5 feature!)"
    };

    // Channel
    arch->datapath.channel_databus_bits = {
        64,  // Single channel
        VerificationStatus::VERIFIED,
        "Standard DDR5 memory controller interface",
        "Per-DIMM interface width"
    };

    // ===== BANDWIDTH LIMITS (INFERRED) =====

    /* 1.11.57 (audit C003): the retired literal said "DDR5-4800: 8 bits x
     * 2.4 GHz" -- a bin this tree does not simulate (1.11.56 moved this object
     * to the DDR5_3200AN preset) and a clock this object does not carry
     * (clock_freq_mhz is 1600). Derived now. */
    arch->bandwidth_limits.inference_method =
        "DERIVED: bank = (bank_serialization_bits / 8) x clock_freq_GHz, on an "
        "8-bit bank path estimated to be DDR4's; bank group = bank x 2 (the "
        "bank-group port multiplier, UNSOURCED).";
    arch->bandwidth_limits.confidence_level = "Medium - based on DDR4 scaling";

    // ===== ORGANIZATION (VERIFIED) =====

    /* 1.11.61 (ruling R1): THE DENSITY FOLLOWS THE PRESET.
     *
     * DDR5's preset is DDR5_8Gb_x8, {8<<10, 8, {1, 1, 8, 2, 1<<16, 1<<10}} at
     * DDR5.cpp:16 -- an 8 Gb device, 65536 rows x 1024 columns x 8 bits =
     * 1024 B per row. The lines below read 4 / 2 MB / 256 MB / 2 GB, i.e. a
     * 2 Gb part; the die came out at 6.35 mm^2 against a row whose own source
     * is "Micron D1a, 8 Gb / 25.41 mm^2 (TechInsights)". At the preset's
     * density it lands at 25.40 mm^2, on that measured part.
     *
     * 1.11.66 (round 5, A2/F2): THE RESIDUAL IS RESOLVED, AND THE PRESET WAS
     * RIGHT. The 1.11.61 note that stood here claimed "a JEDEC DDR5 x8 device
     * really has 32 banks" and kept this object at 8 BG x 4 Ba against the
     * preset's 8 BG x 2. JESD79-5D (misc/, published Nov 2025), Table 4
     * printed p.7: the 8 Gb x8 addressing is "BG Address BG0~BG2 | Bank
     * Address in a BG BA0 | # BG / # Banks per BG / # Banks = 8 / 2 / 16".
     * 32 banks (BA0~BA1) begins at 16 Gb (Tables 5-7). The preset
     * DDR5_8Gb_x8 = {8 BG, 2 Ba} was correct and 1.11.64's JESD79-5D pass
     * verified it; this object and main.cpp's per-tech table were the two
     * wrong authorities -- the HBM2 1174A defect class with the sign
     * reversed. Consequences while wrong: tech_min_banks forced >= 256
     * banks per rank where the part has 128, so total_units x pages_per_unit
     * modelled an address space 2x the simulated rank, and the bus-shared PE
     * count read 32 where the device has 16. Both authorities now carry
     * JEDEC's 8 Gb shape, 32 x 64 MB is no longer claimed, and 16 x 64 MB =
     * 1024 MB reproduces chip_size_mb -- the two authorities agree. The
     * blast radius the old note named (CACTI area query, effectiveDramBanks)
     * is exactly what moves, and moves to the part. */
    arch->organization.subarrays_per_bank = 128;  // DERIVED: 64 MB bank / 512 KB subarray
    arch->organization.banks_per_bank_group = 2;  // JESD79-5D Tbl 4 p.7: 8 Gb x8 = BA0 -> 2 per BG
    arch->organization.bank_groups_per_chip = 8;  // JESD79-5D Tbl 4 p.7: BG0~BG2 -> 8 groups
    /* 1.11.72: the two literals above describe the 8 Gb part and are the
     * FALLBACK ONLY. RamulatorWrapper::applyPresetBankGroupingToArchitecture()
     * stamps the simulated preset's grouping at runtime -- 4 banks/BG for the
     * 16 Gb part the default 4800B grade selects (JESD79-5D Table 4), 2 for
     * the 8 Gb part at grade 3200 -- so every consumer of getBanksPerBankGroup()
     * sees the part the cycles are counted for. */
    arch->organization.chips_per_rank = 8;  // x8 organization
    arch->organization.ranks_per_channel = 2;  // Typical DIMM
    arch->organization.subarray_size_kb = 512;  // Typical
    arch->organization.bank_size_mb = 64;   // preset DDR5_8Gb_x8: 8 Gb / 16 banks
    arch->organization.chip_size_mb = 1024; // preset DDR5_8Gb_x8: 8 Gb device
    arch->organization.rank_size_gb = 8;    // DERIVED: 8 x 1024 MB

    // ===== TIMING (JEDEC DDR5; rate = the simulated DDR5_3200AN bin) =====

    /* 1.11.56 (audit D002's defect, found by the B042 reconciliation check):
     * the Ramulator preset selected for DDR5 is DDR5_3200AN, so this tree
     * simulates a 3200 MT/s part. This object said 4800 -- a 1.5x gap
     * between the bin whose cycles are counted and the bin whose bandwidth
     * is reported as mem.bandwidth (the device M/D/1 service rate). The ns
     * timings are absolute and stay within JEDEC's range across bins; the
     * RATE and the burst follow the preset. */
    arch->timing.clock_freq_mhz = 1600;  // 3200 MT/s DDR -> 1.6 GHz core
    arch->timing.data_rate_mtps = 3200;  // preset DDR5_3200AN
    /* 1.11.59: the bin label was stale. 1.11.56 re-binned this object to the
     * DDR5_3200AN preset this tree simulates; the ns timings are absolute and
     * stayed, but this comment still named the 4800 MT/s bin they were
     * originally quoted at. The VALUE is unchanged and within JEDEC's range
     * across bins -- only the label was wrong.
     *
     * 1.11.63 (R6-5): AND THAT WAS THE DEFECT, NOT THE LABEL. "Absolute ns" is
     * true of a JEDEC timing and says nothing about WHICH BIN's absolute ns
     * these are: 16.67 is the 4800 bin's, and the preset this tree counts
     * cycles against is DDR5_3200AN -- nCL/nRCD/nRP 24, nRAS 52, at
     * tCK = 1E6/(3200/2) = 625 ps (DDR5.cpp timing_presets, tCK derived at
     * DDR5.cpp:392). That is 15.00 ns, so the object was 11% high on the three
     * timings a run reports and prices with, for every release since 1.11.56
     * moved the rate and left them. RamulatorWrapper::
     * applyPresetTimingsToArchitecture() stamps these four at runtime; the
     * literals are the same arithmetic so the stamp is a check.
     *
     * MOVES: tRCD/tCAS/tRP 16.67 -> 15.00 (-10.0%), tRAS 32.0 -> 32.5
     * (JEDEC_rounding: 52 whole cycles). tRC = tRAS + tRP falls 48.67 ->
     * 47.50 ns, so DDR5's array activate energy falls with it, and
     * bank_access_ns falls 50.01 -> 45.00. */
    arch->timing.tRCD_ns = 24 * 0.625;  // DERIVED: DDR5_3200AN nRCD 24 x tCK 625 ps
    arch->timing.tCAS_ns = 24 * 0.625;  // DERIVED: nCL 24
    arch->timing.tRP_ns = 24 * 0.625;   // DERIVED: nRP 24
    arch->timing.tRAS_ns = 52 * 0.625;  // DERIVED: nRAS 52
    arch->timing.tBurst_ns = 16.0 * 1000.0 / 3200.0;  // 16 beats @ 3200 MT/s = 5.0 ns

    // Inner-bank datapath timing (INFERRED - scaled from DDR4 with better process)
    arch->timing.inner_bank.column_decoder_ns = 0.30;       // Faster process
    arch->timing.inner_bank.column_mux_ns = 0.50;
    arch->timing.inner_bank.subarray_output_drv_ns = 0.45;
    arch->timing.inner_bank.local_io_ns = 0.70;
    arch->timing.inner_bank.htree_horizontal_ns = 1.00;     // Better layout
    arch->timing.inner_bank.htree_vertical_ns = 1.00;
    arch->timing.inner_bank.global_io_ns = 1.30;
    arch->timing.inner_bank.bank_io_driver_ns = 0.55;
    arch->timing.inner_bank.verification_status = VerificationStatus::INFERRED;
    arch->timing.inner_bank.source =
        "INFERRED from DDR4 CACTI model with process scaling for DDR5 (7nm-10nm node)";

    /* 1.11.59 (audit C016): DERIVED -- see deriveHierarchicalAccessTimes().
     * These were the literals 33.34 and 50.01, and DDR5 is the other
     * technology whose literals still reproduced their own declared sums
     * (16.67 + 16.67, 3 x 16.67). Nothing moves here. */
    arch->deriveHierarchicalAccessTimes();
    arch->timing.chip_access_ns = 70.0;  // ESTIMATED
    arch->timing.rank_access_ns = 90.0;  // ESTIMATED

    // ===== ENERGY (INFERRED) =====

    arch->energy.subarray_energy_pJ = 0.8;  // Lower voltage (1.1V vs 1.2V)
    arch->energy.bank_energy_pJ = 1.6;
    arch->energy.chip_energy_pJ = 4.0;
    arch->energy.rank_energy_pJ = 8.0;
    arch->energy.energy_source =
        "INFERRED from DDR4 with voltage scaling (1.1V vs 1.2V = ~15% lower energy)";

    // ===== PE BUS CONSTRAINTS (for PE placement) =====

    // Subarray-level PEs
    arch->pe_bus_constraints.subarray_level.data_bus_width_bits = 8192 * 8;
    arch->pe_bus_constraints.subarray_level.max_bandwidth_gbps = 12.0;
    arch->pe_bus_constraints.subarray_level.row_buffer_size_bytes = 8192;
    arch->pe_bus_constraints.subarray_level.has_dedicated_bus = true;

    // Bank-level PEs
    arch->pe_bus_constraints.bank_level.data_bus_width_bits = 64;
    arch->pe_bus_constraints.bank_level.max_bandwidth_gbps = 38.4;  // DDR5-4800 per-bank BW
    arch->pe_bus_constraints.bank_level.has_dedicated_bus = false;

    // Chip-level PEs
    arch->pe_bus_constraints.chip_level.data_bus_width_bits = 64;
    arch->pe_bus_constraints.chip_level.max_bandwidth_gbps = 38.4;
    arch->pe_bus_constraints.chip_level.has_dedicated_bus = false;

    // Rank-level PEs
    arch->pe_bus_constraints.rank_level.data_bus_width_bits = 64;
    arch->pe_bus_constraints.rank_level.max_bandwidth_gbps = 38.4;
    arch->pe_bus_constraints.rank_level.has_dedicated_bus = false;

    // Logic die level (not applicable for DDR5)
    arch->pe_bus_constraints.logic_die_level.data_bus_width_bits = 1024;
    arch->pe_bus_constraints.logic_die_level.max_bandwidth_gbps = 256.0;
    arch->pe_bus_constraints.logic_die_level.has_dedicated_bus = true;

    return arch;
}

inline std::unique_ptr<DRAMArchitectureV2> createDDR5_4800_Verified(double port_width_scale) {
    auto arch = createDDR5_4800_Verified();
    arch->port_width_scale = port_width_scale;
    return arch;
}

//=============================================================================
// HBM3 (Rigorously Verified)
//=============================================================================

inline std::unique_ptr<DRAMArchitectureV2> createHBM3_Verified() {
    auto arch = std::make_unique<DRAMArchitectureV2>("HBM3", "HBM3");

    // ===== DATAPATH STAGES =====

    // Stage 1: Row Buffer
    arch->datapath.row_buffer_bits = {
        16384,  // 16Kb typical (same as HBM2)
        VerificationStatus::INFERRED,
        "Typical HBM3 subarray, similar to HBM2",
        "Wide rows for high bandwidth"
    };

    // Stage 2: Global Sense Amplifiers
    arch->datapath.gsa_datapath_bits = {
        512,  // Similar to HBM2
        VerificationStatus::INFERRED,
        "Estimated from HBM3 architecture papers",
        "Wider column I/O for high throughput"
    };

    // Stage 3: Prefetch Datapath (JEDEC verified - 8n prefetch!)
    arch->datapath.prefetch_datapath_bits = {
        512,  // 8n prefetch x 64-bit pseudo-channel
        VerificationStatus::VERIFIED,
        "JEDEC JESD238: HBM3 has 8n prefetch, 64-bit pseudo-channels",
        "8 bursts x 64 bits = 512 bits (4x DDR4!)"
    };

    // Stage 4: Bank Serialization (wider than HBM2 due to higher speed TSV)
    arch->datapath.bank_serialization_bits = {
        128,  // INFERRED: wider TSV for higher bandwidth
        VerificationStatus::INFERRED,
        "ESTIMATED from 2x HBM2 bandwidth with improved TSV technology",
        "TSV improvements enable wider internal paths"
    };

    // Stage 5: Chip I/O (JEDEC verified)
    arch->datapath.chip_io_bits = {
        1024,  // 16 pseudo-channels x 64 bits
        VerificationStatus::VERIFIED,
        "JEDEC JESD238: HBM3 has 16 pseudo-channels of 64-bit each",
        "Total 1024-bit interface (same as HBM2, but higher speed)"
    };

    // Per pseudo-channel
    arch->datapath.rank_databus_bits = {
        64,  // Per pseudo-channel
        VerificationStatus::VERIFIED,
        "JEDEC JESD238: 64-bit pseudo-channel width",
        "HBM3 uses pseudo-channels vs full channels"
    };

    /* 1.11.57 (audit C007): ONE channel, not the stack -- see the HBM2 note.
     * At 16 channels this was the 16x case: the ladder's L5 rung was 1024 bits
     * / 819.2 GB/s where the rung under it is 64 bits / 51.2 GB/s, and the
     * system root multiplied that stack figure by 16 again. */
    arch->datapath.channel_databus_bits = {
        64,  // one channel; the stack is 16 of these (JESD238)
        VerificationStatus::VERIFIED,
        "JEDEC JESD238: 64-bit channel, 16 channels per stack",
        "One channel's data bus; stack width = this x channels"
    };

    // ===== BANDWIDTH LIMITS (INFERRED) =====

    /* 1.11.57 (audit C003): this is the row that cost the most. "16.0 GB/s,
     * 2x HBM2" against a 128-bit bank path implies a 1.0 GHz array clock; this
     * object's core clock is 3.2 GHz, so the ladder clocked HBM3's L1 and L2
     * at a third of the object's own array speed and charged every crossing on
     * the reference cell accordingly. Derived now: 128 / 8 x 3.2 = 51.2 GB/s,
     * which is the same statement as "one 128-bit beat per core clock". */
    arch->bandwidth_limits.inference_method =
        "DERIVED: bank = (bank_serialization_bits / 8) x clock_freq_GHz, on an "
        "estimated 128-bit improved-TSV bank path; bank group = bank x 2 (the "
        "bank-group port multiplier, UNSOURCED).";
    arch->bandwidth_limits.confidence_level = "Medium - TSV improvements not fully specified";

    // ===== ORGANIZATION (VERIFIED) =====

    arch->organization.subarrays_per_bank = 4;
    /* 1.11.66 (round 5, B3): 8 bank groups per CHANNEL, not 4 -- the same
     * per-pseudo-channel-vs-per-channel slip 1.11.64 fixed on HBM2. JESD238B
     * Table 4 gives BA[3:0] = 16 banks per pseudo-channel in 4 groups; a
     * channel is two pseudo-channels, so 32 banks in 8 groups per channel --
     * which is what the preset {2 Pch, 4 Bg, 4 Ba}, main.cpp's per-tech
     * table and three vendor papers (Ryu p.1052, Park p.257, Lee p.239) all
     * say. This object was the one authority still at 16, and bank_size_mb
     * below already divided by 32. Latent (nothing HBM3 read this field for
     * a live number in 1.11.65) but a disagreement in the register. */
    arch->organization.banks_per_bank_group = 4;  // JESD238B Tbl 4/5: 4 per group
    arch->organization.bank_groups_per_chip = 8;  // 4 per pseudo-channel x 2 PC
    arch->organization.chips_per_rank = 16;  // 16 pseudo-channels
    arch->organization.ranks_per_channel = 1;  // Single stack
    arch->organization.subarray_size_kb = 1024;  // Same as HBM2
    /* 1.11.63 (R6-2): 16 MB, the preset's bank -- HBM3_4Gb spreads 4 Gb per
     * channel over 2 pseudo-channels x 4 BG x 4 banks = 32 banks, so
     * 512/32 = 16 MB. See the HBM2 factory for the full note; HBM3's
     * pages_per_unit moves 4x with this. */
    arch->organization.bank_size_mb = 16;  // preset HBM3_4Gb: 4 Gb/channel / 32 banks
    /* 1.11.61 (ruling R2) -> 1.11.63 (R6-1): RECONCILED. The three-authority
     * disagreement this comment used to record is resolved, and this note
     * records the resolution rather than the disagreement.
     *
     * WHAT WAS OPEN. Three numbers in this tree claimed HBM3's capacity and no
     * two agreed:
     *   this object   8 core dies x 2048 MB = 16 GiB
     *   the preset    16 channels x 4 Gb    =  8 GiB  (HBM3_4Gb, HBM3.cpp)
     *   the wrapper   capacity_             =  4 GiB  (ramulator_wrapper.cpp)
     * Ruling R2 deliberately kept 2048 and made the capacity cross-check FIRE
     * on every HBM3 run so the disagreement could not be missed, leaving the
     * choice of which authority moves to a ruling of its own. Unlike HBM2
     * there is no measured anchor here that could arbitrate:
     * vendorDieDensity()'s HBM3 row is a density (0.16 Gb/mm^2, SemiAnalysis),
     * not a die capacity.
     *
     * THE RULING (R6, user, 2026-08-24): PIMID relies on the Ramulator models,
     * those models are calibrated to JEDEC and the vendor data in misc/, and
     * PIMID ITSELF PROVIDES NO NUMBERS -- so a PIMID-side literal that
     * paraphrases a preset must be DERIVED from it. The wrapper literal has no
     * standing; the object follows the preset. On the core-die unit R2 fixed,
     * the preset implies 2 channels x 4 Gb = 8 Gb = 1024 MB per core die, and
     * 1024 x 8 dies = 8 GiB per stack -- the preset's stack exactly.
     *
     * WHAT MOVES: the reported DRAM die area HALVES, 100.00 -> 50.00 mm^2/die,
     * and the memory total 800 -> 400 mm^2 (the die-area path is linear in
     * chip_size_mb because the JEDEC calibration cancels the raw CACTI run).
     * capacity_ in the wrapper rises 4 -> 8 GiB, being derived from the same
     * preset. The capacity cross-check now RECONCILES on HBM3, so a firing
     * there is a real error again and not an expected note.
     *
     * The value is not written here as a literal either: applyPresetDensity-
     * ToArchitecture() derives it, and this line is the check. */
    arch->organization.chip_size_mb = 1024;  // DERIVED: ONE CORE DIE (2 channels x 4 Gb)
    arch->organization.rank_size_gb = 8;     // DERIVED: 16 channels x 512 MB stack

    // ===== TIMING (JEDEC HBM3; rate = the simulated HBM3_6.4Gbps bin) =====

    /* 1.11.56 (audit D002's defect, found by the B042 reconciliation check):
     * the Ramulator preset selected for HBM3 is HBM3_6.4Gbps and the wrapper
     * asserts bandwidth_ = 819000 MB/s (1024 b x 6.4 GT/s / 8) beside it.
     * This object said 4.0 GT/s, so its rank/channel/chip-I/O bandwidths --
     * and, since 1.11.56, the hierarchy link ladder derived from them --
     * described a part 1.6x slower than the one whose cycles are counted.
     * That 1.6x is what made HBM3's three bandwidth scopes fail to
     * reconcile. The ns timings are absolute and stay; the RATE follows. */
    arch->timing.clock_freq_mhz = 3200;  // 6.4 GT/s DDR -> 3.2 GHz core
    arch->timing.data_rate_mtps = 6400;  // preset HBM3_6.4Gbps
    arch->timing.tRCD_ns = 16.0;  // JESD238 spec-minimum (was 10.0, below physical minimum)
    arch->timing.tCAS_ns = 16.0;  // JESD238
    arch->timing.tRP_ns = 10.0;  // VERIFIED
    arch->timing.tRAS_ns = 24.0;  // VERIFIED
    arch->timing.tBurst_ns = 8.0 * 1000.0 / 6400.0;  // 8 beats @ 6400 MT/s = 1.25 ns

    // Inner-bank datapath timing (INFERRED - faster than HBM2)
    arch->timing.inner_bank.column_decoder_ns = 0.20;
    arch->timing.inner_bank.column_mux_ns = 0.30;
    arch->timing.inner_bank.subarray_output_drv_ns = 0.25;
    arch->timing.inner_bank.local_io_ns = 0.35;
    arch->timing.inner_bank.htree_horizontal_ns = 0.35;
    arch->timing.inner_bank.htree_vertical_ns = 0.35;
    arch->timing.inner_bank.global_io_ns = 0.50;
    arch->timing.inner_bank.bank_io_driver_ns = 0.30;
    arch->timing.inner_bank.verification_status = VerificationStatus::INFERRED;
    arch->timing.inner_bank.source =
        "INFERRED from HBM2 with advanced process scaling (5nm logic die)";

    /* 1.11.59 (audit C016): DERIVED -- see deriveHierarchicalAccessTimes().
     * WAS 20.0 and 30.0; IS 32.0 (16.0 + 16.0) and 42.0 (10.0 + 16.0 + 16.0),
     * 37% and 29% higher -- the largest of the four gaps. WHY they drifted:
     * 20.0 = 10.0 + 10.0 and 30.0 = 3 x 10.0 -- this object's arithmetic
     * before release 1.1.1 raised tRCD and tCAS from 10.0 to the JESD238 spec
     * minimum 16.0 without moving the two derived literals. NOT a 1.11.56
     * casualty: that release changed the rate, the core clock and tBurst and
     * left every ns timing alone. */
    arch->deriveHierarchicalAccessTimes();
    arch->timing.chip_access_ns = 35.0;
    arch->timing.rank_access_ns = 40.0;

    // ===== ENERGY (INFERRED) =====

    arch->energy.subarray_energy_pJ = 0.4;  // Lower due to advanced process
    arch->energy.bank_energy_pJ = 0.8;
    arch->energy.chip_energy_pJ = 1.5;
    arch->energy.rank_energy_pJ = 2.5;
    arch->energy.energy_source =
        "INFERRED from HBM2 with process/voltage scaling for advanced nodes";

    // ===== PE BUS CONSTRAINTS (for PE placement) =====

    // Subarray-level PEs
    arch->pe_bus_constraints.subarray_level.data_bus_width_bits = 8192 * 8;
    arch->pe_bus_constraints.subarray_level.max_bandwidth_gbps = 20.0;
    arch->pe_bus_constraints.subarray_level.row_buffer_size_bytes = 8192;
    arch->pe_bus_constraints.subarray_level.has_dedicated_bus = true;

    // Bank-level PEs
    arch->pe_bus_constraints.bank_level.data_bus_width_bits = 64;
    arch->pe_bus_constraints.bank_level.max_bandwidth_gbps = 64.0;
    arch->pe_bus_constraints.bank_level.has_dedicated_bus = false;

    // Chip-level PEs
    arch->pe_bus_constraints.chip_level.data_bus_width_bits = 128;
    arch->pe_bus_constraints.chip_level.max_bandwidth_gbps = 256.0;
    arch->pe_bus_constraints.chip_level.has_dedicated_bus = false;

    // Rank-level PEs (stack level for HBM)
    arch->pe_bus_constraints.rank_level.data_bus_width_bits = 1024;
    arch->pe_bus_constraints.rank_level.max_bandwidth_gbps = 512.0;  // HBM3 peak
    arch->pe_bus_constraints.rank_level.has_dedicated_bus = false;

    // Logic die level (HBM3 advantage!)
    arch->pe_bus_constraints.logic_die_level.data_bus_width_bits = 1024;
    arch->pe_bus_constraints.logic_die_level.max_bandwidth_gbps = 512.0;
    arch->pe_bus_constraints.logic_die_level.has_dedicated_bus = true;

    return arch;
}

inline std::unique_ptr<DRAMArchitectureV2> createHBM3_Verified(double port_width_scale) {
    auto arch = createHBM3_Verified();
    arch->port_width_scale = port_width_scale;
    return arch;
}

//=============================================================================
// Verification Report
//=============================================================================

inline void DRAMArchitectureV2::printVerificationReport() const {
    std::cout << "\n+==================================================================+\n";
    std::cout << "|  VERIFICATION REPORT: " << std::left << std::setw(43) << name << "|\n";
    std::cout << "+==================================================================+\n\n";

    /* 1.11.56 (audit D073): ASCII TAGS. These strings carried emoji and box-
     * drawing characters, which violates the project's ASCII-only rule for
     * source and generated files and makes the log unreadable on any terminal
     * or parser that is not UTF-8. The status a line reports is unchanged;
     * only its glyph is. The rest of this file was swept the same way (times
     * signs, arrows, superscripts, degree and micro signs in comments). */
    auto printStatus = [](const VerifiedValue& v) {
        std::string status_str;
        switch (v.status) {
            case VerificationStatus::VERIFIED:  status_str = "[VERIFIED]  "; break;
            case VerificationStatus::INFERRED:  status_str = "[INFERRED]  "; break;
            case VerificationStatus::ESTIMATED: status_str = "[ESTIMATED] "; break;
            case VerificationStatus::UNKNOWN:   status_str = "[UNKNOWN]   "; break;
        }
        std::cout << "  " << status_str << std::setw(6) << v.value_bits << " bits\n";
        std::cout << "     Source: " << v.source << "\n";
        if (!v.notes.empty()) {
            std::cout << "     Notes:  " << v.notes << "\n";
        }
        std::cout << "\n";
    };

    std::cout << "DATAPATH STAGES:\n";
    std::cout << "-----------------\n";
    std::cout << "Row Buffer:\n";
    printStatus(datapath.row_buffer_bits);

    std::cout << "Global Sense Amplifiers (GSA):\n";
    printStatus(datapath.gsa_datapath_bits);

    std::cout << "Prefetch Datapath:\n";
    printStatus(datapath.prefetch_datapath_bits);

    std::cout << "Bank Serialization (CRITICAL BOTTLENECK!):\n";
    printStatus(datapath.bank_serialization_bits);

    std::cout << "Chip I/O:\n";
    printStatus(datapath.chip_io_bits);

    std::cout << "Rank Data Bus:\n";
    printStatus(datapath.rank_databus_bits);

    std::cout << "\nBANDWIDTH LIMITS (INFERRED):\n";
    std::cout << "----------------------------\n";
    // 1.11.57 (audit C003): derived on read, so this line cannot print a
    // bandwidth that the object's own width and clock do not produce.
    std::cout << "  Bank effective BW:       " << getBankEffectiveBW() << " GB/s\n";
    std::cout << "  Bank Group effective BW: " << getBankGroupEffectiveBW() << " GB/s\n";
    std::cout << "  Method: " << bandwidth_limits.inference_method << "\n";
    std::cout << "  Confidence: " << bandwidth_limits.confidence_level << "\n";

    std::cout << "\nENERGY:\n";
    std::cout << "-------\n";
    std::cout << "  Source: " << energy.energy_source << "\n";
    std::cout << "  Subarray: " << energy.subarray_energy_pJ << " pJ/byte\n";
    std::cout << "  Bank:     " << energy.bank_energy_pJ << " pJ/byte\n";

    std::cout << "\n";
}

inline double DRAMArchitectureV2::getRankBW() const {
    return (datapath.rank_databus_bits.value_bits / 8.0) * (timing.data_rate_mtps / 1000.0);
}

inline std::string DRAMArchitectureV2::getOverallConfidence() const {
    // Assess overall confidence based on critical parameters
    if (datapath.bank_serialization_bits.status == VerificationStatus::ESTIMATED ||
        datapath.bank_serialization_bits.status == VerificationStatus::UNKNOWN) {
        return "MEDIUM - Bank serialization width is estimated (critical for PIM!)";
    }
    return "HIGH - Most critical parameters verified from JEDEC or papers";
}

} // namespace memory
} // namespace pimid

#endif // PIMID_DRAM_ARCHITECTURE_V2_H
