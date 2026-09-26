#ifndef RAMULATOR_PIMID_ORG_PROBE_H
#define RAMULATOR_PIMID_ORG_PROBE_H
/* 1.11.66 (PIMID, round 5 A6): a C++17-safe window onto the organization of
 * the device Ramulator actually instantiated. dram/dram.h is C++20 (concepts,
 * consteval) and cannot be included from PIMID's C++17 translation units, so
 * the shape cross-check in RamulatorWrapper::createRamulatorInstance() reads
 * the live level sizes through this plain function instead. Implemented in
 * pimid_org_probe.cpp, compiled inside the ramulator-dram object library. */
namespace Ramulator { class IMemorySystem; }
namespace Ramulator { namespace pimid_probe {
struct LiveOrganization {
    long long channels       = 1;   // 1 where the level does not exist
    long long pseudochannels = 1;   // 1 where the level does not exist
    long long bankgroups     = 1;   // 1 where the level does not exist
    long long banks          = -1;  // banks per (bankgroup or pseudochannel or device)
    long long rows           = -1;
    long long columns        = -1;
    /* 1.11.91 (audit R8-3): the instantiated channel's width and DQ, so the
     * wrapper can bind pimid_energy::accessPathFor() to the device Ramulator
     * actually simulates (DDR3/4/5). -1 where unreadable. */
    long long channel_width  = -1;
    long long dq             = -1;
    bool      valid          = false;
};
/* Returns valid=false if no IDRAM interface is reachable. Never throws. */
LiveOrganization liveOrganization(IMemorySystem* sys);
}}
#endif
