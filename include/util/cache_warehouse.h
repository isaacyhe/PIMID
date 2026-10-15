/**
 * @file cache_warehouse.h
 * @brief Centralized characterization-cache warehouse for PIMID.
 *
 * PIMID memoizes expensive, deterministic backend characterizations (today:
 * NVSim's multi-minute design-space search; other backends may opt in later).
 * This module is the single source of truth for:
 *   - WHETHER caching is active and in which direction (the cache *mode*),
 *   - WHERE cached artifacts live (the *warehouse*: one root, per-backend
 *     subdirs, plus an append-only manifest for inspection / staleness checks).
 *
 * Modes (read/write directions):
 *   RW  - read existing entries, write new ones                 (default)
 *   RO  - read existing entries, never write                    (frozen warehouse)
 *   WO  - ignore existing entries, recompute and overwrite      (refresh stale data)
 *   OFF - bypass the warehouse entirely                         (always recompute)
 *
 * Resolution precedence for both mode and dir (high -> low):
 *   CLI flag  >  env var  >  YAML config  >  built-in default
 * All of that precedence logic lives in configure(); callers just pass the raw
 * strings they parsed (any may be empty / "unset").
 */
#ifndef PIMID_UTIL_CACHE_WAREHOUSE_H
#define PIMID_UTIL_CACHE_WAREHOUSE_H

#include <string>

namespace pimid {
namespace cache {

enum class Mode { RW, RO, WO, OFF };

/**
 * Resolve and store the global warehouse configuration. Call once at startup,
 * before any backend characterization. Empty strings mean "not set at this
 * level" and fall through to the next precedence tier.
 *
 * @param cli_mode  CLI-provided mode ("rw"|"ro"|"wo"|"off"|"") -- highest priority.
 * @param cli_dir   CLI-provided warehouse root, or "".
 * @param yaml_mode YAML-provided mode, or "".
 * @param yaml_dir  YAML-provided warehouse root, or "".
 *
 * Env vars consulted (between CLI and YAML): PIMID_CACHE_MODE, PIMID_CACHE_DIR,
 * and PIMID_CACHE_DISABLE (=1 -> OFF). Back-compat: PIMID_NVSIM_CACHE_DIR and
 * PIMID_NVSIM_CACHE_DISABLE are still honored if the generic ones are unset.
 */
void configure(const std::string& cli_mode, const std::string& cli_dir,
               const std::string& yaml_mode, const std::string& yaml_dir);

Mode mode();
bool readEnabled();   ///< true for RW, RO  (may consult the warehouse)
bool writeEnabled();  ///< true for RW, WO  (may persist new entries)

/// Resolved warehouse root (e.g. <pimid>/cache). Valid after configure();
/// if configure() was never called, lazily resolves defaults+env on first use.
const std::string& warehouseRoot();

/// "<root>/<backend>" -- created (mkdir -p) on demand. e.g. backendDir("nvsim").
std::string backendDir(const std::string& backend);

/* 1.12.5 (ruling F5, user 2026-10-15): the warehouse is the only store. The
 * pre-1.11.52 per-user location (~/.cache/pimid/<backend>) was read as a
 * fallback from 1.11.52 to 1.12.4 (legacyBackendDir); nothing reads it now. */

/// Version stamp written into manifest records (for staleness detection).
std::string toolVersion();

/**
 * Append one record to the warehouse manifest (<root>/index.jsonl), one JSON
 * object per line (append-only -> safe for many concurrent sweep processes).
 * timestamp and tool_version are added automatically. params_json / value_json
 * are caller-formatted JSON *object bodies without the surrounding braces*, e.g.
 *   recordManifest("nvsim", "t1_c8388608_n22",
 *                  "\"nvm_type\":1,\"capacity_bytes\":8388608,\"node_nm\":22",
 *                  "\"read_latency_s\":1.2e-9,\"area_mm2\":0.05");
 * No-op when writeEnabled() is false.
 */
void recordManifest(const std::string& backend, const std::string& key,
                    const std::string& params_json, const std::string& value_json);

Mode parseMode(const std::string& s);  ///< "rw"/"ro"/"wo"/"off" (case-insensitive); default RW.
const char* modeName(Mode m);

}  // namespace cache
}  // namespace pimid

#endif  // PIMID_UTIL_CACHE_WAREHOUSE_H
