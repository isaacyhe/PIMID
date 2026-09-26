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

#include "ooo_core.h"
#include <algorithm>
#include <cstdlib>
#include <queue>
#include <string>
#include "bithacks.h"
#include "decoder_simple.h"
#include "filter_cache.h"
#include "zsim.h"

/* Uncomment to induce backpressure to the IW when the load/store buffers fill up. In theory, more detailed,
 * but sometimes much slower (as it relies on range poisoning in the IW, potentially O(n^2)), and in practice
 * makes a negligible difference (ROB backpressures).
 */
//#define LSU_IW_BACKPRESSURE

#define DEBUG_MSG(args...)
//#define DEBUG_MSG(args...) info(args)

// Core parameters
// TODO(dsm): Make OOOCore templated, subsuming these

// Stages --- more or less matched to Westmere, but have not seen detailed pipe diagrams anywhare
#define FETCH_STAGE 1
#define DECODE_STAGE 4  // NOTE: Decoder adds predecode delays to decode
#define ISSUE_STAGE 7
#define DISPATCH_STAGE 13  // RAT + ROB + RS, each is easily 2 cycles

#define L1D_LAT 4  // fixed, and FilterCache does not include L1 delay
#define FETCH_BYTES_PER_CYCLE 16
#define ISSUES_PER_CYCLE 4
#define RF_READS_PER_CYCLE 3

OOOCore::OOOCore(FilterCache* _l1i, FilterCache* _l1d, g_string& _name) : Core(_name), l1i(_l1i), l1d(_l1d), cRec(0, _name) {
    decodeCycle = DECODE_STAGE;  // allow subtracting from it
    curCycle = 0;
    phaseEndCycle = zinfo->phaseLength;

    for (uint32_t i = 0; i < MAX_REGISTERS; i++) {
        regScoreboard[i] = 0;
    }
    prevBbl = nullptr;

    lastStoreCommitCycle = 0;
    lastStoreAddrCommitCycle = 0;
    curCycleRFReads = 0;
    curCycleIssuedUops = 0;
    branchPc = 0;

    instrs = uops = bbls = approxInstrs = branches = mispredBranches = 0;
    decodedBbls = syntheticBbls = memMismatchLoads = memMismatchStores = 0;
    repDrainedLoads = repDrainedStores = 0;
    oooDebug = (getenv("PIMID_OOO_DEBUG") != nullptr);
    oooDebugBbls = 0;

    for (uint32_t i = 0; i < FWD_ENTRIES; i++) fwdArray[i].set((Address)(-1L), 0);
}

void OOOCore::initStats(AggregateStat* parentStat) {
    AggregateStat* coreStat = new AggregateStat();
    coreStat->init(name.c_str(), "Core stats");
    // 1.11.18: pgActivePhases now emitted (ROI-relative) by mixInitStats.
    mixInitStats(coreStat);   // 1.11.10 measured instruction mix

    // Report cycles/instrs RELATIVE to the ROI baseline (roi_begin); roiBase* are
    // 0 until roi_begin, so non-ROI workloads are unaffected.
    /* 1.11.57 (latent F026/F027): WHICH COUNTERS ARE ON WHICH WINDOW.
     *
     * This note used to end "cCycles/uops/bbls/approxInstrs/mispredBranches
     * are left absolute as diagnostic counters" -- naming four counters that
     * 1.9.33 and 1.11.9 subsequently windowed. cCycles, uops, bbls and
     * mispredBranches are all LambdaStats against a roiBase below; only
     * approxInstrs is still a raw ProxyStat. A reader trusting the comment
     * would have believed four ROI-windowed numbers were whole-run.
     *
     * The counters that ARE whole-run, and are emitted in this same block
     * beside the windowed ones, are the raw ProxyStats: approxInstrs,
     * decodedBbls, syntheticBbls, memMismatch{Loads,Stores},
     * repDrained{Loads,Stores}, indirBranches, indirMispreds, rasReturns,
     * rasMispreds, depStalls, issueStalls, mispredStallCycles. Their
     * descriptions now say so, because nothing else in the output does:
     * docs/cores.md lists them next to instrs with no base annotation, and a
     * ratio taken across the two bases divides an ROI numerator by a
     * whole-run denominator. No consumer forms such a ratio today (none of
     * these names appears in src/main.cpp or src/power/), so this is a reader
     * trap rather than a live number -- which is precisely why the trap is
     * closed in the label rather than by rebasing counters nothing reads. */
    auto x = [this]() {
        uint64_t c = cRec.getUnhaltedCycles(curCycle);
        c = (c > pimidPhantomWait) ? (c - pimidPhantomWait) : 0;   // 1.6.1 wall-free
        return (c > roiBaseCycle) ? (c - roiBaseCycle) : 0;
    };
    LambdaStat<decltype(x)>* cyclesStat = new LambdaStat<decltype(x)>(x);
    cyclesStat->init("cycles", "Simulated unhalted cycles");

    auto y = [this]() -> uint64_t {
        uint64_t c = cRec.getContentionCycles();
        /* 1.11.9: ROI-windowed. 1.11.51 (L255), correcting the record: the
         * 1.11.9 note motivated this as a host_wall_cycles consistency fix,
         * but the rebase applies to EVERY core's cCycles in EVERY scope --
         * device PEs included -- so it moved contention-derived numbers for
         * every node in system scope, not just the host sum. The windowing
         * itself is correct (all activity stats share the ROI window); what
         * was wrong was the stated blast radius. */
        return (c > roiBaseCCycles) ? (c - roiBaseCCycles) : 0;
    };
    LambdaStat<decltype(y)>* cCyclesStat = new LambdaStat<decltype(y)>(y);
    cCyclesStat->init("cCycles", "Cycles due to contention stalls");

    auto z = [this]() -> uint64_t { return instrs - roiBaseInstrs; };
    LambdaStat<decltype(z)>* instrsStat = new LambdaStat<decltype(z)>(z);
    instrsStat->init("instrs", "Simulated instructions");
    /* 1.9.33: ROI-windowed, matching instrs above. These were raw whole-run
     * ProxyStats while instrs was windowed, so any mix derived from their
     * ratio divided an ROI numerator by a whole-run denominator. */
    auto zu = [this]() -> uint64_t { return uops - roiBaseUops; };
    LambdaStat<decltype(zu)>* uopsStat = new LambdaStat<decltype(zu)>(zu);
    uopsStat->init("uops", "Retired micro-ops");
    auto zb = [this]() -> uint64_t { return bbls - roiBaseBbls; };
    LambdaStat<decltype(zb)>* bblsStat = new LambdaStat<decltype(zb)>(zb);
    bblsStat->init("bbls", "Basic blocks");
    auto zbr = [this]() -> uint64_t { return branches - roiBaseBranches; };
    LambdaStat<decltype(zbr)>* branchesStat = new LambdaStat<decltype(zbr)>(zbr);
    branchesStat->init("branches", "Resolved branches fed to the predictor");
    ProxyStat* approxInstrsStat = new ProxyStat();
    approxInstrsStat->init("approxInstrs", "Instrs with approx uop decoding [whole run]", &approxInstrs);
    auto zm = [this]() -> uint64_t { return mispredBranches - roiBaseMispred; };
    LambdaStat<decltype(zm)>* mispredBranchesStat = new LambdaStat<decltype(zm)>(zm);
    mispredBranchesStat->init("mispredBranches", "Mispredicted branches");
    ProxyStat* decodedBblsStat = new ProxyStat();
    decodedBblsStat->init("decodedBbls", "BBLs run through the decoded dataflow OOO path [whole run]", &decodedBbls);
    ProxyStat* syntheticBblsStat = new ProxyStat();
    syntheticBblsStat->init("syntheticBbls", "BBLs run through the synthetic 1-CPI fallback [whole run]", &syntheticBbls);
    ProxyStat* memMismatchLoadsStat = new ProxyStat();
    memMismatchLoadsStat->init("memMismatchLoads", "Decoded/runtime load-count divergences drained [whole run]", &memMismatchLoads);
    ProxyStat* memMismatchStoresStat = new ProxyStat();
    memMismatchStoresStat->init("memMismatchStores", "Decoded/runtime store-count divergences drained [whole run]", &memMismatchStores);
    ProxyStat* repDrainedLoadsStat = new ProxyStat();
    repDrainedLoadsStat->init("repDrainedLoads", "Expected rep-string loads drained (block-copy model) [whole run]", &repDrainedLoads);
    ProxyStat* repDrainedStoresStat = new ProxyStat();
    repDrainedStoresStat->init("repDrainedStores", "Expected rep-string stores drained (block-copy model) [whole run]", &repDrainedStores);
    ProxyStat* indirBranchesStat = new ProxyStat();
    indirBranchesStat->init("indirBranches", "Indirect jmp/call resolutions fed to the BTB [whole run]", &indirBranches);
    ProxyStat* indirMispredsStat = new ProxyStat();
    indirMispredsStat->init("indirMispreds", "Indirect jmp/call target mispredictions [whole run]", &indirMispreds);
    ProxyStat* rasReturnsStat = new ProxyStat();
    rasReturnsStat->init("rasReturns", "Returns resolved against the RAS [whole run]", &rasReturns);
    ProxyStat* rasMispredsStat = new ProxyStat();
    rasMispredsStat->init("rasMispreds", "Return-target mispredictions (RAS miss) [whole run]", &rasMispreds);

    coreStat->append(cyclesStat);
    coreStat->append(cCyclesStat);
    coreStat->append(instrsStat);
    coreStat->append(uopsStat);
    coreStat->append(bblsStat);
    coreStat->append(approxInstrsStat);
    coreStat->append(branchesStat);   // 1.9.33
    /* 1.11.16 (verification audit): the oooBbl-null-based syntheticInstrs
     * stat is GONE -- it also counted real >1024-insn fallback TBs as
     * synthetic (subtracting real work from the retired base), and the
     * flag-based counter in Core::mixInitStats now reports the stat for
     * every core type. Emitting both here would make the parser sum two
     * "syntheticInstrs" keys in this core's group and subtract twice. The
     * syntheticBbls stat below is unchanged: it describes the MODELING path
     * taken (decoded vs 1-CPI fallback), not injected charges. */
    coreStat->append(mispredBranchesStat);
    coreStat->append(decodedBblsStat);
    coreStat->append(syntheticBblsStat);
    coreStat->append(memMismatchLoadsStat);
    coreStat->append(memMismatchStoresStat);
    coreStat->append(repDrainedLoadsStat);
    coreStat->append(repDrainedStoresStat);
    coreStat->append(indirBranchesStat);
    coreStat->append(indirMispredsStat);
    coreStat->append(rasReturnsStat);
    coreStat->append(rasMispredsStat);
    /* 1.11.93 (F6): ROI-windowed twins of indirBranches/rasReturns -- the
     * BTB and RAS activity the power model prices (in_order_core.h). */
    auto zib = [this]() -> uint64_t { return indirBranches - roiBaseIndir; };
    LambdaStat<decltype(zib)>* roiIndirStat = new LambdaStat<decltype(zib)>(zib);
    roiIndirStat->init("roiIndirBranches", "Indirect jmp/call resolutions fed to the BTB (ROI)");
    coreStat->append(roiIndirStat);
    auto zrr = [this]() -> uint64_t { return rasReturns - roiBaseRas; };
    LambdaStat<decltype(zrr)>* roiRasStat = new LambdaStat<decltype(zrr)>(zrr);
    roiRasStat->init("roiRasReturns", "Returns resolved against the RAS (ROI)");
    coreStat->append(roiRasStat);

#ifdef OOO_STALL_STATS
    profFetchStalls.init("fetchStalls",  "Fetch stalls");  coreStat->append(&profFetchStalls);
    profDecodeStalls.init("decodeStalls", "Decode stalls"); coreStat->append(&profDecodeStalls);
    profIssueStalls.init("issueStalls",  "Issue stalls");  coreStat->append(&profIssueStalls);
#endif

    parentStat->append(coreStat);
}

uint64_t OOOCore::getInstrs() const {return instrs;}
uint64_t OOOCore::getPhaseCycles() const {return curCycle % zinfo->phaseLength;}

void OOOCore::contextSwitch(int32_t gid) {
    if (gid == -1) {
        // Do not execute previous BBL, as we were context-switched
        prevBbl = nullptr;
        indirMispredPending = false;  // pending redirect belongs to a dropped BBL

        // Invalidate virtually-addressed filter caches
        l1i->contextSwitch();
        l1d->contextSwitch();
    }
}


InstrFuncPtrs OOOCore::GetFuncPtrs() {return {LoadFunc, StoreFunc, BblFunc, BranchFunc, PredLoadFunc, PredStoreFunc, FPTR_ANALYSIS, {0}};}

inline void OOOCore::load(Address addr) {
    if (loads < 256) loadAddrs[loads] = addr;
    loads++;
}

void OOOCore::store(Address addr) {
    if (stores < 256) storeAddrs[stores] = addr;
    stores++;
}

// Predicated loads and stores call this function, gets recorded as a 0-cycle op.
// Predication is rare enough that we don't need to model it perfectly to be accurate (i.e. the uops still execute, retire, etc), but this is needed for correctness.
void OOOCore::predFalseLoad() {
    if (loads < 256) loadAddrs[loads] = -1L;
    loads++;
}

void OOOCore::predFalseStore() {
    if (stores < 256) storeAddrs[stores] = -1L;
    stores++;
}

void OOOCore::branch(Address pc, bool taken, Address takenNpc, Address notTakenNpc) {
    branchPc = pc;
    branchTaken = taken;
    branchTakenNpc = takenNpc;
    branchNotTakenNpc = notTakenNpc;
}

inline void OOOCore::bbl(Address bblAddr, BblInfo* bblInfo) {
    if (!prevBbl) {
        // This is the 1st BBL since scheduled, nothing to simulate
        prevBbl = bblInfo;
        // Kill lingering ops from previous BBL
        loads = stores = 0;
        return;
    }

    /* Simulate execution of previous BBL */

    uint32_t bblInstrs = prevBbl->instrs;
    DynBbl* bbl = &(prevBbl->oooBbl[0]);
    /* 1.11.51 (L207): keep the pointer to the BBL being RETIRED. Everything
     * charged "at retire" below -- the measured mix, the PG-residency
     * activity mark, the FP soft-float penalty -- used to read bblInfo, the
     * NEXT block that just arrived, while the cycle cost came from this
     * one: a one-block attribution shift on every accumulator, and a wrong
     * window class whenever a real block retired as a synthetic one arrived
     * (or vice versa). Run totals barely moved; per-window attribution and
     * the synth/real split did. */
    BblInfo* retiredBbl = prevBbl;
    prevBbl = bblInfo;

    uint64_t lastCommitCycle = 0;  // used to find misprediction penalty

    if (bbl->uops > 0) {
        /* Full OOO simulation with decoded micro-ops */
        uint32_t loadIdx = 0;
        uint32_t storeIdx = 0;
        uint32_t prevDecCycle = 0;

        for (uint32_t i = 0; i < bbl->uops; i++) {
            DynUop* uop = &(bbl->uop[i]);

            // Decode stalls
            uint32_t decDiff = uop->decCycle - prevDecCycle;
            decodeCycle = MAX(decodeCycle + decDiff, uopQueue.minAllocCycle());
            if (decodeCycle > curCycle) {
                uint32_t cdDiff = decodeCycle - curCycle;
#ifdef OOO_STALL_STATS
                profDecodeStalls.inc(cdDiff);
#endif
                curCycleIssuedUops = 0;
                curCycleRFReads = 0;
                for (uint32_t j = 0; j < cdDiff; j++) insWindow.advancePos(curCycle);
            }
            prevDecCycle = uop->decCycle;
            uopQueue.markLeave(curCycle);

            // Implement issue width limit --- we can only issue 4 uops/cycle
            if (curCycleIssuedUops >= ISSUES_PER_CYCLE) {
#ifdef OOO_STALL_STATS
                profIssueStalls.inc();
#endif
                curCycleIssuedUops = 0;
                curCycleRFReads = 0;
                insWindow.advancePos(curCycle);
            }
            curCycleIssuedUops++;

            regScoreboard[0] = curCycle;

            uint64_t c0 = regScoreboard[uop->rs[0]];
            uint64_t c1 = regScoreboard[uop->rs[1]];

            curCycleRFReads += ((c0 < curCycle)? 1 : 0) + ((c1 < curCycle)? 1 : 0);
            if (curCycleRFReads > RF_READS_PER_CYCLE) {
                curCycleRFReads -= RF_READS_PER_CYCLE;
                curCycleIssuedUops = 0;
                insWindow.advancePos(curCycle);
            }

            uint64_t c2 = rob.minAllocCycle();
            uint64_t c3 = curCycle;

            uint64_t cOps = MAX(c0, c1);
            uint64_t dispatchCycle = MAX(cOps, MAX(c2, c3) + (DISPATCH_STAGE - ISSUE_STAGE));

            insWindow.schedule(curCycle, dispatchCycle, uop->portMask, uop->extraSlots);

            if (curCycle > c3) {
                curCycleIssuedUops = 0;
                curCycleRFReads = 0;
            }

            uint64_t commitCycle;

            switch (uop->type) {
                case UOP_GENERAL:
                    commitCycle = dispatchCycle + uop->lat;
                    break;

                case UOP_LOAD:
                    {
                        uint64_t lqCycle = loadQueue.minAllocCycle();
                        if (lqCycle > dispatchCycle) {
#ifdef LSU_IW_BACKPRESSURE
                            insWindow.poisonRange(curCycle, lqCycle, 0x4 /*PORT_2, loads*/);
#endif
                            dispatchCycle = lqCycle;
                        }
                        dispatchCycle = MAX(lastStoreAddrCommitCycle+1, dispatchCycle);

                        // Tolerant consumption: the decoder's static load count
                        // may not exactly match QEMU's dynamic per-access
                        // callbacks (e.g. rep-string ops, or instructions the
                        // minimal decoder approximates). If the decoded stream
                        // expects more loads than were delivered, treat the
                        // extra as a predicated/no-op load (0-cycle) rather than
                        // reading past the buffer. Any UNDER-count (runtime
                        // loads beyond the decoded uops) is drained after the
                        // loop so memory traffic/NoC stay accounted.
                        Address addr;
                        if (loadIdx < loads) { addr = loadAddrs[loadIdx++]; }
                        else { addr = (Address)-1L; memMismatchLoads++; }
                        uint64_t reqSatisfiedCycle = dispatchCycle;
                        if (addr != ((Address)-1L)) {
                            reqSatisfiedCycle = l1d->load(addr, dispatchCycle) + L1D_LAT;
                            cRec.record(curCycle, dispatchCycle, reqSatisfiedCycle);
                        }

                        uint32_t fwdIdx = (addr>>2) & (FWD_ENTRIES-1);
                        if (fwdArray[fwdIdx].addr == addr) {
                            reqSatisfiedCycle = MAX(reqSatisfiedCycle, fwdArray[fwdIdx].storeCycle);
                        }

                        commitCycle = reqSatisfiedCycle;
                        loadQueue.markRetire(commitCycle);
                    }
                    break;

                case UOP_STORE:
                    {
                        uint64_t sqCycle = storeQueue.minAllocCycle();
                        if (sqCycle > dispatchCycle) {
#ifdef LSU_IW_BACKPRESSURE
                            insWindow.poisonRange(curCycle, sqCycle, 0x10 /*PORT_4, stores*/);
#endif
                            dispatchCycle = sqCycle;
                        }
                        dispatchCycle = MAX(lastStoreAddrCommitCycle+1, dispatchCycle);

                        // Tolerant consumption (see UOP_LOAD note above).
                        Address addr;
                        if (storeIdx < stores) { addr = storeAddrs[storeIdx++]; }
                        else { addr = (Address)-1L; memMismatchStores++; }
                        uint64_t reqSatisfiedCycle = dispatchCycle;
                        if (addr != ((Address)-1L)) {
                            reqSatisfiedCycle = l1d->store(addr, dispatchCycle) + L1D_LAT;
                            cRec.record(curCycle, dispatchCycle, reqSatisfiedCycle);
                            fwdArray[(addr>>2) & (FWD_ENTRIES-1)].set(addr, reqSatisfiedCycle);
                        }

                        commitCycle = reqSatisfiedCycle;
                        lastStoreCommitCycle = MAX(lastStoreCommitCycle, reqSatisfiedCycle);
                        storeQueue.markRetire(commitCycle);
                    }
                    break;

                case UOP_STORE_ADDR:
                    commitCycle = dispatchCycle + uop->lat;
                    lastStoreAddrCommitCycle = MAX(lastStoreAddrCommitCycle, commitCycle);
                    break;

                default:
                    assert((UopType) uop->type == UOP_FENCE);
                    commitCycle = dispatchCycle + uop->lat;
                    lastStoreAddrCommitCycle = MAX(commitCycle, MAX(lastStoreAddrCommitCycle, lastStoreCommitCycle + uop->lat));
            }

            rob.markRetire(commitCycle);
            regScoreboard[uop->rd[0]] = commitCycle;
            regScoreboard[uop->rd[1]] = commitCycle;
            lastCommitCycle = commitCycle;
        }

        // Drain any runtime accesses not consumed by the decoded uop stream.
        // Two distinct populations, counted separately:
        //  - EXPECTED rep-string traffic (bbl->repInstrs > 0): rep movs/stos
        //    iteration counts are dynamic, so the decoder intentionally leaves
        //    their per-iteration accesses to this drain (block-copy cost model:
        //    serial L1-latency chain) -> repDrained{Loads,Stores}.
        //  - true decode/runtime divergences (approximated instructions that
        //    still touched memory) -> memMismatch{Loads,Stores}.
        // Either way the accesses go through the cache so DRAM/NoC traffic
        // stays accounted; charge at lastCommit.
        bool repBbl = (bbl->repInstrs > 0);
        uint64_t drainCycle = MAX(lastCommitCycle, curCycle);
        while (loadIdx < loads) {
            Address a = loadAddrs[loadIdx++];
            if (repBbl) repDrainedLoads++; else memMismatchLoads++;
            if (a != (Address)-1L) {
                uint64_t r = l1d->load(a, drainCycle) + L1D_LAT;
                cRec.record(curCycle, drainCycle, r);
                lastCommitCycle = MAX(lastCommitCycle, r);
            }
        }
        while (storeIdx < stores) {
            Address a = storeAddrs[storeIdx++];
            if (repBbl) repDrainedStores++; else memMismatchStores++;
            if (a != (Address)-1L) {
                uint64_t r = l1d->store(a, drainCycle) + L1D_LAT;
                cRec.record(curCycle, drainCycle, r);
                lastCommitCycle = MAX(lastCommitCycle, r);
            }
        }
        decodedBbls++;

    } else {
        /* Synthetic BBL (trace/QEMU path): no decoded uops available.
         * Process buffered loads/stores through the cache hierarchy directly,
         * and model execution as a simple 1-CPI pipeline + memory latency. */
        uint64_t commitCycle = curCycle;
        uint32_t cappedLoads = MIN(loads, (uint32_t)256);
        uint32_t cappedStores = MIN(stores, (uint32_t)256);

        for (uint32_t i = 0; i < cappedLoads; i++) {
            Address addr = loadAddrs[i];
            if (addr != (Address)-1L) {
                uint64_t respCycle = l1d->load(addr, commitCycle) + L1D_LAT;
                cRec.record(curCycle, commitCycle, respCycle);
                commitCycle = MAX(commitCycle, respCycle);
            }
        }
        for (uint32_t i = 0; i < cappedStores; i++) {
            Address addr = storeAddrs[i];
            uint64_t respCycle = l1d->store(addr, commitCycle) + L1D_LAT;
            cRec.record(curCycle, commitCycle, respCycle);
            commitCycle = MAX(commitCycle, respCycle);
        }

        // Advance by at least 1 cycle per instruction
        lastCommitCycle = MAX(commitCycle, curCycle + bblInstrs);

        // ADVANCE THE CORE CLOCK. The decoded-uop path advances curCycle inside
        // its scheduling loop; this synthetic path computed lastCommitCycle but
        // never applied it, so under QEMU (no decoded uops) curCycle stayed
        // frozen at ~0 and the core reported "cycles: 0" with nonzero instrs.
        // Applying it implements the stated 1-CPI + memory-latency model.
        curCycle = lastCommitCycle;
        syntheticBbls++;
        syntheticInstrs += bblInstrs;   // 1.9.33: these are injected CYCLES, not instructions
    }

    instrs += bblInstrs;
        { /* 1.11.8 PG residency. 1.11.18: an INJECTED timing charge is a
           * WAIT (barrier/flush/launch), not retirement -- marking it
           * active credited the very windows power gating exists for as
           * busy, so a co-sim PE could never show idle residency.
           * 1.11.51 (L207): classify by the block being RETIRED. */
          if (!retiredBbl->synth) { uint64_t _ph = zinfo->numPhases;
              pgAct.touch(_ph); zinfo->pgres.anyCore.touch(_ph); } }
    mixAdd(retiredBbl);  // 1.11.10 measured mix; 1.11.51 (L207): the RETIRED block
    /* 1.11.47 (L203): soft-float on an FPU-less OOO element -- the same
     * charge simple/in-order/alu cores have carried since 1.11.11. Applied at
     * retire alongside the mix, as a serialising penalty: emulated FP is a
     * library call chain the OOO window cannot hide. (L207's known off-by-one
     * BBL attribution applies to this the same way it applies to the mix --
     * one finding, not two.) */
    if (!coreHasFpu_ && coreFpEmulCycles_ && retiredBbl->nFp) {
        curCycle += (uint64_t)retiredBbl->nFp * coreFpEmulCycles_;
    }
    uops += bbl->uops;
    bbls++;
    approxInstrs += bbl->approxInstrs;

    // Env-gated visibility into the decoded OOO path (proves the improvement is
    // decode-driven, not a constant): dump the first few decoded BBLs' uop mix
    // and the cycles they advanced.
    if (unlikely(oooDebug) && bbl->uops > 0 && oooDebugBbls < 40) {
        oooDebugBbls++;
        info("[ooo-dbg %s] bbl@0x%lx instrs=%u uops=%u approx=%u loads=%u stores=%u "
             "curCycle=%lu", name.c_str(), (unsigned long)bblAddr, bblInstrs,
             bbl->uops, bbl->approxInstrs, loads, stores, (unsigned long)curCycle);
    }

#ifdef BBL_PROFILING
    if (approxInstrs) Decoder::profileBbl(bbl->bblIdx);
#endif

    loads = stores = 0;


    /* Simulate frontend for branch pred + fetch of this BBL
     *
     * NOTE: We assume that the instruction length predecoder and the IQ are
     * weak enough that they can't hide any ifetch or bpred stalls. In fact,
     * predecoder stalls are incorporated in the decode stall component (see
     * decoder.cpp). So here, we compute fetchCycle, then use it to adjust
     * decodeCycle.
     */

    // Model fetch-decode delay (fixed, weak predec/IQ assumption)
    uint64_t fetchCycle = decodeCycle - (DECODE_STAGE - FETCH_STAGE);
    uint32_t lineSize = 1 << lineBits;

    // Simulate branch prediction
    /* 1.9.33: count the branch itself, not only the misses. branchPc != 0 is
     * exactly the core's own test for "this BBL ended in a branch", so this
     * adds a measured total without touching the predictor call or its
     * ordering -- predict() is still invoked identically. */
    if (branchPc) branches++;
    if (branchPc && !branchPred.predict(branchPc, branchTaken)) {
        mispredBranches++;

        /* Simulate wrong-path fetches
         *
         * This is not for a latency reason, but sometimes it increases fetched
         * code footprint and L1I MPKI significantly. Also, we assume a perfect
         * BTB here: we always have the right address to missfetch on, and we
         * never need resteering.
         *
         * NOTE: Resteering due to BTB misses is done at the BAC unit, is
         * relatively rare, and carries an 8-cycle penalty, which should be
         * partially hidden if the branch is predicted correctly --- so we
         * don't simulate it.
         *
         * Since we don't have a BTB, we just assume the next branch is not
         * taken. With a typical branch mispred penalty of 17 cycles, we
         * typically fetch 3-4 lines in advance (16B/cycle). This sets a higher
         * limit, which can happen with branches that take a long time to
         * resolve (because e.g., they depend on a load). To set this upper
         * bound, assume a completely backpressured IQ (18 instrs), uop queue
         * (28 uops), IW (36 uops), and 16B instr length predecoder buffer. At
         * ~3.5 bytes/instr, 1.2 uops/instr, this is about 5 64-byte lines.
         */

        // info("Mispredicted branch, %ld %ld %ld | %ld %ld", decodeCycle, curCycle, lastCommitCycle,
        //         lastCommitCycle-decodeCycle, lastCommitCycle-curCycle);
        Address wrongPathAddr = branchTaken? branchNotTakenNpc : branchTakenNpc;
        uint64_t reqCycle = fetchCycle;
        for (uint32_t i = 0; i < 5*64/lineSize; i++) {
            uint64_t fetchLat = l1i->load(wrongPathAddr + lineSize*i, curCycle) - curCycle;
            cRec.record(curCycle, curCycle, curCycle + fetchLat);
            uint64_t respCycle = reqCycle + fetchLat;
            if (respCycle > lastCommitCycle) {
                break;
            }
            // Model fetch throughput limit
            reqCycle = respCycle + lineSize/FETCH_BYTES_PER_CYCLE;
        }

        fetchCycle = lastCommitCycle;
    }
    branchPc = 0;  // clear for next BBL

    // Indirect jmp/call/ret target misprediction (BTB/RAS miss, flagged by
    // ctrlFlow): charge the same front-end redirect a conditional mispredict
    // pays -- the next fetch cannot start until the branch resolves. Wrong-path
    // ifetches are NOT simulated for indirects (no plausible wrong-path stream
    // worth modeling for a first-target BTB).
    if (indirMispredPending) {
        indirMispredPending = false;
        fetchCycle = MAX(fetchCycle, lastCommitCycle);
    }

    // Simulate current bbl ifetch
    Address endAddr = bblAddr + bblInfo->bytes;
    for (Address fetchAddr = bblAddr; fetchAddr < endAddr; fetchAddr += lineSize) {
        // The Nehalem frontend fetches instructions in 16-byte-wide accesses.
        // Do not model fetch throughput limit here, decoder-generated stalls already include it
        // We always call fetches with curCycle to avoid upsetting the weave
        // models (but we could move to a fetch-centric recorder to avoid this)
        uint64_t fetchLat = l1i->load(fetchAddr, curCycle) - curCycle;
        cRec.record(curCycle, curCycle, curCycle + fetchLat);
        fetchCycle += fetchLat;
    }

    // If fetch rules, take into account delay between fetch and decode;
    // If decode rules, different BBLs make the decoders skip a cycle
    decodeCycle++;
    uint64_t minFetchDecCycle = fetchCycle + (DECODE_STAGE - FETCH_STAGE);
    if (minFetchDecCycle > decodeCycle) {
#ifdef OOO_STALL_STATS
        profFetchStalls.inc(decodeCycle - minFetchDecCycle);
#endif
        decodeCycle = minFetchDecCycle;
    }
}

// Timing simulation code
void OOOCore::join() {
    DEBUG_MSG("[%s] Joining, curCycle %ld phaseEnd %ld", name.c_str(), curCycle, phaseEndCycle);
    uint64_t targetCycle = cRec.notifyJoin(curCycle);
    if (targetCycle > curCycle) {
        // 1.9.2: window-safe bulk clock advance under thread-MPI rendezvous.
        // The parked-join path used to raw-jump curCycle=targetCycle assuming
        // an "empty pipeline". That assumption is false at the 1.6.3 weave-
        // quantum boundary: bfs's per-frontier-level collectives park the core
        // while long-latency NoC loads still sit in insWindow's unbounded
        // window (ubWin). A raw jump of 100K-1M+ cycles orphans those entries
        // behind curCycle; the next advancePos rebase then computes a negative
        // nextWinPos and trips ooo_core.h:226. longAdvance() drains the window
        // (retiring the in-flight uops) before jumping, and is byte-identical
        // to the old raw jump when the window is genuinely empty (occupancy==0).
        // Draining is bounded by the 1024-cycle window horizon, not the delta.
        if (pimidCommParked) insWindow.longAdvance(curCycle, targetCycle);
        else advance(targetCycle);
    }
    phaseEndCycle = zinfo->globPhaseCycles + zinfo->phaseLength;
    // assert(targetCycle <= phaseEndCycle);
    DEBUG_MSG("[%s] Joined, curCycle %ld phaseEnd %ld", name.c_str(), curCycle, phaseEndCycle);
}

void OOOCore::leave() {
    DEBUG_MSG("[%s] Leaving, curCycle %ld phaseEnd %ld", name.c_str(), curCycle, phaseEndCycle);
    cRec.notifyLeave(curCycle);
}

void OOOCore::cSimStart() {
    uint64_t targetCycle = cRec.cSimStart(curCycle);
    assert(targetCycle >= curCycle);
    if (targetCycle > curCycle) advance(targetCycle);
}

void OOOCore::cSimEnd() {
    uint64_t targetCycle = cRec.cSimEnd(curCycle);
    assert(targetCycle >= curCycle);
    if (targetCycle > curCycle) advance(targetCycle);
}

void OOOCore::advance(uint64_t targetCycle) {
    assert(targetCycle > curCycle);
    decodeCycle += targetCycle - curCycle;
    insWindow.longAdvance(curCycle, targetCycle);
    curCycleRFReads = 0;
    curCycleIssuedUops = 0;
    assert(targetCycle == curCycle);
    /* NOTE: Validation with weave mems shows that not advancing internal cycle
     * counters in e.g., the ROB does not change much; consider full-blown
     * rebases though if weave models fail to validate for some app.
     */
}

// Pin interface code

void OOOCore::LoadFunc(THREADID tid, ADDRINT addr) {static_cast<OOOCore*>(cores[tid])->load(addr);}
void OOOCore::StoreFunc(THREADID tid, ADDRINT addr) {static_cast<OOOCore*>(cores[tid])->store(addr);}

void OOOCore::PredLoadFunc(THREADID tid, ADDRINT addr, BOOL pred) {
    OOOCore* core = static_cast<OOOCore*>(cores[tid]);
    if (pred) core->load(addr);
    else core->predFalseLoad();
}

void OOOCore::PredStoreFunc(THREADID tid, ADDRINT addr, BOOL pred) {
    OOOCore* core = static_cast<OOOCore*>(cores[tid]);
    if (pred) core->store(addr);
    else core->predFalseStore();
}

void OOOCore::BblFunc(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo) {
    OOOCore* core = static_cast<OOOCore*>(cores[tid]);
    core->bbl(bblAddr, bblInfo);

    while (core->curCycle > core->phaseEndCycle) {
        core->phaseEndCycle += zinfo->phaseLength;

        uint32_t cid = getCid(tid);
        // NOTE: TakeBarrier may take ownership of the core, and so it will be used by some other thread. If TakeBarrier context-switches us,
        // the *only* safe option is to return inmmediately after we detect this, or we can race and corrupt core state. However, the information
        // here is insufficient to do that, so we could wind up double-counting phases.
        uint32_t newCid = TakeBarrier(tid, cid);
        // NOTE: Upon further observation, we cannot race if newCid == cid, so this code should be enough.
        // It may happen that we had an intervening context-switch and we are now back to the same core.
        // This is fine, since the loop looks at core values directly and there are no locals involved,
        // so we should just advance as needed and move on.
        if (newCid != cid) break;  /*context-switch, we do not own this context anymore*/
    }
}

/* Indirect control-flow resolution (kind >= CF_IND_JMP, see CtrlFlowKind).
 * target = the resolved actual target (next TB start); retAddr = the call's
 * fall-through address (calls only). A wrong prediction arms the same
 * front-end redirect a conditional mispredict pays, consumed in bbl(). */
void OOOCore::ctrlFlow(uint32_t kind, Address pc, Address target, Address retAddr) {
    switch (kind) {
        case CF_DIR_CALL:
            indirPred.push(retAddr);  /* direct call: target always predicted */
            break;
        case CF_IND_CALL:
            indirBranches++;
            if (!indirPred.indirect(pc, target)) { indirMispreds++; indirMispredPending = true; }
            indirPred.push(retAddr);
            break;
        case CF_IND_JMP:
            indirBranches++;
            if (!indirPred.indirect(pc, target)) { indirMispreds++; indirMispredPending = true; }
            break;
        case CF_RET:
            rasReturns++;
            if (!indirPred.ret(target)) { rasMispreds++; indirMispredPending = true; }
            break;
        default: break;
    }
}

void OOOCore::BranchFunc(THREADID tid, ADDRINT pc, BOOL taken, ADDRINT takenNpc, ADDRINT notTakenNpc) {
    OOOCore* core = static_cast<OOOCore*>(cores[tid]);
    // taken <= 1: classic conditional direction feed. taken >= 2: CtrlFlowKind
    // for indirect jmp/call/ret and direct-call RAS pushes (takenNpc carries
    // the resolved target, notTakenNpc the call fall-through address).
    if (taken <= CF_COND_T) core->branch(pc, taken, takenNpc, notTakenNpc);
    else core->ctrlFlow((uint32_t)taken, pc, takenNpc, notTakenNpc);
}

