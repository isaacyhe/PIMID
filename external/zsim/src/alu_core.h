/** $lic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 *
 * This file is part of zsim.
 *
 * zsim is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, version 2.
 *
 * If you use this software in your research, we request that you reference
 * the zsim paper ("ZSim: Fast and Accurate Microarchitectural Simulation of
 * Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June 2013) as the
 * source of the simulator in any publications that use this software, and that
 * you send us a citation of your work.
 *
 * zsim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ALU_CORE_H_
#define ALU_CORE_H_

// A minimal ALU-only core model with NO cache hierarchy.
// 5 scaling factors for design space exploration:
//   compute_factor:    cycles-per-instruction multiplier (default 1.0)
//   access_factor:     cycles per load/store (default 1.0, 0.0 = free local access)
//   throughput_factor:  effective parallelism divider (default 1.0)
//   operand_width:     operand bit-width (default 32)
//   energy_factor:     per-op energy scale for reporting (does not affect cycle simulation)
//
// When a PEMemoryInterface is wired (mi_ != nullptr), remote memory accesses
// traverse the in-memory network hierarchy for realistic NoC latency.

#include <cmath>
#include "core.h"
#include "memory_hierarchy.h"
#include "pad.h"

class PEMemoryInterface;  // forward declaration

class ALUCore : public Core {
    private:
        /* 1.11.43 (audit E23): per-core FPU capability. Was the GLOBAL
         * zinfo->hierarchy.peHasFpu/fpEmulCycles, which applied the device
         * PE's soft-float penalty to every core in the simulation -- a co-sim
         * host with a real FPU included. Set per group by init.cpp. */
        bool coreHasFpu_ = true;
        uint32_t coreFpEmulCycles_ = 0;
    public:
        void setFpuCapability(bool hasFpu, uint32_t emulCycles) {
            coreHasFpu_ = hasFpu; coreFpEmulCycles_ = emulCycles;
        }

    protected:
        uint64_t instrs;
        uint64_t curCycle;
        uint64_t phaseEndCycle; //next stopping point

        // ROI baselines: at roi_begin we snapshot instrs/curCycle so the
        // reported "cycles"/"instrs" stats reflect ONLY the region of interest
        // (the parallel kernel), excluding serial pre-ROI init/setup that runs
        // on the launcher PE (otherwise PE0's serial array-init dominates the
        // measurement and swamps the parallel kernel). 0 until roi_begin fires.
        uint64_t roiBaseInstrs = 0;
        uint64_t roiBaseCycle  = 0;

        // 6 ALU scaling factors for design space exploration
        double computeFactor;      // cycles-per-instruction multiplier (default 1.0)
        double accessFactor;       // cycles per load/store (default 1.0)
        double throughputFactor;   // parallelism divider on instruction count (default 1.0)
        int    operandWidth;       // operand bit-width (default 32)
        double energyFactor;       // per-op energy scale for reporting (default 1.0)
        bool   bitSerial;          // true = bit-serial (compute cost ~ operandWidth); false = parallel (default)

        // PE memory interface (nullptr = simple model only, no hierarchy)
        PEMemoryInterface* mi_;
        uint32_t srcId_;           // global core index for MemReq.srcId
        /* 1.11.94 (H04 x05-zsim-cores-2): fractional carries. The compute
         * cost instrs*computeFactor*steps/throughputFactor and the per-access
         * accessFactor are real-valued; each used to be truncated to an
         * integer per charge with no remainder kept, so a block (or access)
         * costing below one cycle was free (lanes 8 / throughput_factor 8
         * with 5-instruction blocks charged 0 against the documented 0.625
         * per block). The whole-cycle part is charged now and the remainder
         * in [0,1) carries to the next charge. Separate carries for compute
         * and access so each documented formula holds on its own. */
        double computeCarry_ = 0.0;
        double accessCarry_  = 0.0;
        inline uint64_t chargeCompute(double cyc) {
            double acc = computeCarry_ + cyc;
            double whole = std::floor(acc);
            computeCarry_ = acc - whole;
            return (uint64_t)whole;
        }
        inline uint64_t chargeAccess() {
            double acc = accessCarry_ + accessFactor;
            double whole = std::floor(acc);
            accessCarry_ = acc - whole;
            return (uint64_t)whole;
        }
        volatile bool dmaWindow_ = false;  // staging memcpy in flight: its bulk
                                           // cost is charged on the link, so the
                                           // copy loop charges flat accessFactor

    public:
        ALUCore(g_string& _name, double _computeFactor = 1.0, double _accessFactor = 1.0,
                double _throughputFactor = 1.0, int _operandWidth = 32, double _energyFactor = 1.0,
                PEMemoryInterface* mi = nullptr, uint32_t srcId = 0, bool _bitSerial = false);
        void initStats(AggregateStat* parentStat);

        uint64_t getInstrs() const {return instrs;}
        uint64_t getPhaseCycles() const;
        uint64_t getCycles() const {return curCycle;}

        void contextSwitch(int32_t gid);
        virtual void join();

        InstrFuncPtrs GetFuncPtrs();

        void addDelay(uint32_t cycles) override { curCycle += cycles; }
        void pimidRewindCycles(uint64_t delta) override {
            /* Floor at the ROI baseline: rewinding below it makes the
             * reported (curCycle - roiBaseCycle) wrap negative. */
            uint64_t floorCyc = roiBaseCycle;
            uint64_t tgt = (curCycle > delta) ? (curCycle - delta) : 0;
            curCycle = (tgt > floorCyc) ? tgt : floorCyc;
        }

        // Snapshot current counters as the ROI baseline (called on roi_begin).
        void markRoiBegin() override { roiBaseInstrs = instrs; roiBaseCycle = curCycle; mixMarkRoi(); }
        uint64_t getRoiBaseCycle() const override { return roiBaseCycle; }
        uint32_t getSrcId() const override { return srcId_; }

        void setMemPricingPaused(bool paused) override { dmaWindow_ = paused; }

    protected:
        inline void bbl(BblInfo* bblInstrs);


        static void LoadFunc(THREADID tid, ADDRINT addr);
        static void StoreFunc(THREADID tid, ADDRINT addr);
        static void BblFunc(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo);
        static void PredLoadFunc(THREADID tid, ADDRINT addr, BOOL pred);
        static void PredStoreFunc(THREADID tid, ADDRINT addr, BOOL pred);

        static void BranchFunc(THREADID, ADDRINT, BOOL, ADDRINT, ADDRINT) {}
} ATTR_LINE_ALIGNED; //Cache line aligned to avoid false sharing

#endif  // ALU_CORE_H_
