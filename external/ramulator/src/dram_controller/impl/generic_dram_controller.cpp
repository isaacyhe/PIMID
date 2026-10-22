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
        m_dram->issue_command(req_it->command, req_it->addr_vec);

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
        if (!m_dram->check_ready(it->command, it->addr_vec)) continue;
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

    /* issue req_it from buffer: the same steps as tick() step 4 */
    void pimid_issue(ReqBuffer::iterator req_it, ReqBuffer* buffer) {
      if (req_it->is_stat_updated == false) {
        update_request_stats(req_it);
      }
      m_dram->issue_command(req_it->command, req_it->addr_vec);
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
      std::vector<AddrVec_t> used;
      int issued = 0;
      for (int n = 0; n < m_pimid_banks_per_channel; n++) {
        ReqBuffer::iterator req_it;
        ReqBuffer* buffer = nullptr;
        bool found = false;
        // 1. requests already activating
        if (pimid_best(m_active_buffer, used, req_it)) { buffer = &m_active_buffer; found = true; }
        // 2. maintenance (refresh) keeps schedule_request's rule: when its head is
        //    not ready nothing else issues; it is served only as the cycle's first command
        if (!found && m_priority_buffer.size() != 0) {
          if (issued > 0) break;
          req_it = m_priority_buffer.begin();
          req_it->command = m_dram->get_preq_command(req_it->final_command, req_it->addr_vec);
          if (!m_dram->check_ready(req_it->command, req_it->addr_vec)) break;
          buffer = &m_priority_buffer;
          found = true;
        }
        // 3. reads and writes: no shared data bus, so both are candidates
        if (!found) {
          ReqBuffer::iterator r, w;
          const bool fr = pimid_best(m_read_buffer, used, r);
          const bool fw = pimid_best(m_write_buffer, used, w);
          if (fr && fw) {
            const bool hr = (r->command == r->final_command), hw = (w->command == w->final_command);
            if ((hr && !hw) || (hr == hw && r->arrive <= w->arrive)) { req_it = r; buffer = &m_read_buffer; }
            else { req_it = w; buffer = &m_write_buffer; }
            found = true;
          } else if (fr) { req_it = r; buffer = &m_read_buffer; found = true; }
          else if (fw) { req_it = w; buffer = &m_write_buffer; found = true; }
        }
        m_rowpolicy->update(found, req_it);
        for (auto plugin : m_plugins) plugin->update(found, req_it);
        if (!found) break;
        const bool maintenance = (buffer == &m_priority_buffer);
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

    void update_request_stats(ReqBuffer::iterator& req)
    {
      req->is_stat_updated = true;

      if (req->type_id == Request::Type::Read) 
      {
        if (is_row_hit(req)) {
          s_read_row_hits++;
          s_row_hits++;
          if (req->source_id != -1)
            s_read_row_hits_per_core[req->source_id]++;
        } else if (is_row_open(req)) {
          s_read_row_conflicts++;
          s_row_conflicts++;
          if (req->source_id != -1)
            s_read_row_conflicts_per_core[req->source_id]++;
        } else {
          s_read_row_misses++;
          s_row_misses++;
          if (req->source_id != -1)
            s_read_row_misses_per_core[req->source_id]++;
        } 
      } 
      else if (req->type_id == Request::Type::Write) 
      {
        if (is_row_hit(req)) {
          s_write_row_hits++;
          s_row_hits++;
        } else if (is_row_open(req)) {
          s_write_row_conflicts++;
          s_row_conflicts++;
        } else {
          s_write_row_misses++;
          s_row_misses++;
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
        if (m_dram->check_ready(req_it->command, req_it->addr_vec)) {
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
          
          request_found = m_dram->check_ready(req_it->command, req_it->addr_vec);
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
            request_found = m_dram->check_ready(req_it->command, req_it->addr_vec);
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
