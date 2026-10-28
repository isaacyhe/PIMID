#include <string>
#include <iostream>
#include <cstdlib>
#include <stdexcept>
#include <algorithm>
#include <vector>

#include "base/base.h"
#include "dram_controller/controller.h"
#include "dram_controller/scheduler.h"
#include "dram_controller/rowpolicy.h"

namespace Ramulator {

class OpenRowPolicy : public IRowPolicy, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IRowPolicy, OpenRowPolicy, "OpenRowPolicy", "Open Row Policy.")
  private:
    
  public:
    void init() override { };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override { };

    void update(bool request_found, ReqBuffer::iterator& req_it) override { 
      // OpenRowPolicy does not need to take any actions
    };


};

class ClosedRowPolicy : public IRowPolicy, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IRowPolicy, ClosedRowPolicy, "ClosedRowPolicy", "Close Row Policy.")
  private:
    IDRAM* m_dram;
    
    int m_PRE_req_id = -1;
    
    int m_cap = -1;
    
    int m_rank_level = -1;
    int m_bankgroup_level = -1;
    int m_bank_level = -1;
    int m_row_level = -1;
    int m_num_ranks = -1;
    int m_num_bankgroups = -1;
    int m_num_banks = -1;

    int s_num_close_reqs = 0;

    std::vector<uint64_t> m_col_accesses;

  public:
    void init() override { };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      m_ctrl = cast_parent<IDRAMController>();
      m_dram = m_ctrl->m_dram;

      m_cap = param<int>("cap").default_val(10000000); // TODO

      m_rank_level = m_dram->m_levels("rank");
      m_bankgroup_level = m_dram->m_levels("bankgroup");
      m_bank_level = m_dram->m_levels("bank");
      m_row_level = m_dram->m_levels("row");

      m_PRE_req_id = m_dram->m_requests("close-row");

      m_num_ranks = m_dram->get_level_size("rank");
      m_num_bankgroups = m_dram->get_level_size("bankgroup");
      m_num_banks = m_dram->get_level_size("bank");
      
      m_col_accesses.resize(m_num_banks * m_num_bankgroups * m_num_ranks, 0);

      register_stat(s_num_close_reqs).name("num_close_reqs");
    };

    void update(bool request_found, ReqBuffer::iterator& req_it) override {

      if (!request_found)
        return;

      if (m_dram->m_command_meta(req_it->command).is_closing ||
          m_dram->m_command_meta(req_it->command).is_refreshing)  // PRE or REF 
      {  

        if (req_it->addr_vec[m_bankgroup_level] == -1 && req_it->addr_vec[m_bank_level] == -1) {  // all bank closes
          for (int b = 0; b < m_num_banks; b++) {
            for (int bg = 0; bg < m_num_bankgroups; bg++) {
              int rank_id = req_it->addr_vec[m_rank_level];
              int flat_bank_id = b + bg * m_num_banks + rank_id * m_num_banks * m_num_bankgroups;
              m_col_accesses[flat_bank_id] = 0;
            }
          }
        } else if (req_it->addr_vec[m_bankgroup_level] == -1) {  // same bank closes
          for (int bg = 0; bg < m_num_bankgroups; bg++) {
            int bank_id = req_it->addr_vec[m_bank_level];
            int rank_id = req_it->addr_vec[m_rank_level];
            int flat_bank_id = bank_id + bg * m_num_banks + rank_id * m_num_banks * m_num_bankgroups;
            m_col_accesses[flat_bank_id] = 0;
          }
        } else {  // single bank closes  (PRE, VRR, RDA, WRA)
          int flat_bank_id = req_it->addr_vec[m_bank_level] + 
                             req_it->addr_vec[m_bankgroup_level] * m_num_banks + 
                             req_it->addr_vec[m_rank_level] * m_num_banks * m_num_bankgroups;

          m_col_accesses[flat_bank_id] = 0;
        }
      } else if (m_dram->m_command_meta(req_it->command).is_accessing)  // RD or WR
      {
        int flat_bank_id = req_it->addr_vec[m_bank_level] + 
                           req_it->addr_vec[m_bankgroup_level] * m_num_banks + 
                           req_it->addr_vec[m_rank_level] * m_num_banks * m_num_bankgroups;
        
        m_col_accesses[flat_bank_id]++;

        if (m_col_accesses[flat_bank_id] >= m_cap) {
          Request req(req_it->addr_vec, m_PRE_req_id);
          m_ctrl->priority_send(req);
          m_col_accesses[flat_bank_id] = 0;
          s_num_close_reqs++;
        }
      }
    };
};


/* PIMID 1.12.18 (user ruling 2026-10-24, "DRAM row policy"; sources in docs/yaml_reference.md, memory.row_policy):
 * THE ROW POLICY PER REQUEST SOURCE.
 *   PE traffic (source_id 0 in the ROI, 2 outside): OPEN page until a conflict, every technology -- SK hynix
 *     GDDR6-AiM, Hot Chips 34 (2022) memory-controller block (rows opened by ACT4/ACT16 and held across MAC
 *     streams); Samsung HBM-PIM PIMSimulator system_hbm.ini "ROW_BUFFER_POLICY=open_page".
 *   HOST traffic (source_id 1 / 3; the lock-step shadow copies 4 / 6 follow the host):
 *     host: closed   -- closed page. Lenovo Press LP1836 p.12 (4th/5th Gen Xeon SP, DDR5): "Closed (Default)".
 *                       host_closed_mode auto_precharge (default): RD/WR become RDA/WRA (controller.h
 *                       m_pimid_host_autoprecharge); explicit_pre: a PRE after every host column access
 *                       (= ClosedRowPolicy cap 1).
 *     host: timeout  -- adaptive open page: a bank whose row a host access left open is closed after
 *                       host_idle_timeout memory clocks with no access to it. 128 = AMD BKDG Family 15h
 *                       Models 00h-0Fh (42301 Rev 3.14) D18F2x90 DynPageCloseEn=0, p.307-308: "automatically
 *                       closed after 128 clocks of inactivity" -- the only published timer value; Intel's
 *                       adaptive default (Lenovo LP1477 p.18, DDR4) publishes none.
 *     host: open     -- open page (FR-FCFS closes on a conflict).
 * A PE access to a bank disarms the host timer (the row is then the PE's, open page). */
class PimidSourceRowPolicy : public IRowPolicy, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IRowPolicy, PimidSourceRowPolicy, "PimidSourceRowPolicy", "PIMID: open page for PE traffic, the ruled host policy for host traffic.")
  private:
    IDRAM* m_dram = nullptr;
    int m_PRE_req_id = -1;
    int m_bank_level = -1;
    std::vector<int> m_radix;          // level counts 1..bank
    std::string m_host = "open", m_closed_mode = "auto_precharge";
    int m_timeout = 128;
    bool m_explicit_close = false, m_use_timeout = false;
    std::vector<uint8_t> m_armed;
    std::vector<Clk_t> m_last;
    std::vector<AddrVec_t> m_addr;
    Clk_t m_scanned = -1;
    int s_timeout_closes = 0, s_explicit_closes = 0;
    static bool host_src(int s) { return s == 1 || s == 3 || s == 4 || s == 6; }
    int flat(const AddrVec_t& a) const {
      int f = 0;
      for (int l = 1; l <= m_bank_level; l++) { const int v = a[l] < 0 ? 0 : a[l]; f = f * m_radix[l - 1] + v; }
      return f;
    }
    bool covers(const AddrVec_t& scope, const AddrVec_t& bank) const {
      for (int l = 1; l <= m_bank_level; l++) if (scope[l] != -1 && scope[l] != bank[l]) return false;
      return true;
    }
  public:
    void init() override { };
    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      m_ctrl = cast_parent<IDRAMController>();
      m_dram = m_ctrl->m_dram;
      m_host = param<std::string>("host").default_val("open");
      m_closed_mode = param<std::string>("host_closed_mode").default_val("auto_precharge");
      m_timeout = param<int>("host_idle_timeout").default_val(128);
      if (m_host != "open" && m_host != "closed" && m_host != "timeout")
        throw std::runtime_error("PimidSourceRowPolicy: host must be open, closed or timeout");
      if (m_closed_mode != "auto_precharge" && m_closed_mode != "explicit_pre")
        throw std::runtime_error("PimidSourceRowPolicy: host_closed_mode must be auto_precharge or explicit_pre");
      if (m_timeout < 1) throw std::runtime_error("PimidSourceRowPolicy: host_idle_timeout must be >= 1 memory clock");
      m_bank_level = m_dram->m_levels("bank");
      /* 1.12.18 (AA10, gate 423393 tech_DDR3): every name this plugin needs is CHECKED before it is looked up, so a
       * standard that lacks one gets a FATAL naming it (or the ruled fallback), never an uncaught
       * unordered_map::at. Auto-precharge needs RDA/WRA; without them the closed host policy falls back to an
       * explicit PRE after the access (= ClosedRowPolicy cap 1, the same ruling's stated alternative). The
       * timeout and explicit-PRE policies need the standard's close-row request (PRE). */
      if (m_host == "closed" && m_closed_mode == "auto_precharge" &&
          !(m_dram->m_commands.contains("RD") && m_dram->m_commands.contains("WR") &&
            m_dram->m_commands.contains("RDA") && m_dram->m_commands.contains("WRA"))) {
        std::cerr << "[ramulator] NOTE: PimidSourceRowPolicy: this DRAM standard defines no RD/WR/RDA/WRA command set; the closed host "
                     "page policy closes rows by an explicit PRE after each host access instead of auto-precharge" << std::endl;
        m_closed_mode = "explicit_pre";
      }
      if (m_host == "timeout" || (m_host == "closed" && m_closed_mode == "explicit_pre")) {
        if (!m_dram->m_requests.contains("close-row")) {
          std::cerr << "[ramulator] FATAL: PimidSourceRowPolicy: host policy '" << m_host << "' needs the DRAM standard's 'close-row' "
                       "request (a PRE), which this standard does not define; add it to the standard's m_requests / "
                       "m_request_translations (as DDR4 does) or use an open host policy" << std::endl;
          std::exit(2);
        }
        m_PRE_req_id = m_dram->m_requests("close-row");
      }
      int nb = 1;
      for (int l = 1; l <= m_bank_level; l++) { const int c = std::max(1, m_dram->m_organization.count[l]); m_radix.push_back(c); nb *= c; }
      if (m_host == "closed") {
        if (m_closed_mode == "auto_precharge") m_ctrl->m_pimid_host_autoprecharge = true;
        else m_explicit_close = true;
      }
      m_use_timeout = (m_host == "timeout");
      m_armed.assign(nb, 0); m_last.assign(nb, 0); m_addr.assign(nb, AddrVec_t());
      register_stat(s_timeout_closes).name("pimid_host_timeout_closes");
      register_stat(s_explicit_closes).name("pimid_host_explicit_closes");
    };
    void update(bool request_found, ReqBuffer::iterator& req_it) override {
      const Clk_t clk = m_ctrl->pimid_clk();
      if (m_use_timeout && clk != m_scanned) {   // once per memory clock
        m_scanned = clk;
        for (size_t b = 0; b < m_armed.size(); b++)
          if (m_armed[b] && clk - m_last[b] >= m_timeout) {
            Request req(m_addr[b], m_PRE_req_id);
            if (m_ctrl->priority_send(req)) { m_armed[b] = 0; s_timeout_closes++; }
          }
      }
      if (!request_found) return;
      const auto& meta = m_dram->m_command_meta(req_it->command);
      if (meta.is_closing || meta.is_refreshing) {
        if (m_use_timeout)
          for (size_t b = 0; b < m_armed.size(); b++)
            if (m_armed[b] && covers(req_it->addr_vec, m_addr[b])) m_armed[b] = 0;
        return;
      }
      if (!meta.is_accessing) return;
      const int b = flat(req_it->addr_vec);
      if (b < 0 || b >= (int)m_armed.size()) return;
      if (!host_src(req_it->source_id)) { m_armed[b] = 0; return; }   // a PE access: the row is the PE's (open page)
      /* PIMID (1.12.151): a lock-step host request (source_id 1/3 with the chip split) holds its row on every chip of the
       * rank: each chip's bank is closed (as each chip copy's was before) */
      const int ls = (req_it->source_id == 1 || req_it->source_id == 3) ? m_ctrl->m_pimid_lockstep_chips : 1;
      const int rl = (ls > 1 && m_dram->m_levels.contains("rank")) ? m_dram->m_levels("rank") : -1;
      for (int c = 0; c < (rl >= 0 ? ls : 1); c++) {
        AddrVec_t a = req_it->addr_vec;
        if (rl >= 0) a[rl] = (a[rl] / ls) * ls + c;
        const int bc = (rl >= 0) ? flat(a) : b;
        if (m_explicit_close) {
          Request req(a, m_PRE_req_id);
          m_ctrl->priority_send(req);
          s_explicit_closes++;
        } else if (m_use_timeout && bc >= 0 && bc < (int)m_armed.size()) {
          m_armed[bc] = 1; m_last[bc] = clk; m_addr[bc] = a;
        }
      }
    };
};

}       // namespace Ramulator
