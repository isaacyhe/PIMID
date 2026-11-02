#include <vector>
#include <cstdlib>
#include <string>

#include "base/base.h"
#include "dram_controller/controller.h"
#include "dram_controller/scheduler.h"

namespace Ramulator {

class FRFCFS : public IScheduler, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IScheduler, FRFCFS, "FRFCFS", "FRFCFS DRAM Scheduler.")
  private:
    IDRAM* m_dram;

  public:
    void init() override { };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      m_dram = cast_parent<IDRAMController>()->m_dram;
    };

    ReqBuffer::iterator compare(ReqBuffer::iterator req1, ReqBuffer::iterator req2) override {
      bool ready1 = m_dram->check_ready(req1->command, req1->addr_vec);
      bool ready2 = m_dram->check_ready(req2->command, req2->addr_vec);

      if (ready1 ^ ready2) {
        if (ready1) {
          return req1;
        } else {
          return req2;
        }
      }

      // Fallback to FCFS
      if (req1->arrive <= req2->arrive) {
        return req1;
      } else {
        return req2;
      } 
    }

    /* PIMID (1.12.229): host speed. The pick is the fold of compare() over the queue: a ready request before one that is
     * not, then the earlier arrival, then queue order. It is taken in one pass that evaluates each request's readiness
     * once (the fold asked it of the running candidate again at every step). Where the standard counts its channels'
     * node updates (IDRAM::pimid_chan_ver, given with pimid_ready_at), a request keeps its prerequisite command and the
     * clock its readiness waits for -- check_ready passes exactly when the clock reaches pimid_ready_at -- while the
     * count of its channel and its final command are unchanged: neither can change without a node update. The
     * commands written to the queue and the pick are the same. With PIMID_DEBUG_CHECKS=1 every pick is compared with
     * the fold. */
    int m_pimid_mode = -1;   // -1 not set up, 0 = no update count (readiness asked of the DRAM), 1 = kept per request
    int m_pimid_ch_level = 0;
    std::vector<const uint64_t*> m_pimid_vers;
    bool m_pimid_check = false;

    void pimid_setup() {
      m_pimid_mode = 0;
      const char* e = std::getenv("PIMID_DEBUG_CHECKS");
      m_pimid_check = (e && std::string(e) == "1");
      if (!m_dram->m_levels.contains("channel")) return;
      m_pimid_ch_level = m_dram->m_levels("channel");
      const int n = m_dram->get_level_size("channel");
      std::vector<const uint64_t*> v;
      for (int c = 0; c < n; c++) {
        const uint64_t* p = m_dram->pimid_chan_ver(c);
        if (!p) return;
        v.push_back(p);
      }
      if (v.empty()) return;
      m_pimid_vers = v;
      m_pimid_mode = 1;
    }

    ReqBuffer::iterator pimid_fold(ReqBuffer& buffer, Clk_t clk) {   // the pick as compare() folds it (debug check)
      for (auto& req : buffer) {
        if (req.command != m_dram->get_preq_command(req.final_command, req.addr_vec)) {
          spdlog::error("[FRFCFS] PIMID_DEBUG_CHECKS: a kept prerequisite command differs from the DRAM's");
          std::exit(-1);
        }
        if (req.pimid_pv != ~0ull && req.pimid_pcmd == req.command &&
            (clk >= req.pimid_pra) != m_dram->check_ready(req.command, req.addr_vec)) {
          spdlog::error("[FRFCFS] PIMID_DEBUG_CHECKS: a kept ready clock disagrees with check_ready");
          std::exit(-1);
        }
      }
      auto candidate = buffer.begin();
      for (auto next = std::next(buffer.begin(), 1); next != buffer.end(); next++) {
        candidate = compare(candidate, next);
      }
      return candidate;
    }

    ReqBuffer::iterator get_best_request(ReqBuffer& buffer) override {
      if (buffer.size() == 0) {
        return buffer.end();
      }
      if (m_pimid_mode < 0) pimid_setup();

      const Clk_t clk = m_dram->pimid_dram_clk();
      auto best = buffer.end();
      bool best_ready = false;
      for (auto it = buffer.begin(); it != buffer.end(); ++it) {
        Request& req = *it;
        bool ready;
        const int ch = (m_pimid_mode == 1) ? req.addr_vec[m_pimid_ch_level] : -1;
        if (ch >= 0 && ch < (int)m_pimid_vers.size()) {
          const uint64_t v = *m_pimid_vers[ch];
          if (req.pimid_pv != v || req.pimid_pfc != req.final_command) {
            req.pimid_pcmd = m_dram->get_preq_command(req.final_command, req.addr_vec);
            req.pimid_pra = m_dram->pimid_ready_at(req.pimid_pcmd, req.addr_vec);
            req.pimid_pv = v;
            req.pimid_pfc = req.final_command;
          }
          req.command = req.pimid_pcmd;
          ready = (clk >= req.pimid_pra);
        } else {
          req.command = m_dram->get_preq_command(req.final_command, req.addr_vec);
          ready = m_dram->check_ready(req.command, req.addr_vec);
        }
        if (best == buffer.end() || (ready && !best_ready) || (ready == best_ready && req.arrive < best->arrive)) {
          best = it;
          best_ready = ready;
        }
      }
      if (m_pimid_check && pimid_fold(buffer, clk) != best) {
        spdlog::error("[FRFCFS] PIMID_DEBUG_CHECKS: the one-pass pick differs from the fold of compare()");
        std::exit(-1);
      }
      return best;
    }
};

}       // namespace Ramulator
