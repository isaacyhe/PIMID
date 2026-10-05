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

#include "null_core.h"
#include "bithacks.h"
#include "zsim.h"

NullCore::NullCore(g_string& _name) : Core(_name), instrs(0), curCycle(0), phaseEndCycle(0) {}

void NullCore::initStats(AggregateStat* parentStat) {
    AggregateStat* coreStat = new AggregateStat();
    coreStat->init(name.c_str(), "Core stats");
    // 1.11.18: pgActivePhases now emitted (ROI-relative) by mixInitStats.
    mixInitStats(coreStat);   // 1.11.10 measured instruction mix

    // Report cycles/instrs RELATIVE to the ROI baseline (roi_begin); roiBase* are
    // 0 until roi_begin, so non-ROI workloads are unaffected. IPC=1, so both the
    // "cycles" and "instrs" stats derive from instrs (curCycle can be skewed fwd).
    auto x = [this]() -> uint64_t { return instrs - roiBaseInstrs; }; //simulated instrs == simulated cycles
    auto cyclesStat = makeLambdaStat(x);
    cyclesStat->init("cycles", "Simulated cycles");
    auto xi = [this]() -> uint64_t { return instrs - roiBaseInstrs; };
    auto instrsStat = makeLambdaStat(xi);
    instrsStat->init("instrs", "Simulated instructions");
    coreStat->append(cyclesStat);
    coreStat->append(instrsStat);
    parentStat->append(coreStat);
}

uint64_t NullCore::getPhaseCycles() const {
    return curCycle - zinfo->globPhaseCycles;
}

void NullCore::bbl(BblInfo* bblInfo) {
    instrs += bblInfo->instrs;
        { /* 1.11.8 PG residency. 1.11.18: an INJECTED timing charge is a
           * WAIT (barrier/flush/launch), not retirement -- marking it
           * active credited the very windows power gating exists for as
           * busy, so a co-sim PE could never show idle residency. */
          if (!bblInfo->synth) { uint64_t _ph = zinfo->numPhases;
              pgAct.touch(_ph); zinfo->pgres.anyCore.touch(_ph); zinfo->pgres.hostUnion.touch(_ph); } }
        mixAdd(bblInfo);  // 1.11.10 measured instruction mix
    curCycle += bblInfo->instrs;
}

void NullCore::contextSwitch(int32_t gid) {}

void NullCore::join() {
    curCycle = MAX(curCycle, zinfo->globPhaseCycles);
    phaseEndCycle = zinfo->globPhaseCycles + zinfo->phaseLength;
}

//Static class functions: Function pointers and trampolines

InstrFuncPtrs NullCore::GetFuncPtrs() {
    return {LoadFunc, StoreFunc, BblFunc, BranchFunc, PredLoadFunc, PredStoreFunc, FPTR_ANALYSIS, {0}};
}

void NullCore::LoadFunc(THREADID tid, ADDRINT addr) {}
void NullCore::StoreFunc(THREADID tid, ADDRINT addr) {}
void NullCore::PredLoadFunc(THREADID tid, ADDRINT addr, BOOL pred) {}
void NullCore::PredStoreFunc(THREADID tid, ADDRINT addr, BOOL pred) {}

void NullCore::BblFunc(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo) {
    NullCore* core = static_cast<NullCore*>(cores[tid]);
    core->bbl(bblInfo);

    while (unlikely(core->curCycle > core->phaseEndCycle)) {
        assert(core->phaseEndCycle == zinfo->globPhaseCycles + zinfo->phaseLength);
        core->phaseEndCycle += zinfo->phaseLength;

        uint32_t cid = getCid(tid);
        //NOTE: TakeBarrier may take ownership of the core, and so it will be used by some other thread. If TakeBarrier context-switches us,
        //the *only* safe option is to return inmmediately after we detect this, or we can race and corrupt core state. If newCid == cid,
        //we're not at risk of racing, even if we were switched out and then switched in.
        uint32_t newCid = TakeBarrier(tid, cid);
        if (newCid != cid) break; /*context-switch*/
    }
}

