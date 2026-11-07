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

#ifndef NULL_CORE_H_
#define NULL_CORE_H_

//A core model with IPC=1 and no hooks into the memory hierarchy. Useful to isolate threads that need to be run for simulation purposes.

#include "core.h"
#include "pad.h"

class NullCore : public Core {
    public:
        bool pimidDeviceGrid() const override { return true; }   // (1.12.283): a device element, on the device's phase grid
    protected:
        uint64_t instrs;
        uint64_t curCycle;
        uint64_t phaseEndCycle; //next stopping point

        // ROI baselines: snapshot at roi_begin so reported cycles/instrs reflect
        // ONLY the region of interest. NullCore has IPC=1, so both cycles and
        // instrs derive from instrs; we baseline instrs only. 0 until roi_begin.
        uint64_t roiBaseInstrs = 0;
        uint64_t roiBaseCycle  = 0;

    public:
        explicit NullCore(g_string& _name);
        void initStats(AggregateStat* parentStat);

        uint64_t getInstrs() const {return instrs;}
        uint64_t getPhaseCycles() const;
        uint64_t getCycles() const {return instrs; /*IPC=1*/ }

        void contextSwitch(int32_t gid);
        virtual void join();

        InstrFuncPtrs GetFuncPtrs();

        // Snapshot current counters as the ROI baseline (called on roi_begin).
        // 1.11.16 (verification audit): the mix census must rebase WITH
        // instrs -- every other core type calls mixMarkRoi() here, and a
        // null-core run with pre-ROI warm-up was reporting ROI-windowed
        // instrs against a whole-run census (the 1.9.29 counter-base defect
        // class, activated by decode-always).
        void markRoiBegin() override { roiBaseInstrs = instrs; roiBaseCycle = curCycle; mixMarkRoi(); }

    protected:
        inline void bbl(BblInfo* bblInstrs);

        static void LoadFunc(THREADID tid, ADDRINT addr);
        static void StoreFunc(THREADID tid, ADDRINT addr);
        static void BblFunc(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo);
        static void PredLoadFunc(THREADID tid, ADDRINT addr, BOOL pred);
        static void PredStoreFunc(THREADID tid, ADDRINT addr, BOOL pred);

        static void BranchFunc(THREADID, ADDRINT, BOOL, ADDRINT, ADDRINT) {}
} ATTR_LINE_ALIGNED; //This needs to take up a whole cache line, or false sharing will be extremely frequent

#endif  // NULL_CORE_H_

