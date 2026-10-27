/**
 * @file cache_warehouse.cpp
 * @brief Implementation of the centralized PIMID cache warehouse.
 */
#include <iostream>
#include "util/cache_warehouse.h"

#include <cstdlib>
#include <cctype>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>

namespace pimid {
namespace cache {

namespace {

/* Bump when the cached artifact format / characterization semantics change,
 * so the manifest can distinguish entries produced by older PIMID builds.
 *
 * 1.11.57 (latent C014): this had never been bumped, through three releases
 * that each changed what an entry MEANS or which inputs produce it -- 1.11.49
 * added the device corner to the key, 1.11.52 added the temperature, and
 * 1.11.56 put the configured cache line width into the query and rewrote the
 * NVSim delay accessors. Nothing reachable differed, because the corner and
 * the temperature also went into the FILENAME, so a stale entry could not be
 * served for a different one. The trap is the semantic change that does NOT
 * move the filename -- 1.11.56's line width and accessor rewrite are exactly
 * that shape -- and the legacy-store copy-forward is the path that would
 * import such an entry from a pre-1.11.52 tree.
 *
 * Set to the release that last changed characterization semantics. The rule
 * from here: this string moves whenever a release changes what a cached
 * artifact means, whether or not the filename moves with it. */
/* (1.12.93): the PCM cell read energy (sample_PCRAM.cell -ReadEnergy) is read and priced;
 * the PCM cell SET/RESET energies are Lee et al. ISCA 2009 Table 1. */
/* (1.12.94): the PCM write supply efficiency is the published charge-pump 0.388. */
/* (1.12.108): a PCM write is priced at the mean of the SET and RESET energies. */
const char* kToolVersion = "1.12.108";

struct State {
    Mode mode = Mode::RW;
    std::string root;
    bool configured = false;
};

State& state() {
    static State s;
    return s;
}

std::mutex& mtx() {
    static std::mutex m;
    return m;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const char* env(const char* name) {
    const char* v = std::getenv(name);
    return (v && v[0]) ? v : nullptr;
}

/* 1.11.52 (user ruling): the cache SHIPS WITH PIMID and starts EMPTY.
 *
 * It used to default to ~/.cache/pimid, which put the tool's generated
 * memory designs in a per-user location outside the tree -- invisible to
 * anyone inspecting the checkout, not shipped, and silently different
 * between two trees on one machine. The store belongs beside the simulator
 * that fills it: `<pimid>/cache/<backend>/`. The directory is committed
 * empty (.gitkeep + README) and populated at runtime.
 *
 * Located from the RUNNING BINARY, not the working directory: readlink
 * /proc/self/exe, take its directory, and drop a trailing /build so both
 * `pimid/build/pimid` and an installed `pimid/bin/pimid` resolve to
 * `pimid/cache`. --cache-dir / PIMID_CACHE_DIR still override; without them
 * and without /proc the run is refused (1.12.5, ruling F5: no wrong folders
 * and no old cache files -- the pre-1.11.52 per-user store ~/.cache/pimid is
 * neither a default nor a fallback any more). */
std::string exeDir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return std::string();
    buf[n] = '\0';
    std::string p(buf);
    size_t slash = p.find_last_of('/');
    if (slash == std::string::npos) return std::string();
    return p.substr(0, slash);
}

std::string defaultRoot() {
    std::string d = exeDir();
    if (!d.empty()) {
        const std::string build = "/build";
        if (d.size() > build.size() &&
            d.compare(d.size() - build.size(), build.size(), build) == 0)
            d = d.substr(0, d.size() - build.size());
        else {
            size_t slash = d.find_last_of('/');
            std::string last = (slash == std::string::npos) ? d : d.substr(slash + 1);
            if (last == "bin" && slash != std::string::npos) d = d.substr(0, slash);
        }
        return d + "/cache";
    }
    std::cerr << "[cache] FATAL: the cache warehouse root could not be located from the running binary"
                 " (/proc/self/exe unreadable) and none was given. Pass --cache-dir <path> or set"
                 " PIMID_CACHE_DIR; no home-directory store stands in (1.12.5, ruling F5)." << std::endl;
    std::exit(2);
}

// mkdir -p
void ensureDir(const std::string& path) {
    if (path.empty()) return;
    std::string acc;
    for (size_t i = 0; i < path.size(); ++i) {
        acc += path[i];
        if (path[i] == '/' && acc.size() > 1) {
            mkdir(acc.c_str(), 0755);  // ignore EEXIST
        }
    }
    mkdir(acc.c_str(), 0755);
}

// Resolve once, honoring full precedence. Idempotent.
void resolveIfNeeded() {
    State& s = state();
    if (s.configured) return;
    // configure() not called yet -> resolve from env + defaults only.
    configure("", "", "", "");
}

}  // namespace

Mode parseMode(const std::string& s) {
    std::string m = lower(s);
    if (m == "ro" || m == "read-only" || m == "readonly")  return Mode::RO;
    if (m == "wo" || m == "write-only" || m == "writeonly") return Mode::WO;
    if (m == "off" || m == "none" || m == "disable" || m == "disabled") return Mode::OFF;
    return Mode::RW;  // "rw", "on", "", anything else
}

const char* modeName(Mode m) {
    switch (m) {
        case Mode::RW:  return "rw";
        case Mode::RO:  return "ro";
        case Mode::WO:  return "wo";
        case Mode::OFF: return "off";
    }
    return "rw";
}

void configure(const std::string& cli_mode, const std::string& cli_dir,
               const std::string& yaml_mode, const std::string& yaml_dir) {
    std::lock_guard<std::mutex> lk(mtx());
    State& s = state();

    // ---- Mode: CLI > env > YAML > default(RW) ----
    std::string mode_str;
    if (!cli_mode.empty())            mode_str = cli_mode;
    else if (env("PIMID_CACHE_DISABLE")) mode_str = "off";
    else if (env("PIMID_CACHE_MODE")) mode_str = env("PIMID_CACHE_MODE");
    else if (env("PIMID_NVSIM_CACHE_DISABLE")) mode_str = "off";  // back-compat
    else if (!yaml_mode.empty())      mode_str = yaml_mode;
    {   /* 1.11.106 (census S2): an unknown mode word used to run rw in silence. */
        static const char* kWords[] = { "", "rw", "on", "ro", "read-only", "readonly", "wo", "write-only", "writeonly", "off", "none", "disable", "disabled" };
        const std::string m = lower(mode_str); bool ok = false;
        for (const char* w : kWords) if (m == w) { ok = true; break; }
        if (!ok) {
            std::cerr << "[config] FATAL: cache mode '" << mode_str << "' (cache.mode, --cache or PIMID_CACHE_MODE) is not a value this build has. "
                         "Valid: rw, ro, wo, off (aliases: on, read-only, readonly, write-only, writeonly, none, disable, disabled). "
                         "An unknown word used to run rw in silence." << std::endl;
            std::exit(2);
        }
    }
    s.mode = parseMode(mode_str);  // empty -> RW

    // ---- Dir: CLI > env > YAML > default ----
    std::string dir;
    if (!cli_dir.empty())                  dir = cli_dir;
    else if (env("PIMID_CACHE_DIR"))       dir = env("PIMID_CACHE_DIR");
    else if (env("PIMID_NVSIM_CACHE_DIR")) {
        /* 1.12.5 (ruling F5): the pre-1.11.52 variable named the nvsim subdir of
         * the old per-user store. It is no longer a location: refused, so a stale
         * environment cannot point a run at old cache files in silence. */
        std::cerr << "[cache] FATAL: PIMID_NVSIM_CACHE_DIR is set. It named the pre-1.11.52 per-user store, which this"
                     " build does not read; give the warehouse root with PIMID_CACHE_DIR or --cache-dir (1.12.5)." << std::endl;
        std::exit(2);
    }
    else if (!yaml_dir.empty())            dir = yaml_dir;
    else                                   dir = defaultRoot();
    s.root = dir;

    s.configured = true;
}

Mode mode() {
    resolveIfNeeded();
    return state().mode;
}

bool readEnabled() {
    Mode m = mode();
    return m == Mode::RW || m == Mode::RO;
}

bool writeEnabled() {
    Mode m = mode();
    return m == Mode::RW || m == Mode::WO;
}

const std::string& warehouseRoot() {
    resolveIfNeeded();
    return state().root;
}

std::string backendDir(const std::string& backend) {
    std::string d = warehouseRoot() + "/" + backend;
    ensureDir(d);
    return d;
}

std::string toolVersion() {
    return kToolVersion;
}

void recordManifest(const std::string& backend, const std::string& key,
                    const std::string& params_json, const std::string& value_json) {
    if (!writeEnabled()) return;
    const std::string root = warehouseRoot();
    ensureDir(root);
    const std::string manifest = root + "/index.jsonl";

    std::time_t now = std::time(nullptr);

    // Append one self-contained JSON object on its own line. Append-only keeps
    // concurrent sweep processes from corrupting each other; a reader dedups by
    // (backend,key) taking the last line, and can flag tool_version mismatches.
    std::lock_guard<std::mutex> lk(mtx());
    std::ofstream f(manifest, std::ios::app);
    if (!f.is_open()) return;
    f << "{"
      << "\"backend\":\"" << backend << "\","
      << "\"key\":\"" << key << "\","
      << "\"tool_version\":\"" << kToolVersion << "\","
      << "\"timestamp\":" << static_cast<long long>(now) << ","
      << "\"params\":{" << params_json << "},"
      << "\"value\":{" << value_json << "}"
      << "}\n";
}

}  // namespace cache
}  // namespace pimid
