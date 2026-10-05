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

#include "simple_core.h"
#include "filter_cache.h"
#include "zsim.h"

SimpleCore::SimpleCore(FilterCache* _l1i, FilterCache* _l1d, g_string& _name) : Core(_name), l1i(_l1i), l1d(_l1d), instrs(0), curCycle(0), haltedCycles(0) {
}

void SimpleCore::initStats(AggregateStat* parentStat) {
    AggregateStat* coreStat = new AggregateStat();
    coreStat->init(name.c_str(), "Core stats");
    // 1.11.18: pgActivePhases now emitted (ROI-relative) by mixInitStats.
    mixInitStats(coreStat);   // 1.11.10 measured instruction mix
    // Report cycles/instrs RELATIVE to the ROI baseline (roi_begin); roiBase* are
    // 0 until roi_begin, so non-ROI workloads are unaffected.
    auto x = [this]() -> uint64_t {
        assert(curCycle >= haltedCycles);
        uint64_t c = curCycle - haltedCycles;
        c = (c > pimidPhantomWait) ? (c - pimidPhantomWait) : 0;   // 1.9.4 wall-free (frozen-clock MPI rewind)
        return (c > roiBaseCycle) ? (c - roiBaseCycle) : 0;
    };
    auto cyclesStat = makeLambdaStat(x);
    cyclesStat->init("cycles", "Simulated cycles");
    auto xi = [this]() -> uint64_t { return instrs - roiBaseInstrs; };
    auto instrsStat = makeLambdaStat(xi);
    instrsStat->init("instrs", "Simulated instructions");
    coreStat->append(cyclesStat);
    coreStat->append(instrsStat);
    parentStat->append(coreStat);
}

uint64_t SimpleCore::getPhaseCycles() const {
    return curCycle % zinfo->phaseLength;
}

void SimpleCore::load(Address addr) {
    curCycle = l1d->load(addr, curCycle);
}

void SimpleCore::store(Address addr) {
    curCycle = l1d->store(addr, curCycle);
}

void SimpleCore::bbl(Address bblAddr, BblInfo* bblInfo) {
    //info("BBL %s %p", name.c_str(), bblInfo);
    //info("%d %d", bblInfo->instrs, bblInfo->bytes);
    instrs += bblInfo->instrs;
        { /* 1.11.8 PG residency. 1.11.18: an INJECTED timing charge is a
           * WAIT (barrier/flush/launch), not retirement -- marking it
           * active credited the very windows power gating exists for as
           * busy, so a co-sim PE could never show idle residency. */
          if (!bblInfo->synth) { uint64_t _ph = zinfo->numPhases;
              pgAct.touch(_ph); zinfo->pgres.anyCore.touch(_ph); zinfo->pgres.hostUnion.touch(_ph); } }
        mixAdd(bblInfo);  // 1.11.10 measured instruction mix
        if (!coreHasFpu_ && coreFpEmulCycles_ &&
            bblInfo->nFp) {   // 1.11.11 (#113): soft-float on an FPU-less element
            curCycle += (uint64_t)bblInfo->nFp * coreFpEmulCycles_;
        }
    curCycle += bblInfo->instrs;

    Address endBblAddr = bblAddr + bblInfo->bytes;
    for (Address fetchAddr = bblAddr; fetchAddr < endBblAddr; fetchAddr+=(1 << lineBits)) {
        curCycle = l1i->load(fetchAddr, curCycle);
    }
}

void SimpleCore::contextSwitch(int32_t gid) {
    if (gid == -1) {
        l1i->contextSwitch();
        l1d->contextSwitch();
    }
}

void SimpleCore::join() {
    //info("[%s] Joining, curCycle %ld phaseEnd %ld haltedCycles %ld", name.c_str(), curCycle, phaseEndCycle, haltedCycles);
    if (curCycle < zinfo->globPhaseCycles) { //carry up to the beginning of the phase
        haltedCycles += (zinfo->globPhaseCycles - curCycle);
        curCycle = zinfo->globPhaseCycles;
    }
    phaseEndCycle = zinfo->globPhaseCycles + zinfo->phaseLength;
    //note that with long events, curCycle can be arbitrarily larger than phaseEndCycle; however, it must be aligned in current phase
    //info("[%s] Joined, curCycle %ld phaseEnd %ld haltedCycles %ld", name.c_str(), curCycle, phaseEndCycle, haltedCycles);
}


//Static class functions: Function pointers and trampolines

InstrFuncPtrs SimpleCore::GetFuncPtrs() {
    return {LoadFunc, StoreFunc, BblFunc, BranchFunc, PredLoadFunc, PredStoreFunc, FPTR_ANALYSIS, {0}};
}

void SimpleCore::LoadFunc(THREADID tid, ADDRINT addr) {
    static_cast<SimpleCore*>(cores[tid])->load(addr);
}

void SimpleCore::StoreFunc(THREADID tid, ADDRINT addr) {
    static_cast<SimpleCore*>(cores[tid])->store(addr);
}

void SimpleCore::PredLoadFunc(THREADID tid, ADDRINT addr, BOOL pred) {
    if (pred) static_cast<SimpleCore*>(cores[tid])->load(addr);
}

void SimpleCore::PredStoreFunc(THREADID tid, ADDRINT addr, BOOL pred) {
    if (pred) static_cast<SimpleCore*>(cores[tid])->store(addr);
}

void SimpleCore::BblFunc(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo) {
    SimpleCore* core = static_cast<SimpleCore*>(cores[tid]);
    core->bbl(bblAddr, bblInfo);

    while (core->curCycle > core->phaseEndCycle) {
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

