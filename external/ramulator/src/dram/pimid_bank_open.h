#ifndef RAMULATOR_DRAM_PIMID_BANK_OPEN_H
#define RAMULATOR_DRAM_PIMID_BANK_OPEN_H
// ---------------------------------------------------------------------------
// PIMID 1.11.91 (audit R8-7, user ruling (b)): THE MEASURED BANK-OPEN
// FRACTION.
//
// Micron TN-41-01 p.5 prices DRAM background in two states, "precharged (all
// of the banks are precharged) or active (one or more banks are open)", with
// BNK_PRE% = the percentage of time all banks are precharged. PIMID had no
// measurement of that and used "memory-controller traffic in the 10k-cycle
// phase" in its place. This tracker measures it from the device state
// Ramulator2 already keeps: every bank-level node's m_state.
//
// UNIT. The state is decided per IDD-bearing group of devices that share
// bank state: a RANK where the organisation has a "rank" level (the DDR
// classes, LPDDR5 -- every device of a rank receives the same commands), a
// CHANNEL otherwise (HBM2/HBM3: the per-channel IDD basis, JESD238B.01
// cl.9.1; GDDR6). A unit is ACTIVE in a cycle when >= 1 of its banks is
// "Opened" (or LPDDR5's "Pre-Opened", the first half of its two-step
// activate: a bank in it is not precharged).
//
// COST. The per-unit answer is recomputed only when the device state can
// change -- after an issued command and after a future action (refresh
// completion) -- for the unit(s) that command addresses; the per-tick work
// is one add of the cached open-unit count. The per-tick sums are what the
// counters export:
//   unit_cycles       sum over ticks of the number of units
//   open_unit_cycles  sum over ticks of the number of units with >= 1 bank
//                     open
// so open_unit_cycles / unit_cycles is BNK_ACT% averaged over the units.
// With one rank on one channel (every corpus configuration) the window is
// simply the number of memory-system ticks.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include "base/type.h"
#include "dram/spec.h"

namespace Ramulator {

template<class NodeT>
class PimidBankOpenTracker {
  public:
    bool ok = false;

    void init(const std::vector<NodeT*>& channels, const SpecDef& levels,
              const SpecDef& states) {
      ok = false;
      const bool dbg = std::getenv("PIMID_DEBUG_BANKOPEN") != nullptr;
      if (channels.empty() || !levels.contains("bank") || !levels.contains("channel")) {
        if (dbg) std::fprintf(stderr, "[bankopen] init: channels=%zu bank=%d channel=%d -> not ok\n",
                              channels.size(), (int)levels.contains("bank"), (int)levels.contains("channel"));
        return;
      }
      m_bank_level = levels("bank");
      m_unit_level = levels.contains("rank") ? levels("rank") : levels("channel");
      m_st_open    = states.contains("Opened")     ? states("Opened")     : -1;
      m_st_preopen = states.contains("Pre-Opened") ? states("Pre-Opened") : -1;
      if (m_st_open < 0) { if (dbg) std::fprintf(stderr, "[bankopen] init: no Opened state -> not ok\n"); return; }
      m_units.clear();
      m_banks.clear();
      for (NodeT* ch : channels) collectUnits(ch);
      m_units_per_channel = m_units.size() / channels.size();
      if (m_units.empty() || m_units_per_channel == 0) {
        if (dbg) std::fprintf(stderr, "[bankopen] init: units=%zu per channel=%zu -> not ok\n", m_units.size(), m_units_per_channel);
        return;
      }
      m_open.assign(m_units.size(), 0);
      m_n_open = 0;
      ok = true;
      if (dbg) std::fprintf(stderr, "[bankopen] init: ok, units=%zu banks=%zu unit_level=%d\n", m_units.size(), m_banks.size(), m_unit_level);
    }

    /* After a command or future action at addr_vec: recompute the unit(s)
     * it can have changed. addr_vec[0] is the channel; a -1 at the unit
     * level (a channel-scope command) recomputes every unit of the channel. */
    void touch(const AddrVec_t& addr_vec) {
      if (!ok) return;
      const int ch = addr_vec.empty() ? -1 : addr_vec[0];
      if (ch < 0 || static_cast<size_t>(ch) * m_units_per_channel >= m_units.size()) {
        for (size_t u = 0; u < m_units.size(); u++) recompute(u);
        return;
      }
      const size_t base = static_cast<size_t>(ch) * m_units_per_channel;
      int sub = -1;
      if (m_unit_level > 0 && static_cast<size_t>(m_unit_level) < addr_vec.size())
        sub = addr_vec[m_unit_level];
      if (m_units_per_channel == 1) { recompute(base); return; }
      if (sub < 0 || static_cast<size_t>(sub) >= m_units_per_channel) {
        for (size_t u = base; u < base + m_units_per_channel; u++) recompute(u);
      } else {
        recompute(base + sub);
      }
    }

    void tick(uint64_t& unit_cycles, uint64_t& open_unit_cycles) const {
      if (!ok) return;
      unit_cycles += m_units.size();
      open_unit_cycles += m_n_open;
    }

  private:
    int m_bank_level = -1, m_unit_level = -1;
    int m_st_open = -1, m_st_preopen = -1;
    size_t m_units_per_channel = 0;
    std::vector<NodeT*> m_units;
    std::vector<std::vector<NodeT*>> m_banks;   // the bank nodes under each unit
    std::vector<uint8_t> m_open;
    uint64_t m_n_open = 0;

    void collectUnits(NodeT* n) {
      if (n->m_level == m_unit_level) {
        m_units.push_back(n);
        m_banks.emplace_back();
        collectBanks(n, m_banks.back());
        return;
      }
      for (NodeT* c : n->m_child_nodes) collectUnits(c);
    }
    void collectBanks(NodeT* n, std::vector<NodeT*>& out) {
      if (n->m_level == m_bank_level) { out.push_back(n); return; }
      for (NodeT* c : n->m_child_nodes) collectBanks(c, out);
    }
    void recompute(size_t u) {
      uint8_t now = 0;
      for (NodeT* b : m_banks[u]) {
        if (b->m_state == m_st_open || (m_st_preopen >= 0 && b->m_state == m_st_preopen)) {
          now = 1; break;
        }
      }
      if (now != m_open[u]) {
        if (now) m_n_open++; else m_n_open--;
        m_open[u] = now;
      }
    }
};

}   // namespace Ramulator
#endif
