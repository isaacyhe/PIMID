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

#include "alu_core.h"
#include "pe_memory_interface.h"
#include "dram_epoch_replay.h"   // (1.12.189): DMA transfers in the closed-loop replay
#include "zsim.h"
#include "constants.h"
#include <algorithm>
#include <vector>

// 1.8.7 post-ROI protocol-tail receipt, indexed by global core index (srcId_).
// Fixed BSS storage (see core.h) -- deliberately NOT a Core member, so Core's
// object size stays byte-identical and device-scope runs stay bit-identical.
uint64_t g_mpiProtocolTailCyc[MAX_THREADS] = {0};
uint64_t g_mpiRoiCyc[MAX_THREADS] = {0};   // 1.12.3: the kernel-window receipt
bool g_peTailRun[MAX_THREADS] = {false};   // (1.12.181): the PE runs a co-sim MPI rank's post-ROI tail
std::atomic<uint8_t> g_peCutHold[MAX_THREADS];   // (1.12.188): the cut hold (core.h); static zero-init: nothing held

// ALU Core: Pure computational core with optional PE memory interface

ALUCore::ALUCore(g_string& _name, double _computeFactor, double _accessFactor,
                 double _throughputFactor, int _operandWidth, double _energyFactor,
                 PEMemoryInterface* mi, uint32_t srcId, bool _bitSerial)
    : Core(_name), computeFactor(_computeFactor), accessFactor(_accessFactor),
      throughputFactor(std::max(_throughputFactor, 1e-9)),
      operandWidth(_operandWidth), energyFactor(_energyFactor), bitSerial(_bitSerial),
      mi_(mi), srcId_(srcId) {
    instrs = 0;
    curCycle = 0;
    phaseEndCycle = 0;

    info("ALU Core '%s': compute=%.2f, access=%.2f, throughput=%.2f, width=%d, serial=%s, energy=%.2f, mi=%s",
         name.c_str(), computeFactor, accessFactor, throughputFactor, operandWidth,
         bitSerial ? "yes" : "no", energyFactor, mi_ ? "wired" : "none");
}

uint64_t ALUCore::getPhaseCycles() const {
    return curCycle % zinfo->phaseLength;
}

void ALUCore::initStats(AggregateStat* parentStat) {
    AggregateStat* coreStat = new AggregateStat();
    coreStat->init(name.c_str(), "Core stats");
    // 1.11.18: pgActivePhases now emitted (ROI-relative) by mixInitStats.
    mixInitStats(coreStat);   // 1.11.10 measured instruction mix

    // Report cycles/instrs RELATIVE to the ROI baseline (roi_begin), so serial
    // pre-ROI init/setup on the launcher PE is excluded from the kernel metric.
    // (roiBase* are 0 until roi_begin, so non-ROI workloads are unaffected.)
    /* 1.12.18 (user ruling 2026-10-23, "sync to waker"): 'cycles' is BUSY,
     * the PE's own ROI activity without the clock jumps a futex wake synced
     * it by (those are waiting; wakeSyncCycles below). */
    auto x = [this]() -> uint64_t { return roiBusyCycles(); };
    auto cyclesStat = makeLambdaStat(x);
    cyclesStat->init("cycles", "Simulated cycles (busy: this PE's own ROI activity, without wake/lock sync jumps; the run time is roiWallCycles, 1.12.18)");
    coreStat->append(cyclesStat);
    auto xs = [this]() -> uint64_t { return syncSkip_ - roiSyncSkip_; };
    auto skipStat = makeLambdaStat(xs);
    skipStat->init("wakeSyncCycles", "Cycles this PE's clock jumped forward to its waker's release on futex wakes (ROI; waiting, not in cycles)");
    coreStat->append(skipStat);
    auto xn = [this]() -> uint64_t { return wakeSyncs_ - roiWakeSyncs_; };
    auto nStat = makeLambdaStat(xn);
    nStat->init("wakeSyncs", "Futex wakes that moved this PE's clock to its waker's release (ROI)");
    coreStat->append(nStat);

    auto xi = [this]() -> uint64_t { return instrs - roiBaseInstrs; };
    auto instrsStat = makeLambdaStat(xi);
    instrsStat->init("instrs", "Simulated instructions");
    coreStat->append(instrsStat);

    /* 1.12.18: the scratchpad's ROI activity (zero on a PE without one). */
    if (spmCap_) {
        auto sr = [this]() -> uint64_t { return spmReads_ - roiSpmReads_; };
        auto srS = makeLambdaStat(sr); srS->init("spmReads", "Scratchpad word reads (ROI)"); coreStat->append(srS);
        auto sw = [this]() -> uint64_t { return spmWrites_ - roiSpmWrites_; };
        auto swS = makeLambdaStat(sw); swS->init("spmWrites", "Scratchpad word writes (ROI)"); coreStat->append(swS);
        auto dO = [this]() -> uint64_t { return dmaOps_ - roiDmaOps_; };
        auto dOS = makeLambdaStat(dO); dOS->init("dmaOps", "DMA block transfers (ROI)"); coreStat->append(dOS);
        auto dL = [this]() -> uint64_t { return dmaLines_ - roiDmaLines_; };
        auto dLS = makeLambdaStat(dL); dLS->init("dmaLines", "DMA lines moved (ROI)"); coreStat->append(dLS);
        auto dC = [this]() -> uint64_t { return dmaCycles_ - roiDmaCycles_; };
        auto dCS = makeLambdaStat(dC); dCS->init("dmaCycles", "Cycles the PE waited on DMA transfers (ROI)"); coreStat->append(dCS);
        auto cR = [this]() -> uint64_t { return cmdReads_ - roiCmdReads_; };
        auto cRS = makeLambdaStat(cR); cRS->init("dmaCmdReads", "DMA command-register reads (ROI)"); coreStat->append(cRS);
        auto cW = [this]() -> uint64_t { return cmdWrites_ - roiCmdWrites_; };
        auto cWS = makeLambdaStat(cW); cWS->init("dmaCmdWrites", "DMA command-register writes (ROI)"); coreStat->append(cWS);
        if (stkInSpm_) {
        auto kR = [this]() -> uint64_t { return stkReads_ - roiStkReads_; };
        auto kRS = makeLambdaStat(kR); kRS->init("spmStackReads", "Stack word reads in the scratchpad (ROI, in spmReads)"); coreStat->append(kRS);
        auto kW = [this]() -> uint64_t { return stkWrites_ - roiStkWrites_; };
        auto kWS = makeLambdaStat(kW); kWS->init("spmStackWrites", "Stack word writes in the scratchpad (ROI, in spmWrites)"); coreStat->append(kWS);
        }
        auto kB = [this]() -> uint64_t { return stkMaxT_ >= stkMinT_ ? stkMaxT_ + 8 - stkMinT_ : 0; };
        auto kBS = makeLambdaStat(kB); kBS->init("stackSpanBytes", "Own stack span the PE touched (ROI, bytes): what pim.pe.spm_stack_bytes must hold"); coreStat->append(kBS);
    }

    // 1.8.7: explicit post-ROI protocol-tail RECEIPT for co-sim MPI (device PEs
    // are the MPI ranks; the collective tail is device-resident work that stays
    // in 'cycles' -- this line only exposes its magnitude). Read from the fixed
    // global array (indexed by this PE's global core index srcId_), NOT a Core
    // member, to keep Core's object layout byte-identical.
    //
    // Emitted ONLY in a co-sim run (host + device cores coexist). In a
    // device-scope run every core is an ALU PE, protocolTail is always 0, and
    // appending an extra LambdaStat here would shift heap layout and perturb the
    // contention sim's address-ordered tie-breaking.
    /* 1.11.57 (latent F031): what that perturbation is and is NOT. This note
     * used to finish "-- breaking device-scope bit-identity vs the 1.6/1.8.6
     * baseline", a guarantee D14 withdrew. core.h states the honest form: SAME
     * BINARY plus same config reproduces bit-identically, always; a DIFFERENT
     * BUILD of the same source may drift a few cycles (6 cycles measured on
     * identical 1.11.15 source). So the reason to keep the stat conditional is
     * that it perturbs a cross-build comparison that is already only
     * approximately stable -- not that it breaks an exact guarantee the
     * project no longer makes. */
    // zinfo->cores is fully populated
    // before this initStats pass (init.cpp), so the host-core probe is safe.
    bool cosimRun = false;
    for (uint32_t c = 0; c < zinfo->numCores; c++)
        if (zinfo->cores[c] &&
            (zinfo->cores[c]->asInOrderCore() || zinfo->cores[c]->asOOOCore())) {
            cosimRun = true; break;
        }
    if (cosimRun) {
        uint32_t sid = srcId_;
        auto xt = [sid]() -> uint64_t { return g_mpiProtocolTailCyc[sid]; };
        auto protocolTailStat = makeLambdaStat(xt);
        protocolTailStat->init("protocolTail", "Post-ROI MPI inter-PE collective tail cycles (device-resident)");
        coreStat->append(protocolTailStat);
        /* 1.12.3 (ruling 1 (a)): the kernel-window receipt beside the tail,
         * in the SAME basis as 'cycles' (ROI-relative: the opener's pre-ROI
         * cycles are rebased out of its 'cycles' line, so the receipt is
         * derived from that line, not from the raw marker). For a marked
         * rank roiCycles + protocolTail == cycles exactly; 0 for an unmarked
         * rank. The consumer (src/main.cpp) prices the device over cycles
         * minus the tail; this line lets a reader of the dump see the window. */
        auto xr = [this, sid]() -> uint64_t {
            if (g_mpiRoiCyc[sid] == 0) return 0;   // never marked
            uint64_t c = roiBusyCycles(), t = g_mpiProtocolTailCyc[sid];   // 1.12.18: same basis as 'cycles' (busy)
            return (c > t) ? (c - t) : 0;
        };
        auto roiCyclesStat = makeLambdaStat(xr);
        roiCyclesStat->init("roiCycles", "Kernel-window cycles: PE cycles at its closing-barrier marker (= cycles - protocolTail; 0 = unmarked rank, window is the whole run)");
        coreStat->append(roiCyclesStat);
    }

    parentStat->append(coreStat);
}

void ALUCore::contextSwitch(int32_t gid) {
    if (gid == -1) {
        // No gid switch, just sync
    }
}

void ALUCore::join() {
    /* 1.12.18 (user ruling 2026-10-23, "sync to waker"): re-base the phase
     * budget on the clock, do not grant one. This was phaseEndCycle +=
     * phaseLength on every join: a PE that left mid-phase came back with
     * its unused remainder PLUS a fresh full phase, so each wait handed it
     * up to 2 x phaseLength of its own cycles in one global phase and the
     * ROI elapsed time on the phase clock depended on phaseLength. Now the
     * core runs to the end of the grid phase its clock is in (the phase it
     * left in, or -- after syncToWaker -- its waker's). With the futex
     * syscall blacklisted (scheduler.cpp: the synthetic PC 0x1000+202 makes
     * EVERY futex a real leave/join, wakes and EAGAIN returns included),
     * such a non-blocking leave/join now costs zero simulated time. */
    phaseEndCycle = gridPhaseEnd(curCycle);
}

uint64_t ALUCore::gridPhaseEnd(uint64_t c) {   // 1.12.18 (sync to waker), see alu_core.h
    const uint64_t pl = zinfo->phaseLength ? zinfo->phaseLength : 1;
    return (c / pl + 1) * pl;
}

/* 1.12.18 (user ruling 2026-10-23, "model the lock word"), see core.h: the
 * same re-base as syncToWaker -- the clock moves to the releaser's release
 * and the PE runs to the end of the grid phase that clock is in. The jump is
 * waiting, not activity: it is kept out of 'cycles' (roiBusyCycles). */
uint64_t ALUCore::syncToLock(uint64_t t) {
    if (t <= curCycle) return 0;
    const uint64_t skip = t - curCycle;
    lockSyncCycles += skip; lockSyncs++;
    curCycle = t;
    phaseEndCycle = gridPhaseEnd(curCycle);
    return skip;
}

uint64_t ALUCore::syncToWaker(uint64_t t) {   // 1.12.18 (user ruling 2026-10-23), see core.h
    uint64_t skip = 0;
    if (t > curCycle) {
        skip = t - curCycle;
        syncSkip_ += skip; wakeSyncs_++;
        curCycle = t;
        phaseEndCycle = gridPhaseEnd(curCycle);
    }
    return skip;
}

// Basic block execution - only ALU operations, NO memory
inline void ALUCore::bbl(BblInfo* bblInfo) {
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

    // Datapath mode: bit-serial charges ~W bit-steps per op (cost proportional to
    // operand width, W-bit op = W single-bit steps); bit-parallel (default) charges
    // one step regardless of width, like a full-width ALU. So bit_serial = false
    // leaves compute cost independent of operand_width (backward-compatible).
    uint64_t steps = bitSerial ? (uint64_t)std::max(operandWidth, 1) : 1ull;
    /* 1.11.94 (H04 x05-zsim-cores-2): carry the fraction across blocks
     * (chargeCompute in alu_core.h) instead of truncating per block. On the
     * defaults (computeFactor 1.0, throughputFactor 1.0, steps 1 or the
     * integer operandWidth) the product is an exact integer in double, the
     * carry stays exactly 0.0, floor() returns the product itself, and the
     * charged cycles are the same integer as the old truncation. */
    curCycle += chargeCompute(bblInfo->instrs * computeFactor * steps / throughputFactor);
}

// Static callback functions for Pin instrumentation
void ALUCore::BblFunc(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo) {
    ALUCore* core = static_cast<ALUCore*>(cores[tid]);
    core->bbl(bblInfo);

    // Timing simulation only within phases
    while (core->curCycle > core->phaseEndCycle) {
        core->phaseEndCycle += zinfo->phaseLength;
        uint32_t cid = getCid(tid);

        // NOTE: TakeBarrier may take ownership of the core, and so it will be used by some other thread.
        // If TakeBarrier context-switches us, the *only* safe option is to return immediately after we
        // detect this, or we can race and corrupt core state.
        uint32_t newCid = TakeBarrier(tid, cid);
        if (newCid != cid) break; /*context-switch*/
        /* (1.12.188): held by the cut hold: take the next barrier without running, the clock frozen */
        while (cid < MAX_THREADS && g_peCutHold[cid].load(std::memory_order_acquire)) {
            newCid = TakeBarrier(tid, cid);
            if (newCid != cid) break;
        }
        if (newCid != cid) break; /*context-switch*/
    }
}

// Load -- local: accessFactor cycles; remote: accessFactor + MI hierarchy
// traversal. ONE rule everywhere: a device PE's accesses go through ITS OWN
// device model (the PE-MI), in device scope and co-sim alike. The only
// exception is a DMA window (dmaWindow_): the staging copy's bulk cost is
// already charged on the host-device link, so the loop charges flat cost.
void ALUCore::LoadFunc(THREADID tid, ADDRINT addr) {
    ALUCore* core = static_cast<ALUCore*>(cores[tid]);
    if (core->spmCap_ && !core->stkInSpm_) core->peekStack(addr);   // 1.12.18: stack: dram, measured only
    if (core->inSpm(addr)) {   // 1.12.18: the scratchpad, never the memory interface
        core->curCycle += core->chargeAccess() + core->chargeSpm();
        core->spmReads_++;
        return;
    }
    if (core->inCmd(addr)) {   // 1.12.18: the DMA engine's command registers
        core->curCycle += core->chargeAccess();
        core->cmdReads_++;
        return;
    }
    if (core->stkInSpm_ && core->inStack(addr)) {   // 1.12.18: the PE's own stack, in the scratchpad (stack: spm)
        core->curCycle += core->chargeAccess() + core->chargeSpm();
        core->spmReads_++; core->stkReads_++;
        return;
    }
    Address lineAddr = addr >> lineBits;
    if (!core->mi_ || core->dmaWindow_ || core->mi_->isLocalAddress(lineAddr)) {
        // No MI, DMA window, or local address: direct array access cost
        core->curCycle += core->chargeAccess();  // 1.11.94 (H04): carried fraction
    } else {
        // Remote: PE overhead + hierarchy traversal via MI
        core->curCycle += core->chargeAccess();  // 1.11.94 (H04): carried fraction
        MESIState state = I;
        MemReq req = {lineAddr, GETS, 0, &state, core->curCycle, nullptr, state, core->srcId_, 0};
        core->curCycle = core->mi_->access(req);
    }
}

void ALUCore::StoreFunc(THREADID tid, ADDRINT addr) {
    ALUCore* core = static_cast<ALUCore*>(cores[tid]);
    if (core->spmCap_ && !core->stkInSpm_) core->peekStack(addr);   // 1.12.18: stack: dram, measured only
    if (core->inSpm(addr)) {   // 1.12.18: the scratchpad, never the memory interface
        core->curCycle += core->chargeAccess() + core->chargeSpm();
        core->spmWrites_++;
        return;
    }
    if (core->inCmd(addr)) {   // 1.12.18: the DMA engine's command registers
        core->curCycle += core->chargeAccess();
        core->cmdWrites_++;
        return;
    }
    if (core->stkInSpm_ && core->inStack(addr)) {   // 1.12.18: the PE's own stack, in the scratchpad (stack: spm)
        core->curCycle += core->chargeAccess() + core->chargeSpm();
        core->spmWrites_++; core->stkWrites_++;
        return;
    }
    Address lineAddr = addr >> lineBits;
    if (!core->mi_ || core->dmaWindow_ || core->mi_->isLocalAddress(lineAddr)) {
        core->curCycle += core->chargeAccess();  // 1.11.94 (H04): carried fraction
    } else {
        core->curCycle += core->chargeAccess();  // 1.11.94 (H04): carried fraction
        MESIState state = I;
        MemReq req = {lineAddr, GETX, 0, &state, core->curCycle, nullptr, state, core->srcId_, 0};
        core->curCycle = core->mi_->access(req);
    }
}

void ALUCore::PredLoadFunc(THREADID tid, ADDRINT addr, BOOL pred) {
    if (pred) {
        ALUCore* core = static_cast<ALUCore*>(cores[tid]);
        if (core->spmCap_ && !core->stkInSpm_) core->peekStack(addr);   // 1.12.18: stack: dram, measured only
        if (core->inSpm(addr)) {   // 1.12.18: the scratchpad, never the memory interface
            core->curCycle += core->chargeAccess() + core->chargeSpm();
            core->spmReads_++;
            return;
        }
        if (core->inCmd(addr)) {   // 1.12.18: the DMA engine's command registers
            core->curCycle += core->chargeAccess();
            core->cmdReads_++;
            return;
        }
        if (core->stkInSpm_ && core->inStack(addr)) {   // 1.12.18: the PE's own stack, in the scratchpad (stack: spm)
            core->curCycle += core->chargeAccess() + core->chargeSpm();
            core->spmReads_++; core->stkReads_++;
            return;
        }
        Address lineAddr = addr >> lineBits;
        if (!core->mi_ || core->dmaWindow_ || core->mi_->isLocalAddress(lineAddr)) {
            core->curCycle += core->chargeAccess();  // 1.11.94 (H04): carried fraction
        } else {
            core->curCycle += core->chargeAccess();  // 1.11.94 (H04): carried fraction
            MESIState state = I;
            MemReq req = {lineAddr, GETS, 0, &state, core->curCycle, nullptr, state, core->srcId_, 0};
            core->curCycle = core->mi_->access(req);
        }
    }
}

void ALUCore::PredStoreFunc(THREADID tid, ADDRINT addr, BOOL pred) {
    if (pred) {
        ALUCore* core = static_cast<ALUCore*>(cores[tid]);
        if (core->spmCap_ && !core->stkInSpm_) core->peekStack(addr);   // 1.12.18: stack: dram, measured only
        if (core->inSpm(addr)) {   // 1.12.18: the scratchpad, never the memory interface
            core->curCycle += core->chargeAccess() + core->chargeSpm();
            core->spmWrites_++;
            return;
        }
        if (core->inCmd(addr)) {   // 1.12.18: the DMA engine's command registers
            core->curCycle += core->chargeAccess();
            core->cmdWrites_++;
            return;
        }
        if (core->stkInSpm_ && core->inStack(addr)) {   // 1.12.18: the PE's own stack, in the scratchpad (stack: spm)
            core->curCycle += core->chargeAccess() + core->chargeSpm();
            core->spmWrites_++; core->stkWrites_++;
            return;
        }
        Address lineAddr = addr >> lineBits;
        if (!core->mi_ || core->dmaWindow_ || core->mi_->isLocalAddress(lineAddr)) {
            core->curCycle += core->chargeAccess();  // 1.11.94 (H04): carried fraction
        } else {
            core->curCycle += core->chargeAccess();  // 1.11.94 (H04): carried fraction
            MESIState state = I;
            MemReq req = {lineAddr, GETX, 0, &state, core->curCycle, nullptr, state, core->srcId_, 0};
            core->curCycle = core->mi_->access(req);
        }
    }
}

/* 1.12.18: the kernel binds (or with bytes 0 releases) the RAM it allocated
 * for this PE's scratchpad, and the 64-byte block that holds its DMA engine's
 * command registers (0 = none, core staging). Refused on a PE without a
 * scratchpad, and refused when the RAM is larger than the configured capacity
 * or the command block overlaps it. */
bool ALUCore::inStack(ADDRINT a) {
    const uint64_t x = (uint64_t)a;
    if (stkHi_ == 0) {
        if (!mi_ || !mi_->ownsStack(x, &stkLo_, &stkHi_)) return false;
        info("%s: stack [0x%lx, 0x%lx) is this PE's: priced in its scratchpad", name.c_str(),
             (unsigned long)stkLo_, (unsigned long)stkHi_);
    }
    if (x - stkLo_ >= stkHi_ - stkLo_) return false;
    if (x < stkMinT_) stkMinT_ = x;
    if (x > stkMaxT_) stkMaxT_ = x;
    if (stkMaxT_ + 8 - stkMinT_ > stkReserve_ && roiBaseCycle && !zinfo->terminationConditionMet)
        panic("%s: the stack spans %lu bytes in the ROI, more than its scratchpad reserve "
              "pim.pe.spm_stack_bytes = %lu (pim.pe.stack: spm): raise spm_stack_bytes, and spm_bytes with it",
              name.c_str(), (unsigned long)(stkMaxT_ + 8 - stkMinT_), (unsigned long)stkReserve_);
    return true;
}

/* stack: dram -- the access goes to the memory interface, which claims the
 * stack (1.12.17); the PE only records the span it touches. */
void ALUCore::peekStack(ADDRINT a) {
    const uint64_t x = (uint64_t)a;
    if (stkHi_ == 0 && (!mi_ || !mi_->stackOwned(x, &stkLo_, &stkHi_))) return;
    if (x - stkLo_ >= stkHi_ - stkLo_) return;
    if (x < stkMinT_) stkMinT_ = x;
    if (x > stkMaxT_) stkMaxT_ = x;
}

/* 1.12.18: at roi_begin the PE running the thread claims the stack under its
 * stack pointer (the 1.12.17 first-touch claim, made at that point): a thread
 * that only touches its own frame above that pointer (a serial main) owns it
 * all the same. */
void ALUCore::spmOwnStack(uint64_t sp) {
    if (!spmCap_ || stkHi_ || !mi_ || sp < 8) return;
    if (!stkInSpm_) {   // stack: dram -- measured only; ownership and placement stay as in 1.12.17
        PEMemoryInterface::stackRange(sp - 8, &stkLo_, &stkHi_);
        return;
    }
    if (mi_->ownsStack(sp - 8, &stkLo_, &stkHi_))
        info("%s: stack [0x%lx, 0x%lx) is this PE's: priced in its scratchpad", name.c_str(),
             (unsigned long)stkLo_, (unsigned long)stkHi_);
}

bool ALUCore::spmBind(uint64_t lo, uint64_t bytes, uint64_t cmd) {
    if (bytes == 0) { spmLo_ = spmHi_ = cmdLo_ = cmdHi_ = 0; return true; }
    if (spmCap_ == 0 || bytes > spmBufferCap()) return false;
    if (cmd && cmd < lo + bytes && cmd + 64 > lo) return false;
    spmLo_ = lo; spmHi_ = lo + bytes;
    cmdLo_ = cmd; cmdHi_ = cmd ? cmd + 64 : 0;
    return true;
}

/* 1.12.18: one DMA block transfer between DRAM and the scratchpad. The engine
 * issues the block's lines through this PE's memory interface in address
 * order at the interface's service rate (its bandwidth at the PE clock), and the
 * PE waits for the last one: the transfer costs the DRAM and network time the
 * memory model measures, and no PE instructions. The caller (the plugin) then
 * moves the bytes. false = no DMA engine here (core staging, or none). */
bool ALUCore::spmDma(uint64_t dst, uint64_t src, uint64_t bytes, bool toDram) {
    if (spmMode_ != 1 || spmCap_ == 0 || spmHi_ == spmLo_) return false;
    const uint64_t spmSide = toDram ? src : dst, dramSide = toDram ? dst : src;
    if (spmSide < spmLo_ || spmSide + bytes > spmHi_)
        panic("%s: DMA block [0x%lx, +%lu) is not inside the bound scratchpad [0x%lx, 0x%lx)",
              name.c_str(), (unsigned long)spmSide, (unsigned long)bytes,
              (unsigned long)spmLo_, (unsigned long)spmHi_);
    if (dramSide < spmHi_ && dramSide + bytes > spmLo_)
        panic("%s: DMA memory side [0x%lx, +%lu) overlaps the scratchpad", name.c_str(),
              (unsigned long)dramSide, (unsigned long)bytes);
    if (!mi_)
        panic("%s: a scratchpad DMA needs the PE memory interface (no device memory hierarchy is wired)", name.c_str());
    if (bytes == 0) return true;
    const double gap = mi_->lineIssueGapCycles();
    if (gap <= 0.0)
        panic("%s: a scratchpad DMA needs the memory interface's bandwidth, and none is configured", name.c_str());
    const uint64_t start = curCycle + chargeAccess();   // the DMA command is one access instruction
    const Address first = dramSide >> lineBits, last = (dramSide + bytes - 1) >> lineBits;
    uint64_t done = start;
    uint64_t i = 0;
    /* (1.12.111): an SRAM/NVM line waits for its bank in the memory interface's bank model
     * (reads and writes, at the bank's own access and write time); the engine issues at
     * the interface's rate. 1.12.90 spaced writes to a unit here, at the previous
     * write's return time (bank time plus the network). */
    DramEpochReplay::dmaBurst(true);   // (1.12.189): the lines below are one transfer
    for (Address l = first; l <= last; l++, i++) {
        MESIState state = I;
        uint64_t issue = start + (uint64_t)std::floor((double)i * gap);
        MemReq req = {l, toDram ? GETX : GETS, 0, &state, issue, nullptr, state, srcId_, 0};
        uint64_t t = mi_->access(req);
        if (t > done) done = t;
    }
    DramEpochReplay::dmaBurst(false);
    const uint64_t words = (bytes + 7) / 8;   // the RAM side: one access per 8-byte word
    if (toDram) spmReads_ += words; else spmWrites_ += words;
    dmaOps_++;
    dmaLines_ += i;
    dmaCycles_ += done - curCycle;
    curCycle = done;
    return true;
}

// Return function pointers for Pin instrumentation
InstrFuncPtrs ALUCore::GetFuncPtrs() {
    return {LoadFunc, StoreFunc, BblFunc, BranchFunc, PredLoadFunc, PredStoreFunc, FPTR_ANALYSIS, {0}};
}
