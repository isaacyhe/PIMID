#ifndef RAMULATOR_CONTROLLER_CONTROLLER_H
#define RAMULATOR_CONTROLLER_CONTROLLER_H

#include <vector>
#include <deque>

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include "base/base.h"
#include "dram/dram.h"
#include "dram_controller/scheduler.h"
#include "dram_controller/plugin.h"
#include "dram_controller/refresh.h"
#include "dram_controller/rowpolicy.h"


namespace Ramulator {

class IDRAMController : public Clocked<IDRAMController> {
  RAMULATOR_REGISTER_INTERFACE(IDRAMController, "Controller", "Memory Controller Interface");

  public:
    IDRAM*  m_dram = nullptr;          
    IScheduler*   m_scheduler = nullptr;
    IRefreshManager*   m_refresh = nullptr;
    IRowPolicy*   m_rowpolicy = nullptr;
    std::vector<IControllerPlugin*> m_plugins;

    int m_channel_id = -1;
  public:
    /**
     * @brief       Send a request to the memory controller.
     * 
     * @param    req        The request to be enqueued.
     * @return   true       Successful.
     * @return   false      Failed (e.g., buffer full).
     */
    virtual bool send(Request& req) = 0;

    /**
     * @brief       Send a high-priority request to the memory controller.
     * 
     */
    virtual bool priority_send(Request& req) = 0;

    /* PIMID 1.11.98 (ruling 4 (c), row 28): this controller's cumulative row
     * hits, misses (bank closed) and conflicts (another row open) and the
     * refresh commands its refresh manager issued. Added to the totals; a
     * controller that does not keep them adds nothing. */
    virtual void pimid_ctrl_totals(uint64_t& /*hits*/, uint64_t& /*misses*/,
                                   uint64_t& /*conflicts*/, uint64_t& /*refreshes*/) const {}

    /**
     * @brief       Ticks the memory controller.
     * 
     */
    virtual void tick() = 0;
   
};

}       // namespace Ramulator

#endif  // RAMULATOR_CONTROLLER_CONTROLLER_H