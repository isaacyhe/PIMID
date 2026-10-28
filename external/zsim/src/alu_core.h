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

        /* 1.12.18 (user ruling 2026-10-23, "sync to waker"): curCycle is this
         * PE's TIMELINE: busy cycles plus the forward jumps a futex wake
         * synced it by (syncToWaker). The jumps are waiting, not activity:
         * syncSkip_ keeps them, and the 'cycles' stat (busy, what the power
         * model prices activity over) is curCycle - roiBaseCycle - the ROI
         * part of syncSkip_. */
        uint64_t syncSkip_ = 0, roiSyncSkip_ = 0;
        uint64_t wakeSyncs_ = 0, roiWakeSyncs_ = 0;
        /* the end of the phase this clock is in: the phase grid is the
         * core's own clock in phaseLength steps (BblFunc advances
         * phaseEndCycle by phaseLength), so a (re)joining or synced core
         * runs to the end of the grid phase its clock is in -- the same
         * phase end as the waker when synced to it -- not a fresh phase. */
        static uint64_t gridPhaseEnd(uint64_t c);   // alu_core.cpp

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
        /* 1.12.18: the scratchpad. spmCap_ is the configured capacity (0 =
         * none); [spmLo_, spmHi_) is the RAM the running kernel bound to this
         * PE. A load or store inside it costs the PE's own access charge plus
         * the RAM's access time (CACTI, carried fraction like accessFactor)
         * and never reaches the memory interface. spmMode_ 1 = DMA engine,
         * 2 = the PE copies (pim.pe.spm_staging). A DMA transfer issues its
         * lines at the memory interface's service rate. */
        uint64_t spmCap_ = 0;
        uint32_t spmMode_ = 0;
        double   spmLatency_ = 0.0;
        uint64_t spmLo_ = 0, spmHi_ = 0;
        /* the DMA engine's command registers: a 64-byte block bound with the
         * RAM. The kernel writes a transfer's description there (as a real
         * engine is loaded through its registers), so an access costs the PE's
         * own access charge and nothing else, and is counted for the engine's
         * register energy. */
        uint64_t cmdLo_ = 0, cmdHi_ = 0;
        uint64_t cmdReads_ = 0, cmdWrites_ = 0, roiCmdReads_ = 0, roiCmdWrites_ = 0;
        inline bool inCmd(ADDRINT a) const { return (uint64_t)a - cmdLo_ < cmdHi_ - cmdLo_; }
        double   spmCarry_ = 0.0;
        uint64_t spmReads_ = 0, spmWrites_ = 0, dmaOps_ = 0, dmaLines_ = 0, dmaCycles_ = 0;
        uint64_t roiSpmReads_ = 0, roiSpmWrites_ = 0, roiDmaOps_ = 0, roiDmaLines_ = 0, roiDmaCycles_ = 0;
        inline bool inSpm(ADDRINT a) const { return (uint64_t)a - spmLo_ < spmHi_ - spmLo_; }
        /* 1.12.18: where the PE's own stack lives (pim.pe.stack). Its stack is
         * the registered guest stack it owns (the 1.12.17 first-touch claim);
         * another thread's stack (an OpenMP master's shared locals) is always
         * in that PE's memory. stkInSpm_ (stack: spm): the stack shares the
         * scratchpad with the buffers, which get spmCap_ - stkReserve_
         * (pim.pe.spm_stack_bytes); its accesses are scratchpad accesses, and
         * a ROI stack span beyond the reserve stops the run. Otherwise
         * (stack: dram, the default) it stays in the PE's DRAM as in 1.12.17
         * and its ROI span [stkMinT_, stkMaxT_] is only measured. */
        bool     stkInSpm_ = false;
        uint64_t stkReserve_ = 0;
        uint64_t stkLo_ = 0, stkHi_ = 0;
        uint64_t stkMinT_ = ~0ull, stkMaxT_ = 0;
        uint64_t stkReads_ = 0, stkWrites_ = 0, roiStkReads_ = 0, roiStkWrites_ = 0;
        bool inStack(ADDRINT a);
        void peekStack(ADDRINT a);
        inline uint64_t chargeSpm() {
            double acc = spmCarry_ + spmLatency_;
            double whole = std::floor(acc);
            spmCarry_ = acc - whole;
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

        /* 1.12.18 (user ruling 2026-10-23, "sync to waker"), see core.h */
        bool wakeStampable() const override { return true; }
        uint64_t syncToWaker(uint64_t t) override;   // alu_core.cpp
        /* 1.12.18 (user ruling 2026-10-23, "model the lock word"), see core.h */
        uint32_t lockClockDomain() const override { return 2; }
        uint64_t lockClock() const override { return curCycle; }
        uint64_t syncToLock(uint64_t t) override;    // alu_core.cpp
        /* busy = ROI cycles without the wake-sync and lock-sync jumps (never
         * negative: pimidRewindCycles may pull curCycle back toward the
         * baseline) */
        uint64_t roiBusyCycles() const {
            const uint64_t el = curCycle - roiBaseCycle,
                           sk = (syncSkip_ - roiSyncSkip_) + (lockSyncCycles - roiBaseLockSyncCycles);   // 1.12.18: + lock syncs
            return (el > sk) ? el - sk : 0;
        }

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
        void markRoiBegin() override {
            roiBaseInstrs = instrs; roiBaseCycle = curCycle; mixMarkRoi();
            roiSyncSkip_ = syncSkip_; roiWakeSyncs_ = wakeSyncs_;              // 1.12.18 (sync to waker)
            roiSpmReads_ = spmReads_; roiSpmWrites_ = spmWrites_;            // 1.12.18
            roiDmaOps_ = dmaOps_; roiDmaLines_ = dmaLines_; roiDmaCycles_ = dmaCycles_;
            roiCmdReads_ = cmdReads_; roiCmdWrites_ = cmdWrites_;
            roiStkReads_ = stkReads_; roiStkWrites_ = stkWrites_;
            stkMinT_ = ~0ull; stkMaxT_ = 0;
        }
        uint64_t getRoiBaseCycle() const override { return roiBaseCycle; }
        uint32_t getSrcId() const override { return srcId_; }

        void setMemPricingPaused(bool paused) override { dmaWindow_ = paused; }

        /* 1.12.18: the scratchpad (see the fields above). */
        void setScratchpad(uint64_t cap, uint32_t mode, double latency, bool stackInSpm, uint64_t stackReserve) {
            spmCap_ = cap; spmMode_ = mode; spmLatency_ = latency;
            stkInSpm_ = cap && stackInSpm; stkReserve_ = stkInSpm_ ? stackReserve : 0;
        }
        /* the RAM a kernel may bind: the scratchpad less the stack's reserve */
        uint64_t spmBufferCap() const { return spmCap_ - stkReserve_; }
        PEMemoryInterface* firstTouchMI() const override { return mi_; }   // (1.12.165)
        void spmQuery(uint64_t* cap, uint64_t* mode) const override { *cap = spmBufferCap(); *mode = spmCap_ ? spmMode_ : 0; }
        bool spmBind(uint64_t lo, uint64_t bytes, uint64_t cmd) override;
        bool spmDma(uint64_t dst, uint64_t src, uint64_t bytes, bool toDram) override;
        void spmOwnStack(uint64_t sp) override;

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
