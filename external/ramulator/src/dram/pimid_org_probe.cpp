#include "dram/pimid_org_probe.h"
#include "dram/dram.h"
#include "memory_system/memory_system.h"
#include "base/base.h"

namespace Ramulator { namespace pimid_probe {

LiveOrganization liveOrganization(IMemorySystem* sys) {
    LiveOrganization o;
    if (!sys) return o;
    try {
        IDRAM* dram = sys->get_ifce<IDRAM>();
        if (!dram) return o;
        long long v;
        v = dram->get_level_size("channel");       if (v > 0) o.channels = v;
        v = dram->get_level_size("pseudochannel"); if (v > 0) o.pseudochannels = v;
        v = dram->get_level_size("bankgroup");     if (v > 0) o.bankgroups = v;
        o.banks   = dram->get_level_size("bank");
        o.rows    = dram->get_level_size("row");
        o.columns = dram->get_level_size("column");
        o.channel_width = dram->m_channel_width;          // 1.11.91 (R8-3)
        o.dq            = dram->m_organization.dq;        // 1.11.91 (R8-3)
        o.valid   = (o.banks > 0 && o.rows > 0 && o.columns > 0);
    } catch (...) {
        o.valid = false;
    }
    return o;
}

}}
