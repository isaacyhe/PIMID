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
    /* PIMID 1.12.18 (user ruling 2026-10-24, row policy): set by PimidSourceRowPolicy when the HOST policy is
     * closed page with auto-precharge: a host column command (source_id 1/3, lock-step shadows 4/6) is issued as
     * RDA/WRA when no other queued request hits its row (chosen at issue; PE requests: open page). */
    bool m_pimid_host_autoprecharge = false;
    /* PIMID (1.12.151): chips per rank one host request drives in lock step (the chip split; 1 = none). Set from the
     * controller's pimid_lockstep_chips; the row policy closes a host row on every chip of the rank. */
    int m_pimid_lockstep_chips = 1;
    Clk_t pimid_clk() const { return m_clk; }   // PIMID 1.12.18 (row policy): the controller clock, for a row-policy timer
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
    /* PIMID 1.12.18: activate commands this controller ISSUED (every command
     * whose meta is_opening), cumulative; refresh-forced re-opens included. */
    virtual void pimid_cmd_totals(uint64_t& /*activates*/) const {}
    /* PIMID 1.12.18 (user ruling 2026-10-24): chip ACTs of the host lock-step requests beyond chip 0 and the
     * candidate-cycles a host command waited for the DIMM CA bus; (1.12.151): the host commands that took a CA-bus
     * slot (one per rank-wide command) and the ACTs among them */
    virtual void pimid_chip_totals(uint64_t& /*shadowActs*/, uint64_t& /*caWaits*/, uint64_t& /*caCmds*/, uint64_t& /*caActs*/) const {}
    /* PIMID (1.12.169): the host data bus at bank-parallel placements -- the host column commands that took it and the
     * burst clocks it carried */
    virtual void pimid_hdb_totals(uint64_t& /*cmds*/, uint64_t& /*burstClocks*/) const {}
    /* PIMID 1.12.20: precharge power-down -- unit-cycles spent in power-down (an open interval counted to now),
     * the power-down units this controller holds, and the power-down exits (each cost tXP) */
    virtual void pimid_pd_totals(uint64_t& /*pdUnitCycles*/, uint64_t& /*units*/, uint64_t& /*exits*/) const {}
    /* PIMID 1.12.18 (ruling 17 / R0605): the row hits / misses / conflicts and
     * activates of the requests sent with source_id 1 (the host's), cumulative. */
    virtual void pimid_src_totals(uint64_t& /*hits*/, uint64_t& /*misses*/,
                                  uint64_t& /*conflicts*/, uint64_t& /*activates*/) const {}
    /* PIMID 1.12.18 (R1): the requests sent with source_id 2 or 3 (outside the ROI). */
    virtual void pimid_out_totals(uint64_t& /*hits*/, uint64_t& /*misses*/, uint64_t& /*conflicts*/,
                                  uint64_t& /*activates*/, uint64_t& /*hostActs*/) const {}

    /**
     * @brief       Ticks the memory controller.
     * 
     */
    virtual void tick() = 0;
   
};

}       // namespace Ramulator

#endif  // RAMULATOR_CONTROLLER_CONTROLLER_H