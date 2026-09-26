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

#ifndef OOO_CORE_H_
#define OOO_CORE_H_

#include <algorithm>
#include <queue>
#include <string>
#include "core.h"
#include "g_std/g_multimap.h"
#include "memory_hierarchy.h"
#include "ooo_core_recorder.h"
#include "pad.h"

// Uncomment to enable stall stats
// #define OOO_STALL_STATS

class FilterCache;

/* 2-level branch predictor:
 *  - L1: Branch history shift registers (bshr): 2^NB entries, HB bits of history/entry, indexed by XOR'd PC
 *  - L2: Pattern history table (pht): 2^LB entries, 2-bit sat counters, indexed by XOR'd bshr contents
 *  NOTE: Assumes LB is in [NB, HB] range for XORing (e.g., HB = 18 and NB = 10, LB = 13 is OK)
 */
template<uint32_t NB, uint32_t HB, uint32_t LB>
class BranchPredictorPAg {
    private:
        uint32_t bhsr[1 << NB];
        uint8_t pht[1 << LB];

    public:
        BranchPredictorPAg() {
            uint32_t numBhsrs = 1 << NB;
            uint32_t phtSize = 1 << LB;

            for (uint32_t i = 0; i < numBhsrs; i++) {
                bhsr[i] = 0;
            }
            for (uint32_t i = 0; i < phtSize; i++) {
                pht[i] = 1;  // weak non-taken
            }

            static_assert(LB <= HB, "Too many PHT entries");
            static_assert(LB >= NB, "Too few PHT entries (you'll need more XOR'ing)");
        }

        // Predicts and updates; returns false if mispredicted
        inline bool predict(Address branchPc, bool taken) {
            uint32_t bhsrMask = (1 << NB) - 1;
            uint32_t histMask = (1 << HB) - 1;
            uint32_t phtMask  = (1 << LB) - 1;

            // Predict
            // uint32_t bhsrIdx = ((uint32_t)( branchPc ^ (branchPc >> NB) ^ (branchPc >> 2*NB) )) & bhsrMask;
            uint32_t bhsrIdx = ((uint32_t)( branchPc >> 1)) & bhsrMask;
            uint32_t phtIdx = bhsr[bhsrIdx];

            // Shift-XOR-mask to fit in PHT
            phtIdx ^= (phtIdx & ~phtMask) >> (HB - LB); // take the [HB-1, LB] bits of bshr, XOR with [LB-1, ...] bits
            phtIdx &= phtMask;

            // If uncommented, behaves like a global history predictor
            // bhsrIdx = 0;
            // phtIdx = (bhsr[bhsrIdx] ^ ((uint32_t)branchPc)) & phtMask;

            bool pred = pht[phtIdx] > 1;

            // info("BP Pred: 0x%lx bshr[%d]=%x taken=%d pht=%d pred=%d", branchPc, bhsrIdx, phtIdx, taken, pht[phtIdx], pred);

            // Update
            pht[phtIdx] = taken? (pred? 3 : (pht[phtIdx]+1)) : (pred? (pht[phtIdx]-1) : 0); //2-bit saturating counter
            bhsr[bhsrIdx] = ((bhsr[bhsrIdx] << 1) & histMask ) | (taken? 1: 0); //we apply phtMask here, dependence is further away

            // info("BP Update: newPht=%d newBshr=%x", pht[phtIdx], bhsr[bhsrIdx]);
            return (taken == pred);
        }
};

/* Control-flow kinds fed through the branchPtr callback. Values 0/1 are the
 * classic conditional-branch direction (not-taken/taken); >= 2 are indirect
 * control-flow resolutions handled by IndirectPredictor (BTB + RAS). */
enum CtrlFlowKind : uint32_t {
    CF_COND_NT = 0, CF_COND_T = 1,
    CF_IND_JMP = 2,   /* jmp r/m   (FF /4,/5)             */
    CF_IND_CALL = 3,  /* call r/m  (FF /2,/3): BTB + push */
    CF_RET = 4,       /* ret       (C3/C2): RAS pop       */
    CF_DIR_CALL = 5   /* call rel  (E8): RAS push only    */
};

/* Minimal indirect-branch predictor: a direct-mapped, PC-tagged BTB predicting
 * "same target as last time" for indirect jmp/call, plus a circular
 * return-address stack for call/ret pairs. Standard minimal structures:
 *  - BTB: 2^BTB_BITS entries (default 512), full-PC tag, single target.
 *  - RAS: RAS_SZ entries (default 16); push on call, pop+compare on ret;
 *    pop on empty and capacity wrap-around behave like real hardware
 *    (mispredict / silent overwrite of oldest).
 */
template<uint32_t BTB_BITS, uint32_t RAS_SZ>
class IndirectPredictor {
    private:
        struct BtbEntry { Address pc; Address target; };
        BtbEntry btb[1 << BTB_BITS];
        Address ras[RAS_SZ];
        uint32_t rasTop;
        uint32_t rasCount;

    public:
        IndirectPredictor() {
            for (uint32_t i = 0; i < (1u << BTB_BITS); i++) btb[i] = {0, 0};
            for (uint32_t i = 0; i < RAS_SZ; i++) ras[i] = 0;
            rasTop = 0; rasCount = 0;
        }

        /* Predict+update for an indirect jmp/call; returns true if the BTB had
         * this PC's actual target (correct prediction). */
        inline bool indirect(Address pc, Address target) {
            uint32_t idx = ((uint32_t)(pc >> 1)) & ((1u << BTB_BITS) - 1);
            bool hit = (btb[idx].pc == pc && btb[idx].target == target);
            btb[idx].pc = pc; btb[idx].target = target;
            return hit;
        }

        inline void push(Address retAddr) {
            ras[rasTop] = retAddr;
            rasTop = (rasTop + 1) % RAS_SZ;
            if (rasCount < RAS_SZ) rasCount++;
        }

        /* Pop+compare for ret; returns true if predicted correctly. */
        inline bool ret(Address target) {
            if (rasCount == 0) return false;  /* empty RAS -> mispredict */
            rasTop = (rasTop + RAS_SZ - 1) % RAS_SZ;
            rasCount--;
            return ras[rasTop] == target;
        }
};


template<uint32_t H, uint32_t WSZ>
class WindowStructure {
    private:
        // NOTE: Nehalem has POPCNT, but we want this to run reasonably fast on Core2's, so let's keep track of both count and mask.
        struct WinCycle {
            uint8_t occUnits;
            uint8_t count;
            inline void set(uint8_t o, uint8_t c) {occUnits = o; count = c;}
        };

        WinCycle* curWin;
        WinCycle* nextWin;
        typedef g_map<uint64_t, WinCycle> UBWin;
        typedef typename UBWin::iterator UBWinIterator;
        UBWin ubWin;
        uint32_t occupancy;  // elements scheduled in the future

        uint32_t curPos;

        uint8_t lastPort;

    public:
        WindowStructure() {
            curWin = gm_calloc<WinCycle>(H);
            nextWin = gm_calloc<WinCycle>(H);
            curPos = 0;
            occupancy = 0;
        }


        void schedule(uint64_t& curCycle, uint64_t& schedCycle, uint8_t portMask, uint32_t extraSlots = 0) {
            if (!extraSlots) {
                scheduleInternal<true, false>(curCycle, schedCycle, portMask);
            } else {
                scheduleInternal<true, true>(curCycle, schedCycle, portMask);
                uint64_t extraSlotCycle = schedCycle+1;
                uint8_t extraSlotPortMask = 1 << lastPort;
                // This is not entirely accurate, as an instruction may have been scheduled already
                // on this port and we'll have a non-contiguous allocation. In practice, this is rare.
                for (uint32_t i = 0; i < extraSlots; i++) {
                    scheduleInternal<false, false>(curCycle, extraSlotCycle, extraSlotPortMask);
                    // info("extra slot %d allocated on cycle %ld", i, extraSlotCycle);
                    extraSlotCycle++;
                }
            }
            assert(occupancy <= WSZ);
        }

        inline void advancePos(uint64_t& curCycle) {
            occupancy -= curWin[curPos].count;
            curWin[curPos].set(0, 0);
            curPos++;
            curCycle++;

            if (curPos == H) {  // rebase
                // info("[%ld] Rebasing, curCycle=%ld", curCycle/H, curCycle);
                std::swap(curWin, nextWin);
                curPos = 0;
                uint64_t nextWinHorizon = curCycle + 2*H;  // first cycle out of range

                if (!ubWin.empty()) {
                    UBWinIterator it = ubWin.begin();
                    while (it != ubWin.end() && it->first < nextWinHorizon) {
                        uint32_t nextWinPos = it->first - H - curCycle;
                        assert_msg(nextWinPos < H, "WindowStructure: ubWin elem exceeds limit cycle=%ld curCycle=%ld nextWinPos=%d", it->first, curCycle, nextWinPos);
                        nextWin[nextWinPos] = it->second;
                        // info("Moved %d events from unbounded window, cycle %ld (%d cycles away)", it->second, it->first, it->first - curCycle);
                        it++;
                    }
                    ubWin.erase(ubWin.begin(), it);
                }
            }
        }

        void longAdvance(uint64_t& curCycle, uint64_t targetCycle) {
            assert(curCycle <= targetCycle);

            // Drain IW
            while (occupancy && curCycle < targetCycle) {
                advancePos(curCycle);
            }

            if (occupancy) {
                // info("advance: window not drained at %ld, %d uops left", curCycle, occupancy);
                assert(curCycle == targetCycle);
            } else {
                // info("advance: window drained at %ld, jumping to %ld", curCycle, targetCycle);
                assert(curCycle <= targetCycle);
                curCycle = targetCycle;  // with zero occupancy, we can just jump to it
            }
        }

        // Poisons a range of cycles; used by the LSU to apply backpressure to the IW
        void poisonRange(uint64_t curCycle, uint64_t targetCycle, uint8_t portMask) {
            uint64_t startCycle = curCycle;  // curCycle should not be modified...
            uint64_t poisonCycle = curCycle;
            while (poisonCycle < targetCycle) {
                scheduleInternal<false, false>(curCycle, poisonCycle, portMask);
            }
            // info("Poisoned port mask %x from %ld to %ld (tgt %ld)", portMask, curCycle, poisonCycle, targetCycle);
            assert(startCycle == curCycle);
        }

    private:
        template <bool touchOccupancy, bool recordPort>
        void scheduleInternal(uint64_t& curCycle, uint64_t& schedCycle, uint8_t portMask) {
            // If the window is full, advance curPos until it's not
            while (touchOccupancy && occupancy == WSZ) {
                advancePos(curCycle);
            }

            uint32_t delay = (schedCycle > curCycle)? (schedCycle - curCycle) : 0;

            // Schedule, progressively increasing delay if we cannot find a slot
            uint32_t curWinPos = curPos + delay;
            while (curWinPos < H) {
                if (trySchedule<touchOccupancy, recordPort>(curWin[curWinPos], portMask)) {
                    schedCycle = curCycle + (curWinPos - curPos);
                    break;
                } else {
                    curWinPos++;
                }
            }
            if (curWinPos >= H) {
                uint32_t nextWinPos = curWinPos - H;
                while (nextWinPos < H) {
                    if (trySchedule<touchOccupancy, recordPort>(nextWin[nextWinPos], portMask)) {
                        schedCycle = curCycle + (nextWinPos + H - curPos);
                        break;
                    } else {
                        nextWinPos++;
                    }
                }
                if (nextWinPos >= H) {
                    schedCycle = curCycle + (nextWinPos + H - curPos);
                    UBWinIterator it = ubWin.lower_bound(schedCycle);
                    while (true) {
                        if (it == ubWin.end()) {
                            WinCycle wc = {0, 0};
                            bool success = trySchedule<touchOccupancy, recordPort>(wc, portMask);
                            assert(success);
                            ubWin.insert(std::pair<uint64_t, WinCycle>(schedCycle, wc));
                        } else if (it->first != schedCycle) {
                            WinCycle wc = {0, 0};
                            bool success = trySchedule<touchOccupancy, recordPort>(wc, portMask);
                            assert(success);
                            ubWin.insert(it /*hint, makes insert faster*/, std::pair<uint64_t, WinCycle>(schedCycle, wc));
                        } else {
                            if (!trySchedule<touchOccupancy, recordPort>(it->second, portMask)) {
                                // Try next cycle
                                it++;
                                schedCycle++;
                                continue;
                            }  // else scheduled correctly
                        }
                        break;
                    }
                    // info("Scheduled event in unbounded window, cycle %ld", schedCycle);
                }
            }
            if (touchOccupancy) occupancy++;
        }

        template <bool touchOccupancy, bool recordPort>
        inline uint8_t trySchedule(WinCycle& wc, uint8_t portMask) {
            static_assert(!(recordPort && !touchOccupancy), "Can't have recordPort and !touchOccupancy");
            if (touchOccupancy) {
                uint8_t availMask = (~wc.occUnits) & portMask;
                if (availMask) {
                    // info("PRE: occUnits=%x portMask=%x availMask=%x", wc.occUnits, portMask, availMask);
                    uint8_t firstAvail = __builtin_ffs(availMask) - 1;
                    // NOTE: This is not fair across ports. I tried round-robin scheduling, and there is no measurable difference
                    // (in our case, fairness comes from following program order)
                    if (recordPort) lastPort = firstAvail;
                    wc.occUnits |= 1 << firstAvail;
                    wc.count++;
                    // info("POST: occUnits=%x count=%x firstAvail=%d", wc.occUnits, wc.count, firstAvail);
                }
                return availMask;
            } else {
                // This is a shadow req, port has only 1 bit set
                uint8_t availMask = (~wc.occUnits) & portMask;
                wc.occUnits |= portMask;  // or anyway, no conditionals
                return availMask;
            }
        }
};

template<uint32_t SZ, uint32_t W>
class ReorderBuffer {
    private:
        uint64_t buf[SZ];
        uint64_t curRetireCycle;
        uint32_t curCycleRetires;
        uint32_t idx;

    public:
        ReorderBuffer() {
            for (uint32_t i = 0; i < SZ; i++) buf[i] = 0;
            idx = 0;
            curRetireCycle = 0;
            curCycleRetires = 1;
        }

        inline uint64_t minAllocCycle() {
            return buf[idx];
        }

        inline void markRetire(uint64_t minRetireCycle) {
            if (minRetireCycle <= curRetireCycle) {  // retire with bundle
                if (curCycleRetires == W) {
                    curRetireCycle++;
                    curCycleRetires = 0;
                } else {
                    curCycleRetires++;
                }

                /* No branches version (careful, width should be power of 2...)
                 * curRetireCycle += curCycleRetires/W;
                 * curCycleRetires = (curCycleRetires + 1) % W;
                 *  NOTE: After profiling, version with branch seems faster
                 */
            } else {  // advance
                curRetireCycle = minRetireCycle;
                curCycleRetires = 1;
            }

            buf[idx++] = curRetireCycle;
            if (idx == SZ) idx = 0;
        }
};

// Similar to ReorderBuffer, but must have in-order allocations and retires (--> faster)
template<uint32_t SZ>
class CycleQueue {
    private:
        uint64_t buf[SZ];
        uint32_t idx;

    public:
        CycleQueue() {
            for (uint32_t i = 0; i < SZ; i++) buf[i] = 0;
            idx = 0;
        }

        inline uint64_t minAllocCycle() {
            return buf[idx];
        }

        inline void markLeave(uint64_t leaveCycle) {
            //assert(buf[idx] <= leaveCycle);
            buf[idx++] = leaveCycle;
            if (idx == SZ) idx = 0;
        }
};

struct BblInfo;

class OOOCore : public Core {
    private:
        /* 1.11.47 (FIX-PRE-FLEET L203): per-core FPU capability, the E23
         * pattern. OOOCore and NullCore were the two selectable PE types the
         * 1.11.11 soft-float charge never reached -- an FPU-less OOO element
         * silently kept hardware-float timing. */
        bool coreHasFpu_ = true;
        uint32_t coreFpEmulCycles_ = 0;
    public:
        void setFpuCapability(bool hasFpu, uint32_t emulCycles) {
            coreHasFpu_ = hasFpu; coreFpEmulCycles_ = emulCycles;
        }

    private:
        FilterCache* l1i;
        FilterCache* l1d;

        uint64_t phaseEndCycle; //next stopping point

        uint64_t curCycle; //this model is issue-centric; curCycle refers to the current issue cycle
        uint64_t regScoreboard[MAX_REGISTERS]; //contains timestamp of next issue cycles where each reg can be sourced

        BblInfo* prevBbl;

        //Record load and store addresses
        Address loadAddrs[256];
        Address storeAddrs[256];
        uint32_t loads;
        uint32_t stores;

        uint64_t lastStoreCommitCycle;
        uint64_t lastStoreAddrCommitCycle; //tracks last store addr uop, all loads queue behind it

        //LSU queues are modeled like the ROB. Surprising? Entries are grabbed in dataflow order,
        //and for ordering purposes should leave in program order. In reality they are associative
        //buffers, but we split the associative component from the limited-size modeling.
        //NOTE: We do not model the 10-entry fill buffer here; the weave model should take care
        //to not overlap more than 10 misses.
        ReorderBuffer<32, 4> loadQueue;
        ReorderBuffer<32, 4> storeQueue;

        uint32_t curCycleRFReads; //for RF read stalls
        uint32_t curCycleIssuedUops; //for uop issue limits

        //This would be something like the Atom... (but careful, the iw probably does not allow 2-wide when configured with 1 slot)
        //WindowStructure<1024, 1 /*size*/, 2 /*width*/> insWindow; //this would be something like an Atom, except all the instruction pairing business...

        //Nehalem
        WindowStructure<1024, 36 /*size*/> insWindow; //NOTE: IW width is implicitly determined by the decoder, which sets the port masks according to uop type
        ReorderBuffer<128, 4> rob;

        // Agner's guide says it's a 2-level pred and BHSR is 18 bits, so this is the config that makes sense;
        // in practice, this is probably closer to the Pentium M's branch predictor, (see Uzelac and Milenkovic,
        // ISPASS 2009), which get the 18 bits of history through a hybrid predictor (2-level + bimodal + loop)
        // where a few of the 2-level history bits are in the tag.
        // Since this is close enough, we'll leave it as is for now. Feel free to reverse-engineer the real thing...
        // UPDATE: Now pht index is XOR-folded BSHR. This has 6656 bytes total -- not negligible, but not ridiculous.
        BranchPredictorPAg<11, 18, 14> branchPred;

        Address branchPc;  //0 if last bbl was not a conditional branch
        bool branchTaken;
        Address branchTakenNpc;
        Address branchNotTakenNpc;

        uint64_t decodeCycle;
        CycleQueue<28> uopQueue;  // models issue queue

        /* 1.9.33: `branches` added. The OOO core ran a predictor but only ever
         * counted its MISSES, so no total branch count existed and the power
         * model was left inferring one from the basic-block count. That proxy
         * assumes a BBL is a multi-instruction block ending in a control
         * transfer; on this QEMU-fed decode path a BBL averages ~1.09
         * instructions, so the proxy reported nearly every instruction as a
         * branch. The predictor already knows when it is looking at a branch --
         * count it. The in-order core has reported this all along. */
        uint64_t instrs, uops, bbls, approxInstrs, branches, mispredBranches;

        // QEMU-decoded OOO path instrumentation (option B). decodedBbls counts
        // BBLs that ran the real dataflow pipeline (decoded DynUops present);
        // syntheticBbls counts the 1-CPI fallback. memMismatch* count decoded
        // vs runtime load/store count divergences handled by the tolerant
        // drain. These prove the OOO improvement is decode-driven, not constant.
        uint64_t decodedBbls = 0, syntheticBbls = 0;
        /* 1.9.33: instructions attributable to SYNTHETIC timing-injection BBLs.
         *
         * The plugin charges coherence flush, kernel-launch and barrier latency
         * by manufacturing a BblInfo whose `instrs` field carries a CYCLE COUNT
         * (createSimpleBblInfo(cycles, ...)), because the 1-CPI fallback advances
         * curCycle by bblInstrs. Those cycles then land in `instrs` and are
         * indistinguishable from executed instructions.
         *
         * It is not a rounding error. In a co-simulation ROI the host executes
         * NO real code -- it prepared its data before roi_begin and then waits
         * for the device -- so all 32 of its ROI BBLs were synthetic and its
         * reported instrs of 781,398 was entirely injected cycles, about 24,400
         * per charge. The power model consumed that as an instruction count.
         *
         * Tracked separately rather than removed from `instrs`, because `instrs`
         * also feeds the reported IPC and the cycle-based gates; changing its
         * meaning would move published numbers.
         *
         * 1.11.57 (latent F014): THIS COUNTER IS DEAD, AND THE SENTENCE THAT
         * USED TO CLOSE THE PARAGRAPH -- "the power path subtracts this to
         * recover the real count" -- NAMED A DIFFERENT COUNTER. 1.11.16
         * removed the stat that exported this member (it inferred synthetic
         * blocks from oooBbl == 0, which also misclassified real >1024-insn
         * fallback TBs) and moved the job to the flag-based Core::mixSyn,
         * which counts BblInfo::synth and is emitted for every core type by
         * Core::mixInitStats. So what the power path subtracts is mixSyn;
         * this member is incremented at retire, rebased at roi_begin, and
         * read by nothing. It is kept only because the OOO retire path uses
         * it as a local running total in the same expression that advances
         * the 1-CPI fallback -- deleting it would be a behavioural edit to
         * the retire path for a documentation defect. What is corrected is
         * the claim: no consumer reads this. */
        uint64_t syntheticInstrs = 0;
        uint64_t roiBaseSyntheticInstrs = 0;
        uint64_t memMismatchLoads = 0, memMismatchStores = 0;
        // Expected rep-string (movs/stos) accesses drained through the
        // block-copy cost model (DynBbl.repInstrs > 0) -- NOT divergences.
        uint64_t repDrainedLoads = 0, repDrainedStores = 0;

        // Indirect control-flow prediction (BTB 512 entries + RAS 16 entries).
        // Fed by the plugin with resolved targets (kind codes >= 2 through the
        // branchPtr callback); a wrong target sets indirMispredPending, consumed
        // in bbl() as the same front-end redirect a conditional mispredict pays
        // (fetchCycle = lastCommitCycle; wrong-path ifetches NOT simulated for
        // indirects). Gated by PIMID_OOO_NOBRANCH like all branch modeling.
        IndirectPredictor<9, 16> indirPred;
        bool indirMispredPending = false;
        uint64_t indirBranches = 0, indirMispreds = 0;
        uint64_t rasReturns = 0, rasMispreds = 0;
        bool oooDebug = false;       // PIMID_OOO_DEBUG=1 -> per-BBL uop trace
        uint32_t oooDebugBbls = 0;

        // ROI baselines: snapshot at roi_begin so reported cycles/instrs reflect
        // ONLY the region of interest. roiBaseCycle snapshots the unhalted-cycle
        // metric (cRec.getUnhaltedCycles(curCycle)). 0 until roi_begin fires.
        uint64_t roiBaseInstrs = 0;
        uint64_t roiBaseCycle  = 0;
        /* 1.9.33: the ACTIVITY counters need the same ROI baseline that instrs
         * and cycles already had. Before this they were reported as raw
         * whole-run totals while instrs was reported ROI-windowed, so any ratio
         * taken between them divided a region-of-interest numerator by a
         * whole-run denominator. That is what made the measured instruction mix
         * nonsensical -- on one co-simulation dump, instrs 52,093 against bbls
         * 649,332 and uops 4,247,062, implying more basic blocks than
         * instructions. Same base, same window, or the ratio means nothing. */
        uint64_t roiBaseUops     = 0;
        uint64_t roiBaseCCycles  = 0;  // 1.11.9
        uint64_t roiBaseBbls     = 0;
        uint64_t roiBaseBranches = 0;
        uint64_t roiBaseMispred  = 0;
        uint64_t roiBaseIndir    = 0;   // 1.11.93 (F6): see in_order_core.h
        uint64_t roiBaseRas      = 0;

#ifdef OOO_STALL_STATS
        Counter profFetchStalls, profDecodeStalls, profIssueStalls;
#endif

        // Load-store forwarding
        // Just a direct-mapped array of last store cycles to 4B-wide blocks
        // (i.e., indexed by (addr >> 2) & (FWD_ENTRIES-1))
        struct FwdEntry {
            Address addr;
            uint64_t storeCycle;
            void set(Address a, uint64_t c) {addr = a; storeCycle = c;}
        };

        #define FWD_ENTRIES 32  // 2 lines, 16 4B entries/line
        FwdEntry fwdArray[FWD_ENTRIES];

        OOOCoreRecorder cRec;

    public:
        OOOCore(FilterCache* _l1i, FilterCache* _l1d, g_string& _name);

        void initStats(AggregateStat* parentStat);

        uint64_t getInstrs() const;
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

        // Virtual type check for use without RTTI (Pin 4.x requires -fno-rtti)
        OOOCore* asOOOCore() override { return this; }

        // Contention simulation interface
        bool hasContentionSim() const override { return true; }
        EventRecorder* getEventRecorder() override {return cRec.getEventRecorder();}
        void cSimStart() override;
        void cSimEnd() override;

        // Snapshot current counters as the ROI baseline (called on roi_begin).
        void markRoiBegin() override {
            roiBaseInstrs = instrs; roiBaseCycle = getCycles();  // adjusted clock: pre-ROI phantom excluded
            // 1.9.33: rebase the activity counters on the same window as instrs.
            roiBaseUops = uops; roiBaseBbls = bbls;
            roiBaseBranches = branches; roiBaseMispred = mispredBranches;
            roiBaseIndir = indirBranches; roiBaseRas = rasReturns;   // 1.11.93 (F6)
            roiBaseSyntheticInstrs = syntheticInstrs;  // 1.9.33
            /* 1.11.9 (audit): cCycles was "left absolute as diagnostic", but
             * the parser SUMS it with ROI-windowed cycles for
             * host_wall_cycles -- mixing bases. Rebase it like the rest. */
            roiBaseCCycles = cRec.getContentionCycles();
            mixMarkRoi();   // 1.11.10
        }

    private:
        inline void load(Address addr);
        inline void store(Address addr);

        /* NOTE: Analysis routines cannot touch curCycle directly, must use
         * advance() for long jumps or insWindow.advancePos() for 1-cycle
         * jumps.
         *
         * UPDATE: With decodeCycle, this difference is more serious. ONLY
         * cSimStart and cSimEnd should call advance(). advance() is now meant
         * to advance the cycle counters in the whole core in lockstep.
         */
        inline void advance(uint64_t targetCycle);

        // Predicated loads and stores call this function, gets recorded as a 0-cycle op.
        // Predication is rare enough that we don't need to model it perfectly to be accurate (i.e. the uops still execute, retire, etc), but this is needed for correctness.
        inline void predFalseLoad();
        inline void predFalseStore();

        inline void branch(Address pc, bool taken, Address takenNpc, Address notTakenNpc);

        // Indirect control-flow resolution (kind >= CF_IND_JMP): BTB/RAS
        // query+update; sets indirMispredPending on a wrong target.
        inline void ctrlFlow(uint32_t kind, Address pc, Address target, Address retAddr);

        inline void bbl(Address bblAddr, BblInfo* bblInfo);

        static void LoadFunc(THREADID tid, ADDRINT addr);
        static void StoreFunc(THREADID tid, ADDRINT addr);
        static void PredLoadFunc(THREADID tid, ADDRINT addr, BOOL pred);
        static void PredStoreFunc(THREADID tid, ADDRINT addr, BOOL pred);
        static void BblFunc(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo);
        static void BranchFunc(THREADID tid, ADDRINT pc, BOOL taken, ADDRINT takenNpc, ADDRINT notTakenNpc);
} ATTR_LINE_ALIGNED;  // Take up an int number of cache lines

#endif  // OOO_CORE_H_
