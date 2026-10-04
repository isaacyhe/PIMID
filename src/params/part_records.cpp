// part_records.cpp -- see include/params/part_records.h.
//
// 1.11.94 (step 1 of the parameter-file migration). The cross-check below is
// deliberately verbose: it names every field it compares, so the gate can
// FIRE it by editing one value in a record and asserting the refusal.
#include "params/part_records.h"
#include "memory/cacti_wrapper.h"
#include "memory/ramulator_wrapper.h"

#include <yaml-cpp/yaml.h>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

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
        !need(n, "channels", r.channels, error)) {
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

/* The code tables this release still carries, and the record must equal:
 *   generation      CACTIWrapper::generationClass()
 *   feature_nm      CACTIWrapper::generationFeatureNm()
 *   density         CACTIWrapper::vendorDieDensity()
 *   presets/channels RamulatorWrapper (resolved at the default knobs)
 *   stack_dies      main.cpp's rule max(4|8, channels/2) -- restated here,
 *                   the duplicate dies with the rule in step 2
 *   refresh ladder  pimid_energy::refreshTempFactor() via the wrapper */
bool crossCheckDramPartRecord(const DramPartRecord& rec, std::vector<std::string>& errors, bool check_presets) {
    errors.clear();
    const std::string& t = rec.technology;

    const std::string gen = CACTIWrapper::generationClass(t);
    if (gen != rec.generation)
        errors.push_back("generation: record '" + rec.generation + "' vs code '" + gen + "'");
    const double f = CACTIWrapper::generationFeatureNm(rec.generation);
    if (!close(f, rec.feature_nm))
        errors.push_back("feature_nm: record " + fmt(rec.feature_nm) + " vs code " + fmt(f));
    if (rec.cell_factor_f2 != 6)
        errors.push_back("cell_factor_f2: record " + std::to_string(rec.cell_factor_f2) + " vs code 6 (6F^2 buried wordline)");
    const double d = CACTIWrapper::vendorDieDensity(t);
    if (!close(d, rec.density_mb_per_mm2, 1e-6))
        errors.push_back("density_mb_per_mm2: record " + fmt(rec.density_mb_per_mm2) + " vs code " + fmt(d));

    try {
        RamulatorWrapper w("", t);
        w.initialize();
        const auto& org = w.getPresetOrganization();
        const auto& tim = w.getPresetTiming();
        if (check_presets && org.preset_name != rec.organization_preset)
            errors.push_back("organization_preset: record '" + rec.organization_preset + "' vs code '" + org.preset_name + "'");
        if (check_presets && tim.preset_name != rec.timing_preset)
            errors.push_back("timing_preset: record '" + rec.timing_preset + "' vs code '" + tim.preset_name + "'");
        const int nch = static_cast<int>(w.getNumChannels());
        if (nch != rec.channels)
            errors.push_back("channels: record " + std::to_string(rec.channels) + " vs code " + std::to_string(nch));
        if (t == "HBM2" || t == "HBM3") {
            const int code_dies = std::max(t == "HBM2" ? 4 : 8, rec.channels / 2);
            if (code_dies != rec.stack_dies)
                errors.push_back("stack_dies: record " + std::to_string(rec.stack_dies) + " vs code " + std::to_string(code_dies));
            const double per_ch_gb = static_cast<double>(org.density_mb) / 1024.0;   // GB per channel (density_mb is MB per channel for HBM)
            const double stack_gb_bytes = per_ch_gb * rec.channels;                     // GB per stack
            const double die_gb = stack_gb_bytes * 8.0 / rec.stack_dies;                // Gb per die
            if (!close(die_gb, rec.die_capacity_gb, 1e-6))
                errors.push_back("die_capacity_gb: record " + fmt(rec.die_capacity_gb) + " vs code " + fmt(die_gb) +
                                 " (= preset " + std::to_string(org.density_mb) + " MB/channel x " + std::to_string(rec.channels) +
                                 " channels x 8 / " + std::to_string(rec.stack_dies) + " dies)");
        }
        /* refresh ladder: evaluate the code's factor at the midpoint of every
         * rung the record declares and just above each threshold. */
        for (size_t i = 0; i < rec.refresh_ladder.size(); ++i) {
            const double probe_c = rec.refresh_ladder[i].above_c + 5.0;
            const int probe_k = static_cast<int>(std::lround(probe_c + 273.15));
            w.setTemperatureK(probe_k);
            const double code_f = w.getRefreshTempFactor();
            if (!close(code_f, rec.refresh_ladder[i].factor, 1e-9))
                errors.push_back("refresh_ladder above " + fmt(rec.refresh_ladder[i].above_c) + " C: record " +
                                 fmt(rec.refresh_ladder[i].factor) + " vs code " + fmt(code_f));
        }
    } catch (const std::exception& e) {
        errors.push_back(std::string("the Ramulator wrapper could not be queried for the cross-check: ") + e.what());
    }
    return errors.empty();
}

std::string describeDramPartRecord(const DramPartRecord& rec) {
    std::ostringstream o;
    o << "[params] DRAM part record " << rec.file << ": " << rec.vendor << " " << rec.part
      << " (" << rec.generation << ", F " << rec.feature_nm << " nm, " << rec.cell_factor_f2 << "F^2, "
      << rec.density_mb_per_mm2 << " MB/mm^2; " << rec.organization_preset << " / " << rec.timing_preset
      << ", " << rec.channels << " channel(s)";
    if (rec.stack_dies > 0) o << ", " << rec.stack_dies << " dies x " << rec.die_capacity_gb << " Gb";
    o << ") -- cross-checked against the code tables (step 1 of the parameter-file migration)";
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
    o << "[params] cache record " << rec.file << ": " << rec.record << " (slice " << rec.slice_mb << " MB; ";
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
