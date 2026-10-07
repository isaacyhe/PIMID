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

#ifndef CORE_H_
#define CORE_H_

#include <stdint.h>
#include "zsim_types.h"
#include "decoder_simple.h"
#include "g_std/g_string.h"
#include "stats.h"

/* 1.11.10 (#112): STATIC instruction-class census of this basic block,
 * filled once at decode from the class the decoder already assigns to every
 * instruction (x86_decoder.h OpClass) and then thrown away. Cores add these
 * to their own dynamic accumulators at retirement, so the mix McPAT is fed
 * is COUNTED, not the documented fractions it fell back to. uint16_t: a
 * basic block that holds 65k instructions of one class does not exist, and
 * the saturating adds below say so rather than wrapping silently.
 * Synthetic timing BBLs (createSimpleBblInfo) leave these zero -- they are
 * charges, not code, and must not enter the mix. */
struct BblInfo {
    uint32_t instrs;
    uint32_t bytes;
    uint16_t nInt;     // ALU/MOV/LEA/NOP/GENERIC (1.11.16: matches the classifier -- IDIV is in nMul)
    uint16_t nMul;     // IMUL + IDIV (integer multiply/divide unit)
    uint16_t nFp;      // FADD/FMUL/FDIV/FMA/VECALU/VECMOV
    uint16_t nBr;      // BRANCH
    uint16_t nLoad;
    uint16_t nStore;
    /* 1.11.16 (verification audit): explicit marker for INJECTED timing
     * charges (barrier latency, PCIe launch/transfer, drain trailers). Their
     * "instrs" are cycles, not executed code, and every core type must be
     * able to subtract them from its retired-instruction base -- before this
     * flag only OOOCore inferred it (from oooBbl absence, which also
     * misclassified real >1024-insn fallback TBs), so on alu/simple/null
     * PEs the injected cycles inflated the base the mix census is compared
     * against and could push the deficit guard into full rejection. Both
     * allocation sites gm_calloc, so decoded/real BBLs carry 0. Field packs
     * into existing padding: sizeof(BblInfo) stays 24, heap layout untouched. */
    uint16_t synth;    // 1 = injected timing charge, 0 = real code
    DynBbl oooBbl[0]; //0 bytes, but will be 1-sized when we have an element (and that element has variable size as well)
};

/* Analysis function pointer struct
 * As an artifact of having a shared code cache, we need these to be the same for different core types.
 */
struct InstrFuncPtrs {  // NOLINT(whitespace)
    void (*loadPtr)(THREADID, ADDRINT);
    void (*storePtr)(THREADID, ADDRINT);
    void (*bblPtr)(THREADID, ADDRINT, BblInfo*);
    void (*branchPtr)(THREADID, ADDRINT, BOOL, ADDRINT, ADDRINT);
    // Same as load/store functions, but last arg indicated whether op is executing
    void (*predLoadPtr)(THREADID, ADDRINT, BOOL);
    void (*predStorePtr)(THREADID, ADDRINT, BOOL);
    uint64_t type;
    uint64_t pad[1];
    //NOTE: By having the struct be a power of 2 bytes, indirect calls are simpler (w/ gcc 4.4 -O3, 6->5 instructions, and those instructions are simpler)
};


//TODO: Switch type to an enum by using sizeof macros...
#define FPTR_ANALYSIS (0L)
#define FPTR_JOIN (1L)
#define FPTR_NOP (2L)
#define FPTR_RETRY (3L)

// Forward declarations for type checking without RTTI
class InOrderCore;
class OOOCore;
class EventRecorder;

/* 1.8.7 co-sim MPI post-ROI protocol-tail RECEIPT, indexed by GLOBAL CORE INDEX
 * (== ALUCore::srcId_ == the plugin's cid). Kept in a FIXED file-scope array
 * rather than a Core member: adding a field to Core changes every core object's
 * size and shifts heap layout, which perturbs the contention sim's
 * address-ordered tie-breaking. Written by the plugin's
 * recordProtocolTailStats() (co-sim MPI only; stays 0 otherwise) and read by the
 * per-PE 'protocolTail' LambdaStat in ALUCore::initStats. Never alters 'cycles'.
 *
 * 1.11.20 (user decision D14) -- THE GUARANTEE THIS BUYS, STATED HONESTLY.
 * The original comment claimed the BSS array preserved bit-identity "vs the
 * 1.6/1.8.6 baseline", i.e. ACROSS versions. It does not, and 1.11.10 proved
 * it: adding pgAct/mix members to Core for power gating changed the layout
 * anyway. What we actually provide, and what every gate actually asserts:
 *
 *   SAME BINARY + same config          -> bit-identical, always.
 *   DIFFERENT BUILD of the same source -> may drift a few cycles.
 *
 * Measured: identical 1.11.15 source built in two trees gave 10164688 vs
 * 10164694 cycles on the deterministic 1-PE cell -- a 6-cycle cross-binary
 * drift with no source difference at all. Gates therefore always compare a
 * within-build NEW/REF pair, which is existing practice, now written down.
 *
 * Restoring the strict no-Core-members rule was considered and rejected: by
 * the measurement above it would NOT deliver cross-version bit-identity, and
 * it would churn all five core types to buy nothing. The array stays because
 * it is still the right place for a receipt; the overclaim is what changed. */
extern uint64_t g_mpiProtocolTailCyc[];

//Generic core class

class Core : public GlobAlloc {
    private:
        uint64_t lastUpdateCycles;
        uint64_t lastUpdateInstrs;

    protected:
        g_string name;

    public:
        /* 1.11.8 (#84): per-core active-phase tracker for power-gating
         * residency. Touched on the instruction-retirement path in each
         * core type's .cpp (which see zinfo); also marks the global
         * anyCore tracker there, from which all-idle overlap derives. */
        PhaseActivity pgAct;

        /* 1.11.10 (#112): dynamic instruction-mix accumulators, added from
         * each retired BBL's static census. Held in the base class so every
         * core type reports the same five stats and the parser needs one
         * rule. ROI-rebased alongside instrs (see markRoiBegin overrides). */
        uint64_t mixInt = 0, mixMul = 0, mixFp = 0, mixLd = 0, mixSt = 0;
        uint64_t mixBr = 0;   // 1.11.15: decoder-classified control transfers
        /* 1.11.16: injected timing charges (BblInfo::synth) counted HERE so
         * every core type reports syntheticInstrs, not just OOOCore -- on
         * alu/simple/null PEs the barrier/PCIe charges were inflating the
         * retired base with no way to subtract them. */
        uint64_t mixSyn = 0;
        uint64_t roiBaseMixInt = 0, roiBaseMixMul = 0, roiBaseMixFp = 0,
                 roiBaseMixLd = 0, roiBaseMixSt = 0, roiBaseMixBr = 0,
                 roiBaseMixSyn = 0;
        inline void mixAdd(const BblInfo* b) {
            mixInt += b->nInt; mixMul += b->nMul; mixFp += b->nFp;
            mixLd  += b->nLoad; mixSt += b->nStore; mixBr += b->nBr;
            if (b->synth) mixSyn += b->instrs;   // injected cycles-as-instrs
        }
        inline void mixMarkRoi() {
            roiBaseMixInt = mixInt; roiBaseMixMul = mixMul; roiBaseMixFp = mixFp;
            roiBaseMixLd  = mixLd;  roiBaseMixSt  = mixSt;  roiBaseMixBr = mixBr;
            roiBaseMixSyn = mixSyn;
            /* 1.11.18 (audit go-through): the PG residency counter rebases
             * WITH the rest. It was whole-run while every activity counter
             * it is weighed against is ROI-relative, so a pre-ROI warm-up
             * moved the residency without moving the work it prices. */
            roiBasePgActive = pgAct.activePhases;
        }
        uint64_t roiBasePgActive = 0;
        inline void mixInitStats(AggregateStat* coreStat) {
            struct M { const char* n; const char* d; uint64_t* v; };
            /* ROI-relative like instrs: the deltas are what the power model
             * consumes, and mixing bases is the defect 1.11.9 root-caused. */
            /* 1.11.57 (latent F030): a `static thread_local uint64_t dummy;`
             * followed by `(void)dummy;` used to sit here. It was read by
             * nothing, written by nothing and named after nothing -- the
             * residue of an earlier shape of this function -- and a
             * deliberately-unused variable in a stats emitter reads as a
             * layout pin someone must not touch. It is not one: it is TLS,
             * not heap, so it cannot influence the stats-object addresses the
             * contention sim's tie-breaking is sensitive to. Deleted. */
            auto add = [&](const char* n, const char* d, uint64_t* cur, uint64_t* base) {
                auto fn = [cur, base]() -> uint64_t {
                    return (*cur > *base) ? (*cur - *base) : 0;
                };
                auto* st = new LambdaStat<decltype(fn)>(fn);
                st->init(n, d);
                coreStat->append(st);
            };
            add("mixInt", "Integer-class instructions (measured)", &mixInt, &roiBaseMixInt);
            add("mixMul", "Multiply/divide-class instructions (measured)", &mixMul, &roiBaseMixMul);
            add("mixFp",  "FP/vector-class instructions (measured)", &mixFp, &roiBaseMixFp);
            add("mixLd",  "Load uops (measured)", &mixLd, &roiBaseMixLd);
            add("mixSt",  "Store uops (measured)", &mixSt, &roiBaseMixSt);
            add("mixBr",  "Branch-class instructions (measured)", &mixBr, &roiBaseMixBr);
            /* 1.11.18: PG residency, ROI-relative and emitted HERE so all
             * five core types share one definition (the five per-core
             * ProxyStat copies were whole-run). */
            /* 1.11.57 (latent F030): the cast STRIPS volatile from a counter
             * that PhaseActivity::touch advances with __sync_fetch_and_add
             * under a CAS. That is safe here and only here: the pointer is
             * dereferenced exclusively by the LambdaStat above, which runs at
             * stats-dump time on one thread with the simulation quiesced, so
             * there is no concurrent writer for the compiler to have cached
             * around. Said out loud because a volatile-stripping cast in a
             * multi-threaded counter path is otherwise exactly the shape of a
             * defect. */
            add("pgActivePhases", "Phases with retirement (PG residency)",
                (uint64_t*)&pgAct.activePhases, &roiBasePgActive);
            /* 1.11.16: flag-based, all core types. OOOCore's own oooBbl-null
             * inference (which also misclassified real >1024-insn fallback
             * TBs as synthetic) no longer emits a stat -- ONE producer, or
             * the parser would sum both and subtract twice. */
            add("syntheticInstrs", "Of instrs, injected timing charges (not executed code)", &mixSyn, &roiBaseMixSyn);
        }

        explicit Core(g_string& _name) : lastUpdateCycles(0), lastUpdateInstrs(0), name(_name) {}

        virtual uint64_t getInstrs() const = 0; // typically used to find out termination conditions or dumps
        virtual uint64_t getPhaseCycles() const = 0; // used by RDTSC faking --- we need to know how far along we are in the phase, but not the total number of phases
        virtual uint64_t getCycles() const = 0;

        /* 1.6 thread-MPI frozen-clock waits: rewind the cycle counter by the
         * raw growth a transport wait injected (park-rejoin fast-forward is
         * wall-dependent; the rendezvous ADVANCE re-places the clock from
         * deterministic arithmetic right after). No-op for cores with weave
         * event state (rewinding under scheduled events is unsafe) -- those
         * get frozen-clock support in 1.6.1. */
        virtual void pimidRewindCycles(uint64_t delta) { (void)delta; }

        /* 1.6.1 thread-MPI: set while the guest is parked in a transport wait
         * (COMM window). OOOCore::join consults it: a parked core's pipeline
         * is empty, so the post-wake join JUMPS to the phase clock instead of
         * simulating the pipeline across a wall-dependent gap (advance() there
         * converted wake luck into unhalted cycles -- the +-1-phase ooo
         * nondeterminism signature). */
        volatile bool pimidCommParked = false;

        virtual void initStats(AggregateStat* parentStat) = 0;
        virtual void contextSwitch(int32_t gid) = 0; //gid == -1 means descheduled, otherwise this is the new gid

        //Called by scheduler on every leave and join action, before barrier methods are called
        virtual void leave() {}
        virtual void join() {}

        virtual InstrFuncPtrs GetFuncPtrs() = 0;

        // Virtual type checks for use without RTTI (Pin 4.x requires -fno-rtti)
        virtual InOrderCore* asInOrderCore() { return nullptr; }
        virtual OOOCore* asOOOCore() { return nullptr; }

        // Virtual contention simulation interface (returns false if not supported)
        virtual bool hasContentionSim() const { return false; }
        virtual EventRecorder* getEventRecorder() { return nullptr; }
        virtual void cSimStart() {}
        virtual void cSimEnd() {}

        // Inject a delay (in cycles) into the core's cycle counter.
        // Used by MPI timing handlers and PE-MC remote access modeling.
        // Default: no-op (overridden by ALUCore, InOrderCore, OOOCore)
        virtual void addDelay(uint32_t /*cycles*/) {}

        // Snapshot the ROI baseline so per-core cycle/instr stats report only
        // the region of interest (roi_begin..roi_end). Default no-op.
        virtual void markRoiBegin() {}

        // 1.9.0 deterministic thread-MPI epoch pricing: the ROI baseline cycle
        // (set at markRoiBegin) and this core's global MemReq.srcId index. The
        // per-core roiRel axis (curCycle - roiBaseCycle) is the DETERMINISTIC,
        // cross-core-comparable stamp the epoch cut uses (the global numPhases is
        // wall-decoupled from a core's own curCycle at COMM-window parks). Default
        // 0 (non-ALU cores; only ALUCore overrides for the validated device path).
        virtual uint64_t getRoiBaseCycle() const { return 0; }
        virtual uint32_t getSrcId() const { return 0; }

        // Address-routed pricing DMA window (v1.1.1): while paused, loads and
        // stores are NOT priced by the serving memory -- used to bracket an
        // explicit staging memcpy whose transfer cost is already charged as a
        // sized M/D/1 link transfer (otherwise the copy loop would pay the
        // attach toll per element AND the link charge: double counting).
        // Default no-op (only ALUCore consults address-routed pricing).
        virtual void setMemPricingPaused(bool /*paused*/) {}
};

#endif  // CORE_H_

