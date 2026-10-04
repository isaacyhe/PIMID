#include <cstdio>
#include <cstdlib>
#include <vector>

#include "base/base.h"
#include "dram_controller/controller.h"
#include "dram_controller/refresh.h"

namespace Ramulator {

class AllBankRefresh : public IRefreshManager, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IRefreshManager, AllBankRefresh, "AllBank", "All-Bank Refresh scheme.")
  private:
    Clk_t m_clk = 0;
    IDRAM* m_dram;
    IDRAMController* m_ctrl;

    int m_dram_org_levels = -1;
    int m_num_ranks = -1;

    int m_nrefi = -1;
    int m_ref_req_id = -1;
    int m_ref_scope = -1;   // PIMID 1.11.98: level index of the REFab command's scope
    Clk_t m_next_refresh_cycle = -1;
    size_t s_num_refresh = 0;   // 1.11.98 (PIMID row 28)

  public:
    void init() override { 
      m_ctrl = cast_parent<IDRAMController>();
    };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      m_dram = m_ctrl->m_dram;

      m_dram_org_levels = m_dram->m_levels.size();
      m_nrefi = m_dram->m_timing_vals("nREFI");
      m_ref_req_id = m_dram->m_requests("all-bank-refresh");
      /* PIMID 1.11.98 (gate 1208A): send the all-bank refresh at the scope the
       * DRAM model declares for its command. Upstream looped over
       * get_level_size("rank"), which is -1 for a model with no rank level:
       * GDDR6, HBM2 and HBM3 declare REFab at CHANNEL scope and have no rank,
       * so the loop ran zero times and those parts were NEVER refreshed (no
       * tRFC blocking in any PIMID timing path that uses Ramulator: the
       * oracle, the host-side controller, the device replay). DDR3/4/5 and
       * LPDDR5 declare REFab at rank scope (level 1) and are unchanged. */
      m_ref_scope = m_dram->m_command_scopes(m_dram->m_request_translations(m_ref_req_id));
      if (m_ref_scope == 0) {
        m_num_ranks = 1;                                     // one REFab per channel
      } else if (m_ref_scope == 1) {
        m_num_ranks = m_dram->m_organization.count[1];       // one REFab per rank / pseudochannel
      } else {
        throw ConfigurationError("AllBank refresh: the all-bank refresh command's scope is level {}; only channel (0) or level 1 is supported", m_ref_scope);
      }

      m_next_refresh_cycle = m_nrefi;
      /* 1.11.98 (PIMID, sweep-94 row 28): the refresh commands this
       * controller issued, a MEASURED controller-background quantity. */
      register_stat(s_num_refresh).name("num_refresh_{}", m_ctrl->m_channel_id);
      if (std::getenv("PIMID_DEBUG_REPLAY"))   /* diagnostic (1.11.98 gate 1208A) */
        std::fprintf(stderr, "[refreshdbg] setup ch=%d nREFI=%d next=%lld per-interval=%d scope=%d req=%d\n", m_ctrl->m_channel_id, m_nrefi,
                     (long long)m_next_refresh_cycle, m_num_ranks, m_ref_scope, m_ref_req_id);
    };

    size_t pimid_num_refresh() const override { return s_num_refresh; }   // PIMID 1.11.98 (row 28)

    void tick() {
      m_clk++;
      if (m_clk == 1 && std::getenv("PIMID_DEBUG_REPLAY"))   /* diagnostic (1.11.98 gate 1208A) */
        std::fprintf(stderr, "[refreshdbg] first tick ch=%d next=%lld nREFI=%d\n", m_ctrl->m_channel_id, (long long)m_next_refresh_cycle, m_nrefi);

      if (m_clk == m_next_refresh_cycle) {
        m_next_refresh_cycle += m_nrefi;
        for (int r = 0; r < m_num_ranks; r++) {
          std::vector<int> addr_vec(m_dram_org_levels, -1);
          addr_vec[0] = m_ctrl->m_channel_id;
          if (m_ref_scope >= 1) addr_vec[1] = r;   // PIMID 1.11.98: channel-scope REFab names the channel only
          Request req(addr_vec, m_ref_req_id);

          bool is_success = m_ctrl->priority_send(req);
          if (!is_success) {
            throw std::runtime_error("Failed to send refresh!");
          }
          s_num_refresh++;   // 1.11.98 (PIMID row 28)
        }
      }
    };

};

}       // namespace Ramulator
