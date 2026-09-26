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

#include "in_order_core.h"
#include <cstdlib>
#include "bithacks.h"
#include "decoder_simple.h"
#include "filter_cache.h"
#include "zsim.h"

#define DEBUG_MSG(args...)
//#define DEBUG_MSG(args...) info(args)

InOrderCore::InOrderCore(FilterCache* _l1i, FilterCache* _l1d, uint32_t _domain, g_string& _name,
                         uint32_t _issueWidth)
    : Core(_name), l1i(_l1i), l1d(_l1d), instrs(0), uops(0), bbls(0),
      curCycle(0), cRec(_domain, _name) {
    /* 1.11.44 (user ruling): the legacy IPC=1 NODECODE path is DELETED. Its
     * A/B purpose ended with the 1.4.x validation; releases are the baseline
     * now, and the path bred audit findings (E25: a year of structurally
     * inert mix/FP code). One core, one timing model. */
    // In-order superscalar issue width. Precedence (highest wins):
    //   1. PIMID_INORDER_WIDTH env var  (experimentation override)
    //   2. _issueWidth ctor arg         (YAML pim.pe.issue_width -> ZSim issueWidth)
    //   3. default 2 (dual-issue)
    // Clamped to [1, NUM_PORTS]; out-of-range values fall back to the default.
    issueWidth = (_issueWidth >= 1 && _issueWidth <= NUM_PORTS) ? _issueWidth : 2;
    const char* w = getenv("PIMID_INORDER_WIDTH");
    if (w) {
        int v = atoi(w);
        if (v >= 1 && v <= (int)NUM_PORTS) issueWidth = (uint32_t)v;
    }

    prevBbl = nullptr;
    loads = stores = 0;
    for (uint32_t i = 0; i < MAX_REGISTERS; i++) regScoreboard[i] = 0;
    for (uint32_t i = 0; i < NUM_PORTS; i++) portFreeCycle[i] = 0;
    memRespCycle = 0;
    slotsUsed = 0;
    slotCycle = 0;
    maxOutstanding = 0;

    decodedBbls = syntheticBbls = depStalls = issueStalls = 0;
    memMismatchLoads = memMismatchStores = 0;
    repDrainedLoads = repDrainedStores = 0;
    phaseEndCycle = 0;

    // Branch misprediction: front-end flush/refill bubble. Default 7 cycles ~=
    // the OOO model's fetch-to-issue depth (ISSUE_STAGE), i.e. the redirect
    // cost of a short in-order pipeline (Cortex-A53 class is ~8 cycles).
    // Overridable via PIMID_INORDER_MISPRED_PENALTY for sensitivity studies.
    // The whole feed is disabled by PIMID_INORDER_NOBRANCH=1 (plugin-side gate).
    branchPc = 0;
    branchTaken = false;
    mispredPenalty = 7;
    const char* mp = getenv("PIMID_INORDER_MISPRED_PENALTY");
    if (mp) {
        int v = atoi(mp);
        if (v >= 0 && v <= 1000) mispredPenalty = (uint32_t)v;
    }
    branches = mispredBranches = mispredStallCycles = 0;

    // Indirect control flow (BTB + RAS; IndirectPredictor default-constructs).
    indirMispredPend = false;
    indirBranches = indirMispreds = rasReturns = rasMispreds = 0;
}

uint64_t InOrderCore::getPhaseCycles() const {
    return curCycle % zinfo->phaseLength;
}

void InOrderCore::initStats(AggregateStat* parentStat) {
    AggregateStat* coreStat = new AggregateStat();
    coreStat->init(name.c_str(), "Core stats");
    // 1.11.18: pgActivePhases now emitted (ROI-relative) by mixInitStats.
    mixInitStats(coreStat);   // 1.11.10 measured instruction mix

    // Report cycles/instrs RELATIVE to the ROI baseline (roi_begin); roiBase* are
    // 0 until roi_begin, so non-ROI workloads are unaffected. cCycles (contention)
    // is left absolute as it is a diagnostic counter, not an ROI duration metric.
    auto x = [this]() {
        uint64_t c = cRec.getUnhaltedCycles(curCycle);
        c = (c > pimidPhantomWait) ? (c - pimidPhantomWait) : 0;   // 1.6.1 wall-free
        return (c > roiBaseCycle) ? (c - roiBaseCycle) : 0;
    };
    LambdaStat<decltype(x)>* cyclesStat = new LambdaStat<decltype(x)>(x);
    cyclesStat->init("cycles", "Simulated unhalted cycles");
    coreStat->append(cyclesStat);

    /* 1.11.17 (audit go-through): ROI-rebased like the OOO core (1.11.9
     * blocker 3) -- this copy still reported absolute contention cycles
     * against ROI-windowed cycles/instrs. */
    auto y = [this]() {
        uint64_t c = cRec.getContentionCycles();
        return (c > roiBaseCCycles) ? (c - roiBaseCCycles) : 0;
    };
    LambdaStat<decltype(y)>* cCyclesStat = new LambdaStat<decltype(y)>(y);
    cCyclesStat->init("cCycles", "Cycles due to contention stalls");
    coreStat->append(cCyclesStat);

    auto z = [this]() -> uint64_t { return instrs - roiBaseInstrs; };
    LambdaStat<decltype(z)>* instrsStat = new LambdaStat<decltype(z)>(z);
    instrsStat->init("instrs", "Simulated instructions");
    coreStat->append(instrsStat);

    // In-order pipeline diagnostics (analogous to the OOO core).
    /* 1.9.33: ROI-windowed, matching instrs above. */
    auto zu = [this]() -> uint64_t { return uops - roiBaseUops; };
    LambdaStat<decltype(zu)>* uopsStat = new LambdaStat<decltype(zu)>(zu);
    uopsStat->init("uops", "Retired micro-ops");
    coreStat->append(uopsStat);
    auto zb = [this]() -> uint64_t { return bbls - roiBaseBbls; };
    LambdaStat<decltype(zb)>* bblsStat = new LambdaStat<decltype(zb)>(zb);
    bblsStat->init("bbls", "Basic blocks");
    coreStat->append(bblsStat);
    ProxyStat* decodedBblsStat = new ProxyStat();
    decodedBblsStat->init("decodedBbls", "BBLs run through the decoded in-order scoreboard [whole run]", &decodedBbls);
    coreStat->append(decodedBblsStat);
    ProxyStat* syntheticBblsStat = new ProxyStat();
    syntheticBblsStat->init("syntheticBbls", "BBLs run through the synthetic 1-CPI fallback [whole run]", &syntheticBbls);
    coreStat->append(syntheticBblsStat);
    ProxyStat* depStallsStat = new ProxyStat();
    depStallsStat->init("depStalls", "Cycles lost to RAW dependency stalls [whole run]", &depStalls);
    coreStat->append(depStallsStat);
    ProxyStat* issueStallsStat = new ProxyStat();
    issueStallsStat->init("issueStalls", "Cycles lost to issue-width/port stalls [whole run]", &issueStalls);
    coreStat->append(issueStallsStat);
    ProxyStat* memMismatchLoadsStat = new ProxyStat();
    memMismatchLoadsStat->init("memMismatchLoads", "Decoded/runtime load-count divergences drained [whole run]", &memMismatchLoads);
    coreStat->append(memMismatchLoadsStat);
    ProxyStat* memMismatchStoresStat = new ProxyStat();
    memMismatchStoresStat->init("memMismatchStores", "Decoded/runtime store-count divergences drained [whole run]", &memMismatchStores);
    coreStat->append(memMismatchStoresStat);
    ProxyStat* repDrainedLoadsStat = new ProxyStat();
    repDrainedLoadsStat->init("repDrainedLoads", "Expected rep-string loads drained (block-copy model) [whole run]", &repDrainedLoads);
    coreStat->append(repDrainedLoadsStat);
    ProxyStat* repDrainedStoresStat = new ProxyStat();
    repDrainedStoresStat->init("repDrainedStores", "Expected rep-string stores drained (block-copy model) [whole run]", &repDrainedStores);
    coreStat->append(repDrainedStoresStat);
    auto zbr = [this]() -> uint64_t { return branches - roiBaseBranches; };
    LambdaStat<decltype(zbr)>* branchesStat = new LambdaStat<decltype(zbr)>(zbr);
    branchesStat->init("branches", "Resolved conditional branches fed to the predictor");
    coreStat->append(branchesStat);
    auto zm = [this]() -> uint64_t { return mispredBranches - roiBaseMispred; };
    LambdaStat<decltype(zm)>* mispredBranchesStat = new LambdaStat<decltype(zm)>(zm);
    mispredBranchesStat->init("mispredBranches", "Mispredicted branches");
    coreStat->append(mispredBranchesStat);
    ProxyStat* mispredStallCyclesStat = new ProxyStat();
    mispredStallCyclesStat->init("mispredStallCycles", "Cycles charged for mispredict flush/refill bubbles [whole run]", &mispredStallCycles);
    coreStat->append(mispredStallCyclesStat);
    ProxyStat* indirBranchesStat = new ProxyStat();
    indirBranchesStat->init("indirBranches", "Indirect jmp/call resolutions fed to the BTB [whole run]", &indirBranches);
    coreStat->append(indirBranchesStat);
    ProxyStat* indirMispredsStat = new ProxyStat();
    indirMispredsStat->init("indirMispreds", "Indirect jmp/call target mispredictions [whole run]", &indirMispreds);
    coreStat->append(indirMispredsStat);
    ProxyStat* rasReturnsStat = new ProxyStat();
    rasReturnsStat->init("rasReturns", "Returns resolved against the RAS [whole run]", &rasReturns);
    coreStat->append(rasReturnsStat);
    ProxyStat* rasMispredsStat = new ProxyStat();
    rasMispredsStat->init("rasMispreds", "Return-target mispredictions (RAS miss) [whole run]", &rasMispreds);
    coreStat->append(rasMispredsStat);
    /* 1.11.93 (F6): ROI-windowed twins of indirBranches/rasReturns -- the
     * BTB and RAS activity the power model prices. */
    auto zib = [this]() -> uint64_t { return indirBranches - roiBaseIndir; };
    LambdaStat<decltype(zib)>* roiIndirStat = new LambdaStat<decltype(zib)>(zib);
    roiIndirStat->init("roiIndirBranches", "Indirect jmp/call resolutions fed to the BTB (ROI)");
    coreStat->append(roiIndirStat);
    auto zrr = [this]() -> uint64_t { return rasReturns - roiBaseRas; };
    LambdaStat<decltype(zrr)>* roiRasStat = new LambdaStat<decltype(zrr)>(zrr);
    roiRasStat->init("roiRasReturns", "Returns resolved against the RAS (ROI)");
    coreStat->append(roiRasStat);

    parentStat->append(coreStat);
}


void InOrderCore::contextSwitch(int32_t gid) {
    if (gid == -1) {
        // Do not simulate the lingering previous BBL across a context switch.
        prevBbl = nullptr;
        loads = stores = 0;
        branchPc = 0;
        indirMispredPend = false;
        drainPipeline();  // scheduler boundary: no overlap across a switch
        l1i->contextSwitch();
        l1d->contextSwitch();
    }
}

void InOrderCore::join() {
    DEBUG_MSG("[%s] Joining, curCycle %ld phaseEnd %ld", name.c_str(), curCycle, phaseEndCycle);
    drainPipeline();  // scheduler boundary: outstanding completions charge here
    curCycle = cRec.notifyJoin(curCycle);
    phaseEndCycle = zinfo->globPhaseCycles + zinfo->phaseLength;
    DEBUG_MSG("[%s] Joined, curCycle %ld phaseEnd %ld", name.c_str(), curCycle, phaseEndCycle);
}

void InOrderCore::leave() {
    drainPipeline();  // taper must cover all outstanding completions
    cRec.notifyLeave(curCycle);
}

/* ---- Load/store address capture for the deferred decoded path ---- */

void InOrderCore::loadAndRecord(Address addr) {
    if (loads < 256) loadAddrs[loads] = addr;
    loads++;
}

void InOrderCore::storeAndRecord(Address addr) {
    if (stores < 256) storeAddrs[stores] = addr;
    stores++;
}

/* ---- Instruction fetch of the current BBL ---- */

inline void InOrderCore::ifetch(Address bblAddr, BblInfo* bblInfo) {
    Address endBblAddr = bblAddr + bblInfo->bytes;
    for (Address fetchAddr = bblAddr; fetchAddr < endBblAddr; fetchAddr += (1 << lineBits)) {
        // Recorder-safe issue point: the serialized CoreRecorder chain requires
        // each RECORDED access to start at/after the previous response, so the
        // cache call is made at MAX(curCycle, memRespCycle). The FRONT-END,
        // however, pays only the fetch LATENCY from its own clock (the I-port
        // is separate hardware; an L1I hit must not re-serialize the front-end
        // behind an outstanding trailing data miss -- same practice as the OOO
        // core's "always call fetches with curCycle" note).
        uint64_t startCycle = MAX(curCycle, memRespCycle);
        uint64_t resp = l1i->load(fetchAddr, startCycle);
        cRec.record(startCycle);
        memRespCycle = resp;
        uint64_t fetchLat = resp - startCycle;
        curCycle += fetchLat;  // fetch latency stalls the in-order front-end
        if (curCycle > maxOutstanding) maxOutstanding = curCycle;
    }
}

/* ---- Synthetic 1-CPI fallback (no decoded uops for this BBL) ---- */

inline void InOrderCore::simulateSyntheticBbl(BblInfo* bblInfo) {
    uint64_t commitCycle = curCycle + bblInfo->instrs;  // 1-CPI compute
    for (uint32_t i = 0; i < loads && i < 256; i++) {
        Address addr = loadAddrs[i];
        if (addr == (Address)-1L) continue;
        uint64_t startCycle = MAX(commitCycle, memRespCycle);
        uint64_t resp = l1d->load(addr, startCycle);
        cRec.record(startCycle);
        memRespCycle = resp;
        commitCycle = resp;
    }
    for (uint32_t i = 0; i < stores && i < 256; i++) {
        Address addr = storeAddrs[i];
        if (addr == (Address)-1L) continue;
        uint64_t startCycle = MAX(commitCycle, memRespCycle);
        uint64_t resp = l1d->store(addr, startCycle);
        cRec.record(startCycle);
        memRespCycle = resp;
        commitCycle = resp;
    }
    if (commitCycle > curCycle) curCycle = commitCycle;
    syntheticBbls++;
}

/* ---- Real in-order scoreboard simulation of one BBL ---- */

inline void InOrderCore::simulateDecodedBbl(BblInfo* bblInfo) {
    DynBbl* db = &(bblInfo->oooBbl[0]);
    uint32_t nUops = db->uops;
    uint32_t loadIdx = 0, storeIdx = 0;
    uint64_t lastDone = curCycle;

    regScoreboard[0] = 0;  // SB_NONE is always ready

    for (uint32_t i = 0; i < nUops; i++) {
        DynUop* uop = &(db->uop[i]);

        // --- RAW hazard: sources must be ready before issue (in-order stall) ---
        uint64_t srcReady = MAX(regScoreboard[uop->rs[0]], regScoreboard[uop->rs[1]]);
        uint64_t iss = curCycle;
        if (srcReady > iss) { depStalls += (srcReady - iss); iss = srcReady; }

        // --- Issue width: at most issueWidth independent uops per cycle.
        // slotsUsed counts uops issued at cycle slotCycle; slotCycle carries
        // across BBL boundaries so cross-BBL same-cycle issue is limited too. ---
        if (iss == slotCycle && slotsUsed >= issueWidth) { iss++; issueStalls++; }

        // --- Functional-unit port contention (in-order: whole front-end waits) ---
        uint8_t mask = uop->portMask ? uop->portMask : 0x01;
        uint64_t bestFree = (uint64_t)-1L; int bestPort = -1;
        for (uint32_t p = 0; p < NUM_PORTS; p++) {
            if (mask & (1u << p)) {
                if (portFreeCycle[p] < bestFree) { bestFree = portFreeCycle[p]; bestPort = p; }
            }
        }
        if (bestPort < 0) { bestPort = 0; bestFree = portFreeCycle[0]; }
        if (bestFree > iss) { issueStalls += (bestFree - iss); iss = bestFree; }

        // Commit the issue at cycle `iss`.
        if (iss != slotCycle) { slotCycle = iss; slotsUsed = 0; }
        portFreeCycle[bestPort] = iss + 1;  // 1 uop/cycle throughput on this port
        slotsUsed++;
        curCycle = iss;  // in-order issue cursor advances monotonically

        // --- Execute / complete ---
        uint64_t done;
        if (uop->type == UOP_LOAD) {
            Address addr;
            if (loadIdx < loads) addr = loadAddrs[loadIdx++];
            else { addr = (Address)-1L; memMismatchLoads++; }
            if (addr != (Address)-1L) {
                uint64_t startCycle = MAX(iss, memRespCycle);
                uint64_t resp = l1d->load(addr, startCycle);
                cRec.record(startCycle);
                memRespCycle = resp;
                done = resp;  // load-use latency gates the destination register
            } else {
                done = iss;   // predicated / mismatched -> 0-cycle
            }
        } else if (uop->type == UOP_STORE) {
            Address addr;
            if (storeIdx < stores) addr = storeAddrs[storeIdx++];
            else { addr = (Address)-1L; memMismatchStores++; }
            if (addr != (Address)-1L) {
                uint64_t startCycle = MAX(iss, memRespCycle);
                uint64_t resp = l1d->store(addr, startCycle);
                cRec.record(startCycle);
                memRespCycle = resp;
                done = resp;
            } else {
                done = iss;
            }
        } else {
            // UOP_GENERAL / UOP_STORE_ADDR / UOP_FENCE: fixed FU latency.
            done = iss + uop->lat;
        }

        // --- Scoreboard: destination registers become ready at `done` ---
        if (uop->rd[0]) regScoreboard[uop->rd[0]] = done;
        if (uop->rd[1]) regScoreboard[uop->rd[1]] = done;
        if (done > lastDone) lastDone = done;
        if (done > maxOutstanding) maxOutstanding = done;
    }

    // Drain any runtime memory accesses not consumed by the decoded uop stream.
    // Two populations, counted separately (same convention as the OOO core):
    //  - EXPECTED rep-string traffic (db->repInstrs > 0): rep movs/stos have
    //    dynamic iteration counts, so their per-iteration accesses are left to
    //    this serial drain (block-copy cost model) -> repDrained{Loads,Stores};
    //  - true decode/runtime divergences -> memMismatch{Loads,Stores}.
    // Either way they go through the cache so DRAM/NoC traffic stays accounted.
    bool repBbl = (db->repInstrs > 0);
    while (loadIdx < loads) {
        Address a = loadAddrs[loadIdx++];
        if (repBbl) repDrainedLoads++; else memMismatchLoads++;
        if (a != (Address)-1L) {
            uint64_t startCycle = MAX(lastDone, memRespCycle);
            uint64_t resp = l1d->load(a, startCycle);
            cRec.record(startCycle);
            memRespCycle = resp;
            if (resp > lastDone) lastDone = resp;
        }
    }
    while (storeIdx < stores) {
        Address a = storeAddrs[storeIdx++];
        if (repBbl) repDrainedStores++; else memMismatchStores++;
        if (a != (Address)-1L) {
            uint64_t startCycle = MAX(lastDone, memRespCycle);
            uint64_t resp = l1d->store(a, startCycle);
            cRec.record(startCycle);
            memRespCycle = resp;
            if (resp > lastDone) lastDone = resp;
        }
    }

    // NO end-of-BBL drain: the scoreboard (register ready cycles, port free
    // cycles, issue slot cursor) carries across BBL boundaries, so a trailing
    // load overlaps with independent work in the next block -- still strictly
    // in program order (only true dependencies and issue/port limits stall).
    // Drained accesses (rep/mismatch) fold into maxOutstanding so flush and
    // scheduler-boundary drains still cover them.
    if (lastDone > maxOutstanding) maxOutstanding = lastDone;
    uops += nUops;
    decodedBbls++;
}

/* ---- BBL entry point ---- */

void InOrderCore::bblAndRecord(Address bblAddr, BblInfo* bblInfo) {

    // Deferred decoded path: simulate the PREVIOUS BBL now that its memory
    // addresses have all been buffered (mem_cb fires between BBL callbacks).
    if (!prevBbl) {
        prevBbl = bblInfo;
        loads = stores = 0;
        branchPc = 0;  // any pending branch belongs to a BBL we never simulated
        indirMispredPend = false;
        return;
    }

    BblInfo* sim = prevBbl;
    prevBbl = bblInfo;

    instrs += sim->instrs;
    /* 1.11.8 PG residency; 1.11.18: the SIMULATED block decides (deferred
     * path), and an injected charge is a wait, not retirement. */
    if (!sim->synth) { uint64_t _ph = zinfo->numPhases;
        pgAct.touch(_ph); zinfo->pgres.anyCore.touch(_ph); }
    /* 1.11.16 (verification audit): count the mix and charge soft-float for
     * the BBL being SIMULATED (sim = the deferred previous BBL), not the one
     * just arriving -- instrs and the census were offset by one basic block
     * (the exact counter pair the deficit guard compares), and the FP charge
     * landed for a block whose execution had not been simulated yet. */
    mixAdd(sim);  // 1.11.10 measured instruction mix
    if (!coreHasFpu_ && coreFpEmulCycles_ &&
        sim->nFp) {   // 1.11.11 (#113): soft-float on an FPU-less element
        curCycle += (uint64_t)sim->nFp * coreFpEmulCycles_;
    }
    bbls++;

    if (sim->oooBbl[0].uops > 0) {
        simulateDecodedBbl(sim);
    } else {
        simulateSyntheticBbl(sim);
    }

    // Branch misprediction: the plugin delivered (right before this bblPtr) the
    // resolved direction of the conditional branch TERMINATING `sim`. Query and
    // update the predictor; on a mispredict, charge the front-end flush/refill
    // bubble. In-order cores resolve the branch at execute, so the redirect
    // lands after the BBL drains (curCycle is at last completion here).
    if (branchPc) {
        branches++;
        if (!branchPred.predict(branchPc, branchTaken)) {
            mispredBranches++;
            mispredStallCycles += mispredPenalty;
            // A mispredict FLUSHES the front-end: the redirect cannot begin
            // until the outstanding work (incl. whatever resolved the branch)
            // completes, so this boundary DRAINS. Correctly-predicted and
            // fall-through boundaries do not (cross-BBL overlap flows on).
            drainPipeline();
            curCycle += mispredPenalty;
        }
        branchPc = 0;
    }

    // Indirect jmp/call/ret target misprediction (BTB/RAS miss, armed by
    // ctrlFlow for the terminator of `sim`): same flush semantics + bubble as
    // a conditional mispredict.
    if (indirMispredPend) {
        indirMispredPend = false;
        mispredStallCycles += mispredPenalty;
        drainPipeline();
        curCycle += mispredPenalty;
    }

    loads = stores = 0;

    // Fetch the current BBL's instructions (models L1I traffic + stalls).
    ifetch(bblAddr, bblInfo);
}


InstrFuncPtrs InOrderCore::GetFuncPtrs() {
    return {LoadAndRecordFunc, StoreAndRecordFunc, BblAndRecordFunc, BranchFunc, PredLoadAndRecordFunc, PredStoreAndRecordFunc, FPTR_ANALYSIS, {0}};
}

void InOrderCore::LoadAndRecordFunc(THREADID tid, ADDRINT addr) {
    static_cast<InOrderCore*>(cores[tid])->loadAndRecord(addr);
}

void InOrderCore::StoreAndRecordFunc(THREADID tid, ADDRINT addr) {
    static_cast<InOrderCore*>(cores[tid])->storeAndRecord(addr);
}

void InOrderCore::BblAndRecordFunc(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo) {
    InOrderCore* core = static_cast<InOrderCore*>(cores[tid]);
    core->bblAndRecord(bblAddr, bblInfo);

    while (core->curCycle > core->phaseEndCycle) {
        core->phaseEndCycle += zinfo->phaseLength;
        uint32_t cid = getCid(tid);
        uint32_t newCid = TakeBarrier(tid, cid);
        if (newCid != cid) break; /*context-switch*/
    }
}

void InOrderCore::PredLoadAndRecordFunc(THREADID tid, ADDRINT addr, BOOL pred) {
    if (pred) static_cast<InOrderCore*>(cores[tid])->loadAndRecord(addr);
}

void InOrderCore::PredStoreAndRecordFunc(THREADID tid, ADDRINT addr, BOOL pred) {
    if (pred) static_cast<InOrderCore*>(cores[tid])->storeAndRecord(addr);
}

/* ---- Branch feed (plugin calls this right before bblPtr; see plugin gate) ---- */

void InOrderCore::branch(Address pc, bool taken) {
    branchPc = pc;
    branchTaken = taken;
}

/* Indirect control-flow resolution (kind >= CF_IND_JMP, see CtrlFlowKind in
 * ooo_core.h). target = resolved actual target (next TB start); retAddr = the
 * call's fall-through (calls only). Wrong prediction arms the mispredPenalty
 * bubble consumed after the terminator's BBL is simulated. */
void InOrderCore::ctrlFlow(uint32_t kind, Address pc, Address target, Address retAddr) {
    switch (kind) {
        case CF_DIR_CALL:
            indirPred.push(retAddr);  /* direct call: target always predicted */
            break;
        case CF_IND_CALL:
            indirBranches++;
            if (!indirPred.indirect(pc, target)) { indirMispreds++; indirMispredPend = true; }
            indirPred.push(retAddr);
            break;
        case CF_IND_JMP:
            indirBranches++;
            if (!indirPred.indirect(pc, target)) { indirMispreds++; indirMispredPend = true; }
            break;
        case CF_RET:
            rasReturns++;
            if (!indirPred.ret(target)) { rasMispreds++; indirMispredPend = true; }
            break;
        default: break;
    }
}

void InOrderCore::BranchFunc(THREADID tid, ADDRINT pc, BOOL taken, ADDRINT takenNpc, ADDRINT notTakenNpc) {
    InOrderCore* core = static_cast<InOrderCore*>(cores[tid]);
    // taken <= 1: conditional direction feed (wrong-path fetches not modeled).
    // taken >= 2: CtrlFlowKind for indirect jmp/call/ret and direct-call RAS
    // pushes (takenNpc = resolved target, notTakenNpc = call fall-through).
    if (taken <= CF_COND_T) core->branch(pc, taken != 0);
    else core->ctrlFlow((uint32_t)taken, pc, takenNpc, notTakenNpc);
}
