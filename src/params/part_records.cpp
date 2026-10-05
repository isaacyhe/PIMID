// part_records.cpp -- see include/params/part_records.h.
//
// 1.11.94 (step 1 of the parameter-file migration) cross-checked the record
// against the code tables; 1.11.101 (step 2) deleted the tables: the record
// is loaded, validated for its own consistency, registered, and read by every
// former table site. The gate FIRES the registry by editing one value in a
// copied record (the priced area moves) and by deleting one field (refusal).
#include "params/part_records.h"
#include "memory/cacti_wrapper.h"
#include "memory/ramulator_wrapper.h"

#include <yaml-cpp/yaml.h>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <iostream>
#include <map>

namespace pimid {
namespace params {

std::string paramsDir() {
    const char* env = std::getenv("PIMID_PARAMS");
    if (env && env[0]) return std::string(env);
#ifdef PIMID_PARAMS_DIR
    return std::string(PIMID_PARAMS_DIR);
#else
    return std::string("params");
#endif
}

namespace {

template <typename T>
bool need(const YAML::Node& n, const char* key, T& out, std::string& error) {
    if (!n[key]) { error = std::string("required field '") + key + "' is missing"; return false; }
    try { out = n[key].as<T>(); }
    catch (const std::exception& e) { error = std::string("field '") + key + "' is not readable: " + e.what(); return false; }
    return true;
}

bool close(double a, double b, double rel = 1e-9) {
    const double m = std::fabs(a) > std::fabs(b) ? std::fabs(a) : std::fabs(b);
    return std::fabs(a - b) <= rel * (m > 0 ? m : 1.0);
}

std::string fmt(double v) { std::ostringstream o; o.precision(10); o << v; return o.str(); }

} // namespace

bool loadDramPartRecord(const std::string& tech, DramPartRecord& out, std::string& error) {
    std::string t = tech;
    for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string path = paramsDir() + "/dram/" + t + ".yaml";
    std::ifstream probe(path);
    if (!probe) {
        error = "part record " + path + " is missing. The simulator ships one record per DRAM "
                "technology under params/dram/; set PIMID_PARAMS to the directory that holds "
                "them, or run from the tree that built this binary.";
        return false;
    }
    YAML::Node n;
    try { n = YAML::LoadFile(path); }
    catch (const std::exception& e) { error = "part record " + path + " does not parse: " + e.what(); return false; }

    DramPartRecord r; r.file = path;
    if (!need(n, "technology", r.technology, error) || !need(n, "part", r.part, error) ||
        !need(n, "vendor", r.vendor, error) || !need(n, "generation", r.generation, error) ||
        !need(n, "feature_nm", r.feature_nm, error) || !need(n, "cell_factor_f2", r.cell_factor_f2, error) ||
        !need(n, "density_mb_per_mm2", r.density_mb_per_mm2, error) || !need(n, "density_source", r.density_source, error) ||
        !need(n, "organization_preset", r.organization_preset, error) || !need(n, "timing_preset", r.timing_preset, error) ||
        !need(n, "channels", r.channels, error) ||
        !need(n, "channel_width_bits", r.channel_width_bits, error)) {   // 1.11.101
        error = "part record " + path + ": " + error; return false;
    }
    const bool stacked = (r.technology == "HBM2" || r.technology == "HBM3");
    if (stacked) {
        if (!need(n, "stack_dies", r.stack_dies, error) || !need(n, "die_capacity_gb", r.die_capacity_gb, error)) {
            error = "part record " + path + ": " + error; return false;
        }
    }
    if (!n["refresh_ladder"] || !n["refresh_ladder"].IsSequence() || n["refresh_ladder"].size() == 0) {
        error = "part record " + path + ": required sequence 'refresh_ladder' is missing or empty"; return false;
    }
    for (const auto& rung : n["refresh_ladder"]) {
        RefreshRung g;
        if (!need(rung, "above_c", g.above_c, error) || !need(rung, "factor", g.factor, error)) {
            error = "part record " + path + ": refresh_ladder rung: " + error; return false;
        }
        r.refresh_ladder.push_back(g);
    }
    if (r.technology != tech) {
        error = "part record " + path + " says technology '" + r.technology + "' but was loaded for '" + tech + "'";
        return false;
    }
    out = r;
    return true;
}

namespace {
std::map<std::string, DramPartRecord>& registry() { static std::map<std::string, DramPartRecord> m; return m; }
}

void registerDramPartRecord(const DramPartRecord& rec) { registry()[rec.technology] = rec; }
bool hasDramPartRecord(const std::string& tech) { return registry().count(tech) > 0; }
const DramPartRecord& dramPartRecord(const std::string& tech) {
    auto it = registry().find(tech);
    if (it == registry().end()) {
        /* a technology the run did not name (a reference figure, an
         * announcement, a cross-technology comparison): its record is read
         * on demand, not validated against a wrapper (the wrapper reads this
         * registry) */
        DramPartRecord rec; std::string err;
        if (loadDramPartRecord(tech, rec, err)) {
            registry()[rec.technology] = rec;
            it = registry().find(tech);
        }
    }
    if (it == registry().end()) {
        std::cerr << "[params] FATAL: no DRAM part record is registered for '" << tech
                  << "'. Since 1.11.101 the record (params/dram/" << tech << ".yaml, or the directory "
                     "PIMID_PARAMS names) is the only source of the part's generation, feature size, "
                     "density, presets, channels, dies and refresh ladder; nothing is substituted for it."
                  << std::endl;
        std::exit(2);
    }
    return it->second;
}

/* 1.11.101 (step 2): the record's own consistency, in place of the deleted
 * cross-check. The HBM capacity identity is ruling 16; the channel-count
 * check ties the record to the IDD row the energy model prices with (the
 * row's basis is a measurement context, not a second table of the part). */
bool validateDramPartRecord(const DramPartRecord& rec, std::vector<std::string>& errors) {
    errors.clear();
    const std::string& t = rec.technology;
    const int cw = rec.channel_width_bits;
    if (cw < 16 || cw > 256 || (cw & (cw - 1)) != 0)
        errors.push_back("channel_width_bits " + std::to_string(cw) + " is not a power of two from 16 to 256");
    if (rec.feature_nm <= 0.0) errors.push_back("feature_nm must be positive");
    if (rec.cell_factor_f2 <= 0) errors.push_back("cell_factor_f2 must be positive");
    if (rec.density_mb_per_mm2 <= 0.0) errors.push_back("density_mb_per_mm2 must be positive");
    if (rec.density_source.empty()) errors.push_back("density_source must name the measurement");
    if (rec.channels < 1) errors.push_back("channels must be at least 1");
    double last_c = -1e9;
    for (const auto& g : rec.refresh_ladder) {
        if (g.above_c <= last_c) errors.push_back("refresh_ladder rungs must rise in temperature (above_c " + fmt(g.above_c) + ")");
        if (g.factor <= 0.0 || g.factor > 1.0) errors.push_back("refresh_ladder factor " + fmt(g.factor) + " is outside (0, 1]");
        last_c = g.above_c;
    }
    const bool stacked = (t == "HBM2" || t == "HBM3");
    if (stacked && rec.stack_dies < 1) errors.push_back("stack_dies must be at least 1 for a stacked part");
    try {
        RamulatorWrapper w("", t);
        w.initialize();
        const auto& org = w.getPresetOrganization();
        if (!org.valid) errors.push_back("the organization preset '" + rec.organization_preset + "' has no transcription in this build");
        if (!w.getPresetTiming().valid) errors.push_back("the timing preset '" + rec.timing_preset + "' has no transcription in this build");
        if (stacked && org.valid) {
            const double per_ch_gb = static_cast<double>(org.density_mb) / 1024.0;   // GB per channel (density_mb is MB per channel for HBM)
            const double die_gb = per_ch_gb * rec.channels * 8.0 / rec.stack_dies;     // Gb per die
            if (!close(die_gb, rec.die_capacity_gb, 1e-6))
                errors.push_back("die_capacity_gb " + fmt(rec.die_capacity_gb) + " != stack capacity / dies = " + fmt(die_gb) +
                                 " Gb (preset " + std::to_string(org.density_mb) + " MB/channel x " + std::to_string(rec.channels) +
                                 " channels x 8 / " + std::to_string(rec.stack_dies) + " dies; ruling 16)");
        }
        const int idd_ch = w.getIddRowChannels();
        if (stacked && idd_ch > 0 && idd_ch != rec.channels)
            errors.push_back("channels " + std::to_string(rec.channels) + " != the IDD row's basis of " + std::to_string(idd_ch) +
                             " channels per stack (the energy model's background population)");
    } catch (const std::exception& e) {
        errors.push_back(std::string("the Ramulator wrapper could not be built from the record: ") + e.what());
    }
    return errors.empty();
}

std::string describeDramPartRecord(const DramPartRecord& rec) {
    std::ostringstream o;
    o << "[params] DRAM part record " << rec.file << ": " << rec.vendor << " " << rec.part
      << " (" << rec.generation << ", F " << rec.feature_nm << " nm, " << rec.cell_factor_f2 << "F^2, "
      << rec.density_mb_per_mm2 << " MB/mm^2; " << rec.organization_preset << " / " << rec.timing_preset
      << ", " << rec.channels << " channel(s)";
    o << ", " << rec.channel_width_bits << "-bit channel";
    if (rec.stack_dies > 0) o << ", " << rec.stack_dies << " dies x " << rec.die_capacity_gb << " Gb";
    o << ") -- the only source (step 2 of the parameter-file migration, 1.11.101)";
    return o.str();
}

} // namespace params
} // namespace pimid


/* ---- 1.11.95: cache part record ---------------------------------------- */
namespace pimid { namespace params {

const CacheLevelRecord& CacheRecord::level(const std::string& name) const {
    if (name == "l1d") return l1d;
    if (name == "l1i") return l1i;
    if (name == "l2") return l2;
    return l3;
}

int cacheBanksSlice(int size_kb, int slice_mb) {
    const int slice_kb = std::max(1, slice_mb) * 1024;
    return std::max(1, std::min(32, size_kb / slice_kb));
}

static bool loadCacheLevel(const YAML::Node& n, const std::string& name, CacheLevelRecord& out,
                           const std::string& file, std::string& error) {
    if (!n || !n.IsMap()) { error = "cache record " + file + ": level '" + name + "' is missing"; return false; }
    for (const char* f : {"ways", "line_bytes", "banks"}) {
        if (!n[f]) { error = "cache record " + file + ": level '" + name + "' lacks '" + f + "'"; return false; }
    }
    out.ways = n["ways"].as<int>(-1);
    out.line_bytes = n["line_bytes"].as<int>(-1);
    const std::string b = n["banks"].as<std::string>("");
    if (b == "slice") out.banks = -1;
    else {
        try { out.banks = std::stoi(b); } catch (...) { out.banks = 0; }
        if (out.banks < 1 || out.banks > 32) {
            error = "cache record " + file + ": level '" + name + "' banks '" + b + "' is not 'slice' or an integer in CACTI's range 1..32";
            return false;
        }
    }
    if (out.ways < 1 || out.ways > 64) { error = "cache record " + file + ": level '" + name + "' ways " + std::to_string(out.ways) + " outside 1..64"; return false; }
    if (out.line_bytes != 32 && out.line_bytes != 64 && out.line_bytes != 128) { error = "cache record " + file + ": level '" + name + "' line_bytes " + std::to_string(out.line_bytes) + " is not 32, 64 or 128"; return false; }
    return true;
}

bool loadCacheRecord(CacheRecord& out, std::string& error) {
    out = CacheRecord();
    out.file = paramsDir() + "/cache/default.yaml";
    std::ifstream probe(out.file);
    if (!probe.good()) {
        error = "cache record " + out.file + " is missing. The simulator ships params/cache/default.yaml; set PIMID_PARAMS to a directory that holds cache/default.yaml.";
        return false;
    }
    YAML::Node n;
    try { n = YAML::LoadFile(out.file); } catch (const std::exception& e) { error = "cache record " + out.file + ": " + e.what(); return false; }
    if (!n["record"] || !n["slice_mb"] || !n["levels"]) { error = "cache record " + out.file + " lacks 'record', 'slice_mb' or 'levels'"; return false; }
    out.record = n["record"].as<std::string>("");
    out.slice_mb = n["slice_mb"].as<int>(0);
    if (out.slice_mb < 1 || out.slice_mb > 64) { error = "cache record " + out.file + ": slice_mb " + std::to_string(out.slice_mb) + " outside 1..64"; return false; }
    /* 1.11.101 (BIG-CACHE (b)): the replication bound and the home-slice hop */
    if (!n["max_slices"] || !n["home_hop_cycles"]) { error = "cache record " + out.file + " lacks 'max_slices' or 'home_hop_cycles' (1.11.101)"; return false; }
    out.max_slices = n["max_slices"].as<int>(0);
    out.home_hop_cycles = n["home_hop_cycles"].as<int>(-1);
    if (out.max_slices < 1 || out.max_slices > 32) { error = "cache record " + out.file + ": max_slices " + std::to_string(out.max_slices) + " outside CACTI's bank range 1..32"; return false; }
    if (out.home_hop_cycles < 0 || out.home_hop_cycles > 64) { error = "cache record " + out.file + ": home_hop_cycles " + std::to_string(out.home_hop_cycles) + " outside 0..64"; return false; }
    return loadCacheLevel(n["levels"]["l1d"], "l1d", out.l1d, out.file, error) &&
           loadCacheLevel(n["levels"]["l1i"], "l1i", out.l1i, out.file, error) &&
           loadCacheLevel(n["levels"]["l2"], "l2", out.l2, out.file, error) &&
           loadCacheLevel(n["levels"]["l3"], "l3", out.l3, out.file, error);
}

std::string describeCacheRecord(const CacheRecord& rec) {
    std::ostringstream o;
    auto lv = [&](const char* nm, const CacheLevelRecord& l) {
        o << nm << " " << l.ways << "-way " << l.line_bytes << " B " << (l.banks > 0 ? std::to_string(l.banks) + " banks" : "slice-rule banks");
    };
    o << "[params] cache record " << rec.file << ": " << rec.record << " (slice " << rec.slice_mb << " MB, replicated above "
      << rec.max_slices << " slices at " << rec.home_hop_cycles << " cycles per home-slice hop; ";
    lv("L1D", rec.l1d); o << "; "; lv("L1I", rec.l1i); o << "; "; lv("L2", rec.l2); o << "; "; lv("L3", rec.l3); o << ")";
    return o.str();
}

}}  // namespace pimid::params


/* ---- 1.11.97: core part record ----------------------------------------- */
namespace pimid { namespace params {

static bool coreInt(const YAML::Node& n, const std::string& type, const char* key, int lo, int hi,
                    int& out, const std::string& file, std::string& error) {
    if (!n[key]) { error = "core record " + file + ": '" + type + "' lacks '" + key + "'"; return false; }
    try { out = n[key].as<int>(); }
    catch (const std::exception&) {
        error = "core record " + file + ": " + type + "." + key + " is not an integer"; return false;
    }
    if (out < lo || out > hi) {
        error = "core record " + file + ": " + type + "." + key + " " + std::to_string(out) +
                " outside " + std::to_string(lo) + ".." + std::to_string(hi);
        return false;
    }
    return true;
}

bool loadCoreRecord(CoreRecord& out, std::string& error) {
    out = CoreRecord();
    out.file = paramsDir() + "/core/default.yaml";
    std::ifstream probe(out.file);
    if (!probe.good()) {
        error = "core record " + out.file + " is missing. The simulator ships params/core/default.yaml; set PIMID_PARAMS to a directory that holds core/default.yaml.";
        return false;
    }
    YAML::Node n;
    try { n = YAML::LoadFile(out.file); } catch (const std::exception& e) { error = "core record " + out.file + ": " + e.what(); return false; }
    if (!n["record"] || !n["in_order"] || !n["ooo"] || !n["in_order"].IsMap() || !n["ooo"].IsMap()) {
        error = "core record " + out.file + " lacks 'record', 'in_order' or 'ooo'"; return false;
    }
    out.record = n["record"].as<std::string>("");
    const YAML::Node io = n["in_order"], oo = n["ooo"];
    /* Ranges: penalties 0..1000 = the range the in-order core has always
     * accepted from PIMID_INORDER_MISPRED_PENALTY; fetch width 1..64 bytes
     * (at most one 64 B line per cycle, so the wrong-path throughput step
     * lineSize / fetch_width stays >= 1 cycle; checked against the run's
     * line size at apply time). */
    if (!coreInt(io, "in_order", "mispredict_penalty_cycles", 0, 1000, out.in_order.mispredict_penalty_cycles, out.file, error) ||
        !coreInt(io, "in_order", "resteer_penalty_cycles", 0, 1000, out.in_order.resteer_penalty_cycles, out.file, error) ||
        !coreInt(oo, "ooo", "mispredict_penalty_cycles", 0, 1000, out.ooo.mispredict_penalty_cycles, out.file, error) ||
        !coreInt(oo, "ooo", "fetch_width_bytes", 1, 64, out.ooo.fetch_width_bytes, out.file, error) ||
        !coreInt(oo, "ooo", "retire_width", 1, 64, out.ooo.retire_width, out.file, error))
        return false;
    if (!io["retire_width"] || io["retire_width"].as<std::string>("") != "issue") {
        error = "core record " + out.file + ": in_order.retire_width must be 'issue' (InOrderCore issues and "
                "commits on one cursor, so it retires exactly its issue width; an integer would price a retire "
                "limit the timing core does not have)";
        return false;
    }
    out.in_order.retire_width = -1;
    if (out.in_order.resteer_penalty_cycles > out.in_order.mispredict_penalty_cycles) {
        error = "core record " + out.file + ": in_order.resteer_penalty_cycles " + std::to_string(out.in_order.resteer_penalty_cycles) +
                " exceeds mispredict_penalty_cycles " + std::to_string(out.in_order.mispredict_penalty_cycles) +
                " (a decode-depth resteer cannot cost more than an execute-depth flush)";
        return false;
    }
    if (out.ooo.retire_width != kOooRobRetireWidth) {
        error = "core record " + out.file + ": ooo.retire_width " + std::to_string(out.ooo.retire_width) +
                " is not the retire width zsim OOOCore is compiled with (" + std::to_string(kOooRobRetireWidth) +
                ", ReorderBuffer<128, 4> in external/zsim/src/ooo_core.h); McPAT would price a commit width the "
                "timing core does not simulate";
        return false;
    }
    return true;
}

std::string describeCoreRecord(const CoreRecord& rec) {
    std::ostringstream o;
    o << "[params] core record " << rec.file << ": " << rec.record
      << " (in_order: mispredict " << rec.in_order.mispredict_penalty_cycles << " cyc, resteer "
      << rec.in_order.resteer_penalty_cycles << " cyc, retire = issue width; ooo: mispredict "
      << rec.ooo.mispredict_penalty_cycles << " cyc, fetch " << rec.ooo.fetch_width_bytes
      << " B/cyc, retire " << rec.ooo.retire_width << ")";
    return o.str();
}

}}  // namespace pimid::params
