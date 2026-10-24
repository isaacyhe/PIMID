#include <cmath>
#include "dram_controller/controller.h"
#include "memory_system/memory_system.h"

namespace Ramulator {

class GenericDRAMController final : public IDRAMController, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IDRAMController, GenericDRAMController, "Generic", "A generic DRAM controller.");
  private:
    std::deque<Request> pending;          // A queue for read requests that are about to finish (callback after RL)

    ReqBuffer m_active_buffer;            // Buffer for requests being served. This has the highest priority 
    ReqBuffer m_priority_buffer;          // Buffer for high-priority requests (e.g., maintenance like refresh).
    ReqBuffer m_read_buffer;              // Read request buffer
    ReqBuffer m_write_buffer;             // Write request buffer

    int m_bank_addr_idx = -1;

    float m_wr_low_watermark;
    float m_wr_high_watermark;
    bool  m_is_write_mode = false;

    /* PIMID 1.12.17: bank-parallel mode for processing elements placed below the
     * channel tier. Each element drives its own bank, so there is no shared
     * command bus (one command per BANK per cycle, not per channel) and no shared
     * data bus to turn around (reads and writes both served). The DRAM's
     * remaining timing constraints decide readiness, re-evaluated after every
     * command issued in the cycle, so activation limits (tRRD, tFAW) still hold
     * across banks. Off by default: the shipped behaviour is unchanged. */
    bool m_pimid_bank_parallel = false;
    int  m_pimid_banks_per_channel = 1;
    /* PIMID 1.12.18 (user ruling 2026-10-24): per-chip bank state for PE traffic at chip-level placement.
     * Each x8 chip is its own Ramulator rank; a host request is sent once per chip, copy 0 with its own source_id
     * (1 in the ROI, 3 outside) and the lockstep SHADOW copies with source_id + 3 (4 / 6): replayed, timed and
     * state-changing, never in the row-hit/miss/conflict counts; their ACTs are host chip ACTs (energy).
     * The DIMM command/address bus binds HOST commands only (PE commands are issued by each chip's in-chip
     * controller): a host copy-0 command occupies the channel's CA bus for its command cycles (the channel-level
     * ACT-following constraint of the unfiltered timing table: DDR5 two-cycle ACT/RD/WR = 2, else 1); the shadow
     * copies ride the same broadcast slot. Off unless the replay sets pimid_host_ca_bus. */
    bool m_pimid_host_ca = false;
    std::vector<int> m_pimid_ca_occ;
    long m_pimid_ca_free = 0;
    uint64_t s_pimid_shadow_acts = 0, s_pimid_ca_waits = 0;
    /* PIMID 1.12.20: PRECHARGE POWER-DOWN in the replayed controller. A power-down unit (the rank -- with the
     * per-chip split, one chip -- or the whole channel on a standard without a rank level: HBM, GDDR6) with no
     * request queued and no command for pimid_pd_idle_clocks memory clocks enters power-down (CKE low). The first
     * request or refresh that needs it wakes it, and no command may issue to it for pimid_pd_txp_clocks (tXP)
     * after the wake: the exit latency the access pays. The unit-cycles spent in power-down are counted for the
     * energy model, which credits them at IDD2P instead of the standby current (one measurement for timing and
     * energy). Off unless the replay sets pimid_power_down. */
    bool m_pimid_pd = false;
    int m_pimid_pd_idle = 64, m_pimid_pd_txp = 0, m_pimid_pd_level = 0, m_pimid_pd_txp_nck = 0;
    double m_pimid_pd_txp_ns = 0.0;
    std::vector<int> m_pimid_pd_radix;
    std::vector<uint8_t> m_pd_in, m_pd_busy;
    std::vector<Clk_t> m_pd_since, m_pd_last, m_pd_wake;
    uint64_t s_pimid_pd_cycles = 0, s_pimid_pd_exits = 0;
    int pd_unit(const AddrVec_t& a) const {
      if (m_pimid_pd_level <= 0) return 0;
      int u = 0;
      for (int l = 1; l <= m_pimid_pd_level; l++) { if (a[l] < 0) return -1; u = u * m_pimid_pd_radix[l - 1] + a[l]; }
      return u;
    }
    bool pd_ok(const AddrVec_t& a) const {
      if (!m_pimid_pd) return true;
      const int u = pd_unit(a);
      if (u >= 0) return !m_pd_in[u] && m_clk >= m_pd_wake[u];
      for (size_t i = 0; i < m_pd_in.size(); i++) if (m_pd_in[i] || m_clk < m_pd_wake[i]) return false;
      return true;
    }
    bool pimid_ready(int cmd, const AddrVec_t& a) { return pd_ok(a) && m_dram->check_ready(cmd, a); }
    void pd_mark(const AddrVec_t& a, std::vector<uint8_t>& v) {
      const int u = pd_unit(a);
      if (u >= 0) v[u] = 1; else std::fill(v.begin(), v.end(), 1);
    }
    void pd_issued(const AddrVec_t& a) {
      if (!m_pimid_pd) return;
      const int u = pd_unit(a);
      if (u >= 0) m_pd_last[u] = m_clk; else std::fill(m_pd_last.begin(), m_pd_last.end(), m_clk);
    }
    void pd_tick() {
      if (!m_pimid_pd) return;
      std::fill(m_pd_busy.begin(), m_pd_busy.end(), 0);
      for (ReqBuffer* b : {&m_read_buffer, &m_write_buffer, &m_active_buffer, &m_priority_buffer})
        for (auto it = b->begin(); it != b->end(); ++it) pd_mark(it->addr_vec, m_pd_busy);
      for (size_t u = 0; u < m_pd_in.size(); u++) {
        if (m_pd_busy[u]) {
          if (m_pd_in[u]) { m_pd_in[u] = 0; s_pimid_pd_cycles += (uint64_t)(m_clk - m_pd_since[u]); s_pimid_pd_exits++; m_pd_wake[u] = m_clk + m_pimid_pd_txp; }
          m_pd_last[u] = m_clk;
        } else if (!m_pd_in[u] && m_clk - m_pd_last[u] >= m_pimid_pd_idle) {
          m_pd_in[u] = 1; m_pd_since[u] = m_clk;
        }
      }
    }
    static int pimid_sid(int s) { return (s >= 4) ? s - 3 : s; }
    bool pimid_ca_host(const Request& r) const { return m_pimid_host_ca && (r.source_id == 1 || r.source_id == 3); }

    size_t s_row_hits = 0;
    size_t s_row_misses = 0;
    size_t s_row_conflicts = 0;
    size_t s_read_row_hits = 0;
    size_t s_read_row_misses = 0;
    size_t s_read_row_conflicts = 0;
    size_t s_write_row_hits = 0;
    size_t s_write_row_misses = 0;
    size_t s_write_row_conflicts = 0;

    size_t m_num_cores = 0;
    std::vector<size_t> s_read_row_hits_per_core;
    std::vector<size_t> s_read_row_misses_per_core;
    std::vector<size_t> s_read_row_conflicts_per_core;

    size_t s_num_read_reqs = 0;
    size_t s_num_write_reqs = 0;
    size_t s_num_other_reqs = 0;
    size_t s_queue_len = 0;
    size_t s_read_queue_len = 0;
    size_t s_write_queue_len = 0;
    size_t s_priority_queue_len = 0;
    float s_queue_len_avg = 0;
    float s_read_queue_len_avg = 0;
    float s_write_queue_len_avg = 0;
    float s_priority_queue_len_avg = 0;

    size_t s_read_latency = 0;
    float s_avg_read_latency = 0;


  public:
    void init() override {
      m_wr_low_watermark =  param<float>("wr_low_watermark").desc("Threshold for switching back to read mode.").default_val(0.2f);
      m_wr_high_watermark = param<float>("wr_high_watermark").desc("Threshold for switching to write mode.").default_val(0.8f);
      m_pimid_bank_parallel = param<bool>("pimid_bank_parallel").desc("PIMID: one command per bank per cycle, no shared bus (processing elements below the channel tier).").default_val(false);

      m_pimid_pd = param<bool>("pimid_power_down").desc("PIMID 1.12.20: precharge power-down after an idle timer, tXP exit.").default_val(false);
      m_pimid_pd_idle = param<int>("pimid_pd_idle_clocks").desc("PIMID 1.12.20: memory clocks idle before power-down entry.").default_val(64);
      m_pimid_pd_txp = param<int>("pimid_pd_txp_clocks").desc("PIMID 1.12.20: power-down exit time tXP, memory clocks (0: from the ns/nCK pair).").default_val(0);
      m_pimid_pd_txp_ns = param<double>("pimid_pd_txp_ns").desc("PIMID 1.12.20: tXP, ns part of max(nCK, ns).").default_val(0.0);
      m_pimid_pd_txp_nck = param<int>("pimid_pd_txp_nck").desc("PIMID 1.12.20: tXP, nCK part of max(nCK, ns).").default_val(0);
      m_pimid_host_ca = param<bool>("pimid_host_ca_bus").desc("PIMID 1.12.18: host commands obey the DIMM CA bus; lockstep shadow copies (source_id 4/6).").default_val(false);
      m_scheduler = create_child_ifce<IScheduler>();
      m_refresh = create_child_ifce<IRefreshManager>();    
      m_rowpolicy = create_child_ifce<IRowPolicy>();    

      if (m_config["plugins"]) {
        YAML::Node plugin_configs = m_config["plugins"];
        for (YAML::iterator it = plugin_configs.begin(); it != plugin_configs.end(); ++it) {
          m_plugins.push_back(create_child_ifce<IControllerPlugin>(*it));
        }
      }
    };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      m_dram = memory_system->get_ifce<IDRAM>();
      m_bank_addr_idx = m_dram->m_levels("bank");
      m_priority_buffer.max_size = 512*3 + 32;
      if (m_pimid_bank_parallel) {
        /* every bank has its own requester: the buffers hold one default-sized
         * queue per bank rather than one per channel */
        m_pimid_banks_per_channel = 1;
        for (int l = 1; l <= m_bank_addr_idx; l++)
          m_pimid_banks_per_channel *= std::max(1, m_dram->m_organization.count[l]);
        m_read_buffer.max_size   *= m_pimid_banks_per_channel;
        m_write_buffer.max_size  *= m_pimid_banks_per_channel;
        m_active_buffer.max_size *= m_pimid_banks_per_channel;
      }

      if (m_pimid_host_ca) {   /* PIMID 1.12.18: CA occupancy per command, from the still-unfiltered channel level */
        const int ncmd = (int)m_dram->m_commands.size();
        /* 1.12.18 (AA10): the opening command by name, checked (LPDDR5 splits it into ACT-1/ACT-2); without one every
         * command keeps the 1-cycle default CA occupancy */
        const int act = m_dram->m_commands.contains("ACT") ? m_dram->m_commands("ACT")
                      : m_dram->m_commands.contains("ACT-1") ? m_dram->m_commands("ACT-1") : -1;
        m_pimid_ca_occ.assign(ncmd, 1);
        if (!m_dram->m_timing_cons.empty())
          for (int pc = 0; pc < ncmd && pc < (int)m_dram->m_timing_cons[0].size(); pc++)
            for (const auto& t : m_dram->m_timing_cons[0][pc])
              if (t.cmd == act && t.val > m_pimid_ca_occ[pc]) m_pimid_ca_occ[pc] = t.val;
      }
      if (m_pimid_pd) {   /* PIMID 1.12.20: the power-down unit is the rank where the standard has one, else the channel */
        m_pimid_pd_level = m_dram->m_levels.contains("rank") ? m_dram->m_levels("rank") : 0;
        int nu = 1;
        for (int l = 1; l <= m_pimid_pd_level; l++) { const int c = std::max(1, m_dram->m_organization.count[l]); m_pimid_pd_radix.push_back(c); nu *= c; }
        m_pd_in.assign(nu, 0); m_pd_busy.assign(nu, 0); m_pd_since.assign(nu, 0); m_pd_last.assign(nu, 0); m_pd_wake.assign(nu, 0);
        if (m_pimid_pd_txp <= 0) {   // tXP = max(nCK, ns) at this standard's own clock
          const int tck_ps = m_dram->m_timings.contains("tCK_ps") ? m_dram->m_timing_vals("tCK_ps") : 0;
          int c = m_pimid_pd_txp_nck;
          if (tck_ps > 0 && m_pimid_pd_txp_ns > 0.0) c = std::max(c, (int)std::ceil(m_pimid_pd_txp_ns * 1000.0 / (double)tck_ps));
          m_pimid_pd_txp = std::max(1, c);
        }
      }
      m_num_cores = frontend->get_num_cores();

      s_read_row_hits_per_core.resize(m_num_cores, 0);
      s_read_row_misses_per_core.resize(m_num_cores, 0);
      s_read_row_conflicts_per_core.resize(m_num_cores, 0);

      register_stat(s_row_hits).name("row_hits_{}", m_channel_id);
      register_stat(s_row_misses).name("row_misses_{}", m_channel_id);
      register_stat(s_row_conflicts).name("row_conflicts_{}", m_channel_id);
      register_stat(s_read_row_hits).name("read_row_hits_{}", m_channel_id);
      register_stat(s_read_row_misses).name("read_row_misses_{}", m_channel_id);
      register_stat(s_read_row_conflicts).name("read_row_conflicts_{}", m_channel_id);
      register_stat(s_write_row_hits).name("write_row_hits_{}", m_channel_id);
      register_stat(s_write_row_misses).name("write_row_misses_{}", m_channel_id);
      register_stat(s_write_row_conflicts).name("write_row_conflicts_{}", m_channel_id);

      for (size_t core_id = 0; core_id < m_num_cores; core_id++) {
        register_stat(s_read_row_hits_per_core[core_id]).name("read_row_hits_core_{}", core_id);
        register_stat(s_read_row_misses_per_core[core_id]).name("read_row_misses_core_{}", core_id);
        register_stat(s_read_row_conflicts_per_core[core_id]).name("read_row_conflicts_core_{}", core_id);
      }

      register_stat(s_num_read_reqs).name("num_read_reqs_{}", m_channel_id);
      register_stat(s_num_write_reqs).name("num_write_reqs_{}", m_channel_id);
      register_stat(s_num_other_reqs).name("num_other_reqs_{}", m_channel_id);
      register_stat(s_queue_len).name("queue_len_{}", m_channel_id);
      register_stat(s_read_queue_len).name("read_queue_len_{}", m_channel_id);
      register_stat(s_write_queue_len).name("write_queue_len_{}", m_channel_id);
      register_stat(s_priority_queue_len).name("priority_queue_len_{}", m_channel_id);
      register_stat(s_queue_len_avg).name("queue_len_avg_{}", m_channel_id);
      register_stat(s_read_queue_len_avg).name("read_queue_len_avg_{}", m_channel_id);
      register_stat(s_write_queue_len_avg).name("write_queue_len_avg_{}", m_channel_id);
      register_stat(s_priority_queue_len_avg).name("priority_queue_len_avg_{}", m_channel_id);

      register_stat(s_read_latency).name("read_latency_{}", m_channel_id);
      register_stat(s_avg_read_latency).name("avg_read_latency_{}", m_channel_id);
    };

    bool send(Request& req) override {
      req.final_command = m_dram->m_request_translations(req.type_id);

      switch (req.type_id) {
        case Request::Type::Read: {
          s_num_read_reqs++;
          break;
        }
        case Request::Type::Write: {
          s_num_write_reqs++;
          break;
        }
        default: {
          s_num_other_reqs++;
          break;
        }
      }

      // Forward existing write requests to incoming read requests
      if (req.type_id == Request::Type::Read) {
        auto compare_addr = [req](const Request& wreq) {
          return wreq.addr == req.addr;
        };
        if (std::find_if(m_write_buffer.begin(), m_write_buffer.end(), compare_addr) != m_write_buffer.end()) {
          // The request will depart at the next cycle
          req.depart = m_clk + 1;
          pending.push_back(req);
          return true;
        }
      }

      // Else, enqueue them to corresponding buffer based on request type id
      bool is_success = false;
      req.arrive = m_clk;
      if        (req.type_id == Request::Type::Read) {
        is_success = m_read_buffer.enqueue(req);
      } else if (req.type_id == Request::Type::Write) {
        is_success = m_write_buffer.enqueue(req);
      } else {
        throw std::runtime_error("Invalid request type!");
      }
      if (!is_success) {
        // We could not enqueue the request
        req.arrive = -1;
        return false;
      }

      return true;
    };

    bool priority_send(Request& req) override {
      req.final_command = m_dram->m_request_translations(req.type_id);

      bool is_success = false;
      is_success = m_priority_buffer.enqueue(req);
      return is_success;
    }

    void tick() override {
      m_clk++;

      // Update statistics
      s_queue_len += m_read_buffer.size() + m_write_buffer.size() + m_priority_buffer.size() + pending.size();
      s_read_queue_len += m_read_buffer.size() + pending.size();
      s_write_queue_len += m_write_buffer.size();
      s_priority_queue_len += m_priority_buffer.size();

      // 1. Serve completed reads
      serve_completed_reads();

      m_refresh->tick();
      pd_tick();   // PIMID 1.12.19

      if (m_pimid_bank_parallel) {
        pimid_tick_bank_parallel();
        return;
      }

      // 2. Try to find a request to serve.
      ReqBuffer::iterator req_it;
      ReqBuffer* buffer = nullptr;
      bool request_found = schedule_request(req_it, buffer);

      // 2.1 Take row policy action
      m_rowpolicy->update(request_found, req_it);

      // 3. Update all plugins
      for (auto plugin : m_plugins) {
        plugin->update(request_found, req_it);
      }

      // 4. Finally, issue the commands to serve the request
      if (request_found) {
        // If we find a real request to serve
        if (req_it->is_stat_updated == false) {
          update_request_stats(req_it);
        }
        pimid_choose_autoprecharge(req_it);   // PIMID 1.12.18
        pd_issued(req_it->addr_vec);   // PIMID 1.12.19
        m_dram->issue_command(req_it->command, req_it->addr_vec);
        if (m_dram->m_command_meta(req_it->command).is_opening) { const int sid_ = pimid_sid(req_it->source_id); s_pimid_activates++; if (req_it->source_id >= 4) s_pimid_shadow_acts++; if (sid_ == 1) s_pimid_src_acts++; if (sid_ >= 2) { s_pimid_out_acts++; if (sid_ == 3) s_pimid_out_hacts++; } }   /* PIMID 1.12.18: a lockstep shadow (4/6) is a chip ACT of the host (1/3) */   /* PIMID 1.12.18 (R1): 2/3 = outside the ROI */   // PIMID 1.12.18; 1.12.18 (R0605): the host's share

        // If we are issuing the last command, set depart clock cycle and move the request to the pending queue
        if (req_it->command == req_it->final_command) {
          if (req_it->type_id == Request::Type::Read) {
            req_it->depart = m_clk + m_dram->m_read_latency;
            pending.push_back(*req_it);
          } else if (req_it->type_id == Request::Type::Write) {
            /* PIMID 1.11.96 (review H12): a write COMPLETES when its WR command issues (the data is accepted by the device
             * after nCWL + nBL, which the next command's constraints already enforce). Ramulator2 shipped this branch as a
             * TODO, so an external requester never heard of its writes: the zsim controller held every write event forever
             * and reported zero writes. The callback carries the departure clock like a read's. */
            req_it->depart = m_clk + m_dram->m_read_latency;   /* write data phase: CWL + BL, the same order as the read latency this device reports */
            if (req_it->callback) {
              req_it->callback(*req_it);
            }
          }
          buffer->remove(req_it);
        } else {
          if (m_dram->m_command_meta(req_it->command).is_opening) {
            if (m_active_buffer.enqueue(*req_it)) {
              buffer->remove(req_it);
            }
          }
        }

      }

    };


  private:
    /* PIMID 1.12.17: the bank-parallel tick (see m_pimid_bank_parallel). */
    bool pimid_same_bank(const AddrVec_t& a, const AddrVec_t& b) const {
      for (int i = 0; i <= m_bank_addr_idx; i++)
        if (a[i] != b[i] && a[i] != -1 && b[i] != -1) return false;
      return true;
    }

    /* the best READY request of `buf` whose bank has not issued this cycle:
     * row hits first, then the oldest (FR-FCFS per bank); a closing command
     * that would close a row a request in the active buffer is opening is
     * skipped, as schedule_request does */
    bool pimid_best(ReqBuffer& buf, const std::vector<AddrVec_t>& used, ReqBuffer::iterator& out) {
      ReqBuffer::iterator cand = buf.end();
      bool cand_hit = false;
      for (auto it = buf.begin(); it != buf.end(); ++it) {
        bool taken = false;
        for (const auto& k : used) if (pimid_same_bank(k, it->addr_vec)) { taken = true; break; }
        if (taken) continue;
        it->command = m_dram->get_preq_command(it->final_command, it->addr_vec);
        if (!pimid_ready(it->command, it->addr_vec)) continue;
        if (pimid_ca_host(*it) && m_clk < m_pimid_ca_free) { s_pimid_ca_waits++; continue; }   // PIMID 1.12.18: the DIMM CA bus is busy
        if (&buf != &m_active_buffer && m_dram->m_command_meta(it->command).is_closing) {
          bool blocks = false;
          for (auto a = m_active_buffer.begin(); a != m_active_buffer.end(); ++a)
            if (pimid_same_bank(a->addr_vec, it->addr_vec)) { blocks = true; break; }
          if (blocks) continue;
        }
        const bool hit = (it->command == it->final_command);
        if (cand == buf.end() || (hit && !cand_hit) || (hit == cand_hit && it->arrive < cand->arrive)) {
          cand = it;
          cand_hit = hit;
        }
      }
      if (cand == buf.end()) return false;
      out = cand;
      return true;
    }

    /* PIMID 1.12.18 (user ruling 2026-10-24 host closed page): the auto-precharge is chosen AT ISSUE.
     * A host (source 1/3, lock-step shadows 4/6) column command becomes RDA/WRA only when no other queued request
     * hits the same row of the same bank -- the close-when-no-pending-hit behaviour of a closed-page controller
     * (Synopsys uMCTL2 SCHED.PAGECLOSE, reset 1: "bank is kept open only while there are page hit transactions
     * available in the CAM to that bank"). An earlier version translated every host RD/WR to
     * RDA/WRA at send, which re-opened the row for each of a sequential stream's queued hits. */
    bool pimid_pending_hit(ReqBuffer& buf, const ReqBuffer::iterator& self) {
      const int row_lv = m_dram->m_levels("row");
      for (auto it = buf.begin(); it != buf.end(); ++it) {
        if (&(*it) == &(*self)) continue;
        bool same = true;
        for (int l = 0; l <= row_lv && same; l++) if (it->addr_vec[l] != self->addr_vec[l]) same = false;
        if (same) return true;
      }
      return false;
    }
    void pimid_choose_autoprecharge(ReqBuffer::iterator& req_it) {
      if (!m_pimid_host_autoprecharge) return;
      const int s = req_it->source_id;
      if (!(s == 1 || s == 3 || s == 4 || s == 6)) return;
      if (req_it->command != req_it->final_command) return;   // not the column command yet
      if (!(m_dram->m_commands.contains("RD") && m_dram->m_commands.contains("WR") && m_dram->m_commands.contains("RDA") &&
            m_dram->m_commands.contains("WRA"))) return;   // 1.12.18 (AA10): the plugin never sets the flag then; belt and braces
      const int rd = m_dram->m_commands("RD"), wr = m_dram->m_commands("WR");
      if (req_it->command != rd && req_it->command != wr) return;
      if (pimid_pending_hit(m_read_buffer, req_it) || pimid_pending_hit(m_write_buffer, req_it) ||
          pimid_pending_hit(m_active_buffer, req_it)) return;   // a queued hit keeps the row open
      const int ap = m_dram->m_commands(req_it->command == rd ? "RDA" : "WRA");
      if (!pimid_ready(ap, req_it->addr_vec)) return;   // not ready as RDA/WRA this cycle: plain RD/WR (the row stays open)
      req_it->command = ap;
      req_it->final_command = ap;
    }
    /* issue req_it from buffer: the same steps as tick() step 4 */
    void pimid_issue(ReqBuffer::iterator req_it, ReqBuffer* buffer) {
      if (req_it->is_stat_updated == false) {
        update_request_stats(req_it);
      }
      pimid_choose_autoprecharge(req_it);   // PIMID 1.12.18
      pd_issued(req_it->addr_vec);   // PIMID 1.12.19
      m_dram->issue_command(req_it->command, req_it->addr_vec);
      if (pimid_ca_host(*req_it) && req_it->command >= 0 && req_it->command < (int)m_pimid_ca_occ.size())   // PIMID 1.12.18
        m_pimid_ca_free = m_clk + m_pimid_ca_occ[req_it->command];
      if (m_dram->m_command_meta(req_it->command).is_opening) { const int sid_ = pimid_sid(req_it->source_id); s_pimid_activates++; if (req_it->source_id >= 4) s_pimid_shadow_acts++; if (sid_ == 1) s_pimid_src_acts++; if (sid_ >= 2) { s_pimid_out_acts++; if (sid_ == 3) s_pimid_out_hacts++; } }   /* PIMID 1.12.18: a lockstep shadow (4/6) is a chip ACT of the host (1/3) */   /* PIMID 1.12.18 (R1): 2/3 = outside the ROI */   // PIMID 1.12.18; 1.12.18 (R0605): the host's share
      if (req_it->command == req_it->final_command) {
        if (req_it->type_id == Request::Type::Read) {
          req_it->depart = m_clk + m_dram->m_read_latency;
          pending.push_back(*req_it);
        } else if (req_it->type_id == Request::Type::Write) {
          req_it->depart = m_clk + m_dram->m_read_latency;   /* as tick(): 1.11.96 H12 */
          if (req_it->callback) {
            req_it->callback(*req_it);
          }
        }
        buffer->remove(req_it);
      } else {
        if (m_dram->m_command_meta(req_it->command).is_opening) {
          if (m_active_buffer.enqueue(*req_it)) {
            buffer->remove(req_it);
          }
        }
      }
    }

    void pimid_tick_bank_parallel() {
      /* PIMID 1.12.18 (user ruling 2026-10-24, write drain): the standard
       * watermark write-drain mode, Ramulator's own set_write_mode: write mode
       * is entered when the write queue holds more than wr_high_watermark x
       * its size or no read is pending, and left when it holds less than
       * wr_low_watermark x its size while a read is pending. PIMID emits the
       * published ratio, a high/low water mark of 32/16 on a 48-entry write
       * queue per channel (Chatterjee et al., "Staged Reads", HPCA 2012,
       * Table 1 p.2 and p.6): 2/3 and 1/3 of the queue in use. In
       * bank-parallel mode the queue in use is the per-bank-scaled buffer
       * (32 x banks per channel, setup()), so the marks are fractions of that.
       * Every source counts (device, host, lock-step shadows 4/6; outside-ROI
       * 2/3): they all occupy the queue. Bank-parallel mode bypassed this. */
      set_write_mode();
      std::vector<AddrVec_t> used;
      int issued = 0;
      /* PIMID 1.12.18: a BANK-scoped priority request -- the row
       * policy's close-row PRE (ClosedRowPolicy, cap 4, on DDR4/DDR5) -- is
       * that bank's own command. The 1.12.17 port applied the single-requester
       * rule to every priority request (a head that is not ready stops the
       * whole channel; it issues only as the cycle's first command), which is
       * right for rank/channel maintenance (REFab, PREA) but made each per-bank
       * PRE -- not ready until tWR after the bank's last write -- stall every
       * other bank: the co-sim DDR5 MPI flush (61k writes over ~16 banks)
       * drained at ~50 DRAM cycles per write, one bank at a time. Now a pending
       * bank-scoped PRE blocks only its own bank until it issues; a
       * rank/channel-scoped head keeps the old rule. */
      const bool bankScopedHead = m_priority_buffer.size() != 0 &&
                                  m_priority_buffer.begin()->addr_vec[m_bank_addr_idx] != -1;
      for (int n = 0; n < m_pimid_banks_per_channel; n++) {
        ReqBuffer::iterator req_it;
        ReqBuffer* buffer = nullptr;
        bool found = false;
        std::vector<AddrVec_t> blocked = used;   // used banks + banks with a pending, not-ready PRE
        if (bankScopedHead) {
          // 0. bank-scoped priority requests, in order, up to the first rank/channel-scoped one
          for (auto it = m_priority_buffer.begin(); it != m_priority_buffer.end(); ++it) {
            if (it->addr_vec[m_bank_addr_idx] == -1) break;
            bool taken = false;
            for (const auto& k : blocked) if (pimid_same_bank(k, it->addr_vec)) { taken = true; break; }
            if (taken) continue;
            it->command = m_dram->get_preq_command(it->final_command, it->addr_vec);
            if (pimid_ready(it->command, it->addr_vec)) { req_it = it; buffer = &m_priority_buffer; found = true; break; }
            blocked.push_back(it->addr_vec);   // its bank waits for it
          }
        }
        // 1. requests already activating
        if (!found && pimid_best(m_active_buffer, blocked, req_it)) { buffer = &m_active_buffer; found = true; }
        // 2. maintenance (refresh) keeps schedule_request's rule: when its head is
        //    not ready nothing else issues; it is served only as the cycle's first command
        if (!found && !bankScopedHead && m_priority_buffer.size() != 0) {
          if (issued > 0) break;
          req_it = m_priority_buffer.begin();
          req_it->command = m_dram->get_preq_command(req_it->final_command, req_it->addr_vec);
          if (!pimid_ready(req_it->command, req_it->addr_vec)) break;
          buffer = &m_priority_buffer;
          found = true;
        }
        // 3. reads and writes: no shared data bus, so both are candidates
        if (!found) {
          ReqBuffer::iterator r, w;
          const bool fr = pimid_best(m_read_buffer, blocked, r);
          const bool fw = pimid_best(m_write_buffer, blocked, w);
          if (fr && fw) {
            /* PIMID 1.12.18 (write drain): reads beat writes outside write
             * mode, writes win inside it; each is row-hit first, then oldest
             * (pimid_best). A lone candidate always issues (below). */
            if (!m_is_write_mode) { req_it = r; buffer = &m_read_buffer; }
            else { req_it = w; buffer = &m_write_buffer; }
            found = true;
          } else if (fr) { req_it = r; buffer = &m_read_buffer; found = true; }
          else if (fw) { req_it = w; buffer = &m_write_buffer; found = true; }
        }
        m_rowpolicy->update(found, req_it);
        for (auto plugin : m_plugins) plugin->update(found, req_it);
        if (!found) break;
        const bool maintenance = (buffer == &m_priority_buffer) && !bankScopedHead;
        AddrVec_t key = req_it->addr_vec;
        pimid_issue(req_it, buffer);
        used.push_back(key);
        issued++;
        if (maintenance) break;   // a maintenance command may span every bank of its scope
      }
    }

    /**
     * @brief    Helper function to check if a request is hitting an open row
     * @details
     * 
     */
    bool is_row_hit(ReqBuffer::iterator& req)
    {
        return m_dram->check_rowbuffer_hit(req->final_command, req->addr_vec);
    }
    /**
     * @brief    Helper function to check if a request is opening a row
     * @details
     * 
    */
    bool is_row_open(ReqBuffer::iterator& req)
    {
        return m_dram->check_node_open(req->final_command, req->addr_vec);
    }

    /**
     * @brief    
     * @details
     * 
     */
    void pimid_ctrl_totals(uint64_t& hits, uint64_t& misses, uint64_t& conflicts, uint64_t& refreshes) const override {   // PIMID 1.11.98
      hits += s_row_hits; misses += s_row_misses; conflicts += s_row_conflicts;
      if (m_refresh) refreshes += m_refresh->pimid_num_refresh();
    }
    uint64_t s_pimid_activates = 0;   // PIMID 1.12.18
    void pimid_cmd_totals(uint64_t& activates) const override { activates += s_pimid_activates; }
    void pimid_pd_totals(uint64_t& pdUnitCycles, uint64_t& units, uint64_t& exits) const override {   // PIMID 1.12.19
      if (!m_pimid_pd) return;
      uint64_t c = s_pimid_pd_cycles;
      for (size_t u = 0; u < m_pd_in.size(); u++) if (m_pd_in[u]) c += (uint64_t)(m_clk - m_pd_since[u]);
      pdUnitCycles += c; units += m_pd_in.size(); exits += s_pimid_pd_exits;
    }
    void pimid_chip_totals(uint64_t& shadowActs, uint64_t& caWaits) const override { shadowActs += s_pimid_shadow_acts; caWaits += s_pimid_ca_waits; }   // PIMID 1.12.18
    /* PIMID 1.12.18 (ruling 17 / R0605): the host's share (source_id 1). */
    uint64_t s_pimid_src_hits = 0, s_pimid_src_misses = 0, s_pimid_src_conflicts = 0, s_pimid_src_acts = 0;
    void pimid_src_totals(uint64_t& hits, uint64_t& misses, uint64_t& conflicts, uint64_t& activates) const override {
      hits += s_pimid_src_hits; misses += s_pimid_src_misses; conflicts += s_pimid_src_conflicts; activates += s_pimid_src_acts;
    }
    /* PIMID 1.12.18 (user ruling R1 2026-10-24): requests sent with source_id
     * 2 (device) or 3 (host) were stamped after the ROI end: replayed, not
     * priced. Their row hits / misses / conflicts and activates (hostActs: the
     * source_id 3 share), cumulative. */
    uint64_t s_pimid_out_hits = 0, s_pimid_out_misses = 0, s_pimid_out_conflicts = 0, s_pimid_out_acts = 0, s_pimid_out_hacts = 0;
    void pimid_out_totals(uint64_t& hits, uint64_t& misses, uint64_t& conflicts, uint64_t& activates, uint64_t& hostActs) const override {
      hits += s_pimid_out_hits; misses += s_pimid_out_misses; conflicts += s_pimid_out_conflicts; activates += s_pimid_out_acts; hostActs += s_pimid_out_hacts;
    }

    void update_request_stats(ReqBuffer::iterator& req)
    {
      req->is_stat_updated = true;
      if (req->source_id >= 4) return;   // PIMID 1.12.18: a lockstep shadow copy is not a request of its own

      if (req->type_id == Request::Type::Read) 
      {
        /* PIMID 1.12.18 (R0605): source_id 1 is the host; the per-core
         * vectors hold get_num_cores() entries (1 for the GEM5 frontend), so
         * an id past them is not indexed. */
        const bool pimid_core = (req->source_id >= 0 && (size_t)req->source_id < s_read_row_hits_per_core.size());
        if (is_row_hit(req)) {
          s_read_row_hits++;
          s_row_hits++;
          if (req->source_id == 1) s_pimid_src_hits++; if (req->source_id >= 2) s_pimid_out_hits++;
          if (pimid_core)
            s_read_row_hits_per_core[req->source_id]++;
        } else if (is_row_open(req)) {
          s_read_row_conflicts++;
          s_row_conflicts++;
          if (req->source_id == 1) s_pimid_src_conflicts++; if (req->source_id >= 2) s_pimid_out_conflicts++;
          if (pimid_core)
            s_read_row_conflicts_per_core[req->source_id]++;
        } else {
          s_read_row_misses++;
          s_row_misses++;
          if (req->source_id == 1) s_pimid_src_misses++; if (req->source_id >= 2) s_pimid_out_misses++;
          if (pimid_core)
            s_read_row_misses_per_core[req->source_id]++;
        } 
      } 
      else if (req->type_id == Request::Type::Write) 
      {
        if (is_row_hit(req)) {
          s_write_row_hits++;
          s_row_hits++;
          if (req->source_id == 1) s_pimid_src_hits++; if (req->source_id >= 2) s_pimid_out_hits++;   // PIMID 1.12.18 (R0605)
        } else if (is_row_open(req)) {
          s_write_row_conflicts++;
          s_row_conflicts++;
          if (req->source_id == 1) s_pimid_src_conflicts++; if (req->source_id >= 2) s_pimid_out_conflicts++;
        } else {
          s_write_row_misses++;
          s_row_misses++;
          if (req->source_id == 1) s_pimid_src_misses++; if (req->source_id >= 2) s_pimid_out_misses++;
        }
      }
    }

    /**
     * @brief    Helper function to serve the completed read requests
     * @details
     * This function is called at the beginning of the tick() function.
     * It checks the pending queue to see if the top request has received data from DRAM.
     * If so, it finishes this request by calling its callback and poping it from the pending queue.
     */
    void serve_completed_reads() {
      if (pending.size()) {
        // Check the first pending request
        auto& req = pending[0];
        if (req.depart <= m_clk) {
          // Request received data from dram
          if (req.depart - req.arrive > 1) {
            // Check if this requests accesses the DRAM or is being forwarded.
            // TODO add the stats back
            s_read_latency += req.depart - req.arrive;
          }

          if (req.callback) {
            // If the request comes from outside (e.g., processor), call its callback
            req.callback(req);
          }
          // Finally, remove this request from the pending queue
          pending.pop_front();
        }
      };
    };


    /**
     * @brief    Checks if we need to switch to write mode
     * 
     */
    void set_write_mode() {
      if (!m_is_write_mode) {
        if ((m_write_buffer.size() > m_wr_high_watermark * m_write_buffer.max_size) || m_read_buffer.size() == 0) {
          m_is_write_mode = true;
        }
      } else {
        if ((m_write_buffer.size() < m_wr_low_watermark * m_write_buffer.max_size) && m_read_buffer.size() != 0) {
          m_is_write_mode = false;
        }
      }
    };


    /**
     * @brief    Helper function to find a request to schedule from the buffers.
     * 
     */
    bool schedule_request(ReqBuffer::iterator& req_it, ReqBuffer*& req_buffer) {
      bool request_found = false;
      // 2.1    First, check the act buffer to serve requests that are already activating (avoid useless ACTs)
      if (req_it= m_scheduler->get_best_request(m_active_buffer); req_it != m_active_buffer.end()) {
        if (pimid_ready(req_it->command, req_it->addr_vec)) {
          request_found = true;
          req_buffer = &m_active_buffer;
        }
      }

      // 2.2    If no requests can be scheduled from the act buffer, check the rest of the buffers
      if (!request_found) {
        // 2.2.1    We first check the priority buffer to prioritize e.g., maintenance requests
        if (m_priority_buffer.size() != 0) {
          req_buffer = &m_priority_buffer;
          req_it = m_priority_buffer.begin();
          req_it->command = m_dram->get_preq_command(req_it->final_command, req_it->addr_vec);
          
          request_found = pimid_ready(req_it->command, req_it->addr_vec);
          if (!request_found & m_priority_buffer.size() != 0) {
            return false;
          }
        }

        // 2.2.1    If no request to be scheduled in the priority buffer, check the read and write buffers.
        if (!request_found) {
          // Query the write policy to decide which buffer to serve
          set_write_mode();
          auto& buffer = m_is_write_mode ? m_write_buffer : m_read_buffer;
          if (req_it = m_scheduler->get_best_request(buffer); req_it != buffer.end()) {
            request_found = pimid_ready(req_it->command, req_it->addr_vec);
            req_buffer = &buffer;
          }
        }
      }

      // 2.3 If we find a request to schedule, we need to check if it will close an opened row in the active buffer.
      if (request_found) {
        if (m_dram->m_command_meta(req_it->command).is_closing) {
          auto& rowgroup = req_it->addr_vec;
          for (auto _it = m_active_buffer.begin(); _it != m_active_buffer.end(); _it++) {
            auto& _it_rowgroup = _it->addr_vec;
            bool is_matching = true;
            for (int i = 0; i < m_bank_addr_idx + 1 ; i++) {
              if (_it_rowgroup[i] != rowgroup[i] && _it_rowgroup[i] != -1 && rowgroup[i] != -1) {
                is_matching = false;
                break;
              }
            }
            if (is_matching) {
              request_found = false;
              break;
            }
          }
        }
      }

      return request_found;
    }

    void finalize() override {
      s_avg_read_latency = (float) s_read_latency / (float) s_num_read_reqs;

      s_queue_len_avg = (float) s_queue_len / (float) m_clk;
      s_read_queue_len_avg = (float) s_read_queue_len / (float) m_clk;
      s_write_queue_len_avg = (float) s_write_queue_len / (float) m_clk;
      s_priority_queue_len_avg = (float) s_priority_queue_len / (float) m_clk;

      return;
    }

};
  
}   // namespace Ramulator
