#include "memory/nvsim_wrapper.h"
#include "util/cache_warehouse.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>
#include <cctype>       // 1.11.57 (latent D027): cell-file name sanitising
#include <cstring>
#include <stdexcept>
#include <map>
#include <sys/stat.h>
#include <sys/types.h>

// Include NVSim headers if available
#ifdef HAVE_NVSIM
#include "InputParameter.h"
#include "MemCell.h"
#include "Result.h"
#include "Bank.h"
#include "BankWithHtree.h"
#include "BankWithoutHtree.h"
#include "Technology.h"
#include "Wire.h"
#include "global.h"
#include "formula.h"
#include "macros.h"

// NVSim global pointers are declared as 'extern' in global.h and defined
// in nvsim_globals.cpp. We access them here - do NOT redefine them.
// The globals are: nvsim::inputParameter, nvsim::tech, nvsim::cell, etc.

// Use nvsim namespace to access NVSim types and globals
using namespace nvsim;
#endif

namespace pimid {

#ifdef HAVE_NVSIM

//=============================================================================
// NVSimWrapper Implementation
//=============================================================================

NVSimWrapper::NVSimWrapper(const NVMConfig& config)
    : config_(config)
    , nvsim_input_(nullptr)
    , nvsim_cell_(nullptr)
    , nvsim_result_(nullptr)
    , initialized_(false)
    , valid_(false)
    , error_message_("")
{
}

NVSimWrapper::~NVSimWrapper() {
    // Clean up in reverse order of creation
    // Result first (it may reference Bank which uses other objects)
    if (nvsim_result_) {
        delete nvsim_result_;
        nvsim_result_ = nullptr;
    }

    // Clean up Technology and Wire that we allocated in runNVSim()
    // These are owned by us since we created them
    if (tech != nullptr) {
        delete tech;
        tech = nullptr;
    }
    if (localWire != nullptr) {
        delete localWire;
        localWire = nullptr;
    }
    if (globalWire != nullptr) {
        delete globalWire;
        globalWire = nullptr;
    }

    // Clean up cell - reset global first to avoid dangling pointer
    if (nvsim_cell_) {
        if (cell == nvsim_cell_) {
            cell = nullptr;
        }
        delete nvsim_cell_;
        nvsim_cell_ = nullptr;
    }

    // Clean up input - reset global first to avoid dangling pointer
    if (nvsim_input_) {
        if (inputParameter == nvsim_input_) {
            inputParameter = nullptr;
        }
        delete nvsim_input_;
        nvsim_input_ = nullptr;
    }
}

void NVSimWrapper::initialize() {
    if (initialized_) {
        std::cerr << "[NVSimWrapper] Warning: Already initialized, reinitializing..." << std::endl;

        // Clean up in reverse order of creation (same as destructor)
        if (nvsim_result_) {
            delete nvsim_result_;
            nvsim_result_ = nullptr;
        }

        // Clean up Technology and Wire
        if (tech != nullptr) {
            delete tech;
            tech = nullptr;
        }
        if (localWire != nullptr) {
            delete localWire;
            localWire = nullptr;
        }
        if (globalWire != nullptr) {
            delete globalWire;
            globalWire = nullptr;
        }

        // Clean up cell and reset global
        if (nvsim_cell_) {
            if (cell == nvsim_cell_) {
                cell = nullptr;
            }
            delete nvsim_cell_;
            nvsim_cell_ = nullptr;
        }

        // Clean up input and reset global
        if (nvsim_input_) {
            if (inputParameter == nvsim_input_) {
                inputParameter = nullptr;
            }
            delete nvsim_input_;
            nvsim_input_ = nullptr;
        }
    }

    validateConfiguration();
    if (!valid_) {
        throw std::runtime_error("[NVSimWrapper] Invalid configuration: " + error_message_);
    }

    try {
        createNVSimInput();
        loadCellParameters();
        runNVSim();
        initialized_ = true;

        std::cout << "[NVSimWrapper] Initialized with:" << std::endl;
        std::cout << "  Capacity: " << (config_.capacity_bytes / 1024) << " KB" << std::endl;
        std::cout << "  Word Width: " << config_.word_width_bits << " bits" << std::endl;
        std::cout << "  Technology: " << config_.process_node_nm << " nm" << std::endl;

        if (valid_ && nvsim_result_) {
            std::cout << "  Read Latency: " << (getReadLatency() * 1e9) << " ns" << std::endl;
            std::cout << "  Write Latency: " << (getWriteLatency() * 1e9) << " ns" << std::endl;
            std::cout << "  Area: " << getArea() << " mm^2" << std::endl;
            std::cout << "  Read Energy: " << getReadDynamicEnergy() << " nJ" << std::endl;
            std::cout << "  Write Energy: " << getWriteDynamicEnergy() << " nJ" << std::endl;
        }
    } catch (const std::exception& e) {
        valid_ = false;
        error_message_ = std::string("Initialization failed: ") + e.what();
        throw;
    }
}

void NVSimWrapper::reconfigure(const NVMConfig& config) {
    config_ = config;
    initialized_ = false;
    initialize();
}

void NVSimWrapper::validateConfiguration() {
    valid_ = true;
    error_message_ = "";

    // Validate capacity
    if (config_.capacity_bytes < 1024 || config_.capacity_bytes > (16ULL * 1024 * 1024 * 1024)) {
        valid_ = false;
        error_message_ = "Capacity out of range (1KB - 16GB)";
        return;
    }

    // Validate word width
    if (config_.word_width_bits < 8 || config_.word_width_bits > 1024) {
        valid_ = false;
        error_message_ = "Word width out of range (8 - 1024 bits)";
        return;
    }

    // Validate technology node
    if (config_.process_node_nm < 7 || config_.process_node_nm > 90) {
        valid_ = false;
        error_message_ = "Technology node out of range (7nm - 90nm)";
        return;
    }
}

std::string NVSimWrapper::getCellFileName() const {
    // If user provided a cell file, use it
    if (!config_.cell_file.empty()) {
        return config_.cell_file;
    }

    // Otherwise, use default based on NVM type
    switch (config_.nvm_type) {
        case NVMType::STTRAM:
            return "sample_STTRAM.cell";
        case NVMType::PCRAM:
            return "sample_PCRAM.cell";
        case NVMType::RERAM:
            return "sample_RRAM.cell";
        case NVMType::SLCNAND:
            return "sample_SLCNAND.cell";
        default:
            return "sample_STTRAM.cell";  // Default to STT-RAM
    }
}

void NVSimWrapper::createNVSimInput() {
    // NVSim classes are now in the nvsim:: namespace (local copy in external/nvsim)
    // to avoid symbol collision with CACTI's global namespace classes.
    nvsim_input_ = new InputParameter();

    // Basic parameters
    nvsim_input_->capacity = config_.capacity_bytes;
    nvsim_input_->wordWidth = config_.word_width_bits;
    nvsim_input_->processNode = config_.process_node_nm;
    nvsim_input_->temperature = config_.temperature_k;

    // Device roadmap - IMPORTANT: must be set for Technology initialization
    /* 1.11.49 (L77): from power.device_corner via config, no longer hardwired
     * HP. NVSim has real ITRS HP/LSTP/LOP columns, so unlike the DRAM-
     * periphery family (one comm-dram column, corner refused) the corner is
     * genuinely selectable here. */
    nvsim_input_->deviceRoadmap = (config_.device_corner == 1) ? LSTP
                                : (config_.device_corner == 2) ? LOP : HP;

    // Design target
    if (config_.is_cache) {
        nvsim_input_->designTarget = nvsim::cache;  // qualify: avoid clash with pimid::cache namespace
        nvsim_input_->associativity = config_.associativity;
    } else {
        nvsim_input_->designTarget = RAM_chip;
        nvsim_input_->associativity = 1;
    }

    // Optimization target (select the first enabled optimization)
    if (config_.optimize_read_latency) {
        nvsim_input_->optimizationTarget = read_latency_optimized;
    } else if (config_.optimize_write_latency) {
        nvsim_input_->optimizationTarget = write_latency_optimized;
    } else if (config_.optimize_read_energy) {
        nvsim_input_->optimizationTarget = read_energy_optimized;
    } else if (config_.optimize_write_energy) {
        nvsim_input_->optimizationTarget = write_energy_optimized;
    } else if (config_.optimize_leakage) {
        nvsim_input_->optimizationTarget = leakage_optimized;
    } else if (config_.optimize_area) {
        nvsim_input_->optimizationTarget = area_optimized;
    } else {
        // Default to read energy optimization
        nvsim_input_->optimizationTarget = read_energy_optimized;
    }

    // Page and block sizes for Flash/DRAM
    nvsim_input_->pageSize = config_.page_size_bits;
    nvsim_input_->flashBlockSize = config_.block_size_bits;

    // Set cell file
    nvsim_input_->fileMemCell = getCellFileName();

    // Use reasonable defaults for other parameters
    nvsim_input_->routingMode = h_tree;
    nvsim_input_->internalSensing = true;
    nvsim_input_->useCactiAssumption = false;

    // NOTE: Don't set global inputParameter here - do it in runNVSim() after all
    // objects are created, to match NVSim's main() initialization order

    std::cout << "[NVSimWrapper] Input parameters configured" << std::endl;
}

void NVSimWrapper::loadCellParameters() {
    // MemCell is in nvsim:: namespace (no collision with other libraries)
    nvsim_cell_ = new MemCell();

    // Build cell file path from NVSIM_DATA_DIR
    std::string cell_filename = getCellFileName();
#ifdef NVSIM_DATA_DIR
    std::string cell_path = std::string(NVSIM_DATA_DIR) + "/" + cell_filename;
#else
    std::string cell_path = cell_filename;
#endif
    std::cout << "[NVSimWrapper] Loading cell parameters from: " << cell_path << std::endl;

    // Try to load from file
    std::ifstream test(cell_path);
    if (test.good()) {
        test.close();
        nvsim_cell_->ReadCellFromFile(cell_path);
        std::cout << "[NVSimWrapper] Cell loaded: type=" << nvsim_cell_->memCellType
                  << " area=" << nvsim_cell_->area << " F^2" << std::endl;
    } else {
        // Fall back to manual type assignment if file not found
        std::cerr << "[NVSimWrapper] Cell file not found: " << cell_path
                  << ", using manual type assignment" << std::endl;
        switch (config_.nvm_type) {
            case NVMType::STTRAM:  nvsim_cell_->memCellType = MRAM;      break;
            case NVMType::PCRAM:   nvsim_cell_->memCellType = PCRAM;     break;
            case NVMType::RERAM:   nvsim_cell_->memCellType = memristor; break;
            case NVMType::SLCNAND: nvsim_cell_->memCellType = SLCNAND;  break;
            case NVMType::MLCNAND: nvsim_cell_->memCellType = MLCNAND;  break;
            case NVMType::FBDRAM:  nvsim_cell_->memCellType = FBRAM;    break;
            default:               nvsim_cell_->memCellType = MRAM;      break;
        }
    }

    // NOTE: Don't set global cell here - do it in runNVSim() to match
    // NVSim's main() initialization order (after Technology is initialized)
}

// Process-wide cache of NVSim characterization results, keyed on the parameters
// the design-space search depends on. Avoids re-running the (~minutes-long)
// search for identical configs -- notably the timing + power paths of a single
// simulation, which build the same config twice.
namespace {
    struct NVMCacheKey {
        int nvm_type; uint64_t capacity_bytes; int process_node_nm; uint32_t word_width_bits;
        /* 1.11.52 (audit D055 + the D-series cache-key findings): TEMPERATURE
         * is a key axis now that power.temperature_k exists. It was safe to
         * omit only while every call site passed 350 K; wiring the knob into
         * the array queries makes two reachable configurations differ in it,
         * which is exactly the LSTP-served-an-HP-entry failure that put
         * device_corner in the key in 1.11.49. */
        /* 1.11.49 (gate 1159H X1): the DEVICE CORNER is part of the result's
         * identity. Without it, an LSTP query silently served the pre-generated
         * HP entry -- the L77 fix compiled and did nothing, and the gate
         * caught it: LSTP leakage == HP leakage exactly. */
        int device_corner;
        int temperature_k;
        /* 1.11.57 (latent D027): THE REST OF THE DESIGN INPUTS.
         *
         * createNVSimInput() feeds NVSim five more things that select a
         * different design point, and none of them were in the key:
         * designTarget/associativity (from is_cache), optimizationTarget (the
         * first-true precedence over the six optimize_* flags), pageSize and
         * flashBlockSize, and fileMemCell (from cell_file). The omission was
         * safe only by coincidence -- every reachable call site happens to pass
         * the same value for all five today -- so the key answered a question
         * it had not been asked the moment any of them was wired to a knob.
         * That is exactly how the 1.11.49 corner bug worked: the fix compiled,
         * did nothing, and served the pre-generated entry instead.
         *
         * opt_target is the RESOLVED target, not the raw flags, because the
         * precedence collapses many flag combinations onto one design point;
         * keying on the resolution avoids splitting the cache for
         * configurations NVSim would characterize identically. */
        int design_target;       // 0 = RAM_chip, 1 = cache
        int associativity;       // as passed to NVSim (forced to 1 for RAM_chip)
        int opt_target;          // 0 rd-lat 1 wr-lat 2 rd-eng 3 wr-eng 4 leak 5 area
        uint64_t page_size_bits;
        uint64_t block_size_bits;
        std::string cell_file;   // "" = the per-type sample cell
        bool operator<(const NVMCacheKey& o) const {
            if (nvm_type != o.nvm_type) return nvm_type < o.nvm_type;
            if (capacity_bytes != o.capacity_bytes) return capacity_bytes < o.capacity_bytes;
            if (process_node_nm != o.process_node_nm) return process_node_nm < o.process_node_nm;
            if (temperature_k != o.temperature_k) return temperature_k < o.temperature_k;
            if (word_width_bits != o.word_width_bits) return word_width_bits < o.word_width_bits;
            if (device_corner != o.device_corner) return device_corner < o.device_corner;
            if (design_target != o.design_target) return design_target < o.design_target;
            if (associativity != o.associativity) return associativity < o.associativity;
            if (opt_target != o.opt_target) return opt_target < o.opt_target;
            if (page_size_bits != o.page_size_bits) return page_size_bits < o.page_size_bits;
            if (block_size_bits != o.block_size_bits) return block_size_bits < o.block_size_bits;
            return cell_file < o.cell_file;
        }
    };
    struct NVMCacheVal {
        double read_latency_s, write_latency_s, read_energy_nj,
               write_energy_nj, leakage_mw, area_mm2;
        /* 1.11.25: the sub-bank ladder. NVSim resolves subarray and mat
         * latency, and the tier model needs them -- but characterization is
         * slow, so the results are PREGENERATED and a run normally reads this
         * cache rather than NVSim. If the ladder is not in the cache it is
         * not available at all, which is why it lives here and not only in
         * the result tree. Default -1 = absent, e.g. an older cache file:
         * the tier is then reported unsourceable, never filled. */
        double subarray_latency_s = -1.0;
        double mat_latency_s = -1.0;
        /* 1.11.73 (one level below the bank): the MAT is the NVM tier below
         * the bank, so its width and count must survive the cache like its
         * latency does. -1 = absent (older cache file): the tier's width and
         * count are then reported unsourceable, never filled. */
        int    mat_width_bits = -1;   // bank->mat.numDataBit
        int    mats_per_bank  = -1;   // bank->numRowMat x numColumnMat
    };
    static std::map<NVMCacheKey, NVMCacheVal> g_nvsim_cache;

    // Persistent (on-disk, XML) NVSim characterization cache. NVSim's design-space
    // search costs minutes; each sweep cell is a separate process, so an in-memory
    // cache alone does not help across cells. We persist results to one XML file
    // per (type, capacity, node) under ~/.cache/pimid/nvsim so the first cell
    // characterizes a config and all later cells (and the power path) reuse it.
    static std::string nvsimCacheDir() {
        // Routed through the centralized cache warehouse so --cache-dir /
        // PIMID_CACHE_DIR relocate the nvsim cache automatically (and the
        // backend subdir is mkdir -p'd for us).
        return pimid::cache::backendDir("nvsim");
    }
    /* 1.11.57 (latent D033): ONE identity string for a characterization, built
     * from the WHOLE key, used by both the cache filename and the manifest.
     *
     * The manifest key used to be assembled by hand from three fields
     * (type, capacity, node) while the filename already carried more, so two
     * genuinely different characterizations -- different width, corner,
     * temperature, optimization target -- landed on one manifest identity, and
     * a reader that dedups by (backend, key) taking the last line silently
     * dropped all but one of them. That is the same failure mode the cache key
     * itself had before 1.11.49/1.11.52/1.11.57: an entry answering a question
     * it was not asked. Two hand-maintained field lists was the root cause, so
     * there is now only one; anything added to NVMCacheKey has to be added
     * here, and the manifest cannot drift behind the cache again. */
    static std::string nvsimCacheStem(const NVMCacheKey& k) {
        std::ostringstream os;
        os << "nvm_t" << k.nvm_type
           << "_c" << k.capacity_bytes << "_n" << k.process_node_nm
           << "_w" << k.word_width_bits;
        /* 1.11.49: corner joins the FILENAME for non-HP so the pre-generated
         * HP set keeps its names (no regeneration for the default), while
         * LSTP/LOP can never collide with it. */
        if (k.device_corner != 0) os << "_dc" << k.device_corner;
        /* 1.11.52: same discipline as the corner -- the DEFAULT 350 K keeps
         * the pre-existing filenames (no regeneration of the warmed set),
         * any other temperature gets its own entry. */
        if (k.temperature_k != 350) os << "_t" << k.temperature_k;
        /* 1.11.57 (latent D027): the five newly-keyed inputs join the FILENAME
         * under the same rule the corner and the temperature already use --
         * only a NON-DEFAULT value appends a suffix, so every entry in the
         * pre-generated set keeps its name and nothing has to be recharacterized
         * for this fix. The defaults are the values every reachable call site
         * passes today: RAM_chip, associativity 1, read-energy-optimized, no
         * page/block size, and the per-type sample cell file. */
        if (k.design_target != 0) os << "_ca" << k.associativity;
        if (k.opt_target != 2)    os << "_o" << k.opt_target;
        if (k.page_size_bits != 0)  os << "_pg" << k.page_size_bits;
        if (k.block_size_bits != 0) os << "_bl" << k.block_size_bits;
        if (!k.cell_file.empty()) {
            // Basename only, and only characters that are safe in a filename.
            std::string base = k.cell_file.substr(k.cell_file.find_last_of('/') + 1);
            std::string safe;
            for (char c : base) {
                safe += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' ||
                         c == '_' || c == '.') ? c : '_';
            }
            os << "_cell" << safe;
        }
        return os.str();
    }
    static std::string nvsimCachePath(const NVMCacheKey& k) {
        return nvsimCacheDir() + "/" + nvsimCacheStem(k) + ".xml";
    }
    // Minimal scalar XML reader (looks for <field>value</field>). Returns true on
    // a complete, well-formed hit.
    static bool nvsimDiskLoad(const NVMCacheKey& k, NVMCacheVal& v) {
        std::string path = nvsimCachePath(k);
        std::ifstream f(path);
        /* 1.11.52 (user ruling: the cache ships with pimid, initially empty):
         * on a miss in the TREE store, consult the pre-1.11.52 per-user
         * location once. A characterization costs a full design-space search
         * (584k-1.16M valid points in this tree's own logs), so silently
         * regenerating entries that already exist would be the expensive
         * kind of wrong. A hit is copied forward and announced; the legacy
         * store is never written. */
        std::string from_legacy;
        if (!f.good()) {
            const std::string ldir = pimid::cache::legacyBackendDir("nvsim");
            if (ldir.empty()) return false;
            std::string base = path.substr(path.find_last_of('/') + 1);
            from_legacy = ldir + "/" + base;
            f.open(from_legacy);
            if (!f.good()) return false;
        }
        std::string xml((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (!from_legacy.empty()) {
            std::ofstream out(path);
            if (out.good()) {
                out << xml;
                std::cout << "[NVSimWrapper] migrated cached characterization "
                          << "into the tree cache (" << path << ")" << std::endl;
            }
        }
        auto get = [&](const char* tag, double& out) -> bool {
            std::string open = std::string("<") + tag + ">";
            std::string close = std::string("</") + tag + ">";
            size_t a = xml.find(open); if (a == std::string::npos) return false;
            a += open.size();
            size_t b = xml.find(close, a); if (b == std::string::npos) return false;
            try { out = std::stod(xml.substr(a, b - a)); } catch (...) { return false; }
            return true;
        };
        /* 1.11.57 (latent D034): THE STORED RECORD IS CHECKED AGAINST THE KEY
         * THAT ASKED FOR IT.
         *
         * The XML used to carry three of the key's fields (type, capacity,
         * node) while the FILENAME carried all of them, so a cache file that
         * had been renamed, hand-copied or restored into the wrong name was
         * undetectable: the load matched on the name, the body could not
         * contradict it, and the run priced one configuration with another
         * one's characterization. cache/README.md states the opposite
         * discipline in so many words -- "The filename IS the key: every input
         * that changes the characterization must appear in it" -- which the
         * filename honoured and the contents did not.
         *
         * nvsimDiskStore now writes every key field (see below), and this is
         * the matching read side: any key field PRESENT in the file must agree
         * with the key that was asked for, or the entry is refused. Absence is
         * not a failure -- a pre-1.11.57 file simply carries fewer fields and
         * is still trusted on its filename, exactly as before -- so no warmed
         * entry has to be recharacterized for this fix. A file rewritten by
         * this version onwards is self-describing and a mismatch is loud. */
        auto agrees = [&](const char* tag, double expected) -> bool {
            double found = 0.0;
            if (!get(tag, found)) return true;        // older file: field absent
            if (found == expected) return true;
            std::cerr << "[NVSimWrapper] REFUSING cached characterization "
                      << path << ": its <" << tag << "> is " << found
                      << " but this query asks for " << expected
                      << ". The file does not describe the configuration its "
                         "name claims -- it has been renamed, copied or "
                         "restored into the wrong entry. Recharacterizing."
                      << std::endl;
            return false;
        };
        auto getStr = [&](const char* tag, std::string& out) -> bool {
            std::string open = std::string("<") + tag + ">";
            std::string close = std::string("</") + tag + ">";
            size_t a = xml.find(open); if (a == std::string::npos) return false;
            a += open.size();
            size_t b = xml.find(close, a); if (b == std::string::npos) return false;
            out = xml.substr(a, b - a);
            return true;
        };
        /* 1.11.60 (audit round 4, C013): THE PRODUCING VERSION IS READ, so the
         * stamp stops being a line of manifest text nobody consults.
         *
         * 1.11.57 bumped cache::kToolVersion to close its own C014, whose note
         * names the real trap precisely: "the semantic change that does NOT
         * move the filename". But the string reached exactly one place -- the
         * manifest JSON in cache_warehouse.cpp -- and no load path anywhere
         * read it, so bumping it changed nothing about which entry gets
         * served. This loader verified eleven key fields and the cell file and
         * not the version that produced them, which is the one check the trap
         * requires: a release that changes what a characterization MEANS,
         * without changing any keyed input, produces a file whose name and
         * whose eleven fields all still match.
         *
         * Absence is trusted, deliberately. Every entry in the shipped warm
         * set predates this field, and refusing them would recharacterize the
         * whole store -- minutes per cell, for entries that are not known to
         * be wrong. So the check binds forward: files written from 1.11.60
         * onwards carry their version and a mismatch refuses them. Entries
         * older than that still ride on their filename, and the legacy
         * copy-forward above still imports them on a name match, exactly as
         * the finding says. That half is closed by regeneration, not by code.
         */
        {
            std::string ver;
            if (getStr("tool_version", ver) && ver != pimid::cache::toolVersion()) {
                std::cerr << "[NVSimWrapper] REFUSING cached characterization "
                          << path << ": it was produced by PIMID " << ver
                          << " and this build's characterization semantics are "
                          << pimid::cache::toolVersion()
                          << ". The keyed inputs all match -- what changed is "
                             "what the stored numbers MEAN, which no filename "
                             "or key field can express. Recharacterizing."
                          << std::endl;
                return false;
            }
        }
        if (!agrees("nvm_type", k.nvm_type)) return false;
        if (!agrees("capacity_bytes", static_cast<double>(k.capacity_bytes))) return false;
        if (!agrees("process_node_nm", k.process_node_nm)) return false;
        if (!agrees("word_width_bits", static_cast<double>(k.word_width_bits))) return false;
        if (!agrees("device_corner", k.device_corner)) return false;
        if (!agrees("temperature_k", k.temperature_k)) return false;
        if (!agrees("design_target", k.design_target)) return false;
        if (!agrees("associativity", k.associativity)) return false;
        if (!agrees("opt_target", k.opt_target)) return false;
        if (!agrees("page_size_bits", static_cast<double>(k.page_size_bits))) return false;
        if (!agrees("block_size_bits", static_cast<double>(k.block_size_bits))) return false;
        {
            std::string cf;
            if (getStr("cell_file", cf) && cf != k.cell_file) {
                std::cerr << "[NVSimWrapper] REFUSING cached characterization "
                          << path << ": its <cell_file> is \"" << cf
                          << "\" but this query asks for \"" << k.cell_file
                          << "\". Recharacterizing." << std::endl;
                return false;
            }
        }

        const bool core = get("read_latency_s", v.read_latency_s)
            && get("write_latency_s", v.write_latency_s)
            && get("read_energy_nj", v.read_energy_nj)
            && get("write_energy_nj", v.write_energy_nj)
            && get("leakage_mw", v.leakage_mw)
            && get("area_mm2", v.area_mm2);
        if (!core) return false;
        /* 1.11.25: the sub-bank ladder is OPTIONAL on read. A cache written
         * before this release does not carry it, and re-characterizing every
         * pregenerated cell to obtain it would cost minutes per cell. Absent
         * leaves the fields at -1 and the subarray/mat tiers are reported
         * unsourceable -- the run still works, it simply cannot price those
         * placements until the cache is regenerated. Never fabricated. */
        if (!get("subarray_latency_s", v.subarray_latency_s)) v.subarray_latency_s = -1.0;
        if (!get("mat_latency_s", v.mat_latency_s)) v.mat_latency_s = -1.0;
        { double t; v.mat_width_bits = get("mat_width_bits", t) ? static_cast<int>(t) : -1; }
        { double t; v.mats_per_bank  = get("mats_per_bank",  t) ? static_cast<int>(t) : -1; }
        return true;
    }
    static void nvsimDiskStore(const NVMCacheKey& k, const NVMCacheVal& v) {
        std::string dir = nvsimCacheDir();
        // best-effort mkdir -p (~/.cache, ~/.cache/pimid, ~/.cache/pimid/nvsim)
        std::string acc;
        for (size_t i = 0; i <= dir.size(); ++i) {
            if (i == dir.size() || dir[i] == '/') {
                if (!acc.empty() && acc != "/") {
                    struct stat st; if (stat(acc.c_str(), &st) != 0) (void)mkdir(acc.c_str(), 0755);
                }
            }
            if (i < dir.size()) acc += dir[i];
        }
        std::ofstream f(nvsimCachePath(k));
        if (!f.good()) return;
        /* 1.11.57 (latent D034): EVERY KEY FIELD IS IN THE BODY NOW, not just
         * the three that happened to be in the key when this writer was
         * written. The record must let a reader reconstruct exactly which
         * inputs produced these numbers without decoding the filename -- and
         * it must let the loader above catch a file that is not what its name
         * says. Adding fields is backward-compatible in both directions: an
         * older reader ignores what it does not look for, and the newer reader
         * treats an absent field as "not stated" rather than as a mismatch. */
        /* 1.11.60 (audit round 4, C013): the producing version is part of the
         * record now. cache::kToolVersion existed since 1.11.52 and moved at
         * 1.11.57 to close a finding about staleness, but it was written only
         * into the manifest and read by nothing, so it could not refuse a
         * single entry. Written here, it is checked by nvsimDiskLoad(). */
        f << "<nvsim_characterization>\n"
          << "  <tool_version>" << pimid::cache::toolVersion() << "</tool_version>\n"
          << "  <nvm_type>" << k.nvm_type << "</nvm_type>\n"
          << "  <capacity_bytes>" << k.capacity_bytes << "</capacity_bytes>\n"
          << "  <process_node_nm>" << k.process_node_nm << "</process_node_nm>\n"
          << "  <word_width_bits>" << k.word_width_bits << "</word_width_bits>\n"
          << "  <device_corner>" << k.device_corner << "</device_corner>\n"
          << "  <temperature_k>" << k.temperature_k << "</temperature_k>\n"
          << "  <design_target>" << k.design_target << "</design_target>\n"
          << "  <associativity>" << k.associativity << "</associativity>\n"
          << "  <opt_target>" << k.opt_target << "</opt_target>\n"
          << "  <page_size_bits>" << k.page_size_bits << "</page_size_bits>\n"
          << "  <block_size_bits>" << k.block_size_bits << "</block_size_bits>\n"
          << "  <cell_file>" << k.cell_file << "</cell_file>\n"
          << "  <read_latency_s>" << v.read_latency_s << "</read_latency_s>\n"
          << "  <write_latency_s>" << v.write_latency_s << "</write_latency_s>\n"
          << "  <read_energy_nj>" << v.read_energy_nj << "</read_energy_nj>\n"
          << "  <write_energy_nj>" << v.write_energy_nj << "</write_energy_nj>\n"
          << "  <leakage_mw>" << v.leakage_mw << "</leakage_mw>\n"
          << "  <area_mm2>" << v.area_mm2 << "</area_mm2>\n"
          << "  <subarray_latency_s>" << v.subarray_latency_s << "</subarray_latency_s>\n"
          << "  <mat_latency_s>" << v.mat_latency_s << "</mat_latency_s>\n"
          << "  <mat_width_bits>" << v.mat_width_bits << "</mat_width_bits>\n"
          << "  <mats_per_bank>" << v.mats_per_bank << "</mats_per_bank>\n"
          << "</nvsim_characterization>\n";
    }
}

void NVSimWrapper::runNVSim() {
    // Cache short-circuit: if this exact (type, capacity, node) was characterized
    // before, reuse the scalar outputs and skip the expensive design-space search.
    /* 1.11.57 (latent D027): resolve the optimization target with EXACTLY the
     * precedence createNVSimInput() uses, so the key records the design point
     * NVSim will actually search for rather than the flag soup that selects
     * it. Keep the two in step if that precedence ever changes. */
    const int opt_target = config_.optimize_read_latency  ? 0
                         : config_.optimize_write_latency ? 1
                         : config_.optimize_read_energy   ? 2
                         : config_.optimize_write_energy  ? 3
                         : config_.optimize_leakage       ? 4
                         : config_.optimize_area          ? 5
                                                          : 2;  // default branch
    NVMCacheKey key{ (int)config_.nvm_type, config_.capacity_bytes,
                     config_.process_node_nm, config_.word_width_bits,
                     config_.device_corner,          // 1.11.49
                     config_.temperature_k,          // 1.11.52 (D055)
                     config_.is_cache ? 1 : 0,       // 1.11.57 (D027)
                     config_.is_cache ? config_.associativity : 1,
                     opt_target,
                     config_.page_size_bits,
                     config_.block_size_bits,
                     config_.cell_file };
    // Reads (both the in-memory map and the on-disk XML) are skipped unless the
    // warehouse mode permits reading -- OFF/WO must truly recompute.
    if (pimid::cache::readEnabled()) {
        // 1) In-memory cache (same process, e.g. timing + power paths).
        auto it = g_nvsim_cache.find(key);
        NVMCacheVal v;
        bool hit = false;
        if (it != g_nvsim_cache.end()) { v = it->second; hit = true; }
        // 2) On-disk XML cache (across sweep processes). Promote into memory.
        else if (nvsimDiskLoad(key, v)) { g_nvsim_cache[key] = v; hit = true;
            std::cout << "[NVSimWrapper] Loaded NVSim characterization from disk cache "
                      << nvsimCachePath(key) << std::endl;
        }
        if (hit) {
            cached_ = true;
            cached_read_latency_s_  = v.read_latency_s;
            cached_write_latency_s_ = v.write_latency_s;
            cached_subarray_latency_s_ = v.subarray_latency_s;
            cached_mat_latency_s_ = v.mat_latency_s;
            cached_mat_width_bits_ = v.mat_width_bits;     // 1.11.73
            cached_mats_per_bank_  = v.mats_per_bank;
            cached_read_energy_nj_  = v.read_energy_nj;
            cached_write_energy_nj_ = v.write_energy_nj;
            cached_leakage_mw_      = v.leakage_mw;
            cached_area_mm2_        = v.area_mm2;
            valid_ = true;
            std::cout << "[NVSimWrapper] Reusing cached NVSim characterization "
                      << "(type=" << (int)config_.nvm_type
                      << ", " << (config_.capacity_bytes / (1024*1024)) << "MB, "
                      << config_.process_node_nm << "nm)" << std::endl;
            return;
        }
    }
    try {
        std::cout << "[NVSimWrapper] Running NVSim analysis..." << std::endl;

        // Set global pointers in the same order as NVSim's main():
        // 1. inputParameter  2. tech  3. cell  4. localWire/globalWire

        inputParameter = nvsim_input_;

        // Set search bounds (equivalent to RESTORE_SEARCH_SIZE macro)
        RESTORE_SEARCH_SIZE;

        // Initialize global Technology object
        if (tech == nullptr) {
            tech = new Technology();
        }
        tech->Initialize(nvsim_input_->processNode, nvsim_input_->deviceRoadmap);

        // Technology interpolation for intermediate nodes (matching NVSim main.cpp:97-123)
        int pn = nvsim_input_->processNode;
        Technology techHigh;
        double alpha = 0;
        if (pn > 200) {
            // No interpolation for > 200nm
        } else if (pn > 120) {
            techHigh.Initialize(200, nvsim_input_->deviceRoadmap);
            alpha = (pn - 120.0) / 60;
        } else if (pn > 90) {
            techHigh.Initialize(120, nvsim_input_->deviceRoadmap);
            alpha = (pn - 90.0) / 30;
        } else if (pn > 65) {
            techHigh.Initialize(90, nvsim_input_->deviceRoadmap);
            alpha = (pn - 65.0) / 25;
        } else if (pn > 45) {
            techHigh.Initialize(65, nvsim_input_->deviceRoadmap);
            alpha = (pn - 45.0) / 20;
        } else if (pn >= 32) {
            techHigh.Initialize(45, nvsim_input_->deviceRoadmap);
            alpha = (pn - 32.0) / 13;
        } else if (pn >= 22) {
            techHigh.Initialize(32, nvsim_input_->deviceRoadmap);
            alpha = (pn - 22.0) / 10;
        }
        tech->InterpolateWith(techHigh, alpha);

        // Set global cell pointer
        cell = nvsim_cell_;

        // Initialize global Wire objects
        if (localWire == nullptr) {
            localWire = new Wire();
        }
        if (globalWire == nullptr) {
            globalWire = new Wire();
        }

        // Now safe to create Result - all global dependencies are initialized
        nvsim_result_ = new Result();

        // --- BIGFOR design-space search (replicating NVSim main.cpp:286-302) ---
        // Declare loop variables used by BIGFOR macro
        int numRowMat, numColumnMat, numActiveMatPerRow, numActiveMatPerColumn;
        int numRowSubarray, numColumnSubarray, numActiveSubarrayPerRow, numActiveSubarrayPerColumn;
        int muxSenseAmp, muxOutputLev1, muxOutputLev2, numRowPerSet;
        int areaOptimizationLevel;
        int localWireType, globalWireType;
        int localWireRepeaterType, globalWireRepeaterType;
        int isLocalWireLowSwing, isGlobalWireLowSwing;

        long long capacity = (long long)nvsim_input_->capacity * 8;  // bytes -> bits
        long blockSize = nvsim_input_->wordWidth;
        int associativity = nvsim_input_->associativity;

        // Adjust block size for SLC NAND or DRAM memory chip
        if (nvsim_input_->designTarget == RAM_chip &&
            (cell->memCellType == SLCNAND || cell->memCellType == DRAM)) {
            blockSize = nvsim_input_->pageSize;
            associativity = 1;
        }

        // Best results array (one per optimization target)
        Result bestDataResults[(int)full_exploration];
        for (int i = 0; i < (int)full_exploration; i++)
            bestDataResults[i].optimizationTarget = (OptimizationTarget)i;

        Bank *dataBank;
        long long numSolution = 0;

        // Suppress NVSim output during search
        std::streambuf* orig_cout = std::cout.rdbuf();
        std::streambuf* orig_cerr = std::cerr.rdbuf();
        std::ostringstream suppress;
        std::cout.rdbuf(suppress.rdbuf());
        std::cerr.rdbuf(suppress.rdbuf());

        INITIAL_BASIC_WIRE;
        BIGFOR {
            if (blockSize / (numActiveMatPerRow * numActiveMatPerColumn *
                             numActiveSubarrayPerRow * numActiveSubarrayPerColumn) == 0) {
                continue;  // Too aggressive partitioning
            }
            CALCULATE(dataBank, data_array);
            if (!dataBank->invalid) {
                Result tempResult;
                numSolution++;
                *(tempResult.bank) = *dataBank;
                *(tempResult.localWire) = *localWire;
                *(tempResult.globalWire) = *globalWire;
                for (int i = 0; i < (int)full_exploration; i++)
                    bestDataResults[i].compareAndUpdate(tempResult);
            }
            delete dataBank;
        }

        // Wire refinement pass (matching NVSim main.cpp:308-319)
        if (numSolution > 0) {
            Bank *trialBank;
            Result tempResult;
            REFINE_LOCAL_WIRE_FORLOOP {
                localWire->Initialize(nvsim_input_->processNode, (WireType)localWireType,
                        (WireRepeaterType)localWireRepeaterType, nvsim_input_->temperature,
                        (bool)isLocalWireLowSwing);
                for (int i = 0; i < (int)full_exploration; i++) {
                    LOAD_GLOBAL_WIRE(bestDataResults[i]);
                    TRY_AND_UPDATE(bestDataResults[i], data_array);
                }
            }
            REFINE_GLOBAL_WIRE_FORLOOP {
                globalWire->Initialize(nvsim_input_->processNode, (WireType)globalWireType,
                        (WireRepeaterType)globalWireRepeaterType, nvsim_input_->temperature,
                        (bool)isGlobalWireLowSwing);
                for (int i = 0; i < (int)full_exploration; i++) {
                    LOAD_LOCAL_WIRE(bestDataResults[i]);
                    TRY_AND_UPDATE(bestDataResults[i], data_array);
                }
            }
        }

        // Restore stdout/stderr
        std::cout.rdbuf(orig_cout);
        std::cerr.rdbuf(orig_cerr);

        // Store the result matching the user's optimization target
        if (numSolution > 0) {
            int optIdx = (int)nvsim_input_->optimizationTarget;
            if (optIdx >= (int)full_exploration) optIdx = (int)read_latency_optimized;
            *(nvsim_result_->bank) = *(bestDataResults[optIdx].bank);
            *(nvsim_result_->localWire) = *(bestDataResults[optIdx].localWire);
            *(nvsim_result_->globalWire) = *(bestDataResults[optIdx].globalWire);
            valid_ = true;
            std::cout << "[NVSimWrapper] NVSim found " << numSolution << " valid design points" << std::endl;
            // Populate both caches so identical future configs (the power path of
            // this run, and every other sweep process) skip the expensive search.
            /* 1.11.25: capture the sub-bank ladder too. This is the ONLY
             * moment it is available -- characterization is slow and every
             * later run reads the cache, so a field not stored here does not
             * exist for the corpus. Read from the result tree directly (the
             * accessors would consult the not-yet-populated cache members). */
            NVMCacheVal v{ getReadLatency(), getWriteLatency(),
                           getReadDynamicEnergy(), getWriteDynamicEnergy(),
                           getLeakagePower(), getArea() };
            if (nvsim_result_ && nvsim_result_->bank) {
                const double sub = nvsim_result_->bank->mat.subarray.readLatency;
                const double mat = nvsim_result_->bank->mat.readLatency;
                v.subarray_latency_s = (sub > 0.0) ? sub : -1.0;
                v.mat_latency_s      = (mat > 0.0) ? mat : -1.0;
                // 1.11.73: the mat's width and the bank's mat count, from NVSim.
                const long mw = nvsim_result_->bank->mat.numDataBit;
                const int  mm = nvsim_result_->bank->numRowMat * nvsim_result_->bank->numColumnMat;
                v.mat_width_bits = (mw > 0) ? static_cast<int>(mw) : -1;
                v.mats_per_bank  = (mm > 0) ? mm : -1;
            }
            // Persist only when the warehouse mode permits writing (RW/WO).
            // The in-memory map is process-local but gated too for consistency.
            if (pimid::cache::writeEnabled()) {
                g_nvsim_cache[key] = v;
                nvsimDiskStore(key, v);
            }
            // Record a manifest entry for this fresh characterization. This is a
            // no-op when writes are disabled, so it's safe to call unconditionally.
            {
                /* 1.11.57 (latent D033): the manifest identity IS the cache
                 * file stem now -- literally the same string, so the record
                 * names the exact file on disk it describes and can never
                 * again be coarser than the key that produced it. The key was
                 * "t<type>_c<cap>_n<node>", which merged every width, corner,
                 * temperature, design target and optimization target into one
                 * provenance row. The stem keeps the tree's suffix rule (a
                 * suffix appears only for a non-default value), so a
                 * default-configuration row reads almost as it did before,
                 * gaining only the "nvm_" prefix and the width that was always
                 * in the filename. params carries every key field spelled out,
                 * so a provenance audit does not have to parse the stem. */
                std::ostringstream keyss, paramss, valss;
                keyss << nvsimCacheStem(key);
                paramss << "\"nvm_type\":" << key.nvm_type
                        << ",\"capacity_bytes\":" << key.capacity_bytes
                        << ",\"process_node_nm\":" << key.process_node_nm
                        << ",\"word_width_bits\":" << key.word_width_bits
                        << ",\"device_corner\":" << key.device_corner
                        << ",\"temperature_k\":" << key.temperature_k
                        << ",\"design_target\":" << key.design_target
                        << ",\"associativity\":" << key.associativity
                        << ",\"opt_target\":" << key.opt_target
                        << ",\"page_size_bits\":" << key.page_size_bits
                        << ",\"block_size_bits\":" << key.block_size_bits
                        << ",\"cell_file\":\"" << key.cell_file << "\"";
                valss << "\"read_latency_s\":" << v.read_latency_s
                      << ",\"write_latency_s\":" << v.write_latency_s
                      << ",\"read_energy_nj\":" << v.read_energy_nj
                      << ",\"write_energy_nj\":" << v.write_energy_nj
                      << ",\"leakage_mw\":" << v.leakage_mw
                      << ",\"area_mm2\":" << v.area_mm2;
                pimid::cache::recordManifest("nvsim", keyss.str(),
                                             paramss.str(), valss.str());
            }
        } else {
            valid_ = false;
            error_message_ = "NVSim found no valid design points for the given configuration";
            std::cerr << "[NVSimWrapper] " << error_message_ << std::endl;
            /* 1.11.52 (found by the corner cache-warming sweep): say WHICH
             * input has no feasible design instead of leaving a bare "no
             * valid design points" for the caller to guess at. MEASURED at
             * 22 nm, 64 KB, widths 512 and 64: PCM characterizes at HP and
             * returns ZERO design points at BOTH low-power corners (4/4
             * failures), while STT-MRAM and ReRAM succeed at all three.
             * That is the expected direction physically -- a PCM RESET
             * needs a high drive current that low-power periphery devices
             * cannot supply -- but the measurement is per (type, corner,
             * node, capacity, width), so this reports the correlation and
             * does not assert it as a law. */
            if (config_.device_corner != 0) {
                std::cerr << "[NVSimWrapper]   the requested DEVICE CORNER is "
                          << (config_.device_corner == 1 ? "LSTP" : "LOP")
                          << ": no array organisation in NVSim's search space "
                             "meets the cell's drive requirement with "
                             "low-power periphery devices. Measured 2026-08-17 "
                             "at 22 nm / 64 KB: PCM fails at LSTP and LOP "
                             "(both widths); STT-MRAM and ReRAM succeed at all "
                             "corners. Use the HP corner for this technology, "
                             "or a technology whose write current the corner "
                             "can drive." << std::endl;
            }
            /* 1.11.56 (D032): THROW. Every query returns 0.0 when !valid_,
             * so a swallowed failure here does not surface as an error --
             * it surfaces as an NVM array of 0.000 nJ read, 0.000 nJ write,
             * 0.000 mW leakage and 0.000 mm^2, which is a fabricated number
             * wearing a real number's units. Callers that can degrade
             * (the model classes, the area query) already catch and say so;
             * the ones that cannot must not be handed a zero. */
            throw std::runtime_error(error_message_);
        }

    } catch (const std::exception& e) {
        if (valid_) {   // not our own no-design-points throw, which already reported
            valid_ = false;
            error_message_ = std::string("NVSim execution failed: ") + e.what();
            std::cerr << "[NVSimWrapper] " << error_message_ << std::endl;
        }
        throw;   // 1.11.56 (D032): see above -- a swallowed failure reads as zeros
    }
}

//=============================================================================
// Query functions
//=============================================================================

double NVSimWrapper::getReadLatency() const {
    if (cached_) return cached_read_latency_s_;
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    return nvsim_result_->bank->readLatency;  // seconds
}

double NVSimWrapper::getWriteLatency() const {
    if (cached_) return cached_write_latency_s_;
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    return nvsim_result_->bank->writeLatency;  // seconds
}
/* 1.11.23: the SET/RESET split, read from NVSim's own bank result rather than
 * asserted as a fraction of the write latency. */
/* 1.11.25: real sub-bank latencies, straight from NVSim's result tree. */
double NVSimWrapper::getSubarrayLatency() const {
    if (cached_) return cached_subarray_latency_s_;   // -1 when the cache predates it
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return -1.0;
    double v = nvsim_result_->bank->mat.subarray.readLatency;
    return (v > 0.0) ? v : -1.0;
}

double NVSimWrapper::getMatLatency() const {
    if (cached_) return cached_mat_latency_s_;
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return -1.0;
    double v = nvsim_result_->bank->mat.readLatency;
    return (v > 0.0) ? v : -1.0;
}

double NVSimWrapper::getSetLatency() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return -1.0;
    double v = nvsim_result_->bank->setLatency;
    return (v > 0.0) ? v : -1.0;
}

double NVSimWrapper::getResetLatency() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return -1.0;
    double v = nvsim_result_->bank->resetLatency;
    return (v > 0.0) ? v : -1.0;
}


double NVSimWrapper::getReadDynamicEnergy() const {
    if (cached_) return cached_read_energy_nj_;
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    return nvsim_result_->bank->readDynamicEnergy * 1e9;  // J -> nJ
}

double NVSimWrapper::getWriteDynamicEnergy() const {
    if (cached_) return cached_write_energy_nj_;
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    return nvsim_result_->bank->writeDynamicEnergy * 1e9;  // J -> nJ
}

double NVSimWrapper::getLeakagePower() const {
    if (cached_) return cached_leakage_mw_;
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    return nvsim_result_->bank->leakage * 1000.0;  // W -> mW
}

double NVSimWrapper::getReadEDP() const {
    return getReadDynamicEnergy() * (getReadLatency() * 1e9);  // Energy * Delay
}

double NVSimWrapper::getWriteEDP() const {
    return getWriteDynamicEnergy() * (getWriteLatency() * 1e9);  // Energy * Delay
}

double NVSimWrapper::getArea() const {
    if (cached_) return cached_area_mm2_;
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    return nvsim_result_->bank->area * 1e6;  // m^2 -> mm^2
}

double NVSimWrapper::getHeight() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    return nvsim_result_->bank->height * 1e3;  // m -> mm
}

double NVSimWrapper::getWidth() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    return nvsim_result_->bank->width * 1e3;  // m -> mm
}

double NVSimWrapper::getCellReadLatency() const {
    if (!valid_ || !nvsim_cell_) return 0.0;
    // For NVM cells, use setPulse as "read" time (voltage/current sensing)
    return nvsim_cell_->setPulse;  // seconds
}

double NVSimWrapper::getCellWriteLatency() const {
    if (!valid_ || !nvsim_cell_) return 0.0;
    // Use resetPulse for write (typically the slower operation)
    return nvsim_cell_->resetPulse;  // seconds
}

double NVSimWrapper::getCellArea() const {
    if (!valid_ || !nvsim_cell_) return 0.0;
    // cell->area is in units of F^2 (feature size squared)
    double F = nvsim_cell_->processNode * 1e-3;  // nm -> um
    return nvsim_cell_->area * F * F;  // um^2
}

uint32_t NVSimWrapper::getNumRows() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0;
    // Total rows = numRowMat * numRowSubarray * subarray rows
    return nvsim_result_->bank->numRowMat *
           nvsim_result_->bank->numRowSubarray *
           nvsim_result_->bank->mat.subarray.numRow;
}

uint32_t NVSimWrapper::getNumColumns() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0;
    return nvsim_result_->bank->numColumnMat *
           nvsim_result_->bank->numColumnSubarray *
           nvsim_result_->bank->mat.subarray.numColumn;
}

uint32_t NVSimWrapper::getNumBanks() const {
    return 1;  // NVSim models a single bank
}

double NVSimWrapper::getReadBandwidth() const {
    if (getReadLatency() == 0) return 0.0;
    double bytes_per_access = config_.word_width_bits / 8.0;
    return bytes_per_access / getReadLatency() / 1e9;  // GB/s
}

double NVSimWrapper::getWriteBandwidth() const {
    if (getWriteLatency() == 0) return 0.0;
    double bytes_per_access = config_.word_width_bits / 8.0;
    return bytes_per_access / getWriteLatency() / 1e9;  // GB/s
}

bool NVSimWrapper::isValid() const {
    return valid_;
}

/* 1.11.57 (latent C011): see the header. valid_ alone does not mean there is a
 * result tree -- a cache hit sets valid_ = true and leaves nvsim_result_ null,
 * and the cache is the normal path. Every per-component accessor in this file
 * needs the tree, so this is the one predicate a caller must consult before
 * attributing a component number to NVSim. */
bool NVSimWrapper::hasComponentBreakdown() const {
    return valid_ && nvsim_result_ != nullptr && nvsim_result_->bank != nullptr;
}

std::string NVSimWrapper::getErrorMessage() const {
    return error_message_;
}

//=============================================================================
// Subarray-Level Characteristics
//=============================================================================

/* 1.11.56 (audit D031): THE BREAKDOWN IS NVSIM'S OWN, NOT A PERCENTAGE SPLIT.
 *
 * These six accessors used to return fixed fractions of mat.readLatency
 * (0.10 / 0.20 / 0.45 / 0.15 / 0.05 / 0.05). Nothing about a particular array
 * reached them: change the cell, the node, the mux ratio or the aspect ratio
 * and every component moved in exact lockstep, because they were one number
 * scaled six ways. They also summed to 0.95, so the "breakdown" did not even
 * reproduce the total it was cut from. The extractors assign them into
 * inner_bank.row_decoder_ns / wordline_ns / bitline_read_ns / sense_amp_ns /
 * column_mux_ns, which the NVM models sum and PRINT as "Inner-bank read
 * latency" -- an invented split presented as tool output.
 *
 * NVSim resolves all of it. SubArray::CalculateLatency() composes
 *
 *   subarray.readLatency = MAX(rowDecoder.readLatency, columnDecoderLatency)
 *                        + bitlineDelay + bitlineMux.readLatency
 *                        + senseAmp.readLatency
 *                        + senseAmpMuxLev1.readLatency
 *                        + senseAmpMuxLev2.readLatency
 *
 * and Mat::CalculateLatency() adds predecoderLatency on top. The accessors
 * below read those terms directly, so the five delay components an extractor
 * sums reconstruct the array NVSim characterized instead of 95% of a single
 * scalar, and each one now responds to the array it describes.
 *
 * Two structural quirks, stated rather than papered over:
 *  - There is no separate wordline term. NVSim charges the wordline INSIDE the
 *    row decoder -- RowDecoder::capLoad is documented as "Load capacitance,
 *    i.e. wordline capacitance" -- so getWordlineDelay() returns 0 and
 *    getDecoderDelay() carries predecode + decode + wordline drive. Splitting
 *    a number NVSim never separated is what produced this finding.
 *  - columnDecoderLatency is MAX'd against the row decoder, not added, so it
 *    is not the additive column term. The additive column path is the mux data
 *    path (bitline mux + both sense-amp mux levels), which is what
 *    getColumnDecoderDelay() returns.
 *
 * All return SECONDS, and 0.0 when there is no result to read -- notably on a
 * cache hit, where the pregenerated NVSim cache carries only the top-level
 * figures. That is the pre-existing contract for these six and is unchanged.
 */
double NVSimWrapper::getDecoderDelay() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    const auto& mat = nvsim_result_->bank->mat;
    // Predecode + row decode + wordline drive: NVSim's row-access path.
    return mat.predecoderLatency + mat.subarray.rowDecoder.readLatency;
}

double NVSimWrapper::getWordlineDelay() const {
    /* Folded into the row decoder above (RowDecoder drives the wordline and
     * its capLoad IS the wordline capacitance). Reporting a separate term here
     * would double-count the same picoseconds. */
    return 0.0;
}

double NVSimWrapper::getBitlineDelay() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    return nvsim_result_->bank->mat.subarray.bitlineDelay;
}

double NVSimWrapper::getSenseAmpDelay() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    return nvsim_result_->bank->mat.subarray.senseAmp.readLatency;
}

double NVSimWrapper::getColumnDecoderDelay() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    const auto& sub = nvsim_result_->bank->mat.subarray;
    // The ADDITIVE column path: bitline mux + both sense-amp mux levels.
    return sub.bitlineMux.readLatency
         + sub.senseAmpMuxLev1.readLatency
         + sub.senseAmpMuxLev2.readLatency;
}

double NVSimWrapper::getPrechargeDelay() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    return nvsim_result_->bank->mat.subarray.precharger.readLatency;
}

uint32_t NVSimWrapper::getSubarrayRows() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0;
    return nvsim_result_->bank->mat.subarray.numRow;
}

uint32_t NVSimWrapper::getSubarrayCols() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0;
    return nvsim_result_->bank->mat.subarray.numColumn;
}

uint32_t NVSimWrapper::getSubarraysPerMat() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0;
    return nvsim_result_->bank->numRowSubarray *
           nvsim_result_->bank->numColumnSubarray;
}

/* 1.11.73: cache-aware, like getMatLatency(). From the cache an older file
 * yields -1 -> 0 here, and the caller reports the count unsourceable. */
uint32_t NVSimWrapper::getMatsPerBank() const {
    if (cached_) return (cached_mats_per_bank_ > 0) ? static_cast<uint32_t>(cached_mats_per_bank_) : 0;
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0;
    const int mm = nvsim_result_->bank->numRowMat * nvsim_result_->bank->numColumnMat;
    return (mm > 0) ? static_cast<uint32_t>(mm) : 0;
}
uint32_t NVSimWrapper::getMatWidthBits() const {
    if (cached_) return (cached_mat_width_bits_ > 0) ? static_cast<uint32_t>(cached_mat_width_bits_) : 0;
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0;
    const long mw = nvsim_result_->bank->mat.numDataBit;
    return (mw > 0) ? static_cast<uint32_t>(mw) : 0;
}

uint32_t NVSimWrapper::getNumSenseAmps() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0;
    // SA count = subarray cols / mux ratio
    int mux = nvsim_result_->bank->muxSenseAmp;
    if (mux <= 0) mux = 1;
    return getSubarrayCols() / mux;
}

double NVSimWrapper::getWordlineLength() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    // Mat width gives the physical extent of the wordline
    return nvsim_result_->bank->mat.width;  // meters
}

double NVSimWrapper::getBitlineLength() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    return nvsim_result_->bank->mat.height;  // meters
}

/* 1.11.57 (latent D030): THE FALLBACK WIRE CONSTANTS NOW STATE THE UNIT THEY
 * ARE ACTUALLY IN.
 *
 * The audit flagged the wordline capacitance fallback as "1000x its own
 * comment", and it was -- but the disagreement is the COMMENT's, not the
 * number's, and the honest fix is therefore the comment. The lengths these
 * four multiply are METRES (getWordlineLength()/getBitlineLength() return
 * bank->mat.width/height, NVSim's own SI fields), so the coefficients are
 * per-metre and must match NVSim's Wire::capWirePerUnit / resWirePerUnit,
 * documented in external/nvsim/Wire.h:90-91 as ohm/m and F/m.
 *
 * WAS: `0.2e-9` annotated "0.2 fF/nm". 0.2 fF/nm is 2e-7 F/m -- a thousand
 * times the coefficient written beside it, and physically absurd: NVSim's own
 * CalculateWireCapacitance (external/nvsim/formula.cpp:362) is a sum of two
 * terms of order 2*PERMITTIVITY*k*(aspect ratio), i.e. some 1e-10 F/m, and
 * Wire.cpp:788 prints capWirePerUnit/1e6 under the label "F/um" for exactly
 * that reason.
 * IS: the same `0.2e-9`, annotated 0.2 fF/um (= 0.2 pF/mm), which is what
 * 0.2e-9 F/m means and the standard local-interconnect figure. The resistance
 * pair reads the same way: 10e6 ohm/m is 10 ohm/um, the right order for a
 * minimum-pitch local wire at these nodes (copper resistivity over a ~30 nm x
 * ~60 nm cross-section). No value changes; only the annotations that would
 * have led the next reader to "correct" a correct number by 1000x.
 *
 * WHY THE ERROR WAS INVISIBLE: these four fallbacks are doubly unreachable.
 * On the fresh-characterization path localWire is initialized by NVSim, so the
 * tool's own per-unit values are used and the fallback branch is dead; on the
 * normal (cached) path nvsim_result_ is null, so the length is 0.0 and the
 * product is 0 whatever the coefficient. On top of that the only consumer of
 * all four is printDetailedResults(), which has no callers. A wrong constant
 * here could not reach a printed number, let alone a reported one. */
double NVSimWrapper::getWordlineCapacitance() const {
    // Estimate from wire model
    if (localWire && localWire->initialized)
        return getWordlineLength() * localWire->capWirePerUnit;
    // fallback: 0.2e-9 F/m = 0.2 fF/um, local wire (see the D030 note above)
    return getWordlineLength() * 0.2e-9;
}

double NVSimWrapper::getWordlineResistance() const {
    if (localWire && localWire->initialized)
        return getWordlineLength() * localWire->resWirePerUnit;
    // fallback: 10e6 ohm/m = 10 ohm/um, local wire
    return getWordlineLength() * 10e6;
}

double NVSimWrapper::getBitlineCapacitance() const {
    if (localWire && localWire->initialized)
        return getBitlineLength() * localWire->capWirePerUnit;
    // fallback: 0.3e-9 F/m = 0.3 fF/um (bitlines run denser than wordlines)
    return getBitlineLength() * 0.3e-9;
}

double NVSimWrapper::getBitlineResistance() const {
    if (localWire && localWire->initialized)
        return getBitlineLength() * localWire->resWirePerUnit;
    // fallback: 15e6 ohm/m = 15 ohm/um
    return getBitlineLength() * 15e6;
}

/* 1.11.57 (latent D059): THE ENERGY BREAKDOWN IS NVSIM'S OWN NOW, TOO.
 *
 * These eight accessors returned fixed percentages of one mat-level scalar:
 * 10/20/40/20/10 of mat.readDynamicEnergy for the five energies and 15/25/30
 * of mat.leakage for the three leakages. That is precisely the defect D031
 * found in the six DELAY accessors, which 1.11.56 rebuilt on NVSim's own
 * terms; the energy half was left behind because its only consumers
 * (subarray_read_energy_pJ, subarray_leakage_mw) are read by nothing today.
 * A percentage split is not a breakdown: change the cell, the node or the mux
 * ratio and all five moved in exact lockstep, because they were one number
 * scaled five ways -- and they were labelled "NVSim extraction" downstream.
 *
 * NVSim resolves every one of them. SubArray::CalculatePower() composes
 *
 *   subarray.readDynamicEnergy = <array/bitline charge>
 *                              + cellReadEnergy + rowDecoder + bitlineMuxDecoder
 *                              + senseAmpMuxLev1Decoder + senseAmpMuxLev2Decoder
 *                              + precharger + bitlineMux + senseAmp
 *                              + senseAmpMuxLev1 + senseAmpMuxLev2
 *
 * (SubArray.cpp:854-856) and the leakage the same way (:872-874), each term a
 * FunctionUnit with its own readDynamicEnergy and leakage. The accessors below
 * read those terms.
 *
 * Two structural notes, same as the delay side:
 *  - There is no separate WORDLINE term. NVSim charges the wordline inside the
 *    row decoder and even overwrites rowDecoder.readDynamicEnergy with the
 *    computed wordline energy for some cell types (SubArray.cpp:789-792), so
 *    getWordlineEnergy()/getWordlineLeakage() return 0 and the decoder term
 *    carries it. Splitting a number NVSim never separated is the defect.
 *  - The BITLINE term is the only one NVSim does not keep as a named member:
 *    it is the array charge computed before the components are added in. It is
 *    recovered exactly by subtracting the named terms from the subarray total,
 *    which is NVSim's own composition run backwards, not an assertion. Clamped
 *    at 0 so an invalid design point (which sets the total to 1e41) or a NAND
 *    path cannot produce a negative energy.
 *
 * All are per SUBARRAY -- the previous percentages were of a MAT, so the
 * extractors' divide-by-subarray-count is removed alongside this. Energies in
 * nJ, leakages in mW, 0.0 when there is no result tree to read (see
 * hasComponentBreakdown -- notably every cache hit). */
double NVSimWrapper::getDecoderEnergy() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    // Row decode + wordline drive: NVSim folds the wordline in here.
    return nvsim_result_->bank->mat.subarray.rowDecoder.readDynamicEnergy * 1e9;
}

double NVSimWrapper::getWordlineEnergy() const {
    /* Folded into the row decoder above; reporting it separately would
     * double-count the same joules. See the block comment. */
    return 0.0;
}

double NVSimWrapper::getBitlineEnergy() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    const auto& sub = nvsim_result_->bank->mat.subarray;
    // NVSim's own sum, inverted: the array charge is the total less the
    // named components it accumulated on top (SubArray.cpp:854-856).
    double components = sub.cellReadEnergy
                      + sub.rowDecoder.readDynamicEnergy
                      + sub.bitlineMuxDecoder.readDynamicEnergy
                      + sub.senseAmpMuxLev1Decoder.readDynamicEnergy
                      + sub.senseAmpMuxLev2Decoder.readDynamicEnergy
                      + sub.precharger.readDynamicEnergy
                      + sub.bitlineMux.readDynamicEnergy
                      + sub.senseAmp.readDynamicEnergy
                      + sub.senseAmpMuxLev1.readDynamicEnergy
                      + sub.senseAmpMuxLev2.readDynamicEnergy;
    double bitline = sub.readDynamicEnergy - components;
    return (bitline > 0.0) ? bitline * 1e9 : 0.0;
}

double NVSimWrapper::getSenseAmpEnergy() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    return nvsim_result_->bank->mat.subarray.senseAmp.readDynamicEnergy * 1e9;
}

double NVSimWrapper::getPrechargerEnergy() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    return nvsim_result_->bank->mat.subarray.precharger.readDynamicEnergy * 1e9;
}

double NVSimWrapper::getDecoderLeakage() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    return nvsim_result_->bank->mat.subarray.rowDecoder.leakage * 1000.0;  // W -> mW
}

double NVSimWrapper::getWordlineLeakage() const {
    // Folded into the row decoder, as with the energy and the delay.
    return 0.0;
}

double NVSimWrapper::getSenseAmpLeakage() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    return nvsim_result_->bank->mat.subarray.senseAmp.leakage * 1000.0;  // W -> mW
}

double NVSimWrapper::getSubarrayArea() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    // Mat area / subarrays_per_mat
    double mat_area_mm2 = nvsim_result_->bank->mat.area * 1e6;  // m^2 -> mm^2
    uint32_t sa_per_mat = getSubarraysPerMat();
    return (sa_per_mat > 0) ? mat_area_mm2 / sa_per_mat : mat_area_mm2;
}

double NVSimWrapper::getMatArea() const {
    if (!valid_ || !nvsim_result_ || !nvsim_result_->bank) return 0.0;
    return nvsim_result_->bank->mat.area * 1e6;  // m^2 -> mm^2
}

void NVSimWrapper::printDetailedResults() const {
    if (!valid_) {
        std::cout << "[NVSimWrapper] No valid results available" << std::endl;
        if (!error_message_.empty()) {
            std::cout << "  Error: " << error_message_ << std::endl;
        }
        return;
    }

    std::cout << "\n=== NVSim Results ===" << std::endl;
    std::cout << "Configuration:" << std::endl;
    std::cout << "  Capacity: " << (config_.capacity_bytes / 1024) << " KB" << std::endl;
    std::cout << "  Word Width: " << config_.word_width_bits << " bits" << std::endl;
    std::cout << "  Technology: " << config_.process_node_nm << " nm" << std::endl;

    std::cout << "\nTiming:" << std::endl;
    std::cout << "  Read Latency: " << (getReadLatency() * 1e9) << " ns" << std::endl;
    std::cout << "  Write Latency: " << (getWriteLatency() * 1e9) << " ns" << std::endl;

    std::cout << "\nTiming Breakdown (subarray-level):" << std::endl;
    std::cout << "  Decoder Delay: " << (getDecoderDelay() * 1e9) << " ns" << std::endl;
    std::cout << "  Wordline Delay: " << (getWordlineDelay() * 1e9) << " ns" << std::endl;
    std::cout << "  Bitline Delay: " << (getBitlineDelay() * 1e9) << " ns" << std::endl;
    std::cout << "  Sense Amp Delay: " << (getSenseAmpDelay() * 1e9) << " ns" << std::endl;
    std::cout << "  Column Decoder Delay: " << (getColumnDecoderDelay() * 1e9) << " ns" << std::endl;

    std::cout << "\nSubarray Organization:" << std::endl;
    std::cout << "  Rows per Subarray: " << getSubarrayRows() << std::endl;
    std::cout << "  Cols per Subarray: " << getSubarrayCols() << std::endl;
    std::cout << "  Subarrays per Mat: " << getSubarraysPerMat() << std::endl;
    std::cout << "  Mats per Bank: " << getMatsPerBank() << std::endl;
    std::cout << "  Sense Amplifiers: " << getNumSenseAmps() << std::endl;

    std::cout << "\nElectrical Parameters:" << std::endl;
    std::cout << "  Wordline Length: " << (getWordlineLength() * 1e6) << " um" << std::endl;
    std::cout << "  Bitline Length: " << (getBitlineLength() * 1e6) << " um" << std::endl;
    std::cout << "  Wordline Capacitance: " << (getWordlineCapacitance() * 1e15) << " fF" << std::endl;
    std::cout << "  Wordline Resistance: " << getWordlineResistance() << " Ohm" << std::endl;
    std::cout << "  Bitline Capacitance: " << (getBitlineCapacitance() * 1e15) << " fF" << std::endl;
    std::cout << "  Bitline Resistance: " << getBitlineResistance() << " Ohm" << std::endl;

    std::cout << "\nArea:" << std::endl;
    std::cout << "  Total Area: " << getArea() << " mm^2" << std::endl;
    std::cout << "  Height: " << getHeight() << " mm" << std::endl;
    std::cout << "  Width: " << getWidth() << " mm" << std::endl;
    std::cout << "  Subarray Area: " << (getSubarrayArea() * 1e6) << " um^2" << std::endl;
    std::cout << "  Mat Area: " << (getMatArea() * 1e6) << " um^2" << std::endl;

    std::cout << "\nEnergy:" << std::endl;
    std::cout << "  Read Energy: " << getReadDynamicEnergy() << " nJ" << std::endl;
    std::cout << "  Write Energy: " << getWriteDynamicEnergy() << " nJ" << std::endl;
    std::cout << "  Read EDP: " << getReadEDP() << " nJ*ns" << std::endl;
    std::cout << "  Write EDP: " << getWriteEDP() << " nJ*ns" << std::endl;

    std::cout << "\nEnergy Breakdown (subarray-level):" << std::endl;
    std::cout << "  Decoder Energy: " << getDecoderEnergy() << " nJ" << std::endl;
    std::cout << "  Wordline Energy: " << getWordlineEnergy() << " nJ" << std::endl;
    std::cout << "  Bitline Energy: " << getBitlineEnergy() << " nJ" << std::endl;
    std::cout << "  Sense Amp Energy: " << getSenseAmpEnergy() << " nJ" << std::endl;

    std::cout << "\nPower:" << std::endl;
    std::cout << "  Leakage Power: " << getLeakagePower() << " mW" << std::endl;

    std::cout << "\nLeakage Breakdown (subarray-level):" << std::endl;
    std::cout << "  Decoder Leakage: " << getDecoderLeakage() << " mW" << std::endl;
    std::cout << "  Wordline Leakage: " << getWordlineLeakage() << " mW" << std::endl;
    std::cout << "  Sense Amp Leakage: " << getSenseAmpLeakage() << " mW" << std::endl;

    std::cout << "\nBandwidth:" << std::endl;
    std::cout << "  Read Bandwidth: " << getReadBandwidth() << " GB/s" << std::endl;
    std::cout << "  Write Bandwidth: " << getWriteBandwidth() << " GB/s" << std::endl;
    std::cout << "===================\n" << std::endl;
}

#else
#error "NVSim is mandatory for PIMID. Check external/nvsim/ and CMakeLists.txt."
#endif  // HAVE_NVSIM

} // namespace pimid
