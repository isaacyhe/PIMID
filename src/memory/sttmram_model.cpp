#include "memory/sttmram_model.h"
#include "memory/nvsim_wrapper.h"
#include "memory/sttmram_architecture.h"
#include "memory/architecture_extractor.h"
#include "config/config_parser.h"
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <algorithm>

static inline double bytesPerAccess(uint32_t access_width_bits) {
    // 64 bits is what initializeNVSim() passes NVSim when the knob is unset.
    double bytes = (access_width_bits > 0 ? access_width_bits : 64u) / 8.0;
    return (bytes > 0.0) ? bytes : 8.0;
}

namespace pimid {

//=============================================================================
// STTMRAMModel Implementation
//=============================================================================

STTMRAMModel::STTMRAMModel(const std::string& config_path)
    : MemoryModel(MemoryTechnology::STT_MRAM, config_path)
    , nvsim_wrapper_(nullptr)
    , mram_arch_(nullptr)
    , total_reads_(0)
    , total_writes_(0)
    , write_cycles_(0)
    , read_energy_(0.0)
    , write_energy_(0.0)
    , leakage_power_(0.0)
    , area_mm2_(0.0)
    , current_cycle_(0)
    , capacity_(0)
    , bandwidth_(0)
    , endurance_(0) {

    // Initialize default STT-MRAM configuration
    mram_config_.capacity = 256ULL * 1024 * 1024;  // 256MB
    mram_config_.banks = 8;
    mram_config_.read_write_ports = 1;
    mram_config_.tech_node_nm = 22;
    /* 1.11.56 (audit D054): nanoseconds, not cycles. Placeholders that the
     * NVSim path overwrites on every reachable run; the model refuses when
     * that binding fails, so they never reach a result. */
    mram_config_.read_latency_ns = 5.0;    // ns (fast read)
    mram_config_.write_latency_ns = 20.0;  // ns (slow MTJ switching)
    mram_config_.endurance = 1e15;    // 10^15 writes (very high)
    mram_config_.is_pim_enabled = true;
}

void STTMRAMModel::initialize() {
    std::cout << "[STTMRAMModel] Initializing STT-MRAM model..." << std::endl;
    loadConfig(config_path_);

    // Calculate derived parameters
    capacity_ = mram_config_.capacity;
    /* 1.11.57 (latent D053): NO FABRICATED BANDWIDTH. This was
     * `bandwidth_ = capacity_ / 100;` -- a bytes/s figure derived from a capacity by an
     * unsourced ratio, answering the same contract method DRAMModel answers
     * from Ramulator. Absent is 0, and getBandwidth() announces it. */
    bandwidth_ = 0;
    endurance_ = mram_config_.endurance;

    // Initialize NVSim FIRST (before architecture creation)
    // This allows us to extract architecture from NVSim results
    initializeNVSim();

    // Initialize STT-MRAM architecture with inner-bank timing
    // PRIORITY: Extract from NVSim if available, otherwise use factory defaults
#ifdef HAVE_NVSIM
    if (nvsim_wrapper_ && nvsim_wrapper_->isValid()) {
        // Extract architecture directly from NVSim results (PREFERRED!)
        mram_arch_ = memory::extractSTTMRAMArchitecture(*nvsim_wrapper_,
            "STTMRAM-NVSim-Extracted", 2.0);

        if (mram_arch_) {
            std::cout << "[STTMRAMModel] Architecture EXTRACTED from NVSim" << std::endl;
            /* 1.11.23: surface the tier ladder with its provenance. Nothing in
             * the log used to say where a latency came from, so a ladder built
             * from invented multipliers was indistinguishable from a tool-read
             * one -- which is how reset = write * 0.3 survived. The residual
             * between mat and bank IS the intra-bank H-tree (NVSim runs
             * with routingMode = h_tree). */
            {
                const auto& t = mram_arch_->timing;
                std::cout << "  [tier] mat      " << t.mat_read_ns
                          << " ns (NVSim components) | bank " << t.bank_read_ns
                          << " ns (NVSim bank->readLatency) | H-tree residual "
                          << (t.bank_read_ns - t.mat_read_ns) << " ns"
                          << std::endl;
            }
        } else {
            std::cerr << "[STTMRAMModel] NVSim extraction failed, using factory defaults" << std::endl;
        }
    }
#endif

    /* 1.11.24: the hand-written factory defaults are GONE. They were a
     * fallback for "tool extraction failed", and that fallback is exactly
     * what let fabricated numbers reach a result while looking tool-sourced:
     * 678 literal assignments across 19 create*() factories, none of them
     * derivable from anything. A technology whose tool binding fails must
     * REFUSE, not quietly report invented specs.
     *
     * This is the vendorArrayFraction() discipline applied to a whole
     * model: absence is reported, never filled. */
    if (!mram_arch_) {
        throw std::runtime_error(
            "[STTMRAMModel] NVSim characterization failed and there is no fallback. "
            "The hand-written default specs were removed in 1.11.24 because "
            "they were unsourced and indistinguishable from tool output. "
            "Fix the NVSim configuration rather than pricing this run from "
            "invented numbers.");
    }

    std::cout << "[STTMRAMModel] Inner-bank read latency: "
              << mram_arch_->timing.inner_bank.getTotalReadLatency() << " ns" << std::endl;
    std::cout << "[STTMRAMModel] Inner-bank write latency: "
              << mram_arch_->timing.inner_bank.getTotalWriteLatency() << " ns" << std::endl;
    std::cout << "[STTMRAMModel] Architecture source: "
              << mram_arch_->timing.inner_bank.source << std::endl;

    // Use architecture energy values as defaults (may have been extracted from NVSim)
    /* 1.11.58 (audit D022/D043, the gated unit change memory_model.h asked
     * for): this member is NANOJOULES PER ACCESS, the contract's unit.
     *
     * It used to hold two different quantities depending on which path ran.
     * The architecture path assigned energy.read_energy_per_byte, which is
     * PICOJOULES PER BYTE; the NVSim path assigned getReadDynamicEnergy(),
     * which is NANOJOULES PER ACCESS. One field, two units, differing by
     * 1000/bytes-per-access -- 125x at the 64-bit default. Whichever path
     * happened to run decided what the number meant, and nothing downstream
     * could tell. Normalised at assignment so the field has ONE unit, and
     * getTotalEnergy() no longer has to guess. */
    read_energy_ = mram_arch_->energy.read_energy_per_byte
                   * bytesPerAccess(access_width_bits_) / 1000.0;   // pJ/B -> nJ/access
    write_energy_ = mram_arch_->energy.write_energy_per_byte
                    * bytesPerAccess(access_width_bits_) / 1000.0;
    leakage_power_ = mram_arch_->energy.chip_leakage_mw / 1000.0;  // mW to W

    std::cout << "[STTMRAMModel] Configuration:" << std::endl;
    std::cout << "  Capacity: " << (mram_config_.capacity / (1024.0 * 1024)) << " MB" << std::endl;
    std::cout << "  Banks: " << mram_config_.banks << std::endl;
    std::cout << "  Technology: " << mram_config_.tech_node_nm << " nm" << std::endl;
    /* 1.11.56 (audit D054): printed as NANOSECONDS, which is what NVSim
     * reported and what these fields hold. They were labelled "cycles" while
     * carrying a 1 GHz product, so the number was right only at 1 GHz. */
    std::cout << "  Read Latency: " << mram_config_.read_latency_ns << " ns" << std::endl;
    std::cout << "  Write Latency: " << mram_config_.write_latency_ns << " ns" << std::endl;
    std::cout << "  Endurance: " << mram_config_.endurance << " writes" << std::endl;
    std::cout << "  PIM Enabled: " << (mram_config_.is_pim_enabled ? "Yes" : "No") << std::endl;
    /* 1.11.60 (audit round 4, C001): nJ PER ACCESS, which is what the two
     * assignments seventeen lines above this actually produce. 1.11.58 re-based
     * the members from pJ/byte to nJ/access and did not move the labels here,
     * so the printed number was 15.6x below the pJ/byte it claimed at the
     * 512-bit line main.cpp configures (125x at the 64-bit model default). It
     * was invisible because the value itself was already correct -- only its
     * name was left on the retired basis -- and because the block comment
     * further down cited THIS print as the evidence for the old unit, so a
     * reader who checked the source was told the same wrong thing twice. */
    std::cout << "  Read Energy: " << read_energy_ << " nJ/access" << std::endl;
    std::cout << "  Write Energy: " << write_energy_ << " nJ/access" << std::endl;
    std::cout << "  Leakage Power: " << leakage_power_ << " W" << std::endl;
    std::cout << "[STTMRAMModel] Initialization complete" << std::endl;
}

void STTMRAMModel::loadConfig(const std::string& config_path) {
    std::cout << "[STTMRAMModel] Loading configuration from: " << config_path << std::endl;

    config::ConfigParser parser;
    std::map<std::string, std::string> config;

    if (!parser.parseFile(config_path, config)) {
        std::cerr << "[STTMRAMModel] Warning: Failed to parse config, using defaults" << std::endl;
        return;
    }

    // Extract STT-MRAM capacity
    if (config.find("stt_mram.capacity_mb") != config.end()) {
        try {
            uint64_t capacity_mb = std::stoull(config["stt_mram.capacity_mb"]);
            mram_config_.capacity = capacity_mb * 1024 * 1024;
        } catch (...) {}
    }

    // Extract organization
    if (config.find("stt_mram.organization.num_arrays") != config.end()) {
        try {
            mram_config_.banks = std::stoi(config["stt_mram.organization.num_arrays"]);
        } catch (...) {}
    }

    // Extract timing
    if (config.find("stt_mram.timing.read_latency_ns") != config.end()) {
        try {
            double read_ns = std::stod(config["stt_mram.timing.read_latency_ns"]);
            mram_config_.read_latency_ns = read_ns;   // 1.11.56 (D054): ns in, ns kept
        } catch (...) {}
    }

    if (config.find("stt_mram.timing.write_latency_ns") != config.end()) {
        try {
            double write_ns = std::stod(config["stt_mram.timing.write_latency_ns"]);
            mram_config_.write_latency_ns = write_ns;  // 1.11.56 (D054): ns in, ns kept
        } catch (...) {}
    }

    // Extract reliability
    if (config.find("stt_mram.reliability.write_endurance") != config.end()) {
        try {
            mram_config_.endurance = std::stoull(config["stt_mram.reliability.write_endurance"]);
        } catch (...) {}
    }

    // Extract technology
    if (config.find("stt_mram.power.tech_node_nm") != config.end()) {
        try {
            mram_config_.tech_node_nm = std::stoi(config["stt_mram.power.tech_node_nm"]);
        } catch (...) {}
    }

    std::cout << "[STTMRAMModel] Configuration loaded successfully" << std::endl;
}

/* 1.11.56 (audit D054): THE ONE PLACE TIME BECOMES CYCLES IN THIS MODEL.
 *
 * MemoryModel's legacy access()/getLatency() return Cycle, and this model is
 * handed no clock -- the simulator drives it through getTierLatencyNs(), which
 * is nanoseconds end to end and never comes through here. So the conversion
 * below is 1 cycle per nanosecond, i.e. exactly the 1 GHz the old code assumed;
 * the difference is that it is stated once, at the boundary that forces it,
 * instead of being baked into the stored fields and mislabelled in the log. If
 * this path ever becomes live, give the model a frequency and convert with
 * that -- do not restore the assumption upstream. */
static inline Cycle legacyNsAsCycles(double ns) {
    if (ns <= 0.0) return 0;
    return static_cast<Cycle>(std::ceil(ns));   // 1 GHz: no clock is supplied
}

/* 1.11.60 (audit round 4, C001): READ THE PARAGRAPH BELOW AS HISTORY, NOT AS
 * A STATEMENT OF THE PRESENT UNIT. It was written at 1.11.57 and it was true
 * then; 1.11.58 re-based read_energy_/write_energy_ to NANOJOULES PER ACCESS
 * at assignment (see the note beside the assignments in initialize()), and
 * this paragraph was not moved with them. Worse, it cited initialize()'s
 * "pJ/byte" printout as its evidence -- and that print had not been moved
 * either, so the source and the log agreed with each other and both disagreed
 * with the arithmetic between them. Both are corrected at 1.11.60. What the
 * members hold today: nJ per access, one unit on every path.
 */
/* 1.11.57 (latent D044): THE MISSING ACCESS SIZE.
 *
 * read_energy_/write_energy_ HELD pJ PER BYTE when this was written -- the
 * model's own printout said so ("pJ/byte", initialize()), and the value came
 * from
 * STTMRAMArchitecture::energy.read_energy_per_byte, which the extractor
 * computes as bank_read_energy_pJ / (word_width_bits / 8)
 * (architecture_extractor.h). getTotalEnergy() and printStats() multiplied
 * that energy DENSITY by an ACCESS COUNT and printed the product as "pJ",
 * which understates the dynamic energy by exactly the access size in bytes: 8x
 * at this model's 64-bit default, 64x at the 512-bit width main.cpp asks the
 * SRAM path for.
 *
 * The consumers want per-access energy, so the arithmetic multiplies the
 * density back by the same bytes-per-access the extractor divided by -- which
 * recovers bank_read_energy_pJ exactly, rather than approximating it. The
 * width has to be read from access_width_bits_ for that identity to hold,
 * because initializeNVSim() is what feeds it to NVSim in the first place; the
 * "0 = model default" case is the 64 bits initializeNVSim() substitutes.
 *
 * WHY IT WAS INVISIBLE: getTotalEnergy() and printStats() have no reachable
 * caller on these models, and the counters they multiply are never incremented
 * because access() is never called either -- the product was 0 * wrong.
 *
 * NOT FIXED HERE, and deliberately: the leakage term below adds JOULES
 * (W x s) to a picojoule sum, and getTotalEnergy() returns picojoules where
 * MemoryModel documents nanojoules. Those are audit D022 and D043, they span
 * all five plugin models, and memory_model.h says in as many words that the
 * unit conversion must be one gated change rather than a rider on a
 * latent-defect pass. */

Cycle STTMRAMModel::access(const MemoryRequest& req) {
    Cycle latency = legacyNsAsCycles(mram_config_.read_latency_ns);  // safe default

    // STT-MRAM has asymmetric read/write latency
    switch (req.type) {
        case MemoryRequestType::READ:
            latency = legacyNsAsCycles(mram_config_.read_latency_ns);
            total_reads_++;
            break;
        case MemoryRequestType::WRITE:
            latency = legacyNsAsCycles(mram_config_.write_latency_ns);  // MTJ switching dominates!
            total_writes_++;
            write_cycles_++;
            updateEndurance(req.addr);
            if (write_cycles_ > endurance_) {
                std::cerr << "[STTMRAMModel] WARNING: Endurance limit exceeded!" << std::endl;
            }
            break;
        case MemoryRequestType::ATOMIC:
            latency = legacyNsAsCycles(mram_config_.read_latency_ns +
                                       mram_config_.write_latency_ns);
            total_reads_++;
            total_writes_++;
            write_cycles_++;
            break;
    }

    // Add queuing delay
    if (!pending_requests_.empty()) {
        latency += pending_requests_.size();
    }

    pending_requests_.push(req);

    return latency;
}

bool STTMRAMModel::canAccept(const MemoryRequest& req) {
    const size_t MAX_PENDING_REQUESTS = 32;
    if (pending_requests_.size() >= MAX_PENDING_REQUESTS) {
        return false;
    }

    // Check endurance for writes
    if ((req.type == MemoryRequestType::WRITE ||
         req.type == MemoryRequestType::ATOMIC) &&
        write_cycles_ >= endurance_) {
        return false;
    }

    return true;
}

void STTMRAMModel::tick() {
    current_cycle_++;

    // Process pending requests
    if (!pending_requests_.empty()) {
        pending_requests_.pop();
    }
}

Cycle STTMRAMModel::getLatency(MemoryRequestType type) const {
    switch (type) {
        // 1.11.56 (audit D054): see legacyNsAsCycles above.
        case MemoryRequestType::READ:
            return legacyNsAsCycles(mram_config_.read_latency_ns);
        case MemoryRequestType::WRITE:
            return legacyNsAsCycles(mram_config_.write_latency_ns);
        case MemoryRequestType::ATOMIC:
            return legacyNsAsCycles(mram_config_.read_latency_ns +
                                    mram_config_.write_latency_ns);
        default:
            return legacyNsAsCycles(mram_config_.read_latency_ns);
    }
}

double STTMRAMModel::getTotalEnergy() const {
    /* 1.11.58 (audit D022/D043): NANOJOULES, as MemoryModel documents.
     *
     * Two errors lived on these three lines. The dynamic term multiplied a
     * pJ/byte density by an access count and called the product picojoules
     * (D044, fixed in 1.11.57 by multiplying the bytes back); now the members
     * are nJ/access at assignment, so the multiply is just a count. The
     * leakage term added JOULES (watts x seconds) to that sum and the
     * function returned picojoules under a header documenting nanojoules --
     * a term 1e9 too small inside a result 1000x too large. Both halves are
     * nanojoules now.
     *
     * The cycle-to-seconds conversion treats one cycle as one nanosecond.
     * That is not an estimate of a clock -- these models carry none; it is
     * the same convention legacyNsAsCycles() states, and it is why leakage
     * in nJ is simply watts x cycles. */
    double dynamic_nj = (total_reads_ * read_energy_) +
                        (total_writes_ * write_energy_);
    double leakage_nj = leakage_power_ * static_cast<double>(current_cycle_);
    return dynamic_nj + leakage_nj;
}

void STTMRAMModel::printStats() const {
    std::cout << "\n=== STT-MRAM Model Statistics ===" << std::endl;
    std::cout << "Total Cycles: " << current_cycle_ << std::endl;
    std::cout << "Total Reads: " << total_reads_ << std::endl;
    std::cout << "Total Writes: " << total_writes_ << std::endl;
    std::cout << "Write Cycles (Endurance): " << write_cycles_ << " / " << endurance_ << std::endl;

    if (total_reads_ + total_writes_ > 0) {
        double read_ratio = static_cast<double>(total_reads_) / (total_reads_ + total_writes_);
        double write_ratio = static_cast<double>(total_writes_) / (total_reads_ + total_writes_);
        std::cout << "Read Ratio: " << (read_ratio * 100.0) << "%" << std::endl;
        std::cout << "Write Ratio: " << (write_ratio * 100.0) << "%" << std::endl;

        double endurance_used = (static_cast<double>(write_cycles_) / endurance_) * 100.0;
        std::cout << "Endurance Used: " << endurance_used << "%" << std::endl;
    }

    std::cout << "\nLatency (Inner-Bank Timing):" << std::endl;
    std::cout << "  Mat Read: " << getSubarrayReadLatency() << " ns" << std::endl;
    std::cout << "  Bank Read: " << getBankReadLatency() << " ns" << std::endl;
    std::cout << "  Chip Read: " << getChipReadLatency() << " ns" << std::endl;
    std::cout << "  Mat Write: " << getSubarrayWriteLatency() << " ns" << std::endl;
    std::cout << "  Bank Write: " << getBankWriteLatency() << " ns" << std::endl;
    std::cout << "  Chip Write: " << getChipWriteLatency() << " ns" << std::endl;

    std::cout << "\nEnergy Consumption:" << std::endl;
    /* 1.11.60 (audit round 4, C002): THE SECOND MULTIPLY IS GONE.
     *
     * These lines multiplied read_energy_/write_energy_ by bytes-per-access.
     * That was right at 1.11.57, when the members were an energy DENSITY;
     * 1.11.58 re-based them to nJ per access at assignment and moved
     * getTotalEnergy() (three lines above) onto the new basis without moving
     * this block, so the same file held both conventions and the "per access"
     * and "Total" lines below came out 64x high at the 512-bit configured line
     * -- printed as "pJ" while carrying nJ, and disagreeing with the "Total
     * Energy" line immediately beneath them by that factor times 1000.
     *
     * It was invisible because printStats() has no caller anywhere in the tree
     * (CompositePowerModel is the only route to it and is never constructed)
     * and because the counters it multiplies are never incremented, so the
     * product was 0 x wrong.
     *
     * The per-access figures are now the members themselves, and the per-byte
     * line is DERIVED from them rather than being the stored quantity, so the
     * two can no longer drift apart again. */
    const double bpa = bytesPerAccess(access_width_bits_);
    std::cout << "  Access Width: " << (bpa * 8.0) << " bits (" << bpa << " B)" << std::endl;
    std::cout << "  Read Energy (per access): " << read_energy_ << " nJ" << std::endl;
    std::cout << "  Write Energy (per access): " << write_energy_ << " nJ" << std::endl;
    std::cout << "  Read Energy (per byte): " << (read_energy_ * 1000.0 / bpa) << " pJ" << std::endl;
    std::cout << "  Write Energy (per byte): " << (write_energy_ * 1000.0 / bpa) << " pJ" << std::endl;
    std::cout << "  Total Read Energy: " << (total_reads_ * read_energy_) << " nJ" << std::endl;
    std::cout << "  Total Write Energy: " << (total_writes_ * write_energy_) << " nJ" << std::endl;
    std::cout << "  Leakage Power: " << leakage_power_ << " W" << std::endl;
    // 1.11.58: nJ, matching the MemoryModel contract getTotalEnergy() now obeys.
    std::cout << "  Total Energy: " << getTotalEnergy() << " nJ" << std::endl;
    std::cout << "================================\n" << std::endl;
}

void STTMRAMModel::resetStats() {
    total_reads_ = 0;
    total_writes_ = 0;
    write_cycles_ = 0;
    current_cycle_ = 0;
}

void STTMRAMModel::updateEndurance(Address addr) {
    // Per-cell endurance tracking
    // Calculate bank and page from address
    uint32_t bank = static_cast<uint32_t>((addr >> 12) % mram_config_.banks);
    uint64_t page = (addr >> 12) / mram_config_.banks;

    // Track writes per bank
    bank_write_counts_[bank]++;

    // Track writes per page (sampled - every 1000th page tracked)
    if (page % 1000 == 0) {
        page_write_counts_[page]++;

        // Check for hot pages (potential wear-out)
        if (page_write_counts_[page] > endurance_ / 1000) {
            std::cerr << "[STTMRAMModel] WARNING: Hot page detected at page "
                      << page << " with " << page_write_counts_[page]
                      << " writes (sampling 1:1000)" << std::endl;
        }
    }
}

void STTMRAMModel::initializeNVSim() {
#ifdef HAVE_NVSIM
    try {
        // Create NVSim configuration for STT-MRAM
        NVSimWrapper::NVMConfig nvsim_config;
        nvsim_config.capacity_bytes = mram_config_.capacity;
        /* 1.11.25: the run's access width when supplied, else the model

         * default. Mismatching this selects a different pregenerated

         * cache entry and characterizes a different access. */

        nvsim_config.word_width_bits = (access_width_bits_ > 0) ? access_width_bits_ : 64;
        nvsim_config.nvm_type = NVSimWrapper::NVMType::STTRAM;
        nvsim_config.process_node_nm = mram_config_.tech_node_nm;
        nvsim_config.temperature_k = temperature_k_;   // 1.11.52 (D055)  // 77 degC typical operating temp
        nvsim_config.optimize_read_energy = true;
        nvsim_config.optimize_write_energy = true;
        nvsim_config.optimize_leakage = true;
        nvsim_config.is_cache = false;

        // Create and initialize NVSim wrapper
        nvsim_wrapper_ = std::make_unique<NVSimWrapper>(nvsim_config);
        nvsim_wrapper_->initialize();

        // Extract NVSim results if valid
        if (nvsim_wrapper_->isValid()) {
            read_energy_ = nvsim_wrapper_->getReadDynamicEnergy();
            write_energy_ = nvsim_wrapper_->getWriteDynamicEnergy();
            leakage_power_ = nvsim_wrapper_->getLeakagePower() / 1000.0;  // mW to W
            area_mm2_ = nvsim_wrapper_->getArea();

            /* 1.11.56 (audit D054): NVSim reports SECONDS. Carry them as
             * nanoseconds. The old code multiplied by a hardcoded 1 GHz and
             * stored the product in a field the printout labelled "cycles", so
             * a run at 2 GHz reported half the cycles it would really spend.
             * There is no clock in this model to convert with, so time is what
             * it keeps. */
            mram_config_.read_latency_ns =
                nvsim_wrapper_->getReadLatency() * 1e9;
            mram_config_.write_latency_ns =
                nvsim_wrapper_->getWriteLatency() * 1e9;

            std::cout << "[STTMRAMModel] Using NVSim-generated parameters" << std::endl;
        } else {
            std::cerr << "[STTMRAMModel] NVSim failed: "
                      << nvsim_wrapper_->getErrorMessage() << std::endl;
            std::cerr << "[STTMRAMModel] Using architecture-based defaults" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[STTMRAMModel] NVSim exception: " << e.what() << std::endl;
        std::cerr << "[STTMRAMModel] Using architecture-based defaults" << std::endl;
    }
#else
    std::cout << "[STTMRAMModel] NVSim not available, using architecture-based values" << std::endl;
#endif
}

//=============================================================================
// Inner-Bank Timing Queries (NEW!)
//=============================================================================

double STTMRAMModel::getSubarrayReadLatency() const {
    if (!mram_arch_) return 0.0;
    return mram_arch_->timing.mat_read_ns;
}

double STTMRAMModel::getBankReadLatency() const {
    if (!mram_arch_) return 0.0;
    return mram_arch_->timing.bank_read_ns;
}

double STTMRAMModel::getChipReadLatency() const {
    if (!mram_arch_) return 0.0;
    return mram_arch_->timing.chip_read_ns;
}

double STTMRAMModel::getSubarrayWriteLatency() const {
    if (!mram_arch_) return 0.0;
    return mram_arch_->timing.mat_write_ns;
}

double STTMRAMModel::getBankWriteLatency() const {
    if (!mram_arch_) return 0.0;
    return mram_arch_->timing.bank_write_ns;
}

double STTMRAMModel::getChipWriteLatency() const {
    if (!mram_arch_) return 0.0;
    return mram_arch_->timing.chip_write_ns;
}

double STTMRAMModel::getInnerBankReadLatency() const {
    if (!mram_arch_) return 0.0;
    return mram_arch_->timing.inner_bank.getTotalReadLatency();
}

double STTMRAMModel::getInnerBankWriteLatency() const {
    if (!mram_arch_) return 0.0;
    return mram_arch_->timing.inner_bank.getTotalWriteLatency();
}

bool STTMRAMModel::supportsBankPIM() const {
    if (!mram_arch_) return false;
    return mram_arch_->isSuitableForPIM();
}

bool STTMRAMModel::supportsSubarrayPIM() const {
    // STT-MRAM supports subarray-level PIM (fast reads, persistent state)
    return true;
}


/* 1.11.24: STTMRAMModel under the plugin contract. NVM is not DRAM-like: subarray,
 * bank and chip only. The subarray/bank separation is the intra-bank H-tree
 * NVSim builds (routingMode = h_tree), not a multiplier -- see 1.11.23. */
double STTMRAMModel::getTierLatencyNs(Tier tier, Op op) const {
    if (op == Op::READ) {
        switch (tier) {
            case Tier::SUBARRAY: return getSubarrayReadLatency();
            case Tier::BANK:     return getBankReadLatency();
            case Tier::CHIP:     return getChipReadLatency();
            default:             return -1.0;
        }
    }
    if (op == Op::WRITE) {
        switch (tier) {
            case Tier::SUBARRAY: return getSubarrayWriteLatency();
            case Tier::BANK:     return getBankWriteLatency();
            case Tier::CHIP:     return getChipWriteLatency();
            default:             return -1.0;
        }
    }
    return -1.0;   // SET/RESET are a PCM distinction
}
bool STTMRAMModel::hasTier(Tier tier) const {
    return tier == Tier::SUBARRAY || tier == Tier::BANK || tier == Tier::CHIP;
}
/* 1.11.73: the one tier below the bank, from NVSim; -1 = not sourceable. */
int STTMRAMModel::l0UnitsPerBank() const {
    if (!mram_arch_) return -1;
    const int v = mram_arch_->organization.mats_per_bank;
    return (v > 0) ? v : -1;
}
int STTMRAMModel::l0WidthBits() const {
    if (!mram_arch_) return -1;
    const int v = mram_arch_->datapath.mat_io_bits;
    return (v > 0) ? v : -1;
}
double STTMRAMModel::l0BandwidthGBs() const {
    if (!mram_arch_) return -1.0;
    const int mw = mram_arch_->datapath.mat_io_bits;      // NVSim mat.numDataBit
    const int bw = mram_arch_->datapath.bank_io_bits;     // the configured word width
    const double lat_ns = mram_arch_->timing.bank_read_ns; // NVSim bank->readLatency
    if (mw <= 0 || bw <= 0 || lat_ns <= 0.0) return -1.0;
    // NVSim bank read bandwidth (word / readLatency) scaled by the mat's share of the word
    return ((bw / 8.0) / lat_ns) * (static_cast<double>(mw) / bw);
}
std::string STTMRAMModel::tierLatencySource(Tier tier, Op op) const {
    if (getTierLatencyNs(tier, op) < 0.0) return "";
    switch (tier) {
        case Tier::SUBARRAY: return "NVSim mat.readLatency (the mat, one tier below the bank)";
        case Tier::BANK:     return "NVSim bank->readLatency (incl. H-tree)";
        case Tier::CHIP:     return "NVSim bank + configured net hop";
        default:             return "";
    }
}


/* 1.11.25: characterize the array the RUN configures, not the model's
 * compiled-in default. Must be called before initialize(). */
void STTMRAMModel::setArrayCapacityBytes(uint64_t bytes) {
    if (bytes > 0) mram_config_.capacity = bytes;
}


/* 1.11.25: characterize the access the RUN performs. Must precede
 * initialize(). 0 keeps the model default. */
/* 1.11.51 (L70): the run's node, replacing the compiled-in default.
 * Must precede initialize(). */
/* 1.11.52 (D055): the run's temperature, replacing the hardcoded 350 K in
 * the tool query below. Must precede initialize(). */
void STTMRAMModel::setTemperatureK(int k) {
    if (k > 0) temperature_k_ = k;
}

void STTMRAMModel::setTechNodeNm(int nm) {
    if (nm > 0) mram_config_.tech_node_nm = nm;
}

void STTMRAMModel::setAccessWidthBits(uint32_t bits) {
    if (bits >= 8 && bits <= 1024) access_width_bits_ = bits;
}


/* 1.11.57 (latent D053): the bandwidth this model can source is NONE.
 * `bandwidth_ = capacity_ / 100` was an invented bytes/s figure -- no tool, no
 * measurement, no unit stated -- answering the same MemoryModel::getBandwidth()
 * that DRAMModel answers from Ramulator's own number. It reached no result
 * only because nothing calls getBandwidth() on these objects today. 0 is the
 * absent value; the note fires once per process so a future caller cannot read
 * the 0 as a measured zero. */
uint64_t STTMRAMModel::getBandwidth() const {
    static bool announced = false;
    if (!announced) {
        announced = true;
        std::cerr << "[STTMRAMModel] getBandwidth() returns 0 = NOT SOURCED. This model "
                     "characterizes latency, energy and area through its tool; "
                     "it has no sustained-bandwidth model, and the literal that "
                     "used to stand here (capacity_ / 100) was fabricated. Compose a "
                     "bandwidth from the tier latency and the access width at "
                     "the call site, or add a sourced model here."
                  << std::endl;
    }
    return 0;
}

} // namespace pimid
