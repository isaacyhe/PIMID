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

#ifndef IN_ORDER_CORE_H_
#define IN_ORDER_CORE_H_

#include "core.h"
#include "core_recorder.h"
#include "event_recorder.h"
#include "memory_hierarchy.h"
#include "ooo_core.h"  /* BranchPredictorPAg -- same predictor structure as OOOCore */
#include "pad.h"

class FilterCache;

/* Genuine in-order pipeline PE core.
 *
 * Historically this core modeled IPC=1 (curCycle += bblInstrs) plus blocking
 * memory latency and a CoreRecorder weave for cross-PE contention -- which made
 * it numerically identical to SimpleCore whenever there was no contention
 * ("in-order in name only"). This version consumes the same x86-decoded DynUop
 * stream the OOO core uses (BblInfo->oooBbl, populated by the plugin's
 * x86_decoder.h when an in-order OR out-of-order core is present) and runs a
 * real in-order scoreboard:
 *
 *   - uops issue in STRICT PROGRAM ORDER (the defining in-order property);
 *   - a per-physical-register ready-cycle scoreboard stalls issue on RAW
 *     hazards (a uop cannot issue until its source registers are ready);
 *   - up to issueWidth INDEPENDENT uops may issue in one cycle, but the
 *     front-end STOPS at the first uop that cannot issue (no reordering);
 *   - functional-unit port contention is modeled from the decoder port masks;
 *   - loads/stores go through the l1d cache path; a load's destination-register
 *     readiness is gated on the returned latency (load-use stall), and memory
 *     accesses are serialized (blocking L1) which also preserves the
 *     CoreRecorder weave invariant;
 *   - scoreboard state (register ready cycles, port free cycles, issue slot
 *     cursor) CARRIES ACROSS BBL boundaries: a trailing load's completion only
 *     stalls uops that actually depend on it, so independent work in the next
 *     basic block overlaps with it (an in-order front-end flows across basic
 *     blocks; only true dependencies and issue/port limits stall). The pipeline
 *     IS drained (curCycle advanced to the newest outstanding completion) at
 *     points where the overlap is not architecturally meaningful: a branch
 *     MISPREDICT (the flush empties the front-end; correctly-predicted and
 *     fall-through boundaries do NOT drain), and scheduler boundaries --
 *     join/leave, the bound->weave transitions (cSimStart/cSimEnd, so the
 *     CoreRecorder taper covers all outstanding latency), and contextSwitch.
 *
 * The CoreRecorder cross-PE contention weave is layered on top unchanged.
 *
 * NOTE on hit-under-miss (non-blocking L1): intentionally NOT modeled. The
 * CoreRecorder event chain requires each recorded access to start at or after
 * the previous response (CoreRecorder::recordAccess asserts
 * startCycle >= prevRespCycle and links events serially), and FilterCache has
 * no side-effect-free probe to know whether an access hits before issuing it,
 * so a second access cannot be safely started under an outstanding miss.
 * Supporting it would need an OOOCoreRecorder-style multi-outstanding weave
 * recorder. Memory therefore stays serialized via memRespCycle.
 *
 * (1.11.44: the PIMID_INORDER_NODECODE legacy IPC=1 escape hatch is DELETED
 * -- its A/B purpose ended with the 1.4.x validation and it bred audit
 * findings. The line below is retained for history only.)
 * Historical: PIMID_INORDER_NODECODE=1 restored the exact legacy IPC=1
 * immediate-processing path (byte-identical A/B baseline). A BBL with no decoded
 * uops falls back to a per-BBL synthetic 1-CPI path that still drains buffered
 * loads/stores through the cache so memory/NoC traffic never desyncs.
 *
 * Branch mispredictions: the plugin resolves each TB-terminating conditional
 * branch's real direction (from the next TB's address) and feeds it via
 * BranchFunc, exactly as for the OOO core. The in-order core runs the SAME
 * BranchPredictorPAg<11,18,14> predictor as OOOCore; on a mispredict it charges
 * a fixed front-end flush/refill bubble (default 7 cycles ~= the OOO model's
 * fetch-to-issue depth, ISSUE_STAGE; typical of short in-order pipelines such
 * as Cortex-A53's ~8-cycle mispredict penalty). Overridable via
 * PIMID_INORDER_MISPRED_PENALTY; disable the whole feed with
 * PIMID_INORDER_NOBRANCH=1 (mirrors PIMID_OOO_NOBRANCH).
 */
class InOrderCore : public Core {
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

    private:
        FilterCache* l1i;
        FilterCache* l1d;

        uint64_t instrs;
        uint64_t uops;
        uint64_t bbls;

        uint64_t curCycle; //phase 1 clock (in-order issue/commit cursor)
        uint64_t phaseEndCycle; //phase 1 end clock

        CoreRecorder cRec;

        // ---- In-order scoreboard state (decoded path) ----
        uint32_t issueWidth; // independent uops issuable per cycle (default 2)

        BblInfo* prevBbl;    // deferred: simulate prev BBL once its mem addrs arrive

        // Buffered memory addresses for the BBL currently executing (decode path)
        Address loadAddrs[256];
        Address storeAddrs[256];
        uint32_t loads;
        uint32_t stores;

        // Per-physical-register ready cycle (timestamp a reg's value is available)
        uint64_t regScoreboard[MAX_REGISTERS];
        // Per functional-unit port: earliest cycle the port is free (1 uop/cyc)
        static const uint32_t NUM_PORTS = 6;
        uint64_t portFreeCycle[NUM_PORTS];
        // Serialization cursor for the blocking L1 / cRec weave invariant: the
        // response cycle of the last memory access threaded through the cache.
        uint64_t memRespCycle;
        uint32_t slotsUsed;  // uops already issued at cycle slotCycle
        uint64_t slotCycle;  // the issue cycle slotsUsed refers to (carries across BBLs)
        // Newest completion time of any issued uop (or drained access). Cross-BBL
        // overlap means curCycle (the ISSUE cursor) may trail this; drainPipeline
        // advances curCycle to it at flush/scheduler boundaries.
        uint64_t maxOutstanding;

        // Diagnostics (analogous to the OOO core)
        uint64_t decodedBbls;    // BBLs run through the decoded in-order scoreboard
        uint64_t syntheticBbls;  // BBLs run through the 1-CPI synthetic fallback
        uint64_t depStalls;      // cycles lost to RAW dependency stalls
        uint64_t issueStalls;    // cycles lost to issue-width/port stalls
        uint64_t memMismatchLoads;   // decoded/runtime load-count divergences drained
        uint64_t memMismatchStores;  // decoded/runtime store-count divergences drained
        // Expected rep-string (movs/stos) accesses drained through the
        // block-copy cost model (DynBbl.repInstrs > 0) -- NOT divergences.
        uint64_t repDrainedLoads;
        uint64_t repDrainedStores;

        // ---- Branch prediction (decode path only) ----
        // Same predictor structure as OOOCore (2-level PAg).
        BranchPredictorPAg<11, 18, 14> branchPred;
        Address branchPc;        // 0 if the BBL being simulated did not end in a jcc
        bool branchTaken;
        uint32_t mispredPenalty; // front-end flush/refill bubble (cycles)
        uint64_t branches;           // resolved conditional branches fed to the predictor
        uint64_t mispredBranches;    // mispredicted branches
        uint64_t mispredStallCycles; // total cycles charged for mispredict bubbles

        // Indirect control-flow prediction: same minimal structures as OOOCore
        // (direct-mapped PC-tagged 512-entry BTB + 16-entry RAS). A wrong
        // target charges the same mispredPenalty bubble as a conditional
        // mispredict. Gated by PIMID_INORDER_NOBRANCH like all branch modeling.
        IndirectPredictor<9, 16> indirPred;
        bool indirMispredPend;
        uint64_t indirBranches;
        uint64_t indirMispreds;
        uint64_t rasReturns;
        uint64_t rasMispreds;

        // ROI baselines: snapshot at roi_begin so reported cycles/instrs reflect
        // ONLY the region of interest. roiBaseCycle snapshots the unhalted-cycle
        // metric (cRec.getUnhaltedCycles(curCycle)). 0 until roi_begin fires.
        uint64_t roiBaseInstrs = 0;
        /* 1.9.33: activity counters share the ROI baseline with instrs. See the
         * matching note in ooo_core.h -- reporting an ROI-windowed instruction
         * count next to whole-run activity totals made every derived ratio
         * meaningless. */
        uint64_t roiBaseUops     = 0;
        uint64_t roiBaseBbls     = 0;
        uint64_t roiBaseBranches = 0;
        uint64_t roiBaseMispred  = 0;
        /* 1.11.93 (F6): the BTB and RAS counters on the ROI window too. The
         * power model now prices the BTB and the RAS this core runs, from
         * these counts; the raw indirBranches/rasReturns stay whole-run
         * diagnostics, and an ROI-priced structure fed a whole-run count is
         * the 1.9.28 base-mismatch defect. */
        uint64_t roiBaseIndir    = 0;
        uint64_t roiBaseRas      = 0;
        uint64_t roiBaseCycle  = 0;
        uint64_t roiBaseCCycles = 0;   // 1.11.17: contention-cycle ROI base (parity with OOO 1.11.9)

    public:
        // _issueWidth: in-order superscalar issue width (YAML pim.pe.issue_width,
        // plumbed via the ZSim config key issueWidth; default 2). Precedence:
        // PIMID_INORDER_WIDTH env var (if set) > YAML/ctor value > default 2.
        InOrderCore(FilterCache* _l1i, FilterCache* _l1d, uint32_t domain, g_string& _name,
                    uint32_t _issueWidth = 2);
        void initStats(AggregateStat* parentStat);

        uint64_t getInstrs() const {return instrs;}
        uint64_t getPhaseCycles() const;
        // 1.6.1 frozen-clock MPI waits for weave cores: rewinding the bound
        // clock under scheduled weave events is unsafe, so the wait's raw
        // growth accumulates here and is SUBTRACTED at every reporting point
        // (getCycles + the ROI cycles stat) instead. One accumulator, two
        // read sites -- stamps, rendezvous math, and zsim.out all see the
        // same wall-free clock.
        uint64_t pimidPhantomWait = 0;
        void pimidRewindCycles(uint64_t delta) override { pimidPhantomWait += delta; }
        uint64_t getCycles() const {
            uint64_t c = cRec.getUnhaltedCycles(curCycle);
            return (c > pimidPhantomWait) ? (c - pimidPhantomWait) : 0;
        }

        void contextSwitch(int32_t gid);
        virtual void join();
        virtual void leave();

        InstrFuncPtrs GetFuncPtrs();

        // Snapshot current counters as the ROI baseline (called on roi_begin).
        void markRoiBegin() override {
            roiBaseInstrs = instrs; roiBaseCycle = getCycles();  // adjusted clock: pre-ROI phantom excluded
            roiBaseUops = uops; roiBaseBbls = bbls;              // 1.9.33
            roiBaseBranches = branches; roiBaseMispred = mispredBranches;
            roiBaseIndir = indirBranches; roiBaseRas = rasReturns;   // 1.11.93 (F6)
            roiBaseCCycles = cRec.getContentionCycles();         // 1.11.17: like OOO (1.11.9)
            mixMarkRoi();   // 1.11.10
        }

        // Virtual type check for use without RTTI (Pin 4.x requires -fno-rtti)
        InOrderCore* asInOrderCore() override { return this; }

        // Contention simulation interface. The bound->weave transitions drain
        // the pipeline first so the CoreRecorder taper covers all outstanding
        // (cross-BBL overlapped) completions -- see drainPipeline().
        bool hasContentionSim() const override { return true; }
        EventRecorder* getEventRecorder() override {return cRec.getEventRecorder();}
        void cSimStart() override {drainPipeline(); curCycle = cRec.cSimStart(curCycle);}
        void cSimEnd() override {
            drainPipeline();
            uint64_t pre = curCycle;
            curCycle = cRec.cSimEnd(curCycle);
            // cSimEnd may fold a contention SKEW into the phase-1 clock: it
            // advances BOTH curCycle and the CoreRecorder's prevRespCycle by the
            // same `skew` (curCycle - pre). memRespCycle is this core's private
            // phase-1-clock serialization cursor -- the response cycle of the
            // last cache access threaded through the weave -- and it gates the
            // start cycle of the NEXT recorded access (startCycle =
            // MAX(issueCursor, memRespCycle)). It lives outside the recorder, so
            // the skew never reached it. When memRespCycle runs AHEAD of the
            // issue cursor (an ifetch/load miss whose response outran the
            // front-end -- the intended cross-BBL/separate-front-end overlap),
            // an un-skewed memRespCycle leaves the next access starting BEFORE
            // the skew-bumped prevRespCycle, tripping CoreRecorder::recordAccess'
            // startCycle>=prevRespCycle assert (seen with an in-order co-sim
            // HOST: its L1/L2/DRAM miss latencies routinely push memRespCycle
            // past the issue cursor at a skewed phase boundary). Advance the
            // cursor by the same skew so it stays in the recorder's clock. This
            // only changes an access start when memRespCycle is the binding
            // term (memRespCycle > issue cursor) -- exactly the crash case;
            // otherwise the issue cursor dominates the MAX and the shift is
            // unobservable, so contention-free / cache-less (wired-MI device PE)
            // runs are bit-identical.
            if (curCycle > pre) {
                // Diagnostic (PIMID_DEBUG_JOINQ): a "binding" firing is one where
                // memRespCycle was AHEAD of the issue cursor (the crash case) and
                // so the skew shift is observable. 0 binding firings on a run =>
                // the fix is inert for that run (byte-identical). Non-binding
                // firings do not change any recorded access start.
                if (getenv("PIMID_DEBUG_JOINQ"))
                    fprintf(stderr, "[JOINQ %s] cSimEnd-skewfix delta=%lu memRespCycle=%lu curCycle=%lu binding=%d\n",
                        name.c_str(), (unsigned long)(curCycle - pre), (unsigned long)memRespCycle, (unsigned long)curCycle, (int)(memRespCycle > curCycle));
                memRespCycle += (curCycle - pre);
            }
        }

    private:
        // Drain the in-order pipeline: advance the issue cursor to the newest
        // outstanding completion. Called at branch-mispredict flushes and at
        // scheduler boundaries (join/leave/cSim*/contextSwitch), where cross-BBL
        // overlap is not architecturally meaningful.
        inline void drainPipeline() { if (maxOutstanding > curCycle) curCycle = maxOutstanding; }

        inline void loadAndRecord(Address addr);
        inline void storeAndRecord(Address addr);
        inline void bblAndRecord(Address bblAddr, BblInfo* bblInstrs);
        inline void record(uint64_t startCycle);

        // Decoded in-order scoreboard simulation of one (previous) BBL.
        inline void simulateDecodedBbl(BblInfo* bblInfo);
        // Synthetic 1-CPI fallback for a BBL without decoded uops.
        inline void simulateSyntheticBbl(BblInfo* bblInfo);
        // Instruction fetch of the current BBL (serialized through the L1I).
        inline void ifetch(Address bblAddr, BblInfo* bblInfo);

        // Records the resolved direction of the branch terminating the BBL that
        // is about to be simulated (the plugin calls this right before bblPtr).
        inline void branch(Address pc, bool taken);

        // Indirect control-flow resolution (kind >= CF_IND_JMP, CtrlFlowKind in
        // ooo_core.h): BTB/RAS query+update; arms the mispredict bubble.
        inline void ctrlFlow(uint32_t kind, Address pc, Address target, Address retAddr);

        static void LoadAndRecordFunc(THREADID tid, ADDRINT addr);
        static void StoreAndRecordFunc(THREADID tid, ADDRINT addr);
        static void BblAndRecordFunc(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo);
        static void PredLoadAndRecordFunc(THREADID tid, ADDRINT addr, BOOL pred);
        static void PredStoreAndRecordFunc(THREADID tid, ADDRINT addr, BOOL pred);

        static void BranchFunc(THREADID tid, ADDRINT pc, BOOL taken, ADDRINT takenNpc, ADDRINT notTakenNpc);
} ATTR_LINE_ALIGNED;

#endif  // IN_ORDER_CORE_H_
