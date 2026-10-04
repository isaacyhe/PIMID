#ifndef     RAMULATOR_CONTROLLER_REFRESH_H
#define     RAMULATOR_CONTROLLER_REFRESH_H

#include <vector>
#include <string>

#include "base/base.h"


namespace Ramulator {

class IRefreshManager {
  RAMULATOR_REGISTER_INTERFACE(IRefreshManager, "RefreshManager", "Refresh Manager Interface.");

  public:
    virtual void tick() = 0;
    /* PIMID 1.11.98 (row 28): the refresh commands issued so far (cumulative);
     * 0 for a manager that does not count them. */
    virtual size_t pimid_num_refresh() const { return 0; }
};

}        // namespace Ramulator


#endif   // RAMULATOR_CONTROLLER_REFRESH_H