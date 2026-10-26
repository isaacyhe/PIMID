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
#include <set>

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

namespace {
std::map<std::string, std::pair<std::string, std::string>>& selection() {   // 1.12.25: tech -> (variant, trfc basis)
    static std::map<std::string, std::pair<std::string, std::string>> m; return m;
}
}
void setDramRecordSelection(const std::string& tech, const std::string& variant, const std::string& trfc_basis) {
    selection()[tech] = {variant, trfc_basis};
}

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
    /* (1.12.52) GDDR6 die count: the channels one die carries (GDDR6 must state it; default 1 elsewhere) */
    if (n["channels_per_die"]) {
        if (!need(n, "channels_per_die", r.channels_per_die, error)) { error = "part record " + path + ": " + error; return false; }
    } else if (r.technology == "GDDR6") {
        error = "part record " + path + ": required field 'channels_per_die' is missing (a GDDR6 device carries two channels, JESD250D 4.1)"; return false;
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
    /* 1.11.104 (IDD-RECORDS step 1): the part's measured data. */
    if (!n["idd"] || !n["idd"].IsMap()) {
        error = "part record " + path + ": required field 'idd' is missing (the part's measured IDD data; step 1 of the IDD migration, 1.11.104)";
        return false;
    }
    {
        const YAML::Node b = n["idd"];
        if (!need(b, "basis", r.idd.basis, error) || !need(b, "channels_basis", r.idd.channels_basis, error) ||
            !need(b, "provenance", r.idd.provenance, error) || !need(b, "idd3n_bank_state", r.idd.idd3n_bank_state, error)) {
            error = "part record " + path + ": idd: " + error; return false;
        }
        auto readRow = [&](const YAML::Node& m, IddRow& row, std::string& err) -> bool {
            if (!need(m, "vdd", row.vdd, err) || !need(m, "idd0", row.idd0, err) || !need(m, "idd2n", row.idd2n, err) ||
                !need(m, "idd3n", row.idd3n, err) || !need(m, "idd4r", row.idd4r, err) || !need(m, "idd4w", row.idd4w, err) ||
                !need(m, "idd5", row.idd5, err) || !need(m, "idd2p", row.idd2p, err) || !need(m, "trfc_ns", row.trfc_ns, err) ||
                !need(m, "trefi_ns", row.trefi_ns, err) || !need(m, "source", row.source, err)) return false;
            if (m["standby_clock"]) {   /* (1.12.79): optional; scale in (0, 1] with its source */
                const YAML::Node c = m["standby_clock"];
                if (!need(c, "scale", row.standby_clock_scale, err) || !need(c, "source", row.standby_clock_source, err)) { err = "standby_clock: " + err; return false; }
                if (!(row.standby_clock_scale > 0.0 && row.standby_clock_scale <= 1.0)) { err = "standby_clock: scale must be in (0, 1]"; return false; }
            }
            if (m["iddq"]) {
                const YAML::Node q = m["iddq"]; row.has_iddq = true;
                if (!need(q, "iddq3n", row.iddq3n, err) || !need(q, "iddq4r", row.iddq4r, err) || !need(q, "iddq4w", row.iddq4w, err) ||
                    !need(q, "vddq", row.vddq, err) || !need(q, "source", row.iddq_source, err)) { err = "iddq: " + err; return false; }
                /* 1.12.18: optional; a true needs its source */
                if (q["wr_includes_rtt_wr"]) {
                    try { row.iddq_wr_includes_rtt_wr = q["wr_includes_rtt_wr"].as<bool>(); }
                    catch (const std::exception&) { err = "iddq: wr_includes_rtt_wr is not true/false"; return false; }
                    if (row.iddq_wr_includes_rtt_wr && !need(q, "wr_rtt_source", row.iddq_wr_rtt_source, err)) { err = "iddq: " + err; return false; }
                }
            }
            if (m["ipp"]) {
                const YAML::Node q = m["ipp"]; row.has_ipp = true;
                if (!need(q, "ipp2n", row.ipp2n, err) || !need(q, "ipp3n", row.ipp3n, err) || !need(q, "vpp", row.vpp, err) ||
                    !need(q, "source", row.ipp_source, err)) { err = "ipp: " + err; return false; }
                /* 1.12.18 (bug 3b): the active rows, optional (a sheet may not publish them) */
                if ((q["ipp0"] && !need(q, "ipp0", row.ipp0, err)) || (q["ipp4r"] && !need(q, "ipp4r", row.ipp4r, err)) ||
                    (q["ipp4w"] && !need(q, "ipp4w", row.ipp4w, err)) || (q["ipp5b"] && !need(q, "ipp5b", row.ipp5b, err))) { err = "ipp: " + err; return false; }
            }
            if (m["io_share"]) {   // 1.12.18 (bug 2b/8a)
                const YAML::Node q = m["io_share"]; row.has_io_share = true;
                if (!need(q, "idd4r_ma", row.idd4r_io, err) || !need(q, "idd4w_ma", row.idd4w_io, err) ||
                    !need(q, "provenance", row.io_share_provenance, err) || !need(q, "source", row.io_share_source, err)) { err = "io_share: " + err; return false; }
                /* 1.12.18 (user ruling 2026-10-23): the optional anchor, all four keys or none */
                const int na = (q["anchor_read_pj_bit"] ? 1 : 0) + (q["anchor_write_pj_bit"] ? 1 : 0) + (q["channel_gbps"] ? 1 : 0) + (q["anchor_source"] ? 1 : 0);
                if (na != 0 && na != 4) { err = "io_share: anchor_read_pj_bit, anchor_write_pj_bit, channel_gbps and anchor_source go together (all four or none)"; return false; }
                if (na == 4) {
                    row.io_share_anchored = true;
                    if (!need(q, "anchor_read_pj_bit", row.io_anchor_rd_pj_bit, err) || !need(q, "anchor_write_pj_bit", row.io_anchor_wr_pj_bit, err) ||
                        !need(q, "channel_gbps", row.io_channel_gbps, err) || !need(q, "anchor_source", row.io_anchor_source, err)) { err = "io_share: " + err; return false; }
                }
            }
            return true;
        };
        if (b["by_grade"]) {
            if (!b["by_grade"].IsMap() || b["by_grade"].size() == 0) {
                error = "part record " + path + ": idd.by_grade must be a non-empty map keyed by the data rate in MT/s"; return false;
            }
            for (const auto& kv : b["by_grade"]) {
                int g = 0;
                try { g = kv.first.as<int>(); }
                catch (...) { error = "part record " + path + ": idd.by_grade has a key that is not a data rate"; return false; }
                IddRow row; std::string err;
                if (!readRow(kv.second, row, err)) { error = "part record " + path + ": idd.by_grade " + std::to_string(g) + ": " + err; return false; }
                r.idd.by_grade[g] = row;
            }
        } else {
            IddRow row; std::string err;
            if (!readRow(b, row, err)) { error = "part record " + path + ": idd: " + err; return false; }
            r.idd.by_grade[0] = row;
        }
        /* (1.12.48) (user ruling 2026-10-25): optional idd.by_width -- per-device-width columns of a single-grade record.
         * Each width row is the single row with the columns the sheet prints for that width replacing it; a column
         * the width row omits is the sheet's "All widths" value (the single row). Its source is required. */
        if (b["by_width"]) {
            if (!b["by_width"].IsMap() || b["by_width"].size() == 0 || r.idd.by_grade.size() != 1) {
                error = "part record " + path + ": idd.by_width must be a non-empty map (x4 | x8 | x16) on a single-grade record"; return false;
            }
            for (const auto& kv : b["by_width"]) {
                const std::string w = kv.first.as<std::string>("");
                if (w != "x4" && w != "x8" && w != "x16") { error = "part record " + path + ": idd.by_width key '" + w + "' is not x4, x8 or x16"; return false; }
                IddRow row = r.idd.by_grade.begin()->second;
                std::string err;
                const char* cols[] = {"idd0", "idd2n", "idd3n", "idd4r", "idd4w", "idd5", "idd2p"};
                double* dst[] = {&row.idd0, &row.idd2n, &row.idd3n, &row.idd4r, &row.idd4w, &row.idd5, &row.idd2p};
                for (int i = 0; i < 7; ++i)
                    if (kv.second[cols[i]] && !need(kv.second, cols[i], *dst[i], err)) { error = "part record " + path + ": idd.by_width " + w + ": " + err; return false; }
                if (!need(kv.second, "source", row.source, err)) { error = "part record " + path + ": idd.by_width " + w + ": " + err; return false; }
                r.idd.by_width[w] = row;
            }
        }
        if (b["e_actpre_pj_override"] && !need(b, "e_actpre_pj_override", r.idd.e_actpre_pj_override, error)) { error = "part record " + path + ": idd: " + error; return false; }
        if (b["stack_floor_mw"] && !need(b, "stack_floor_mw", r.idd.stack_floor_mw, error)) { error = "part record " + path + ": idd: " + error; return false; }
        /* user ruling 2026-10-25 (second supply rails): the VPP share of an override activation (GDDR6, the IDD7 route) */
        if (b["vpp_actpre_pj_override"]) {
            if (!need(b, "vpp_actpre_pj_override", r.idd.vpp_actpre_pj_override, error)) { error = "part record " + path + ": idd: " + error; return false; }
            if (!(r.idd.vpp_actpre_pj_override >= 0.0) || !(r.idd.e_actpre_pj_override >= 0.0)) {
                error = "part record " + path + ": idd.vpp_actpre_pj_override must be non-negative and needs idd.e_actpre_pj_override (it is the VPP share of that route)"; return false;
            }
            for (const auto& kv : r.idd.by_grade) {
                if (!kv.second.has_ipp || !(kv.second.vpp > 0.0)) {
                    error = "part record " + path + ": idd.vpp_actpre_pj_override needs an ipp row with a positive vpp"; return false;
                }
                if (kv.second.ipp0 >= 0.0) {
                    error = "part record " + path + ": idd.vpp_actpre_pj_override and an ipp.ipp0 row both price the VPP activation; give one"; return false;
                }
            }
        }
        /* 1.12.18 (user ruling 2026-10-24, option 5): an HBM row states which loop its idd0 is */
        if (b["idd0_loop"] && !need(b, "idd0_loop", r.idd.idd0_loop, error)) { error = "part record " + path + ": idd: " + error; return false; }
        if ((r.technology == "HBM2" || r.technology == "HBM3") && r.idd.idd0_loop != "ONE_PC_PER_TRC") {
            error = "part record " + path + ": idd.idd0_loop must be ONE_PC_PER_TRC on an HBM record (found '" + r.idd.idd0_loop +
                    "'): the energy model charges vdd x (idd0 - baseline) x tRC per ACT of ONE pseudo-channel page. A JEDEC IDD0"
                    " (JESD235D Table 60 / JESD238B.01 Table 85) activates PC0 and PC1 every nRC -- convert it first:"
                    " idd0 = baseline + (IDD0_JEDEC - baseline) / 2";
            return false;
        }
        /* 1.12.18 (user ruling 2026-10-24): HBM activation VPP as a ratio of the VDD activate energy */
        if (b["vpp_act"]) {
            const YAML::Node v = b["vpp_act"]; r.idd.has_vpp_act = true;
            if (!need(v, "ratio", r.idd.vpp_act_ratio, error) || !need(v, "device", r.idd.vpp_act_device, error) ||
                !need(v, "alt_ratio", r.idd.vpp_act_alt_ratio, error) || !need(v, "alt_device", r.idd.vpp_act_alt_device, error) ||
                !need(v, "provenance", r.idd.vpp_act_provenance, error) || !need(v, "source", r.idd.vpp_act_source, error)) {
                error = "part record " + path + ": idd.vpp_act: " + error; return false;
            }
            if (!(r.idd.vpp_act_ratio >= 0.0 && r.idd.vpp_act_ratio < 1.0) || !(r.idd.vpp_act_alt_ratio >= 0.0 && r.idd.vpp_act_alt_ratio < 1.0)) {
                error = "part record " + path + ": idd.vpp_act.ratio / alt_ratio must be in [0, 1)"; return false;
            }
            for (const auto& kv : r.idd.by_grade)
                if (kv.second.ipp0 >= 0.0) {
                    error = "part record " + path + ": idd.vpp_act and an ipp.ipp0 row both price the VPP activation; give one"; return false;
                }
        }
        if (b["background_guard_mw_stack"]) {   // 1.12.18 (user ruling 2026-10-24, floor option 2): guard only
            const YAML::Node g = b["background_guard_mw_stack"]; r.idd.has_bg_guard = true;
            if (!need(g, "lo", r.idd.bg_guard_lo_mw, error) || !need(g, "hi", r.idd.bg_guard_hi_mw, error) ||
                !need(g, "source", r.idd.bg_guard_source, error)) {
                error = "part record " + path + ": idd.background_guard_mw_stack: " + error; return false;
            }
            if (!(r.idd.bg_guard_lo_mw > 0.0 && r.idd.bg_guard_lo_mw <= r.idd.bg_guard_hi_mw)) {
                error = "part record " + path + ": idd.background_guard_mw_stack needs 0 < lo <= hi"; return false;
            }
        }
        if (b["act_guard_nj_per_kb"]) {
            const YAML::Node g = b["act_guard_nj_per_kb"]; r.idd.has_act_guard = true;
            if (!need(g, "lo", r.idd.act_guard_lo, error) || !need(g, "xpe_ceiling", r.idd.act_guard_ceiling, error) ||
                !need(g, "hi", r.idd.act_guard_hi, error) || !need(g, "source", r.idd.act_guard_source, error)) {
                error = "part record " + path + ": idd.act_guard_nj_per_kb: " + error; return false;
            }
            if (!(r.idd.act_guard_lo > 0.0 && r.idd.act_guard_lo <= r.idd.act_guard_ceiling && r.idd.act_guard_ceiling <= r.idd.act_guard_hi)) {
                error = "part record " + path + ": idd.act_guard_nj_per_kb needs 0 < lo <= xpe_ceiling <= hi"; return false;
            }
        }
    }
    if (!n["component_factors"]) {
        error = "part record " + path + ": required field 'component_factors' is missing (the seven typical/maximum factors, or 'none' for a measured row)";
        return false;
    }
    if (n["component_factors"].IsMap()) {
        const YAML::Node c = n["component_factors"]; r.component_factors.apply = true;
        if (!need(c, "standby", r.component_factors.standby, error) || !need(c, "standby_premium", r.component_factors.standby_premium, error) ||
            !need(c, "act", r.component_factors.act, error) || !need(c, "burst_rd", r.component_factors.burst_rd, error) ||
            !need(c, "burst_wr", r.component_factors.burst_wr, error) || !need(c, "refresh", r.component_factors.refresh, error) ||
            !need(c, "pd", r.component_factors.pd, error) || !need(c, "source", r.component_factors.source, error)) {
            error = "part record " + path + ": component_factors: " + error; return false;
        }
    } else {
        std::string v; try { v = n["component_factors"].as<std::string>(); } catch (...) {}
        if (v != "none") { error = "part record " + path + ": component_factors must be a map of the seven factors or 'none'"; return false; }
        r.component_factors.apply = false;
    }
    if (!n["termination"] || !n["termination"].IsMap()) {
        error = "part record " + path + ": required field 'termination' is missing (the DQ termination scheme and electricals, or scheme none)";
        return false;
    }
    {
        const YAML::Node t2 = n["termination"];
        if (!need(t2, "scheme", r.termination.scheme, error) || !need(t2, "source", r.termination.source, error)) { error = "part record " + path + ": termination: " + error; return false; }
        if (r.termination.scheme != "none") {
            if (!need(t2, "vddq", r.termination.vddq, error) || !need(t2, "ron_rd", r.termination.ron_rd, error) || !need(t2, "rtt_rd", r.termination.rtt_rd, error) ||
                !need(t2, "ron_wr", r.termination.ron_wr, error) || !need(t2, "rtt_wr", r.termination.rtt_wr, error)) { error = "part record " + path + ": termination: " + error; return false; }
        }
    }
    if (n["iddq_band_pj_bit"]) {
        const YAML::Node q = n["iddq_band_pj_bit"]; r.iddq_band.valid = true;
        if (!need(q, "lo", r.iddq_band.lo, error) || !need(q, "hi", r.iddq_band.hi, error) || !need(q, "source", r.iddq_band.source, error)) { error = "part record " + path + ": iddq_band_pj_bit: " + error; return false; }
    }
    if (n["read_energy_guard"]) {   // 1.12.18 (user ruling 2026-10-23): guard only
        const YAML::Node g = n["read_energy_guard"]; r.read_energy_guard.valid = true;
        if (!need(g, "max_generation_gain", r.read_energy_guard.max_generation_gain, error) || !need(g, "gain_source", r.read_energy_guard.gain_source, error)) {
            error = "part record " + path + ": read_energy_guard: " + error; return false;
        }
        if (!g["references"] || !g["references"].IsSequence() || g["references"].size() == 0) {
            error = "part record " + path + ": read_energy_guard: required sequence 'references' is missing or empty"; return false;
        }
        for (const auto& e : g["references"]) {
            ReadEnergyGuardRec::Ref ref;
            if (!need(e, "label", ref.label, error) || !need(e, "pj_bit", ref.pj_bit, error) || !need(e, "source", ref.source, error)) {
                error = "part record " + path + ": read_energy_guard reference: " + error; return false;
            }
            r.read_energy_guard.references.push_back(ref);
        }
    }
    if (r.technology != tech) {
        error = "part record " + path + " says technology '" + r.technology + "' but was loaded for '" + tech + "'";
        return false;
    }
    /* (1.12.61) (user ruling 2026-10-25): the optional data_pattern block. */
    if (n["data_pattern"]) {
        const YAML::Node p = n["data_pattern"];
        auto& d = r.data_pattern; d.valid = true;
        if (!need(p, "form", d.form, error) || !need(p, "provenance", d.provenance, error) || !need(p, "source", d.source, error)) {
            error = "part record " + path + ": data_pattern: " + error; return false;
        }
        if (d.form == "ONES_LINEAR") {
            if (!need(p, "read_ones", d.read_ones, error) || !need(p, "write_ones", d.write_ones, error)) {
                error = "part record " + path + ": data_pattern: " + error; return false;
            }
            if (std::fabs(d.read_ones) >= 2.0 || std::fabs(d.write_ones) >= 2.0) {
                error = "part record " + path + ": data_pattern read_ones / write_ones must lie in (-2, 2) (the factor stays positive)"; return false;
            }
        } else if (d.form == "DQ_TOGGLE_RELATIVE") {
            if (!need(p, "floor_pj_bit", d.floor_pj_bit, error) || !need(p, "c_dq", d.c_dq, error) || !need(p, "c_tsv", d.c_tsv, error) ||
                !need(p, "c_bg", d.c_bg, error) || !need(p, "tsv_rate", d.tsv_rate, error) || !need(p, "bg_rate", d.bg_rate, error) ||
                !need(p, "ref_dq_rate", d.ref_dq_rate, error) || !need(p, "ref_tsv_rate", d.ref_tsv_rate, error) ||
                !need(p, "ref_bg_rate", d.ref_bg_rate, error)) {
                error = "part record " + path + ": data_pattern: " + error; return false;
            }
            if (p["apply_to_writes"]) {
                try { d.apply_to_writes = p["apply_to_writes"].as<bool>(); }
                catch (const std::exception&) { error = "part record " + path + ": data_pattern.apply_to_writes is not true/false"; return false; }
            }
            if (!(d.floor_pj_bit > 0.0)) { error = "part record " + path + ": data_pattern.floor_pj_bit must be positive"; return false; }
        } else {
            error = "part record " + path + ": data_pattern.form '" + d.form + "' is not ONES_LINEAR or DQ_TOGGLE_RELATIVE"; return false;
        }
    }
    /* 1.12.25: optional voltage variants and tRFC bases. The record's own values are the default's; the run's
     * selection (setDramRecordSelection) replaces them here, so every reader of the record sees one part. */
    if (n["variants"]) {
        const YAML::Node v = n["variants"];
        if (!v.IsMap() || !need(v, "default", r.variant_default, error)) { error = "part record " + path + ": variants: " + error; return false; }
        for (const auto& kv : v) {
            const std::string name = kv.first.as<std::string>();
            if (name == "default") continue;
            VariantRec vr;
            if (!need(kv.second, "vdd", vr.vdd, error) || !need(kv.second, "vddq", vr.vddq, error) || !need(kv.second, "source", vr.source, error)) {
                error = "part record " + path + ": variants." + name + ": " + error; return false;
            }
            r.variants[name] = vr;
        }
        const auto d = r.variants.find(r.variant_default);
        if (d == r.variants.end()) { error = "part record " + path + ": variants.default '" + r.variant_default + "' names no variant"; return false; }
        for (const auto& kv : r.idd.by_grade)
            if (!close(kv.second.vdd, d->second.vdd) || !close(r.termination.vddq, d->second.vddq)) {
                error = "part record " + path + ": idd vdd / termination.vddq (" + fmt(kv.second.vdd) + " / " + fmt(r.termination.vddq) +
                        ") must be the default variant's (" + r.variant_default + ": " + fmt(d->second.vdd) + " / " + fmt(d->second.vddq) + ")";
                return false;
            }
    }
    if (n["trfc_bases"]) {
        const YAML::Node v = n["trfc_bases"];
        if (!v.IsMap() || !need(v, "default", r.trfc_basis_default, error)) { error = "part record " + path + ": trfc_bases: " + error; return false; }
        for (const auto& kv : v) {
            const std::string name = kv.first.as<std::string>();
            if (name == "default") continue;
            TrfcBasisRec tb;
            if (!need(kv.second, "trfc_ns", tb.trfc_ns, error) || !need(kv.second, "source", tb.source, error)) {
                error = "part record " + path + ": trfc_bases." + name + ": " + error; return false;
            }
            r.trfc_bases[name] = tb;
        }
        const auto d = r.trfc_bases.find(r.trfc_basis_default);
        if (d == r.trfc_bases.end()) { error = "part record " + path + ": trfc_bases.default '" + r.trfc_basis_default + "' names no basis"; return false; }
        for (const auto& kv : r.idd.by_grade)
            if (!close(kv.second.trfc_ns, d->second.trfc_ns)) {
                error = "part record " + path + ": idd trfc_ns " + fmt(kv.second.trfc_ns) + " must be the default tRFC basis's (" +
                        r.trfc_basis_default + ": " + fmt(d->second.trfc_ns) + " ns)";
                return false;
            }
    }
    {
        const auto sit = selection().find(r.technology);
        const std::string want_v  = (sit != selection().end()) ? sit->second.first : std::string();
        const std::string want_tb = (sit != selection().end()) ? sit->second.second : std::string();
        if (!r.variants.empty()) {
            r.variant = want_v.empty() ? r.variant_default : want_v;
            const auto it = r.variants.find(r.variant);
            if (it == r.variants.end()) {
                error = "part record " + path + " has no voltage variant '" + r.variant + "' (variants:";
                for (const auto& kv : r.variants) error += " " + kv.first;
                error += ")"; return false;
            }
            for (auto& kv : r.idd.by_grade) kv.second.vdd = it->second.vdd;
            for (auto& kv : r.idd.by_width) kv.second.vdd = it->second.vdd;   // (1.12.48): the width rows share the variant
            r.termination.vddq = it->second.vddq;
            if (r.variant != r.variant_default) r.termination.source += "; 1.12.25: variant " + r.variant + ": " + it->second.source;
        } else if (!want_v.empty()) {
            error = "part record " + path + " has no voltage variants, so '" + want_v + "' cannot be selected"; return false;
        }
        if (!r.trfc_bases.empty()) {
            r.trfc_basis = want_tb.empty() ? r.trfc_basis_default : want_tb;
            const auto it = r.trfc_bases.find(r.trfc_basis);
            if (it == r.trfc_bases.end()) {
                error = "part record " + path + " has no tRFC basis '" + r.trfc_basis + "' (trfc_bases:";
                for (const auto& kv : r.trfc_bases) error += " " + kv.first;
                error += ")"; return false;
            }
            for (auto& kv : r.idd.by_grade) kv.second.trfc_ns = it->second.trfc_ns;
            for (auto& kv : r.idd.by_width) kv.second.trfc_ns = it->second.trfc_ns;   // (1.12.48): and the tRFC basis
        }
        /* a basis named for a record without trfc_bases is refused by the caller, which knows every record of the run */
    }
    out = r;
    return true;
}

namespace {
std::map<std::string, DramPartRecord>& registry() { static std::map<std::string, DramPartRecord> m; return m; }
}

void registerDramPartRecord(const DramPartRecord& rec) { registry()[rec.technology] = rec; }
bool hasDramPartRecord(const std::string& tech) { return registry().count(tech) > 0; }
int dramRecordGradeMTs(const std::string& tech) {   // 1.12.18: "DDR5_5600B" -> 5600
    const std::string& n = dramPartRecord(tech).timing_preset;
    const auto us = n.find('_');
    int v = 0;
    for (size_t i = (us == std::string::npos ? n.size() : us + 1); i < n.size() && n[i] >= '0' && n[i] <= '9'; ++i) v = v * 10 + (n[i] - '0');
    return v;
}
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
    if (rec.channels_per_die < 1 || (!stacked && rec.channels > 0 && rec.channels % rec.channels_per_die != 0))
        errors.push_back("channels_per_die must be at least 1 and divide channels");   // (1.12.52) GDDR6 die count
    /* 1.11.104 (IDD-RECORDS step 1): the measured data's own consistency.
     * No ordering rule between IDD3N and IDD2N: the measured HBM2 means have
     * IDD3N below IDD2N (the row's stated ill-conditioning), so that is data,
     * not an error. */
    {
        static const std::set<std::string> prov = {"MEASURED", "CALIBRATED", "DERIVED"};
        static const std::set<std::string> bank = {"ALL_BANKS", "ONE_BANK", "UNVERIFIED"};
        static const std::set<std::string> basis = {"per device", "per channel"};
        static const std::set<std::string> schemes = {"POD", "SSTL", "LVSTL", "none"};
        if (!prov.count(rec.idd.provenance)) errors.push_back("idd.provenance '" + rec.idd.provenance + "' is not MEASURED, CALIBRATED or DERIVED");
        if (!bank.count(rec.idd.idd3n_bank_state)) errors.push_back("idd.idd3n_bank_state '" + rec.idd.idd3n_bank_state + "' is not ALL_BANKS, ONE_BANK or UNVERIFIED");
        if (!basis.count(rec.idd.basis)) errors.push_back("idd.basis '" + rec.idd.basis + "' is not 'per device' or 'per channel'");
        if (rec.idd.channels_basis < 1) errors.push_back("idd.channels_basis must be at least 1");
        if (stacked && rec.idd.channels_basis != rec.channels)
            errors.push_back("idd.channels_basis " + std::to_string(rec.idd.channels_basis) + " != channels " + std::to_string(rec.channels) + " (a stacked row aggregates the stack's channels)");
        if (t == "DDR5") {
            for (int g : {3200, 4800, 5600})
                if (!rec.idd.by_grade.count(g)) errors.push_back("idd.by_grade lacks the " + std::to_string(g) + " MT/s row (the three DDR5 grades this build transcribes)");
        } else if (!rec.idd.by_grade.count(0) || rec.idd.by_grade.size() != 1) {
            errors.push_back("idd must be a single row (no by_grade) for " + t);
        }
        for (const auto& kv : rec.idd.by_width) {   // (1.12.48) (user ruling 2026-10-25)
            const IddRow& r = kv.second;
            const std::string k = "idd.by_width " + kv.first;
            if (r.idd0 < 0.0 || r.idd2n < 0.0 || r.idd3n < 0.0 || r.idd4r < 0.0 || r.idd4w < 0.0 || r.idd5 < 0.0 || r.idd2p < 0.0) errors.push_back(k + ": currents must be non-negative");
            if (r.idd2p > r.idd2n) errors.push_back(k + ": idd2p " + fmt(r.idd2p) + " exceeds idd2n " + fmt(r.idd2n));
            if (r.idd3n < r.idd2n || r.idd4r < r.idd3n || r.idd4w < r.idd3n || r.idd5 < r.idd2n) errors.push_back(k + ": needs idd3n >= idd2n, idd4r / idd4w >= idd3n, idd5 >= idd2n");
        }
        for (const auto& kv : rec.idd.by_grade) {
            const IddRow& r = kv.second;
            const std::string k = kv.first ? "idd.by_grade " + std::to_string(kv.first) : std::string("idd");
            if (!(r.vdd > 0.0)) errors.push_back(k + ": vdd must be positive");
            if (r.idd0 < 0.0 || r.idd2n < 0.0 || r.idd3n < 0.0 || r.idd4r < 0.0 || r.idd4w < 0.0 || r.idd5 < 0.0 || r.idd2p < 0.0) errors.push_back(k + ": currents must be non-negative");
            if (r.idd2p > r.idd2n) errors.push_back(k + ": idd2p " + fmt(r.idd2p) + " exceeds idd2n " + fmt(r.idd2n) + " (power-down cannot draw more than precharge standby)");
            if (!(r.trfc_ns > 0.0) || !(r.trefi_ns > r.trfc_ns)) errors.push_back(k + ": trfc_ns must be positive and below trefi_ns");
            if (r.has_iddq && !(r.vddq > 0.0)) errors.push_back(k + ": iddq.vddq must be positive");
            if (r.has_ipp && !(r.vpp > 0.0)) errors.push_back(k + ": ipp.vpp must be positive");
            if (r.has_ipp && ((r.ipp0 < 0.0 && r.ipp0 != -1.0) || (r.ipp4r < 0.0 && r.ipp4r != -1.0) || (r.ipp4w < 0.0 && r.ipp4w != -1.0) || (r.ipp5b < 0.0 && r.ipp5b != -1.0)))
                errors.push_back(k + ": ipp.ipp0 / ipp4r / ipp4w / ipp5b must be non-negative when given");   // 1.12.18
            if (r.has_io_share && (!(r.idd4r_io >= 0.0) || !(r.idd4w_io >= 0.0) || r.idd4r_io > r.idd4r - r.idd3n || r.idd4w_io > r.idd4w - r.idd3n))
                errors.push_back(k + ": io_share must be non-negative and inside the burst excess (idd4r - idd3n, idd4w - idd3n)");   // 1.12.18
            if (r.has_io_share && r.io_share_provenance != "DERIVED" && r.io_share_provenance != "MEASURED")
                errors.push_back(k + ": io_share.provenance must be DERIVED or MEASURED");   // 1.12.18
            if (r.io_share_anchored) {   // 1.12.18 (user ruling 2026-10-23): the share IS its anchor at the channel rate
                if (!(r.io_anchor_rd_pj_bit > 0.0) || !(r.io_anchor_wr_pj_bit > 0.0) || !(r.io_channel_gbps > 0.0))
                    errors.push_back(k + ": io_share.anchor_read_pj_bit / anchor_write_pj_bit / channel_gbps must be positive");
                else {
                    const double rd = r.io_anchor_rd_pj_bit * r.io_channel_gbps / r.vdd, wr = r.io_anchor_wr_pj_bit * r.io_channel_gbps / r.vdd;
                    if (std::fabs(rd - r.idd4r_io) > 0.05 + 1e-9 || std::fabs(wr - r.idd4w_io) > 0.05 + 1e-9)
                        errors.push_back(k + ": io_share idd4r_ma / idd4w_ma " + fmt(r.idd4r_io) + " / " + fmt(r.idd4w_io) + " != anchor x channel_gbps / vdd = "
                                         + fmt(rd) + " / " + fmt(wr) + " mA (to 0.05 mA)");
                }
                if (rec.component_factors.apply)
                    errors.push_back(k + ": an anchored io_share prices the DQ I/O at its own value; component factors would scale only the burst side of it");
            }
            if (r.source.empty()) errors.push_back(k + ": source must name the datasheet or measurement");
        }
        if (rec.component_factors.apply) {
            const ComponentFactorsRec& c = rec.component_factors;
            const std::pair<const char*, double> fs[] = {{"standby", c.standby}, {"standby_premium", c.standby_premium}, {"act", c.act}, {"burst_rd", c.burst_rd}, {"burst_wr", c.burst_wr}, {"refresh", c.refresh}, {"pd", c.pd}};
            for (const auto& f : fs) if (!(f.second > 0.0) || f.second > 1.0) errors.push_back(std::string("component_factors.") + f.first + " " + fmt(f.second) + " is outside (0, 1]");
        }
        if (!schemes.count(rec.termination.scheme)) errors.push_back("termination.scheme '" + rec.termination.scheme + "' is not POD, SSTL, LVSTL or none");
        if (rec.termination.scheme != "none") {
            if (!(rec.termination.vddq > 0.0)) errors.push_back("termination.vddq must be positive");
            if (!(rec.termination.ron_rd > 0.0) || !(rec.termination.ron_wr > 0.0)) errors.push_back("termination.ron_rd / ron_wr must be positive");
            if (rec.termination.rtt_rd < 0.0 || rec.termination.rtt_wr < 0.0) errors.push_back("termination.rtt_rd / rtt_wr must be non-negative (0 = no DC termination path)");
        }
        if (rec.iddq_band.valid && !(rec.iddq_band.lo > 0.0 && rec.iddq_band.hi >= rec.iddq_band.lo)) errors.push_back("iddq_band_pj_bit must satisfy 0 < lo <= hi");
        if (rec.read_energy_guard.valid) {   // 1.12.18 (user ruling 2026-10-23)
            if (!(rec.read_energy_guard.max_generation_gain >= 0.0 && rec.read_energy_guard.max_generation_gain < 1.0))
                errors.push_back("read_energy_guard.max_generation_gain must be in [0, 1)");
            for (const auto& ref : rec.read_energy_guard.references)
                if (!(ref.pj_bit > 0.0)) errors.push_back("read_energy_guard reference " + ref.label + ": pj_bit must be positive");
        }
    }
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
    o << "; idd: " << rec.idd.provenance << " row (" << rec.idd.basis << ", " << rec.idd.idd3n_bank_state << ", "
      << rec.idd.by_grade.size() << " grade row(s)) the source (IDD-RECORDS step 2, 1.11.105)";
    if (!rec.variant.empty() && rec.variants.count(rec.variant)) {   // 1.12.19
        const VariantRec& v = rec.variants.at(rec.variant);
        o << "; variant " << rec.variant << " (VDD " << v.vdd << " V, VDDQ " << v.vddq << " V)"
          << (rec.variant == rec.variant_default ? " (the record's default)" : " (memory.dram.ddr3_variant)");
    }
    if (!rec.trfc_basis.empty() && rec.trfc_bases.count(rec.trfc_basis))
        o << "; tRFC " << rec.trfc_bases.at(rec.trfc_basis).trfc_ns << " ns (" << rec.trfc_basis << ")"
          << (rec.trfc_basis == rec.trfc_basis_default ? " (the record's default)" : " (memory.dram.trfc_basis)");
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
        !coreInt(io, "in_order", "fetch_width_bytes", 1, 64, out.in_order.fetch_width_bytes, out.file, error) ||   // PIMID (1.12.71)
        !coreInt(oo, "ooo", "mispredict_penalty_cycles", 0, 1000, out.ooo.mispredict_penalty_cycles, out.file, error) ||
        !coreInt(oo, "ooo", "fetch_width_bytes", 1, 64, out.ooo.fetch_width_bytes, out.file, error) ||
        !coreInt(oo, "ooo", "retire_width", 1, 64, out.ooo.retire_width, out.file, error))
        return false;
    /* 1.12.18: the TLB geometry, required like every field.
     * Ranges: McPAT sizes a TLB as entries x line bytes, so 1..65536 entries;
     * associativity 1..64 (a power of two, CACTI's rule) and at most the
     * entries of each array it describes. */
    if (!coreInt(oo, "ooo", "itlb_entries", 1, 65536, out.ooo.itlb_entries, out.file, error) ||
        !coreInt(oo, "ooo", "itlb_large_entries", 0, 65536, out.ooo.itlb_large_entries, out.file, error) ||
        !coreInt(oo, "ooo", "dtlb_entries", 1, 65536, out.ooo.dtlb_entries, out.file, error) ||
        !coreInt(oo, "ooo", "dtlb_large_entries", 0, 65536, out.ooo.dtlb_large_entries, out.file, error) ||
        !coreInt(oo, "ooo", "tlb_associativity", 1, 64, out.ooo.tlb_associativity, out.file, error) ||
        !coreInt(oo, "ooo", "stlb_entries", 0, 1048576, out.ooo.stlb_entries, out.file, error))
        return false;
    {
        const int a = out.ooo.tlb_associativity;
        if ((a & (a - 1)) != 0 || a > out.ooo.dtlb_entries || a > out.ooo.itlb_entries) {
            error = "core record " + out.file + ": ooo.tlb_associativity " + std::to_string(a) +
                    " must be a power of two no larger than either first-level TLB";
            return false;
        }
    }
    if (!io["retire_width"] || io["retire_width"].as<std::string>("") != "issue") {
        error = "core record " + out.file + ": in_order.retire_width must be 'issue' (InOrderCore issues and "
                "commits on one cursor, so it retires exactly its issue width; an integer would price a retire "
                "limit the timing core does not have)";
        return false;
    }
    out.in_order.retire_width = -1;
    /* 1.12.1 (ticket #114): the in-order element's branch predictor --
     * required like every field (no value lives in code); pag | none. */
    if (!io["branch_predictor"]) {
        error = "core record " + out.file + ": 'in_order' lacks 'branch_predictor' (in_order.branch_predictor: "
                "pag | none, required since 1.12.1)";
        return false;
    }
    out.in_order.branch_predictor = io["branch_predictor"].IsScalar() ? io["branch_predictor"].Scalar() : std::string();
    if (out.in_order.branch_predictor != "pag" && out.in_order.branch_predictor != "none") {
        error = "core record " + out.file + ": in_order.branch_predictor '" + out.in_order.branch_predictor +
                "' is not pag or none (pag: the PAg direction predictor + BTB + RAS; none: no prediction "
                "structure, static not-taken)";
        return false;
    }
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
      << rec.in_order.resteer_penalty_cycles << " cyc, fetch " << rec.in_order.fetch_width_bytes
      << " B/cyc, retire = issue width, branch predictor "
      << rec.in_order.branch_predictor << "; ooo: mispredict "
      << rec.ooo.mispredict_penalty_cycles << " cyc, fetch " << rec.ooo.fetch_width_bytes
      << " B/cyc, retire " << rec.ooo.retire_width
      << "; TLB: ITLB " << rec.ooo.itlb_entries << "+" << rec.ooo.itlb_large_entries
      << ", DTLB " << rec.ooo.dtlb_entries << "+" << rec.ooo.dtlb_large_entries
      << " (4 KB + large), " << rec.ooo.tlb_associativity << "-way, STLB "
      << rec.ooo.stlb_entries << ")";
    return o.str();
}

}}  // namespace pimid::params
