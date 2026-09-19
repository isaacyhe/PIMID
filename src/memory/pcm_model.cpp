#include "memory/pcm_model.h"
#include "memory/nvsim_wrapper.h"
#include "memory/pcm_architecture.h"
#include "memory/architecture_extractor.h"
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <map>
#include <unordered_map>

static inline double bytesPerAccess(uint32_t access_width_bits) {
    // 64 bits is what initializeNVSim() passes NVSim when the knob is unset.
    double bytes = (access_width_bits > 0 ? access_width_bits : 64u) / 8.0;
    return (bytes > 0.0) ? bytes : 8.0;
}

namespace pimid {

//=============================================================================
// PCMModel Implementation
//=============================================================================

PCMModel::PCMModel(const std::string& config_path)
    : MemoryModel(MemoryTechnology::PCM, config_path)
    , nvsim_wrapper_(nullptr)
    , pcm_arch_(nullptr)
    , total_reads_(0)
    , total_set_writes_(0)
    , write_cycles_(0)
    , read_energy_(0.0)
    , write_energy_(0.0)
    , leakage_power_(0.0)
    , area_mm2_(0.0)
    , current_cycle_(0)
    , capacity_(0)
    , bandwidth_(0)
    , endurance_(0) {

    // Initialize default PCM configuration
    pcm_config_.capacity = 1ULL * 1024 * 1024 * 1024;  // 1GB
    pcm_config_.banks = 16;
    pcm_config_.read_write_ports = 1;
    pcm_config_.tech_node_nm = 90;
    /* 1.11.56 (audit D054): nanoseconds, not cycles. These are placeholders
     * that initializeNVSim() overwrites on every reachable path; they exist so
     * the struct is not read uninitialized if the tool binding fails, and the
     * model refuses (throws) in that case anyway. */
    pcm_config_.read_latency_ns = 12.0;         // ns (moderate)
    pcm_config_.set_write_latency_ns = 100.0;   // ns (VERY SLOW!)
    pcm_config_.reset_write_latency_ns = 40.0;  // ns (faster than SET)
    pcm_config_.endurance = 1e8;           // 10^8 writes (limited)
    pcm_config_.is_pim_enabled = true;
}

void PCMModel::initialize() {
    std::cout << "[PCMModel] Initializing PCM model..." << std::endl;
    loadConfig(config_path_);

    // Calculate derived parameters
    capacity_ = pcm_config_.capacity;
    /* 1.11.57 (latent D053): NO FABRICATED BANDWIDTH. This was
     * `bandwidth_ = capacity_ / 200;` -- a bytes/s figure derived from a capacity by an
     * unsourced ratio, answering the same contract method DRAMModel answers
     * from Ramulator. Absent is 0, and getBandwidth() announces it. */
    bandwidth_ = 0;
    endurance_ = pcm_config_.endurance;

    // Initialize NVSim FIRST (before architecture creation)
    initializeNVSim();

    // Initialize PCM architecture with inner-bank timing
    // PRIORITY: Extract from NVSim if available, otherwise use factory defaults
#ifdef HAVE_NVSIM
    if (nvsim_wrapper_ && nvsim_wrapper_->isValid()) {
        // Extract architecture directly from NVSim results (PREFERRED!)
        pcm_arch_ = memory::extractPCMArchitecture(*nvsim_wrapper_,
            "PCM-NVSim-Extracted", 0.5);

        if (pcm_arch_) {
            std::cout << "[PCMModel] Architecture EXTRACTED from NVSim" << std::endl;
            /* 1.11.23: surface the tier ladder with its provenance. Nothing in
             * the log used to say where a latency came from, so a ladder built
             * from invented multipliers was indistinguishable from a tool-read
             * one -- which is how reset = write * 0.3 survived. The residual
             * between mat and bank IS the intra-bank H-tree (NVSim runs
             * with routingMode = h_tree). */
            {
                const auto& t = pcm_arch_->timing;
                std::cout << "  [tier] mat      " << t.mat_read_ns
                          << " ns (NVSim components) | bank " << t.bank_read_ns
                          << " ns (NVSim bank->readLatency) | H-tree residual "
                          << (t.bank_read_ns - t.mat_read_ns) << " ns"
                          << std::endl;
            }
        } else {
            std::cerr << "[PCMModel] NVSim extraction failed, using factory defaults" << std::endl;
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
    if (!pcm_arch_) {
        throw std::runtime_error(
            "[PCMModel] NVSim characterization failed and there is no fallback. "
            "The hand-written default specs were removed in 1.11.24 because "
            "they were unsourced and indistinguishable from tool output. "
            "Fix the NVSim configuration rather than pricing this run from "
            "invented numbers.");
    }

    std::cout << "[PCMModel] Inner-bank read latency: "
              << pcm_arch_->timing.inner_bank.getTotalReadLatency() << " ns" << std::endl;
    /* 1.11.56 (audit D062): say when the SET path is a substitution. */
    std::cout << "[PCMModel] Inner-bank SET write latency: "
              << pcm_arch_->timing.inner_bank.getTotalSetWriteLatency() << " ns"
              << (pcm_arch_->timing.set_from_generic_write
                  ? " (NVSim did not resolve setLatency -- this IS the generic"
                    " write path)"
                  : "")
              << std::endl;
    std::cout << "[PCMModel] Inner-bank RESET write latency: "
              << pcm_arch_->timing.inner_bank.getTotalResetWriteLatency() << " ns" << std::endl;
    std::cout << "[PCMModel] Architecture source: "
              << pcm_arch_->timing.inner_bank.source << std::endl;

    // Use architecture energy values (may have been extracted from NVSim)
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
    read_energy_ = pcm_arch_->energy.read_energy_per_byte
                   * bytesPerAccess(access_width_bits_) / 1000.0;   // pJ/B -> nJ/access
    /* 1.11.60 (audit round 4, C015): THE SET ENERGY, because every write this
     * model sees is charged as a SET everywhere else.
     *
     * This used to read energy.write_energy_per_byte, which the extractor
     * builds as 0.5 x SET + 0.5 x RESET (architecture_extractor.h) with RESET
     * itself asserted at 0.55 x SET -- so the field held 0.775 x SET. It was
     * then multiplied by total_set_writes_ under a comment saying "every write
     * counted as SET", and the latency side charges set_write_latency_ns for
     * every write, and the counter D056 left is the SET counter. Three of the
     * four statements about a PCM write said SET; the energy said mean, and
     * nothing reconciled them. On the technology whose whole point is that SET
     * and RESET differ, that is not a rounding difference: it was 22.5% low
     * per write against the basis its own comment claimed.
     *
     * Resolved toward SET rather than toward the mean, for two reasons. The
     * counters cannot tell the two apart -- MemoryRequestType has no RESET,
     * which is exactly why D056 deleted the RESET counter -- so a mean has no
     * measured mix behind it; and bank_set_energy_pJ is NVSim's own
     * getWriteDynamicEnergy(), whereas the mean reaches its value only through
     * the unsourced 0.55 RESET ratio. Taking the bank figure directly also
     * skips the per-byte round trip: this model is what hands NVSim its
     * word_width_bits (initializeNVSim()), so the bank write energy IS the
     * per-access energy at this width, and dividing by 1000 is the whole
     * conversion. The read line above reconstructs the same identity the long
     * way round and is left alone so its arithmetic stays visible.
     *
     * It was invisible because getTotalEnergy() has no reachable caller and
     * access() -- the only thing that increments the counter -- is never
     * called, so the term was 0 x wrong. */
    write_energy_ = pcm_arch_->energy.bank_set_energy_pJ / 1000.0;   // pJ/access -> nJ/access
    leakage_power_ = pcm_arch_->energy.chip_leakage_mw / 1000.0;

    std::cout << "[PCMModel] Configuration:" << std::endl;
    std::cout << "  Capacity: " << (pcm_config_.capacity / (1024.0 * 1024 * 1024)) << " GB" << std::endl;
    std::cout << "  Banks: " << pcm_config_.banks << std::endl;
    std::cout << "  Technology: " << pcm_config_.tech_node_nm << " nm" << std::endl;
    /* 1.11.56 (audit D054): printed as NANOSECONDS, which is what NVSim
     * reported and what these fields hold. They were labelled "cycles" while
     * carrying a 1 GHz product, so the number was right only for a 1 GHz PE. */
    std::cout << "  Read Latency: " << pcm_config_.read_latency_ns << " ns" << std::endl;
    std::cout << "  SET Write Latency: " << pcm_config_.set_write_latency_ns << " ns (SLOW!)" << std::endl;
    /* 1.11.56 (audit D045): name the substitution when there is one. */
    std::cout << "  RESET Write Latency: " << pcm_config_.reset_write_latency_ns << " ns"
              << (reset_latency_is_set_path_
                  ? " (NVSim did not resolve resetLatency -- this IS the SET path)"
                  : " (NVSim FunctionUnit::resetLatency)")
              << std::endl;
    std::cout << "  Endurance: " << pcm_config_.endurance << " writes (limited)" << std::endl;
    std::cout << "  WARNING: PCM only suitable for read-heavy PIM workloads!" << std::endl;
    /* 1.11.60 (audit round 4, C001): nJ PER ACCESS. 1.11.58 re-based the
     * member and left this label on the retired pJ/byte basis, so the printed
     * number was 15.6x below what it claimed at the 512-bit configured line
     * (125x at the 64-bit model default). Invisible because the value was
     * already correct and because the block comment further down cited this
     * very print as the evidence for the old unit. The "(Nx read)" ratio on
     * the write line is unit-invariant and is unaffected. */
    std::cout << "  Read Energy: " << read_energy_ << " nJ/access" << std::endl;
    /* 1.11.56 (audit D046): the "(30x read!)" tag was a constant printed over
     * a computed number. Nothing ever compared the two -- on the shipped
     * characterization the real ratio is nowhere near 30 -- so the log asserted
     * a headline figure that the run itself contradicted. Compute the ratio
     * from the two numbers being printed, or print no ratio at all. */
    std::cout << "  SET Write Energy: " << write_energy_ << " nJ/access";   // 1.11.60 (C001, C015)
    if (read_energy_ > 0.0)
        std::cout << " (" << (write_energy_ / read_energy_) << "x read)";
    std::cout << std::endl;
    std::cout << "  Leakage Power: " << leakage_power_ << " W" << std::endl;
    std::cout << "[PCMModel] Initialization complete" << std::endl;
}

void PCMModel::loadConfig(const std::string& config_path) {
    std::cout << "[PCMModel] Loading configuration from: " << config_path << std::endl;
    std::cout << "[PCMModel] Using default 1GB PCM configuration" << std::endl;
}

/* 1.11.56 (audit D054): THE ONE PLACE TIME BECOMES CYCLES IN THIS MODEL.
 *
 * MemoryModel's legacy access()/getLatency() return Cycle, and this model is
 * handed no clock -- the simulator drives it through getTierLatencyNs(), which
 * is nanoseconds end to end and never comes through here. So the conversion
 * below is 1 cycle per nanosecond, i.e. exactly the 1 GHz the old code assumed;
 * the difference is that it is now stated once, at the boundary that forces it,
 * instead of being baked into the stored fields and then mislabelled in the
 * log. If this path ever becomes live, give the model a frequency and convert
 * with that -- do not restore the assumption upstream. */
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
 * read_energy_/write_energy_ HELD pJ PER BYTE when this was written --
 * initialize() printed them as "pJ/byte", and they came from
 * PCMArchitecture::energy.read_energy_per_byte /
 * write_energy_per_byte, which the extractor computes as a bank energy divided
 * by (word_width_bits / 8) (architecture_extractor.h). getTotalEnergy() and
 * printStats() multiplied that energy DENSITY by an ACCESS COUNT and printed
 * the product as "pJ", understating the dynamic energy by exactly the access
 * size in bytes -- 8x at this model's 64-bit default.
 *
 * The consumers want per-access energy, so the arithmetic multiplies the
 * density back by the same bytes-per-access the extractor divided by, which
 * recovers the bank energy exactly instead of approximating it. The width must
 * come from access_width_bits_ for that identity to hold, since
 * initializeNVSim() is what hands it to NVSim; "0 = model default" is the
 * 64 bits initializeNVSim() substitutes.
 *
 * WHY IT WAS INVISIBLE: getTotalEnergy() and printStats() have no reachable
 * caller, and access() -- the only thing that increments the counters -- is
 * never called either, so the product was 0 * wrong.
 *
 * NOT FIXED HERE, deliberately: the leakage term adds JOULES (W x s) to a
 * picojoule sum, and getTotalEnergy() returns picojoules where MemoryModel
 * documents nanojoules. Those are audit D022 and D043; they span all five
 * plugin models and memory_model.h requires them to be one gated change. */

Cycle PCMModel::access(const MemoryRequest& req) {
    Cycle latency = legacyNsAsCycles(pcm_config_.read_latency_ns);  // safe default

    // PCM has VERY asymmetric read/write latency
    switch (req.type) {
        case MemoryRequestType::READ:
            latency = legacyNsAsCycles(pcm_config_.read_latency_ns);
            total_reads_++;
            break;
        case MemoryRequestType::WRITE:
            // Use SET latency (conservative estimate)
            latency = legacyNsAsCycles(pcm_config_.set_write_latency_ns);  // VERY SLOW!
            total_set_writes_++;
            write_cycles_++;
            updateEndurance(req.addr);
            if (write_cycles_ > endurance_) {
                std::cerr << "[PCMModel] WARNING: Endurance limit exceeded!" << std::endl;
            }
            break;
        case MemoryRequestType::ATOMIC:
            latency = legacyNsAsCycles(pcm_config_.read_latency_ns +
                                       pcm_config_.set_write_latency_ns);
            total_reads_++;
            total_set_writes_++;
            write_cycles_++;
            break;
    }

    // Add queuing delay
    if (!pending_requests_.empty()) {
        latency += pending_requests_.size() * 2;  // Higher penalty
    }

    pending_requests_.push(req);

    return latency;
}

bool PCMModel::canAccept(const MemoryRequest& req) {
    const size_t MAX_PENDING_REQUESTS = 16;  // Smaller queue due to slow writes
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

void PCMModel::tick() {
    current_cycle_++;

    if (!pending_requests_.empty()) {
        pending_requests_.pop();
    }
}

Cycle PCMModel::getLatency(MemoryRequestType type) const {
    switch (type) {
        // 1.11.56 (audit D054): see legacyNsAsCycles above.
        case MemoryRequestType::READ:
            return legacyNsAsCycles(pcm_config_.read_latency_ns);
        case MemoryRequestType::WRITE:
            return legacyNsAsCycles(pcm_config_.set_write_latency_ns);  // Conservative (SET)
        case MemoryRequestType::ATOMIC:
            return legacyNsAsCycles(pcm_config_.read_latency_ns +
                                    pcm_config_.set_write_latency_ns);
        default:
            return legacyNsAsCycles(pcm_config_.read_latency_ns);
    }
}

double PCMModel::getTotalEnergy() const {
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
    /* 1.11.60 (audit round 4, C015): the comment on this line used to be the
     * only place that said what the write term was, and it said "every write
     * counted as SET" while write_energy_ held the SET/RESET mean. The counter
     * and the latency were SET; the energy was not. write_energy_ is now the
     * SET energy at assignment (see initialize()), so the line means what it
     * says: every write is priced as a SET, in time and in energy, because the
     * request interface carries no way to tell a SET from a RESET. */
    double dynamic_nj = (total_reads_ * read_energy_) +
                        (total_set_writes_ * write_energy_);   // every write is a SET (D056, C015)
    double leakage_nj = leakage_power_ * static_cast<double>(current_cycle_);
    return dynamic_nj + leakage_nj;
}

void PCMModel::printStats() const {
    std::cout << "\n=== PCM Model Statistics ===" << std::endl;
    std::cout << "Total Cycles: " << current_cycle_ << std::endl;
    std::cout << "Total Reads: " << total_reads_ << std::endl;
    std::cout << "Total SET Writes: " << total_set_writes_ << std::endl;
    /* 1.11.57 (latent D056): the "Total RESET Writes" line is gone with its
     * counter -- see pcm_model.h. Every write this model sees is priced and
     * counted as a SET, and the label now says that instead of implying a
     * split that access() never performed. */
    std::cout << "Total Writes (all counted as SET): "
              << total_set_writes_ << std::endl;
    std::cout << "Write Cycles (Endurance): " << write_cycles_ << " / " << endurance_ << std::endl;

    uint64_t total_ops = total_reads_ + total_set_writes_;
    if (total_ops > 0) {
        double read_ratio = static_cast<double>(total_reads_) / total_ops;
        double write_ratio = static_cast<double>(total_set_writes_) / total_ops;
        std::cout << "Read Ratio: " << (read_ratio * 100.0) << "%" << std::endl;
        std::cout << "Write Ratio: " << (write_ratio * 100.0) << "%" << std::endl;

        if (write_ratio > 0.2) {
            std::cout << "WARNING: High write ratio detected! PCM is best for read-heavy workloads." << std::endl;
        }

        double endurance_used = (static_cast<double>(write_cycles_) / endurance_) * 100.0;
        std::cout << "Endurance Used: " << endurance_used << "%" << std::endl;
    }

    std::cout << "\nLatency (Inner-Bank Timing):" << std::endl;
    std::cout << "  Mat Read: " << getSubarrayReadLatency() << " ns" << std::endl;
    std::cout << "  Bank Read: " << getBankReadLatency() << " ns" << std::endl;
    std::cout << "  Chip Read: " << getChipReadLatency() << " ns" << std::endl;
    std::cout << "  Mat SET Write: " << getSubarraySetWriteLatency() << " ns (SLOW!)" << std::endl;
    std::cout << "  Bank SET Write: " << getBankSetWriteLatency() << " ns" << std::endl;
    std::cout << "  Chip SET Write: " << getChipSetWriteLatency() << " ns" << std::endl;

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
    // 1.11.56 (audit D046): the ratio is measured here, not asserted.
    std::cout << "  SET Write Energy (per access): " << write_energy_ << " nJ";   // 1.11.60 (C015)
    if (read_energy_ > 0.0)
        std::cout << " (" << (write_energy_ / read_energy_) << "x read)";
    std::cout << std::endl;
    std::cout << "  Read Energy (per byte): " << (read_energy_ * 1000.0 / bpa) << " pJ" << std::endl;
    std::cout << "  SET Write Energy (per byte): " << (write_energy_ * 1000.0 / bpa) << " pJ" << std::endl;
    std::cout << "  Total Read Energy: " << (total_reads_ * read_energy_) << " nJ" << std::endl;
    std::cout << "  Total Write Energy: "
              << (total_set_writes_ * write_energy_) << " nJ" << std::endl;
    std::cout << "  Leakage Power: " << leakage_power_ << " W" << std::endl;
    // 1.11.58: nJ, matching the MemoryModel contract getTotalEnergy() now obeys.
    std::cout << "  Total Energy: " << getTotalEnergy() << " nJ" << std::endl;
    std::cout << "================================\n" << std::endl;
}

void PCMModel::resetStats() {
    total_reads_ = 0;
    total_set_writes_ = 0;
    write_cycles_ = 0;
    current_cycle_ = 0;
}

void PCMModel::updateEndurance(Address addr) {
    // Per-cell endurance tracking with wear-leveling for PCM
    // PCM has LIMITED endurance (~10^8 writes) - CRITICAL to track!

    // Calculate bank and page from address
    uint32_t bank = static_cast<uint32_t>((addr >> 12) % pcm_config_.banks);
    uint64_t page = (addr >> 12) / pcm_config_.banks;
    uint64_t cell = addr >> 6;  // Cell granularity (64-byte line)

    // Track writes per bank
    bank_write_counts_[bank]++;

    // Track writes per page (every 50th page - more aggressive due to low endurance)
    if (page % 50 == 0) {
        page_write_counts_[page]++;

        // PCM has very limited endurance - warn at lower threshold
        uint64_t hot_threshold = endurance_ / 50;  // Scaled by sampling factor
        if (page_write_counts_[page] > hot_threshold) {
            // Critical warning for PCM - endurance is very limited!
            std::cerr << "[PCMModel] CRITICAL: Hot page at " << page
                      << " with " << page_write_counts_[page]
                      << " writes (sampling 1:50)!" << std::endl;
            std::cerr << "[PCMModel] PCM endurance is LIMITED (~10^8). "
                      << "Consider wear-leveling!" << std::endl;
        }
    }

    // Track high-write cells for wear-leveling (sample every 500th cell)
    // More aggressive sampling for PCM due to lower endurance
    if (cell % 500 == 0) {
        cell_write_counts_[cell]++;

        // Warn about hot cells much earlier for PCM
        if (cell_write_counts_[cell] > endurance_ / 5000) {
            std::cerr << "[PCMModel] WARNING: Hot cell at address 0x"
                      << std::hex << addr << std::dec
                      << " with " << cell_write_counts_[cell] << " writes."
                      << " (sampling 1:500)" << std::endl;
            std::cerr << "[PCMModel] Consider applying wear-leveling to extend lifetime."
                      << std::endl;
        }
    }

    // Report bank-level wear imbalance periodically (every 50K writes for PCM)
    if (write_cycles_ % 50000 == 0 && write_cycles_ > 0) {
        reportWearImbalance();
    }

    // Suggest wear-leveling when approaching 10% endurance consumption
    if (write_cycles_ % 1000000 == 0) {
        double endurance_used_pct = (static_cast<double>(write_cycles_) / endurance_) * 100.0;
        if (endurance_used_pct > 10.0) {
            std::cerr << "[PCMModel] WARNING: " << endurance_used_pct
                      << "% of total endurance consumed!" << std::endl;
            std::cerr << "[PCMModel] Recommend enabling wear-leveling if not already active."
                      << std::endl;
        }
    }
}

void PCMModel::reportWearImbalance() const {
    if (bank_write_counts_.empty()) return;

    uint64_t max_bank_writes = 0;
    uint64_t min_bank_writes = UINT64_MAX;
    uint32_t max_bank_id = 0;
    uint32_t min_bank_id = 0;

    for (const auto& [bank_id, count] : bank_write_counts_) {
        if (count > max_bank_writes) {
            max_bank_writes = count;
            max_bank_id = bank_id;
        }
        if (count < min_bank_writes) {
            min_bank_writes = count;
            min_bank_id = bank_id;
        }
    }

    if (min_bank_writes > 0 && max_bank_writes / min_bank_writes > 2) {
        std::cerr << "[PCMModel] WARNING: Bank wear imbalance detected!" << std::endl;
        std::cerr << "  Max writes: bank " << max_bank_id << " (" << max_bank_writes << " writes)" << std::endl;
        std::cerr << "  Min writes: bank " << min_bank_id << " (" << min_bank_writes << " writes)" << std::endl;
        std::cerr << "  Imbalance ratio: " << (max_bank_writes / min_bank_writes) << "x" << std::endl;
        std::cerr << "  CRITICAL: PCM has limited endurance! Enable wear-leveling now." << std::endl;
    }

    // Also check for cells approaching wear-out
    uint64_t cells_near_wearout = 0;
    for (const auto& [cell_id, count] : cell_write_counts_) {
        // If sampled cell has more than 1/1000th of endurance, it's a hot cell
        if (count * 500 > endurance_ / 1000) {  // Account for 1:500 sampling
            cells_near_wearout++;
        }
    }

    if (cells_near_wearout > 0) {
        std::cerr << "[PCMModel] WARNING: ~" << (cells_near_wearout * 500)
                  << " cells estimated to be approaching wear-out!" << std::endl;
    }
}

//=============================================================================
// Inner-Bank Timing Queries (NEW!)
//=============================================================================

double PCMModel::getSubarrayReadLatency() const {
    if (!pcm_arch_) return 0.0;
    return pcm_arch_->timing.mat_read_ns;
}

double PCMModel::getBankReadLatency() const {
    if (!pcm_arch_) return 0.0;
    return pcm_arch_->timing.bank_read_ns;
}

double PCMModel::getChipReadLatency() const {
    if (!pcm_arch_) return 0.0;
    return pcm_arch_->timing.chip_read_ns;
}

double PCMModel::getSubarraySetWriteLatency() const {
    if (!pcm_arch_) return 0.0;
    return pcm_arch_->timing.mat_set_ns;
}

double PCMModel::getBankSetWriteLatency() const {
    if (!pcm_arch_) return 0.0;
    return pcm_arch_->timing.bank_set_ns;
}

double PCMModel::getChipSetWriteLatency() const {
    if (!pcm_arch_) return 0.0;
    return pcm_arch_->timing.chip_set_ns;
}

double PCMModel::getSubarrayResetWriteLatency() const {
    if (!pcm_arch_) return 0.0;
    return pcm_arch_->timing.mat_reset_ns;
}

double PCMModel::getBankResetWriteLatency() const {
    if (!pcm_arch_) return 0.0;
    return pcm_arch_->timing.bank_reset_ns;
}

double PCMModel::getChipResetWriteLatency() const {
    if (!pcm_arch_) return 0.0;
    return pcm_arch_->timing.chip_reset_ns;
}

double PCMModel::getInnerBankReadLatency() const {
    if (!pcm_arch_) return 0.0;
    return pcm_arch_->timing.inner_bank.getTotalReadLatency();
}

bool PCMModel::supportsBankPIM() const {
    if (!pcm_arch_) return false;
    return pcm_arch_->isSuitableForPIM();
}

bool PCMModel::supportsSubarrayPIM() const {
    // PCM supports subarray PIM, but ONLY for read-heavy workloads
    return true;
}

//=============================================================================
// NVSim Integration (NEW!)
//=============================================================================

void PCMModel::initializeNVSim() {
#ifdef HAVE_NVSIM
    try {
        // Create NVSim configuration for PCM
        NVSimWrapper::NVMConfig nvsim_config;
        nvsim_config.capacity_bytes = pcm_config_.capacity;
        /* 1.11.25: the run's access width when supplied, else the model

         * default. Mismatching this selects a different pregenerated

         * cache entry and characterizes a different access. */

        nvsim_config.word_width_bits = (access_width_bits_ > 0) ? access_width_bits_ : 64;
        nvsim_config.nvm_type = NVSimWrapper::NVMType::PCRAM;  // PCM type
        nvsim_config.process_node_nm = pcm_config_.tech_node_nm;
        nvsim_config.temperature_k = temperature_k_;   // 1.11.52 (D055)  // ~77 degC typical operating temp
        nvsim_config.optimize_read_energy = true;
        nvsim_config.optimize_write_energy = false;  // PCM writes are inherently slow
        nvsim_config.optimize_leakage = true;
        nvsim_config.is_cache = false;

        // Create and initialize NVSim wrapper
        nvsim_wrapper_ = std::make_unique<NVSimWrapper>(nvsim_config);
        nvsim_wrapper_->initialize();

        // Extract NVSim results if valid
        if (nvsim_wrapper_->isValid()) {
            /* 1.11.56 (audit D054): NVSim reports SECONDS. Carry them as
             * nanoseconds. The old code multiplied by a hardcoded 1 GHz and
             * stored the product in a field called *_latency, which the
             * printout then labelled "cycles" -- so a run at 2 GHz reported
             * half the cycles it would really spend. There is no clock in this
             * model to convert with, so time is what it keeps. */
            double read_ns = nvsim_wrapper_->getReadLatency() * 1e9;
            double write_ns = nvsim_wrapper_->getWriteLatency() * 1e9;

            if (read_ns > 0) {
                pcm_config_.read_latency_ns = read_ns;
            }
            if (write_ns > 0) {
                pcm_config_.set_write_latency_ns = write_ns;
                /* 1.11.56 (audit D045): RESET is a MELT-QUENCH PULSE, and its
                 * duration is a cell property NVSim resolves as
                 * FunctionUnit::resetLatency -- not 30% of a composed write
                 * latency. 1.11.23 removed that assertion from the extractor
                 * and its release note said so, but this copy survived and is
                 * what the "RESET Write Latency" line prints. Ask the tool; if
                 * the tool does not resolve it (notably on a cache hit, where
                 * the pregenerated entry carries only the top-level figures),
                 * collapse to the SET path and say so, exactly as
                 * extractPCMArchitecture does. Reporting SET under a RESET
                 * label is honest; manufacturing a RESET number is not. */
                const double rst_s = nvsim_wrapper_->getResetLatency();
                if (rst_s > 0.0) {
                    pcm_config_.reset_write_latency_ns = rst_s * 1e9;
                    reset_latency_is_set_path_ = false;
                } else {
                    pcm_config_.reset_write_latency_ns = write_ns;
                    reset_latency_is_set_path_ = true;
                }
            }

            // Update energy values
            read_energy_ = nvsim_wrapper_->getReadDynamicEnergy();
            write_energy_ = nvsim_wrapper_->getWriteDynamicEnergy();
            leakage_power_ = nvsim_wrapper_->getLeakagePower() / 1000.0;  // mW to W
            area_mm2_ = nvsim_wrapper_->getArea();

            std::cout << "[PCMModel] Using NVSim-generated parameters" << std::endl;
            std::cout << "[PCMModel] NVSim Read Latency: " << read_ns << " ns" << std::endl;
            std::cout << "[PCMModel] NVSim Write Latency: " << write_ns << " ns" << std::endl;
        } else {
            std::cerr << "[PCMModel] NVSim failed: "
                      << nvsim_wrapper_->getErrorMessage() << std::endl;
            std::cerr << "[PCMModel] Using architecture-based defaults" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[PCMModel] NVSim exception: " << e.what() << std::endl;
        std::cerr << "[PCMModel] Using architecture-based defaults" << std::endl;
    }
#else
    std::cout << "[PCMModel] NVSim not available, using architecture-based values" << std::endl;
#endif
}


/* 1.11.24: PCM under the plugin contract. It is the ONE technology whose
 * write splits into SET and RESET, and the contract carries that rather than
 * flattening it -- 1.11.23 found RESET being asserted as write * 0.3 when
 * NVSim resolves FunctionUnit::setLatency/resetLatency separately.
 * Op::WRITE maps to SET, the slower and conservative path. */
double PCMModel::getTierLatencyNs(Tier tier, Op op) const {
    switch (op) {
        case Op::READ:
            switch (tier) {
                case Tier::SUBARRAY: return getSubarrayReadLatency();
                case Tier::BANK:     return getBankReadLatency();
                case Tier::CHIP:     return getChipReadLatency();
                default:             return -1.0;
            }
        case Op::SET:
        case Op::WRITE:
            switch (tier) {
                case Tier::SUBARRAY: return getSubarraySetWriteLatency();
                case Tier::BANK:     return getBankSetWriteLatency();
                case Tier::CHIP:     return getChipSetWriteLatency();
                default:             return -1.0;
            }
        case Op::RESET:
            switch (tier) {
                case Tier::SUBARRAY: return getSubarrayResetWriteLatency();
                case Tier::BANK:     return getBankResetWriteLatency();
                case Tier::CHIP:     return getChipResetWriteLatency();
                default:             return -1.0;
            }
    }
    return -1.0;
}
bool PCMModel::hasTier(Tier tier) const {
    return tier == Tier::SUBARRAY || tier == Tier::BANK || tier == Tier::CHIP;
}
/* 1.11.73: the one tier below the bank, from NVSim; -1 = not sourceable. */
int PCMModel::l0UnitsPerBank() const {
    if (!pcm_arch_) return -1;
    const int v = pcm_arch_->organization.mats_per_bank;
    return (v > 0) ? v : -1;
}
int PCMModel::l0WidthBits() const {
    if (!pcm_arch_) return -1;
    const int v = pcm_arch_->datapath.mat_io_bits;
    return (v > 0) ? v : -1;
}
double PCMModel::l0BandwidthGBs() const {
    if (!pcm_arch_) return -1.0;
    const int mw = pcm_arch_->datapath.mat_io_bits;      // NVSim mat.numDataBit
    const int bw = pcm_arch_->datapath.bank_io_bits;     // the configured word width
    const double lat_ns = pcm_arch_->timing.bank_read_ns; // NVSim bank->readLatency
    if (mw <= 0 || bw <= 0 || lat_ns <= 0.0) return -1.0;
    // NVSim bank read bandwidth (word / readLatency) scaled by the mat's share of the word
    return ((bw / 8.0) / lat_ns) * (static_cast<double>(mw) / bw);
}
std::string PCMModel::tierLatencySource(Tier tier, Op op) const {
    if (getTierLatencyNs(tier, op) < 0.0) return "";
    /* 1.11.56 (audit D062): ATTRIBUTE WHAT WAS ACTUALLY READ. This claimed
     * "NVSim FunctionUnit::setLatency" for the SET/WRITE ops unconditionally,
     * including on the cache-hit path where getSetLatency() returns -1 and the
     * extractor substitutes the generic write latency -- which is the normal
     * path, not a corner. A provenance string that cannot be wrong is not
     * provenance. */
    const bool set_substituted =
        pcm_arch_ && pcm_arch_->timing.set_from_generic_write;
    const char* q = (op == Op::RESET) ? "NVSim FunctionUnit::resetLatency"
                  : (op == Op::READ)  ? "NVSim read path"
                  : set_substituted   ? "NVSim write path (setLatency not "
                                        "resolved; generic write substituted)"
                                      : "NVSim FunctionUnit::setLatency";
    return std::string(q) + " @ " + tierName(tier);
}


/* 1.11.25: characterize the array the RUN configures, not the model's
 * compiled-in default. Must be called before initialize(). */
void PCMModel::setArrayCapacityBytes(uint64_t bytes) {
    if (bytes > 0) pcm_config_.capacity = bytes;
}


/* 1.11.25: characterize the access the RUN performs. Must precede
 * initialize(). 0 keeps the model default. */
/* 1.11.51 (L70): the run's node, replacing the compiled-in default.
 * Must precede initialize(). */
/* 1.11.52 (D055): the run's temperature, replacing the hardcoded 350 K in
 * the tool query below. Must precede initialize(). */
void PCMModel::setTemperatureK(int k) {
    if (k > 0) temperature_k_ = k;
}

void PCMModel::setTechNodeNm(int nm) {
    if (nm > 0) pcm_config_.tech_node_nm = nm;
}

void PCMModel::setAccessWidthBits(uint32_t bits) {
    if (bits >= 8 && bits <= 1024) access_width_bits_ = bits;
}


/* 1.11.57 (latent D053): the bandwidth this model can source is NONE.
 * `bandwidth_ = capacity_ / 200` was an invented bytes/s figure -- no tool, no
 * measurement, no unit stated -- answering the same MemoryModel::getBandwidth()
 * that DRAMModel answers from Ramulator's own number. It reached no result
 * only because nothing calls getBandwidth() on these objects today. 0 is the
 * absent value; the note fires once per process so a future caller cannot read
 * the 0 as a measured zero. */
uint64_t PCMModel::getBandwidth() const {
    static bool announced = false;
    if (!announced) {
        announced = true;
        std::cerr << "[PCMModel] getBandwidth() returns 0 = NOT SOURCED. This model "
                     "characterizes latency, energy and area through its tool; "
                     "it has no sustained-bandwidth model, and the literal that "
                     "used to stand here (capacity_ / 200) was fabricated. Compose a "
                     "bandwidth from the tier latency and the access width at "
                     "the call site, or add a sourced model here."
                  << std::endl;
    }
    return 0;
}

} // namespace pimid
