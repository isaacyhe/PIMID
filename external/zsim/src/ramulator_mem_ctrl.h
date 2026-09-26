#ifndef RAMULATOR_MEM_CTRL_H_
#define RAMULATOR_MEM_CTRL_H_

#include <map>
#include <string>
#include "g_std/g_string.h"
#include "memory_hierarchy.h"
#include "pad.h"
#include "stats.h"

namespace Ramulator { class IMemorySystem; class IFrontEnd; }

class RamulatorAccEvent;

class RamulatorMemory : public MemObject {
    private:
        g_string name;
        uint32_t minLatency;
        uint32_t domain;

        Ramulator::IMemorySystem* ramulatorSys;
        Ramulator::IFrontEnd* ramulatorFE;

        std::multimap<uint64_t, RamulatorAccEvent*> inflightRequests;

        uint64_t curCycle;
        bool pgIsDevice = false;   // 1.11.18: which PG tracker this MC marks

        PAD();
        Counter profReads;
        Counter profWrites;
        Counter profTotalRdLat;
        Counter profTotalWrLat;
        /* 1.11.91 (audit R8-7): the measured bank-open fraction. Counters in
         * this controller's aggregate, which sits in the rebased "mem" group
         * (1.11.90 roiRebase), so both report the ROI window only. Registered
         * only when the Ramulator2 device keeps the sums
         * (dram/pimid_bank_open.h); absent otherwise, which the power model
         * reads as UNMEASURED. */
        Counter profBankOpenCycles;
        Counter profBankOpenWindow;
        bool bankOpenTracked = false;
        PAD();

    public:
        RamulatorMemory(const std::string& configFile,
                        uint64_t cpuFreqHz,
                        uint32_t _minLatency,
                        uint32_t _domain,
                        const g_string& _name);
        ~RamulatorMemory();

        const char* getName() { return name.c_str(); }
        void initStats(AggregateStat* parentStat);
        uint64_t access(MemReq& req);
        /* 1.11.18 (audit go-through): PG residency. This controller marked
         * NOTHING, so a machine whose memory is Ramulator-backed reported an
         * always-idle MC and took the maximum gating credit silently. The
         * flag says which tracker this instance owns (device MCs are built
         * by the SystemRouter block; the global one is the host's). */
        void setPGDevice(bool isDevice) { pgIsDevice = isDevice; }
        uint32_t tick(uint64_t cycle);
        void enqueue(RamulatorAccEvent* ev, uint64_t cycle);

    private:
        void completionCallback(RamulatorAccEvent* ev);
};

#endif  // RAMULATOR_MEM_CTRL_H_
