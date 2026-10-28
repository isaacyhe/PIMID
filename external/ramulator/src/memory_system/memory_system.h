#ifndef     RAMULATOR_MEMORYSYSTEM_MEMORY_H
#define     RAMULATOR_MEMORYSYSTEM_MEMORY_H

#include <map>
#include <vector>
#include <string>
#include <functional>

#include "base/base.h"
#include "frontend/frontend.h"

namespace Ramulator {

class IMemorySystem : public TopLevel<IMemorySystem> {
  RAMULATOR_REGISTER_INTERFACE(IMemorySystem, "MemorySystem", "Memory system interface (e.g., communicates between processor and memory controller).")

  friend class Factory;

  protected:
    IFrontEnd* m_frontend;
    uint m_clock_ratio = 1;

  public:
    virtual void connect_frontend(IFrontEnd* frontend) { 
      m_frontend = frontend; 
      m_impl->setup(frontend, this);
      for (auto component : m_components) {
        component->setup(frontend, this);
      }
    };

    virtual void finalize() { 
      for (auto component : m_components) {
        component->finalize();
      }

      YAML::Emitter emitter;
      emitter << YAML::BeginMap;
      m_impl->print_stats(emitter);
      emitter << YAML::EndMap;
      std::cout << emitter.c_str() << std::endl;
    };

    /**
     * @brief         Tries to send the request to the memory system
     * 
     * @param    req      The request
     * @return   true     Request is accepted by the memory system.
     * @return   false    Request is rejected by the memory system, maybe the memory controller is full?
     */
    virtual bool send(Request req) = 0;

    /**
     * @brief         Ticks the memory system
     * 
     */
    virtual void tick() = 0;

    /**
     * @brief    Returns 
     * 
     * @return   int 
     */
    int get_clock_ratio() { return m_clock_ratio; };

    // /**
    //  * @brief    Get the integer id of the request type from the memory spec
    //  * 
    //  */
    // virtual const SpecDef& get_supported_requests() = 0;

    virtual float get_tCK() { return -1.0f; };

    /* PIMID 1.11.91 (audit R8-7): the cumulative bank-open sums of the DRAM
     * device (dram/pimid_bank_open.h). false when the device does not keep
     * them; the caller then exports nothing. */
    virtual bool pimid_bank_open_totals(uint64_t& /*unit_cycles*/,
                                        uint64_t& /*open_unit_cycles*/) { return false; };

    /* PIMID 1.11.98 (ruling 4 (c), row 28): row hits / misses / conflicts and
     * refresh commands summed over every controller, cumulative. false when
     * the memory system does not keep them. */
    virtual bool pimid_ctrl_totals(uint64_t& /*hits*/, uint64_t& /*misses*/,
                                   uint64_t& /*conflicts*/, uint64_t& /*refreshes*/) { return false; };
    /* PIMID 1.12.18: activate commands issued, summed over every controller. */
    virtual bool pimid_cmd_totals(uint64_t& /*activates*/) { return false; };
    virtual bool pimid_chip_totals(uint64_t& /*shadowActs*/, uint64_t& /*caWaits*/, uint64_t& /*caCmds*/, uint64_t& /*caActs*/) { return false; };
    virtual bool pimid_pd_totals(uint64_t& /*pdUnitCycles*/, uint64_t& /*units*/, uint64_t& /*exits*/) { return false; };   // PIMID 1.12.20   // PIMID 1.12.18
    /* PIMID 1.12.18 (ruling 17 / R0605): the share of the row hits / misses /
     * conflicts and activates that belongs to requests sent with source_id 1
     * (the HOST's accesses on a replayed controller that also serves the
     * device), summed over every controller, cumulative. */
    virtual bool pimid_src_totals(uint64_t& /*hits*/, uint64_t& /*misses*/,
                                  uint64_t& /*conflicts*/, uint64_t& /*activates*/) { return false; };
    /* PIMID 1.12.18 (R1): the share of requests sent with source_id 2 / 3
     * (stamped after the ROI end), summed over every controller, cumulative. */
    virtual bool pimid_out_totals(uint64_t& /*hits*/, uint64_t& /*misses*/, uint64_t& /*conflicts*/,
                                  uint64_t& /*activates*/, uint64_t& /*hostActs*/) { return false; };
};

}        // namespace Ramulator


#endif   // RAMULATOR_MEMORYSYSTEM_MEMORY_H