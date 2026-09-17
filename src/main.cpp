/**
 * @file main.cpp
 * @brief PIMID Simulation Infrastructure - Single Entry Point for All Simulation Modes
 *
 * Binary: pimid (main PIMID binary)
 *
 * This is the PRIMARY PIMID simulation infrastructure binary supporting all modes:
 * - sim:        Config-driven PIM architecture simulation (default)
 * - standalone: PIM-enabled workload execution
 * - host:       Host-only simulation (for split host/device testing)
 * - device:     Device-only simulation (for split host/device testing)
 *
 * Usage:
 *   pimid --method exec      --config architecture.yaml --workload ./bench
 *   pimid --method trace     --config architecture.yaml --trace-file in.trace
 *   pimid --method trace-gen --workload ./bench --trace-file out.pimtrace
 *   pimid --method exec      --scope system --config cosim.yaml \
 *                            --workload ./cosim_bench
 *   pimid --method exec      --mpi-ranks 4 --workload ./mpi_app
 *
 * External Models Integrated:
 *   - Ramulator2: DRAM timing simulation
 *   - CACTI/McPAT: SRAM/cache and power modeling
 *   - NVSim: Non-volatile memory modeling
 *   - GARNET: Network-on-chip simulation
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <set>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <signal.h>
#include <cerrno>
#include <limits.h>
#include <libgen.h>
#include <numeric>

// Trace infrastructure
#include "trace/trace_writer.h"

// External model wrappers for querying real memory timing
#include "memory/nvsim_wrapper.h"
#include "util/cache_warehouse.h"
#include "memory/cacti_wrapper.h"
#include "memory/ramulator_wrapper.h"
#include "memory/memory_model.h"
#include "memory/internal_dram_network.h"
#include "sparse_htree.h"
#include "pimid_noc_shm.h"

#ifdef HAVE_HDF5
#include <hdf5.h>
#include <hdf5_hl.h>
#endif

// ZSim trace driver library (in-process trace replay)
#include "zsim_trace_api.h"

// Helper to get PIMID root directory (from executable path or environment)
static std::string getPimidRoot() {
    // Check environment variable first
    const char* env_root = getenv("PIMID_ROOT");
    if (env_root && access(env_root, F_OK) == 0) {
        return std::string(env_root);
    }

    // Get executable path and derive root
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len != -1) {
        exe_path[len] = '\0';
        // exe is in build/, go up one level to pimid/
        std::string path(dirname(exe_path));
        size_t build_pos = path.rfind("/build");
        if (build_pos != std::string::npos) {
            return path.substr(0, build_pos);
        }
        // If not in build/, assume exe is in pimid/ root
        return path;
    }

    // Fallback to current directory
    return ".";
}

// Forward declaration for UnifiedConfig
struct UnifiedConfig;

/**
 * @brief Get memory latency in cycles from external models (DEFAULT) or complete YAML override.
 *
 * Timing source:
 * - DEFAULT: External model query (NVSim for NVM, CACTI for SRAM, Ramulator for DRAM)
 * - OVERRIDE: YAML config ONLY if user provides ALL required params (timing + energy + power)
 *
 * NO hardcoded fallback values. External models provide calibrated timing based on
 * technology node, capacity, and other physical parameters.
 *
 * @param memory_tech Canonical memory technology string (e.g. "DDR4",
 *        "STT_MRAM"); "DRAM" is an alias canonicalMemTech() has already
 *        resolved to DDR4 by this point -- 1.11.57 (latent B044)
 * @param frequency_mhz Operating frequency in MHz
 * @param use_yaml_override true if user provided complete YAML memory params
 * @param yaml_latency_ns Latency from YAML (only used if use_yaml_override is true)
 * @return Memory access latency in cycles
 */
/* 1.11.25: placement now reaches the ARRAY, not just the network.
 *
 * Until now this function took no placement argument, so a subarray-placed PE
 * and a chip-placed PE were charged the SAME array access -- one 64 KB
 * per-bank characterization for every tier. The whole of Figure 2's NVM/SRAM
 * tier separation therefore came from the network path (fewer hierarchy
 * levels traversed), and none of it from the array. That is incomplete: a PE
 * at the subarray genuinely sees a shorter array path than one at the chip.
 *
 * pe_hierarchy_level (0 SUBARRAY, 1 BANK, 2 BANK_GROUP, 3 CHIP, 4 RANK,
 * -1 HOST_MC) now selects the tier, and the number comes from the memory
 * plugin contract added in 1.11.24 -- so it is the technology's own tool
 * answering, not a multiplier. A tier the technology does not have, or one
 * the tool cannot source, returns <0 and we fall back to the flat
 * characterization with a stated reason rather than inventing a value. */
static pimid::MemoryModel::Tier tierForPlacement(int pe_hierarchy_level) {
    switch (pe_hierarchy_level) {
        case 0:  return pimid::MemoryModel::Tier::SUBARRAY;
        case 1:  return pimid::MemoryModel::Tier::BANK;
        case 2:  return pimid::MemoryModel::Tier::BANKGROUP;
        case 3:  return pimid::MemoryModel::Tier::CHIP;
        case 4:  return pimid::MemoryModel::Tier::RANK;
        default: return pimid::MemoryModel::Tier::CHANNEL;   // 5/6/-1: host-side
    }
}

static int validateTechNodeNm(int node_nm, const char* what);  // 1.11.51 (L70): node authority, declared early for the latency helper

/* 1.11.51 (L70): array_tech_node_nm is REQUIRED. The SRAM/NVM latency
 * queries below characterized at a hardcoded 22 nm while the energy/area
 * queries for the same array used the configured node -- timing and energy
 * of one array priced at two different processes. Every caller now passes
 * the node that governs its array, and the same positive-list authority
 * validates it (DRAM branches ignore it: JEDEC timing has no free node). */
/* 1.11.57 (latent B024): the array_capacity_bytes parameter is GONE.
 *
 * It was accepted from every caller, immediately discarded with a (void)
 * cast, and passed through the recursive call to be discarded again -- while
 * the doc comment beside it still told the reader that the caller's capacity
 * "is used so the timing matches the energy model". Two callers computed a
 * banks x 64 KB product specifically to feed it. Nothing moved, because the
 * per-bank unit below has governed SRAM/NVM access timing since that comment
 * was written; the defect was a parameter that documented an influence it did
 * not have, waiting for someone to trust it. Removed rather than renamed:
 * there is no capacity to pass that this function would honour. */
static int getMemoryLatencyCycles(const std::string& memory_tech, double frequency_mhz,
                                   int array_tech_node_nm,
                                   bool use_yaml_override = false, double yaml_latency_ns = -1.0,
                                   int pe_hierarchy_level = -999,
                                   int array_temperature_k = 350,   // 1.11.52 (D055)
                                   int access_line_bytes = 64) {    // 1.11.56 (B018)
    double latency_ns = 0.0;

    /* 1.11.56 (audit B018): the SRAM/NVM array queries hardcoded a 512-bit
     * access, i.e. a 64 B line, while system.cache_line_size is a real knob
     * that reaches ZSim as sys.lineSize. A run configured with 128 B lines
     * simulated 128 B traffic against an array characterized for 64 B, and
     * CACTI/NVSim access time is close to linear in output width above the
     * bitline -- so the array timing was optimistic by about the ratio. */
    if (access_line_bytes <= 0) access_line_bytes = 64;
    const uint32_t access_width_bits =
        static_cast<uint32_t>(access_line_bytes) * 8u;

    // Normalize technology name for comparison
    std::string tech = memory_tech;
    std::transform(tech.begin(), tech.end(), tech.begin(), ::toupper);

    // For SRAM/NVM the access latency depends on the array size (CACTI/NVSim
    // characterize bitline/wordline length), and the unit characterized here is
    // ONE BANK: each PE reads/writes its OWN local 64KB bank, so the access is
    // characterized on a single bank -- not the whole (num_banks x 64KB) device
    // array. This is the physically correct local-access latency for PIM and
    // keeps NVSim off the multi-minute large-array run. There is deliberately no
    // caller-supplied capacity (1.11.57, latent B024): the per-bank unit is the
    // only array this function times.
    const uint64_t PER_BANK_BYTES = 64 * 1024;
    uint64_t sram_cap = PER_BANK_BYTES;
    uint64_t nvm_cap  = PER_BANK_BYTES;

    if (use_yaml_override && yaml_latency_ns > 0.0) {
        // User provided COMPLETE memory params in YAML - use their values
        latency_ns = yaml_latency_ns;
    }
    /* 1.11.25: the plugin path. Ask the technology's own model for the tier
     * the PE actually sits at. This is the single place a memory technology
     * is priced, so adding one is "implement the contract + bind your tool",
     * with no edit here. */
    else if (pe_hierarchy_level != -999) {
        double tier_ns = -1.0;
        std::string prov;
        try {
            pimid::MemoryTechnology mt = pimid::parseMemoryTechnology(memory_tech);
            auto model = pimid::MemoryModelFactory::createMemoryModel(mt, "");
            if (model) {
                /* 1.11.25: characterize the SAME array the flat path does --
                 * the 64 KB per-bank unit a PE owns. Without this the model
                 * uses its compiled-in default (STT-MRAM: 256 MB) and the
                 * ladder describes a different device: 148x apart in read
                 * latency on our own cached characterization. */
                model->setArrayCapacityBytes(PER_BANK_BYTES);
                model->setAccessWidthBits(access_width_bits);  // 1.11.56 (B018): the configured line
                /* 1.11.51 (L70): tier timing at the RUN's validated node --
                 * the plugin models carried their own divergent compiled-in
                 * defaults (22/22/32/90 nm) that no caller ever set. */
                model->setTechNodeNm(validateTechNodeNm(array_tech_node_nm,
                                                        "tier latency query"));
                model->setTemperatureK(array_temperature_k);   // 1.11.52 (D055)
                model->initialize();
                const auto tier = tierForPlacement(pe_hierarchy_level);
                if (model->hasTier(tier)) {
                    tier_ns = model->getTierLatencyNs(tier, pimid::MemoryModel::Op::READ);
                    prov = model->tierLatencySource(tier, pimid::MemoryModel::Op::READ);
                }
                if (tier_ns > 0.0) {
                    std::cout << "  [mem] " << memory_tech << " @ "
                              << pimid::MemoryModel::tierName(tier) << " placement: "
                              << tier_ns << " ns (" << prov << ")" << std::endl;
                } else {
                    std::cout << "  [mem] " << memory_tech << " has no sourceable "
                              << pimid::MemoryModel::tierName(tier)
                              << " tier; using the flat characterization"
                              << std::endl;
                }
            }
        } catch (const std::exception& e) {
            std::cout << "  [mem] plugin path unavailable (" << e.what()
                      << "); using the flat characterization" << std::endl;
        }
        if (tier_ns > 0.0) {
            latency_ns = tier_ns;
        } else {
            latency_ns = getMemoryLatencyCycles(memory_tech, frequency_mhz,
                                                array_tech_node_nm,
                                                false, -1.0,
                                                -999, array_temperature_k,
                                                access_line_bytes)  // 1.11.56 (B018)
                         * 1000.0 / frequency_mhz;
        }
    }
    else {
        // DEFAULT: Query external models for timing
        // External models use calibrated parameters based on technology/process node

        if (tech == "SRAM") {
            // Query CACTI for SRAM timing (RAM mode, not cache)
            pimid::CACTIWrapper::SRAMConfig cfg;
            cfg.capacity_bytes = sram_cap;
            cfg.tech_node_nm = validateTechNodeNm(array_tech_node_nm, "SRAM latency query");
            cfg.temperature = static_cast<uint32_t>(array_temperature_k);   // 1.11.52 (D055)
            cfg.output_width_bits = access_width_bits;   // 1.11.56 (B018)
            cfg.is_cache = false;  // SRAM as memory, not cache
            pimid::CACTIWrapper wrapper(cfg);
            wrapper.initialize();
            latency_ns = wrapper.getAccessTime() * 1e9;  // seconds -> ns
        }
        // --- NVSim-backed NVM technologies ---
        else if (tech == "STT_MRAM" || tech == "STTMRAM" || tech == "STT-MRAM" || tech == "MRAM") {
            pimid::NVSimWrapper::NVMConfig cfg;
            cfg.nvm_type = pimid::NVSimWrapper::NVMType::STTRAM;
            cfg.capacity_bytes = nvm_cap;
            cfg.process_node_nm = validateTechNodeNm(array_tech_node_nm, "NVM latency query");
            cfg.temperature_k = array_temperature_k;   // 1.11.52 (D055)
            cfg.word_width_bits = access_width_bits;   // 1.11.56 (B018): the configured line
            pimid::NVSimWrapper wrapper(cfg);
            wrapper.initialize();
            latency_ns = wrapper.getReadLatency() * 1e9;
        }
        else if (tech == "PCM" || tech == "PCRAM" || tech == "3DXPOINT") {
            pimid::NVSimWrapper::NVMConfig cfg;
            cfg.nvm_type = pimid::NVSimWrapper::NVMType::PCRAM;
            cfg.capacity_bytes = nvm_cap;
            cfg.process_node_nm = validateTechNodeNm(array_tech_node_nm, "NVM latency query");
            cfg.temperature_k = array_temperature_k;   // 1.11.52 (D055)
            cfg.word_width_bits = access_width_bits;   // 1.11.56 (B018): the configured line
            pimid::NVSimWrapper wrapper(cfg);
            wrapper.initialize();
            latency_ns = wrapper.getReadLatency() * 1e9;
        }
        else if (tech == "RERAM" || tech == "RESISTIVE" || tech == "MEMRISTOR") {
            pimid::NVSimWrapper::NVMConfig cfg;
            cfg.nvm_type = pimid::NVSimWrapper::NVMType::RERAM;
            cfg.capacity_bytes = nvm_cap;
            cfg.process_node_nm = validateTechNodeNm(array_tech_node_nm, "NVM latency query");
            cfg.temperature_k = array_temperature_k;   // 1.11.52 (D055)
            cfg.word_width_bits = access_width_bits;   // 1.11.56 (B018): the configured line
            pimid::NVSimWrapper wrapper(cfg);
            wrapper.initialize();
            latency_ns = wrapper.getReadLatency() * 1e9;
        }
        // --- Ramulator-backed DRAM technologies ---
        /* 1.11.57 (latent B044): "DRAM" removed from this list. It is an
         * ALIAS that canonicalMemTech() resolves to DDR4 before any technology
         * string reaches here, so the branch was unreachable -- and keeping it
         * meant a string that HAD bypassed canonicalisation would be timed as
         * DRAM in silence instead of hitting the announcement below, which
         * exists precisely to catch that. */
        else if (tech == "DDR3" || tech == "DDR4" || tech == "DDR5" ||
                 tech == "LPDDR5" || tech == "GDDR6" ||
                 tech == "HBM2" || tech == "HBM3") {
            // Query Ramulator wrapper for DRAM timing (tRCD + tCAS)
            pimid::RamulatorWrapper wrapper("", tech);
            wrapper.initialize();
            latency_ns = wrapper.getTRCD() + wrapper.getTCAS();
        }
        else {
            /* 1.11.52 (audit B025): reaching here means a technology string
             * got past canonicalMemTech(), which validates against the
             * supported set and is the single point every technology passes
             * through. Silently timing it as DDR4 would hand back a number
             * for a part nobody described -- the exact failure canonical-
             * isation exists to prevent -- so say so. The DDR4 answer is
             * still produced (a refusal here would abort mid-report), but it
             * is announced as a substitution, not presented as the
             * technology's own. */
            std::cerr << "[mem] WARNING: no timing model for memory technology '"
                      << tech << "' in the latency helper; substituting DDR4 "
                         "timing. This string bypassed canonicalisation -- the "
                         "number below is DDR4's, not " << tech << "'s."
                      << std::endl;
            pimid::RamulatorWrapper wrapper("", "DDR4");
            wrapper.initialize();
            latency_ns = wrapper.getTRCD() + wrapper.getTCAS();
        }
    }

    // Convert nanoseconds to cycles: cycles = latency_ns * freq_mhz / 1000
    double cycles = latency_ns * frequency_mhz / 1000.0;
    int result = static_cast<int>(std::round(cycles));
    return std::max(1, result);
}

/**
 * @brief Get cache latency in cycles from CACTI for a given cache configuration.
 *
 * Queries CACTI with physical parameters (size, associativity, line size) to get
 * calibrated access timing. Substitutes fallback_cycles on CACTI failure, and
 * says so -- see the 1.11.57 note on the fallback below.
 *
 * @param size_kb Cache size in KB
 * @param ways Set associativity
 * @param line_size Cache line size in bytes
 * @param frequency_mhz Operating frequency in MHz
 * @param tech_node_nm Technology node in nanometers (e.g. 22, 14, 7)
 * @param fallback_cycles Substituted latency, IN CYCLES, if CACTI cannot answer
 * @return Cache access latency in cycles
 */
static int validateTechNodeNm(int node_nm, const char* what);  // 1.11.17: single node authority

/* 1.11.57 (latent B020): how many cache latencies in this run are NOT CACTI's.
 *
 * The provenance banner prints "(CACTI <n>nm)" beside every cache latency
 * unconditionally, and the query below returned the caller's literal on any
 * failure without a word -- so a build with no CACTI tables would have printed
 * four hand-written cycle counts under the tool's name, which is the false
 * provenance claim this project treats most seriously. It was invisible only
 * because CACTI succeeds in every observed run. The counter lets the banner
 * tell the truth when it does not. */
static int g_cache_latency_fallbacks = 0;
static int cacheLatencyFallbackCount() { return g_cache_latency_fallbacks; }

/* 1.11.57 (latent B017): the fallback argument is a CYCLE COUNT, named as
 * such now (it was "default_cycles", which reads as "the default", not as a
 * unit). Every call site passes it as a bare literal -- 4, 3, 12, 20 -- so the
 * unit is stated at each one too. */
static int getCacheLatencyCycles(int size_kb, int ways, int line_size,
                                  double frequency_mhz, int tech_node_nm,
                                  int fallback_cycles) {
    /* 1.11.17 (audit go-through): this CACTI query used to keep the exact
     * bottom-only clamp 1.11.2 removed everywhere else -- an invalid node
     * silently priced cache TIMING at 22 nm while the power path fatally
     * rejected the same node. One authority for the node surface. */
    int cacti_tech = validateTechNodeNm(tech_node_nm, "cache-latency query");
    pimid::CACTIWrapper::SRAMConfig cfg;
    cfg.capacity_bytes = static_cast<uint64_t>(size_kb) * 1024;
    cfg.associativity = ways;
    cfg.line_size = line_size;
    // Scale banks with cache size: 1 bank per 64KB (L1=1, 2MB L2=32)
    cfg.banks = std::max(1, size_kb / 64);
    cfg.is_cache = true;
    cfg.tech_node_nm = cacti_tech;

    cfg.output_width_bits = line_size * 8;
    cfg.quiet = true;  // latency-only query: energies are not consumed
    const char* why = "the query was rejected (isValid() false)";
    try {
        pimid::CACTIWrapper wrapper(cfg);
        wrapper.initialize();
        if (wrapper.isValid()) {
            double ns = wrapper.getAccessTime() * 1e9;
            int cycles = static_cast<int>(std::round(ns * frequency_mhz / 1000.0));
            return std::max(1, cycles);
        }
    } catch (const std::exception& e) {
        why = e.what();
    } catch (...) {
        why = "an unknown exception escaped the CACTI wrapper";
    }
    /* 1.11.57 (latent B020): ANNOUNCE the substitution. This is the ACCESS
     * LATENCY of one cache level -- it governs every hit in the timing model
     * and the provenance line the run prints beside it. Returning the
     * caller's literal in silence let a run report hand-written cycle counts
     * as CACTI's answer. The literal is still returned (refusing here would
     * abort the banner mid-report and the caller has no other source), but it
     * is announced, counted, and the banner stops claiming CACTI for it. */
    ++g_cache_latency_fallbacks;
    std::cerr << "[cache] WARNING: CACTI could not price the " << size_kb
              << " KB " << ways << "-way cache at " << cacti_tech
              << " nm (" << why << "). Its ACCESS LATENCY falls back to the "
                 "declared " << fallback_cycles
              << " cycles, which is a written-down number, not a "
                 "characterization of this array." << std::endl;
    return fallback_cycles;
}


/* 1.11.57 (latent B021 / B023): ONE refusal for "the DRAM oracle did not
 * answer".
 *
 * Three sites queried RamulatorWrapper for a quantity that has no other
 * source -- the aggregate/channel bandwidth, the DQ turnaround, the channel
 * count -- and each swallowed the failure and carried on with a literal: a
 * zero that downstream reads as a measurement, a 6400 MB/s struct default, or
 * a written-down 12800. For a DRAM technology Ramulator IS the datasheet, and
 * the technology has already been whitelisted, so a throw here is a broken
 * tool, not a configuration the run should model around. Refuse, name the
 * quantity and say what it governs. */
[[noreturn]] static void refuseWithoutDramOracle(const std::string& tech,
                                                 const std::string& why,
                                                 const std::string& governs) {
    std::cerr << "\n[memory] FATAL: the Ramulator preset for " << tech
              << " could not be read (" << why << ").\n"
                 "  It is the only source of " << governs << ".\n"
                 "  Substituting a literal here would report a memory system "
                 "that was measured by nothing, so the run refuses instead.\n"
              << std::endl;
    std::exit(2);
}

/* 1.11.57 (latent B022): the sentence the M/D/1 bandwidth derivation owes the
 * reader when it cannot ask the oracle. Not fatal -- the user has named the
 * controller explicitly here and their own bandwidth is a legitimate thing to
 * keep -- but it must be stated as theirs, or as a struct placeholder, and
 * never left to look like the technology's own limit. */
static void announceUnclampedBandwidth(const std::string& tech,
                                       const std::string& why,
                                       bool user_set) {
    std::cerr << "[bw] WARNING: the " << tech << " rank bandwidth could not be "
                 "read from Ramulator (" << why << "), so no physical M/D/1 "
                 "bandwidth cap is derived for this run. "
              << (user_set
                    ? "The user's memory.bandwidth_mbs stands UNCLAMPED."
                    : "The M/D/1 bandwidth stays at the struct default of "
                      "6400 MB/s, which is a placeholder, not a property of "
                      "this part.")
              << std::endl;
}

/**
 * @brief Find qemu-x86_64 binary.
 *
 * Searches /usr/bin, /usr/local/bin, then PATH.
 * Returns empty string if not found.
 */
static std::string findQemuBinary() {
    /* 1.11.55: an explicit override, first. Without it an A/B comparison
     * between two trees silently uses TWO DIFFERENT GUEST EMULATORS -- each
     * tree prefers its own external/qemu build -- and the guest instruction
     * stream then differs by a handful of accesses (measured: 25406 vs 25394
     * packets on one cell), which moves NoC duty and every number derived
     * from it. That is the N6 layout-sensitivity class, and it is a gate
     * hazard rather than a model defect, so the fix is to make the emulator
     * selectable and state which one ran. */
    if (const char* q = getenv("PIMID_QEMU")) {
        if (q[0] && access(q, X_OK) == 0) return std::string(q);
        std::cerr << "[qemu] PIMID_QEMU=" << (q ? q : "")
                  << " is not executable; falling back to the search path."
                  << std::endl;
    }
    std::string pimid_root = getPimidRoot();
    std::vector<std::string> qemu_paths = {
        // Prefer project-local QEMU build (built with --enable-plugins)
        pimid_root + "/external/qemu/build/qemu-x86_64",
        pimid_root + "/external/qemu/build/qemu-x86_64",
        "/usr/bin/qemu-x86_64",
        "/usr/local/bin/qemu-x86_64",
    };
    const char* path_env = getenv("PATH");
    if (path_env) {
        std::istringstream ss(path_env);
        std::string dir;
        while (std::getline(ss, dir, ':')) {
            qemu_paths.push_back(dir + "/qemu-x86_64");
        }
    }
    for (const auto& p : qemu_paths) {
        if (access(p.c_str(), X_OK) == 0) {
            return p;
        }
    }
    return "";
}

/**
 * @brief Find a QEMU plugin shared library by name.
 *
 * Searches build directories relative to PIMID root and current directory.
 * @param plugin_name e.g. "libpimid_trace.so" or "libzsim_qemu.so"
 * Returns empty string if not found.
 */
static std::string findQemuPlugin(const std::string& plugin_name) {
    // SELF-EXE-RELATIVE FIRST (the only trustworthy anchor): the plugin MUST
    // come from the same build tree as this binary. Resolving via PIMID_ROOT
    // or the CWD let sweep jobs silently pair a fresh binary with a days-old
    // plugin from another build tree (a Frankenbuild that "un-deployed" every
    // fix and burned a full day of fleet debugging). CWD fallbacks are kept
    // only for exotic setups, and the caller prints the chosen path.
    std::vector<std::string> plugin_paths;
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        std::string exeDir(exe);
        size_t slash = exeDir.rfind('/');
        if (slash != std::string::npos) {
            exeDir = exeDir.substr(0, slash);
            // binary at <build>/pimid -> plugin at <build>/external/zsim/
            plugin_paths.push_back(exeDir + "/external/zsim/" + plugin_name);
        }
    }
    std::string pimid_root = getPimidRoot();
    plugin_paths.push_back(pimid_root + "/build/external/zsim/" + plugin_name);
    plugin_paths.push_back(pimid_root + "/external/zsim/build/" + plugin_name);
    plugin_paths.push_back("./" + plugin_name);
    for (const auto& p : plugin_paths) {
        if (access(p.c_str(), F_OK) == 0) {
            return p;
        }
    }
    return "";
}

/**
 * @brief Find the zsim_trace executable.
 *
 * Searches build directories relative to PIMID root and current directory.
 * Returns empty string if not found.
 */
/**
 * @brief Find the libpimid_mpi.so shared library.
 *
 * Searches build directories relative to PIMID root and current directory.
 * Returns empty string if not found.
 */
static std::string findPimidMpiLib() {
    std::string pimid_root = getPimidRoot();
    // PIMID_ROOT typically points at the repo root, and the build lives at
    // ${ROOT}/build/. The other paths are kept for non-standard layouts.
    std::vector<std::string> search_paths = {
        pimid_root + "/build/libpimid_mpi.so",  // standard layout
        pimid_root + "/build/pimid/libpimid_mpi.so",
        pimid_root + "/build/lib/libpimid_mpi.so",
        pimid_root + "/build/libpimid_mpi.so",
        "./libpimid_mpi.so",
        "./build/libpimid_mpi.so",
    };
    for (const auto& p : search_paths) {
        if (access(p.c_str(), F_OK) == 0) {
            return p;
        }
    }
    return "";
}

/**
 * @brief Parsed ZSim output statistics for a single rank/instance.
 *
 * These are extracted from zsim.out and used to generate synthetic trace events.
 */
struct ZSimParsedOutput {
    uint64_t cycles = 0;
    uint64_t instrs = 0;

    // 1.9.10 co-sim power-integration fix: contention-INCLUSIVE wall-clock cycle
    // counts, tracked per core group. `cycles` above is the FIRST core's unhalted
    // (contention-excluded) count, which grossly understates the true runtime for
    // memory-contended or offloaded workloads and was inflating co-sim/baseline
    // McPAT power by 1-3 orders of magnitude. These give each system node a runtime
    // that matches the window over which its accesses actually occurred.
    //   host_wall_cycles = max over host cores of (unhalted cycles + contention cycles)
    //   dev_wall_cycles  = max over device PEs of (simulated cycles)  [Garnet-timed;
    //                      device PE cycles already fold in memory-network stalls]
    // Only consumed by runPerNodePowerAnalysis (system scope). The device-SCOPE
    // path (runPowerAnalysis) keeps using `cycles`, so its values are unchanged.
    uint64_t host_wall_cycles = 0;
    uint64_t dev_wall_cycles = 0;
    /* 1.11.7 (#85): host<->device crossing counters from the plugin
     * (zinfo->xing via ProxyStats). LATCH-LAST semantics in the parser: the
     * counters are monotonic run totals, so the final dump's value is the
     * truth even when multiple dumps appear in one zsim.out. */
    uint64_t xing_h2d_bytes = 0;
    uint64_t xing_d2h_bytes = 0;
    uint64_t xing_count = 0;
    uint64_t xing_flush_bytes = 0;
    /* 1.11.8 (#84): PG residency (latch-last root scalars + per-group
     * per-core sums). r_idle = 1 - activePhases/phases at consumption. */
    uint64_t pg_anycore_active = 0;
    uint64_t pg_sharedcache_active = 0;
    uint64_t pg_noc_active = 0;
    uint64_t pg_hostmc_active = 0;
    uint64_t pg_devmc_active = 0;
    uint64_t pg_phase_window = 0;   // 1.11.18: ROI-relative denominator (0 = use phases)
    /* 1.11.51 (E17): the devMC inter-access gap histogram, read back from
     * the stats stream (log2-cycle buckets, counts + cycles, ROI span).
     * The power-down residency derives from THIS at the sourced tXP
     * threshold; the 10k-cycle phase residency above cannot see tXP-scale
     * windows (it measured r_idle = 0 while ~90% of ROI cycles sat in
     * usable gaps). */
    static const int kGapBuckets = 40;
    uint64_t devmc_gap_cnt[kGapBuckets] = {0};
    uint64_t devmc_gap_cyc[kGapBuckets] = {0};
    uint64_t devmc_gap_span = 0;
    /* 1.11.18: the window every PG residency divides by. Pre-1.11.18 dumps
     * carry no pgPhaseWindow, so the whole-run phase count is used and the
     * old numbers reproduce exactly. */
    uint64_t pg_window() const {
        return (pg_phase_window > 0) ? pg_phase_window : phases;
    }
    uint64_t phases = 0;
    /* 1.11.9 (#86, audit): the PE-MI locality split has been EMITTED since
     * 1.5.3 and parsed by nobody -- the one measurement that says whether a
     * placement actually kept its accesses local. */
    /* 1.11.52 (audit D003): MEASURED row-buffer behaviour from the PE-MI. */
    uint64_t row_hits = 0, row_misses = 0;
    double rowMissFraction() const {
        uint64_t n = row_hits + row_misses;
        return (n > 0) ? static_cast<double>(row_misses) / static_cast<double>(n)
                       : -1.0;   // <0 = the run carried no row measurement
    }
    uint64_t pemi_local_acc = 0;
    uint64_t pemi_remote_acc = 0;
    /* 1.11.10 (#112): MEASURED instruction mix, summed over cores like every
     * other activity counter (1.11.9 put instrs on this same base, which is
     * what lets the mix be used at all). */
    uint64_t mix_int = 0, mix_mul = 0, mix_fp = 0, mix_ld = 0, mix_st = 0, mix_br = 0;
    /* 1.9.29: per-node measured counters. Everything below this comment that is
     * NOT inside `host`/`dev` is an ALL-NODES total, kept for the device-SCOPE
     * path (runPowerAnalysis), which simulates one node and is correct as-is.
     *
     * The system-scope path must NOT use those totals. Before 1.9.29 it split
     * every one of them across nodes by CORE-COUNT proportion
     * (node.num_cores / sum(num_cores)), so on a 1-core host beside a 16-PE
     * device the host was credited with 1/17 of its own work and the device
     * with work it never executed. 1.9.28 fixed that for the instruction count
     * alone and left the rest split, which made the host internally
     * inconsistent: node_instrs was its own 77.5M while uops was 85.9M*0.06 =
     * 5.2M -- fewer uops than instructions, and a 0.4% branch rate. Each node
     * needs its OWN measured counters, so they live here as a set. */
    struct GroupCounters {
        uint64_t pgActivePhases = 0;  // 1.11.8: sum of per-core PG activity
        uint64_t mix_int = 0, mix_mul = 0, mix_fp = 0, mix_ld = 0, mix_st = 0, mix_br = 0;  // 1.11.10/.15
        uint64_t instrs = 0;
        /* 1.9.33: of `instrs`, the portion that is injected timing charges
         * (coherence flush, kernel launch, barrier latency) rather than executed
         * code -- the plugin manufactures those BBLs with the CYCLE COUNT in the
         * instrs field. In a co-simulation ROI the host executes no real code at
         * all, so this can be the WHOLE of its reported instruction count. */
        uint64_t syntheticInstrs = 0;
        // Core activity. zsim reports these per core; the power model used to
        // build McPAT's instruction mix from fixed fractions (70% int / 10% fp
        // / 10% branch / 1% mispredict) applied identically to every workload.
        uint64_t uops = 0;              // retired micro-ops (OOO path)
        uint64_t bbls = 0;              // basic blocks -- each ends in a control
                                        // transfer, so a MEASURED branch proxy
        uint64_t branches = 0;          // conditional branches resolved
        uint64_t mispredBranches = 0;   // of those, mispredicted
        uint64_t indirBranches = 0;     // indirect jmp/call resolutions
        uint64_t rasReturns = 0;        // returns resolved against the RAS

        uint64_t l1i_fhGETS = 0, l1i_hGETS = 0, l1i_mGETS = 0;
        uint64_t l1d_fhGETS = 0, l1d_hGETS = 0, l1d_mGETS = 0;
        uint64_t l1d_fhGETX = 0, l1d_hGETX = 0, l1d_mGETXIM = 0;
        uint64_t l2_hGETS = 0, l2_mGETS = 0, l2_hGETX = 0, l2_mGETXIM = 0;
        uint64_t l3_hGETS = 0, l3_mGETS = 0, l3_hGETX = 0, l3_mGETXIM = 0;
        uint64_t mem_rd = 0, mem_wr = 0;

        uint64_t l1i_total_reads() const { return l1i_fhGETS + l1i_hGETS + l1i_mGETS; }
        uint64_t l1d_total_reads() const { return l1d_fhGETS + l1d_hGETS + l1d_mGETS; }
        uint64_t l1d_total_writes() const { return l1d_fhGETX + l1d_hGETX + l1d_mGETXIM; }
        uint64_t l2_total_reads() const { return l2_hGETS + l2_mGETS; }
        uint64_t l2_total_writes() const { return l2_hGETX + l2_mGETXIM; }
        uint64_t l3_total_reads() const { return l3_hGETS + l3_mGETS; }
        uint64_t l3_total_writes() const { return l3_hGETX + l3_mGETXIM; }
        // True when this group carried any measured work at all. A node with no
        // activity must fall back rather than be priced at zero.
        /* 1.9.35: "was this node OBSERVED", not "did it do work".
         *
         * These must not be conflated. A co-simulation host executes no real
         * code during the offload window -- it prepared its data beforehand and
         * is now waiting -- so its measured activity is legitimately ZERO. The
         * previous test (real_instrs() > 0 || uops > 0) read that as "no
         * measurements available" and fell back to the core-fraction guess,
         * which then INVENTED tens of thousands of instructions for a core that
         * provably executed none. A node proven idle must be priced as idle.
         * The fallback exists for a dump with no per-node breakdown at all --
         * an older stats file -- and that is what `seen` distinguishes. */
        bool seen = false;
        bool has_activity() const { return seen; }
        /* Executed instructions, with injected timing charges removed. */
        uint64_t real_instrs() const {
            return (instrs > syntheticInstrs) ? (instrs - syntheticInstrs) : 0;
        }
    };
    GroupCounters host;
    GroupCounters dev;
    /* 1.9.28 measured core activity. The power model previously built McPAT's
     * instruction mix from fixed fractions of the instruction count (70% int,
     * 10% fp, 10% branch) for every workload, so per-core dynamic power was
     * driven by a constant rather than by what the program executed. zsim
     * already reports these; they were simply never parsed. Zero means the
     * counter was absent, and the caller falls back to the old fractions. */
    uint64_t syntheticInstrs = 0;   // 1.9.33: injected timing charges inside instrs
    /* 1.9.43: the same accessor GroupCounters has, at the whole-run level. It was
     * missing here, so the one consumer that reads the whole-run counters -- the
     * co-simulation summary line -- had nothing to call and printed the raw
     * total, while every other consumer used the group-level accessor and got the
     * corrected one. Two ways to ask the same question, one of which did not
     * exist, is how that line drifted. */
    uint64_t real_instrs() const {
        return (instrs > syntheticInstrs) ? (instrs - syntheticInstrs) : 0;
    }
    uint64_t uops = 0;              // retired micro-ops (OOO path)
    uint64_t bbls = 0;              // basic blocks -- each ends in a control transfer,
                                    // so this is a MEASURED branch-count proxy
    uint64_t branches = 0;          // conditional branches resolved
    uint64_t mispredBranches = 0;   // of those, mispredicted
    uint64_t indirBranches = 0;     // indirect jmp/call resolutions
    uint64_t rasReturns = 0;        // returns resolved against the RAS

    // L1I: fhGETS (filtered hits), hGETS (hits), mGETS (misses) -- accumulated across all instances
    uint64_t l1i_fhGETS = 0;
    uint64_t l1i_hGETS = 0;
    uint64_t l1i_mGETS = 0;

    // L1D: reads (fhGETS+hGETS+mGETS), writes (fhGETX+hGETX+mGETXIM), misses
    uint64_t l1d_fhGETS = 0;
    uint64_t l1d_hGETS = 0;
    uint64_t l1d_mGETS = 0;
    uint64_t l1d_fhGETX = 0;
    uint64_t l1d_hGETX = 0;
    uint64_t l1d_mGETXIM = 0;

    // L2: reads (hGETS+mGETS), writes (hGETX+mGETXIM), misses, evictions
    uint64_t l2_hGETS = 0;
    uint64_t l2_mGETS = 0;
    uint64_t l2_hGETX = 0;
    uint64_t l2_mGETXIM = 0;
    uint64_t l2_PUTS = 0;
    uint64_t l2_PUTX = 0;

    // L3: reads (hGETS+mGETS), writes (hGETX+mGETXIM), misses
    uint64_t l3_hGETS = 0;
    uint64_t l3_mGETS = 0;
    uint64_t l3_hGETX = 0;
    uint64_t l3_mGETXIM = 0;

    // Memory controller: rd, wr
    uint64_t mem_rd = 0;
    uint64_t mem_wr = 0;

    // Derived totals (computed after parsing)
    uint64_t l1i_total_reads() const { return l1i_fhGETS + l1i_hGETS + l1i_mGETS; }
    uint64_t l1d_total_reads() const { return l1d_fhGETS + l1d_hGETS + l1d_mGETS; }
    uint64_t l1d_total_writes() const { return l1d_fhGETX + l1d_hGETX + l1d_mGETXIM; }
    uint64_t l2_total_reads() const { return l2_hGETS + l2_mGETS; }
    uint64_t l2_total_writes() const { return l2_hGETX + l2_mGETXIM; }
    uint64_t l3_total_reads() const { return l3_hGETS + l3_mGETS; }
    uint64_t l3_total_writes() const { return l3_hGETX + l3_mGETXIM; }

    // Legacy compatibility
    uint64_t l1d_hits() const { return l1d_fhGETS + l1d_hGETS + l1d_fhGETX + l1d_hGETX; }
    uint64_t l1d_misses() const { return l1d_mGETS + l1d_mGETXIM; }
    uint64_t l1i_hits() const { return l1i_fhGETS + l1i_hGETS; }
    uint64_t l1i_misses() const { return l1i_mGETS; }
    uint64_t l2_hits() const { return l2_hGETS + l2_hGETX; }
    uint64_t l2_misses() const { return l2_mGETS + l2_mGETXIM; }
    uint64_t l3_hits() const { return l3_hGETS + l3_hGETX; }
    uint64_t l3_misses() const { return l3_mGETS + l3_mGETXIM; }
};

/**
 * @brief Parse a zsim.out file and extract key statistics.
 *
 * ZSim text stats format (from text_stats.cpp):
 *   AggregateStat: "name: # description" followed by indented children
 *   ScalarStat: "name: value # description"
 *
 * We track the current scope (l1d-N, l1i-N, l2-N, mem-N) by matching scope
 * header lines, then extract specific stat keys within each scope.
 */
static ZSimParsedOutput parseZSimOutputFile(const std::string& path) {
    ZSimParsedOutput out;
    std::ifstream f(path);
    if (!f.is_open()) return out;

    /* 1.11.9 (#86, audit blocker): zsim appends a FULL stats dump, terminated
     * by "===", every time stats are written -- periodic dumps, a final dump,
     * and (in multi-process system runs) one set per QEMU process. The parser
     * accumulated across all of them, so every counter was multiplied by the
     * dump count: a run with one periodic dump plus the final dump reported
     * double the accesses it made. zsim's counters are MONOTONIC RUN TOTALS,
     * so the last dump alone is the truth. Read only the final dump segment.
     * If more than one dump is present the count is announced, because a
     * multi-PROCESS file (disjoint groups per process) needs the operator to
     * know that only the last process's groups are being read -- that
     * topology is out of scope for this build and must not pass silently. */
    std::vector<std::string> all_lines;
    {
        std::string l;
        while (std::getline(f, l)) all_lines.push_back(l);
    }
    std::vector<size_t> dump_ends;
    for (size_t i = 0; i < all_lines.size(); i++)
        if (all_lines[i] == "===") dump_ends.push_back(i);
    size_t start_idx = 0, end_idx = all_lines.size();
    if (dump_ends.size() >= 2) {
        // dump_ends[0] is the header separator; the final dump lies between
        // the last two "===" lines.
        start_idx = dump_ends[dump_ends.size() - 2] + 1;
        end_idx   = dump_ends[dump_ends.size() - 1];
        size_t n_dumps = dump_ends.size() - 1;
        if (n_dumps > 1) {
            std::cout << "  [stats] " << n_dumps << " stats dumps in "
                      << path << "; reading the LAST (counters are monotonic "
                         "run totals -- accumulating across dumps would "
                         "multiply every counter by " << n_dumps << ")"
                      << std::endl;
        }
    }
    size_t line_cursor = start_idx;

    // Scope tracking: which cache/MC are we inside?
    enum class Scope { NONE, ROOT, L1D, L1I, L2, L3, MEM };
    Scope scope = Scope::NONE;
    int scope_indent = 0;  // indentation level of the current scope header

    // 1.9.10: track which core group we are inside (host_cores vs device_pes) so
    // we can accumulate contention-inclusive per-group wall-clock cycles.
    enum class CoreGroup { NONE, HOST, DEVICE };
    CoreGroup core_group = CoreGroup::NONE;
    uint64_t cur_core_cycles = 0;  // last "cycles:" seen in the current core block

    /* 1.9.29: which node owns the cache scope we are inside. Caches sit in their
     * own top-level groups, not nested under the core group, so `core_group`
     * cannot answer this -- it needs its own tracker. */
    CoreGroup cache_group = CoreGroup::NONE;

    /* 1.9.29: learn the node names instead of hardcoding them. The config writer
     * emits core groups as "<node.name>_cores" for the host and "<node.name>_pes"
     * for a device (see the system-scope cfg writer), and caches as
     * "<node.name>_l1d" / "_l1i" / "_l2" / "_l3". Those cache groups appear AFTER
     * the core groups in the dump, so by the time a cache header is read the
     * names are known and the cache can be attributed to the right node without
     * assuming any particular name. */
    std::string host_node_name;
    std::string dev_node_name;

    auto extractScalarValue = [](const std::string& line) -> uint64_t {
        // Format: " statname: value # description"
        auto colon = line.find(':');
        if (colon == std::string::npos) return 0;
        std::string rest = line.substr(colon + 1);
        // Trim leading whitespace
        size_t start = rest.find_first_not_of(" \t");
        if (start == std::string::npos) return 0;
        rest = rest.substr(start);
        // If starts with '#', it's an aggregate header, not a scalar
        if (rest[0] == '#') return 0;
        // Extract number (stop at space or '#')
        size_t end = rest.find_first_of(" \t#");
        if (end != std::string::npos) rest = rest.substr(0, end);
        try { return std::stoull(rest); } catch (...) { return 0; }
    };

    auto getIndent = [](const std::string& line) -> int {
        int indent = 0;
        for (char c : line) {
            if (c == ' ') indent++;
            else break;
        }
        return indent;
    };

    auto getTrimmedKey = [](const std::string& line) -> std::string {
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) return "";
        auto colon = line.find(':', start);
        if (colon == std::string::npos) return "";
        return line.substr(start, colon - start);
    };

    std::string line;
    while (line_cursor < end_idx) {
        line = all_lines[line_cursor++];
        if (line.empty() || line[0] == '#' || line == "===") continue;

        int indent = getIndent(line);
        std::string key = getTrimmedKey(line);
        if (key.empty()) continue;

        // Check if this is an aggregate header: value after colon starts with "#"
        // (as opposed to a scalar like "cycles: 2328579 # Simulated cycles")
        bool is_aggregate = false;
        {
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                size_t after = line.find_first_not_of(" \t", colon + 1);
                if (after != std::string::npos && line[after] == '#') {
                    is_aggregate = true;
                }
            }
        }

        // If we went back to same or lower indent than scope header, leave scope
        if (scope != Scope::NONE && scope != Scope::ROOT && indent <= scope_indent) {
            scope = Scope::ROOT;
        }

        // Core-group detection: "host_cores"/"device_pes"/"cores" aggregate headers.
        // These are NOT cache scopes; core scalars are read at ROOT scope, so we only
        // tag which group's cores we are currently traversing.
        if (is_aggregate) {
            /* 1.9.29: match the "<name>_cores" / "<name>_pes" convention rather
             * than the literal names, and record the node name so the cache
             * groups below can be attributed to the same node. Headers appear
             * both bare ("host_cores") and per-instance ("host_cores-0"), so
             * compare against the part before any "-N" suffix. */
            std::string base = key.substr(0, key.find('-'));
            auto endsWith = [](const std::string& s, const std::string& suf) {
                return s.size() > suf.size() &&
                       s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
            };
            if (endsWith(base, "_cores")) {
                core_group = CoreGroup::HOST;
                host_node_name = base.substr(0, base.size() - 6);
            } else if (endsWith(base, "_pes")) {
                core_group = CoreGroup::DEVICE;
                dev_node_name = base.substr(0, base.size() - 4);
            } else if (key.substr(0, 10) == "host_cores") {
                core_group = CoreGroup::HOST;
                host_node_name = "host";
            } else if (key.substr(0, 10) == "device_pes") {
                core_group = CoreGroup::DEVICE;
                dev_node_name = "device";
            }
        }

        // Scope detection: match "l1d-N:", "l1i-N:", "l2-N:", "mem-N:" aggregate headers
        if (is_aggregate) {
            /* 1.9.29: strip an optional "<node>_" prefix before matching, and
             * remember which node it named.
             *
             * The system-scope config writer emits caches as "<node.name>_l1d",
             * so a co-sim dump carries "host_l1d-0" and friends. The pre-1.9.29
             * matcher tested the RAW key against "l1d-"/"l1i-"/"l2-"/"l3-",
             * which a prefixed name never satisfies -- so in system scope no
             * cache scope was ever entered, every cache counter parsed as zero,
             * and all cache dynamic power was priced at zero activity. The
             * device-scope writer emits bare "l1d", which is why that path was
             * unaffected and the defect stayed invisible. Same failure mode as
             * the 1.9.24 Garnet key-prefix mismatch. */
            std::string ck = key;
            std::string node_prefix;
            {
                size_t us = ck.rfind('_');
                if (us != std::string::npos && us + 1 < ck.size()) {
                    std::string tail = ck.substr(us + 1);
                    if (tail.compare(0, 3, "l1d") == 0 || tail.compare(0, 3, "l1i") == 0 ||
                        tail.compare(0, 2, "l2") == 0  || tail.compare(0, 2, "l3") == 0) {
                        node_prefix = ck.substr(0, us);
                        ck = tail;
                    }
                }
            }
            // Attribute by the learned node names; an unprefixed cache (the
            // device-scope single-node dump) stays ungrouped and feeds only the
            // all-nodes totals, exactly as before.
            CoreGroup cg = CoreGroup::NONE;
            if (!node_prefix.empty()) {
                if (!host_node_name.empty() && node_prefix == host_node_name) cg = CoreGroup::HOST;
                else if (!dev_node_name.empty() && node_prefix == dev_node_name) cg = CoreGroup::DEVICE;
                else if (node_prefix == "host") cg = CoreGroup::HOST;
                else cg = CoreGroup::DEVICE;
            }
            auto isScope = [&ck](const char* n, size_t len) {
                // matches "l1d", "l1d-0", "l1d_0"
                if (ck.compare(0, len, n) != 0) return false;
                return ck.size() == len || ck[len] == '-' || ck[len] == '_';
            };
            if (isScope("l1d", 3)) {
                scope = Scope::L1D;
                scope_indent = indent;
                cache_group = cg;
                continue;
            } else if (isScope("l1i", 3)) {
                scope = Scope::L1I;
                scope_indent = indent;
                cache_group = cg;
                continue;
            } else if (isScope("l2", 2)) {
                scope = Scope::L2;
                scope_indent = indent;
                cache_group = cg;
                continue;
            } else if (isScope("l3", 2)) {
                scope = Scope::L3;
                scope_indent = indent;
                cache_group = cg;
                continue;
            } else if (key.substr(0, 4) == "mem-" || key.substr(0, 4) == "mem_"
                       || key.substr(0, 6) == "pe-mc-" || key.substr(0, 6) == "pe-mi-") {
                scope = Scope::MEM;
                scope_indent = indent;
                /* 1.9.29: accept "pe-mi-" -- the name the simulator actually emits.
                 *
                 * The device PE-side controller is registered as "pe-mi-<N>"
                 * ("PE memory interface", external/zsim/src/init.cpp), but this
                 * matcher only ever tested for "pe-mc-", which nothing emits. In
                 * DEVICE scope the PE-MIs are the ONLY memory group in the dump,
                 * so mem_rd/mem_wr parsed as zero and the simulator's own DRAM
                 * energy report printed "Total dynamic: 0.0 mJ" on every fig2 and
                 * fig3 cell -- while the same log carried 7.4M reads and 744M
                 * writes in its pe-mi-N groups. The published DRAM energy was not
                 * affected: the analysis scripts grep the raw rd:/wr: lines out of
                 * the log rather than trusting this value.
                 *
                 * Third instance of one interface naming a thing differently on
                 * each side (1.9.24 Garnet dotted keys, 1.9.29 node-prefixed cache
                 * groups, this). A name-contract check between the emitters and
                 * this reader belongs in 1.16 (#96).
                 *
                 * Attribution: the host controller is "mem-N"; the device PE-side
                 * interface is "pe-mi-N"/"pe-mc-N". In CO-SIM the PE-MIs are not
                 * registered at all (init.cpp clears them, leaving the host's
                 * "mem-N"), so this correctly charges co-sim "mem-N" to the host. */
                bool device_mi = (key.substr(0, 6) == "pe-mc-" ||
                                  key.substr(0, 6) == "pe-mi-");
                cache_group = device_mi ? CoreGroup::DEVICE : CoreGroup::HOST;
                continue;
            }
        }

        // Extract scalars within scope
        if (!is_aggregate) {
            uint64_t val = extractScalarValue(line);

            /* 1.9.29: the group whose counters this line belongs to, or null when
             * the dump is a single-node (device-scope) one, where only the
             * all-nodes totals are used. */
            ZSimParsedOutput::GroupCounters* grp =
                (core_group == CoreGroup::HOST)   ? &out.host :
                (core_group == CoreGroup::DEVICE) ? &out.dev  : nullptr;
            ZSimParsedOutput::GroupCounters* cgrp =
                (cache_group == CoreGroup::HOST)   ? &out.host :
                (cache_group == CoreGroup::DEVICE) ? &out.dev  : nullptr;

            // Top-level stats
            if (scope == Scope::NONE || scope == Scope::ROOT) {
                if (key == "cycles" && out.cycles == 0) out.cycles = val;
                /* 1.11.9 (#86, audit): instrs is SUMMED across cores, like every
                 * other activity counter. It used to latch the FIRST core's
                 * value while uops/bbls/branches/syntheticInstrs were all-core
                 * sums -- THE unexplained "different bases" the 1.9.28 mix gate
                 * documented and could not account for. Measured on a 16-PE
                 * HBM3 cell: first core 51,660 vs true total 512,788, so McPAT
                 * (which divides by num_cores to get its per-core figure) was
                 * modelling each PE as doing a tenth of its actual work, and
                 * the self-consistency guard then rejected the measured mix and
                 * fell back to documented fractions. Cycles stay first-core:
                 * they are a DURATION, not work, and summing them would be the
                 * mirror-image error. */
                if (key == "instrs") {
                    out.instrs += val;
                    if (grp) { grp->instrs += val; grp->seen = true; }
                }
                /* 1.9.28: accumulate measured activity across cores.
                 * 1.9.29: and per node -- the all-nodes totals below cannot be
                 * split by core-count proportion without making each node's mix
                 * inconsistent with its own instruction count. */
                else if (key == "xingH2DBytes") { out.xing_h2d_bytes = val; }
                else if (key == "xingD2HBytes") { out.xing_d2h_bytes = val; }
                else if (key == "xingCount") { out.xing_count = val; }
                else if (key == "xingFlushBytes") { out.xing_flush_bytes = val; }
                else if (key == "mixInt") { out.mix_int += val; if (grp) grp->mix_int += val; }
                else if (key == "mixMul") { out.mix_mul += val; if (grp) grp->mix_mul += val; }
                else if (key == "mixFp")  { out.mix_fp  += val; if (grp) grp->mix_fp  += val; }
                else if (key == "mixLd")  { out.mix_ld  += val; if (grp) grp->mix_ld  += val; }
                else if (key == "mixSt")  { out.mix_st  += val; if (grp) grp->mix_st  += val; }
                else if (key == "mixBr")  { out.mix_br  += val; if (grp) grp->mix_br  += val; }
                else if (key == "pgAnyCoreActivePhases") { out.pg_anycore_active = val; }
                else if (key == "pgSharedCacheActivePhases") { out.pg_sharedcache_active = val; }
                else if (key == "pgNocActivePhases") { out.pg_noc_active = val; }
                else if (key == "pgHostMCActivePhases") { out.pg_hostmc_active = val; }
                else if (key == "pgDevMCActivePhases") { out.pg_devmc_active = val; }
                else if (key == "phase") { out.phases = val; }
                /* 1.11.18: the PG denominator is the PRICED window (phases
                 * since roi_begin), matching the now-ROI-relative numerators.
                 * Absent on pre-1.11.18 dumps -> falls back to `phase`. */
                else if (key == "pgPhaseWindow") { out.pg_phase_window = val; }
                else if (key == "pgDevMCGapSpan") { out.devmc_gap_span = val; }
                else if (key.rfind("pgDevMCGapCnt_", 0) == 0) {
                    int b = std::atoi(key.c_str() + 14);
                    if (b >= 0 && b < ZSimParsedOutput::kGapBuckets)
                        out.devmc_gap_cnt[b] = val;
                }
                else if (key.rfind("pgDevMCGapCyc_", 0) == 0) {
                    int b = std::atoi(key.c_str() + 14);
                    if (b >= 0 && b < ZSimParsedOutput::kGapBuckets)
                        out.devmc_gap_cyc[b] = val;
                }
                else if (key == "syntheticInstrs") {
                    out.syntheticInstrs += val; if (grp) grp->syntheticInstrs += val;
                }
                else if (key == "uops") { out.uops += val; if (grp) grp->uops += val; }
                else if (key == "bbls") { out.bbls += val; if (grp) grp->bbls += val; }
                else if (key == "branches") { out.branches += val; if (grp) grp->branches += val; }
                else if (key == "mispredBranches") {
                    out.mispredBranches += val; if (grp) grp->mispredBranches += val;
                } else if (key == "indirBranches") {
                    out.indirBranches += val; if (grp) grp->indirBranches += val;
                } else if (key == "rasReturns") {
                    out.rasReturns += val; if (grp) grp->rasReturns += val;
                } else if (key == "pgActivePhases") {
                    /* 1.11.8: per-core PG residency, summed per group; each
                     * PE's own residency = its stat / phases, and the group
                     * MEAN residency = sum / (numCores*phases). */
                    if (grp) grp->pgActivePhases += val;
                }

                // 1.9.10: contention-inclusive per-group wall-clock cycles.
                if (key == "cycles") {
                    cur_core_cycles = val;
                    if (grp) grp->seen = true;   // 1.9.35: observed, even if idle
                    if (core_group == CoreGroup::HOST)
                        out.host_wall_cycles = std::max(out.host_wall_cycles, val);
                    else if (core_group == CoreGroup::DEVICE)
                        out.dev_wall_cycles = std::max(out.dev_wall_cycles, val);
                } else if (key == "cCycles" && core_group == CoreGroup::HOST) {
                    // Host cores report "cCycles" (contention stall cycles) separately
                    // from unhalted "cycles"; the true elapsed time is their sum.
                    out.host_wall_cycles =
                        std::max(out.host_wall_cycles, cur_core_cycles + val);
                }
            }

            /* Each cache/MC counter feeds BOTH the all-nodes total (used by the
             * device-scope path) and the owning node's own set (1.9.29, used by
             * the system-scope path). `cgrp` is null for an unprefixed cache, so
             * a single-node dump behaves exactly as it did before. */

            // L1D scope
            if (scope == Scope::L1D) {
                if (key == "fhGETS") { out.l1d_fhGETS += val; if (cgrp) cgrp->l1d_fhGETS += val; }
                else if (key == "hGETS") { out.l1d_hGETS += val; if (cgrp) cgrp->l1d_hGETS += val; }
                else if (key == "mGETS") { out.l1d_mGETS += val; if (cgrp) cgrp->l1d_mGETS += val; }
                else if (key == "fhGETX") { out.l1d_fhGETX += val; if (cgrp) cgrp->l1d_fhGETX += val; }
                else if (key == "hGETX") { out.l1d_hGETX += val; if (cgrp) cgrp->l1d_hGETX += val; }
                else if (key == "mGETXIM") { out.l1d_mGETXIM += val; if (cgrp) cgrp->l1d_mGETXIM += val; }
            }

            // L1I scope
            if (scope == Scope::L1I) {
                if (key == "fhGETS") { out.l1i_fhGETS += val; if (cgrp) cgrp->l1i_fhGETS += val; }
                else if (key == "hGETS") { out.l1i_hGETS += val; if (cgrp) cgrp->l1i_hGETS += val; }
                else if (key == "mGETS") { out.l1i_mGETS += val; if (cgrp) cgrp->l1i_mGETS += val; }
            }

            // L2 scope
            if (scope == Scope::L2) {
                if (key == "hGETS") { out.l2_hGETS += val; if (cgrp) cgrp->l2_hGETS += val; }
                else if (key == "mGETS") { out.l2_mGETS += val; if (cgrp) cgrp->l2_mGETS += val; }
                else if (key == "hGETX") { out.l2_hGETX += val; if (cgrp) cgrp->l2_hGETX += val; }
                else if (key == "mGETXIM") { out.l2_mGETXIM += val; if (cgrp) cgrp->l2_mGETXIM += val; }
                else if (key == "PUTS") out.l2_PUTS += val;
                else if (key == "PUTX") out.l2_PUTX += val;
            }

            // L3 scope
            if (scope == Scope::L3) {
                if (key == "hGETS") { out.l3_hGETS += val; if (cgrp) cgrp->l3_hGETS += val; }
                else if (key == "mGETS") { out.l3_mGETS += val; if (cgrp) cgrp->l3_mGETS += val; }
                else if (key == "hGETX") { out.l3_hGETX += val; if (cgrp) cgrp->l3_hGETX += val; }
                else if (key == "mGETXIM") { out.l3_mGETXIM += val; if (cgrp) cgrp->l3_mGETXIM += val; }
            }

            // Memory controller scope. Both the host MC and the PE-MC export
            // rd/wr counters; localAcc/remoteAcc are a LOCALITY split of the
            // same accesses (every access is also rd or wr) and must not be
            // added on top -- that double-counted reads and priced remote
            // accesses as writes.
            if (scope == Scope::MEM) {
                /* 1.11.15 (audit): localAcc/remoteAcc are CHILDREN of the
                 * pe-mi-<N> aggregate, i.e. Scope::MEM -- parsing them at ROOT
                 * scope left them zero forever and the locality report never
                 * printed. They are a locality SPLIT, not memory accesses:
                 * do not add them to mem_rd/mem_wr. */
                if (key == "rowHits")   { out.row_hits += val; }
                else if (key == "rowMisses") { out.row_misses += val; }
                else if (key == "localAcc")  { out.pemi_local_acc += val; }
                else if (key == "remoteAcc") { out.pemi_remote_acc += val; }
                if (key == "rd") { out.mem_rd += val; if (cgrp) cgrp->mem_rd += val; }
                else if (key == "wr") { out.mem_wr += val; if (cgrp) cgrp->mem_wr += val; }
            }
        }
    }
    return out;
}

// Minimal includes to avoid header conflicts
// Full model integration happens through runtime configuration
#include "common/types.h"
#include "power/mcpat_wrapper.h"
#include "power/cacti_io_wrapper.h"   // 1.11.42 (E21): derived link clock

// YAML parsing (if available)
#ifdef HAVE_YAML_CPP
#include <yaml-cpp/yaml.h>
#endif

using namespace pimid;

//=============================================================================
// Configuration Structure
//=============================================================================

struct UnifiedConfig {
    // Simulation dimensions
    std::string method;  // "exec", "trace", or "trace-gen"
    std::string scope;   // "device" or "system" ("cosim" remapped to "system")

    // Trace options
    std::string trace_file;      // Trace file path (input or output)
    // generate_trace removed -- use method "trace-gen" instead

    // Common metadata
    std::string name;
    std::string description;

    // Memory configuration
    std::string memory_tech;
    int num_banks;
    int subarrays_per_bank;
    bool subarrays_per_bank_user_set = false;  // true when user set the count directly
    int subarray_height = 0;                    // rows per subarray; 0 = per-tech default
    int bg_per_chip_override = 0;                // bank-groups/chip; 0 = per-tech JEDEC default
    int banks_per_bg_override = 0;              // banks/bank-group; 0 = per-tech JEDEC default
    int memory_latency_override;  // -1 means auto-derive from tech
    int ports_per_bank = 1;       // Physical RW ports per bank (SRAM multiport, DRAM/NVM = 1)
    // DRAM device width (x4/x8/x16). Empty = use the technology's JEDEC default.
    // Selects both the Ramulator2 org preset and PIMID's chips/rank + BG/chip
    // so the timing model and the in-memory hierarchy stay consistent.
    std::string dram_device_width;

    // Comprehensive memory characteristics (for YAML override of external models)
    // If use_yaml_memory_params is true, user provided complete params in YAML
    bool use_yaml_memory_params = false;  // true = use YAML values, false = use external models

    struct MemoryParams {
        // REQUIRED for YAML override (5 parameters):
        double read_latency_ns = -1.0;      // Read latency in nanoseconds
        double write_latency_ns = -1.0;     // Write latency in nanoseconds
        double read_energy_nj = -1.0;       // Read energy in nanojoules
        double write_energy_nj = -1.0;      // Write energy in nanojoules
        double static_power_mw = -1.0;      // Static/leakage power in milliwatts

        // Check if user provided ALL required parameters
        bool isComplete() const {
            return read_latency_ns > 0 &&
                   write_latency_ns > 0 &&
                   read_energy_nj > 0 &&
                   write_energy_nj > 0 &&
                   static_power_mw > 0;
        }
    } memory_params;

    // Memory controller config (auto-derived from technology)
    std::string zsim_mem_controller_type = "auto"; // auto, simple, weavesimple, ramulator
    int md1_bandwidth_mbs = 6400;
    bool md1_bandwidth_user_set = false;  // true when YAML bandwidth_mbs is specified

    // WeaveSimple params
    int weave_bound_latency = -1;  // -1 = same as mem_latency (default)

    // Ramulator params
    std::string ramulator_config_file = "";  // Path to Ramulator2 YAML config

    // ZSim output directory (set at runtime, used to find zsim.out / garnet_stats.txt)
    std::string zsim_output_dir = "";

    // PE configuration
    std::string pe_type;
    std::string placement_level;
    int num_pes;

    /* 1.9.32: the compute unit's datapath, as configuration rather than as a
     * constant buried in the power model. These four say what the element IS:
     * how many values it operates on at once, how wide each one is, whether it
     * can do floating point, and how much program it can hold. The power and
     * area models read them directly -- before this they existed in the power
     * configuration but nothing ever set them, so every element was described
     * with the same fixed defaults regardless of what the timing model ran.
     *
     * Defaults: a scalar 32-bit element with floating point, because three of
     * the five kernels (stream_triad, gemv, stencil_2d) are FP32 and the other
     * two (histogram, bfs) are INT32 -- so an element that cannot do FP32
     * cannot run most of the suite. 4 KB of instruction memory sits between a
     * command-driven in-bank engine and a fully programmable near-bank one. */
    int  pe_lanes = 1;
    bool pe_has_fp = true;
    /* 1.11.11 (#113): cycles charged per FP-class instruction when the
     * element has no FPU (soft-float emulation). Default 0 = the pre-1.11.11
     * behaviour (nothing charged), so no existing run moves; the run now
     * REPORTS the contradiction either way. A documented starting point for
     * users who want it priced: soft-float add/mul on an integer datapath is
     * tens of integer operations (glibc soft-fp, ~20-60 depending on op and
     * width) -- the knob is deliberately not defaulted to an invented
     * constant. */
    uint32_t pe_fp_emul_cycles = 0;
    /* 1.11.13 (#121): device CORNER for LOGIC-family components. CACTI's
     * tables carry three logic corners -- hp (high performance), lstp (low
     * standby power), lop (low operating power) -- and McPAT already selects
     * between them via sys.device_type; the choice was simply never exposed,
     * so every logic domain was priced hp. Default "hp" keeps that exactly.
     *
     * There is NO corner axis for the DRAM-periphery family, and that is a
     * property of the DATA, not a decision: each table holds exactly ONE
     * commodity-DRAM device column (lp-dram, the only alternative, is
     * all-zero at 22 nm). A speed corner for HBM periphery or a mobile
     * corner for LPDDR5 periphery cannot be derived from tables that do not
     * contain them, so the request is refused there and says why. */
    std::string device_corner = "hp";   // power.device_corner: hp|lstp|lop
    /* 1.11.30 (user ruling E5): ONE interconnect projection for the whole die.
     * CACTI was pinned to conservative and McPAT defaulted to aggressive, so
     * the same metal stack was modelled two ways -- arrays and caches lossier,
     * cores/NoC/controllers idealised.
     *
     * Default CONSERVATIVE, on physics rather than caution: the aggressive
     * column sets barrier_thickness = 0 at EVERY node (22/32/45nm), and a
     * copper wire with no diffusion barrier is not manufacturable -- Cu
     * diffuses into SiO2 and the barrier does not scale with the wire. It also
     * holds alpha_scatter = 1.00, i.e. no surface/grain-boundary scattering,
     * which measurement contradicts once wire dimensions approach the electron
     * mean free path (hence conservative's 1.05 at 22nm and 1.00 at 32/45).
     * Aggressive is the ITRS TARGET projection; the industry did not meet it
     * on interconnect resistivity. Conservative describes silicon that exists,
     * which is also what our FIMDRAM/Sohn area anchors are measurements of.
     * 0 = aggressive, 1 = conservative. */
    int interconnect_projection = 1;   // power.interconnect_projection
    /* 1.11.35 (user ruling E13): the LOGIC REFERENCE clock, a user setting.
     * The feasibility bound for a DRAM-periphery PE is calculated -- reference
     * divided by the CV/I delay ratio read from the CACTI columns -- but a
     * ratio is dimensionless and needs an anchor, and the anchor cannot come
     * from the PE's own clock without the test becoming circular (an earlier
     * draft did exactly that and warned on every device-scope run).
     * So the user states what logic process this design is being compared
     * against; PIMID calculates the ceiling from it and warns when the
     * requested PE clock exceeds it. 0 = unset: in co-sim the host's clock is
     * used, and in device scope the bound is skipped with a printed reason
     * rather than invented. */
    double logic_reference_mhz = 0.0;   // power.logic_reference_mhz
    int  pe_imem_bytes = 4096;
    /* Datapath WIDTH is deliberately absent here. It already exists as
     * alu_operand_width (pim.pe.operand_width), which the timing model reads as
     * ALUCore::operandWidth. 1.9.39 added a second field for the power model and
     * parsed it separately -- two names for one physical quantity, free to
     * disagree the moment anyone set either one. That is the exact pattern this
     * release train exists to remove, so the second name is gone and the power
     * model reads the same field the timing model does. */

    // Simulator parallelism (YAML: simulation.parallel, default true). ONE knob for
    // both OMP and MPI paths: true = parallelize the simulation whenever it
    // is safe to do so, false = force a serial simulation. OpenMP is exact at
    // any simulator thread count (work-locked phases), so it parallelizes to
    // the simulated core count. MPI is always simulated serially today for
    // determinism regardless of this knob; when parallel MPI simulation
    // becomes safe it will start honoring sim_parallel with no config change.
    bool sim_parallel = true;

    // ALU scaling factors (5 design-point factors)
    double alu_compute_factor;     // cycles-per-instruction multiplier (default 1.0)
    double alu_access_factor;      // cycles per load/store (default 1.0)
    double alu_throughput_factor;  // parallelism divider (default 1.0)
    int    alu_operand_width;      // operand bit-width (default 32)
    double alu_energy_factor;      // per-op energy scale for reporting (default 1.0)
    bool   alu_bit_serial = false; // bit-serial datapath (false = parallel; default)

    // In-order PE issue width (pim.pe.issue_width; in_order_core only).
    // Default 2 (dual-issue) = the core's historical hardcoded value, so
    // configs without the key are numerically unchanged. The env var
    // PIMID_INORDER_WIDTH, if set, overrides this inside the core.
    int    inorder_issue_width = 2;

    // System configuration
    int frequency_mhz;
    int cache_line_size;
    int tech_node_nm;  // Technology node for CACTI queries (default 22nm)

    // Cache configuration
    int l1d_size_kb;
    int l1d_ways;
    int l1i_size_kb;
    int l1i_ways;
    int l2_size_kb;
    int l2_ways;
    bool enable_l2;
    int l2_count;        // Number of L2 instances (default 1 = shared by all PEs)

    // L3 cache (optional, unified LLC above clustered L2)
    bool enable_l3;
    int l3_size_kb;
    int l3_ways;

    // Cache timing/energy/power overrides (mirrors MemoryParams pattern)
    struct CacheParams {
        double latency_ns = -1.0;       // Access latency in nanoseconds
        double energy_nj = -1.0;        // Access energy in nanojoules
        double static_power_mw = -1.0;  // Static/leakage power in milliwatts

        bool isComplete() const {
            return latency_ns > 0 && energy_nj > 0 && static_power_mw > 0;
        }
    } l1d_params, l1i_params, l2_params, l3_params;

    bool use_yaml_cache_params = false;  // true if ALL active cache levels have complete params

    // NoC configuration
    std::string noc_topology;
    bool noc_topology_user_set = false;  // true when YAML noc.topology is specified
    int noc_router_latency;
    int noc_link_latency;
    // Channel-aware Garnet H-tree link latency for DRAM detailed/parallel
    // (accuracy fix). 0 = use noc_link_latency (no channel-aware override).
    // Set from per-channel DRAM bandwidth so detailed differentiates techs by
    // bandwidth; only the cycle-accurate Garnet network reads it (analytical
    // simple/calibrated/curve/approximate use noc_link_latency, unchanged).
    int noc_garnet_link_latency = 0;
    bool noc_cycle_accurate;
    std::string noc_routing;              // empty = default for topology
    int noc_vcs_per_vnet;
    int noc_buffers_per_vc;
    // 1.10.3: did the user actually ask for these, or are they carrying the
    // built-in default? The per-technology fabric defaults below must not
    // overwrite an explicit choice.
    bool noc_vcs_user_set = false;
    bool noc_buffers_user_set = false;
    std::string noc_topology_file;        // required for CUSTOM topology
    std::string noc_routing_table_file;   // optional for TABLE routing
    int noc_control_msg_bits;             // 0 = default (64 bits)
    int noc_data_msg_bits;                // 0 = default (576 bits for cacheline)
    bool noc_ring_unidirectional;         // true = unidirectional ring (CW only)

    // Per-level network model choice ("simple", "md1", or "detailed")
    // Indexed by hierarchy level (0=subarray, ..., 6=system)
    // "analytical" accepted as backward-compat alias for "simple"
    std::array<std::string, 7> network_level_model = {
        // DEFAULT noc model is detailed; noc.model=analytical switches every
        // level to "simple" (the per-level analytical path).
        "detailed", "detailed", "detailed", "detailed",
        "detailed", "detailed", "detailed"
    };
    // 1.11.56 (audit B058): true where noc.levels[i].model was explicitly
    // given, so a blanket device-level model does not overwrite it.
    std::array<bool, 7> network_level_model_user_set = {
        false, false, false, false, false, false, false
    };

    // Per-level network link overrides (optional, for forward-looking research)
    // -1 / empty / NaN = use technology default
    struct NetworkLevelOverride {
        int link_width_bits = -1;
        double frequency_ghz = -1.0;
        int latency_cycles = -1;
        std::string topology;    // empty = use tech default
        // Router params (negative = use tech default)
        int router_latency = -1;
        int router_pipeline = -1;   // 0=FULL, 1=REDUCED, 2=SIMPLE, 3=MINIMAL
        int router_bypass = -1;     // 0=off, 1=on
        int virtual_networks = -1;
        int virtual_channels_per_vn = -1;
        int input_buffer_depth = -1;
        int output_buffer_depth = -1;
    };
    std::array<NetworkLevelOverride, 7> network_level_overrides;

    // Per-boundary bridge overrides (optional)
    // Index 0=L0<->L1, 1=L1<->L2, ..., 5=L5<->L6
    // Bridge model is auto-derived from adjacent tier network models
    struct BridgeOverride {
        /* 1.11.57 (latent B030): count, lower/upper_frequency_mhz and
         * fifo_depth are gone. They were written by the YAML parser and read
         * by nothing -- the bridge override interface takes router and link
         * parameters only -- so setting them changed no emitted value. The
         * corresponding keys are now refused at parse time. */
        int lower_width_bits = -1;
        int upper_width_bits = -1;
        double latency_ns = -1.0;
        // Backward compat (legacy single-width fields)
        int width_bits = -1;             // sets both lower + upper
        int latency_cycles = -1;         // converted to latency_ns
        // Independent bridge model ("" = AUTO, "simple", "md1", "detailed")
        std::string model;
        // Router params (negative = use tech default)
        int router_latency = -1;
        int router_pipeline = -1;
        int router_bypass = -1;
        int virtual_networks = -1;
        int virtual_channels_per_vn = -1;
        int input_buffer_depth = -1;
        int output_buffer_depth = -1;
    };
    std::array<BridgeOverride, 6> bridge_overrides;

    // Pre-computed hierarchy latencies (populated by computeHierarchyLatencies)
    std::array<int, 7> hierarchy_level_latency = {0,0,0,0,0,0,0};
    std::array<int, 6> hierarchy_bridge_latency = {0,0,0,0,0,0};
    std::array<std::string, 6> hierarchy_bridge_model = {"auto","auto","auto","auto","auto","auto"};
    int pe_hierarchy_level = 1;  // 0=SUBARRAY, 1=BANK, 2=BANK_GROUP, 3=CHIP, 4=RANK
    int hierarchy_banks_per_bg = 4;
    int hierarchy_bg_per_chip = 4;
    int hierarchy_chips_per_rank = 8;
    int hierarchy_ranks_per_channel = 1;    // >1 for multi-rank (L5)
    /* 1.11.57 (latent B031): hierarchy_channels_per_system is GONE.
     *
     * It was declared "1, >1 for multi-device (L6)", emitted to the plugin as
     * channelsPerSystem and read at the L6 root-width derivation -- and it had
     * NO writer. Not a YAML key, not a synthesis step, nothing: it was 1 in
     * every run that has ever executed, so the multiplication it fed was the
     * identity and the emitted key was a constant. On the plugin side it
     * reaches zsim.h and stops there. A configurable-looking field pinned to
     * one value is worse than a constant, because the next reader believes the
     * L6 width already scales with the system's channel count. It does not.
     * Making it real needs three things this file cannot supply on its own -- a
     * key, a census of channels across devices, and a plugin consumer -- so the
     * field is removed and both use sites state the constant they always had. */
    // TEMP (pre-arXiv): parallel DRAM channel count for the M/D/c contention
    // stop-gap (HBM3=16, HBM2=8, others=1). Set in the DRAM H-tree block.
    int hierarchy_dram_channels = 1;
    /* 1.11.59 (audit round 3, B005): the ADOPTED per-level ladder, carried so
     * the Garnet topology builder draws the same fabric the latency model
     * prices. Empty width array = no ladder was adopted for this technology
     * (it did not reconcile, or it is not DRAM), and the builder keeps its
     * per-technology table. */
    std::array<int, 7>    sourced_ladder_w  = {0,0,0,0,0,0,0};
    std::array<double, 7> sourced_ladder_bw = {0,0,0,0,0,0,0};
    bool sourced_ladder_valid = false;
    /* 1.11.63 (R6-7, audit round 4 B018): which rungs of the adopted ladder
     * are NOT sourced -- bit i set = rung i came back with a non-positive
     * width or bandwidth, so applySourcedLadder() skipped it and that level
     * kept the per-technology PLACEHOLDER table. 0 = every rung is sourced,
     * which is the case for all four technologies whose ladder is adopted
     * today. Carried so the announcement can name them and the stated-constant
     * register can count them, instead of "sourced" being claimed for a
     * mixture. */
    int sourced_ladder_placeholder_mask = 0;
    // Real datasheet AGGREGATE sustainable bandwidth (MB/s) of the memory tech,
    // from Ramulator (getBandwidth() = per-channel x channels). Used by the
    // detailed NoC model to cap effective DRAM bandwidth at the channel
    // bottleneck (a shared M/D/c queue across all bank-MIs of a channel), so
    // detailed is an accurate DRAM ground truth instead of permitting
    // num_banks x per-MI-BW (which is ~10x the real DDR4 channel). 0 = unknown
    // (no cap; pre-fix behavior). Set in the DRAM H-tree block.
    long long hierarchy_agg_bandwidth_mbs = 0;
    /* 1.10.6: DQ-bus turnaround -- the delay charged when the shared channel
     * data bus reverses direction. 0 = off. Default ON for DRAM.
     *
     * 1.11.57 (latent B052): the field is NANOSECONDS x100 and was named
     * "..._cycles", with a declaration comment that said "Cycles (network
     * clock)". Everything that touches the value already treated it as ns x100
     * -- the emitted key is dqTurnNsX100, the plugin field is dqTurnNsX100, and
     * the plugin divides by 100 and converts at its own clock -- so no number
     * was wrong. What was wrong was the one name a reader meets first: anybody
     * adding a second consumer would have spent it as cycles at whatever clock
     * was to hand. Renamed to carry the unit rather than converted, because the
     * ns x100 fixed-point form is what the plugin's integer field wants. */
    int hierarchy_dq_turn_ns_x100 = 0;
    bool dq_turnaround_enabled = true;   // memory.dq_turnaround: false to disable
    bool hierarchy_enabled = false;

    // PE-MI distributed memory interface config
    bool pe_mc_enabled = false;             // true when pim.mc section present in YAML
    std::string pe_mc_type = "simple";     // always "simple" (M/D/1 always active)
    int pes_per_mc = 1;                     // PEs sharing each MI (0 = host MC mode)
    bool pes_per_mc_user_set = false;       // true when user explicitly set pes_per_mc
    int pe_mc_local_latency = -1;           // -1 = auto from technology
    int pe_mc_bandwidth_mbs = -1;           // -1 = auto from technology
    // MC placement: false = with_core (default; MC co-located with core/cache),
    // true = standalone (MC is its own NoC endpoint, one per memory org, so even
    // a local PE access incurs an extra NoC hop to reach the MC fronting memory).
    bool mc_standalone = false;
    struct PEMCGroupOverride {
        // 1.11.57 (latent B032): the `type` field is gone -- it was written by
        // the parser and read by nothing, and the emitted per-group block
        // carries only bandwidthMBs and localLatency. The key now warns at
        // parse time instead of being stored.
        std::vector<int> ids;
        int local_latency = -1;
        int bandwidth_mbs = -1;
    };
    std::vector<PEMCGroupOverride> pe_mc_group_overrides;

    // PE-to-Memory-Organization mapping (M:N connectivity)
    // Connection mode: shared_io (PE embedded in mem org) or separate_endpoints (own NI)
    enum class PEMemConnectionMode { SHARED_IO, SEPARATE_ENDPOINTS };
    PEMemConnectionMode pe_mem_connection = PEMemConnectionMode::SHARED_IO;
    int local_link_latency = 2;  // cycles for PE<->local-mem hop (separate_endpoints only)

    // Explicit PE-to-mem-org mapping table
    struct PEMemMapping {
        int pe_id;
        std::vector<int> mem_org_ids;
    };
    std::vector<PEMemMapping> pe_mem_map;  // empty = auto 1:1

    // Derived: total memory org units at placement level
    int total_mem_orgs = -1;  // -1 = auto from memory organization
    int pages_per_unit = 32;  // contiguous block (pages) per placement-level unit; sized by subarray capacity
    // Derived: total network endpoints (depends on connection mode)
    int total_network_endpoints = -1;

    /* 1.10.4: what the placement tree ACTUALLY built, recorded when it is built.
     * The power model has been describing a different machine from the one the
     * timing model routes on -- a square mesh of one router per element -- because
     * it had no way to ask. These are that answer. -1 = no tree was built. */
    int htree_branch_routers = -1;   // routers with >= 2 children (real branch points)
    int htree_level_branch[7]    = {-1,-1,-1,-1,-1,-1,-1};  // 1.11: per-level census
    int htree_level_endpoints[7] = {-1,-1,-1,-1,-1,-1,-1};
    int htree_all_routers    = -1;   // including degenerate single-child pass-throughs
    int htree_endpoints      = -1;   // PE endpoints + aggregated-region endpoints
    int htree_abstract       = -1;   // of those, the aggregated regions
    // Derived: topology-aware NoC average one-way latency (cycles)
    int noc_avg_one_way_latency = 0;
    // Derived: bisection bandwidth in links (for M/D/1 contention)
    int noc_bisection_links = 1;
    // Derived: NoC physical parameters for simple model
    int noc_flits_per_packet = 5;   // ceil(data_msg_bits / link_width_bits)
    int noc_per_hop_cycles = 2;     // router_lat + link_lat
    int noc_link_width_bits_cfg = 128;  // link width (flit size) in bits
    int noc_avg_hops_times_100 = 100;  // avgHops x 100 (fixed-point for shm)
    int noc_num_nodes = 16;            // total network nodes
    int noc_topology_class = 2;        // 0=BUS, 1=CROSSBAR, 2=MULTI_HOP
    int noc_total_channels = 32;       // total unidirectional channels
    int noc_hotspot_factor_100 = 100;  // hotspot factor x 100 (fixed-point)
    /* 1.11.57 (latent B035): noc_injector_calib, noc_curve_model and
     * noc_calqueue are gone.
     *
     * All three were pinned to 0 by construction: every assignment in the
     * parser set 0, and the parser accepts exactly two model names
     * (analytical, detailed) and hard-errors on anything else -- including the
     * "curve" and "calqueue" spellings the comments beside these fields named
     * as the way to turn them on. The plugin gates its probe paths on them, so
     * those paths were unreachable too. What was emitted was three constant
     * zeros described as switches. The keys are still emitted as literals so
     * the plugin's config lookups are unchanged. */
    int noc_mlp_model = 0;             // 1 = analytical (hop+M/D/1+MLP); set by noc.model=analytical.
                                       // DEFAULT noc model is detailed (see noc_cycle_accurate)
    /* 1.11.57 (latent B048): the second line of this comment used to say the
     * value was "resolved by defaultMlpForPeType at config emission". No such
     * function exists -- not in src/, include/ or the plugin -- and the
     * emission site does not read pe_type at all: it substitutes a single
     * number when no explicit noc.mlp was given. Said plainly now, because a
     * comment naming a per-core-model derivation is how a reader concludes the
     * MLP degree already tracks the core. */
    int noc_mlp_degree = -1;           // noc.mlp -> M = PE outstanding-access window;
                                       // -1 = no explicit setting -> one substituted
                                       // default at emission (NOT per pe_type)

    // Workload (required for both methods)
    std::string workload_binary;
    std::vector<std::string> workload_args;
    std::map<std::string, std::string> workload_env;

    // Simulation parameters
    int phase_length;
    long long max_instructions;
    int stats_interval;

    // Legacy co-sim field -- set when scope "cosim" was auto-remapped to "system"
    bool cosim_remapped = false;

    // Host processor config (for cosim->system synthesis)
    std::string host_core_type = "ooo_core";
    int host_num_cores = 4;
    double host_frequency_mhz = 3000.0;
    int host_l1d_kb = 32;
    int host_l1i_kb = 32;
    int host_l2_kb = 1024;
    int host_l3_kb = 8192;
    std::string host_memory_tech = "DDR4";
    int host_tech_node_nm = -1;  // -1 = inherit device node (config.tech_node_nm); see power.host_tech_node_nm
    // 1.11.2: extra area factor for SUBARRAY-placed PEs (bitline-pitch layout
    // constraint, DRISA-style). Default unity: no speculative precision until
    // the IDD cross-check shows the single-factor model failing.
    double subarray_pitch_factor = 1.0;  // power.subarray_pitch_factor

    // Power analysis
    bool enable_power = true;  // --power / --no-power / YAML power.enabled
    std::string power_report_detail = "standard";  // summary, standard, verbose
    std::map<std::string, double> mcpat_overrides;  // power.mcpat_overrides from YAML

    // PCIe transfer modeling (cosim->system synthesis + system links)
    bool pcie_enabled = true;       // power.pcie.enabled (default true for cosim)
    bool pcie_enabled_user_set = false;  // 1.11.47 (L176): explicit user choice wins
    int pcie_num_units = 1;         // power.pcie.num_units
    int pcie_num_channels = 16;     // power.pcie.num_channels (x16)
    /* 1.11.40 (E19): duty_cycle is DERIVED from measured bytes / elapsed
     * time in the co-sim path -- it is a quantity the simulation reveals.
     * This field is now an explicit OVERRIDE, applied only when the user
     * set it, and printed when it is. The 0.01 it used to default to had
     * no citation and was wrong by 4329x or 30x depending on regime. */
    double pcie_duty_cycle = 0.01;  // power.pcie.duty_cycle (override only)
    bool pcie_duty_cycle_user_set = false;
    double pcie_load_perc = 0.01;   // power.pcie.total_load_perc
    bool   pcie_load_perc_user_set = false;   // 1.11.56 (B015)
    // PCIe timing model (bandwidth + latency constrained link)
    bool pcie_timing_configured = false;  // set true when power.pcie section exists in YAML
    double pcie_base_latency_ns = 500.0;  // per-transaction overhead
    double pcie_bandwidth_GBs = 63.0;     // peak unidirectional throughput
    int pcie_num_lanes = 16;              // lane count (also feeds McPAT num_channels)
    double pcie_pj_per_bit_override = -1.0;  // 1.11.7: user pJ/bit for unknown link types (printed)
    /* 1.11.8 (#84): per-component power-gating flags (v7 spec). Default
     * FALSE: a config with no pg: keys is bit-identical to 1.11.7. */
    bool pg_pe = false;    // pim.pe.pg      (PE cores + their caches)
    bool pg_noc = false;   // noc.pg         (device tree fabric)
    /* 1.11.41 (user ruling): pim.mc.pg gates the memory CONTROLLER -- logic,
     * a real Vdd cut through sleep transistors, state lost and rebuilt on
     * wake. That is the only thing it means now.
     *
     * It used to ALSO drive DRAM precharge power-down and NVM array-periphery
     * gating, which are different mechanisms with different physics:
     *   - DRAM power-down (IDD2P) is a JEDEC STATE, not gating. The array is
     *     never unpowered -- cells are capacitors and must be refreshed -- so
     *     "power gating DRAM" is a category error. Entry/exit is tXP, spec
     *     defined, and nothing is switched off.
     *   - NVM array periphery CAN be gated retention-free, because the cells
     *     are non-volatile. That is a property of the MEMORY, not of its
     *     controller.
     * One flag driving three mechanisms meant a user could not enable the one
     * they meant, and the reported saving mixed them. */
    bool pg_mc = false;         // pim.mc.pg          controller LOGIC gating
    bool mem_power_down = false; // memory.power_down  DRAM JEDEC power-down (IDD2P)
    /* 1.11.52 (user ask "the n and temp of mcpat need fixed"): the McPAT /
     * factor-derivation temperature, previously a hardcoded 350 at three
     * sites with no config surface. 350 K = 77 C is the value every corpus
     * number so far was priced at, so it stays the default; the CACTI
     * I_off tables carry 0..100 C rows, so anything outside 273..373 K is
     * refused rather than extrapolated. */
    int temperature_k = 350;     // power.temperature_k (or power.temperature_c)
    /* 1.11.63 (R7): the termination override this tree has ADVERTISED in
     * three console messages since 1.11.59 -- "set
     * power.termination_pj_per_bit" -- had a setter with no caller: the key
     * was never parsed, so following the advice silently did nothing. Wired
     * now. <0 = model default (the R7 rd/wr split loops); >=0 prices BOTH
     * directions at the stated pJ/bit (a user-stated operating point). */
    double termination_pj_per_bit = -1.0;
    /* 1.11.51 (E17): power-down entry threshold override, ns. <=0 = use the
     * per-generation sourced tXP default; techs with no sourced tXP refuse
     * the gap-based descent rather than inventing a threshold. */
    double power_down_threshold_ns = -1.0;
    bool mem_array_pg = false;   // memory.array_pg    NVM retention-free periphery gating
    std::string pcie_model = "simple";    // "simple" or "md1" for PCIe timing model
    // Host<->device link technology. Selects preset latency/BW/overhead unless
    // the user overrides them. "interposer" = 2.5D silicon interposer (UCIe-class
    // on-package: very high BW, low latency, ~no protocol/coherence overhead).
    std::string pcie_link_type = "pcie_gen5";
    int pcie_header_bytes = 20;            // per-transaction protocol overhead bytes
    double pcie_coherence_extra_ns = 0.0;  // avg extra latency for coherent access

    // Host<->device TWO-LAYER BRIDGE (1.7.1). protocol x phy selects the
    // per-transaction overhead + phy pipeline latency; ALL fields optional,
    // defaults derive from the DEVICE memory technology (resolveBridge).
    // Emitted as sys.bridge.* in system scope; supersedes the flat pcie charge.
    bool   bridge_present = false;             // system-scope co-sim -> emit sys.bridge
    std::string bridge_protocol;               // "" = auto: native|ddr_t|cxl_mem|loadstore
    std::string bridge_phy;                    // "" = auto: on_die|pcb|interposer|serdes
    double bridge_bandwidth_gbs = -1.0;        // per-channel GB/s (-1 = auto)
    double bridge_latency_ns = -1.0;           // wire + cmd-iface/MC pipeline (-1 = auto)
    int    bridge_channels = -1;               // -1 = auto
    double bridge_protocol_overhead_ns = -1.0; // -1 = auto
    double bridge_uncached_ns = -1.0;          // pure serialized crossing (-1 = auto)

    // Case-1 COHERENCE flush accounting (1.7.2, HANDOFF ISSUE 3). Emitted as
    // sys.coherence.* in system scope; charged on the host core at roi_begin.
    // mode: "unified" (Case 1, flush inputs + invalidate outputs) or "separate"
    // (Case 2, cache bypass -> no flush). Present only in the co-sim path.
    std::string coherence_mode = "unified";        // unified | separate
    double coherence_writeback_bw_gbs = -1.0;      // host cache writeback BW (-1 = auto from host tech)
    double coherence_flush_fixed_ns = 200.0;       // fixed flush/wbinvd latency (ns)
    // Kernel input+output working-set footprint (bytes). REG_DEVBUF is retired to
    // a no-op (no live buffer registration), so this is an explicit config field
    // with a conservative default. LIMITATION: no per-run WSS tracking; the whole
    // footprint is treated as dirty (upper bound, conservative-against-PIM).
    /* 1.11.40 (audit N7, user ruling: the footprint is MEASURED). 0 = measure
     * it at roi_begin from the host caches' dirty (M) lines. A positive value
     * is an explicit OVERRIDE and is reported as one.
     *
     * The old 16 MiB default was 56.9x the host cache hierarchy it claimed to
     * flush and consumed 99.4% of the host's simulated cycles on a
     * 4096-element kernel -- a constant standing in for a quantity the
     * simulation can reveal exactly. */
    long long coherence_footprint_bytes = 0;   // 0 = measure (N7)

    // Kernel LAUNCH cost tree (1.7.3, HANDOFF ISSUE 2). Emitted as sys.launch.*
    // in system scope; charged on the host core at the offload doorbell. Real GPU
    // kernel-launch latency is famously ~5-20us (driver + runtime dispatch); the
    // decomposed defaults sit at that band's low end. doorbell/dispatch given in
    // ns (cycle-converted at the host/reference clock at emission); cmd/ack sized
    // in bytes and priced by the two-layer bridge crossing.
    double launch_doorbell_ns = 300.0;   // doorbell write + cmd-packet formation (user-mode, no syscall)
    double launch_dispatch_ns = 5000.0;  // HIP/CUDA-launch-analog runtime dispatch software cost (~5us)
    int    launch_cmd_bytes   = 64;      // cmd packet size (host->device, crosses bridge)
    int    launch_ack_bytes   = 64;      // ack packet size (device->host, crosses bridge)

    // Output configuration
    std::string stats_file;
    bool enable_detailed_stats;

    // Parallel workload configuration
    std::string workload_type;   // "serial", "openmp", "mpi"
    int mpi_ranks;               // Number of MPI ranks (0 = auto)

    //=========================================================================
    // Multi-Host Multi-Device System Architecture (scope: "system")
    //=========================================================================

    struct SystemNode {
        std::string name;
        enum Role { HOST, DEVICE } role = DEVICE;
        enum DeviceType { COMPUTE, MEMORY_ONLY } device_type = COMPUTE;
        enum Attachment { INTERNAL, EXTERNAL } attachment = EXTERNAL;

        // Core config
        std::string core_type = "ooo_core";
        int num_cores = 0;
        double frequency_mhz = 1000.0;
        int tech_node_nm = 22;

        // Cache config
        int l1d_kb = 32, l1i_kb = 32, l2_kb = 256, l3_kb = 0;
        bool enable_l2 = true, enable_l3 = false;
        int l2_ways = 8, l3_ways = 16;
        int l1d_ways = 8, l1i_ways = 4;

        // Memory
        std::string memory_tech = "DDR4";
        int ports_per_bank = 1;
        int banks = 0;                 // 0 = use global default

        /* 1.11.16 (verification audit): per-node PG flags. The 1.11.15
         * per-node setPGSpec block keyed on the GLOBAL config.pg_* flags,
         * which only the device-scope YAML surface (top-level pim:/noc:)
         * can set -- no system-scope config could reach it. Spec #84 says
         * per-component pg: at initialization, so system-scope nodes parse
         * their own devices[].pim.pe.pg / devices[].pim.mc.pg /
         * devices[].noc.pg (default false = bit-identical). */
        bool pg_pe  = false;
        /* 1.11.43 (E23): per-node FPU capability -- a PE property that must
         * not leak onto the host through a global. */
        bool     pe_has_fpu = true;
        uint32_t pe_fp_emul_cycles = 0;
        /* 1.11.56 (audit B054): the element's datapath shape, per node.
         * pim.pe.bit_serial and pim.pe.issue_width were parsed into the
         * GLOBAL config only, which the system-scope core-group emitter never
         * reads -- so a bit-serial element ran as a parallel one in co-sim
         * (a 32x per-op cycle difference at operand_width: 32) and an
         * in-order PE kept the hardcoded dual issue. */
        bool alu_bit_serial = false;
        int  inorder_issue_width = 2;
        /* 1.11.56 (audit B055): whether this node actually DECLARED a
         * pim.mc block. pe_mc_type defaults to "simple" and can only be
         * overwritten by a key inside that block, so !pe_mc_type.empty() was
         * a tautology and the device-scope "PEs at this tier require a memory
         * controller" error could never fire in system scope. */
        bool pe_mc_declared = false;
        bool pg_noc = false;
        bool pg_mc  = false;
        /* 1.11.20 (user decision D7): HOST role. One flag, hosts[].pg,
         * because the user's 2026-08-14 decree is that the host gates "as an
         * entire piece" -- cores and host MC together, not as separately
         * gateable components. Default false, so a config without the key is
         * bit-identical. Device nodes keep the three-flag surface above:
         * there the components genuinely have their own gate signals. */
        bool pg_host = false;
        // Memory-topology knob (1.7.4, HANDOFF MEMORY-TOPOLOGY ADDENDUM).
        // DEVICE role only. true (DEFAULT): this PIM device IS the host's main
        // memory -- host tech = device tech BY CONSTRUCTION (preserves the
        // matrix). false: the device is accelerator-side memory only and the
        // host MUST supply a host.mem block (resolveMemoryTopology enforces).
        bool is_default_mem = true;
        // Host memory bandwidth/channel overrides (HOST role). -1 = auto from
        // technology (per-channel BW = aggregate/channels; DDR5 c=1 x 25.6 GB/s,
        // HBM3 c=16 x 51.2 GB/s). User-settable via host memory: block.
        int mem_bandwidth_mbs = -1;    // per-channel bandwidth override (MB/s)
        int mem_channels = -1;         // channel (host MC) count override
        // Host-path idle-latency adder (ns). Calibrated constant added on top of
        // the physical composition (tRCD+tCAS + MC/queue + hop) so the effective
        // idle host memory latency matches measured real sockets. -1 = auto
        // per-tech default (getHostPathAdderNs); >=0 = user override (0 = none).
        // HOST-ROLE ONLY -- never applied to device (PE) memory pricing.
        double mem_latency_adder_ns = -1.0;

        // Host-path DECOMPOSITION overrides (1.7.2). Optional per-component
        // overrides that MERGE over the per-tech default split (getHostPathSplit).
        // -1 = use the default for that component; >=0 = override. Setting ANY of
        // these AND mem_latency_adder_ns is a config error (competing totals).
        double host_path_fabric_ns    = -1.0;
        double host_path_coherence_ns = -1.0;
        double host_path_mc_pipeline_ns = -1.0;
        double host_path_phy_ns       = -1.0;
        bool   host_path_any_set = false;   // true if any host_path.* was given

        // SEPARATE host main memory (1.7.4, HANDOFF UNIVERSALITY ADDENDUM).
        // HOST role only; consumed when the paired device sets is_default_mem
        // false. host.mem instantiates any of the 11 techs as a PLAIN non-PIM
        // host main memory (no PE arrays, H-tree, or PIM windows) reusing the
        // SAME per-tech channel/timing tables the device path uses. When the
        // device is_default_mem is true (default) this block is ignored (the
        // device tech drives host mem). resolveMemoryTopology validates + wires
        // host_mem_tech -> memory_tech and the optional BW/channel overrides
        // into mem_bandwidth_mbs / mem_channels.
        bool        host_mem_present = false;   // host.mem block supplied
        std::string host_mem_tech;              // host.mem.technology (canonical)
        // 1.11.57 (latent B034): host_mem_capacity_gb removed -- write-only,
        // "advisory" by its own comment, and the key is refused at parse time.
        int         host_mem_bandwidth_mbs = -1; // host.mem.bandwidth_gbs -> per-channel MB/s override
        int         host_mem_channels = -1;      // host.mem.channels override

        // Host NoC config (HOST with num_cores>1): analytic crossbar fabric.
        // 1-core hosts have no fabric (crossbar degenerates at a single core).
        std::string host_noc_topology = "crossbar";
        std::string host_noc_model = "analytical";
        int host_noc_hop_cycles = 4;   // core->LLC/MC one-hop latency (core clock)

        // PIM config (DEVICE+COMPUTE only)
        std::string pe_type;
        int num_pes = 0;
        std::string placement_level = "BANK";
        double alu_compute_factor = 1.0;
        double alu_access_factor = 1.0;
        double alu_throughput_factor = 1.0;
        int alu_operand_width = 32;
        double alu_energy_factor = 1.0;
        std::string pe_mc_type = "simple";
        int pes_per_mc = 1;

        // NoC config (DEVICE with PEs)
        std::string noc_topology = "MESH_2D";
        std::string noc_model = "simple";

        // Per-node workload (optional -- inherits top-level if empty)
        std::string workload_binary;
        std::vector<std::string> workload_args;

        // Computed at config time
        int core_start_idx = 0;        // first core index in global ZSim array
        int core_end_idx = 0;          // last+1 core index
        double freq_scale = 1.0;       // reference_freq / node_freq
        uint64_t addr_start = 0;       // address range start (auto-assigned)
        uint64_t addr_end = 0;         // address range end (exclusive)
        int node_network_id = 0;       // position in system network topology
    };

    struct SystemLinkConfig {
        std::string src_name, dst_name;
        std::string link_type = "pcie_gen5";
        // Supported: pcie_gen4, pcie_gen5, cxl_2_0, cxl_3_0,
        //            nvlink_3_0, nvlink_4_0, nvlink_c2c, ualink_1_0, interposer
        /* 1.11.57 (latent B033): `lanes` and `coherence` are gone.
          *
          * `lanes` was parsed (default 16) and written by the legacy PCIe path
          * as well, and read by NOTHING: the link's rate reaches the model as
          * bandwidth_GBs, either declared or taken from the type preset, and
          * no code multiplies it by a lane count. A user halving the lanes got
          * the full-width link. The key is now refused at parse time rather
          * than accepted and dropped.
          *
          * `coherence` was the preset's coherence-class LABEL -- "bias",
          * "directory" -- assigned beside coherence_extra_ns and never read.
          * Worse, the legacy PCIe path (which fills coherence_extra_ns from
          * config.pcie_coherence_extra_ns) left the label at "none", so the
          * field and the charge beside it could already contradict each other.
          * The charged quantity, coherence_extra_ns, is what the model uses
          * and is what stays. */
        double base_latency_ns = 500.0;
        double bandwidth_GBs = 63.0;
        int header_bytes = 20;             // protocol overhead per transaction
        double coherence_extra_ns = 0.0;   // average extra latency for coherent access
    };

    struct SystemNetworkConfig {
        std::string topology = "crossbar";
        std::string model = "simple";      // simple, md1, detailed
        int link_width_bits = 512;
        double frequency_ghz = 1.0;
        int latency_cycles = 5;
        int router_latency = 1;
        int virtual_channels_per_vn = 1;
        int input_buffer_depth = 4;
        int output_buffer_depth = 4;
        std::vector<SystemLinkConfig> links;
    };

    std::vector<SystemNode> system_nodes;
    SystemNetworkConfig system_network;
    double reference_frequency_mhz = 0;  // max of all node frequencies (0 = auto)

    // Default constructor
    UnifiedConfig() :
        method("exec"),
        scope("device"),
        name("PIMID_Simulation"),
        description(""),
        memory_tech("SRAM"),
        num_banks(4),
        subarrays_per_bank(4),
        memory_latency_override(-1),
        pe_type("in_order_core"),
        placement_level("BANK"),
        num_pes(4),
        alu_compute_factor(1.0),
        alu_access_factor(1.0),
        alu_throughput_factor(1.0),
        alu_operand_width(32),
        alu_energy_factor(1.0),
        frequency_mhz(2000),
        cache_line_size(64),
        tech_node_nm(22),
        l1d_size_kb(32),
        l1d_ways(8),
        l1i_size_kb(16),
        l1i_ways(4),
        l2_size_kb(2048),
        l2_ways(16),
        enable_l2(true),
        l2_count(1),
        enable_l3(false),
        l3_size_kb(4096),
        l3_ways(16),
        noc_topology("MESH_2D"),
        noc_router_latency(1),
        noc_link_latency(1),
        noc_cycle_accurate(true),   // DEFAULT noc model = detailed (cycle-accurate Garnet)
        noc_routing(""),
        noc_vcs_per_vnet(4),
        noc_buffers_per_vc(4),
        noc_topology_file(""),
        noc_routing_table_file(""),
        noc_control_msg_bits(0),
        noc_data_msg_bits(0),
        noc_ring_unidirectional(false),
        phase_length(10000),
        max_instructions(1000000000000LL),  // runaway guard only; must NOT truncate real workloads (1e9 silently cut 256MB-WSS kernels mid-run)
        stats_interval(100000),
        stats_file("results/stats.txt"),
        enable_detailed_stats(true),
        workload_type("serial"),
        mpi_ranks(0) {}
};

/**
 * @brief Parse a bandwidth string with unit suffix and return MB/s.
 *
 * Accepts: "25600" (bare number = MB/s), "25600 MB/s", "25.6 GB/s",
 *          "1 TB/s", "6400000 KB/s". Case-insensitive units.
 * Returns the value in MB/s, or -1 on parse failure.
 */
static int parseBandwidthToMBs(const std::string& input) {
    if (input.empty()) return -1;

    // Find where the numeric part ends
    double value = 0;
    size_t pos = 0;
    try {
        value = std::stod(input, &pos);
    } catch (...) {
        return -1;
    }

    // Extract unit suffix (skip whitespace)
    std::string unit;
    for (size_t i = pos; i < input.size(); i++) {
        if (!std::isspace(input[i])) unit += std::toupper(input[i]);
    }

    // Strip trailing "/S" if present
    if (unit.size() >= 2 && unit.substr(unit.size() - 2) == "/S") {
        unit = unit.substr(0, unit.size() - 2);
    }

    double mbs;
    if (unit.empty() || unit == "MB" || unit == "MBS") {
        mbs = value;
    } else if (unit == "KB" || unit == "KBS") {
        mbs = value / 1000.0;
    } else if (unit == "GB" || unit == "GBS") {
        mbs = value * 1000.0;
    } else if (unit == "TB" || unit == "TBS") {
        mbs = value * 1000000.0;
    } else {
        return -1;
    }

    return std::max(1, static_cast<int>(std::round(mbs)));
}

/**
 * @brief Canonicalize a memory-technology spelling at parse time.
 *
 * Every downstream comparison in this file tests the UPPERCASE canonical name
 * (tech == "RERAM", == "STT_MRAM", ...). Accepting mixed-case spellings at the
 * YAML layer without normalizing means a valid input like "ReRAM" misses every
 * one of those tests and silently falls through to the generic-DRAM defaults
 * (128-bank DDR4-like org, 10-cycle DDR local latency) while the NVSim/power
 * path -- which uppercases locally -- still prices it as ReRAM. Normalize once
 * here so a technology has exactly one spelling past the parser.
 */
/* 1.11.56 (audit B046): ONE core-type normaliser.
 *
 * Device scope open-coded its own list and system scope had this
 * lambda, and they had already drifted -- "compute_unit_pe" was
 * accepted by one and hard-rejected by the other, so the same string
 * named a valid machine or a fatal config error depending on which
 * scope read it. The old comment said both must be updated together;
 * that is what a single function is for. */
static std::string normalizeCoreTypeName(const std::string& ct) {
                if (ct == "OOO" || ct == "OoO" || ct == "ooo" || ct == "out-of-order")
                    return "ooo_core";
                if (ct == "InOrder" || ct == "in-order" || ct == "in_order")
                    return "in_order_core";
                /* 1.9.36: the element is a COMPUTE UNIT, not an "ALU core".
                 * "ALU" was already inaccurate -- three of the five kernels
                 * are FP32 (stream_triad, gemv and stencil_2d carry float;
                 * histogram and bfs carry int), so it has always needed a
                 * floating-point unit. "Compute unit" is also the PIM
                 * literature's term: Samsung's in-bank engine is a
                 * programmable computing unit.
                 * Normalised HERE, at the single entry point every core-type
                 * string passes through, so the ~20 internal comparisons keep
                 * working unchanged -- the same discipline as canonicalMemTech.
                 * The former spellings stay accepted: the entire sweep corpus
                 * names alu_core, and an unrecognised type must not silently
                 * become a different core model. */
                if (ct == "compute_unit" || ct == "ComputeUnit" ||
                    ct == "compute_unit_pe" || ct == "cu")
                    return "alu_core";
                /* 1.9.36: bare "alu"/"ALU" retired -- no configuration in the
                 * corpus used it (0 files, against 3100 naming alu_core), so
                 * removing it costs nothing and narrows the surface. alu_core
                 * STAYS: it names the entire sweep corpus and dropping it
                 * would invalidate every cell ever run. */
                if (ct == "Simple" || ct == "simple") return "simple_core";
                if (ct == "Null" || ct == "null") return "null_core";
                // STRICT: anything else must already be a canonical name.
                // Unknown values used to pass through and silently fall to
                // the "Simple" default at zsim emission (e.g. the removed
                // timing_core ran hosts as Simple cores). Hard error now,
                // same policy as the pim.pe.type gate.
                if (ct == "ooo_core" || ct == "in_order_core" ||
                    ct == "alu_core" || ct == "simple_core" || ct == "null_core")
                    return ct;
                std::cerr << "ERROR: unknown core type '" << ct << "' in "
                          << "system.hosts[].core_type / devices[].pe_type.\n"
                          << "Valid: compute_unit (alias alu_core), "
                          << "ooo_core, in_order_core, simple_core, "
                          << "alu_core, null_core.\n";
                exit(1);
}

static std::string canonicalMemTech(std::string t) {
    std::transform(t.begin(), t.end(), t.begin(), ::toupper);
    if (t == "STTMRAM" || t == "STT-MRAM" || t == "MRAM") return "STT_MRAM";
    if (t == "RESISTIVE" || t == "MEMRISTOR") return "RERAM";
    if (t == "PCRAM" || t == "3DXPOINT") return "PCM";
    /* 1.9.35: an unrecognised technology used to pass straight through here and
     * then be classified by EXCLUSION downstream -- is_dram_tech is written as
     * !(SRAM || STT_MRAM || PCM || RERAM) at three sites -- so any string that
     * is not one of those four became "DRAM" and was priced with Ramulator2's
     * JEDEC tables. A typo, or a technology we do not support such as NAND,
     * silently produced plausible DDR-shaped energy for a part that is not DRAM.
     *
     * Defect #8 was this same failure for a CASE mismatch (mixed-case ReRAM fell
     * through to generic DRAM); it was fixed by adding the uppercase transform
     * above without closing the structure that allowed it. This closes it.
     *
     * Validating HERE rather than rewriting the three downstream tests is the
     * smaller and safer change: every one of them is correct for canonical
     * input, and this is the single point every technology string passes
     * through. Nothing that is currently valid changes classification. */
    /* 1.11.51 (L76, second half): "DRAM" is an ALIAS, resolved HERE. It sat
     * in the supported set as its own name, but downstream the string hit
     * two different truths -- Ramulator prices it with the DDR4 preset while
     * a device-scope validator rejected it outright (gate 1161I found the
     * dr cell failing rc=1 on "unknown memory technology 'DRAM'"), and two
     * host-path tables classed it with DDR5 and DDR4 respectively. One
     * resolution point ends that: the generic name becomes the generation
     * that actually times it, and every consumer sees only canonical names. */
    /* 1.11.57 (latent B044): this resolution is now the ONLY place the string
     * "DRAM" exists as a technology. Six downstream tables still carried a
     * "DRAM" branch after it -- the latency helper, two controller lists, the
     * DRAM-detect predicate, the McPAT MC table (grouped with DDR4), the
     * host-path split (grouped with DDR5) and the bridge defaults (grouped
     * with DDR5). None could fire, and two of them disagreed about which
     * generation the alias was, so whoever revived the spelling would have
     * inherited a contradiction. They are gone; this line is the authority. */
    if (t == "DRAM") {
        std::cout << "[config] memory technology 'DRAM' is the generic alias; "
                     "priced as DDR4 (the Ramulator preset that times it)."
                  << std::endl;
        t = "DDR4";
    }
    static const std::set<std::string> kSupported = {
        // DRAM, priced by Ramulator2
        "DDR3", "DDR4", "DDR5", "LPDDR5", "GDDR6", "HBM2", "HBM3",
        // non-DRAM, priced by NVSim/CACTI
        "SRAM", "STT_MRAM", "PCM", "RERAM"
    };
    if (!kSupported.count(t)) {
        std::cerr << "[config] FATAL: unsupported memory technology '" << t
                  << "'. Supported: DDR3 DDR4 DDR5 LPDDR5 GDDR6 HBM2 HBM3 "
                     "SRAM STT_MRAM PCM RERAM (aliases: DRAM -> DDR4, "
                     "MRAM/STTMRAM -> STT_MRAM, "
                     "PCRAM/3DXPOINT -> PCM, MEMRISTOR/RESISTIVE -> RERAM). "
                     "An unknown value would otherwise be classified as DRAM by "
                     "exclusion and priced with JEDEC DRAM tables." << std::endl;
        std::exit(2);
    }
    return t;
}

/* 1.11.57 (latent A013): ONE table for "how many DRAM devices make a 64-bit
 * DDR-class rank at this device width".
 *
 * It was written down twice -- once in the hierarchy derivation, where it sets
 * hierarchy_chips_per_rank, and once inside memorySystemDieCount(), where it
 * sets the die population the system area total is built from. The two agreed,
 * which is exactly why a divergence would have been silent: one of them would
 * have been edited and the other left describing a different part. An empty or
 * unrecognised width means "keep the JEDEC default organisation", which for
 * every DDR-class preset in the lineup is x8.
 *
 * The width -> chips relation is arithmetic, not a datasheet lookup: a rank
 * presents 64 data bits, so it takes 64/width devices to build one. */
static int chipsPerRankForDeviceWidth(const std::string& device_width) {
    if (device_width == "x4")  return 16;
    if (device_width == "x16") return 4;
    return 8;                      // "x8", and the default when none is set
}

/**
 * @brief Auto-derive memory controller type and parameters from technology.
 *
 * Maps memory technologies to ZSim controller models:
 *   DDR3/DDR4/DDR5/LPDDR5/GDDR6/HBM/HBM2/HBM3 -> Ramulator (cycle-accurate)
 *   STT-MRAM/PCM/ReRAM                          -> MD1 (M/D/1 queuing)
 *   SRAM                                         -> MD1 (fixed latency)
 *
 * Skipped if user explicitly set zsim_mem_controller_type != "auto".
 */
/* 1.11.40 (audit E18): is the system's memory ONE shared array, or does the
 * host hold its own? Used to be derived inline in the memory-reporting block;
 * the link-energy block needs the SAME answer, and re-deriving it there would
 * be two copies of one claim that can drift apart. Single definition, two
 * callers.
 *
 * Coupled means the host and the device address the same silicon -- which in a
 * PIM system is the DEVICE's DRAM, on the far side of the link. That is what
 * makes the coherence flush link traffic; see the E18 comment at the byte
 * total. Empty tech on either side collapses to coupled, preserving the
 * pre-1.11.9 single-array behaviour the corpus was measured under. */
static bool sharedMemoryCoupled(const UnifiedConfig& config);

static void autoGenerateRamulatorConfig(UnifiedConfig& config, const std::string& tech);

static void getMemControllerConfig(UnifiedConfig& config) {
    // Helper to check if technology is DRAM-based
    // 1.11.57 (latent B044): "DRAM" dropped -- canonicalMemTech() resolves the
    // generic alias to DDR4 upstream, so no string reaching here can be it.
    auto isDramBased = [](const std::string& tech) {
        return tech == "DDR3" || tech == "DDR4" || tech == "DDR5" ||
               tech == "LPDDR5" || tech == "GDDR6" ||
               tech == "HBM2" || tech == "HBM3";
    };

    std::string tech = config.memory_tech;
    std::transform(tech.begin(), tech.end(), tech.begin(), ::toupper);

    // Validate user-specified controller against technology
    if (config.zsim_mem_controller_type != "auto") {
        std::string ct = config.zsim_mem_controller_type;
        // Reject removed types
        if (ct == "md1" || ct == "weavemd1") {
            std::cerr << "  [WARN] Controller type '" << ct << "' has been removed. "
                      << "Using 'simple' (M/D/1 is always active)." << std::endl;
            config.zsim_mem_controller_type = "simple";
            ct = "simple";
        }
        if (ct == "ramulator" && !isDramBased(tech)) {
            std::cerr << "  [WARN] Controller '" << ct << "' requires DRAM-based technology, "
                      << "but memory_tech is '" << config.memory_tech << "'. Falling back to simple." << std::endl;
            config.zsim_mem_controller_type = "simple";
        }
        // When using Simple/WeaveSimple with DRAM tech, derive bandwidth from Ramulator.
        // User-set bandwidth is clamped to the tech limit (can't exceed physical BW).
        if ((ct == "simple" || ct == "weavesimple") && isDramBased(tech)) {
            try {
                pimid::RamulatorWrapper bw_query("", tech);
                bw_query.initialize();
                double rank_bw_gbs = bw_query.getRankBandwidth();
                int tech_bw = static_cast<int>(rank_bw_gbs * 1000.0);
                if (config.md1_bandwidth_user_set) {
                    if (config.md1_bandwidth_mbs > tech_bw) {
                        std::cerr << "  [WARN] User bandwidth (" << config.md1_bandwidth_mbs
                                  << " MB/s) exceeds " << config.memory_tech << " physical limit ("
                                  << tech_bw << " MB/s). Clamping." << std::endl;
                    }
                    config.md1_bandwidth_mbs = std::min(config.md1_bandwidth_mbs, tech_bw);
                } else {
                    config.md1_bandwidth_mbs = tech_bw;
                }
            }
            /* 1.11.57 (latent B022): SAY SO. "Keep user-set or default
             * bandwidth on failure" describes two very different outcomes
             * under one comment. If the user set a bandwidth, keeping it is
             * defensible -- but it is then UNCLAMPED, and the clamp is the
             * only thing stopping a config from claiming more bandwidth than
             * the part has. If the user set nothing, what is kept is
             * md1_bandwidth_mbs's struct default of 6400 MB/s, a placeholder
             * that is not a property of any DRAM, and it becomes the M/D/1
             * service rate of the whole memory. The sibling SRAM/NVM branch a
             * few lines below has announced exactly this since 1.11.56; this
             * one did it in silence. Announced rather than fatal, because the
             * user has named the controller explicitly and their own figure is
             * a legitimate thing to keep -- as long as the run says it is
             * theirs and not the technology's. */
            catch (const std::exception& e) {
                announceUnclampedBandwidth(config.memory_tech, e.what(),
                                           config.md1_bandwidth_user_set);
            } catch (...) {
                announceUnclampedBandwidth(config.memory_tech,
                    "a non-standard exception escaped the wrapper",
                    config.md1_bandwidth_user_set);
            }
        }
        /* 1.11.56 (audit B028): only skip the DRAM auto-derivation.
         *
         * This goto jumped over the SRAM/NVM bandwidth derivation as well, so
         * "memory.controller.type: simple" on an NVM device -- naming the
         * controller the code would have chosen anyway -- left
         * md1_bandwidth_mbs at the struct default of 6400 MB/s, which is a
         * placeholder and not a property of any array. Naming the controller
         * should decide the controller, not silently delete the memory's
         * physical bandwidth cap. */
        if (isDramBased(tech)) goto weave_upgrade;
    }

    // --- Ramulator-backed DRAM technologies ---
    if (tech == "DDR3" || tech == "DDR4" || tech == "DDR5" ||     // 1.11.57 (latent B044)
        tech == "LPDDR5" || tech == "GDDR6" ||
        tech == "HBM2" || tech == "HBM3") {
        config.zsim_mem_controller_type = "ramulator";
        // Auto-generate Ramulator config if none provided
        if (config.ramulator_config_file.empty()) {
            autoGenerateRamulatorConfig(config, tech);
        }
    }
    // --- NVSim-backed NVM technologies ---
    // For each tech, compute the physical bandwidth limit from access time.
    // User-set bandwidth is clamped to this limit.
    else if (tech == "STT_MRAM" || tech == "STTMRAM" || tech == "STT-MRAM" || tech == "MRAM" ||
             tech == "PCM" || tech == "PCRAM" || tech == "3DXPOINT" ||
             tech == "RERAM" || tech == "RESISTIVE" || tech == "MEMRISTOR" ||
             tech == "SRAM") {
        /* 1.11.56 (audit B004): ASK THE TOOL.
         *
         * This block used to divide by an invented access time -- 10 ns for
         * STT-MRAM, 50 ns for PCM, 20 ns for ReRAM (and 2 ns for SRAM at the
         * host sites) -- and the result is the M/D/1 bandwidth cap of every
         * SRAM/NVM memory in the simulator. NVSim and CACTI are queried a few
         * hundred lines below for the read latency of the SAME array, at the
         * same node, corner, temperature and access width, and were simply not
         * consulted here. Four banks of STT-MRAM came out at 25600 MB/s from an
         * assumption, whatever the characterization said.
         *
         * The cap is banks x line / access_time. Everything on the right now
         * comes from the array model. If the characterization is unavailable
         * the run says so and leaves the user's bandwidth alone rather than
         * substituting a number. */
        config.zsim_mem_controller_type = "simple";
        double acc_ns = -1.0;
        try {
            int lat_cy = getMemoryLatencyCycles(tech, config.frequency_mhz,
                                                config.tech_node_nm, false, -1.0,
                                                -999, config.temperature_k,
                                                config.cache_line_size);
            if (lat_cy > 0 && config.frequency_mhz > 0)
                acc_ns = lat_cy * 1000.0 / config.frequency_mhz;
        } catch (const std::exception& e) {
            std::cerr << "[bw] " << tech << " array characterization failed ("
                      << e.what() << ")" << std::endl;
        }
        if (acc_ns > 0.0) {
            int tech_bw = static_cast<int>(config.num_banks * config.cache_line_size
                                           * 1000.0 / acc_ns);
            if (tech_bw < 1) tech_bw = 1;
            std::cout << "  [bw] " << tech << " M/D/1 cap = " << config.num_banks
                      << " banks x " << config.cache_line_size << " B / "
                      << acc_ns << " ns = " << tech_bw
                      << " MB/s (access time from the array model, not a preset)"
                      << std::endl;
            config.md1_bandwidth_mbs = config.md1_bandwidth_user_set ?
                std::min(config.md1_bandwidth_mbs, tech_bw) : tech_bw;
        } else {
            std::cerr << "[bw] WARNING: no array access time for " << tech
                      << ", so no physical M/D/1 bandwidth cap is derived for "
                         "this run. "
                      << (config.md1_bandwidth_user_set
                            ? "The user's memory.bandwidth_mbs stands UNCLAMPED."
                            : "The M/D/1 bandwidth floor is whatever the default "
                              "carries; it is not a property of this array.")
                      << std::endl;
        }
    } else {
        // Unknown tech, fall back to simple
        config.zsim_mem_controller_type = "simple";
    }

weave_upgrade:
    // Weave auto-upgrade: when the PE core is in_order_core or ooo_core and the
    // controller is Simple, upgrade to WeaveSimple for proper weave-phase
    // interaction. Ramulator already handles this internally.
    {
        std::string ct = config.zsim_mem_controller_type;
        bool weave_core = (config.pe_type == "in_order_core" ||
                           config.pe_type == "ooo_core");
        if (weave_core && ct == "simple") {
            config.zsim_mem_controller_type = "weavesimple";
        }
    }
}

// Forward declarations (needed because computeHierarchyLatencies calls these)
static void autoGeneratePEMemMap(UnifiedConfig& config);
static void validatePEMemMapping(const UnifiedConfig& config);
static double avgHopsForTopology(const std::string& topology, int num_nodes, bool ring_unidir = false);
static int bisectionLinksForTopology(const std::string& topology, int num_nodes, bool ring_unidir = false);
static int totalChannelsForTopology(const std::string& topology, int num_nodes, bool ring_unidir = false);
static double hotspotFactorForTopology(const std::string& topology);
static int topologyClassForTopology(const std::string& topology);

/**
 * @brief Emit a per-tech DRAM CUSTOM topology file for the detailed Garnet NoC.
 *
 * CONCEPTUAL FRAMING (important): the DRAM "network" here has NO real packet
 * routers. Each Garnet router emitted below is a SIMPLIFIED ABSTRACTION OF A
 * DATAPATH TRANSITION / MUX POINT in the DRAM hierarchy -- bank IO, the
 * channel/DQ bus, and the system/IMC root. Each LINK models that transition's
 * datapath BANDWIDTH (link width) and LATENCY (cycles). The N parallel
 * channel-DQ subtrees model independent DRAM channels -- the concurrency that
 * makes HBM3(16ch) > HBM2(8) > 1-channel DDR. There is no real routing; this
 * is purely a datapath/bandwidth model.
 *
 * Topology: SPARSE, PLACEMENT-DRIVEN H-TREE.
 *
 * 1.11.57 (latent B047): everything this block used to say about the topology
 * was describing a builder that no longer exists. It specified a CHANNEL-DQ
 * CHOKEPOINT, STAR-OF-BANKS shape with a fixed 128 endpoints, 16 logical banks
 * and a round-robin "endpoint e -> bank (e % 16)" attachment, and gave port
 * counts for it. The function has built a sparse H-tree from the placement
 * since 1.10 (see buildSparseHTree below): the tree is generated per
 * simulation from the elements' actual homes, only PE-bearing branches are
 * materialized down to the placement level, each router with no PE children
 * collapses to ONE abstract endpoint at its maximal empty subtree, and the
 * shape comes from the real organisation ladder (subarrays/bank, banks/BG,
 * BGs/chip, chips/rank, ranks/channel, channels) rather than from 16 and 128.
 * The endpoint count is therefore a property of the configuration, not a
 * constant, and the coverage assertion a few dozen lines down checks it
 * against total_mem_orgs. Nothing printed the old description and no emitted
 * number depended on it -- the emitted file's own header has described the
 * sparse tree correctly all along -- so this is a comment correction, and the
 * reason it matters is that the two descriptions differ in the one place a
 * reader would look before changing the fanout.
 *
 * What survives from the old shape is its POINT, which the sparse tree keeps:
 * traffic between organisations under different channels has no low common
 * ancestor and must cross the shared, narrow channel-DQ links -- the bandwidth
 * wall of real DRAM -- while the N parallel channel subtrees supply the
 * concurrency that makes HBM3(16ch) > HBM2(8) > single-channel DDR.
 *
 * Per-link width/latency model (UNCHANGED):
 *   layer_BW_GBs = link_width_bits/8 * freq_GHz
 *   width(bits)  = clamp( round(128 * layer_BW_GBs / REF_BW), 1, 128 )   // occupancy = ceil(128/width)
 *   latency(cyc) = max(1, round(REF_FREQ / freq_GHz))                    // slower clock -> more latency
 *   REF_BW   = max layer BW across all techs = HBM3 L0 = 512/8*1.8 = 115.2 GB/s
 *   REF_FREQ = 2.4 GHz
 *
 * Each int edge is emitted in BOTH directions with identical lat/width.
 * Returns true on success.
 */
/* 1.11.56 (audit B012): ONE reference anchor, derived, not written down.
 *
 * Three sites carried "the fastest per-channel DRAM in the lineup" as a
 * literal -- 51200 MB/s twice and 115.2 GB/s once -- and every one of them
 * scales something the run reports: the emitted per-layer link widths, the
 * Garnet link latency, and the flits-per-access multiplier. Because they were
 * literals, changing the HBM3 Ramulator preset moved the measured aggregate
 * bandwidth and left all three anchors behind, so the ladder they normalise
 * would have silently stopped describing the part. Ask the preset instead, and
 * cache the answer (this is a process-lifetime constant, and initializing
 * Ramulator is not free).
 *
 * The fallbacks are the previous literals, and the run says when it uses one:
 * an unavailable HBM3 preset is a broken build, not a modelling choice. */
static double referencePerChannelMBs() {
    static double cached = -1.0;
    if (cached > 0.0) return cached;
    try {
        pimid::RamulatorWrapper ref("", "HBM3");
        ref.setAnchorQuiet(true);   // 1.11.61 (D4): anchor, not the run's part
        ref.initialize();
        double agg = static_cast<double>(ref.getBandwidth());   // MB/s, whole stack
        uint32_t nch = ref.getNumChannels();
        if (agg > 0.0 && nch >= 1) {
            cached = agg / static_cast<double>(nch);
            return cached;
        }
    } catch (const std::exception&) { /* reported below */ }
    std::cerr << "[ref] WARNING: the HBM3 preset could not be read, so the "
                 "bandwidth reference anchor falls back to its last recorded "
                 "value (51200 MB/s per channel). Every normalised NoC width, "
                 "link latency and flit count in this run is relative to that "
                 "number, not to the preset." << std::endl;
    cached = 51200.0;
    return cached;
}

/* 1.11.56 (audit B012): HBM3's L0 bandwidth, the widest layer in the lineup,
 * from the architecture object rather than as a written-down product. */
static double referenceLayer0BandwidthGBs() {
    static double cached = -1.0;
    if (cached > 0.0) return cached;
    try {
        pimid::RamulatorWrapper ref("", "HBM3");
        ref.setAnchorQuiet(true);   // 1.11.61 (D4): anchor, not the run's part
        ref.initialize();
        double bw = ref.getSubarrayBandwidth();    // GB/s at the GSA datapath
        if (bw > 0.0) { cached = bw; return cached; }
    } catch (const std::exception&) { /* reported below */ }
    std::cerr << "[ref] WARNING: the HBM3 preset could not be read, so the "
                 "per-layer link-width anchor falls back to its last recorded "
                 "value (115.2 GB/s)." << std::endl;
    cached = 115.2;
    return cached;
}


static bool dramHTreeBuilder(const std::string& tech,
                             const std::string& outPath,
                             UnifiedConfig& config) {
    int num_pes = config.num_pes;
    int pe_level = config.pe_hierarchy_level;
    // Per-tech per-layer (link_width_bits, freq_GHz), layers leaf->root: L0,L1,L2,L3; plus channel count N.
    struct Layer { double width_bits; double freq_ghz; };
    Layer L[4];          // L[0]=L0 (subarray-leaf) ... L[3]=L3 (channel)
    int N = 1;           // parallel DRAM channels

    if (tech == "DDR3") {
        L[0]={512,0.8}; L[1]={256,0.8}; L[2]={256,0.8}; L[3]={192,0.8}; N=1;
    } else if (tech == "DDR4") {
        L[0]={512,1.2}; L[1]={256,1.2}; L[2]={256,1.2}; L[3]={192,1.2}; N=1;
    } else if (tech == "DDR5") {
        L[0]={512,2.4}; L[1]={256,2.4}; L[2]={256,2.4}; L[3]={128,2.4}; N=1;
    } else if (tech == "LPDDR5") {
        L[0]={256,1.6}; L[1]={256,1.6}; L[2]={256,1.6}; L[3]={64,1.6};  N=1;
    } else if (tech == "GDDR6") {
        L[0]={256,2.0}; L[1]={256,2.0}; L[2]={256,2.0}; L[3]={128,2.0}; N=1;
    } else if (tech == "HBM2") {
        // freq = 1.2 GHz (= 2.4 GT/s I/O clock / 2), NOT 1.0: at 1.0 the leaf
        // L0 = 256/8*1.0 = 32 GB/s fell BELOW HBM2's 38.4 GB/s per-channel rate
        // (an inversion -- subarray link slower than the channel it bypasses) and
        // the subarray aggregate 32*8 = 256 undershot the 307 GB/s stack. At 1.2,
        // L0 = 38.4 (= per-channel egress, BW-neutral) and aggregate = 38.4*8 =
        // 307.2 GB/s = the real HBM2 stack. Gentle width ladder kept: HBM2's
        // near-data win is the N=8 channel parallelism, not a wide subarray link.
        L[0]={256,1.2}; L[1]={192,1.2}; L[2]={160,1.2}; L[3]={128,1.2}; N=8;
    } else if (tech == "HBM3") {
        L[0]={512,1.8}; L[1]={384,1.8}; L[2]={256,1.8}; L[3]={128,1.8}; N=16;
    } else {
        return false;  // not a modeled DRAM tech
    }

    /* 1.11.59 (audit round 3, B005): ONE LADDER PER RUN.
     *
     * The table above is a SECOND per-tier ladder, and on a detailed DRAM run
     * both were live: sys.hierarchy's levels came from the sourced ladder
     * (what the PE is charged), while this table drew the Garnet CUSTOM
     * topology (what Garnet actually simulates). They disagreed -- DDR4's
     * chip tier is 192 bits here against the 8 bits an x8 part has and which
     * the sourced ladder now carries -- so the cycle-accurate network was
     * measuring a different fabric from the one being priced, in the same
     * run. 1.11.56 said these tables "survive only as the fallback for a
     * technology with no architecture object"; this copy was not a fallback,
     * it was the only authority for the topology.
     *
     * The builder's four layers are leaf->root (subarray, bank, bank group,
     * channel), so they take ladder rungs 0, 1, 2 and 5; the frequency is the
     * one the rung's own bandwidth and width imply, which is what
     * applySourcedLadder back-derived. When no ladder was adopted -- the
     * technology did not reconcile with its preset, so its ladder is not
     * tool-sourced -- the table stands, and that is a real fallback. */
    if (config.sourced_ladder_valid) {
        /* 1.11.60 (ONE FABRIC, user-approved design; corrects round-4 B001).
         *
         * The four topology layers are ANCHORED AT THE CHANNEL and measured
         * inward -- that is their defined meaning (sparse_htree.h, 1.10.3):
         * L3 is the channel link itself, L2 one tier inward, L1 two, L0
         * everything deeper, folded. The 1.11.59 map {0,1,2,5} was
         * LEAF-anchored -- read from the wrong end of the ladder -- so on
         * every DDR part the rank-tier Garnet link took the bank-group rung
         * and the chip-tier link the bank rung.
         *
         * The projection, derived from that anchor rather than chosen:
         *   L3 <- rung[channel]          (5)
         *   L2 <- one tier inward        (rank=4 for DDR; BG=2 for folded HBM)
         *   L1 <- two tiers inward       (chip=3 for DDR; bank=1 folded)
         *   L0 <- the remaining inner rungs, folded at their MINIMUM
         *         bandwidth -- the bottleneck governs a multi-tier path.
         * channelBearingLevel() already says whether the technology folds its
         * channels into the chip slot (HBM), so the fold follows it. */
        const int chanL = pimid_htree::channelBearingLevel(
            std::max(1, config.hierarchy_dram_channels),
            config.hierarchy_chips_per_rank);
        auto rungOfTreeTier = [&](int t) -> int {
            // folded HBM: the tree's chip-slot tier carries the CHANNEL link
            if (chanL == 3) return (t < 3) ? t : 5;
            return t;                     // unfolded DDR: tree tier == rung
        };
        for (int li = 3; li >= 1; --li) {
            const int tier = chanL - (3 - li);
            if (tier < 0) break;
            const int r = rungOfTreeTier(tier);
            const int    wb = config.sourced_ladder_w[r];
            const double bw = config.sourced_ladder_bw[r];
            if (wb > 0 && bw > 0.0) {
                L[li].width_bits = wb;
                L[li].freq_ghz   = bw * 8.0 / static_cast<double>(wb);
            }
        }
        {   // L0: fold the inner rungs at the bottleneck
            double best_bw = -1.0; int best_w = 0;
            for (int tier = 0; tier <= chanL - 3; ++tier) {
                const int r = rungOfTreeTier(tier);
                const int    wb = config.sourced_ladder_w[r];
                const double bw = config.sourced_ladder_bw[r];
                if (wb > 0 && bw > 0.0 && (best_bw < 0.0 || bw < best_bw)) {
                    best_bw = bw; best_w = wb;
                }
            }
            if (best_w > 0) {
                L[0].width_bits = best_w;
                L[0].freq_ghz   = best_bw * 8.0 / static_cast<double>(best_w);
            }
        }
        N = std::max(1, config.hierarchy_dram_channels);

        /* Test hook, gate-only: PIMID_FABRIC_BREAK corrupts one layer so the
         * gate can prove the invariant FIRES. Five vacuous passes this cycle
         * came from checks whose firing was never demonstrated. */
        if (getenv("PIMID_FABRIC_BREAK")) L[3].width_bits *= 2;

        /* THE INVARIANT (design section 5): each 1:1 layer must carry exactly
         * its source rung's bandwidth; L0 the stated fold. Violation is FATAL
         * -- this is the check that turns "the two fabrics drifted" from a
         * four-round audit finding into an immediate failure. */
        for (int li = 3; li >= 1; --li) {
            const int tier = chanL - (3 - li);
            if (tier < 0) continue;
            const int r = rungOfTreeTier(tier);
            const double rung_bw = config.sourced_ladder_bw[r];
            if (rung_bw <= 0.0) continue;
            const double layer_bw = L[li].width_bits / 8.0 * L[li].freq_ghz;
            if (std::fabs(layer_bw - rung_bw) > 0.01 * rung_bw) {
                std::cerr << "[fabric] FATAL: topology layer L" << li
                          << " carries " << layer_bw << " GB/s but its source"
                             " rung " << r << " carries " << rung_bw
                          << " GB/s. One fabric description, two consumers --"
                             " the projection must preserve each tier's"
                             " bandwidth, and this divergence is exactly the"
                             " drift the invariant exists to refuse."
                          << std::endl;
                std::exit(2);
            }
        }
        std::cout << "  [htree] Garnet topology PROJECTED from the sourced"
                     " ladder, channel-anchored (bits/layer "
                  << L[0].width_bits << "/" << L[1].width_bits
                  << "/" << L[2].width_bits << "/" << L[3].width_bits
                  << ", " << N << " channel(s); L0 = bottleneck fold of the"
                     " inner rungs) -- invariant checked\n";
    } else {
        std::cout << "  [htree] Garnet topology from the per-technology TABLE:"
                     " no sourced ladder was adopted for " << tech
                  << ", so these widths are placeholders, not tool-sourced\n";
    }

    /* 1.10.3: the per-technology table above is the DEFAULT, not a ceiling.
     *
     * Defaults describe the memory as it is built. But the point of this
     * simulator is to ask what a memory could be, and a user exploring a
     * PIM-optimised part may well want a wider channel link or a faster inner
     * datapath than any shipping device has. noc.levels[<tier>].link_width_bits
     * and .frequency_ghz already exist for exactly that, and are already parsed
     * -- they simply never reached this builder, so on the DEFAULT detailed-DRAM
     * path they were accepted and silently discarded. The same shape of fault as
     * the fabric key 1.10.1 had to start reporting.
     *
     * The override is expressed per TIER (subarray..channel), which is how a
     * user thinks about the device, and mapped onto the layer that tier belongs
     * to by the same rule the tree itself uses. Overriding a tier that shares a
     * layer with another tier moves both -- unavoidable with four layers over
     * six tiers, and better than ignoring the key. */
    {
        const int chanLevel = pimid_htree::channelBearingLevel(
            N, config.hierarchy_chips_per_rank);
        for (int lvl = 0; lvl < 6; ++lvl) {
            const auto& ov = config.network_level_overrides[lvl];
            if (ov.link_width_bits <= 0 && ov.frequency_ghz <= 0.0) continue;
            int li = pimid_htree::layerForLevel(lvl, chanLevel);
            if (ov.link_width_bits > 0) L[li].width_bits = ov.link_width_bits;
            if (ov.frequency_ghz > 0.0) L[li].freq_ghz  = ov.frequency_ghz;
            std::cerr << "[config] in-memory fabric: tier " << lvl
                      << " overridden by noc.levels -> layer L" << li
                      << " width=" << L[li].width_bits << "b freq="
                      << L[li].freq_ghz << "GHz (hypothetical fabric, not "
                      << tech << " as built)\n";
        }
    }

    /* 1.11.56 (audit B012): REF_BW is defined as "the widest layer bandwidth
     * across all technologies = HBM3 L0", so ask the HBM3 architecture object
     * for its L0 (GSA datapath) bandwidth rather than writing the product
     * down. REF_FREQ has no such definition to derive from -- it is the
     * reference clock the per-layer latency ladder is quoted against -- so it
     * stays a stated constant and is declared as one in the provenance
     * report. */
    const double REF_BW   = referenceLayer0BandwidthGBs();
    const double REF_FREQ = 2.4;    // GHz -- stated reference clock, not derived

    // Per-layer emitted link width (bits) and latency (cycles).
    // CONCURRENCY WEIGHTING: a bank-placed PE accesses exactly ONE channel, so
    // the channels are utilized COLLECTIVELY -- the number of channels actually
    // active = the number of channels that hold an active PE-bank =
    // min(num_pes, N) (PEs round-robin across the N channel subtrees). Weight
    // each link's effective bandwidth (width) by that concurrency factor:
    //   * single-PE (num_pes==1) -> factor 1 -> links carry the TRUE per-bank /
    //     per-channel bandwidth -> shows INTERNAL bandwidth (channel count
    //     irrelevant; differentiated by per-bank width/clock).
    //   * multi-PE (num_pes>=N)  -> factor N -> AGGREGATE bandwidth (HBM3 16x >
    //     HBM2 8x > single-channel DDR).
    // This is the physical basis for the two-part DRAM sweep (1-PE internal vs
    // multi-PE aggregate) for the same workload.
    /* 1.11.60 (ONE FABRIC, user-approved; corrects round-4 B002).
     *
     * The width written into the topology file is RAW BITS -- the same number
     * the analytical ladder prices -- because the file's field is documented
     * and consumed as bits. The old code wrote a REF-normalised SCORE
     * (128 x bw x concurrency / REF_BW; DDR4's 256-bit subarray rung went in
     * as "24") into that bits field, so Garnet flit-ised messages against a
     * dimensionless number. One description, two consumers, same units.
     *
     * Garnet's flit engine caps a link at 128 bits; a wider rung is clamped
     * AT THE CONSUMER BOUNDARY and the clamp is printed -- the description
     * itself keeps the true width, and the invariant above was checked
     * against the description, not the clamp. The concurrency weighting is
     * gone with the score: channel parallelism is carried by the N
     * materialised channel subtrees of the tree itself, and weighting the
     * width too counted it twice. */
    int layer_w[4], layer_lat[4];
    bool any_clamped = false;
    for (int i = 0; i < 4; ++i) {
        int w = L[i].width_bits;
        if (w < 1) w = 1;
        if (w > 128) { w = 128; any_clamped = true; }
        layer_w[i] = w;
        long lat = std::lround(REF_FREQ / L[i].freq_ghz);
        if (lat < 1) lat = 1;
        layer_lat[i] = (int)lat;
    }
    if (any_clamped)
        std::cout << "  [htree] one or more layer widths exceed Garnet's"
                     " 128-bit flit bound and are clamped IN THE CONSUMER;"
                     " the fabric description keeps the true widths\n";

    // -- SPARSE, PLACEMENT-DRIVEN H-TREE (regenerated per sim) ----------------
    // Built by the SHARED builder (external/zsim/src/sparse_htree.h) that the
    // PE-MI routing ALSO calls, so the emitted Garnet endpoint ids and the runtime
    // routing can never drift (invariant #2). Only PE-hosting branches are
    // materialized down to the placement level; each router with empty children
    // gets ONE abstract endpoint at the maximal-empty-subtree root (invariant #4).
    // A non-PE unit routes to its region's abstract endpoint (real tiered distance)
    // + Ramulator device time; own unit = 0 hops. Sparse sweeps stay tiny
    // (nodes ~ PEs x depth); a fully-placed device grows the complete tree.
    std::vector<uint64_t> peHomes;
    for (int pe = 0; pe < num_pes; ++pe) {
        uint64_t home = (uint64_t)pe;
        if (pe < (int)config.pe_mem_map.size() && !config.pe_mem_map[pe].mem_org_ids.empty())
            home = (uint64_t)config.pe_mem_map[pe].mem_org_ids[0];
        peHomes.push_back(home);
    }
    pimid_htree::SparseHTree tree = pimid_htree::buildSparseHTree(
        peHomes, pe_level, N,
        config.subarrays_per_bank, config.hierarchy_banks_per_bg,
        config.hierarchy_bg_per_chip, config.hierarchy_chips_per_rank,
        config.hierarchy_ranks_per_channel, layer_w, layer_lat);

    /* 1.10: the tree must cover the memory exactly. Every organisation at the
     * placement level is either hosted by a processing element or sits behind
     * exactly one aggregated endpoint, so the covered total must equal the count
     * the rest of the system uses -- the same field the element-to-memory
     * mapping and the power model are derived from.
     *
     * Checked against THAT field, deliberately, and not against a second
     * computation of our own. A first version of this check recomputed the total
     * independently, agreed with itself, and sailed past a sixteen-fold
     * channel double-count on every stacked-memory cell. A tree verified only
     * against itself proves nothing.
     *
     * A mismatch is fatal rather than a warning: every access cost and every
     * energy figure is computed against this structure, so a gap means some
     * memory is priced by nothing, or by something twice. */
    {
        long covered = tree.coveredOrgs();
        long expected = (long)config.total_mem_orgs;
        if (expected > 0 && covered != expected) {
            std::cerr << "[htree] FATAL: the placement tree does not cover the "
                      << "configured memory. It accounts for " << covered
                      << " organisations at the placement level; the configuration "
                      << "describes " << expected << ". Every access cost and every "
                      << "energy figure is computed against this tree, so this is "
                      << "not a reporting problem -- some memory would be priced by "
                      << "nothing, or by something twice.\n";
            std::exit(2);
        }
        /* 1.10.5: record what was actually built, for the power model.
         *
         * Until now the power model described a different machine from the one
         * the timing model routes on: a square mesh with one router per
         * element. The tree is neither square nor one-per-element -- it is
         * sparse, its router count follows elements x depth, and many of its
         * routers are single-child pass-throughs that are wire, not logic.
         *
         * A pass-through is counted separately from a branch point because
         * only a branch point arbitrates. Charging a router's crossbar and
         * arbiter to a node that merely forwards would inflate fabric power by
         * whatever fraction of the tree is degenerate -- which for a coarse
         * placement is most of it. */
        std::map<int,int> childrenOf;
        for (const auto& l : tree.intLinks) childrenOf[l.a]++;
        int branch = 0;
        for (const auto& kv : childrenOf) if (kv.second >= 2) ++branch;
        config.htree_all_routers    = tree.numRouters;
        config.htree_branch_routers = branch;
        for (int l = 0; l < 7; ++l) {
            config.htree_level_branch[l]    = tree.branchAtLevel[l];
            config.htree_level_endpoints[l] = tree.endpointsAtLevel[l];
        }
        config.htree_endpoints      = tree.totalEndpoints();
        config.htree_abstract       = tree.numAbstract;
        std::cout << "[htree] " << branch << " branch routers of "
                  << tree.numRouters << " (" << (tree.numRouters - branch)
                  << " pass-through), " << tree.totalEndpoints()
                  << " endpoints\n";

        std::cout << "[htree] " << tree.numRouters << " routers, "
                  << tree.totalEndpoints() << " endpoints ("
                  << tree.numPEs << " PE + " << tree.numAbstract
                  << " aggregated), covering " << covered
                  << " organisations at level " << pe_level << std::endl;

        /* 1.10: WHERE the aggregated endpoints sit, and how much each fronts.
         *
         * This decides whether the memory below them needs a network model at
         * all. Everything under an aggregated endpoint is array, and the array's
         * shared datapath -- bank conflicts, bus turnaround, the global dataline
         * -- is already priced by the technology's own timing model. Adding a
         * second, analytical network term over the same path would charge that
         * contention twice.
         *
         * That reasoning holds only while the endpoint sits AT OR BELOW the
         * channel, because the memory model works within a channel. An endpoint
         * at rank or system level fronts a path that neither the network model
         * nor the memory model covers, and that IS a gap rather than a
         * duplication. Reported rather than assumed, because the level is
         * emergent today -- it falls out of wherever the emptiness begins. */
        static const char* lvl_name[] = { "subarray", "bank", "bankgroup",
                                          "chip", "rank", "channel", "system" };
        std::map<int,int>  per_level;
        std::map<int,long> orgs_at_level;
        for (const auto& kv : tree.frontsLevel) {
            per_level[kv.second]++;
            auto c = tree.coverageOf.find(kv.first);
            if (c != tree.coverageOf.end()) orgs_at_level[kv.second] += c->second;
        }
        for (const auto& kv : per_level) {
            int L = kv.first;
            const char* nm = (L >= 0 && L <= 6) ? lvl_name[L] : "?";
            std::cout << "[htree]   " << kv.second << " aggregated at " << nm
                      << " level, fronting " << orgs_at_level[L] << " organisations"
                      << (L > 5 ? "  <-- ABOVE CHANNEL: not covered by the memory "
                                  "model either; this path is priced by nothing"
                                : "")
                      << std::endl;
        }
    }

    std::ofstream f(outPath);
    if (!f.is_open()) return false;
    f << "# Auto-generated SPARSE placement-driven DRAM H-tree (regenerated per sim).\n";
    f << "# Only PE-hosting branches materialized to the placement level; each empty\n";
    f << "# region -> ONE abstract endpoint. endpoint e<num_pes == PE e; e>=num_pes\n";
    f << "# == an abstract region (non-PE units route there + Ramulator device time).\n";
    f << "# tech=" << tech << " channels=" << N << " num_pes=" << num_pes
      << " pe_level=" << pe_level << " routers=" << tree.numRouters
      << " endpoints=" << tree.totalEndpoints()
      << " (PE=" << tree.numPEs << " abstract=" << tree.numAbstract << ")\n";
    f << "# per-layer (width_bits, latency_cyc): L0=(" << layer_w[0] << "," << layer_lat[0]
      << ") L1=(" << layer_w[1] << "," << layer_lat[1] << ") L2=(" << layer_w[2] << ","
      << layer_lat[2] << ") L3=(" << layer_w[3] << "," << layer_lat[3] << ")\n";
    f << "routers " << tree.numRouters << "\n";
    f << "endpoints " << tree.totalEndpoints() << "\n";
    for (auto& e : tree.intLinks) {
        f << "int " << e.a << " " << e.b << " 1 " << e.lat << " " << e.w << "\n";
        f << "int " << e.b << " " << e.a << " 1 " << e.lat << " " << e.w << "\n";
    }
    for (auto& e : tree.extLinks)
        f << "ext " << e.a << " " << e.b << " " << e.lat << " " << e.w << "\n";
    f.close();
    return true;
}

/**
 * @brief Pre-compute hierarchy latencies from InternalDRAMNetwork.
 *
 * Instantiates the full 7-level hierarchy for the configured memory technology,
 * applies bridge overrides, and queries per-level transfer and bridge latencies
 * for a 64-byte cache line. Results are stored as integers in UnifiedConfig
 * so they can be emitted into the ZSim config file without library dependencies.
 */
static bool sharedMemoryCoupled(const UnifiedConfig& config) {
    /* 1.11.46 (FIX-PRE-FLEET L234/L244): coupled-vs-decoupled is the DECLARED
     * TOPOLOGY -- device.is_default_mem, the same flag resolveMemoryTopology
     * already enforces (false requires a host.mem block; true drives host main
     * memory from the device technology BY CONSTRUCTION). It used to be
     * decided by memory-technology STRING EQUALITY, whose .empty() guards were
     * dead (memory_tech defaults to "DDR4") and which disagreed with the
     * topology resolver the moment a decoupled system happened to name the
     * same technology on both sides -- same tech string does not mean same
     * silicon. E27 (1.11.45) guarantees at most one memory-bearing device.
     *
     * 1.11.57 (latent B045): the .empty() guard the paragraph above calls dead
     * survived the rewrite and was still filtering this loop. SystemNode's
     * memory_tech defaults to "DDR4" and every assignment to it runs through
     * canonicalMemTech(), which refuses an empty string -- so the test could
     * only ever be true, and reading it invited the belief that a device with
     * no declared memory is skipped here. It is not, and there is no such
     * device. The first DEVICE node decides, which is what E27 guarantees is
     * unambiguous. */
    for (const auto& node : config.system_nodes) {
        if (node.role == UnifiedConfig::SystemNode::DEVICE)
            return node.is_default_mem;
    }
    return true;   // no device node at all: single-memory system, coupled
}

/* 1.11.56 (audit B005, B006, B007, B008, B009, B011, B013, B014, B016):
 * the stated-constant register.
 *
 * A block of quantities in this file are neither measured nor tool-sourced:
 * per-technology host-path latency splits, the host/device bridge table, the
 * flush and kernel-launch fixed costs, the system-link presets, the analytical
 * H-tree scaling constants, the network-interface overhead, the MLP degree, and
 * three bare cycle counts for the host fabric, the PE local link and the system
 * network. Each is live -- every one of them is emitted into the timing model or
 * the power model -- and each was silent.
 *
 * The project rule is that a quantity the tools can produce must be MEASURED,
 * and one they cannot must be STATED with its provenance and never presented as
 * a measurement. These are the second kind: there is no tool in this tree that
 * answers "how long does a kernel launch take", and inventing a citation would
 * be worse than admitting there is none. So the run now declares them -- value,
 * the key that overrides it, and what it governs -- for the ones actually in
 * effect. A reader can then see exactly which numbers in the report rest on an
 * assumption, and the paper can say so.
 *
 * This prints; it changes nothing. Anything here that a tool CAN answer should
 * be moved out of this list and derived instead (B004 and B012 were, this
 * release). */
/* 1.11.56 (audit B042): one memory, three caps, reconciled out loud.
 *
 * This file uses three different quantities under the name "the memory's
 * bandwidth", and emits all three into the same ZSim config:
 *   - the RANK data bus, as mem.bandwidth (the device M/D/1 service rate)
 *   - the AGGREGATE device/stack figure, as nocAggBandwidthMBs (the NoC cap)
 *   - the PER-CHANNEL figure, as the host mem.bandwidth (the host MC cap)
 * For HBM3 the first and second differ by 16x, and nothing said whether that
 * is the channel count doing its job or two contradictory claims. They are
 * consistent exactly when aggregate == per_channel x channels and
 * per_channel == rank_bw x ranks_per_channel. This states the three with
 * their scopes and CHECKS that relation, so a preset whose numbers do not
 * reconcile is visible instead of silently producing three caps that cannot
 * all describe one part. */
/* 1.11.57 (audit round 3, A007 + B003): ONE answer to "which clock are these
 * cycles in?".
 *
 * A simulated cycle count belongs to the clock of the machine that ticked it.
 * In DEVICE scope that is config.frequency_mhz. In SYSTEM scope it is the
 * DEVICE node's clock -- and config.frequency_mhz is then the top-level
 * system.frequency_mhz, which the adoption block never syncs from the node.
 * Two separate sites had each reached for config.frequency_mhz and each been
 * wrong in system scope: the hierarchy ns->cycle conversion (a 4x overcharge
 * at 2000 vs 500 MHz) and the NoC duty-cycle window (off by ref/top-level).
 * Both now ask here, so a third site cannot invent a third answer. */
static double deviceCycleClockMHz(const UnifiedConfig& config) {
    if (config.scope == "system") {
        for (const auto& n : config.system_nodes) {
            if (n.role == UnifiedConfig::SystemNode::DEVICE && n.frequency_mhz > 0.0)
                return n.frequency_mhz;
        }
    }
    return config.frequency_mhz > 0.0 ? config.frequency_mhz : 1000.0;
}

/* 1.11.60 (audit round 4, A012): ONE answer to "how many memory interfaces
 * does this run have?".
 *
 * num_pes / pes_per_mc is INTEGER division, and pes_per_mc is a user knob the
 * derivation at the end of computeHierarchyLatencies only ever RAISES -- it is
 * never capped at num_pes -- so one PE with pim.mc.pes_per_mc: 2 makes the
 * quotient 0. 1.11.57 (A004) fixed that where it deleted a component from
 * McPAT's power total, and the ZSim emitter has clamped since it was written,
 * but the three PRINT sites were left on the raw quotient. The result is worse
 * than the defect it half-fixed: the provenance table a reader uses to audit
 * the run now says "PE-MIs (x0)" and "endpoints: 4 PEs + 0 MCs" while McPAT
 * prices one controller and ZSim instantiates one. It was invisible because
 * nothing cross-checks a printed count against a priced one, and 0 is a legal
 * value to print.
 *
 * The AUTHORITY is what the run instantiates: the emitted ZSim configuration
 * (pesPerMC / the PE-MI block), which is this clamped count, and which McPAT
 * follows wherever no H-tree endpoint count supersedes it. Every reporting
 * site asks here now, so a fourth cannot invent a fourth answer. */
static int peMemoryInterfaceCount(const UnifiedConfig& config) {
    if (!(config.pe_mc_enabled && config.pes_per_mc > 0))
        return 1;                       // HOST_MC mode: the single host controller
    return std::max(1, config.num_pes / config.pes_per_mc);
}

static bool reportBandwidthScopes(const std::string& tech, int ranks_per_channel) {
    if (!pimid::isDRAM(pimid::parseMemoryTechnology(tech))) return true;
    try {
        pimid::RamulatorWrapper q("", tech);
        q.initialize();
        double agg_mbs  = static_cast<double>(q.getBandwidth());
        int    nch      = static_cast<int>(q.getNumChannels());
        if (nch < 1) nch = 1;
        double rank_gbs = q.getRankBandwidth();
        double chan_gbs = q.getChannelBandwidth();
        double per_chan_mbs = agg_mbs / static_cast<double>(nch);
        int rpc = ranks_per_channel > 0 ? ranks_per_channel : 1;

        std::cout << "  [bw] " << tech << " bandwidth by scope: rank bus "
                  << rank_gbs << " GB/s (device M/D/1 service rate), databus "
                  << chan_gbs << " GB/s (one channel, every family since "
                     "1.11.57 C007), aggregate " << (agg_mbs / 1000.0)
                  << " GB/s over " << nch << " channel(s) (NoC cap; host MC sees "
                  << (per_chan_mbs / 1000.0) << " GB/s per controller)"
                  << std::endl;

        /* The invariant that holds for EVERY family:
         *      aggregate == rank_bus x ranks_per_channel x channels
         *
         * Gate 1166C: the first version of this check compared the "channel"
         * figure against rank x ranks, which is a DDR reading. The
         * architecture object's channel_databus_bits means something
         * different per family -- 64 bits (one channel) for DDR, 1024 bits
         * (the whole STACK, all 16 pseudo-channels) for HBM, where the field
         * called "rank" is the pseudo-channel. So that field cannot carry a
         * family-independent test, and comparing it against the per-channel
         * aggregate reported HBM3 as broken when it was not. The relation
         * above is the one that is true regardless of what each tier is
         * named, and it is exactly the relation the speed-bin drift broke. */
        double expect_agg_mbs = rank_gbs * static_cast<double>(rpc) *
                                static_cast<double>(nch) * 1000.0;
        bool agg_ok = expect_agg_mbs > 0.0 &&
                      std::fabs(expect_agg_mbs - agg_mbs) <= 0.02 * agg_mbs;
        if (!agg_ok) {
            std::cerr << "[bw] WARNING: the " << tech << " bandwidth figures do "
                         "not reconcile. The rank/pseudo-channel bus carries "
                      << rank_gbs << " GB/s, and with " << rpc << " rank(s) per "
                         "channel over " << nch << " channel(s) that is "
                      << (expect_agg_mbs / 1000.0) << " GB/s aggregate -- but "
                         "the preset reports " << (agg_mbs / 1000.0)
                      << " GB/s. The device M/D/1 cap, the NoC cap and the host "
                         "MC cap in this run therefore describe DIFFERENT parts."
                      << std::endl;
        }
        return agg_ok;
    } catch (const std::exception&) {
        // The callers that need these figures report their own failures.
    }
    return false;
}

static void reportStatedConstants(const UnifiedConfig& config) {
    struct Row { const char* what; std::string value; const char* knob; const char* governs; };
    std::vector<Row> rows;
    const bool sysscope = (config.scope == "system");

    if (sysscope) {
        rows.push_back({"host-path idle-latency split", "per-technology table (ns)",
                        "host.host_path.* / host.mem_latency_adder_ns",
                        "the host memory latency emitted to the timing model"});
        rows.push_back({"host<->device bridge table",
                        "per-technology latency / bandwidth / uncached-fraction",
                        "system.bridge.*",
                        "every offload transfer's PHY and protocol charge"});
        rows.push_back({"coherence flush fixed cost",
                        std::to_string(config.coherence_flush_fixed_ns) + " ns",
                        "system.coherence.flush_fixed_ns",
                        "host cycles charged at each ROI entry (the FOOTPRINT half"
                        " of the same charge is measured)"});
        rows.push_back({"kernel-launch cost",
                        "doorbell " + std::to_string(config.launch_doorbell_ns) +
                        " ns + dispatch " + std::to_string(config.launch_dispatch_ns) + " ns",
                        "launch.*",
                        "host cycles charged at each offload"});
        rows.push_back({"system-link presets",
                        "per-link-type latency / bandwidth / header bytes",
                        "system.links[].* / power.link.*",
                        "the offload transfer charge and the link energy duty"});
    }
    /* 1.11.57 (audit round 3, B008): the DETAILED path has stated constants
     * too, and this table claimed to list the run's assumptions while naming
     * only the analytical ones. The H-tree builder's reference clock,
     * resolution and reference hop count run on BOTH paths -- the detailed
     * DRAM topology is emitted from the same builder -- so gating them on
     * !noc_cycle_accurate hid them from exactly the runs the corpus uses. */
    rows.push_back({"H-tree scaling constants",
                    "reference network clock 2.0 GHz, resolution 10, reference hops 8",
                    "(none)",
                    "the NoC link latency, and with it every hierarchy "
                    "traversal, on the analytical AND detailed paths"});
    rows.push_back({"bank-group port width",
                    "the bank serialisation width x 2 (an interleaving assumption)",
                    "(none -- no architecture object carries this field)",
                    "the L2 rung of the hierarchy link ladder: its width, its "
                    "bandwidth, and every level-2 crossing derived from them"});
    /* 1.11.63 (R6-7, audit round 4 B018): COUNT THE PLACEHOLDER RUNGS HERE
     * TOO. The bank-group row above declares the ONE rung that is always an
     * assertion. A rung the architecture object failed to supply is the same
     * kind of thing -- a value in the timing model with no tool behind it --
     * and it belongs in the register a reader uses to audit the run, not only
     * in a line of the hierarchy block. Absent (mask 0) on every technology
     * this tree adopts a ladder for today, which is why the row is
     * conditional: an empty claim would be noise. */
    if (config.sourced_ladder_valid && config.sourced_ladder_placeholder_mask != 0) {
        static const char* kRungNameReg[7] = {
            "subarray (GSA)", "bank", "bank group", "chip DQ",
            "rank bus", "channel", "system root" };
        int n_ph = 0;
        std::string which;
        for (int li = 0; li < 7; ++li) {
            if (config.sourced_ladder_placeholder_mask & (1 << li)) {
                ++n_ph;
                if (!which.empty()) which += ", ";
                which += "L" + std::to_string(li) + " " + kRungNameReg[li];
            }
        }
        rows.push_back({"unsourced hierarchy ladder rungs",
                        std::to_string(n_ph) + " of 7 (" + which +
                            ") on the per-technology placeholder table",
                        "network.levels[].link.width_bits / bandwidth",
                        "those levels' widths, bandwidths and every crossing "
                        "derived from them -- and the Garnet layers projected "
                        "from the same rungs. The run's ladder is a MIXTURE of "
                        "sourced and placeholder values"});
    }
    rows.push_back({"bandwidth reference anchor",
                    "HBM3's per-channel and L0 bandwidth, read from its preset",
                    "(derived, not a constant -- listed so the normalisation is visible)",
                    "per-layer link widths, Garnet link latency and "
                    "flits-per-access on the detailed path"});
    if (!config.noc_cycle_accurate) {
        rows.push_back({"MLP degree M",
                        std::to_string(config.noc_mlp_degree > 0 ? config.noc_mlp_degree : 10),
                        "noc.mlp",
                        "the PE outstanding-access window in the M/D/1 model"});
    }
    rows.push_back({"network-interface overhead", "6 cycles", "(none)",
                    "the average one-way NoC latency, added for every topology "
                    "including BUS and CROSSBAR where the hop count is zero"});
    rows.push_back({"PE local-link latency",
                    std::to_string(config.local_link_latency) + " cycles",
                    "pim.pe.local_link_latency",
                    "the PE<->local-memory hop"});
    if (sysscope) {
        /* 1.11.57 (audit round 3, A008): print the value IN FORCE, not the
         * default. This table exists to tell a reader which numbers the run
         * rests on, so printing a literal where the config carries a parsed
         * override is the one mistake it must not make -- it would report an
         * assumption the run is not actually using. */
        int hop_cy = 4;
        for (const auto& n : config.system_nodes) {
            if (n.role == UnifiedConfig::SystemNode::HOST) {
                hop_cy = n.host_noc_hop_cycles; break;
            }
        }
        rows.push_back({"host fabric hop",
                        std::to_string(hop_cy) + " cycles (core clock)",
                        "hosts[].noc.hop_cycles",
                        "the host core->LLC/MC hop"});
        rows.push_back({"system-network latency",
                        std::to_string(config.system_network.latency_cycles) + " cycles",
                        "system.network.latency_cycles",
                        "the inter-node network hop"});
    }

    std::cout << "\n[provenance] Stated constants in effect (no tool source; "
                 "each is an assumption this run rests on):" << std::endl;
    for (const auto& r : rows) {
        std::cout << "  " << r.what << " = " << r.value << std::endl;
        std::cout << "      governs: " << r.governs << std::endl;
        std::cout << "      override: " << r.knob << std::endl;
    }
    std::cout << "  (Quantities the tools CAN answer are measured instead and "
                 "are not listed here.)" << std::endl;
}

static void computeHierarchyLatencies(UnifiedConfig& config) {
    // Map placement_level string to integer
    if (config.placement_level == "SUBARRAY")       config.pe_hierarchy_level = 0;
    else if (config.placement_level == "BANK")      config.pe_hierarchy_level = 1;
    else if (config.placement_level == "BANK_GROUP") config.pe_hierarchy_level = 2;
    else if (config.placement_level == "CHIP")      config.pe_hierarchy_level = 3;
    else if (config.placement_level == "RANK")      config.pe_hierarchy_level = 4;
    else if (config.placement_level == "CHANNEL")   config.pe_hierarchy_level = 5;  // aggregation (N=1 techs)
    else if (config.placement_level == "LOGIC_DIE") config.pe_hierarchy_level = 6;  // aggregation (HBM base die)
    else if (config.placement_level == "HOST_MC")   config.pe_hierarchy_level = -1;  // PEs share host MC
    else                                             config.pe_hierarchy_level = 1;  // default BANK

    // Validate: PEs at a non-MC tier require a PE-MC at that tier
    // Skip for trace-gen: it only records QEMU execution, no simulation needed
    if (config.method != "trace-gen" && config.method != "synthetic" &&
        config.placement_level != "HOST_MC" && !config.pe_mc_enabled) {
        std::cerr << "ERROR: PEs placed at " << config.placement_level
                  << " level require a memory controller at that tier.\n"
                  << "  Add a 'pim.mc' section to your YAML config, or use "
                  << "'pim.placement.level: HOST_MC' if PEs share the host MC.\n";
        exit(1);
    }

    // HOST_MC: PEs share the host MC, no hierarchy computation needed
    if (config.pe_hierarchy_level == -1) {
        config.hierarchy_enabled = true;
        config.total_mem_orgs = config.num_pes;  // 1:1 identity for mapping
        config.hierarchy_banks_per_bg = 1;
        config.hierarchy_bg_per_chip = 1;
        config.hierarchy_chips_per_rank = 1;
        autoGeneratePEMemMap(config);
        validatePEMemMapping(config);
        config.total_network_endpoints = config.num_pes;
        return;
    }

    // Determine DRAM organization defaults
    int banks_per_bg = 4, bg_per_chip = 4, chips_per_rank = 8;

    // Technology-specific overrides
    std::string tech = config.memory_tech;
    if (tech == "DDR3")          { banks_per_bg = 8; bg_per_chip = 1; chips_per_rank = 8; }
    else if (tech == "DDR4")     { banks_per_bg = 4; bg_per_chip = 4; chips_per_rank = 8; }
    else if (tech == "DDR5")     { banks_per_bg = 4; bg_per_chip = 8; chips_per_rank = 8; }
    else if (tech == "LPDDR5")   { banks_per_bg = 4; bg_per_chip = 4; chips_per_rank = 1; }
    else if (tech == "GDDR6")    { banks_per_bg = 4; bg_per_chip = 4; chips_per_rank = 1; }
    // HBM has 2 pseudo-channels/channel; PIMID has no pseudo-ch level, so fold
    // them into the BG count to match Ramulator2/JEDEC per-channel org:
    //   HBM2 {1,2,4,4} = 2 pch x 4 BG x 4 banks = 32 banks (8 BG/ch)
    //   HBM3 {1,2,4,4} = 2 pch x 4 BG x 4 banks = 32 banks (8 BG/ch)
    // chips_per_rank = channels per stack (HBM2 8, HBM3 16).
    /* 1.11.64: HBM2 folds to 32 banks per channel, not 16 -- the FOURTH and
     * last site of the JESD235D bank/row correction, and the one that
     * actually reaches the emitted geometry. This table feeds tech_min_banks
     * (the "technology requires at least N banks" guard below) and, through
     * config.num_banks, the --total-units the kernel is invoked with. With
     * HBM2 left at 2 x 8 = 16 here while the org preset had moved to 32, a
     * config pinning `banks: 16` was silently accepted, total_units stayed
     * 128 while pages_per_unit halved, and the modelled capacity halved --
     * gate 1174B's H2' arm, which asserts units x pages_per_unit directly.
     * HBM2 and HBM3 now carry the same per-channel bank shape, which is what
     * JEDEC gives them (BA[3:0] per pseudo-channel in both); they still
     * differ in channels per stack, which is the chips_per_rank column. */
    else if (tech == "HBM2") { banks_per_bg = 4; bg_per_chip = 8; chips_per_rank = 8; }
    else if (tech == "HBM3")     { banks_per_bg = 4; bg_per_chip = 8; chips_per_rank = 16; }
    /* 1.9.35: the two lines above BOOK THE CHANNEL DIMENSION TWICE for HBM.
     * chips_per_rank is set to the channel count per stack, as the comment above
     * says, while the channel count is ALSO supplied separately as the tree's
     * root fanout and as the link-width concurrency multiplier. Nothing today
     * multiplies the two, because the rank tier is degenerate and the
     * organisation size is computed without a channel factor -- so every
     * processing element decomposes to channel 0, rank 0 and the duplication
     * cancels. It stops cancelling the moment either tier is given real fanout,
     * and the error would then be a silent eight- or sixteen-fold inflation of
     * the HBM tree, in the direction that makes the fabric look larger and more
     * expensive than it is. Refuse that combination rather than discover it in a
     * result. */
    if ((tech == "HBM2" || tech == "HBM3") && config.hierarchy_ranks_per_channel > 1) {
        std::cerr << "[config] FATAL: " << tech << " folds the channel count into "
                     "chips_per_rank, and memory.ranks_per_channel > 1 would then "
                     "count the channel dimension twice -- an "
                  << chips_per_rank << "x inflation of the in-memory tree. "
                     "Give the channel tier real fanout first (see the NoC "
                     "tree-fidelity work) or leave ranks_per_channel at 1."
                  << std::endl;
        std::exit(2);
    }
    else if (tech == "SRAM" || tech == "STT_MRAM" || tech == "PCM" || tech == "RERAM") {
        banks_per_bg = config.num_banks; bg_per_chip = 1; chips_per_rank = 1;
    }

    // Optional JEDEC device-width override (memory.dram.device_width: x4|x8|x16).
    // Sets chips/rank = 64 / width for a 64-bit DDR channel, and adjusts bank
    // groups/chip per the JEDEC organization (DDR4 x16 -> 2 BG, DDR5 x16 -> 4 BG).
    // The same width also selects the Ramulator2 org preset (see ramulator_wrapper)
    // so the timing model and this hierarchy describe the same part.
    if (!config.dram_device_width.empty()) {
        const std::string& w = config.dram_device_width;
        bool is_ddr = (tech == "DDR3" || tech == "DDR4" || tech == "DDR5");
        bool is_lpddr5 = (tech == "LPDDR5");
        bool is_gddr6 = (tech == "GDDR6");
        bool is_hbm = (tech == "HBM2" || tech == "HBM3");

        if (is_hbm) {
            std::cerr << "ERROR: memory.dram.device_width is not applicable to "
                      << tech << " (stacked HBM has a fixed 128-bank organization, "
                      << "no x4/x8/x16 device width). Remove the key for HBM.\n";
            std::exit(1);
        }
        if (w != "x4" && w != "x8" && w != "x16") {
            std::cerr << "ERROR: memory.dram.device_width=" << w
                      << " is invalid. Supported values: x4, x8, x16.\n";
            std::exit(1);
        }
        // Per-tech legal width sets (from Ramulator2 org presets):
        //   DDR3/4/5: {x4,x8,x16}   LPDDR5: {x16}   GDDR6: {x8,x16}
        if (is_lpddr5 && w != "x16") {
            std::cerr << "ERROR: LPDDR5 only has an x16 organization (Ramulator2 "
                      << "presets are LPDDR5_*_x16). device_width=" << w
                      << " is not available for LPDDR5.\n";
            std::exit(1);
        }
        if (is_gddr6 && w == "x4") {
            std::cerr << "ERROR: GDDR6 supports x8 or x16 only (Ramulator2 presets "
                      << "are GDDR6_*_x8 / _x16). device_width=x4 is not available "
                      << "for GDDR6.\n";
            std::exit(1);
        }
        if (is_ddr || is_lpddr5 || is_gddr6) {
            // chips/rank for a 64-bit DDR-class channel -- 1.11.57 (latent
            // A013): from the shared table, which the die-count helper reads too
            chips_per_rank = chipsPerRankForDeviceWidth(w);

            // Bank-group count is coupled to width in the JEDEC tables.
            if (tech == "DDR4")      bg_per_chip = (w == "x16") ? 2 : 4;
            else if (tech == "DDR5") bg_per_chip = (w == "x16") ? 4 : 8;
            // DDR3 has no bank groups (banks_per_bg=8, bg_per_chip=1) at any width.
            // LPDDR5/GDDR6 keep 4 bank groups across their available widths.
        }
    }

    // Optional YAML override of the per-tech JEDEC defaults (bank-groups/chip and
    // banks/bank-group are real params): memory.organization.{bank_groups,
    // banks_per_group}. 0 = keep the JEDEC default set above.
    if (config.bg_per_chip_override > 0)  bg_per_chip  = config.bg_per_chip_override;
    if (config.banks_per_bg_override > 0) banks_per_bg = config.banks_per_bg_override;

    config.hierarchy_banks_per_bg = banks_per_bg;
    config.hierarchy_bg_per_chip = bg_per_chip;
    config.hierarchy_chips_per_rank = chips_per_rank;

    /* 1.10.1: activate-budget (tFAW) density warning.
     *
     * JEDEC bounds row activation per rank with a rolling four-activate window:
     * at most four ACTIVATE commands may begin in any tFAW interval. The window
     * is a CHARGE limit, not a timing convenience -- activation is the
     * current-hungry operation, and tFAW is what holds a rank inside its supply
     * budget. Ramulator2 carries it per technology as the nFAW/.window entry.
     *
     * A PIM placement can ask for more than the window allows. Placing elements
     * below the rank gives each one its own rows to open, so the rank is asked
     * for as many independent activation streams as there are elements sharing
     * it. Past four, the standard says they cannot all proceed.
     *
     * We warn rather than refuse. Beating the window is exactly the kind of
     * design this simulator exists to explore, and a real part could widen it
     * with more charge pumps -- that is the user's call, not ours. But it must
     * be a stated call: the timing model prices these accesses WITHOUT
     * enforcing tFAW, so a placement past the window is optimistic by however
     * much the standard would have serialised, and the power model has no term
     * for the extra charge either. Silence here would read as endorsement. */
    const bool faw_tech = !(tech == "SRAM" || tech == "STT_MRAM" ||
                            tech == "PCM"  || tech == "RERAM");
    if (faw_tech && config.pe_hierarchy_level >= 0 &&
        config.pe_hierarchy_level <= 3 && config.num_pes > 0) {
        const int kActivateWindow = 4;   // JEDEC nFAW: four activates per tFAW
        int ranks = config.hierarchy_ranks_per_channel;
        if (ranks < 1) ranks = 1;
        int pes_per_rank = (config.num_pes + ranks - 1) / ranks;
        if (pes_per_rank > kActivateWindow) {
            std::cerr << "[config] WARNING: " << config.num_pes << " elements at "
                      << config.placement_level << " placement put " << pes_per_rank
                      << " independent activation streams on one " << tech
                      << " rank, above the JEDEC four-activate (tFAW) window.\n"
                      << "  The window is a charge limit. PIMID does not enforce "
                         "it in the timing model, so this configuration is "
                         "optimistic by whatever tFAW would have serialised, and "
                         "the power model carries no term for the extra "
                         "activation charge.\n"
                      << "  Keep it if the design intends to widen the window "
                         "(more charge pumps); otherwise reduce pim.pe.count to "
                      << kActivateWindow * ranks
                      << " or place the elements at a coarser tier.\n";
        }
    }

    // --- Subarray geometry (physical sub-bank structure, BELOW the JEDEC row) ---
    // A subarray is a vertical group of wordlines (rows) inside a bank. It is NOT
    // part of the JEDEC standard, nor of Ramulator2 (both stop at row/column), so
    // we size it explicitly: subarrays_per_bank = bank_rows / subarray_height.
    //
    // subarray_height (rows) is grounded in published die measurements where they
    // exist, otherwise the DRAM-family representative. We round the height UP to a
    // power of two so the per-bank count is a clean power of two for addressing,
    // and the rounded height stays >= measured (coarser than silicon, never finer):
    //   DDR4 measured subarray height ~= 576-640 rows  -> 512-row DDR/LPDDR/GDDR default
    //   HBM2 measured subarray height ~= 768-832 rows  -> 1024-row HBM default
    /* 1.11.61 (ruling R4): bank_rows IS DERIVED FROM THE SIMULATED PRESET, not
     * tabulated beside it.
     *
     * The literal table this replaces stated its own basis -- "the JEDEC
     * per-device bank row count at the representative density (DDR3/4 + HBM2
     * 8Gb, DDR5 + GDDR6 16Gb, LPDDR5 + HBM3 8Gb)" -- and that basis is not the
     * part any run simulates. Against the org presets the wrapper names, three
     * of the seven rows were 2x off (_1164audit/DENSITY_INVESTIGATION.md 6.3):
     *
     *   DDR3    preset DDR3_8Gb_x8  131072 rows   table  65536   2x LOW
     *   HBM2    preset HBM2_4Gb      32768 rows   table  65536   2x HIGH
     *   HBM3    preset HBM3_4Gb      16384 rows   table  32768   2x HIGH
     *   DDR4/DDR5/LPDDR5/GDDR6                    table == preset, all four
     *
     * This is LIVE-NUM and structural: subarrays_per_bank = bank_rows /
     * subarray_height feeds total_mem_orgs, the sparse H-tree's shape, the
     * tree-coverage assertion, pages_per_unit and the NoC endpoint and router
     * counts. The row count belongs to the device, so it comes from the device
     * -- RamulatorWrapper::getPresetRowsPerBank(), which reads the org preset
     * row this run's timing model parses, keyed by the configured device
     * width.
     *
     * THE CHECK THAT THE DERIVATION IS RIGHT: the four technologies whose table
     * entry already agreed with their preset must reproduce IDENTICALLY under
     * it. They do -- DDR4 65536, DDR5 65536, LPDDR5 32768, GDDR6 16384 -- so
     * only the three known-wrong rows move, and they move onto the preset.
     *
     * subarray_height does NOT come from the preset and stays a literal here:
     * a subarray is a vertical group of wordlines BELOW the JEDEC row, absent
     * from both the standard and Ramulator2, so the preset has nothing to say
     * about it. The heights below are die measurements, rounded up to a power
     * of two (coarser than silicon, never finer).
     *
     * The user may still override the height (memory.subarray_height) or the
     * count (memory.subarrays_per_bank) directly in YAML. */
    {
        bool is_dram = !(tech == "SRAM" || tech == "STT_MRAM" ||
                         tech == "PCM"  || tech == "RERAM");
        if (is_dram) {
            int subarray_height = config.subarray_height;  // 0 = per-tech default
            // Measured die geometry, not preset organisation -- see above.
            if (subarray_height <= 0)
                subarray_height = (tech == "HBM2" || tech == "HBM3")
                                      ? 1024    // HBM2 measured ~768-832 -> 1024
                                      : 512;    // DDR4 measured ~576-640 -> 512
            config.subarray_height = subarray_height;       // store the resolved height

            long long bank_rows = 0;
            std::string preset_name;
            try {
                pimid::RamulatorWrapper geo("", tech);
                geo.setDeviceWidth(config.dram_device_width);
                geo.initialize();
                bank_rows = static_cast<long long>(geo.getPresetRowsPerBank());
                preset_name = geo.getPresetOrganization().preset_name;
            } catch (const std::exception&) { bank_rows = 0; }

            if (bank_rows <= 0) {
                /* Refuse rather than substitute. The old table is gone on
                 * purpose: reinstating a literal here would restore the second
                 * authority this ruling removed, and every quantity that
                 * depends on it (the tree shape, the coverage assertion,
                 * pages_per_unit) would be built on a part nothing simulates. */
                std::cerr << "[config] FATAL: could not read the bank row count "
                             "for " << tech << " from the Ramulator org preset "
                             "the run simulates. subarrays_per_bank, the "
                             "in-memory tree shape, the tree-coverage assertion "
                             "and pages_per_unit all derive from it, and this "
                             "release removed the per-technology literal table "
                             "that used to stand in for it (1.11.61 ruling R4), "
                             "because that table described a different part."
                          << std::endl;
                std::exit(2);
            }

            long long derived_sa = bank_rows / subarray_height;
            if (derived_sa < 1) derived_sa = 1;
            if (!config.subarrays_per_bank_user_set)
                config.subarrays_per_bank = static_cast<int>(derived_sa);

            /* The derivation is printed because it decides the tree. Before
             * this release the count came from a table nothing showed. */
            std::cout << "  Subarray geometry from preset " << preset_name
                      << ": " << bank_rows << " rows/bank / " << subarray_height
                      << " rows per subarray = " << derived_sa
                      << " subarrays/bank";
            if (config.subarrays_per_bank_user_set)
                std::cout << " -- OVERRIDDEN by memory.subarrays_per_bank = "
                          << config.subarrays_per_bank;
            std::cout << std::endl;
        }
    }

    // Validate PE count fits the memory organization at the placement level
    int pe_level = config.pe_hierarchy_level;
    int slots = 1;
    /* Compute total available slots at the PE placement level.
     * Level 0=subarray, 1=bank, 2=BG, 3=chip, 4=rank, 5=channel, 6=logic die.
     *
     * 1.11.56 (audit B060): levels 4, 5 and 6 all used to land on slots=1 --
     * 4 by an explicit "one rank" that ignored memory.ranks_per_channel, and
     * 5/6 by having no branch at all. slots feeds the sharing warning,
     * total_mem_orgs for DRAM, the tree-coverage assertion and
     * pages_per_unit, so a 16-PE LOGIC_DIE HBM3 cell was described as sharing
     * ONE memory organisation, every PE mapped to org 0, and the whole device
     * became a single contiguous address block. */
    const bool ladder_is_dram = !(tech == "SRAM" || tech == "STT_MRAM" ||
                                  tech == "PCM"  || tech == "RERAM");
    int ranks_pc = std::max(1, config.hierarchy_ranks_per_channel);
    /* The DRAM CHANNEL count, not the multi-device count, and resolved HERE
     * because the slot computation runs several hundred lines before the
     * block that normally sets it. Gate 1166A K6:
     * the retired hierarchy_channels_per_system was 1 in every run, so using
     * it left HBM3's 16 channels invisible and LOGIC_DIE still
     * collapsed to one slot. The technology's own count is 1 for
     * DDR/LPDDR5/GDDR6, 8 for HBM2 and 16 for HBM3; it is stored back into
     * the config so the later block and this one cannot disagree. */
    if (ladder_is_dram && config.hierarchy_dram_channels <= 1) {
        try {
            pimid::RamulatorWrapper ch("", tech);
            ch.initialize();
            uint32_t nch = ch.getNumChannels();
            if (nch >= 1) config.hierarchy_dram_channels = static_cast<int>(nch);
        } catch (const std::exception&) { /* leaves the default of 1 */ }
    }
    int chans    = std::max(1, config.hierarchy_dram_channels);
    /* Gate 1166B: levels 0-3 keep the ONE-RANK frame they have always used.
     * Multiplying them by the channel count is arithmetically defensible and
     * wrong here: the placement tree is built within a rank, so an HBM3 BANK
     * cell went from 512 organisations to 8192 and the tree-coverage check
     * refused the run -- correctly, because the tree would then have priced
     * only a sixteenth of the memory the config described. The tiers B060 is
     * actually about are the three at the top, which had no branch at all. */
    if (pe_level == 0)      slots = config.subarrays_per_bank * banks_per_bg * bg_per_chip * chips_per_rank;
    else if (pe_level == 1) slots = banks_per_bg * bg_per_chip * chips_per_rank;
    else if (pe_level == 2) slots = bg_per_chip * chips_per_rank;
    else if (pe_level == 3) slots = chips_per_rank;
    else if (pe_level == 4) slots = ranks_pc;           // ranks in the channel
    else if (pe_level == 5) slots = chans;              // channels
    /* LOGIC_DIE: there is one base die, but the elements on it front the
     * stack's channels -- that is the endpoint count they distribute over.
     * Charging them one slot said a 16-PE HBM3 stack shares a single memory
     * organisation, mapped every element to org 0, and made pages_per_unit
     * the whole device in one contiguous block. */
    else if (pe_level == 6) slots = chans;

    if (config.num_pes > slots) {
        std::cerr << "WARNING: " << config.num_pes << " PEs requested at "
                  << config.placement_level << " level, but memory organization only has "
                  << slots << " slots. PEs will share memory org endpoints.\n";
    }

    auto hierarchy = pimid::createInternalDRAMNetwork(
        config.memory_tech, config.subarrays_per_bank,
        banks_per_bg, bg_per_chip, chips_per_rank);

    if (!hierarchy) {
        config.hierarchy_enabled = false;
        return;
    }

    /* 1.11.56 (audit D064/D065): take the per-level link widths from the DRAM
     * architecture object instead of the per-technology tables, which the file
     * itself labels "design-specific placeholders ... Current values may be
     * inverted".
     *
     * The two ladders CONTRADICTED each other. The tables gave DDR4 L3 -- the
     * level named "chip DQ pins" -- a 192-bit link, against the 8 bits an x8
     * part actually has, a factor of 24; and they gave DDR5 L5 (channel) an
     * 88-bit link chosen to land near 26.4 GB/s, a bus width no DRAM has. The
     * oracle already carries the real ladder per tier (GSA datapath, bank
     * serialisation, bank-group port, chip DQ, rank data bus, channel data
     * bus) with the matching bandwidths, so it becomes the one authority and
     * the tables stay only as the fallback for a technology with no
     * architecture object.
     *
     * YAML overrides are applied AFTER this, so a user width still wins. */
    /* 1.11.57 (audit round 3, B001 + B002): CHECK BEFORE ADOPTING.
     *
     * Two defects, one fix. B001: RamulatorWrapper builds an architecture
     * object for DDR4/DDR5/HBM2/HBM3 only, and hands DDR3, LPDDR5 and GDDR6
     * the DDR4-2400 one from an unannounced else. So those three received
     * DDR4's ladder -- measured, DDR3/DDR4/DDR5/LPDDR5/GDDR6 all printed the
     * identical 256/8/16/8/64/64/64 -- under a line reading "from the GDDR6
     * architecture object", which is a false provenance claim, the category
     * this project treats most seriously. B002: the reconciliation check that
     * detects exactly this ran sixty lines LATER and only warned, after the
     * ladder had already consumed the numbers.
     *
     * The check now runs FIRST and gates adoption. A technology whose
     * architecture object does not reconcile with its own simulated preset
     * keeps the per-technology table -- which the source already labels a
     * placeholder -- and the run says plainly that its ladder is not
     * tool-sourced. Refusing to claim provenance is the honest outcome;
     * fabricating architecture objects for the three missing technologies
     * would not be. */
    bool ladder_sourced = false;
    if (ladder_is_dram) {
        ladder_sourced = reportBandwidthScopes(tech, config.hierarchy_ranks_per_channel);
        if (!ladder_sourced) {
            std::cerr << "[hierarchy] WARNING: the " << tech << " architecture "
                         "object does not reconcile with the preset this tree "
                         "simulates, so its per-level link ladder is NOT adopted. "
                         "The levels keep the per-technology table, which the "
                         "source labels a design-specific placeholder. Level "
                         "widths, bandwidths and latencies in this run are NOT "
                         "tool-sourced." << std::endl;
        }
    }
    if (ladder_is_dram && ladder_sourced) {
        try {
            pimid::RamulatorWrapper lad("", tech);
            lad.setDeviceWidth(config.dram_device_width);
            lad.initialize();
            int    w[7]  = {0,0,0,0,0,0,0};
            double bw[7] = {0,0,0,0,0,0,0};
            w[0]  = lad.getSubarrayPortBits();    bw[0] = lad.getSubarrayBandwidth();
            w[1]  = lad.getBankPortBits();        bw[1] = lad.getBankBandwidth();
            w[2]  = lad.getBankGroupPortBits();   bw[2] = lad.getBankGroupBandwidth();
            w[3]  = lad.getChipIOBits();          bw[3] = lad.getChipIOBandwidth();
            w[4]  = lad.getRankDataBits();        bw[4] = lad.getRankBandwidth();
            w[5]  = lad.getChannelDataBits();     bw[5] = lad.getChannelBandwidth();
            /* L6 is the system root: every channel in parallel over one bus.
             *
             * 1.11.57 (latent B031): the factor here used to read
             * config.hierarchy_channels_per_system, a field with no writer
             * anywhere in the tree -- it was 1 in every run, the identity
             * dressed as a system-scale term. That field is gone.
             *
             * 1.11.57 (audit round 3, C007 residual): the factor that DOES
             * belong here is the DRAM channel count. Until this release
             * channel_databus_bits meant one channel for DDR and the whole
             * STACK for HBM, so L5 already carried the stack and L6 needed no
             * multiplier. C007 made that field mean ONE channel for every
             * family, which left HBM's system root one channel wide -- 64 bits
             * for a part whose root bus is 1024. The root is every channel in
             * parallel, so it is L5 times the technology's channel count: 1
             * for DDR/LPDDR5, 2 for GDDR6, 8 for HBM2, 16 for HBM3. */
            const int nch = std::max(1, config.hierarchy_dram_channels);
            w[6]  = w[5] * nch;                   bw[6] = bw[5] * nch;
            hierarchy->applySourcedLadder(w, bw);
            /* 1.11.63 (R6-7, audit round 4 B018): RECORD WHICH RUNGS THE
             * LADDER DID NOT ACTUALLY SOURCE.
             *
             * applySourcedLadder() adopts a rung only `if (wb > 0 && bw > 0)`
             * and leaves the rest at the per-technology placeholder table --
             * but sourced_ladder_valid was set unconditionally as soon as it
             * returned, and the block below announced "from the architecture
             * object" for all seven. A partially valid ladder would therefore
             * produce a silent MIXTURE of tool-sourced and placeholder levels
             * (and, through sourced_ladder_w, of Garnet topology layers) under
             * a provenance line claiming the whole thing. It is latent today:
             * every accessor returns a positive pair for the four technologies
             * whose ladder is adopted, so the mask below comes out 0 and
             * nothing about the output changes. It goes live the moment a
             * technology is added whose object lacks a datapath field.
             *
             * The same test as applySourcedLadder's, computed once here so the
             * announcement, the register and the network cannot disagree about
             * which rungs are real. */
            config.sourced_ladder_placeholder_mask = 0;
            for (int li = 0; li < 7; ++li) {
                if (!(w[li] > 0 && bw[li] > 0.0))
                    config.sourced_ladder_placeholder_mask |= (1 << li);
            }
            // 1.11.59 (B005): remember it for the Garnet topology builder.
            for (int li = 0; li < 7; ++li) {
                config.sourced_ladder_w[li]  = w[li];
                config.sourced_ladder_bw[li] = bw[li];
            }
            config.sourced_ladder_valid = true;
            /* 1.11.57 (audit round 3, C006): the REAL node counts at the rank
             * and channel tiers. InternalDRAMNetwork assumed 2 and 2 for L4
             * and L5 -- the hop-count input for those levels -- in the same
             * release that taught the placement side to count 16 HBM3
             * channels. The two are now the same numbers. */
            hierarchy->setTopLevelNodeCounts(std::max(1, config.hierarchy_ranks_per_channel),
                                             nch);
            /* 1.11.57 (audit round 3, B006): say that this SUPERSEDES the
             * table printed a few lines above. InternalDRAMNetwork prints its
             * per-level summary from its constructor, which runs before the
             * sourced ladder is applied -- so every DRAM run printed the
             * placeholder widths as "the network initialised", then replaced
             * them silently. A reader or a log parser takes that block as the
             * fabric being simulated. */
            std::cout << "  Hierarchy link ladder from the " << tech
                      << " architecture object (bits/level): "
                      << w[0] << "/" << w[1] << "/" << w[2] << "/" << w[3] << "/"
                      << w[4] << "/" << w[5] << "/" << w[6] << "\n";
            /* 1.11.60 (ONE FABRIC, user requirement): the table prints WITH
             * its upstream references -- each rung's VerifiedValue status and
             * citation from the architecture object itself, so "sourced"
             * is checkable line by line rather than asserted once. */
            static const char* kRungName[7] = {
                "subarray (GSA)", "bank", "bank group", "chip DQ",
                "rank bus", "channel", "system root" };
            for (int li = 0; li < 7; ++li)
                std::cout << "    rung " << li << " " << kRungName[li] << ": "
                          << w[li] << " b, " << bw[li] << " GB/s  ["
                          << lad.getLadderRungProvenance(li) << "]\n";
            std::cout << ""
                      << "    (this REPLACES the per-level table printed above,"
                         " which is the technology placeholder the network is"
                         " constructed with)\n"
                      /* 1.11.58: name the rung that is NOT sourced. The
                       * architecture object carries no bank-group datapath
                       * field, and L2's width and bandwidth are both the bank
                       * figure times an asserted 2. That is a plausible
                       * reading of bank-group interleaving, not a
                       * specification value -- nothing in JEDEC fixes a
                       * bank-group port width, because a bank group is not an
                       * interface boundary. Six of the seven rungs are
                       * sourced; saying "from the architecture object" without
                       * this line would claim seven. */
                      << "    L2 (bank group) is ASSERTED, not sourced: it is"
                         " L1 x 2, an interleaving assumption. The other six"
                         " rungs come from the architecture object.\n";
            /* 1.11.63 (R6-7, B018): and if any rung came back empty, name it.
             * The line above states the ONE rung known to be an assertion;
             * without this, a rung the architecture object failed to supply
             * would sit in the same table looking equally sourced while the
             * network and the Garnet projection had both quietly kept the
             * placeholder value for it. Says the count, the rung numbers and
             * what replaced them. */
            if (config.sourced_ladder_placeholder_mask != 0) {
                int n_ph = 0;
                for (int li = 0; li < 7; ++li)
                    if (config.sourced_ladder_placeholder_mask & (1 << li)) ++n_ph;
                std::cout << "    WARNING: " << n_ph << " of the 7 rungs above are"
                             " NOT sourced -- the " << tech << " architecture object"
                             " supplied no usable width/bandwidth for";
                for (int li = 0; li < 7; ++li) {
                    if (config.sourced_ladder_placeholder_mask & (1 << li))
                        std::cout << " L" << li << " (" << kRungName[li] << ")";
                }
                std::cout << ". Those levels keep the per-technology PLACEHOLDER"
                             " table the source labels design-specific, and the"
                             " Garnet layers projected from them keep it too. This"
                             " ladder is a MIXTURE, not a sourced fabric.\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "[hierarchy] WARNING: the " << tech << " architecture "
                         "object is unavailable (" << e.what() << "), so the "
                         "per-level link widths fall back to the technology "
                         "table, which the source labels a placeholder. Level "
                         "bandwidths in this run are not tool-sourced.\n";
        }
    }

    // Apply per-level link + router overrides from YAML
    bool has_overrides = false;
    for (int i = 0; i < 7; ++i) {
        const auto& ov = config.network_level_overrides[i];
        bool has_link = ov.link_width_bits > 0 || ov.frequency_ghz > 0 ||
                        ov.latency_cycles > 0 || !ov.topology.empty();
        bool has_router = ov.router_latency >= 0 || ov.router_pipeline >= 0 ||
                          ov.router_bypass >= 0 || ov.virtual_networks > 0 ||
                          ov.virtual_channels_per_vn > 0 || ov.input_buffer_depth > 0 ||
                          ov.output_buffer_depth > 0;
        if (has_link || has_router) {
            hierarchy->overrideLevelConfig(i, ov.link_width_bits, ov.frequency_ghz,
                                           ov.latency_cycles, ov.topology,
                                           ov.router_latency, ov.router_pipeline,
                                           ov.router_bypass, ov.virtual_networks,
                                           ov.virtual_channels_per_vn,
                                           ov.input_buffer_depth, ov.output_buffer_depth);
            if (!has_overrides) {
                std::cout << "  YAML level overrides applied:\n";
                has_overrides = true;
            }
            std::cout << "    L" << i << ":";
            if (ov.link_width_bits > 0)        std::cout << " width=" << ov.link_width_bits << "b";
            if (ov.frequency_ghz > 0)          std::cout << " freq=" << ov.frequency_ghz << "GHz";
            if (ov.latency_cycles >= 0)        std::cout << " lat=" << ov.latency_cycles << "cy";
            if (!ov.topology.empty())          std::cout << " topo=" << ov.topology;
            if (ov.router_latency >= 0)        std::cout << " rtr_lat=" << ov.router_latency;
            if (ov.virtual_channels_per_vn > 0) std::cout << " vcs=" << ov.virtual_channels_per_vn;
            std::cout << "\n";
        }
    }

    // Apply per-level model overrides
    for (int i = 0; i < 7; ++i) {
        const auto& m = config.network_level_model[i];
        /* 1.11.52 (audit B026): VALIDATE the per-level model name. Anything
         * that was not "detailed" became SIMPLE, so a typo ("detaild",
         * "Detailed") silently downgraded that level's network model and the
         * run reported results for a model the user did not ask for. The
         * accepted spellings are listed; anything else is a config error. */
        if (m == "detailed") {
            hierarchy->setLevelModel(i, pimid::NetworkModelType::DETAILED);
        } else if (m.empty() || m == "simple" || m == "md1" || m == "analytical") {
            // "simple", "md1", "analytical" (backward compat) -> SIMPLE
            hierarchy->setLevelModel(i, pimid::NetworkModelType::SIMPLE);
        } else {
            std::cerr << "[config] FATAL: noc.levels[" << i << "].model = '" << m
                      << "' is not a network model. Valid: detailed, simple "
                         "(aliases md1, analytical). An unrecognised name used "
                         "to become SIMPLE silently." << std::endl;
            std::exit(2);
        }
    }

    // Apply per-bridge overrides (router params, link params, model)
    for (int i = 0; i < 6; ++i) {
        const auto& ov = config.bridge_overrides[i];
        // Apply router param overrides
        hierarchy->overrideBridgeConfig(i, ov.router_latency, ov.router_pipeline,
                                        ov.router_bypass, ov.virtual_networks,
                                        ov.virtual_channels_per_vn,
                                        ov.input_buffer_depth, ov.output_buffer_depth);
        // Apply bridge model override
        if (!ov.model.empty()) {
            if (ov.model == "simple" || ov.model == "md1")
                hierarchy->setBridgeModel(i, pimid::NetworkModelType::SIMPLE);
            else if (ov.model == "detailed")
                hierarchy->setBridgeModel(i, pimid::NetworkModelType::DETAILED);
            // "auto" or unknown -> leave as AUTO (default)
        }
    }

    /* Query per-level transfer latency for one 64 B line.
     *
     * 1.11.56 (audit B051): ask for TIME, then convert once at the PE clock.
     * These seven numbers and the six bridge numbers are summed by
     * computeHierTraversal() into a single figure the timing model spends as
     * PE cycles -- but each level ticks at its own frequency (since D064 the
     * DQ tiers are at the data rate and the array tiers at the core clock),
     * so summing them as cycles added quantities that are not the same unit.
     * The bridge term was worse: a router cycle count, a nanosecond constant
     * and a beat count went into one integer. */
    /* 1.11.57 (audit round 3, B003): the clock that SPENDS these cycles.
     *
     * 1.11.56 fixed a unit error here and introduced a clock error. These
     * cycles are charged on the DEVICE's elements as they traverse the
     * device's memory hierarchy, so they belong to the device clock -- but
     * config.frequency_mhz is the top-level system.frequency_mhz, and the
     * system-scope adoption block copies the node's technology, PE count,
     * placement, banks and NoC model WITHOUT copying its frequency. On the
     * shipped co-sim example that is 2000 MHz against a 500 MHz device: a 4x
     * overcharge on every hierarchy traversal, and summed in one integer with
     * an array latency that IS quoted at the device clock. The emitter beside
     * it already resolves this properly for nocBandwidthFreqMHz; the same
     * rule applies here. */
    const double hier_clk_mhz = deviceCycleClockMHz(config);
    if (config.scope == "system" && hier_clk_mhz != config.frequency_mhz) {
        std::cout << "  Hierarchy latencies converted at the DEVICE clock ("
                  << hier_clk_mhz << " MHz), not system.frequency_mhz ("
                  << config.frequency_mhz << " MHz)." << std::endl;
    }
    const double pe_ghz = hier_clk_mhz / 1000.0;
    for (int i = 0; i < 7; ++i) {
        double ns = hierarchy->getTransferLatencyNs(
            static_cast<pimid::NetworkLevel>(i), 0, 1, 64);
        int cyc = static_cast<int>(std::ceil(ns * pe_ghz));
        config.hierarchy_level_latency[i] = cyc > 0 ? cyc : 1;
    }
    // Query per-bridge crossing latency (64B)
    // Bridge latency now computed by the bridge model (SIMPLE/MD1/DETAILED/AUTO)
    const auto& bridges = hierarchy->getBridges();
    for (int i = 0; i < 6; ++i) {
        const auto& br = bridges[i];
        const auto& ov = config.bridge_overrides[i];

        // Apply YAML link overrides with backward compat for the latency display
        int lower_w = (ov.lower_width_bits > 0) ? ov.lower_width_bits :
                      (ov.width_bits > 0) ? ov.width_bits : br.lower_width_bits;
        int upper_w = (ov.upper_width_bits > 0) ? ov.upper_width_bits :
                      (ov.width_bits > 0) ? ov.width_bits : br.upper_width_bits;
        double base_ns = (ov.latency_ns >= 0) ? ov.latency_ns :
                         (ov.latency_cycles >= 0) ? static_cast<double>(ov.latency_cycles) :
                         br.latency_ns;

        /* Bridge = 1 hop, 2-port router.
         *
         * 1.11.56 (audit B051): one unit. getBridgeLatencyNs charges the
         * router in the ingress clock, the base delay as the time it already
         * was, and the serialisation as the SLOWER of ingest and egress --
         * then this converts once, at the PE clock, like every level does.
         * The router-latency and bypass resolution moved INTO that call so
         * one place owns it. */
        double br_ns = hierarchy->getBridgeLatencyNs(
            i, 64, lower_w, upper_w, base_ns,
            (ov.router_latency >= 0) ? ov.router_latency : -1,
            (ov.router_bypass  >= 0) ? ov.router_bypass  : -1);
        int br_cyc = static_cast<int>(std::ceil(br_ns * pe_ghz));
        config.hierarchy_bridge_latency[i] = br_cyc > 0 ? br_cyc : 1;

        // Resolve and store bridge model string for display
        // Only show explicit model tag when user overrides; auto-derived stays "auto"
        std::string resolved_model = "auto";
        if (!ov.model.empty()) {
            resolved_model = ov.model;
        }
        config.hierarchy_bridge_model[i] = resolved_model;
    }

    /* 1.11.59: PRINT them. These numbers govern every hierarchy traversal the
     * timing model charges, and until then they existed only inside the
     * generated ZSim config -- which is written to a temporary directory and
     * discarded, so no run's own output showed what it charged. Gate 1169A
     * could not test the B014 fix for exactly this reason: there was no
     * observable. A quantity this load-bearing should be visible in the run
     * that used it.
     *
     * 1.11.60 (audit round 4, B011): print the BRIDGES too, and move the print
     * below the loop that computes them.
     *
     * 1.11.59 printed the seven level latencies under a comment saying those
     * seven "govern every hierarchy traversal". They govern half of it.
     * computeHierTraversal() sums the levels it crosses AND the bridge at every
     * tier boundary between them, and both arrays are emitted side by side as
     * levelLatency0..6 and bridgeLatency0..5. The bridges are the same order of
     * magnitude -- on the measured DDR4 ladder they are ~110/109/57/58/9/12 PE
     * cycles against levels 14/112/59/59/15/12/10 -- so the print closed the
     * smaller half of the gap it was added for while its own comment claimed
     * the whole of it. It was invisible because the only bridge numbers any
     * config-load run emits are the PRE-ladder placeholders inside the
     * construction banner, which applySourcedLadder then overwrites; the
     * post-ladder values appear only in the run summary, which the config-load
     * path never reaches. */
    std::cout << "  Hierarchy level latency (PE cycles, 64 B): ";
    for (int i = 0; i < 7; ++i)
        std::cout << config.hierarchy_level_latency[i] << (i < 6 ? "/" : "");
    std::cout << " at " << hier_clk_mhz << " MHz\n";
    std::cout << "  Hierarchy bridge latency (PE cycles, 64 B): ";
    for (int i = 0; i < 6; ++i)
        std::cout << config.hierarchy_bridge_latency[i] << (i < 5 ? "/" : "");
    std::cout << " at " << hier_clk_mhz << " MHz"
                 " (one per tier boundary; a traversal charges the levels it"
                 " crosses AND the bridges between them)\n";

    config.hierarchy_enabled = true;

    // Compute total_mem_orgs at placement level.
    //
    // DRAM technologies (DDR3/4/5, LPDDR5, GDDR6, HBM2/3) have standardized,
    // complete memory organizations -- the hierarchy is fixed by the standard.
    // Users cannot specify arbitrary bank counts for DRAM. total_mem_orgs is
    // derived from the technology's inherent organization.
    //
    // SRAM/NVM technologies are on-chip scratchpad where the designer has full
    // control over the organization. total_mem_orgs = user's num_banks.
    bool is_dram_tech = !(tech == "SRAM" || tech == "STT_MRAM" || tech == "PCM" || tech == "RERAM");

    if (config.total_mem_orgs < 0) {
        if (is_dram_tech) {
            // DRAM: total_mem_orgs = technology-derived slot count
            config.total_mem_orgs = slots;

            /* Validate: the user's num_banks must meet the technology minimum.
             *
             * 1.11.57 (audit round 3, B011): substitute a BANK count, not
             * `slots`. slots is the endpoint count at the PLACEMENT tier, and
             * it is a bank count only when the placement is BANK. Since
             * 1.11.56 gave the top three tiers their real counts (B060),
             * a RANK, CHANNEL or LOGIC_DIE placement made this line assign a
             * number of RANKS or CHANNELS into num_banks -- under a message
             * that says "banks" and quotes a per-chip bank minimum. The
             * technology's own bank count is what belongs here. */
            int tech_min_banks = banks_per_bg * bg_per_chip;  // minimum per chip
            if (config.num_banks < tech_min_banks) {
                const int tech_banks = banks_per_bg * bg_per_chip * chips_per_rank;
                std::cerr << "WARNING: " << tech << " technology requires at least "
                          << tech_min_banks << " banks per chip ("
                          << banks_per_bg << " banks/BG x " << bg_per_chip << " BG/chip). "
                          << "User specified num_banks=" << config.num_banks
                          << ". Using the technology's bank count of " << tech_banks
                          << ".\n";
                config.num_banks = tech_banks;  // enforce minimum
            }
        } else {
            // SRAM/NVM: user controls the organization freely
            if (pe_level == 0)  // SUBARRAY
                config.total_mem_orgs = config.num_banks * config.subarrays_per_bank;
            else  // BANK or higher
                config.total_mem_orgs = config.num_banks;
        }
    }

    // Placement-layer divisibility: the PE<->mem-org distribution at the chosen
    // tier must be UNIFORM -- either M PEs per mem-org (pes >= orgs, M = pes/orgs)
    // or 1 PE per N mem-orgs (orgs >= pes, N = orgs/pes). That requires one side
    // to divide the other; otherwise the coverage is lopsided (some PEs own one
    // extra unit). We only adjust the AUTO-generated mapping (an explicit or
    // sentinel pe_mem_map is the user's choice -- left untouched). num_pes is the
    // design knob, so we snap IT (never the physically-derived org count): for
    // pes < orgs, down to the largest divisor of orgs <= pes; for pes >= orgs, to
    // the nearest multiple of orgs.
    if (config.pe_mem_map.empty() && config.num_pes > 0 && config.total_mem_orgs > 0) {
        int pes  = config.num_pes;
        int orgs = config.total_mem_orgs;
        bool clean = (pes <= orgs) ? (orgs % pes == 0) : (pes % orgs == 0);
        if (!clean) {
            int adj;
            const char* mode;
            if (pes < orgs) {
                adj = pes;
                while (adj > 1 && (orgs % adj) != 0) --adj;   // largest divisor <= pes
                mode = "1:N (one PE per N mem orgs)";
            } else {
                int lo = (pes / orgs) * orgs;                 // multiple of orgs <= pes
                int hi = lo + orgs;
                adj = (pes - lo <= hi - pes) ? lo : hi;        // nearest multiple
                if (adj < orgs) adj = orgs;
                mode = "M:1 (M PEs per mem org)";
            }
            if (adj >= 1 && adj != pes) {
                std::cerr << "WARNING: num_pes=" << pes << " does not divide evenly "
                          << "with " << orgs << " mem orgs at placement level "
                          << config.placement_level << "; adjusting num_pes -> " << adj
                          << " for a uniform " << mode << " distribution.\n";
                config.num_pes = adj;
            }
        }
    }

    // Contiguous block size (pages) per placement-level unit (point-1 addressing
    // fix): each device-local unit owns a contiguous address range sized by the
    // subarray capacity x subarrays-per-unit, so a PE's contiguous working set
    // lands in its own coverage (local) instead of being page-interleaved across
    // every unit (the flat-placement bug).
    {
        /* 1.11.56 (audit B010): size the subarray from the RESOLVED geometry
         * instead of a fixed 128 KB. memory.subarray_height is a real knob --
         * it is what sets config.subarrays_per_bank a few hundred lines up --
         * and pages_per_unit reaches both the emitted config (pagesPerUnit)
         * and the benchmark (--pages-per-unit), so it decides the
         * address-to-unit mapping and therefore PE locality and routed hop
         * distance. With the literal, asking for subarray_height: 1024
         * doubled the physical subarray and left the workload's contiguous
         * block at 128 KB.
         *
         * The subarray's capacity is the bank's, divided by the number of
         * subarrays the height resolved to. Bank size comes from the DRAM
         * architecture object where there is one; SRAM/NVM keep the 64 KB
         * per-bank unit the latency and energy paths already use. */
        const int PAGE_BYTES = 4096;
        uint64_t bank_bytes = 0;
        if (ladder_is_dram) {
            try {
                pimid::RamulatorWrapper geo("", tech);
                geo.setDeviceWidth(config.dram_device_width);
                geo.initialize();
                bank_bytes = geo.getBankSizeMB() * 1024ULL * 1024ULL;
            } catch (const std::exception&) { bank_bytes = 0; }
        }
        if (bank_bytes == 0) bank_bytes = 64ULL * 1024ULL;   // SRAM/NVM per-bank unit
        int sa_per_bank = std::max(1, config.subarrays_per_bank);
        uint64_t subarray_bytes = bank_bytes / static_cast<uint64_t>(sa_per_bank);
        int subarray_pages = static_cast<int>(subarray_bytes / PAGE_BYTES);
        if (subarray_pages < 1) subarray_pages = 1;   // subarray smaller than a page

        /* 1.11.57 (audit round 3, B010): count subarrays over the SAME
         * population that total_mem_orgs counts.
         *
         * total_subarrays is a one-rank, one-channel figure, and since
         * 1.11.56 (B060) total_mem_orgs at RANK, CHANNEL or LOGIC_DIE
         * placement counts ranks or channels across the whole device. Dividing
         * the first by the second then gave each unit 1/ranks (or 1/channels)
         * of the subarrays it actually owns, so pages_per_unit -- which is
         * emitted as pagesPerUnit and passed to the benchmark as
         * --pages-per-unit -- described a contiguous block covering a
         * fraction of the memory. Both sides are now whole-device counts. */
        int ranks_pc_pg = std::max(1, config.hierarchy_ranks_per_channel);
        int chans_pg    = std::max(1, config.hierarchy_dram_channels);
        long long total_subarrays = static_cast<long long>(config.subarrays_per_bank)
                              * banks_per_bg * bg_per_chip * chips_per_rank
                              * ranks_pc_pg * chans_pg;
        long long spu = (config.total_mem_orgs > 0)
                  ? (total_subarrays / config.total_mem_orgs) : 1;
        if (spu < 1) spu = 1;
        config.pages_per_unit = static_cast<int>(subarray_pages * spu);
    }

    // Validate ports_per_bank
    if (config.ports_per_bank < 1) config.ports_per_bank = 1;

    // Auto-derive pes_per_mc from available MI ports
    if (config.pe_mc_enabled) {
        int max_mis = config.total_mem_orgs * config.ports_per_bank;
        int min_pes_per_mi = (config.num_pes + max_mis - 1) / max_mis;  // ceil division
        if (min_pes_per_mi < 1) min_pes_per_mi = 1;

        if (config.pes_per_mc_user_set) {
            if (config.pes_per_mc < min_pes_per_mi) {
                std::cerr << "WARNING: pes_per_mc=" << config.pes_per_mc
                          << " requires " << (config.num_pes / config.pes_per_mc)
                          << " MIs but only " << max_mis << " ports available ("
                          << config.total_mem_orgs << " mem_orgs x "
                          << config.ports_per_bank << " ports/bank); clamping to "
                          << min_pes_per_mi << std::endl;
                config.pes_per_mc = min_pes_per_mi;
            }
        } else {
            config.pes_per_mc = min_pes_per_mi;
        }
    }

    // Resolve PE-to-mem-org mapping
    autoGeneratePEMemMap(config);
    validatePEMemMapping(config);

    // Compute total network endpoints based on connection mode
    // L2 instances get separate endpoints when clustered (count > 1)
    // L3 always gets its own endpoint when present
    int cache_endpoints = 0;
    if (config.enable_l2 && config.l2_count > 1) cache_endpoints += config.l2_count;
    if (config.enable_l3) cache_endpoints += 1;

    if (config.pe_mem_connection == UnifiedConfig::PEMemConnectionMode::SEPARATE_ENDPOINTS) {
        config.total_network_endpoints = config.num_pes + config.total_mem_orgs + cache_endpoints;
    } else {
        config.total_network_endpoints = std::max(config.num_pes, config.total_mem_orgs) + cache_endpoints;
    }

    // -- DRAM internal network = Garnet H-tree with JEDEC-physics link latency --
    //
    // Standard DRAM is a hierarchical (tree) distribution fabric, not a flat 2D
    // mesh. For DRAM technologies we default the device network to H_TREE and set
    // the per-hop LINK latency from physics: the time to drain one flit through
    // the technology's effective (aggregate, channel-parallel) datapath.
    //
    //   link_latency_cycles = ceil( flit_bytes / aggregate_BW_bytes_per_cycle )
    //
    // where aggregate_BW = per-channel BW x num_channels (HBM3 = 16 ch, HBM2 = 8,
    // DDR = 1), taken from Ramulator's JEDEC organization. High-bandwidth /
    // many-channel technologies drain a flit faster -> lower link latency -> a
    // faster tree, reproducing the JEDEC bandwidth hierarchy WITHOUT hand-tuned
    // per-tier tables. Computed at a fixed reference network clock so per-tech
    // core frequency does not invert the ordering. Only applied when the user
    // has not overridden the topology (default MESH_2D).
    {
        bool is_dram = (tech == "DDR3" || tech == "DDR4" || tech == "DDR5" ||
                        tech == "LPDDR5" || tech == "GDDR6" ||
                        tech == "HBM2" || tech == "HBM3");  // 1.11.57 (latent B044)
        // DRAM's internal datapath is physically a hierarchical (H-tree) fabric,
        // NOT a flat mesh -- so DRAM ALWAYS uses the H-tree, overriding any
        // mesh/ring/crossbar the config may request (those are only meaningful for
        // the host-side network and for non-DRAM device memories like SRAM/NVM).
        // A user H_TREE/CUSTOM choice is left as-is.
        if (is_dram) {
            // -- ALWAYS query the datasheet aggregate BW + channel count for any
            //    DRAM tech, regardless of whether the user already chose H_TREE.
            //    These feed the detailed NoC model's channel-BW bottleneck cap
            //    (accuracy fix). Previously this whole block was gated on
            //    "topology != H_TREE", so an explicit H_TREE config (as the
            //    validation cells use) left agg-BW=0 and the cap never engaged. --
            double agg_mbs = 0.0;
            double per_chan_mbs = 0.0;
            int num_chan = 1;
            try {
                pimid::RamulatorWrapper bw_q("", tech);
                bw_q.initialize();
                agg_mbs = static_cast<double>(bw_q.getBandwidth());  // aggregate (stack/device) BW
                // Real datasheet AGGREGATE sustainable BW (MB/s): the detailed
                // NoC model caps effective DRAM BW at this (channel bottleneck).
                config.hierarchy_agg_bandwidth_mbs = (long long)agg_mbs;
                /* 1.10.6: the shared channel DQ bus pays a penalty to reverse
                 * direction, and a bandwidth-limited link does not know that.
                 * tWTR (write-to-read) is the dominant JEDEC turnaround; the
                 * value is each technology's OWN, TRANSCRIBED from the
                 * Ramulator2 preset this run selects -- every line below
                 * carries its preset name and the nWTR_L x tCK arithmetic
                 * that produced it. 1.11.52 (audit B003) corrects the
                 * previous wording, which said the values were READ from
                 * the preset: no tWTR accessor exists on the wrapper or in
                 * DRAMTiming, so nothing reads them at run time and a
                 * preset change would not move them. Sourced constants
                 * with their derivation shown, not model output. Read-to-write is approximated by the same
                 * figure; stated approximation, conservative in direction.
                 * Off with memory.dq_turnaround: false -- a design with a
                 * dedicated PIM interconnect has no shared bus to turn. */
                if (config.dq_turnaround_enabled) {
                    double twtr_ns =
                        (tech=="DDR3")   ? 7.50 :   // DDR3_1600H  : 6ck  x 1.250
                        (tech=="DDR4")   ? 7.50 :   // DDR4_2400R  : 9ck  x 0.833
                        (tech=="DDR5")   ? 10.00 :  // DDR5_3200AN : 16ck x 0.625
                        (tech=="LPDDR5") ? 12.50 :  // LPDDR5_6400 : 10ck x 1.250
                        (tech=="GDDR6")  ? 6.27 :   // GDDR6_2000  : 11ck x 0.570
                        (tech=="HBM2")   ? 8.33 :   // HBM2_2.4    : 10ck x 0.833
                        (tech=="HBM3")   ? 8.11 :   // HBM3_6.4    : 26ck x 0.312
                        0.0;
                    /* Stored as ns x100; the interface converts at the SAME
                     * clock its service-time formula uses, so the two cannot
                     * be quoted in different cycle domains. */
                    config.hierarchy_dq_turn_ns_x100 =
                        (int)std::lround(twtr_ns * 100.0);
                }
                // Parallel DRAM channel count (HBM3=16, HBM2=8, DDR/LPDDR/GDDR=1).
                uint32_t nch = bw_q.getNumChannels();
                config.hierarchy_dram_channels = (nch >= 1) ? (int)nch : 1;
                num_chan = config.hierarchy_dram_channels;
                // Per-channel BW = aggregate / channels. This is the bandwidth of
                // the SHARED DQ datapath that bounds an individual access stream.
                per_chan_mbs = (num_chan > 0) ? (agg_mbs / (double)num_chan) : agg_mbs;
            }
            /* 1.11.57 (latent B021): REFUSE. This catch used to read "keep
             * defaults on failure", and the defaults are not defaults -- they
             * are three zeros that the rest of the run reads as measurements:
             *
             *   hierarchy_agg_bandwidth_mbs stays 0, and 0 is emitted as
             *     nocAggBandwidthMBs, the detailed NoC model's channel
             *     bandwidth cap -- the cap that stops a device claiming ten
             *     times its channel's rate. Zero disengages it.
             *   hierarchy_dq_turn_ns_x100 stays 0, which is the SAME value
             *     memory.dq_turnaround: false produces, so a tool failure
             *     becomes indistinguishable from a user switching the DQ
             *     turnaround penalty off.
             *   hierarchy_dram_channels stays 1, which for HBM2 is off by
             *     eight and for HBM3 by sixteen, and that count multiplies the
             *     placement slot ladder, the H-tree root fanout and the
             *     per-channel bandwidth every link width is scaled by.
             *
             * None of the three is recoverable from anywhere else: for a DRAM
             * technology Ramulator IS the datasheet. A run that continues from
             * here reports a memory system nobody described, so it does not
             * continue. Invisible only because Ramulator initialises in every
             * observed run. */
            catch (const std::exception& e) {
                refuseWithoutDramOracle(tech, e.what(),
                    "the aggregate channel bandwidth (the NoC model's "
                    "bandwidth cap), the DQ-bus turnaround (whose 'unknown' "
                    "and 'switched off' values are both 0) and the DRAM "
                    "channel count (which sizes the placement ladder and the "
                    "H-tree root)");
            } catch (...) {
                refuseWithoutDramOracle(tech,
                    "a non-standard exception escaped the wrapper",
                    "the aggregate channel bandwidth, the DQ-bus turnaround "
                    "and the DRAM channel count");
            }

            // -- ACCURACY FIX (suspect #1): channel-aware Garnet H-tree link
            //    latency. The detailed (cycle-accurate shared Garnet) model's
            //    reported cycles ARE the Garnet batch tick count, and that tick
            //    count is driven by the H-tree per-link latency (verified: link=1
            //    -> 80.7M cyc / avgLat 23 ; link=8 -> 240M cyc / avgLat 84, a
            //    clean monotone lever; the per-access return added via the M/D/c
            //    queue does NOT couple to the cycle count in async-detailed mode,
            //    so the link latency is the ONLY working bandwidth knob). Before
            //    this fix the H-tree always used linkLatency=1 for EVERY DRAM
            //    tech, so DDR4 and HBM2 produced IDENTICAL cycles (~78-80M) and
            //    detailed could not tell a slow channel from a fast one ("too
            //    optimistic" / undifferentiated). We now set the H-tree link
            //    latency inversely proportional to the PER-CHANNEL BW (the shared
            //    DQ bus that bounds one access stream), anchored so the highest-
            //    per-channel-BW tech rounds to link=1. Aggregate (multi-channel)
            //    bandwidth advantage is still expressed by channel concurrency in
            //    the batch replay; per-access latency reflects the per-channel DQ
            //    rate. Applied for ALL DRAM (independent of force_htree) so the
            //    explicit-H_TREE validation cells get the channel-aware latency.
            //    Only overrides when the user has NOT hand-tuned noc.link_latency
            //    (still at its default 1).
            //    This sets a DEDICATED Garnet-only link latency
            //    (noc_garnet_link_latency); the analytical noc_link_latency used
            //    by simple/calibrated/curve/approximate is left untouched so
            //    those models are unchanged. Only the detailed (and parallel)
            //    cycle-accurate Garnet H-tree picks it up.
            if (per_chan_mbs > 0.0) {
                // Reference per-channel BW = the fastest modeled DRAM channel
                // (HBM3 @ 819000/16 = 51200 MB/s). Anchors HBM3 -> link 1.
                const double REF_PERCHAN_MBS = referencePerChannelMBs();  // 1.11.56 (B012)
                double ll_d = REF_PERCHAN_MBS / per_chan_mbs;     // >=1, larger = slower channel
                int ll = (int)std::lround(ll_d);
                if (ll < 1) ll = 1;
                if (ll > 64) ll = 64;                              // safety bound
                config.noc_garnet_link_latency = ll;
            }

            // -- DETAILED DRAM: per-tech CUSTOM topology (per-link latency+bw +
            //    channel concurrency). For the cycle-accurate (detailed) Garnet
            //    on a real DRAM tech we emit a TREE topology
            //    file that encodes the tech's DRAM hierarchy with per-layer link
            //    latency and per-link width (bandwidth via occupancy), then point
            //    Garnet at it via CUSTOM. NOTE (1.9.35): this comment used to
            //    claim "N parallel channel subtrees". It does not build them.
            //    ranks_per_channel is 1 unless configured and the organisation
            //    size carries no channel factor, so every processing element
            //    decomposes to channel 0, rank 0 and BOTH the channel and rank
            //    routers have exactly one child -- pure pass-through latency
            //    hops. N is used only as the root fanout in the abstract-endpoint
            //    test and as the link-width concurrency multiplier. This
            //    SUPERSEDES the scalar noc_garnet_link_latency above for detailed
            //    DRAM (that value is kept as the linkLatency fallback / inherited
            //    default for any link that omits per-link fields, and still drives
            //    non-DRAM paths). simple/calibrated/approximate/curve are left
            //    unchanged.
            /* 1.10.1: remember what the user asked for before ANY override, so
             * the fabric-mismatch warning below can compare against the fabric
             * actually used. Two different overrides can fire here -- the
             * detailed-DRAM CUSTOM emitter and the H_TREE fallback -- so the
             * comparison has to happen after both, not inside either. */
            const std::string requested_topo = config.noc_topology;

            bool detailed_dram =
                config.noc_cycle_accurate &&
                (tech == "DDR3" || tech == "DDR4" || tech == "DDR5" ||
                 tech == "LPDDR5" || tech == "GDDR6" ||
                 tech == "HBM2" || tech == "HBM3");
            if (detailed_dram) {
                // Write <TECH>.topo into the current working directory (stable,
                // absolute) so the generated config + Garnet subprocess can reach
                // it. cwd is where the run is launched and where the generated
                // config lives in practice.
                // Filename carries the tech, PE count (topology widths depend on
                // it via min(pes,N) concurrency) and PID, so concurrent runs and
                // different-PE sweeps never clobber each other's topology file.
                std::string topo_name = tech + "_pe" + std::to_string(config.num_pes)
                                      + "_" + std::to_string((long)getpid()) + ".topo";
                std::string topo_path;
                char cwdbuf[PATH_MAX];
                if (getcwd(cwdbuf, sizeof(cwdbuf)) != nullptr) {
                    topo_path = std::string(cwdbuf) + "/" + topo_name;
                } else {
                    topo_path = topo_name;
                }
                if (dramHTreeBuilder(tech, topo_path, config)) {
                    config.noc_topology = "CUSTOM";
                    config.noc_topology_file = topo_path;
                    // The CUSTOM DRAM tree is routed by TreeRouter (up*/down*
                    // VC-class separation, external/garnet/src/TreeRouter.{hh,cc}),
                    // provably deadlock-free with as few as 2 VCs/vnet (1 UP +
                    // 1 DOWN) -- this replaces the old 32-VC stopgap. Floor at 2
                    // (the minimum the scheme needs); the config default of 4
                    // (2 UP + 2 DOWN) gives head-of-line headroom on the single-
                    // channel bottleneck.
                    /* 1.10.3: the DRAM fabric is described simply by default.
                     *
                     * A DRAM die's internal datapath is wires and repeaters
                     * driven by the command path -- there is no packet-switched
                     * router with a deep virtual-channel pool anywhere in it. We
                     * route it through Garnet because that is how PIMID prices
                     * contention, not because the memory contains that machine,
                     * so the default description should be the smallest one the
                     * routing scheme actually requires. Four VCs and four-deep
                     * buffers described a fabric a DRAM part does not have, and
                     * bought it head-of-line headroom the silicon has no reason
                     * to enjoy.
                     *
                     * TWO is the floor, not one. TreeRouter's deadlock-freedom
                     * argument rests on separating UP traffic from DOWN traffic
                     * into different VC classes; with a single VC the up and
                     * down phases of the same tree share buffers and the
                     * converging hub can close a cycle -- which is exactly the
                     * deadlock the old 32-VC stopgap was papering over. So the
                     * simple default is 1 UP + 1 DOWN, and buffers follow at 2.
                     *
                     * A user who names either value keeps it, including a
                     * deeper fabric for a logic-die design that genuinely has
                     * one. We only refuse to go below the deadlock floor. */
                    if (!config.noc_vcs_user_set)     config.noc_vcs_per_vnet = 2;
                    if (!config.noc_buffers_user_set) config.noc_buffers_per_vc = 2;
                    if (config.noc_vcs_per_vnet < 2) {
                        std::cerr << "[config] WARNING: noc.vcs_per_vnet="
                                  << config.noc_vcs_per_vnet << " is below the two "
                                     "the tree routing needs to keep UP and DOWN "
                                     "traffic in separate VC classes; raising to 2 "
                                     "to stay deadlock-free.\n";
                        config.noc_vcs_per_vnet = 2;
                    }

                    // -- NI FLIT-COUNT bandwidth model (replaces the old per-link
                    //    occupancy, which pinned upstream VCs for K cycles and
                    //    deadlocked the single-channel converging hub). Split each
                    //    DATA access into Fbw flits, Fbw ~ inverse per-channel
                    //    bandwidth (REF/per_chan): a lower-bandwidth tech gets MORE
                    //    flits/access, so its channel-DQ carries each access for
                    //    more cycles => lower per-channel bandwidth; the channel
                    //    COUNT (topology) yields the aggregate (N x 1/Fbw ~
                    //    agg_mbs/REF). Flits move one-per-cycle and never pin a VC,
                    //    so no deadlock. Detailed DRAM only -- simple/calibrated/
                    //    curve/approximate keep their own dataMsgBits untouched.
                    /* 1.11.60 (ONE FABRIC, user-approved; corrects round-4
                     * B003): the Fbw message inflation is REMOVED.
                     *
                     * The cross-technology bandwidth penalty is applied
                     * EXACTLY ONCE, in the port width: the topology file now
                     * carries each layer's RAW bits, so a narrow tier
                     * flit-ises a message into more flits by itself -- DDR4's
                     * 8-bit chip rung turns a 576-bit access into 72 flits
                     * where HBM3's 64-bit channel takes 9. Multiplying the
                     * message size by REF/per_chan ON TOP of that applied the
                     * same penalty twice, so the cross-technology flit ratio
                     * was the PRODUCT of two bandwidth ratios (round-4 B003:
                     * 14.4x where the model wants 2.67x). The NI flit-count
                     * transport itself (one flit per cycle, no VC pinning, no
                     * deadlock) is unchanged -- only the inflation is gone.
                     * dataMsgBits stays the user's value or the 576-bit
                     * cacheline default. */
                } else {
                    std::cerr << "WARNING: failed to emit DRAM CUSTOM topology "
                              << "for tech " << tech << " at " << topo_path
                              << "; falling back to scalar link latency.\n";
                }
            }

            // DRAM's internal datapath is physically a hierarchical (H-tree)
            // fabric, NOT a flat mesh -- so DRAM ALWAYS uses the H-tree, overriding
            // any mesh/ring/crossbar the config may request. A user H_TREE/CUSTOM
            // choice is left as-is. The physics-derived link latency below is only
            // applied when WE force the topology to H_TREE (i.e. the user did not
            // already pick H_TREE/CUSTOM), to preserve any user link tuning.
            bool force_htree = (config.noc_topology != "H_TREE" &&
                                config.noc_topology != "CUSTOM");
            if (force_htree && agg_mbs > 0.0) {
                config.noc_topology = "H_TREE";
                // PER-CHANNEL bytes/cycle at a fixed reference network clock.
                // A single access traverses ONE channel's DQ datapath, so the
                // per-hop cost follows the PER-CHANNEL bandwidth (= agg/c).
                // Aggregate (multi-channel) bandwidth is expressed by the
                // analytical model's bandwidth floor (P*D/c), not by making
                // each individual hop cheaper. Using aggregate BW here crushed
                // HBM2/HBM3 per-access network cost ~10-20x below the detailed
                // ground truth (single-channel techs were unaffected: agg ==
                // per-channel for c=1). Validated vs detailed at 4PE/size-1024:
                // GDDR6/HBM2 went from 0.55-0.59x of detailed to within ~20%.
                const double NET_GHz = 2.0;
                const double FLIT_BYTES = 64.0;
                double chan_mbs = (per_chan_mbs > 0.0) ? per_chan_mbs : agg_mbs;
                double bytes_per_cycle = (chan_mbs * 1e6) / (NET_GHz * 1e9);
                if (bytes_per_cycle > 0.0) {
                    // RES scales sub-cycle differences into integer link latencies so
                    // very-high-bandwidth techs (HBM2 vs HBM3) remain distinguishable,
                    // and makes the (bandwidth-derived) network cost dominate the
                    // per-access total so ordering follows effective bandwidth rather
                    // than cell-access latency.
                    const double RES = 10.0;
                    double lat_cycles = RES * FLIT_BYTES / bytes_per_cycle;  // continuous

                    // Hop-normalize: the bandwidth bottleneck cost must be independent
                    // of how DEEP the H-tree is (i.e. of bank count). A technology with
                    // few banks (shallow tree, e.g. LPDDR5) would otherwise traverse
                    // fewer hops and appear faster than its bandwidth warrants. We scale
                    // the per-hop link latency by REF_HOPS / actual_hops so that the
                    // total network cost (link_latency x hops) reflects effective
                    // bandwidth alone, not tree depth.
                    // Depth = the PER-CHANNEL subtree: an access fans down one
                    // channel's bank subtree (orgs/c leaves), so channel count
                    // widens the tree without lengthening the path.
                    const double REF_HOPS = 8.0;
                    int endpoints = (config.total_mem_orgs > 1) ? config.total_mem_orgs : 2;
                    int chan_eps = endpoints / std::max(1, num_chan);
                    if (chan_eps < 2) chan_eps = 2;
                    // Cap at 128: the detailed model's ground-truth replay
                    // tree is fixed at 128 endpoints for every technology, so
                    // the analytical depth normalization never assumes a tree
                    // deeper than that reference. Only DDR5 (256 per-channel
                    // endpoints) exceeds it -- DDR4 sits exactly at 128 and
                    // every other tech is below. Lifts DDR5 from 0.65x of
                    // detailed; all other techs provably unchanged.
                    if (chan_eps > 128) chan_eps = 128;
                    double actual_hops = std::ceil(std::log2((double)chan_eps));
                    if (actual_hops < 1.0) actual_hops = 1.0;
                    lat_cycles *= (REF_HOPS / actual_hops);

                    int ll = static_cast<int>(std::lround(lat_cycles));
                    if (ll < 1) ll = 1;
                    if (getenv("PIMID_LL_DIAG")) {
                        fprintf(stderr, "[LLDIAG] tech=%s agg_mbs=%.0f num_chan=%d "
                                "per_chan_mbs=%.0f endpoints=%d chan_eps=%d hops=%.0f ll=%d\n",
                                tech.c_str(), agg_mbs, num_chan, per_chan_mbs,
                                endpoints, chan_eps, actual_hops, ll);
                    }
                    config.noc_link_latency = ll;
                    config.noc_router_latency = 1;  // minimal router; link dominates

                    // Remove the old per-tier analytical model: each tier hop costs
                    // the SAME physics-derived link latency (the H-tree edge
                    // traversal), so the per-access remote cost scales with the
                    // technology's effective bandwidth, not hand-authored per-tier
                    // tables. Bridges add no extra latency.
                    /* 1.11.56 (audit B041): only when there is no tiered
                     * ladder to flatten.
                     *
                     * This branch is reached when force_htree is true, i.e.
                     * when the topology is still the default MESH_2D -- but
                     * the DETAILED path sets noc_topology = "CUSTOM" a few
                     * hundred lines above, so force_htree was really a proxy
                     * for "the analytical model is in use". The result: an
                     * analytical run of a device flattened all seven tiers to
                     * one number with zero bridges, while a detailed run of
                     * the SAME device kept the per-tier ladder. Comparing the
                     * two compared two hierarchy models, not two network
                     * models. The scalar noc_link_latency above still applies
                     * to the flat fabric either way. */
                    /* 1.11.60 (audit round 4, B016): the guard is DEAD and is
                     * deleted, and the record is corrected with it.
                     *
                     * The flattening the note above describes stood HERE,
                     * under `if (!config.hierarchy_enabled)`. Establishing
                     * which of the two -- guard or ledger -- was true: the only
                     * path in computeHierarchyLatencies() that leaves the flag
                     * false is the createInternalDRAMNetwork() failure, which
                     * RETURNS several hundred lines above this point; the flag
                     * is then set true unconditionally before the ladder work,
                     * and nothing between there and here clears it. So the
                     * condition could not hold and the body could not run.
                     *
                     * 1.11.59's notes record this as closed under B013 -- "the
                     * condition read as though the other case existed" -- and
                     * nothing in the file was edited, in a release whose own
                     * preamble confesses the same verification gap for B005,
                     * B014 and C018. The behaviour was and is right: a run that
                     * reaches this line always has a tiered ladder, and
                     * flattening all seven tiers to one number is exactly what
                     * 1.11.56's B041 fix existed to stop. Deleting the branch
                     * removes the claim that the other case exists without
                     * changing a single emitted value. The scalar
                     * noc_link_latency set above still applies to the flat
                     * fabric, which is the only thing this branch was reached
                     * for. */
                }
            }

            /* 1.10.1: fabric-mismatch warning.
             *
             * Overriding the fabric is correct -- a DRAM die's internal datapath
             * is a hierarchical tree, not a flat mesh, and the detailed path
             * goes further and emits a per-technology CUSTOM tree. What was
             * missing is telling the user. Someone who wrote noc.topology:
             * MESH_2D got a tree and no indication their key had been discarded.
             *
             * Compared here, after BOTH overrides (CUSTOM above, H_TREE just
             * now), because either can be the one that fires. Only when the key
             * was actually set -- the default is MESH_2D, and complaining about
             * a default nobody chose would be noise. */
            if (config.noc_topology_user_set &&
                config.noc_topology != requested_topo) {
                std::cerr << "[config] WARNING: noc.topology=" << requested_topo
                          << " was requested, but " << tech << " is a DRAM device "
                             "whose internal datapath is a hierarchical tree, not "
                             "a flat fabric. Using " << config.noc_topology
                          << " instead.\n"
                          << "  The requested fabric is not what this memory has, "
                             "so honouring it would price a network the device "
                             "does not contain. To model a genuine logic-die mesh, "
                             "place the elements at LOGIC_DIE; to supply your own "
                             "fabric, set noc.topology to CUSTOM with a topology "
                             "file.\n";
            }
        }
    }

    // Compute topology-aware NoC parameters for simple model
    int noc_nodes = config.total_network_endpoints > 0
                    ? config.total_network_endpoints : config.num_pes;
    bool ru = config.noc_ring_unidirectional;
    double avg_hops = avgHopsForTopology(config.noc_topology, noc_nodes, ru);
    int per_hop = config.noc_link_latency + config.noc_router_latency;

    // Flit/serialization: link_width = flit size, data_msg_bits = packet payload
    int link_width_bits = 128;  // matches Garnet flit size (ni_flit_size * 8)
    int data_msg_bits = config.noc_data_msg_bits > 0 ? config.noc_data_msg_bits : 576;
    int flits_per_packet = (data_msg_bits + link_width_bits - 1) / link_width_bits;

    config.noc_per_hop_cycles = per_hop;
    config.noc_flits_per_packet = flits_per_packet;
    config.noc_link_width_bits_cfg = link_width_bits;

    // One-way latency = NI overhead + internal hop traversal + wormhole serialization
    // NI overhead: 2xNI pipeline + 2xext links = 6 cycles (validated against Garnet)
    int ni_overhead = 6;
    config.noc_avg_one_way_latency = ni_overhead
                                   + static_cast<int>(std::ceil(avg_hops * per_hop))
                                   + (flits_per_packet > 1 ? flits_per_packet - 1 : 0);

    // Store topology-aware parameters for contention model
    config.noc_avg_hops_times_100 = static_cast<int>(avg_hops * 100.0 + 0.5);
    config.noc_num_nodes = noc_nodes;
    config.noc_bisection_links = bisectionLinksForTopology(config.noc_topology, noc_nodes, ru);
    config.noc_topology_class = topologyClassForTopology(config.noc_topology);
    config.noc_total_channels = totalChannelsForTopology(config.noc_topology, noc_nodes, ru);
    double hotspot = hotspotFactorForTopology(config.noc_topology);
    config.noc_hotspot_factor_100 = static_cast<int>(hotspot * 100.0 + 0.5);
}

/**
 * @brief Auto-generate PE-to-mem-org mapping when not explicitly provided.
 *
 * Handles three cases:
 *   1. Empty map -> 1:1 with wraparound if PEs > mem_orgs
 *   2. Uniform M:1 sentinel (pe_id == -M) -> M PEs per mem org
 *   3. Uniform 1:N sentinel (pe_id == -1, mem_org_ids.size() == N) -> N mem orgs per PE
 */
static void autoGeneratePEMemMap(UnifiedConfig& config) {
    int num_pes = config.num_pes;
    if (num_pes <= 0) return;  // nothing to map
    if (num_pes > 2048) {
        std::cerr << "WARNING: num_pes=" << num_pes
                  << " exceeds MAX_THREADS (2048). Clamping to 2048.\n";
        num_pes = 2048;
        config.num_pes = 2048;
    }
    int num_orgs = config.total_mem_orgs;
    if (num_orgs <= 0) num_orgs = num_pes;

    // Check for sentinel-based uniform mode from YAML parsing
    if (config.pe_mem_map.size() == 1 && config.pe_mem_map[0].pe_id < 0) {
        int sentinel_pe_id = config.pe_mem_map[0].pe_id;
        int sentinel_mo_count = static_cast<int>(config.pe_mem_map[0].mem_org_ids.size());

        if (sentinel_mo_count > 0 && config.pe_mem_map[0].mem_org_ids[0] == -1) {
            // 1:N mode -- one PE manages N mem orgs
            int mos_per_pe = sentinel_mo_count;
            config.pe_mem_map.clear();
            for (int pe = 0; pe < num_pes; pe++) {
                UnifiedConfig::PEMemMapping m;
                m.pe_id = pe;
                for (int j = 0; j < mos_per_pe; j++) {
                    int mo = pe * mos_per_pe + j;
                    if (mo < num_orgs)
                        m.mem_org_ids.push_back(mo);
                }
                if (m.mem_org_ids.empty())
                    m.mem_org_ids.push_back(pe % num_orgs);
                config.pe_mem_map.push_back(m);
            }
        } else {
            // M:1 mode -- M PEs share each mem org
            int pes_per_mo = -sentinel_pe_id;
            config.pe_mem_map.clear();
            for (int pe = 0; pe < num_pes; pe++) {
                UnifiedConfig::PEMemMapping m;
                m.pe_id = pe;
                int mo = pe / pes_per_mo;
                if (mo >= num_orgs) mo = num_orgs - 1;
                m.mem_org_ids.push_back(mo);
                config.pe_mem_map.push_back(m);
            }
        }
        return;
    }

    // If already explicitly populated (from YAML explicit mode), leave as-is
    if (!config.pe_mem_map.empty()) return;

    // Default: auto STRIDED/DISTANT placement. Spread the PEs evenly across
    // the device's memory-org units instead of clustering them at the front
    // (the old `pe % num_orgs` packed PEs 0,1,2,... onto orgs 0,1,2,...). Under
    // the distant-placement policy each PE taps its own evenly-spaced slice:
    //   pe i -> org round(i * num_orgs / num_pes)
    // When num_pes < num_orgs every PE lands on a distinct, maximally-separated
    // unit; when num_pes >= num_orgs they wrap evenly (contiguous PEs share the
    // nearest units in balanced groups). Only reached when pe_mem_map is empty
    // (fully auto); explicit/sentinel YAML maps are handled above.
    config.pe_mem_map.clear();
    for (int pe = 0; pe < num_pes; pe++) {
        UnifiedConfig::PEMemMapping m;
        m.pe_id = pe;
        // Store only the distant HOME org (slice start). The slice is contiguous
        // [home, home + orgs_per_pe) and is expanded to full coverage in init.cpp
        // (storing all orgs would overflow peMemMapData[4096] for fine placement).
        long mo = std::lround((double)pe * (double)num_orgs / (double)num_pes);
        if (mo < 0) mo = 0;
        if (mo >= num_orgs) mo = num_orgs - 1;
        m.mem_org_ids.push_back((int)mo);
        config.pe_mem_map.push_back(m);
    }

    // Loud one-line note whenever the auto map places >1 PE (i.e. it actually
    // strides). Lets the placement / PE-count sweeps confirm the spread.
    if (num_pes > 1) {
        std::cerr << "NOTE: auto PE->mem-org map is STRIDED/DISTANT (num_pes="
                  << num_pes << ", num_orgs=" << num_orgs << "): ";
        int shown = num_pes <= 32 ? num_pes : 32;
        for (int pe = 0; pe < shown; pe++) {
            std::cerr << pe << "->" << config.pe_mem_map[pe].mem_org_ids[0]
                      << (pe + 1 < shown ? " " : "");
        }
        if (shown < num_pes) std::cerr << " ...";
        std::cerr << "\n";
    }
}

/**
 * @brief Validate PE-to-mem-org mapping table.
 */
static void validatePEMemMapping(const UnifiedConfig& config) {
    int num_pes = config.num_pes;
    int num_orgs = config.total_mem_orgs;
    if (num_orgs <= 0) return;

    // Check all PEs are mapped
    std::vector<bool> pe_mapped(num_pes, false);
    for (const auto& m : config.pe_mem_map) {
        if (m.pe_id < 0 || m.pe_id >= num_pes) {
            std::cerr << "WARNING: PE mapping has invalid pe_id=" << m.pe_id << "\n";
            continue;
        }
        pe_mapped[m.pe_id] = true;
        for (int mo : m.mem_org_ids) {
            if (mo < 0 || mo >= num_orgs) {
                std::cerr << "WARNING: PE " << m.pe_id << " maps to invalid mem_org_id="
                          << mo << " (valid range 0-" << (num_orgs - 1) << ")\n";
            }
        }
    }
    for (int pe = 0; pe < num_pes; pe++) {
        if (!pe_mapped[pe]) {
            std::cerr << "WARNING: PE " << pe << " has no mem org mapping\n";
        }
    }
}

/**
 * @brief Emit the sys.hierarchy { ... } block into a ZSim config file.
 *
 * Pre-computed latencies are written as integers so the zsim_trace driver
 * can read them without any PIMID library dependency.
 */
// Emit just the PCIe/CXL/interposer offload-link keys (consumed by
// getPCIeLatency for the WORK_BEGIN/END M/D/1 transfer charge). These live
// under sys.hierarchy.pcie*, but the full hierarchy block is only emitted for
// device DRAM hierarchies -- so on the host<->device cosim path (often SRAM, no
// DRAM hierarchy) we emit a minimal standalone hierarchy block carrying only
// the pcie* keys. Caller must ensure no other hierarchy block is emitted.
static void emitZSimPcieBlock(std::ostream& out, const UnifiedConfig& config,
                              double sys_freq_mhz = 0.0) {
    if (!config.pcie_timing_configured) return;
    /* 1.11.56 (audit B061): these cycles are SPENT by the host cores, which
     * run at the clock emitted as sys.frequency -- the reference frequency
     * (max over nodes) in system scope. config.frequency_mhz is the top-level
     * system.frequency_mhz, unrelated to it: with the reference co-sim (host
     * 3000 MHz, top-level default 2000) a 500 ns PCIe base latency was
     * emitted as 1000 cycles and consumed as 333 ns, understating every
     * offload by 1.5x and overstating bytes/cycle by the same ratio. In
     * device scope the two are the same number, so this changes nothing
     * there. */
    double freq_mhz = (sys_freq_mhz > 0.0) ? sys_freq_mhz : config.frequency_mhz;
    uint32_t base_lat_cycles = static_cast<uint32_t>(
        config.pcie_base_latency_ns * freq_mhz / 1000.0 + 0.5);
    double bytes_per_cycle = (config.pcie_bandwidth_GBs * 1e9) / (freq_mhz * 1e6);
    out << "\n    hierarchy = {\n";
    out << "        pcieEnabled = 1;\n";
    out << "        pcieBaseLatencyCycles = " << base_lat_cycles << ";\n";
    out << "        pcieModel = 0;\n";
    out << "        pcieBytesPerCycle = \"" << std::fixed << std::setprecision(4)
        << bytes_per_cycle << "\";\n" << std::defaultfloat;
    out << "        pcieHeaderBytes = " << config.pcie_header_bytes << ";\n";
    uint32_t coh_cycles = static_cast<uint32_t>(
        config.pcie_coherence_extra_ns * freq_mhz / 1000.0 + 0.5);
    out << "        pcieCoherenceExtraCycles = " << coh_cycles << ";\n";
    out << "    };\n";
}

static void emitZSimHierarchyBlock(std::ostream& out, const UnifiedConfig& config,
                                   double device_bw_freq_mhz = 0.0,
                                   double sys_freq_mhz = 0.0) {
    if (!config.hierarchy_enabled) return;
    out << "\n    hierarchy = {\n";
    // Clock (MHz) used to convert device memory bandwidth into bytes/cycle for
    // the M/D/1 + bandwidth-floor contention. MUST be the DEVICE's own clock so
    // the device cycle count is invariant to the host/reference clock. Device
    // scope: config.frequency_mhz already IS the device. System-scope co-sim:
    // sys.frequency = max = HOST, so the caller passes the DEVICE node's
    // frequency here (device_bw_freq_mhz) to override it.
    {
        double bw_freq = (device_bw_freq_mhz > 0.0)
                         ? device_bw_freq_mhz : config.frequency_mhz;
        out << "        nocBandwidthFreqMHz = "
            << static_cast<int>(bw_freq) << ";\n";
    }
    out << "        placementLevel = " << config.pe_hierarchy_level << ";\n";
    out << "        subarraysPerBank = " << config.subarrays_per_bank << ";\n";
    out << "        banksPerBG = " << config.hierarchy_banks_per_bg << ";\n";
    out << "        bgPerChip = " << config.hierarchy_bg_per_chip << ";\n";
    out << "        chipsPerRank = " << config.hierarchy_chips_per_rank << ";\n";
    out << "        ranksPerChannel = " << config.hierarchy_ranks_per_channel << ";\n";
    // 1.11.57 (latent B031): a literal, because the field behind it never had
    // a writer. The plugin still receives the key it has always received.
    out << "        channelsPerSystem = 1;\n";
    out << "        dramChannels = " << config.hierarchy_dram_channels << ";\n";
    out << "        nocAggBandwidthMBs = " << config.hierarchy_agg_bandwidth_mbs << ";\n";
    out << "        dqTurnNsX100 = " << config.hierarchy_dq_turn_ns_x100 << ";\n";
    /* 1.11.11 (#113): the element's FP capability and the cost of not having
     * one now reach the TIMING model, which since 1.11.10 can see FP-class
     * instructions per block. */
    /* 1.11.57 (latent B040): these two keys are the FALLBACK, not the answer.
     *
     * The element's FP capability is emitted twice -- here as a hierarchy
     * global from config.pe_has_fp / pe_fp_emul_cycles, and again per group as
     * hasFpu / fpEmulCycles from the NODE's own fields. The plugin resolves it
     * as config.get<bool>(prefix + "hasFpu", zinfo->hierarchy.peHasFpu): the
     * group key wins and this one is only its default. The system-scope emitter
     * writes the group keys unconditionally for every group, so in that scope
     * these two lines are never consulted at all.
     *
     * The two sources could once disagree: the system-scope adoption block
     * copied the node's technology, element count, banks, placement, MC and
     * NoC model but not its FP capability, so the global stayed at the
     * top-level default while the groups carried the node's. 1.11.56 (B054/
     * B056) closed that by copying pe_has_fpu and pe_fp_emul_cycles with the
     * rest, which is what makes this pair a harmless fallback rather than a
     * second opinion. It stops being harmless if any group emission ever
     * becomes conditional -- the group key must be written for EVERY group. */
    out << "        fpEmulCycles = " << config.pe_fp_emul_cycles << ";\n";
    out << "        peHasFpu = " << (config.pe_has_fp ? "true" : "false") << ";\n";
    for (int i = 0; i < 7; ++i)
        out << "        levelLatency" << i << " = " << config.hierarchy_level_latency[i] << ";\n";
    for (int i = 0; i < 6; ++i)
        out << "        bridgeLatency" << i << " = " << config.hierarchy_bridge_latency[i] << ";\n";
    for (int i = 0; i < 6; ++i)
        out << "        bridgeModel" << i << " = \"" << config.hierarchy_bridge_model[i] << "\";\n";

    // M:N PE-to-memory-org mapping
    out << "        connectionMode = " << static_cast<int>(config.pe_mem_connection) << ";\n";
    out << "        localLinkLatency = " << config.local_link_latency << ";\n";
    out << "        mcStandalone = " << (config.mc_standalone ? 1 : 0) << ";\n";
    out << "        totalMemOrgs = " << config.total_mem_orgs << ";\n";
    out << "        pagesPerUnit = " << config.pages_per_unit << ";\n";
    /* 1.11.52 (audit D003): the DRAM ROW size, so the memory interface can
     * MEASURE the row-buffer hit rate the array-energy model used to assume
     * (ROW_MISS_FRAC = 0.5). JEDEC page size is a spec fact per generation;
     * 1 KB is the DDR3/DDR4 x8 page and 2 KB the wide-interface HBM/GDDR
     * page. Non-DRAM technologies have no row buffer -> 0, and the consumer
     * then claims no measurement. */
    {
        const std::string& mt = config.memory_tech;
        uint32_t row_bytes = 0;
        if (mt == "DDR3" || mt == "DDR4" || mt == "DDR5") row_bytes = 1024;
        else if (mt == "LPDDR5")                          row_bytes = 2048;
        else if (mt == "GDDR6" || mt == "HBM2" || mt == "HBM3") row_bytes = 2048;
        out << "        dramRowBytes = " << row_bytes << ";\n";
    }
    out << "        assumeLocal = 1;\n";  // perfect data prep: device computes local (both scopes)
    out << "        chargePrep = " << ((config.scope == "system") ? 1 : 0) << ";\n";  // co-sim: first-touch reorg+transfer
    // Host->device link transfer (perfect-prep): charged once per first-touch line
    // in co-sim when the device is EXTERNALLY attached (pcie/cxl/interposer);
    // on-package/internal = 0 (no external link to cross). bytes/cycle from the
    // link BW at the DEVICE clock; 64B line / bytes-per-cycle = per-line cost.
    {
        uint32_t hostLinkXferCycles = 0;
        if (config.scope == "system") {
            bool external = false;
            for (const auto& n : config.system_nodes)
                if (n.role == UnifiedConfig::SystemNode::DEVICE &&
                    n.attachment == UnifiedConfig::SystemNode::EXTERNAL) { external = true; break; }
            if (external && config.pcie_bandwidth_GBs > 0.0) {
                double bwf = (device_bw_freq_mhz > 0.0) ? device_bw_freq_mhz : config.frequency_mhz;
                double bpc = (config.pcie_bandwidth_GBs * 1e9) / (bwf * 1e6);
                if (bpc > 0.0) { hostLinkXferCycles = (uint32_t)(64.0 / bpc + 0.5);
                                 if (hostLinkXferCycles < 1) hostLinkXferCycles = 1; }
            }
        }
        out << "        hostLinkXferCycles = " << hostLinkXferCycles << ";\n";
    }
    out << "        totalNetworkEndpoints = " << config.total_network_endpoints << ";\n";
    out << "        nocAvgOneWayLatency = " << config.noc_avg_one_way_latency << ";\n";
    out << "        nocBisectionLinks = " << config.noc_bisection_links << ";\n";
    out << "        nocFlitsPerPacket = " << config.noc_flits_per_packet << ";\n";
    out << "        nocPerHopCycles = " << config.noc_per_hop_cycles << ";\n";
    out << "        nocLinkWidthBits = " << config.noc_link_width_bits_cfg << ";\n";
    out << "        nocVcsPerVnet = " << config.noc_vcs_per_vnet << ";\n";
    out << "        nocAvgHopsTimes100 = " << config.noc_avg_hops_times_100 << ";\n";
    out << "        nocNumNodes = " << config.noc_num_nodes << ";\n";
    out << "        nocTopologyClass = " << config.noc_topology_class << ";\n";
    out << "        nocTotalChannels = " << config.noc_total_channels << ";\n";
    out << "        nocHotspotFactor100 = " << config.noc_hotspot_factor_100 << ";\n";
    /* 1.11.57 (latent B035): literals, because the fields behind them could
     * never be anything else. The plugin's probe paths gate on these keys and
     * stay unreachable, as they always have been -- what changes is that the
     * config struct no longer advertises three switches with no positions. */
    out << "        nocInjectorCalib = 0;\n";
    out << "        nocCurveModel = 0;\n";
    out << "        nocCalQueue = 0;\n";
    out << "        nocMlpModel = " << config.noc_mlp_model << ";\n";
    /* M (MLP intensity) for the analytical model. Explicit noc.mlp wins;
     * otherwise ONE substituted number, 10, for every core model.
     *
     * 1.11.57 (latent B048): this header said "otherwise AUTO-derive from the
     * PE core model" and then listed three core types against the value each
     * would get. The code below reads no core type; all three entries in that
     * list were 10, so the table described a derivation that did not exist and
     * could not have produced a different answer if it had. What the 10 is:
     *   CALIBRATED for alu_core, vs detailed Garnet (P-sweep 2026-06-10,
     *     err <= 1.11x across P=8/16/32 x DDR3/HBM3);
     *   PLACEHOLDER for in_order/simple_core -- a cached core does not fit a
     *     single M (the NoC only sees misses; a miss-rate-aware load term is
     *     future work);
     *   PLACEHOLDER for ooo_core -- no cycle data under QEMU (known caveat).
     * All three are user-overridable with noc.mlp. The numeric consequence of
     * the one-size-fits-all default is tracked separately as B014. */
    {
        int mlpM = config.noc_mlp_degree;
        if (mlpM < 1) mlpM = 10;   // no explicit noc.mlp: the single default
        out << "        nocMlpDegree = " << mlpM << ";\n";
    }

    // Flattened mapping: offsets as space-separated, data as space-separated
    if (!config.pe_mem_map.empty()) {
        size_t map_size = std::min(config.pe_mem_map.size(), static_cast<size_t>(2048));
        // Count total data entries to validate against 4096 limit
        size_t total_data = 0;
        for (size_t i = 0; i < map_size; i++)
            total_data += config.pe_mem_map[i].mem_org_ids.size();
        if (total_data > 4096) {
            std::cerr << "WARNING: PE-mem mapping has " << total_data
                      << " total entries, exceeding 4096 limit. Truncating.\n";
        }

        out << "        peMemMapSize = " << map_size << ";\n";
        // Build prefix-sum offsets
        std::ostringstream offsets_ss, data_ss;
        int offset = 0;
        for (size_t i = 0; i < map_size; i++) {
            if (i > 0) offsets_ss << " ";
            offsets_ss << offset;
            for (size_t j = 0; j < config.pe_mem_map[i].mem_org_ids.size(); j++) {
                if (offset >= 4096) break;  // don't overflow peMemMapData[4096]
                if (offset > 0 || j > 0) data_ss << " ";
                data_ss << config.pe_mem_map[i].mem_org_ids[j];
                offset++;
            }
        }
        offsets_ss << " " << offset;  // sentinel
        out << "        peMemMapOffsets = \"" << offsets_ss.str() << "\";\n";
        out << "        peMemMapData = \"" << data_ss.str() << "\";\n";
    }

    // PCIe/CXL timing model -- only emit when user explicitly configured it
    // (pcie_timing_configured is set when power.pcie section exists in YAML)
    if (config.pcie_timing_configured) {
        // 1.11.56 (audit B061): the clock the host cores are emitted at.
        double freq_mhz = (sys_freq_mhz > 0.0) ? sys_freq_mhz : config.frequency_mhz;
        uint32_t base_lat_cycles = static_cast<uint32_t>(
            config.pcie_base_latency_ns * freq_mhz / 1000.0 + 0.5);
        double bytes_per_cycle = (config.pcie_bandwidth_GBs * 1e9) / (freq_mhz * 1e6);
        out << "        pcieEnabled = 1;\n";
        out << "        pcieBaseLatencyCycles = " << base_lat_cycles << ";\n";
        out << "        pcieModel = 0;\n";  // always SIMPLE (includes M/D/1 queuing)
        // Emit as string so ZSim config parser can read it as const char*
        out << "        pcieBytesPerCycle = \"" << std::fixed << std::setprecision(4)
            << bytes_per_cycle << "\";\n" << std::defaultfloat;

        /* Protocol overhead and coherence.
         *
         * 1.11.52 (audit B039): ONE source, whichever block emits it. This
         * path read the system-network link entry while the sibling
         * emitZSimPcieBlock reads config.pcie_header_bytes /
         * pcie_coherence_extra_ns -- and the caller picks between the two
         * blocks purely on whether a DRAM hierarchy exists. So the same
         * machine emitted different protocol overheads depending on an
         * unrelated toggle, and power.link.header_bytes was silently
         * inert whenever the hierarchy was on. The resolved config fields
         * are the single source (the link-sync already folds the declared
         * topology into them); the link entry is consulted only if those
         * were never resolved. */
        int header_bytes = (config.pcie_header_bytes > 0)
                           ? config.pcie_header_bytes : 20;
        double coherence_extra_ns = config.pcie_coherence_extra_ns;
        if (config.pcie_header_bytes <= 0 && !config.system_network.links.empty()) {
            header_bytes = config.system_network.links[0].header_bytes;
            coherence_extra_ns = config.system_network.links[0].coherence_extra_ns;
        }
        out << "        pcieHeaderBytes = " << header_bytes << ";\n";
        uint32_t coh_cycles = static_cast<uint32_t>(coherence_extra_ns * freq_mhz / 1000.0 + 0.5);
        out << "        pcieCoherenceExtraCycles = " << coh_cycles << ";\n";
    }

    // PE-MI distributed memory interface config
    if (config.pe_mc_enabled && config.placement_level != "HOST_MC") {
        int mc_count = peMemoryInterfaceCount(config);   // 1.11.60 (A012)

        // Compute total units at the placement level. This MUST equal the org
        // count the PE coverage/mapping uses (config.total_mem_orgs); otherwise
        // addrToUnit (% totalUnits) returns unit ids in a different range than the
        // coverage org ids -> PEs beyond #0 are structurally all-remote (a bug that
        // was masked while assumeLocal force-set every access local).
        int total_units = (config.total_mem_orgs > 0)
            ? config.total_mem_orgs
            : (config.placement_level == "SUBARRAY"
               ? config.num_banks * config.subarrays_per_bank : config.num_banks);

        /* 1.11.52 (audit B001): the PE-MI's local access latency is the
         * ARRAY's access time, and the array models already answer that.
         *
         * This was a per-technology table in CYCLES, which is not a property
         * of an array at all: the same silicon was charged 10 cycles at any
         * PE clock, i.e. 20 ns at 500 MHz and 2.5 ns at 4 GHz -- an 8x swing
         * in modelled array time across the frequency sweep, and it is the
         * base term of EVERY PE-MI access (pe_memory_interface.h). The tool
         * answer is the placement-tier latency in NANOSECONDS
         * (getMemoryLatencyCycles, which queries Ramulator/CACTI/NVSim at the
         * PE's own tier and node), converted at the PE clock here. The old
         * table survives only as the stated fallback when no tool answer is
         * available, and says so. */
        int local_latency = config.pe_mc_local_latency;
        if (local_latency < 0) {
            const double pe_clk_mhz = (device_bw_freq_mhz > 0.0)
                                      ? device_bw_freq_mhz : config.frequency_mhz;
            int tool_cycles = getMemoryLatencyCycles(
                config.memory_tech, pe_clk_mhz, config.tech_node_nm,
                config.use_yaml_memory_params,
                config.memory_params.read_latency_ns,
                config.pe_hierarchy_level, config.temperature_k,
                config.cache_line_size);                 // 1.11.56 (B018)
            if (tool_cycles > 0) {
                local_latency = tool_cycles;
                std::cout << "  [pe-mi] local access latency " << local_latency
                          << " cycles at " << pe_clk_mhz
                          << " MHz, from the array model at this placement "
                             "(scales with the PE clock, as an array time must)"
                          << std::endl;
            } else {
                std::string tech = config.memory_tech;
                if (tech == "SRAM")             local_latency = 3;
                else if (tech == "STT_MRAM")    local_latency = 20;
                else if (tech == "PCM")         local_latency = 50;
                else if (tech == "RERAM")       local_latency = 30;
                else if (tech == "HBM2" || tech == "HBM3") local_latency = 8;
                else                            local_latency = 10;  // DDR default
                std::cerr << "  [pe-mi] WARNING: no array-model latency for "
                          << tech << " at this placement; falling back to the "
                             "fixed per-technology cycle table (" << local_latency
                          << " cycles), which does NOT scale with the PE clock."
                          << std::endl;
            }
        }

        /* 1.11.52 (audit B002): the PE-MI's bandwidth is the memory's own
         * aggregate, which this run already derived from Ramulator into
         * config.hierarchy_agg_bandwidth_mbs. The table below contradicted
         * it (e.g. HBM3 51200 against the derived rate) while both were
         * emitted into ONE sys.hierarchy block, and this one sets the M/D/1
         * service rate. Prefer the derived value; keep the table as the
         * stated fallback. */
        int bw_mbs = config.pe_mc_bandwidth_mbs;
        if (bw_mbs < 0) {
            if (config.hierarchy_agg_bandwidth_mbs > 0) {
                bw_mbs = static_cast<int>(config.hierarchy_agg_bandwidth_mbs);
                std::cout << "  [pe-mi] bandwidth " << bw_mbs
                          << " MB/s, from the memory model's own aggregate"
                          << std::endl;
            } else {
                std::string tech = config.memory_tech;
                if (tech == "HBM2")             bw_mbs = 25600;
                else if (tech == "HBM3")        bw_mbs = 51200;
                else if (tech == "GDDR6")       bw_mbs = 19200;
                else if (tech == "SRAM")        bw_mbs = 51200;
                else                            bw_mbs = 12800;  // DDR default
                std::cerr << "  [pe-mi] WARNING: no derived aggregate bandwidth"
                             " for " << tech << "; falling back to the fixed "
                             "per-technology table (" << bw_mbs << " MB/s)."
                          << std::endl;
            }
        }

        out << "        totalUnits = " << total_units << ";\n";
        out << "        totalMCs = " << mc_count << ";\n";
        out << "        pesPerMC = " << config.pes_per_mc << ";\n";
        out << "        localLatency = " << local_latency << ";\n";
        out << "        defaultBandwidthMBs = " << bw_mbs << ";\n";

        // Emit per-group overrides
        for (const auto& grp : config.pe_mc_group_overrides) {
            for (int gid : grp.ids) {
                out << "        mcGroup" << gid << " = {\n";
                if (grp.bandwidth_mbs > 0)
                    out << "            bandwidthMBs = " << grp.bandwidth_mbs << ";\n";
                if (grp.local_latency > 0)
                    out << "            localLatency = " << grp.local_latency << ";\n";
                out << "        };\n";
            }
        }
    }

    out << "    };\n";

}

/**
 * @brief Synthesize system nodes from existing single-device config.
 *
 * For backward compatibility: when scope is "device" (or remapped from "cosim"),
 * create SystemNode entries from the existing flat UnifiedConfig fields so that
 * the system-level code path can treat all scopes uniformly.
 *
 * When cosim_remapped is true, also synthesizes the system_network from
 * the legacy host_* and pcie_* fields.
 */
static void synthesizeSystemNodes(UnifiedConfig& config) {
    if (config.scope == "system" && !config.cosim_remapped) return;  // already parsed from YAML

    config.system_nodes.clear();

    if (config.cosim_remapped) {
        // Host node (from legacy cosim host_* fields)
        UnifiedConfig::SystemNode host;
        host.name = "host";
        host.role = UnifiedConfig::SystemNode::HOST;
        host.core_type = config.host_core_type;
        host.num_cores = config.host_num_cores;
        host.frequency_mhz = config.host_frequency_mhz;
        // Resolve host process node: inherit the device node (config.tech_node_nm)
        // when no explicit host override was given (host_tech_node_nm < 0).
        host.tech_node_nm = (config.host_tech_node_nm >= 0)
            ? config.host_tech_node_nm : config.tech_node_nm;
        host.l1d_kb = config.host_l1d_kb;
        host.l1i_kb = config.host_l1i_kb;
        host.l2_kb = config.host_l2_kb;
        host.l3_kb = config.host_l3_kb;
        host.enable_l3 = (config.host_l3_kb > 0);
        host.memory_tech = config.host_memory_tech;
        config.system_nodes.push_back(host);

        // Synthesize system_network from pcie config
        config.system_network.topology = "crossbar";
        config.system_network.model = config.pcie_model;  // always "simple" (md1 mapped earlier)
        if (config.pcie_timing_configured) {
            UnifiedConfig::SystemLinkConfig link;
            link.src_name = "host";
            link.dst_name = "device";
            link.link_type = config.pcie_link_type;
            link.base_latency_ns = config.pcie_base_latency_ns;
            link.bandwidth_GBs = config.pcie_bandwidth_GBs;
            link.header_bytes = config.pcie_header_bytes;
            link.coherence_extra_ns = config.pcie_coherence_extra_ns;
            config.system_network.links.push_back(link);
        }
    }

    // Device node (always present for device and cosim-remapped scopes)
    UnifiedConfig::SystemNode dev;
    dev.name = "device";
    dev.role = UnifiedConfig::SystemNode::DEVICE;
    dev.device_type = UnifiedConfig::SystemNode::COMPUTE;
    dev.attachment = config.pcie_timing_configured ?
        UnifiedConfig::SystemNode::EXTERNAL : UnifiedConfig::SystemNode::INTERNAL;
    dev.core_type = config.pe_type;
    dev.num_cores = config.num_pes;
    dev.num_pes = config.num_pes;
    dev.pe_type = config.pe_type;
    dev.frequency_mhz = config.frequency_mhz;
    dev.tech_node_nm = config.tech_node_nm;
    dev.memory_tech = config.memory_tech;
    dev.placement_level = config.placement_level;
    dev.alu_compute_factor = config.alu_compute_factor;
    dev.alu_access_factor = config.alu_access_factor;
    dev.alu_throughput_factor = config.alu_throughput_factor;
    dev.alu_operand_width = config.alu_operand_width;
    dev.alu_energy_factor = config.alu_energy_factor;
    dev.l1d_kb = config.l1d_size_kb;
    dev.l1i_kb = config.l1i_size_kb;
    dev.l2_kb = config.enable_l2 ? config.l2_size_kb : 0;
    dev.l3_kb = config.enable_l3 ? config.l3_size_kb : 0;
    dev.noc_topology = config.noc_topology;
    config.system_nodes.push_back(dev);
}

/**
 * @brief Normalize system config: assign core indices, address ranges, freq scaling.
 *
 * Called after YAML parsing + synthesize. Sets up:
 * - reference_frequency_mhz = max(all node frequencies)
 * - Per-node core_start_idx / core_end_idx (global ZSim core array)
 * - Per-node freq_scale = reference_freq / node_freq
 * - Per-device addr_start / addr_end (4GB-aligned contiguous ranges)
 * - Per-node node_network_id (position in system network topology)
 */
static void normalizeSystemConfig(UnifiedConfig& config) {
    if (config.system_nodes.empty()) return;

    // Validate: hosts must have at least one accompanying memory device
    bool has_host = false, has_device = false;
    for (const auto& n : config.system_nodes) {
        if (n.role == UnifiedConfig::SystemNode::HOST) has_host = true;
        if (n.role == UnifiedConfig::SystemNode::DEVICE) has_device = true;
    }
    if (has_host && !has_device) {
        std::cerr << "ERROR: A host must be accompanied by at least one memory device.\n"
                  << "  Add a device entry under 'system.devices' in your YAML config.\n";
        exit(1);
    }

    // 1. Compute reference frequency (max of all)
    double max_freq = 0;
    for (const auto& n : config.system_nodes) {
        if (n.frequency_mhz > max_freq) max_freq = n.frequency_mhz;
    }
    config.reference_frequency_mhz = max_freq;

    // 2. Assign core indices and freq_scale
    int core_idx = 0;
    for (auto& n : config.system_nodes) {
        n.core_start_idx = core_idx;
        n.core_end_idx = core_idx + n.num_cores;
        core_idx += n.num_cores;
        n.freq_scale = max_freq / n.frequency_mhz;
    }

    // 3. Assign network node IDs (all nodes) and address ranges (devices only)
    // Hosts access device memory through the system network -- no host address range.
    uint64_t addr = 0;
    const uint64_t RANGE_SIZE = 0x100000000ULL;  // 4GB per device
    int net_id = 0;
    for (auto& n : config.system_nodes) {
        n.node_network_id = net_id++;
        if (n.role == UnifiedConfig::SystemNode::DEVICE) {
            n.addr_start = addr;
            n.addr_end = addr + RANGE_SIZE;
            addr += RANGE_SIZE;
        } else {
            // Host: no address range (accesses device memory through system network)
            n.addr_start = 0;
            n.addr_end = 0;
        }
    }

    // 4. Inherit top-level workload for nodes without explicit workload
    for (auto& n : config.system_nodes) {
        if (n.workload_binary.empty() && n.num_cores > 0) {
            n.workload_binary = config.workload_binary;
            if (n.workload_args.empty())
                n.workload_args = config.workload_args;
        }
    }
}

/**
 * @brief Resolve the memory-topology knob (1.7.4, HANDOFF MEMORY-TOPOLOGY +
 *        UNIVERSALITY ADDENDA). System scope only; runs after normalize.
 *
 * device.is_default_mem TRUE (default): the PIM device IS the host's main
 *   memory -- the host memory technology is forced = the device technology BY
 *   CONSTRUCTION (preserving the host-tech = device-tech matrix). A host.mem
 *   block is redundant here and is ignored with a warning.
 * device.is_default_mem FALSE: the device is accelerator-side memory only. The
 *   host MUST supply host.mem.technology (one of the 11 known techs); it drives
 *   the host memory pricing (emitHostMemBlock's host-path adder + BW keyed on
 *   host.mem.technology). Omission = config ERROR (rc=1, no silent fallback).
 *
 * The two-layer bridge (1.7.1) ALWAYS keys off the DEVICE tech regardless of
 * this knob -- the device is still attached over its own bridge. Only
 * host<->device boundary traffic crosses it; the host's own memory traffic is
 * priced at host.mem tech. Coherence mode (1.7.2) stays an INDEPENDENT knob
 * (no force-coupling to is_default_mem).
 *
 * @return 0 on success, 1 on a config error (missing/unknown host.mem).
 */
static int resolveMemoryTopology(UnifiedConfig& config) {
    if (config.scope != "system") return 0;

    /* 1.11.45 (audit E27, user ruling: multi-memory is v2 scope; the current
     * picture is ONE host memory + ONE device memory). The 1.9.42 FATAL
     * stopped every multi-memory topology; 1.11.9 rightly removed it for the
     * standard decoupled pair but left NOTHING guarding a second memory of
     * the same role -- the pricing loop latches host_done/dev_done, so a
     * third memory-bearing node ran to completion and was silently DROPPED
     * from the energy total. Underneath, measured counters exist as a single
     * host/device pair, so such a node has nothing to be priced from.
     * Refused at config time, naming the nodes, until v2's per-node counter
     * plumbing exists. */
    {
        const UnifiedConfig::SystemNode* first_mem[2] = {nullptr, nullptr};
        for (const auto& n : config.system_nodes) {
            if (n.memory_tech.empty()) continue;
            int r = (n.role == UnifiedConfig::SystemNode::HOST) ? 0 : 1;
            if (!first_mem[r]) { first_mem[r] = &n; continue; }
            std::cerr << "ERROR: two " << (r == 0 ? "HOST" : "DEVICE")
                      << " nodes carry memory ('" << first_mem[r]->name << "' "
                      << first_mem[r]->memory_tech << " and '" << n.name
                      << "' " << n.memory_tech << "). This build prices ONE "
                         "memory per role -- measured counters exist as a "
                         "single host/device pair -- and silently dropping '"
                      << n.name << "' from the energy total is not acceptable. "
                         "Multi-memory topologies are v2 scope.\n"
                      /* 1.11.60 (audit round 4, A013): say what this actually
                       * refuses, and what to do about it.
                       *
                       * The guard is correct and stays -- the shipped example
                       * examples/cosim/multi_device.yaml was the wrong one,
                       * and is corrected there. But the message was misleading
                       * in a way that mattered: memory_tech is a SystemNode
                       * field defaulting to "DDR4", overwritten only where a
                       * memory: block is parsed, so every DEVICE node carries
                       * a technology whether or not it declares one and this
                       * refuses the SECOND DEVICE NODE, not the second
                       * memory: block. Deleting a memory: block does not
                       * satisfy it -- the node is then named as DDR4, a
                       * technology the config never mentioned, which is what
                       * made the real predicate invisible. Nothing loads the
                       * shipped examples on the config-load path, so the one
                       * file this refuses was never seen to be refused. */
                      << "  Note this refuses the second DEVICE NODE, not the "
                         "second memory: block: an undeclared node.memory_tech "
                         "defaults to DDR4 rather than to nothing, so deleting "
                         "a memory: block does not satisfy it. Declare ONE "
                         "device node per system until v2's per-node memory "
                         "counters exist."
                      << std::endl;
            return 1;
        }
    }

    UnifiedConfig::SystemNode* host = nullptr;
    UnifiedConfig::SystemNode* dev  = nullptr;
    for (auto& n : config.system_nodes)
        if (n.role == UnifiedConfig::SystemNode::HOST) { host = &n; break; }
    // Prefer a compute device (the PIM device); fall back to any device.
    for (auto& n : config.system_nodes)
        if (n.role == UnifiedConfig::SystemNode::DEVICE &&
            n.device_type == UnifiedConfig::SystemNode::COMPUTE) { dev = &n; break; }
    if (!dev)
        for (auto& n : config.system_nodes)
            if (n.role == UnifiedConfig::SystemNode::DEVICE) { dev = &n; break; }

    if (!host) return 0;  // pure-device system scope: no host memory to resolve

    // Canonical whitelist -- the 11 role-independent techs (UNIVERSALITY
    // ADDENDUM: any may be host mem OR PIM device). Same set device scope uses.
    static const std::set<std::string> kValidTechs = {
        "DDR3", "DDR4", "DDR5", "LPDDR5", "GDDR6", "HBM2", "HBM3",
        "SRAM", "STT_MRAM", "PCM", "RERAM"
    };

    bool is_default = dev ? dev->is_default_mem : true;

    if (is_default) {
        // Default: device IS host main memory. host.mem is redundant here;
        // warn + ignore rather than error (the default path must stay lenient;
        // the dangerous direction -- FALSE without host.mem -- is hard-errored
        // below). The device tech drives host mem by construction.
        if (host->host_mem_present) {
            std::cerr << "Warning: host '" << host->name << "' supplies a host.mem "
                      << "block but the paired device is_default_mem=true; "
                      << "ignoring host.mem (the device technology drives host "
                      << "main memory by construction).\n";
        }
        /* 1.11.52 (audit B027): announce the OVERWRITE, not just the ignored
         * block. The warning above fires only when a host.mem block is
         * present; a host memory technology that arrived by any other route
         * was replaced here without a word. Under is_default_mem the device
         * IS the host's memory, so the overwrite is correct -- but a user who
         * set a different host technology must be told which one runs. */
        if (dev) {
            if (!host->memory_tech.empty() &&
                host->memory_tech != dev->memory_tech) {
                std::cerr << "Warning: host '" << host->name << "' memory technology "
                          << host->memory_tech << " is replaced by the device's "
                          << dev->memory_tech
                          << " (device.is_default_mem=true: the device IS the "
                             "host's main memory)." << std::endl;
            }
            host->memory_tech = dev->memory_tech;
        }
    } else {
        // Separate memory: the host must declare its own main memory tech.
        if (!host->host_mem_present || host->host_mem_tech.empty()) {
            std::cerr << "ERROR: device.is_default_mem=false requires a host.mem "
                      << "block with host.mem.technology on host '" << host->name
                      << "'.\n  The accelerator-side device is NOT the host's "
                      << "main memory; there is no silent DDR5 fallback.\n";
            return 1;
        }
        if (kValidTechs.find(host->host_mem_tech) == kValidTechs.end()) {
            std::cerr << "ERROR: unknown host.mem.technology '"
                      << host->host_mem_tech << "' on host '" << host->name << "'.\n"
                      << "Supported: DDR3, DDR4, DDR5, LPDDR5, GDDR6, HBM2, HBM3, "
                      << "SRAM, STT_MRAM, PCM, ReRAM.\n";
            return 1;
        }
        // host.mem.technology drives the host memory pricing; the device keeps
        // its own tech (and its own bridge). Genuinely decoupled techs.
        host->memory_tech = host->host_mem_tech;
        if (host->host_mem_bandwidth_mbs > 0) host->mem_bandwidth_mbs = host->host_mem_bandwidth_mbs;
        if (host->host_mem_channels > 0)      host->mem_channels      = host->host_mem_channels;
    }
    return 0;
}

/**
 * @brief Check if system nodes require multiple QEMU processes (different binaries).
 */
static bool needsMultiQemu(const UnifiedConfig& config) {
    std::set<std::string> unique_binaries;
    for (const auto& node : config.system_nodes) {
        if (node.num_cores > 0 && !node.workload_binary.empty())
            unique_binaries.insert(node.workload_binary);
    }
    return unique_binaries.size() > 1;
}

/**
 * @brief Group system nodes by workload binary for multi-QEMU orchestration.
 * Each group becomes one QEMU process with a contiguous core range.
 */
struct ProcessGroup {
    std::string binary;
    std::vector<std::string> args;
    int core_start = 0;
    int core_end = 0;    // exclusive
    std::vector<int> node_indices;
};

static std::vector<ProcessGroup> buildProcessGroups(const UnifiedConfig& config) {
    std::vector<ProcessGroup> groups;
    std::map<std::string, size_t> binary_to_group;

    for (size_t i = 0; i < config.system_nodes.size(); i++) {
        const auto& node = config.system_nodes[i];
        if (node.num_cores == 0 || node.workload_binary.empty()) continue;

        auto it = binary_to_group.find(node.workload_binary);
        if (it == binary_to_group.end()) {
            ProcessGroup pg;
            pg.binary = node.workload_binary;
            pg.args = node.workload_args;
            pg.core_start = node.core_start_idx;
            pg.core_end = node.core_end_idx;
            pg.node_indices.push_back((int)i);
            binary_to_group[node.workload_binary] = groups.size();
            groups.push_back(std::move(pg));
        } else {
            auto& pg = groups[it->second];
            pg.core_start = std::min(pg.core_start, node.core_start_idx);
            pg.core_end = std::max(pg.core_end, node.core_end_idx);
            pg.node_indices.push_back((int)i);
        }
    }
    return groups;
}

/**
 * @brief Get average hop count for a topology with N nodes.
 * Same formulas used by InternalDRAMNetwork for tier networks.
 */
static double avgHopsForTopology(const std::string& topology, int num_nodes,
                                 bool ring_unidir) {
    if (num_nodes <= 1) return 0.0;
    int N = num_nodes;
    std::string topo = topology;
    // Normalize to uppercase
    for (auto& c : topo) c = std::toupper(c);

    // BUS/CROSSBAR: single central router, 0 internal hops.
    // NI->Router->NI uses external links only (accounted for separately).
    if (topo == "CROSSBAR") return 0.0;
    if (topo == "BUS") return 0.0;

    if (topo == "RING") {
        if (ring_unidir) {
            // Unidirectional ring: avg hops = (N-1)/2
            return (N - 1) / 2.0;
        }
        // Bidirectional ring: exact combinatorial avg hops
        if (N == 2) return 1.0;
        if (N % 2 == 0)
            return (double)(N * N) / (4.0 * (N - 1));
        else
            return (N + 1) / 4.0;
    }

    if (topo == "MESH_2D" || topo == "MESH") {
        // kxk mesh, XY routing: avg hops per dimension = (k^2-1)/(3k), 2 dimensions
        double k = std::ceil(std::sqrt((double)N));
        return 2.0 * (k * k - 1.0) / (3.0 * k);
    }

    if (topo == "TORUS_2D" || topo == "TORUS") {
        // kxk torus: avg hops per dimension = k/4 (even k), 2 dimensions
        double k = std::ceil(std::sqrt((double)N));
        if ((int)k % 2 == 0)
            return 2.0 * (k / 4.0);
        else
            return 2.0 * (k * k - 1.0) / (4.0 * k);
    }

    if (topo == "FAT_TREE" || topo == "H_TREE") {
        // Binary tree (arity=2): exact LCA-based average internal hops
        int a = 2;
        int L = (int)std::ceil(std::log((double)N) / std::log((double)a));
        double totalHops = 0.0;
        int totalPairs = 0;
        int subtreeSize = a;
        for (int d = L; d >= 1; d--) {
            int prevSubtreeSize = (d == L) ? 1 : subtreeSize / a;
            int newPeers = subtreeSize - prevSubtreeSize;
            int hopCount = 2 * (L - d);
            totalHops += (double)newPeers * hopCount;
            totalPairs += newPeers;
            subtreeSize *= a;
        }
        return (totalPairs > 0) ? totalHops / totalPairs : 0.0;
    }

    if (topo == "STAR") return 2.0;  // always through center
    // Default: crossbar-like
    return 0.0;
}

/**
 * @brief Get bisection bandwidth (in links) for a topology with N nodes.
 * Bisection = min links cut to split network into two equal halves.
 */
static int bisectionLinksForTopology(const std::string& topology, int num_nodes,
                                     bool ring_unidir) {
    if (num_nodes <= 1) return 1;
    std::string topo = topology;
    for (auto& c : topo) c = std::toupper(c);

    if (topo == "CROSSBAR") return num_nodes / 2;
    if (topo == "FAT_TREE") return num_nodes / 2;  // full bisection BW
    /* 1.11.52 (audit B043): ONE grid derivation. avgHops uses
     * ceil(sqrt(N)) -- the grid that actually holds N nodes -- while these
     * two used a TRUNCATING sqrt, so for any non-square N the bisection
     * width and the hop count described different meshes (N=12: hops on a
     * 4x4, bisection on a 3x3). The mesh a router count is built for is the
     * one that fits the nodes. */
    const int grid_side = (int)std::ceil(std::sqrt((double)num_nodes));
    if (topo == "TORUS_2D" || topo == "TORUS") {
        return 2 * grid_side;  // cut wraps around both dimensions
    }
    if (topo == "MESH_2D" || topo == "MESH") {
        return grid_side;      // cut along one dimension
    }
    if (topo == "RING") return ring_unidir ? 1 : 2;  // uni: 1 CW link; bi: 2 links
    if (topo == "BUS") return 1;      // single shared medium
    if (topo == "H_TREE") return 1;   // root link
    if (topo == "STAR") return 1;     // center node
    return num_nodes / 2;  // default: crossbar-like
}

/**
 * @brief Total unidirectional channels in the network.
 * Each channel carries 1 flit/cycle; this determines per-channel load.
 */
static int totalChannelsForTopology(const std::string& topology, int num_nodes,
                                    bool ring_unidir) {
    if (num_nodes <= 1) return 1;
    std::string topo = topology;
    for (auto& c : topo) c = std::toupper(c);

    int N = num_nodes;
    int k = (int)std::ceil(std::sqrt((double)N));   // 1.11.52 (B043): same grid as avgHops

    if (topo == "BUS") return 1;           // shared bus: 1 flit/cycle total
    if (topo == "CROSSBAR") return N;      // N output ports
    if (topo == "RING") return ring_unidir ? N : 2 * N;  // uni: N CW; bi: N CW + N CCW
    if (topo == "MESH_2D" || topo == "MESH")
        return 4 * k * (k - 1);           // 2 dir x 2 dim x k rows x (k-1) links
    if (topo == "TORUS_2D" || topo == "TORUS")
        return 4 * k * k;                 // 2 dir x 2 dim x k rows x k links
    if (topo == "FAT_TREE" || topo == "H_TREE") {
        int levels = (int)std::ceil(std::log2((double)N));
        int total_routers = (1 << levels) - 1;
        return 2 * std::max(1, total_routers - 1);
    }
    return N;
}

/**
 * @brief Hotspot factor: ratio of max-loaded channel to average.
 * Accounts for non-uniform link loading per topology.
 */
static double hotspotFactorForTopology(const std::string& topology) {
    std::string topo = topology;
    for (auto& c : topo) c = std::toupper(c);

    if (topo == "BUS" || topo == "CROSSBAR") return 1.0;
    if (topo == "RING") return 1.0;
    if (topo == "MESH_2D" || topo == "MESH") return 2.0;
    if (topo == "TORUS_2D" || topo == "TORUS") return 3.5;
    if (topo == "FAT_TREE" || topo == "H_TREE") return 1.5;
    return 1.0;
}

/**
 * @brief Topology contention class: 0=BUS, 1=CROSSBAR, 2=MULTI_HOP.
 */
static int topologyClassForTopology(const std::string& topology) {
    std::string topo = topology;
    for (auto& c : topo) c = std::toupper(c);
    if (topo == "BUS") return 0;
    if (topo == "CROSSBAR") return 1;
    return 2;  // RING, MESH, TORUS, FAT_TREE, H_TREE
}

/**
 * @brief Compute system network latency between two nodes (in cycles at system net freq).
 *
 * Uses the same avgHops-based formula as intra-device tier networks.
 * Returns latency in reference frequency cycles.
 */
static uint32_t computeSystemNetLatency(const UnifiedConfig& config,
                                          int src_node_id, int dst_node_id) {
    if (src_node_id == dst_node_id) return 0;
    const auto& net = config.system_network;
    int num_nodes = (int)config.system_nodes.size();
    double hops = avgHopsForTopology(net.topology, num_nodes);
    double base_cycles = hops * (net.router_latency + net.latency_cycles);
    // Serialization: 64B cache line
    double data_bits = 64.0 * 8.0;
    double serialization = std::ceil(data_bits / net.link_width_bits);
    double net_lat_sys_cycles = base_cycles + serialization;

    // Convert from system network frequency to reference frequency
    double ref_freq_ghz = config.reference_frequency_mhz / 1000.0;
    double ratio = ref_freq_ghz / net.frequency_ghz;
    return static_cast<uint32_t>(std::ceil(net_lat_sys_cycles * ratio));
}

/**
 * @brief Write Ramulator2 YAML config content for a given DRAM technology.
 *
 * Reusable helper: writes the YAML to any ostream.  Called by both the
 * single-device autoGenerateRamulatorConfig() and the multi-device
 * generateSystemConfig() per-device path.
 */
// device_width: "" keeps each technology's JEDEC default org preset; "x4"/"x8"/
// "x16" selects the matching Ramulator2 preset so the timing model agrees with
// PIMID's chips/rank + BG/chip derivation (see the device_width block above).
// Width legality per tech is validated earlier; this only swaps the suffix.
static void writeRamulatorConfigYaml(std::ostream& ofs, const std::string& tech,
                                     const std::string& device_width = "") {
    ofs << "Frontend:\n  impl: GEM5\n\n";

    bool useClosedRow = false;

    if (tech == "DDR3") {
        std::string w = device_width.empty() ? "x8" : device_width;
        ofs << "MemorySystem:\n  impl: GenericDRAM\n  clock_ratio: 1\n  DRAM:\n    impl: DDR3\n"
            << "    org:\n      preset: DDR3_8Gb_" << w << "\n    timing:\n      preset: DDR3_1600H\n";
    } else if (tech == "DDR5") {
        std::string w = device_width.empty() ? "x8" : device_width;
        ofs << "MemorySystem:\n  impl: GenericDRAM\n  clock_ratio: 1\n  DRAM:\n    impl: DDR5\n"
            << "    org:\n      preset: DDR5_8Gb_" << w << "\n    timing:\n      preset: DDR5_3200AN\n"
            << "    RFM:\n      BRC: 2\n";
        useClosedRow = true;
    } else if (tech == "LPDDR5") {
        // LPDDR5 only has an x16 organization in Ramulator2.
        ofs << "MemorySystem:\n  impl: GenericDRAM\n  clock_ratio: 1\n  DRAM:\n    impl: LPDDR5\n"
            << "    org:\n      preset: LPDDR5_8Gb_x16\n    timing:\n      preset: LPDDR5_6400\n";
    } else if (tech == "GDDR6") {
        std::string w = device_width.empty() ? "x16" : device_width;
        ofs << "MemorySystem:\n  impl: GenericDRAM\n  clock_ratio: 1\n  DRAM:\n    impl: GDDR6\n"
            << "    org:\n      preset: GDDR6_8Gb_" << w << "\n    timing:\n      preset: GDDR6_2000_1350mV_double\n";
    } else if (tech == "HBM2") {
        ofs << "MemorySystem:\n  impl: GenericDRAM\n  clock_ratio: 1\n  DRAM:\n    impl: HBM2\n"
            << "    org:\n      preset: HBM2_4Gb\n    timing:\n      preset: HBM2_2.4Gbps\n";
    } else if (tech == "HBM3") {
        ofs << "MemorySystem:\n  impl: GenericDRAM\n  clock_ratio: 1\n  DRAM:\n    impl: HBM3\n"
            << "    org:\n      preset: HBM3_4Gb\n    timing:\n      preset: HBM3_6.4Gbps\n";
    } else {
        // Default: DDR4
        std::string w = device_width.empty() ? "x8" : device_width;
        ofs << "MemorySystem:\n  impl: GenericDRAM\n  clock_ratio: 1\n  DRAM:\n    impl: DDR4\n"
            << "    org:\n      preset: DDR4_8Gb_" << w << "\n    timing:\n      preset: DDR4_2400R\n";
        useClosedRow = true;
    }

    ofs << "  Controller:\n    impl: Generic\n"
        << "    Scheduler:\n      impl: FRFCFS\n"
        << "    RefreshManager:\n      impl: AllBank\n";
    if (useClosedRow) {
        ofs << "    RowPolicy:\n      impl: ClosedRowPolicy\n      cap: 4\n";
    } else {
        ofs << "    RowPolicy:\n      impl: OpenRowPolicy\n";
    }
    ofs << "  AddrMapper:\n    impl: RoBaRaCoCh\n";
}


/**
 * @brief Auto-generate a Ramulator2 YAML config file based on memory technology.
 */
static void autoGenerateRamulatorConfig(UnifiedConfig& config, const std::string& tech) {
    std::string tmpCfg = "/tmp/pimid_ramulator_" + std::to_string(getpid()) + ".yaml";
    std::ofstream ofs(tmpCfg);
    writeRamulatorConfigYaml(ofs, tech, config.dram_device_width);
    ofs.close();
    config.ramulator_config_file = tmpCfg;
}

/**
 * @brief Emit the appropriate ZSim mem = {...} block based on controller type.
 *
 * @param out Output stream for the config file
 * @param config UnifiedConfig with controller parameters
 * @param mem_latency Latency in cycles (used for MD1/Simple/Ramulator only)
 */
static void emitZSimMemBlock(std::ostream& out, const UnifiedConfig& config, int mem_latency) {
    std::string ct = config.zsim_mem_controller_type;
    int bound_lat = (config.weave_bound_latency >= 0) ? config.weave_bound_latency : mem_latency;

    if (ct == "ramulator") {
        out << "    mem = {\n";
        out << "        type = \"Ramulator\";\n";
        out << "        configFile = \"" << config.ramulator_config_file << "\";\n";
        out << "        latency = " << mem_latency << ";\n";
        out << "    };\n";
    } else if (ct == "weavesimple") {
        out << "    mem = {\n";
        out << "        type = \"WeaveSimple\";\n";
        out << "        latency = " << mem_latency << ";\n";
        out << "        bandwidth = " << config.md1_bandwidth_mbs << ";\n";
        out << "        boundLatency = " << bound_lat << ";\n";
        out << "    };\n";
    } else {
        // Simple (M/D/1 always active)
        out << "    mem = {\n";
        out << "        type = \"Simple\";\n";
        out << "        latency = " << mem_latency << ";\n";
        out << "        bandwidth = " << config.md1_bandwidth_mbs << ";\n";
        out << "    };\n";
    }
}

/**
 * @brief Emit the ZSim Garnet network configuration block.
 *
 * Handles all topologies (not just MESH_2D). Computes mesh dimensions for
 * grid topologies, derives effective routing from topology if not overridden.
 */
static void emitZSimNetworkBlock(std::ostream& out, const UnifiedConfig& config) {
    // Compute mesh dimensions from total network endpoints (not just num_pes)
    int num_nodes = config.total_network_endpoints > 0
                    ? config.total_network_endpoints : config.num_pes;
    // The emitted network is shared by every endpoint that registers into it:
    // device PEs/mem-orgs AND each system node's per-core caches (host_l1d-N,
    // N up to num_cores-1). Size it to cover the largest registering set so a
    // 16-core host does not overflow a 4-node grid (previously high-id caches
    // silently wrapped onto wrong nodes via modulo).
    for (const auto& n : config.system_nodes)
        if (n.num_cores > num_nodes) num_nodes = n.num_cores;
    if (config.num_pes > num_nodes) num_nodes = config.num_pes;
    int mesh_size = static_cast<int>(std::sqrt(num_nodes));
    if (mesh_size * mesh_size < num_nodes) mesh_size++;

    // Derive effective routing: if user didn't specify, use topology default
    std::string effective_routing = config.noc_routing;
    if (effective_routing.empty()) {
        // Map topology to default routing string
        if (config.noc_topology == "MESH_2D")       effective_routing = "XY";
        else if (config.noc_topology == "TORUS_2D")  effective_routing = "DOR";
        else if (config.noc_topology == "RING")      effective_routing = "SHORTEST";
        else if (config.noc_topology == "CROSSBAR")  effective_routing = "DIRECT";
        else if (config.noc_topology == "FAT_TREE")  effective_routing = "NCA";
        else if (config.noc_topology == "BUS")       effective_routing = "DIRECT";
        else if (config.noc_topology == "H_TREE")    effective_routing = "NCA";
        else if (config.noc_topology == "CUSTOM")    effective_routing = "TABLE";
        else                                         effective_routing = "XY";
    }

    // Node-grid sizing. MESH/TORUS need a 2D grid, so pad to a square
    // (ceil(sqrt(endpoints))^2); spare nodes carry no traffic and registration is
    // range-checked. Non-grid topologies (H_TREE, RING, BUS, FAT_TREE, CROSSBAR)
    // are sized to the EXACT endpoint count -- otherwise an over-padded H-tree
    // builds too many leaves and inflates hop counts.
    int noc_rows = mesh_size, noc_cols = mesh_size;
    if (config.noc_topology != "MESH_2D" && config.noc_topology != "TORUS_2D") {
        noc_rows = 1;
        noc_cols = num_nodes;
    }

    out << "    networkType = \"garnet\";\n";
    out << "    network = {\n";
    out << "        topology = \"" << config.noc_topology << "\";\n";
    out << "        rows = " << noc_rows << ";\n";
    out << "        cols = " << noc_cols << ";\n";
    out << "        routerLatency = " << config.noc_router_latency << ";\n";
    // Channel-aware Garnet link latency for DRAM detailed/parallel (accuracy
    // fix): when set (>0) the cycle-accurate Garnet H-tree uses the per-channel-
    // BW-derived link latency so DRAM techs are differentiated by bandwidth
    // (DDR4 slower channel -> higher link latency than HBM2/HBM3). The
    // analytical models do not read this network block, so simple/calibrated/
    // curve/approximate stay on noc_link_latency, unchanged.
    {
        // Apply the channel-aware override ONLY for the cycle-accurate Garnet
        // (detailed). The non-cycle-accurate models build a PROBE network from
        // this same block (calibrated/curve), so we must NOT alter their
        // linkLatency or their probed L0 would shift -> keep them on
        // noc_link_latency for cycleAccurate=false.
        bool detailed_only = config.noc_cycle_accurate;
        int garnet_link = (detailed_only && config.noc_garnet_link_latency > 0)
                          ? config.noc_garnet_link_latency : config.noc_link_latency;
        out << "        linkLatency = " << garnet_link << ";\n";
    }
    out << "        cycleAccurate = " << (config.noc_cycle_accurate ? "true" : "false") << ";\n";
    out << "        routing = \"" << effective_routing << "\";\n";
    out << "        vcsPerVnet = " << config.noc_vcs_per_vnet << ";\n";
    out << "        buffersPerVc = " << config.noc_buffers_per_vc << ";\n";
    if (!config.noc_topology_file.empty()) {
        out << "        topologyFile = \"" << config.noc_topology_file << "\";\n";
    }
    if (!config.noc_routing_table_file.empty()) {
        out << "        routingTableFile = \"" << config.noc_routing_table_file << "\";\n";
    }
    if (config.noc_control_msg_bits > 0) {
        out << "        controlMsgBits = " << config.noc_control_msg_bits << ";\n";
    }
    if (config.noc_data_msg_bits > 0) {
        out << "        dataMsgBits = " << config.noc_data_msg_bits << ";\n";
    }
    if (config.noc_ring_unidirectional) {
        out << "        ringUnidirectional = true;\n";
    }
    out << "    };\n";
}

//=============================================================================
// Simulation Method Implementations
//=============================================================================

/**
 * Parsed Garnet network statistics for McPAT power modeling
 * Matches the stats output by ZSim's GarnetNetwork::writeStatsFile()
 */
struct GarnetParsedStats {
    uint64_t total_packets = 0;
    uint64_t total_flits = 0;
    uint64_t total_hops = 0;
    uint64_t buffer_reads = 0;
    uint64_t buffer_writes = 0;
    uint64_t crossbar_traversals = 0;
    uint64_t arbiter_events = 0;
    uint64_t link_traversals = 0;
    uint64_t total_cycles = 0;
    uint64_t total_latency = 0;
    uint32_t num_routers = 0;
    uint32_t num_rows = 0;
    uint32_t num_cols = 0;
    uint32_t flit_size_bits = 128;
    double clock_mhz = 1000.0;
};

/**
 * Parse Garnet stats file (key=value format) into GarnetParsedStats.
 * Returns default (zero) stats if the file doesn't exist.
 */
static GarnetParsedStats parseGarnetStatsFile(const std::string& path) {
    GarnetParsedStats stats;
    std::ifstream f(path);
    if (!f.is_open()) return stats;

    std::string line;
    while (std::getline(f, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // Trim whitespace
        while (!key.empty() && std::isspace(key.back())) key.pop_back();
        while (!val.empty() && std::isspace(val.front())) val.erase(val.begin());
        /* 1.9.22: the writer emits DOTTED keys ("garnet.total_packets = N"),
         * while the comparisons below use bare names. Without stripping the
         * prefix every field silently kept its zero default: the file parsed
         * without error and yielded nothing, so co-sim NoC power fell back to
         * the placeholder duty cycle even though a full measurement was on
         * disk. Strip a leading "garnet." (and tolerate any other prefix). */
        {
            auto dot = key.rfind('.');
            if (dot != std::string::npos) key = key.substr(dot + 1);
            while (!key.empty() && std::isspace(key.front())) key.erase(key.begin());
        }

        try {
            if (key == "total_packets") stats.total_packets = std::stoull(val);
            else if (key == "total_flits") stats.total_flits = std::stoull(val);
            else if (key == "total_hops") stats.total_hops = std::stoull(val);
            else if (key == "buffer_reads") stats.buffer_reads = std::stoull(val);
            else if (key == "buffer_writes") stats.buffer_writes = std::stoull(val);
            else if (key == "crossbar_traversals") stats.crossbar_traversals = std::stoull(val);
            else if (key == "arbiter_events") stats.arbiter_events = std::stoull(val);
            else if (key == "link_traversals") stats.link_traversals = std::stoull(val);
            else if (key == "total_cycles") stats.total_cycles = std::stoull(val);
            else if (key == "total_latency") stats.total_latency = std::stoull(val);
            else if (key == "num_routers") stats.num_routers = std::stoul(val);
            else if (key == "num_rows") stats.num_rows = std::stoul(val);
            else if (key == "num_cols") stats.num_cols = std::stoul(val);
            else if (key == "flit_size_bits") stats.flit_size_bits = std::stoul(val);
            else if (key == "clock_mhz") stats.clock_mhz = std::stod(val);
        } catch (...) {
            // Skip unparseable lines
        }
    }
    return stats;
}

/* 1.11.2: a DRAM die has NO free process-node knob. Its process is a
 * consequence of the technology generation (vendors speak in derived classes
 * -- 1x/1y/1z/1a/1b -- not nanometers), so power.tech_node_nm must not reach
 * the DRAM array: before this release, setting a 45 nm *logic* node silently
 * re-priced every HBM3 die as 45 nm DRAM silicon. The class below is the
 * generation the technology implies (vendor/ISSCC provenance, same map the
 * device-model spec fixes), and cacti_table_nm is only the nearest real CACTI
 * table used to shape the structure response -- the absolute scale is the
 * JEDEC k-calibration's job (1.11.1), which divides the table choice out. */
struct DRAMGenClass { const char* cls; int cacti_table_nm; };
/* 1.11.51 (N3, user ruling 2026-08-17 "go with the banded form"): the DRAM
 * generation's feature size F, nm -- cell area = 6F^2 (buried-wordline
 * architectural fact), so the array's bitline pitch is 2F. Mid-values for
 * compound classes. Used to REPORT the generation's own pitch beside the
 * derived pitch-factor band; the band's endpoints are published silicon
 * (FIMDRAM ISSCC 2021 25.4 lean-SIMD bound; UPMEM HC31-implied
 * general-purpose end incl. its 3-metal-layer routing loss). */
static double dramGenFeatureNm(const std::string& cls) {
    if (cls == "3x/2x") return 25.0;
    if (cls == "1x")    return 19.0;
    if (cls == "1y")    return 17.5;
    if (cls == "1y/1z") return 16.5;
    if (cls == "1z")    return 15.5;
    if (cls == "1a")    return 14.0;
    if (cls == "1a/1b") return 13.25;
    if (cls == "1b")    return 12.5;
    return 0.0;
}

static DRAMGenClass getDRAMGenClass(const std::string& tech) {
    /* 1.11.14 (borders rule): the generation map moved into the CACTI fork
     * with the calibration it serves; this is a thin read of the tool's
     * tables so callers keep their shape. */
    return { pimid::CACTIWrapper::generationClass(tech),
             pimid::CACTIWrapper::generationTableNm(tech) };
}

/* 1.11.28 (user ruling E3): ONE owner for the placement x technology process
 * family. It was decided in the device path and NOWHERE else, so the synthetic
 * in-memory NoC probe -- which builds its own McPAT config and declares
 * device_scope=true -- silently defaulted to family 0 and priced the on-die
 * fabric in a LOGIC process while the PEs beside it, on the same die, priced
 * as DRAM-periphery. Two copies of a decision is how they drift.
 *
 * 1.11.57 (latent A027): the sentence that used to end that paragraph -- "this
 * is the single copy" -- is withdrawn, because it is not true and a reader who
 * believes it will edit here and stop. This function is the single owner of
 * ONE thing: McPATWrapper::SystemConfig::process_family and the generation pin
 * that goes with it. The same "is this fabric on DRAM silicon?" matrix is
 * re-derived at three further sites, deliberately, because each asks it about
 * a different subject:
 *   - buildNoCLevelsForMcPAT sets a PER-LEVEL on_dram_die (levels 0-3, plus 5
 *     on channel-centric parts), which this function has no per-level answer
 *     for. Its dram_family_tech guard was missing until 1.11.52 (B038).
 *   - the system-scope per-node site asks about node.memory_tech, while
 *     everything here reads the GLOBAL config.memory_tech. Identical today;
 *     divergent the moment a decoupled system prices a node whose technology
 *     differs from the global one.
 *   - the termination / MC-interface tier predicate asks a related but
 *     distinct question (does this access cross an off-package DQ pin?).
 * They cannot disagree in any configuration this build accepts -- the whole
 * family transform in processor.cc, including every on_dram_die read, lives
 * inside `if (dram_periph_family == 1)`, which only this function sets. Any
 * future use of on_dram_die outside that guard makes them live.
 *
 * Returns true when the caller sits on DRAM silicon. The caller supplies the
 * placement level, because the probe and the PE path answer that differently
 * (the probe describes the fabric AT a placement, not a PE at one). */
static bool applyProcessFamily(pimid::McPATWrapper::SystemConfig& mcfg,
                               const UnifiedConfig& config,
                               int pe_hierarchy_level,
                               bool quiet) {
    const bool dram_family_tech =
        !(config.memory_tech == "SRAM" || config.memory_tech == "STT_MRAM" ||
          config.memory_tech == "PCM"  || config.memory_tech == "RERAM");
    const bool channel_centric =
        config.memory_tech.rfind("LPDDR", 0) == 0 ||
        config.memory_tech.rfind("GDDR", 0) == 0 ||
        config.memory_tech.rfind("HBM", 0) == 0;
    const bool on_dram = dram_family_tech &&
        ((pe_hierarchy_level >= 0 && pe_hierarchy_level <= 3) ||
         (pe_hierarchy_level == 5 && channel_centric));
    if (on_dram) {
        mcfg.process_family = 1;
        DRAMGenClass gc = getDRAMGenClass(config.memory_tech);
        mcfg.dram_periph_table_nm = gc.cacti_table_nm;
        /* 1.11.52 (N2 completion; audit 1162-A): the helper now carries the
         * generation pin too, so its caller -- the in-memory NoC probe --
         * prices the fabric at the same node as the PEs beside it. Without
         * this the probe kept config.tech_node_nm: one die, two processes,
         * the exact defect the probe's own E3 comment says was fixed. */
        if (mcfg.tech_node_nm != gc.cacti_table_nm) {
            if (!quiet)
                std::cout << "  [tech] on-die fabric: process pinned to "
                             "generation " << gc.cls << " (CACTI "
                          << gc.cacti_table_nm << "nm table)." << std::endl;
            mcfg.tech_node_nm = gc.cacti_table_nm;
        }
        if (!quiet)
            std::cout << "  [tech] process family: DRAM-periphery (placement "
                      << pe_hierarchy_level << " on " << config.memory_tech
                      << ", class " << gc.cls << ")" << std::endl;
    } else {
        mcfg.process_family = 0;
    }
    return on_dram;
}


/**
 * Map memory technology to McPAT MC parameters.
 */
/* 1.11.2: validate a requested technology node against the positive-list.
 *
 * The 1.9.35 clamp only guarded the bottom (sub-22 raised to 22, loudly).
 * Everything else passed through, which admitted two failure modes the tools
 * themselves do not guard: CACTI silently INTERPOLATES between its tables for
 * any intermediate value (28 nm quietly becomes a 32/22 blend of unvalidated
 * quality), and its 16nm.dat is a 25-byte stub reading "Invalid technology
 * nodes" that the dispatcher will happily try to parse for 16-21 nm requests.
 *
 * The only nodes every linked tool evaluates from real tables are 22, 32, 45,
 * 65 and 90 nm (CACTI exact tables; NVSim branches; McPAT rides the CACTI
 * params; 22 is also the McPAT calibration floor). Anything else is now a
 * fatal configuration error naming the valid set -- not a clamp, not a warn:
 * a swept study must fail loudly at the invalid point, not plateau silently. */
static int validateTechNodeNm(int requested_nm, const char* site) {
    if (requested_nm <= 0) return 22;  // unset: the calibrated default
    static const int kValidNodes[] = {22, 32, 45, 65, 90};
    for (int n : kValidNodes) {
        if (requested_nm == n) return requested_nm;
    }
    std::cerr << "ERROR: technology node " << requested_nm << " nm (" << site
              << ") is not supported. Valid nodes: 22, 32, 45, 65, 90 nm "
                 "(the nodes all linked tools evaluate from real, calibrated "
                 "tables; 22 nm is the finest)." << std::endl;
    std::exit(1);
}

static pimid::McPATWrapper::MCTechParams getMCTechParamsForMcPAT(
    const std::string& tech, int num_mcs)
{
    pimid::McPATWrapper::MCTechParams p;
    p.number_mcs = std::max(1, num_mcs);

    if (tech == "DDR3") {
        p.peak_transfer_rate = 1600; p.databus_width = 64; p.number_ranks = 2;
    } else if (tech == "DDR4") {
        /* 1.11.57 (latent B044): the "DRAM" spelling is gone from this row and
         * the 1.11.51 (L76) note above it is withdrawn. That note said "DRAM"
         * was an ACCEPTED CANONICAL TECHNOLOGY; it is not -- canonicalMemTech()
         * resolves the generic alias to DDR4 and every consumer sees only
         * generation names. The row is unchanged for real DDR4. */
        p.peak_transfer_rate = 3200; p.databus_width = 64; p.number_ranks = 2;
    } else if (tech == "DDR5") {
        p.peak_transfer_rate = 4800; p.databus_width = 64; p.number_ranks = 2;
    } else if (tech == "LPDDR5") {
        p.peak_transfer_rate = 6400; p.databus_width = 32; p.number_ranks = 1;
    } else if (tech == "GDDR6") {
        p.peak_transfer_rate = 4000; p.databus_width = 32; p.number_ranks = 1;
    } else if (tech == "HBM2") {
        p.peak_transfer_rate = 2400; p.databus_width = 128; p.number_ranks = 1;
    } else if (tech == "HBM3") {
        p.peak_transfer_rate = 6400; p.databus_width = 128; p.number_ranks = 1;
    } else {
        // SRAM / NVM -- simple controller
        p.peak_transfer_rate = 1600; p.databus_width = 64; p.number_ranks = 1;
    }
    return p;
}

/**
 * Build per-hierarchy-level NoCLevelConfig for McPAT.
 * If hierarchy is enabled, creates one NoC instance per active level.
 * Otherwise, creates a single NoC from the flat topology config.
 *
 * All derived parameters (duty_cycle, chip_coverage) are printed with
 * formulas and can be overridden via YAML power.mcpat_overrides.
 */
static std::vector<pimid::McPATWrapper::NoCLevelConfig> buildNoCLevelsForMcPAT(
    const UnifiedConfig& config,
    const GarnetParsedStats& garnet,
    uint64_t total_cycles,
    const std::map<std::string, double>& overrides)
{
    using NoCLevel = pimid::McPATWrapper::NoCLevelConfig;
    std::vector<NoCLevel> levels;

    // Map PIMID topology string to McPAT type (0=bus, 1=router-based)
    auto topologyToMcPATType = [](const std::string& topo) -> int {
        if (topo == "BUS") return 0;
        return 1;  // MESH_2D, TORUS_2D, RING, CROSSBAR, FAT_TREE, H_TREE, CUSTOM
    };

    if (config.hierarchy_enabled && config.pe_hierarchy_level >= 0 && config.pe_hierarchy_level < 7) {
        // Names for hierarchy levels
        static const char* level_names[] = {
            "L0_subarray", "L1_bank", "L2_bankgroup",
            "L3_chip", "L4_rank", "L5_channel", "L6_system"
        };

        // Count active levels (from PE placement level up to system)
        int pe_level = config.pe_hierarchy_level;
        int num_active = 7 - pe_level;
        if (num_active < 1) num_active = 1;

        /* 1.9.22: McPAT's NoC `total_accesses` counts ROUTER TRAVERSALS, not
         * end-to-end packets -- a packet crossing N routers is N accesses. Using
         * total_packets undercounted the activity by the average hop count
         * (measured 161,802 hops for 79,496 packets on a co-sim HBM3 cell, i.e.
         * 2.04x). Prefer the measured hop count and fall back to packets only if
         * hops were not recorded. */
        uint64_t garnet_packets = (garnet.total_hops > 0)
                                ? garnet.total_hops : garnet.total_packets;

        /* 1.11.50 (L74): the DRAM-periphery family used to blanket EVERY
         * level in this vector -- processor.cc transformed rank/channel/
         * system fabrics that the placement matrix itself declares to be
         * logic (buffer die or host board). Each level now carries the
         * matrix's own verdict: subarray..chip (0-3) fabrics sit on the
         * die; the channel fabric (5) is on-die only for channel-centric
         * parts (LPDDR/GDDR/HBM), where the channel periphery is the die's
         * own interface; rank (4) and system (6) are always off-die logic.
         * Same predicate as applyProcessFamily -- one matrix, two readers. */
        const bool noc_channel_centric =
            config.memory_tech.rfind("LPDDR", 0) == 0 ||
            config.memory_tech.rfind("GDDR", 0) == 0 ||
            config.memory_tech.rfind("HBM", 0) == 0;

        /* 1.11.52 (A021): which levels will actually be priced? A level with
         * neither a branch router nor an endpoint is wire and is skipped
         * below; the weight and coverage denominators must match that set. */
        double surviving_weight = 0.0;
        int surviving_levels = 0;
        for (int l = pe_level; l < 7; l++) {
            if (config.htree_level_branch[0] >= 0 &&
                (config.htree_level_branch[l] + config.htree_level_endpoints[l]) <= 0)
                continue;
            surviving_weight += 1.0 / (1 + l - pe_level);
            surviving_levels++;
        }
        if (surviving_levels == 0) { surviving_levels = 1; surviving_weight = 1.0; }
        if (surviving_levels != num_active)
            std::cout << "  [NoC] " << (num_active - surviving_levels)
                      << " hierarchy level(s) are pure pass-through wire and are"
                         " not priced; traversal weights and chip coverage are"
                         " normalised over the " << surviving_levels
                      << " level(s) that are" << std::endl;

        for (int lvl = pe_level; lvl < 7; lvl++) {
            NoCLevel nc;
            nc.name = level_names[lvl];
            /* 1.11.52 (audit B038): the family owner's guard was dropped
             * here. applyProcessFamily requires dram_family_tech BEFORE the
             * placement test, so an SRAM or NVM device is family 0 at every
             * level -- but this predicate omitted it and declared the fabric
             * of an SRAM/NVM device to be ON THE DRAM DIE at levels 0-3,
             * which is what decides whether processor.cc applies the
             * DRAM-periphery transform to that fabric. Same matrix, same
             * guard, one reader.
             *
             * 1.11.56 (audit A026): "one reader" is true of the matrix, not
             * of the technology it is asked about. This helper reads the
             * GLOBAL config.memory_tech while the per-node site reads the
             * node's own -- identical today, and divergent the moment a
             * decoupled system prices a node whose technology differs from
             * the global one. Whoever adds that case must pass the node's
             * technology in here too. */
            const bool noc_dram_family_tech =
                !(config.memory_tech == "SRAM" || config.memory_tech == "STT_MRAM" ||
                  config.memory_tech == "PCM"  || config.memory_tech == "RERAM");
            nc.on_dram_die = (noc_dram_family_tech &&
                              ((lvl <= 3) ||
                               (lvl == 5 && noc_channel_centric))) ? 1 : 0;

            // Lower levels (subarray/bank) -> bus; upper levels -> router NoC
            nc.type = (lvl <= 1) ? 0 : topologyToMcPATType(config.noc_topology);

            /* 1.11: size each level from the BUILT tree, not from the raw
             * organisation count. total_network_endpoints counts every bank and
             * subarray -- ~528 on a 16-element HBM3 config -- so this loop was
             * pricing a 23x23 bus and a 12x12 bank-group NoC: hundreds of
             * routers, 53 of the device's 63 mm^2 and 1.3 W of leakage, for a
             * tree that actually built ONE branch router. A level's chargeable
             * nodes are its branch routers plus the endpoints attached there;
             * a level with neither is wire and is skipped. Falls back to the
             * old estimate only when no tree was built. */
            // Estimate node counts per level (use network endpoints at PE level)
            int nodes = 1;
            int pe_level_nodes = (config.total_network_endpoints > 0)
                                  ? config.total_network_endpoints : config.num_pes;
            if (config.htree_level_branch[0] >= 0) {
                int chg = config.htree_level_branch[lvl]
                        + config.htree_level_endpoints[lvl];
                if (chg <= 0) continue;   // pure pass-through wire: nothing to price
                nodes = chg;
                int g = static_cast<int>(std::ceil(std::sqrt((double)chg)));
                nc.horizontal_nodes = g;
                nc.vertical_nodes = g;
            } else if (lvl == pe_level) {
                int grid = static_cast<int>(std::ceil(std::sqrt(pe_level_nodes)));
                nc.horizontal_nodes = grid;
                nc.vertical_nodes = grid;
                nodes = pe_level_nodes;
            } else {
                // Higher levels have fewer nodes
                int factor = 1;
                for (int l = pe_level; l < lvl; l++) {
                    // Fan-in from level l to l+1 = physical org count ratio.
                    // l==0 (subarray->bank) MUST use subarrays_per_bank, not a
                    // hardcoded 2: at SUBARRAY placement (pe_level==0) a wrong ratio
                    // leaves every upper level (bank/bankgroup/chip) over-counted by
                    // subarrays_per_bank/2, inflating NoC leakage ~10x. This branch
                    // only executes for pe_level==0, so BANK+ placements are unchanged.
                    if (l == 0) factor *= config.subarrays_per_bank;
                    else if (l == 1) factor *= config.hierarchy_banks_per_bg;
                    else if (l == 2) factor *= config.hierarchy_bg_per_chip;
                    else if (l == 3) factor *= config.hierarchy_chips_per_rank;
                    /* 1.11.56 (audit A012): the top two ratios are physical
                     * too, and a literal 2 is not either of them. This ladder
                     * is taken whenever the H-tree was not built, i.e. on
                     * every analytical-NoC run -- half the shipped model
                     * lineup -- and it sets the node count that divides NoC
                     * router/link area and leakage at the channel and system
                     * levels. On DDR5 at BANK placement the true ratio is 1
                     * and the literal gave those levels half the nodes they
                     * have; on HBM3 (16 channels) the channel level got 2x
                     * too few and the system level 4x too many. */
                    else if (l == 4) factor *= std::max(1, config.hierarchy_ranks_per_channel);
                    else if (l == 5) factor *= std::max(1, config.hierarchy_dram_channels);
                    else factor *= 1;   // above the system root there is no further fan-in
                }
                nodes = std::max(2, pe_level_nodes / factor);
                int g = static_cast<int>(std::ceil(std::sqrt(nodes)));
                nc.horizontal_nodes = g;
                nc.vertical_nodes = g;
            }

            nc.input_ports = (nc.type == 0) ? nodes : 5;
            nc.output_ports = (nc.type == 0) ? nodes : 5;
            /* 1.9.22: take the flit width and the network clock from the MEASURED
             * Garnet stats rather than a literal and the PE frequency. The run
             * writes both to garnet_stats.txt; hardcoding 128 silently ignored a
             * reconfigured flit size, and config.frequency_mhz is the PE clock,
             * not the network's. */
            nc.flit_bits = (garnet.flit_size_bits > 0)
                         ? static_cast<int>(garnet.flit_size_bits) : 128;
            /* 1.11.60 (audit round 4, B014): ONE clock authority, numerator
             * and denominator alike.
             *
             * The duty-cycle conversion five lines below asks
             * deviceCycleClockMHz() -- 1.11.57's A007 fix, which exists
             * because in system scope config.frequency_mhz is the top-level
             * system.frequency_mhz and not the device's. A007 converted the
             * DENOMINATOR and left this fallback on the raw field, so the two
             * halves of one ratio came from two different clocks. The fallback
             * is live whenever no Garnet stats were parsed, i.e. on every
             * analytical run -- half the shipped model lineup -- and on the
             * shipped co-sim example that is 2000 MHz against a 500 MHz
             * device: net_cycles scaled by 4 instead of 1, so every NoC
             * level's duty_cycle handed to McPAT is 4x too small and the
             * fabric's dynamic power with it. Invisible because in device
             * scope the two fields are the same number, which is the scope the
             * A007 gate exercised. */
            nc.clock_mhz = (garnet.clock_mhz > 0.0)
                         ? garnet.clock_mhz : deviceCycleClockMHz(config);

            /* Distribute Garnet traversals: lower levels see more traffic;
             * level i gets a share proportional to 1/(i+1-pe_level).
             *
             * 1.11.52 (audit A021): NORMALISE OVER THE LEVELS THAT SURVIVE.
             * Levels with no chargeable node are skipped by the `continue`
             * above (pure pass-through wire), but the weight sum and the
             * coverage denominator still counted them -- so a skipped level's
             * share of the MEASURED hop count was dropped on the floor and
             * the emitted chip_coverage no longer tiled the die, silently.
             * Both denominators now use the surviving set, computed once. */
            double level_weight = 1.0 / (1 + lvl - pe_level);
            uint64_t level_accesses = static_cast<uint64_t>(
                garnet_packets * level_weight / surviving_weight);
            double chip_cov = 1.0 / static_cast<double>(surviving_levels);

            /* duty_cycle: fraction of peak bandwidth [0,1].
             * Peak = nodes x 1 access/cycle, so duty = accesses / (cycles x nodes).
             *
             * 1.11.52 (audit A016): the CYCLES MUST BE THE NETWORK'S OWN. The
             * caller passes the PE/zsim-domain cycle count, while nc.clock_mhz
             * two lines below is the MEASURED Garnet clock -- so when the
             * fabric runs at a different frequency than the elements, the duty
             * was off by exactly their ratio. Convert the window into network
             * cycles before dividing. */
            double net_cycles = static_cast<double>(total_cycles);
            {   // 1.11.57 (A007): the clock these cycles were ticked in.
                const double cyc_mhz = deviceCycleClockMHz(config);
                if (nc.clock_mhz > 0.0 && cyc_mhz > 0.0)
                    net_cycles *= (nc.clock_mhz / cyc_mhz);
            }
            double duty = (net_cycles > 0.0 && nodes > 0)
                ? static_cast<double>(level_accesses) / (net_cycles * nodes) : 0.0;

            // Apply overrides
            std::string prefix = "noc." + std::to_string(lvl - pe_level);
            auto it_dc = overrides.find(prefix + ".duty_cycle");
            if (it_dc != overrides.end()) duty = it_dc->second;
            auto it_cc = overrides.find(prefix + ".chip_coverage");
            if (it_cc != overrides.end()) chip_cov = it_cc->second;
            auto it_acc = overrides.find(prefix + ".total_accesses");
            if (it_acc != overrides.end()) level_accesses = static_cast<uint64_t>(it_acc->second);

            nc.total_accesses = level_accesses;
            nc.duty_cycle = duty;
            nc.chip_coverage = chip_cov;

            levels.push_back(nc);
        }
    } else {
        // Single flat NoC
        NoCLevel nc;
        nc.name = "NoC_flat";
        nc.type = topologyToMcPATType(config.noc_topology);
        int flat_nodes = (config.total_network_endpoints > 0)
                          ? config.total_network_endpoints : config.num_pes;
        int grid = static_cast<int>(std::ceil(std::sqrt(flat_nodes)));
        nc.horizontal_nodes = grid;
        nc.vertical_nodes = grid;
        nc.input_ports = 5;
        nc.output_ports = 5;
        /* 1.11.52 (audit A032): the flat branch takes the same MEASURED
         * inputs 1.9.22 gave the hierarchical one. A hardcoded 128-bit flit
         * ignored a reconfigured width; the PE clock is not the network's;
         * and McPAT's total_accesses counts ROUTER TRAVERSALS, so packets
         * undercount by the average hop count (measured 2.04x on a co-sim
         * HBM3 cell). This branch runs whenever hierarchy is disabled or the
         * placement level is out of range. */
        nc.flit_bits = (garnet.flit_size_bits > 0)
                     ? static_cast<int>(garnet.flit_size_bits) : 128;
        // 1.11.60 (audit round 4, B014): same authority as the hierarchical
        // branch, and the same one the duty-cycle denominator below uses.
        nc.clock_mhz = (garnet.clock_mhz > 0.0) ? garnet.clock_mhz
                                                : deviceCycleClockMHz(config);
        nc.chip_coverage = 1.0;

        uint64_t accesses = (garnet.total_hops > 0) ? garnet.total_hops
                                                    : garnet.total_packets;
        // duty_cycle: fraction of peak bandwidth [0,1], in NETWORK cycles (A016)
        double flat_net_cycles = static_cast<double>(total_cycles);
        {   // 1.11.57 (A007): same authority as the hierarchical branch.
            const double cyc_mhz = deviceCycleClockMHz(config);
            if (nc.clock_mhz > 0.0 && cyc_mhz > 0.0)
                flat_net_cycles *= (nc.clock_mhz / cyc_mhz);
        }
        double duty = (flat_net_cycles > 0.0 && flat_nodes > 0)
            ? static_cast<double>(accesses) / (flat_net_cycles * flat_nodes) : 0.0;

        auto it_dc = overrides.find("noc.0.duty_cycle");
        if (it_dc != overrides.end()) duty = it_dc->second;
        auto it_cc = overrides.find("noc.0.chip_coverage");
        if (it_cc != overrides.end()) nc.chip_coverage = it_cc->second;
        auto it_acc = overrides.find("noc.0.total_accesses");
        if (it_acc != overrides.end()) accesses = static_cast<uint64_t>(it_acc->second);

        nc.total_accesses = accesses;
        nc.duty_cycle = duty;

        levels.push_back(nc);
    }
    return levels;
}

/* 1.11.17 (audit go-through): the FP-without-FPU contradiction report, ONE
 * copy. Two defects in the old shape: (1) it lived only inside the two
 * power-analysis paths, so --no-power hid the contradiction while the
 * timing charge still fired; (2) it used the ALL-NODE mix_fp, so in a
 * co-simulation the HOST's FP stream was attributed to the FPU-less device
 * elements. The device group's own census is used when the dump carries
 * one. */
static void reportFpWithoutFpu(const UnifiedConfig& config,
                               const ZSimParsedOutput& zsim_stats) {
    if (config.pe_has_fp) return;
    uint64_t dev_fp = zsim_stats.dev.has_activity() ? zsim_stats.dev.mix_fp
                                                    : zsim_stats.mix_fp;
    if (dev_fp == 0) return;
    std::cout << "  [fp] " << dev_fp
              << " FP-class instructions executed on an element declared "
                 "WITHOUT an FP unit";
    if (config.pe_fp_emul_cycles > 0) {
        std::cout << "; charged " << config.pe_fp_emul_cycles
                  << " cycles each as soft-float emulation ("
                  << (dev_fp * (uint64_t)config.pe_fp_emul_cycles)
                  << " cycles total)" << std::endl;
    } else {
        std::cout << "; charged NOTHING (pim.pe.fp_emulation_cycles=0) -- "
                     "the reported time is for hardware this element does "
                     "not have" << std::endl;
    }
}

static int memorySystemDieCount(const std::string& tech,
                                const std::string& device_width,
                                int ranks_per_channel, int channels);  // 1.11.51 (L243)

/* 1.11.52 (audit A018/A019): ONE predicate for "does an access at this
 * placement drive OFF-PACKAGE DQ pins, and therefore pay termination?"
 *
 * There were two, and they disagreed. The MC-interface tier says levels 5
 * (on channel-centric parts) and 6 are INTERPOSER -- TSVs and microbumps,
 * priced at no ODT "by physics" -- while the termination predicate charged
 * everything at level >= 4. The reachable non-zero disagreements were
 * CHANNEL placement on GDDR6 (0.333 nJ/access, ~5% of the memory's
 * per-access dynamic) and on LPDDR5 (0.036 nJ), plus any DDR technology
 * configured at LOGIC_DIE. HBM was inert only because its termination model
 * already returns 0 by physics -- an accident, not agreement.
 *
 * The tiers, stated once: on-die (subarray..chip) drives no pins; interposer
 * (LOGIC_DIE, or the channel tier of a channel-centric part) crosses TSVs
 * and microbumps, which our termination model prices at zero ODT; rank and
 * above, and HOST_MC, cross the package boundary and pay full termination. */
static bool crossesOffPackageDQ(int pe_hierarchy_level,
                                const std::string& memory_tech) {
    const bool channel_centric =
        memory_tech.rfind("LPDDR", 0) == 0 ||
        memory_tech.rfind("GDDR", 0) == 0 ||
        memory_tech.rfind("HBM", 0) == 0;
    if (pe_hierarchy_level == -1) return true;             // HOST_MC: host DIMM pins
    if (pe_hierarchy_level >= 0 && pe_hierarchy_level <= 3) return false;  // on-die
    if (pe_hierarchy_level == 6) return false;             // LOGIC_DIE: interposer
    if (pe_hierarchy_level == 5) return !channel_centric;  // channel tier
    return true;                                           // RANK and anything above
}

/* 1.11.52 (audit 1162 A001/A002/A003/A025): ONE OWNER for the device
 * corner, the DRAM-periphery factors and the periphery clock ceiling.
 *
 * The per-node (system/co-sim) path carried a DIVERGENT COPY of this
 * decision, and the copy was wrong in three ways at once: it REFUSED the
 * corner outright where this logic maps it to CACTI's lp-dram column (E2);
 * it priced the area factor from two hardcoded literals (2.44 / 2.46)
 * instead of reading the columns, so the corner, the baseline device and
 * the temperature never entered the number; and it guarded the clock with
 * a literal 700 MHz instead of the calculated ceiling. Its own comment
 * claimed "Same behavior as the device-scope site, per node" -- it was not.
 * Both paths now call this, so they cannot drift again.
 *
 * `label` prefixes every message ("" in device scope, "<node>: " per node);
 * `freq_mhz` and `gc` belong to the element being priced. */
static void applyCornerAndPeripheryPricing(
        pimid::McPATWrapper::SystemConfig& mcfg,
        const UnifiedConfig& config,
        const std::map<std::string, double>& overrides,
        const DRAMGenClass& gc,
        double freq_mhz,
        const std::string& label) {
    auto ovInt = [&overrides](const std::string& key, int fallback) -> int {
        auto it = overrides.find(key);
        return (it != overrides.end()) ? static_cast<int>(it->second) : fallback;
    };
        int corner = (config.device_corner == "lstp") ? 1
                   : (config.device_corner == "lop")  ? 2 : 0;
        /* 1.11.21 (user ruling E1+E2): the BASELINE McPAT will actually price
         * at -- override included. The refusal below tests THIS, not the
         * corner alone, because power.mcpat_overrides.device_type used to be
         * applied AFTER the refusal and silently reinstated what was refused. */
        int baseline = ovInt("device_type", corner);
        /* 1.11.21 (E2): on a DRAM-periphery placement the low-power device is
         * lp-dram (CACTI column 3), not logic lstp/lop. The corner names an
         * intent; the family decides which column carries it. */
        if (mcfg.process_family == 1 && !overrides.count("device_type") &&
            (baseline == 1 || baseline == 2)) {
            std::cout << "  [tech] " << label << "power.device_corner=" << config.device_corner
                      << " on a DRAM-periphery placement maps to the lp-dram "
                         "column: the low-power variant of a DRAM periphery "
                         "device is lp-dram, not logic lstp/lop." << std::endl;
            baseline = 3;
        }
        if (mcfg.process_family == 1) {
            double fa_c = 0, fd_c = 0, fl_c = 0;
            const bool coherent = pimid::McPATWrapper::periphFactorsFor(
                mcfg.dram_periph_table_nm, mcfg.tech_node_nm, baseline,
                mcfg.temperature_k, fa_c, fd_c, fl_c);
            if (!coherent) {
                std::cerr << "[config] FATAL: " << label << "DRAM-periphery components cannot be "
                             "priced at device column " << baseline
                          << " (power.device_corner=" << config.device_corner
                          << (overrides.count("device_type")
                                  ? ", overridden by power.mcpat_overrides.device_type"
                                  : "")
                          << ").\n"
                             "  All three family factors are ratios against "
                             "that column in the "
                          << mcfg.dram_periph_table_nm << " nm table, and the "
                             "column is not populated there (lp-dram is all-zero "
                             "at 22 nm; it is real data at 32 and 45 nm). "
                             "Deriving a factor from an empty column would "
                             "produce a number with no referent.\n"
                             "  Either run this placement at 32/45 nm, drop the "
                             "corner, or move the PE to a LOGIC placement.\n";
                std::exit(2);
            }
            corner = baseline;
            /* 1.11.17 (audit go-through): report the factor actually APPLIED
             * -- the emitted XML carries fa x subarray_pitch_factor, and the
             * old print showed fa alone, so at SUBARRAY placement the
             * reported factor was not the one in use. */
            /* 1.11.21: the value in USE, read from the table, not a literal. */
            double fa = fa_c;
            double pitch = (mcfg.subarray_pitch_factor > 0.0)
                               ? mcfg.subarray_pitch_factor : 1.0;
            std::cout << "  [tech] " << label << "periphery factors READ: comm-dram from "
                      << mcfg.dram_periph_table_nm << "nm.dat / baseline from "
                      << mcfg.tech_node_nm << "nm.dat at "
                      << (mcfg.temperature_k - 273) << "C: area " << fa
                      << "x, dynamic " << fd_c << "x, leakage " << fl_c << "x";
            if (pitch != 1.0) {
                /* 1.11.54 (audit E001, adjacent): PRINT WHAT IS APPLIED.
                 * Since 1.11.50/1.11.51 the pitch penalty lands on the
                 * measured on-pitch LOGIC SHARE of the core, not on the whole
                 * core: the transform applies fa * (1 + (pitch-1)*share). This
                 * line kept printing fa * pitch -- on the SUBARRAY/HBM3 cell
                 * it claimed 3.05556x while the transform applied 2.5574x, a
                 * 19.5% gap between the reported factor and the real one,
                 * under a comment promising "the factor actually APPLIED".
                 * The share is measured inside the fork (it needs McPAT's own
                 * area breakdown), so it is named here rather than restated:
                 * the fork prints the share and the effective multiplier on
                 * the [fam] line. */
                std::cout << " x pitch " << pitch
                          << " on the on-pitch logic share only (the fork's"
                             " [fam] line prints the share and the effective"
                             " multiplier; the flat product " << (fa * pitch)
                          << "x is NOT what is applied)";
            }
            /* 1.11.27: the band is no longer open at the top. Samsung's
             * FIMDRAM (Kwon, ISSCC 2021 25.4) replaced half the cell array in
             * each bank with a 16-wide SIMD PCU on a 20nm DRAM process, at
             * unchanged HBM2 dimensions: 8 GB -> 6 GB, so 2 GB of array bought
             * the compute. At our measured HBM2 density (Sohn, ISSCC 2016
             * 18.2: 8 Gb / 96 mm^2) that is ~24 mm^2 per die over 16 PCUs, so
             * ~1.5 mm^2 per PCU -- an UPPER bound, since some of the reclaimed
             * area is control and routing.
             * Our comparable PE prices at 0.4925 mm^2 in logic. The linear end
             * puts it at 1.204 mm^2, UNDER that bound; the squared end puts it
             * at 2.943 mm^2, 1.96x ABOVE it. Silicon therefore contradicts the
             * pessimistic end and is consistent with the linear one, which is
             * the value we apply. The UPMEM die is NOT a quantitative anchor:
             * its Hot Chips 31 deck carries no die area at all. */
            std::cout << " -- uncertainty band ["
                      << (fa * pitch) << ", " << (fa * fa * pitch)
                      << "]: the linear ratio is applied. Samsung FIMDRAM "
                         "(ISSCC 2021 25.4, ~1.5 mm^2/PCU upper bound) is "
                         "consistent with the linear end and rules out the "
                         "squared one, which would exceed it by 1.96x."
                      << std::endl;
            /* 1.11.52: the CALCULATED periphery clock ceiling travels with
             * the factors (audit A003) -- the per-node copy used a literal
             * 700 MHz and no reference-clock anchor at all. */
            /* 1.11.35 (user ruling E13): the feasible ceiling is CALCULATED,
             * not cited. The threshold comes from the device physics in the
             * CACTI tables; published parts are used to CHECK it, never to set
             * it.
             *
             * A DRAM-periphery device is slower than a logic device by its
             * CV/I delay ratio -- computed from the same two columns the area
             * and dynamic factors already use:
             *     t = (C_g_ideal + C_fringe) * Vdd / I_on
             *     22 nm  hp 1.179e-13 s   comm-dram 2.491e-13 s   -> 2.113x
             *     32 nm                                           -> 1.291x
             * NOTE this is NOT the I_on ratio (2.885x at 22 nm). Drive alone
             * overstates the penalty, because the periphery device also has
             * LOWER gate capacitance (1.99e-16 against 3.27e-16 F/um), which
             * partly offsets its weaker drive. An earlier draft of this guard
             * used I_on alone and was wrong by 37%.
             *
             * So the ceiling for a periphery PE is the logic reference clock
             * this machine runs at, divided by that ratio. Above it, the same
             * design in a logic process would have to exceed the reference --
             * the clock is not a cost question, it is unreachable.
             *
             * CORROBORATION, deliberately not the threshold: shipped parts sit
             * well under this bound, because real designs are limited by power,
             * thermals and pitch as well as by transistor delay --
             *   UPMEM DPU  500 MHz  (Devaux, Hot Chips 31 p3/p4; p10 notes the
             *                        14 pipeline stages needed to reach it)
             *   FIMDRAM    300 MHz  (Kwon, ISSCC 2021 25.4)
             * An earlier comment here claimed "UPMEM DPU: 350-466 MHz", a
             * figure that appears in no source we hold. */
            {
                const double f = freq_mhz;
                const double delay_ratio =
                    (gc.cacti_table_nm == 32) ? 1.291 : 2.113;   // CV/I, CACTI columns
                /* 1.11.35: the anchor is the USER'S logic reference, or a
                 * genuine logic node's clock (the host) when co-simulating.
                 * It is NOT reference_frequency_mhz -- that is the max over ALL
                 * nodes, which in device scope is the PE itself, making the
                 * test circular. An earlier draft did that and warned on every
                 * device-scope run. */
                double ref = config.logic_reference_mhz;
                const char* ref_src = "power.logic_reference_mhz";
                if (!(ref > 0.0)) {
                    for (const auto& n2 : config.system_nodes)
                        if (n2.role == UnifiedConfig::SystemNode::HOST &&
                            n2.frequency_mhz > ref) {
                            ref = n2.frequency_mhz; ref_src = "host node clock";
                        }
                }
                const double demonstrated_max = 500.0;   // UPMEM, for the note only
                if (!(ref > 0.0)) {
                    /* No logic anchor exists. Say so instead of inventing one
                     * or comparing the PE against itself. */
                    if (f > demonstrated_max)
                        std::cout << "  [tech] " << label << "NOTE: " << f
                                  << " MHz PE in DRAM periphery. No logic "
                                     "reference is configured, so the calculated "
                                     "ceiling (reference / CV/I ratio "
                                  << delay_ratio << ") cannot be evaluated -- set "
                                     "power.logic_reference_mhz to enable it. For "
                                     "context only, demonstrated parts run 300 MHz "
                                     "(FIMDRAM) to 500 MHz (UPMEM DPU)."
                                  << std::endl;
                } else if (f > ref / delay_ratio) {
                    const double ceiling = ref / delay_ratio;
                    std::cout << "  [tech] " << label << "WARNING: " << f
                              << " MHz PE in DRAM periphery exceeds the "
                                 "calculated ceiling of " << ceiling
                              << " MHz (logic reference " << ref
                              << " MHz from " << ref_src
                              << " / CV/I delay ratio " << delay_ratio
                              << " from the " << gc.cacti_table_nm
                              << "nm hp vs comm-dram columns). The same design "
                                 "in a logic process would have to beat the "
                                 "reference clock. Power is reported at the "
                                 "requested clock; the clock is the hypothesis."
                              << std::endl;
                } else if (f > demonstrated_max) {
                    std::cout << "  [tech] " << label << "NOTE: " << f
                              << " MHz is within the calculated ceiling ("
                              << (ref / delay_ratio) << " MHz) but above every DEMONSTRATED "
                                 "DRAM-periphery part -- UPMEM DPU 500 MHz "
                                 "(Hot Chips 31), FIMDRAM 300 MHz (ISSCC 2021 "
                                 "25.4). Real parts are bounded by power, "
                                 "thermals and pitch as well as transistor "
                                 "delay." << std::endl;
                }
            }
        } else if (corner != 0) {
            /* 1.11.17: say when a corner is APPLIED, not only when refused --
             * 1.11.13's own stated defect ("priced hp without saying so")
             * survived for every non-hp logic run. */
            std::cout << "  [tech] " << label << "device corner " << config.device_corner
                      << " applied to logic components (CACTI "
                      << (corner == 1 ? "lstp" : "lop") << " device column)."
                      << std::endl;
        }
        mcfg.device_type = corner;   // 1.11.21: override already folded in above
}



/* 1.11.51 (E17): DRAM power-down residency from the MEASURED devMC gap
 * histogram, at a SOURCED per-generation threshold (settable via
 * memory.power_down_threshold_ns). Bucket b holds gaps in [2^b, 2^(b+1));
 * only buckets whose FLOOR clears the threshold count (the boundary bucket
 * is excluded -- conservative), and each qualifying gap pays the threshold
 * once as its entry/exit cost. Returns r in [0,1], or <0 when refused:
 * no histogram in the dump, or a technology with no sourced tXP (GDDR6 and
 * HBM power-down exit timings are not publicly tabulated to us -- the E17
 * ruling forbids inventing one). */
static double gapPowerDownResidency(const UnifiedConfig& config,
                                    const ZSimParsedOutput& z,
                                    double clock_mhz, std::string* prov) {
    /* 1.11.56 (audit A024): SAY why, at both refusal points. This function
     * refuses on purpose -- the E17 ruling forbids inventing a tXP we do not
     * have -- but it refused in silence, and on GDDR6 and HBM2/HBM3, the
     * flagship technologies in this corpus, that means the gap-measured
     * residency never supersedes the phase estimate and no line in the log
     * says so. Nothing numeric changes: the phase-granular background is the
     * correct fallback. What was missing is the reader's ability to tell
     * "refused, no sourced tXP" from "applied, and agreed". */
    if (z.devmc_gap_span == 0 || clock_mhz <= 0.0) {
        std::cout << "  [pg] gap-measured power-down residency NOT used: "
                  << (z.devmc_gap_span == 0
                        ? "no gap histogram was dumped for this run"
                        : "no device memory-controller clock")
                  << ". The reported background is the phase-granular estimate."
                  << std::endl;
        return -1.0;
    }
    double th_ns = config.power_down_threshold_ns;
    const char* src = "memory.power_down_threshold_ns (user override)";
    if (th_ns <= 0.0) {
        const std::string& t = config.memory_tech;
        if      (t == "DDR3")   { th_ns = 6.0; src = "tXP max(3nCK,6ns), JESD79-3D"; }
        else if (t == "DDR4")   { th_ns = 6.0; src = "tXP max(4nCK,6ns), JESD79-4"; }
        else if (t == "DDR5")   { th_ns = 7.5; src = "tXP 7.5ns, JESD79-5"; }
        else if (t == "LPDDR5") { th_ns = 7.5; src = "tXP 7.5ns, JESD209-5"; }
        else {
            std::cout << "  [pg] gap-measured power-down residency NOT used for "
                      << t << ": no power-down exit time (tXP) is tabulated here "
                         "for this technology, and the E17 ruling forbids "
                         "inventing one. Set memory.power_down_threshold_ns to "
                         "supply it. The reported background is the "
                         "phase-granular estimate." << std::endl;
            return -1.0;
        }
    }
    uint64_t th_cyc = static_cast<uint64_t>(th_ns * clock_mhz / 1000.0);
    if (th_cyc < 1) th_cyc = 1;
    uint64_t usable = 0;
    for (int b = 0; b < ZSimParsedOutput::kGapBuckets; b++) {
        if ((1ull << b) < th_cyc) continue;
        uint64_t cyc = z.devmc_gap_cyc[b], cnt = z.devmc_gap_cnt[b];
        uint64_t cost = cnt * th_cyc;
        usable += (cyc > cost) ? (cyc - cost) : 0;
    }
    if (prov) {
        char buf[160];
        snprintf(buf, sizeof(buf), "threshold %.1f ns = %llu cyc @%.0f MHz, %s",
                 th_ns, (unsigned long long)th_cyc, clock_mhz, src);
        *prov = buf;
    }
    /* 1.11.52 (audit A014): REBASE ONTO THE PRICED WINDOW. `usable` and
     * `devmc_gap_span` both live in the histogram's own span -- last event
     * minus first -- while the consumer multiplies this fraction against
     * the FULL priced (ROI) duration when weighting background power. Two
     * denominators fed one weight. The gap span is a sub-interval of the
     * priced window (it starts at the first access after arming and ends at
     * the last), so the usable idle it found is rescaled onto that window;
     * idle outside the first/last access is real idle and is credited as
     * such only to the extent the window extends beyond the span. */
    double r = static_cast<double>(usable) / static_cast<double>(z.devmc_gap_span);
    const double window_cyc = static_cast<double>(z.pg_window())
                            * static_cast<double>(config.phase_length > 0
                                                  ? config.phase_length : 10000);
    if (window_cyc > 0.0) {
        const double span = static_cast<double>(z.devmc_gap_span);
        if (span > 0.0 && span <= window_cyc) {
            /* usable idle within the span, plus the untouched remainder of
             * the priced window, over the priced window. */
            r = (static_cast<double>(usable) + (window_cyc - span)) / window_cyc;
        }
    }
    return (r > 1.0) ? 1.0 : r;
}

/**
 * Run McPAT power analysis using simulation stats.
 * Called after successful simulation in exec/trace/MPI modes.
 *
 * All power/area results come from McPAT.
 * Derived McPAT input parameters are printed with formulas and are
 * overridable via YAML power.mcpat_overrides.
 */
static void runPowerAnalysis(const UnifiedConfig& config,
                             const ZSimParsedOutput& zsim_stats,
                             const std::string& output_dir) {
    using McPAT = pimid::McPATWrapper;

    // YAML overrides for McPAT derived parameters
    const std::map<std::string, double>& overrides = config.mcpat_overrides;

    /* 1.11.11 (#113) / 1.11.17: shared report (see reportFpWithoutFpu). */
    reportFpWithoutFpu(config, zsim_stats);

    /* 1.11.16 (verification audit): the PE-MI locality report existed only
     * in runPerNodePowerAnalysis (system scope), but the placement corpus --
     * the only sweep this measurement exists for -- is entirely DEVICE
     * scope, so the very runs that carry remoteAcc never printed it. */
    if (zsim_stats.pemi_local_acc + zsim_stats.pemi_remote_acc > 0) {
        uint64_t tot = zsim_stats.pemi_local_acc + zsim_stats.pemi_remote_acc;
        std::cout << "  [locality] PE-MI accesses: "
                  << zsim_stats.pemi_local_acc << " local + "
                  << zsim_stats.pemi_remote_acc << " remote ("
                  << std::fixed << std::setprecision(1)
                  << (100.0 * static_cast<double>(zsim_stats.pemi_local_acc) / tot)
                  << "% local at this placement)" << std::defaultfloat
                  << std::setprecision(6) << std::endl;   // 1.11.17: precision must not leak
    }

    /* 1.11.8 (#84): per-component PG residencies from the run's measured
     * activity (r = 1 - activePhases/phases; phases from zsim.out). PE
     * residency uses the device group's per-core mean so N idle PEs and
     * one busy PE weight correctly. Flags off => spec all-false =>
     * bit-identical path (gate invariant). */
    pimid::McPATWrapper::PGSpec pgspec;
    {
        double ph = static_cast<double>(zsim_stats.pg_window());  // 1.11.18: priced window
        if (ph > 0.0 && (config.pg_pe || config.pg_noc || config.pg_mc)) {
            if (config.pg_pe && config.num_pes > 0) {
                double meanActive = static_cast<double>(zsim_stats.dev.pgActivePhases)
                                    / (config.num_pes * ph);
                pgspec.pg_core = true;
                pgspec.r_core = 1.0 - std::min(1.0, meanActive);
                /* 1.11.18: the shared caches carry their OWN residency (the
                 * counter has existed since 1.11.8 and was never consumed). */
                if (zsim_stats.pg_sharedcache_active > 0) {
                    pgspec.have_shared_cache = true;
                    pgspec.r_shared_cache = 1.0 - std::min(1.0,
                        static_cast<double>(zsim_stats.pg_sharedcache_active) / ph);
                }
            }
            if (config.pg_noc) {
                pgspec.pg_noc = true;
                pgspec.r_noc = 1.0 - std::min(1.0,
                    static_cast<double>(zsim_stats.pg_noc_active) / ph);
            }
            if (config.pg_mc) {
                pgspec.pg_mc = true;
                pgspec.r_mc = 1.0 - std::min(1.0,
                    static_cast<double>(zsim_stats.pg_devmc_active) / ph);
            }
            /* 1.11.18 (audit go-through): a residency of exactly 1.0 while
             * the run DID move memory traffic means the tracker was never
             * armed on this path (only the PE-MI and the Ramulator/Simple
             * controllers mark them), not that the component idled. Crediting
             * the whole leakage on an unarmed counter is the most dangerous
             * failure mode this model has, so it is refused and said out
             * loud rather than silently taken. */
            {
                uint64_t traffic = zsim_stats.mem_rd + zsim_stats.mem_wr;
                auto refuse = [&](const char* what, bool& flag, double& r) {
                    if (!flag || r < 1.0 || traffic == 0) return;
                    std::cout << "  [pg] WARNING: " << what << " residency came out "
                                 "1.0 (never marked active) while the run moved "
                              << traffic << " memory accesses -- the tracker is not "
                                 "armed on this path. Refusing the gating credit "
                                 "for it (r=0); this is a missing counter, not an "
                                 "idle component." << std::endl;
                    r = 0.0;
                };
                refuse("NoC", pgspec.pg_noc, pgspec.r_noc);
                refuse("device MC", pgspec.pg_mc, pgspec.r_mc);
            }
            std::cout << "  [pg] residencies (idle fraction): "
                      << (pgspec.pg_core ? ("pe=" + std::to_string(pgspec.r_core) + " ") : "pe=none(by design) ")
                      << (pgspec.have_shared_cache
                              ? ("shared$=" + std::to_string(pgspec.r_shared_cache) + " ")
                              : "shared$=none(no counter) ")   // 1.11.18
                      << (pgspec.pg_noc ? ("noc=" + std::to_string(pgspec.r_noc) + " ") : "noc=none(by design) ")
                      << (pgspec.pg_mc ? ("mc=" + std::to_string(pgspec.r_mc)) : "mc=none(by design)")
                      << "  [all-idle overlap="
                      << (1.0 - std::min(1.0, static_cast<double>(zsim_stats.pg_anycore_active) / ph))
                      << " -- shared-domain comparison]" << std::endl;
        }
    }

    // -- Device McPAT --
    McPAT::SystemConfig mcfg;
    mcfg.num_cores = config.num_pes;
    mcfg.core_clock_mhz = config.frequency_mhz;
    /* 1.11.51 (L68, closing the audit finding): on a DRAM-periphery
     * placement this node is NOT a free knob anymore -- it is the LOGIC-
     * EQUIVALENT baseline that McPAT prices at, and since 1.11.21 the family
     * factors are ratios AGAINST THIS SAME NODE's table (comm-dram @
     * generation table / baseline @ this node), which was DESIGNED to make
     * the baseline choice cancel. 1.11.51 extended that to the wrapper's XML
     * emission, which had still derived its factors at the table's own node
     * (L105). MEASURED 1.11.51 (the N2 check the audit demanded): the
     * cancellation is PARTIAL ONLY -- the same HBM3/BANK cell at 22 vs 45 nm
     * gives core area 11.67 vs 23.55 mm^2 (2.0x) and total power 0.195 vs
     * 1.726 W (8.8x), because McPAT's own node scaling is not the .dat l/C/V
     * ratios the factor divides by. So for a family-1 PE this knob remains a
     * LIVE, physically meaningless degree of freedom; pinning it to the
     * generation table (the N2 proposal) is a pending user ruling. For LOGIC
     * placements the knob is the real process choice, as it should be. */
    mcfg.tech_node_nm = validateTechNodeNm(config.tech_node_nm, "device PE node");
    mcfg.temperature_k = config.temperature_k;   // 1.11.52: was a literal 350
    if (config.temperature_k != 350)
        std::cout << "  [tech] temperature " << config.temperature_k
                  << " K (leakage rows snapped to the nearest 10 C step)"
                  << std::endl;

    /* 1.11.2: placement x technology -> PE process family. Every row is
     * anchored to shipped silicon:
     *   SUBARRAY..CHIP on a DRAM tech  -> DRAM_PERIPHERY (UPMEM DPU; DRISA)
     *   RANK/CHANNEL on a DRAM tech    -> LOGIC (DIMM/base-die buffer, AxDIMM)
     *   LOGIC_DIE                      -> LOGIC (HBM base die)
     *   any level on SRAM/NVM techs    -> LOGIC (bitcell density is the
     *       array model's problem; the PE beside the array is ordinary CMOS,
     *       eMRAM/ePCM being BEOL over a logic wafer)
     * HOST_MC PEs sit in the host controller: LOGIC by definition. */
    {
        bool dram_family_tech =
            !(config.memory_tech == "SRAM" || config.memory_tech == "STT_MRAM" ||
              config.memory_tech == "PCM"  || config.memory_tech == "RERAM");
        /* 1.11.17 (audit go-through): CHANNEL placement follows the LOCKED
         * per-tech ladder. DDR-class is rank-centric -- its channel tier is
         * a DIMM/buffer die, LOGIC. LPDDR/GDDR/HBM are channel-centric: the
         * channel tier lives ON the DRAM die (there is no buffer die to put
         * it on), so a channel-placed PE there is DRAM-periphery silicon.
         * Pricing only; the DQ/PHY predicates are separate claims. */
        bool channel_centric =
            config.memory_tech.rfind("LPDDR", 0) == 0 ||
            config.memory_tech.rfind("GDDR", 0) == 0 ||
            config.memory_tech.rfind("HBM", 0) == 0;
        bool on_dram_silicon = dram_family_tech &&
            ((config.pe_hierarchy_level >= 0 && config.pe_hierarchy_level <= 3) ||
             (config.pe_hierarchy_level == 5 && channel_centric));
        /* 1.11.16 (verification audit): the MCPHY question is PLACEMENT, not
         * process family. An on-die element MC (subarray..chip) drives no
         * off-chip DQ pins whatever the bitcell technology -- an SRAM or PCM
         * PIM at BANK placement was keeping the full off-chip PHY while its
         * termination term was already placement-gated to zero. The sibling
         * crosses_dq predicate uses the same placement test with no tech
         * condition; this now matches it.
         *
         * 1.11.19 (user decisions D2+D3): three tiers, not a boolean. The
         * driver depends on what the controller physically reaches:
         *   0..3 subarray..chip -> NONE       (on-die, no pins)
         *   5,6  channel/logic-die -> INTERPOSER (TSVs + microbumps): the
         *        HBM base die and the channel tier of channel-centric parts
         *        drive an interposer, which our termination model already
         *        prices at zero "by physics" and our link table at 0.5
         *        pJ/bit -- charging them an off-package DDR driver
         *        contradicted both in the same run.
         *   4,-1 rank / host MC -> OFFCHIP    (real DQ pins with ODT) */
        {
            using Tier = McPAT::SystemConfig::MCPhyTier;
            const int lvl = config.pe_hierarchy_level;
            bool interposer_tier =
                (lvl == 6) ||                       // LOGIC_DIE (HBM base die)
                (lvl == 5 && channel_centric);      // channel tier on LPDDR/GDDR/HBM
            if (lvl >= 0 && lvl <= 3)      mcfg.mc_phy_tier = Tier::NONE;
            else if (interposer_tier)      mcfg.mc_phy_tier = Tier::INTERPOSER;
            else                           mcfg.mc_phy_tier = Tier::OFFCHIP;
            std::cout << "  [tech] element MC interface: "
                      << (mcfg.mc_phy_tier == Tier::NONE ? "none (on-die, drives no pins)"
                          : mcfg.mc_phy_tier == Tier::INTERPOSER
                              ? "interposer/TSV (~0.5 pJ/bit, no ODT)"
                              : "off-package DQ (full PHY + ODT)")
                      << "; backend priced on the full-MC fit at every "
                         "placement (iso-model ladder)" << std::endl;
        }
        /* 1.11.51 (N1, raised by the user at E7): PITCH IS GEOMETRY, NOT
         * FAMILY. The pitch gating used to live inside the DRAM-periphery
         * branch, so a PE pressed against an SRAM/STT-MRAM/PCM/ReRAM
         * subarray could never be charged for on-pitch layout -- but the
         * array's pitch constrains the neighbouring logic whatever process
         * the transistors came from. The gate is now PLACEMENT only
         * (SUBARRAY); the family decides which process prices the result,
         * not whether geometry applies. 1.11.31's announce-the-drop rule
         * kept. */
        if (config.pe_hierarchy_level == 0) {
            mcfg.subarray_pitch_factor = config.subarray_pitch_factor;
            /* 1.11.51 (N3): DERIVED, BANDED default when the user set no
             * hypothesis. Low end = the only direct silicon measurement
             * (FIMDRAM, HBM2/1y, <=1.25 given our family area factor);
             * upper end ~4 = UPMEM's stated ~10x density penalty divided by
             * the family factor -- a general-purpose CPU with the 3-metal
             * routing loss included. Our PEs are lean elements, so the low
             * end is the default and the band is printed with it. Non-DRAM
             * arrays have no published PIM pitch datum: no default is
             * invented there. */
            if (config.subarray_pitch_factor == 1.0) {
                if (on_dram_silicon) {
                    DRAMGenClass g3 = getDRAMGenClass(config.memory_tech);
                    double F3 = dramGenFeatureNm(g3.cls);
                    mcfg.subarray_pitch_factor = 1.25;
                    std::cout << "  [tech] SUBARRAY pitch: derived default "
                                 "1.25 = the FIMDRAM silicon bound (ISSCC "
                                 "2021 25.4, lean SIMD end) of the published "
                                 "band [1.25, ~4] (upper end UPMEM-implied, "
                                 "HC31: general-purpose CPU, 3 metal layers); "
                                 "generation " << g3.cls
                              << " array pitch 2F ~= " << (2.0 * F3)
                              << " nm (6F^2). Settable: "
                                 "power.subarray_pitch_factor." << std::endl;
                } else {
                    std::cout << "  [tech] SUBARRAY pitch: no derived default "
                                 "for a non-DRAM array (no published PIM "
                                 "pitch datum); priced at 1.0 unless "
                                 "power.subarray_pitch_factor is set."
                              << std::endl;
                }
            }
        } else {
            mcfg.subarray_pitch_factor = 1.0;
            if (config.subarray_pitch_factor != 1.0) {
                std::cout << "  [tech] power.subarray_pitch_factor="
                          << config.subarray_pitch_factor
                          << " IGNORED at " << config.placement_level
                          << " placement: the pitch penalty applies only "
                             "where the PE sits in the array pitch "
                             "(SUBARRAY). Priced at 1.0." << std::endl;
            }
        }
        if (on_dram_silicon) {
            mcfg.process_family = 1;  // DRAM_PERIPHERY (per-class factors)
            DRAMGenClass gc = getDRAMGenClass(config.memory_tech);
            mcfg.dram_periph_table_nm = gc.cacti_table_nm;
            /* 1.11.51 (N2, user ruling 2026-08-17): an on-DRAM-die PE's
             * process is the DIE's OWN GENERATION -- a DRAM die has no logic
             * node to choose. MEASURED before ruling: the logic-node knob
             * moved the same cell by 2.0x area / 8.8x power between 22 and
             * 45 nm because the factor design only partially cancels it.
             * The knob remains the real process choice for OFF-die PEs
             * (buffer chip, MC die, HBM base die, host). */
            if (mcfg.tech_node_nm != gc.cacti_table_nm) {
                std::cout << "  [tech] on-DRAM-die PE: process pinned to the "
                             "die's generation (class " << gc.cls
                          << " -> CACTI " << gc.cacti_table_nm
                          << "nm table); technology.node_nm="
                          << mcfg.tech_node_nm
                          << " is a logic-die knob and is IGNORED at this "
                             "placement." << std::endl;
                mcfg.tech_node_nm = gc.cacti_table_nm;
            }
            std::cout << "  [tech] PE process family: DRAM-periphery (placement "
                      << config.placement_level << " on " << config.memory_tech
                      << ", class " << gc.cls << "); factors from CACTI "
                      << gc.cacti_table_nm << "nm hp/comm-dram columns"
                      << std::endl;
            /* 1.11.52: the periphery clock ceiling moved into
             * applyCornerAndPeripheryPricing() with the factors it belongs
             * to, so the per-node path gets it too (audit A003). */
        } else {
            mcfg.process_family = 0;  // LOGIC (native)
            std::cout << "  [tech] PE process family: logic ("
                      << validateTechNodeNm(config.tech_node_nm, "device PE node")
                      << " nm, placement " << config.placement_level << " on "
                      << config.memory_tech << ")" << std::endl;
        }
    }
    /* 1.9.32: this is the memory die, not the host processor. See
     * SystemConfig::device_scope -- it selects which of McPAT's two calibrated
     * die populations the element is priced against. */
    mcfg.device_scope = true;
    mcfg.pe_lanes       = config.pe_lanes;
    mcfg.pe_element_bits= config.alu_operand_width;   // ONE width, shared
    mcfg.pe_has_fp      = config.pe_has_fp;
    mcfg.fp_emul_cycles = (int)config.pe_fp_emul_cycles;   // 1.11.51 (L215/L223)
    mcfg.pe_imem_bytes  = config.pe_imem_bytes;

    // Derive McPAT architecture from pe_type
    if (config.pe_type == "ooo_core") {
        mcfg.pipeline_depth = 19;
        mcfg.issue_width = 4;
        mcfg.num_alus = 4;
        mcfg.num_muls = 2;
        mcfg.num_fpus = 2;
    } else if (config.pe_type == "alu_core" || config.pe_type == "alu") {
        mcfg.pipeline_depth = 5;
        mcfg.issue_width = 1;
        mcfg.num_alus = 1;
        mcfg.num_muls = 0;
        mcfg.num_fpus = 0;
    } else {
        // in_order_core (real in-order, zsim InOrderCore), simple_core (coarse)
        mcfg.pipeline_depth = 14;
        mcfg.issue_width = 1;
        mcfg.num_alus = 3;
        mcfg.num_muls = 1;
        mcfg.num_fpus = 1;
    }
    /* 1.11.51 (L214): pim.pe.floating_point=false must remove the FPU from
     * the POWER model on every profile, not just on ALU (whose default
     * happens to be zero). Before this, an FPU-less OOO or in-order element
     * still priced 2 or 1 FPUs in McPAT while its timing charged soft-float
     * emulation -- power for hardware the element declares it does not
     * have. An explicit power.mcpat_overrides.num_fpus below still wins. */
    if (!config.pe_has_fp && mcfg.num_fpus > 0) {
        std::cout << "  [power] pim.pe.floating_point=false: FPU removed from "
                     "the power model (profile default num_fpus="
                  << mcfg.num_fpus << " -> 0)." << std::endl;
        mcfg.num_fpus = 0;
    }

    // Apply mcpat_overrides as escape hatch for architecture-level knobs
    auto ov_get_int = [&overrides](const std::string& key, int fallback) -> int {
        auto it = overrides.find(key);
        return (it != overrides.end()) ? static_cast<int>(it->second) : fallback;
    };
    mcfg.pipeline_depth = ov_get_int("pipeline_depth", mcfg.pipeline_depth);
    mcfg.issue_width = ov_get_int("issue_width", mcfg.issue_width);
    mcfg.num_alus = ov_get_int("num_alus", mcfg.num_alus);
    mcfg.num_muls = ov_get_int("num_muls", mcfg.num_muls);
    mcfg.num_fpus = ov_get_int("num_fpus", mcfg.num_fpus);
    /* 1.11.13/1.11.21/1.11.35, now shared: corner selection, DRAM-periphery
     * factors read from the tables, uncertainty band, and the calculated
     * clock ceiling. 1.11.52 moved the body into ONE OWNER both scopes call
     * (audit A001-A003). */
    applyCornerAndPeripheryPricing(mcfg, config, overrides,
                                   getDRAMGenClass(config.memory_tech),
                                   config.frequency_mhz, "");
    mcfg.longer_channel_device = ov_get_int("longer_channel_device", 1);
    mcfg.number_hardware_threads = ov_get_int("number_hardware_threads", 1);
    mcfg.interconnect_projection_type =
        ov_get_int("interconnect_projection_type", config.interconnect_projection);  // 1.11.30 E5

    bool alu_only = (config.pe_type == "alu_core" || config.pe_type == "alu");
    if (alu_only) {
        mcfg.l1i_size_bytes = 0;
        mcfg.l1d_size_bytes = 0;
        mcfg.l2_size_bytes = 0;
        mcfg.l3_size_bytes = 0;
    } else {
        mcfg.l1i_size_bytes = config.l1i_size_kb * 1024ULL;
        mcfg.l1d_size_bytes = config.l1d_size_kb * 1024ULL;
        mcfg.l2_size_bytes = config.enable_l2 ? config.l2_size_kb * 1024ULL : 0;
        mcfg.l3_size_bytes = config.enable_l3 ? config.l3_size_kb * 1024ULL : 0;
    }

    /* 1.10.5: a controller per REGION, not per group of elements.
     *
     * num_pes / pes_per_mc counts controllers by how the elements were grouped,
     * which is a statement about the elements and not about the memory. What a
     * controller actually serves is a region: the memory behind one endpoint of
     * the placement tree. An element's own region is one such endpoint; a
     * region with no element in it is an aggregated endpoint, and it needs a
     * controller too -- memory that only ever responds still has to be
     * sequenced, refreshed and its bus turned around.
     *
     * That is also why an aggregated endpoint is a controller and not an
     * injecting network interface: it never issues traffic of its own. Counting
     * it as an injector would give the fabric a source that does not exist.
     *
     * The endpoint count comes from the tree that was actually built. Where no
     * tree exists (non-DRAM, or hierarchy disabled) the old grouping is the
     * only information available, so it stands. */
    if (config.htree_endpoints > 0) {
        mcfg.num_memory_controllers = config.htree_endpoints;
    } else if (config.pe_mc_enabled && config.pes_per_mc > 0) {
        /* 1.11.57 (audit round 3, A004): CLAMP, as the per-node twin does.
         * Integer division gives 0 whenever pes_per_mc exceeds num_pes -- one
         * PE with pim.mc.pes_per_mc: 2 is enough -- and a McPAT run with zero
         * memory controllers reports no MC dynamic and no MC leakage at all,
         * while the timing model still instantiates one. Since C032 made the
         * system total follow the described-component predicate, a zero here
         * deletes the controller from System Total Power, not merely from the
         * breakdown. */
        mcfg.num_memory_controllers =
            std::max(1, config.num_pes / config.pes_per_mc);
    } else {
        mcfg.num_memory_controllers = 1;
    }
    mcfg.mc_clock_mhz = config.frequency_mhz / 2.0;
    // The in-memory NoC (DRAM datapath hierarchy) is a property of the memory
    // organization, not the PE count: it exists whenever the hierarchy is enabled,
    // even for a single PE. Gating on num_pes>1 alone dropped the ~4.6W NoC leakage
    // for 1-PE device/hierarchy runs, collapsing their power to core+MC (~0.1W).
    mcfg.has_noc = (config.num_pes > 1 || config.hierarchy_enabled);
    mcfg.noc_clock_mhz = config.frequency_mhz;
    /* 1.10.5: the fabric priced here is the one the timing model routes on.
     *
     * It was one router per element on a ceil(sqrt(elements)) square grid --
     * a mesh, of a size derived from the element count. The device has no such
     * thing. It has the placement tree: sparse, as deep as the hierarchy the
     * elements sit in, with a router count that follows elements x depth rather
     * than elements, and no square anywhere in it. So the two halves of the
     * simulator were describing different machines, and the power half was
     * describing one that is not built.
     *
     * Only BRANCH routers are charged. A single-child router in the tree is a
     * pass-through: signal enters, signal leaves, nothing is arbitrated. Its
     * cost is wire, already carried by the link, and charging it a crossbar and
     * an allocator would bill logic that is not there -- on a coarse placement,
     * for most of the tree.
     *
     * Rows and columns describe a mesh, and a tree is not one. Reported as a
     * single row of branch routers rather than a square, so the geometry claims
     * only what is known: how many arbitrating nodes there are. */
    if (config.htree_branch_routers >= 0) {
        int branch = config.htree_branch_routers > 0 ? config.htree_branch_routers : 1;
        mcfg.noc_num_routers = branch;
        mcfg.noc_num_rows = 1;
        mcfg.noc_num_cols = branch;
    } else {
        mcfg.noc_num_routers = config.num_pes;
        int grid_side = static_cast<int>(std::ceil(std::sqrt(config.num_pes)));
        mcfg.noc_num_rows = grid_side;
        mcfg.noc_num_cols = grid_side;
    }

    McPAT mcpat(mcfg);
    mcpat.setPGSpec(pgspec);   // 1.11.8

    /* Device profile.
     * 1.9.37: the out-of-order case was MISSING here. This path had only
     * "compute unit or in-order", so an element declared ooo_core was described
     * as in-order -- no reorder buffer, no instruction window, no renaming --
     * while zsim simulated a real out-of-order core. The per-node path
     * (runPerNodePowerAnalysis) already selected an out-of-order profile
     * correctly; only this device-scope path did not, which is why the defect
     * showed on device-scope cells and not on co-simulation ones. */
    if (alu_only)
        mcpat.setDeviceProfile(McPAT::DeviceProfile::DEVICE_ALU);
    else if (config.pe_type == "ooo_core")
        mcpat.setDeviceProfile(McPAT::DeviceProfile::OOO);
    else
        mcpat.setDeviceProfile(McPAT::DeviceProfile::DEVICE_INORDER);

    mcpat.initialize();

    // Feed simulation stats
    // OOO cores in QEMU mode may report cycles=0 (contention sim not triggered);
    // estimate cycles from instrs assuming IPC~=1 so McPAT gets reasonable values
    uint64_t cycles = zsim_stats.cycles;
    uint64_t instrs = zsim_stats.instrs > 0 ? zsim_stats.instrs : 1;
    if (cycles == 0 && instrs > 1) {
        /* 1.11.48 (FIX-PRE-FLEET L220): 1.11.9 made `instrs` the ALL-CORE
         * SUM; cycles are a per-core DURATION, so IPC=1 estimated from the
         * sum inflated the duration by the PE count (a 16-PE cell claimed a
         * 16x longer run). Per-core IPC=1 is instrs / cores. */
        cycles = instrs / std::max(1, config.num_pes > 0 ? config.num_pes : 1);
        if (cycles == 0) cycles = 1;
        std::cout << "  Note: cycles=0 (OOO contention sim not triggered), "
                  << "estimating cycles=" << cycles << " from instrs/core "
                  << "(IPC=1 per core; instrs is the all-core sum since 1.11.9)"
                  << std::endl;
    }
    if (cycles == 0) cycles = 1;
    mcpat.setTotalCycles(cycles);
    mcpat.setBusyCycles(cycles);
    /* 1.9.33: subtract injected timing charges -- see the parser note. */
    if (zsim_stats.syntheticInstrs > 0 && instrs > zsim_stats.syntheticInstrs)
        instrs -= zsim_stats.syntheticInstrs;
    mcpat.setTotalInstructions(instrs);
    /* 1.9.29: measured instruction mix on the DEVICE-SCOPE path too.
     *
     * 1.9.28 wired the measured micro-op, basic-block and mispredict counters
     * into the system-scope paths and left this one supplying only the count,
     * so the mix here was still the invented 70% integer / 10% floating-point
     * / 10% branch / 1% mispredict applied identically to every workload. This
     * is the path behind the device-only sweeps, so that assumed mix reached
     * far more cells than the co-simulation one did.
     *
     * The all-nodes totals ARE this node's totals here: device scope simulates
     * a single node. Zero counters (an ALU processing element reports no
     * micro-ops) fall back to the fractions inside the wrapper, which is the
     * honest choice when the core model genuinely does not track them. */
    mcpat.setMeasuredCoreActivity(zsim_stats.uops, zsim_stats.branches,
                                  zsim_stats.mispredBranches);
    mcpat.setMeasuredMix(zsim_stats.mix_int, zsim_stats.mix_mul,
                         zsim_stats.mix_fp, zsim_stats.mix_ld, zsim_stats.mix_st,
                         zsim_stats.mix_br);  // 1.11.10/.15

    // Split cache stats from ZSim
    mcpat.setL1IAccesses(zsim_stats.l1i_total_reads(), zsim_stats.l1i_mGETS);
    mcpat.setL1DAccesses(zsim_stats.l1d_total_reads(), zsim_stats.l1d_total_writes(),
                         zsim_stats.l1d_mGETS, zsim_stats.l1d_mGETXIM);
    mcpat.setL2Accesses(zsim_stats.l2_total_reads(), zsim_stats.l2_total_writes(),
                        zsim_stats.l2_mGETS, zsim_stats.l2_mGETXIM);
    mcpat.setL3Accesses(zsim_stats.l3_total_reads(), zsim_stats.l3_total_writes(),
                        zsim_stats.l3_mGETS, zsim_stats.l3_mGETXIM);

    // MC stats from ZSim
    mcpat.setMemControllerAccesses(zsim_stats.mem_rd, zsim_stats.mem_wr);

    // MC technology params
    McPAT::MCTechParams mc_tech = getMCTechParamsForMcPAT(config.memory_tech, 1);
    auto it_ptr = overrides.find("mc.peak_transfer_rate");
    if (it_ptr != overrides.end()) mc_tech.peak_transfer_rate = static_cast<int>(it_ptr->second);
    auto it_dbw = overrides.find("mc.databus_width");
    if (it_dbw != overrides.end()) mc_tech.databus_width = static_cast<int>(it_dbw->second);
    auto it_nr = overrides.find("mc.number_ranks");
    if (it_nr != overrides.end()) mc_tech.number_ranks = static_cast<int>(it_nr->second);
    mcpat.setMCTechParams(mc_tech);

    // Garnet stats
    std::string garnet_path = output_dir + "/garnet_stats.txt";
    GarnetParsedStats garnet = parseGarnetStatsFile(garnet_path);

    // Build per-level NoC configs. Include the single-PE hierarchy case: the
    // in-memory network spans all memory-org levels regardless of PE count.
    if (config.num_pes > 1 || config.hierarchy_enabled) {
        auto noc_levels = buildNoCLevelsForMcPAT(config, garnet, cycles, overrides);
        mcpat.setNoCLevels(noc_levels);
    }

    // Report detail level
    bool detail_verbose = (config.power_report_detail == "verbose");
    bool detail_summary = (config.power_report_detail == "summary");
    // detail_standard = !detail_verbose && !detail_summary

    // -- Print derived-parameter transparency block (verbose only) --
    if (detail_verbose) {
        /* 1.11.56 (audit A025): stop labelling this an applied override.
         * pipeline_duty_cycle is a function-local that nothing downstream
         * reads -- the wrapper recomputes it as busy_cycles_/total_cycles_ and
         * writes THAT to the XML, and this path sets both to `cycles`, so the
         * value McPAT sees is 1.0 whatever the user writes. Printing the
         * user's number under "[OVERRIDE from YAML]" claimed an effect that
         * does not exist. */
        double pipeline_duty_cycle = static_cast<double>(cycles) / cycles;  // always 1.0 (busy=total)
        auto it_pdc = overrides.find("core.pipeline_duty_cycle");

        std::cout << "\nMcPAT Derived Inputs:" << std::endl;
        std::cout << "  core.pipeline_duty_cycle    = busy_cycles / total_cycles = "
                  << cycles << " / " << cycles << " = " << pipeline_duty_cycle << std::endl;
        if (it_pdc != overrides.end())
            std::cout << "    NOTE: power.mcpat_overrides.core.pipeline_duty_cycle = "
                      << it_pdc->second << " was parsed but is NOT applied. McPAT "
                         "derives this ratio itself from the cycle counts it is "
                         "given, and this path reports the element as busy for "
                         "every simulated cycle, so the ratio is 1.0 by "
                         "construction. Remove the key or model the idle time."
                      << std::endl;

        std::cout << "  mc.peak_transfer_rate       = " << mc_tech.peak_transfer_rate << " MT/s"
                  << (it_ptr != overrides.end() ? "  [OVERRIDE from YAML]" : ("  [from " + config.memory_tech + " spec]"))
                  << std::endl;
        std::cout << "  mc.databus_width            = " << mc_tech.databus_width << " bits"
                  << (it_dbw != overrides.end() ? "  [OVERRIDE from YAML]" : ("  [from " + config.memory_tech + " spec]"))
                  << std::endl;
        std::cout << "  mc.number_ranks             = " << mc_tech.number_ranks
                  << (it_nr != overrides.end() ? "  [OVERRIDE from YAML]" : ("  [from " + config.memory_tech + " spec]"))
                  << std::endl;
        std::cout << "  mc.memory_reads             = " << zsim_stats.mem_rd << std::endl;
        std::cout << "  mc.memory_writes            = " << zsim_stats.mem_wr << std::endl;

        std::cout << "  pcie.total_load_perc        = 0.000  [no co-sim transfers]" << std::endl;
    }

    // -- Run device McPAT --
    try {
        mcpat.computePower();
    } catch (const std::exception& e) {
        /* 1.11.52 (gate 1162C C4): POWER WAS ASKED FOR AND NOT PRODUCED.
         * This used to print two lines and return, leaving the run to exit
         * 0 with a report that simply has no power in it -- a fleet cell
         * would land in the corpus looking complete. A tool failure on a
         * requested analysis is a failed run. */
        std::cerr << "\n[Power] McPAT failed: " << e.what() << std::endl;
        std::cerr << "[Power] FATAL: power analysis was requested (--power) "
                     "and produced nothing. Refusing to report a run with a "
                     "silently missing power result." << std::endl;
        std::exit(3);
    }

    // -- Print results (gated by report_detail) --
    // Summary: one-line totals only
    // Standard: component breakdown + memory array + coverage table
    // Verbose: all of standard + derived inputs (above) + per-component area + XML dump
    if (detail_summary) {
        mcpat.printSummaryLine();
    } else {
        mcpat.printDetailedResults();

        // Per-level NoC power breakdown
        const auto& noc_level_power = mcpat.getNoCLevelPower();
        if (noc_level_power.size() > 1) {
            std::cout << "NoC power spans " << noc_level_power.size()
                      << " hierarchy levels (see breakdown above)." << std::endl;
        }

        // Verbose: per-component area table
        if (detail_verbose) {
            std::cout << "\nPer-Component Area Breakdown:" << std::endl;
            std::cout << "  Cores:              " << std::fixed << std::setprecision(3)
                      << mcpat.getComponentArea(McPAT::ComponentType::CORE) << " mm^2" << std::endl;
            std::cout << "  L2 Caches:          "
                      << mcpat.getComponentArea(McPAT::ComponentType::L2_CACHE) << " mm^2" << std::endl;
            std::cout << "  L3 Cache:           "
                      << mcpat.getComponentArea(McPAT::ComponentType::L3_CACHE) << " mm^2" << std::endl;
            std::cout << "  Memory Controllers: "
                      << mcpat.getComponentArea(McPAT::ComponentType::MEMORY_CONTROLLER) << " mm^2" << std::endl;
            std::cout << "  NoC:                "
                      << mcpat.getComponentArea(McPAT::ComponentType::NOC) << " mm^2" << std::endl;
            std::cout << "  Total:              "
                      << mcpat.getTotalArea() << " mm^2" << std::defaultfloat << std::endl;

            /* Dump the priced McPAT XML for post-analysis.
             *
             * 1.11.56 (audit C014): ask the wrapper WHICH file it wrote. This
             * read a fixed /tmp/mcpat_input.xml, a path createMcPATInput() has
             * not written since the per-instance index was added -- so the
             * copy silently found nothing and NO run has archived the XML it
             * was actually priced from. A missing archive is now reported
             * rather than skipped, because the whole point of the file is to
             * make the priced inputs auditable after the fact. */
            if (!output_dir.empty()) {
                std::string xml_src = mcpat.lastInputXmlPath();
                std::string xml_dst = output_dir + "/mcpat_config.xml";
                std::ifstream src(xml_src);
                if (src.good()) {
                    std::ofstream dst(xml_dst);
                    dst << src.rdbuf();
                    std::cout << "\nMcPAT XML saved to: " << xml_dst << std::endl;
                } else {
                    std::cerr << "[power] WARNING: the McPAT input XML ("
                              << (xml_src.empty() ? std::string("<no path recorded>")
                                                  : xml_src)
                              << ") could not be read, so this run archives no "
                                 "record of the inputs its power numbers came "
                                 "from." << std::endl;
                }
            }
        }
    }

    // -- Host McPAT for system scope with host nodes (standard/verbose) --
    bool has_host_node = false;
    for (const auto& n : config.system_nodes) {
        if (n.role == UnifiedConfig::SystemNode::HOST) { has_host_node = true; break; }
    }
    if (!detail_summary && config.scope == "system" && has_host_node) {
        std::cout << "\n--- Host Power (System) ---" << std::endl;

        /* 1.11.57 (audit round 3, A005): price the host node that EXISTS.
         *
         * This block synced num_cores from the parsed HOST system node and
         * then read the clock, all four cache sizes, the memory technology
         * and the process node from the LEGACY top-level `host:` block --
         * which an explicit `system:` YAML never writes (synthesizeSystemNodes
         * returns at its first line when system nodes are already present).
         * On the shipped host_device_basic.yaml under --method trace that
         * meant a 3000 MHz clock against the node's 2000, a 1024 KB L2
         * against its 256, a phantom 8 MB L3, and DDR4 controller parameters
         * for a DDR5 host. The node's own values win where a HOST node
         * exists; the legacy fields remain the fallback for the old
         * single-host shape. */
        const UnifiedConfig::SystemNode* host_node = nullptr;
        for (const auto& n : config.system_nodes) {
            if (n.role == UnifiedConfig::SystemNode::HOST) { host_node = &n; break; }
        }
        const double host_clk_mhz =
            (host_node && host_node->frequency_mhz > 0.0) ? host_node->frequency_mhz
                                                          : config.host_frequency_mhz;

        McPAT::SystemConfig host_cfg;
        host_cfg.num_cores = config.host_num_cores;
        host_cfg.core_clock_mhz = host_clk_mhz;
        host_cfg.pipeline_depth = 19;
        host_cfg.issue_width = 4;
        host_cfg.num_alus = 4;
        host_cfg.num_muls = 2;
        host_cfg.num_fpus = 2;
        // 1.9.32: the host IS a server part -- stated, not left to the default.
        host_cfg.device_scope = false;
        /* 1.11.49 (FIX-PRE-FLEET L119): power.device_corner never reached the
         * dual-McPAT HOST -- the exact case 1.11.13 says the knob exists for
         * (pricing the host at a different corner than the device). The host
         * is a LOGIC part, so no DRAM-periphery refusal applies; the corner
         * maps directly. */
        host_cfg.device_type = (config.device_corner == "lstp") ? 1
                             : (config.device_corner == "lop")  ? 2 : 0;
        // 1.11.57 (A005): the node's caches where a HOST node exists.
        host_cfg.l1i_size_bytes = (host_node ? host_node->l1i_kb : config.host_l1i_kb) * 1024;
        host_cfg.l1d_size_bytes = (host_node ? host_node->l1d_kb : config.host_l1d_kb) * 1024;
        host_cfg.l2_size_bytes  = (host_node ? host_node->l2_kb  : config.host_l2_kb)  * 1024;
        host_cfg.l3_size_bytes  = (host_node ? host_node->l3_kb  : config.host_l3_kb)  * 1024;
        host_cfg.num_memory_controllers = 1;
        /* 1.11.52 (audit A011): DERIVED, like every other MC clock in this
         * file (mcfg.mc_clock_mhz = frequency/2 at both other sites). The
         * literal 1200 had no source and no print, so a host at any clock
         * was priced with a 1.2 GHz controller. */
        host_cfg.mc_clock_mhz = (host_clk_mhz > 0.0 ? host_clk_mhz
                                                    : config.frequency_mhz) / 2.0;
        host_cfg.has_noc = false;
        /* 1.11.57 (audit round 3, A006): state the metal stack.
         *
         * This was the one McPAT site that never set it, so it took the
         * enum's 0 = AGGRESSIVE -- the projection the config validator itself
         * tells users no process can build -- applied by omission, at the one
         * site with no override path. Every other McPAT site in this file
         * passes config.interconnect_projection. */
        host_cfg.interconnect_projection_type = config.interconnect_projection;
        // Host process node: resolved host override, else the device node (uniform).
        // std::max(22,...) is the CACTI/McPAT lower-bound floor (models below 22nm are
        // not supported by the linked CACTI), not a study default.
        {
            // 1.11.57 (A005): the host node's own process node, where it has one.
            int host_tn = (config.host_tech_node_nm >= 0)
                ? config.host_tech_node_nm
                : (host_node && host_node->tech_node_nm > 0) ? host_node->tech_node_nm
                                                             : config.tech_node_nm;
            host_cfg.tech_node_nm = validateTechNodeNm(host_tn, "host node");
        }
        host_cfg.temperature_k = config.temperature_k;   // 1.11.52

        McPAT host_mcpat(host_cfg);
        host_mcpat.setDeviceProfile(McPAT::DeviceProfile::OOO);
        host_mcpat.initialize();

        /* 1.9.29: price the host from ITS OWN measured counters.
         *
         * This block is reached whenever scope == "system", i.e. exactly when a
         * co-simulation dump DOES carry measured host stats -- and it ignored
         * them, deriving all nine inputs from fixed divisors of the DEVICE's
         * instruction count: busy = cycles/10 ("host mostly waiting"),
         * instructions = instrs/10, L1I = instrs/10, L1D = instrs/20, L2 =
         * instrs/200, L3 = instrs/2000, MC = instrs/20000. None of those ratios
         * was measured, and the comment "will be overridden by real stats when
         * available" described an override that did not exist. The estimate is
         * kept only for the case where no host counters were parsed, and it now
         * says so in the output rather than presenting itself as measurement. */
        const auto& hgrp = zsim_stats.host;
        const bool host_measured = hgrp.has_activity();
        if (host_measured) {
            uint64_t host_cycles = (zsim_stats.host_wall_cycles > 0)
                                 ? zsim_stats.host_wall_cycles : cycles;
            host_mcpat.setTotalCycles(host_cycles);
            host_mcpat.setBusyCycles(host_cycles);
            host_mcpat.setTotalInstructions(hgrp.real_instrs());   // 1.9.33
            host_mcpat.setL1IAccesses(hgrp.l1i_total_reads(), hgrp.l1i_mGETS);
            host_mcpat.setL1DAccesses(hgrp.l1d_total_reads(), hgrp.l1d_total_writes(),
                                      hgrp.l1d_mGETS, hgrp.l1d_mGETXIM);
            host_mcpat.setL2Accesses(hgrp.l2_total_reads(), hgrp.l2_total_writes(),
                                     hgrp.l2_mGETS, hgrp.l2_mGETXIM);
            host_mcpat.setL3Accesses(hgrp.l3_total_reads(), hgrp.l3_total_writes(),
                                     hgrp.l3_mGETS, hgrp.l3_mGETXIM);
            host_mcpat.setMemControllerAccesses(hgrp.mem_rd, hgrp.mem_wr);
            host_mcpat.setMeasuredCoreActivity(hgrp.uops, hgrp.branches,
                                               hgrp.mispredBranches);
            host_mcpat.setMeasuredMix(hgrp.mix_int, hgrp.mix_mul, hgrp.mix_fp,
                                      hgrp.mix_ld, hgrp.mix_st, hgrp.mix_br);  // 1.11.10/.15
            /* 1.11.17: print the number McPAT is PRICED on (real_instrs =
             * retired minus injected timing charges), not the raw counter --
             * in an offload ROI the two differ by the whole charge. */
            std::cout << "  [Activity] host: measured -- instrs=" << hgrp.real_instrs()
                      << " (raw " << hgrp.instrs << ", synth "
                      << hgrp.syntheticInstrs << ")"
                      << " cycles=" << host_cycles
                      << " uops=" << hgrp.uops
                      << " l1d=" << (hgrp.l1d_total_reads() + hgrp.l1d_total_writes())
                      << " mem=" << (hgrp.mem_rd + hgrp.mem_wr) << std::endl;
        } else {
            /* 1.11.48 (FIX-PRE-FLEET L250, under the E26 ruling: "no
             * measurements should never happen"). This branch synthesised a
             * host from NINE fixed divisors of the DEVICE's instruction
             * count -- busy=cycles/10, instrs/10, L1I=instrs/10 ... MC=
             * instrs/20000 -- none measured, and 1.11.9's instrs-summing
             * silently rebased every one of them by the PE count. A
             * system-scope dump with no host counters is an INVALID INPUT;
             * pricing an invented host hides that inside a result. */
            std::cerr << "ERROR: system-scope dump carries no measured host "
                         "counters; refusing to synthesise a host from fixed "
                         "divisors of the device instruction count. Fix the "
                         "input; do not trust this run." << std::endl;
            exit(1);
        }
        /* 1.11.57 (audit round 3, A005): the HOST NODE's memory technology.
         * config.host_memory_tech is the legacy top-level field an explicit
         * system: config never writes, so a DDR5 host was priced with DDR4
         * controller parameters. */
        {
            const std::string& hmt =
                (host_node && !host_node->memory_tech.empty()) ? host_node->memory_tech
                                                               : config.host_memory_tech;
            host_mcpat.setMCTechParams(getMCTechParamsForMcPAT(hmt, 1));
        }

        // PCIe for host-device transfers (configurable via power.pcie YAML)
        if (config.pcie_enabled) {
            McPAT::PCIeStats pcie;
            pcie.number_units = config.pcie_num_units;
            // Use num_lanes for num_channels if available
            pcie.num_channels = (config.pcie_num_lanes > 0) ? config.pcie_num_lanes : config.pcie_num_channels;
            pcie.duty_cycle = config.pcie_duty_cycle;
            /* 1.11.56 (audit B015): the link controller's load fraction is a
             * MEASUREMENT on the per-node path (measured crossing bytes over
             * the link's capacity for the run), and this path -- the
             * system-scope trace path and the per-node fallback -- still took
             * the 0.01 default. Nothing measured 1%: it is a placeholder that
             * has been scaling host link-controller dynamic power ever since.
             * There are no transfer statistics here, so the honest value is
             * ZERO load with a stated reason, not an invented one percent. A
             * user who knows the figure can still set it. */
            if (config.pcie_load_perc_user_set) {
                pcie.total_load_perc = config.pcie_load_perc;
            } else {
                pcie.total_load_perc = 0.0;
                std::cout << "  [power] host link controller: no measured "
                             "crossing traffic on this path, so its load "
                             "fraction is 0. The controller's LEAKAGE is still "
                             "priced; its dynamic term is not invented. Set "
                             "power.pcie.total_load_perc to supply one."
                          << std::endl;
            }
            /* 1.11.56 (audit C017): give the host link the same clock and
             * name the per-node path derives. Without them the wrapper fell
             * back to an unsourced 350 MHz controller clock and to the generic
             * "pcie" link class (McPAT's 4 Gb/s SerDes), so the host link
             * controller's dynamic power was priced for a different link than
             * the one the run configured. */
            {
                double clk = PIMID::CactiIOWrapper::linkControllerClockMHz(
                                 config.pcie_link_type);
                if (clk > 0.0) {
                    pcie.link_clock_mhz = static_cast<int>(clk + 0.5);
                } else {
                    pcie.link_clock_mhz = 1000;
                    std::cerr << "[power] WARNING: no sourced controller clock "
                                 "for host link type '" << config.pcie_link_type
                              << "' (non-PIPE family); using 1000 MHz ASSUMED -- "
                                 "controller dynamic scales with it." << std::endl;
                }
            }
            pcie.link_type_name = config.pcie_link_type;
            host_mcpat.setPCIeStats(pcie);
        }

        try {
            host_mcpat.computePower();
            host_mcpat.printDetailedResults();

            auto dev_power = mcpat.getSystemPower();
            auto host_power = host_mcpat.getSystemPower();
            std::cout << "\n--- System Power Summary ---" << std::endl;
            std::cout << "  Host Power:     " << host_power.total_power << " W" << std::endl;
            std::cout << "  Device Power:   " << dev_power.total_power << " W" << std::endl;
            std::cout << "  Total System:   " << (host_power.total_power + dev_power.total_power) << " W" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[Power] Host McPAT failed: " << e.what() << std::endl;
        }
    }

    // -- Memory Array Power/Area (standard/verbose) --
    bool is_dram_tech = !(config.memory_tech == "SRAM" || config.memory_tech == "STT_MRAM" ||
                          config.memory_tech == "PCM" || config.memory_tech == "RERAM");

    if (!detail_summary) {
    std::cout << "\n--- Memory Array Power/Area ---" << std::endl;

    if (is_dram_tech) {
        // DRAM: Ramulator2 energy model
        try {
            pimid::RamulatorWrapper ram_oracle("", config.memory_tech);
            ram_oracle.setDeviceWidth(config.dram_device_width);   // 1.11.46 (L181)
            /* 1.11.63 (R7): gate 1173B E3 caught this half-wired -- the key
             * was parsed into config but the setter was never called, so the
             * knob was still dead at the point of use. Both scopes apply it
             * now, before initialize(). */
            ram_oracle.setTerminationOverridePJPerBit(config.termination_pj_per_bit);
            /* 1.11.52 (audit D003): the array's activate/precharge share is
             * now weighted by the run's OWN measured row-buffer miss rate
             * (PE-MI rowHits/rowMisses) instead of a hardcoded 0.5. It is
             * the dominant term in DRAM access energy, so a coin flip there
             * was worth roughly +/-40% on per-access read energy. */
            {
                const double rmf = zsim_stats.rowMissFraction();
                ram_oracle.setRowMissFraction(rmf);
                if (rmf >= 0.0)
                    std::cout << "  [mem] row-buffer miss fraction MEASURED "
                              << rmf << " (" << zsim_stats.row_misses << " of "
                              << (zsim_stats.row_hits + zsim_stats.row_misses)
                              << " accesses opened a new row); it weights the "
                                 "activate/precharge share of array energy"
                              << std::endl;
                else
                    std::cout << "  [mem] row-buffer miss fraction NOT MEASURED "
                                 "in this run (no PE-MI row counters); array "
                                 "energy uses the stated 0.5 fallback for the "
                                 "activate/precharge share" << std::endl;
            }
            ram_oracle.initialize();
            // 1.9.10: use the INTENSIVE per-access accessors (the older
            // extensive pair returned 0 on a fresh oracle). getArrayReadEnergyNJ()
            // folds activation + column access AND the DQ burst current --
            // JEDEC IDD4R is measured with the outputs driving.
            //
            // 1.11.5 (audit): the old iface term was bit-identical to that DQ
            // burst current, so every access was double-charged; and it was
            // charged for ALL placements under a comment claiming host-side
            // only. The interface term is now TERMINATION (ODT) -- the
            // genuinely additional off-chip energy, previously computed and
            // never charged -- and it is placement-aware: an access from an
            // on-die PE (subarray..chip) never crosses the DQ pins, so it
            // carries no termination at all. HBM terminates nothing at any
            // placement (interposer microbumps; the model returns 0 there by
            // physics, not by this gate).
            double rd_energy = ram_oracle.getArrayReadEnergyNJ();
            double wr_energy = ram_oracle.getArrayWriteEnergyNJ();
            bool crosses_dq = crossesOffPackageDQ(config.pe_hierarchy_level,
                                                  config.memory_tech);  // 1.11.52 (A018)
            /* 1.11.58: the DQ interface is DRIVER SWITCHING + PHY + TERMINATION.
             *
             * PIMID has charged termination alone since it had an interface at
             * all. 1.11.40 modelled the other two terms and left the accessor
             * without a caller, so the correction sat in the code and reached
             * no reported number -- the audit found it with no caller anywhere
             * in src/ or include/. Termination is the SMALL term on a modern
             * bus: LVSTL exists precisely to make it negligible, which is why
             * leaving driver and PHY out understates LPDDR5's interface by
             * roughly two orders of magnitude. Both halves are charged now,
             * and reported separately so the split is visible rather than
             * folded into one number. */
            /* 1.11.60 (audit round 4, A009/A010 -- REVERT of 1.11.58's energy
             * wiring, user-approved).
             *
             * 1.11.58 added CACTI-IO's driver+PHY term to the charged
             * interface energy, believing it closed an omission. Round 4
             * showed it is a DOUBLE-COUNT: the term is charged only where
             * crosses_dq is true (RANK placement and above), and those are
             * exactly the placements where McPAT is already told withPHY=1 --
             * the controller-side PHY is already priced in device silicon.
             * One bit crossing, billed twice; the 4.2x rise the 1168 gates
             * measured was the overlap, not a correction. And A010: the PHY
             * static power inside that term is amortized at 100% bus
             * utilization and charged as per-access DYNAMIC energy, so it was
             * also the wrong kind of number. The charged interface is
             * termination-only again -- the pre-1.11.58 basis the corpus was
             * built on -- and CACTI-IO's figure prints as an INFORMATIONAL
             * line below, summed into nothing. */
            /* 1.11.63 (R7, user ruling): READ/WRITE SPLIT. Reads price the
             * DRAM driver into the RX termination (RTT_NOM class); writes
             * price the controller driver into the DRAM's RTT_WR class --
             * a deliberately stronger setting in every DDR family, so the
             * write crossing costs more than the read crossing. */
            const double iface_term_rd_nj = crosses_dq ? ram_oracle.getTerminationEnergyNJ(false) : 0.0;
            const double iface_term_wr_nj = crosses_dq ? ram_oracle.getTerminationEnergyNJ(true)  : 0.0;
            const double iface_drv_nj  = crosses_dq ? ram_oracle.getInterfaceDynamicEnergyNJ() : 0.0;
            /* 1.11.8, corrected 1.11.56 (audit A028): with memory.power_down
             * the idle controller descends the DRAM into precharge power-down
             * (IDD2P) during measured no-traffic residency; refresh always
             * continues. Without the flag (or with zero phases) this is
             * exactly getBackgroundPowerMW(). The flag named here used to be
             * pim.mc.pg, which 1.11.45 split off into a separate knob that
             * gates the CONTROLLER's own logic -- this branch has never read
             * it, so anyone following the old text changed the wrong key. */
            /* 1.11.20 (D15): the idle residency is a MEASUREMENT, and it
             * applies with or without pim.mc.pg. An idle controller closes
             * its pages either way, so an idle device sits at PRECHARGE
             * standby (IDD2N); pg only decides whether the descent continues
             * to power-down (IDD2P). Hence no config.pg_mc in this condition
             * -- that gate is what kept the baseline knowingly at IDD3N.
             *
             * UNARMED-COUNTER REFUSAL: pgDevMCActivePhases is touched by
             * every non-PUTS request through the Ramulator MC and the PE-MI
             * (ramulator_mem_ctrl.cpp, pe_memory_interface.h). If a run has
             * memory traffic but the counter never advanced, this machine is
             * not driving that tracker, and r_idle would come out 1.0 -- the
             * whole memory reported idle. Refuse the descent and say so,
             * rather than print a number sourced from a dead counter. */
            double mc_r_idle = 0.0;
            const bool devmc_armed =
                (zsim_stats.pg_devmc_active > 0) ||
                (zsim_stats.mem_rd + zsim_stats.mem_wr == 0);
            if (zsim_stats.pg_window() > 0 && devmc_armed) {
                mc_r_idle = 1.0 - std::min(1.0,
                    static_cast<double>(zsim_stats.pg_devmc_active)
                        / static_cast<double>(zsim_stats.pg_window()));
            } else if (!devmc_armed) {
                std::cout << "  [pg] DRAM idle residency UNAVAILABLE: the "
                             "device-MC phase counter never advanced despite "
                          << (zsim_stats.mem_rd + zsim_stats.mem_wr)
                          << " accesses. Background reported at active "
                             "standby (IDD3N); no idle descent claimed."
                          << std::endl;
            }
            /* 1.11.51 (E17): the measured gap histogram supersedes the
             * phase-granular residency when present and sourced -- the
             * 10k-cycle phase window cannot see tXP-scale gaps. */
            {
                std::string gprov;
                double r_gap = gapPowerDownResidency(config, zsim_stats,
                                                     config.frequency_mhz, &gprov);
                if (r_gap >= 0.0) {
                    std::cout << "  [pg] power-down residency from the MEASURED "
                                 "gap histogram: " << r_gap << " (" << gprov
                              << "); phase-granular residency was " << mc_r_idle
                              << " (10k-cycle window, granularity-limited)."
                              << std::endl;
                    mc_r_idle = r_gap;
                }
            }
            /* 1.11.20 (D13): population-scaled. One DDR chip / one HBM
             * channel is the unit the JEDEC IDD table is written against. */
            const int bg_units =
                ram_oracle.getBackgroundUnits(config.dram_device_width,
                                              config.hierarchy_ranks_per_channel,
                                              config.hierarchy_dram_channels);  // 1.11.52 (A015)
            double bg_power_mw = ram_oracle.getBackgroundSystemMW(
                mc_r_idle, config.mem_power_down, config.dram_device_width,
                config.hierarchy_ranks_per_channel,
                config.hierarchy_dram_channels);   // 1.11.52 (A015)
            if (mc_r_idle > 0.0) {
                std::cout << "  [pg] DRAM idle residency " << mc_r_idle
                          << " -> background "
                          << ram_oracle.getBackgroundPowerMW() * bg_units
                          << " -> " << bg_power_mw << " mW ("
                          << (config.mem_power_down ? "IDD2N page-close then IDD2P "
                                             "power-down"
                                           : "IDD2N page-close; memory.power_down "
                                             "off, no power-down")   // 1.11.56 (A028)
                          << ", refresh always on)" << std::endl;
            }
            /* 1.11.20 (D13): scaled to the same population as the background
             * beside it, so the two lines are the same memory system. */
            double ref_energy = ram_oracle.getRefreshPowerMW() * bg_units;
            double leakage_mw = bg_power_mw;

            double total_rd_nj = rd_energy * zsim_stats.mem_rd;
            double total_wr_nj = wr_energy * zsim_stats.mem_wr;
            double total_act_nj = 0.0;  // folded into array rd/wr (no double-count)
            double total_iface_nj = iface_term_rd_nj * zsim_stats.mem_rd
                                  + iface_term_wr_nj * zsim_stats.mem_wr;

            std::cout << "  Technology:      " << config.memory_tech << " (Ramulator2 energy model)" << std::endl;
            std::cout << "  Per-access:      read=" << std::fixed << std::setprecision(3)
                      << rd_energy << " nJ, write=" << wr_energy << " nJ" << std::endl;
            std::cout << "  DQ interface:    rd=" << iface_term_rd_nj
                      << " nJ, wr=" << iface_term_wr_nj << " nJ per access ("
                      << (crosses_dq ? "accesses cross the DQ pins at this placement"
                                     : "on-die placement: no DQ crossing, no interface charge")
                      << ")" << std::endl;
            if (crosses_dq) {
                std::cout << "    termination:   rd=" << iface_term_rd_nj
                          << " nJ (DRAM RON -> RX RTT_NOM class), wr="
                          << iface_term_wr_nj
                          << " nJ (ctrl RON -> DRAM RTT_WR class)"
                             " [R7 split]  CACTI-IO driver+PHY: "
                          << iface_drv_nj
                          << " nJ [INFORMATIONAL, NOT charged -- 1.11.60: the"
                             " controller-side PHY is already priced by McPAT"
                             " (withPHY=1 at these placements), so charging"
                             " this too double-counts one crossing]"
                          << std::endl;
                /* 1.11.59 printed a BAND here while LPDDR5's Rtt was
                 * unsourced. 1.11.63: JESD209-5C Tbl 84 p.144 gives DQ ODT
                 * default = Disable, so the default termination term is zero
                 * by citation and the accessor now always returns false --
                 * this block is kept as the documented consumer. */
                {
                    double t_lo = 0.0, t_hi = 0.0; std::string t_prov;
                    if (ram_oracle.getTerminationEnergyBandNJ(t_lo, t_hi, t_prov)) {
                        std::cout << "    termination BAND " << t_lo << "-" << t_hi
                                  << " nJ [" << t_prov << "]" << std::endl;
                    }
                }
                if (iface_drv_nj <= 0.0)
                    std::cout << "    NOTE: CACTI-IO has no exact parameter map"
                                 " for this technology, so no informational"
                                 " driver/PHY figure is available." << std::endl;
            }
            std::cout << "  Array incl act+col in read/write terms above" << std::endl;
            /* 1.11.56 (audit A029): these three lines are one quantity seen
             * three ways, and the old labels invited adding them. Refresh is a
             * COMPONENT of Background (the header's own relation is
             * backgroundMW == vdd*idd3n + refreshMW), and Leakage is a literal
             * copy of Background -- a DRAM array's standby current IS its
             * leakage, there is no fourth term. Summing the three double-counts
             * refresh and doubles standby, about 2.1x the true background.
             * Nothing in this file sums them; a reader or a downstream parser
             * would. Note also that at r_idle > 0 the Refresh line and the
             * refresh inside Background are not the same baseline, since
             * Background is the state-aware getBackgroundSystemMW. */
            std::cout << "  Background:      " << bg_power_mw
                      << " mW (standby+refresh over " << bg_units << " "
                      << (config.memory_tech.substr(0, 3) == "HBM"
                              ? "channels/stack" : "chips/rank")
                      << ")" << std::endl;
            std::cout << "    of which refresh: " << ref_energy
                      << " mW (a component of Background, not an addition to it;"
                         " quoted at the IDD3N baseline)" << std::endl;
            std::cout << "    leakage:          " << leakage_mw
                      << " mW (the same standby current, named the way the "
                         "logic-side report names it -- not a further term)"
                      << std::endl;
            /* 1.11.60 (audit round 4, A011): the aggregate names the WHOLE
             * interface, not its smaller half.
             *
             * 1.11.58 folded driver switching and PHY into total_iface_nj and
             * printed the split two lines above, then left this breakdown
             * labelled "term=" -- the name the quantity carried when
             * termination was all of it. On DDR4 the number under that label
             * is 12.959 nJ/access where termination is 3.070, so a reader, or
             * a log parser reconciling this corpus against the pre-1.11.58
             * "term=" column, attributes the entire 4.2x interface correction
             * to termination. It was invisible because the label kept working:
             * the column still parses, it just means something else now. */
            std::cout << "  Total dynamic:   " << std::setprecision(1)
                      << (total_rd_nj + total_wr_nj + total_act_nj + total_iface_nj) / 1e6
                      << " mJ (rd=" << total_rd_nj / 1e6 << " + wr=" << total_wr_nj / 1e6
                      << " + act=" << total_act_nj / 1e6 << " + iface=" << total_iface_nj / 1e6
                      << " [termination + driver/PHY])"
                      << std::defaultfloat << std::endl;

            // DRAM die area via CACTI 7 (commercial DRAM cell model)
            try {
                pimid::CACTIWrapper::SRAMConfig dram_cfg;
                dram_cfg.ic_proj_type = config.interconnect_projection;   // 1.11.30 E5: one metal stack
                uint64_t chip_bytes = ram_oracle.getChipSizeMB() * 1024ULL * 1024ULL;
                uint32_t total_banks = ram_oracle.getBanksPerBankGroup()
                                     * ram_oracle.getBankGroupsPerChip();
                dram_cfg.capacity_bytes = chip_bytes;
                /* 1.11: CACTI requires block_bytes*8 >= output width. A 64B
                 * block is fine for x4/x8/x16 parts but rejects the wide HBM
                 * interface outright ("Block size must be at least 128"), and
                 * the failure silently degraded every HBM die area to the
                 * JEDEC density fallback. Size the block from the interface,
                 * exactly the relation CACTI enforces. */
                {
                    uint32_t iow = static_cast<uint32_t>(
                        std::max(8, ram_oracle.getChipIOBits()));
                    dram_cfg.line_size = std::max(64u, iow / 8u);
                }
                dram_cfg.associativity = 1;
                dram_cfg.banks = std::max(1u, total_banks);
                /* 1.11.2: the DRAM die's process is derived from its
                 * technology generation, NOT taken from power.tech_node_nm
                 * (that knob now names logic domains only). */
                {
                    DRAMGenClass gc = getDRAMGenClass(config.memory_tech);
                    dram_cfg.tech_node_nm = gc.cacti_table_nm;
                    dram_cfg.temperature = static_cast<uint32_t>(config.temperature_k);  // D055
                    std::cout << "  [tech] DRAM array process: generation class "
                              << gc.cls << " (derived from " << config.memory_tech
                              << "; CACTI " << gc.cacti_table_nm
                              << " nm table shapes structure, JEDEC k sets scale;"
                                 " independent of power.tech_node_nm)" << std::endl;
                }
                dram_cfg.is_cache = false;
                dram_cfg.is_main_memory = true;
                dram_cfg.cell_type = pimid::CACTIWrapper::COMM_DRAM;
                dram_cfg.output_width_bits = static_cast<uint32_t>(
                    std::max(8, ram_oracle.getChipIOBits()));
                dram_cfg.page_sz_bits = 8192;  // 1KB page typical
                dram_cfg.burst_len = 8;
                dram_cfg.int_prefetch_w = 8;

/* 1.11.1: CALIBRATED CACTI (user design, 2026-08-13). Neither source
                 * alone is right: CACTI's absolute die areas failed the JEDEC
                 * cross-check (four techs identical, HBM3 past a reticle), while
                 * the JEDEC density figure knows nothing about structure -- it
                 * cannot respond when a user reconfigures banks or IO. So each
                 * contributes what it is good at: k = JEDEC(preset org) /
                 * CACTI(preset org), computed at the technology's own Ramulator2
                 * organisation, and the reported area is CACTI(effective org) x k.
                 * At the stock organisation this is exactly the vendor-anchored
                 * figure; under reconfiguration it moves by CACTI's structural
                 * derivative. k is printed, so a calibrated number can never be
                 * mistaken for a raw tool output. */
                /* 1.11.14 (#122, borders rule): the k-calibration MOVED INTO
                 * the CACTI fork. Two runs still happen because the CALIBRATION
                 * is defined against the preset organisation while the reported
                 * die is the effective one -- that is the model, and it stays.
                 * What left is the arithmetic and the vendor-density table:
                 * both now live in the tool, where the scope gate also keeps
                 * them away from every cache query McPAT sends through the same
                 * library. */
                auto ref_cfg = dram_cfg;
                ref_cfg.memory_tech = config.memory_tech;   // opens calibration
                pimid::CACTIWrapper cacti_ref(ref_cfg);
                cacti_ref.initialize();
                auto eff_cfg = ref_cfg;
                {
                    int bpb = (config.banks_per_bg_override > 0)
                              ? config.banks_per_bg_override
                              : ram_oracle.getBanksPerBankGroup();
                    int bgc = (config.bg_per_chip_override > 0)
                              ? config.bg_per_chip_override
                              : ram_oracle.getBankGroupsPerChip();
                    eff_cfg.banks = std::max(1, bpb * bgc);
                }
                pimid::CACTIWrapper cacti_eff(eff_cfg);
                cacti_eff.initialize();
                auto ca_ref = cacti_ref.getCalibratedDieArea();
                double die_area_reported = 0.0;   // 1.11.51 (L243)
                if (ca_ref.calibrated && cacti_eff.isValid() &&
                    cacti_eff.getArea() > 0.0) {
                    double die_area = cacti_eff.getArea() * ca_ref.k;
                    die_area_reported = die_area;
                    std::cout << "  Area:            " << std::fixed << std::setprecision(2)
                              << die_area << " mm^2/die (CACTI x k, k="
                              << std::setprecision(3) << ca_ref.k
                              << " JEDEC-calibrated, FULL-DIE basis"
                              << (pimid::CACTIWrapper::vendorDieDensitySourced(
                                      config.memory_tech) ? "" : ", density DERIVED not measured")
                              << "; raw CACTI "
                              << std::setprecision(2) << cacti_eff.getArea()
                              << ")" << std::endl;
                } else {
                    /* 1.11.51 (L87): this fallback used to divide MBIT by a
                     * MB/mm^2 density (8x high) and then multiply by the
                     * phantom 1.12 E29 removed from the tool -- a ~9x error
                     * whenever it fired. The tool now owns the arithmetic. */
                    double die_area = pimid::CACTIWrapper::vendorAnchorAreaMM2(
                        config.memory_tech, chip_bytes);
                    die_area_reported = die_area;
                    std::cout << "  Area:            " << std::fixed << std::setprecision(2)
                              << die_area << " mm^2/die (JEDEC density fallback)"
                              << std::endl;
                }
                /* 1.11.51 (L243): the SYSTEM-COMPARABLE total, printed in
                 * device scope too. 1.11.9 justified adding the memory die
                 * to the system-scope total by claiming device scope already
                 * counted it -- it never did (its "Total Area" is McPAT
                 * silicon only; the die printed per-die above), so the two
                 * scopes reported non-comparable totals for one machine.
                 * Same populated basis as the system total (L237). ADDS a
                 * line; existing lines unchanged. */
                /* 1.11.58: the IO AREA joins the total.
                 *
                 * getInterfaceAreaMM2() has had no caller since 1.11.40 added
                 * it, so the DQ interface's silicon -- which PIMID's own
                 * report used to list as "unmodelled" -- was absent from every
                 * area total the simulator has printed. It is per-die IO, so
                 * it is added to the per-die figure before the population
                 * scaling, and reported separately so the change is visible
                 * rather than folded into the die number. */
                if (die_area_reported > 0.0) {
                    const double io_area = ram_oracle.getInterfaceAreaMM2();
                    if (io_area > 0.0) {
                        std::cout << "    CACTI-IO channel IO area: " << std::fixed
                                  << std::setprecision(3) << io_area
                                  << " mm^2/CHANNEL [INFORMATIONAL, NOT in any"
                                     " total -- 1.11.60: the full-die JEDEC"
                                     " calibration above already contains the"
                                     " DRAM's IO ring, and McPAT prices the"
                                     " controller PHY]" << std::defaultfloat
                                  << std::endl;
                        /* 1.11.60 (audit round 4, A001/A002/A009 -- REVERT,
                         * user-approved): NOT added. Three reasons, each
                         * sufficient. (1) io_area is a WHOLE-CHANNEL figure --
                         * extio.cc sums (num_dq + num_dqs + num_ca + num_clk)
                         * IO circuits, ~107 of them for a 64-bit channel --
                         * and this block labelled it per-die and multiplied
                         * by the die count: the 1168 gate's own numbers prove
                         * the 8x (47.91 - 28.36 = 19.55 = 2.444 x 8 exactly).
                         * (2) The memory die area above is a FULL-DIE JEDEC
                         * calibration, which already contains the DRAM's IO
                         * ring. (3) McPAT already prices the controller-side
                         * PHY. Both ends of the wire were counted before
                         * 1.11.58 added a third copy. The figure prints so
                         * the two estimates stay comparable; it enters no
                         * total. */
                    } else {
                        /* 1.11.60 (audit round 4, A003): STATE the absence, in
                         * the same voice the energy line beside it already
                         * uses.
                         *
                         * getInterfaceAreaMM2() returns 0 for four of the
                         * seven DRAM technologies, and the disclosure above
                         * was gated on the VALUE -- so the whole line, and
                         * with it the fact that the total below carries no DQ
                         * interface silicon at all, simply vanished. The
                         * driver/PHY energy path 150 lines up announces its
                         * own zero for exactly this reason; the area path did
                         * not, so a technology with no IO area and a
                         * technology whose IO area happens to be zero printed
                         * identically. That is the term this simulator's
                         * cross-technology area sweep compares.
                         *
                         * The two causes are different and are named
                         * separately. LPDDR5 and HBM2/HBM3 have no exact
                         * CACTI-IO parameter map, so nothing is computed --
                         * the same condition the energy note reports. GDDR6
                         * DOES have an exact map and receives the driver+PHY
                         * energy, but its area is withheld by CACTI-IO's own
                         * validity ceiling: the area polynomial's cubic term
                         * equals its linear term at 3162 MHz and GDDR6's bus
                         * clock is 7000 MHz, so above the crossover the figure
                         * is an extrapolation and cacti_io_wrapper.cpp returns
                         * zero rather than report it. No value changes here;
                         * what changes is that a missing term stops looking
                         * like a zero one. */
                        const bool iface_exact_map =
                            ram_oracle.getInterfaceDynamicEnergyNJ() > 0.0;
                        std::cout << "    + IO area:     none for this"
                                     " technology -- "
                                  << (iface_exact_map
                                      ? "CACTI-IO's area polynomial is an"
                                        " extrapolation above 3162 MHz and is"
                                        " withheld at this bus clock"
                                      : "CACTI-IO has no exact parameter map"
                                        " for it")
                                  << ", so the total below carries no DQ"
                                     " interface silicon (DDR3/DDR4/DDR5 do)."
                                  << std::endl;
                    }
                    int dies = memorySystemDieCount(config.memory_tech,
                                                    config.dram_device_width,
                                                    config.hierarchy_ranks_per_channel,
                                                    config.hierarchy_dram_channels);
                    std::cout << "  With memory:     " << std::fixed
                              << std::setprecision(2)
                              << (mcpat.getTotalArea() + die_area_reported * dies)
                              << " mm^2 (device silicon + " << dies
                              << " dies x " << die_area_reported
                              << " -- the system-scope comparable total)"
                              << std::defaultfloat << std::endl;
                }
            } catch (const std::exception& e) {
                std::cerr << "  [DRAM area] CACTI query failed: " << e.what() << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "  [DRAM energy] Ramulator2 query failed: " << e.what() << std::endl;
        }
    } else if (config.memory_tech == "SRAM") {
        // SRAM: CACTI model
        try {
            // Per-bank characterization: model ONE 64KB bank (CACTI supports only
            // 1-32 banks; a 256-bank device would overflow its range), then scale
            // the extensive quantities (leakage, area) by the bank count. Per-access
            // energy is already per-bank -- one access hits one bank.
            pimid::CACTIWrapper::SRAMConfig sram_cfg;
            sram_cfg.device_corner = (config.device_corner == "lstp") ? 1 : (config.device_corner == "lop") ? 2 : 0;  // 1.11.49 (L59)
                sram_cfg.ic_proj_type = config.interconnect_projection;   // 1.11.30 E5: one metal stack
            sram_cfg.capacity_bytes = 64 * 1024;  // one bank
            sram_cfg.line_size = config.cache_line_size;
            sram_cfg.associativity = 1;
            sram_cfg.banks = 1;
            sram_cfg.tech_node_nm = validateTechNodeNm(config.tech_node_nm, "SRAM array");
            sram_cfg.temperature = static_cast<uint32_t>(config.temperature_k);  // 1.11.52 (D055)
            sram_cfg.is_cache = false;  // RAM mode
            sram_cfg.read_write_ports = config.ports_per_bank;

            pimid::CACTIWrapper cacti(sram_cfg);
            cacti.initialize();

            double rd_energy = cacti.getDynamicReadEnergy();  // per-access (one bank)
            double wr_energy = cacti.getDynamicWriteEnergy();
            double leakage = cacti.getLeakagePower() * config.num_banks;  // device = num_banks banks
            double area = cacti.getArea() * config.num_banks;

            /* 1.11.18 (audit go-through): pim.mc.pg reached the DRAM branch
             * (IDD2P descent) and the NVM branch (retention floor) and did
             * NOTHING here -- an SRAM main memory silently ignored the flag
             * in the same if/else chain. SRAM is VOLATILE: gating the array
             * loses the data, so only the PERIPHERY can gate, at the same
             * CACTI-class sleep-tx residual the McPAT endpoints use
             * (Vcc_min/Vdd ~= 0.35). The cell array keeps its full leakage.
             * The split is stated rather than assumed: CACTI does not
             * separate them here, so the periphery share is declared. */
            /* 1.11.41: memory-side flag. This gates the ARRAY periphery, which
             * is a property of the memory, not of its controller -- it was
             * driven by pim.mc.pg, so gating the controller silently gated the
             * SRAM periphery too and the two savings could not be separated.
             *
             * RETENTION IS NOT MODELLED, and this is where it would go. The
             * model below gates the periphery and leaves the CELL ARRAY at full
             * leakage, which is the conservative bound for a volatile array:
             * you cannot cut Vdd to an SRAM cell without losing its contents.
             * A real drowsy/retention design holds the array at a reduced
             * retention voltage instead -- keeping data while cutting leakage
             * somewhere between full and gated. Adding that needs a
             * retention-voltage leakage model, and CACTI does not provide one
             * (it computes power_gated_leakage, the DESTRUCTIVE case, not a
             * retention floor). So the reported SRAM saving is a LOWER BOUND,
             * stated rather than closed with an invented retention factor.
             *
             * The alternative that IS fully modellable with what we hold:
             * flush-then-gate. Write back the dirty lines (countDirtyLines()
             * from 1.11.40 N7 already measures exactly that set and the flush
             * energy is already priced), then gate destructively at CACTI's
             * power_gated_leakage. That trades a real writeback cost for the
             * full gated floor and invents nothing. Logged for a later release
             * rather than bundled here. */
            /* 1.11.57 (audit round 3, A003): the SAME unarmed-counter
             * refusal the DRAM branch above applies. A leakage CREDIT taken
             * on a counter that never advanced is indistinguishable from a
             * measured idle device: pg_devmc_active stays 0 both when the
             * memory truly idled and when nothing ever armed the counter, and
             * only the second case is a non-measurement. Without the guard an
             * array with real traffic collected the full array-gating credit
             * on a dead counter. */
            const bool a3_devmc_armed =
                (zsim_stats.pg_devmc_active > 0) ||
                (zsim_stats.mem_rd + zsim_stats.mem_wr == 0);
            if (config.mem_array_pg && zsim_stats.pg_window() > 0 && !a3_devmc_armed) {
                std::cout << "  [pg] array-gating residency UNAVAILABLE: the "
                             "device-MC phase counter never advanced despite "
                             "measured traffic, so no idle residency was "
                             "measured and no leakage credit is taken."
                          << std::endl;
            }
            if (config.mem_array_pg && zsim_stats.pg_window() > 0 && a3_devmc_armed) {
                double r_idle = 1.0 - std::min(1.0,
                    static_cast<double>(zsim_stats.pg_devmc_active)
                        / static_cast<double>(zsim_stats.pg_window()));
                if (r_idle > 0.0) {
                    /* 1.11.20 (user decision D16): DERIVE the periphery share
                     * instead of declaring it. A second CACTI query on the same
                     * geometry with the periphery minimised (single small bank,
                     * one port) isolates the cell-array leakage; what the full
                     * configuration carries above that is periphery. Replaces
                     * the 1.11.18 declared 0.30 -- tool-sourced-everything
                     * applies to this constant too. If the probe fails we say
                     * so and refuse the credit rather than fall back to a
                     * made-up number. */
                    double periph_share = -1.0;
                    try {
                        pimid::CACTIWrapper::SRAMConfig cell_cfg = sram_cfg;
                        cell_cfg.read_write_ports = 1;
                        cell_cfg.quiet = true;
                        pimid::CACTIWrapper cell_probe(cell_cfg);
                        cell_probe.initialize();
                        if (cell_probe.isValid()) {
                            double cell_only = cell_probe.getLeakagePower() * config.num_banks;
                            if (leakage > 0.0 && cell_only > 0.0 && cell_only < leakage)
                                periph_share = 1.0 - (cell_only / leakage);
                        }
                    } catch (const std::exception&) { periph_share = -1.0; }

                    if (periph_share <= 0.0) {
                        std::cout << "  [pg] SRAM periphery gating REFUSED: the "
                                     "array-only CACTI probe did not isolate a "
                                     "periphery share, so there is no measured "
                                     "split to gate on (no credit taken)."
                                  << std::endl;
                    } else {
                        /* 1.11.41 (user ruling: harness the SRAM PG with a
                         * RETENTION path). Both voltage ratios below are
                         * CACTI's own, not ours -- parameter.cc derives them
                         * per node as a fraction of the interpolated Vdd:
                         *     peri_global.Vcc_min = Vdd * 0.35   (periphery)
                         *     sram_cell.Vcc_min   = Vdd * 0.65   (cell array)
                         * and CACTI documents Vcc_min for a memory cell as
                         * "the lowest vcc for data retention". Its power-gating
                         * model drops a block to Vcc_min (decoder.cc:212,
                         * detalV = Vdd - Vcc_min), so CACTI's "gated" state IS
                         * a retention state -- the tool already had the model
                         * PIMID was missing.
                         *
                         * The array therefore does NOT have to stay at full
                         * leakage. It cannot be gated to zero (volatile: that
                         * loses the data), but it CAN sit at its retention
                         * voltage and keep it. Previously the cells were held
                         * at full Vdd throughout, which understated the saving
                         * -- stated at the time as a lower bound, now closed
                         * with the tool's own figure rather than an invented
                         * retention factor. */
                        const double kPeriphResidual = 0.35;   // peri_global.Vcc_min/Vdd
                        const double kCellRetention  = 0.65;   // sram_cell.Vcc_min/Vdd
                        /* Using the VOLTAGE ratio as the LEAKAGE ratio is the
                         * conservative direction: subthreshold leakage falls
                         * faster than linearly in Vdd (DIBL), so real retention
                         * leakage at 0.65*Vdd is BELOW 0.65x. The credited
                         * saving is therefore an under-estimate, never an
                         * over-estimate -- the same bias direction as every
                         * other bound in this file. */
                        double before = leakage;
                        double periph = leakage * periph_share;
                        double cells  = leakage - periph;
                        /* Periphery gates (state rebuilt on wake); cells go to
                         * retention (state kept). Different mechanisms, so
                         * different residuals, applied over the same measured
                         * idle residency. */
                        leakage = cells  * ((1.0 - r_idle) + kCellRetention  * r_idle)
                                + periph * ((1.0 - r_idle) + kPeriphResidual * r_idle);
                        std::cout << "  [pg] SRAM gating: idle residency " << r_idle
                                  << " -> leakage " << before << " -> " << leakage
                                  << " mW (periphery share " << periph_share
                                  << " DERIVED from an array-only CACTI query; "
                                     "periphery gates to Vcc_min/Vdd=" << kPeriphResidual
                                  << ", cells hold DATA at retention Vcc_min/Vdd="
                                  << kCellRetention << " -- both CACTI per-node values)"
                                  << std::endl;
                    }
                }
            }

            double total_rd_nj = rd_energy * zsim_stats.mem_rd;
            double total_wr_nj = wr_energy * zsim_stats.mem_wr;

            std::cout << "  Technology:      SRAM (CACTI 7)" << std::endl;
            std::cout << "  Per-access:      read=" << std::fixed << std::setprecision(3)
                      << rd_energy << " nJ, write=" << wr_energy << " nJ" << std::endl;
            std::cout << "  Leakage:         " << leakage << " mW" << std::endl;
            std::cout << "  Total dynamic:   " << std::setprecision(1)
                      << (total_rd_nj + total_wr_nj) / 1e6 << " mJ"
                      << std::defaultfloat << std::endl;
            std::cout << "  Area:            " << std::fixed << std::setprecision(3)
                      << area << " mm^2" << std::defaultfloat << std::endl;
            /* 1.11.57 (audit round 3, A009): the SYSTEM-COMPARABLE total, for
             * SRAM and NVM too. 1.11.51 (L243) added this line so device and
             * system scope would report comparable totals for one machine, and
             * added it inside the DRAM branch only -- so for four of the eleven
             * technologies a device-scope run's only total stayed McPAT silicon
             * while the same machine in system scope reported McPAT silicon
             * plus the array. The array area is printed on the line above; it
             * was simply never added to anything here. */
            std::cout << "  With memory:     " << std::fixed << std::setprecision(3)
                      << (mcpat.getTotalArea() + area)
                      << " mm^2 (device silicon + the array above"
                         " -- the system-scope comparable total)"
                      << std::defaultfloat << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "  [SRAM power] CACTI query failed: " << e.what() << std::endl;
        }
    } else {
        // NVM: NVSim model (STT_MRAM, PCM, RERAM)
        try {
            // Per-bank characterization: model ONE 64KB bank, then scale leakage/area
            // by the bank count (per-access energy is already per-bank). Also keeps
            // NVSim off the multi-minute 16MB-array characterization.
            pimid::NVSimWrapper::NVMConfig nvm_cfg;
            nvm_cfg.device_corner = (config.device_corner == "lstp") ? 1
                                  : (config.device_corner == "lop")  ? 2 : 0;  // 1.11.49 (L77)
            nvm_cfg.capacity_bytes = 64 * 1024;  // one bank
            nvm_cfg.word_width_bits = config.cache_line_size * 8;  // one full line per access (64 B = 512 b), matching the CACTI/SRAM path
            nvm_cfg.process_node_nm = validateTechNodeNm(config.tech_node_nm, "NVM array");
            nvm_cfg.temperature_k = config.temperature_k;   // 1.11.52 (D055)
            if (config.memory_tech == "STT_MRAM")
                nvm_cfg.nvm_type = pimid::NVSimWrapper::NVMType::STTRAM;
            else if (config.memory_tech == "PCM")
                nvm_cfg.nvm_type = pimid::NVSimWrapper::NVMType::PCRAM;
            else if (config.memory_tech == "RERAM")
                nvm_cfg.nvm_type = pimid::NVSimWrapper::NVMType::RERAM;

            pimid::NVSimWrapper nvsim(nvm_cfg);
            nvsim.initialize();

            double rd_energy = nvsim.getReadDynamicEnergy();  // per-access (one bank)
            double wr_energy = nvsim.getWriteDynamicEnergy();
            double leakage = nvsim.getLeakagePower() * config.num_banks;  // device = num_banks banks
            /* 1.11.8: NVM periphery gates RETENTION-FREE -- non-volatile
             * cells hold state unpowered, so with memory.array_pg the array
             * periphery leakage descends during measured no-traffic
             * residency.
             *
             * 1.11.51 (L142): the gated residual is CACTI's OWN sleep-
             * transistor state, the same authority the SRAM path cites --
             * peri_global.Vcc_min/Vdd = 0.35 (decoder.cc detalV model) --
             * not the invented 2% floor this block used to carry. NVSim's
             * reported leakage is ALL CMOS periphery (the cells are non-
             * volatile and do not leak), so the old floor claimed a 50x
             * leakage cut for which no linked tool provides a model, 17x
             * deeper than the sleep-tx state the comment cited. Retention-
             * free means the periphery COULD be cut harder than a retention
             * design -- but neither CACTI nor NVSim models a deeper off
             * state, so 0.35 is reported as the tool-backed BOUND rather
             * than closing the gap with a made-up number. */
            /* 1.11.57 (audit round 3, A003): the SAME unarmed-counter
             * refusal the DRAM branch above applies. A leakage CREDIT taken
             * on a counter that never advanced is indistinguishable from a
             * measured idle device: pg_devmc_active stays 0 both when the
             * memory truly idled and when nothing ever armed the counter, and
             * only the second case is a non-measurement. Without the guard an
             * array with real traffic collected the full array-gating credit
             * on a dead counter. */
            const bool a3_devmc_armed =
                (zsim_stats.pg_devmc_active > 0) ||
                (zsim_stats.mem_rd + zsim_stats.mem_wr == 0);
            if (config.mem_array_pg && zsim_stats.pg_window() > 0 && !a3_devmc_armed) {
                std::cout << "  [pg] array-gating residency UNAVAILABLE: the "
                             "device-MC phase counter never advanced despite "
                             "measured traffic, so no idle residency was "
                             "measured and no leakage credit is taken."
                          << std::endl;
            }
            if (config.mem_array_pg && zsim_stats.pg_window() > 0 && a3_devmc_armed) {   // 1.11.41: memory-side flag
                double r_idle = 1.0 - std::min(1.0,
                    static_cast<double>(zsim_stats.pg_devmc_active)
                        / static_cast<double>(zsim_stats.pg_window()));
                if (r_idle > 0.0) {
                    const double kPeriphResidual = 0.35;  // CACTI peri_global.Vcc_min/Vdd
                    double before = leakage;
                    leakage = leakage * (1.0 - r_idle) + leakage * kPeriphResidual * r_idle;
                    std::cout << "  [pg] NVM periphery gating (sleep-tx state, "
                                 "CACTI Vcc_min/Vdd=" << kPeriphResidual
                              << ", tool-backed bound; retention-free could cut "
                                 "deeper but no linked tool models it): idle "
                                 "residency " << r_idle << " -> periphery leakage "
                              << before << " -> " << leakage << " mW" << std::endl;
                }
            }
            double area = nvsim.getArea() * config.num_banks;

            double total_rd_nj = rd_energy * zsim_stats.mem_rd;
            double total_wr_nj = wr_energy * zsim_stats.mem_wr;

            std::cout << "  Technology:      " << config.memory_tech << " (NVSim)" << std::endl;
            std::cout << "  Per-access:      read=" << std::fixed << std::setprecision(3)
                      << rd_energy << " nJ, write=" << wr_energy << " nJ" << std::endl;
            std::cout << "  Leakage:         " << leakage << " mW" << std::endl;
            std::cout << "  Total dynamic:   " << std::setprecision(1)
                      << (total_rd_nj + total_wr_nj) / 1e6 << " mJ"
                      << std::defaultfloat << std::endl;
            std::cout << "  Area:            " << std::fixed << std::setprecision(3)
                      << area << " mm^2" << std::defaultfloat << std::endl;
            /* 1.11.57 (audit round 3, A009): the SYSTEM-COMPARABLE total, for
             * SRAM and NVM too. 1.11.51 (L243) added this line so device and
             * system scope would report comparable totals for one machine, and
             * added it inside the DRAM branch only -- so for four of the eleven
             * technologies a device-scope run's only total stayed McPAT silicon
             * while the same machine in system scope reported McPAT silicon
             * plus the array. The array area is printed on the line above; it
             * was simply never added to anything here. */
            std::cout << "  With memory:     " << std::fixed << std::setprecision(3)
                      << (mcpat.getTotalArea() + area)
                      << " mm^2 (device silicon + the array above"
                         " -- the system-scope comparable total)"
                      << std::defaultfloat << std::endl;
        } catch (const std::exception& e) {
            /* 1.11.56 (D032): the memory array IS the memory power for an NVM
             * cell. Printing the error and continuing leaves a corpus row that
             * looks complete and carries no array energy at all -- the same
             * failure the McPAT path was made fatal for. */
            std::cerr << "  [NVM power] NVSim query failed: " << e.what() << std::endl;
            std::cerr << "  [NVM power] FATAL: the " << config.memory_tech
                      << " array could not be characterized, so this run has no "
                         "memory-array energy, leakage or area. Refusing to report "
                         "it as a result." << std::endl;
            std::exit(3);
        }
    }

    // -- Model Coverage Table --
    std::cout << "\nModel Coverage:" << std::endl;
    std::cout << "  Component                Timing        Power         Area" << std::endl;
    std::cout << "  -------------------------------------------------------------" << std::endl;

    // PE cores
    std::cout << "  PE cores (x" << config.num_pes << ")";
    int pad = 22 - static_cast<int>(std::to_string(config.num_pes).size()) - 12;
    for (int i = 0; i < std::max(1, pad); i++) std::cout << " ";
    std::cout << "ZSim          McPAT         McPAT" << std::endl;

    // L2 cache
    if (!alu_only && config.enable_l2) {
        std::string l2_label = "L2 cache (x" + std::to_string(config.l2_count) + ")";
        std::cout << "  " << std::left << std::setw(24) << l2_label
                  << "CACTI         McPAT         McPAT" << std::endl;
    }

    // L3 cache
    if (!alu_only && config.enable_l3) {
        std::cout << "  " << std::left << std::setw(24) << "L3 cache (x1)"
                  << "CACTI         McPAT         McPAT" << std::endl;
    }

    // PE-MIs
    if (config.pe_mc_enabled && config.pes_per_mc > 0) {
        int mi_count = peMemoryInterfaceCount(config);   // 1.11.60 (A012)
        std::string mi_label = "PE-MIs (x" + std::to_string(mi_count) + ")";
        std::cout << "  " << std::left << std::setw(24) << mi_label
                  << std::setw(14) << "Simple" << "McPAT         McPAT" << std::endl;
    }

    // Memory array
    {
        std::string mem_label = "Memory (" + config.memory_tech + ")";
        std::string timing_src, power_src, area_src;
        if (is_dram_tech) {
            timing_src = "Ramulator2";
            power_src = "Ramulator2";
            area_src = "CACTI";
        } else if (config.memory_tech == "SRAM") {
            timing_src = "CACTI";
            power_src = "CACTI";
            area_src = "CACTI";
        } else {
            timing_src = "NVSim";
            power_src = "NVSim";
            area_src = "NVSim";
        }
        std::cout << "  " << std::left << std::setw(24) << mem_label
                  << std::setw(14) << timing_src << std::setw(14) << power_src
                  << area_src << std::endl;
    }

    // Host MC (when PEs use HOST_MC placement -- MC is a NoC endpoint)
    if (!config.pe_mc_enabled && config.num_pes > 1) {
        std::string hmc_label = "Host MC (x1)";
        std::string hmc_timing = is_dram_tech ? "Ramulator2" : "Simple";
        std::cout << "  " << std::left << std::setw(24) << hmc_label
                  << std::setw(14) << hmc_timing << "McPAT         McPAT" << std::endl;
    }

    // NoC (connects PEs <-> MCs -- memory controllers are network endpoints)
    if (config.num_pes > 1) {
        int active_levels = 0;
        for (int i = 0; i < 7; ++i) {
            /* 1.11.60 (audit round 4, B013): hierarchy_bridge_latency is a
             * std::array<int,6> -- there are six BOUNDARIES between seven
             * levels -- and this was the one of three read sites without the
             * i < 6 guard, so i == 6 was an out-of-bounds read on every run
             * with more than one element. Undefined behaviour that happened
             * to return whatever sits after the array. */
            const bool bridge_active = (i < 6) && (config.hierarchy_bridge_latency[i] > 0);
            if (config.hierarchy_level_latency[i] > 0 || bridge_active)
                active_levels++;
        }
        int mc_endpoints = peMemoryInterfaceCount(config);   // 1.11.60 (A012)

        std::string noc_label = "NoC";
        if (active_levels > 0)
            noc_label += " (" + std::to_string(active_levels) + " lvl)";
        std::string noc_detail = std::to_string(config.num_pes) + " PEs + "
                                 + std::to_string(mc_endpoints) + " MCs";
        // Determine displayed NoC model
        std::string timing_model;
        if (config.noc_cycle_accurate) {
            timing_model = "Garnet";
        } else {
            timing_model = "Simple";  // hop count + M/D/1 queuing
        }
        std::cout << "  " << std::left << std::setw(24) << noc_label
                  << std::setw(14) << timing_model << "McPAT         McPAT" << std::endl;
        std::cout << "    endpoints: " << noc_detail << std::endl;
        /* 1.11.60 (audit round 4, A012): when the clamp is what produced the
         * printed count, say so and name the authority -- otherwise the table
         * differs from the raw quotient a reader would compute from the two
         * keys in front of them, with nothing to explain the difference. */
        if (config.pe_mc_enabled && config.pes_per_mc > 0 &&
            config.num_pes / config.pes_per_mc < 1)
            std::cout << "      (pim.mc.pes_per_mc=" << config.pes_per_mc
                      << " exceeds pim.pe.count=" << config.num_pes
                      << "; the run instantiates and prices ONE interface --"
                         " the emitted ZSim configuration is the authority and"
                         " McPAT follows it)" << std::endl;
    }

    // Host (system scope with host nodes)
    if (config.scope == "system") {
        bool show_host = false;
        for (const auto& n : config.system_nodes) {
            if (n.role == UnifiedConfig::SystemNode::HOST) { show_host = true; break; }
        }
        if (show_host) {
            std::cout << "  " << std::left << std::setw(24) << "Host processor"
                      << "ZSim          McPAT         McPAT" << std::endl;
            std::string pcie_model_display = "Simple";  // always includes M/D/1 queuing
            std::cout << "  " << std::left << std::setw(24) << "PCIe/CXL"
                      << std::setw(14) << pcie_model_display << "McPAT         McPAT" << std::endl;
        }
    }
    std::cout << std::right;  // restore default alignment

    } // !detail_summary
}

/**
 * @brief Per-node power analysis for system scope -- runs McPAT once per node type.
 * Each node gets its own DeviceProfile derived from its core_type.
 */
/* 1.9.42: the one memory's array energy, for the scopes that were not charging
 * it. Written as its own small computation rather than by relocating the
 * device-scope block, so that path stays byte-for-byte what it was and is
 * bit-identical by construction.
 *
 * WHAT WAS WRONG: the array energy computation lived only inside
 * runPowerAnalysis, the device-scope path. Co-simulation goes through
 * runPerNodePowerAnalysis and never reached it, so a co-simulated system
 * reported NO memory array energy at all -- not for the host, and not for the
 * processing device either, though the identical configuration in device scope
 * reported it. The models were never missing. The call was.
 *
 * WHY ONE TERM AND NOT TWO: this build models one host and one memory. The
 * memory either IS the processing device, or it is a plain non-PIM main memory
 * and the run is host-only. resolveMemoryTopology has already resolved which,
 * into node.memory_tech, so every node names the SAME array -- in a
 * co-simulation the host's accesses and the elements' accesses land on one
 * piece of silicon. Summing them and charging once is the model; charging a host
 * term beside a device term would price that silicon twice, which is the fault
 * this release exists to avoid rather than introduce.
 *
 * Interface energy is deliberately excluded here. It is charged host-side in the
 * device-scope path already, and it is inherited constant rather than measured
 * (tracked separately), so adding it in a second place would compound an
 * approximation instead of a measurement. */
/* 1.11.9 (#86, audit): the memory die's AREA never appeared in system scope,
 * so a co-simulated system's "System Total" area silently omitted the memory
 * -- the largest piece of silicon in a PIM system. Same JEDEC-calibrated CACTI
 * model as device scope (1.11.1): k = JEDEC(preset org) / CACTI(preset org),
 * area = CACTI(effective) x k, k printed.
 *
 * NOTE (borders rule): this duplicates the device-scope block's logic rather
 * than relocating it, so that path stays bit-identical here. 1.11.14 migrates
 * the calibration INTO the CACTI fork and both call sites collapse onto one
 * tool call -- this helper is the seam that migration will use. */
/* 1.11.46 (audit, FIX-PRE-FLEET L237): HOW MANY DIES the memory system is.
 * System Total area used to add ONE DRAM die for the whole subsystem -- a 16x
 * understatement for the DDR5 preset (x8 chips: 8 chips/rank) and the HBM3
 * preset (16 channels). The counts below are the JEDEC organisation the rest
 * of the model already declares, not new assumptions:
 *   DDR3/4/5   chips/rank from the device width (x4->16, x8->8, x16->4 for a
 *              64-bit channel; the same table backgroundUnits() uses, D13)
 *              x ranks/channel x channels.
 *   HBM2/3     dies per stack, JEDEC MINIMUM: 2 channels per core die
 *              (JESD235/238), so 8ch->4 dies, 16ch->8 dies. Taller stacks
 *              exist; this is the LOWER BOUND and is stated as one.
 *   LPDDR5/GDDR6  point-to-point: one die per channel x channels.
 * Population is PHYSICAL organisation -- it does not depend on placement. */
/* 1.11.46 (FIX-PRE-FLEET L242): non-DRAM system memory is silicon too.
 * computeDramDieAreaMM2 correctly returns 0 for SRAM/NVM, and the system
 * total then silently carried NO area for those memories while a DRAM system
 * got a full (now populated) die. Priced with the LIVE tools -- the same
 * queries the device-scope path already makes (CACTI for SRAM, NVSim
 * pre-generated cache for the NVMs), per bank x banks. Returns 0 with a note
 * on failure; the caller's fallback story is the note, not a guess. */
/* 1.11.52 (audit A009/A021): the SAME array, queried the SAME way in both
 * scopes. This helper built its CACTI/NVSim configs from a short argument
 * list and left out everything else -- device_corner, the interconnect
 * projection, read/write ports (SRAM) and the corner again (NVM) -- so one
 * machine's non-DRAM memory was characterized differently depending on
 * which scope asked. It now takes the config and sets the same fields the
 * device-scope blocks set.
 *
 * A021: the non-exception failure paths returned 0 SILENTLY, deleting the
 * memory from System Total area while the header promised "Returns 0 with a
 * note on failure; the caller's fallback story is the note". Every return 0
 * now says which query failed and what that costs. */
static double computeNonDramMemAreaMM2(const std::string& tech, int banks,
                                       int tech_node_nm, int line_size,
                                       const UnifiedConfig& config) {
    if (banks < 1) banks = 1;
    const int corner = (config.device_corner == "lstp") ? 1
                     : (config.device_corner == "lop")  ? 2 : 0;
    auto omitted = [&](const char* why) -> double {
        std::cerr << "[area] NOTE: " << tech << " memory die area unavailable ("
                  << why << "); System Total AREA omits this memory, stated "
                     "here rather than reported as zero silicon." << std::endl;
        return 0.0;
    };
    try {
        if (tech == "SRAM") {
            pimid::CACTIWrapper::SRAMConfig cfg;
            cfg.capacity_bytes = 64 * 1024;                 // one bank
            cfg.line_size = static_cast<uint32_t>(line_size > 0 ? line_size : 64);
            cfg.associativity = 1;
            cfg.banks = 1;
            cfg.tech_node_nm = tech_node_nm > 0 ? tech_node_nm : 22;
            cfg.is_cache = false;
            cfg.is_main_memory = true;
            cfg.quiet = true;
            cfg.device_corner = corner;                              // A009
            cfg.temperature = static_cast<uint32_t>(config.temperature_k);  // D055
            cfg.ic_proj_type = config.interconnect_projection;       // A009 (E5)
            cfg.read_write_ports = config.ports_per_bank;            // A009
            pimid::CACTIWrapper cacti(cfg);
            cacti.initialize();
            if (cacti.isValid() && cacti.getArea() > 0.0)
                return cacti.getArea() * banks;
            return omitted("CACTI returned no valid SRAM area");
        } else if (tech == "STT_MRAM" || tech == "PCM" || tech == "RERAM") {
            pimid::NVSimWrapper::NVMConfig cfg;
            cfg.capacity_bytes = 64 * 1024;                 // one bank
            cfg.word_width_bits = static_cast<uint32_t>((line_size > 0 ? line_size : 64) * 8);
            cfg.process_node_nm = tech_node_nm > 0 ? tech_node_nm : 22;
            cfg.device_corner = corner;                              // A009 (L77)
            cfg.temperature_k = config.temperature_k;                // D055
            cfg.nvm_type = (tech == "STT_MRAM") ? pimid::NVSimWrapper::NVMType::STTRAM
                         : (tech == "PCM")      ? pimid::NVSimWrapper::NVMType::PCRAM
                                                : pimid::NVSimWrapper::NVMType::RERAM;
            pimid::NVSimWrapper nvsim(cfg);
            nvsim.initialize();
            if (nvsim.getArea() > 0.0) return nvsim.getArea() * banks;
            return omitted("NVSim returned no valid array area");
        }
    } catch (const std::exception& e) {
        std::cerr << "[area] NOTE: " << tech << " memory die area query failed ("
                  << e.what() << "); the system total omits it, stated here."
                  << std::endl;
        return 0.0;
    }
    return omitted("technology has no non-DRAM area model in this build");
}

static int memorySystemDieCount(const std::string& tech,
                                const std::string& device_width,
                                int ranks_per_channel, int channels) {
    if (channels < 1) channels = 1;
    if (ranks_per_channel < 1) ranks_per_channel = 1;
    if (tech == "DDR3" || tech == "DDR4" || tech == "DDR5") {
        /* 1.11.57 (latent A013): the x4/x8/x16 -> chips/rank table used to be
         * written out here as well as in the hierarchy derivation. One table
         * now, shared. */
        return chipsPerRankForDeviceWidth(device_width)
             * ranks_per_channel * channels;
    }
    /* 1.11.57 (latent A013): READ THE CHANNEL COUNT.
     *
     * These two lines returned 4 and 8 as literals and never touched the
     * `channels` argument that determines them -- the comments even showed the
     * arithmetic (8 channels / 2 per core die, 16 / 2) without performing it.
     * The literals were right only because hierarchy_dram_channels is written
     * in exactly one place, from the Ramulator preset, which gives HBM2 8 and
     * HBM3 16 and nothing else. A preset revision or a channel-count knob
     * would have moved the input and left the die population behind, and the
     * die population multiplies the whole memory-side area total.
     *
     * The literal survives as a FLOOR, which is what its comment already
     * called it ("min"): a JEDEC HBM2 stack is at least 4 core dies and an
     * HBM3 stack at least 8, whatever a channel census says. Above that floor
     * the argument decides. Two channels land on one core die in both
     * generations (the pseudo-channel pair). */
    if (tech == "HBM2") return std::max(4, channels / 2);
    if (tech == "HBM3") return std::max(8, channels / 2);
    if (tech == "LPDDR5" || tech == "GDDR6") return channels;
    return 1;
}

static double computeDramDieAreaMM2(const std::string& memory_tech, bool print,
                                    int effective_banks = 0)
{
    /* 1.11.14 (#122, borders rule): the calibration MOVED INTO the CACTI
     * fork (CACTIWrapper::getCalibratedDieArea). This is now description +
     * a tool call: build the query the DRAM die is, ask, report. The density
     * and generation tables went with it. The scope gate lives in the tool,
     * where it also protects the thousands of cache/RF/TLB queries McPAT
     * makes through the SAME library. */
    const bool is_dram = !(memory_tech == "SRAM" || memory_tech == "STT_MRAM" ||
                           memory_tech == "PCM"  || memory_tech == "RERAM");
    if (memory_tech.empty() || !is_dram) return 0.0;
    try {
        pimid::RamulatorWrapper ram_oracle("", memory_tech);
        ram_oracle.initialize();
        uint64_t chip_bytes = ram_oracle.getChipSizeMB() * 1024ULL * 1024ULL;
        uint32_t total_banks = ram_oracle.getBanksPerBankGroup()
                             * ram_oracle.getBankGroupsPerChip();
        pimid::CACTIWrapper::SRAMConfig cfg;
        cfg.capacity_bytes = chip_bytes;
        uint32_t iow = static_cast<uint32_t>(std::max(8, ram_oracle.getChipIOBits()));
        cfg.line_size = std::max(64u, iow / 8u);
        cfg.associativity = 1;
        cfg.banks = std::max(1u, total_banks);
        cfg.tech_node_nm = pimid::CACTIWrapper::generationTableNm(memory_tech);
        cfg.is_cache = false;
        cfg.is_main_memory = true;
        cfg.cell_type = pimid::CACTIWrapper::COMM_DRAM;
        cfg.memory_tech = memory_tech;      // 1.11.14: opens the calibration
        cfg.output_width_bits = iow;
        cfg.page_sz_bits = 8192;
        cfg.burst_len = 8;
        cfg.int_prefetch_w = 8;
        cfg.quiet = true;
        /* 1.11.46 (L238): a failed CACTI run must NOT silently delete the
         * memory from the system total. The vendor anchor (capacity/density)
         * needs no CACTI; fall back to it and say so. */
        auto jedecFallback = [&](const char* why) -> double {
            /* 1.11.51 (L87): arithmetic owned by the tool. */
            double mm2 = pimid::CACTIWrapper::vendorAnchorAreaMM2(memory_tech,
                                                                  chip_bytes);
            if (mm2 <= 0.0) return 0.0;
            std::cerr << "[area] NOTE: CACTI could not price the " << memory_tech
                      << " die (" << why << "); using the vendor density anchor "
                      << mm2 << " mm^2/die directly." << std::endl;
            return mm2;
        };
        pimid::CACTIWrapper cacti(cfg);
        cacti.initialize();
        if (!cacti.isValid()) return jedecFallback("query invalid");
        auto ca = cacti.getCalibratedDieArea();
        if (ca.area_mm2 <= 0.0) return jedecFallback("no calibrated area");
        /* 1.11.45 (audit E28): the single-query form was ALGEBRAICALLY the
         * JEDEC anchor -- k = jedec/raw then raw*k cancels raw, so the CACTI
         * run was decorative and a node's bank reconfiguration was silently
         * dropped. Device scope has had the honest two-query form since
         * 1.11.14 (k from the PRESET organisation, area from the EFFECTIVE
         * one); system scope now does the same when the node reconfigures. */
        if (effective_banks > 0 &&
            static_cast<uint32_t>(effective_banks) != cfg.banks) {
            auto eff_cfg = cfg;
            eff_cfg.banks = static_cast<uint32_t>(effective_banks);
            pimid::CACTIWrapper cacti_eff(eff_cfg);
            cacti_eff.initialize();
            /* 1.11.46 (L256): a reconfiguration CACTI cannot solve (e.g. more
             * banks than its 32-bank ceiling) must not delete the area line --
             * fall through to the preset-organisation figure below and say so. */
            if (!cacti_eff.isValid() || cacti_eff.getArea() <= 0.0) {
                std::cerr << "[area] NOTE: CACTI cannot solve the effective "
                             "organisation (" << effective_banks << " banks); "
                             "reporting the preset-organisation area instead."
                          << std::endl;
            }
            if (cacti_eff.isValid() && cacti_eff.getArea() > 0.0) {
                double die = cacti_eff.getArea() * ca.k;
                if (print) {
                    std::cout << "  Die area:      " << std::fixed
                              << std::setprecision(2) << die
                              << " mm^2/die (CACTI x k at EFFECTIVE org "
                              << effective_banks << " banks, k="
                              << std::setprecision(3) << ca.k
                              << " from the preset org)"
                              << std::defaultfloat << std::endl;
                }
                return die;
            }
        }
        /* 1.11.52 (audit D036): SAY WHAT THIS NUMBER IS. When the run does
         * not reconfigure the organisation, k = jedec/raw and area = raw*k
         * cancels exactly -- the reported die is the VENDOR ANCHOR
         * (capacity / measured density) and carries no CACTI content at
         * all. 1.11.45 (E28) identified that algebra and fixed only the
         * reconfigured branch; this branch kept printing "CACTI x k", which
         * reads as a tool result. The number is unchanged (it is the right
         * number -- the vendor anchor is the sourced one); what changes is
         * that it no longer claims to be something it is not, and the raw
         * CACTI figure beside it is labelled as the divergence it measures. */
        if (print) {
            const bool anchor_only = (effective_banks <= 0);
            std::cout << "  Die area:      " << std::fixed << std::setprecision(2)
                      << ca.area_mm2 << " mm^2/die ("
                      << (anchor_only
                            ? "VENDOR ANCHOR: capacity / measured die density; "
                              "the CACTI run cancels out of this branch "
                              "(k = jedec/raw, area = raw*k), so no CACTI "
                              "structure is in this number"
                            : "CACTI x k")
                      << ", k=" << std::setprecision(3) << ca.k
                      << " FULL-DIE basis"
                      << (pimid::CACTIWrapper::vendorDieDensitySourced(memory_tech)
                              ? "" : ", density DERIVED not measured")
                      << "; raw CACTI " << std::setprecision(2)
                      << ca.raw_mm2 << " -- the gap between the tool and the "
                         "vendor anchor)" << std::defaultfloat << std::endl;
        }
        return ca.area_mm2;
    } catch (const std::exception& e) {
        /* 1.11.57 (latent A023): this catch returned 0.0 in silence, and 0.0
         * is how this function says "not a DRAM technology" -- so a tool that
         * threw would have DELETED the memory die from the system area total
         * with the same value that legitimately omits an SRAM array, and the
         * total would still have printed as if it were complete. Every failure
         * mode the try anticipates already prints (jedecFallback, the
         * effective-organisation note, the vendor-anchor line); only a genuine
         * library throw arrived here, which is why it was never seen. The
         * sibling computeNonDramMemAreaMM2 has printed on its catch all along;
         * this is the same sentence. */
        std::cerr << "[area] NOTE: " << memory_tech << " DRAM die area query "
                     "failed (" << e.what() << "); the system total OMITS the "
                     "memory die, stated here so the total is not read as "
                     "complete." << std::endl;
        return 0.0;
    }
}

/* 1.11.20 (user decisions D6): system scope prices memory the way device
 * scope does. Two things were missing here and present there, so the SAME
 * machine reported different memory power depending on which scope ran it:
 *   - the DRAM power-down descent under pim.mc.pg (r_idle -> IDD2P), and
 *   - the DQ termination charge (1.11.5), which is placement-aware.
 * Both are passed in rather than recomputed, so there is one owner of each
 * decision. r_idle < 0 means "pg off / no counter" and reproduces the old
 * background exactly; crosses_dq false means an on-die placement, where the
 * accesses never reach a DQ pin. */
/* 1.11.45 (audit E31, user ruling): RETURNS the memory system's power in
 * WATTS on the caller's wall clock, so System Total Power can include the
 * silicon System Total Area already counts. Base discipline (the 1.11.18
 * Joules-vs-Watts class): the array and termination terms are ENERGIES and
 * divide by wall_seconds -- the SAME 1.9.10 single time base every node's
 * McPAT power uses -- while the background is already a rate and adds
 * directly. wall_seconds <= 0 returns 0 and the total simply omits memory,
 * as before. */
/* 1.11.52 (audit A007/A008): ONE definition of "the die's effective bank
 * organisation". Device scope builds it from the technology's own JEDEC
 * preset (banks/bank-group x bank-groups/chip) with the YAML overrides
 * applied; system scope was passing config.num_banks -- a different knob
 * entirely (memory.organization.num_banks, used elsewhere only as the
 * SRAM/NVM per-bank multiplier), so a default DDR5 co-sim reported a
 * 16-bank die where device scope reported the preset-org die. And the die
 * PRINTED by the memory report used the preset org while the die ADDED to
 * System Total used num_banks, so the two lines of one report could
 * disagree. Both now call this. */
static int effectiveDramBanks(const std::string& memory_tech,
                              const UnifiedConfig& config) {
    try {
        pimid::RamulatorWrapper oracle("", memory_tech);
        oracle.initialize();
        int bpb = (config.banks_per_bg_override > 0)
                  ? config.banks_per_bg_override : oracle.getBanksPerBankGroup();
        int bgc = (config.bg_per_chip_override > 0)
                  ? config.bg_per_chip_override : oracle.getBankGroupsPerChip();
        int n = bpb * bgc;
        return (n > 0) ? n : 0;
    } catch (const std::exception& e) {
        std::cerr << "[area] NOTE: could not read the " << memory_tech
                  << " bank organisation (" << e.what()
                  << "); the die is reported at its preset organisation."
                  << std::endl;
        return 0;   // 0 = preset org, the documented meaning downstream
    }
}

static double reportSharedMemoryArrayEnergy(const std::string& memory_tech,
                                          uint64_t mem_rd, uint64_t mem_wr,
                                          double r_idle = -1.0,
                                          bool crosses_dq = false,
                                          bool pg_enabled = false,
                                          const std::string& device_width = "",
                                          double wall_seconds = 0.0,
                                          int effective_banks = 0,
                                          int ranks_per_channel = 1,   // 1.11.52 (A015)
                                          int channels = 1,
                                          double termination_pj_per_bit = -1.0)   // 1.11.63 (R7, gate 1173B E3)
{
    if (memory_tech.empty()) return 0.0;
    /* 1.11.52 (audit A020): A MEMORY WITH NO ACCESSES IS NOT A MEMORY WITH NO
     * POWER. This returned 0.0 silently on zero accesses, deleting the whole
     * memory section from the report and its BACKGROUND term from System
     * Total -- but DRAM standby and refresh are rates that exist whether or
     * not the workload touched the array. It is the same argument 1.11.46
     * (L236) made for AREA ("the silicon exists whether or not the workload
     * touched it"), which was fixed there and left standing here. On the
     * reference co-sim cell the memory term is 1.12 W of a 2.34 W total, so
     * an untouched memory could cut reported system power by tens of
     * percent. Zero-access memories now report their background honestly;
     * only the dynamic terms are zero. */
    if ((mem_rd + mem_wr) == 0) {
        const bool is_dram_zero = !(memory_tech == "SRAM" || memory_tech == "STT_MRAM" ||
                                    memory_tech == "PCM"  || memory_tech == "RERAM");
        if (!is_dram_zero) return 0.0;   // non-DRAM: no standby/refresh model here
        std::cout << "  [mem] " << memory_tech
                  << ": no accesses in this run -- dynamic terms are zero, but "
                     "standby and refresh are not: the background term is "
                     "reported and counted." << std::endl;
    }

    const bool is_dram = !(memory_tech == "SRAM" || memory_tech == "STT_MRAM" ||
                           memory_tech == "PCM"  || memory_tech == "RERAM");
    if (!is_dram) {
        /* NVM and SRAM arrays are priced by NVSim/CACTI on a different footing.
         * Say so rather than print a DRAM-shaped number for them. */
        std::cout << "\n--- Memory Array Energy (system) ---" << std::endl;
        std::cout << "  Technology:    " << memory_tech
                  << " -- array energy not charged in system scope yet; the "
                     "non-DRAM path is priced by its own model in device scope."
                  << std::endl;
        return 0.0;
    }

    try {
        pimid::RamulatorWrapper ram_oracle("", memory_tech);
        ram_oracle.setDeviceWidth(device_width);   // 1.11.46 (L181)
        /* 1.11.63 (R7): gate 1173B E3 caught the knob half-wired (parsed,
         * never applied). Set before initialize(), both scopes. */
        ram_oracle.setTerminationOverridePJPerBit(termination_pj_per_bit);
        ram_oracle.initialize();
        /* Intensive per-access accessors. getArrayReadEnergyNJ folds activation
         * and column access, so act/pre are NOT added separately -- adding them
         * would double-count within the array term itself. */
        const double rd_nj = ram_oracle.getArrayReadEnergyNJ();
        const double wr_nj = ram_oracle.getArrayWriteEnergyNJ();
        /* 1.11.20 (D6 + D13 + D15): same background device scope reports --
         * population-scaled and state-aware. r_idle < 0 means the residency
         * was never measured, which is NOT the same as measured-zero: the
         * former reports pure active standby, the latter is a real busy
         * device. */
        const int bg_units = ram_oracle.getBackgroundUnits(
            device_width, ranks_per_channel, channels);   // 1.11.52 (A015)
        /* 1.11.57 (audit round 3, A002): the SAME population as the unit
         * count on the line above. The 1.11.52 A015 fix added
         * ranks_per_channel and channels to both accessors and then passed
         * them to only one of the two adjacent calls, so the printed unit
         * count described devices x ranks x channels while the printed power
         * described a single rank on a single channel. Reachable with no
         * extra knob on GDDR6, whose preset carries channels_ = 2: the
         * memory power read 2x low against a System Total Area that counts
         * both dies, and device scope on the same machine reported 2x more
         * than system scope for one memory. */
        const double bg_mw = ram_oracle.getBackgroundSystemMW(
            (r_idle > 0.0 ? r_idle : 0.0), pg_enabled, device_width,
            ranks_per_channel, channels);
        /* 1.11.20 (D6): DQ termination, placement-aware like 1.11.5. */
        // 1.11.58: driver switching + PHY + termination, as device scope.
        // 1.11.60 (A009/A010 revert, user-approved): termination-only, as
        // device scope. CACTI-IO's driver+PHY prints informational below.
        /* 1.11.63 (R7): read/write split, as device scope. */
        const double iface_term_rd_nj = crosses_dq ? ram_oracle.getTerminationEnergyNJ(false) : 0.0;
        const double iface_term_wr_nj = crosses_dq ? ram_oracle.getTerminationEnergyNJ(true)  : 0.0;
        const double iface_drv_nj  = crosses_dq ? ram_oracle.getInterfaceDynamicEnergyNJ() : 0.0;

        const double total_rd_mj = rd_nj * static_cast<double>(mem_rd) / 1e6;
        const double total_wr_mj = wr_nj * static_cast<double>(mem_wr) / 1e6;
        /* 1.11.60 (audit round 4, A011): named for what it holds. Since
         * 1.11.58 this is termination PLUS driver PLUS PHY; its previous name
         * described only the first of the three, and the printed label below
         * inherited that name. */
        const double total_iface_mj =
            (iface_term_rd_nj * static_cast<double>(mem_rd)
             + iface_term_wr_nj * static_cast<double>(mem_wr)) / 1e6;

        std::cout << "\n--- Memory Array Energy (system) ---" << std::endl;
        std::cout << "  Technology:    " << memory_tech
                  << " (Ramulator2 energy model)" << std::endl;
        std::cout << "  Accesses:      " << (mem_rd + mem_wr)
                  << " on this node's memory" << std::endl;
        std::cout << "  Per-access:    read=" << std::fixed << std::setprecision(3)
                  << rd_nj << " nJ, write=" << wr_nj << " nJ (incl act+col)"
                  << std::endl;
        std::cout << "  Background:    " << std::setprecision(3) << bg_mw
                  << " mW (standby+refresh over " << bg_units << " "
                  << (memory_tech.substr(0, 3) == "HBM" ? "channels/stack"
                                                        : "chips/rank")
                  << (r_idle > 0.0
                          ? (pg_enabled ? ", IDD2N page-close then IDD2P "
                                          "power-down at measured idle"
                                        : ", IDD2N page-close at measured idle")
                          : "")
                  << ")" << std::endl;
        /* 1.11.56 (audit D074): name the one term for what it is. Background
         * already CONTAINS refresh (the relation is vdd*idd3n + refreshMW),
         * and a DRAM array's standby current IS its leakage -- there is no
         * separate leakage term to add. Device scope carries the same note. */
        std::cout << "    (this figure already includes refresh, and it IS the "
                     "array's leakage -- these are one quantity, not three)"
                  << std::endl;
        std::cout << "  DQ interface:  " << std::setprecision(3)
                  << "rd=" << iface_term_rd_nj << " nJ, wr=" << iface_term_wr_nj
                  << " nJ per access ("
                  << (crosses_dq ? "accesses cross the DQ pins at this placement"
                                 : "on-die placement: no DQ crossing, no interface charge")
                  << ")" << std::endl;
        if (crosses_dq) {
            std::cout << "    termination: rd=" << iface_term_rd_nj
                      << " nJ (DRAM RON -> RX RTT_NOM class), wr="
                      << iface_term_wr_nj
                      << " nJ (ctrl RON -> DRAM RTT_WR class) [R7 split]"
                         "  CACTI-IO driver+PHY: " << iface_drv_nj
                      << " nJ [INFORMATIONAL, NOT charged -- 1.11.60 revert:"
                         " McPAT already prices the PHY here]" << std::endl;
            double t_lo = 0.0, t_hi = 0.0; std::string t_prov;   // 1.11.59
            if (ram_oracle.getTerminationEnergyBandNJ(t_lo, t_hi, t_prov))
                std::cout << "    termination BAND " << t_lo << "-" << t_hi
                          << " nJ [" << t_prov << "]" << std::endl;
        }
        /* 1.11.60 (audit round 4, A011): same mislabel as device scope, same
         * correction -- what is printed here is the whole DQ interface since
         * 1.11.58, not the termination component of it. */
        std::cout << "  Array dynamic: " << std::setprecision(3)
                  << (total_rd_mj + total_wr_mj + total_iface_mj)
                  << " mJ (rd=" << total_rd_mj
                  << " + wr=" << total_wr_mj
                  << " + iface=" << total_iface_mj
                  << " [termination + driver/PHY])"
                  << std::defaultfloat << std::endl;
        computeDramDieAreaMM2(memory_tech, /*print=*/true,
                              effective_banks);   // 1.11.9; 1.11.52 A008: same org as the total
        /* 1.11.45 (E31): the wattage this report just printed, on the wall
         * clock. Energy terms divide by the same seconds node power uses;
         * background is already a rate. */
        if (wall_seconds > 0.0) {
            return (total_rd_mj + total_wr_mj + total_iface_mj) / 1000.0 / wall_seconds
                   + bg_mw / 1000.0;
        }
        return 0.0;
    } catch (const std::exception& e) {
        std::cerr << "  [Memory array] Ramulator2 query failed: " << e.what()
                  << std::endl;
    }
    return 0.0;
}

static void runPerNodePowerAnalysis(const UnifiedConfig& config,
                                     const ZSimParsedOutput& zsim_stats,
                                     const std::string& output_dir) {
    using McPAT = pimid::McPATWrapper;

    // If no system nodes or no stats, fall back to original
    if (config.system_nodes.empty() || (zsim_stats.cycles == 0 && zsim_stats.instrs == 0)) {
        runPowerAnalysis(config, zsim_stats, output_dir);
        return;
    }

    std::cout << "\n--- Per-Node Power ---" << std::endl;

    struct NodePowerResult {
        std::string name;
        std::string core_desc;
        double total_power = 0;
        double dynamic_power = 0;
        double leakage_power = 0;
        double area = 0;
        bool valid = false;
    };
    std::vector<NodePowerResult> results;

    // Use global stats as a fallback when per-core stats unavailable
    uint64_t total_cycles = zsim_stats.cycles > 0 ? zsim_stats.cycles : 1;
    uint64_t total_instrs = zsim_stats.instrs > 0 ? zsim_stats.instrs : 1;

    // 1.9.10 power-integration fix: derive a single wall-clock TIME (seconds) that
    // every node is priced over, from the contention-INCLUSIVE per-group wall cycles
    // in their own clock domains. Previously each node's McPAT runtime was
    // `zsim_stats.cycles * core_frac`, where zsim_stats.cycles is the FIRST (host)
    // core's contention-EXCLUDED unhalted count. For memory-latency-bound kernels
    // (bfs: 5M unhalted but ~790M elapsed) and offloaded co-sim runs (host offloads
    // in ~41K cycles while the device runs for hundreds of millions), that runtime
    // was 1-3 orders of magnitude too short, so accesses/cycle became nonphysical
    // and McPAT dynamic power exploded to hundreds/thousands of Watts (or, when the
    // host ran long, collapsed below the host-only baseline). Pricing each node over
    // the true wall-clock in its own clock domain fixes all three failure modes.
    double wall_seconds = 0.0;
    for (const auto& node : config.system_nodes) {
        if (node.num_cores == 0 || node.frequency_mhz <= 0) continue;
        uint64_t node_wall =
            (node.role == UnifiedConfig::SystemNode::HOST)
                ? zsim_stats.host_wall_cycles : zsim_stats.dev_wall_cycles;
        if (node_wall == 0) continue;
        double t = static_cast<double>(node_wall) / (node.frequency_mhz * 1e6);
        if (t > wall_seconds) wall_seconds = t;
    }

    const std::map<std::string, double>& overrides = config.mcpat_overrides;
    auto ov_get_int = [&overrides](const std::string& key, int fallback) -> int {
        auto it = overrides.find(key);
        return (it != overrides.end()) ? static_cast<int>(it->second) : fallback;
    };

    /* 1.11.16 (verification audit): "priced exactly once" must be a property
     * of the CODE, not of the single-host config shape -- a two-host YAML
     * parses fine today and would have priced the same crossing bytes on
     * every HOST node. First host carries the link; the warning latch keeps
     * the unknown-link message to one line instead of one per node. */
    bool xing_link_charged = false;
    bool xing_warned_unknown = false;

    for (const auto& node : config.system_nodes) {
        if (node.num_cores == 0) continue;  // memory-only, skip

        NodePowerResult result;
        result.name = node.name;

        McPAT::SystemConfig mcfg;
        mcfg.num_cores = node.num_cores;
        mcfg.core_clock_mhz = node.frequency_mhz;
        mcfg.tech_node_nm = validateTechNodeNm(node.tech_node_nm, "system node");
        mcfg.temperature_k = config.temperature_k;   // 1.11.52

        /* 1.11.2: same placement x technology -> process family matrix as the
         * device-scope path. Placement is global (pim.placement), the memory
         * technology is the node's own. Host nodes are logic by definition. */
        if (node.role == UnifiedConfig::SystemNode::DEVICE) {
            bool dram_family_tech =
                !(node.memory_tech == "SRAM" || node.memory_tech == "STT_MRAM" ||
                  node.memory_tech == "PCM"  || node.memory_tech == "RERAM");
            /* 1.11.17: channel-centric ladder parity with the device-scope
             * site (LPDDR/GDDR/HBM channel tier lives on the DRAM die). */
            bool channel_centric =
                node.memory_tech.rfind("LPDDR", 0) == 0 ||
                node.memory_tech.rfind("GDDR", 0) == 0 ||
                node.memory_tech.rfind("HBM", 0) == 0;
            /* 1.11.19 (D2+D3): same three interface tiers as device scope. */
            {
                using Tier = McPAT::SystemConfig::MCPhyTier;
                const int lvl = config.pe_hierarchy_level;
                bool interposer_tier = (lvl == 6) || (lvl == 5 && channel_centric);
                if (lvl >= 0 && lvl <= 3)  mcfg.mc_phy_tier = Tier::NONE;
                else if (interposer_tier)  mcfg.mc_phy_tier = Tier::INTERPOSER;
                else                       mcfg.mc_phy_tier = Tier::OFFCHIP;
            }
            /* 1.11.51 (N1): pitch gating hoisted out of the family branch
             * here too -- geometry, not family; mirrors the device path. */
            if (config.pe_hierarchy_level == 0) {
                mcfg.subarray_pitch_factor = config.subarray_pitch_factor;
                /* 1.11.51 (N3): same banded derived default as the device
                 * path; see there for the sources. */
                if (config.subarray_pitch_factor == 1.0 && dram_family_tech) {
                    DRAMGenClass g3 = getDRAMGenClass(node.memory_tech);
                    mcfg.subarray_pitch_factor = 1.25;
                    std::cout << "  [tech] " << node.name
                              << ": SUBARRAY pitch derived default 1.25 "
                                 "(band [1.25, ~4]; FIMDRAM low end), gen "
                              << g3.cls << " 2F ~= "
                              << (2.0 * dramGenFeatureNm(g3.cls)) << " nm."
                              << std::endl;
                }
            } else {
                mcfg.subarray_pitch_factor = 1.0;
                if (config.subarray_pitch_factor != 1.0)
                    std::cout << "  [tech] node: power.subarray_pitch_factor="
                              << config.subarray_pitch_factor
                              << " IGNORED at " << config.placement_level
                              << " placement (pitch penalty is SUBARRAY-only)."
                              << std::endl;
            }
            if (dram_family_tech &&
                ((config.pe_hierarchy_level >= 0 && config.pe_hierarchy_level <= 3) ||
                 (config.pe_hierarchy_level == 5 && channel_centric))) {
                mcfg.process_family = 1;
                DRAMGenClass ngc = getDRAMGenClass(node.memory_tech);
                mcfg.dram_periph_table_nm = ngc.cacti_table_nm;
                /* 1.11.51 (N2): same pinning as the device path -- an on-die
                 * node prices at its die's generation; the logic node is for
                 * off-die parts. */
                if (mcfg.tech_node_nm != ngc.cacti_table_nm) {
                    std::cout << "  [tech] " << node.name
                              << ": on-DRAM-die PE process pinned to generation "
                              << ngc.cls << " (CACTI " << ngc.cacti_table_nm
                              << "nm table); node tech_node_nm IGNORED here."
                              << std::endl;
                    mcfg.tech_node_nm = ngc.cacti_table_nm;
                }
                std::cout << "  [tech] " << node.name
                          << ": PE process family DRAM-periphery (placement "
                          << config.placement_level << " on " << node.memory_tech
                          << ", class " << ngc.cls << "; factors from CACTI "
                          << ngc.cacti_table_nm << "nm hp/comm-dram columns)"
                          << std::endl;
            }
            /* 1.11.17 tried to end the system-scope divergence by COPYING
             * the device-scope logic here; 1.11.52 (audit A001-A003) ends it
             * by DELETING the copy. The copy had drifted on all three of its
             * claims -- it refused the corner instead of mapping it to
             * lp-dram, priced the area factor from literals instead of the
             * tables, and guarded the clock with a literal 700 MHz -- while
             * its comment said "Same behavior as the device-scope site". One
             * owner, called from both scopes. */
            applyCornerAndPeripheryPricing(
                mcfg, config, config.mcpat_overrides,
                getDRAMGenClass(node.memory_tech),
                node.frequency_mhz, node.name + ": ");
        } else if (node.role == UnifiedConfig::SystemNode::HOST) {
            /* 1.11.49 (gate 1159I Y2, completing L119): the corner block above
             * is DEVICE-only, so a system-scope HOST node kept device_type=0
             * regardless of power.device_corner -- every emitted XML carried
             * value="0" under corner=lstp, which is how the gate caught it.
             * The host is a LOGIC part: no DRAM-periphery refusal applies and
             * the corner maps directly, same as the dual-McPAT host path. */
            mcfg.process_family = 0;
            int corner = (config.device_corner == "lstp") ? 1
                       : (config.device_corner == "lop")  ? 2 : 0;
            if (corner != 0) {
                std::cout << "  [tech] " << node.name << ": device corner "
                          << config.device_corner
                          << " applied to the host's logic components."
                          << std::endl;
            }
            mcfg.device_type = corner;
        }

        McPAT::DeviceProfile profile;
        bool is_alu = false;

        // Host nodes carry their core type in node.core_type; device nodes
        // carry it in node.pe_type (see YAML parse at hosts[].core_type vs
        // devices[].pe_type). Selecting the wrong field was making ALU device
        // nodes look like OOO hosts, which made the dual-McPAT device XML
        // emit OOO + caches and triggered downstream errors.
        const std::string& effective_type =
            (node.role == UnifiedConfig::SystemNode::DEVICE)
                ? node.pe_type : node.core_type;

        if (effective_type == "ooo_core") {
            mcfg.pipeline_depth = 19; mcfg.issue_width = 4;
            mcfg.num_alus = 4; mcfg.num_muls = 2; mcfg.num_fpus = 2;
            profile = McPAT::DeviceProfile::OOO;
            result.core_desc = "OOO";
        } else if (effective_type == "alu_core") {
            mcfg.pipeline_depth = 5; mcfg.issue_width = 1;
            mcfg.num_alus = 1; mcfg.num_muls = 0; mcfg.num_fpus = 0;
            profile = McPAT::DeviceProfile::DEVICE_ALU;
            is_alu = true;
            result.core_desc = "ALU";
        } else {
            mcfg.pipeline_depth = 14; mcfg.issue_width = 1;
            mcfg.num_alus = 3; mcfg.num_muls = 1; mcfg.num_fpus = 1;
            profile = McPAT::DeviceProfile::DEVICE_INORDER;
            result.core_desc = (effective_type == "in_order_core") ? "InOrder" : "Simple";
        }
        /* 1.11.51 (L214): same rule per node -- an element that declares no
         * FPU prices none, on every profile. Node-scoped flag (E23/E24);
         * an explicit num_fpus override below still wins. */
        if (!node.pe_has_fpu && mcfg.num_fpus > 0) {
            std::cout << "  [power] " << node.name
                      << ": floating_point=false -- FPU removed from the power "
                         "model (profile default num_fpus=" << mcfg.num_fpus
                      << " -> 0)." << std::endl;
            mcfg.num_fpus = 0;
        }

        /* 1.9.32: reference class follows the node's role -- a device node
         * sits on the memory die, a host node is a server part. */
        mcfg.device_scope   = (node.role == UnifiedConfig::SystemNode::DEVICE);
        mcfg.pe_lanes       = config.pe_lanes;
        mcfg.pe_element_bits= config.alu_operand_width;   // ONE width, shared
        /* 1.11.52 (audit C031): the NODE's flag, not the global one. The FPU
         * REMOVAL above keys on node.pe_has_fpu while this fed the ALU
         * profile's FPU emission (num_fpus = pe_has_fp ? lanes : 0, and
         * phy_regs_frf) from the GLOBAL pim.pe.floating_point -- so on the
         * ALU profile a node declared FPU-less could still emit `lanes`
         * FPUs, and because the soft-float fold gates on config_.num_fpus
         * (which IS 0), the same FP ops were then ALSO charged as integer
         * work. One flag, node-scoped, for both. */
        mcfg.pe_has_fp      = node.pe_has_fpu;
        /* 1.11.51 (L215/L223): node-scoped emulation cost for the energy fold. */
        mcfg.fp_emul_cycles = (int)node.pe_fp_emul_cycles;
        mcfg.pe_imem_bytes  = config.pe_imem_bytes;

        // Apply overrides
        mcfg.pipeline_depth = ov_get_int("pipeline_depth", mcfg.pipeline_depth);
        mcfg.issue_width = ov_get_int("issue_width", mcfg.issue_width);
        mcfg.num_alus = ov_get_int("num_alus", mcfg.num_alus);
        mcfg.num_muls = ov_get_int("num_muls", mcfg.num_muls);
        mcfg.num_fpus = ov_get_int("num_fpus", mcfg.num_fpus);
        /* 1.11.49 (gate 1159J Z1): the default here was a literal 0, which
         * silently clobbered the corner mapping the role branches above had
         * just written -- the "[tech] ... corner applied" line printed and the
         * emitted XML still carried device_type=0. Default to the value
         * already on mcfg; an explicit per-node override still wins. */
        mcfg.device_type = ov_get_int("device_type", mcfg.device_type);
        mcfg.longer_channel_device = ov_get_int("longer_channel_device", 1);
        mcfg.number_hardware_threads = ov_get_int("number_hardware_threads", 1);
        mcfg.interconnect_projection_type =
        ov_get_int("interconnect_projection_type", config.interconnect_projection);  // 1.11.30 E5

        // Caches
        if (is_alu) {
            mcfg.l1i_size_bytes = 0; mcfg.l1d_size_bytes = 0;
            mcfg.l2_size_bytes = 0; mcfg.l3_size_bytes = 0;
        } else {
            mcfg.l1i_size_bytes = node.l1i_kb * 1024ULL;
            mcfg.l1d_size_bytes = node.l1d_kb * 1024ULL;
            mcfg.l2_size_bytes = node.enable_l2 ? node.l2_kb * 1024ULL : 0;
            mcfg.l3_size_bytes = node.enable_l3 ? node.l3_kb * 1024ULL : 0;
        }

        /* MC. 1.11.52 (audit A004): the DEVICE node's controller count is
         * the same model quantity device scope derives -- one controller per
         * REGION of the placement tree (1.10.5), not the literal 1 this path
         * used to emit. The literal made MC area and leakage differ between
         * the two scopes by the endpoint count (tens to hundreds) for one
         * machine. A HOST node keeps a single controller: it has no PE
         * placement tree, and its channel count is the memory-side quantity
         * priced by the MC-tech parameters, not by this field. */
        if (node.role == UnifiedConfig::SystemNode::DEVICE) {
            if (config.htree_endpoints > 0) {
                mcfg.num_memory_controllers = config.htree_endpoints;
            } else if (config.pe_mc_enabled && config.pes_per_mc > 0) {
                mcfg.num_memory_controllers =
                    std::max(1, config.num_pes / config.pes_per_mc);
            } else {
                mcfg.num_memory_controllers = 1;
            }
            if (mcfg.num_memory_controllers != 1)
                std::cout << "  [power] " << node.name << ": "
                          << mcfg.num_memory_controllers
                          << " element controllers (one per placement-tree "
                             "region, as in device scope)" << std::endl;
        } else {
            mcfg.num_memory_controllers = 1;
        }
        mcfg.mc_clock_mhz = node.frequency_mhz / 2.0;

        // NoC (for device nodes with PEs)
        if (node.role == UnifiedConfig::SystemNode::DEVICE && node.num_pes > 1) {
            mcfg.has_noc = true;
            mcfg.noc_clock_mhz = node.frequency_mhz;
            mcfg.noc_num_routers = node.num_pes;
            int grid_side = static_cast<int>(std::ceil(std::sqrt(node.num_pes)));
            mcfg.noc_num_rows = grid_side;
            mcfg.noc_num_cols = grid_side;
        } else {
            mcfg.has_noc = false;
        }

        try {
            McPAT mcpat(mcfg);
            mcpat.setDeviceProfile(profile);

            /* 1.11.15 (audit): power gating never reached system scope --
             * setPGSpec had one call site, in the device-scope path. Device
             * nodes here get the same residency-driven spec.
             *
             * 1.11.16 (verification audit): the 1.11.15 block keyed on the
             * GLOBAL config.pg_* flags, which no documented system-scope
             * config can set (they parse from top-level pim:/noc: keys, the
             * device-scope schema) -- the block was dead for every shipped
             * system YAML. Per-node devices[].pim.pe.pg / .pim.mc.pg /
             * .noc.pg now drive it (global flags still honored as an OR for
             * mixed configs), and the applied residencies are PRINTED with
             * the node name -- a PG run must be tellable from a non-PG run
             * in its own log. */
            {
                bool pe_pg  = node.pg_pe  || config.pg_pe;
                bool noc_pg = node.pg_noc || config.pg_noc;
                bool mc_pg  = node.pg_mc  || config.pg_mc;
                if (node.role == UnifiedConfig::SystemNode::DEVICE &&
                    (pe_pg || noc_pg || mc_pg) && zsim_stats.pg_window() > 0) {
                    pimid::McPATWrapper::PGSpec nps;
                    double ph = static_cast<double>(zsim_stats.pg_window());
                    if (pe_pg && node.num_cores > 0) {
                        double meanActive =
                            static_cast<double>(zsim_stats.dev.pgActivePhases)
                            / (node.num_cores * ph);
                        nps.pg_core = true;
                        nps.r_core = 1.0 - std::min(1.0, meanActive);
                    }
                    if (noc_pg) {
                        nps.pg_noc = true;
                        nps.r_noc = 1.0 - std::min(1.0,
                            static_cast<double>(zsim_stats.pg_noc_active) / ph);
                    }
                    if (mc_pg) {
                        nps.pg_mc = true;
                        nps.r_mc = 1.0 - std::min(1.0,
                            static_cast<double>(zsim_stats.pg_devmc_active) / ph);
                    }
                    mcpat.setPGSpec(nps);
                    std::cout << "  [pg] " << node.name
                              << " residencies (idle fraction):"
                              << " pe=" << (nps.pg_core ? nps.r_core : 0.0)
                              << (nps.pg_core ? "" : " (pg off)")
                              << " noc=" << (nps.pg_noc ? nps.r_noc : 0.0)
                              << (nps.pg_noc ? "" : " (pg off)")
                              << " mc=" << (nps.pg_mc ? nps.r_mc : 0.0)
                              << (nps.pg_mc ? "" : " (pg off)")
                              << std::endl;
                }
                /* 1.11.20 (user decision D7): the HOST node had no PG surface
                 * at all -- the branch above is DEVICE-only, so a system-scope
                 * host could never gate anything. Per the 2026-08-14 decree the
                 * host gates AS AN ENTIRE PIECE: cores and host MC share one
                 * domain and one residency, rather than being credited
                 * separately.
                 *
                 * The piece is idle only when NOTHING in it is busy, so the
                 * active signal is the UNION of core-retire activity and
                 * host-MC activity. We hold two per-phase counters but not
                 * their union, and union >= max(a, b). Taking max() is the
                 * CONSERVATIVE choice: it can only overstate activity, i.e.
                 * under-credit the gating. Stated here rather than left for a
                 * reader to infer. */
                if (node.role == UnifiedConfig::SystemNode::HOST &&
                    node.pg_host && zsim_stats.pg_window() > 0) {
                    pimid::McPATWrapper::PGSpec hps;
                    const double ph = static_cast<double>(zsim_stats.pg_window());
                    const double act = std::max(
                        static_cast<double>(zsim_stats.pg_anycore_active),
                        static_cast<double>(zsim_stats.pg_hostmc_active));
                    const double r = 1.0 - std::min(1.0, act / ph);
                    hps.pg_core = true;  hps.r_core = r;
                    hps.pg_mc   = true;  hps.r_mc   = r;
                    mcpat.setPGSpec(hps);
                    std::cout << "  [pg] " << node.name
                              << " host gates as ONE piece (cores + host MC),"
                              << " idle residency " << r
                              << " (union bounded below by max(core, mc)"
                              << " -- conservative, under-credits)"
                              << std::endl;
                }
            }

            /* 1.11.7 (#85): crossing energy in the path co-sim actually
             * takes. BOTH ends of the link carry a controller (audit: only
             * the host end was ever priced, and only in the unreachable
             * trace path); the link dynamic itself is byte-driven inside
             * the McPAT fork from the measured plugin counters -- zero
             * traffic prices to zero. pJ/bit from the cited per-link-type
             * table; unknown types are a fatal config error unless the
             * user supplies pcie_pj_per_bit_override. */
            {
                /* 1.11.47 (FIX-PRE-FLEET L176, completion): this block never
                 * consulted power.pcie.enabled -- "ignored on the only path a
                 * co-simulation takes", the finding's own words. The first
                 * half of the fix (not force-overriding an explicit false)
                 * shipped earlier in this release; gating the CONSUMER is the
                 * half gate 1159C proved missing. Timing keeps the link
                 * either way -- this is energy pricing only. */
                if (!config.pcie_enabled) {
                    static bool said_disabled = false;
                    if (!said_disabled) {
                        said_disabled = true;
                        std::cout << "  [xing] link ENERGY pricing disabled "
                                     "(power.pcie.enabled=false, user choice); "
                                     "the declared link keeps its timing."
                                  << std::endl;
                    }
                } else {
                /* 1.11.15 (audit), three corrections in this block.
                 * (1) FLUSH bytes leave the link: the coherence flush is the
                 * host writing back its own dirty lines to memory -- it rides
                 * the memory channel, not the PCIe/CXL PHY, and it was
                 * 99.999% of the priced bytes. It is charged as memory writes
                 * below instead. (2) The link is priced ONCE: the pJ/bit
                 * table is an end-to-end link figure, so only the host end
                 * carries transferred_bytes; the device end keeps its
                 * controller (area+leakage) with zero transfer. (3) Unknown
                 * link types no longer abort AFTER the simulation ran --
                 * validation is loud but the run's results survive, with the
                 * link dynamic explicitly marked unpriced. */
                /* 1.11.16 (verification audit), four corrections.
                 * (a) pj_per_bit_override now OVERRIDES: a knob named
                 *     override that was only consulted when the table failed
                 *     was a config-surface trap.
                 * (b) The unknown-link warning is latched (was once per
                 *     node) and only computed where it matters.
                 * (c) An unpriced link is MARKED in the [xing] summary line
                 *     on stdout, not just a stderr note a batch harness
                 *     drops.
                 * (d) The link is charged to the FIRST host node only
                 *     (xing_link_charged), and the byte parenthetical now
                 *     sums to the number in front of it -- flush is reported
                 *     as the separate reattributed quantity it is. */
                /* 1.11.40 (audit E18, user ruling): the flush is BACK on the
                 * link -- but only when the shared array is the DEVICE's.
                 *
                 * 1.11.15 took it off with this reason: "the coherence flush
                 * is the host writing back its own dirty lines to memory --
                 * it rides the memory channel, not the PCIe/CXL PHY". Every
                 * word of that is true, and it settles nothing on its own,
                 * because it does not say WHERE that memory channel is. In a
                 * COUPLED system the host and device address one array and
                 * that array is the device's DRAM (see the memory block
                 * below: tech = d_tech.empty() ? h_tech : d_tech, "host and
                 * device accesses land on the same silicon"). The memory
                 * channel the flush rides is then on the FAR SIDE of the
                 * link, so the bytes traverse the PHY and land in DRAM. Two
                 * pieces of silicon, two energies; charging both is not
                 * double-counting, and charging only the array is an
                 * omission. In a DECOUPLED system the host really does write
                 * back to its own memory, nothing crosses, and 1.11.15's
                 * reason holds exactly -- that path is unchanged.
                 *
                 * Corroboration from the other coherence mode: mode=separate
                 * is documented as "the 1.7.1 bridge bulk-DMA path already
                 * prices the crossing". Case 2 prices it; Case 1 did not; and
                 * Case 1 is the mode whose memory IS the shared device array.
                 *
                 * SCALE: on the measured co-sim cell this is 128 B -> 16.8 MB,
                 * 7.2 nJ -> ~940 uJ at 7 pJ/bit. The flush is the offload's
                 * data-PREP cost -- the price of using the device -- so
                 * under-charging it flattered PIM, the direction that matters
                 * most in a paper arguing for PIM. Note also that the
                 * footprint's stated "conservative-against-PIM" upper bound
                 * (whole footprint treated as dirty, main.cpp coherence
                 * defaults) only ever applied to the array charge; on the link
                 * it was not conservative but absent. */
                const bool xing_coupled = sharedMemoryCoupled(config);
                /* 1.11.47 (FIX-PRE-FLEET L190): PROTOCOL FRAMING IS TRAFFIC.
                 * The timing model charges pcie_header_bytes per transaction;
                 * the energy model priced payload only, and xingCount -- the
                 * measured transaction count that supplies exactly this term
                 * -- was parsed and used for nothing but a log line. Framing
                 * bytes cross the same SerDes as payload and are priced at
                 * the same pJ/bit. */
                uint64_t xframing = zsim_stats.xing_count *
                                    static_cast<uint64_t>(
                                        config.pcie_header_bytes > 0
                                            ? config.pcie_header_bytes : 0);
                uint64_t xbytes = zsim_stats.xing_h2d_bytes +
                                  zsim_stats.xing_d2h_bytes +
                                  (xing_coupled ? zsim_stats.xing_flush_bytes : 0) +
                                  xframing;
                McPAT::McPATWrapper::LinkEnergyBand pjband =
                    McPAT::McPATWrapper::linkEnergyBandPJPerBit(config.pcie_link_type);
                double pjbit;
                bool pj_overridden = false;
                if (config.pcie_pj_per_bit_override >= 0.0) {
                    pjbit = config.pcie_pj_per_bit_override;
                    /* 1.11.56 (audit A030): the band beside the value must
                     * describe the value. pjband was read from the link-type
                     * table before this branch and never revised, so with
                     * power.pcie.pj_per_bit_override set the printed number
                     * came from the user and the bracket after it from the
                     * table -- two different quantities on one line, and on
                     * an override far from the table arbitrarily inconsistent.
                     * An overridden figure has no sourced range: say that
                     * instead of borrowing one. */
                    pjband = McPAT::McPATWrapper::LinkEnergyBand();
                    pj_overridden = true;
                } else {
                    pjbit = McPAT::linkEnergyPJPerBit(config.pcie_link_type);
                }
                bool link_unpriced = false;
                if (pjbit < 0.0) {
                    if (!xing_warned_unknown) {
                        xing_warned_unknown = true;
                        std::cerr << "[power] WARNING: unknown link type '"
                                  << config.pcie_link_type << "' and no "
                                     "power.pcie.pj_per_bit_override given -- link "
                                     "transfer dynamic UNPRICED (0). Known: "
                                     "pcie_gen3/4/5, cxl*, nvlink*, interposer."
                                  << std::endl;
                    }
                    pjbit = 0.0;
                    link_unpriced = true;
                }
                bool charge_here =
                    (node.role == UnifiedConfig::SystemNode::HOST) &&
                    !xing_link_charged;
                if (charge_here) xing_link_charged = true;
                McPAT::PCIeStats ps;
                /* 1.11.34: the HOST carries one controller PER ATTACHED DEVICE.
                 * It was hardcoded to 1, so a multi-device system reported a
                 * single controller's area and leakage on the host side however
                 * many devices hung off it -- a silent undercount that scales
                 * with device count, and multi_device.yaml is a shipped example.
                 * A device end still has exactly one: its own link to the host.
                 *
                 * NOT FIXED, and stated rather than papered over: the crossing
                 * counters (xing_h2d_bytes / xing_d2h_bytes) are GLOBAL, so
                 * per-device link traffic cannot be attributed and the dynamic
                 * term is still charged once, at the host, on the total. With
                 * mixed link types across devices, one pJ/bit is applied to all.
                 * Both need per-device counters in the plugin; inventing an
                 * attribution here would be worse than the known limitation. */
                int link_units = 1;
                if (node.role == UnifiedConfig::SystemNode::HOST) {
                    int ndev = 0;
                    for (const auto& n2 : config.system_nodes)
                        if (n2.role == UnifiedConfig::SystemNode::DEVICE) ++ndev;
                    link_units = (ndev > 0) ? ndev : 1;
                }
                ps.number_units = link_units;
                /* 1.11.34 (UCIe): an interposer is counted in MODULES of 64
                 * lanes (advanced package), not in PCIe lanes. Using the PCIe
                 * lane default for it compared different quantities. */
                {
                    const int lpm = McPAT::linkLanesPerModule(config.pcie_link_type);
                    ps.num_channels = (lpm > 0) ? lpm : config.pcie_num_lanes;
                }
                /* 1.11.40 (audit E19, user ruling): the link's duty cycle is a
                 * quantity THE SIMULATION REVEALS, not a knob and not a
                 * default. It is bytes-crossed over time-elapsed, and both
                 * terms are already measured here:
                 *
                 *   duty = (xbytes / link_bytes_per_s) / wall_seconds
                 *
                 * McPAT applies duty_cycle ONLY to the TDP branch
                 * (iocontrollers.cc:329), so this sets what "peak link
                 * dynamic" means; runtime dynamic already comes from the same
                 * measured bytes via link_pj_per_bit, which is why
                 * total_load_perc stays 0 -- the legacy scaling would
                 * double-count a number we now know.
                 *
                 * WHAT IT REPLACES: a hand-set power.pcie.duty_cycle whose
                 * 0.01 default carried no citation and was wrong in BOTH
                 * regimes, in opposite directions. On the reference co-sim run
                 * (host 1758987 cyc @ 2 GHz, 63 GB/s):
                 *     flush excluded from the link   true duty 2.31e-06
                 *                                    (0.01 was 4329x HIGH)
                 *     flush included (E18)           true duty 3.03e-01
                 *                                    (0.01 was 30x LOW)
                 * No constant can be right across that, and the derived value
                 * moves with the workload -- which is what makes it measured
                 * rather than configured.
                 *
                 * The YAML key survives as an explicit OVERRIDE only, and is
                 * printed when used so it can never be silently in force. */
                double link_bps = config.pcie_bandwidth_GBs * 1e9;
                double duty_derived = -1.0;
                if (link_bps > 0.0 && wall_seconds > 0.0) {
                    duty_derived = (static_cast<double>(xbytes) / link_bps) / wall_seconds;
                    if (duty_derived > 1.0) duty_derived = 1.0;   // saturated link
                    if (duty_derived < 0.0) duty_derived = 0.0;
                }
                if (config.pcie_duty_cycle_user_set) {
                    ps.duty_cycle = config.pcie_duty_cycle;
                    std::cout << "  [xing] " << node.name
                              << ": link duty OVERRIDE " << ps.duty_cycle
                              << " (measured would be ";
                    if (duty_derived >= 0.0) std::cout << duty_derived;
                    else                     std::cout << "underivable";
                    std::cout << ")" << std::endl;
                } else if (duty_derived >= 0.0) {
                    ps.duty_cycle = duty_derived;
                } else {
                    /* Cannot derive: no bandwidth or no elapsed time. Say so --
                     * a silent fallback here is exactly the placeholder this
                     * change exists to remove. */
                    ps.duty_cycle = 1.0;
                    std::cerr << "[power] WARNING: link duty cycle NOT derivable"
                                 " (bandwidth_GBs=" << config.pcie_bandwidth_GBs
                              << ", wall_seconds=" << wall_seconds
                              << "); reporting peak at full rate." << std::endl;
                }
                /* 1.11.41 (E20 completion). perc_load = 0 was justified as
                 * "the legacy scaling would double-count a number we now know"
                 * -- but it also zeroed the CONTROLLER's own digital dynamic
                 * (LTSSM, replay buffers, flit assembly), which made the E20
                 * assignment-vs-addition fix inert: adding to a zeroed base.
                 * The two terms are different silicon and BOTH are now real:
                 *   controller digital = McPAT's model x MEASURED duty (E19)
                 *   SerDes             = measured bytes x pJ/bit (added in
                 *                        iocontrollers.cc, E20)
                 * No double-count: the pJ/bit figures are SerDes papers and do
                 * not include protocol logic. */
                ps.total_load_perc = (duty_derived >= 0.0) ? duty_derived : 0.0;
                ps.transferred_bytes =
                    charge_here ? static_cast<double>(xbytes) : 0.0;   // link priced once
                ps.link_pj_per_bit = pjbit;
                /* 1.11.42 (audit E21): DERIVED, was a two-valued literal
                 * (gen3 500, everything else 1000). The clock cancels out of
                 * the SerDes and byte-driven terms but linearly scales the
                 * controller digital dynamic E20 made live, so the literal
                 * scaled real power for every non-gen3 link. Under the stated
                 * 32-bit PIPE convention: gen3 250 (the literal's 500 was 2x
                 * high), gen4 500 (literal 2x high), gen5/CXL 1000 (literal
                 * correct). Non-PIPE families (NVLink/UALink/UCIe) have no
                 * sourced controller clock: WARN and keep 1000, labelled
                 * ASSUMED, rather than pretending a derivation exists. */
                {
                    double clk = PIMID::CactiIOWrapper::linkControllerClockMHz(
                                     config.pcie_link_type);
                    if (clk > 0.0) {
                        ps.link_clock_mhz = static_cast<int>(clk + 0.5);
                    } else {
                        ps.link_clock_mhz = 1000;
                        std::cerr << "[power] WARNING: no sourced controller "
                                     "clock for link type '"
                                  << config.pcie_link_type
                                  << "' (non-PIPE family); using 1000 MHz "
                                     "ASSUMED -- controller dynamic scales "
                                     "with it." << std::endl;
                    }
                }
                ps.link_type_name = config.pcie_link_type;   // 1.11.29: name it
                mcpat.setPCIeStats(ps);
                if (charge_here) {
                    std::cout << "  [xing] " << node.name << ": link "
                              << config.pcie_link_type << ", " << xbytes
                              << " B crossed (h2d " << zsim_stats.xing_h2d_bytes
                              << " + d2h " << zsim_stats.xing_d2h_bytes
                              << (xing_coupled
                                    ? " + flush " + std::to_string(zsim_stats.xing_flush_bytes)
                                    : std::string())
                              << " + framing " << xframing
                              << "), " << zsim_stats.xing_count
                              /* 1.11.40 (user ruling): report the BAND, not a
                               * midpoint pretending to be a measurement. A
                               * single-point entry says so, because that is a
                               * fact about our sourcing, not the hardware. */
                              << " crossings, " << pjbit << " pJ/bit"
                              << (pj_overridden
                                    ? std::string(" [power.pcie.pj_per_bit_override;"
                                                  " no sourced range for a user value]")
                                    : std::string())
                              << (pjband.valid()
                                    ? (pjband.single_point
                                         ? std::string(" [SINGLE POINT, range not sourced: ")
                                             + pjband.provenance + "]"
                                         : " [band " + std::to_string(pjband.lo) + "-"
                                             + std::to_string(pjband.hi) + " pJ/bit -> link energy "
                                             + std::to_string(xbytes * 8.0 * pjband.lo * 1e-12) + "-"
                                             + std::to_string(xbytes * 8.0 * pjband.hi * 1e-12) + " J]")
                                    : std::string())
                              /* 1.11.40 (E19): the duty cycle is measured, so
                               * report it as a measurement -- a reader must be
                               * able to see what peak link dynamic was scaled
                               * by without reading the source. */
                              << ", peak duty " << ps.duty_cycle
                              << (config.pcie_duty_cycle_user_set ? " [OVERRIDE]" : " [derived: bytes/time]")
                              << (link_unpriced ? " [link dynamic UNPRICED: unknown link type]" : "")
                              /* 1.11.40 (E18): the parenthetical must sum to the
                               * number in front of it -- the 1.11.15 line put
                               * flush OUTSIDE the sum and said it was not link
                               * traffic, which is only true when the host owns
                               * its memory. Say which case this run is. */
                              << "; flush " << zsim_stats.xing_flush_bytes << " B "
                              << (xing_coupled
                                    ? "CROSSES the link (shared array is device "
                                      "memory) and is ALSO charged as array writes "
                                      "-- PHY and DRAM are different silicon"
                                    : "rides the host's OWN memory channel "
                                      "(decoupled: host array is local, nothing "
                                      "crosses) -- charged as writes only")
                              << std::endl;
                }
            }
            }

            // Avoid McPAT's homogeneous_NoCs=1 code path (number_of_NoCs == 1),
            // which CACTI 6.5-P routes through a configuration that crashes on
            // "Must have at least one port" regardless of the per-bank ports
            // emitted in the XML. So we must always emit >= 2 NoC levels.
            //
            // 1.9.22: for DEVICE nodes, source those levels from the MEASURED
            // Garnet traffic the run already wrote to garnet_stats.txt, via the
            // same buildNoCLevelsForMcPAT() the device-scope path uses. Before
            // this, per-node (co-sim) analysis emitted a placeholder pair with
            // total_accesses = 0 and a hardcoded duty_cycle = 0.1, so every
            // co-sim NoC was priced off a guess while a full measurement
            // (packets, hops, buffer reads/writes, crossbar and link
            // traversals, router count) sat unread on disk. Device-scope runs
            // were already correct; only the co-sim path was not.
            bool noc_from_measurement = false;
            if (node.role == UnifiedConfig::SystemNode::DEVICE) {
                std::string gpath = output_dir + "/garnet_stats.txt";
                GarnetParsedStats g = parseGarnetStatsFile(gpath);
                if (g.total_packets > 0) {
                    uint64_t dev_cycles = (zsim_stats.dev_wall_cycles > 0)
                                        ? zsim_stats.dev_wall_cycles
                                        : (g.total_cycles > 0 ? g.total_cycles : total_cycles);
                    auto measured = buildNoCLevelsForMcPAT(config, g, dev_cycles, overrides);
                    if (measured.size() >= 2) {
                        mcpat.setNoCLevels(measured);
                        noc_from_measurement = true;
                        std::cout << "  [NoC] " << node.name << ": priced from measured Garnet"
                                  << " (" << measured.size() << " levels, "
                                  << g.total_packets << " packets, "
                                  << g.total_hops << " hops)" << std::endl;
                    }
                }
                if (!noc_from_measurement) {
                    std::cout << "  [NoC] " << node.name
                              << ": WARNING no usable Garnet stats -- falling back to the"
                                 " placeholder duty cycle (NoC power is NOT measured)."
                              << " path=" << gpath
                              << " exists=" << (access(gpath.c_str(), R_OK) == 0 ? "yes" : "no")
                              << " packets=" << g.total_packets
                              << " hops=" << g.total_hops
                              << std::endl;
                }
            }
            if (!noc_from_measurement)
            {
                using NoCLevel = pimid::McPATWrapper::NoCLevelConfig;
                std::vector<NoCLevel> two_levels;
                NoCLevel pe_lvl;
                pe_lvl.name = "PE_noc";
                pe_lvl.type = 1;  // router-based
                pe_lvl.horizontal_nodes = mcfg.has_noc ? mcfg.noc_num_cols : 1;
                pe_lvl.vertical_nodes   = mcfg.has_noc ? mcfg.noc_num_rows : 1;
                pe_lvl.clock_mhz = mcfg.has_noc ? mcfg.noc_clock_mhz
                                               : node.frequency_mhz;
                /* 1.11.52 (audit A010): DERIVED, not invented. McPAT scales
                 * NoC area and leakage by chip_coverage, so the literals 0.5
                 * and 0.1 here priced real host silicon from two numbers with
                 * no source -- on a branch every co-simulated HOST takes.
                 * There is no measurement of a host fabric (PIMID does not
                 * model one), so the honest basis is the STRUCTURE we do
                 * describe: the two entries exist only because McPAT's
                 * homogeneous-NoC path crashes below two levels, and they
                 * carry leakage alone (duty and accesses are zero). Split the
                 * die between them by their own node counts instead of by
                 * assertion, and say that the split is structural. */
                pe_lvl.chip_coverage = 0.0;   // set below from node counts
                /* 1.9.25: do NOT fabricate activity. The previous hardcoded
                 * duty cycle charged a fixed fraction of peak to a fabric that
                 * may not exist at all -- a 1-core host has no meaningful
                 * on-die network (the crossbar is degenerate; core -> caches ->
                 * MC is direct), yet it was billed as if 10% busy.
                 *
                 * PIMID does not simulate a host-side interconnect, so there is
                 * no measurement to substitute: garnet_stats.txt describes the
                 * DEVICE's in-memory network, and pricing the host from it
                 * would be worse than a placeholder. The honest treatment is
                 * zero activity -- the structure is still emitted because
                 * McPAT's homogeneous-NoC path crashes CACTI below two levels,
                 * so the entry carries its leakage and nothing else -- and to
                 * SAY SO when a fabric that would carry traffic is unpriced. */
                pe_lvl.duty_cycle = 0.0;
                pe_lvl.total_accesses = 0;
                two_levels.push_back(pe_lvl);
                NoCLevel mc_lvl = pe_lvl;
                mc_lvl.name = "MC_noc";
                mc_lvl.type = 0;  // bus
                mc_lvl.chip_coverage = 0.0;   // set below from node counts
                {
                    /* Structural split: the PE/core mesh covers its own node
                     * count, the MC bus one node. Both are leakage-only
                     * entries (duty 0, accesses 0); this decides only how the
                     * die area is apportioned between them, and it is now a
                     * ratio of described structure rather than 0.5 / 0.1. */
                    const double pe_nodes =
                        std::max(1.0, static_cast<double>(pe_lvl.horizontal_nodes) *
                                      static_cast<double>(pe_lvl.vertical_nodes));
                    const double mc_nodes = 1.0;
                    const double tot = pe_nodes + mc_nodes;
                    pe_lvl.chip_coverage = pe_nodes / tot;
                    mc_lvl.chip_coverage = mc_nodes / tot;
                    two_levels[0].chip_coverage = pe_lvl.chip_coverage;
                    std::cout << "  [NoC] " << node.name
                              << ": placeholder fabric coverage split "
                              << pe_lvl.chip_coverage << " / "
                              << mc_lvl.chip_coverage
                              << " by node count (leakage-only entries; no host"
                                 " fabric is measured)" << std::endl;
                }
                /* 1.11.52 (audit C011): these placeholder levels describe a
                 * HOST's on-die fabric (or a device's when no Garnet
                 * measurement exists). on_dram_die defaults to 1, which would
                 * hand a host's crossbar the DRAM-periphery transform. The
                 * host is logic; a device without measured levels takes the
                 * same matrix answer the measured path computes. */
                {
                    const bool on_die = (node.role == UnifiedConfig::SystemNode::DEVICE)
                        && (mcfg.process_family == 1);
                    pe_lvl.on_dram_die = on_die ? 1 : 0;
                    mc_lvl.on_dram_die = on_die ? 1 : 0;
                    two_levels[0].on_dram_die = pe_lvl.on_dram_die;
                }
                two_levels.push_back(mc_lvl);
                mcpat.setNoCLevels(two_levels);
                if (node.role == UnifiedConfig::SystemNode::HOST) {
                    if (node.num_cores > 1) {
                        std::cout << "  [NoC] " << node.name
                                  << ": WARNING host has " << node.num_cores
                                  << " cores but PIMID does not model a host-side"
                                     " interconnect -- its NoC carries leakage only,"
                                     " activity is NOT measured" << std::endl;
                    } else {
                        std::cout << "  [NoC] " << node.name
                                  << ": single-core host -- on-die fabric is"
                                     " degenerate, priced at zero activity"
                                  << std::endl;
                    }
                }
            }

            mcpat.initialize();

            // Distribute stats proportionally by core count
            double core_frac = static_cast<double>(node.num_cores) /
                std::max(1, [&]() {
                    int total = 0;
                    for (const auto& n : config.system_nodes) total += n.num_cores;
                    return total;
                }());
            // 1.9.10 fix: price this node over the true wall-clock TIME in ITS OWN
            // clock domain (node_cycles = wall_seconds * node_clock), instead of the
            // first-core contention-excluded count scaled by core fraction. The access
            // counts below stay core-fraction split, so accesses/node_cycles now equals
            // the physical access RATE over the real elapsed time. When wall_seconds is
            // unavailable (no per-group contention stats parsed), fall back to the
            // legacy total_cycles*core_frac basis to preserve prior behavior.
            uint64_t node_cycles;
            if (wall_seconds > 0.0 && node.frequency_mhz > 0) {
                node_cycles = static_cast<uint64_t>(wall_seconds * node.frequency_mhz * 1e6);
            } else {
                /* 1.11.44 (E26): same ruling as the instruction fallback below
                 * -- total_cycles x core_frac is the same uniform-work
                 * invention on the time axis. A run with no derivable wall
                 * clock is an invalid input, not a scaling problem. */
                std::cerr << "ERROR: node '" << node.name << "' has no "
                             "derivable wall clock (wall_seconds="
                          << wall_seconds << ", freq=" << node.frequency_mhz
                          << " MHz); refusing the total_cycles x core_frac "
                             "split. Fix the input." << std::endl;
                exit(1);
            }
            /* 1.9.29: feed McPAT THIS node's own measured counters.
             *
             * Everything below used to be the all-nodes total scaled by
             * `core_frac` (this node's core count over the system's). That is
             * not an attribution -- it is an assumption that every core in the
             * system did identical work, which is exactly false for a co-sim
             * host driving a device. 1.9.28 replaced the split for the
             * instruction count alone and left the rest, which made each node
             * internally inconsistent (host: 77.5M instrs against 5.2M uops).
             *
             * `grp` is the node's measured set. It is null, or empty, when the
             * dump carries no per-node breakdown (a single-node run, or an old
             * stats file); in that case fall back to the previous core_frac
             * behaviour so nothing regresses. */
            const ZSimParsedOutput::GroupCounters* grp =
                (node.role == UnifiedConfig::SystemNode::HOST) ? &zsim_stats.host
                                                               : &zsim_stats.dev;
            const bool have_grp = grp->has_activity();
            /* 1.11.44 (audit E26, user ruling: "no measurements should never
             * happen"). The fallback here priced an unmeasured node at
             * total_instrs x core_frac -- a UNIFORM-WORK assumption, exactly
             * false for a host driving a device. Worse, 1.11.9's
             * instrs-summing changed total_instrs from first-core to all-core
             * SUM, silently rebasing this estimate by ~Ncores while the
             * comment claimed it "preserves prior behaviour". A node with no
             * measured counters is an INVALID INPUT (the dump predates
             * per-node groups, or the parse failed) -- pricing it on an
             * invented split hides the defect inside a result. Refused. The
             * path is dead on every healthy run: both groups measure in every
             * co-sim dump this train has produced. */
            if (!have_grp) {
                std::cerr << "ERROR: node '" << node.name << "' has no "
                             "measured per-node counters in the stats dump; "
                             "refusing to price it on a uniform-work split "
                             "(total x core_frac). The dump predates per-node "
                             "groups or the parse failed -- fix the input; do "
                             "not trust this run." << std::endl;
                exit(1);
            }

            /* 1.9.33: executed instructions only. grp->instrs still carries the
             * injected timing charges the plugin encodes in that field. */
            uint64_t node_instrs = grp->real_instrs();
            if (node_cycles == 0) node_cycles = 1;
            if (node_instrs == 0) node_instrs = 1;

            mcpat.setTotalCycles(node_cycles);
            mcpat.setBusyCycles(node_cycles);
            /* 1.9.28: node_instrs was computed and then never handed to McPAT,
             * so total_instructions_ kept its constructor value of zero and
             * EVERY core activity stat in the generated XML was zero --
             * int/fp/branch/committed counts and ROB reads alike. Core dynamic
             * power was therefore ~0 by construction in every system-scope
             * (co-simulation) cell, which is why per-core dynamic looked
             * implausibly low. The device-scope path always supplied it. */
            mcpat.setTotalInstructions(node_instrs);

            /* Cache accesses. Before 1.9.29 these ALSO parsed as zero in system
             * scope -- the cache stat groups are named "<node>_l1d" and the
             * parser matched bare "l1d-", so no cache scope was ever entered
             * and all cache dynamic power was priced at zero activity. Both the
             * parse and the attribution are fixed; see the parser note. */
            if (have_grp) {
                mcpat.setL1IAccesses(grp->l1i_total_reads(), grp->l1i_mGETS);
                mcpat.setL1DAccesses(grp->l1d_total_reads(), grp->l1d_total_writes(),
                                     grp->l1d_mGETS, grp->l1d_mGETXIM);
                mcpat.setL2Accesses(grp->l2_total_reads(), grp->l2_total_writes(),
                                    grp->l2_mGETS, grp->l2_mGETXIM);
                mcpat.setL3Accesses(grp->l3_total_reads(), grp->l3_total_writes(),
                                    grp->l3_mGETS, grp->l3_mGETXIM);
                mcpat.setMemControllerAccesses(grp->mem_rd, grp->mem_wr);
                /* 1.9.28: measured core activity instead of fixed instruction-mix
                 * fractions. 1.9.29: this node's own, so the mix is consistent
                 * with the instruction count it describes. */
                mcpat.setMeasuredCoreActivity(grp->uops, grp->branches,
                                              grp->mispredBranches);
                mcpat.setMeasuredMix(grp->mix_int, grp->mix_mul, grp->mix_fp,
                                     grp->mix_ld, grp->mix_st, grp->mix_br);  // 1.11.10/.15
            }
            /* 1.11.44 (E26): the per-component core_frac fallback that lived
             * here is gone with its trigger -- have_grp is now a hard
             * precondition (refused above), so a scaled all-nodes split can
             * never be fed to McPAT. */
            std::cout << "  [Activity] " << node.name
                      << ": instrs=" << node_instrs
                      << " (synth=" << (have_grp ? grp->syntheticInstrs : 0) << ")"
                      << " cycles=" << node_cycles
                      << " uops=" << (have_grp ? grp->uops : 0)
                      << " l1d=" << (have_grp ? grp->l1d_total_reads() +
                                                grp->l1d_total_writes() : 0)
                      << " l2=" << (have_grp ? grp->l2_total_reads() +
                                               grp->l2_total_writes() : 0)
                      << " mem=" << (have_grp ? grp->mem_rd + grp->mem_wr : 0)
                      << (have_grp ? "  (measured, per-node)"
                                   : "  (core-fraction fallback -- no per-node counters)")
                      << std::endl;
            mcpat.setMCTechParams(getMCTechParamsForMcPAT(node.memory_tech, 1));

            mcpat.computePower();

            auto power = mcpat.getSystemPower();
            result.total_power = power.total_power;
            result.dynamic_power = power.total_dynamic;
            result.leakage_power = power.total_leakage;
            result.area = mcpat.getTotalArea();
            result.valid = true;

            std::cout << "  " << node.name << " (" << result.core_desc
                      << ", " << static_cast<int>(node.frequency_mhz) << "MHz"
                      << ", " << node.tech_node_nm << "nm): "
                      << std::fixed << std::setprecision(2) << result.total_power << " W ("
                      << result.dynamic_power << " dyn + "
                      << result.leakage_power << " leak), "
                      << result.area << " mm^2"
                      << std::defaultfloat << std::endl;
            /* 1.11.29 (user ruling): system scope reports its COMPONENTS, not
             * just a per-node total. Until now a co-sim run printed one line
             * per node and nothing else -- you could not see what the link,
             * the NoC or the MCs cost, which is why the link-share question
             * could not be answered without instrumenting the binary.
             * printComponentBreakdown() already existed and had NO caller.
             * It reports only what this node was configured with, so the
             * shape follows the system the user defined. */
            mcpat.printComponentBreakdown();
        } catch (const std::exception& e) {
            std::cerr << "  " << node.name << ": McPAT failed: " << e.what() << std::endl;
        }

        results.push_back(result);
    }

    /* 1.9.42: charge the one memory's array, which this scope never did.
     * Technologies are CHECKED, not assumed: if two nodes ever disagree about
     * what the memory is, that is a topology this build does not model and it
     * stops, rather than charging one of them and silently dropping the other.
     * Several memories is 2.1. */
    /* 1.11.9 (#86, audit blocker): the 1.9.42 one-memory check hard-aborted
     * on DECOUPLED co-sim -- host DDR5 + device HBM3 is the STANDARD
     * configuration, and the timing side has priced each node's accesses
     * with its own local technology since 1.1.0. The energy side now does
     * the same: each node's memory is charged with ITS OWN technology and
     * ITS OWN group counters (has_activity() guards unmeasured groups,
     * 1.9.35). Nodes sharing one memory (device IS the host memory) simply
     * name the same technology and their charges land on it separately --
     * the sum is identical to the old single-tech path (gate invariant).
     * The audit's rd/wr-polarity blocker is contained by the same change:
     * counters are no longer summed ACROSS sides before charging. */
    /* 1.11.9 (#86, audit): report the measured locality split, and say
     * explicitly when a placement produces no device memory group at all
     * (HOST_MC: the elements sit at the host controller, so their accesses
     * are host-memory accesses BY CONSTRUCTION -- a structural zero, not a
     * missing measurement, and the report must distinguish the two). */
    /* 1.11.11 (#113) / 1.11.17: shared report; uses the DEVICE group's own
     * census so the host's FP stream is not pinned on the elements. */
    reportFpWithoutFpu(config, zsim_stats);
    if (zsim_stats.pemi_local_acc + zsim_stats.pemi_remote_acc > 0) {
        uint64_t tot = zsim_stats.pemi_local_acc + zsim_stats.pemi_remote_acc;
        std::cout << "  [locality] PE-MI accesses: "
                  << zsim_stats.pemi_local_acc << " local + "
                  << zsim_stats.pemi_remote_acc << " remote ("
                  << std::fixed << std::setprecision(1)
                  << (100.0 * static_cast<double>(zsim_stats.pemi_local_acc) / tot)
                  << "% local at this placement)" << std::defaultfloat
                  << std::setprecision(6) << std::endl;   // 1.11.17: precision must not leak
    }
    /* 1.11.17 (audit go-through): the old guard tested dev.has_activity(),
     * a CORE-group flag -- HOST_MC elements retire instructions, so the
     * flag was true and this message was unreachable on exactly the runs
     * it explains. The structural zero is the device MEMORY group. */
    if (config.pe_hierarchy_level == -1 &&
        (zsim_stats.dev.mem_rd + zsim_stats.dev.mem_wr) == 0) {
        std::cout << "  [mem] HOST_MC placement: the elements share the host "
                     "memory controller, so their accesses ARE host-memory "
                     "accesses -- no separate device memory group exists by "
                     "construction (structural zero, not a missing counter)"
                  << std::endl;
    }

    double mem_area_total = 0.0;   // 1.11.9: memory silicon, summed into the system total
    double mem_power_total = 0.0;  // 1.11.45 (E31): its POWER, same wall clock, same total
    int    mem_die_count = 0;      // 1.11.46 (L237): population, for the report
    double mem_die_mm2  = 0.0;
    int    mem_die_techs = 0;      // 1.11.56 (A031): how many technologies were priced
    {
        /* COUPLED vs DECOUPLED. When the device IS the host's memory
         * (resolveMemoryTopology copies the device tech onto the host), both
         * nodes name ONE piece of silicon: its accesses are the sum of both
         * sides and it is charged ONCE -- byte-for-byte the pre-1.11.9
         * behaviour, so coupled runs (the whole existing corpus) are
         * unchanged. Only genuinely decoupled systems -- host with its own
         * memory block, device with another technology, the case that used
         * to abort -- charge per node. */
        std::string h_tech, d_tech;
        for (const auto& node : config.system_nodes) {
            if (node.memory_tech.empty()) continue;
            if (node.role == UnifiedConfig::SystemNode::HOST && h_tech.empty())
                h_tech = node.memory_tech;
            if (node.role == UnifiedConfig::SystemNode::DEVICE && d_tech.empty())
                d_tech = node.memory_tech;
        }
        const bool coupled = sharedMemoryCoupled(config);   // 1.11.40: one definition
        /* 1.11.15 (audit): the coherence-flush footprint is host dirty lines
         * written back to MEMORY -- it belongs in the array-write charge, not
         * on the PCIe PHY (where 1.11.7 had priced it as 99.999% of the link
         * bytes). One line per write.
         *
         * 1.11.16 (verification audit): (a) the line size is the CONFIGURED
         * one, not a literal 64 -- with sys.cache_line_size=128 the flush
         * contributed 2x the writeback events it should have; (b) the
         * "charged as memory writes" line prints AFTER a branch actually
         * lands the charge -- the 1.11.15 shape announced the charge
         * unconditionally and then could drop it (empty tech, no host
         * activity), asserting an energy charge that was never made. */
        /* 1.11.20 (D6): the same two inputs device scope uses, computed ONCE
         * here and handed to every array report below, so system scope and
         * device scope cannot drift apart again. */
        /* 1.11.52 (audit A005/A006): the DRAM idle descent is gated on
         * memory.power_down OR the controller flag, not on pim.mc.pg alone.
         * 1.11.45 split those flags apart -- power_down is the DRAM's own
         * IDD2P state, pg_mc is the CONTROLLER's gating -- and device scope
         * was updated (its comment says so explicitly: "no config.pg_mc in
         * this condition"), while this path kept the controller gate. A
         * co-sim run with memory.power_down: true and no pim.mc.pg reported
         * IDD3N standby while device scope descended, for one machine.
         *
         * A006: the UNARMED-COUNTER refusal device scope has since 1.11.18
         * was also missing here. A dead pg_devmc_active yields r_idle = 1.0
         * -- the whole memory reported idle -- and it was fed to the
         * background model with no message at all. */
        const bool sys_pd_asked = config.mem_power_down || config.pg_mc;
        const bool sys_devmc_armed =
            (zsim_stats.pg_devmc_active > 0) ||
            (zsim_stats.mem_rd + zsim_stats.mem_wr == 0);
        double sys_r_idle = -1.0;
        if (sys_pd_asked && zsim_stats.pg_window() > 0 && sys_devmc_armed) {
            sys_r_idle = 1.0 - std::min(1.0,
                static_cast<double>(zsim_stats.pg_devmc_active)
                    / static_cast<double>(zsim_stats.pg_window()));
        } else if (sys_pd_asked && !sys_devmc_armed) {
            std::cout << "  [pg] DRAM idle residency UNAVAILABLE: the "
                         "device-MC phase counter never advanced despite "
                      << (zsim_stats.mem_rd + zsim_stats.mem_wr)
                      << " accesses. Background reported at active standby "
                         "(IDD3N); no idle descent claimed." << std::endl;
        }
        /* 1.11.51 (E17): same supersession as device scope (D6: one
         * derivation, two scopes). */
        if (sys_pd_asked && sys_devmc_armed) {
            std::string gprov;
            /* 1.11.52 (audit A017): the histogram's cycles are in the clock
             * the SYSTEM zsim config was emitted at -- the reference clock,
             * i.e. the max over all nodes -- not the device's own frequency.
             * Converting the tXP threshold with the device clock put the
             * threshold in the wrong units by their ratio; since the buckets
             * are powers of two, a ratio past 2x admits or drops a whole
             * bucket of idle. */
            const double hist_mhz = (config.reference_frequency_mhz > 0.0)
                                    ? config.reference_frequency_mhz
                                    : config.frequency_mhz;
            double r_gap = gapPowerDownResidency(config, zsim_stats,
                                                 hist_mhz, &gprov);
            if (r_gap >= 0.0) {
                std::cout << "  [pg] power-down residency from the MEASURED "
                             "gap histogram: " << r_gap << " (" << gprov
                          << ")." << std::endl;
                sys_r_idle = r_gap;
            }
        }
        /* 1.11.52 (audit A018/A019): the shared predicate, and it is asked
         * PER MEMORY, not once for the whole system. The device's PE
         * placement decided whether the HOST's own array paid termination:
         * with elements at BANK placement the host's DIMM was reported as
         * "on-die placement: no DQ crossing, no termination" -- about its
         * own memory, which a host CPU always drives across real DQ pins.
         * The host's memory is always off-package from the host's point of
         * view (HOST_MC semantics); the device's memory is judged by the
         * device's placement. */
        const bool sys_crosses_dq =
            crossesOffPackageDQ(config.pe_hierarchy_level, config.memory_tech);
        const bool host_crosses_dq = crossesOffPackageDQ(-1, config.memory_tech);

        uint64_t line_b = (config.cache_line_size > 0)
                              ? static_cast<uint64_t>(config.cache_line_size) : 64;
        uint64_t flush_wr = zsim_stats.xing_flush_bytes / line_b;
        bool flush_charged = false;
        auto sayFlushCharged = [&](const char* where) {
            if (flush_wr == 0) return;
            flush_charged = true;
            std::cout << "  [mem] coherence flush: " << zsim_stats.xing_flush_bytes
                      << " B (" << flush_wr << " line writebacks, "
                      << line_b << " B lines) charged as memory writes on "
                      << where << std::endl;
        };
        if (coupled) {
            std::string tech = d_tech.empty() ? h_tech : d_tech;
            uint64_t all_rd = 0, all_wr = 0;
            if (zsim_stats.host.has_activity()) { all_rd += zsim_stats.host.mem_rd; all_wr += zsim_stats.host.mem_wr; }
            if (zsim_stats.dev.has_activity())  { all_rd += zsim_stats.dev.mem_rd;  all_wr += zsim_stats.dev.mem_wr;  }
            all_wr += flush_wr;   // 1.11.15: the flush lands on the shared array
            if (!tech.empty()) {
                std::cout << "  [mem] one memory (" << tech
                          << "): host and device accesses land on the same "
                             "silicon and are charged once" << std::endl;
                sayFlushCharged("the shared array");
                mem_power_total += reportSharedMemoryArrayEnergy(tech, all_rd, all_wr,
                                              sys_r_idle, sys_crosses_dq,
                                              config.mem_power_down,        // 1.11.45: split flag (was pg_mc)
                                              config.dram_device_width,
                                              wall_seconds,     // 1.11.20 D13
                                              effectiveDramBanks(tech, config),   // A008
                                              config.hierarchy_ranks_per_channel, // A015
                                              config.hierarchy_dram_channels,
                                              config.termination_pj_per_bit);   // 1.11.63 (R7)
                {
                double die = computeDramDieAreaMM2(tech, false,
                                                   effectiveDramBanks(tech, config));
                if (die <= 0.0)   // 1.11.46 (L242)
                    mem_area_total += computeNonDramMemAreaMM2(
                        tech, config.num_banks, config.tech_node_nm,
                        config.cache_line_size, config);
                int dies = memorySystemDieCount(tech, config.dram_device_width,
                                                config.hierarchy_ranks_per_channel,
                                                config.hierarchy_dram_channels);
                mem_area_total += die * dies;   // 1.11.46 (L237): POPULATED silicon
                mem_die_count = dies; mem_die_mm2 = die; ++mem_die_techs;  // 1.11.56 (A031)
            }
            }
        } else {
        bool host_done = false, dev_done = false;
        std::set<std::string> priced_techs;
        std::cout << "  [mem] decoupled system: host " << h_tech
                  << " + device " << d_tech
                  << " -- each charged with its own technology" << std::endl;
        for (const auto& node : config.system_nodes) {
            if (node.memory_tech.empty()) continue;
            /* 1.11.46 (FIX-PRE-FLEET L236): AREA IS STRUCTURAL. The silicon
             * exists whether or not the workload touched it this run; only
             * ENERGY is activity-dependent. Area was inside the activity
             * gates below, so an idle memory vanished from System Total area. */
            if (priced_techs.insert(node.memory_tech).second) {
                double die = computeDramDieAreaMM2(
                    node.memory_tech, false,
                    effectiveDramBanks(node.memory_tech, config));   // 1.11.52 A007
                if (die <= 0.0) {   // 1.11.46 (L242): non-DRAM silicon counts too
                    mem_area_total += computeNonDramMemAreaMM2(
                        node.memory_tech, node.banks > 0 ? node.banks : config.num_banks,
                        config.tech_node_nm, config.cache_line_size, config);
                    /* no continue: the ENERGY branches below must still run --
                     * their reporter states its own non-DRAM scope honestly. */
                } else {
                int dies = memorySystemDieCount(node.memory_tech, config.dram_device_width,
                                                config.hierarchy_ranks_per_channel,
                                                config.hierarchy_dram_channels);
                mem_area_total += die * dies;   // 1.11.46 (L237): populated
                mem_die_count = dies; mem_die_mm2 = die; ++mem_die_techs;  // 1.11.56 (A031)
                }
            }
            if (node.role == UnifiedConfig::SystemNode::HOST && !host_done &&
                zsim_stats.host.has_activity()) {
                std::cout << "  [mem] " << node.name << " (" << node.memory_tech
                          << "): host-side array energy" << std::endl;
                sayFlushCharged("the host-side array");
                mem_power_total += reportSharedMemoryArrayEnergy(node.memory_tech,
                                              zsim_stats.host.mem_rd,
                                              zsim_stats.host.mem_wr + flush_wr,  // 1.11.15
                                              sys_r_idle, host_crosses_dq,        // 1.11.52 (A019)
                                              config.mem_power_down,  // 1.11.45: split flag (1.11.41)
                                              config.dram_device_width,
                                              wall_seconds,
                                              effectiveDramBanks(node.memory_tech, config),
                                              config.hierarchy_ranks_per_channel,  // A015
                                              config.hierarchy_dram_channels,
                                              config.termination_pj_per_bit);   // 1.11.63 (R7)
                host_done = true;
            } else if (node.role == UnifiedConfig::SystemNode::DEVICE && !dev_done &&
                       zsim_stats.dev.has_activity()) {
                std::cout << "  [mem] " << node.name << " (" << node.memory_tech
                          << "): device-side array energy" << std::endl;
                /* 1.11.46: E31 wired the host-side and coupled calls into the
                 * total and MISSED this one -- the decoupled device memory's
                 * power was computed, printed, and dropped from System Total. */
                mem_power_total += reportSharedMemoryArrayEnergy(node.memory_tech,
                                              zsim_stats.dev.mem_rd,
                                              zsim_stats.dev.mem_wr,
                                              sys_r_idle, sys_crosses_dq,         // 1.11.20
                                              config.mem_power_down,  // 1.11.45: split flag (1.11.41)
                                              config.dram_device_width,
                                              wall_seconds,
                                              effectiveDramBanks(node.memory_tech, config),
                                              config.hierarchy_ranks_per_channel,  // A015
                                              config.hierarchy_dram_channels,
                                              config.termination_pj_per_bit);   // 1.11.63 (R7)
                dev_done = true;
            }
        }
        }
        if (flush_wr > 0 && !flush_charged) {
            /* 1.11.16: reporting must not outrun the accounting -- if no
             * branch landed the writebacks, say so instead of staying
             * silent after the bytes were measured. */
            std::cerr << "[power] WARNING: coherence flush ("
                      << zsim_stats.xing_flush_bytes << " B) was NOT charged "
                         "-- no host-visible array took the writebacks "
                         "(empty memory tech or no host activity)."
                      << std::endl;
        }
    }

    // System totals
    double sys_power = 0, sys_dyn = 0, sys_leak = 0, sys_area = 0;
    for (const auto& r : results) {
        if (!r.valid) continue;
        sys_power += r.total_power;
        sys_dyn += r.dynamic_power;
        sys_leak += r.leakage_power;
        sys_area += r.area;
    }

    std::cout << "\n--- System Total ---" << std::endl;
    /* 1.11.45 (audit E31, user ruling): the memory die's POWER joins the
     * total the memory die's AREA has been in since 1.11.9 -- the two
     * adjacent lines described different systems. Same wall clock as every
     * node's McPAT power (1.9.10 single time base); shared memory counted
     * once, like area; split printed so the addition is auditable. */
    sys_power += mem_power_total;
    std::cout << "  Power: " << std::fixed << std::setprecision(2)
              << sys_power << " W (" << sys_dyn << " dyn + " << sys_leak << " leak";
    if (mem_power_total > 0.0) {
        std::cout << " + " << mem_power_total << " memory";
        /* 1.11.51 (E31 residue): the wall clock the memory watts are priced
         * over, printed so an external reader can reconstruct the term
         * (energy / wall + background rate) from the report alone. */
        std::cout << " over " << std::setprecision(6) << wall_seconds
                  << " s" << std::setprecision(2);
    }
    std::cout << ")" << std::endl;
    /* 1.11.9 (#86, audit): the memory die is silicon too. (1.11.51, L243:
     * the original justification here -- that device scope already counted
     * the die in ITS total -- was FALSE; device scope's "Total Area" was
     * McPAT silicon only, so 1.11.9 CREATED a cross-scope mismatch rather
     * than fixing one. Device scope now prints the same populated
     * "with memory" total, so the two scopes are comparable again.)
     * Shared memory is counted ONCE (a device that IS the host's memory
     * names one technology and is priced once). */
    sys_area += mem_area_total;
    std::cout << "  Area:  " << sys_area << " mm^2";
    /* 1.11: the aggregate alone hides the number that matters for a PIM
     * add-on: how much silicon the device ADDS to a host that already
     * exists. Print the per-node split, so "host socket X + device Y"
     * can be quoted without re-deriving it from logs. */
    {
        bool first = true;
        for (const auto& r : results) {
            if (!r.valid) continue;
            std::cout << (first ? "  (" : " + ") << r.name << " " << r.area;
            first = false;
        }
        if (mem_area_total > 0.0) {
            std::cout << (first ? "  (" : " + ") << "memory " << mem_area_total;
            /* 1.11.56 (audit A031): only factorise when there is ONE
             * technology to factorise. mem_area_total accumulates over every
             * priced technology, but the die count and per-die area were
             * plain assignments, so a decoupled system (host DDR5 + device
             * HBM3) printed the total for both beside a bracket describing
             * only the last one -- a factorisation that does not multiply out
             * to the number in front of it. The total itself was always
             * right. */
            if (mem_die_techs > 1)
                std::cout << " [" << mem_die_techs << " memory technologies priced;"
                             " per-die factorisation omitted]";
            else if (mem_die_count > 1)
                std::cout << " [" << mem_die_count << " dies x " << mem_die_mm2 << "]";
        }
        if (!first || mem_area_total > 0.0) std::cout << " per node)";
    }
    std::cout << std::defaultfloat << std::endl;
}

/**
 * ZSim configuration helper.
 * Generates ZSim .cfg files for use by QEMU+ZSim (exec mode) and zsim_trace (trace mode).
 */
class ZSimSimulator {
public:
    ZSimSimulator(const UnifiedConfig& config) : config_(config) {}

    /**
     * Generate ZSim configuration file.
     * Used by exec mode (QEMU + ZSim) and trace mode (zsim_trace replay).
     */
    std::string generateConfig() {
        std::string cfg_path = "/tmp/pimid_qemu_zsim_" + std::to_string(getpid()) + ".cfg";
        std::ofstream cfg(cfg_path);
        if (!cfg.is_open()) return "";

        std::string core_type = "Simple";
        if (config_.pe_type == "ooo_core") {
            core_type = "OoO";
        } else if (config_.pe_type == "in_order_core") {
            // The cycle-detailed in-order core (zsim InOrderCore).
            core_type = "InOrder";
        } else if (config_.pe_type == "alu_core" || config_.pe_type == "alu") {
            core_type = "ALU";
        } else if (config_.pe_type == "null_core" || config_.pe_type == "null") {
            core_type = "Null";
        }   // simple_core / unknown -> "Simple" (coarse SimpleCore)

        // Memory latency: priority is override > YAML config > external models > defaults
        int mem_latency;
        if (config_.memory_latency_override >= 0) {
            mem_latency = config_.memory_latency_override;
        } else {
            // Use external models (default) or complete YAML override.
            /* 1.11.57 (latent B024): this site used to compute a
             * banks x 64 KB array capacity and pass it, under a comment
             * saying the access latency would reflect the configured
             * organisation. The callee discarded it. The array that is
             * actually characterized is the single 64 KB bank a PE owns, so
             * the product is not computed any more and the comment that
             * promised otherwise is gone with it. */
            /* 1.11.25: pass the PLACEMENT. This is the device path -- the PE
             * sits at config_.pe_hierarchy_level, so the array access it sees
             * is that tier's, not a flat per-bank number for every tier. */
            mem_latency = getMemoryLatencyCycles(config_.memory_tech, config_.frequency_mhz,
                                                  config_.tech_node_nm,
                                                  config_.use_yaml_memory_params,
                                                  config_.memory_params.read_latency_ns,
                                                  config_.pe_hierarchy_level,
                                                  config_.temperature_k,   // 1.11.52 (D055)
                                                  config_.cache_line_size);  // 1.11.56 (B018)
        }

        // Cache latencies: YAML override > CACTI > defaults
        int l1d_latency, l1i_latency, l2_latency = 0, l3_latency = 0;
        if (config_.use_yaml_cache_params) {
            l1d_latency = static_cast<int>(std::round(
                config_.l1d_params.latency_ns * config_.frequency_mhz / 1000.0));
            l1i_latency = static_cast<int>(std::round(
                config_.l1i_params.latency_ns * config_.frequency_mhz / 1000.0));
            if (config_.enable_l2)
                l2_latency = static_cast<int>(std::round(
                    config_.l2_params.latency_ns * config_.frequency_mhz / 1000.0));
            if (config_.enable_l3)
                l3_latency = static_cast<int>(std::round(
                    config_.l3_params.latency_ns * config_.frequency_mhz / 1000.0));
        } else {
            l1d_latency = getCacheLatencyCycles(config_.l1d_size_kb, config_.l1d_ways,
                                                 config_.cache_line_size, config_.frequency_mhz, config_.tech_node_nm, /*fallback_cycles=*/4);
            l1i_latency = getCacheLatencyCycles(config_.l1i_size_kb, config_.l1i_ways,
                                                 config_.cache_line_size, config_.frequency_mhz, config_.tech_node_nm, /*fallback_cycles=*/3);
            if (config_.enable_l2)
                l2_latency = getCacheLatencyCycles(config_.l2_size_kb, config_.l2_ways,
                                                    config_.cache_line_size, config_.frequency_mhz, config_.tech_node_nm, /*fallback_cycles=*/12);
            if (config_.enable_l3)
                l3_latency = getCacheLatencyCycles(config_.l3_size_kb, config_.l3_ways,
                                                    config_.cache_line_size, config_.frequency_mhz, config_.tech_node_nm, /*fallback_cycles=*/20);
        }
        l1d_latency = std::max(1, l1d_latency);
        l1i_latency = std::max(1, l1i_latency);
        if (config_.enable_l2) l2_latency = std::max(1, l2_latency);
        if (config_.enable_l3) l3_latency = std::max(1, l3_latency);

        cfg << "// Auto-generated ZSim config by PIMID (QEMU mode)\n";
        cfg << "// Memory: " << config_.memory_tech << ", PEs: " << config_.num_pes << "\n";
        cfg << "// Frequency: " << config_.frequency_mhz << " MHz\n\n";

        cfg << "sys = {\n";
        cfg << "    lineSize = " << config_.cache_line_size << ";\n";
        cfg << "    frequency = " << config_.frequency_mhz << ";\n";
        cfg << "\n";
        // ALU + Null cores have no cache hierarchy (PIM ALU has analog/digital
        // compute units only; Null is a no-op for memory/network-only studies).
        bool core_has_caches = (core_type != "ALU" && core_type != "Null");

        cfg << "    cores = {\n";
        cfg << "        pim_pes = {\n";
        cfg << "            type = \"" << core_type << "\";\n";
        cfg << "            cores = " << config_.num_pes << ";\n";
        /* 1.11.43 (E23): per-group FPU keys; in device scope every core IS a
         * PE, so the group carries the configured PE values directly. */
        cfg << "            hasFpu = " << (config_.pe_has_fp ? "true" : "false") << ";\n";
        cfg << "            fpEmulCycles = " << config_.pe_fp_emul_cycles << ";\n";
        if (core_type == "ALU") {
            cfg << std::fixed << std::setprecision(2);
            cfg << "            computeFactor = " << config_.alu_compute_factor << ";\n";
            cfg << "            accessFactor = " << config_.alu_access_factor << ";\n";
            cfg << "            throughputFactor = " << config_.alu_throughput_factor << ";\n";
            cfg << std::defaultfloat;
            cfg << "            operandWidth = " << config_.alu_operand_width << ";\n";
            cfg << "            bitSerial = " << (config_.alu_bit_serial ? "true" : "false") << ";\n";
            cfg << std::fixed << std::setprecision(2);
            cfg << "            energyFactor = " << config_.alu_energy_factor << ";\n";
            cfg << std::defaultfloat;
        } else if (core_has_caches) {
            cfg << "            dcache = \"l1d\";\n";
            cfg << "            icache = \"l1i\";\n";
            if (core_type == "InOrder") {
                // In-order superscalar issue width (YAML pim.pe.issue_width;
                // default 2 == the core's historical hardcoded value).
                cfg << "            issueWidth = " << config_.inorder_issue_width << ";\n";
            }
        }
        // Null cores: no per-core attributes beyond type + count
        cfg << "        };\n";
        cfg << "    };\n";
        cfg << "\n";

        if (core_has_caches) {
            cfg << "    caches = {\n";
            cfg << "        l1d = {\n";
            cfg << "            caches = " << config_.num_pes << ";\n";
            cfg << "            size = " << (config_.l1d_size_kb * 1024) << ";\n";
            cfg << "            array = { type = \"SetAssoc\"; ways = " << config_.l1d_ways << "; };\n";
            cfg << "            latency = " << l1d_latency << ";\n";
            cfg << "        };\n";
            cfg << "        l1i = {\n";
            cfg << "            caches = " << config_.num_pes << ";\n";
            cfg << "            size = " << (config_.l1i_size_kb * 1024) << ";\n";
            cfg << "            array = { type = \"SetAssoc\"; ways = " << config_.l1i_ways << "; };\n";
            cfg << "            latency = " << l1i_latency << ";\n";
            cfg << "        };\n";
            if (config_.enable_l2) {
                cfg << "        l2 = {\n";
                cfg << "            caches = " << config_.l2_count << ";\n";
                cfg << "            size = " << (config_.l2_size_kb * 1024) << ";\n";
                cfg << "            array = { type = \"SetAssoc\"; ways = " << config_.l2_ways << "; };\n";
                cfg << "            children = \"l1i|l1d\";\n";
                cfg << "            latency = " << l2_latency << ";\n";
                cfg << "        };\n";
            }
            if (config_.enable_l3) {
                cfg << "        l3 = {\n";
                cfg << "            caches = 1;\n";
                cfg << "            size = " << (config_.l3_size_kb * 1024) << ";\n";
                cfg << "            array = { type = \"SetAssoc\"; ways = " << config_.l3_ways << "; };\n";
                cfg << "            children = \"l2\";\n";
                cfg << "            latency = " << l3_latency << ";\n";
                cfg << "        };\n";
            }
            cfg << "    };\n";
            cfg << "\n";
        }

        emitZSimMemBlock(cfg, config_, mem_latency);
        cfg << "\n";

        // Configure Garnet-based network for all topologies
        emitZSimNetworkBlock(cfg, config_);
        emitZSimHierarchyBlock(cfg, config_);

        cfg << "};\n\n";

        cfg << "sim = {\n";
        cfg << "    phaseLength = " << config_.phase_length << ";\n";
        cfg << "    maxTotalInstrs = " << config_.max_instructions << "L;\n";
        cfg << "    statsPhaseInterval = " << config_.stats_interval << ";\n";
        // Simulator thread count (sim_parallel knob):
        // OMP + parallel: all PEs run concurrently (work-locked phases keep
        // results exact at ANY simulator thread count -- speed knob only).
        // MPI: always 1 -- zsim's deterministic round-robin core rotation.
        // Ranks park/wake on transport waits, and with a single simulator
        // thread every interleaving (numPhases, batch content, EWMA order)
        // is a pure function of simulated state: bit-exact by construction,
        // on any host. sim_parallel starts being honored for MPI when the
        // PDES conservative-sync scheduler lands (1.8 roadmap).
        int parallelism = (config_.workload_type == "openmp" &&
                           config_.sim_parallel)
                          ? config_.num_pes : 1;
        cfg << "    parallelism = " << parallelism << ";\n";
        cfg << "    printHierarchy = true;\n";
        cfg << "    aslr = false;\n";
        // process0.command is never consumed by ZSim internals (it was read
        // by the Pin harness which no longer exists). Disable strict config
        // checking to avoid a panic on this unused setting.
        cfg << "    strictConfig = false;\n";
        cfg << "};\n\n";

        cfg << "// QEMU mode: workload is launched by QEMU, not by ZSim\n";
        cfg << "process0 = {\n";
        cfg << "    command = \"<qemu-managed>\";\n";
        cfg << "};\n";

        cfg.close();
        return cfg_path;
    }

private:
    UnifiedConfig config_;
};

/**
 * @brief Generate a single combined ZSim .cfg for multi-host/multi-device system.
 *
 * Produces a single config with all hosts and devices combined:
 * - Named core groups per node (e.g., "cpu0_cores", "hbm_pim0_pes")
 * - Per-node cache hierarchies (host: full L1/L2/L3, ALU device: no caches)
 * - Flat address space with SystemAddressRouter
 * - System network config (topology, latency, model)
 * - Node map for runtime core->node resolution
 * - Frequency normalized to reference (max of all nodes)
 */
// Per-tech host-memory bandwidth defaults. Aggregate BW + channel count come
// from the SAME per-tech source the device hierarchy uses (RamulatorWrapper:
// DDR5 25.6 GB/s c=1; HBM3 819 GB/s c=16 -> 51.2 GB/s per channel). Per-channel
// bandwidth = aggregate / channels. NVM/SRAM derive an analytic aggregate from
// banks x line-size / access-ns (single channel). This feeds the EXISTING
// SimpleMemory/WeaveSimpleMemory M/D/1 queueing (Pollaczek-Khinchine) per
// controller -- no new analytic term is introduced (that would double-count
// the bandwidth cap the MC already models; see co-sim defect #4).
struct HostMemBW { int per_channel_mbs; int channels; bool sourced; };
static HostMemBW getHostMemBandwidth(const std::string& tech_in,
                                     int num_banks, int line_size,
                                     int tech_node_nm = 22,
                                     int temperature_k = 350,
                                     double freq_mhz = 2000.0) {
    std::string tech = tech_in;
    std::transform(tech.begin(), tech.end(), tech.begin(), ::toupper);
    HostMemBW r; r.per_channel_mbs = 6400; r.channels = 1; r.sourced = false;
    if (pimid::isDRAM(pimid::parseMemoryTechnology(tech_in))) {
        try {
            pimid::RamulatorWrapper bw_q("", tech);
            bw_q.initialize();
            double agg = static_cast<double>(bw_q.getBandwidth());  // aggregate MB/s
            uint32_t nch = bw_q.getNumChannels();
            r.channels = (nch >= 1) ? static_cast<int>(nch) : 1;
            r.per_channel_mbs = static_cast<int>(agg / static_cast<double>(r.channels));
            r.sourced = true;
        }
        /* 1.11.57 (latent B023): the "keep defaults" this catch preserved were
         * 6400 MB/s on one channel -- a struct initialiser, not a datasheet --
         * and they become the HOST memory's per-channel bandwidth and channel
         * count, which is what the co-sim's M/D/1 queue saturates against. For
         * a DRAM host memory there is no second source for either, so the run
         * refuses rather than charging host traffic against an invented rate.
         * (The non-DRAM branch below already announces its own failure and
         * leaves the placeholder standing, because an array model that cannot
         * characterize is a different situation from a missing datasheet.) */
        catch (const std::exception& e) {
            refuseWithoutDramOracle(tech, e.what(),
                "the host memory's aggregate bandwidth and channel count, "
                "which set the M/D/1 service rate every host access queues "
                "against");
        } catch (...) {
            refuseWithoutDramOracle(tech,
                "a non-standard exception escaped the wrapper",
                "the host memory's aggregate bandwidth and channel count");
        }
    } else {
        /* 1.11.56 (audit B004): the SRAM/NVM branch used to divide by an
         * invented access time (10 / 50 / 20 / 2 ns), and the result is the
         * host memory bandwidth the co-sim charges against. The array model
         * that answers this question for real is queried a few thousand lines
         * up for the same technology; use it here too, and say plainly when
         * it cannot answer instead of substituting an assumption. */
        double acc_ns = -1.0;
        try {
            int lat_cy = getMemoryLatencyCycles(tech, freq_mhz, tech_node_nm,
                                                false, -1.0, -999,
                                                temperature_k, line_size);
            if (lat_cy > 0 && freq_mhz > 0) acc_ns = lat_cy * 1000.0 / freq_mhz;
        } catch (const std::exception&) { /* reported below */ }
        if (acc_ns > 0.0) {
            r.per_channel_mbs = static_cast<int>(num_banks * line_size * 1000.0 / acc_ns);
            r.sourced = true;
        } else {
            std::cerr << "[bw] WARNING: no array access time for host memory "
                      << tech << "; its bandwidth falls back to "
                      << r.per_channel_mbs << " MB/s, which is a placeholder, "
                         "not a property of this array. Set the host memory "
                         "bandwidth explicitly." << std::endl;
        }
    }
    if (r.per_channel_mbs < 1) r.per_channel_mbs = 1;
    if (r.channels < 1) r.channels = 1;
    return r;
}

// -----------------------------------------------------------------------------
// HOST-PATH IDLE-LATENCY ADDER (ns) -- per-tech calibrated constant.
//
// SCOPE: HOST-role memory pricing in SYSTEM scope (co-sim + baseline) ONLY.
//        Called exclusively from emitHostMemBlock(); NEVER touches device (PE)
//        memory pricing (that path is getMemoryLatencyCycles + the device NoC/
//        PE-MI hierarchy, unchanged). The 270-cell device-only dataset is
//        therefore bit-unaffected by this table.
//
// MECHANISM: the physical composition alone (tRCD+tCAS from Ramulator, plus the
// MC M/D/1 queue and any host-fabric hop) idles ~30-40ns -- far below what real
// CPU sockets measure, because it omits the IO-die / mesh / fabric traversal and
// the deep MC command pipeline that a real host load-to-use pays. This adder
// closes that gap so the effective idle host memory latency matches measured
// machines. Adder = target_total - physical(tRCD+tCAS); at a 1-core host the
// hop is 0 and the idle MC queue adds ~0, so effective idle ~= physical + adder.
//
// LITERATURE ANCHORS (idle load-to-use, cached demand-miss class):
//   DDR5   target ~110ns  adder 77  -- Sapphire Rapids ~107, Genoa 118
//                                       (73 IO-die + 35 device); server class.
//   HBM3   target ~235ns  adder 203 -- MI300A CPU->HBM3 measured 236-241ns
//                                       (arXiv 2508.12743), the only shipping
//                                       HBM3-CPU class. Penalty is the COMMAND-
//                                       INTERFACE + MC pipeline (low-clocked
//                                       parallel CA bus, pseudo-channel
//                                       arbitration, deep MC queueing) -- HBM
//                                       has NO SerDes; core timings comparable.
//   HBM2   target ~130ns  adder 98  -- Xeon Max HBM2e flat-mode measured 121-135.
//   DDR3   target ~110ns  adder 82  -- same server class as DDR5.
//   DDR4   target ~110ns  adder 83  -- same server class as DDR5.
//   LPDDR5 target ~120ns  adder 84  -- client-class fabric but slower device.
//   GDDR6  target ~150ns  adder 120 -- graphics MC pipeline (deep reordering).
//   SRAM   target ~physical adder 0 -- on-die, below any IO-die/fabric.
//   STT    target ~physical adder 0 -- on-die, same as SRAM (user decree).
//   ReRAM  adder 77 (= DDR5)        -- pcb-attached; device media slowness is
//   PCM    adder 77 (= DDR5)           already in the NVSim tech tables, so the
//                                      host-path adder is the DDR5-class fabric
//                                      cost only (avoid double-counting media).
//
// TODO(1.7.1+, HANDOFF "CACHED VS PURE ACCESS SPLIT"): this adder calibrates the
// CACHED demand-miss price only. The PURE serialized/uncached price
// (mem.uncached_ns, DDR5 ~200 / HBM3 ~335) belongs to the bridge/bypass step and
// is NOT implemented here.
//
// HOST-PATH DECOMPOSITION (1.7.2, HANDOFF "HOST-PATH DECOMPOSITION KNOBS"):
// the per-tech adder is expressed as the SUM of four documented components --
//   fabric_ns      IO-die / mesh / interconnect traversal (core->MC path)
//   coherence_ns   snoop / directory / coherence-engine latency
//   mc_pipeline_ns memory-controller command pipeline + queueing depth
//   phy_ns         PHY / command-interface wire + serialization tail
// The TOTAL is measurement-anchored (real-socket idle load-to-use, per the
// LITERATURE ANCHORS above); the four-way SPLIT is INFERRED -- no published
// per-component decomposition exists (user acceptance 2026-07-09: docs state
// the total is measured, the default split is a documented allocation, and any
// override makes the split the user's own modeling choice). The two anchored
// splits are HBM3 (90+60+45+8=203) and DDR5 (45+15+13+4=77); the rest are
// INFERRED allocations that SUM EXACTLY to the shipped 1.7.0 totals.
struct HostPathSplit {
    double fabric_ns    = 0.0;
    double coherence_ns = 0.0;
    double mc_pipeline_ns = 0.0;
    double phy_ns       = 0.0;
    double sum() const { return fabric_ns + coherence_ns + mc_pipeline_ns + phy_ns; }
};

// Per-tech DEFAULT decomposition. Every row SUMS to the pre-1.7.2 adder total
// (see getHostPathAdderNs history: DDR5 77, DDR4 83, DDR3 82, LPDDR5 84,
// GDDR6 120, HBM2 98, HBM3 203, SRAM/STT 0, ReRAM/PCM 77). LOCKED rows carry
// the two anchored splits; INFERRED rows apportion the same buckets sensibly.
static HostPathSplit getHostPathSplit(const std::string& tech_in) {
    std::string tech = tech_in;
    std::transform(tech.begin(), tech.end(), tech.begin(), ::toupper);
    HostPathSplit s;
    // 1.11.57 (latent B044): "DRAM" removed. It was grouped with DDR5 HERE and
    // with DDR4 in the MC table -- two different classifications of one alias --
    // and neither could fire, because canonicalMemTech() resolves it to DDR4
    // upstream of every caller. One less contradiction to inherit.
    if (tech == "DDR5")                        { s = {45, 15, 13, 4}; }  // LOCKED  sum 77
    else if (tech == "DDR4")                   { s = {48, 16, 15, 4}; }  // INFERRED sum 83
    else if (tech == "DDR3")                   { s = {48, 16, 14, 4}; }  // INFERRED sum 82
    else if (tech == "LPDDR5")                 { s = {49, 16, 15, 4}; }  // INFERRED sum 84
    else if (tech == "GDDR6")                  { s = {60, 20, 34, 6}; }  // INFERRED sum 120 (deep graphics MC reorder)
    else if (tech == "HBM2")                   { s = {43, 29, 22, 4}; }  // INFERRED sum 98  (interposer, HBM3-scaled)
    else if (tech == "HBM3")                   { s = {90, 60, 45, 8}; }  // LOCKED  sum 203
    else if (tech == "SRAM")                   { s = {0, 0, 0, 0}; }     // on-die, below any fabric
    else if (tech == "STT_MRAM" || tech == "STTMRAM" ||
             tech == "STT-MRAM" || tech == "MRAM") { s = {0, 0, 0, 0}; } // on-die, same as SRAM
    else if (tech == "RERAM" || tech == "RESISTIVE" ||
             tech == "MEMRISTOR")              { s = {45, 15, 13, 4}; }  // DDR5-class fabric, sum 77
    else if (tech == "PCM" || tech == "PCRAM" ||
             tech == "3DXPOINT")               { s = {45, 15, 13, 4}; }  // DDR5-class fabric, sum 77
    else                                       { s = {45, 15, 13, 4}; }  // unknown DRAM-class default, sum 77
    return s;
}

// Aggregate per-tech host-path adder (ns) = sum of the four decomposed
// components. Backward-compatible: same effective totals as pre-1.7.2. Retained
// as the documented aggregate entry point (emitHostMemBlock now composes the
// split directly to allow per-component overrides).
[[maybe_unused]] static double getHostPathAdderNs(const std::string& tech_in) {
    return getHostPathSplit(tech_in).sum();
}

// Emit the shared sys.mem block as the HOST main memory (Mode-1: host tech =
// device tech; under NO_OFFLOAD the host runs the whole kernel against it, and
// under offload the device is served by its own PE-MIs so this block is still
// the host's memory).
//
// PART-A design: reuse the EXISTING SimpleMemory/WeaveSimpleMemory M/D/1
// (Pollaczek-Khinchine) -- NOT a new analytic term (that would double-count the
// bandwidth cap the MC already prices; co-sim defect #4) and NOT a per-host
// Ramulator (the detailed host tier is a later 1.7.x increment). The host MC is
// a SINGLE M/D/1 queue clocked at the AGGREGATE bandwidth = channel_count x
// per-channel GB/s (DDR5 1 x 25.6; HBM3 16 x 51.2 = 819 GB/s). The utilization
// rho = host_demand / aggregate_bandwidth then saturates the one DDR5 channel
// under many host cores while HBM3's 32x-wider aggregate stays unsaturated --
// the designed contrast, from ONE honest queue.
//
// (A per-channel SplitAddrMemory fan-out over `controllers = channel_count` was
// prototyped but inflates single-core cycles ~40% at zero contention via a
// weave-phase multi-domain scheduling artifact; the aggregate single-queue form
// is the standard M/D/1 memory-BW model and avoids that artifact. The channel
// count enters as the aggregate multiplier, exactly the PART-A specification.)
static void emitHostMemBlock(std::ostream& out, const UnifiedConfig& config,
                             const UnifiedConfig::SystemNode& host) {
    double host_freq = (host.frequency_mhz > 0.0) ? host.frequency_mhz
                                                   : config.reference_frequency_mhz;
    // Idle per-access latency (cycles @ host clock). Physical composition first:
    // the DRAM tRCD+tCAS from getMemoryLatencyCycles() (DRAM path). Then add the
    // per-tech HOST-PATH ADDER (getHostPathAdderNs, ns) so the effective idle
    // host memory latency matches measured real sockets (DDR5 ~110, HBM3 ~235).
    // The adder is HOST-ROLE ONLY -- device (PE) pricing never routes here.
    /* 1.11.59 (audit round 3, B016): the RUN's temperature, not a literal.
     * 1.11.56 threaded the line size through here and left the temperature at
     * a hardcoded 350 K, while getHostMemBandwidth() a few lines below -- in
     * this same function, for this same array -- passes config.temperature_k.
     * One array characterized at two different temperatures, for its latency
     * and for its bandwidth. */
    int phys_latency = getMemoryLatencyCycles(host.memory_tech, host_freq,
                                              host.tech_node_nm, false, 0.0,
                                              -999, config.temperature_k,
                                              config.cache_line_size);  // 1.11.56 (B018)
    phys_latency = std::max(1, phys_latency);
    // Host-path adder (ns): the aggregate override wins outright; otherwise the
    // per-tech DECOMPOSED default split (getHostPathSplit) with any per-component
    // host_path.* override merged over it (1.7.2). Aggregate vs decomposed forms
    // are mutually exclusive (enforced as a config error at parse time).
    double adder_ns;
    if (host.mem_latency_adder_ns >= 0.0) {
        adder_ns = host.mem_latency_adder_ns;
    } else {
        HostPathSplit sp = getHostPathSplit(host.memory_tech);
        if (host.host_path_fabric_ns    >= 0.0) sp.fabric_ns    = host.host_path_fabric_ns;
        if (host.host_path_coherence_ns >= 0.0) sp.coherence_ns = host.host_path_coherence_ns;
        if (host.host_path_mc_pipeline_ns >= 0.0) sp.mc_pipeline_ns = host.host_path_mc_pipeline_ns;
        if (host.host_path_phy_ns       >= 0.0) sp.phy_ns       = host.host_path_phy_ns;
        adder_ns = sp.sum();
    }
    int adder_cycles = static_cast<int>(std::round(adder_ns * host_freq / 1000.0));
    if (adder_cycles < 0) adder_cycles = 0;
    int mem_latency = std::max(1, phys_latency + adder_cycles);
    // Diagnostic (stderr, ASCII, gated): host memory idle-latency composition in
    // ns for calibration/validation. Effective idle = physical(tRCD+tCAS) +
    // host-path adder (1-core host: hop 0, idle MC queue ~0).
    if (getenv("PIMID_DEBUG_HOSTMEM")) {
        double inv_ghz = 1000.0 / host_freq;  // ns per cycle
        HostPathSplit dbg = getHostPathSplit(host.memory_tech);
        if (host.host_path_fabric_ns    >= 0.0) dbg.fabric_ns    = host.host_path_fabric_ns;
        if (host.host_path_coherence_ns >= 0.0) dbg.coherence_ns = host.host_path_coherence_ns;
        if (host.host_path_mc_pipeline_ns >= 0.0) dbg.mc_pipeline_ns = host.host_path_mc_pipeline_ns;
        if (host.host_path_phy_ns       >= 0.0) dbg.phy_ns       = host.host_path_phy_ns;
        std::cerr << "[host-mem] tech=" << host.memory_tech
                  << " phys=" << (phys_latency * inv_ghz) << "ns"
                  << " adder=" << adder_ns << "ns";
        if (host.mem_latency_adder_ns >= 0.0) {
            std::cerr << " (aggregate override)";
        } else {
            std::cerr << " [fabric=" << dbg.fabric_ns
                      << " coherence=" << dbg.coherence_ns
                      << " mc_pipeline=" << dbg.mc_pipeline_ns
                      << " phy=" << dbg.phy_ns << "]";
        }
        std::cerr << " effective_idle=" << (mem_latency * inv_ghz) << "ns"
                  << " (" << mem_latency << " cy @ " << host_freq << "MHz)\n";
    }

    HostMemBW bw = getHostMemBandwidth(host.memory_tech,
                                       config.num_banks, config.cache_line_size,
                                       host.tech_node_nm, config.temperature_k,
                                       host_freq);   // 1.11.56 (B004)
    if (host.mem_bandwidth_mbs > 0) bw.per_channel_mbs = host.mem_bandwidth_mbs;
    if (host.mem_channels > 0)      bw.channels = host.mem_channels;
    // Aggregate host memory bandwidth = per-channel x channels (the M/D/1 cap).
    long long agg_mbs = (long long)bw.per_channel_mbs * (long long)bw.channels;
    if (agg_mbs < 1) agg_mbs = 1;

    // OoO / in-order host cores need the weave-phase MC (SimpleMemory subclass,
    // same M/D/1); ALU / simple cores use the plain Simple MC.
    bool weave = (host.core_type == "ooo_core" || host.core_type == "in_order_core");
    // Self-documenting (1.7.4): host main-memory technology + effective idle
    // latency. Under is_default_mem=true this tech = the device tech; under
    // false it is host.mem.technology (decoupled from the device / bridge tech).
    out << "    // host main memory (1.7.4): tech=" << host.memory_tech
        << " idle=" << std::fixed << std::setprecision(1)
        << (mem_latency * 1000.0 / host_freq) << "ns" << std::defaultfloat
        << " (" << mem_latency << " cy @ " << static_cast<int>(host_freq) << "MHz)\n";
    out << "    mem = {\n";
    if (weave) {
        out << "        type = \"WeaveSimple\";\n";
        out << "        latency = " << mem_latency << ";\n";
        out << "        bandwidth = " << agg_mbs << ";\n";
        out << "        boundLatency = " << mem_latency << ";\n";
    } else {
        out << "        type = \"Simple\";\n";
        out << "        latency = " << mem_latency << ";\n";
        out << "        bandwidth = " << agg_mbs << ";\n";
    }
    out << "        controllers = 1;\n";
    out << "    };\n";
}

//===========================================================================
// Host<->device TWO-LAYER BRIDGE defaults (1.7.1).
//
// bridge.protocol (native|ddr_t|cxl_mem|loadstore) selects the per-transaction
// overhead terms; bridge.phy (on_die|pcb|interposer|serdes) selects the
// physical-attach latency = WIRE + COMMAND-INTERFACE/MC PIPELINE (for HBM the
// pipeline is the low-clocked parallel CA bus + pseudo-channel arbitration + MC
// queueing -- HBM has NO SerDes, so the interposer phy latency is NEVER a
// "SerDes/PHY pipeline"). Anchors: Xeon Max HBM idle +22-25ns over DDR5, and
// MI300A CPU->HBM3 ~236-241ns as the loaded/serialized bound.
//
// ALL fields optional; unset fields derive from the DEVICE memory technology
// via the locked per-tech table below (BW = simulator L5 channel rates).
//===========================================================================
struct BridgeDefaults {
    std::string protocol;          // native | ddr_t | cxl_mem | loadstore
    std::string phy;               // on_die | pcb | interposer | serdes
    double bandwidth_gbs;          // PER-CHANNEL GB/s
    double latency_ns;             // wire + command-interface/MC pipeline
    int    channels;
    double protocol_overhead_ns;   // per-transaction protocol handshake
    double uncached_ns;            // pure serialized cross-bridge access
};

// Locked per-tech defaults (HANDOFF ISSUE 6 table + calibration corrections).
static BridgeDefaults bridgeDefaultsForTech(const std::string& tech_in) {
    std::string t = tech_in;
    std::transform(t.begin(), t.end(), t.begin(), ::toupper);
    // canonicalize NVM spellings
    if (t == "STTMRAM" || t == "STT-MRAM" || t == "MRAM") t = "STT_MRAM";
    if (t == "PCRAM" || t == "3DXPOINT") t = "PCM";
    if (t == "RESISTIVE" || t == "MEMRISTOR") t = "RERAM";

    // {protocol, phy, bw/ch GB/s, latency_ns (wire+pipeline), channels,
    //  protocol_overhead_ns, uncached_ns}
    if (t == "DDR3")     return {"native",    "pcb",        12.8, 20.0,  1,  0.0, 200.0};
    if (t == "DDR4")     return {"native",    "pcb",        19.2, 20.0,  1,  0.0, 200.0};
    // 1.11.57 (latent B044): "DRAM" removed -- resolved to DDR4 upstream, and
    // this table classed it with DDR5 while the MC table classed it with DDR4.
    if (t == "DDR5")     return {"native",    "pcb",        25.6, 18.0,  1,  0.0, 200.0};
    if (t == "LPDDR5")   return {"native",    "pcb",        12.8, 12.0,  1,  0.0, 200.0};
    if (t == "GDDR6")    return {"native",    "pcb",        32.0, 10.0,  2,  0.0, 200.0};
    // HBM interposer latency = 5-6ns wire + ~24-25ns command-interface/MC
    // pipeline = ~30ns (NOT SerDes). uncached bound from MI300A anchor.
    if (t == "HBM2")     return {"native",    "interposer", 38.4, 30.0,  8,  0.0, 230.0};
    if (t == "HBM3")     return {"native",    "interposer", 51.2, 30.0, 16,  0.0, 335.0};
    // On-die load/store port at core clock (~1-2ns). STT locked == SRAM.
    if (t == "SRAM")     return {"loadstore", "on_die",    128.0,  2.0,  1,  0.0,  30.0};
    if (t == "STT_MRAM") return {"loadstore", "on_die",    128.0,  2.0,  1,  0.0,  30.0};
    // ReRAM = middle rung: DDR-transactional attach over a pcb + a ~30ns
    // media/handshake overhead on top of the ~18ns pcb pipeline.
    if (t == "RERAM")    return {"ddr_t",     "pcb",        25.6, 18.0,  1, 30.0, 250.0};
    // PCM = post-Optane CXL.mem over SerDes: ~150-250ns RT (200 default) +
    // CXL flit packetization overhead.
    if (t == "PCM")      return {"cxl_mem",   "serdes",     64.0, 200.0, 1, 40.0, 300.0};
    // Unknown -> DDR5-class commodity default.
    return {"native", "pcb", 25.6, 18.0, 1, 0.0, 200.0};
}

// Default per-transaction overhead for a protocol (used when the user overrides
// bridge.protocol without giving protocol_overhead_ns).
static double bridgeProtocolOverheadNs(const std::string& protocol) {
    if (protocol == "ddr_t")   return 30.0;  // DDR-transactional handshake
    if (protocol == "cxl_mem") return 40.0;  // CXL flit packetization
    return 0.0;                              // native / loadstore = direct MC
}

// Validate a (protocol, phy) combination. Returns "" if legal, else an error
// message. Rules (HANDOFF ISSUE 6): cxl_mem needs serdes; loadstore needs
// on_die; native forbidden on serdes (native needs the tech's own MC class).
static std::string bridgeValidityError(const std::string& protocol,
                                       const std::string& phy) {
    static const char* PROTOS[] = {"native", "ddr_t", "cxl_mem", "loadstore"};
    static const char* PHYS[]   = {"on_die", "pcb", "interposer", "serdes"};
    bool proto_ok = false, phy_ok = false;
    for (auto* p : PROTOS) if (protocol == p) proto_ok = true;
    for (auto* p : PHYS)   if (phy == p)       phy_ok = true;
    if (!proto_ok)
        return "bridge.protocol '" + protocol +
               "' invalid (native|ddr_t|cxl_mem|loadstore)";
    if (!phy_ok)
        return "bridge.phy '" + phy +
               "' invalid (on_die|pcb|interposer|serdes)";
    if (protocol == "cxl_mem" && phy != "serdes")
        return "bridge.protocol 'cxl_mem' requires phy 'serdes' (got '" + phy + "')";
    if (protocol == "loadstore" && phy != "on_die")
        return "bridge.protocol 'loadstore' requires phy 'on_die' (got '" + phy + "')";
    if (protocol == "native" && phy == "serdes")
        return "bridge.protocol 'native' forbidden on phy 'serdes' "
               "(native requires the tech's own memory-controller class)";
    return "";
}

// Resolve the effective bridge parameters for a device tech, applying user
// overrides on top of the locked defaults and validating the combo. On a
// validity error prints to stderr and exits (config error, not a warning).
// Fills config.bridge_* resolved fields in place.
static BridgeDefaults resolveBridge(UnifiedConfig& config,
                                    const std::string& device_tech) {
    BridgeDefaults d = bridgeDefaultsForTech(device_tech);
    // Protocol / phy: user override or tech default.
    std::string protocol = config.bridge_protocol.empty() ? d.protocol
                                                           : config.bridge_protocol;
    std::string phy = config.bridge_phy.empty() ? d.phy : config.bridge_phy;

    std::string err = bridgeValidityError(protocol, phy);
    if (!err.empty()) {
        std::cerr << "ERROR: illegal host<->device bridge configuration: "
                  << err << ".\n"
                  << "  Legal rules: cxl_mem->serdes, loadstore->on_die, "
                  << "native forbidden on serdes.\n";
        exit(1);
    }

    d.protocol = protocol;
    d.phy = phy;
    // Numeric fields: user override (>=0 / >0) or tech default. If the user
    // overrode protocol but not the overhead, fall to the protocol's default
    // overhead so an override reads sensibly.
    if (config.bridge_bandwidth_gbs > 0.0) d.bandwidth_gbs = config.bridge_bandwidth_gbs;
    if (config.bridge_latency_ns >= 0.0)   d.latency_ns = config.bridge_latency_ns;
    if (config.bridge_channels > 0)        d.channels = config.bridge_channels;
    if (config.bridge_protocol_overhead_ns >= 0.0)
        d.protocol_overhead_ns = config.bridge_protocol_overhead_ns;
    else if (!config.bridge_protocol.empty())
        d.protocol_overhead_ns = bridgeProtocolOverheadNs(protocol);
    if (config.bridge_uncached_ns >= 0.0)  d.uncached_ns = config.bridge_uncached_ns;
    return d;
}

// Emit the sys.bridge { ... } block (cycle-converted at the host/reference
// clock the plugin's host cores run on) into a system-scope ZSim config.
static void emitZSimBridgeBlock(std::ostream& out, const BridgeDefaults& d,
                                double ref_freq_mhz) {
    double f = (ref_freq_mhz > 0.0) ? ref_freq_mhz : 1000.0;
    uint32_t phy_cyc = (uint32_t)(d.latency_ns * f / 1000.0 + 0.5);
    uint32_t proto_cyc = (uint32_t)(d.protocol_overhead_ns * f / 1000.0 + 0.5);
    uint32_t unc_cyc = (uint32_t)(d.uncached_ns * f / 1000.0 + 0.5);
    // Aggregate bandwidth = per-channel x channels (all channels serve a bulk
    // transfer in parallel); bytes/cycle at the host/reference clock.
    double agg_gbs = d.bandwidth_gbs * (double)d.channels;
    double bytes_per_cycle = (agg_gbs * 1e9) / (f * 1e6);
    out << "\n    bridge = {\n";
    out << "        enabled = 1;\n";
    out << "        protocol = \"" << d.protocol << "\";\n";
    out << "        phy = \"" << d.phy << "\";\n";
    out << "        phyLatencyCycles = " << phy_cyc << ";\n";
    out << "        protocolOverheadCycles = " << proto_cyc << ";\n";
    out << "        channels = " << d.channels << ";\n";
    out << "        bytesPerCycle = \"" << std::fixed << std::setprecision(4)
        << bytes_per_cycle << "\";\n" << std::defaultfloat;
    out << "        uncachedCycles = " << unc_cyc << ";\n";
    out << "    };\n";
}

// Emit the sys.coherence { ... } block (1.7.2, Case-1 flush accounting). The
// writeback bandwidth is cycle-converted to bytes/cycle at the host/reference
// clock the host cores run on; footprintBytes stays as a decimal string
// (64-bit). mode=unified charges the flush at roi_begin; mode=separate emits the
// block disabled-for-flush (Case-2 cache bypass -> no flush, the 1.7.1 bridge
// bulk-DMA path already prices the crossing).
static void emitZSimCoherenceBlock(std::ostream& out, const UnifiedConfig& config,
                                   double writeback_bw_gbs, double ref_freq_mhz) {
    double f = (ref_freq_mhz > 0.0) ? ref_freq_mhz : 1000.0;
    bool unified = (config.coherence_mode != "separate");
    uint32_t fixed_cyc = (uint32_t)(config.coherence_flush_fixed_ns * f / 1000.0 + 0.5);
    // bytes/cycle at the host/reference clock: GB/s * 1e9 / (MHz * 1e6).
    double wb_bytes_per_cycle = (writeback_bw_gbs > 0.0)
                                    ? (writeback_bw_gbs * 1e9) / (f * 1e6)
                                    : 0.0;
    out << "\n    coherence = {\n";
    out << "        enabled = 1;\n";
    out << "        mode = " << (unified ? 0 : 1) << ";  // 0=unified(flush) 1=separate(bypass)\n";
    out << "        footprintBytes = \"" << config.coherence_footprint_bytes << "\";\n";
    out << "        writebackBytesPerCycle = \"" << std::fixed << std::setprecision(4)
        << wb_bytes_per_cycle << "\";\n" << std::defaultfloat;
    out << "        flushFixedCycles = " << fixed_cyc << ";\n";
    out << "    };\n";
    if (getenv("PIMID_DEBUG_COHERENCE")) {
        // Login-node observable: the deterministic flush charge the plugin will
        // apply at roi_begin (unified) or skip (separate).
        double flush_cyc = fixed_cyc;
        // 1.11.59 (F021): the footprint is always measured, so there is no
        // configured-size term to add here.
        (void)wb_bytes_per_cycle;
        double flush_ns = flush_cyc * 1000.0 / f;
        /* 1.11.40 (N7): with footprint 0 the flush size is not knowable on the
         * login node -- it is measured at roi_begin from the caches' dirty
         * lines. Say that, rather than printing a 0 that reads as "no flush". */
        std::cerr << "[coherence] mode=" << (unified ? "unified" : "separate")
                  << " footprint=MEASURED at roi_begin (host dirty lines)"   // 1.11.59 (F021)
                  << " writeback_bw=" << writeback_bw_gbs << "GB/s"
                  << " fixed=" << config.coherence_flush_fixed_ns << "ns"
                  << " -> flush=" << (unified ? (uint64_t)flush_cyc : 0)
                  << " cy (" << (unified ? flush_ns : 0.0) << "ns @ " << f << "MHz)\n";
    }
}

// Deterministic bridge crossing cost (cycles) for `bytes`, mirroring the plugin's
// getBridgeLatency: phy pipeline + protocol overhead + size/aggregate-BW
// serialization. Used only for the emitted launch block's debug preview; the
// plugin recomputes it at runtime from the emitted sys.bridge.* fields.
static uint32_t bridgeCrossingCycles(const BridgeDefaults& d, uint32_t bytes,
                                     double ref_freq_mhz) {
    double f = (ref_freq_mhz > 0.0) ? ref_freq_mhz : 1000.0;
    uint32_t phy_cyc = (uint32_t)(d.latency_ns * f / 1000.0 + 0.5);
    uint32_t proto_cyc = (uint32_t)(d.protocol_overhead_ns * f / 1000.0 + 0.5);
    double agg_gbs = d.bandwidth_gbs * (double)d.channels;
    double bytes_per_cycle = (agg_gbs * 1e9) / (f * 1e6);
    uint32_t ser = 0;
    if (bytes_per_cycle > 0.0 && bytes > 0)
        ser = (uint32_t)std::ceil((double)bytes / bytes_per_cycle);
    return phy_cyc + proto_cyc + ser;
}

// Emit the sys.launch { ... } block (1.7.3, HANDOFF ISSUE 2). doorbell/dispatch
// ns are cycle-converted at the host/reference clock; cmd/ack bytes cross the
// two-layer bridge at runtime. The plugin charges the total on the host core at
// the offload doorbell (co-sim roi_begin / WORK_BEGIN), before device migration.
static void emitZSimLaunchBlock(std::ostream& out, const UnifiedConfig& config,
                                const BridgeDefaults& d, double ref_freq_mhz) {
    double f = (ref_freq_mhz > 0.0) ? ref_freq_mhz : 1000.0;
    uint32_t doorbell_cyc = (uint32_t)(config.launch_doorbell_ns * f / 1000.0 + 0.5);
    uint32_t dispatch_cyc = (uint32_t)(config.launch_dispatch_ns * f / 1000.0 + 0.5);
    uint32_t cmd_bytes = (uint32_t)(config.launch_cmd_bytes > 0 ? config.launch_cmd_bytes : 0);
    uint32_t ack_bytes = (uint32_t)(config.launch_ack_bytes > 0 ? config.launch_ack_bytes : 0);
    out << "\n    launch = {\n";
    out << "        enabled = 1;\n";
    out << "        doorbellCycles = " << doorbell_cyc << ";\n";
    out << "        dispatchCycles = " << dispatch_cyc << ";\n";
    out << "        cmdBytes = " << cmd_bytes << ";\n";
    out << "        ackBytes = " << ack_bytes << ";\n";
    out << "    };\n";
    if (getenv("PIMID_DEBUG_LAUNCH")) {
        // Login-node observable: the deterministic launch charge the plugin will
        // apply at the offload doorbell = doorbell + dispatch + 2 bridge crossings.
        uint32_t cmd_cyc = bridgeCrossingCycles(d, cmd_bytes, f);
        uint32_t ack_cyc = bridgeCrossingCycles(d, ack_bytes, f);
        uint64_t total = (uint64_t)doorbell_cyc + dispatch_cyc + cmd_cyc + ack_cyc;
        double total_ns = (double)total * 1000.0 / f;
        std::cerr << "[launch] tech=" << d.protocol << "/" << d.phy
                  << " doorbell=" << config.launch_doorbell_ns << "ns(" << doorbell_cyc << "cy)"
                  << " dispatch=" << config.launch_dispatch_ns << "ns(" << dispatch_cyc << "cy)"
                  << " bridge_cmd(" << cmd_bytes << "B)=" << cmd_cyc << "cy"
                  << " bridge_ack(" << ack_bytes << "B)=" << ack_cyc << "cy"
                  << " -> total=" << total << "cy (" << total_ns << "ns @ " << f << "MHz)\n";
    }
}

static std::string generateSystemConfig(UnifiedConfig& config) {
    std::string cfg_path = "/tmp/pimid_system_zsim_" + std::to_string(getpid()) + ".cfg";
    std::ofstream cfg(cfg_path);
    if (!cfg.is_open()) return "";

    double ref_freq = config.reference_frequency_mhz;
    int total_cores = 0;
    for (const auto& n : config.system_nodes) total_cores += n.num_cores;

    cfg << "// Auto-generated ZSim config by PIMID (System multi-node mode)\n";
    cfg << "// Nodes: " << config.system_nodes.size() << ", Total cores: " << total_cores << "\n";
    cfg << "// Reference frequency: " << ref_freq << " MHz\n\n";

    cfg << "sys = {\n";
    cfg << "    lineSize = " << config.cache_line_size << ";\n";
    cfg << "    frequency = " << static_cast<int>(ref_freq) << ";\n\n";

    // -- Cores --
    cfg << "    cores = {\n";
    for (size_t node_idx = 0; node_idx < config.system_nodes.size(); node_idx++) {
        const auto& node = config.system_nodes[node_idx];
        if (node.num_cores == 0) continue;  // memory-only

        std::string group_name = node.name + (node.role == UnifiedConfig::SystemNode::HOST ? "_cores" : "_pes");
        std::string core_type = "Simple";
        bool is_alu = false;

        if (node.core_type == "ooo_core") core_type = "OoO";
        else if (node.core_type == "in_order_core") core_type = "InOrder";
        else if (node.core_type == "alu_core") { core_type = "ALU"; is_alu = true; }
        else if (node.core_type == "null_core") core_type = "Null";
        // simple_core / unknown -> "Simple" (coarse SimpleCore)

        cfg << "        " << group_name << " = {\n";
        cfg << "            type = \"" << core_type << "\";\n";
        cfg << "            cores = " << node.num_cores << ";\n";
        /* 1.11.43 (audit E23): the FPU flag is a property of THIS GROUP's
         * cores, not of the simulation. It was one global
         * (sys.hierarchy.peHasFpu), so a co-sim HOST with an in-order core
         * charged the DEVICE's soft-float penalty on its own FP instructions
         * -- pim.pe.fp_emulation_cycles is a PE property that leaked onto a
         * host with a real FPU. Emitted per group: device groups carry the
         * configured PE values; host groups always have an FPU. */
        /* 1.11.43 (user ruling): BOTH roles carry their own node's FPU
         * capability -- the host is configurable too (host-level
         * floating_point / fp_emulation_cycles keys), defaulting to a real
         * FPU. No role forces the other's value. */
        cfg << "            hasFpu = " << (node.pe_has_fpu ? "true" : "false") << ";\n";
        cfg << "            fpEmulCycles = " << node.pe_fp_emul_cycles << ";\n";

        if (is_alu) {
            // ALU factors are clock-INVARIANT: a device PE's cycle count must be
            // the same whether the coupled co-sim runs it at the reference clock
            // or a slower domain clock. The compute/access factors are therefore
            // emitted exactly as device scope emits them (no freq_scale). Baking
            // freq_scale here inflated the device's reported curCycle by
            // freq_scale (e.g. x4 for a 500 MHz device under a 2000 MHz
            // reference), so a device-scope-equivalent run reported ~4x its true
            // cycle count purely because the host clock was faster. The clock
            // difference is a downstream wall-clock effect (cycles / node_freq),
            // NOT a change in the simulated cycle count. accessFactor is the
            // user's flat per-access knob ONLY -- the memory-technology cost comes
            // from the device model itself (PE-MI -> device NoC -> memory), the
            // same path device scope uses. Never bake tech latency in here: it
            // would double-count the device model.
            double scaled_compute = node.alu_compute_factor;
            double scaled_access = node.alu_access_factor;
            cfg << std::fixed << std::setprecision(2);
            cfg << "            computeFactor = " << scaled_compute << ";\n";
            cfg << "            accessFactor = " << scaled_access << ";\n";
            cfg << "            throughputFactor = " << node.alu_throughput_factor << ";\n";
            cfg << std::defaultfloat;
            cfg << "            operandWidth = " << node.alu_operand_width << ";\n";
            /* 1.11.56 (audit B054): the datapath shape, which device scope has
             * emitted since the key existed. Without it a bit-serial element
             * was co-simulated as a parallel one. */
            cfg << "            bitSerial = " << (node.alu_bit_serial ? "true" : "false") << ";\n";
            cfg << std::fixed << std::setprecision(2);
            cfg << "            energyFactor = " << node.alu_energy_factor << ";\n";
            cfg << std::defaultfloat;
        } else {
            std::string dcache = node.name + "_l1d";
            std::string icache = node.name + "_l1i";
            cfg << "            dcache = \"" << dcache << "\";\n";
            cfg << "            icache = \"" << icache << "\";\n";
            /* 1.11.56 (audit B054): device scope emits issueWidth for an
             * in-order core; system scope did not, so pim.pe.issue_width was
             * inert in co-sim and every in-order PE kept the built-in dual
             * issue no matter what the config asked for. */
            if (core_type == "InOrder")
                cfg << "            issueWidth = " << node.inorder_issue_width << ";\n";
        }

        cfg << "        };\n";
    }
    cfg << "    };\n\n";

    // -- Caches --
    bool any_caches = false;
    for (const auto& node : config.system_nodes) {
        if (node.num_cores == 0) continue;
        if (node.core_type == "alu_core") continue;
        if (node.core_type == "null_core") continue;  // Null: no caches
        any_caches = true;
        break;
    }

    if (any_caches) {
        cfg << "    caches = {\n";
        for (const auto& node : config.system_nodes) {
            if (node.num_cores == 0) continue;
            if (node.core_type == "alu_core") continue;  // ALU: no caches
            if (node.core_type == "null_core") continue; // Null: no caches

            int node_freq = static_cast<int>(ref_freq);  // use reference freq for latency calc
            int l1d_lat = getCacheLatencyCycles(node.l1d_kb, node.l1d_ways, config.cache_line_size,
                                                 node_freq, node.tech_node_nm, /*fallback_cycles=*/4);
            int l1i_lat = getCacheLatencyCycles(node.l1i_kb, node.l1i_ways, config.cache_line_size,
                                                 node_freq, node.tech_node_nm, /*fallback_cycles=*/3);
            l1d_lat = std::max(1, l1d_lat);
            l1i_lat = std::max(1, l1i_lat);

            cfg << "        " << node.name << "_l1d = {\n";
            cfg << "            caches = " << node.num_cores << ";\n";
            cfg << "            size = " << (node.l1d_kb * 1024) << ";\n";
            cfg << "            array = { type = \"SetAssoc\"; ways = " << node.l1d_ways << "; };\n";
            cfg << "            latency = " << l1d_lat << ";\n";
            cfg << "        };\n";

            cfg << "        " << node.name << "_l1i = {\n";
            cfg << "            caches = " << node.num_cores << ";\n";
            cfg << "            size = " << (node.l1i_kb * 1024) << ";\n";
            cfg << "            array = { type = \"SetAssoc\"; ways = " << node.l1i_ways << "; };\n";
            cfg << "            latency = " << l1i_lat << ";\n";
            cfg << "        };\n";

            if (node.l2_kb > 0) {
                int l2_lat = getCacheLatencyCycles(node.l2_kb, node.l2_ways, config.cache_line_size,
                                                    node_freq, node.tech_node_nm, /*fallback_cycles=*/12);
                l2_lat = std::max(1, l2_lat);
                cfg << "        " << node.name << "_l2 = {\n";
                cfg << "            caches = " << node.num_cores << ";\n";
                cfg << "            size = " << (node.l2_kb * 1024) << ";\n";
                cfg << "            array = { type = \"SetAssoc\"; ways = " << node.l2_ways << "; };\n";
                cfg << "            children = \"" << node.name << "_l1i|" << node.name << "_l1d\";\n";
                cfg << "            latency = " << l2_lat << ";\n";
                cfg << "        };\n";
            }

            if (node.l3_kb > 0) {
                int l3_lat = getCacheLatencyCycles(node.l3_kb, node.l3_ways, config.cache_line_size,
                                                    node_freq, node.tech_node_nm, /*fallback_cycles=*/20);
                l3_lat = std::max(1, l3_lat);
                std::string parent = (node.l2_kb > 0) ? (node.name + "_l2") : (node.name + "_l1i|" + node.name + "_l1d");
                cfg << "        " << node.name << "_l3 = {\n";
                cfg << "            caches = 1;\n";
                cfg << "            size = " << (node.l3_kb * 1024) << ";\n";
                cfg << "            array = { type = \"SetAssoc\"; ways = " << node.l3_ways << "; };\n";
                cfg << "            children = \"" << parent << "\";\n";
                cfg << "            latency = " << l3_lat << ";\n";
                cfg << "        };\n";
            }
        }
        cfg << "    };\n\n";
    }

    // -- Memory (flat address space with SystemAddressRouter) --
    // Count devices that contribute memory
    int num_mem_devices = 0;
    for (const auto& n : config.system_nodes) {
        if (n.role == UnifiedConfig::SystemNode::DEVICE) num_mem_devices++;
    }

    // The DEVICE's own clock, captured here so the hierarchy block can clock the
    // device memory-bandwidth contention at the device frequency (not the
    // reference/host clock = sys.frequency). First device node wins; the device
    // memory hierarchy belongs to the device. 0 -> falls back to ref in the
    // emitter (single-host degenerate case).
    double sys_device_bw_freq_mhz = 0.0;
    for (const auto& n : config.system_nodes) {
        if (n.role == UnifiedConfig::SystemNode::DEVICE && n.frequency_mhz > 0.0) {
            sys_device_bw_freq_mhz = n.frequency_mhz;
            break;
        }
    }

    if (num_mem_devices > 1) {
        // Multi-device: SystemAddressRouter
        cfg << "    mem = {\n";
        cfg << "        type = \"SystemRouter\";\n";
        cfg << "        numDevices = " << num_mem_devices << ";\n";

        int dev_idx = 0;
        for (const auto& node : config.system_nodes) {
            if (node.role != UnifiedConfig::SystemNode::DEVICE) continue;

            // Auto-derive MC type from technology
            std::string mc_type = "Simple";
            // CLOCK-INVARIANT device memory latency: the access latency in cycles
            // must use the DEVICE's own clock, not the reference/max (host) clock.
            // getMemoryLatencyCycles converts latency_ns -> cycles via freq_mhz;
            // passing ref_freq (= max(all node freqs) = host) inflated the device's
            // memory cycles by host/device (e.g. x5 at host4000/dev800), leaking the
            // host clock into the DEVICE cycle count. Device scope uses the device's
            // own frequency here (see config.frequency_mhz path), so match it.
            double dev_freq = (node.frequency_mhz > 0.0) ? node.frequency_mhz : ref_freq;
            /* 1.11.56 (audit B053): pass the PLACEMENT and the operating
             * temperature, as device scope does. Five arguments left
             * pe_hierarchy_level at its -999 sentinel, which skips the 1.11.25
             * tier model entirely and falls through to the flat
             * characterization -- so a SUBARRAY-placed and a CHIP-placed PE
             * were charged the identical array access in every co-simulated
             * run, and the tier spread the model exists to produce (up to
             * 148x for STT-MRAM between array units) was inert. */
            int mem_latency = getMemoryLatencyCycles(node.memory_tech, dev_freq,
                                                     node.tech_node_nm, false, 0.0,
                                                     config.pe_hierarchy_level,
                                                     config.temperature_k,
                                                     config.cache_line_size);  // 1.11.56 (B018)
            mem_latency = std::max(1, mem_latency);

            std::string tech_upper = node.memory_tech;
            std::transform(tech_upper.begin(), tech_upper.end(), tech_upper.begin(), ::toupper);

            // All devices use Simple (M/D/1) in multi-device SystemRouter mode.
            // Latency from external models (Ramulator/NVSim/CACTI), M/D/1 adds contention.
            // Ramulator2 full instances are too heavy for multi-device shared memory.
            mc_type = "Simple";
            int md1_bw = 6400;  // default bandwidth MB/s

            bool is_dram = pimid::isDRAM(pimid::parseMemoryTechnology(node.memory_tech));
            if (is_dram) {
                // Derive bandwidth from Ramulator wrapper (same as single-device path)
                try {
                    pimid::RamulatorWrapper bw_query("", tech_upper);
                    bw_query.initialize();
                    double rank_bw_gbs = bw_query.getRankBandwidth();
                    md1_bw = static_cast<int>(rank_bw_gbs * 1000.0);
                }
                /* 1.11.57 (latent B023): the "12.8 GB/s fallback" is a
                 * written-down number that was emitted straight into the
                 * device block as `bandwidth = ...`, where the plugin reads it
                 * as this memory's measured rate and saturates its M/D/1
                 * against it. It matches no technology in the lineup in
                 * particular. The sibling NVM branch below already refuses to
                 * substitute; the DRAM branch now refuses too, because for a
                 * DRAM part the preset is the only datasheet there is. */
                catch (const std::exception& e) {
                    refuseWithoutDramOracle(tech_upper, e.what(),
                        std::string("the emitted M/D/1 bandwidth of device node '")
                        + node.name + "'");
                } catch (...) {
                    refuseWithoutDramOracle(tech_upper,
                        "a non-standard exception escaped the wrapper",
                        std::string("the emitted M/D/1 bandwidth of device node '")
                        + node.name + "'");
                }
            } else {
                /* NVM/SRAM: ASK THE ARRAY MODEL.
                 *
                 * 1.11.57 (audit round 3, B012): the last site still dividing
                 * by an invented access time. 1.11.56 (B004) replaced the
                 * 10/50/20/2 ns literals with the array model's characterized
                 * read latency at the device-scope site and at the host site,
                 * and missed this one -- the multi-device system-scope emit --
                 * so a third copy of the same assumption kept setting the
                 * emitted per-device bandwidth. Same derivation as the other
                 * two, at this node's own process node and line size. */
                double acc_ns = -1.0;
                try {
                    int lat_cy = getMemoryLatencyCycles(tech_upper, dev_freq,
                                                        node.tech_node_nm, false, -1.0,
                                                        -999, config.temperature_k,
                                                        config.cache_line_size);
                    if (lat_cy > 0 && dev_freq > 0)
                        acc_ns = lat_cy * 1000.0 / dev_freq;
                } catch (const std::exception&) { /* announced below */ }
                if (acc_ns > 0.0) {
                    md1_bw = static_cast<int>(config.num_banks *
                                              config.cache_line_size * 1000.0 / acc_ns);
                } else {
                    std::cerr << "[bw] WARNING: no array access time for "
                              << tech_upper << " on node '" << node.name
                              << "', so its emitted M/D/1 bandwidth is not a "
                                 "property of this array." << std::endl;
                }
            }

            cfg << "        device" << dev_idx << " = {\n";
            cfg << "            type = \"" << mc_type << "\";\n";
            cfg << "            addrStart = " << node.addr_start << ";\n";
            cfg << "            addrEnd = " << node.addr_end << ";\n";
            cfg << "            tech = \"" << node.memory_tech << "\";\n";
            cfg << "            latency = " << mem_latency << ";\n";
            cfg << "            networkNodeId = " << node.node_network_id << ";\n";
            cfg << "            bandwidth = " << md1_bw << ";\n";
            cfg << "        };\n";
            dev_idx++;
        }
        cfg << "    };\n\n";
    } else {
        // Single device. The shared sys.mem block is the HOST main memory: in
        // co-sim the device is served by its own PE-MIs (init.cpp clears `mems`
        // then rebuilds the host MC from sys.mem), and under NO_OFFLOAD the host
        // runs the whole kernel against it. Emit it from the HOST node so the
        // host memory carries the correct per-tech bandwidth + channel count and
        // the existing SimpleMemory/WeaveSimpleMemory M/D/1 saturates honestly.
        const UnifiedConfig::SystemNode* host_node = nullptr;
        for (const auto& node : config.system_nodes) {
            if (node.role == UnifiedConfig::SystemNode::HOST) { host_node = &node; break; }
        }
        if (host_node) {
            emitHostMemBlock(cfg, config, *host_node);
            cfg << "\n";
        } else {
            // No host node (pure device system scope): legacy device-MC emit.
            for (const auto& node : config.system_nodes) {
                if (node.role != UnifiedConfig::SystemNode::DEVICE) continue;
                // CLOCK-INVARIANT device memory latency: convert to cycles at the
                // DEVICE's own clock, not the reference/max (host) clock.
                double dev_freq = (node.frequency_mhz > 0.0) ? node.frequency_mhz : ref_freq;
                // 1.11.56 (audit B053/B018): placement, temperature and line size.
                int mem_latency = getMemoryLatencyCycles(node.memory_tech, dev_freq,
                                                         node.tech_node_nm, false, 0.0,
                                                         config.pe_hierarchy_level,
                                                         config.temperature_k,
                                                         config.cache_line_size);
                mem_latency = std::max(1, mem_latency);
                emitZSimMemBlock(cfg, config, mem_latency);
                cfg << "\n";
                break;
            }
        }
    }

    // -- Network (for device-internal NoC) --
    emitZSimNetworkBlock(cfg, config);

    // -- Host<->device link -> offload M/D/1 charge --
    // The explicit system.network.links path sets per-hop linkLatency_N_M but
    // does not populate the pcie.* struct that getPCIeLatency() uses to charge
    // the synchronous-offload (WORK_BEGIN/END) transfer. Derive those fields
    // from the first host<->device link so the chosen link_type (pcie/cxl/
    // interposer) also drives the offload transfer cost, not only hop latency.
    {
        for (const auto& lnk : config.system_network.links) {
            bool host_dev = false;
            for (const auto& n : config.system_nodes) {
                if ((n.name == lnk.src_name || n.name == lnk.dst_name) &&
                    n.role == UnifiedConfig::SystemNode::DEVICE) host_dev = true;
            }
            /* 1.11.51 (N10): a declared link must never be IGNORED IN
             * SILENCE. The reference cosim.yaml declares only type+width --
             * no src/dst -- so the host_dev test failed and the entry was
             * skipped without a word; config.pcie_link_type stayed at its
             * default, which HAPPENED to match the reference's declared
             * type, hiding the skip for the whole corpus until a gate
             * edited the type and nothing changed (1157c). Two closures:
             * a src/dst-less entry in a system with a device is applied by
             * INFERENCE (one host-device link is what it can mean), and
             * every other skip states itself and what was kept. */
            if (!host_dev && lnk.src_name.empty() && lnk.dst_name.empty()) {
                for (const auto& n : config.system_nodes)
                    if (n.role == UnifiedConfig::SystemNode::DEVICE)
                        { host_dev = true; break; }
                if (host_dev)
                    std::cout << "  [xing] links entry '" << lnk.link_type
                              << "' has no src/dst; applied to the host-device "
                                 "link by inference." << std::endl;
            }
            if (!host_dev) {
                std::cerr << "  [xing] WARNING: links entry '" << lnk.link_type
                          << "' (src='" << lnk.src_name << "' dst='"
                          << lnk.dst_name << "') names no DEVICE node -- NOT "
                             "applied; link type stays '"
                          << config.pcie_link_type << "'." << std::endl;
                continue;
            }
            if (lnk.base_latency_ns <= 0.0 || lnk.bandwidth_GBs <= 0.0) {
                std::cerr << "  [xing] WARNING: links entry '" << lnk.link_type
                          << "' carries no timing (base_latency_ns="
                          << lnk.base_latency_ns << ", bandwidth_GBs="
                          << lnk.bandwidth_GBs << ") -- the TYPE is applied "
                             "for energy pricing but the offload timing keeps "
                             "its current source." << std::endl;
                /* the type is still authoritative (D8): apply it before
                 * skipping the timing fields. */
                if (config.pcie_timing_configured &&
                    config.pcie_link_type != lnk.link_type)
                    std::cout << "  [xing] link type from the declared "
                                 "topology: " << lnk.link_type << std::endl;
                config.pcie_link_type = lnk.link_type;
                continue;
            }
            /* 1.11.19 (user decision D8): the TIMING link type is
             * AUTHORITATIVE for energy. This sync used to be skipped whenever
             * a power.pcie block existed, so a config could time an
             * interposer while pricing PCIe-gen5 -- a 14x skew between two
             * views of one wire. The link TYPE now always comes from the
             * declared topology; power.pcie keeps only its pJ/bit override
             * (a real override since 1.11.17). Disagreement is impossible by
             * construction rather than validated after the fact. */
            if (config.pcie_timing_configured &&
                config.pcie_link_type != lnk.link_type) {
                std::cout << "  [xing] link type from the declared topology: "
                          << lnk.link_type << " (power.pcie said '"
                          << config.pcie_link_type
                          << "'; the timing view wins -- one wire, one type)"
                          << std::endl;
            }
            config.pcie_link_type = lnk.link_type;
            if (!config.pcie_timing_configured) {
                config.pcie_timing_configured = true;
                /* 1.11.47 (FIX-PRE-FLEET L176): a declared link enables link
                 * pricing BY DEFAULT, but an explicit power.pcie.enabled=false
                 * is the user's call and is no longer silently overridden. */
                if (!config.pcie_enabled_user_set) config.pcie_enabled = true;
                else if (!config.pcie_enabled)
                    std::cout << "  [xing] power.pcie.enabled=false: the "
                                 "declared link keeps its TIMING; link ENERGY "
                                 "pricing is disabled by user choice."
                              << std::endl;
                config.pcie_base_latency_ns = lnk.base_latency_ns;
                config.pcie_bandwidth_GBs = lnk.bandwidth_GBs;
                config.pcie_header_bytes = lnk.header_bytes;
                config.pcie_coherence_extra_ns = lnk.coherence_extra_ns;
            }
            break;
        }
    }

    // -- Hierarchy (for device-internal DRAM hierarchy) --
    if (config.hierarchy_enabled) {
        // Pass the DEVICE node's clock so the device memory-bandwidth contention
        // (M/D/1 service rate + bandwidth floor) is clocked at the device freq,
        // NOT sys.frequency (= max = host) -- the host-clock invariance fix.
        emitZSimHierarchyBlock(cfg, config, sys_device_bw_freq_mhz, ref_freq);
    } else {
        // No DRAM hierarchy block, but still emit the host<->device offload-link
        // pcie* keys so getPCIeLatency charges the chosen link_type.
        emitZSimPcieBlock(cfg, config, ref_freq);
    }

    // -- Host<->device TWO-LAYER BRIDGE (1.7.1) --
    // Emitted whenever a device node with a memory tech is present (the co-sim
    // path). Supersedes the flat pcie charge at WORK_BEGIN/END; the plugin
    // falls back to getPCIeLatency only when no sys.bridge block is present
    // (legacy configs). Defaults derive from the device tech; user overrides
    // (system.bridge.*) validated here -- illegal combos are config errors.
    {
        const UnifiedConfig::SystemNode* dev_node = nullptr;
        for (const auto& node : config.system_nodes) {
            if (node.role != UnifiedConfig::SystemNode::DEVICE) continue;
            if (node.device_type == UnifiedConfig::SystemNode::COMPUTE) { dev_node = &node; break; }
            if (!dev_node) dev_node = &node;  // fall back to first (memory-only) device
        }
        if (dev_node) {
            BridgeDefaults bd = resolveBridge(config, dev_node->memory_tech);
            config.bridge_present = true;
            emitZSimBridgeBlock(cfg, bd, ref_freq);

            // -- Case-1 COHERENCE flush accounting (1.7.2) --
            // Charged on the host core at roi_begin in co-sim. Writeback BW
            // defaults to the HOST memory aggregate bandwidth (per-channel x
            // channels); user override via system.coherence.writeback_bw_gbs.
            const UnifiedConfig::SystemNode* host_node2 = nullptr;
            for (const auto& node : config.system_nodes) {
                if (node.role == UnifiedConfig::SystemNode::HOST) { host_node2 = &node; break; }
            }
            double wb_bw_gbs = config.coherence_writeback_bw_gbs;
            if (wb_bw_gbs <= 0.0 && host_node2) {
                HostMemBW hbw = getHostMemBandwidth(host_node2->memory_tech,
                                                    config.num_banks, config.cache_line_size,
                                                    host_node2->tech_node_nm,
                                                    config.temperature_k,
                                                    ref_freq);   // 1.11.56 (B004)
                if (host_node2->mem_bandwidth_mbs > 0) hbw.per_channel_mbs = host_node2->mem_bandwidth_mbs;
                if (host_node2->mem_channels > 0)      hbw.channels = host_node2->mem_channels;
                wb_bw_gbs = ((double)hbw.per_channel_mbs * (double)hbw.channels) / 1000.0;  // MB/s -> GB/s
            }
            emitZSimCoherenceBlock(cfg, config, wb_bw_gbs, ref_freq);

            // -- Kernel LAUNCH cost tree (1.7.3, HANDOFF ISSUE 2) --
            // Charged on the host core at the offload doorbell in co-sim. Bridge
            // crossings for the cmd/ack packets are priced by the same `bd`
            // resolved above, so DDR5 vs HBM3 differ only in the bridge component.
            emitZSimLaunchBlock(cfg, config, bd, ref_freq);
        }
    }

    // -- Node Map --
    cfg << "\n    nodeMap = {\n";
    cfg << "        numNodes = " << config.system_nodes.size() << ";\n";
    for (size_t i = 0; i < config.system_nodes.size(); i++) {
        const auto& n = config.system_nodes[i];
        cfg << "        node" << i << " = {\n";
        cfg << "            name = \"" << n.name << "\";\n";
        cfg << "            role = " << (n.role == UnifiedConfig::SystemNode::HOST ? 0 : 1) << ";\n";
        cfg << "            coreStart = " << n.core_start_idx << ";\n";
        cfg << "            coreEnd = " << n.core_end_idx << ";\n";
        cfg << "            freqMHz = " << static_cast<int>(n.frequency_mhz) << ";\n";
        cfg << "            networkNodeId = " << n.node_network_id << ";\n";
        cfg << "            addrStart = " << n.addr_start << ";\n";
        cfg << "            addrEnd = " << n.addr_end << ";\n";
        cfg << "        };\n";
    }
    cfg << "    };\n";

    // -- System Network --
    cfg << "\n    systemNetwork = {\n";
    cfg << "        enabled = 1;\n";
    cfg << "        numNodes = " << config.system_nodes.size() << ";\n";
    cfg << "        topology = \"" << config.system_network.topology << "\";\n";
    int model_id = 0;  // 0=SIMPLE, 2=DETAILED ("md1" mapped to "simple" during YAML parse)
    if (config.system_network.model == "detailed") model_id = 2;
    cfg << "        model = " << model_id << ";\n";
    cfg << "        linkWidthBits = " << config.system_network.link_width_bits << ";\n";
    cfg << "        frequencyGHz = \"" << std::fixed << std::setprecision(2)
        << config.system_network.frequency_ghz << "\";\n" << std::defaultfloat;
    cfg << "        latencyCycles = " << config.system_network.latency_cycles << ";\n";
    cfg << "        routerLatency = " << config.system_network.router_latency << ";\n";
    cfg << "        virtualChannelsPerVn = " << config.system_network.virtual_channels_per_vn << ";\n";
    cfg << "        inputBufferDepth = " << config.system_network.input_buffer_depth << ";\n";
    cfg << "        outputBufferDepth = " << config.system_network.output_buffer_depth << ";\n";

    // Pre-compute inter-node latencies in reference cycles
    for (size_t i = 0; i < config.system_nodes.size(); i++) {
        for (size_t j = 0; j < config.system_nodes.size(); j++) {
            if (i == j) continue;
            uint32_t lat = computeSystemNetLatency(config, (int)i, (int)j);

            // Check for per-link override
            for (const auto& lnk : config.system_network.links) {
                bool match = (lnk.src_name == config.system_nodes[i].name &&
                              lnk.dst_name == config.system_nodes[j].name) ||
                             (lnk.src_name == config.system_nodes[j].name &&
                              lnk.dst_name == config.system_nodes[i].name);
                if (match) {
                    // Override: base_latency_ns -> cycles + bandwidth-based serialization
                    double base_cycles = lnk.base_latency_ns * ref_freq / 1000.0;
                    double serial_cycles = (64.0 / lnk.bandwidth_GBs) * (ref_freq * 1e6 / 1e9);
                    lat = static_cast<uint32_t>(std::ceil(base_cycles + serial_cycles));
                    break;
                }
            }

            cfg << "        linkLatency_" << i << "_" << j << " = " << lat << ";\n";
        }
    }
    cfg << "    };\n";

    // -- Host Network (analytic crossbar fabric for multi-core hosts) --
    // ISSUE-5 host-side default topology = crossbar (uniform one-hop; contention
    // at ports, not hops). A 1-core host has NO fabric (a crossbar degenerates at
    // a single core: core->caches->MC direct) -- emit enabled=0. For >1 core we
    // add a fixed one-hop crossbar latency (hopCycles, core-clock) on the host
    // memory path; port CONTENTION is already priced by the host MC M/D/1 (PART
    // A), so this stays analytic -- no Garnet instance (the detailed host tier is
    // a later 1.7.x increment).
    {
        const UnifiedConfig::SystemNode* host_node = nullptr;
        for (const auto& n : config.system_nodes)
            if (n.role == UnifiedConfig::SystemNode::HOST) { host_node = &n; break; }
        bool host_fabric = host_node && host_node->num_cores > 1;
        cfg << "\n    hostNetwork = {\n";
        cfg << "        enabled = " << (host_fabric ? 1 : 0) << ";\n";
        if (host_node) {
            cfg << "        topology = \"" << host_node->host_noc_topology << "\";\n";
            cfg << "        model = \"" << host_node->host_noc_model << "\";\n";
            cfg << "        hopCycles = "
                << (host_fabric ? host_node->host_noc_hop_cycles : 0) << ";\n";
        }
        cfg << "    };\n";
    }

    cfg << "};\n\n";

    // -- Simulation parameters --
    // Simulator thread count (sim_parallel knob), same contract as the
    // device-scope generator:
    // MPI REQUIRES parallelism=1: the ranks are run by zsim's deterministic
    // round-robin core rotation (they park/wake on transport waits). With
    // parallelism>1 the rotation never engages and only rank 0 ever executes
    // -- which silently made every system-scope MPI run single-rank.
    // OMP + parallel sizes to the cores actually simulated: the HOST cores
    // for a no-offload baseline (the workload runs there), the device PEs
    // otherwise. Results are exact at any thread count (work-locked phases).
    int omp_sim_cores = ((getenv("PIMID_COSIM_NO_OFFLOAD") != nullptr) &&
                         config.host_num_cores > 0)
                        ? config.host_num_cores : config.num_pes;
    int parallelism = (config.workload_type == "openmp" && config.sim_parallel)
                      ? omp_sim_cores : 1;
    cfg << "sim = {\n";
    cfg << "    phaseLength = " << config.phase_length << ";\n";
    cfg << "    maxTotalInstrs = " << config.max_instructions << "L;\n";
    cfg << "    statsPhaseInterval = " << config.stats_interval << ";\n";
    cfg << "    parallelism = " << parallelism << ";\n";
    cfg << "    printHierarchy = true;\n";
    cfg << "    aslr = false;\n";
    cfg << "    strictConfig = false;\n";
    cfg << "};\n\n";

    // Emit per-process blocks (one per unique binary in multi-QEMU, or single process0)
    if (needsMultiQemu(config)) {
        auto groups = buildProcessGroups(config);
        for (size_t i = 0; i < groups.size(); i++) {
            cfg << "process" << i << " = {\n";
            cfg << "    command = \"<qemu-managed>\";\n";
            cfg << "    mask = \"" << groups[i].core_start << ":" << groups[i].core_end << "\";\n";
            cfg << "};\n";
        }
    } else {
        cfg << "process0 = {\n";
        cfg << "    command = \"<qemu-managed>\";\n";
        cfg << "};\n";
    }

    cfg.close();
    return cfg_path;
}

/* 1.11.57 (latent B037): SimConfigParser, ComprehensiveSimResult and
 * ComprehensiveSimulator -- 644 lines -- were DELETED here.
 *
 * They were unreachable: a whole-tree grep found no construction of either
 * class outside their own definitions, and main() dispatches on a method
 * validated to exec | trace | trace-gen | synthetic and has never built them.
 * What they contained is why this is a deletion and not a TODO. The class
 * printed banners reading "CACTI (SRAM/cache modeling)" and "[Power] McPAT
 * power model initialized", then produced its results from written-down
 * numbers under the heading "COMPREHENSIVE SIMULATION RESULTS": 50 mW per
 * core times the core count for PE power, 20 mW per bank for memory, an
 * average hop count of 2.5, a hit rate floored at 0.7, a remote access priced
 * at the read latency plus 50, and a CPU memory time of 50x the modelled one.
 * That is fabricated output wearing two tools' names, which is the one thing
 * this project refuses outright. Leaving it in place for someone to wire up
 * later is not a neutral act -- the wiring is one line, and the banners would
 * have made the numbers look sourced. */



//=============================================================================
// Main Entry Point
//=============================================================================

void printUsage(const char* program_name) {
    std::cout << "PIMID - Unified Processing-In-Memory Simulator" << std::endl;
    std::cout << "\nUsage: " << program_name << " --method <method> [options]" << std::endl;
    std::cout << "\nSimulation Methods:" << std::endl;
    std::cout << "  exec        Execution-driven cycle-accurate simulation (default)" << std::endl;
    std::cout << "  trace       Trace-based simulation (replay trace through models)" << std::endl;
    std::cout << "  trace-gen   Trace generation (record workload, no simulation)" << std::endl;
    std::cout << "\nCommon Options:" << std::endl;
    std::cout << "  --method METHOD      Simulation method (default: exec)" << std::endl;
    std::cout << "  --config FILE        Configuration file (YAML)" << std::endl;
    std::cout << "  --workload BINARY    Workload binary to simulate" << std::endl;
    std::cout << "  --help, -h           Show this help message" << std::endl;
    std::cout << "  --version, -v        Show version information" << std::endl;
    std::cout << "\nTrace Options:" << std::endl;
    std::cout << "  --trace-file FILE    Trace file path:" << std::endl;
    std::cout << "                       trace-gen mode: Output path (required)" << std::endl;
    std::cout << "                       trace mode:     Input path (required)" << std::endl;
    std::cout << "\nMPI Options:" << std::endl;
    std::cout << "  --mpi-ranks N        Number of MPI ranks (implies --workload-type mpi)" << std::endl;
    std::cout << "                       PIMID forks N QEMU children with libpimid_mpi.so" << std::endl;
    std::cout << "                       preloaded; ranks talk over a shm-mailbox transport." << std::endl;
    std::cout << "  --workload-type TYPE Workload type: serial (default), openmp, mpi" << std::endl;
    std::cout << "\nPower Analysis Options:" << std::endl;
    std::cout << "  --power              Enable McPAT power analysis (default)" << std::endl;
    std::cout << "  --no-power           Disable power analysis" << std::endl;
    std::cout << "  --power-report LEVEL Report detail: summary, standard (default), verbose" << std::endl;
    std::cout << "\nCharacterization Cache Options:" << std::endl;
    std::cout << "  --cache MODE         Warehouse mode: rw (default), ro, wo, off" << std::endl;
    std::cout << "  --no-cache           Disable the characterization cache (alias for --cache off)" << std::endl;
    std::cout << "  --cache-dir PATH     Warehouse root directory for cached characterizations" << std::endl;
    std::cout << "\nSystem Scope Options:" << std::endl;
    std::cout << "  --scope SCOPE        Simulation scope: device (default), system" << std::endl;
    std::cout << "                       (cosim accepted as deprecated alias for system)" << std::endl;
    std::cout << "\nExternal Models (used by exec and trace modes):" << std::endl;
    std::cout << "  - Ramulator2: DRAM timing simulation" << std::endl;
    std::cout << "  - CACTI:      SRAM/cache modeling" << std::endl;
    std::cout << "  - NVSim:      NVM modeling (STT-MRAM, PCM, ReRAM)" << std::endl;
    std::cout << "  - McPAT:      Power modeling" << std::endl;
    std::cout << "  - GARNET:     Network-on-chip simulation" << std::endl;
    std::cout << "\nExamples:" << std::endl;
    std::cout << "  " << program_name << " --method exec --config cfg.yaml --workload ./benchmark" << std::endl;
    std::cout << "  " << program_name << " --method trace --config cfg.yaml --trace-file recorded.pimtrace" << std::endl;
    std::cout << "  " << program_name << " --method trace-gen --workload ./benchmark --trace-file output.pimtrace" << std::endl;
    std::cout << "  " << program_name << " --method exec --config cfg.yaml --workload ./mpi_app --mpi-ranks 4" << std::endl;
    std::cout << "\nQEMU Prerequisites:" << std::endl;
    std::cout << "  The exec and trace-gen methods require the qemu-user package (dynamic build)." << std::endl;
    std::cout << "  Install with: sudo apt install qemu-user" << std::endl;
    std::cout << "  Note: qemu-user-static does NOT support -plugin and will not work." << std::endl;
    std::cout << "\nFor more information: https://github.com/isaacyhe/pimid" << std::endl;
}

void printVersion() {
    std::cout << "PIMID - Processing-In-Memory Infrastructure for Design-space exploration" << std::endl;
    std::cout << "Version " << PIMID_VERSION << std::endl;
    std::cout << std::endl;
    std::cout << "Integrated External Models:" << std::endl;
#ifdef HAVE_RAMULATOR
    std::cout << "  [OK] Ramulator2 (DRAM timing simulation)" << std::endl;
#else
    std::cout << "  o Ramulator2 (not linked)" << std::endl;
#endif
    std::cout << "  [OK] CACTI (SRAM/cache modeling)" << std::endl;
    std::cout << "  [OK] NVSim (NVM modeling)" << std::endl;
    std::cout << "  [OK] McPAT (power modeling)" << std::endl;
    std::cout << std::endl;
    std::cout << "Build date: " << __DATE__ << " " << __TIME__ << std::endl;
}

int main(int argc, char** argv) {
    UnifiedConfig config;
    std::string config_file;
    bool parsing_workload_args = false;
    // 1.11.56 (audit B057): --print-mem-info is answered after the config load.
    std::string print_mem_info_tech;
    double      print_mem_info_freq = 2000.0;

    // Characterization-cache warehouse: CLI-provided mode/dir (empty = unset).
    // Precedence (CLI > env > YAML > default) is resolved inside cache::configure().
    std::string cli_cache_mode;
    std::string cli_cache_dir;
    // YAML-provided warehouse mode/dir (top-level `cache:` block; empty = unset).
    std::string yaml_cache_mode;
    std::string yaml_cache_dir;

    // Defaults: exec method, device scope
    config.method = "exec";
    config.scope = "device";

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (parsing_workload_args) {
            // Stop collecting workload args if we hit a known PIMID flag
            if (arg == "--mpi-ranks" || arg == "--mpich-path" ||
                arg == "--workload-type" || arg == "--config" ||
                arg == "--method" || arg == "--scope" ||
                arg == "--trace-file" || arg == "--power" || arg == "--no-power" ||
                arg == "--power-report" || arg == "--cache" || arg == "--no-cache" ||
                arg == "--cache-dir" || arg == "--help" || arg == "-h") {
                parsing_workload_args = false;
                // Fall through to normal flag parsing below
            } else {
                config.workload_args.push_back(arg);
                continue;
            }
        }

        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--version" || arg == "-v") {
            printVersion();
            return 0;
        } else if (arg == "--print-mem-info" && i + 1 < argc) {
            /* Composer helper: print the simulator's own per-tech memory
             * parameters (access latency at the given clock + rank bandwidth)
             * so the composed co-sim driver shares ONE source of truth with
             * the simulator instead of a hand-copied table.
             *
             * 1.11.56 (audit B057): DEFER it. This used to answer and return
             * 0 from inside the argv loop, before any YAML was read, so
             * config.tech_node_nm was still its constructor default of 22 --
             * every config at another node got a printed access latency for
             * 22 nm silicon, under its own name, and the composed driver
             * copied that number. Recorded here, answered after the config
             * load, with the technology canonicalised like every other
             * technology string. */
            print_mem_info_tech = argv[++i];
            print_mem_info_freq = (i + 1 < argc) ? std::atof(argv[i + 1]) : 2000.0;
            if (print_mem_info_freq <= 0.0) print_mem_info_freq = 2000.0; else ++i;
        } else if (arg == "--method" && i + 1 < argc) {
            config.method = argv[++i];
        } else if (arg == "--scope" && i + 1 < argc) {
            config.scope = argv[++i];
            if (config.scope == "cosim") {
                config.scope = "system";
                config.cosim_remapped = true;
            }
        } else if (arg == "--mode" && i + 1 < argc) {
            // Legacy support: map old modes to new method/scope
            std::string mode = argv[++i];
            if (mode == "sim" || mode == "standalone") {
                config.method = "exec";
                config.scope = "device";
            } else if (mode == "cosim") {
                config.method = "exec";
                config.scope = "system";
                config.cosim_remapped = true;
            } else {
                std::cerr << "Warning: Unknown legacy mode '" << mode << "', using defaults" << std::endl;
            }
        } else if (arg == "--config" && i + 1 < argc) {
            config_file = argv[++i];
        } else if (arg == "--workload" && i + 1 < argc) {
            config.workload_binary = argv[++i];
            parsing_workload_args = true;
        } else if (arg == "--mpi-ranks" && i + 1 < argc) {
            config.mpi_ranks = std::stoi(argv[++i]);
            config.workload_type = "mpi";
        } else if (arg == "--mpich-path" && i + 1 < argc) {
            std::cerr << "Warning: --mpich-path is deprecated and ignored "
                         "(PIMID no longer uses mpirun for MPI launch)" << std::endl;
            ++i;
        } else if (arg == "--workload-type" && i + 1 < argc) {
            config.workload_type = argv[++i];
        } else if (arg == "--trace-file" && i + 1 < argc) {
            config.trace_file = argv[++i];
        } else if (arg == "--power") {
            config.enable_power = true;
        } else if (arg == "--no-power") {
            config.enable_power = false;
        } else if (arg == "--power-report" && i + 1 < argc) {
            config.power_report_detail = argv[++i];
            if (config.power_report_detail != "summary" &&
                config.power_report_detail != "standard" &&
                config.power_report_detail != "verbose") {
                std::cerr << "Invalid --power-report value: " << config.power_report_detail
                          << " (expected: summary, standard, verbose)" << std::endl;
                return 1;
            }
        } else if (arg == "--cache" && i + 1 < argc) {
            // Characterization-cache warehouse mode: rw|ro|wo|off
            cli_cache_mode = argv[++i];
        } else if (arg == "--no-cache") {
            // Alias for --cache off
            cli_cache_mode = "off";
        } else if (arg == "--cache-dir" && i + 1 < argc) {
            cli_cache_dir = argv[++i];
        } else if (arg[0] != '-' && config_file.empty()) {
            // Positional argument - treat as config file
            config_file = arg;
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }

    // Save CLI-specified values before YAML loading (CLI overrides YAML)
    // We track which fields were explicitly set on the command line
    bool cli_method_set = false;
    bool cli_workload_set = false;
    bool cli_workload_type_set = false;
    bool cli_mpi_ranks_set = false;

    // Re-scan argv to detect which flags were explicitly given on CLI
    bool cli_power_set = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--method") cli_method_set = true;
        else if (a == "--workload") cli_workload_set = true;
        else if (a == "--workload-type") cli_workload_type_set = true;
        else if (a == "--mpi-ranks") { cli_mpi_ranks_set = true; cli_workload_type_set = true; }
        else if (a == "--power" || a == "--no-power") cli_power_set = true;
    }

    // Load YAML config early so it can provide method, workload, etc.
    if (!config_file.empty()) {
        try {
            YAML::Node yaml_cfg = YAML::LoadFile(config_file);

            // Top-level simulation settings (can be overridden by CLI)
            if (!cli_method_set && yaml_cfg["method"]) {
                config.method = yaml_cfg["method"].as<std::string>();
            }

            // Scope: device or system (cosim accepted as deprecated alias -> system)
            if (yaml_cfg["scope"]) {
                config.scope = yaml_cfg["scope"].as<std::string>();
                if (config.scope == "cosim") {
                    config.scope = "system";
                    config.cosim_remapped = true;
                }
            }

            // Workload section
            if (yaml_cfg["workload"]) {
                if (!cli_workload_set && yaml_cfg["workload"]["binary"]) {
                    config.workload_binary = yaml_cfg["workload"]["binary"].as<std::string>();
                }
                if (!cli_workload_type_set && yaml_cfg["workload"]["type"]) {
                    config.workload_type = yaml_cfg["workload"]["type"].as<std::string>();
                }
                if (!cli_mpi_ranks_set && yaml_cfg["workload"]["mpi_ranks"]) {
                    config.mpi_ranks = yaml_cfg["workload"]["mpi_ranks"].as<int>();
                    /* 1.11.63 (R6-6): THE YAML KEY IMPLIES THE TYPE, EXACTLY AS
                     * THE CLI FLAG DOES.
                     *
                     * --mpi-ranks N sets workload_type = "mpi" on the spot (see
                     * the argv loop above) and the usage text promises it does.
                     * workload.mpi_ranks did not: every downstream consumer is
                     * gated on `workload_type == "mpi" && mpi_ranks > 0`, so a
                     * YAML that set the ranks and not the type ran SINGLE-RANK
                     * in silence -- no warning, no line in the report, and a
                     * plausible-looking rank count sitting in the config that
                     * nothing read. Gate 1172B hit exactly this and spent a
                     * full arm proving the simulator right and the gate wrong.
                     *
                     * CLI-CONSISTENT MEANS IMPLYING, NOT REFUSING. Refusing the
                     * orphan key would leave the two paths disagreeing about
                     * what the same quantity means -- one implies, one rejects
                     * -- and would break every existing YAML that pairs the key
                     * with its sibling correctly. Implying makes the two paths
                     * one rule: naming a rank count asks for MPI.
                     *
                     * An EXPLICIT non-mpi type still wins and is warned about,
                     * because that is a genuine contradiction in one file and
                     * silently overriding the user's own words would be a
                     * different footgun. Note that workload.type was read a few
                     * lines above, so config.workload_type here is already the
                     * file's own value if it set one -- and a --workload-type
                     * on the command line has already won over that. The test
                     * is therefore "did anyone STATE a type", not "is the type
                     * still the constructor default". */
                    const bool type_stated =
                        cli_workload_type_set ||
                        static_cast<bool>(yaml_cfg["workload"]["type"]);
                    if (config.mpi_ranks > 0) {
                        if (!type_stated) {
                            config.workload_type = "mpi";
                            std::cout << "[config] NOTE: workload.mpi_ranks = "
                                      << config.mpi_ranks
                                      << " implies workload.type: mpi (the same "
                                         "implication --mpi-ranks carries on the "
                                         "command line). Without it the key is "
                                         "inert and the run would be single-rank."
                                      << std::endl;
                        } else if (config.workload_type != "mpi") {
                            std::cerr << "[config] WARNING: workload.mpi_ranks = "
                                      << config.mpi_ranks
                                      << " is set, but workload.type is \""
                                      << config.workload_type
                                      << "\", not \"mpi\". The explicit type wins, "
                                         "so the rank count is IGNORED and this run "
                                         "is not multi-rank. Set workload.type: mpi "
                                         "or remove workload.mpi_ranks." << std::endl;
                        }
                    }
                }
                if (yaml_cfg["workload"]["mpich_path"]) {
                    std::cerr << "Warning: workload.mpich_path in YAML is deprecated "
                                 "and ignored (PIMID no longer uses mpirun)" << std::endl;
                }
                // YAML args (CLI --workload args override entirely)
                if (!cli_workload_set && yaml_cfg["workload"]["args"]) {
                    for (const auto& a : yaml_cfg["workload"]["args"]) {
                        config.workload_args.push_back(a.as<std::string>());
                    }
                }
                // Environment variables
                if (yaml_cfg["workload"]["env"]) {
                    for (const auto& e : yaml_cfg["workload"]["env"]) {
                        config.workload_env[e.first.as<std::string>()] = e.second.as<std::string>();
                    }
                }
            }

            // Load simulation metadata
            if (yaml_cfg["name"]) {
                config.name = yaml_cfg["name"].as<std::string>();
            }
            if (yaml_cfg["description"]) {
                config.description = yaml_cfg["description"].as<std::string>();
            }

            // Load PE configuration
            if (yaml_cfg["pim"]) {
                if (yaml_cfg["pim"]["pe"]) {
                    config.num_pes = yaml_cfg["pim"]["pe"]["count"].as<int>(config.num_pes);
                    if (yaml_cfg["pim"]["pe"]["core_type"])
                        config.pe_type = yaml_cfg["pim"]["pe"]["core_type"].as<std::string>();
                    else
                        config.pe_type = yaml_cfg["pim"]["pe"]["type"].as<std::string>(config.pe_type);
                    config.pg_pe = yaml_cfg["pim"]["pe"]["pg"].as<bool>(config.pg_pe);  // 1.11.8
                    /* 1.11.56 (audit B046): the SAME normaliser the system-node
                     * path uses. This block used to open-code its own alias list
                     * and its own whitelist, and the two had already drifted --
                     * "compute_unit_pe" was a valid element name in system scope
                     * and a fatal config error in device scope. The old comment
                     * said both must be updated together, which is exactly the
                     * job of one function. normalizeCoreTypeName() rejects
                     * unknown names itself, with the same message. */
                    config.pe_type = normalizeCoreTypeName(config.pe_type);
                    // PE frequency (alternative to system.frequency_mhz)
                    if (yaml_cfg["pim"]["pe"]["frequency_mhz"])
                        config.frequency_mhz = yaml_cfg["pim"]["pe"]["frequency_mhz"].as<int>();

                    /* 1.9.32: the compute unit's datapath. Every configuration
                     * written before this release omits all four and gets the
                     * documented defaults, so no existing config changes
                     * meaning. */
                    config.pe_lanes = yaml_cfg["pim"]["pe"]["lanes"].as<int>(config.pe_lanes);
                    config.pe_has_fp = yaml_cfg["pim"]["pe"]["floating_point"].as<bool>(config.pe_has_fp);
                    config.pe_fp_emul_cycles = yaml_cfg["pim"]["pe"]["fp_emulation_cycles"]
                                                   .as<uint32_t>(config.pe_fp_emul_cycles);                     /* 1.11.47 (L203): null_core has NO timing model, so a
                     * soft-float timing charge cannot mean anything there.
                     * Refused rather than silently accepted-and-ignored. */
                    if (!config.pe_has_fp && config.pe_type == "null_core") {
                        std::cerr << "ERROR: pim.pe.floating_point=false with "
                                     "pe.type=null_core -- the null core has no "
                                     "timing model to charge FP emulation on. "
                                     "Use a timing core type." << std::endl;
                        return 1;
                    }
 // 1.11.11
                    if (yaml_cfg["pim"]["pe"]["element_bits"]) {
                        std::cerr << "Error: pim.pe.element_bits was withdrawn. The "
                                  << "element's datapath width is pim.pe.operand_width, "
                                  << "which the timing model already reads; a second "
                                  << "name for it let the two halves disagree.\n";
                        return 1;
                    }
                    config.pe_imem_bytes =
                        yaml_cfg["pim"]["pe"]["imem_bytes"].as<int>(config.pe_imem_bytes);
                    if (config.pe_lanes < 1) {
                        std::cerr << "Error: pim.pe.lanes must be at least 1 (got "
                                  << config.pe_lanes << ").\n";
                        return 1;
                    }
                    // ALU scaling factors
                    config.alu_compute_factor = yaml_cfg["pim"]["pe"]["compute_factor"].as<double>(config.alu_compute_factor);
                    config.alu_access_factor = yaml_cfg["pim"]["pe"]["access_factor"].as<double>(config.alu_access_factor);
                    config.alu_throughput_factor = yaml_cfg["pim"]["pe"]["throughput_factor"].as<double>(config.alu_throughput_factor);
                    config.alu_operand_width = yaml_cfg["pim"]["pe"]["operand_width"].as<int>(config.alu_operand_width);
                    /* 1.9.40: validated now that it reaches the power model too.
                     * It was previously read only by the bit-serial cycle
                     * charge, which clamps with max(w,1), so a zero was merely
                     * inert; it now sizes register files and buses, where a
                     * zero-bit array aborts the array model outright. */
                    if (config.alu_operand_width < 1) {
                        std::cerr << "Error: pim.pe.operand_width must be at least 1 (got "
                                  << config.alu_operand_width << ").\n";
                        return 1;
                    }
                    config.alu_bit_serial = yaml_cfg["pim"]["pe"]["bit_serial"].as<bool>(config.alu_bit_serial);
                    config.alu_energy_factor = yaml_cfg["pim"]["pe"]["energy_factor"].as<double>(config.alu_energy_factor);
                    // In-order PE issue width (in_order_core only; default 2)
                    config.inorder_issue_width = yaml_cfg["pim"]["pe"]["issue_width"].as<int>(config.inorder_issue_width);

                    /* 1.9.40: the two halves must agree about what the element
                     * is. Both checks below are placed AFTER the scaling factors
                     * are parsed, because each compares a power-model
                     * description against a timing-model one. */

                    /* An element described as wide in power but scalar in
                     * timing. lanes replicates the arithmetic, the register file
                     * and the result bus in the power model; the timing model
                     * expresses the same width through throughput_factor, which
                     * divides the per-instruction cost. Declaring lanes without
                     * throughput_factor gives an element that PAYS for W lanes
                     * and RUNS like one. That is a legitimate thing to model
                     * deliberately (a wide datapath the code fails to use), so
                     * this warns rather than refuses -- but silently is exactly
                     * how the previous defects happened. */
                    if (config.pe_lanes > 1 && config.alu_throughput_factor <= 1.0) {
                        std::cerr << "[config] WARNING: pim.pe.lanes=" << config.pe_lanes
                                  << " widens the POWER description (one arithmetic unit "
                                  << "and register file per lane) but throughput_factor is "
                                  << config.alu_throughput_factor << ", so the TIMING model "
                                  << "still charges a scalar element. The result is an "
                                  << "element that pays for " << config.pe_lanes
                                  << " lanes and runs like one. Set throughput_factor to "
                                  << "match unless the serialisation is intended.\n";
                    }

                    /* An element described without floating point, running
                     * floating-point kernels. Removing the FPU from the power
                     * description does NOT make the timing model emulate
                     * floating point in software, which is what a real part
                     * lacking an FPU would have to do -- tens to hundreds of
                     * operations per operation. So this is honest for an
                     * integer kernel and not honest for a floating-point one,
                     * and the model cannot tell which it is about to run. */
                    if (!config.pe_has_fp) {
                        std::cerr << "[config] pim.pe.floating_point=false: the element "
                                  << "has no FP unit. Since 1.11.10 the timing model CAN "
                                  << "see FP-class instructions, so the run will report "
                                  << "how many executed";
                        if (config.pe_fp_emul_cycles > 0) {
                            std::cerr << " and charge " << config.pe_fp_emul_cycles
                                      << " cycles each as soft-float emulation.\n";
                        } else {
                            std::cerr << ", but charge nothing for them "
                                      << "(pim.pe.fp_emulation_cycles=0). Honest for a "
                                      << "kernel with no FP; for one that has FP, set the "
                                      << "knob or the element runs code it has no unit "
                                      << "for at full speed.\n";
                        }
                    }
                }
                // placement can live either at pim.placement (older flat form)
                // or pim.pe.placement (newer nested form, used by README and
                // YAML_REFERENCE examples). Accept both; nested overrides flat.
                auto placement_node = yaml_cfg["pim"]["placement"];
                if (yaml_cfg["pim"]["pe"] && yaml_cfg["pim"]["pe"]["placement"]) {
                    placement_node = yaml_cfg["pim"]["pe"]["placement"];
                }
                if (placement_node) {
                    config.placement_level = placement_node["level"].as<std::string>(config.placement_level);
                    // Connection mode: shared_io (default) or separate_endpoints
                    if (placement_node["connection"]) {
                        std::string conn = placement_node["connection"].as<std::string>();
                        if (conn == "separate_endpoints")
                            config.pe_mem_connection = UnifiedConfig::PEMemConnectionMode::SEPARATE_ENDPOINTS;
                        else
                            config.pe_mem_connection = UnifiedConfig::PEMemConnectionMode::SHARED_IO;
                    }
                    config.local_link_latency = placement_node["local_link_latency"].as<int>(config.local_link_latency);
                }
                // PE-to-memory-org mapping (M:N connectivity)
                if (yaml_cfg["pim"]["mapping"]) {
                    auto mapping = yaml_cfg["pim"]["mapping"];
                    std::string mode = mapping["mode"].as<std::string>("uniform");
                    if (mode == "uniform") {
                        int pes_per_mo = mapping["pes_per_mem_org"].as<int>(0);
                        int mos_per_pe = mapping["mem_orgs_per_pe"].as<int>(0);
                        // Defer actual map generation to autoGeneratePEMemMap()
                        // Store ratios temporarily in pe_mem_map as empty with sentinel
                        if (pes_per_mo > 0) {
                            // M:1 -- multiple PEs per mem org
                            // We'll build the map after we know total_mem_orgs
                            config.pe_mem_map.clear();
                            // Use negative pe_id as sentinel: -pes_per_mo
                            config.pe_mem_map.push_back({-pes_per_mo, {}});
                        } else if (mos_per_pe > 0) {
                            // 1:N -- one PE manages multiple mem orgs
                            config.pe_mem_map.clear();
                            config.pe_mem_map.push_back({-1, std::vector<int>(mos_per_pe, -1)});
                        }
                    } else if (mode == "explicit") {
                        config.pe_mem_map.clear();
                        if (mapping["map"]) {
                            for (const auto& entry : mapping["map"]) {
                                UnifiedConfig::PEMemMapping m;
                                m.pe_id = entry["pe"].as<int>();
                                if (entry["mem_orgs"].IsSequence()) {
                                    for (const auto& mo : entry["mem_orgs"])
                                        m.mem_org_ids.push_back(mo.as<int>());
                                }
                                config.pe_mem_map.push_back(m);
                            }
                        }
                    }
                }
                // PE-MI distributed memory interface config
                if (yaml_cfg["pim"]["mc"]) {
                    config.pg_mc = yaml_cfg["pim"]["mc"]["pg"].as<bool>(config.pg_mc);  // 1.11.8
                    config.pe_mc_enabled = true;
                    auto mc = yaml_cfg["pim"]["mc"];
                    config.pe_mc_type = mc["type"].as<std::string>(config.pe_mc_type);
                    /* 1.9.35: the key reads as a choice but there is exactly one
                     * implementation. The old SimplePEMemoryController and
                     * MD1PEMemoryController were merged, and M/D/1 queuing is
                     * always active, so any value here yields the same model.
                     * Say so rather than letting a config claim a mode it did
                     * not get. */
                    if (config.pe_mc_type != "simple" && !config.pe_mc_type.empty()) {
                        std::cout << "  [config] WARNING: pim.mc.type = '"
                                  << config.pe_mc_type << "' is accepted but has no "
                                     "separate implementation; the PE memory interface "
                                     "always uses the merged coverage-routing + "
                                     "hierarchy-traversal + M/D/1 model." << std::endl;
                    }
                    if (mc["pes_per_mc"]) {
                        config.pes_per_mc = mc["pes_per_mc"].as<int>();
                        config.pes_per_mc_user_set = true;
                    }
                    if (mc["local_latency"])
                        config.pe_mc_local_latency = mc["local_latency"].as<int>();
                    if (mc["bandwidth_mbs"])
                        config.pe_mc_bandwidth_mbs = mc["bandwidth_mbs"].as<int>();

                    // MC placement: with_core (default) or standalone NoC endpoint
                    if (mc["placement"]) {
                        std::string pl = mc["placement"].as<std::string>();
                        config.mc_standalone = (pl == "standalone");
                    }

                    // Per-group overrides
                    if (mc["groups"]) {
                        for (const auto& grp : mc["groups"]) {
                            UnifiedConfig::PEMCGroupOverride ov;
                            if (grp["id"].IsSequence()) {
                                for (const auto& id : grp["id"])
                                    ov.ids.push_back(id.as<int>());
                            } else if (grp["id"]) {
                                ov.ids.push_back(grp["id"].as<int>());
                            }
                            /* 1.11.57 (latent B032): the per-group `type` key
                             * was parsed into a field with no reader -- the
                             * per-group emitter writes bandwidthMBs and
                             * localLatency and nothing else -- so a config
                             * that gave one MC group a different controller
                             * got the same controller as every other group,
                             * silently. The GLOBAL pim.mc.type at least warns
                             * that it has no separate implementation (see the
                             * 1.9.35 note above); the per-group copy did not
                             * even do that. Same warning here, in the same
                             * terms, rather than a field that stores an
                             * answer nobody asks for. */
                            if (grp["type"]) {
                                std::string gt = grp["type"].as<std::string>();
                                if (gt != "simple" && !gt.empty())
                                    std::cout << "  [config] WARNING: "
                                                 "pim.mc.groups[].type = '" << gt
                                              << "' is accepted but has no "
                                                 "separate implementation, and "
                                                 "nothing in the emitted per-"
                                                 "group block carries it. Every "
                                                 "group uses the merged "
                                                 "coverage-routing + hierarchy-"
                                                 "traversal + M/D/1 model."
                                              << std::endl;
                            }
                            if (grp["local_latency"])
                                ov.local_latency = grp["local_latency"].as<int>();
                            if (grp["bandwidth_mbs"])
                                ov.bandwidth_mbs = grp["bandwidth_mbs"].as<int>();
                            config.pe_mc_group_overrides.push_back(ov);
                        }
                    }
                }
            }

            // Load system configuration
            if (yaml_cfg["system"]) {
                config.frequency_mhz = yaml_cfg["system"]["frequency_mhz"].as<int>(config.frequency_mhz);
                config.cache_line_size = yaml_cfg["system"]["cache_line_size"].as<int>(config.cache_line_size);
                config.tech_node_nm = yaml_cfg["system"]["tech_node_nm"].as<int>(config.tech_node_nm);
            }

            // Alternative technology section (technology.node_nm)
            if (yaml_cfg["technology"] && yaml_cfg["technology"]["node_nm"]) {
                config.tech_node_nm = yaml_cfg["technology"]["node_nm"].as<int>();
            }

            // Load cache configuration
            if (yaml_cfg["cache"]) {
                // Characterization-cache warehouse settings (mode/dir). These feed
                // cache::configure() after parsing; precedence is resolved there.
                if (yaml_cfg["cache"]["mode"]) {
                    yaml_cache_mode = yaml_cfg["cache"]["mode"].as<std::string>("");
                } else if (yaml_cfg["cache"]["enabled"] &&
                           !yaml_cfg["cache"]["enabled"].as<bool>(true)) {
                    // cache.enabled: false -> treat as mode "off" (only if mode unset)
                    yaml_cache_mode = "off";
                }
                if (yaml_cfg["cache"]["dir"]) {
                    yaml_cache_dir = yaml_cfg["cache"]["dir"].as<std::string>("");
                }
                if (yaml_cfg["cache"]["l1d"]) {
                    config.l1d_size_kb = yaml_cfg["cache"]["l1d"]["size_kb"].as<int>(config.l1d_size_kb);
                    config.l1d_ways = yaml_cfg["cache"]["l1d"]["ways"].as<int>(config.l1d_ways);
                }
                if (yaml_cfg["cache"]["l1i"]) {
                    config.l1i_size_kb = yaml_cfg["cache"]["l1i"]["size_kb"].as<int>(config.l1i_size_kb);
                    config.l1i_ways = yaml_cfg["cache"]["l1i"]["ways"].as<int>(config.l1i_ways);
                }
                if (yaml_cfg["cache"]["l2"]) {
                    config.enable_l2 = yaml_cfg["cache"]["l2"]["enabled"].as<bool>(config.enable_l2);
                    config.l2_size_kb = yaml_cfg["cache"]["l2"]["size_kb"].as<int>(config.l2_size_kb);
                    config.l2_ways = yaml_cfg["cache"]["l2"]["ways"].as<int>(config.l2_ways);
                    config.l2_count = yaml_cfg["cache"]["l2"]["count"].as<int>(config.l2_count);
                }
                if (yaml_cfg["cache"]["l3"]) {
                    config.enable_l3 = yaml_cfg["cache"]["l3"]["enabled"].as<bool>(config.enable_l3);
                    config.l3_size_kb = yaml_cfg["cache"]["l3"]["size_kb"].as<int>(config.l3_size_kb);
                    config.l3_ways = yaml_cfg["cache"]["l3"]["ways"].as<int>(config.l3_ways);
                }

                // Parse cache timing/energy/power overrides
                if (yaml_cfg["cache"]["l1d"]) {
                    auto n = yaml_cfg["cache"]["l1d"];
                    if (n["latency_ns"]) config.l1d_params.latency_ns = n["latency_ns"].as<double>();
                    if (n["energy_nj"])  config.l1d_params.energy_nj = n["energy_nj"].as<double>();
                    if (n["static_power_mw"]) config.l1d_params.static_power_mw = n["static_power_mw"].as<double>();
                }
                if (yaml_cfg["cache"]["l1i"]) {
                    auto n = yaml_cfg["cache"]["l1i"];
                    if (n["latency_ns"]) config.l1i_params.latency_ns = n["latency_ns"].as<double>();
                    if (n["energy_nj"])  config.l1i_params.energy_nj = n["energy_nj"].as<double>();
                    if (n["static_power_mw"]) config.l1i_params.static_power_mw = n["static_power_mw"].as<double>();
                }
                if (yaml_cfg["cache"]["l2"]) {
                    auto n = yaml_cfg["cache"]["l2"];
                    if (n["latency_ns"]) config.l2_params.latency_ns = n["latency_ns"].as<double>();
                    if (n["energy_nj"])  config.l2_params.energy_nj = n["energy_nj"].as<double>();
                    if (n["static_power_mw"]) config.l2_params.static_power_mw = n["static_power_mw"].as<double>();
                }
                if (yaml_cfg["cache"]["l3"]) {
                    auto n = yaml_cfg["cache"]["l3"];
                    if (n["latency_ns"]) config.l3_params.latency_ns = n["latency_ns"].as<double>();
                    if (n["energy_nj"])  config.l3_params.energy_nj = n["energy_nj"].as<double>();
                    if (n["static_power_mw"]) config.l3_params.static_power_mw = n["static_power_mw"].as<double>();
                }

                // Check if user provided complete overrides for all active cache levels
                config.use_yaml_cache_params =
                    config.l1d_params.isComplete() &&
                    config.l1i_params.isComplete() &&
                    (!config.enable_l2 || config.l2_params.isComplete()) &&
                    (!config.enable_l3 || config.l3_params.isComplete());

                if (!config.use_yaml_cache_params) {
                    // Check if any partial params were provided
                    bool any_provided =
                        config.l1d_params.latency_ns > 0 || config.l1d_params.energy_nj > 0 || config.l1d_params.static_power_mw > 0 ||
                        config.l1i_params.latency_ns > 0 || config.l1i_params.energy_nj > 0 || config.l1i_params.static_power_mw > 0 ||
                        config.l2_params.latency_ns > 0 || config.l2_params.energy_nj > 0 || config.l2_params.static_power_mw > 0 ||
                        config.l3_params.latency_ns > 0 || config.l3_params.energy_nj > 0 || config.l3_params.static_power_mw > 0;
                    if (any_provided) {
                        std::cerr << "Warning: Partial YAML cache params provided.\n"
                                  << "To override CACTI, provide ALL 3 params (latency_ns, energy_nj, static_power_mw)\n"
                                  << "for ALL active cache levels (l1d, l1i"
                                  << (config.enable_l2 ? ", l2" : "")
                                  << (config.enable_l3 ? ", l3" : "") << ").\n"
                                  << "Using CACTI-derived cache latencies instead.\n";
                        std::cerr.flush();
                    }
                }
            }

            // Load NoC configuration
            if (yaml_cfg["noc"]) {
                config.pg_noc = yaml_cfg["noc"]["pg"].as<bool>(config.pg_noc);  // 1.11.8
                if (yaml_cfg["noc"]["topology"]) config.noc_topology_user_set = true;
                config.noc_topology = yaml_cfg["noc"]["topology"].as<std::string>(config.noc_topology);
                std::transform(config.noc_topology.begin(), config.noc_topology.end(),
                               config.noc_topology.begin(), ::toupper);
                config.noc_router_latency = yaml_cfg["noc"]["router_latency"].as<int>(config.noc_router_latency);
                config.noc_link_latency = yaml_cfg["noc"]["link_latency"].as<int>(config.noc_link_latency);

                // Parse noc.model. The lineup is exactly TWO models -- no aliases:
                //   "analytical" -> closed-form hop-count + M/D/1 + MLP (see noc.mlp)
                //   "detailed"   -> cycle-accurate Garnet
                if (yaml_cfg["noc"]["model"]) {
                    std::string noc_model = yaml_cfg["noc"]["model"].as<std::string>();
                    if (noc_model == "detailed") {
                        config.noc_cycle_accurate = true;
                        config.noc_mlp_model = 0;
                    } else if (noc_model == "analytical") {
                        config.noc_cycle_accurate = false;
                        config.noc_mlp_model = 1;
                    } else {
                        std::cerr << "Error: unknown noc.model '" << noc_model
                                  << "'. Valid: analytical | detailed" << std::endl;
                        return 1;
                    }
                    // Propagate to per-level network models ("simple" is the
                    // INTERNAL name of the per-level analytical path).
                    std::string effective_model = noc_model;
                    if (effective_model == "analytical")
                        effective_model = "simple";
                    for (int i = 0; i < 7; ++i) {
                        config.network_level_model[i] = effective_model;
                    }
                }
                // noc.mlp: M, the PE outstanding-access window for the 'mlp' model.
                if (yaml_cfg["noc"]["mlp"]) {
                    config.noc_mlp_degree = yaml_cfg["noc"]["mlp"].as<int>(config.noc_mlp_degree);
                    if (config.noc_mlp_degree < 1) config.noc_mlp_degree = 1;
                }
                // (the legacy noc.cycle_accurate boolean key was removed --
                //  noc.model: analytical | detailed is the only selector)
                config.noc_routing = yaml_cfg["noc"]["routing"].as<std::string>(config.noc_routing);
                std::transform(config.noc_routing.begin(), config.noc_routing.end(),
                               config.noc_routing.begin(), ::toupper);
                if (yaml_cfg["noc"]["vcs_per_vnet"]) config.noc_vcs_user_set = true;
                config.noc_vcs_per_vnet = yaml_cfg["noc"]["vcs_per_vnet"].as<int>(config.noc_vcs_per_vnet);
                // Accept both virtual_channels_per_vn and vcs_per_vnet
                if (yaml_cfg["noc"]["virtual_channels_per_vn"]) {
                    config.noc_vcs_per_vnet = yaml_cfg["noc"]["virtual_channels_per_vn"].as<int>();
                    config.noc_vcs_user_set = true;
                }
                if (yaml_cfg["noc"]["buffers_per_vc"]) config.noc_buffers_user_set = true;
                config.noc_buffers_per_vc = yaml_cfg["noc"]["buffers_per_vc"].as<int>(config.noc_buffers_per_vc);
                /* 1.11.56 (audit B049): say what this key actually does.
                 *
                 * noc.clock_mhz reads as a NoC-fabric frequency override; it
                 * sets config.frequency_mhz, which is sys.frequency -- the PE
                 * clock, the divisor of every ns-to-cycle conversion in the
                 * device path, the clock McPAT prices the core at, and the
                 * device bandwidth clock. And because the noc: block is
                 * parsed AFTER system:, it silently overrode an explicit
                 * system.frequency_mhz. This tree has no separate NoC clock
                 * domain to set, so the key keeps its effect and stops being
                 * a misdescription: the run states that the device clock
                 * moved, and names the key it overrode. */
                if (yaml_cfg["noc"]["clock_mhz"]) {
                    int noc_clk = yaml_cfg["noc"]["clock_mhz"].as<int>();
                    std::cout << "[config] NOTE: noc.clock_mhz = " << noc_clk
                              << " sets the WHOLE device clock (sys.frequency), "
                                 "not a separate NoC domain -- this tree models "
                                 "one clock for the element, its caches and its "
                                 "fabric. It was " << config.frequency_mhz
                              << " MHz";
                    if (yaml_cfg["system"] && yaml_cfg["system"]["frequency_mhz"])
                        std::cout << ", set explicitly by system.frequency_mhz, "
                                     "which this key overrides";
                    std::cout << "." << std::endl;
                    config.frequency_mhz = noc_clk;
                }
                /* 1.11.57 (latent B036): the two lines that used to sit here
                 * said the key was "accepted (stored for synthetic)". It was
                 * never parsed -- there is no read of noc.flit_size_bits
                 * anywhere, only the Garnet STATS parser's identically named
                 * field -- so a user setting it got no effect and no warning,
                 * which is the shape of failure this pass exists to remove.
                 *
                 * Garnet's flit is fixed at 128 bits internally and this file
                 * cannot change that, so the key cannot be wired up here. It is
                 * accepted only at the value that is true (128), where it is a
                 * harmless restatement of the fabric, and refused otherwise
                 * rather than silently not applying. Three shipped test configs
                 * declare 128 and keep working. */
                if (yaml_cfg["noc"]["flit_size_bits"]) {
                    int fsb = yaml_cfg["noc"]["flit_size_bits"].as<int>();
                    if (fsb != 128) {
                        std::cerr << "Error: noc.flit_size_bits = " << fsb
                                  << " is NOT IMPLEMENTED. Garnet's flit width "
                                     "is fixed at 128 bits in this build, and "
                                     "the key has never been read, so the run "
                                     "would have used 128 without saying so. "
                                     "Remove the key or set it to 128."
                                  << std::endl;
                        return 1;
                    }
                }
                config.noc_topology_file = yaml_cfg["noc"]["topology_file"].as<std::string>(config.noc_topology_file);
                config.noc_routing_table_file = yaml_cfg["noc"]["routing_table_file"].as<std::string>(config.noc_routing_table_file);
                // Message sizes in bits (0 = use defaults: control=64, data=576)
                config.noc_control_msg_bits = yaml_cfg["noc"]["control_message_bits"].as<int>(config.noc_control_msg_bits);
                config.noc_data_msg_bits = yaml_cfg["noc"]["data_message_bits"].as<int>(config.noc_data_msg_bits);

                // Ring direction: "unidirectional"/"uni" or "bidirectional"/"bi" (default)
                if (yaml_cfg["noc"]["ring_direction"]) {
                    std::string dir = yaml_cfg["noc"]["ring_direction"].as<std::string>();
                    std::transform(dir.begin(), dir.end(), dir.begin(), ::tolower);
                    config.noc_ring_unidirectional = (dir == "unidirectional" || dir == "uni");
                }

                // Per-level overrides (under noc.levels)
                if (yaml_cfg["noc"]["levels"]) {
                    const auto& levels = yaml_cfg["noc"]["levels"];
                    const std::array<std::string, 7> level_keys = {
                        "subarray", "bank", "bank_group", "chip", "rank", "channel", "system"
                    };
                    for (int i = 0; i < 7; ++i) {
                        if (levels[level_keys[i]]) {
                            const auto& lv = levels[level_keys[i]];
                            if (lv["model"]) {
                                config.network_level_model[i] = lv["model"].as<std::string>();
                                config.network_level_model_user_set[i] = true;  // 1.11.56 (B058)
                            }
                            if (lv["link_width_bits"])
                                config.network_level_overrides[i].link_width_bits = lv["link_width_bits"].as<int>();
                            if (lv["frequency_ghz"])
                                config.network_level_overrides[i].frequency_ghz = lv["frequency_ghz"].as<double>();
                            if (lv["latency_cycles"])
                                config.network_level_overrides[i].latency_cycles = lv["latency_cycles"].as<int>();
                            if (lv["topology"])
                                config.network_level_overrides[i].topology = lv["topology"].as<std::string>();
                            // Router params
                            if (lv["router_latency"])
                                config.network_level_overrides[i].router_latency = lv["router_latency"].as<int>();
                            if (lv["router_pipeline"]) {
                                std::string rp = lv["router_pipeline"].as<std::string>();
                                if (rp == "full")           config.network_level_overrides[i].router_pipeline = 0;
                                else if (rp == "reduced")   config.network_level_overrides[i].router_pipeline = 1;
                                else if (rp == "simple")    config.network_level_overrides[i].router_pipeline = 2;
                                else if (rp == "minimal")   config.network_level_overrides[i].router_pipeline = 3;
                                else                        config.network_level_overrides[i].router_pipeline = lv["router_pipeline"].as<int>();
                            }
                            if (lv["router_bypass"])
                                config.network_level_overrides[i].router_bypass = lv["router_bypass"].as<bool>() ? 1 : 0;
                            if (lv["virtual_networks"])
                                config.network_level_overrides[i].virtual_networks = lv["virtual_networks"].as<int>();
                            if (lv["virtual_channels_per_vn"])
                                config.network_level_overrides[i].virtual_channels_per_vn = lv["virtual_channels_per_vn"].as<int>();
                            if (lv["input_buffer_depth"])
                                config.network_level_overrides[i].input_buffer_depth = lv["input_buffer_depth"].as<int>();
                            if (lv["output_buffer_depth"])
                                config.network_level_overrides[i].output_buffer_depth = lv["output_buffer_depth"].as<int>();
                        }
                    }
                }

                // Per-boundary bridge overrides (under noc.bridges, backward compat: noc.gateways)
                auto parse_bridge_overrides = [&](const YAML::Node& node) {
                    const std::array<std::string, 6> br_keys = {
                        "subarray_bank", "bank_bankgroup", "bankgroup_chip",
                        "chip_rank", "rank_channel", "channel_system"
                    };
                    for (int i = 0; i < 6; ++i) {
                        if (node[br_keys[i]]) {
                            auto& ov = config.bridge_overrides[i];
                            const auto& n = node[br_keys[i]];
                            /* 1.11.57 (latent B030): REJECT the four bridge
                             * keys nothing consumes.
                             *
                             * count, lower_frequency_mhz, upper_frequency_mhz
                             * and fifo_depth were parsed into BridgeOverride
                             * fields that have no reader anywhere:
                             * overrideBridgeConfig() takes router latency,
                             * pipeline, bypass, vnets, VCs and the two buffer
                             * depths, and no bridge count, clock or FIFO depth.
                             * A user who set them got a run that looked like it
                             * had honoured them. Wiring them up is not possible
                             * from this file -- the bridge model that would
                             * consume them lives in the internal-network
                             * library -- so the keys are refused at the point
                             * they are read, which is the one outcome that
                             * cannot mislead. */
                            {
                                static const char* kUnimplBridgeKeys[] = {
                                    "count", "lower_frequency_mhz",
                                    "upper_frequency_mhz", "fifo_depth" };
                                for (const char* bk : kUnimplBridgeKeys) {
                                    if (!n[bk]) continue;
                                    std::cerr << "[config] FATAL: noc.bridges."
                                              << br_keys[i] << "." << bk
                                              << " is parsed but NOT IMPLEMENTED"
                                                 " -- the bridge override "
                                                 "interface carries router and "
                                                 "link parameters only, so this "
                                                 "key would change nothing in "
                                                 "the emitted model. Remove it "
                                                 "rather than run with a "
                                                 "setting that does not apply."
                                              << std::endl;
                                    std::exit(2);
                                }
                            }
                            // New dual-link fields
                            if (n["lower_width_bits"])
                                ov.lower_width_bits = n["lower_width_bits"].as<int>();
                            if (n["upper_width_bits"])
                                ov.upper_width_bits = n["upper_width_bits"].as<int>();
                            if (n["latency_ns"])
                                ov.latency_ns = n["latency_ns"].as<double>();
                            // Legacy single-width fields (backward compat)
                            if (n["width_bits"])
                                ov.width_bits = n["width_bits"].as<int>();
                            if (n["latency_cycles"])
                                ov.latency_cycles = n["latency_cycles"].as<int>();
                            // Independent bridge model
                            if (n["model"])
                                ov.model = n["model"].as<std::string>();
                            // Router params
                            if (n["router_latency"])
                                ov.router_latency = n["router_latency"].as<int>();
                            if (n["router_pipeline"]) {
                                std::string rp = n["router_pipeline"].as<std::string>();
                                if (rp == "full")           ov.router_pipeline = 0;
                                else if (rp == "reduced")   ov.router_pipeline = 1;
                                else if (rp == "simple")    ov.router_pipeline = 2;
                                else if (rp == "minimal")   ov.router_pipeline = 3;
                                else                        ov.router_pipeline = n["router_pipeline"].as<int>();
                            }
                            if (n["router_bypass"])
                                ov.router_bypass = n["router_bypass"].as<bool>() ? 1 : 0;
                            if (n["virtual_networks"])
                                ov.virtual_networks = n["virtual_networks"].as<int>();
                            if (n["virtual_channels_per_vn"])
                                ov.virtual_channels_per_vn = n["virtual_channels_per_vn"].as<int>();
                            if (n["input_buffer_depth"])
                                ov.input_buffer_depth = n["input_buffer_depth"].as<int>();
                            if (n["output_buffer_depth"])
                                ov.output_buffer_depth = n["output_buffer_depth"].as<int>();
                        }
                    }
                };
                if (yaml_cfg["noc"]["bridges"]) {
                    parse_bridge_overrides(yaml_cfg["noc"]["bridges"]);
                } else if (yaml_cfg["noc"]["gateways"]) {
                    std::cerr << "WARNING: 'noc.gateways' is deprecated, use 'noc.bridges' instead\n";
                    parse_bridge_overrides(yaml_cfg["noc"]["gateways"]);
                }
            }

                /* 1.11.57 (audit round 3, A001): these keys are parsed HERE,
                 * at the top level of the config load, and not inside the
                 * noc.model branch they had been nested in.
                 *
                 * memory.power_down, memory.power_down_threshold_ns,
                 * memory.array_pg and power.temperature_k/_c -- together with
                 * the temperature range validator -- sat inside
                 * `if (yaml_cfg["noc"]["model"])`. The indentation had always
                 * shown it (a 4-space block inside a 16-space body), but the
                 * consequence is scope-shaped and so no gate caught it: every
                 * DEVICE config in the corpus sets a top-level noc.model, and
                 * every CO-SIM config declares the model under
                 * system.devices[].noc.model instead. So on exactly the shape
                 * that co-simulation uses, all four knobs were read from a
                 * branch that never executed and silently discarded -- which
                 * made the 1.11.52 temperature work (D055/C001) and the 1.11.52
                 * memory.power_down scope-parity fix (A005) both inert in
                 * co-simulation, the one scope they were written for. The
                 * power-down gate config happens to carry a top-level
                 * noc.model, so it passed. */
    /* 1.11.41: memory-side power keys, split out of pim.mc.pg.
     *   memory.power_down  DRAM JEDEC precharge power-down (IDD2P). NOT power
     *                      gating -- the array stays powered and refreshed.
     *   memory.array_pg    array-PERIPHERY power gating. Retention-free on NVM
     *                      (non-volatile cells hold state unpowered); on SRAM
     *                      the periphery gates but the volatile cell array
     *                      keeps full leakage. Illegal on DRAM, whose array
     *                      cannot be gated at all.
     *
     * 1.11.60 (audit round 4, A007): the DRAM refusal this comment has
     * promised since 1.11.41 now EXISTS. It did not: grep found the field, this
     * parse and four consumers -- two in the SRAM branch and two in the NVM
     * branch -- and no DRAM branch reads the flag at all, so `array_pg: true`
     * on a DDR4 cell was accepted in silence, gated nothing, and left the only
     * place that documents the key asserting the run would have refused it.
     * That is the "guard described in a comment that no configuration reaches"
     * shape at its purest: the described refusal had no code. It stayed
     * invisible because the failure mode is silence -- no warning, no changed
     * number, nothing for a gate to assert on.
     *
     * The comment is what is true to the physics, so the refusal is what is
     * implemented rather than the comment being softened: a DRAM cell is a
     * capacitor that must be refreshed, and refresh needs the array powered,
     * so there is no state in which the array is gated. The check needs the
     * RESOLVED technology -- which in system scope arrives from the device
     * node hundreds of lines below this parse -- so it lives beside
     * resolveMemoryTopology() rather than here. */
    /* 1.11.60 (audit round 4, A005): the POWER keys are not gated on a
     * memory: block.
     *
     * 1.11.57 (A001) moved this whole block out of `if (noc.model)`, which was
     * the reported defect, and left it inside `if (yaml_cfg["memory"])` --
     * which is the SAME defect one level down. power.temperature_k and
     * power.temperature_c are power: keys; requiring an unrelated top-level
     * memory: block for them to be read is exactly as arbitrary as requiring
     * a noc: block was. And it is not hypothetical: NONE of the six shipped
     * co-sim examples has a top-level memory: block, so the temperature knob
     * stayed inert in co-simulation -- the precise scope A001 was raised
     * about, and which I reported as fixed.
     *
     * The gate arm did not catch it because it used a DEVICE-scope config,
     * and device configs do carry a top-level memory: block. Verified before
     * this fix: system scope with temperature_k: 999 and no memory: block
     * exits 0; the same value with a memory: block exits 2.
     *
     * The memory.* keys keep their memory: guard, which is correct for them. */
    if (yaml_cfg["power"] && yaml_cfg["power"]["temperature_c"])
        config.temperature_k =
            yaml_cfg["power"]["temperature_c"].as<int>(77) + 273;
    if (yaml_cfg["power"] && yaml_cfg["power"]["temperature_k"])
        config.temperature_k =
            yaml_cfg["power"]["temperature_k"].as<int>(350);
    /* 1.11.63 (R7): see the member's comment -- this key was advertised but
     * unparsed. Top level of the power: block, same reachability rationale
     * as the temperature keys above. */
    if (yaml_cfg["power"] && yaml_cfg["power"]["termination_pj_per_bit"])
        config.termination_pj_per_bit =
            yaml_cfg["power"]["termination_pj_per_bit"].as<double>(-1.0);

    /* 1.11.60 (A005): the RANGE VALIDATOR moves with the parse. Leaving
     * it inside the memory: guard would accept an unevaluable temperature
     * on exactly the configs whose knob had just been made reachable.
     *
     * 1.11.52 (user ruling: "the temp accepted range should follow the
         * upstream tools"): the range is the INTERSECTION of what the linked
         * tools actually accept, each read from its own source rather than
         * restated here as a preference:
         *
         *   CACTI   external/cacti/io.cc:2259 --
         *             if (temp < 300 || temp > 400 || temp%10 != 0) -> error
         *           A CHECKED constraint: violating it aborts the power run.
         *   NVSim   external/nvsim/Technology.h:81-84 --
         *             double currentOnNmos[101]  ... currentOffNmos[101]
         *             /* NMOS off current (from 300K to 400K) *[/]
         *           and every consumer indexes tech->currentOnNmos[
         *             inputParameter->temperature - 300] (SubArray.cpp:131,
         *           Mux.cpp:69, OutputDriver.cpp:68). NVSim performs NO check,
         *           so a temperature outside 300..400 K is an out-of-bounds
         *           READ, not an error -- silent corruption. Our validation is
         *           what protects it. NVSim's granularity is 1 K.
         *   CACTI leakage rows -- I_off_n is tabulated at 0..100 C in 10 C
         *           steps; periphFamilyFactors() snaps to the nearest row and
         *           says so, so this one shapes accuracy, not legality.
         *
         * Intersection: 300..400 K in steps of 10 (CACTI's step is the
         * binding one; NVSim's wider 1 K granularity is a superset). The
         * first cut of this knob used [273, 373] with no step rule -- my own
         * numbers, not the tools' -- and accepted 358 K, after which McPAT
         * aborted and the run printed "Power analysis skipped" while still
         * exiting 0 (gate 1162C C4). */
        if (config.temperature_k < 300 || config.temperature_k > 400 ||
            (config.temperature_k % 10) != 0) {
            int lo = (config.temperature_k / 10) * 10;
            if (lo < 300) lo = 300;
            if (lo > 400) lo = 400;
            int hi = lo + 10; if (hi > 400) hi = 400;
            std::cerr << "[config] FATAL: power.temperature_k="
                      << config.temperature_k
                      << " is not evaluable by the linked tools: CACTI "
                         "accepts 300..400 K in steps of 10 "
                         "(external/cacti/io.cc:2259, checked); NVSim indexes "
                         "a 300..400 K table with no bounds check at all "
                         "(external/nvsim/Technology.h:81-84). Nearest legal: "
                      << lo << " K (" << (lo - 273) << " C) or "
                      << hi << " K (" << (hi - 273) << " C). Default 350 K "
                         "(77 C) is what the corpus is priced at."
                      << std::endl;
            std::exit(2);
        }

    if (yaml_cfg["memory"]) {
        if (yaml_cfg["memory"]["power_down"])
            config.mem_power_down = yaml_cfg["memory"]["power_down"].as<bool>(config.mem_power_down);
        if (yaml_cfg["memory"]["power_down_threshold_ns"])   // 1.11.51 (E17)
            config.power_down_threshold_ns =
                yaml_cfg["memory"]["power_down_threshold_ns"].as<double>(-1.0);
        /* 1.11.52: temperature knob; K wins if both given. Parsed ABOVE now
         * (1.11.60 A005); these two lines are kept so a config that does have
         * a memory: block behaves identically, and are harmless duplicates. */
        if (yaml_cfg["power"] && yaml_cfg["power"]["temperature_c"])
            config.temperature_k =
                yaml_cfg["power"]["temperature_c"].as<int>(77) + 273;
        if (yaml_cfg["power"] && yaml_cfg["power"]["temperature_k"])
            config.temperature_k =
                yaml_cfg["power"]["temperature_k"].as<int>(350);
        if (yaml_cfg["memory"]["array_pg"])
            config.mem_array_pg = yaml_cfg["memory"]["array_pg"].as<bool>(config.mem_array_pg);
    }

            // Load memory configuration
            if (yaml_cfg["memory"]) {
                if (yaml_cfg["memory"]["dq_turnaround"])
                    config.dq_turnaround_enabled =
                        yaml_cfg["memory"]["dq_turnaround"].as<bool>(true);
                config.memory_tech = canonicalMemTech(
                    yaml_cfg["memory"]["technology"].as<std::string>(config.memory_tech));

                // Reject unknown memory technologies up front rather than
                // silently falling back to DDR4 (which would hide user typos
                // and produce misleading results). HBM gen-1 was removed --
                // use HBM2/HBM3. The whitelist is canonical-only: input is
                // normalized by canonicalMemTech() before this check.
                {
                    static const std::set<std::string> kValidTechs = {
                        "DDR3", "DDR4", "DDR5", "LPDDR5", "GDDR6", "HBM2", "HBM3",
                        "SRAM", "STT_MRAM", "PCM", "RERAM"
                    };
                    if (kValidTechs.find(config.memory_tech) == kValidTechs.end()) {
                        std::cerr << "ERROR: unknown memory technology '"
                                  << config.memory_tech << "'.\n"
                                  << "Supported: DDR3, DDR4, DDR5, LPDDR5, GDDR6, HBM2, HBM3, "
                                  << "SRAM, STT_MRAM, PCM, ReRAM.\n"
                                  << "(HBM gen-1 is not supported -- use HBM2 or HBM3.)\n";
                        std::exit(1);
                    }
                }

                config.num_banks = yaml_cfg["memory"]["banks"].as<int>(config.num_banks);
                /* 1.9.35: ranks_per_channel was plumbed END TO END -- declared
                 * here, emitted into the zsim configuration, and read by the
                 * plugin, the trace driver and the analytical hierarchy model --
                 * with NO configuration key anywhere. It could therefore never
                 * be anything but its default of 1, so the rank tier of the tree
                 * was a pass-through hop for every run ever made, while the code
                 * read as though multi-rank were supported. Completing the
                 * plumbing costs one line; the default is unchanged, so no
                 * existing configuration moves. */
                if (yaml_cfg["memory"]["ranks_per_channel"]) {
                    config.hierarchy_ranks_per_channel =
                        yaml_cfg["memory"]["ranks_per_channel"].as<int>(
                            config.hierarchy_ranks_per_channel);
                }
                // subarray geometry: optional height (rows) and/or an explicit
                // count. If the count is set it wins; otherwise the per-tech
                // default (or the given height) derives it in the org block.
                if (yaml_cfg["memory"]["subarray_height"])
                    config.subarray_height = yaml_cfg["memory"]["subarray_height"].as<int>();
                if (yaml_cfg["memory"]["subarrays_per_bank"]) {
                    config.subarrays_per_bank = yaml_cfg["memory"]["subarrays_per_bank"].as<int>();
                    config.subarrays_per_bank_user_set = true;
                }
                // Optional JEDEC-org overrides (0/absent = per-tech JEDEC default).
                if (yaml_cfg["memory"]["organization"]) {
                    auto org = yaml_cfg["memory"]["organization"];
                    if (org["bank_groups"])
                        config.bg_per_chip_override = org["bank_groups"].as<int>();
                    if (org["banks_per_group"])
                        config.banks_per_bg_override = org["banks_per_group"].as<int>();
                }
                config.memory_latency_override = yaml_cfg["memory"]["latency"].as<int>(config.memory_latency_override);
                config.ports_per_bank = yaml_cfg["memory"]["ports_per_bank"].as<int>(config.ports_per_bank);
                if (yaml_cfg["memory"]["dram"] && yaml_cfg["memory"]["dram"]["device_width"]) {
                    config.dram_device_width =
                        yaml_cfg["memory"]["dram"]["device_width"].as<std::string>(config.dram_device_width);
                }

                // Parse memory parameters from YAML
                // To override external models, user must provide ALL 5 required params:
                //   1. read_latency_ns    2. write_latency_ns
                //   3. read_energy_nj     4. write_energy_nj
                //   5. static_power_mw
                auto& p = config.memory_params;

                // Timing parameters (accept multiple naming conventions)
                if (yaml_cfg["memory"]["timing"]) {
                    const auto& t = yaml_cfg["memory"]["timing"];
                    // Read latency
                    if (t["read_latency_ns"]) p.read_latency_ns = t["read_latency_ns"].as<double>();
                    else if (t["subarray_read_ns"]) p.read_latency_ns = t["subarray_read_ns"].as<double>();
                    // Write latency
                    if (t["write_latency_ns"]) p.write_latency_ns = t["write_latency_ns"].as<double>();
                    else if (t["subarray_write_ns"]) p.write_latency_ns = t["subarray_write_ns"].as<double>();
                }

                // Energy parameters
                if (yaml_cfg["memory"]["energy"]) {
                    const auto& e = yaml_cfg["memory"]["energy"];
                    if (e["read_energy_nj"]) p.read_energy_nj = e["read_energy_nj"].as<double>();
                    if (e["write_energy_nj"]) p.write_energy_nj = e["write_energy_nj"].as<double>();
                }

                // Power parameters
                if (yaml_cfg["memory"]["power"]) {
                    const auto& pw = yaml_cfg["memory"]["power"];
                    if (pw["static_power_mw"]) p.static_power_mw = pw["static_power_mw"].as<double>();
                }

                // Check if user provided ALL 5 required params to override external models
                config.use_yaml_memory_params = p.isComplete();

                if (!config.use_yaml_memory_params &&
                    (p.read_latency_ns > 0 || p.write_latency_ns > 0 ||
                     p.read_energy_nj > 0 || p.write_energy_nj > 0 || p.static_power_mw > 0)) {
                    std::cerr << "Warning: Partial YAML memory params provided.\n"
                              << "To override external models, provide ALL 5 required params:\n"
                              << "  memory:\n"
                              << "    timing:\n"
                              << "      read_latency_ns: <value>\n"
                              << "      write_latency_ns: <value>\n"
                              << "    energy:\n"
                              << "      read_energy_nj: <value>\n"
                              << "      write_energy_nj: <value>\n"
                              << "    power:\n"
                              << "      static_power_mw: <value>\n"
                              << "Using external models (NVSim/CACTI/Ramulator) instead.\n";
                    std::cerr.flush();
                }
            }

            // Parse optional memory controller overrides
            if (yaml_cfg["memory"] && yaml_cfg["memory"]["controller"]) {
                const auto& ctrl = yaml_cfg["memory"]["controller"];
                if (ctrl["type"])
                    config.zsim_mem_controller_type = ctrl["type"].as<std::string>(config.zsim_mem_controller_type);
                if (ctrl["bandwidth"]) {
                    // Accepts "25.6 GB/s", "19200 MB/s", "1 TB/s", "6400000 KB/s", or bare number (MB/s)
                    int parsed = parseBandwidthToMBs(ctrl["bandwidth"].as<std::string>());
                    if (parsed > 0) {
                        config.md1_bandwidth_mbs = parsed;
                        config.md1_bandwidth_user_set = true;
                    } else {
                        std::cerr << "  [WARN] Could not parse bandwidth: '"
                                  << ctrl["bandwidth"].as<std::string>() << "'" << std::endl;
                    }
                }
                // WeaveSimple
                if (ctrl["bound_latency"])
                    config.weave_bound_latency = ctrl["bound_latency"].as<int>(config.weave_bound_latency);
                // Ramulator
                if (ctrl["ramulator_config"])
                    config.ramulator_config_file = ctrl["ramulator_config"].as<std::string>(config.ramulator_config_file);
            }

            // Load simulation parameters
            if (yaml_cfg["simulation"]) {
                config.phase_length = yaml_cfg["simulation"]["phase_length"].as<int>(config.phase_length);
                config.max_instructions = yaml_cfg["simulation"]["max_instructions"].as<long long>(config.max_instructions);
                config.stats_interval = yaml_cfg["simulation"]["stats_interval"].as<int>(config.stats_interval);
                // Simulator parallelism: ONE knob, both workload paths (see
                // UnifiedConfig::sim_parallel).
                config.sim_parallel = yaml_cfg["simulation"]["parallel"].as<bool>(config.sim_parallel);
            }

            // Power analysis toggle (CLI --power/--no-power overrides YAML)
            if (!cli_power_set && yaml_cfg["power"] && yaml_cfg["power"]["enabled"]) {
                config.enable_power = yaml_cfg["power"]["enabled"].as<bool>(config.enable_power);
            }

            // Power report detail level
            if (yaml_cfg["power"] && yaml_cfg["power"]["report_detail"]) {
                config.power_report_detail =
                    yaml_cfg["power"]["report_detail"].as<std::string>(config.power_report_detail);
                /* 1.11.57 (audit round 3, A011): validate it, as the command
                 * line already does. An unrecognised value here fell through
                 * to "standard" in silence, so a typo did not produce the
                 * report the user asked for and nothing said so -- the same
                 * class as B026's per-level model names. */
                const std::string& rd = config.power_report_detail;
                if (rd != "summary" && rd != "standard" && rd != "verbose") {
                    std::cerr << "[config] FATAL: power.report_detail = '" << rd
                              << "' is not a report level. Valid: summary, "
                                 "standard, verbose. An unrecognised value used "
                                 "to become 'standard' silently." << std::endl;
                    std::exit(2);
                }
            }

            // 1.9.10: user-controllable process (technology) node.
            //   power.tech_node_nm         -> applies to ALL domains (host + device)
            //   power.device_tech_node_nm  -> device-only override
            //   power.host_tech_node_nm    -> host-only override
            // Default: host inherits the device node (uniform process unless
            // overridden). The device default (config.tech_node_nm, 22nm) is
            // unchanged, so existing device-side flows are bit-identical at default.
            if (yaml_cfg["power"] && yaml_cfg["power"]["tech_node_nm"]) {
                int n = yaml_cfg["power"]["tech_node_nm"].as<int>(config.tech_node_nm);
                config.tech_node_nm = n;
                config.host_tech_node_nm = n;  // uniform unless a host override follows
            }
            if (yaml_cfg["power"] && yaml_cfg["power"]["device_tech_node_nm"]) {
                config.tech_node_nm =
                    yaml_cfg["power"]["device_tech_node_nm"].as<int>(config.tech_node_nm);
            }
            if (yaml_cfg["power"] && yaml_cfg["power"]["host_tech_node_nm"]) {
                config.host_tech_node_nm =
                    yaml_cfg["power"]["host_tech_node_nm"].as<int>(config.host_tech_node_nm);
            }
            // 1.11.2: subarray bitline-pitch area knob (default unity)
            /* 1.11.30 (E5): power.interconnect_projection -- one setting for
             * both tools. Validated at parse like every other process knob. */
            if (yaml_cfg["power"] && yaml_cfg["power"]["interconnect_projection"]) {
                std::string ip =
                    yaml_cfg["power"]["interconnect_projection"].as<std::string>("conservative");
                if (ip == "conservative")      config.interconnect_projection = 1;
                else if (ip == "aggressive")   config.interconnect_projection = 0;
                else {
                    std::cerr << "ERROR: power.interconnect_projection '" << ip
                              << "' is invalid. Valid: conservative | aggressive.\n"
                              << "  conservative is the default and describes a "
                                 "manufacturable stack; aggressive sets the copper "
                                 "barrier thickness to zero, which no process can "
                                 "build.\n";
                    std::exit(1);
                }
            }
            if (yaml_cfg["power"] && yaml_cfg["power"]["logic_reference_mhz"]) {
                const double lr =
                    yaml_cfg["power"]["logic_reference_mhz"].as<double>(0.0);
                if (!(lr > 0.0) || lr > 20000.0) {
                    std::cerr << "ERROR: power.logic_reference_mhz = " << lr
                              << " is invalid. Give the clock of the LOGIC-process\n"
                              << "  design this PE is compared against (e.g. 2000 for a\n"
                              << "  2 GHz core). PIMID divides it by the CV/I delay ratio\n"
                              << "  from the CACTI tables to bound a DRAM-periphery PE.\n";
                    std::exit(1);
                }
                config.logic_reference_mhz = lr;
            }
            if (yaml_cfg["power"] && yaml_cfg["power"]["device_corner"]) {
                config.device_corner =
                    yaml_cfg["power"]["device_corner"].as<std::string>(config.device_corner);
                if (config.device_corner != "hp" && config.device_corner != "lstp" &&
                    config.device_corner != "lop") {
                    std::cerr << "ERROR: power.device_corner '" << config.device_corner
                              << "' is not a corner CACTI's tables carry. Valid: hp "
                                 "(high performance), lstp (low standby power), lop "
                                 "(low operating power)." << std::endl;
                    std::exit(1);
                }
            }
            /* 1.11.31 (user ruling E7): validated, bounded and explained. This
             * was the ONE knob in the 1.11 process surface with no check at
             * all -- 0, negative and 1e9 all parsed, and a later guard quietly
             * turned non-positive into 1.0, so a typo silently became "no
             * penalty".
             *
             * WHAT IT IS: circuits placed against the memory array must be
             * laid out ON THE ARRAY'S PITCH -- a sense amplifier cannot be
             * wider than the bitline pair it serves, however small its
             * transistors are. That is a LAYOUT constraint, separate from the
             * device-size penalty the 2.44x area factor already carries.
             * Vogelsang (MICRO 2010) names the distinction directly: on-pitch
             * circuitry (sense-amp stripes, local wordline drivers) is limited
             * by transistor size and number, off-pitch circuitry by wiring.
             *
             * FLOOR 1.0: the knob multiplies a PENALTY. Below 1 it becomes a
             * discount, claiming the PE is DENSER than the DRAM-periphery
             * baseline it is being measured against.
             * CEILING 10: it compounds into the reported band [fa*p, fa^2*p];
             * at p=10 the band top is ~60x, which no silicon supports.
             * WARNING above 1.25, from the only silicon anchor we have:
             * Samsung's FIMDRAM bounds a DRAM-process compute unit at
             * ~1.5 mm^2 (ISSCC 2021 25.4 + our HBM2 density), our comparable
             * PE prices at 0.4925 mm^2 in logic, so the TOTAL observed penalty
             * is <= 3.05x; with fa = 2.444 that leaves <= 1.25x for pitch.
             * Their PCU is bank-shared rather than subarray-pressed, so this
             * is a warning and not a refusal. */
            if (yaml_cfg["power"] && yaml_cfg["power"]["subarray_pitch_factor"]) {
                const double pf =
                    yaml_cfg["power"]["subarray_pitch_factor"].as<double>(1.0);
                if (!std::isfinite(pf) || pf < 1.0 || pf > 10.0) {
                    std::cerr << "ERROR: power.subarray_pitch_factor = " << pf
                              << " is out of range. Valid: [1.0, 10.0].\n"
                              << "  It multiplies the DRAM-periphery AREA factor, so it is a\n"
                              << "  PENALTY for laying a PE out on the array's bitline pitch.\n"
                              << "  Below 1.0 it would claim the PE is DENSER than the\n"
                              << "  DRAM-periphery baseline it is measured against; above 10\n"
                              << "  the reported band top exceeds 60x, which no silicon supports.\n";
                    std::exit(1);
                }
                if (pf > 1.25) {
                    std::cout << "  [tech] WARNING: subarray_pitch_factor=" << pf
                              << " puts the TOTAL DRAM-process penalty at "
                              << (pf * 2.444) << "x. Samsung's FIMDRAM (ISSCC 2021 "
                                 "25.4) bounds a real DRAM-process compute unit at "
                                 "~3.05x, i.e. pitch <= ~1.25 given our area factor. "
                                 "Reported at your value; the value is the hypothesis."
                              << std::endl;
                }
                config.subarray_pitch_factor = pf;
            }

            // McPAT derived-parameter overrides
            if (yaml_cfg["power"] && yaml_cfg["power"]["mcpat_overrides"]) {
                auto ov = yaml_cfg["power"]["mcpat_overrides"];
                for (auto it = ov.begin(); it != ov.end(); ++it) {
                    /* 1.11.56 (audit B029): a discarded key is a config error,
                     * not a no-op. power.mcpat_overrides takes numbers;
                     * writing a string there (device_type: lstp is the
                     * natural mistake) used to vanish without a word, and the
                     * user went on believing the override was in effect. */
                    try {
                        config.mcpat_overrides[it->first.as<std::string>()] =
                            it->second.as<double>();
                    } catch (const std::exception&) {
                        std::cerr << "[config] FATAL: power.mcpat_overrides."
                                  << it->first.as<std::string>()
                                  << " is not a number. Every McPAT override is "
                                     "numeric; a name or a boolean cannot be "
                                     "applied, and this used to be discarded in "
                                     "silence." << std::endl;
                        std::exit(2);
                    }
                }
            }

            // PCIe transfer modeling config (for cosim)
            /* 1.11.34: the surface is power.link.*, because the model accepts
             * ELEVEN link classes across five families (pcie_gen3/4/5,
             * cxl_2_0/cxl_3_0/cxl_mem, nvlink_3_0/4_0/c2c, ualink_1_0,
             * interposer) and naming all of them "pcie" misdescribes every one
             * that is not. An interposer is a parallel on-package link with no
             * SerDes at all -- calling its configuration "pcie" invites exactly
             * the category error found in its lane count this release.
             * power.pcie.* stays as a DEPRECATED ALIAS so no existing config
             * breaks; it is read only when power.link is absent. */
            if (yaml_cfg["power"] && (yaml_cfg["power"]["link"] || yaml_cfg["power"]["pcie"])) {
                const bool legacy_key = !yaml_cfg["power"]["link"];
                if (legacy_key)
                    std::cout << "  [config] power.pcie.* is deprecated; use "
                                 "power.link.* (the model carries pcie, cxl, "
                                 "nvlink, ualink and interposer classes). The "
                                 "alias still works." << std::endl;
                auto pc = legacy_key ? yaml_cfg["power"]["pcie"]
                                     : yaml_cfg["power"]["link"];
                config.pcie_timing_configured = true;  // power.pcie section present
                if (pc["enabled"]) config.pcie_enabled_user_set = true;   // 1.11.47 (L176)
                config.pcie_enabled = pc["enabled"].as<bool>(config.pcie_enabled);
                config.pcie_num_units = pc["num_units"].as<int>(config.pcie_num_units);
                config.pcie_num_channels = pc["num_channels"].as<int>(config.pcie_num_channels);
                if (pc["duty_cycle"]) {
                    config.pcie_duty_cycle = pc["duty_cycle"].as<double>(config.pcie_duty_cycle);
                    config.pcie_duty_cycle_user_set = true;   // 1.11.40 (E19)
                }
                if (pc["total_load_perc"]) config.pcie_load_perc_user_set = true;  // 1.11.56 (B015)
                config.pcie_load_perc = pc["total_load_perc"].as<double>(config.pcie_load_perc);
                // PCIe timing model params (tunable for CXL-like behavior)
                config.pcie_base_latency_ns = pc["base_latency_ns"].as<double>(config.pcie_base_latency_ns);
                config.pcie_bandwidth_GBs = pc["bandwidth_GBs"].as<double>(config.pcie_bandwidth_GBs);
                config.pcie_num_lanes = pc["num_lanes"].as<int>(config.pcie_num_lanes);
                config.pcie_pj_per_bit_override = pc["pj_per_bit_override"].as<double>(config.pcie_pj_per_bit_override);
                config.pcie_link_type = pc["link_type"].as<std::string>(config.pcie_link_type);
                config.pcie_model = pc["model"].as<std::string>(config.pcie_model);
                if (config.pcie_model == "md1") config.pcie_model = "simple";  // backward compat
                // Link technology + per-transaction overhead (tunable per link).
                config.pcie_link_type = pc["link_type"].as<std::string>(config.pcie_link_type);
                config.pcie_header_bytes = pc["header_bytes"].as<int>(config.pcie_header_bytes);
                config.pcie_coherence_extra_ns =
                    pc["coherence_extra_ns"].as<double>(config.pcie_coherence_extra_ns);
                // Interposer preset (2.5D on-package): fill only fields the user
                // did NOT set explicitly, so an interposer link is one knob away.
                // Preset matches the explicit system.network.links interposer
                // entry (2.5D on-package): low latency, high BW, no overhead.
                if (config.pcie_link_type == "interposer") {
                    if (!pc["base_latency_ns"])   config.pcie_base_latency_ns = 5.0;      // ~few ns on-package
                    if (!pc["bandwidth_GBs"])     config.pcie_bandwidth_GBs   = 256.0;    // 2.5D interposer BW
                    if (!pc["header_bytes"])      config.pcie_header_bytes    = 0;        // no protocol framing
                    if (!pc["coherence_extra_ns"])config.pcie_coherence_extra_ns = 0.0;   // no protocol coherence
                }
            }

            // Host processor config (for co-sim ZSim-based host modeling)
            if (yaml_cfg["host"]) {
                auto h = yaml_cfg["host"];
                config.host_core_type = h["core_type"].as<std::string>(config.host_core_type);
                // STRICT: same whitelist as pim.pe.type. Unknown host core
                // types used to fall through to the "Simple" default at zsim
                // emission silently (e.g. the removed timing_core).
                if (config.host_core_type != "ooo_core" &&
                    config.host_core_type != "in_order_core" &&
                    config.host_core_type != "simple_core" &&
                    config.host_core_type != "alu_core" &&
                    config.host_core_type != "null_core") {
                    std::cerr << "Error: unknown host.core_type '"
                              << config.host_core_type
                              << "'. Valid: ooo_core | in_order_core | simple_core"
                              << " | alu_core | null_core" << std::endl;
                    return 1;
                }
                config.host_num_cores = h["num_cores"].as<int>(config.host_num_cores);
                config.host_frequency_mhz = h["frequency_mhz"].as<double>(config.host_frequency_mhz);
                config.host_tech_node_nm = h["tech_node_nm"].as<int>(config.host_tech_node_nm);
                if (h["cache"]) {
                    config.host_l1d_kb = h["cache"]["l1d_kb"].as<int>(config.host_l1d_kb);
                    config.host_l1i_kb = h["cache"]["l1i_kb"].as<int>(config.host_l1i_kb);
                    config.host_l2_kb = h["cache"]["l2_kb"].as<int>(config.host_l2_kb);
                    config.host_l3_kb = h["cache"]["l3_kb"].as<int>(config.host_l3_kb);
                }
                if (h["memory"]) {
                    config.host_memory_tech = canonicalMemTech(
                        h["memory"]["technology"].as<std::string>(config.host_memory_tech));
                }
            }

            //=================================================================
            // Multi-host/multi-device system configuration (scope: "system")
            //=================================================================
            if (config.scope == "system" && yaml_cfg["system"]) {
                auto sys = yaml_cfg["system"];

                // 1.11.56 (audit B046): the one normaliser, shared with device scope.
                auto normalizeCoreType = normalizeCoreTypeName;

                // Parse hosts
                if (sys["hosts"]) {
                    for (const auto& h : sys["hosts"]) {
                        UnifiedConfig::SystemNode node;
                        node.name = h["name"].as<std::string>("host" + std::to_string(config.system_nodes.size()));
                        node.role = UnifiedConfig::SystemNode::HOST;
                        node.pg_host = h["pg"].as<bool>(node.pg_host);  // 1.11.20 (D7)
                        node.core_type = normalizeCoreType(h["core_type"].as<std::string>("ooo_core"));
                        node.num_cores = h["num_cores"].as<int>(4);
                        node.frequency_mhz = h["frequency_mhz"].as<double>(3000.0);
                        // Host node process node: explicit YAML wins; else the
                        // resolved host default (power.host_tech_node_nm if set, else
                        // the device node -- uniform process). Replaces the old
                        // hardcoded 7nm host default.
                        {
                            int host_default = (config.host_tech_node_nm >= 0)
                                ? config.host_tech_node_nm : config.tech_node_nm;
                            node.tech_node_nm = h["tech_node_nm"].as<int>(host_default);
                        }
                        if (h["cache"]) {
                            node.l1d_kb = h["cache"]["l1d_kb"].as<int>(node.l1d_kb);
                            node.l1i_kb = h["cache"]["l1i_kb"].as<int>(node.l1i_kb);
                            node.l2_kb = h["cache"]["l2_kb"].as<int>(node.l2_kb);
                            node.l3_kb = h["cache"]["l3_kb"].as<int>(node.l3_kb);
                            node.enable_l3 = (node.l3_kb > 0);
                        }
                        if (h["memory"]) {
                            node.memory_tech = canonicalMemTech(
                                h["memory"]["technology"].as<std::string>(node.memory_tech));
                            // Optional per-channel BW / channel-count overrides
                            // (auto-derived from technology when absent).
                            node.mem_bandwidth_mbs =
                                h["memory"]["bandwidth_mbs"].as<int>(node.mem_bandwidth_mbs);
                            node.mem_channels =
                                h["memory"]["channels"].as<int>(node.mem_channels);
                            // Host-path idle-latency adder override (ns). Absent =
                            // auto per-tech default (getHostPathAdderNs).
                            node.mem_latency_adder_ns =
                                h["memory"]["latency_adder_ns"].as<double>(node.mem_latency_adder_ns);
                            // Host-path DECOMPOSITION overrides (1.7.2). Partial
                            // override merges over the per-tech default split.
                            if (h["memory"]["host_path"]) {
                                auto hp = h["memory"]["host_path"];
                                if (hp["fabric_ns"]) {
                                    node.host_path_fabric_ns = hp["fabric_ns"].as<double>();
                                    node.host_path_any_set = true;
                                }
                                if (hp["coherence_ns"]) {
                                    node.host_path_coherence_ns = hp["coherence_ns"].as<double>();
                                    node.host_path_any_set = true;
                                }
                                if (hp["mc_pipeline_ns"]) {
                                    node.host_path_mc_pipeline_ns = hp["mc_pipeline_ns"].as<double>();
                                    node.host_path_any_set = true;
                                }
                                if (hp["phy_ns"]) {
                                    node.host_path_phy_ns = hp["phy_ns"].as<double>();
                                    node.host_path_any_set = true;
                                }
                            }
                            // Competing-totals guard: the aggregate adder and the
                            // decomposed host_path form are mutually exclusive.
                            if (h["memory"]["latency_adder_ns"] && node.host_path_any_set) {
                                std::cerr << "ERROR: host '" << node.name
                                          << "' sets BOTH memory.latency_adder_ns AND "
                                          << "memory.host_path.* -- competing host-path "
                                          << "totals. Use one form only (aggregate "
                                          << "latency_adder_ns OR the decomposed "
                                          << "host_path block)." << std::endl;
                                exit(1);
                            }
                        }
                        // SEPARATE host main memory (1.7.4). Consumed only when
                        // the paired device sets is_default_mem=false; a plain
                        // non-PIM memory of any of the 11 techs. technology is
                        // canonicalized + validated in resolveMemoryTopology.
                        // bandwidth_gbs / channels are optional per-channel
                        // overrides (auto-derived from tech when absent).
                        /* 1.11.43 (E23, user ruling): the host's FPU is
                         * CONFIGURABLE, symmetric with the device. A host
                         * without one charges its own fp_emulation_cycles --
                         * per node, so neither side's setting leaks. */
                        node.pe_has_fpu = h["floating_point"].as<bool>(node.pe_has_fpu);
                        node.pe_fp_emul_cycles = h["fp_emulation_cycles"].as<uint32_t>(node.pe_fp_emul_cycles);
                        if (h["mem"]) {
                            node.host_mem_present = true;
                            if (h["mem"]["technology"])
                                node.host_mem_tech = canonicalMemTech(
                                    h["mem"]["technology"].as<std::string>());
                            /* 1.11.57 (latent B034): REJECT host.mem.capacity_gb.
                             *
                             * It was parsed into a field whose own declaration
                             * comment called it "advisory" and which nothing
                             * ever read. There is nowhere for it to go: host
                             * nodes are given NO address range at all (the
                             * range assignment hands 4 GB windows to devices
                             * and 0..0 to hosts), and no bandwidth, latency or
                             * energy term in this model has a capacity
                             * dimension. So a config declaring 16 GB of host
                             * memory got exactly the same run as one declaring
                             * none, with nothing said. Wiring it up would mean
                             * inventing a host address-space model; refusing
                             * the key is the honest half of that choice. */
                            if (h["mem"]["capacity_gb"]) {
                                std::cerr << "[config] FATAL: host.mem.capacity_gb"
                                             " is NOT IMPLEMENTED -- host nodes "
                                             "carry no address range and no term "
                                             "in this model scales with memory "
                                             "capacity, so the key would change "
                                             "nothing. Remove it rather than run "
                                             "with a capacity that is not "
                                             "simulated." << std::endl;
                                std::exit(2);
                            }
                            if (h["mem"]["bandwidth_gbs"]) {
                                double gbs = h["mem"]["bandwidth_gbs"].as<double>();
                                node.host_mem_bandwidth_mbs =
                                    (int)std::llround(gbs * 1000.0);  // GB/s -> per-channel MB/s
                            }
                            node.host_mem_channels =
                                h["mem"]["channels"].as<int>(node.host_mem_channels);
                        }
                        // Host NoC (analytic crossbar): topology/model/hop_cycles.
                        if (h["noc"]) {
                            node.host_noc_topology =
                                h["noc"]["topology"].as<std::string>(node.host_noc_topology);
                            node.host_noc_model =
                                h["noc"]["model"].as<std::string>(node.host_noc_model);
                            node.host_noc_hop_cycles =
                                h["noc"]["hop_cycles"].as<int>(node.host_noc_hop_cycles);
                        }
                        if (h["workload"]) {
                            node.workload_binary = h["workload"]["binary"].as<std::string>("");
                            if (h["workload"]["args"]) {
                                for (const auto& a : h["workload"]["args"])
                                    node.workload_args.push_back(a.as<std::string>());
                            }
                        }
                        config.system_nodes.push_back(node);
                    }
                }

                // Parse devices
                if (sys["devices"]) {
                    for (const auto& d : sys["devices"]) {
                        UnifiedConfig::SystemNode node;
                        node.name = d["name"].as<std::string>("device" + std::to_string(config.system_nodes.size()));
                        node.role = UnifiedConfig::SystemNode::DEVICE;

                        std::string dtype = d["type"].as<std::string>("compute");
                        node.device_type = (dtype == "memory") ?
                            UnifiedConfig::SystemNode::MEMORY_ONLY :
                            UnifiedConfig::SystemNode::COMPUTE;

                        std::string attach = d["attachment"].as<std::string>("external");
                        node.attachment = (attach == "internal") ?
                            UnifiedConfig::SystemNode::INTERNAL :
                            UnifiedConfig::SystemNode::EXTERNAL;

                        node.frequency_mhz = d["frequency_mhz"].as<double>(1000.0);
                        // Device node process node: explicit YAML wins; else the
                        // device default (config.tech_node_nm / power.device_tech_node_nm,
                        // 22nm unless overridden). Default unchanged -> device flows
                        // are bit-identical when no override is given.
                        node.tech_node_nm = d["tech_node_nm"].as<int>(config.tech_node_nm);

                        if (d["memory"]) {
                            node.memory_tech = canonicalMemTech(
                                d["memory"]["technology"].as<std::string>(node.memory_tech));
                            node.ports_per_bank = d["memory"]["ports_per_bank"].as<int>(node.ports_per_bank);
                            node.banks = d["memory"]["banks"].as<int>(node.banks);
                        }

                        // Memory-topology knob (1.7.4). true (default): this
                        // device IS host main memory (host tech = device tech).
                        // false: accelerator-side memory only -- the host MUST
                        // supply a host.mem block (resolveMemoryTopology enforces).
                        node.is_default_mem = d["is_default_mem"].as<bool>(node.is_default_mem);

                        // PIM config (only for compute devices)
                        if (node.device_type == UnifiedConfig::SystemNode::COMPUTE) {
                            node.pe_type = normalizeCoreType(d["pe_type"].as<std::string>("alu_core"));
                            node.num_pes = d["num_pes"].as<int>(0);
                            node.num_cores = node.num_pes;  // PEs are cores in ZSim
                            node.core_type = node.pe_type;

                            if (d["pim"]) {
                                auto pim = d["pim"];
                                if (pim["placement"] && pim["placement"]["level"])
                                    node.placement_level = pim["placement"]["level"].as<std::string>();
                                if (pim["pe"]) {
                                    node.alu_compute_factor = pim["pe"]["compute_factor"].as<double>(node.alu_compute_factor);
                                    node.alu_access_factor = pim["pe"]["access_factor"].as<double>(node.alu_access_factor);
                                    node.alu_throughput_factor = pim["pe"]["throughput_factor"].as<double>(node.alu_throughput_factor);
                                    node.alu_operand_width = pim["pe"]["operand_width"].as<int>(node.alu_operand_width);
                                    node.alu_energy_factor = pim["pe"]["energy_factor"].as<double>(node.alu_energy_factor);
                                    node.pg_pe = pim["pe"]["pg"].as<bool>(node.pg_pe);  // 1.11.16 (#84)
                                    /* 1.11.43 (E23): the FPU keys were only
                                     * parsed in DEVICE scope, so co-sim could
                                     * not configure an FPU-less PE at all --
                                     * and a global would have leaked it onto
                                     * the host. Per node, like pg. */
                                    node.pe_has_fpu = pim["pe"]["floating_point"].as<bool>(node.pe_has_fpu);
                                    node.pe_fp_emul_cycles = pim["pe"]["fp_emulation_cycles"].as<uint32_t>(node.pe_fp_emul_cycles);
                                    // 1.11.56 (B054): per node, like the FPU keys.
                                    node.alu_bit_serial = pim["pe"]["bit_serial"].as<bool>(node.alu_bit_serial);
                                    node.inorder_issue_width = pim["pe"]["issue_width"].as<int>(node.inorder_issue_width);
                                }
                                if (pim["mc"]) {
                                    node.pe_mc_declared = true;   // 1.11.56 (B055)
                                    node.pe_mc_type = pim["mc"]["type"].as<std::string>(node.pe_mc_type);
                                    node.pes_per_mc = pim["mc"]["pes_per_mc"].as<int>(node.pes_per_mc);
                                    node.pg_mc = pim["mc"]["pg"].as<bool>(node.pg_mc);  // 1.11.16 (#84)
                                }
                            }

                            if (d["noc"]) {
                                node.noc_topology = d["noc"]["topology"].as<std::string>(node.noc_topology);
                                node.noc_model = d["noc"]["model"].as<std::string>(node.noc_model);
                                node.pg_noc = d["noc"]["pg"].as<bool>(node.pg_noc);  // 1.11.16 (#84)
                            }
                        } else {
                            // Memory-only device: no PEs/cores
                            node.num_cores = 0;
                            node.num_pes = 0;
                        }

                        if (d["cache"]) {
                            node.l1d_kb = d["cache"]["l1d_kb"].as<int>(node.l1d_kb);
                            node.l1i_kb = d["cache"]["l1i_kb"].as<int>(node.l1i_kb);
                            node.l2_kb = d["cache"]["l2_kb"].as<int>(node.l2_kb);
                            node.l3_kb = d["cache"]["l3_kb"].as<int>(node.l3_kb);
                        }

                        if (d["workload"]) {
                            node.workload_binary = d["workload"]["binary"].as<std::string>("");
                            if (d["workload"]["args"]) {
                                for (const auto& a : d["workload"]["args"])
                                    node.workload_args.push_back(a.as<std::string>());
                            }
                        }

                        config.system_nodes.push_back(node);
                    }
                }

                // Parse system network
                if (sys["network"]) {
                    auto net = sys["network"];
                    config.system_network.topology = net["topology"].as<std::string>(config.system_network.topology);
                    config.system_network.model = net["model"].as<std::string>(config.system_network.model);
                    if (config.system_network.model == "md1" || config.system_network.model == "analytical")
                        config.system_network.model = "simple";  // backward compat
                    config.system_network.link_width_bits = net["link_width_bits"].as<int>(config.system_network.link_width_bits);
                    config.system_network.frequency_ghz = net["frequency_ghz"].as<double>(config.system_network.frequency_ghz);
                    config.system_network.latency_cycles = net["latency_cycles"].as<int>(config.system_network.latency_cycles);
                    config.system_network.router_latency = net["router_latency"].as<int>(config.system_network.router_latency);
                    config.system_network.virtual_channels_per_vn = net["virtual_channels_per_vn"].as<int>(config.system_network.virtual_channels_per_vn);
                    config.system_network.input_buffer_depth = net["input_buffer_depth"].as<int>(config.system_network.input_buffer_depth);
                    config.system_network.output_buffer_depth = net["output_buffer_depth"].as<int>(config.system_network.output_buffer_depth);

                    // Per-link overrides
                    if (net["links"]) {
                        for (const auto& lnk : net["links"]) {
                            UnifiedConfig::SystemLinkConfig link;
                            link.src_name = lnk["src"].as<std::string>("");
                            link.dst_name = lnk["dst"].as<std::string>("");
                            link.link_type = lnk["type"].as<std::string>("pcie_gen5");
                            // 1.11.57 (latent B033): see SystemLinkConfig.
                            if (lnk["lanes"]) {
                                std::cerr << "[config] FATAL: "
                                             "system.network.links[].lanes is NOT "
                                             "IMPLEMENTED -- the link's rate comes "
                                             "from bandwidth_GBs or the type "
                                             "preset, and no lane count scales it. "
                                             "Set bandwidth_GBs instead."
                                          << std::endl;
                                std::exit(2);
                            }
                            link.base_latency_ns = lnk["base_latency_ns"].as<double>(-1.0);
                            link.bandwidth_GBs = lnk["bandwidth_GBs"].as<double>(-1.0);

                            // Apply presets from link type if not explicitly set
                            // Presets: {latency_ns, bw_GBs, header_bytes, coherence, coh_extra_ns}
                            if (link.base_latency_ns < 0 || link.bandwidth_GBs < 0) {
                                double preset_lat = 500.0, preset_bw = 63.0;
                                int preset_hdr = 20; double preset_coh_ns = 0.0;
                                if (link.link_type == "pcie_gen4")        { preset_lat = 500.0; preset_bw = 31.5; preset_hdr = 20; }
                                else if (link.link_type == "pcie_gen5")   { preset_lat = 500.0; preset_bw = 63.0; preset_hdr = 20; }
                                else if (link.link_type == "cxl_2_0")    { preset_lat = 200.0; preset_bw = 63.0; preset_hdr = 16; preset_coh_ns = 50.0; }
                                else if (link.link_type == "cxl_3_0")    { preset_lat = 100.0; preset_bw = 126.0; preset_hdr = 16; preset_coh_ns = 30.0; }
                                else if (link.link_type == "nvlink_3_0") { preset_lat = 700.0; preset_bw = 50.0; preset_hdr = 16; }
                                else if (link.link_type == "nvlink_4_0") { preset_lat = 500.0; preset_bw = 100.0; preset_hdr = 16; }
                                else if (link.link_type == "nvlink_c2c") { preset_lat = 100.0; preset_bw = 450.0; preset_hdr = 16; preset_coh_ns = 50.0; }
                                else if (link.link_type == "ualink_1_0") { preset_lat = 200.0; preset_bw = 100.0; preset_hdr = 16; preset_coh_ns = 40.0; }
                                else if (link.link_type == "interposer") { preset_lat = 5.0;   preset_bw = 256.0; preset_hdr = 0; }
                                if (link.base_latency_ns < 0) link.base_latency_ns = preset_lat;
                                if (link.bandwidth_GBs < 0)   link.bandwidth_GBs = preset_bw;
                                link.header_bytes = preset_hdr;
                                link.coherence_extra_ns = preset_coh_ns;
                            }

                            config.system_network.links.push_back(link);
                        }
                    }
                }

                // Host<->device two-layer bridge (1.7.1). All fields optional;
                // unset fields derive from the device memory tech at emission.
                if (sys["bridge"]) {
                    auto br = sys["bridge"];
                    config.bridge_protocol = br["protocol"].as<std::string>(config.bridge_protocol);
                    config.bridge_phy = br["phy"].as<std::string>(config.bridge_phy);
                    config.bridge_bandwidth_gbs = br["bandwidth_gbs"].as<double>(config.bridge_bandwidth_gbs);
                    config.bridge_latency_ns = br["latency_ns"].as<double>(config.bridge_latency_ns);
                    config.bridge_channels = br["channels"].as<int>(config.bridge_channels);
                    config.bridge_protocol_overhead_ns =
                        br["protocol_overhead_ns"].as<double>(config.bridge_protocol_overhead_ns);
                    config.bridge_uncached_ns = br["uncached_ns"].as<double>(config.bridge_uncached_ns);
                }

                // Case-1 COHERENCE flush accounting (1.7.2). All fields optional.
                if (sys["coherence"]) {
                    auto co = sys["coherence"];
                    config.coherence_mode = co["mode"].as<std::string>(config.coherence_mode);
                    config.coherence_writeback_bw_gbs =
                        co["writeback_bw_gbs"].as<double>(config.coherence_writeback_bw_gbs);
                    config.coherence_flush_fixed_ns =
                        co["flush_fixed_ns"].as<double>(config.coherence_flush_fixed_ns);
                    /* 1.11.59 (audit round 3, F021): REFUSE a non-zero value.
                     *
                     * The flush footprint is MEASURED at roi_begin from the
                     * host's dirty lines (N7), and since then the zsim side
                     * panics on any configured footprintBytes -- so this key
                     * can only abort the run it claims to configure. It was
                     * still parsed, still emitted, and the debug line still
                     * printed an "(OVERRIDE)" branch describing an override
                     * that cannot take effect. Refusing here says so at the
                     * point the user wrote it, instead of at a panic three
                     * layers down. */
                    {
                        long long fp = co["footprint_bytes"].as<long long>(0);
                        if (fp != 0) {
                            std::cerr << "[config] FATAL: coherence.footprint_bytes = "
                                      << fp << " cannot be honoured. The flush "
                                         "footprint is measured at ROI entry from "
                                         "the host's dirty cache lines; a configured "
                                         "value is refused by the timing model, so "
                                         "this run would abort. Remove the key to "
                                         "use the measurement." << std::endl;
                            std::exit(2);
                        }
                        config.coherence_footprint_bytes = 0;
                    }
                }

                // Kernel LAUNCH cost tree (1.7.3). All fields optional.
                if (sys["launch"]) {
                    auto la = sys["launch"];
                    config.launch_doorbell_ns = la["doorbell_ns"].as<double>(config.launch_doorbell_ns);
                    config.launch_dispatch_ns = la["dispatch_ns"].as<double>(config.launch_dispatch_ns);
                    config.launch_cmd_bytes = la["cmd_bytes"].as<int>(config.launch_cmd_bytes);
                    config.launch_ack_bytes = la["ack_bytes"].as<int>(config.launch_ack_bytes);
                }
            }

        } catch (const YAML::Exception& e) {
            /* 1.11.52 (audit B019): A HALF-APPLIED CONFIG IS NOT A CONFIG.
             * This caught every YAML error -- including one thrown midway
             * through applying the file -- printed "Warning", and let the run
             * continue with whatever subset of keys had been assigned before
             * the throw. The result is a simulation of a machine the user
             * never described, reported as if it were theirs, with defaults
             * silently standing in for the rest. A config that cannot be
             * fully applied is a fatal input error. */
            std::cerr << "[config] FATAL: failed to apply the YAML config: "
                      << e.what() << "\n"
                         "  Keys applied before the error are already in effect,"
                         " so continuing would simulate a machine that is"
                         " neither the file's nor the defaults'. Fix the config"
                         " and re-run." << std::endl;
            std::exit(2);
        }
    }

    /* Auto-derive memory controller type and parameters from technology.
     *
     * 1.11.57 (audit round 3, B009): DEVICE SCOPE ONLY here. In system scope
     * config.memory_tech is still the constructor default "SRAM" at this
     * point -- the device node's technology is adopted a few hundred lines
     * below -- so running this now derived the controller and the M/D/1
     * service rate for an SRAM device that does not exist, and printed a
     * "SRAM M/D/1 cap" line on a co-simulation with no SRAM anywhere. The
     * adoption block calls this itself, once the technology is real. */
    if (config.scope != "system") {
        getMemControllerConfig(config);
    }

    // Pre-compute internal DRAM hierarchy latencies. The device in a co-sim
    // IS the device: system scope adopts the first compute device node's
    // parameters and runs the SAME derivation device scope runs, so init
    // builds the same PE memory interfaces / device NoC / memory model.
    /* 1.11.51 (N2, user ruling): in DEVICE scope, an on-DRAM-die PE's node
     * is the die's generation for EVERYTHING -- pinned BEFORE the hierarchy
     * derivation so timing (hierarchy latencies, cache probes, array
     * queries) and power see one node. Gate 1161I3 K1 caught the late pin:
     * areas equalized but cycles still differed 1.5% because the hierarchy
     * had already been derived at the logic knob's value. The predicate
     * uses the placement STRING (the level integer is derived inside the
     * call this must precede). System scope pins per device node instead. */
    if (config.scope != "system") {
        /* 1.11.56 (audit B059): validate the REQUESTED node before the pin
         * replaces it. The unconditional validator 90 lines below never saw
         * an on-die value, because this block had already overwritten it with
         * a table node -- so a sweep over technology.node_nm in {22, 28, 32}
         * quietly produced identical numbers at 22 and 28 for every on-die
         * placement, which is exactly the plateau 1.11.51 (L75) set out to
         * remove, while an off-die placement in the same sweep failed at 28. */
        validateTechNodeNm(config.tech_node_nm, "config load: technology.node_nm");
        const std::string& mt = config.memory_tech;
        const std::string& pl = config.placement_level;
        bool dft = !(mt == "SRAM" || mt == "STT_MRAM" || mt == "PCM" ||
                     mt == "RERAM");
        bool cc = mt.rfind("LPDDR", 0) == 0 || mt.rfind("GDDR", 0) == 0 ||
                  mt.rfind("HBM", 0) == 0;
        bool on_die = dft &&
            (pl == "SUBARRAY" || pl == "BANK" || pl == "BANK_GROUP" ||
             pl == "CHIP" || (pl == "CHANNEL" && cc));
        if (on_die) {
            DRAMGenClass gpin = getDRAMGenClass(mt);
            if (config.tech_node_nm != gpin.cacti_table_nm) {
                std::cout << "[tech] on-DRAM-die PE: technology.node_nm="
                          << config.tech_node_nm
                          << " is a logic-die knob; the die's generation (class "
                          << gpin.cls << " -> CACTI " << gpin.cacti_table_nm
                          << "nm table) governs this placement -- pinned for "
                             "timing and power alike." << std::endl;
                config.tech_node_nm = gpin.cacti_table_nm;
            }
        }
    }
    if (config.scope != "system") {
        computeHierarchyLatencies(config);
    } else {
        for (const auto& n : config.system_nodes) {
            if (n.role != UnifiedConfig::SystemNode::DEVICE) continue;
            if (n.device_type != UnifiedConfig::SystemNode::COMPUTE) continue;
            if (n.num_pes <= 0) continue;
            config.memory_tech = n.memory_tech;
            config.num_pes = n.num_pes;
            if (n.banks > 0) config.num_banks = n.banks;
            if (!n.placement_level.empty()) config.placement_level = n.placement_level;
            config.pe_mc_enabled = n.pe_mc_declared;   // 1.11.56 (B055)
            /* 1.11.56 (audit B054/B056): carry the element's datapath shape
             * and its FP capability into the scope that emits them. Without
             * this, bit_serial/issue_width stayed at the global defaults for
             * every co-simulated device, and reportFpWithoutFpu() returned at
             * its first line because config.pe_has_fp was still true even
             * when hasFpu = false had been emitted for the group -- so the
             * contradiction that report exists to surface was invisible in
             * exactly the scope that can configure it. */
            config.alu_bit_serial = n.alu_bit_serial;
            config.inorder_issue_width = n.inorder_issue_width;
            config.pe_has_fp = n.pe_has_fpu;
            config.pe_fp_emul_cycles = n.pe_fp_emul_cycles;
            if (n.pes_per_mc > 0) config.pes_per_mc = n.pes_per_mc;
            if (n.ports_per_bank > 0) config.ports_per_bank = n.ports_per_bank;
            /* Device NoC model selection: same switch as the top-level noc parse.
             *
             * 1.11.56 (audit B058): the blanket per-level assignment now runs
             * only where the user did NOT set noc.levels[i].model. Setting
             * devices[].noc.model used to overwrite every per-tier choice
             * parsed from noc.levels, so a config that modelled one tier
             * DETAILED and the rest SIMPLE lost that the moment an unrelated
             * key was present -- and did not lose it when the key was absent,
             * since neither branch matches the "simple" default. */
            if (n.noc_model == "detailed") {
                config.noc_cycle_accurate = true;
                config.noc_mlp_model = 0;
                for (int i = 0; i < 7; ++i)
                    if (!config.network_level_model_user_set[i])
                        config.network_level_model[i] = "detailed";
            } else if (n.noc_model == "analytical") {
                config.noc_cycle_accurate = false;
                config.noc_mlp_model = 1;
                for (int i = 0; i < 7; ++i)
                    if (!config.network_level_model_user_set[i])
                        config.network_level_model[i] = "simple";
            }
            if (!n.noc_topology.empty()) {
                config.noc_topology = n.noc_topology;
                std::transform(config.noc_topology.begin(), config.noc_topology.end(),
                               config.noc_topology.begin(), ::toupper);
            }
            /* 1.11.57 (audit round 3, B009): re-derive the memory controller
             * and the M/D/1 bandwidth cap NOW, for the technology this device
             * actually has.
             *
             * getMemControllerConfig() runs once, hundreds of lines above,
             * against config.memory_tech -- which in system scope is still the
             * constructor default "SRAM" until the line at the top of this
             * block adopts the node's technology. So every co-simulation
             * derived its controller type and its M/D/1 service rate for an
             * SRAM device that does not exist. Measured on the shipped
             * host_device_basic.yaml (host DDR5, device DDR4, no SRAM
             * anywhere), the run's first bandwidth line read
             * "SRAM M/D/1 cap = ... 512000 MB/s". */
            getMemControllerConfig(config);
            computeHierarchyLatencies(config);
            break;  // first compute device defines the (single) device model for now
        }
    }

    // 1.11.56 (audit B005-B016): declare what this run assumes.
    reportStatedConstants(config);
    /* 1.11.56 (audit B042): the three bandwidth caps.
     * 1.11.57 (B002): the RECONCILIATION now runs inside
     * computeHierarchyLatencies, before the ladder consumes these numbers,
     * and gates whether the ladder is adopted at all. This call is gone
     * rather than duplicated -- it printed after the fact and changed
     * nothing. */

    // Synthesize system nodes from device/cosim config (backward compat)
    synthesizeSystemNodes(config);
    normalizeSystemConfig(config);

    // Explicit system-scope configs parse the host's core count into the HOST
    // system node (see line ~7052), but config.host_num_cores stays at its
    // default. Sync it so host-baseline parallelism -- OMP threads, MPI ranks,
    // and the devorg --pes injection below -- uses the REAL host core count
    // rather than the default (which silently capped multi-core baselines).
    if (config.scope == "system") {
        for (const auto& n : config.system_nodes) {
            if (n.role == UnifiedConfig::SystemNode::HOST && n.num_cores > 0) {
                config.host_num_cores = n.num_cores;
                break;
            }
        }
    }

    // Resolve the memory-topology knob (1.7.4): is_default_mem + host.mem.
    // TRUE  -> host tech = device tech by construction.
    // FALSE -> host.mem.technology drives host memory (required; else rc=1).
    if (resolveMemoryTopology(config) != 0) return 1;

    /* 1.11.60 (audit round 4, A007): the promised DRAM refusal, at the first
     * point where the memory technology is RESOLVED for both scopes (device
     * scope sets it at parse; system scope adopts the compute device's a few
     * hundred lines above). See the memory.array_pg note in the parse block
     * for why the comment's claim, not the code's silence, is the correct
     * behaviour. */
    if (config.mem_array_pg &&
        pimid::isDRAM(pimid::parseMemoryTechnology(config.memory_tech))) {
        std::cerr << "[config] FATAL: memory.array_pg is not applicable to "
                  << config.memory_tech
                  << ". Array power gating gates the array PERIPHERY while the "
                     "cells hold their state -- which a DRAM cell cannot do: it "
                     "is a capacitor, it must be refreshed, and refresh needs "
                     "the array powered. There is no DRAM state in which this "
                     "key means anything, and PIMID models none.\n"
                     "  The key applies to SRAM (periphery gates, the volatile "
                     "array keeps leaking) and to NVM (retention-free). For "
                     "DRAM the corresponding knob is memory.power_down, which "
                     "descends the idle controller to the JEDEC precharge "
                     "power-down state (IDD2P) and keeps refreshing."
                  << std::endl;
        return 1;
    }

    // Configure the characterization-cache warehouse ONCE, after CLI + YAML are
    // both parsed and before any backend (NVSim/CACTI/Ramulator) characterization.
    // Precedence (CLI > env > YAML > default) is resolved inside configure().
    pimid::cache::configure(cli_cache_mode, cli_cache_dir,
                            yaml_cache_mode, yaml_cache_dir);
    std::cout << "[pimid] cache mode: "
              << pimid::cache::modeName(pimid::cache::mode()) << std::endl;

    /* 1.11.51 (L75): validate the technology node UNCONDITIONALLY at config
     * load. The 1.11.2 positive-list ran only inside the power-analysis
     * paths, so --no-power / power.enabled:false silently accepted an
     * invalid node (e.g. 28 nm) that still shaped the run wherever timing
     * consumed it -- a swept study with power off plateaued instead of
     * failing at the invalid point. One authority, called once for every
     * node surface the config carries; power paths keep their own calls
     * (idempotent) so no path can regress to unvalidated. */
    validateTechNodeNm(config.tech_node_nm, "config load: technology.node_nm");

    /* 1.11.56 (audit B057): --print-mem-info, answered now that the config
     * has been read, at the run's real node and line size. */
    if (!print_mem_info_tech.empty()) {
        std::string mtech_up = canonicalMemTech(print_mem_info_tech);
        int lat_cy = std::max(1, getMemoryLatencyCycles(mtech_up, print_mem_info_freq,
                                                        config.tech_node_nm, false, 0.0,
                                                        -999, config.temperature_k,
                                                        config.cache_line_size));
        double bw_gbs = 12.8;
        bool bw_sourced = false;
        if (pimid::isDRAM(pimid::parseMemoryTechnology(mtech_up))) {
            try {
                pimid::RamulatorWrapper bw_query("", mtech_up);
                bw_query.initialize();
                bw_gbs = bw_query.getRankBandwidth();
                bw_sourced = true;
            } catch (...) {}
        }
        std::cout << "tech=" << mtech_up << " freq_mhz=" << print_mem_info_freq
                  << " node_nm=" << config.tech_node_nm
                  << " line_bytes=" << config.cache_line_size
                  << " access_latency_cycles=" << lat_cy
                  << " rank_bandwidth_GBs=" << bw_gbs
                  << (bw_sourced ? "" : " (bandwidth NOT tool-sourced: non-DRAM default)")
                  << std::endl;
        return 0;
    }

    if (config.host_tech_node_nm >= 0)
        validateTechNodeNm(config.host_tech_node_nm,
                           "config load: power.host_tech_node_nm");
    for (const auto& vn : config.system_nodes)
        validateTechNodeNm(vn.tech_node_nm,
                           (std::string("config load: node ") + vn.name).c_str());

    // Validate L2/L3 cache configuration
    if (config.l2_count < 1) config.l2_count = 1;
    if (config.enable_l2 && config.l2_count > 1) {
        if (config.num_pes % config.l2_count != 0) {
            std::cerr << "Error: num_pes (" << config.num_pes << ") must be divisible by l2.count ("
                      << config.l2_count << ")" << std::endl;
            return 1;
        }
        if (!config.enable_l3) {
            // No-unified-LLC mode: each L2 instance is a top-level cache.
            // No global coherence across L2 groups -- PEs within the same L2
            // are coherent, but cross-L2 accesses go straight to memory.
            std::cout << "Note: Clustered L2 (x" << config.l2_count
                      << ") without L3 -- no global coherence (each L2 is an independent LLC)." << std::endl;
        }
    }
    if (config.enable_l3 && !config.enable_l2) {
        std::cerr << "Error: L3 cache requires L2 to be enabled.\n"
                  << "  Set cache.l2.enabled: true in your YAML config." << std::endl;
        return 1;
    }

    // If workload_type is "mpi" but mpi_ranks not set, default to the parallel
    // worker count: host_num_cores for a host baseline (NO_OFFLOAD, system scope),
    // where ranks partition work across host cores; else num_pes (device offload
    // maps ranks -> PEs).
    if (config.workload_type == "mpi" && config.mpi_ranks <= 0) {
        bool host_baseline = (getenv("PIMID_COSIM_NO_OFFLOAD") != nullptr)
                             && (config.scope == "system");
        config.mpi_ranks = host_baseline ? config.host_num_cores : config.num_pes;
    }

    // Validate method
    if (config.method != "exec" && config.method != "trace-gen" &&
        config.method != "trace" && config.method != "synthetic") {
        std::cerr << "Error: Unknown simulation method: " << config.method << std::endl;
        std::cerr << "Valid methods: exec, trace, trace-gen, synthetic" << std::endl;
        return 1;
    }

    // -- Synthetic traffic mode (early exit -- no ZSim/workload needed) --
    if (config.method == "synthetic") {
#ifdef HAVE_GARNET
        int num_nodes = config.num_pes;
        int mesh_size = (int)std::sqrt(num_nodes);
        if (mesh_size * mesh_size < num_nodes) mesh_size++;

        std::string topo_str = config.noc_topology;
        std::string routing_str = config.noc_routing;
        if (routing_str.empty()) {
            if (topo_str == "MESH_2D")       routing_str = "XY";
            else if (topo_str == "TORUS_2D") routing_str = "DOR";
            else if (topo_str == "RING")     routing_str = "SHORTEST";
            else if (topo_str == "CROSSBAR") routing_str = "DIRECT";
            else if (topo_str == "FAT_TREE") routing_str = "NCA";
            else if (topo_str == "BUS")      routing_str = "DIRECT";
            else if (topo_str == "H_TREE")   routing_str = "NCA";
            else                             routing_str = "TABLE";
        }

        // Parse synthetic config from YAML
        int pattern = 0;
        double injRate = 0.1;
        uint64_t numPackets = 10000;
        int warmup = 1000;
        bool enablePower = config.enable_power;

        // Injection rate sweep: injection_rate_min/max/step
        double injRateMin = 0.0, injRateMax = 0.0, injRateStep = 0.0;
        bool doSweep = false;

        // Pattern name->id mapping
        static const std::pair<std::string, int> patternMap[] = {
            {"uniform", 0}, {"bit-complement", 1}, {"bitcomp", 1},
            {"tornado", 2}, {"neighbor", 3}, {"transpose", 4},
            {"bit-reverse", 5}, {"bitrev", 5}, {"bit-rotation", 6},
            {"bitrot", 6}, {"shuffle", 7},
            {"memory-directed", 8}, {"memdir", 8}
        };
        static const char* patternNames[] = {
            "uniform", "bit-complement", "tornado", "neighbor",
            "transpose", "bit-reverse", "bit-rotation", "shuffle",
            "memory-directed"
        };

        if (!config_file.empty()) {
            try {
                YAML::Node syn_cfg = YAML::LoadFile(config_file);
                if (syn_cfg["synthetic"]) {
                    auto syn = syn_cfg["synthetic"];
                    if (syn["pattern"]) {
                        std::string p = syn["pattern"].as<std::string>();
                        bool found = false;
                        for (const auto& pm : patternMap) {
                            if (p == pm.first) { pattern = pm.second; found = true; break; }
                        }
                        if (!found) {
                            std::cerr << "Warning: unknown pattern '" << p
                                      << "', using uniform. Valid: uniform, bit-complement, "
                                      << "tornado, neighbor, transpose, bit-reverse, "
                                      << "bit-rotation, shuffle" << std::endl;
                        }
                    }
                    if (syn["injection_rate"]) injRate = syn["injection_rate"].as<double>();
                    if (syn["packets"])        numPackets = syn["packets"].as<uint64_t>();
                    if (syn["warmup"])         warmup = syn["warmup"].as<int>();

                    // Injection rate sweep
                    if (syn["injection_rate_min"] && syn["injection_rate_max"]) {
                        injRateMin = syn["injection_rate_min"].as<double>();
                        injRateMax = syn["injection_rate_max"].as<double>();
                        injRateStep = syn["injection_rate_step"]
                            ? syn["injection_rate_step"].as<double>() : 0.02;
                        if (injRateMin > 0 && injRateMax > injRateMin && injRateStep > 0) {
                            doSweep = true;
                        }
                    }

                    if (syn["power"]) enablePower = syn["power"].as<bool>();
                }
            } catch (const std::exception& e) {
                /* 1.11.56 (audit B029): this catch wrapped the WHOLE synthetic
                 * block, so one malformed key discarded pattern, packets,
                 * injection rate and the sweep together -- and the run went on
                 * with the built-in defaults, reporting a synthetic experiment
                 * nobody asked for under the name of the one they did. */
                std::cerr << "[config] FATAL: the synthetic: block in "
                          << config_file << " could not be read (" << e.what()
                          << "). Refusing to run the built-in defaults under "
                             "the name of the requested experiment." << std::endl;
                std::exit(2);
            }
        }

        std::cout << "\n========================================" << std::endl;
        std::cout << "Synthetic Traffic Test" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "  Topology:       " << topo_str << std::endl;
        std::cout << "  Routing:        " << routing_str << std::endl;
        std::cout << "  Nodes:          " << (mesh_size * mesh_size) << std::endl;
        std::cout << "  Pattern:        " << patternNames[pattern] << std::endl;
        if (doSweep) {
            std::cout << "  Injection rate: " << injRateMin << " -> " << injRateMax
                      << " (step " << injRateStep << ")" << std::endl;
        } else {
            std::cout << "  Injection rate: " << injRate
                      << " flits/node/cycle" << std::endl;
        }
        std::cout << "  Packets:        " << numPackets << std::endl;
        std::cout << "  Warmup:         " << warmup << std::endl;
        std::cout << "  Power analysis: " << (enablePower ? "yes" : "no") << std::endl;
        std::cout << "========================================" << std::endl;

        // Build list of injection rates to test
        std::vector<double> rates;
        if (doSweep) {
            for (double r = injRateMin; r <= injRateMax + 1e-9; r += injRateStep)
                rates.push_back(r);
        } else {
            rates.push_back(injRate);
        }

        // Lambda: run one synthetic test and optionally compute NoC power
        auto runOneSynthetic = [&](double rate) -> int {
            SyntheticTrafficResult result;
            int rc = zsim_synthetic_traffic_ex(
                topo_str.c_str(), routing_str.c_str(),
                (uint32_t)mesh_size, (uint32_t)mesh_size,
                pattern, rate, numPackets, warmup,
                (uint32_t)config.noc_router_latency,
                (uint32_t)config.noc_link_latency,
                (uint32_t)config.noc_vcs_per_vnet,
                (uint32_t)config.noc_buffers_per_vc,
                (double)config.frequency_mhz, 128, &result);

            // Print latency/throughput summary
            if (!doSweep) {
                printf("\n-- Synthetic Traffic Results --\n");
                printf("  Delivered:      %lu packets\n", result.totalPackets);
                printf("  Total cycles:   %lu\n", result.totalCycles);
                printf("  Avg latency:    %.1f cycles\n", result.avgLatency);
                printf("  Min latency:    %lu cycles\n", result.minLatency);
                printf("  Max latency:    %lu cycles\n", result.maxLatency);
                printf("  Throughput:     %.6f flits/node/cycle\n", result.throughput);
            }

            // -- NoC power analysis via McPAT --
            if (enablePower && result.totalPackets > 0) {
                using McPAT = pimid::McPATWrapper;

                // Minimal system config -- NoC-only power
                McPAT::SystemConfig mcfg;
                mcfg.num_cores = 1;  // McPAT needs >=1 core
                mcfg.core_clock_mhz = result.clockMhz;
                mcfg.pipeline_depth = 5;
                mcfg.issue_width = 1;
                mcfg.num_alus = 1;
                mcfg.num_muls = 0;
                mcfg.num_fpus = 0;
                mcfg.l1i_size_bytes = 0;
                mcfg.l1d_size_bytes = 0;
                mcfg.l2_size_bytes = 0;
                mcfg.l3_size_bytes = 0;
                mcfg.num_memory_controllers = 0;
                mcfg.mc_clock_mhz = result.clockMhz / 2.0;
                mcfg.tech_node_nm = validateTechNodeNm(config.tech_node_nm, "device NoC probe");
                /* 1.11.28 (E3), corrected 1.11.57 (audit round 3, A010):
                 * the temperature surface DOES exist now. The note this
                 * replaces said "UnifiedConfig carries no temperature surface
                 * today, so there is nothing to inherit" and promised that
                 * both paths would pick one up if it were ever added. 1.11.52
                 * (D055) added power.temperature_k/_c -- and this probe kept
                 * pricing at McPAT's 350 K default while the device path
                 * beside it moved. At power.temperature_k: 400 the two read
                 * CACTI leakage rows 50 K apart for one machine. */
                mcfg.temperature_k = config.temperature_k;
                mcfg.has_noc = true;
                // 1.9.32: a synthetic probe of the IN-MEMORY network.
                mcfg.device_scope = true;
                /* 1.11.28 (user ruling E3): price this fabric as the silicon it
                 * SITS ON. The probe declared device_scope and then defaulted to
                 * family 0, so the in-memory network was priced in a logic
                 * process while the PEs on the same die priced as DRAM-periphery
                 * -- one fabric, two processes, in one run. The family decision
                 * now has a single owner (applyProcessFamily) that both paths
                 * call, so they cannot drift again.
                 * NOTE this became safe only with 1.11.22: the factor is a ratio
                 * across two tables, and until both ends were named a family-1
                 * probe at tech_node_nm would have been mispriced. */
                applyProcessFamily(mcfg, config, config.pe_hierarchy_level,
                                   /*quiet=*/true);

                // Map topology to McPAT type
                int mcpat_noc_type = (topo_str == "BUS") ? 0 : 1;

                mcfg.noc_topology = mcpat_noc_type;
                mcfg.noc_num_routers = result.numRouters;
                mcfg.noc_num_rows = result.numRows;
                mcfg.noc_num_cols = result.numCols;
                mcfg.noc_flit_size_bits = result.flitSizeBits;
                mcfg.noc_clock_mhz = result.clockMhz;
                mcfg.noc_input_ports = (mcpat_noc_type == 0) ? (int)result.numNodes : 5;
                mcfg.noc_output_ports = (mcpat_noc_type == 0) ? (int)result.numNodes : 5;
                mcfg.noc_vcs_per_vnet = (int)config.noc_vcs_per_vnet;
                mcfg.noc_vc_buffer_size = (int)config.noc_buffers_per_vc;

                // Apply mcpat_overrides
                auto ov_get_int = [&](const std::string& key, int fallback) -> int {
                    auto it = config.mcpat_overrides.find(key);
                    return (it != config.mcpat_overrides.end()) ? (int)it->second : fallback;
                };
                /* 1.11.49: same pattern as the per-node site's clobber fix.
                 * Here nothing sets mcfg.device_type before this line today
                 * (applyProcessFamily touches only process_family), so this is
                 * behavior-preserving -- it just stops the literal-0 default
                 * from silently wiping any future upstream assignment. */
                mcfg.device_type = ov_get_int("device_type", mcfg.device_type);
                mcfg.longer_channel_device = ov_get_int("longer_channel_device", 1);
                mcfg.interconnect_projection_type =
        ov_get_int("interconnect_projection_type", config.interconnect_projection);  // 1.11.30 E5

                McPAT mcpat(mcfg);
                mcpat.setDeviceProfile(McPAT::DeviceProfile::DEVICE_ALU);
                mcpat.initialize();

                mcpat.setTotalCycles(result.totalCycles > 0 ? result.totalCycles : 1);
                mcpat.setBusyCycles(result.totalCycles > 0 ? result.totalCycles : 1);
                mcpat.setTotalInstructions(1);

                // Single flat NoC level with synthetic traffic stats
                McPAT::NoCLevelConfig nc;
                nc.name = "NoC_synthetic";
                nc.type = mcpat_noc_type;
                nc.horizontal_nodes = result.numRows;
                nc.vertical_nodes = result.numCols;
                nc.input_ports = mcfg.noc_input_ports;
                nc.output_ports = mcfg.noc_output_ports;
                nc.flit_bits = result.flitSizeBits;
                nc.clock_mhz = result.clockMhz;
                nc.chip_coverage = 1.0;
                nc.total_accesses = result.totalPackets;
                // duty_cycle = fraction of peak bandwidth used [0,1]
                // Peak = N nodes x 1 packet/cycle, so duty = packets / (cycles x N)
                nc.duty_cycle = (result.totalCycles > 0 && result.numNodes > 0)
                    ? (double)result.totalPackets / (result.totalCycles * result.numNodes)
                    : 0.0;
                /* 1.11.52 (audit C011): the in-memory NoC probe describes the
                 * fabric at the PE's placement, so its die membership is the
                 * family answer applyProcessFamily just computed -- not the
                 * struct default of 1. */
                nc.on_dram_die = (mcfg.process_family == 1) ? 1 : 0;
                mcpat.setNoCLevels({nc});

                try {
                    mcpat.computePower();
                    auto noc_power = mcpat.getComponentPower(McPAT::ComponentType::NOC);
                    double noc_area = mcpat.getComponentArea(McPAT::ComponentType::NOC);

                    double time_s = result.totalCycles / (result.clockMhz * 1e6);
                    double noc_energy_nj = noc_power.total_power * time_s * 1e9;
                    double energy_per_pkt_pj = (result.totalPackets > 0)
                        ? (noc_energy_nj * 1000.0) / result.totalPackets : 0.0;

                    if (!doSweep) {
                        /* 1.11.52: name the node actually PRICED (the family
                         * pin may have overridden the config knob). */
                        printf("\n-- NoC Power Analysis (McPAT, %dnm) --\n", mcfg.tech_node_nm);
                        printf("  Leakage power:    %.4f W (sub=%.4f, gate=%.4f)\n",
                               noc_power.total_leakage, noc_power.subthreshold_leakage,
                               noc_power.gate_leakage);
                        printf("  Dynamic power:    %.4f W\n", noc_power.runtime_dynamic);
                        printf("  Total power:      %.4f W\n", noc_power.total_power);
                        printf("  NoC area:         %.3f mm^2\n", noc_area);
                        printf("  Total energy:     %.2f nJ\n", noc_energy_nj);
                        printf("  Energy/packet:    %.2f pJ\n", energy_per_pkt_pj);
                    } else {
                        // Sweep table row
                        printf("  rate=%.3f  lat=%.1f  tput=%.6f  "
                               "pwr=%.4fW (leak=%.4f dyn=%.4f)  "
                               "E/pkt=%.1fpJ\n",
                               rate, result.avgLatency, result.throughput,
                               noc_power.total_power, noc_power.total_leakage,
                               noc_power.runtime_dynamic, energy_per_pkt_pj);
                    }
                } catch (const std::exception& e) {
                    if (!doSweep)
                        std::cerr << "[Power] McPAT failed: " << e.what() << std::endl;
                }
            } else if (!doSweep) {
                // No power, print basic results
            }

            if (doSweep && !enablePower) {
                printf("  rate=%.3f  lat=%.1f  tput=%.6f\n",
                       rate, result.avgLatency, result.throughput);
            }

            return rc;
        };

        // Run sweep or single test
        int final_rc = 0;
        if (doSweep) {
            printf("\n-- Injection Rate Sweep --\n");
            if (enablePower)
                printf("  %-8s %-8s %-12s %-10s %-10s %-10s %-10s\n",
                       "Rate", "Lat", "Throughput", "Power(W)", "Leak(W)", "Dyn(W)", "E/pkt(pJ)");
            else
                printf("  %-8s %-8s %-12s\n", "Rate", "Lat", "Throughput");
        }

        for (double rate : rates) {
            int rc = runOneSynthetic(rate);
            if (rc != 0) final_rc = rc;
        }

        std::cout << "========================================" << std::endl;
        return final_rc;
#else
        std::cerr << "Error: synthetic mode requires Garnet" << std::endl;
        return 1;
#endif
    }

    // Validate trace arguments based on method
    if (config.method == "trace-gen") {
        if (config.trace_file.empty()) {
            std::cerr << "Error: trace-gen mode requires --trace-file to specify output path" << std::endl;
            return 1;
        }
    } else if (config.method == "trace") {
        if (config.trace_file.empty()) {
            std::cerr << "Error: trace mode requires --trace-file to specify input trace path" << std::endl;
            return 1;
        }
    } else if (config.method == "exec") {
        if (config.workload_binary.empty()) {
            // For system scope, per-node workloads may cover all cores
            bool all_nodes_have_workload = false;
            if (config.scope == "system" && !config.system_nodes.empty()) {
                all_nodes_have_workload = true;
                for (const auto& n : config.system_nodes) {
                    if (n.num_cores > 0 && n.workload_binary.empty()) {
                        all_nodes_have_workload = false;
                        break;
                    }
                }
            }
            if (!all_nodes_have_workload) {
                std::cerr << "Error: exec mode requires --workload to specify a binary" << std::endl;
                return 1;
            }
        }
    }

    // Resolve + validate workload binaries. Paths are tried (1) CWD-relative,
    // (2) relative to the PIMID root (exe location or $PIMID_ROOT) -- so configs
    // shipping root-relative paths (benchmarks/...) work from ANY directory --
    // and (3) with a legacy "pimid/" dev-space prefix stripped, for old configs.
    auto resolveWorkload = [](std::string& path) {
        if (path.empty() || access(path.c_str(), X_OK) == 0) return;
        std::string root = getPimidRoot();
        std::string alt = root + "/" + path;
        if (access(alt.c_str(), X_OK) == 0) { path = alt; return; }
        if (path.rfind("pimid/", 0) == 0) {
            std::string alt2 = root + "/" + path.substr(6);
            if (access(alt2.c_str(), X_OK) == 0) { path = alt2; return; }
        }
    };
    resolveWorkload(config.workload_binary);
    for (auto& n : config.system_nodes) resolveWorkload(n.workload_binary);

    // Device-organization awareness for benchmarks: pass the placement geometry
    // the simulator uses (placement level, PE count, unit count, pages-per-unit)
    // to the workload on its command line, so a device-org-aware kernel can
    // EXPLICITLY relocate each PE's working set into its own unit (near-data).
    // total_units mirrors the emit-side formula (SUBARRAY => banks*subarrays,
    // else banks). Unknown to legacy kernels -> harmlessly ignored.
    if (!config.workload_binary.empty()) {
        // Pass the SAME org count the coverage/addrToUnit use (total_mem_orgs), so
        // the benchmark's per-PE slots land on the PEs' (distant/strided) home units.
        int devorg_total_units = (config.total_mem_orgs > 0)
            ? config.total_mem_orgs
            : (config.pe_hierarchy_level == 0
               ? config.num_banks * config.subarrays_per_bank : config.num_banks);
        // Host baseline (NO_OFFLOAD, system scope) partitions work across host cores,
        // not device PEs -- the MPI kernel's ranks==pes guard must see host_num_cores.
        int devorg_pes = ((getenv("PIMID_COSIM_NO_OFFLOAD") != nullptr) && config.scope == "system")
                         ? config.host_num_cores : config.num_pes;
        auto add_devorg = [&](std::vector<std::string>& args) {
            args.push_back("--placement");     args.push_back(config.placement_level);
            args.push_back("--pes");           args.push_back(std::to_string(devorg_pes));
            args.push_back("--total-units");   args.push_back(std::to_string(devorg_total_units));
            args.push_back("--pages-per-unit");args.push_back(std::to_string(config.pages_per_unit));
        };
        add_devorg(config.workload_args);
        for (auto& n : config.system_nodes)
            if (!n.workload_binary.empty()) add_devorg(n.workload_args);
    }
    if (!config.workload_binary.empty() && config.method != "trace") {
        if (access(config.workload_binary.c_str(), X_OK) != 0) {
            std::cerr << "Warning: workload binary '" << config.workload_binary
                      << "' not found or not executable" << std::endl;
        }
    }

    if (config.scope != "device" && config.scope != "system") {
        std::cerr << "Error: Unknown simulation scope: " << config.scope << std::endl;
        std::cerr << "Valid scopes: device, system (cosim accepted as deprecated alias for system)" << std::endl;
        return 1;
    }

    // Print simulation configuration
    std::cout << "+==============================================================================+" << std::endl;
    std::cout << "|                    PIMID Simulation Infrastructure v1.2.2                      |" << std::endl;
    std::cout << "+==============================================================================+" << std::endl;
    std::cout << "  Method: " << config.method;
    if (config.method == "exec") {
        std::cout << " (QEMU + ZSim cycle-accurate)";
    } else if (config.method == "trace") {
        std::cout << " (replay)";
    } else if (config.method == "trace-gen") {
        std::cout << " (QEMU trace recording)";
    }
    std::cout << " | Scope: " << config.scope << std::endl;
    if (!config_file.empty()) {
        std::cout << "  Config: " << config_file << std::endl;
    }
    if (!config.workload_binary.empty()) {
        std::cout << "  Workload: " << config.workload_binary;
        if (config.workload_type == "mpi") {
            std::cout << " (MPI, " << config.mpi_ranks << " ranks)";
        } else if (config.workload_type != "serial") {
            std::cout << " (" << config.workload_type << ")";
        }
        std::cout << std::endl;
    }
    std::cout << "  Power: " << (config.enable_power ? "enabled" : "disabled") << std::endl;
    if (!config.trace_file.empty()) {
        std::cout << "  Trace: " << config.trace_file;
        if (config.method == "trace") {
            std::cout << " (input)";
        } else {
            std::cout << " (output)";
        }
        std::cout << std::endl;
    }
    std::cout << std::endl;

    // Run appropriate simulator based on method and scope
    bool success = false;

    if (config.scope == "device") {
        // Device-only simulation (PIM only)
        if (config.method == "trace") {
            // -- Trace Replay (ZSim cycle-accurate) --
            // Replays trace through full ZSim hierarchy (cores, caches, MCs, NoC)
            if (config_file.empty()) {
                std::cerr << "Error: --config FILE required for trace simulation" << std::endl;
                return 1;
            }

            // Determine core type (display label)
            std::string core_type = "Simple";
            if (config.pe_type == "ooo_core") {
                core_type = "OoO";
            } else if (config.pe_type == "in_order_core") {
                core_type = "InOrder";
            } else if (config.pe_type == "alu_core" || config.pe_type == "alu") {
                core_type = "ALU";
            } else if (config.pe_type == "null_core" || config.pe_type == "null") {
                core_type = "Null";
            }

            // Generate ZSim config (reuse QEMU config generator)
            ZSimSimulator zsim_helper(config);
            std::string zsim_cfg_path = zsim_helper.generateConfig();
            if (zsim_cfg_path.empty()) {
                std::cerr << "Error: Failed to generate ZSim config" << std::endl;
                return 1;
            }

            // Output directory
            std::string output_dir = "/tmp/pimid_trace_zsim_" + std::to_string(getpid());
            mkdir(output_dir.c_str(), 0755);

            // Banner
            std::cout << "\n========================================" << std::endl;
            std::cout << "Trace Replay (ZSim cycle-accurate)" << std::endl;
            std::cout << "========================================" << std::endl;
            std::cout << "  Trace:     " << config.trace_file << std::endl;
            if (core_type == "ALU") {
                std::cout << "  Core:      ALU (compute=" << std::fixed << std::setprecision(2)
                          << config.alu_compute_factor << ", access=" << config.alu_access_factor
                          << ", throughput=" << config.alu_throughput_factor
                          << ", energy=" << config.alu_energy_factor << std::defaultfloat
                          << ", width=" << config.alu_operand_width << "b)" << std::endl;
            } else {
                std::cout << "  Core:      " << core_type << std::endl;
            }

            // Display cache configuration with latencies (trace mode banner)
            if (core_type != "ALU") {
                std::string cache_src = config.use_yaml_cache_params ? "YAML override" : "CACTI " + std::to_string(std::max(22, config.tech_node_nm)) + "nm";
                int disp_l1d, disp_l1i, disp_l2 = 0, disp_l3 = 0;
                if (config.use_yaml_cache_params) {
                    disp_l1d = std::max(1, static_cast<int>(std::round(
                        config.l1d_params.latency_ns * config.frequency_mhz / 1000.0)));
                    disp_l1i = std::max(1, static_cast<int>(std::round(
                        config.l1i_params.latency_ns * config.frequency_mhz / 1000.0)));
                    if (config.enable_l2)
                        disp_l2 = std::max(1, static_cast<int>(std::round(
                            config.l2_params.latency_ns * config.frequency_mhz / 1000.0)));
                    if (config.enable_l3)
                        disp_l3 = std::max(1, static_cast<int>(std::round(
                            config.l3_params.latency_ns * config.frequency_mhz / 1000.0)));
                } else {
                    disp_l1d = getCacheLatencyCycles(config.l1d_size_kb, config.l1d_ways,
                                                      config.cache_line_size, config.frequency_mhz, config.tech_node_nm, /*fallback_cycles=*/4);
                    disp_l1i = getCacheLatencyCycles(config.l1i_size_kb, config.l1i_ways,
                                                      config.cache_line_size, config.frequency_mhz, config.tech_node_nm, /*fallback_cycles=*/3);
                    if (config.enable_l2)
                        disp_l2 = getCacheLatencyCycles(config.l2_size_kb, config.l2_ways,
                                                         config.cache_line_size, config.frequency_mhz, config.tech_node_nm, /*fallback_cycles=*/12);
                    if (config.enable_l3)
                        disp_l3 = getCacheLatencyCycles(config.l3_size_kb, config.l3_ways,
                                                         config.cache_line_size, config.frequency_mhz, config.tech_node_nm, /*fallback_cycles=*/20);
                    /* 1.11.57 (latent B020): the banner claimed CACTI for
                     * these numbers whatever happened inside the query. If
                     * any of them is a substituted literal, say so here --
                     * the provenance line is where a reader looks. */
                    if (cacheLatencyFallbackCount() > 0)
                        cache_src = "DECLARED FALLBACK cycles, CACTI unavailable";
                }
                std::cout << "  L1D:       " << config.l1d_size_kb << "KB, "
                          << config.l1d_ways << "-way, " << disp_l1d
                          << (disp_l1d == 1 ? " cycle" : " cycles")
                          << " (" << cache_src << ")" << std::endl;
                std::cout << "  L1I:       " << config.l1i_size_kb << "KB, "
                          << config.l1i_ways << "-way, " << disp_l1i
                          << (disp_l1i == 1 ? " cycle" : " cycles")
                          << " (" << cache_src << ")" << std::endl;
                if (config.enable_l2) {
                    std::cout << "  L2:        " << config.l2_size_kb << "KB, "
                              << config.l2_ways << "-way, " << disp_l2
                              << (disp_l2 == 1 ? " cycle" : " cycles");
                    if (config.l2_count > 1) {
                        std::cout << " (x" << config.l2_count << ", "
                                  << (config.num_pes / config.l2_count) << " PEs/L2";
                        if (!config.enable_l3)
                            std::cout << ", independent LLCs";
                        std::cout << ")";
                    }
                    std::cout << " (" << cache_src << ")" << std::endl;
                }
                if (config.enable_l3) {
                    std::cout << "  L3:        " << config.l3_size_kb << "KB, "
                              << config.l3_ways << "-way, " << disp_l3
                              << (disp_l3 == 1 ? " cycle" : " cycles")
                              << " (unified LLC, " << cache_src << ")" << std::endl;
                }
            }

            // Pre-compute memory latency (may trigger CACTI/NVSim/Ramulator output)
            int disp_latency = 0;
            if (config.zsim_mem_controller_type != "ramulator") {
                disp_latency = (config.memory_latency_override >= 0) ?
                    config.memory_latency_override :
                    getMemoryLatencyCycles(config.memory_tech, config.frequency_mhz,
                                           config.tech_node_nm,
                                           config.use_yaml_memory_params,
                                           config.memory_params.read_latency_ns,
                                           -999, config.temperature_k,
                                           config.cache_line_size);  // 1.11.56 (B018)
            }
            // Display memory controller configuration
            std::cout << "  Memory:    " << config.memory_tech << ", Controller: ";
            if (config.zsim_mem_controller_type == "ramulator") {
                std::cout << "Ramulator (Ramulator2 cycle-accurate)" << std::endl;
            } else if (config.zsim_mem_controller_type == "weavesimple") {
                std::cout << "WeaveSimple (phase-aware M/D/1), " << disp_latency << " cycles, "
                          << config.md1_bandwidth_mbs << " MB/s" << std::endl;
            } else {
                std::cout << "Simple (M/D/1), " << disp_latency << " cycles, "
                          << config.md1_bandwidth_mbs << " MB/s" << std::endl;
            }

            // Display NoC configuration
            std::cout << "  NoC:       " << config.noc_topology;
            if (config.noc_topology == "MESH_2D" || config.noc_topology == "TORUS_2D") {
                int mesh_size = static_cast<int>(std::sqrt(config.num_pes));
                if (mesh_size * mesh_size < config.num_pes) mesh_size++;
                std::cout << " (" << mesh_size << "x" << mesh_size << ")";
            } else if (config.noc_topology == "RING") {
                std::cout << " (" << (config.noc_ring_unidirectional ? "unidirectional" : "bidirectional") << ")";
            }
            {
                std::string disp_routing = config.noc_routing;
                if (disp_routing.empty()) {
                    if (config.noc_topology == "MESH_2D")       disp_routing = "XY";
                    else if (config.noc_topology == "TORUS_2D")  disp_routing = "DOR";
                    else if (config.noc_topology == "RING")      disp_routing = "SHORTEST";
                    else if (config.noc_topology == "CROSSBAR")  disp_routing = "DIRECT";
                    else if (config.noc_topology == "FAT_TREE")  disp_routing = "NCA";
                    else if (config.noc_topology == "BUS")       disp_routing = "DIRECT";
                    else if (config.noc_topology == "H_TREE")    disp_routing = "NCA";
                    else if (config.noc_topology == "CUSTOM")    disp_routing = "TABLE";
                    else                                          disp_routing = "XY";
                }
                std::cout << ", routing=" << disp_routing
                          << ", mode=" << (config.noc_cycle_accurate ? "detailed" : "simple") << std::endl;
            }

            // Display hierarchy info
            if (config.hierarchy_enabled) {
                const std::array<std::string, 7> level_names = {
                    "Subarray", "Bank", "BankGroup", "Chip", "Rank", "Channel", "System"
                };
                std::cout << "  Hierarchy: " << config.memory_tech << " internal network" << std::endl;
                if (config.placement_level != "HOST_MC") {
                    for (int i = 0; i < 7; ++i) {
                        int lat = config.hierarchy_level_latency[i];
                        int brlat = (i < 6) ? config.hierarchy_bridge_latency[i] : 0;
                        if (lat == 0 && brlat == 0) continue;  // skip passthrough levels
                        std::cout << "    L" << i << " " << std::setw(12) << std::left
                                  << (level_names[i] + ":") << " " << lat << " cycles";
                        if (i < 6 && brlat > 0) {
                            std::cout << " + bridge " << brlat << " cy";
                            const auto& bmodel = config.hierarchy_bridge_model[i];
                            if (!bmodel.empty() && bmodel != "auto")
                                std::cout << " [" << bmodel << "]";
                        }
                        std::cout << std::endl;
                    }
                }
                // PE placement with M:N mapping info
                if (config.placement_level == "HOST_MC") {
                    std::cout << "    PE placement: HOST_MC (" << config.num_pes << " PEs share host MC)" << std::endl;
                    std::cout << "    PE-MIs: none (PEs share host MC)" << std::endl;
                } else {
                    std::cout << "    PE placement: L" << config.pe_hierarchy_level
                              << " (" << config.placement_level << "), "
                              << config.num_pes << " PEs -> " << config.total_mem_orgs << " mem orgs";
                    // Compute and display ratio
                    if (config.total_mem_orgs > 0 && config.num_pes > 0) {
                        int g = std::gcd(config.num_pes, config.total_mem_orgs);
                        std::cout << " (" << (config.num_pes / g) << ":"
                                  << (config.total_mem_orgs / g) << ", "
                                  << (config.pe_mem_connection == UnifiedConfig::PEMemConnectionMode::SHARED_IO
                                      ? "shared_io" : "separate_endpoints") << ")";
                    }
                    std::cout << std::endl;
                    if (config.pe_mem_connection == UnifiedConfig::PEMemConnectionMode::SEPARATE_ENDPOINTS) {
                        std::cout << "    Endpoints: " << config.total_network_endpoints
                                  << " (PEs=" << config.num_pes << " + mem_orgs=" << config.total_mem_orgs
                                  << "), local_link=" << config.local_link_latency << " cy" << std::endl;
                    }

                    // PE-MI info (always present for non-HOST_MC due to validation)
                    if (config.pe_mc_enabled) {
                        int mi_count = peMemoryInterfaceCount(config);  // 1.11.60 (A012)
                        std::cout << "    PE-MIs: " << mi_count
                                  << " (" << config.pes_per_mc << " PEs/MI";
                        if (config.ports_per_bank > 1)
                            std::cout << ", " << config.ports_per_bank << " ports/bank";
                        std::cout << ")";
                        if (config.pe_mc_local_latency > 0)
                            std::cout << ", local=" << config.pe_mc_local_latency << " cy";
                        if (config.pe_mc_bandwidth_mbs > 0)
                            std::cout << ", bw=" << config.pe_mc_bandwidth_mbs << " MB/s";
                        std::cout << std::endl;
                    }
                }

                // PCIe/CXL interconnect info
                if (config.pcie_timing_configured && config.pcie_enabled) {
                    std::cout << "    Interconnect: " << config.pcie_link_type
                              << " x" << config.pcie_num_lanes
                              << " (base=" << std::fixed << std::setprecision(0)
                              << config.pcie_base_latency_ns << "ns, BW="
                              << std::setprecision(1) << config.pcie_bandwidth_GBs
                              << " GB/s, model=" << config.pcie_model
                              << ")" << std::defaultfloat << std::endl;
                }
            }

            std::cout << "  ZSim config: " << zsim_cfg_path << std::endl;
            std::cout << "  Output dir:  " << output_dir << std::endl;
            std::cout << std::endl;

            // fork/exec: zsim_trace <config.cfg> <trace.pimtrace> <output_dir>
            std::cout << "Running trace through ZSim simulation..." << std::endl;
            std::cout << "----------------------------------------" << std::endl;

            int trace_result = zsim_trace_run(zsim_cfg_path.c_str(),
                                              config.trace_file.c_str(),
                                              output_dir.c_str());

            std::cout << "----------------------------------------" << std::endl;

            if (trace_result == 0) {
                // Parse zsim_trace_stats.txt for summary
                std::string stats_path = output_dir + "/zsim_trace_stats.txt";
                std::ifstream stats_file(stats_path);
                if (stats_file.is_open()) {
                    std::cout << "\nZSim Trace Results:" << std::endl;
                    std::string line;
                    while (std::getline(stats_file, line)) {
                        std::cout << "  " << line << std::endl;
                    }
                }

                // Also parse zsim.out for cache stats
                std::string zsim_out_path = output_dir + "/zsim.out";
                ZSimParsedOutput zsim_stats = parseZSimOutputFile(zsim_out_path);
                if (zsim_stats.cycles > 0) {
                    std::cout << "\nCache Statistics:" << std::endl;
                    if (zsim_stats.l1d_hits() + zsim_stats.l1d_misses() > 0) {
                        double l1d_rate = 100.0 * zsim_stats.l1d_hits() /
                            (zsim_stats.l1d_hits() + zsim_stats.l1d_misses());
                        std::cout << "  L1D hit rate: " << std::fixed << std::setprecision(1)
                                  << l1d_rate << "%" << std::endl;
                    }
                    if (zsim_stats.l1i_hits() + zsim_stats.l1i_misses() > 0) {
                        double l1i_rate = 100.0 * zsim_stats.l1i_hits() /
                            (zsim_stats.l1i_hits() + zsim_stats.l1i_misses());
                        std::cout << "  L1I hit rate: " << std::fixed << std::setprecision(1)
                                  << l1i_rate << "%" << std::endl;
                    }
                    if (zsim_stats.l2_hits() + zsim_stats.l2_misses() > 0) {
                        double l2_rate = 100.0 * zsim_stats.l2_hits() /
                            (zsim_stats.l2_hits() + zsim_stats.l2_misses());
                        std::cout << "  L2 hit rate:  " << std::fixed << std::setprecision(1)
                                  << l2_rate << "%" << std::endl;
                    }
                }

                // Power analysis
                if (config.enable_power) {
                    runPowerAnalysis(config, zsim_stats, output_dir);
                } else {
                    /* 1.11.17: the FP-without-FPU timing charge fires with
                     * or without power analysis -- the contradiction report
                     * must too. */
                    reportFpWithoutFpu(config, zsim_stats);
                }
            }

            std::cout << "\n========================================" << std::endl;
            success = (trace_result == 0);
        } else if (config.method == "trace-gen") {
            // -- QEMU Trace Generation --
            // Uses QEMU user-mode emulation with libpimid_trace.so plugin
            // to generate PIMID-format binary traces
            if (config.workload_binary.empty()) {
                std::cerr << "Error: --workload BINARY required for qemu-trace mode" << std::endl;
                return 1;
            }

            std::cout << "\n========================================" << std::endl;
            std::cout << "QEMU Trace Generation" << std::endl;
            std::cout << "========================================" << std::endl;
            std::cout << "Workload: " << config.workload_binary << std::endl;
            std::cout << "Output:   " << config.trace_file << std::endl;

            // Find qemu-x86_64 binary
            std::string qemu_binary = findQemuBinary();
            if (qemu_binary.empty()) {
                std::cerr << "Error: qemu-x86_64 not found" << std::endl;
                std::cerr << "Install with: sudo apt install qemu-user" << std::endl;
                std::cerr << "Note: qemu-user-static does NOT support -plugin" << std::endl;
                return 1;
            }
            std::cout << "Using QEMU: " << qemu_binary << std::endl;

            // Find libpimid_trace.so plugin
            std::string plugin_path = findQemuPlugin("libpimid_trace.so");
            if (plugin_path.empty()) {
                std::cerr << "Error: libpimid_trace.so not found" << std::endl;
                std::cerr << "Build with: cmake --build . --target pimid_trace" << std::endl;
                return 1;
            }
            std::cout << "Plugin:   " << plugin_path << std::endl;
            std::cout << std::endl;

            if (config.workload_type == "mpi" && config.mpi_ranks > 0) {
                // -- MPI path: per-rank QEMU trace generation via mpirun --
                int ranks = config.mpi_ranks;
                std::string output_base = "/tmp/pimid_qemu_trace_" + std::to_string(getpid());
                mkdir(output_base.c_str(), 0755);

                std::cout << "MPI Mode: " << ranks << " ranks" << std::endl;
                std::cout << "Output directory: " << output_base << std::endl;

                // Create per-rank directories
                for (int rank = 0; rank < ranks; rank++) {
                    std::string rank_dir = output_base + "/rank" + std::to_string(rank);
                    mkdir(rank_dir.c_str(), 0755);
                }

                // Extract base filename components from trace_file
                std::string trace_basename = config.trace_file;
                size_t slash_pos = trace_basename.rfind('/');
                if (slash_pos != std::string::npos) {
                    trace_basename = trace_basename.substr(slash_pos + 1);
                }
                std::string trace_name = trace_basename;
                std::string trace_ext;
                size_t dot_pos = trace_basename.rfind('.');
                if (dot_pos != std::string::npos) {
                    trace_name = trace_basename.substr(0, dot_pos);
                    trace_ext = trace_basename.substr(dot_pos);
                }

                // Find libpimid_mpi.so (PIMID's in-scope shm-mailbox MPI transport)
                std::string pimid_mpi_lib = findPimidMpiLib();
                if (pimid_mpi_lib.empty()) {
                    std::cerr << "Error: libpimid_mpi.so not found (searched build dirs)" << std::endl;
                    std::cerr << "Build pimid_mpi target before using --mpi-ranks" << std::endl;
                    return 1;
                }

                // Unique shm name per launch so concurrent pimid invocations don't collide
                std::string shm_name = "/pimid_mpi_" + std::to_string(getpid());

                std::cout << "MPI transport: libpimid_mpi.so (shm=" << shm_name << ")" << std::endl;
                std::cout << "Launching " << ranks << " ranks (PIMID-managed multi-process)" << std::endl;
                std::cout << "----------------------------------------" << std::endl;

                // Fork N children; each becomes a QEMU+plugin process for one MPI rank
                std::vector<pid_t> child_pids;
                child_pids.reserve(ranks);
                for (int rank = 0; rank < ranks; rank++) {
                    pid_t pid = fork();
                    if (pid < 0) {
                        std::cerr << "Error: fork() failed for rank " << rank << ": "
                                  << strerror(errno) << std::endl;
                        // Reap any already-spawned children before bailing
                        for (pid_t cp : child_pids) {
                            kill(cp, SIGTERM);
                            waitpid(cp, nullptr, 0);
                        }
                        return 1;
                    }
                    if (pid == 0) {
                        // Child: become QEMU rank `rank`
                        std::string rank_dir = output_base + "/rank" + std::to_string(rank);
                        std::string trace_file = rank_dir + "/" + trace_name
                                                 + "_rank" + std::to_string(rank) + trace_ext;
                        std::string plugin_arg = plugin_path + ",output=" + trace_file;

                        setenv("OMP_NUM_THREADS", "1", 1);
                        setenv("LD_PRELOAD", pimid_mpi_lib.c_str(), 1);
                        setenv("PIMID_MPI_RANKS", std::to_string(ranks).c_str(), 1);
                        setenv("PIMID_MPI_RANK", std::to_string(rank).c_str(), 1);
                        setenv("PIMID_MPI_SHM", shm_name.c_str(), 1);
                        for (const auto& [key, val] : config.workload_env) {
                            setenv(key.c_str(), val.c_str(), 1);
                        }

                        // argv: qemu -plugin <plugin>,output=<trace> -- <workload> [args...]
                        std::vector<std::string> argv_owned;
                        argv_owned.push_back(qemu_binary);
                        argv_owned.push_back("-plugin");
                        argv_owned.push_back(plugin_arg);
                        argv_owned.push_back("--");
                        argv_owned.push_back(config.workload_binary);
                        for (const auto& wa : config.workload_args) argv_owned.push_back(wa);

                        std::vector<char*> argv_cstr;
                        argv_cstr.reserve(argv_owned.size() + 1);
                        for (auto& s : argv_owned) argv_cstr.push_back(s.data());
                        argv_cstr.push_back(nullptr);

                        execvp(qemu_binary.c_str(), argv_cstr.data());
                        std::cerr << "Error: execvp(" << qemu_binary << ") failed: "
                                  << strerror(errno) << std::endl;
                        _exit(127);
                    }
                    child_pids.push_back(pid);
                }

                // Parent: wait for all children, aggregate exit code
                int worst_exit = 0;
                for (pid_t cp : child_pids) {
                    int st = 0;
                    waitpid(cp, &st, 0);
                    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
                    if (rc > worst_exit) worst_exit = rc;
                }

                // Unlink shared memory segment now that all ranks exited
                shm_unlink(shm_name.c_str());

                std::cout << "----------------------------------------" << std::endl;

                {
                    int exit_code = worst_exit;
                    std::cout << "\nAll " << ranks << " ranks exited (worst exit code: "
                              << exit_code << ")" << std::endl;

                    // Report per-rank trace stats
                    std::cout << "\n========================================" << std::endl;
                    std::cout << "MPI QEMU Trace Results (" << ranks << " ranks)" << std::endl;
                    std::cout << "========================================" << std::endl;

                    uint64_t total_events = 0;
                    for (int rank = 0; rank < ranks; rank++) {
                        std::string rank_trace_path = output_base + "/rank" + std::to_string(rank)
                            + "/" + trace_name + "_rank" + std::to_string(rank) + trace_ext;

                        struct stat st;
                        if (stat(rank_trace_path.c_str(), &st) == 0) {
                            uint64_t file_size = st.st_size;
                            uint64_t approx_events = (file_size > 576) ? (file_size - 576) / 48 : 0;
                            std::cout << "Rank " << rank << ": " << rank_trace_path
                                      << " (" << approx_events << " events, "
                                      << file_size << " bytes)" << std::endl;
                            total_events += approx_events;
                        } else {
                            std::cout << "Rank " << rank << ": (no trace file)" << std::endl;
                        }
                    }

                    std::cout << "----------------------------------------" << std::endl;
                    std::cout << "Total: ~" << total_events << " events across "
                              << ranks << " ranks" << std::endl;
                    std::cout << "Per-rank traces: " << output_base << "/rank*/\n";

                    success = (exit_code == 0);
                }
            } else {
                // -- Single-rank path --
                // Build plugin argument string
                std::string plugin_arg = plugin_path + ",output=" + config.trace_file;

                // fork/exec: qemu-x86_64 -plugin <plugin>,output=<file> -- <workload> [args]
                std::cout << "Running workload through QEMU trace generation..." << std::endl;
                std::cout << "----------------------------------------" << std::endl;

                pid_t pid = fork();
                if (pid < 0) {
                    std::cerr << "Error: Failed to fork process" << std::endl;
                    return 1;
                }

                if (pid == 0) {
                    // Child process -- set env vars and exec QEMU
                    // OpenMP defaults: without a cap, libgomp sizes the team to
                    // omp_get_num_procs() = the HOST core count under qemu-user,
                    // spawning far more threads than the num_pes simulated PE
                    // contexts. The surplus threads spin at the parallel-region
                    // barrier, the region never completes, ROI_END never fires,
                    // and the sim livelocks (OMP-detailed) or reports wrong cycles
                    // (other OMP modes, from oversubscription). Cap to num_pes +
                    // passive waits. overwrite=0 so explicit workload_env wins.
                    /* 1.9.41: NOT gated on the declaration any more. The settings below are
                     * OpenMP-runtime hygiene: without them libgomp sizes its team from
                     * omp_get_num_procs(), which under qemu-user is the HOST core count, and
                     * leaves threads ACTIVELY spinning at the barrier. The comment above has
                     * described the consequence ("reports wrong cycles") since this code was
                     * written -- but the guard let it happen to any workload that did not
                     * DECLARE itself openmp, and workload_type defaults to "serial".
                     *
                     * That is how the reference benchmark reached this path: it runs an
                     * OpenMP binary and never sets workload.type, so it took the branch the
                     * comment warns about. Team size from the host machine explains why two
                     * nodes disagreed; active spin-waiting explains why repeats on ONE node
                     * disagreed, since the simulator charges cycles for the spinning and the
                     * spinning depends on real scheduling.
                     *
                     * These are inert for a genuinely serial binary -- a program with no
                     * OpenMP runtime never reads them -- and essential for one that turns out
                     * not to be. So they are applied unconditionally: correctness must not
                     * depend on the user having declared the workload accurately. Explicit
                     * workload.env still wins (overwrite=0 below). */
                    if (true) {
                        // Host baseline runs OMP threads across host cores, not device PEs.
                        bool host_baseline = (getenv("PIMID_COSIM_NO_OFFLOAD") != nullptr)
                                             && (config.scope == "system");
                        int par = host_baseline ? config.host_num_cores : config.num_pes;
                        int omp_threads = (par > 0) ? par : 1;
                        setenv("OMP_NUM_THREADS", std::to_string(omp_threads).c_str(), 0);
                        setenv("OMP_DYNAMIC", "FALSE", 0);
                        setenv("OMP_WAIT_POLICY", "PASSIVE", 0);
                        setenv("GOMP_SPINCOUNT", "0", 0);
                    }
                    for (const auto& [key, val] : config.workload_env) {
                        setenv(key.c_str(), val.c_str(), 1);
                    }
                    std::vector<const char*> args;
                    args.push_back(qemu_binary.c_str());
                    args.push_back("-plugin");
                    args.push_back(plugin_arg.c_str());
                    args.push_back("--");
                    args.push_back(config.workload_binary.c_str());
                    for (const auto& wa : config.workload_args) {
                        args.push_back(wa.c_str());
                    }
                    args.push_back(nullptr);

                    execvp(qemu_binary.c_str(), const_cast<char* const*>(args.data()));
                    std::cerr << "Error: Failed to execute QEMU: " << strerror(errno) << std::endl;
                    _exit(1);
                }

                // Parent -- wait for completion
                int status;
                waitpid(pid, &status, 0);

                std::cout << "----------------------------------------" << std::endl;

                if (WIFEXITED(status)) {
                    int exit_code = WEXITSTATUS(status);
                    std::cout << "QEMU completed with exit code: " << exit_code << std::endl;

                    if (exit_code == 0) {
                        // Report trace file stats
                        struct stat st;
                        if (stat(config.trace_file.c_str(), &st) == 0) {
                            uint64_t file_size = st.st_size;
                            // Header is 64 bytes + YAML, events are 48 bytes each
                            uint64_t approx_events = (file_size > 576) ? (file_size - 576) / 48 : 0;
                            std::cout << "Trace file: " << config.trace_file << std::endl;
                            std::cout << "  Size: " << file_size << " bytes" << std::endl;
                            std::cout << "  Approx events: " << approx_events << std::endl;
                        }
                    }

                    success = (exit_code == 0);
                } else if (WIFSIGNALED(status)) {
                    int sig = WTERMSIG(status);
                    std::cerr << "QEMU terminated by signal " << sig
                              << " (" << strsignal(sig) << ")" << std::endl;
                    success = false;
                } else {
                    std::cerr << "QEMU terminated abnormally" << std::endl;
                    success = false;
                }
            }

            std::cout << "========================================" << std::endl;

        } else {
            // -- exec: Cycle-Accurate Simulation (QEMU + ZSim) --
            // Uses QEMU user-mode emulation with libzsim_qemu.so plugin
            // to drive the full ZSim core/cache/memory hierarchy

            std::cout << "\n========================================" << std::endl;
            std::cout << "PIMID CYCLE-ACCURATE MODE (QEMU + ZSim)" << std::endl;
            std::cout << "========================================" << std::endl;
            std::cout << "Workload: " << config.workload_binary << std::endl;

            // Find qemu-x86_64 binary
            std::string qemu_binary = findQemuBinary();
            if (qemu_binary.empty()) {
                std::cerr << "Error: qemu-x86_64 not found" << std::endl;
                std::cerr << "Install with: sudo apt install qemu-user" << std::endl;
                return 1;
            }
            std::cout << "Using QEMU: " << qemu_binary << std::endl;

            // Find libzsim_qemu.so plugin
            std::string plugin_path = findQemuPlugin("libzsim_qemu.so");
            if (plugin_path.empty()) {
                std::cerr << "Error: libzsim_qemu.so not found" << std::endl;
                std::cerr << "Build with: cmake --build . --target zsim_qemu" << std::endl;
                return 1;
            }
            std::cout << "Plugin:   " << plugin_path << std::endl;

            // Determine core type for display
            std::string exec_core_type = "Simple";
            if (config.pe_type == "ooo_core") {
                exec_core_type = "OoO";
            } else if (config.pe_type == "in_order_core") {
                exec_core_type = "InOrder";
            } else if (config.pe_type == "alu_core" || config.pe_type == "alu") {
                exec_core_type = "ALU";
            } else if (config.pe_type == "null_core" || config.pe_type == "null") {
                exec_core_type = "Null";
            }

            // Display core info
            if (exec_core_type == "ALU") {
                std::cout << "  Core:      ALU (compute=" << std::fixed << std::setprecision(2)
                          << config.alu_compute_factor << ", access=" << config.alu_access_factor
                          << ", throughput=" << config.alu_throughput_factor
                          << ", energy=" << config.alu_energy_factor << std::defaultfloat
                          << ", width=" << config.alu_operand_width << "b)" << std::endl;
            } else {
                std::cout << "  Core:      " << exec_core_type << std::endl;
            }

            // Display cache configuration with latencies (exec mode banner)
            if (exec_core_type != "ALU") {
                std::string cache_src = config.use_yaml_cache_params ? "YAML override" : "CACTI " + std::to_string(std::max(22, config.tech_node_nm)) + "nm";
                int disp_l1d, disp_l1i, disp_l2 = 0, disp_l3 = 0;
                if (config.use_yaml_cache_params) {
                    disp_l1d = std::max(1, static_cast<int>(std::round(
                        config.l1d_params.latency_ns * config.frequency_mhz / 1000.0)));
                    disp_l1i = std::max(1, static_cast<int>(std::round(
                        config.l1i_params.latency_ns * config.frequency_mhz / 1000.0)));
                    if (config.enable_l2)
                        disp_l2 = std::max(1, static_cast<int>(std::round(
                            config.l2_params.latency_ns * config.frequency_mhz / 1000.0)));
                    if (config.enable_l3)
                        disp_l3 = std::max(1, static_cast<int>(std::round(
                            config.l3_params.latency_ns * config.frequency_mhz / 1000.0)));
                } else {
                    disp_l1d = getCacheLatencyCycles(config.l1d_size_kb, config.l1d_ways,
                                                      config.cache_line_size, config.frequency_mhz, config.tech_node_nm, /*fallback_cycles=*/4);
                    disp_l1i = getCacheLatencyCycles(config.l1i_size_kb, config.l1i_ways,
                                                      config.cache_line_size, config.frequency_mhz, config.tech_node_nm, /*fallback_cycles=*/3);
                    if (config.enable_l2)
                        disp_l2 = getCacheLatencyCycles(config.l2_size_kb, config.l2_ways,
                                                         config.cache_line_size, config.frequency_mhz, config.tech_node_nm, /*fallback_cycles=*/12);
                    if (config.enable_l3)
                        disp_l3 = getCacheLatencyCycles(config.l3_size_kb, config.l3_ways,
                                                         config.cache_line_size, config.frequency_mhz, config.tech_node_nm, /*fallback_cycles=*/20);
                    /* 1.11.57 (latent B020): the banner claimed CACTI for
                     * these numbers whatever happened inside the query. If
                     * any of them is a substituted literal, say so here --
                     * the provenance line is where a reader looks. */
                    if (cacheLatencyFallbackCount() > 0)
                        cache_src = "DECLARED FALLBACK cycles, CACTI unavailable";
                }
                std::cout << "  L1D:       " << config.l1d_size_kb << "KB, "
                          << config.l1d_ways << "-way, " << disp_l1d
                          << (disp_l1d == 1 ? " cycle" : " cycles")
                          << " (" << cache_src << ")" << std::endl;
                std::cout << "  L1I:       " << config.l1i_size_kb << "KB, "
                          << config.l1i_ways << "-way, " << disp_l1i
                          << (disp_l1i == 1 ? " cycle" : " cycles")
                          << " (" << cache_src << ")" << std::endl;
                if (config.enable_l2) {
                    std::cout << "  L2:        " << config.l2_size_kb << "KB, "
                              << config.l2_ways << "-way, " << disp_l2
                              << (disp_l2 == 1 ? " cycle" : " cycles");
                    if (config.l2_count > 1) {
                        std::cout << " (x" << config.l2_count << ", "
                                  << (config.num_pes / config.l2_count) << " PEs/L2";
                        if (!config.enable_l3)
                            std::cout << ", independent LLCs";
                        std::cout << ")";
                    }
                    std::cout << " (" << cache_src << ")" << std::endl;
                }
                if (config.enable_l3) {
                    std::cout << "  L3:        " << config.l3_size_kb << "KB, "
                              << config.l3_ways << "-way, " << disp_l3
                              << (disp_l3 == 1 ? " cycle" : " cycles")
                              << " (unified LLC, " << cache_src << ")" << std::endl;
                }
            }

            // Pre-compute memory latency (may trigger CACTI/NVSim/Ramulator output)
            int disp_latency = 0;
            if (config.zsim_mem_controller_type != "ramulator") {
                disp_latency = (config.memory_latency_override >= 0) ?
                    config.memory_latency_override :
                    getMemoryLatencyCycles(config.memory_tech, config.frequency_mhz,
                                           config.tech_node_nm,
                                           config.use_yaml_memory_params,
                                           config.memory_params.read_latency_ns,
                                           -999, config.temperature_k,
                                           config.cache_line_size);  // 1.11.56 (B018)
            }
            // Display memory controller configuration
            std::cout << "  Memory:    " << config.memory_tech << ", Controller: ";
            if (config.zsim_mem_controller_type == "ramulator") {
                std::cout << "Ramulator (Ramulator2 cycle-accurate)" << std::endl;
                std::cout << "             Config: " << config.ramulator_config_file << std::endl;
            } else if (config.zsim_mem_controller_type == "weavesimple") {
                int bl = (config.weave_bound_latency >= 0) ? config.weave_bound_latency : disp_latency;
                std::cout << "WeaveSimple (phase-aware M/D/1)" << std::endl;
                std::cout << "             Zero-load latency: " << disp_latency
                          << " cycles, Bandwidth: " << config.md1_bandwidth_mbs << " MB/s"
                          << ", Bound: " << bl << " cycles" << std::endl;
            } else {
                std::cout << "Simple (M/D/1 queuing)" << std::endl;
                std::cout << "             Zero-load latency: " << disp_latency
                          << " cycles, Bandwidth: " << config.md1_bandwidth_mbs << " MB/s" << std::endl;
            }

            // Display NoC configuration
            std::cout << "  NoC:       " << config.noc_topology;
            if (config.noc_topology == "MESH_2D" || config.noc_topology == "TORUS_2D") {
                int mesh_size = static_cast<int>(std::sqrt(config.num_pes));
                if (mesh_size * mesh_size < config.num_pes) mesh_size++;
                std::cout << " (" << mesh_size << "x" << mesh_size << ")";
            } else if (config.noc_topology == "RING") {
                std::cout << " (" << (config.noc_ring_unidirectional ? "unidirectional" : "bidirectional") << ")";
            } else if (config.noc_topology == "CUSTOM" && !config.noc_topology_file.empty()) {
                std::cout << " (from " << config.noc_topology_file << ")";
            }
            {
                std::string disp_routing = config.noc_routing;
                if (disp_routing.empty()) {
                    if (config.noc_topology == "MESH_2D")       disp_routing = "XY";
                    else if (config.noc_topology == "TORUS_2D")  disp_routing = "DOR";
                    else if (config.noc_topology == "RING")      disp_routing = "SHORTEST";
                    else if (config.noc_topology == "CROSSBAR")  disp_routing = "DIRECT";
                    else if (config.noc_topology == "FAT_TREE")  disp_routing = "NCA";
                    else if (config.noc_topology == "BUS")       disp_routing = "DIRECT";
                    else if (config.noc_topology == "H_TREE")    disp_routing = "NCA";
                    else if (config.noc_topology == "CUSTOM")    disp_routing = "TABLE";
                    else                                          disp_routing = "XY";
                }
                std::cout << ", routing=" << disp_routing
                          << ", mode=" << (config.noc_cycle_accurate ? "detailed" : "simple") << std::endl;
                std::cout << "             VCs=" << config.noc_vcs_per_vnet << "/vnet"
                          << ", buffers=" << config.noc_buffers_per_vc << "/VC"
                          << ", router=" << config.noc_router_latency
                          << ", link=" << config.noc_link_latency << " cycles";
                if (config.noc_data_msg_bits > 0 || config.noc_control_msg_bits > 0) {
                    std::cout << ", msg=" << (config.noc_control_msg_bits > 0 ? config.noc_control_msg_bits : 64)
                              << "b/" << (config.noc_data_msg_bits > 0 ? config.noc_data_msg_bits : 576) << "b";
                }
                std::cout << std::endl;
            }

            // Display hierarchy info (exec mode)
            if (config.hierarchy_enabled) {
                const std::array<std::string, 7> level_names = {
                    "Subarray", "Bank", "BankGroup", "Chip", "Rank", "Channel", "System"
                };
                std::cout << "  Hierarchy: " << config.memory_tech << " internal network" << std::endl;
                if (config.placement_level != "HOST_MC") {
                    for (int i = 0; i < 7; ++i) {
                        int lat = config.hierarchy_level_latency[i];
                        int brlat = (i < 6) ? config.hierarchy_bridge_latency[i] : 0;
                        if (lat == 0 && brlat == 0) continue;  // skip passthrough levels
                        std::cout << "    L" << i << " " << std::setw(12) << std::left
                                  << (level_names[i] + ":") << " " << lat << " cycles";
                        if (i < 6 && brlat > 0) {
                            std::cout << " + bridge " << brlat << " cy";
                            const auto& bmodel = config.hierarchy_bridge_model[i];
                            if (!bmodel.empty() && bmodel != "auto")
                                std::cout << " [" << bmodel << "]";
                        }
                        std::cout << std::endl;
                    }
                }
                // PE placement with M:N mapping info
                if (config.placement_level == "HOST_MC") {
                    std::cout << "    PE placement: HOST_MC (" << config.num_pes << " PEs share host MC)" << std::endl;
                    std::cout << "    PE-MIs: none (PEs share host MC)" << std::endl;
                } else {
                    std::cout << "    PE placement: L" << config.pe_hierarchy_level
                              << " (" << config.placement_level << "), "
                              << config.num_pes << " PEs -> " << config.total_mem_orgs << " mem orgs";
                    // Compute and display ratio
                    if (config.total_mem_orgs > 0 && config.num_pes > 0) {
                        int g = std::gcd(config.num_pes, config.total_mem_orgs);
                        std::cout << " (" << (config.num_pes / g) << ":"
                                  << (config.total_mem_orgs / g) << ", "
                                  << (config.pe_mem_connection == UnifiedConfig::PEMemConnectionMode::SHARED_IO
                                      ? "shared_io" : "separate_endpoints") << ")";
                    }
                    std::cout << std::endl;
                    if (config.pe_mem_connection == UnifiedConfig::PEMemConnectionMode::SEPARATE_ENDPOINTS) {
                        std::cout << "    Endpoints: " << config.total_network_endpoints
                                  << " (PEs=" << config.num_pes << " + mem_orgs=" << config.total_mem_orgs
                                  << "), local_link=" << config.local_link_latency << " cy" << std::endl;
                    }

                    // PE-MI info (always present for non-HOST_MC due to validation)
                    if (config.pe_mc_enabled) {
                        int mi_count = peMemoryInterfaceCount(config);  // 1.11.60 (A012)
                        std::cout << "    PE-MIs: " << mi_count
                                  << " (" << config.pes_per_mc << " PEs/MI";
                        if (config.ports_per_bank > 1)
                            std::cout << ", " << config.ports_per_bank << " ports/bank";
                        std::cout << ")";
                        if (config.pe_mc_local_latency > 0)
                            std::cout << ", local_lat=" << config.pe_mc_local_latency << "cy";
                        if (config.pe_mc_bandwidth_mbs > 0)
                            std::cout << ", bw=" << config.pe_mc_bandwidth_mbs << "MB/s";
                        std::cout << std::endl;
                    }
                }
            }

            // -- 1.8.3: thread-based MPI emu is the ONLY exec-method MPI
            // model. All N ranks run as guest threads inside ONE QEMU/zsim
            // process (the OMP execution substrate): one Garnet sees every
            // rank's traffic directly in simulated time, ordered by the
            // phase barriers -- venue-independent by construction. The
            // legacy per-rank-process exec path (PIMID_MPI_PROCESS) is
            // deleted; per-rank processes remain only in the trace method.
            if (config.workload_type == "mpi" && config.mpi_ranks > 0) {
                std::cout << "MPI Mode: " << config.mpi_ranks
                          << " ranks (serial deterministic simulation)"
                          << std::endl;
            }
            {
                // -- Single-process path (single-rank, OMP, and 1.6 thread-MPI) --
                // Thread-MPI needs libpimid_mpi.so resolvable BEFORE the fork
                // so a missing library is a clean launcher error, not a child
                // exec mystery.
                std::string mpi_thread_lib;
                if (config.workload_type == "mpi" && config.mpi_ranks > 0) {
                    mpi_thread_lib = findPimidMpiLib();
                    if (mpi_thread_lib.empty()) {
                        std::cerr << "Error: libpimid_mpi.so not found (searched "
                                  << "build dirs); required for thread-MPI"
                                  << std::endl;
                        return 1;
                    }
                }
                // Generate ZSim configuration file (reuse ZSimSimulator's logic)
                ZSimSimulator zsim_helper(config);
                std::string zsim_cfg_path = zsim_helper.generateConfig();
                if (zsim_cfg_path.empty()) {
                    std::cerr << "Error: Failed to generate ZSim config" << std::endl;
                    return 1;
                }

                std::cout << "ZSim config: " << zsim_cfg_path << std::endl;

                // Config-only mode (same validation aid as the system-scope
                // path): emit the generated ZSim config and stop WITHOUT
                // launching QEMU, so wiring can be inspected on a login node
                // without running any simulation.
                if (getenv("PIMID_EMIT_CONFIG_ONLY")) {
                    std::cout << "  (PIMID_EMIT_CONFIG_ONLY set: config emitted, "
                              << "skipping simulation launch)" << std::endl;
                    return 0;
                }

                // Output directory (also used by QEMU plugin via out= parameter)
                std::string output_dir = "/tmp/pimid_qemu_" + std::to_string(getpid());
                mkdir(output_dir.c_str(), 0755);
                config.zsim_output_dir = output_dir;

                // Build plugin argument string
                std::string plugin_arg = plugin_path
                    + ",cfg=" + zsim_cfg_path
                    + ",out=" + output_dir;

                std::cout << "Output:   " << output_dir << std::endl;
                std::cout << std::endl;

                // fork/exec: qemu-x86_64 -plugin <plugin>,cfg=<cfg>,out=<dir> -- <workload> [args]
                std::cout << "Running workload through QEMU+ZSim simulation..." << std::endl;
                std::cout << "----------------------------------------" << std::endl;

                pid_t pid = fork();
                if (pid < 0) {
                    std::cerr << "Error: Failed to fork process" << std::endl;
                    return 1;
                }

                if (pid == 0) {
                    // Child process -- set env vars and exec QEMU
                    // OpenMP defaults: without a cap, libgomp sizes the team to
                    // omp_get_num_procs() = the HOST core count under qemu-user,
                    // spawning far more threads than the num_pes simulated PE
                    // contexts. The surplus threads spin at the parallel-region
                    // barrier, the region never completes, ROI_END never fires,
                    // and the sim livelocks (OMP-detailed) or reports wrong cycles
                    // (other OMP modes, from oversubscription). Cap to num_pes +
                    // passive waits. overwrite=0 so explicit workload_env wins.
                    /* 1.9.41: NOT gated on the declaration any more. The settings below are
                     * OpenMP-runtime hygiene: without them libgomp sizes its team from
                     * omp_get_num_procs(), which under qemu-user is the HOST core count, and
                     * leaves threads ACTIVELY spinning at the barrier. The comment above has
                     * described the consequence ("reports wrong cycles") since this code was
                     * written -- but the guard let it happen to any workload that did not
                     * DECLARE itself openmp, and workload_type defaults to "serial".
                     *
                     * That is how the reference benchmark reached this path: it runs an
                     * OpenMP binary and never sets workload.type, so it took the branch the
                     * comment warns about. Team size from the host machine explains why two
                     * nodes disagreed; active spin-waiting explains why repeats on ONE node
                     * disagreed, since the simulator charges cycles for the spinning and the
                     * spinning depends on real scheduling.
                     *
                     * These are inert for a genuinely serial binary -- a program with no
                     * OpenMP runtime never reads them -- and essential for one that turns out
                     * not to be. So they are applied unconditionally: correctness must not
                     * depend on the user having declared the workload accurately. Explicit
                     * workload.env still wins (overwrite=0 below). */
                    if (true) {
                        // Host baseline runs OMP threads across host cores, not device PEs.
                        bool host_baseline = (getenv("PIMID_COSIM_NO_OFFLOAD") != nullptr)
                                             && (config.scope == "system");
                        int par = host_baseline ? config.host_num_cores : config.num_pes;
                        int omp_threads = (par > 0) ? par : 1;
                        setenv("OMP_NUM_THREADS", std::to_string(omp_threads).c_str(), 0);
                        setenv("OMP_DYNAMIC", "FALSE", 0);
                        setenv("OMP_WAIT_POLICY", "PASSIVE", 0);
                        setenv("GOMP_SPINCOUNT", "0", 0);
                    }
                    // Thread-MPI: the preloaded libpimid_mpi.so interposes
                    // __libc_start_main and runs the app's UNTOUCHED main() once
                    // per rank on its own guest thread. Rank identity is TLS and
                    // the transport is plain process memory; zsim sees an
                    // OMP-shaped world (N threads -> N cores, one Garnet).
                    // PIMID_MPI_RANKS is the INTERNAL launcher->shim channel
                    // (rank count > 1 engages the emulation; not a user knob).
                    if (config.workload_type == "mpi" && config.mpi_ranks > 0) {
                        // LD_PRELOAD must be GUEST-scoped (qemu -E below): a
                        // host-env preload loads the interposer into QEMU
                        // itself, which then hijacks QEMU's own main and
                        // spawns N QEMUs in one process (instant SIGSEGV).
                        setenv("PIMID_MPI_RANKS",
                               std::to_string(config.mpi_ranks).c_str(), 1);
                        setenv("OMP_NUM_THREADS", "1", 0);
                    }
                    for (const auto& [key, val] : config.workload_env) {
                        setenv(key.c_str(), val.c_str(), 1);
                    }
                    std::vector<const char*> args;
                    std::string guest_preload;
                    args.push_back(qemu_binary.c_str());
                    if (config.workload_type == "mpi" && config.mpi_ranks > 0) {
                        guest_preload = "LD_PRELOAD=" + mpi_thread_lib;
                        args.push_back("-E");
                        args.push_back(guest_preload.c_str());
                    }
                    args.push_back("-plugin");
                    args.push_back(plugin_arg.c_str());
                    args.push_back("--");
                    args.push_back(config.workload_binary.c_str());
                    for (const auto& wa : config.workload_args) {
                        args.push_back(wa.c_str());
                    }
                    args.push_back(nullptr);

                    execvp(qemu_binary.c_str(), const_cast<char* const*>(args.data()));
                    std::cerr << "Error: Failed to execute QEMU: " << strerror(errno) << std::endl;
                    _exit(1);
                }

                // Parent -- wait for completion
                int status;
                waitpid(pid, &status, 0);

                std::cout << "----------------------------------------" << std::endl;

                if (WIFEXITED(status)) {
                    int exit_code = WEXITSTATUS(status);
                    std::cout << "QEMU+ZSim completed with exit code: " << exit_code << std::endl;

                    // Parse ZSim stats regardless of guest exit code
                    // (guest may return non-zero but ZSim still collected valid data)
                    std::string stats_path = output_dir + "/zsim.out";
                    ZSimParsedOutput exec_zsim_stats = parseZSimOutputFile(stats_path);
                    std::ifstream stats_file(stats_path);
                    if (stats_file.is_open()) {
                        std::cout << "\nZSim Statistics:" << std::endl;
                        std::string line;
                        while (std::getline(stats_file, line)) {
                            std::cout << "  " << line << std::endl;
                        }
                    }

                    // 1.6 thread-MPI: print the SAME per-rank/Total summary the
                    // process-mode launcher printed, so every downstream parser
                    // (sweep runners grep "Total: ... (max: N)") works unchanged.
                    // Per-core cycles in the single zsim.out ARE the per-rank
                    // cycles (rank r == core r in thread mode).
                    if (config.workload_type == "mpi" && config.mpi_ranks > 0) {
                        /* 1.11.48 (FIX-PRE-FLEET L248): LAST DUMP ONLY. The
                         * 1.11.9 last-dump-wins fix went to the main parser;
                         * this summary still took the FIRST mpi_ranks cycle
                         * lines -- i.e. the FIRST dump's stale mid-run values
                         * whenever a periodic dump preceded the final one. */
                        std::ifstream sf(stats_path);
                        std::string ln;
                        std::vector<std::string> lines_;
                        while (std::getline(sf, ln)) lines_.push_back(ln);
                        std::vector<size_t> seps_;
                        for (size_t li = 0; li < lines_.size(); li++)
                            if (lines_[li] == "===") seps_.push_back(li);
                        size_t s0 = 0, s1 = lines_.size();
                        if (seps_.size() >= 2) {
                            s0 = seps_[seps_.size() - 2] + 1;
                            s1 = seps_[seps_.size() - 1];
                        }
                        std::vector<uint64_t> rankCycles;
                        for (size_t li = s0; li < s1; li++) {
                            const std::string& l2 = lines_[li];
                            size_t cpos = l2.find("cycles: ");
                            if (cpos != std::string::npos &&
                                l2.find("# Simulated") != std::string::npos) {
                                uint64_t v = strtoull(l2.c_str() + cpos + 8, nullptr, 10);
                                rankCycles.push_back(v);
                            }
                        }
                        uint64_t tot = 0, mx = 0;
                        int printed = 0;
                        for (size_t i = 0; i < rankCycles.size() &&
                             printed < config.mpi_ranks; i++) {
                            if (rankCycles[i] == 0) continue;
                            std::cout << "Rank " << printed << ": " << rankCycles[i]
                                      << " cycles, " << 0 << " instrs" << std::endl;
                            tot += rankCycles[i];
                            if (rankCycles[i] > mx) mx = rankCycles[i];
                            printed++;
                        }
                        std::cout << "Total:  " << tot << " cycles (max: " << mx
                                  << ")" << std::endl;
                        /* 1.9.43: report EXECUTED instructions, not the raw
                         * counter. `instrs` includes injected timing charges --
                         * the synthetic per-basic-block advances the core adds
                         * when running without decoded micro-ops (ooo_core.cpp:
                         * syntheticInstrs and instrs are both incremented on
                         * that path). Those are CYCLES expressed as an
                         * instruction count, not code that ran.
                         *
                         * Every other consumer has been on the corrected basis
                         * since 1.9.29 -- the power model at both call sites, and
                         * the per-node activity line. This one printed line was
                         * never moved with them, so a co-simulated host that
                         * executed a handful of instructions and then waited
                         * through the offload was summarised as having executed
                         * hundreds of thousands.
                         *
                         * Nothing downstream reads this line, so correcting it
                         * moves no computed value -- it stops the summary
                         * contradicting the numbers printed beside it. */
                        std::cout << "        " << exec_zsim_stats.real_instrs()
                                  << " instructions executed"
                                  << " (" << exec_zsim_stats.syntheticInstrs
                                  << " injected timing charges excluded)" << std::endl;
                    }

                    // 1.8.8: OpenMP critical-path cycle summary. zsim.out holds one
                    // "cycles: N # Simulated cycles" line per device PE (core). A PE's
                    // per-core count is only ITS OWN active cycles, so the FIRST line
                    // (core 0) -- which is what a "grep ... | head -1" sweep records and
                    // what parseZSimOutputFile() latches into out.cycles -- is an
                    // unreliable proxy for kernel completion: core 0 can finish well
                    // before the slowest PE. Measured bfs BANK sweep spread: at 64 PEs
                    // core 0 = 9.36M vs max 12.68M (26% low); at 16 PEs 10.98M vs 11.95M
                    // (8% low); at 32 PEs 14.09M vs 14.28M (representative). Recording
                    // core 0 therefore manufactured a spurious +28% "bump" at 32 PEs that
                    // "reversed" at 64. The kernel finishes when the LAST PE finishes, so
                    // the critical-path metric is the MAX across PEs -- exactly what the
                    // MPI path above reports. Emit the same "Total: ... (max: N)" summary
                    // so downstream OMP sweeps can grep a robust critical-path value
                    // instead of head -1. Purely additive: the per-PE zsim.out lines,
                    // out.cycles, and power analysis are untouched (bit-identical).
                    /* 1.9.41: NOT gated on the declaration any more. The settings below are
                     * OpenMP-runtime hygiene: without them libgomp sizes its team from
                     * omp_get_num_procs(), which under qemu-user is the HOST core count, and
                     * leaves threads ACTIVELY spinning at the barrier. The comment above has
                     * described the consequence ("reports wrong cycles") since this code was
                     * written -- but the guard let it happen to any workload that did not
                     * DECLARE itself openmp, and workload_type defaults to "serial".
                     *
                     * That is how the reference benchmark reached this path: it runs an
                     * OpenMP binary and never sets workload.type, so it took the branch the
                     * comment warns about. Team size from the host machine explains why two
                     * nodes disagreed; active spin-waiting explains why repeats on ONE node
                     * disagreed, since the simulator charges cycles for the spinning and the
                     * spinning depends on real scheduling.
                     *
                     * These are inert for a genuinely serial binary -- a program with no
                     * OpenMP runtime never reads them -- and essential for one that turns out
                     * not to be. So they are applied unconditionally: correctness must not
                     * depend on the user having declared the workload accurately. Explicit
                     * workload.env still wins (overwrite=0 below). */
                    if (true) {
                        /* 1.11.48 (FIX-PRE-FLEET L249): LAST DUMP ONLY -- the
                         * same rule parseZSimOutputFile has carried since
                         * 1.11.9, which this reader never received. zsim.out
                         * holds one "===-delimited" dump PER stats write
                         * (periodic + final), and counters are monotonic run
                         * totals; accumulating every dump multiplied `tot`
                         * and the PE count by the dump count (max survived,
                         * mean and Total lied). */
                        std::ifstream sf(stats_path);
                        std::string ln;
                        std::vector<std::string> lines_;
                        while (std::getline(sf, ln)) lines_.push_back(ln);
                        std::vector<size_t> seps_;
                        for (size_t li = 0; li < lines_.size(); li++)
                            if (lines_[li] == "===") seps_.push_back(li);
                        size_t s0 = 0, s1 = lines_.size();
                        if (seps_.size() >= 2) {
                            s0 = seps_[seps_.size() - 2] + 1;
                            s1 = seps_[seps_.size() - 1];
                        }
                        std::vector<uint64_t> peCycles;
                        for (size_t li = s0; li < s1; li++) {
                            const std::string& l2 = lines_[li];
                            size_t cpos = l2.find("cycles: ");
                            if (cpos != std::string::npos &&
                                l2.find("# Simulated") != std::string::npos) {
                                uint64_t v = strtoull(l2.c_str() + cpos + 8, nullptr, 10);
                                if (v > 0) peCycles.push_back(v);
                            }
                        }
                        if (!peCycles.empty()) {
                            uint64_t tot = 0, mx = 0, mn = UINT64_MAX;
                            for (uint64_t v : peCycles) {
                                tot += v;
                                if (v > mx) mx = v;
                                if (v < mn) mn = v;
                            }
                            uint64_t mean = tot / peCycles.size();
                            std::cout << "OMP cycles: " << mx << " (mean: " << mean
                                      << ", min: " << mn << ", pes: " << peCycles.size()
                                      << ", critical-path max)" << std::endl;
                            std::cout << "Total:  " << tot << " cycles (max: " << mx
                                      << ")" << std::endl;
                        }
                    }

                    // Power analysis (runs if stats exist, even for non-zero guest exit)
                    // OOO cores may report cycles=0 (contention sim not triggered in QEMU mode)
                    // but still produce valid instrs -- use instrs as fallback guard
                    if (config.enable_power &&
                        (exec_zsim_stats.cycles > 0 || exec_zsim_stats.instrs > 0)) {
                        runPowerAnalysis(config, exec_zsim_stats, output_dir);
                    }

                    success = (exit_code == 0);
                } else if (WIFSIGNALED(status)) {
                    int sig = WTERMSIG(status);
                    std::cerr << "QEMU+ZSim terminated by signal " << sig
                              << " (" << strsignal(sig) << ")" << std::endl;
                    success = false;
                } else {
                    std::cerr << "QEMU terminated abnormally" << std::endl;
                    success = false;
                }
            }

            std::cout << "========================================" << std::endl;

        }
    } else if (config.scope == "system") {
        // Multi-host/multi-device system simulation
        if (config.method == "exec") {
            // Banner
            int num_hosts = 0, num_compute_devs = 0, num_mem_devs = 0;
            for (const auto& n : config.system_nodes) {
                if (n.role == UnifiedConfig::SystemNode::HOST) num_hosts++;
                else if (n.device_type == UnifiedConfig::SystemNode::COMPUTE) num_compute_devs++;
                else num_mem_devs++;
            }

            std::cout << "\n========================================" << std::endl;
            std::cout << "PIMID SYSTEM: " << num_hosts << " Host"
                      << (num_hosts > 1 ? "s" : "")
                      << " + " << (num_compute_devs + num_mem_devs) << " Device"
                      << ((num_compute_devs + num_mem_devs) > 1 ? "s" : "") << std::endl;
            std::cout << "========================================" << std::endl;

            for (const auto& n : config.system_nodes) {
                if (n.role == UnifiedConfig::SystemNode::HOST) {
                    std::cout << "  Host " << n.name << ": "
                              << n.num_cores << "x ";
                    if (n.core_type == "ooo_core") std::cout << "OOO";
                    else if (n.core_type == "in_order_core") std::cout << "InOrder";
                    else if (n.core_type == "alu_core") std::cout << "ALU";
                    else if (n.core_type == "null_core") std::cout << "Null";
                    else std::cout << "Simple";
                    std::cout << " @ " << static_cast<int>(n.frequency_mhz) << " MHz"
                              << ", " << n.memory_tech << std::endl;
                } else {
                    std::cout << "  Device " << n.name << ": ";
                    if (n.device_type == UnifiedConfig::SystemNode::COMPUTE) {
                        std::cout << n.num_pes << "x ";
                        if (n.core_type == "alu_core") std::cout << "ALU";
                        else if (n.core_type == "ooo_core") std::cout << "OOO";
                        else std::cout << "Simple";
                        std::cout << " @ " << static_cast<int>(n.frequency_mhz) << " MHz";
                    } else {
                        std::cout << "memory-only";
                    }
                    std::cout << ", " << n.memory_tech
                              << " (" << (n.device_type == UnifiedConfig::SystemNode::COMPUTE ? "compute" : "memory")
                              << ", " << (n.attachment == UnifiedConfig::SystemNode::INTERNAL ? "internal" : "external")
                              << ")" << std::endl;
                }
                // Per-node workload display
                if (!n.workload_binary.empty()) {
                    std::cout << "    Workload: " << n.workload_binary;
                    for (const auto& a : n.workload_args) std::cout << " " << a;
                    std::cout << std::endl;
                }
            }

            // Multi-QEMU mode indicator
            if (needsMultiQemu(config)) {
                auto groups = buildProcessGroups(config);
                std::cout << "  Execution: Multi-QEMU (" << groups.size() << " processes)" << std::endl;
            }

            // System network info
            int num_nodes = (int)config.system_nodes.size();
            double avg_hops = avgHopsForTopology(config.system_network.topology, num_nodes);
            uint32_t example_lat = computeSystemNetLatency(config, 0, num_nodes > 1 ? 1 : 0);
            std::cout << "  System Network: " << config.system_network.topology
                      << " [" << config.system_network.model << "]"
                      << ", " << example_lat << " ref-cycles (avg "
                      << std::fixed << std::setprecision(1) << avg_hops << std::defaultfloat << " hops)"
                      << std::endl;

            // Per-link info
            for (const auto& lnk : config.system_network.links) {
                std::cout << "    " << lnk.src_name << " <-> " << lnk.dst_name
                          << ": " << lnk.link_type
                          << " (" << lnk.base_latency_ns << "ns, "
                          << lnk.bandwidth_GBs << " GB/s)" << std::endl;
            }

            std::cout << "  Reference frequency: " << static_cast<int>(config.reference_frequency_mhz)
                      << " MHz" << std::endl;

            // Generate combined ZSim config
            std::string zsim_cfg_path = generateSystemConfig(config);
            if (zsim_cfg_path.empty()) {
                std::cerr << "Error: Failed to generate system ZSim config" << std::endl;
                return 1;
            }
            std::cout << "  ZSim config: " << zsim_cfg_path << std::endl;

            // Config-only mode (1.7.1 validation aid): emit the generated ZSim
            // config and stop WITHOUT launching QEMU. Lets the sys.bridge
            // defaults table be inspected on the login node without running any
            // simulation. Gated on PIMID_EMIT_CONFIG_ONLY.
            if (getenv("PIMID_EMIT_CONFIG_ONLY")) {
                std::cout << "  (PIMID_EMIT_CONFIG_ONLY set: config emitted, "
                          << "skipping simulation launch)" << std::endl;
                return 0;
            }

            // Find QEMU and plugin
            std::string qemu_binary = findQemuBinary();
            if (qemu_binary.empty()) {
                std::cerr << "Error: qemu-x86_64 not found" << std::endl;
                return 1;
            }
            std::string plugin_path = findQemuPlugin("libzsim_qemu.so");
            if (plugin_path.empty()) {
                std::cerr << "Error: libzsim_qemu.so not found" << std::endl;
                return 1;
            }

            // Output directory
            std::string output_dir = "/tmp/pimid_system_zsim_" + std::to_string(getpid());
            mkdir(output_dir.c_str(), 0755);
            config.zsim_output_dir = output_dir;

            // Launch QEMU + ZSim
            int status = 0;
            if (needsMultiQemu(config)) {
                // Multi-QEMU: different binaries on different nodes
                auto groups = buildProcessGroups(config);
                std::vector<pid_t> pids;

                // Clean up any stale sentinel
                std::string sentinel = output_dir + "/.zsim_shmid";
                unlink(sentinel.c_str());

                for (size_t i = 0; i < groups.size(); i++) {
                    if (i > 0) {
                        // Wait for primary to finish init (sentinel file exists)
                        while (access(sentinel.c_str(), F_OK) != 0) usleep(10000);
                    }

                    pid_t pid = fork();
                    if (pid == 0) {
                        setenv("ZSIM_CFG_FILE", zsim_cfg_path.c_str(), 1);
                        setenv("ZSIM_OUTPUT_DIR", output_dir.c_str(), 1);
                        // Same OpenMP team cap as the single-binary system fork
                        // below: without it libgomp oversubscribes the device PEs
                        // (HOST core count under qemu-user) and the coupled co-sim
                        // livelocks. Cap to the largest compute-device PE count.
                        /* 1.9.41: NOT gated on the declaration any more. The settings below are
                     * OpenMP-runtime hygiene: without them libgomp sizes its team from
                     * omp_get_num_procs(), which under qemu-user is the HOST core count, and
                     * leaves threads ACTIVELY spinning at the barrier. The comment above has
                     * described the consequence ("reports wrong cycles") since this code was
                     * written -- but the guard let it happen to any workload that did not
                     * DECLARE itself openmp, and workload_type defaults to "serial".
                     *
                     * That is how the reference benchmark reached this path: it runs an
                     * OpenMP binary and never sets workload.type, so it took the branch the
                     * comment warns about. Team size from the host machine explains why two
                     * nodes disagreed; active spin-waiting explains why repeats on ONE node
                     * disagreed, since the simulator charges cycles for the spinning and the
                     * spinning depends on real scheduling.
                     *
                     * These are inert for a genuinely serial binary -- a program with no
                     * OpenMP runtime never reads them -- and essential for one that turns out
                     * not to be. So they are applied unconditionally: correctness must not
                     * depend on the user having declared the workload accurately. Explicit
                     * workload.env still wins (overwrite=0 below). */
                    if (true) {
                            int omp_threads = 0;
                            for (const auto& n : config.system_nodes) {
                                if (n.role == UnifiedConfig::SystemNode::DEVICE &&
                                    n.num_pes > omp_threads)
                                    omp_threads = n.num_pes;
                            }
                            if (omp_threads <= 0)
                                omp_threads = (config.num_pes > 0) ? config.num_pes : 1;
                            setenv("OMP_NUM_THREADS", std::to_string(omp_threads).c_str(), 0);
                            setenv("OMP_DYNAMIC", "FALSE", 0);
                            setenv("OMP_WAIT_POLICY", "PASSIVE", 0);
                            setenv("GOMP_SPINCOUNT", "0", 0);
                            // DETERMINISM: force a fixed loop->thread mapping at the
                            // offload boundary. The coupled device cycle count is
                            // sensitive to OpenMP scheduling order (run-to-run spread
                            // ~+/-3.6% at matched clocks). Static scheduling pins each
                            // loop iteration to a fixed thread, and PROC_BIND/PLACES
                            // pin threads to cores, so the per-PE work split (and thus
                            // the device curCycle) is reproducible across runs.
                            setenv("OMP_SCHEDULE", "static", 0);
                            setenv("OMP_PROC_BIND", "true", 0);
                            setenv("OMP_PLACES", "cores", 0);
                        }
                        for (const auto& [key, val] : config.workload_env) {
                            setenv(key.c_str(), val.c_str(), 1);
                        }

                        std::string parg = plugin_path + ",cfg=" + zsim_cfg_path
                            + ",out=" + output_dir + ",procIdx=" + std::to_string(i);

                        std::vector<const char*> args;
                        args.push_back(qemu_binary.c_str());
                        args.push_back("-plugin");
                        args.push_back(parg.c_str());
                        args.push_back("--");
                        args.push_back(groups[i].binary.c_str());
                        for (const auto& wa : groups[i].args)
                            args.push_back(wa.c_str());
                        args.push_back(nullptr);

                        execvp(qemu_binary.c_str(), const_cast<char* const*>(args.data()));
                        std::cerr << "Error: Failed to execute QEMU (process " << i << "): "
                                  << strerror(errno) << std::endl;
                        _exit(1);
                    }
                    pids.push_back(pid);
                }

                // Wait for all QEMU processes
                bool all_ok = true;
                for (pid_t p : pids) {
                    int s;
                    waitpid(p, &s, 0);
                    if (!WIFEXITED(s) || WEXITSTATUS(s) != 0) all_ok = false;
                }
                status = all_ok ? 0 : 1;

                // Clean up sentinel
                unlink(sentinel.c_str());
            } else {
                // Single-QEMU: all nodes use the same binary
                std::string plugin_arg = plugin_path
                    + ",cfg=" + zsim_cfg_path
                    + ",out=" + output_dir;

                // 1.6 thread-MPI in SYSTEM scope (co-sim + host baseline).
                // The device-scope path preloads libpimid_mpi.so so the ranks run
                // as guest threads of one process. This path never did, so the
                // guest resolved MPI against the SYSTEM MPI runtime and every
                // system-scope MPI run silently executed as a SINGLE rank
                // (rank 0, size 1) -- host baselines and co-sim alike. Resolve
                // before the fork so a missing library is a clean launcher error.
                std::string sys_mpi_lib;
                if (config.workload_type == "mpi" && config.mpi_ranks > 0) {
                    sys_mpi_lib = findPimidMpiLib();
                    if (sys_mpi_lib.empty()) {
                        std::cerr << "Error: libpimid_mpi.so not found (searched "
                                  << "build dirs); required for thread-MPI"
                                  << std::endl;
                        return 1;
                    }
                    std::cout << "MPI Mode: " << config.mpi_ranks
                              << " ranks (serial deterministic simulation)"
                              << std::endl;
                }
                pid_t pid = fork();
                if (pid == 0) {
                    setenv("ZSIM_CFG_FILE", zsim_cfg_path.c_str(), 1);
                    setenv("ZSIM_OUTPUT_DIR", output_dir.c_str(), 1);
                    // Cap the OpenMP team to the simulated device-PE count. Unlike
                    // the device-scope launch paths, this system (coupled co-sim)
                    // fork previously set NO OMP_NUM_THREADS, so libgomp sized the
                    // team to omp_get_num_procs() = the HOST core count under
                    // qemu-user (e.g. 192). Those surplus threads oversubscribe the
                    // few device-PE contexts: they never get a PE, the workload's
                    // parallel region never completes, ROI_END never fires, and the
                    // co-sim livelocks (unbounded "Thread N starting (domain=DEVICE)"
                    // respawn). Cap to the largest compute-device PE count.
                    // overwrite=0 so an explicit workload_env entry (applied just
                    // below) still wins.
                    /* 1.9.41: NOT gated on the declaration any more. The settings below are
                     * OpenMP-runtime hygiene: without them libgomp sizes its team from
                     * omp_get_num_procs(), which under qemu-user is the HOST core count, and
                     * leaves threads ACTIVELY spinning at the barrier. The comment above has
                     * described the consequence ("reports wrong cycles") since this code was
                     * written -- but the guard let it happen to any workload that did not
                     * DECLARE itself openmp, and workload_type defaults to "serial".
                     *
                     * That is how the reference benchmark reached this path: it runs an
                     * OpenMP binary and never sets workload.type, so it took the branch the
                     * comment warns about. Team size from the host machine explains why two
                     * nodes disagreed; active spin-waiting explains why repeats on ONE node
                     * disagreed, since the simulator charges cycles for the spinning and the
                     * spinning depends on real scheduling.
                     *
                     * These are inert for a genuinely serial binary -- a program with no
                     * OpenMP runtime never reads them -- and essential for one that turns out
                     * not to be. So they are applied unconditionally: correctness must not
                     * depend on the user having declared the workload accurately. Explicit
                     * workload.env still wins (overwrite=0 below). */
                    if (true) {
                        int omp_threads = 0;
                        for (const auto& n : config.system_nodes) {
                            if (n.role == UnifiedConfig::SystemNode::DEVICE &&
                                n.num_pes > omp_threads)
                                omp_threads = n.num_pes;
                        }
                        if (omp_threads <= 0)
                            omp_threads = (config.num_pes > 0) ? config.num_pes : 1;
                        setenv("OMP_NUM_THREADS", std::to_string(omp_threads).c_str(), 0);
                        setenv("OMP_DYNAMIC", "FALSE", 0);
                        setenv("OMP_WAIT_POLICY", "PASSIVE", 0);
                        setenv("GOMP_SPINCOUNT", "0", 0);
                        // DETERMINISM: force a fixed loop->thread mapping at the
                        // offload boundary so the coupled device cycle count is
                        // reproducible run-to-run (static schedule pins iterations
                        // to threads; PROC_BIND/PLACES pin threads to cores).
                        setenv("OMP_SCHEDULE", "static", 0);
                        setenv("OMP_PROC_BIND", "true", 0);
                        setenv("OMP_PLACES", "cores", 0);
                    }
                    // Thread-MPI: ranks become guest threads of this one process.
                    // OMP_NUM_THREADS=1 so each rank stays single-threaded.
                    // PIMID_MPI_RANKS is the internal launcher->shim channel.
                    if (config.workload_type == "mpi" && config.mpi_ranks > 0) {
                        setenv("PIMID_MPI_RANKS",
                               std::to_string(config.mpi_ranks).c_str(), 1);
                        setenv("OMP_NUM_THREADS", "1", 0);
                    }
                    for (const auto& [key, val] : config.workload_env) {
                        setenv(key.c_str(), val.c_str(), 1);
                    }

                    std::vector<const char*> args;
                    std::string sys_guest_preload;
                    args.push_back(qemu_binary.c_str());
                    // LD_PRELOAD must be GUEST-scoped (qemu -E): a host-env preload
                    // would load the interposer into QEMU itself.
                    if (config.workload_type == "mpi" && config.mpi_ranks > 0) {
                        sys_guest_preload = "LD_PRELOAD=" + sys_mpi_lib;
                        args.push_back("-E");
                        args.push_back(sys_guest_preload.c_str());
                    }
                    args.push_back("-plugin");
                    args.push_back(plugin_arg.c_str());
                    args.push_back("--");
                    args.push_back(config.workload_binary.c_str());
                    for (const auto& wa : config.workload_args) {
                        args.push_back(wa.c_str());
                    }
                    args.push_back(nullptr);
                    execvp(qemu_binary.c_str(), const_cast<char* const*>(args.data()));
                    std::cerr << "Error: Failed to execute QEMU: " << strerror(errno) << std::endl;
                    _exit(1);
                }

                waitpid(pid, &status, 0);
                status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            }

            std::cout << "----------------------------------------" << std::endl;
            std::cout << "QEMU+ZSim (system) completed with exit code: " << status << std::endl;

            // Parse ZSim stats
            std::string stats_path = output_dir + "/zsim.out";
            ZSimParsedOutput sys_stats = parseZSimOutputFile(stats_path);
            std::ifstream stats_file(stats_path);
            if (stats_file.is_open()) {
                std::cout << "\nSystem ZSim Statistics:" << std::endl;
                std::string line;
                while (std::getline(stats_file, line)) {
                    std::cout << "  " << line << std::endl;
                }
            }

            // Power analysis
            if (config.enable_power &&
                (sys_stats.cycles > 0 || sys_stats.instrs > 0)) {
                runPerNodePowerAnalysis(config, sys_stats, output_dir);
            } else {
                reportFpWithoutFpu(config, sys_stats);  // 1.11.17: visible sans power
            }

            success = (status == 0);
            std::cout << "========================================" << std::endl;
        } else if (config.method == "trace") {
            // -- Trace Replay for System Scope --
            // Banner (reuse system banner logic)
            int num_hosts = 0, num_compute_devs = 0, num_mem_devs = 0;
            for (const auto& n : config.system_nodes) {
                if (n.role == UnifiedConfig::SystemNode::HOST) num_hosts++;
                else if (n.device_type == UnifiedConfig::SystemNode::COMPUTE) num_compute_devs++;
                else num_mem_devs++;
            }

            std::cout << "\n========================================" << std::endl;
            std::cout << "PIMID SYSTEM TRACE REPLAY: " << num_hosts << " Host"
                      << (num_hosts > 1 ? "s" : "")
                      << " + " << (num_compute_devs + num_mem_devs) << " Device"
                      << ((num_compute_devs + num_mem_devs) > 1 ? "s" : "") << std::endl;
            std::cout << "========================================" << std::endl;

            for (const auto& n : config.system_nodes) {
                if (n.role == UnifiedConfig::SystemNode::HOST) {
                    std::cout << "  Host " << n.name << ": "
                              << n.num_cores << "x ";
                    if (n.core_type == "ooo_core") std::cout << "OOO";
                    else if (n.core_type == "in_order_core") std::cout << "InOrder";
                    else if (n.core_type == "alu_core") std::cout << "ALU";
                    else if (n.core_type == "null_core") std::cout << "Null";
                    else std::cout << "Simple";
                    std::cout << " @ " << static_cast<int>(n.frequency_mhz) << " MHz"
                              << ", " << n.memory_tech << std::endl;
                } else {
                    std::cout << "  Device " << n.name << ": ";
                    if (n.device_type == UnifiedConfig::SystemNode::COMPUTE) {
                        std::cout << n.num_pes << "x ";
                        if (n.core_type == "alu_core") std::cout << "ALU";
                        else if (n.core_type == "ooo_core") std::cout << "OOO";
                        else std::cout << "Simple";
                        std::cout << " @ " << static_cast<int>(n.frequency_mhz) << " MHz";
                    } else {
                        std::cout << "memory-only";
                    }
                    std::cout << ", " << n.memory_tech << std::endl;
                }
            }

            // System network info
            int num_nodes = (int)config.system_nodes.size();
            double avg_hops = avgHopsForTopology(config.system_network.topology, num_nodes);
            uint32_t example_lat = computeSystemNetLatency(config, 0, num_nodes > 1 ? 1 : 0);
            std::cout << "  System Network: " << config.system_network.topology
                      << " [" << config.system_network.model << "]"
                      << ", " << example_lat << " ref-cycles (avg "
                      << std::fixed << std::setprecision(1) << avg_hops << std::defaultfloat << " hops)"
                      << std::endl;

            std::cout << "  Trace: " << config.trace_file << std::endl;

            // Generate combined ZSim config
            std::string zsim_cfg_path = generateSystemConfig(config);
            if (zsim_cfg_path.empty()) {
                std::cerr << "Error: Failed to generate system ZSim config" << std::endl;
                return 1;
            }

            // Output directory
            std::string output_dir = "/tmp/pimid_system_trace_" + std::to_string(getpid());
            mkdir(output_dir.c_str(), 0755);
            config.zsim_output_dir = output_dir;

            std::cout << "  ZSim config: " << zsim_cfg_path << std::endl;
            std::cout << "  Output dir:  " << output_dir << std::endl;
            std::cout << std::endl;

            // Run trace through ZSim simulation
            std::cout << "Running trace through ZSim simulation (system)..." << std::endl;
            std::cout << "----------------------------------------" << std::endl;

            int trace_result = zsim_trace_run(zsim_cfg_path.c_str(),
                                              config.trace_file.c_str(),
                                              output_dir.c_str());

            std::cout << "----------------------------------------" << std::endl;

            if (trace_result == 0) {
                // Parse zsim_trace_stats.txt for summary
                std::string stats_path = output_dir + "/zsim_trace_stats.txt";
                std::ifstream stats_file(stats_path);
                if (stats_file.is_open()) {
                    std::cout << "\nSystem Trace Results:" << std::endl;
                    std::string line;
                    while (std::getline(stats_file, line)) {
                        std::cout << "  " << line << std::endl;
                    }
                }

                // Parse zsim.out for cache stats
                std::string zsim_out_path = output_dir + "/zsim.out";
                ZSimParsedOutput sys_stats = parseZSimOutputFile(zsim_out_path);

                // Power analysis
                if (config.enable_power &&
                    (sys_stats.cycles > 0 || sys_stats.instrs > 0)) {
                    runPowerAnalysis(config, sys_stats, output_dir);
                }
            }

            std::cout << "\n========================================" << std::endl;
            success = (trace_result == 0);
        } else {
            std::cerr << "Error: system scope does not support " << config.method << " method" << std::endl;
            std::cerr << "Supported methods for system scope: exec, trace" << std::endl;
            return 1;
        }
    }

    return success ? 0 : 1;
}
