#include "memory/sram_model.h"
#include "memory/cacti_wrapper.h"
#include "memory/sram_architecture.h"
#include "memory/architecture_extractor.h"
#include "config/config_parser.h"
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <yaml-cpp/yaml.h>

namespace pimid {

//=============================================================================
// SRAMModel Implementation (CACTI Integration)
//=============================================================================

SRAMModel::SRAMModel(const std::string& config_path)
    : MemoryModel(MemoryTechnology::SRAM, config_path)
    , cacti_wrapper_(nullptr)
    , total_reads_(0)
    , total_writes_(0)
    , total_accesses_(0)
    , read_energy_(0.0)
    , write_energy_(0.0)
    , leakage_power_(0.0)
    , area_mm2_(0.0)
    , current_cycle_(0)
    , capacity_(0)
    , bandwidth_(0) {

    // Initialize default SRAM configuration
    sram_config_.capacity = 256 * 1024;  // 256 KB
    sram_config_.line_size = 64;         // 64 bytes
    sram_config_.associativity = 8;      // 8-way set associative
    sram_config_.banks = 8;
    sram_config_.read_write_ports = 1;
    sram_config_.read_ports = 0;
    sram_config_.write_ports = 0;
    sram_config_.tech_node_nm = 22;      // 22nm technology
    /* 1.11.56 (audit D054): nanoseconds, not cycles. A placeholder the CACTI
     * path overwrites on every reachable run; the model refuses when that
     * binding fails, so it never reaches a result. */
    sram_config_.access_time_ns = 2.0;   // ns
}

void SRAMModel::initialize() {
    std::cout << "[SRAMModel] Initializing SRAM model with CACTI..." << std::endl;
    loadConfig(config_path_);

    // Calculate derived parameters
    capacity_ = sram_config_.capacity;
    /* 1.11.57 (latent D053): NO FABRICATED BANDWIDTH. This was
     * `bandwidth_ = capacity_ * 2;  // Simplified: 2x capacity per second` --
     * a number with no tool behind it, in bytes/s, derived from a capacity,
     * sitting behind the same getBandwidth() the DRAM path answers from
     * Ramulator. Nothing reads it today (main.cpp's getBandwidth() calls are
     * on RamulatorWrapper, a different class), which is the only reason it has
     * never been printed as a result. CACTI does report an access time and an
     * access width, but composing a sustained bandwidth from them is a model
     * decision this class is not the place to make -- so it reports ABSENT,
     * and getBandwidth() says so out loud. */
    bandwidth_ = 0;

#ifdef HAVE_CACTI
    try {
        // Create CACTI configuration
        CACTIWrapper::SRAMConfig cacti_config;
        cacti_config.capacity_bytes = sram_config_.capacity;
        cacti_config.line_size = sram_config_.line_size;
        cacti_config.associativity = sram_config_.associativity;
        cacti_config.banks = sram_config_.banks;
        cacti_config.read_write_ports = sram_config_.read_write_ports;
        cacti_config.read_ports = sram_config_.read_ports;
        cacti_config.write_ports = sram_config_.write_ports;
        cacti_config.tech_node_nm = sram_config_.tech_node_nm;
        cacti_config.temperature = sram_config_.temperature_k;   // 1.11.52 (D055)
        cacti_config.is_cache = true;
        /* 1.11.57 (latent D047): THE ACCESS WIDTH REACHES CACTI NOW.
         * setAccessWidthBits() stored into access_width_bits_ and nothing in
         * this file ever read it, so the knob the plugin contract documents as
         * "the ACCESS GEOMETRY ... selects a DIFFERENT design point" was inert
         * for SRAM while the three NVM models honoured it. It was invisible
         * because the one live writer (main.cpp) passes 512, which happens to
         * be CACTIWrapper::SRAMConfig's own default -- so the inert knob and
         * the working knob produced identical numbers. Any other value would
         * have been silently discarded. 0 keeps the wrapper default. */
        if (access_width_bits_ > 0) {
            cacti_config.output_width_bits = access_width_bits_;
        }

        // Create and initialize CACTI wrapper
        cacti_wrapper_ = std::make_unique<CACTIWrapper>(cacti_config);
        cacti_wrapper_->initialize();

        // Extract CACTI results
        if (cacti_wrapper_->isValid()) {
            read_energy_ = cacti_wrapper_->getDynamicReadEnergy();
            write_energy_ = cacti_wrapper_->getDynamicWriteEnergy();
            leakage_power_ = cacti_wrapper_->getLeakagePower() / 1000.0; // Convert mW to W
            area_mm2_ = cacti_wrapper_->getArea();

            /* 1.11.56 (audit D054): CACTI reports SECONDS. Take the time and
              * keep it. getAccessLatencyCycles(1e9) turned it into a "cycle"
              * count against a clock nobody supplied, and the log then printed
              * that count as cycles -- correct only for a 1 GHz PE. */
            sram_config_.access_time_ns = cacti_wrapper_->getAccessTime() * 1e9;

            std::cout << "[SRAMModel] Using CACTI-generated parameters" << std::endl;
        } else {
            /* 1.11.57 (latent D052): NO FALLBACK VALUES. These three sites
             * called useFallbackValues(), which assigned 0.5 nJ read, 0.8 nJ
             * write, 0.05 W leakage and 2.5 mm^2 -- the last surviving
             * counter-example to the 1.11.24 refusal policy this very file
             * states forty lines below. They could not reach a result only
             * because the `if (!sram_arch_)` throw further down fires on the
             * same failure and unwinds first: neutralised by statement order,
             * not by design, and one reordering away from live. The failure is
             * announced and the model refuses. */
            std::cerr << "[SRAMModel] CACTI failed: " << cacti_wrapper_->getErrorMessage() << std::endl;
            std::cerr << "[SRAMModel] There is no fallback: energy, leakage and "
                         "area stay at zero and initialization will refuse."
                      << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[SRAMModel] CACTI exception: " << e.what() << std::endl;
        std::cerr << "[SRAMModel] There is no fallback: energy, leakage and "
                     "area stay at zero and initialization will refuse."
                  << std::endl;
    }
#else
    std::cerr << "[SRAMModel] CACTI is not compiled in. There is no fallback "
                 "characterization; initialization will refuse." << std::endl;
#endif

    std::cout << "[SRAMModel] Configuration:" << std::endl;
    std::cout << "  Capacity: " << (sram_config_.capacity / 1024) << " KB" << std::endl;
    std::cout << "  Line Size: " << sram_config_.line_size << " bytes" << std::endl;
    std::cout << "  Associativity: " << sram_config_.associativity << "-way" << std::endl;
    std::cout << "  Banks: " << sram_config_.banks << std::endl;
    std::cout << "  Ports (RW): " << sram_config_.read_write_ports << std::endl;
    std::cout << "  Technology: " << sram_config_.tech_node_nm << " nm" << std::endl;
    // 1.11.56 (audit D054): nanoseconds, which is what CACTI reported.
    std::cout << "  Access Time: " << sram_config_.access_time_ns << " ns" << std::endl;
    std::cout << "  Area: " << area_mm2_ << " mm^2" << std::endl;
    std::cout << "  Read Energy: " << read_energy_ << " nJ" << std::endl;
    std::cout << "  Write Energy: " << write_energy_ << " nJ" << std::endl;
    std::cout << "  Leakage Power: " << leakage_power_ << " W" << std::endl;

    // Initialize SRAM architecture with inner-bank timing
    // PRIORITY: Extract from CACTI if available, otherwise use factory defaults
#ifdef HAVE_CACTI
    if (cacti_wrapper_ && cacti_wrapper_->isValid()) {
        // Extract architecture directly from CACTI results (PREFERRED!)
        sram_arch_ = memory::extractSRAMArchitecture(*cacti_wrapper_,
            "SRAM-CACTI-Extracted", 3.0);

        if (sram_arch_) {
            std::cout << "[SRAMModel] Architecture EXTRACTED from CACTI 7.0" << std::endl;
        } else {
            std::cerr << "[SRAMModel] CACTI extraction failed, using factory defaults" << std::endl;
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
    if (!sram_arch_) {
        throw std::runtime_error(
            "[SRAMModel] CACTI characterization failed and there is no fallback. "
            "The hand-written default specs were removed in 1.11.24 because "
            "they were unsourced and indistinguishable from tool output. "
            "Fix the CACTI configuration rather than pricing this run from "
            "invented numbers.");
    }

    std::cout << "[SRAMModel] Inner-bank datapath latency: "
              << sram_arch_->timing.inner_bank.getTotalInnerBank() << " ns" << std::endl;
    std::cout << "[SRAMModel] Architecture source: "
              << sram_arch_->timing.inner_bank.source << std::endl;

    std::cout << "[SRAMModel] Initialization complete" << std::endl;
}

void SRAMModel::loadConfig(const std::string& config_path) {
    std::cout << "[SRAMModel] Loading configuration from: " << config_path << std::endl;

    try {
        YAML::Node config = YAML::LoadFile(config_path);

        if (!config["sram"]) {
            std::cout << "[SRAMModel] No 'sram' section found, using default configuration" << std::endl;
            return;
        }

        YAML::Node sram = config["sram"];

        // Load capacity
        if (sram["capacity_mb"]) {
            uint64_t capacity_mb = sram["capacity_mb"].as<uint64_t>();
            sram_config_.capacity = capacity_mb * 1024 * 1024;
        }

        // Load organization
        if (sram["organization"]) {
            auto org = sram["organization"];
            if (org["banks"]) {
                sram_config_.banks = org["banks"].as<uint32_t>();
            }
            if (org["read_write_ports"]) {
                sram_config_.read_write_ports = org["read_write_ports"].as<uint32_t>();
            }
            if (org["read_ports"]) {
                sram_config_.read_ports = org["read_ports"].as<uint32_t>();
            }
            if (org["write_ports"]) {
                sram_config_.write_ports = org["write_ports"].as<uint32_t>();
            }
        }

        // Load cache configuration
        if (sram["cache"]) {
            auto cache = sram["cache"];
            if (cache["line_size_bytes"]) {
                sram_config_.line_size = cache["line_size_bytes"].as<uint32_t>();
            }
            if (cache["associativity"]) {
                sram_config_.associativity = cache["associativity"].as<uint32_t>();
            }
        }

        // Load timing
        if (sram["timing"]) {
            auto timing = sram["timing"];
            /* 1.11.56 (audit D054): the YAML knob is named in CYCLES but this
             * model holds TIME and has no clock to convert with, so a cycle
             * count cannot be honoured as written. Prefer an explicit
             * read_latency_ns; accept the legacy cycles key but say once, at
             * the point of use, that it is being read as nanoseconds -- the
             * same 1 GHz the code silently assumed, now disclosed. */
            if (timing["read_latency_ns"]) {
                sram_config_.access_time_ns = timing["read_latency_ns"].as<double>();
            } else if (timing["read_latency_cycles"]) {
                sram_config_.access_time_ns =
                    timing["read_latency_cycles"].as<double>();
                std::cerr << "[SRAMModel] NOTE: memory.timing.read_latency_cycles "
                             "is being read as NANOSECONDS. This model carries no "
                             "clock, so a cycle count cannot be converted; use "
                             "read_latency_ns to state what you mean."
                          << std::endl;
            }
        }

        // Load power parameters
        if (sram["power"]) {
            auto power = sram["power"];
            if (power["tech_node_nm"]) {
                sram_config_.tech_node_nm = power["tech_node_nm"].as<uint32_t>();
            }
            // Load energy values (will be overridden by CACTI if available)
            if (power["read_energy_nj"]) {
                read_energy_ = power["read_energy_nj"].as<double>();
            }
            if (power["write_energy_nj"]) {
                write_energy_ = power["write_energy_nj"].as<double>();
            }
            if (power["leakage_power_mw"]) {
                leakage_power_ = power["leakage_power_mw"].as<double>() / 1000.0;  // Convert mW to W
            }
        }

        std::cout << "[SRAMModel] Successfully loaded configuration from YAML" << std::endl;
        std::cout << "[SRAMModel] Capacity: " << (sram_config_.capacity / 1024) << " KB" << std::endl;

    } catch (const YAML::Exception& e) {
        std::cerr << "[SRAMModel] YAML parsing error: " << e.what() << std::endl;
        std::cerr << "[SRAMModel] Using default configuration" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[SRAMModel] Error loading config: " << e.what() << std::endl;
        std::cerr << "[SRAMModel] Using default configuration" << std::endl;
    }
}

/* 1.11.56 (audit D054): THE ONE PLACE TIME BECOMES CYCLES IN THIS MODEL.
 *
 * MemoryModel's legacy access()/getLatency() return Cycle, and this model is
 * handed no clock -- the simulator drives it through getTierLatencyNs(), which
 * is nanoseconds end to end and never comes through here. So the conversion
 * below is 1 cycle per nanosecond, i.e. exactly the 1 GHz the old code assumed;
 * the difference is that it is stated once, at the boundary that forces it,
 * instead of being baked into the stored field and mislabelled in the log. If
 * this path ever becomes live, give the model a frequency and convert with
 * that -- do not restore the assumption upstream. */
static inline Cycle legacyNsAsCycles(double ns) {
    if (ns <= 0.0) return 0;
    return static_cast<Cycle>(std::ceil(ns));   // 1 GHz: no clock is supplied
}

Cycle SRAMModel::access(const MemoryRequest& req) {
    // SRAM has fixed access time
    Cycle latency = legacyNsAsCycles(sram_config_.access_time_ns);

    // Update statistics and energy
    total_accesses_++;
    if (req.type == MemoryRequestType::READ) {
        total_reads_++;
        // Energy already accumulated per access
    } else if (req.type == MemoryRequestType::WRITE) {
        total_writes_++;
        // Energy already accumulated per access
    }

    // Add to pending requests
    pending_requests_.push(req);

    return latency;
}

bool SRAMModel::canAccept(const MemoryRequest& req) {
    // SRAM typically has smaller queues than DRAM
    const size_t MAX_PENDING_REQUESTS = 16;
    return pending_requests_.size() < MAX_PENDING_REQUESTS;
}

void SRAMModel::tick() {
    current_cycle_++;

    /* 1.11.57 (latent D048): THE PERIODIC RE-QUERY IS DELETED.
     *
     * It read "Re-query CACTI periodically to update energy models based on
     * temperature (temperature changes due to activity can affect leakage)"
     * and then, every 100K cycles, assigned
     * leakage_power_ = cacti_wrapper_->getLeakagePower() / 1000.0 -- the same
     * value initialize() already assigned. CACTIWrapper::getLeakagePower()
     * reads cacti_result_, which is computed once in its initialize() and
     * never recomputed, and no temperature is passed in on this path anyway.
     * So the block asserted a thermal feedback loop that does not exist and
     * performed a no-op to imply it. Invisible twice over: SRAMModel::tick()
     * has no caller, and the assignment could not change a number even if it
     * did. A temperature-dependent leakage model means re-characterizing at a
     * new temperature (setTemperatureK, which feeds SRAMConfig::temperature);
     * it does not mean re-reading a cached scalar. */

    // Process pending requests: latency is accounted for at access() time;
    // tick() just frees capacity by popping one request.
    if (!pending_requests_.empty()) {
        pending_requests_.pop();
    }

    // Leakage power is constant (but scaled by CACTI if available)
}

Cycle SRAMModel::getLatency(MemoryRequestType type) const {
    // SRAM has fixed latency regardless of request type.
    // 1.11.56 (audit D054): see legacyNsAsCycles above.
    return legacyNsAsCycles(sram_config_.access_time_ns);
}

double SRAMModel::getTotalEnergy() const {
    /* 1.11.58 (audit D022/D043): NANOJOULES, as MemoryModel documents.
     *
     * SRAM's per-access members were already nanojoules on both paths (CACTI's
     * getDynamicReadEnergy() and the YAML read_energy_nj key), so the dynamic
     * term was right. The leakage term was not: it added JOULES (watts x
     * seconds) to a nanojoule sum, a term 1e9 too small, under a comment
     * that noted the 1 GHz assumption and not the unit. The three NVM models
     * had the same defect and a unit-confused dynamic term besides; all four
     * are converted together, which is the single gated change memory_model.h
     * asked for rather than a rider on a latent pass.
     *
     * One cycle is one nanosecond here -- the convention legacyNsAsCycles()
     * states, not an estimate of a clock these models do not have -- so
     * leakage in nanojoules is watts x cycles. */
    double dynamic_nj = (total_reads_ * read_energy_) +
                        (total_writes_ * write_energy_);
    double leakage_nj = leakage_power_ * static_cast<double>(current_cycle_);
    return dynamic_nj + leakage_nj;
}

void SRAMModel::printStats() const {
    std::cout << "\n=== SRAM Model Statistics ===" << std::endl;
    std::cout << "Total Cycles: " << current_cycle_ << std::endl;
    std::cout << "Total Accesses: " << total_accesses_ << std::endl;
    std::cout << "Total Reads: " << total_reads_ << std::endl;
    std::cout << "Total Writes: " << total_writes_ << std::endl;

    if (total_accesses_ > 0) {
        double read_ratio = static_cast<double>(total_reads_) / total_accesses_;
        double write_ratio = static_cast<double>(total_writes_) / total_accesses_;
        std::cout << "Read Ratio: " << (read_ratio * 100.0) << "%" << std::endl;
        std::cout << "Write Ratio: " << (write_ratio * 100.0) << "%" << std::endl;
    }

    std::cout << "\nPhysical Characteristics:" << std::endl;
    std::cout << "  Area: " << area_mm2_ << " mm^2" << std::endl;
    std::cout << "  Technology: " << sram_config_.tech_node_nm << " nm" << std::endl;

    std::cout << "\nEnergy Consumption:" << std::endl;
    std::cout << "  Read Energy (per access): " << read_energy_ << " nJ" << std::endl;
    std::cout << "  Write Energy (per access): " << write_energy_ << " nJ" << std::endl;
    std::cout << "  Total Read Energy: " << (total_reads_ * read_energy_) << " nJ" << std::endl;
    std::cout << "  Total Write Energy: " << (total_writes_ * write_energy_) << " nJ" << std::endl;
    std::cout << "  Leakage Power: " << leakage_power_ << " W" << std::endl;
    std::cout << "  Total Energy: " << getTotalEnergy() << " nJ" << std::endl;
    std::cout << "================================\n" << std::endl;
}

void SRAMModel::resetStats() {
    total_reads_ = 0;
    total_writes_ = 0;
    total_accesses_ = 0;
    current_cycle_ = 0;
}

/* 1.11.57 (latent D052): SRAMModel::useFallbackValues() is DELETED -- see the
 * call sites in initialize(). It was the last "typical for 22nm SRAM" literal
 * block on a tool-bound path. */

//=============================================================================
// Inner-Bank Timing Queries (NEW!)
//=============================================================================

double SRAMModel::getSubbankReadLatency() const {
    if (!sram_arch_) return 0.0;
    return sram_arch_->timing.subbank_access_ns;
}

double SRAMModel::getBankReadLatency() const {
    if (!sram_arch_) return 0.0;
    return sram_arch_->timing.bank_access_ns;
}

double SRAMModel::getChipReadLatency() const {
    if (!sram_arch_) return 0.0;
    return sram_arch_->timing.chip_access_ns;
}

double SRAMModel::getInnerBankDatapathLatency() const {
    if (!sram_arch_) return 0.0;
    return sram_arch_->timing.inner_bank.getTotalInnerBank();
}

bool SRAMModel::supportsBankPIM() const {
    // SRAM supports bank-level PIM
    return true;
}

bool SRAMModel::supportsSubbankPIM() const {
    // SRAM supports subbank-level PIM (fast local operations)
    return true;
}


/* 1.11.24: SRAM is NOT DRAM-like -- no bank groups, no ranks, no channels.
 * Reporting those as absent is the point: a tier that does not exist must not
 * be collapsed onto its neighbour or filled from a multiplier. */
double SRAMModel::getTierLatencyNs(Tier tier, Op op) const {
    if (op != Op::READ && op != Op::WRITE) return -1.0;
    switch (tier) {
        case Tier::SUBARRAY: return getSubbankReadLatency();
        case Tier::BANK:     return getBankReadLatency();
        case Tier::CHIP:     return getChipReadLatency();
        default:             return -1.0;
    }
}
bool SRAMModel::hasTier(Tier tier) const {
    return tier == Tier::SUBARRAY || tier == Tier::BANK || tier == Tier::CHIP;
}
/* 1.11.73: the one tier below the bank, from CACTI; -1 = not sourceable. */
int SRAMModel::l0UnitsPerBank() const {
    if (!sram_arch_) return -1;
    /* PIMID's SRAM "bank" is the whole per-bank array CACTI is run on (its
     * bank tier latency is getAccessTime() of that array). CACTI splits that
     * array into `banks` independent banks (this model's configured split,
     * 8), each of which delivers the full out_w word from one line of mats
     * -- so the subbanks below the PIMID bank are CACTI banks x subbanks per
     * CACTI bank. The split itself is a configured input, not a CACTI
     * result; see the 1.11.73 ledger's open item on the SRAM array mode. */
    const int per_cacti_bank = sram_arch_->organization.subbanks_per_bank;
    const int cacti_banks    = sram_arch_->organization.banks_per_chip;
    if (per_cacti_bank <= 0 || cacti_banks <= 0) return -1;
    return per_cacti_bank * cacti_banks;
}
int SRAMModel::l0WidthBits() const {
    if (!sram_arch_) return -1;
    const int v = sram_arch_->datapath.subbank_io_bits;
    return (v > 0) ? v : -1;
}
double SRAMModel::l0BandwidthGBs() const {
    if (!sram_arch_) return -1.0;
    const int w = sram_arch_->datapath.subbank_io_bits;
    const double cyc = sram_arch_->timing.cycle_time_ns;   // CACTI random cycle time
    if (w <= 0 || cyc <= 0.0) return -1.0;
    return (w / 8.0) / cyc;   // bytes per ns == GB/s
}
std::string SRAMModel::tierLatencySource(Tier tier, Op op) const {
    if (getTierLatencyNs(tier, op) < 0.0) return "";
    switch (tier) {
        case Tier::SUBARRAY: return "CACTI component delays (the subbank, one tier below the bank)";
        case Tier::BANK:     return "CACTI getAccessTime";
        case Tier::CHIP:     return "CACTI getAccessTime + configured net hop";
        default:             return "";
    }
}


/* 1.11.25: characterize the array the RUN configures, not the model's
 * compiled-in default. Must be called before initialize(). */
void SRAMModel::setArrayCapacityBytes(uint64_t bytes) {
    if (bytes > 0) sram_config_.capacity = bytes;
}


/* 1.11.25: characterize the access the RUN performs. Must precede
 * initialize(). 0 keeps the model default. */
/* 1.11.51 (L70): the run's node, replacing the compiled-in default.
 * Must precede initialize(). */
/* 1.11.52 (D055): the run's temperature, replacing the hardcoded 350 K in
 * the tool query below. Must precede initialize(). */
void SRAMModel::setTemperatureK(int k) {
    if (k > 0) sram_config_.temperature_k = static_cast<uint32_t>(k);
}

void SRAMModel::setTechNodeNm(int nm) {
    if (nm > 0) sram_config_.tech_node_nm = nm;
}

void SRAMModel::setAccessWidthBits(uint32_t bits) {
    if (bits >= 8 && bits <= 1024) access_width_bits_ = bits;
}


/* 1.11.57 (latent D053): the bandwidth this model can source is NONE.
 * `bandwidth_ = capacity_ * 2` was an invented bytes/s figure -- no tool, no
 * measurement, no unit stated -- answering the same MemoryModel::getBandwidth()
 * that DRAMModel answers from Ramulator's own number. It reached no result
 * only because nothing calls getBandwidth() on these objects today. 0 is the
 * absent value; the note fires once per process so a future caller cannot read
 * the 0 as a measured zero. */
uint64_t SRAMModel::getBandwidth() const {
    static bool announced = false;
    if (!announced) {
        announced = true;
        std::cerr << "[SRAMModel] getBandwidth() returns 0 = NOT SOURCED. This model "
                     "characterizes latency, energy and area through its tool; "
                     "it has no sustained-bandwidth model, and the literal that "
                     "used to stand here (capacity_ * 2) was fabricated. Compose a "
                     "bandwidth from the tier latency and the access width at "
                     "the call site, or add a sourced model here."
                  << std::endl;
    }
    return 0;
}

} // namespace pimid
