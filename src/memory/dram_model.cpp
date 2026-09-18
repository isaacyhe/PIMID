#include "memory/dram_model.h"
#include "memory/ramulator_wrapper.h"
#include "memory/architecture_extractor.h"
#include "config/config_parser.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>

namespace pimid {

//=============================================================================
// DRAMModel Implementation (Ramulator Integration)
//=============================================================================

/* 1.11.52 (D024): one name for the technology, used for the base class, the
 * Ramulator wrapper and the defaults below. */
static const char* dramTechName(MemoryTechnology t) {
    switch (t) {
        case MemoryTechnology::DDR3:   return "DDR3";
        case MemoryTechnology::DDR5:   return "DDR5";
        case MemoryTechnology::LPDDR5: return "LPDDR5";
        case MemoryTechnology::GDDR6:  return "GDDR6";
        case MemoryTechnology::HBM2:   return "HBM2";
        case MemoryTechnology::HBM3:   return "HBM3";
        default:                       return "DDR4";
    }
}

/* 1.11.57 (latent D023): the per-access array energy for THIS model's part,
 * asked of the tools this file already holds instead of restated as a literal.
 *
 * Order of authority: the Ramulator wrapper's intensive IDD path (per-64 B,
 * per technology, whole-rank basis) first; the architecture object's bank
 * energy second, for the case where the wrapper is absent -- which is the only
 * case in which the caller of these helpers runs at all. If neither exists
 * there is nothing to charge and the model says so once rather than inventing
 * a DDR4 number for whatever part is being simulated.
 *
 * The write helper deliberately does NOT apply a read-to-write ratio of its
 * own: the IDD path derives the write term from IDD4W, and when it is absent
 * this file has no sourced ratio to substitute -- which is exactly the mistake
 * the old 2.5/3.0 pair encoded, a 1.2x asserted for every technology. */
static double dramPerAccessNJ(const RamulatorWrapper* ram,
                              const memory::DRAMArchitectureV2* arch,
                              bool is_write) {
    if (ram) {
        double e = is_write ? ram->getArrayWriteEnergyNJ()
                            : ram->getArrayReadEnergyNJ();
        if (e > 0.0) return e;
    }
    if (arch && arch->energy.bank_energy_pJ > 0.0) {
        const double BYTES_PER_ACCESS = 64.0;
        return arch->energy.bank_energy_pJ * BYTES_PER_ACCESS / 1000.0;
    }
    static bool warned = false;
    if (!warned) {
        warned = true;
        std::cerr << "[DRAMModel] WARNING: no Ramulator wrapper and no DRAM "
                     "architecture object, so per-access array energy cannot "
                     "be sourced. Charging 0 nJ per access rather than "
                     "substituting a literal -- the energy totals from this "
                     "model are incomplete, not small." << std::endl;
    }
    return 0.0;
}

DRAMModel::DRAMModel(const std::string& config_path, MemoryTechnology tech)
    : MemoryModel(tech, config_path)
    , total_reads_(0)
    , total_writes_(0)
    , row_hits_(0)
    , row_misses_(0)
    , row_conflicts_(0)
    , read_energy_(0.0)
    , write_energy_(0.0)
    , leakage_power_(0.0)
    , activation_energy_(0.0)
    , precharge_energy_(0.0)
    , current_cycle_(0) {

    // Initialize default DRAM configuration
    /* 1.11.52 (D024): the standard is the RUN'S technology.
     * 1.11.57 (audit C010): the claim that followed -- "the literals below are
     * overwritten by the architecture extraction in initialize()" -- was not
     * true of THESE fields. The extraction fills dram_arch_; these fields are
     * a separate struct that nothing overwrote, and they were printed. They
     * are still DDR4-2400's shape, and they still stand only until
     * initialize() replaces them from the wrapper and the architecture object;
     * what they are now is a pre-initialisation placeholder, not a
     * description of anything. */
    dram_config_.standard = dramTechName(tech);
    dram_config_.org = "4Gb_x8";
    dram_config_.channels = 1;
    dram_config_.ranks_per_channel = 1;
    dram_config_.banks_per_rank = 8;
    dram_config_.capacity = 4ULL * 1024 * 1024 * 1024; // 4GB
    dram_config_.bandwidth = 25600ULL * 1024 * 1024;    // 25.6 GB/s

    // DDR4-2400 timing parameters (in cycles at 1.2 GHz)
    dram_config_.tCL = 17;   // CAS latency
    dram_config_.tRCD = 17;  // RAS to CAS delay
    dram_config_.tRP = 17;   // Row precharge time
    dram_config_.tRAS = 39;  // Row active time

    // Create Ramulator wrapper instance
    ramulator_instance_ = std::make_unique<RamulatorWrapper>(config_path,
                                                             dramTechName(tech));
}

DRAMModel::~DRAMModel() {
    // Destructor defined here where RamulatorWrapper is complete
}

void DRAMModel::initialize() {
    std::cout << "[DRAMModel] Initializing DRAM model with Ramulator2..." << std::endl;
    loadConfig(config_path_);

    // Initialize Ramulator wrapper FIRST
    if (ramulator_instance_) {
        ramulator_instance_->initialize();
        capacity_ = ramulator_instance_->getCapacity();
        bandwidth_ = ramulator_instance_->getBandwidth();
        std::cout << "[DRAMModel] Using Ramulator2 for cycle-accurate DRAM simulation" << std::endl;
    }

    // Initialize DRAM architecture with inner-bank timing
    // PRIORITY: Extract from Ramulator if available, otherwise use factory defaults
    if (ramulator_instance_) {
        // Extract architecture directly from Ramulator (PREFERRED!)
        dram_arch_ = memory::extractDRAMArchitecture(*ramulator_instance_,
            "DRAM-Ramulator-Extracted");

        if (dram_arch_) {
            std::cout << "[DRAMModel] Architecture EXTRACTED from Ramulator2" << std::endl;
        } else {
            std::cerr << "[DRAMModel] Ramulator extraction failed, using factory defaults" << std::endl;
        }
    }

    // Fallback to factory defaults if extraction failed
    if (!dram_arch_) {
        // Select factory architecture based on config standard
        if (dram_config_.standard == "DDR5" || dram_config_.standard == "DDR5-4800") {
            dram_arch_ = memory::createDDR5_4800_Verified();
            std::cout << "[DRAMModel] Using factory DDR5-4800 specs (hard-coded)" << std::endl;
        } else if (dram_config_.standard == "HBM2") {
            dram_arch_ = memory::createHBM2_Verified();
            std::cout << "[DRAMModel] Using factory HBM2 specs (hard-coded)" << std::endl;
        } else if (dram_config_.standard == "HBM3") {
            dram_arch_ = memory::createHBM3_Verified();
            std::cout << "[DRAMModel] Using factory HBM3 specs (hard-coded)" << std::endl;
        } else {
            // Default to DDR4-2400
            dram_arch_ = memory::createDDR4_2400_Verified();
            std::cout << "[DRAMModel] Using factory DDR4-2400 specs (hard-coded)" << std::endl;
        }
    }

    // Print architecture details
    if (dram_arch_) {
        std::cout << "[DRAMModel] Inner-bank datapath delay: "
                  << dram_arch_->timing.inner_bank.getTotalInnerBankDelay() << " ns" << std::endl;
        std::cout << "[DRAMModel] Architecture source: "
                  << dram_arch_->timing.inner_bank.source << std::endl;
    }

    /* 1.11.57 (audit C010): the block below used to print the CONSTRUCTOR'S
     * DDR4-2400 literals under the run's own technology name.
     *
     * What was wrong: every field here is set once in the constructor and
     * overwritten only by loadConfig(), which this model is always called with
     * an empty path (createMemoryModel(mt, "")), so the parse fails, warns and
     * returns without touching anything. 1.11.52's D024 fix made `standard`
     * follow the technology, and that is what turned a wrong block into a
     * MISLEADING one: an HBM3 run printed "Standard: HBM3" above
     * "Organization: 4Gb_x8 / Channels: 1 / Banks/Rank: 8 / Bandwidth: 25 GB/s
     * / tCL: 17 cycles" -- the right part's name beside another part's machine,
     * a few lines from a wrapper reporting 16 channels and 819 GB/s.
     *
     * Two unit defects went with it. The capacity and bandwidth were stored in
     * BYTES and printed over 1024^3, so the comment's "25.6 GB/s" printed as
     * 25.0: a decimal MB/s figure divided as if it were binary. And tCL/tRCD/
     * tRP/tRAS were printed as "cycles" with no clock named, which for a part
     * whose core clock is not 1.2 GHz is not a translatable number at all.
     *
     * The fields are populated from the wrapper and the architecture object
     * that this model already holds, each line says which of the two it came
     * from, and the JEDEC timings are printed in ns -- the basis they are
     * specified and stored in -- with the cycle count beside them at the clock
     * it belongs to. */
    if (ramulator_instance_) {
        dram_config_.channels          = ramulator_instance_->getNumChannels();
        dram_config_.ranks_per_channel = ramulator_instance_->getRanksPerChannel();
        dram_config_.banks_per_rank    = ramulator_instance_->getBanksPerBankGroup() *
                                         ramulator_instance_->getBankGroupsPerChip();
        dram_config_.capacity          = ramulator_instance_->getCapacity();
        // getBandwidth() is MB/s, decimal. Store bytes/s on the same decimal
        // basis so the print below cannot re-introduce the 1024^3 confusion.
        dram_config_.bandwidth         = ramulator_instance_->getBandwidth() * 1000000ULL;
        /* 1.11.63 (R6-1): NAME THE UNIT chip_size_mb ACTUALLY COUNTS, which is
         * not the same unit for both families.
         *
         * This line multiplied chip_size_mb by chips_per_rank and called the
         * product an organisation. For the DDR family that is right -- both
         * fields count DEVICES, and 1024 MB x 8 = 8 GiB is the capacity line
         * three rows below. For HBM it multiplied a CORE-DIE capacity by a
         * CHANNEL count (the two fields deliberately count different units;
         * see the note on the struct), so an HBM3 run printed an implied
         * capacity 2x its own capacity line, in both directions across the
         * R6-1 correction: 2048 x 16 = 32 GiB beside 4 GiB before, 1024 x 16 =
         * 16 GiB beside 8 GiB after. The arithmetic that DOES close for HBM is
         * core dies, which getPresetDiesPerStack() already answers on the
         * preset's own two-channels-per-die relation: 1024 MB x 8 dies = 8 GiB
         * = the capacity line. Printed per family so neither is described in
         * the other's words. */
        const int hbm_dies = ramulator_instance_->getPresetDiesPerStack();
        if (hbm_dies > 0) {
            dram_config_.org = std::to_string(ramulator_instance_->getChipSizeMB()) +
                               " MB/core die x " + std::to_string(hbm_dies) +
                               " dies/stack, " +
                               std::to_string(ramulator_instance_->getChannelDataBits()) +
                               "-bit channel x " +
                               std::to_string(ramulator_instance_->getNumChannels());
        } else {
            dram_config_.org = std::to_string(ramulator_instance_->getChipSizeMB()) +
                               " MB/device x " +
                               std::to_string(ramulator_instance_->getChipsPerRank()) +
                               " devices/rank, " +
                               std::to_string(ramulator_instance_->getChipIOBits()) +
                               "-bit device";
        }
    }
    if (dram_arch_) {
        const double core_ghz = dram_arch_->timing.clock_freq_mhz / 1000.0;
        auto toCycles = [core_ghz](double ns) -> Cycle {
            return core_ghz > 0.0 ? static_cast<Cycle>(std::ceil(ns * core_ghz)) : 0;
        };
        dram_config_.tCL  = toCycles(dram_arch_->timing.tCAS_ns);
        dram_config_.tRCD = toCycles(dram_arch_->timing.tRCD_ns);
        dram_config_.tRP  = toCycles(dram_arch_->timing.tRP_ns);
        dram_config_.tRAS = toCycles(dram_arch_->timing.tRAS_ns);
    }

    std::cout << "[DRAMModel] Configuration:" << std::endl;
    std::cout << "  Standard: " << dram_config_.standard << std::endl;
    std::cout << "  Organization: " << dram_config_.org << std::endl;
    std::cout << "  Channels: " << dram_config_.channels
              << " (Ramulator preset)" << std::endl;
    std::cout << "  Ranks/Channel: " << dram_config_.ranks_per_channel
              << " (architecture object)" << std::endl;
    std::cout << "  Banks/Rank: " << dram_config_.banks_per_rank
              << " (architecture object: banks/group x groups/chip)" << std::endl;
    std::cout << "  Capacity: " << (dram_config_.capacity / (1024.0 * 1024 * 1024))
              << " GiB (Ramulator preset)" << std::endl;
    std::cout << "  Bandwidth: " << (dram_config_.bandwidth / 1.0e9)
              << " GB/s decimal, aggregate (Ramulator preset)" << std::endl;
    if (dram_arch_) {
        std::cout << "  JEDEC timings (architecture object, ns; cycles at the "
                  << dram_arch_->timing.clock_freq_mhz << " MHz core clock):"
                  << std::endl;
        std::cout << "    tCL:  " << dram_arch_->timing.tCAS_ns << " ns = "
                  << dram_config_.tCL << " cycles" << std::endl;
        std::cout << "    tRCD: " << dram_arch_->timing.tRCD_ns << " ns = "
                  << dram_config_.tRCD << " cycles" << std::endl;
        std::cout << "    tRP:  " << dram_arch_->timing.tRP_ns << " ns = "
                  << dram_config_.tRP << " cycles" << std::endl;
        std::cout << "    tRAS: " << dram_arch_->timing.tRAS_ns << " ns = "
                  << dram_config_.tRAS << " cycles" << std::endl;
    }
    std::cout << "[DRAMModel] Initialization complete" << std::endl;
}

void DRAMModel::loadConfig(const std::string& config_path) {
    /* 1.11.63 (gate 1173C): every co-sim run reached this with an EMPTY
     * path -- no caller passes an override file -- and printed "Loading
     * configuration from: " followed by a YAML parse error and "using
     * defaults". Three alarming lines describing a non-event: this file is
     * an OPTIONAL override layer, and with no file the configuration is
     * what initialize() already extracted from the Ramulator presets and
     * the architecture object. Say that, once, instead of failing to parse
     * the empty string on every run. A NON-empty path that fails to parse
     * is still a real warning. */
    if (config_path.empty()) {
        std::cout << "[DRAMModel] No override file; Ramulator presets + "
                     "architecture object supply the configuration."
                  << std::endl;
        return;
    }
    std::cout << "[DRAMModel] Loading configuration from: " << config_path << std::endl;

    // Parse YAML configuration file
    config::ConfigParser parser;
    std::map<std::string, std::string> config;

    if (!parser.parseFile(config_path, config)) {
        std::cerr << "[DRAMModel] Warning: Failed to parse config file, using defaults" << std::endl;
        return;
    }

    // Extract DRAM standard and speed
    if (config.find("dram.standard") != config.end()) {
        dram_config_.standard = config["dram.standard"];
    }
    if (config.find("dram.speed_grade") != config.end()) {
        // Speed grade is stored but primarily managed by Ramulator
    }

    // Extract organization parameters
    if (config.find("dram.organization.channels") != config.end()) {
        try {
            dram_config_.channels = std::stoi(config["dram.organization.channels"]);
        } catch (...) {
            std::cerr << "[DRAMModel] Warning: Invalid channels value" << std::endl;
        }
    }

    if (config.find("dram.organization.ranks_per_channel") != config.end()) {
        try {
            dram_config_.ranks_per_channel = std::stoi(config["dram.organization.ranks_per_channel"]);
        } catch (...) {
            std::cerr << "[DRAMModel] Warning: Invalid ranks_per_channel value" << std::endl;
        }
    }

    if (config.find("dram.organization.banks_per_chip") != config.end()) {
        try {
            dram_config_.banks_per_rank = std::stoi(config["dram.organization.banks_per_chip"]);
        } catch (...) {
            std::cerr << "[DRAMModel] Warning: Invalid banks_per_chip value" << std::endl;
        }
    }

    // Extract timing parameters (all in memory clock cycles)
    if (config.find("dram.timing.tCL") != config.end()) {
        try {
            dram_config_.tCL = std::stoi(config["dram.timing.tCL"]);
        } catch (...) {
            std::cerr << "[DRAMModel] Warning: Invalid tCL value" << std::endl;
        }
    }

    if (config.find("dram.timing.tRCD") != config.end()) {
        try {
            dram_config_.tRCD = std::stoi(config["dram.timing.tRCD"]);
        } catch (...) {
            std::cerr << "[DRAMModel] Warning: Invalid tRCD value" << std::endl;
        }
    }

    if (config.find("dram.timing.tRP") != config.end()) {
        try {
            dram_config_.tRP = std::stoi(config["dram.timing.tRP"]);
        } catch (...) {
            std::cerr << "[DRAMModel] Warning: Invalid tRP value" << std::endl;
        }
    }

    if (config.find("dram.timing.tRAS") != config.end()) {
        try {
            dram_config_.tRAS = std::stoi(config["dram.timing.tRAS"]);
        } catch (...) {
            std::cerr << "[DRAMModel] Warning: Invalid tRAS value" << std::endl;
        }
    }

    // Calculate capacity based on organization
    // This is a simplified calculation; Ramulator will provide exact values
    if (config.find("dram.organization.rows_per_bank") != config.end() &&
        config.find("dram.organization.columns_per_row") != config.end() &&
        config.find("dram.organization.chips_per_rank") != config.end()) {
        try {
            uint64_t rows = std::stoull(config["dram.organization.rows_per_bank"]);
            uint64_t cols = std::stoull(config["dram.organization.columns_per_row"]);
            uint64_t chips = std::stoull(config["dram.organization.chips_per_rank"]);

            // Capacity = channels * ranks * banks * rows * cols * chip_width (bytes)
            // Assuming 8-bit chip width (x8 device)
            dram_config_.capacity = dram_config_.channels *
                                   dram_config_.ranks_per_channel *
                                   dram_config_.banks_per_rank *
                                   rows * cols * chips;
        } catch (...) {
            std::cerr << "[DRAMModel] Warning: Could not calculate capacity from organization" << std::endl;
        }
    }

    // Extract bandwidth if specified, otherwise calculate from speed grade
    if (config.find("channels.bandwidth_per_channel_gbs") != config.end()) {
        try {
            double bw_per_channel = std::stod(config["channels.bandwidth_per_channel_gbs"]);
            /* 1.11.57 (audit C010): DECIMAL. A DRAM bandwidth in GB/s is a
             * decimal figure -- it is a transfer rate times a bus width -- and
             * multiplying it by 1024^3 stored 7.4% more bytes per second than
             * the user asked for, which the printed line then divided back out
             * by 1024^3 and hid. One basis, stated. */
            dram_config_.bandwidth = static_cast<uint64_t>(
                bw_per_channel * dram_config_.channels * 1.0e9
            );
        } catch (...) {
            std::cerr << "[DRAMModel] Warning: Invalid bandwidth value" << std::endl;
        }
    }

    // Row buffer policy
    if (config.find("dram.row_buffer.policy") != config.end()) {
        std::string policy = config["dram.row_buffer.policy"];
        std::cout << "[DRAMModel] Row buffer policy: " << policy << std::endl;
        // Row buffer policy would be passed to Ramulator
    }

    // Ramulator-specific settings
    if (config.find("dram.ramulator.config_file") != config.end()) {
        std::string ramulator_config = config["dram.ramulator.config_file"];
        std::cout << "[DRAMModel] Ramulator config: " << ramulator_config << std::endl;
        // Pass to Ramulator wrapper during initialization
    }

    std::cout << "[DRAMModel] Configuration loaded successfully" << std::endl;
    std::cout << "[DRAMModel] DRAM Standard: " << dram_config_.standard << std::endl;
    std::cout << "[DRAMModel] Channels: " << dram_config_.channels << std::endl;
    std::cout << "[DRAMModel] Ranks per channel: " << dram_config_.ranks_per_channel << std::endl;
    std::cout << "[DRAMModel] Banks per rank: " << dram_config_.banks_per_rank << std::endl;
}

Cycle DRAMModel::access(const MemoryRequest& req) {
    // If Ramulator is available, send request to it
    if (ramulator_instance_) {
        // Send request to Ramulator
        bool accepted = ramulator_instance_->send(req.addr, req.type);

        if (!accepted) {
            // Request queue is full, return high latency penalty
            return 1000;
        }

        // Ramulator will track the request
        // Return estimated latency
        return ramulator_instance_->getAverageLatency();
    }

    // Fallback: Calculate access latency
    Cycle latency = calculateLatency(req);

    /* 1.11.57 (latent D023): ASK THE MODEL, DO NOT RESTATE IT. These two lines
     * were "read_energy_ += 2.5;  // nJ per read (typical DDR4)" and
     * "write_energy_ += 3.0;", a THIRD independent set of per-access DRAM
     * energy literals in a tree that already has two: the wrapper's
     * updateEnergyMetrics() (2.5/3.0) and the IDD-derived intensive path in
     * pimid_energy.h. Three sets, one of them technology-blind -- an HBM3 run
     * charged DDR4's numbers here. The wrapper this model owns exposes the
     * per-64 B array energy for the run's own technology; use it, and use the
     * architecture object as the second-best source when the wrapper is
     * absent, which is the only way this branch is reached at all (it runs
     * only when ramulator_instance_ is null, and the constructor always makes
     * one). Nothing could observe the difference because access() has no
     * caller in this tree. */
    /* 1.11.60 (audit round 4, C003): ASSIGNED, not accumulated. read_energy_
     * and write_energy_ are the MemoryModel interface's PER-ACCESS fields --
     * see the contract paragraph in memory_model.h -- and this was the second
     * of the two places that made them a running total instead. Per-access
     * energy does not depend on how many accesses have happened, so the
     * assignment is the whole of it; the counters beside it are what scale. */
    if (req.type == MemoryRequestType::READ) {
        total_reads_++;
        read_energy_ = dramPerAccessNJ(ramulator_instance_.get(),
                                       dram_arch_.get(), false);
    } else if (req.type == MemoryRequestType::WRITE) {
        total_writes_++;
        write_energy_ = dramPerAccessNJ(ramulator_instance_.get(),
                                        dram_arch_.get(), true);
    }

    // Update row buffer state
    updateRowBuffer(req.addr);

    // Add to pending requests queue
    pending_requests_.push(req);

    return latency;
}

bool DRAMModel::canAccept(const MemoryRequest& req) {
    // If Ramulator is available, use its queue status
    if (ramulator_instance_) {
        return ramulator_instance_->canAccept();
    }

    // Fallback: Check if request queue has space
    const size_t MAX_PENDING_REQUESTS = 64;
    return pending_requests_.size() < MAX_PENDING_REQUESTS;
}

void DRAMModel::tick() {
    current_cycle_++;

    // Tick Ramulator if available
    if (ramulator_instance_) {
        ramulator_instance_->tick();

        // Update statistics from Ramulator
        total_reads_ = ramulator_instance_->getTotalReads();
        total_writes_ = ramulator_instance_->getTotalWrites();
        /* 1.11.57 (latent D009): the three row counters are no longer copied
         * from the wrapper, because the wrapper's copies never existed as
         * measurements -- they were declared, zeroed in the constructor,
         * zeroed again in resetStats() and incremented nowhere, so these three
         * lines OVERWROTE this model's own row counters (which calculateLatency
         * does increment) with three structural zeroes on every tick. The
         * wrapper's counters and their getters are deleted in 1.11.57; this
         * model keeps its own, which at least count something. */

        /* Update energy metrics.
         * 1.11.60 (audit round 4, C003): THE PER-ACCESS FIGURE, not the
         * wrapper's running total.
         *
         * RamulatorWrapper::getReadEnergy() is total_reads_ x per-access (see
         * updateEnergyMetrics(): cached_read_energy_ = total_reads_ *
         * read_energy_per_access), and copying it into read_energy_ made
         * DRAMModel::getReadEnergy() a CUMULATIVE total under a base-class
         * signature that memory_model.h documents as nanojoules PER ACCESS.
         * 1.11.58 normalised the other three implementations and recorded in
         * that paragraph that all four now obeyed the contract; this one did
         * not, and the paragraph said it did -- so the one line a future
         * polymorphic consumer would rely on was the line that was wrong. Made
         * to obey rather than documented as an exception, because an interface
         * with one exception is an interface nobody can call: a consumer
         * reading these four through MemoryModel* would have seen DRAM's read
         * energy grow without bound over the run while the three NVM models
         * returned a constant, which is exactly the failure the contract
         * exists to prevent.
         *
         * getArrayReadEnergyNJ()/getArrayWriteEnergyNJ() -- reached through
         * dramPerAccessNJ() above -- are the wrapper's own INTENSIVE
         * accessors, documented in ramulator_wrapper.h as the per-64 B figure
         * that does not depend on how many accesses the wrapper has seen. The
         * cumulative total this used to hold is not lost: getTotalEnergy()
         * reconstructs it by multiplying by the counters, which is what a
         * cumulative total is. */
        read_energy_ = dramPerAccessNJ(ramulator_instance_.get(),
                                       dram_arch_.get(), false);
        write_energy_ = dramPerAccessNJ(ramulator_instance_.get(),
                                        dram_arch_.get(), true);
        /* 1.11.57 (latent D021): MILLIWATTS INTO A WATTS FIELD.
         * RamulatorWrapper::getLeakagePower() returns mW (it is derived from
         * the IDD background power, which is a mW quantity, and its own
         * printStats labels it " mW"); leakage_power_ is the MemoryModel
         * interface's WATTS field -- printStats() below prints it as " W" and
         * CompositePowerModel::estimatePower reads it with the comment "in W".
         * Assigning one to the other was a straight 1000x overstatement of
         * memory leakage, invisible because DRAMModel::tick() is never called
         * and CompositePowerModel is never constructed. */
        leakage_power_ = ramulator_instance_->getLeakagePower() / 1000.0;  // mW -> W
        activation_energy_ = ramulator_instance_->getActivationEnergy();
        precharge_energy_ = ramulator_instance_->getPrechargeEnergy();
    } else {
        // Fallback: simple model
        if (!pending_requests_.empty()) {
            pending_requests_.pop();
        }
        /* 1.11.57 (latent D021): the "leakage_power_ = 1.5; // W (typical DDR4
         * idle power)" that stood here was a literal for a quantity the
         * wrapper measures, written into the same field the branch above fills
         * from a tool -- so one variable meant a sourced number or an asserted
         * one depending on which way the branch went, and printStats() could
         * not tell you which. There is no tool on this path at all (the
         * constructor always builds a wrapper, so this branch is unreachable
         * today), and the DRAM architecture object carries no leakage field,
         * so nothing here can source it. Report 0 and say that 0 means
         * unmodelled rather than negligible. */
        static bool warned_leak = false;
        if (!warned_leak) {
            warned_leak = true;
            std::cerr << "[DRAMModel] WARNING: no Ramulator wrapper, so DRAM "
                         "background/leakage power has no source in this model "
                         "and is reported as 0 W -- unmodelled, not negligible."
                      << std::endl;
        }
        leakage_power_ = 0.0;
    }
}

Cycle DRAMModel::getLatency(MemoryRequestType type) const {
    // Base latency depends on request type
    Cycle base_latency = dram_config_.tCL;

    if (type == MemoryRequestType::WRITE) {
        // Write latency is typically similar to read in modern DRAM
        base_latency = dram_config_.tCL;
    } else if (type == MemoryRequestType::ATOMIC) {
        // Atomic operations require read-modify-write
        base_latency = dram_config_.tCL * 2;
    }

    return base_latency;
}

double DRAMModel::getTotalEnergy() const {
    /* 1.11.57 (latent D022): JOULES ADDED TO NANOJOULES. The old line was
     *     double leakage_energy = leakage_power_ * (current_cycle_ / 1e9);
     * with leakage_power_ in WATTS and current_cycle_/1e9 in SECONDS, so the
     * product is JOULES -- and it was summed with read_energy_, write_energy_,
     * activation_energy_ and precharge_energy_, every one of which is
     * NANOJOULES, then returned from a function whose caller prints " nJ".
     * The leakage term was therefore 1e9 times too small: a 1 W part running
     * a million cycles contributed 0.001 to a total measured in nJ. Both
     * halves are fixed here -- the seconds conversion is spelled out against
     * the model's own clock rather than an unstated "Assuming 1 GHz", and the
     * result is converted to nJ before it joins the sum. Invisible because
     * getTotalEnergy() on this model has no reachable caller. */
    double clock_period_ns = 1.0;   // the model's cycle, 1 GHz unless told
    if (dram_arch_ && dram_arch_->timing.clock_freq_mhz > 0.0)
        clock_period_ns = 1000.0 / dram_arch_->timing.clock_freq_mhz;
    const double elapsed_s =
        static_cast<double>(current_cycle_) * clock_period_ns * 1e-9;
    const double leakage_energy_nJ = leakage_power_ * elapsed_s * 1e9;  // J -> nJ
    /* 1.11.60 (audit round 4, C003): the dynamic terms are now a per-access
     * energy times an access count. read_energy_/write_energy_ became the
     * intensive per-access figures the MemoryModel contract asks for, so this
     * function -- which is the ACCUMULATED total, the one member of the four
     * that is meant to grow -- does the accumulating itself. activation_ and
     * precharge_ stay as the wrapper reports them: they are cumulative there,
     * they are not part of the base-class contract, and no counter in this
     * model corresponds to them. */
    return (static_cast<double>(total_reads_) * read_energy_) +
           (static_cast<double>(total_writes_) * write_energy_) +
           activation_energy_ + precharge_energy_ + leakage_energy_nJ;
}

void DRAMModel::printStats() const {
    std::cout << "\n=== DRAM Model Statistics ===" << std::endl;
    std::cout << "Total Cycles: " << current_cycle_ << std::endl;
    std::cout << "Total Reads: " << total_reads_ << std::endl;
    std::cout << "Total Writes: " << total_writes_ << std::endl;
    std::cout << "Row Hits: " << row_hits_ << std::endl;
    std::cout << "Row Misses: " << row_misses_ << std::endl;
    std::cout << "Row Conflicts: " << row_conflicts_ << std::endl;

    /* 1.11.57 (latent D026): GUARD THE DENOMINATOR THAT IS ACTUALLY DIVIDED BY.
     *
     * This tested `total_reads_ + total_writes_ > 0` and then divided by
     * `row_hits_ + row_misses_ + row_conflicts_` -- a different sum entirely.
     * The three row counters are never incremented anywhere in this model, so
     * the instant there was any traffic at all the guard passed and the
     * division was 0/0: a NaN printed as "Row Hit Rate: nan%", or worse, -nan.
     * Invisible only because printStats() has no caller. A rate whose
     * denominator is zero is not zero and not 100% -- it is unmeasured, so say
     * that rather than print a number. */
    const uint64_t row_events = row_hits_ + row_misses_ + row_conflicts_;
    if (row_events > 0) {
        double row_hit_rate = static_cast<double>(row_hits_) /
                              static_cast<double>(row_events);
        std::cout << "Row Hit Rate: " << (row_hit_rate * 100.0) << "%" << std::endl;
    } else {
        std::cout << "Row Hit Rate: not measured (no row events recorded)"
                  << std::endl;
    }

    std::cout << "\nEnergy Consumption:" << std::endl;
    /* 1.11.60 (audit round 4, C003): these two are per-access now and the
     * labels say so; the running totals they used to hold are printed beside
     * them, derived from the counters. */
    std::cout << "  Read Energy (per access): " << read_energy_ << " nJ" << std::endl;
    std::cout << "  Write Energy (per access): " << write_energy_ << " nJ" << std::endl;
    std::cout << "  Total Read Energy: "
              << (static_cast<double>(total_reads_) * read_energy_) << " nJ" << std::endl;
    std::cout << "  Total Write Energy: "
              << (static_cast<double>(total_writes_) * write_energy_) << " nJ" << std::endl;
    std::cout << "  Activation Energy: " << activation_energy_ << " nJ (cumulative)" << std::endl;
    std::cout << "  Precharge Energy: " << precharge_energy_ << " nJ (cumulative)" << std::endl;
    std::cout << "  Leakage Power: " << leakage_power_ << " W" << std::endl;
    std::cout << "  Total Energy: " << getTotalEnergy() << " nJ" << std::endl;
    std::cout << "================================\n" << std::endl;
}

void DRAMModel::resetStats() {
    total_reads_ = 0;
    total_writes_ = 0;
    row_hits_ = 0;
    row_misses_ = 0;
    row_conflicts_ = 0;
    /* 1.11.60 (audit round 4, C003): read_energy_/write_energy_ are NOT
     * zeroed any more. They are the per-access figures the contract asks for,
     * i.e. a property of the part, not a statistic -- zeroing them was the
     * accumulating-total behaviour the finding named, and it is what the three
     * NVM models' resetStats() already declines to do. The cumulative terms
     * (activation, precharge) are statistics and are still cleared. */
    activation_energy_ = 0.0;
    precharge_energy_ = 0.0;
    current_cycle_ = 0;
}

//=============================================================================
// Private Helper Functions
//=============================================================================

Cycle DRAMModel::calculateLatency(const MemoryRequest& req) {
    Cycle latency;

    // Check row buffer status
    if (isRowHit(req.addr)) {
        // Row buffer hit: only CAS latency
        latency = dram_config_.tCL;
        row_hits_++;
    } else {
        // Row buffer miss or conflict
        auto it = row_buffer_state_.find(req.addr & ~0xFFFULL); // Same row?

        if (it != row_buffer_state_.end()) {
            // Row conflict: precharge + activate + CAS
            latency = dram_config_.tRP + dram_config_.tRCD + dram_config_.tCL;
            row_conflicts_++;
            /* 1.11.57 (latent D023): the "activation_energy_ += 5.0; // nJ for
             * activation" and "precharge_energy_ += 3.0;" that stood on these
             * lines were a FOURTH set of invented DRAM energy literals, and
             * they were also a double charge: access() has already added the
             * full per-64 B array energy for this request, and that quantity
             * -- whether it comes from the IDD path or from the architecture
             * object's bank energy -- already contains the activate and
             * precharge share (Micron TN-41-01's row energy is exactly
             * ACT+PRE plus the burst). Charging 8 nJ more per row miss on top
             * of it roughly quadrupled the energy of a missing access, with
             * numbers that matched no technology in the tree. The row-miss
             * COST is where it belongs, in the latency above; the energy split
             * is reported per line item by the wrapper's own
             * getActivationEnergy()/getPrechargeEnergy(), which derive it from
             * the same bank energy instead of asserting it. */
        } else {
            // Row miss: activate + CAS
            latency = dram_config_.tRCD + dram_config_.tCL;
            row_misses_++;
        }
    }

    // Add queuing delay (simplified model)
    if (pending_requests_.size() > 0) {
        latency += pending_requests_.size();
    }

    return latency;
}

void DRAMModel::updateRowBuffer(Address addr) {
    // Extract row address (simplified: upper bits)
    Address row_addr = addr & ~0xFFFULL; // Assuming 4KB rows

    // Update row buffer state for this bank
    row_buffer_state_[row_addr] = current_cycle_;
}

bool DRAMModel::isRowHit(Address addr) const {
    // Check if this address is in the currently open row
    Address row_addr = addr & ~0xFFFULL;
    auto it = row_buffer_state_.find(row_addr);

    if (it == row_buffer_state_.end()) {
        return false;
    }

    // Check if row is still open (not closed due to timeout)
    Cycle row_open_time = current_cycle_ - it->second;
    const Cycle MAX_ROW_OPEN_TIME = 1000; // Cycles

    return row_open_time < MAX_ROW_OPEN_TIME;
}

//=============================================================================
// Inner-Bank Timing Queries (NEW!)
//=============================================================================

double DRAMModel::getSubarrayAccessLatency() const {
    if (!dram_arch_) return 0.0;
    return dram_arch_->timing.subarray_access_ns;
}

double DRAMModel::getBankAccessLatency() const {
    if (!dram_arch_) return 0.0;
    return dram_arch_->timing.bank_access_ns;
}

double DRAMModel::getChipAccessLatency() const {
    if (!dram_arch_) return 0.0;
    return dram_arch_->timing.chip_access_ns;
}

double DRAMModel::getInnerBankDatapathDelay() const {
    if (!dram_arch_) return 0.0;
    return dram_arch_->timing.inner_bank.getTotalInnerBankDelay();
}

bool DRAMModel::supportsBankPIM() const {
    // Bank-level PIM is supported but limited by bank serialization
    // Check if architecture indicates reasonable bank bandwidth
    if (!dram_arch_) return false;
    // 1.11.57 (audit C003): derived on read from the serialization width and
    // the core clock, so this test follows a speed-bin change like everything
    // else on that rung.
    return dram_arch_->getBankEffectiveBW() > 0.5;  // > 0.5 GB/s
}

bool DRAMModel::supportsSubarrayPIM() const {
    // Subarray-level PIM is always supported (direct row buffer access)
    return true;
}


/* 1.11.24: the memory plugin contract. DRAM is the DRAM-like case: it has
 * every tier. Values come from RamulatorWrapper, which since 1.11.23 composes
 * them from the JEDEC timing rather than returning a hand-written literal. */
double DRAMModel::getTierLatencyNs(Tier tier, Op op) const {
    if (op != Op::READ && op != Op::WRITE) return -1.0;   // no SET/RESET in DRAM
    switch (tier) {
        case Tier::SUBARRAY: return getSubarrayAccessLatency();
        case Tier::BANK:     return getBankAccessLatency();
        case Tier::CHIP:     return getChipAccessLatency();
        /* 1.11.57 (latent D025): BANKGROUP, RANK and CHANNEL exist in DRAM and
         * this model cannot price them, which are two different statements --
         * see hasTier() below, where the contract now says which question each
         * predicate answers. The wrapper accessors that would supply them
         * (getBankGroupAccessLatency, and the getChannelAccessLatency that
         * 1.11.57's D020 deleted) either refuse outright or returned an
         * unsourced multiple of a neighbouring tier, so refusing here is the
         * correct answer, not a gap. */
        default:             return -1.0;
    }
}
/* 1.11.57 (latent D025): THE CONTRACT SAID TWO THINGS AT ONCE.
 *
 * hasTier() returned true for BANKGROUP, RANK and CHANNEL while
 * getTierLatencyNs() returned -1 for all three, with only a "// see below"
 * comment linking them. Read as "this model can answer about that tier" the
 * pair is a flat contradiction; read as "the technology physically has that
 * tier" it is consistent, and that IS the documented meaning in
 * memory_model.h. The one live consumer (main.cpp) already asks both and falls
 * back when the latency is negative, so nothing was wrong in today's numbers;
 * a consumer that trusted hasTier() alone would have taken -1.0 ns as a
 * latency. Answered by making the base-class contract explicit rather than by
 * dropping tiers DRAM really has. */
bool DRAMModel::hasTier(Tier tier) const {
    // PHYSICAL existence, per the MemoryModel contract. DRAM has every tier;
    // whether this model can SOURCE a number for one is getTierLatencyNs().
    return tier == Tier::SUBARRAY || tier == Tier::BANK ||
           tier == Tier::BANKGROUP || tier == Tier::CHIP ||
           tier == Tier::RANK || tier == Tier::CHANNEL;
}
std::string DRAMModel::tierLatencySource(Tier tier, Op op) const {
    if (getTierLatencyNs(tier, op) < 0.0) return "";
    switch (tier) {
        case Tier::SUBARRAY: return "Ramulator tRCD+tCAS";
        case Tier::BANK:     return "Ramulator tRP+tRCD+tCAS";
        case Tier::CHIP:     return "Ramulator bank+tBurst";
        default:             return "";
    }
}


/* 1.11.25: DRAM is sized by Ramulator's JEDEC organization, so an external
 * array capacity does not apply -- accepted and ignored, deliberately. */
void DRAMModel::setArrayCapacityBytes(uint64_t /*bytes*/) {}


/* 1.11.25: DRAM access width is fixed by the JEDEC organization. */
void DRAMModel::setAccessWidthBits(uint32_t /*bits*/) {}

void DRAMModel::setTemperatureK(int k) {
    if (ramulator_instance_) ramulator_instance_->setTemperatureK(k);   // 1.11.66 (B4)
}

void DRAMModel::setDramPartKnobs(const std::string& device_width, int ddr5_speed_grade) {
    if (!ramulator_instance_) return;
    ramulator_instance_->setDeviceWidth(device_width);
    ramulator_instance_->setDdr5SpeedGrade(ddr5_speed_grade);
}

} // namespace pimid
