#ifndef PIMID_COMMON_DEBUG_CHECKS_H
#define PIMID_COMMON_DEBUG_CHECKS_H
/* Diagnostic lines for internal verification (per-block derivations,
 * guard and reference comparisons, measured-factor counters). They are not
 * part of the reports and are off by default; PIMID_DEBUG_CHECKS=1 turns
 * them on. The switch is read once (main latches it at startup) and changes
 * only what is printed, never a computed value. */
#include <cstdlib>
#include <cstring>
#include <ostream>

namespace pimid {

inline bool debugChecks() {
    static const bool on = [] {
        const char* e = std::getenv("PIMID_DEBUG_CHECKS");
        return e != nullptr && e[0] != '\0' && std::strcmp(e, "0") != 0;
    }();
    return on;
}

/* `os` when the switch is on; otherwise a sink that discards everything. */
inline std::ostream& debugStream(std::ostream& os) {
    static std::ostream sink(nullptr);
    return debugChecks() ? os : sink;
}

}  // namespace pimid

#endif  // PIMID_COMMON_DEBUG_CHECKS_H
