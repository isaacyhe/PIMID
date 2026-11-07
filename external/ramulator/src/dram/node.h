#ifndef RAMULATOR_DRAM_NODE_H
#define RAMULATOR_DRAM_NODE_H

#include <vector>
#include <map>
#include <deque>
#include <functional>
#include <concepts>

#include "base/type.h"
#include "dram/spec.h"

namespace Ramulator {

class IDRAM;

template<typename T>
concept IsDRAMSpec = requires(T t) { 
  typename T::Node; 
};

// CRTP class defnition is not complete, so we cannot have something nice like:
// template<typename T>
// concept IsDRAMSpec = std::is_base_of_v<IDRAM, T> && requires(T t) { 
//   typename T::Node; 
// };


/**
 * @brief     CRTP-ish (?) base class of a DRAM Device Node
 * 
 */
template<IsDRAMSpec T>
struct DRAMNodeBase {
    using NodeType = typename T::Node;
    NodeType* m_parent_node = nullptr;
    std::vector<NodeType*> m_child_nodes;

    T* m_spec = nullptr;

    int m_level = -1;      // The level of this node in the organization hierarchy
    int m_node_id = -1;    // The id of this node at this level
    int m_size = -1;       // The size of the node (e.g., how many rows in a bank)

    int m_state = -1;      // The state of the node
    /* PIMID (1.12.229): the updates of this channel's nodes, counted at the channel (the root): every state and timing
     * change of a node goes through update_states / update_timing called at the channel, so while the count is
     * unchanged no node of the channel has changed (FRFCFS keeps each request's prerequisite command and ready clock
     * with it while the count holds) */
    uint64_t m_pimid_ver = 0;

    std::vector<Clk_t> m_cmd_ready_clk;             // The next cycle that each command can be issued again at this level
    std::vector<std::deque<Clk_t>> m_cmd_history;   // Issue-history of each command at this level
    /* PIMID (1.12.273): m_cmd_ready_clk by source -- the activation window's constraints (window > 1: the four-activate
     * window, tFAW) and every other constraint; m_cmd_ready_clk is the larger of the two. Read by pimid_window_probe
     * only (the activation-window report): the timing is unchanged. */
    std::vector<Clk_t> m_pimid_win_ready;
    std::vector<Clk_t> m_pimid_oth_ready;

    using RowId_t = int;
    using RowState_t = int;
    std::map<RowId_t, RowState_t> m_row_state;  // The state of the rows, if I am a bank-ish node

    DRAMNodeBase(T* spec, NodeType* parent, int level, int id):
    m_spec(spec), m_parent_node(parent), m_level(level), m_node_id(id) {
      int num_cmds = T::m_commands.size();
      m_cmd_ready_clk.resize(num_cmds, -1);
      m_cmd_history.resize(num_cmds);
      m_pimid_win_ready.resize(num_cmds, -1);   // PIMID (1.12.273)
      m_pimid_oth_ready.resize(num_cmds, -1);
      for (int cmd = 0; cmd < num_cmds; cmd++) {
        int window = 0;
        for (const auto& t : spec->m_timing_cons[level][cmd]) {
          window = std::max(window, t.window);
        }
        if (window != 0) {
          m_cmd_history[cmd].resize(window, -1);
        } else {
          m_cmd_history[cmd].clear();
        }
      }

      m_state = spec->m_init_states[m_level];

      // Recursively construct next levels
      int next_level = level + 1;
      int last_level = T::m_levels["row"];
      if (next_level == last_level) {
        return;
      } else {
        int next_level_size = m_spec->m_organization.count[next_level];
        if (next_level_size == 0) {
          return;
        } else {
          for (int i = 0; i < next_level_size; i++) {
            NodeType* child = new NodeType(spec, static_cast<NodeType*>(this), next_level, i);
            static_cast<NodeType*>(this)->m_child_nodes.push_back(child);
          }
        }
      }
    };

    void update_states(int command, const AddrVec_t& addr_vec, Clk_t clk) {
      if (!m_parent_node) m_pimid_ver++;   // PIMID (1.12.229)
      int child_id = addr_vec[m_level+1];
      if (m_spec->m_actions[m_level][command]) {
        // update the state machine at this level
        m_spec->m_actions[m_level][command](static_cast<NodeType*>(this), command, child_id, clk); 
      }
      if (m_level == m_spec->m_command_scopes[command] || !m_child_nodes.size()) {
        // stop recursion: updated all levels
        return; 
      }
      // recursively update child nodes
      if (child_id == -1) {
        for (auto child : m_child_nodes) {
          child->update_states(command, addr_vec, clk);
        }
      } else {
        m_child_nodes[child_id]->update_states(command, addr_vec, clk);
      }
    };

    void update_powers(int command, const AddrVec_t& addr_vec, Clk_t clk) {
      if (!m_spec->m_drampower_enable)
        return;

      int child_id = addr_vec[m_level+1];
      if (m_spec->m_powers[m_level][command]) {
        // update the power model at this level
        m_spec->m_powers[m_level][command](static_cast<NodeType*>(this), command, addr_vec, clk);
      }
      if (m_level == m_spec->m_command_scopes[command] || !m_child_nodes.size()) {
        // stop recursion: updated all levels
        return; 
      }
      // recursively update child nodes
      if (child_id == -1){
        for (auto child : m_child_nodes) {
          child->update_powers(command, addr_vec, clk);
        }
      } else {
        m_child_nodes[child_id]->update_powers(command, addr_vec, clk);
      }
    };

    void update_timing(int command, const AddrVec_t& addr_vec, Clk_t clk) {
      if (!m_parent_node) m_pimid_ver++;   // PIMID (1.12.229)
      /************************************************
       *         Update Sibling Node Timing
       ***********************************************/
      if (m_node_id != addr_vec[m_level] && addr_vec[m_level] != -1) {
        for (const auto& t : m_spec->m_timing_cons[m_level][command]) {
          if (!t.sibling) {
            // not sibling timing parameter
            continue; 
          }

          // update earliest schedulable time of every command
          Clk_t future = clk + t.val;
          m_cmd_ready_clk[t.cmd] = std::max(m_cmd_ready_clk[t.cmd], future); 
          m_pimid_oth_ready[t.cmd] = std::max(m_pimid_oth_ready[t.cmd], future);   // PIMID (1.12.273)
        }
        // stop recursion
        return;
      }

      /************************************************
       *          Update Target Node Timing
       ***********************************************/
      // Update history
      if (m_cmd_history[command].size()) {
        m_cmd_history[command].pop_back();
        m_cmd_history[command].push_front(clk); 
      }

      for (const auto& t : m_spec->m_timing_cons[m_level][command]) {
        if (t.sibling) {
          continue; 
        }

        // Get the oldest history
        Clk_t past = m_cmd_history[command][t.window-1];
        if (past < 0) {
          // not enough history
          continue; 
        }

        // update earliest schedulable time of every command
        Clk_t future = past + t.val;
        m_cmd_ready_clk[t.cmd] = std::max(m_cmd_ready_clk[t.cmd], future);
        if (t.window > 1) m_pimid_win_ready[t.cmd] = std::max(m_pimid_win_ready[t.cmd], future);   // PIMID (1.12.273)
        else              m_pimid_oth_ready[t.cmd] = std::max(m_pimid_oth_ready[t.cmd], future);
      }

      if (!m_child_nodes.size()) {
        // stop recursion: updated all levels
        return; 
      }

      // recursively update all of my children
      for (auto child : m_child_nodes) {
        child->update_timing(command, addr_vec, clk);
      }
    };

    int get_preq_command(int command, const AddrVec_t& addr_vec, Clk_t m_clk) {
      int child_id = addr_vec[m_level + 1];
      if (m_spec->m_preqs[m_level][command]) {
        int preq_cmd = m_spec->m_preqs[m_level][command](static_cast<NodeType*>(this), command, addr_vec, m_clk);
        if (preq_cmd != -1) {
          // stop recursion: there is a prerequisite at this level
          return preq_cmd; 
        }
      }

      if (!m_child_nodes.size()) {
        // stop recursion: there were no prequisites at any level
        return command; 
      }

      // recursively get_preq_command at my child
      return m_child_nodes[child_id]->get_preq_command(command, addr_vec, m_clk);
    };

    /* PIMID (1.12.127): the largest ready clock (-1 = none) on the nodes check_ready(command, addr_vec, clk) visits, so
     * check_ready passes exactly when clk >= ready_at(...). Ready clocks only grow (update_timing takes the max). */
    Clk_t ready_at(int command, const AddrVec_t& addr_vec) {
      Clk_t r = m_cmd_ready_clk[command];
      int child_id = addr_vec[m_level+1];
      if (m_level == m_spec->m_command_scopes[command] || !m_child_nodes.size()) {
        return r;
      }
      if (child_id == -1) {
        for (auto child : m_child_nodes) r = std::max(r, child->ready_at(command, addr_vec));
        return r;
      }
      return std::max(r, m_child_nodes[child_id]->ready_at(command, addr_vec));
    };

    /* PIMID (1.12.273): the activation-window probe, on the nodes check_ready(command, addr_vec, clk) visits: the latest
     * ready clock from the window constraints (win) and from every other constraint (oth); whether a window
     * constraint ending at command is kept there (has); whether issuing command at clk would break a window (brk: the
     * window's oldest entry plus its latency still ahead -- check_ready forbids it, so this stays false). Reads only.
     * (1.12.286): wlv takes the deepest level on the path that keeps such a constraint: the window's unit is the node there. */
    void pimid_window_probe(int command, const AddrVec_t& addr_vec, Clk_t clk, Clk_t& win, Clk_t& oth, bool& has, bool& brk, int& wlv) {
      win = std::max(win, m_pimid_win_ready[command]);
      oth = std::max(oth, m_pimid_oth_ready[command]);
      const int ncmd = (int)m_spec->m_timing_cons[m_level].size();
      for (int p = 0; p < ncmd; p++) {
        for (const auto& t : m_spec->m_timing_cons[m_level][p]) {
          if (t.sibling || t.window <= 1 || t.cmd != command) continue;
          has = true;
          if (m_level > wlv) wlv = m_level;   // PIMID (1.12.286)
          if ((int)m_cmd_history[p].size() < t.window) continue;
          const Clk_t past = m_cmd_history[p][t.window-1];
          if (past >= 0 && clk < past + t.val) brk = true;
        }
      }
      int child_id = addr_vec[m_level+1];
      if (m_level == m_spec->m_command_scopes[command] || !m_child_nodes.size()) return;
      if (child_id == -1) {
        for (auto child : m_child_nodes) child->pimid_window_probe(command, addr_vec, clk, win, oth, has, brk, wlv);
        return;
      }
      m_child_nodes[child_id]->pimid_window_probe(command, addr_vec, clk, win, oth, has, brk, wlv);
    };

    bool check_ready(int command, const AddrVec_t& addr_vec, Clk_t clk) {
      if (m_cmd_ready_clk[command] != -1 && clk < m_cmd_ready_clk[command]) {
        // stop recursion: the check failed at this level
        return false; 
      }

      int child_id = addr_vec[m_level+1];
      if (m_level == m_spec->m_command_scopes[command] || !m_child_nodes.size()) {
        // stop recursion: the check passed at all levels
        return true; 
      }

      if (child_id == -1) {
        // if it is a same bank command, recurse all children in rank level
        bool ready = true;
        for (auto child : m_child_nodes) {
          ready = ready && child->check_ready(command, addr_vec, clk);
        }
        return ready;
      } else {
        // recursively check my child
        return m_child_nodes[child_id]->check_ready(command, addr_vec, clk);
      }
    };

    bool check_rowbuffer_hit(int command, const AddrVec_t& addr_vec, Clk_t m_clk) {
      // TODO: Optimize this by just checking the bank-levels? Have a dedicated bank structure?
      int child_id = addr_vec[m_level+1];
      if (m_spec->m_rowhits[m_level][command]) {
        // stop recursion: there is a row hit at this level
        return m_spec->m_rowhits[m_level][command](static_cast<NodeType*>(this), command, child_id, m_clk);  
      }

      if (!m_child_nodes.size()) {
        // stop recursion: there were no row hits at any level
        return false; 
      }

      // recursively check for row hits at my child
      return m_child_nodes[child_id]->check_rowbuffer_hit(command, addr_vec, m_clk);
    };    
    
    bool check_node_open(int command, const AddrVec_t& addr_vec, Clk_t m_clk) {

      int child_id = addr_vec[m_level+1];
      if (m_spec->m_rowopens[m_level][command])
        // stop recursion: there is a row open at this level
        return m_spec->m_rowopens[m_level][command](static_cast<NodeType*>(this), command, child_id, m_clk);  

      if (!m_child_nodes.size())
        // stop recursion: there were no row hits at any level
        return false; 

      // recursively check for row hits at my child
      return m_child_nodes[child_id]->check_node_open(command, addr_vec, m_clk);
    }
};

template<class T>
using ActionFunc_t = std::function<void(typename T::Node* node, int cmd, int target_id, Clk_t clk)>;
template<class T>
using PreqFunc_t   = std::function<int (typename T::Node* node, int cmd, const AddrVec_t& addr_vec, Clk_t clk)>;
template<class T>
using RowhitFunc_t = std::function<bool(typename T::Node* node, int cmd, int target_id, Clk_t clk)>;
template<class T>
using RowopenFunc_t = std::function<bool(typename T::Node* node, int cmd, int target_id, Clk_t clk)>;
template<class T>
using PowerFunc_t = std::function<void(typename T::Node* node, int cmd, const AddrVec_t& addr_vec, Clk_t clk)>;

template<typename T>
using FuncMatrix = std::vector<std::vector<T>>;

// TODO: Enable easy syntax for FuncMatrix lookup
// template<typename T, int N, int M>
// class FuncMatrix : public std::array<std::array<T, M>, N> {

// };

}        // namespace Ramulator

#endif   // RAMULATOR_DRAM_NODE_H
