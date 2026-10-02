// part_records.h -- shipped parameter records (the values that used to be
// bound into the code), loaded at startup.
//
// 1.11.94 (user ruling 2026-09-27 "we should not bind the sim and param hard",
// sweep-94 rulings 13a/16/22): a calibrated set is ONE record per part, all
// fields required, provenance beside each value, selectable or replaceable as
// a whole, never patched per field. This release is STEP 1 of the two-step
// migration: the loader reads the record AND cross-checks every field against
// the code tables that still hold the same values; any disagreement refuses
// the run. Step 2 (a later release) deletes the code tables and the record
// becomes the single source. No simulated number depends on the record in
// step 1 -- the cross-check is the proof that file == code.
#ifndef PIMID_PARAMS_PART_RECORDS_H
#define PIMID_PARAMS_PART_RECORDS_H

#include <string>
#include <vector>

namespace pimid {
namespace params {

/* One rung of a refresh ladder: tREFI is multiplied by `factor` when the
 * operating temperature is above `above_c` (JEDEC temperature-compensated
 * refresh); rungs are listed in ascending temperature. */
struct RefreshRung { double above_c; double factor; };

/* The DRAM part record. Field names mirror the YAML keys. */
struct DramPartRecord {
    std::string file;              // the record file that was loaded
    std::string technology;        // DDR3 .. HBM3 (canonical PIMID name)
    std::string part;              // vendor part or family name
    std::string vendor;
    std::string generation;        // vendor generation label (3x/2x, 1z, 1a, ...)
    double feature_nm = 0.0;       // the generation's feature size F
    int cell_factor_f2 = 0;        // cell area in F^2 (6 = buried wordline)
    double density_mb_per_mm2 = 0; // measured full-die density
    std::string density_source;
    std::string organization_preset;   // Ramulator org preset name
    std::string timing_preset;         // Ramulator timing preset name
    int channels = 0;              // channels the preset instantiates (HBM: per stack)
    int stack_dies = 0;            // HBM core dies per stack (0 for non-stacked)
    double die_capacity_gb = 0.0;  // HBM core die capacity (0 for non-stacked)
    std::vector<RefreshRung> refresh_ladder;
};

/* The directory the records are read from: $PIMID_PARAMS when set, else the
 * tree the binary was built from (PIMID_PARAMS_DIR, compiled in like
 * CACTI_DATA_DIR). */
std::string paramsDir();

/* Load params/dram/<tech>.yaml. Returns false with `error` set when the file
 * is missing, unreadable, or lacks a required field. Never substitutes. */
bool loadDramPartRecord(const std::string& tech, DramPartRecord& out, std::string& error);

/* Step-1 cross-check: every record field that the code tables also carry
 * must agree. Returns false with every disagreement listed in `errors`. */
bool crossCheckDramPartRecord(const DramPartRecord& rec, std::vector<std::string>& errors,
                              bool check_presets = true);

/* One line stating the record in force (the only print-out; derivations stay
 * in the record file and the code comments). */
std::string describeDramPartRecord(const DramPartRecord& rec);

} // namespace params
} // namespace pimid

#endif
