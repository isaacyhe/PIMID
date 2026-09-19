/**
 * @file architecture_extractor.h
 * @brief Integration layer to extract architecture specs from external models
 *
 * This file provides functions to populate architecture specifications
 * from external simulation models (CACTI, NVSim, Ramulator) instead of
 * using hard-coded values.
 *
 * INTEGRATION FLOW:
 *   External Model -> Wrapper -> Extractor -> Architecture Spec
 *
 *   CACTI 7.0      -> CACTIWrapper      -> extractSRAMArchitecture()  -> SRAMArchitecture
 *   NVSim          -> NVSimWrapper      -> extractNVMArchitecture()   -> STTMRAMArchitecture
 *   Ramulator2     -> RamulatorWrapper  -> (uses DRAMArchitectureV2 directly)
 *
 * VERSION: 1.0
 * DATE: January 2026
 */

#ifndef PIMID_ARCHITECTURE_EXTRACTOR_H
#define PIMID_ARCHITECTURE_EXTRACTOR_H

#include "memory/cacti_wrapper.h"
#include "memory/nvsim_wrapper.h"
#include "memory/ramulator_wrapper.h"
#include "memory/sram_architecture.h"
#include "memory/sttmram_architecture.h"
#include "memory/pcm_architecture.h"
#include "memory/reram_architecture.h"
#include "memory/dram_architecture_v2.h"

#include <memory>
#include <string>
#include <cmath>
#include <iostream>   // 1.11.57 (latent D061): refusals are announced, not silent

namespace pimid {
namespace memory {

//=============================================================================
// SRAM Architecture Extraction from CACTI 7.0
//=============================================================================

/**
 * @brief Extract SRAM architecture from CACTI wrapper results
 *
 * This function populates a SRAMArchitecture struct using actual values
 * extracted from CACTI 7.0 analysis, replacing hard-coded defaults.
 *
 * @param cacti_wrapper Initialized CACTIWrapper with valid results
 * @param name Architecture name (e.g., "SRAM-L3-Extracted")
 * @param clock_freq_ghz Target clock frequency for cycle calculations
 * @return Populated SRAMArchitecture, or nullptr if extraction fails
 */
inline std::unique_ptr<SRAMArchitecture> extractSRAMArchitecture(
    const CACTIWrapper& cacti_wrapper,
    const std::string& name = "SRAM-Extracted",
    double clock_freq_ghz = 3.0) {

    if (!cacti_wrapper.isValid()) {
        return nullptr;
    }

    const auto& config = cacti_wrapper.getConfig();
    auto arch = std::make_unique<SRAMArchitecture>(name, "SRAM");

    // Process node from config
    arch->process_node = std::to_string(static_cast<int>(config.tech_node_nm)) + "nm";

    // ===== ORGANIZATION (from CACTI config and results) =====
    arch->organization.banks_per_chip = config.banks;
    arch->organization.chip_size_mb = config.capacity_bytes / (1024 * 1024);
    arch->organization.bank_size_kb = (config.capacity_bytes / config.banks) / 1024;

    /* 1.11.73: the tier below the bank is the SUBBANK -- CACTI's line of
     * num_act_mats_hor_dir mats activated together for one data word
     * (parameter.cc 2110/2239: its width is out_w, the bank's). Counts come
     * from CACTI's own geometry (Ndwl x Ndbl subarrays, num_submarray_mats
     * per mat, num_active_mats per access); the old getMatsPerBank() was
     * returning the active-mat count under the wrong name. */
    uint32_t subarrays_per_mat = cacti_wrapper.getSubarraysPerMat();
    uint32_t mats_per_bank = cacti_wrapper.getMatsPerBank();
    uint32_t mats_per_subbank = cacti_wrapper.getActiveMatsPerAccess();
    uint32_t subbanks_per_bank = cacti_wrapper.getSubbanksPerBank();

    arch->organization.subbanks_per_bank = subbanks_per_bank > 0 ? subbanks_per_bank : 1;
    arch->organization.mats_per_subbank = mats_per_subbank > 0 ? mats_per_subbank : 1;

    // Mat grid: one row of mats per subbank, the subbanks stacked
    arch->organization.mats_per_bank_rows = arch->organization.subbanks_per_bank;
    arch->organization.mats_per_bank_cols = arch->organization.mats_per_subbank;
    if (mats_per_bank > 0 &&
        static_cast<uint32_t>(arch->organization.getMatsPerBank()) != mats_per_bank) {
        // geometry CACTI reports but did not split evenly: keep the total honest
        arch->organization.mats_per_bank_rows = 1;
        arch->organization.mats_per_bank_cols = static_cast<int>(mats_per_bank);
    }

    arch->organization.mat_size_kb = arch->organization.bank_size_kb /
                                      arch->organization.getMatsPerBank();

    arch->organization.subarrays_per_mat = subarrays_per_mat > 0 ? subarrays_per_mat : 4;
    arch->organization.subarray_size_kb = arch->organization.mat_size_kb /
                                           arch->organization.subarrays_per_mat;

    arch->organization.rows_per_subarray = cacti_wrapper.getSubarrayRows();
    arch->organization.cols_per_subarray = cacti_wrapper.getSubarrayCols();

    if (arch->organization.rows_per_subarray == 0) arch->organization.rows_per_subarray = 512;
    if (arch->organization.cols_per_subarray == 0) arch->organization.cols_per_subarray = 256;

    // ===== TIMING (from CACTI 7.0 extraction) =====
    arch->timing.clock_freq_ghz = clock_freq_ghz;

    // Total access latencies (extracted)
    /* 1.11.23: the SRAM tier ladder, corrected -- the same defect the NVM
     * extractors had, and CACTI even hands us the separating term.
     *
     * WAS: getAccessTime() assigned to SUBARRAY, then mat = x1.1, and
     * bank = getCycleTime(). Two errors in four lines. getAccessTime() is the
     * full array access INCLUDING the H-tree (CACTI H-trees its mats exactly
     * as NVSim does), so it is the BANK figure, not the subarray one. And
     * getCycleTime() is the random cycle time -- it includes precharge and
     * restore and is NOT an access latency, so it never belonged on the bank
     * tier at all.
     *
     * NOW, with getHtreeDelay() -- which CACTI exposes and nothing used:
     *   subbank  = the in-array component path (1.11.73: ONE tier below the
     *              bank, the subbank; a PE there reads its mats without the
     *              bank H-tree, so the in-array path IS its access. The old
     *              subarray/mat pair described CACTI internals, not tiers.)
     *   bank     = getAccessTime(), CACTI's own full-array number
     *   chip     = bank + ONE configured network hop, raised by the caller
     *              (SRAM is not DRAM-like: bank groups and ranks collapse to
     *              1 and the chip network is ours to specify)
     * cycle time is kept, correctly labelled, for throughput not latency. */
    const double sram_sub_ns =
        (cacti_wrapper.getDecoderDelay() + cacti_wrapper.getWordlineDelay() +
         cacti_wrapper.getBitlineDelay() + cacti_wrapper.getSenseAmpDelay() +
         cacti_wrapper.getSubarrayOutputDelay()) * 1e9;
    arch->timing.subbank_access_ns  = sram_sub_ns;
    arch->timing.bank_access_ns     = cacti_wrapper.getAccessTime() * 1e9;
    /* 1.11.73: the tier below cannot be slower than the tier above. CACTI's
     * access time is H-tree in + in-mat path + H-tree out (+ routing and mux
     * stages), so the in-mat sum must sit below it; if it does not, the
     * components are not describing this array and the tier is REFUSED
     * (-1 -> unsourceable), never used. */
    {
        const double htree_ns = cacti_wrapper.getHtreeDelay() * 1e9;
        const double resid_ns = arch->timing.bank_access_ns - htree_ns - sram_sub_ns;
        std::cout << "  [SRAM] subbank in-mat path " << sram_sub_ns << " ns + H-trees "
                  << htree_ns << " ns vs CACTI access " << arch->timing.bank_access_ns
                  << " ns (residual " << resid_ns << " ns: route-to-bank and mux stages)"
                  << std::endl;
        if (!(sram_sub_ns > 0.0) || sram_sub_ns > arch->timing.bank_access_ns) {
            std::cerr << "  [SRAM] NOTE: CACTI component path (" << sram_sub_ns
                      << " ns) is not below the array access time ("
                      << arch->timing.bank_access_ns << " ns); the SUBBANK tier is "
                         "reported unsourceable rather than priced from it." << std::endl;
            arch->timing.subbank_access_ns = -1.0;
        }
    }
    arch->timing.chip_access_ns     = arch->timing.bank_access_ns;
    arch->timing.cycle_time_ns      = cacti_wrapper.getCycleTime() * 1e9;

    // Inner-bank breakdown (EXTRACTED from CACTI 7.0!)
    arch->timing.inner_bank.row_decoder_ns = cacti_wrapper.getDecoderDelay() * 1e9;
    arch->timing.inner_bank.wordline_ns = cacti_wrapper.getWordlineDelay() * 1e9;
    arch->timing.inner_bank.bitline_ns = cacti_wrapper.getBitlineDelay() * 1e9;
    arch->timing.inner_bank.sense_amp_ns = cacti_wrapper.getSenseAmpDelay() * 1e9;
    arch->timing.inner_bank.column_mux_ns = 0.15;  // Not directly available, use typical
    arch->timing.inner_bank.subarray_output_drv_ns = cacti_wrapper.getSubarrayOutputDelay() * 1e9;

    // H-tree delay (split into horizontal/vertical)
    double htree_total = cacti_wrapper.getHtreeDelay() * 1e9;
    arch->timing.inner_bank.htree_horizontal_ns = htree_total / 2.0;
    arch->timing.inner_bank.htree_vertical_ns = htree_total / 2.0;

    // Local/global I/O (estimate from total)
    double remaining = arch->timing.subbank_access_ns -
                       arch->timing.inner_bank.getRowPath() -
                       arch->timing.inner_bank.getColumnPath() -
                       htree_total;
    arch->timing.inner_bank.local_io_ns = remaining * 0.4;
    arch->timing.inner_bank.global_io_ns = remaining * 0.4;
    arch->timing.inner_bank.bank_output_drv_ns = remaining * 0.2;

    if (arch->timing.inner_bank.local_io_ns < 0) arch->timing.inner_bank.local_io_ns = 0.2;
    if (arch->timing.inner_bank.global_io_ns < 0) arch->timing.inner_bank.global_io_ns = 0.3;
    if (arch->timing.inner_bank.bank_output_drv_ns < 0) arch->timing.inner_bank.bank_output_drv_ns = 0.15;

    /* 1.11.23: see the STT-MRAM block -- the component delays are tool-read,
     * the local/global I/O and output-driver terms are not, so the block is
     * INFERRED. A VERIFIED stamp over asserted values is a false provenance
     * claim, which is the defect this release removes. */
    arch->timing.inner_bank.verification_status = VerificationStatus::INFERRED;
    arch->timing.inner_bank.source = "CACTI 7.0 extraction, " + arch->process_node + " process";

    // ===== ENERGY (EXTRACTED from CACTI 7.0!) =====
    arch->energy.subbank_energy_pJ = cacti_wrapper.getDecoderEnergy() * 1000.0 +
                                      cacti_wrapper.getWordlineEnergy() * 1000.0 +
                                      cacti_wrapper.getBitlineEnergy() * 1000.0 +
                                      cacti_wrapper.getSenseAmpEnergy() * 1000.0;
    arch->energy.bank_energy_pJ = cacti_wrapper.getDynamicReadEnergy() * 1000.0;  // nJ to pJ
    arch->energy.chip_energy_pJ = arch->energy.bank_energy_pJ * 1.2;

    // Energy per byte
    double bytes_per_access = config.line_size;
    arch->energy.subbank_energy_per_byte = arch->energy.subbank_energy_pJ / bytes_per_access;
    arch->energy.bank_energy_per_byte = arch->energy.bank_energy_pJ / bytes_per_access;
    arch->energy.chip_energy_per_byte = arch->energy.chip_energy_pJ / bytes_per_access;

    // Leakage (EXTRACTED from CACTI 7.0!)
    arch->energy.subbank_leakage_mw = cacti_wrapper.getArrayLeakage();
    arch->energy.bank_leakage_mw = cacti_wrapper.getLeakagePower() / config.banks;
    arch->energy.chip_leakage_mw = cacti_wrapper.getLeakagePower();

    arch->energy.energy_source = "CACTI 7.0 extraction, " + arch->process_node;

    // ===== DATAPATH =====
    /* 1.11.73: the subbank's width is the bank's width by CACTI's own
     * construction (num_do_b_subbank = out_w) -- no literal. The chip port
     * remains an inference (SRAM's chip network is ours to specify). */
    arch->datapath.bank_io_bits = static_cast<int>(cacti_wrapper.getOutputWidthBits());
    arch->datapath.subbank_io_bits = arch->datapath.bank_io_bits;
    arch->datapath.chip_io_bits = arch->datapath.bank_io_bits * 4;  // Wider at chip level

    arch->datapath.verification_status = VerificationStatus::INFERRED;
    arch->datapath.source = "CACTI 7.0: subbank/bank width = out_w; chip port inferred";

    return arch;
}

//=============================================================================
// STT-MRAM Architecture Extraction from NVSim
//=============================================================================

/**
 * @brief Extract STT-MRAM architecture from NVSim wrapper results
 */
inline std::unique_ptr<STTMRAMArchitecture> extractSTTMRAMArchitecture(
    const NVSimWrapper& nvsim_wrapper,
    const std::string& name = "STTMRAM-Extracted",
    double clock_freq_ghz = 2.0) {

    if (!nvsim_wrapper.isValid()) {
        return nullptr;
    }

    const auto& config = nvsim_wrapper.getConfig();
    auto arch = std::make_unique<STTMRAMArchitecture>(name, "STT-MRAM");

    arch->process_node = std::to_string(config.process_node_nm) + "nm";

    // ===== ORGANIZATION =====
    int num_banks = nvsim_wrapper.getNumBanks();
    if (num_banks == 0) num_banks = 64;  // Default
    arch->organization.banks_per_chip = num_banks;

    // Estimate grid organization (approximate square)
    int sqrt_banks = static_cast<int>(std::sqrt(num_banks));
    arch->organization.bank_rows = sqrt_banks > 0 ? sqrt_banks : 8;
    arch->organization.bank_cols = num_banks / arch->organization.bank_rows;

    size_t chip_mb = config.capacity_bytes / (1024 * 1024);
    arch->organization.chip_size_mb = chip_mb > 0 ? chip_mb : 8;
    arch->organization.bank_size_kb = (arch->organization.chip_size_mb * 1024) / num_banks;

    /* 1.11.73: the tier below the bank is NVSim's MAT (numRowMat x
     * numColumnMat per bank). Cached since 1.11.73; an older cache entry
     * yields 0 = UNKNOWN, and the run reports the tier unsourceable rather
     * than filling it (the old code multiplied two counts into a subarray
     * total and fell back to a literal 8). */
    const uint32_t mats_per_bank = nvsim_wrapper.getMatsPerBank();
    arch->organization.mats_per_bank = static_cast<int>(mats_per_bank);
    arch->organization.mat_size_kb = (mats_per_bank > 0)
        ? arch->organization.bank_size_kb / mats_per_bank : 0;

    arch->organization.wordlines_per_subarray = nvsim_wrapper.getSubarrayRows();
    arch->organization.bitlines_per_subarray = nvsim_wrapper.getSubarrayCols();

    if (arch->organization.wordlines_per_subarray == 0)
        arch->organization.wordlines_per_subarray = 512;
    if (arch->organization.bitlines_per_subarray == 0)
        arch->organization.bitlines_per_subarray = 256;

    // ===== TIMING (EXTRACTED from NVSim!) =====
    arch->timing.clock_freq_ghz = clock_freq_ghz;

    // Total access latencies
    /* 1.11.23 (user ruling): the tier ladder, corrected. NVSimWrapper::
     * getReadLatency() returns nvsim_result_->bank->readLatency -- it is the
     * BANK figure. Assigning it to subarray and then inflating by 1.2 for
     * bank charged every sub-bank placement the bank latency, and made
     * subarray and bank differ only by an invented constant. That constant
     * WAS the Figure-2 tier separation for these technologies.
     *
     * SRAM/NVM are not DRAM-like: bank groups and ranks collapse to 1
     * (main.cpp, the SRAM/STT_MRAM/PCM/RERAM branch) and none of these
     * headers even declares a bank_group/rank/channel field. The ladder is
     * exactly three levels with a simple interconnect we specify, so:
     *
     *   subarray  = the NVSim component delays, summed -- the path inside
     *               the array, which NVSim resolves directly
     *   bank      = NVSim's own bank->readLatency (what getReadLatency is)
     *   chip      = bank + ONE configured network hop, supplied by the
     *               caller from getTransferLatency(CHIP). Not a multiplier:
     *               the chip-level network for these technologies is ours to
     *               specify, so our network model owns it, exactly as
     *               Ramulator owns the DRAM hierarchy JEDEC fixes.
     *
     * chip_* is left at the bank value here and RAISED by the caller once the
     * network term is known; a caller that never supplies it reports
     * chip == bank, which is the correct floor rather than a guess. */
    /* 1.11.25 CORRECTION: this was the sum of five wrapper accessors that look
     * like NVSim reads and, at the time, were not -- getDecoderDelay() and
     * friends returned INVENTED percentages of mat.readLatency (10/20/45/15/5,
     * summing to 0.95). Summing them replaced one invented multiplier with
     * another. (1.11.56, audit D031: those five now read NVSim's own component
     * latencies, so the objection no longer applies to them -- but the direct
     * subarray read below is still the right thing to use for a TIER, because
     * it is the figure NVSim composed rather than a sum we compose.)
     * NVSim resolves the subarray directly (SubArray derives from
     * FunctionUnit; Mat holds one), so read it. <0 means unavailable --
     * notably on a cache hit, since the pregenerated NVSim cache predates
     * these fields -- and the tier is then reported unsourceable, not
     * filled. */
    const double stt_sub_s = nvsim_wrapper.getMatLatency();  /* 1.11.73: the MAT, one tier below the bank */
    const double stt_sub_read_ns = (stt_sub_s > 0.0) ? stt_sub_s * 1e9 : -1.0;
    arch->timing.bank_read_ns  = nvsim_wrapper.getReadLatency()  * 1e9;
    arch->timing.bank_write_ns = nvsim_wrapper.getWriteLatency() * 1e9;
    arch->timing.mat_read_ns = stt_sub_read_ns;
    /* the write path shares the array traversal and adds the cell's own
     * switching time, which NVSim reports separately as the cell latency */
    arch->timing.mat_write_ns =
        stt_sub_read_ns + nvsim_wrapper.getCellWriteLatency() * 1e9;
    arch->timing.chip_read_ns  = arch->timing.bank_read_ns;
    arch->timing.chip_write_ns = arch->timing.bank_write_ns;

    /* Inner-bank breakdown.
     * 1.11.57 (latent C011): ONLY WHEN THERE IS A BREAKDOWN TO READ. NVSim's
     * per-component accessors need nvsim_result_, and a CACHE HIT -- the normal
     * path, since characterizations are pregenerated -- leaves that null and
     * returns 0.0 from all five. So this block was writing five zeros and then
     * stamping them "NVSim: decoder/wordline/bitline/sense-amp/column-mux",
     * which made getTotalReadLatency() the sum of the three ASSERTED I/O
     * constants alone (1.6 ns) under a tool's name. A zero presented as a
     * measurement is worse than the fraction 1.11.56 replaced. When the
     * breakdown is absent, say so in the provenance string instead. */
    const bool stt_has_components = nvsim_wrapper.hasComponentBreakdown();
    arch->timing.inner_bank.row_decoder_ns = nvsim_wrapper.getDecoderDelay() * 1e9;
    arch->timing.inner_bank.wordline_ns = nvsim_wrapper.getWordlineDelay() * 1e9;
    arch->timing.inner_bank.bitline_read_ns = nvsim_wrapper.getBitlineDelay() * 1e9;
    arch->timing.inner_bank.bitline_write_ns = arch->timing.inner_bank.bitline_read_ns * 2.0;  // Write slower
    arch->timing.inner_bank.sense_amp_ns = nvsim_wrapper.getSenseAmpDelay() * 1e9;
    arch->timing.inner_bank.column_mux_ns = nvsim_wrapper.getColumnDecoderDelay() * 1e9;
    /* 1.11.23: NVSim reports the cell's own write latency; the 0.6 fraction
     * of a composed number was an assertion about where the time goes. */
    arch->timing.inner_bank.mtj_switching_ns = nvsim_wrapper.getCellWriteLatency() * 1e9;

    // Remaining components
    arch->timing.inner_bank.local_io_ns = 0.5;
    arch->timing.inner_bank.global_io_ns = 0.8;
    arch->timing.inner_bank.bank_output_drv_ns = 0.3;

    /* 1.11.23: truthful provenance. This block was stamped VERIFIED and
     * attributed wholesale to NVSim, including values NVSim never supplied --
     * which is worse than an uncommented literal, because the field actively
     * claimed a tool produced them. Tool-read: row_decoder, wordline,
     * bitline_read, sense_amp, column_mux, and (1.11.23) mtj_switching from
     * getCellWriteLatency. NOT tool-read: local_io_ns, global_io_ns,
     * bank_output_drv_ns -- NVSim exposes no equivalent, so the block is
     * INFERRED, not VERIFIED, until they are sourced or removed. */
    arch->timing.inner_bank.verification_status =
        stt_has_components ? VerificationStatus::INFERRED
                           : VerificationStatus::UNKNOWN;
    arch->timing.inner_bank.source = stt_has_components
        ? ("NVSim: decoder/wordline/bitline/sense-amp/column-mux/cell-write; "
           "ASSERTED (unsourced): local_io, global_io, bank_output_drv -- "
           + arch->process_node)
        : ("NOT SOURCED: this characterization came from the NVSim result "
           "CACHE, which carries only the top-level figures, so the "
           "per-component delays are ZERO and the inner-bank total is the "
           "ASSERTED local_io/global_io/bank_output_drv terms alone. "
           "Re-characterize (empty the nvsim cache) to obtain them -- "
           + arch->process_node);

    // ===== ENERGY (EXTRACTED from NVSim!) =====
    /* 1.11.57 (latent D059): the five component-energy accessors are NVSim's
     * own terms now (they were 10/20/40/20/10 percentages of one mat scalar);
     * they are also PER SUBARRAY now, which is what this field claims to be.
     * On the cache path they are zero for the same reason as the delays above,
     * so this field is 0 rather than a fraction of something invented -- and
     * energy_source below says which case a run is in. */
    arch->energy.mat_read_energy_pJ = nvsim_wrapper.getDecoderEnergy() * 1000.0 +
                                            nvsim_wrapper.getWordlineEnergy() * 1000.0 +
                                            nvsim_wrapper.getBitlineEnergy() * 1000.0 +
                                            nvsim_wrapper.getSenseAmpEnergy() * 1000.0;
    arch->energy.mat_write_energy_pJ = arch->energy.mat_read_energy_pJ * 3.0;  // Write higher

    arch->energy.bank_read_energy_pJ = nvsim_wrapper.getReadDynamicEnergy() * 1000.0;  // nJ to pJ
    arch->energy.bank_write_energy_pJ = nvsim_wrapper.getWriteDynamicEnergy() * 1000.0;

    arch->energy.chip_read_energy_pJ = arch->energy.bank_read_energy_pJ * 1.2;
    arch->energy.chip_write_energy_pJ = arch->energy.bank_write_energy_pJ * 1.2;

    // Per-byte energy
    double bytes_per_access = config.word_width_bits / 8.0;
    if (bytes_per_access <= 0) bytes_per_access = 8.0;
    arch->energy.read_energy_per_byte = arch->energy.bank_read_energy_pJ / bytes_per_access;
    arch->energy.write_energy_per_byte = arch->energy.bank_write_energy_pJ / bytes_per_access;

    // Leakage (EXTRACTED from NVSim!)
    /* 1.11.57 (latent D059): NO SECOND DIVISION. getSenseAmpLeakage() used to
     * be 30% of the MAT leakage, so this divided by banks * subarrays to get
     * back to one subarray. It now returns NVSim's own
     * subarray.senseAmp.leakage, which already IS per subarray -- dividing
     * again would report a sense amplifier as a fraction of itself. */
    arch->energy.mat_leakage_mw = nvsim_wrapper.getSenseAmpLeakage();
    /* 1.11.56 (audit D058): ONE BASIS FOR NVSIM'S LEAKAGE, AND IT IS PER-BANK.
     *
     * This block used to read NVSim's figure as a WHOLE-CHIP number: chip =
     * getLeakagePower(), bank = getLeakagePower() / banks_per_chip. main.cpp's
     * device-power path reads the SAME call the other way round -- it asks
     * NVSim for one 64 KB bank and then multiplies by config.num_banks -- so
     * one run's stdout carried two leakage numbers for the same array that
     * differed by the bank count, with nothing saying which was which.
     *
     * The per-bank reading is the correct one, and NVSim says so itself:
     * getNumBanks() returns 1 with the comment "NVSim models a single bank",
     * and every live caller hands NVSim the PER-BANK capacity (main.cpp sets
     * setArrayCapacityBytes(PER_BANK_BYTES) = 64 KB before the plugin models
     * characterize anything). NVSim reports the leakage of the array it was
     * asked to model; that array is one bank. So the bank tier IS the tool's
     * figure, undivided, and the chip tier is banks_per_chip of them.
     *
     * Numerically this is inert while banks_per_chip comes from getNumBanks()
     * (it is 1, so chip == bank either way). It is fixed anyway because the
     * previous form was a division by a count that could only ever be right by
     * accident, and because the two sides of the report now state the same
     * basis. */
    arch->energy.bank_leakage_mw = nvsim_wrapper.getLeakagePower();
    arch->energy.chip_leakage_mw = nvsim_wrapper.getLeakagePower() *
                                   arch->organization.banks_per_chip;

    /* 1.11.57 (latent D059): the bank/chip figures are tool reads; the
     * subarray read energy and leakage are only tool reads when there was a
     * result tree to break down. Say which. */
    arch->energy.energy_source = stt_has_components
        ? ("NVSim extraction, " + arch->process_node)
        : ("NVSim extraction (bank/chip only; subarray components ABSENT -- "
           "cached characterization), " + arch->process_node);

    // ===== DATAPATH =====
    /* 1.11.57 (latent D057): NVSim REPORTS NO DATAPATH WIDTH. Two of these
     * three are literals and the third is an input echoed back, yet the block
     * stamped itself VERIFIED and attributed the lot to "NVSim extraction" --
     * a provenance claim about values no tool produced, which is worse than an
     * uncommented literal because the field asserts the opposite of the truth.
     * Nothing reads verification_status today, so no number moved; the stamp
     * would have been believed the first time a consumer appeared. */
    /* 1.11.73: the mat's width is NVSim's mat.numDataBit (cached; 0 from an
     * older cache = UNKNOWN, reported unsourceable). The literal 64 is gone. */
    arch->datapath.mat_io_bits = static_cast<int>(nvsim_wrapper.getMatWidthBits());
    arch->datapath.bank_io_bits = config.word_width_bits > 0 ? config.word_width_bits : 64;
    arch->datapath.chip_io_bits = arch->datapath.bank_io_bits * 4;

    arch->datapath.verification_status = VerificationStatus::ESTIMATED;
    arch->datapath.source =
        "mat_io_bits = NVSim mat.numDataBit (0 = unknown from an older cache); "
        "chip_io_bits = 4 x bank ASSERTED; bank_io_bits is the CONFIGURED word "
        "width, not a tool output.";

    // ===== ENDURANCE =====
    /* 1.11.57 (latent D063): NVSim MODELS NO ENDURANCE. The source string
     * said "NVSim + STT-MRAM literature" over three literals, half-crediting a
     * tool that has no endurance model at all and naming no paper for the
     * other half. Neither number nor citation is sourceable here, so the field
     * says exactly that. Note the models do not read these; PCMModel carries
     * its own copy of the endurance literal. */
    arch->endurance.write_cycles = 1e15;  // STT-MRAM typical
    arch->endurance.retention_years = 10.0;
    arch->endurance.ecc_required = true;
    arch->endurance.endurance_source =
        "ASSERTED (unsourced literals): NVSim does not model endurance or "
        "retention. No citation is attached to 1e15 cycles / 10 years.";

    return arch;
}

//=============================================================================
// Helper: Update existing architecture from wrapper
//=============================================================================

/**
 * @brief Update an existing SRAM architecture with CACTI-extracted values
 *
 * This is useful when you want to keep the factory-created structure
 * but override specific values with CACTI results.
 */
inline void updateSRAMArchitectureFromCACTI(
    SRAMArchitecture& arch,
    const CACTIWrapper& cacti_wrapper) {

    if (!cacti_wrapper.isValid()) return;

    // Update timing breakdown
    arch.timing.inner_bank.row_decoder_ns = cacti_wrapper.getDecoderDelay() * 1e9;
    arch.timing.inner_bank.wordline_ns = cacti_wrapper.getWordlineDelay() * 1e9;
    arch.timing.inner_bank.bitline_ns = cacti_wrapper.getBitlineDelay() * 1e9;
    arch.timing.inner_bank.sense_amp_ns = cacti_wrapper.getSenseAmpDelay() * 1e9;
    arch.timing.inner_bank.subarray_output_drv_ns = cacti_wrapper.getSubarrayOutputDelay() * 1e9;

    double htree_total = cacti_wrapper.getHtreeDelay() * 1e9;
    arch.timing.inner_bank.htree_horizontal_ns = htree_total / 2.0;
    arch.timing.inner_bank.htree_vertical_ns = htree_total / 2.0;

    // Update access times
    arch.timing.subbank_access_ns = cacti_wrapper.getAccessTime() * 1e9;
    arch.timing.bank_access_ns = cacti_wrapper.getCycleTime() * 1e9;

    // Update energy
    arch.energy.bank_energy_pJ = cacti_wrapper.getDynamicReadEnergy() * 1000.0;
    arch.energy.chip_leakage_mw = cacti_wrapper.getLeakagePower();

    // Update verification status
    /* 1.11.23: see the STT-MRAM block -- the component delays are tool-read,
     * the local/global I/O and output-driver terms are not, so the block is
     * INFERRED. A VERIFIED stamp over asserted values is a false provenance
     * claim, which is the defect this release removes. */
    arch.timing.inner_bank.verification_status = VerificationStatus::INFERRED;
    arch.timing.inner_bank.source = "CACTI 7.0 extraction (updated)";
}

/* 1.11.57 (latent D045-adjacent): updateSTTMRAMArchitectureFromNVSim,
 * updatePCMArchitectureFromNVSim and updateReRAMArchitectureFromNVSim are
 * DELETED. All three had zero callers anywhere in the tree, and all three were
 * stale duplicates of the extract*() functions above that still carried the
 * defects those were repaired for: the PCM one assigned
 * `mat_reset_ns = write_ns * 0.3`, the assertion 1.11.23 removed from the
 * extractor and whose release note said it was gone; all three assigned
 * getReadLatency() (NVSim's BANK figure) straight into subarray_*, the tier
 * collapse 1.11.23 corrected; all three stamped
 * "NVSim extraction (updated)" over the per-component delays that are ZERO on
 * the cached path (latent C011). A dead function is how a removed number gets
 * back in: it looks like the maintained API, so the next caller reintroduces
 * every defect at once. If an in-place update is ever wanted, call the
 * corresponding extract*() and assign the result. */

//=============================================================================
// PCM Architecture Extraction from NVSim
//=============================================================================

/**
 * @brief Extract PCM architecture from NVSim wrapper results
 *
 * PCM (Phase-Change Memory) has unique characteristics:
 * - Very slow SET writes (crystallization: 50-150ns)
 * - Faster RESET writes (amorphization: 10-50ns)
 * - Moderate read latency (6-12ns)
 * - Limited endurance (~10^8 writes)
 */
inline std::unique_ptr<PCMArchitecture> extractPCMArchitecture(
    const NVSimWrapper& nvsim_wrapper,
    const std::string& name = "PCM-Extracted",
    double clock_freq_ghz = 0.5) {

    if (!nvsim_wrapper.isValid()) {
        return nullptr;
    }

    const auto& config = nvsim_wrapper.getConfig();
    auto arch = std::make_unique<PCMArchitecture>(name, "PCM");

    arch->process_node = std::to_string(config.process_node_nm) + "nm";

    // ===== ORGANIZATION =====
    int num_banks = nvsim_wrapper.getNumBanks();
    if (num_banks == 0) num_banks = 32;  // PCM default
    arch->organization.banks_per_chip = num_banks;

    // Estimate grid organization
    int sqrt_banks = static_cast<int>(std::sqrt(num_banks));
    arch->organization.bank_rows = sqrt_banks > 0 ? sqrt_banks : 8;
    arch->organization.bank_cols = num_banks / arch->organization.bank_rows;

    size_t chip_mb = config.capacity_bytes / (1024 * 1024);
    arch->organization.chip_size_mb = chip_mb > 0 ? chip_mb : 16;
    arch->organization.bank_size_kb = (arch->organization.chip_size_mb * 1024) / num_banks;

    /* 1.11.73: NVSim's mat count, cached; 0 = UNKNOWN from an older cache
     * (reported unsourceable, no literal 4 fallback). */
    const uint32_t mats_per_bank = nvsim_wrapper.getMatsPerBank();
    arch->organization.mats_per_bank = static_cast<int>(mats_per_bank);
    arch->organization.mat_size_kb = (mats_per_bank > 0)
        ? arch->organization.bank_size_kb / mats_per_bank : 0;

    arch->organization.wordlines_per_mat = nvsim_wrapper.getSubarrayRows();
    arch->organization.bitlines_per_mat = nvsim_wrapper.getSubarrayCols();

    if (arch->organization.wordlines_per_mat == 0) arch->organization.wordlines_per_mat = 1024;
    if (arch->organization.bitlines_per_mat == 0) arch->organization.bitlines_per_mat = 1024;

    // ===== TIMING (EXTRACTED from NVSim!) =====
    arch->timing.clock_freq_ghz = clock_freq_ghz;

    // Read latencies
    /* 1.11.57 (latent D061): REFUSE, DO NOT FILL. This read `if
     * (read_latency_ns <= 0) read_latency_ns = 8.0;  // PCM typical` and, below,
     * `= 100.0;  // PCM SET typical` -- two unmarked literals on the path whose
     * whole doctrine (1.11.24) is that a failed tool binding must refuse rather
     * than report invented specs. They are unreachable today only because
     * pcm_model.cpp gates extraction on isValid() and every valid wrapper
     * returns a positive cached or computed latency; a corrupt or zeroed cache
     * entry would have silently priced this array at 8 ns / 100 ns while every
     * log line said NVSim. A non-positive latency from NVSim means the
     * characterization is broken, so return nullptr and let the model throw
     * with its existing message. */
    double read_latency_ns = nvsim_wrapper.getReadLatency() * 1e9;
    if (read_latency_ns <= 0) {
        std::cerr << "[extractPCMArchitecture] NVSim returned a non-positive "
                     "read latency (" << read_latency_ns << " ns). Refusing to "
                     "substitute the old 8.0 ns literal: the characterization "
                     "is broken (most likely a corrupt or zeroed NVSim cache "
                     "entry). Delete the cache entry and re-characterize."
                  << std::endl;
        return nullptr;
    }
    /* 1.11.23: same tier correction as STT-MRAM. getReadLatency() is NVSim's
     * BANK figure, so it IS the bank tier; the subarray is the sum of the
     * component delays NVSim resolves inside the array; the chip tier is bank
     * plus ONE configured network hop, raised by the caller (PCM is not
     * DRAM-like: bank groups and ranks collapse to 1 and the chip-level
     * network is ours to specify). 1.25 and 1.5 were assertions. */
    /* 1.11.25 CORRECTION: this was the sum of five wrapper accessors that look
     * like NVSim reads and, at the time, were not -- getDecoderDelay() and
     * friends returned INVENTED percentages of mat.readLatency (10/20/45/15/5,
     * summing to 0.95). Summing them replaced one invented multiplier with
     * another. (1.11.56, audit D031: those five now read NVSim's own component
     * latencies, so the objection no longer applies to them -- but the direct
     * subarray read below is still the right thing to use for a TIER, because
     * it is the figure NVSim composed rather than a sum we compose.)
     * NVSim resolves the subarray directly (SubArray derives from
     * FunctionUnit; Mat holds one), so read it. <0 means unavailable --
     * notably on a cache hit, since the pregenerated NVSim cache predates
     * these fields -- and the tier is then reported unsourceable, not
     * filled. */
    const double pcm_sub_s = nvsim_wrapper.getMatLatency();  /* 1.11.73: the MAT, one tier below the bank */
    const double pcm_sub_read_ns = (pcm_sub_s > 0.0) ? pcm_sub_s * 1e9 : -1.0;
    arch->timing.bank_read_ns     = read_latency_ns;
    arch->timing.mat_read_ns = pcm_sub_read_ns;
    arch->timing.chip_read_ns     = read_latency_ns;

    // Write latencies (PCM has VERY asymmetric SET/RESET)
    // 1.11.57 (latent D061): refuse, do not fill -- see the read path above.
    double write_latency_ns = nvsim_wrapper.getWriteLatency() * 1e9;
    if (write_latency_ns <= 0) {
        std::cerr << "[extractPCMArchitecture] NVSim returned a non-positive "
                     "write latency (" << write_latency_ns << " ns). Refusing "
                     "to substitute the old 100.0 ns literal; re-characterize."
                  << std::endl;
        return nullptr;
    }

    // SET is slowest (crystallization), RESET is faster (amorphization)
    /* 1.11.23: SET is a path NVSim resolves (FunctionUnit::setLatency); it was
     * taken as the generic write latency and then inflated. */
    /* 1.11.56 (audit D062): RECORD THE SUBSTITUTION. getSetLatency() has no
     * cached_ branch, so it returns -1 on every cache hit -- and a cache hit is
     * the normal path, since the pregenerated NVSim entries carry only the
     * top-level figures. That meant the SET tier was almost always the generic
     * write latency while tierLatencySource() went on reporting "NVSim
     * FunctionUnit::setLatency" for it. The RESET branch below already
     * collapses to SET out loud; SET's collapse to the write path was silent,
     * which is the worse of the two failures: a run could not tell whether it
     * was reading a resolved SET path or a substituted one. The flag is what
     * the model prints and what tierLatencySource() attributes from. */
    {
        const double set_s = nvsim_wrapper.getSetLatency();
        const double set_ns = (set_s > 0.0) ? set_s * 1e9 : write_latency_ns;
        arch->timing.set_from_generic_write = !(set_s > 0.0);
        arch->timing.bank_set_ns     = set_ns;
        arch->timing.mat_set_ns = pcm_sub_read_ns;
        arch->timing.chip_set_ns     = set_ns;
    }

    // RESET is ~30% of SET time (amorphization is faster)
    /* 1.11.23: RESET likewise (FunctionUnit::resetLatency). The 0.3 fraction
     * was the least defensible number in this file -- PCM's RESET is a
     * melt-quench pulse whose duration is a CELL property, not a fraction of a
     * composed write latency. If NVSim does not resolve it, report the SET
     * path rather than manufacture a RESET number. */
    {
        const double rst_s = nvsim_wrapper.getResetLatency();
        const double rst_ns = (rst_s > 0.0) ? rst_s * 1e9 : -1.0;
        if (rst_ns > 0.0) {
            arch->timing.bank_reset_ns     = rst_ns;
            arch->timing.mat_reset_ns = pcm_sub_read_ns;
            arch->timing.chip_reset_ns     = rst_ns;
        } else {
            arch->timing.bank_reset_ns     = arch->timing.bank_set_ns;
            arch->timing.mat_reset_ns = arch->timing.mat_set_ns;
            arch->timing.chip_reset_ns     = arch->timing.chip_set_ns;
        }
    }

    /* Inner-bank breakdown.
     * 1.11.57 (latent C011): only when there IS a breakdown -- see the
     * STT-MRAM block. On a cache hit these five accessors return 0.0 and the
     * inner-bank total collapses to the asserted bus/IO constants, so the
     * provenance string below must not keep saying NVSim. */
    const bool pcm_has_components = nvsim_wrapper.hasComponentBreakdown();
    arch->timing.inner_bank.row_decoder_ns = nvsim_wrapper.getDecoderDelay() * 1e9;
    arch->timing.inner_bank.wordline_ns = nvsim_wrapper.getWordlineDelay() * 1e9;
    arch->timing.inner_bank.bitline_read_ns = nvsim_wrapper.getBitlineDelay() * 1e9;
    arch->timing.inner_bank.sense_amp_external_ns = nvsim_wrapper.getSenseAmpDelay() * 1e9;
    arch->timing.inner_bank.column_mux_ns = nvsim_wrapper.getColumnDecoderDelay() * 1e9;
    arch->timing.inner_bank.mat_output_drv_ns = 0.30;

    // PCM uses bus routing (not always H-tree)
    arch->timing.inner_bank.bus_horizontal_ns = 0.75;
    arch->timing.inner_bank.bus_vertical_ns = 0.75;
    arch->timing.inner_bank.global_io_ns = 0.60;
    arch->timing.inner_bank.bank_output_drv_ns = 0.30;

    // PCM-specific write pulses
    arch->timing.inner_bank.set_pulse_ns = arch->timing.mat_set_ns * 0.9;  // Crystallization
    arch->timing.inner_bank.reset_pulse_ns = arch->timing.mat_reset_ns * 0.9;  // Amorphization

    /* 1.11.23: see the STT-MRAM block -- the component delays are tool-read,
     * the local/global I/O and output-driver terms are not, so the block is
     * INFERRED. A VERIFIED stamp over asserted values is a false provenance
     * claim, which is the defect this release removes. */
    arch->timing.inner_bank.verification_status =
        pcm_has_components ? VerificationStatus::INFERRED
                           : VerificationStatus::UNKNOWN;
    arch->timing.inner_bank.source = pcm_has_components
        ? ("NVSim extraction, " + arch->process_node + " PCM")
        : ("NOT SOURCED: cached characterization -- the per-component delays "
           "are ZERO and the inner-bank total is the ASSERTED bus/IO terms "
           "alone. Re-characterize to obtain them. " + arch->process_node
           + " PCM");

    // ===== ENERGY (EXTRACTED from NVSim!) =====
    double read_energy_nJ = nvsim_wrapper.getReadDynamicEnergy();
    double write_energy_nJ = nvsim_wrapper.getWriteDynamicEnergy();

    /* 1.11.57 (latent D059): NVSim's own per-subarray component energies, not
     * percentages of a mat scalar; zero when the run came from the cache. */
    arch->energy.mat_read_energy_pJ = (nvsim_wrapper.getDecoderEnergy() +
                                             nvsim_wrapper.getWordlineEnergy() +
                                             nvsim_wrapper.getBitlineEnergy() +
                                             nvsim_wrapper.getSenseAmpEnergy()) * 1000.0;
    arch->energy.bank_read_energy_pJ = read_energy_nJ * 1000.0;
    arch->energy.chip_read_energy_pJ = arch->energy.bank_read_energy_pJ * 1.7;

    // SET energy is much higher (crystallization requires sustained current)
    arch->energy.mat_set_energy_pJ = arch->energy.mat_read_energy_pJ * 40.0;
    arch->energy.bank_set_energy_pJ = write_energy_nJ * 1000.0;
    arch->energy.chip_set_energy_pJ = arch->energy.bank_set_energy_pJ * 1.5;

    // RESET energy is moderate
    arch->energy.mat_reset_energy_pJ = arch->energy.mat_set_energy_pJ * 0.5;
    arch->energy.bank_reset_energy_pJ = arch->energy.bank_set_energy_pJ * 0.55;
    arch->energy.chip_reset_energy_pJ = arch->energy.chip_set_energy_pJ * 0.55;

    // Per-byte energy
    double bytes_per_access = config.word_width_bits / 8.0;
    if (bytes_per_access <= 0) bytes_per_access = 8.0;
    arch->energy.read_energy_per_byte = arch->energy.bank_read_energy_pJ / bytes_per_access;
    /* Average of SET and RESET for write energy -- an EQUAL-WEIGHT mean over a
     * mix nothing in this tree measures, and it inherits the asserted 0.55
     * RESET/SET ratio set fourteen lines above.
     * 1.11.60 (audit round 4, C015): PCMModel no longer reads this field. It
     * was multiplying this mean by a SET counter, under a comment claiming the
     * SET basis, while its latency side charged the SET path -- so the one
     * number that dissented from "every write is a SET" was this one, and it
     * dissented silently by 0.775x. The model takes bank_set_energy_pJ
     * directly now. The field stays because it is part of the PCMArchitecture
     * description; anyone who picks it up should know it is a mean over an
     * unmeasured mix, not a characterization. */
    arch->energy.write_energy_per_byte = (arch->energy.bank_set_energy_pJ * 0.5 +
                                           arch->energy.bank_reset_energy_pJ * 0.5) / bytes_per_access;

    // Leakage
    /* 1.11.57 (latent D059): no second division -- getSenseAmpLeakage() is
     * NVSim's own per-subarray sense-amp leakage now, not 30% of the mat. */
    arch->energy.mat_leakage_mw = nvsim_wrapper.getSenseAmpLeakage();
    /* 1.11.56 (audit D058): ONE BASIS FOR NVSIM'S LEAKAGE, AND IT IS PER-BANK.
     *
     * This block used to read NVSim's figure as a WHOLE-CHIP number: chip =
     * getLeakagePower(), bank = getLeakagePower() / banks_per_chip. main.cpp's
     * device-power path reads the SAME call the other way round -- it asks
     * NVSim for one 64 KB bank and then multiplies by config.num_banks -- so
     * one run's stdout carried two leakage numbers for the same array that
     * differed by the bank count, with nothing saying which was which.
     *
     * The per-bank reading is the correct one, and NVSim says so itself:
     * getNumBanks() returns 1 with the comment "NVSim models a single bank",
     * and every live caller hands NVSim the PER-BANK capacity (main.cpp sets
     * setArrayCapacityBytes(PER_BANK_BYTES) = 64 KB before the plugin models
     * characterize anything). NVSim reports the leakage of the array it was
     * asked to model; that array is one bank. So the bank tier IS the tool's
     * figure, undivided, and the chip tier is banks_per_chip of them.
     *
     * Numerically this is inert while banks_per_chip comes from getNumBanks()
     * (it is 1, so chip == bank either way). It is fixed anyway because the
     * previous form was a division by a count that could only ever be right by
     * accident, and because the two sides of the report now state the same
     * basis. */
    arch->energy.bank_leakage_mw = nvsim_wrapper.getLeakagePower();
    arch->energy.chip_leakage_mw = nvsim_wrapper.getLeakagePower() * num_banks;

    // 1.11.57 (latent D059): say whether the subarray terms are tool reads.
    arch->energy.energy_source = pcm_has_components
        ? ("NVSim extraction, " + arch->process_node + " PCM")
        : ("NVSim extraction (bank/chip only; subarray components ABSENT -- "
           "cached characterization), " + arch->process_node + " PCM");

    // ===== ENDURANCE =====
    /* 1.11.57 (latent D063): NVSim models no endurance and no retention -- see
     * the STT-MRAM block. "NVSim + PCM literature" half-credited a tool that
     * supplied neither number and named no paper for the other half. */
    arch->endurance.write_cycles = 1e8;  // PCM limited endurance
    arch->endurance.retention_years = 10.0;
    arch->endurance.mlc_support = true;  // PCM can support MLC
    arch->endurance.endurance_source =
        "ASSERTED (unsourced literals): NVSim does not model endurance or "
        "retention. No citation is attached to 1e8 cycles / 10 years.";

    // ===== DATAPATH =====
    /* 1.11.57 (latent D057): NVSim reports no datapath width -- see the
     * STT-MRAM block. mat_io_bits is a literal, chip_io_bits is bank echoed,
     * bank_io_bits is the configured word width. None of it is a tool output,
     * and the block used to stamp itself VERIFIED / "NVSim extraction". */
    /* 1.11.73: mat width from NVSim mat.numDataBit (cached; 0 = unknown). */
    arch->datapath.mat_io_bits = static_cast<int>(nvsim_wrapper.getMatWidthBits());
    arch->datapath.bank_io_bits = config.word_width_bits > 0 ? config.word_width_bits : 64;
    arch->datapath.chip_io_bits = arch->datapath.bank_io_bits;

    arch->datapath.verification_status = VerificationStatus::ESTIMATED;
    arch->datapath.source =
        "mat_io_bits = NVSim mat.numDataBit (0 = unknown from an older cache); "
        "chip_io_bits = bank_io_bits ASSERTED; bank_io_bits is the CONFIGURED "
        "word width, not a tool output.";

    return arch;
}

/* 1.11.57 (latent D045-adjacent): updatePCMArchitectureFromNVSim deleted --
 * see the note above the PCM section. It was the surviving copy of
 * `reset = write * 0.3`. */

//=============================================================================
// ReRAM Architecture Extraction from NVSim
//=============================================================================

/**
 * @brief Extract ReRAM architecture from NVSim wrapper results
 *
 * ReRAM (Resistive RAM) characteristics:
 * - Fast writes (5-20ns, much better than PCM)
 * - Moderate read latency (4-7ns)
 * - Crossbar structure (ideal for analog computing!)
 * - Good endurance (10^10-10^12 cycles)
 */
inline std::unique_ptr<ReRAMArchitecture> extractReRAMArchitecture(
    const NVSimWrapper& nvsim_wrapper,
    const std::string& name = "ReRAM-Extracted",
    double clock_freq_ghz = 1.0,
    bool analog_capable = true) {

    if (!nvsim_wrapper.isValid()) {
        return nullptr;
    }

    const auto& config = nvsim_wrapper.getConfig();
    auto arch = std::make_unique<ReRAMArchitecture>(name, "ReRAM");

    arch->process_node = std::to_string(config.process_node_nm) + "nm";

    // ===== ORGANIZATION =====
    int num_banks = nvsim_wrapper.getNumBanks();
    if (num_banks == 0) num_banks = 16;  // ReRAM default
    arch->organization.banks_per_chip = num_banks;

    // Estimate grid organization
    int sqrt_banks = static_cast<int>(std::sqrt(num_banks));
    arch->organization.bank_rows = sqrt_banks > 0 ? sqrt_banks : 4;
    arch->organization.bank_cols = num_banks / arch->organization.bank_rows;

    size_t chip_mb = config.capacity_bytes / (1024 * 1024);
    arch->organization.chip_size_mb = chip_mb > 0 ? chip_mb : 2;
    arch->organization.bank_size_kb = (arch->organization.chip_size_mb * 1024) / num_banks;

    /* 1.11.73: the tier below the bank is NVSim's MAT (numRowMat x
     * numColumnMat per bank). Cached since 1.11.73; an older cache entry
     * yields 0 = UNKNOWN, and the run reports the tier unsourceable rather
     * than filling it (the old code multiplied two counts into a subarray
     * total and fell back to a literal 8). */
    const uint32_t mats_per_bank = nvsim_wrapper.getMatsPerBank();
    arch->organization.mats_per_bank = static_cast<int>(mats_per_bank);
    arch->organization.mat_size_kb = (mats_per_bank > 0)
        ? arch->organization.bank_size_kb / mats_per_bank : 0;

    // Crossbar dimensions
    arch->organization.crossbar_rows = nvsim_wrapper.getSubarrayRows();
    arch->organization.crossbar_cols = nvsim_wrapper.getSubarrayCols();

    if (arch->organization.crossbar_rows == 0) arch->organization.crossbar_rows = 256;
    if (arch->organization.crossbar_cols == 0) arch->organization.crossbar_cols = 256;

    // ===== TIMING (EXTRACTED from NVSim!) =====
    arch->timing.clock_freq_ghz = clock_freq_ghz;

    // Read latencies
    /* 1.11.57 (latent D061, same class): the 5.0 ns / 12.0 ns literals here
     * were the ReRAM copies of the PCM fallbacks -- unmarked substitutions on
     * a path whose doctrine is refuse-never-fill. Refuse. */
    double read_latency_ns = nvsim_wrapper.getReadLatency() * 1e9;
    if (read_latency_ns <= 0) {
        std::cerr << "[extractReRAMArchitecture] NVSim returned a non-positive "
                     "read latency (" << read_latency_ns << " ns). Refusing to "
                     "substitute the old 5.0 ns literal; re-characterize."
                  << std::endl;
        return nullptr;
    }
    /* 1.11.23: same tier correction (see the STT-MRAM block). */
    /* 1.11.25 CORRECTION: this was the sum of five wrapper accessors that look
     * like NVSim reads and, at the time, were not -- getDecoderDelay() and
     * friends returned INVENTED percentages of mat.readLatency (10/20/45/15/5,
     * summing to 0.95). Summing them replaced one invented multiplier with
     * another. (1.11.56, audit D031: those five now read NVSim's own component
     * latencies, so the objection no longer applies to them -- but the direct
     * subarray read below is still the right thing to use for a TIER, because
     * it is the figure NVSim composed rather than a sum we compose.)
     * NVSim resolves the subarray directly (SubArray derives from
     * FunctionUnit; Mat holds one), so read it. <0 means unavailable --
     * notably on a cache hit, since the pregenerated NVSim cache predates
     * these fields -- and the tier is then reported unsourceable, not
     * filled. */
    const double rer_sub_s = nvsim_wrapper.getMatLatency();  /* 1.11.73: the MAT, one tier below the bank */
    const double rer_sub_read_ns = (rer_sub_s > 0.0) ? rer_sub_s * 1e9 : -1.0;
    arch->timing.bank_read_ns     = read_latency_ns;
    arch->timing.mat_read_ns = rer_sub_read_ns;
    arch->timing.chip_read_ns     = read_latency_ns;

    // Write latencies (ReRAM has fast writes!)
    // 1.11.57 (latent D061, same class): refuse, do not fill.
    double write_latency_ns = nvsim_wrapper.getWriteLatency() * 1e9;
    if (write_latency_ns <= 0) {
        std::cerr << "[extractReRAMArchitecture] NVSim returned a non-positive "
                     "write latency (" << write_latency_ns << " ns). Refusing "
                     "to substitute the old 12.0 ns literal; re-characterize."
                  << std::endl;
        return nullptr;
    }
    arch->timing.bank_write_ns     = write_latency_ns;
    arch->timing.mat_write_ns =
        rer_sub_read_ns + nvsim_wrapper.getCellWriteLatency() * 1e9;
    arch->timing.chip_write_ns = write_latency_ns * 1.35;

    // Analog compute (very fast if supported!)
    if (analog_capable) {
        arch->timing.analog_compute_ns = 3.0;  // Crossbar matrix-vector multiply
    } else {
        arch->timing.analog_compute_ns = 0.0;
    }

    /* Inner-bank breakdown.
     * 1.11.57 (latent C011): only when there IS a breakdown -- see the
     * STT-MRAM block. Zero on the cached path. */
    const bool rer_has_components = nvsim_wrapper.hasComponentBreakdown();
    arch->timing.inner_bank.row_decoder_ns = nvsim_wrapper.getDecoderDelay() * 1e9;
    arch->timing.inner_bank.wordline_ns = nvsim_wrapper.getWordlineDelay() * 1e9;
    arch->timing.inner_bank.bitline_ns = nvsim_wrapper.getBitlineDelay() * 1e9;
    arch->timing.inner_bank.sense_amp_ns = nvsim_wrapper.getSenseAmpDelay() * 1e9;
    arch->timing.inner_bank.column_mux_ns = nvsim_wrapper.getColumnDecoderDelay() * 1e9;
    arch->timing.inner_bank.subarray_output_drv_ns = 0.22;
    arch->timing.inner_bank.local_io_ns = 0.28;
    arch->timing.inner_bank.htree_horizontal_ns = 0.42;
    arch->timing.inner_bank.htree_vertical_ns = 0.42;
    arch->timing.inner_bank.global_io_ns = 0.48;
    arch->timing.inner_bank.bank_output_drv_ns = 0.22;

    // ReRAM-specific write pulses (fast!)
    arch->timing.inner_bank.set_pulse_ns = write_latency_ns * 0.8;
    arch->timing.inner_bank.reset_pulse_ns = write_latency_ns * 0.65;

    // Analog compute timing
    if (analog_capable) {
        arch->timing.inner_bank.analog_multiply_ns = 2.0;  // Crossbar multiply
        arch->timing.inner_bank.analog_accumulate_ns = 1.0;  // Column accumulate
    } else {
        arch->timing.inner_bank.analog_multiply_ns = 0.0;
        arch->timing.inner_bank.analog_accumulate_ns = 0.0;
    }

    /* 1.11.23: see the STT-MRAM block -- the component delays are tool-read,
     * the local/global I/O and output-driver terms are not, so the block is
     * INFERRED. A VERIFIED stamp over asserted values is a false provenance
     * claim, which is the defect this release removes. */
    arch->timing.inner_bank.verification_status =
        rer_has_components ? VerificationStatus::INFERRED
                           : VerificationStatus::UNKNOWN;
    arch->timing.inner_bank.source = rer_has_components
        ? ("NVSim extraction, " + arch->process_node + " ReRAM")
        : ("NOT SOURCED: cached characterization -- the per-component delays "
           "are ZERO and the inner-bank total is the ASSERTED H-tree/IO terms "
           "alone. Re-characterize to obtain them. " + arch->process_node
           + " ReRAM");

    // ===== ENERGY (EXTRACTED from NVSim!) =====
    double read_energy_nJ = nvsim_wrapper.getReadDynamicEnergy();
    double write_energy_nJ = nvsim_wrapper.getWriteDynamicEnergy();

    /* 1.11.57 (latent D059): NVSim's own per-subarray component energies, not
     * percentages of a mat scalar; zero when the run came from the cache. */
    arch->energy.mat_read_energy_pJ = (nvsim_wrapper.getDecoderEnergy() +
                                             nvsim_wrapper.getWordlineEnergy() +
                                             nvsim_wrapper.getBitlineEnergy() +
                                             nvsim_wrapper.getSenseAmpEnergy()) * 1000.0;
    arch->energy.bank_read_energy_pJ = read_energy_nJ * 1000.0;
    arch->energy.chip_read_energy_pJ = arch->energy.bank_read_energy_pJ * 1.95;

    // Write energy (moderate for ReRAM)
    arch->energy.mat_write_energy_pJ = arch->energy.mat_read_energy_pJ * 8.0;
    arch->energy.bank_write_energy_pJ = write_energy_nJ * 1000.0;
    arch->energy.chip_write_energy_pJ = arch->energy.bank_write_energy_pJ * 1.65;

    // Analog compute energy (very low!)
    if (analog_capable) {
        arch->energy.analog_compute_energy_pJ = 0.5;  // Very efficient
    } else {
        arch->energy.analog_compute_energy_pJ = 0.0;
    }

    // Per-byte energy
    double bytes_per_access = config.word_width_bits / 8.0;
    if (bytes_per_access <= 0) bytes_per_access = 8.0;
    arch->energy.read_energy_per_byte = arch->energy.bank_read_energy_pJ / bytes_per_access;
    arch->energy.write_energy_per_byte = arch->energy.bank_write_energy_pJ / bytes_per_access;

    // Leakage
    /* 1.11.57 (latent D059): no second division -- getSenseAmpLeakage() is
     * NVSim's own per-subarray sense-amp leakage now, not 30% of the mat. */
    arch->energy.mat_leakage_mw = nvsim_wrapper.getSenseAmpLeakage();
    /* 1.11.56 (audit D058): ONE BASIS FOR NVSIM'S LEAKAGE, AND IT IS PER-BANK.
     *
     * This block used to read NVSim's figure as a WHOLE-CHIP number: chip =
     * getLeakagePower(), bank = getLeakagePower() / banks_per_chip. main.cpp's
     * device-power path reads the SAME call the other way round -- it asks
     * NVSim for one 64 KB bank and then multiplies by config.num_banks -- so
     * one run's stdout carried two leakage numbers for the same array that
     * differed by the bank count, with nothing saying which was which.
     *
     * The per-bank reading is the correct one, and NVSim says so itself:
     * getNumBanks() returns 1 with the comment "NVSim models a single bank",
     * and every live caller hands NVSim the PER-BANK capacity (main.cpp sets
     * setArrayCapacityBytes(PER_BANK_BYTES) = 64 KB before the plugin models
     * characterize anything). NVSim reports the leakage of the array it was
     * asked to model; that array is one bank. So the bank tier IS the tool's
     * figure, undivided, and the chip tier is banks_per_chip of them.
     *
     * Numerically this is inert while banks_per_chip comes from getNumBanks()
     * (it is 1, so chip == bank either way). It is fixed anyway because the
     * previous form was a division by a count that could only ever be right by
     * accident, and because the two sides of the report now state the same
     * basis. */
    arch->energy.bank_leakage_mw = nvsim_wrapper.getLeakagePower();
    arch->energy.chip_leakage_mw = nvsim_wrapper.getLeakagePower() * num_banks;

    // 1.11.57 (latent D059): say whether the subarray terms are tool reads.
    arch->energy.energy_source = rer_has_components
        ? ("NVSim extraction, " + arch->process_node + " ReRAM")
        : ("NVSim extraction (bank/chip only; subarray components ABSENT -- "
           "cached characterization), " + arch->process_node + " ReRAM");

    // ===== ENDURANCE =====
    /* 1.11.57 (latent D063): NVSim models no endurance and no retention -- see
     * the STT-MRAM block. */
    arch->endurance.write_cycles = 1e11;  // Good for ReRAM
    arch->endurance.retention_years = 10.0;
    arch->endurance.mlc_support = analog_capable;
    arch->endurance.analog_capable = analog_capable;
    arch->endurance.endurance_source =
        "ASSERTED (unsourced literals): NVSim does not model endurance or "
        "retention. No citation is attached to 1e11 cycles / 10 years.";

    // ===== DATAPATH =====
    /* 1.11.57 (latent D057): NVSim reports no datapath width -- see the
     * STT-MRAM block. All four of these are asserted or echoed inputs, and the
     * block used to stamp itself VERIFIED / "NVSim extraction". */
    /* 1.11.73: mat width from NVSim mat.numDataBit (cached; 0 = unknown);
     * the 64/128-by-analog literal is gone. */
    arch->datapath.mat_io_bits = static_cast<int>(nvsim_wrapper.getMatWidthBits());
    arch->datapath.bank_io_bits = config.word_width_bits > 0 ? config.word_width_bits : 64;
    arch->datapath.chip_io_bits = arch->datapath.bank_io_bits * 2;
    arch->datapath.crossbar_analog_bits = analog_capable ? 8 : 0;  // 8-bit analog resolution

    arch->datapath.verification_status = VerificationStatus::ESTIMATED;
    arch->datapath.source =
        "mat_io_bits = NVSim mat.numDataBit (0 = unknown from an older cache); "
        "chip_io_bits = 2 x bank and crossbar_analog_bits = 8 ASSERTED; "
        "bank_io_bits is the CONFIGURED word width, not a tool output.";

    return arch;
}

/* 1.11.57 (latent D045-adjacent): updateReRAMArchitectureFromNVSim deleted --
 * see the note above the PCM section. */

//=============================================================================
// DRAM Architecture Extraction from Ramulator
//=============================================================================

/**
 * @brief Extract DRAM architecture from Ramulator wrapper
 *
 * This function creates a DRAMArchitectureV2 populated with values
 * extracted from Ramulator2 simulation results, replacing hard-coded defaults.
 *
 * @param ramulator_wrapper Initialized RamulatorWrapper
 * @param name Architecture name (e.g., "DDR4-Ramulator-Extracted")
 * @return Populated DRAMArchitectureV2, or nullptr if extraction fails
 */
inline std::unique_ptr<DRAMArchitectureV2> extractDRAMArchitecture(
    const RamulatorWrapper& ramulator_wrapper,
    const std::string& name = "DRAM-Extracted") {

    // If Ramulator already has an architecture, return a copy with updated values
    const DRAMArchitectureV2* existing_arch = ramulator_wrapper.getDRAMArchitecture();

    auto arch = std::make_unique<DRAMArchitectureV2>(name, "DRAM");
    arch->version = "2.0-extracted";

    // ===== ORGANIZATION (from Ramulator) =====
    arch->organization.subarrays_per_bank = ramulator_wrapper.getSubarraysPerBank();
    arch->organization.banks_per_bank_group = ramulator_wrapper.getBanksPerBankGroup();
    arch->organization.bank_groups_per_chip = ramulator_wrapper.getBankGroupsPerChip();
    arch->organization.chips_per_rank = ramulator_wrapper.getChipsPerRank();
    arch->organization.ranks_per_channel = ramulator_wrapper.getRanksPerChannel();

    // Capacities
    arch->organization.subarray_size_kb = ramulator_wrapper.getSubarraySizeKB();
    arch->organization.bank_size_mb = ramulator_wrapper.getBankSizeMB();
    arch->organization.chip_size_mb = ramulator_wrapper.getChipSizeMB();

    // Calculate rank size
    uint64_t chips = arch->organization.chips_per_rank;
    arch->organization.rank_size_gb = (arch->organization.chip_size_mb * chips) / 1024;

    // ===== DATAPATH STAGES (from Ramulator) =====

    // Row buffer (estimate from subarray size)
    arch->datapath.row_buffer_bits = {
        8192,  // Typical row buffer width
        VerificationStatus::INFERRED,
        "Estimated from Ramulator configuration",
        "Row buffer in bitline sense amplifiers"
    };

    // Global sense amplifiers
    arch->datapath.gsa_datapath_bits = {
        256,  // Typical GSA width
        VerificationStatus::INFERRED,
        "Estimated from DRAM architecture papers",
        "Column I/O width"
    };

    /* Prefetch datapath.
     *
     * 1.11.59 (C018-class): the provenance string was false. This value is
     * getSubarrayPortBits(), which returns the architecture object's GSA
     * datapath width -- the global sense-amplifier bus -- not a prefetch
     * configuration, and nothing on this path consults a Ramulator prefetch
     * setting at all. Same category as the chip_io_bits and rank_databus_bits
     * mis-stamps corrected beside it: a value presented as extracted from a
     * tool that was never asked. The VALUE is unchanged; only the claim is. */
    int prefetch_bits = ramulator_wrapper.getSubarrayPortBits();
    bool prefetch_sourced = (prefetch_bits > 0);
    if (!prefetch_sourced) prefetch_bits = 64;
    arch->datapath.prefetch_datapath_bits = {
        prefetch_bits,
        VerificationStatus::INFERRED,
        prefetch_sourced
            ? "The architecture object's GSA datapath width (getSubarrayPortBits); "
              "NOT a Ramulator prefetch setting -- no prefetch configuration is "
              "consulted on this path"
            : "ASSERTED 64 bits: the architecture object reported no GSA datapath "
              "width and no prefetch configuration is consulted here",
        "Prefetch buffer width (GSA-derived)"
    };

    // Bank serialization (critical for PIM!)
    int bank_bits = ramulator_wrapper.getBankPortBits();
    if (bank_bits <= 0) bank_bits = 8;  // Conservative default
    arch->datapath.bank_serialization_bits = {
        bank_bits,
        VerificationStatus::INFERRED,
        "Extracted/estimated from Ramulator bank configuration",
        "Bank-to-peripheral serialization - CRITICAL for PIM bandwidth!"
    };

    /* 1.11.59 (audit C018): SAY WHERE THESE TWO ACTUALLY COME FROM.
     *
     * Both are read off the wrapper's DRAM architecture object. Neither is
     * "extracted from Ramulator": no Ramulator device object is consulted
     * anywhere on this path. The chip DQ width stamp used to read "Extracted
     * from Ramulator device configuration (x4/x8/x16)" while the value ignored
     * the configured device width entirely -- a claim of x4/x8/x16 sourcing on
     * a fixed 8. The width IS honoured now (RamulatorWrapper::setDeviceWidth ->
     * applyDeviceWidthToArchitecture), so the parenthetical has become true;
     * the "extracted from Ramulator" half has not, and is corrected here.
     *
     * The rank stamp said "Calculated from chips_per_rank x chip_io_bits",
     * which was false for HBM by two orders of magnitude: HBM2 holds 8 x 1024
     * in those two fields against the 128 this value carries, because an HBM
     * "rank" is one 128-bit channel and not a set of devices on a shared bus.
     * The relation does hold for DDR-class parts, and holds at every device
     * width since C018 -- so it is stated as what it is, family-dependent. */
    // Chip I/O
    int chip_io = ramulator_wrapper.getChipIOBits();
    if (chip_io <= 0) chip_io = 8;
    arch->datapath.chip_io_bits = {
        chip_io,
        VerificationStatus::VERIFIED,
        "DRAM architecture object's chip DQ width, set by the configured JEDEC "
        "device width (memory.dram.device_width x4/x8/x16) where one is given, "
        "otherwise the object's factory organization",
        "External package DQ pins of one device (HBM: the stack interface)"
    };

    // Rank databus
    int rank_bits = ramulator_wrapper.getRankDataBits();
    if (rank_bits <= 0) rank_bits = 64;
    arch->datapath.rank_databus_bits = {
        rank_bits,
        VerificationStatus::VERIFIED,
        /* 1.11.60 (audit round 4, C011): "one 128-bit channel for HBM" was
         * right for HBM2 and wrong for HBM3, whose rank_databus_bits is 64 --
         * one 64-bit pseudo-channel, per JESD238 and per the object at
         * dram_architecture_v2.h. The 1.11.59 note directly above corrected
         * this string precisely because the old one "was false for HBM by two
         * orders of magnitude", and replaced it with a family statement that
         * is still a single constant across a family whose two members differ.
         * It reads as a per-technology fact now, which is what it is. */
        "DRAM architecture object's rank data bus: the JEDEC 64-bit rank for "
        "DDR-class parts (= chips_per_rank x chip_io_bits at every device "
        "width), and one channel's data bus for HBM, where a rank is not built "
        "from devices on a shared bus -- 128 bits on HBM2 (JESD235C, 128-bit "
        "channels) and 64 on HBM3 (JESD238, 64-bit pseudo-channels)",
        "First wide interface in DDR hierarchy"
    };

    // Channel databus
    int channel_bits = ramulator_wrapper.getChannelDataBits();
    if (channel_bits <= 0) channel_bits = 64;
    arch->datapath.channel_databus_bits = {
        channel_bits,
        VerificationStatus::VERIFIED,
        "Standard memory controller interface",
        "Channel-level data width"
    };

    // ===== TIMING (from Ramulator) =====

    // Extract from existing architecture if available, otherwise from wrapper
    if (existing_arch) {
        arch->timing.clock_freq_mhz = existing_arch->timing.clock_freq_mhz;
        arch->timing.data_rate_mtps = existing_arch->timing.data_rate_mtps;
    } else {
        arch->timing.clock_freq_mhz = 1200.0;  // Default DDR4-2400
        arch->timing.data_rate_mtps = 2400.0;
    }

    // JEDEC timing parameters from Ramulator
    arch->timing.tRCD_ns = ramulator_wrapper.getTRCD();
    arch->timing.tCAS_ns = ramulator_wrapper.getTCAS();
    arch->timing.tRP_ns = ramulator_wrapper.getTRP();
    arch->timing.tRAS_ns = ramulator_wrapper.getTRAS();
    arch->timing.tBurst_ns = ramulator_wrapper.getTBurst();

    // Inner-bank datapath timing (from existing arch or estimated)
    if (existing_arch) {
        arch->timing.inner_bank = existing_arch->timing.inner_bank;
        /* 1.11.23: see the STT-MRAM block -- the component delays are tool-read,
     * the local/global I/O and output-driver terms are not, so the block is
     * INFERRED. A VERIFIED stamp over asserted values is a false provenance
     * claim, which is the defect this release removes. */
    arch->timing.inner_bank.verification_status = VerificationStatus::INFERRED;
        arch->timing.inner_bank.source = "Ramulator + DRAMArchitectureV2 extraction";
    } else {
        // Estimate based on typical DDR4 values
        arch->timing.inner_bank.column_decoder_ns = 0.35;
        arch->timing.inner_bank.column_mux_ns = 0.55;
        arch->timing.inner_bank.subarray_output_drv_ns = 0.50;
        arch->timing.inner_bank.local_io_ns = 0.75;
        arch->timing.inner_bank.htree_horizontal_ns = 1.20;
        arch->timing.inner_bank.htree_vertical_ns = 1.20;
        arch->timing.inner_bank.global_io_ns = 1.50;
        arch->timing.inner_bank.bank_io_driver_ns = 0.60;
        arch->timing.inner_bank.verification_status = VerificationStatus::ESTIMATED;
        arch->timing.inner_bank.source = "Estimated from CACTI and Ramulator timing";
    }

    // Hierarchical access latencies
    arch->timing.subarray_access_ns = ramulator_wrapper.getSubarrayAccessLatency();
    arch->timing.bank_access_ns = ramulator_wrapper.getBankAccessLatency();
    arch->timing.chip_access_ns = ramulator_wrapper.getChipAccessLatency();
    arch->timing.rank_access_ns = ramulator_wrapper.getRankAccessLatency();

    /* ===== BANDWIDTH LIMITS =====
     * 1.11.57 (audit C003): nothing to copy. The bank and bank-group figures
     * are no longer stored fields to be filled in from somewhere else; they
     * are derived on read from the datapath width and the core clock this
     * function has just written above, which is the only way the width and
     * the bandwidth of a rung cannot end up describing different parts. */
    arch->bandwidth_limits.inference_method =
        "DERIVED on read from the extracted datapath widths and clock";
    arch->bandwidth_limits.confidence_level = "High - based on Ramulator model";

    // ===== ENERGY (from Ramulator) =====
    arch->energy.subarray_energy_pJ = ramulator_wrapper.getSubarrayEnergyPerByte();
    arch->energy.bank_energy_pJ = ramulator_wrapper.getBankEnergyPerByte();
    arch->energy.chip_energy_pJ = ramulator_wrapper.getChipEnergyPerByte();
    arch->energy.rank_energy_pJ = ramulator_wrapper.getRankEnergyPerByte();
    arch->energy.energy_source = "Ramulator energy model extraction";

    // ===== PE BUS CONSTRAINTS (derived from extracted values) =====

    // Subarray-level
    arch->pe_bus_constraints.subarray_level.data_bus_width_bits = 8192 * 8;  // Row buffer
    arch->pe_bus_constraints.subarray_level.max_bandwidth_gbps =
        ramulator_wrapper.getSubarrayBandwidth();
    arch->pe_bus_constraints.subarray_level.row_buffer_size_bytes = 8192;
    arch->pe_bus_constraints.subarray_level.has_dedicated_bus = true;

    // Bank-level
    arch->pe_bus_constraints.bank_level.data_bus_width_bits = bank_bits;
    arch->pe_bus_constraints.bank_level.max_bandwidth_gbps =
        ramulator_wrapper.getBankBandwidth();
    arch->pe_bus_constraints.bank_level.has_dedicated_bus = false;

    // Chip-level
    arch->pe_bus_constraints.chip_level.data_bus_width_bits = chip_io;
    arch->pe_bus_constraints.chip_level.max_bandwidth_gbps =
        ramulator_wrapper.getChipIOBandwidth();
    arch->pe_bus_constraints.chip_level.has_dedicated_bus = false;

    // Rank-level
    arch->pe_bus_constraints.rank_level.data_bus_width_bits = rank_bits;
    arch->pe_bus_constraints.rank_level.max_bandwidth_gbps =
        ramulator_wrapper.getRankBandwidth();
    arch->pe_bus_constraints.rank_level.has_dedicated_bus = false;

    // Logic die (for HBM)
    arch->pe_bus_constraints.logic_die_level.data_bus_width_bits = 1024;
    arch->pe_bus_constraints.logic_die_level.max_bandwidth_gbps =
        ramulator_wrapper.getChannelBandwidth();
    arch->pe_bus_constraints.logic_die_level.has_dedicated_bus = true;

    return arch;
}

/**
 * @brief Update an existing DRAM architecture with Ramulator-extracted values
 */
inline void updateDRAMArchitectureFromRamulator(
    DRAMArchitectureV2& arch,
    const RamulatorWrapper& ramulator_wrapper) {

    // Update timing
    arch.timing.tRCD_ns = ramulator_wrapper.getTRCD();
    arch.timing.tCAS_ns = ramulator_wrapper.getTCAS();
    arch.timing.tRP_ns = ramulator_wrapper.getTRP();
    arch.timing.tRAS_ns = ramulator_wrapper.getTRAS();
    arch.timing.tBurst_ns = ramulator_wrapper.getTBurst();

    // Update access latencies
    arch.timing.subarray_access_ns = ramulator_wrapper.getSubarrayAccessLatency();
    arch.timing.bank_access_ns = ramulator_wrapper.getBankAccessLatency();

    /* 1.11.57 (audit C003): the bank and bank-group bandwidths are derived on
     * read from this object's own width and clock, so there is nothing here to
     * update -- and no way for an update to leave them describing a different
     * part from the datapath beside them. */

    // Update energy
    arch.energy.subarray_energy_pJ = ramulator_wrapper.getSubarrayEnergyPerByte();
    arch.energy.bank_energy_pJ = ramulator_wrapper.getBankEnergyPerByte();
    arch.energy.chip_energy_pJ = ramulator_wrapper.getChipEnergyPerByte();

    // Update verification status
    /* 1.11.23: see the STT-MRAM block -- the component delays are tool-read,
     * the local/global I/O and output-driver terms are not, so the block is
     * INFERRED. A VERIFIED stamp over asserted values is a false provenance
     * claim, which is the defect this release removes. */
    arch.timing.inner_bank.verification_status = VerificationStatus::INFERRED;
    arch.timing.inner_bank.source = "Ramulator extraction (updated)";
}

} // namespace memory
} // namespace pimid

#endif // PIMID_ARCHITECTURE_EXTRACTOR_H
