/**
 * ZSim Trace-Driven Simulation Driver
 *
 * Standalone main() that drives ZSim from a PIMID binary trace file.
 * Provides the same simulation fidelity as the QEMU plugin (cores, caches,
 * memory controllers, Garnet NoC) but feeds events from a pre-recorded trace
 * instead of live QEMU execution.
 *
 * Usage:
 *   Called in-process via zsim_trace_run(cfgPath, tracePath, outputDir)
 *
 * Architecture:
 *   1. Parse args, set up shared memory (GlobSimInfo)
 *   2. SimInit(cfg, outputDir) -> creates cores, caches, MCs, network
 *   3. Read PIMID trace header + events
 *   4. Event loop: dispatch to ZSim core function pointers
 *   5. SimEnd() -> dump stats
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <atomic>

/* ZSim headers (compiled with ZSIM_USE_QEMU, no Pin dependency) */
#include "alu_core.h"
#include "null_core.h"
#include "constants.h"
#include "contention_sim.h"
#include "core.h"
#include "decoder_simple.h"
#include "ooo_core.h"      // 1.11.100 (H40): CtrlFlowKind, the terminator codes the branch feed carries
#include "x86_decoder.h"   // 1.11.100 (C6/H37): the same decoder the execution path uses
#include "dram_epoch_replay.h"   // 1.11.100 (H36): the replay window opens at roi_begin
#include <map>
#include <deque>   // 1.11.100 (H42): deferred events of parked threads
#include <fstream>
#include <string>
#include "event_queue.h"
#include "galloc.h"
#include "garnet_network.h"
#include "hierarchy_util.h"
#include "init.h"
#include "log.h"
#include "pad.h"
#include "process_tree.h"
#include "profile_stats.h"
#include "scheduler.h"
#include "stats.h"
#include "zsim.h"

/* ---- Globals (same as qemu_zsim_plugin.cpp) ---- */

GlobSimInfo* zinfo;
uint32_t procIdx;
uint32_t lineBits;
uint64_t procMask;
Core* cores[MAX_THREADS];
InstrFuncPtrs fPtrs[MAX_THREADS] ATTR_LINE_ALIGNED;

/* tid->cid mapping */
#define INVALID_CID ((uint32_t)-1)
#define UNINITIALIZED_CID ((uint32_t)-2)
static uint32_t cids[MAX_THREADS];

static volatile uint32_t perProcessEndFlag;

/* Thread state */
static bool thread_initialized[MAX_THREADS];

/* Single-active-thread protocol: at most one thread RUNNING in the barrier
 * at any time, preventing deadlock in the single-threaded trace driver. */
static constexpr uint32_t INVALID_TID = 0xFFFFFFFF;

/* ---- PIMID trace format (inline, avoids external header dependencies) ---- */

static const uint32_t PIMID_TRACE_MAGIC = 0x50494D54;  /* "PIMT" */

struct PimidTraceHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t header_flags;
    uint64_t num_events;
    uint64_t header_size;
    uint64_t num_pes;
    uint64_t first_cycle;
    uint64_t last_cycle;
    uint32_t reserved[4];
};

struct PimidTraceEvent {
    uint64_t cycle;
    uint64_t address;
    uint64_t aux_data;
    uint32_t pe_id;
    uint32_t size;
    uint32_t src_node;
    uint32_t dst_node;
    uint32_t reserved;
    uint16_t event_type;
    uint16_t flags;
};

/* Event type constants (match trace_format.h) */
enum PimidEventType : uint16_t {
    EVT_MEM_READ        = 0x0001,
    EVT_MEM_WRITE       = 0x0002,
    EVT_MEM_ATOMIC      = 0x0003,
    EVT_PIM_COMPUTE     = 0x0010,
    EVT_PIM_GATHER      = 0x0011,
    EVT_PIM_SCATTER     = 0x0012,
    EVT_PIM_REDUCE      = 0x0013,
    EVT_NET_SEND        = 0x0020,
    EVT_NET_RECV        = 0x0021,
    EVT_COMPUTE_INT     = 0x0030,
    EVT_COMPUTE_FP      = 0x0031,
    EVT_COMPUTE_VECTOR  = 0x0032,
    EVT_BARRIER         = 0x0040,
    EVT_TASK_START      = 0x0041,
    EVT_TASK_END        = 0x0042,
    EVT_OFFLOAD_START   = 0x0070,
    EVT_OFFLOAD_END     = 0x0071,
    EVT_ROI_BEGIN       = 0x0080,   // 1.11.100 (H36)
    EVT_ROI_END         = 0x0081,
};
static const uint16_t FLAG_DEVICE_DOMAIN = 0x0020;

/* 1.11.100 (review C6 / H37): the blocks of the trace -- instruction bytes
 * read from "<trace>.blocks" (written at translation by the trace plugin) and
 * decoded ONCE per block address with x86dec::createDecodedBblInfo, the
 * decoder the execution path runs, so the in-order and out-of-order cores
 * replay the real instruction stream. A block the side file does not carry
 * (or no side file at all) is a synthetic block of the event's instruction
 * count and byte size, and the run says how many. */
struct TraceBlock { uint32_t n = 0, bytes = 0; std::vector<uint8_t> lens; std::vector<uint8_t> data; };
/* 1.11.100 (H40): a block's TERMINATOR, classified from its last instruction's
 * bytes exactly as the execution plugin classifies a translation block at
 * translation (qemu_zsim_plugin.cpp tb_trans_cb): a conditional jcc gives a
 * direction feed, a direct call (E8) a RAS push, an indirect call (FF /2,/3)
 * a BTB check plus a RAS push, an indirect jmp (FF /4,/5) a BTB check, a ret
 * (C3/C2) a RAS pop. A direct jmp (E9/EB) is fed as CF_DIR_JMP since 1.12.1
 * (ticket #114): the decode resteer of the predictor-less in-order mode; the
 * OOO and PAg in-order cores ignore it. The outcome is
 * resolved from the NEXT block's start address and fed to the core through
 * branchPtr BEFORE that block's bbl(), as the execution path does. */
struct BlockTerm { bool cond = false; uint64_t brPc = 0, brTaken = 0, brFall = 0; uint8_t kind = 0; uint64_t termPc = 0, termRet = 0; };
struct CachedBlock { BblInfo* bbl = nullptr; BlockTerm term; };
/* keyed by (address, instruction count): a block translated again with another
 * length is another block, so the replay retires exactly the instructions the
 * trace recorded (the side file carries every translation) */
static std::map<std::pair<uint64_t, uint32_t>, TraceBlock> g_blocks;
static std::map<std::pair<uint64_t, uint32_t>, CachedBlock> g_bblCache;
static uint64_t g_decodedBlocks = 0, g_syntheticBlocks = 0;
static bool g_decodeBlocks = true;

static void classifyTerminator(const TraceBlock& b, uint64_t vaddr, BlockTerm& t) {
    if (b.n == 0) return;
    uint64_t lpc = vaddr;
    for (uint32_t i = 0; i + 1 < b.n; i++) lpc += b.lens[i];
    size_t lsz = b.lens[b.n - 1];
    if (lsz == 0 || lsz > 16) return;
    const uint8_t* lb = b.data.data() + (size_t)(b.n - 1) * 16;
    uint32_t p = 0;
    /* Skip legacy prefixes (segment/hint 2E/3E/26/36/64/65, opsize 66,
     * addrsize 67, rep F2/F3 -- e.g. the "rep ret" idiom F3 C3) and REX. */
    while (p < lsz && (lb[p] == 0x2E || lb[p] == 0x3E || lb[p] == 0x26 ||
                       lb[p] == 0x36 || lb[p] == 0x64 || lb[p] == 0x65 ||
                       lb[p] == 0x66 || lb[p] == 0x67 ||
                       lb[p] == 0xF2 || lb[p] == 0xF3)) p++;
    if (p < lsz && lb[p] >= 0x40 && lb[p] <= 0x4F) p++;        /* REX */
    int64_t rel = 0; bool isCond = false;
    if (p < lsz && lb[p] >= 0x70 && lb[p] <= 0x7F && (p + 1) < lsz) {
        rel = (int8_t)lb[p + 1]; isCond = true;                /* jcc rel8 */
    } else if ((p + 5) < lsz + 1 && lb[p] == 0x0F &&
               lb[p + 1] >= 0x80 && lb[p + 1] <= 0x8F && (p + 5) < lsz) {
        rel = (int32_t)((uint32_t)lb[p + 2] | ((uint32_t)lb[p + 3] << 8) |
                        ((uint32_t)lb[p + 4] << 16) | ((uint32_t)lb[p + 5] << 24));
        isCond = true;                                        /* jcc rel32 */
    }
    if (isCond) {
        t.cond = true; t.brPc = lpc; t.brFall = lpc + lsz; t.brTaken = lpc + lsz + (uint64_t)rel;
    } else if (p < lsz) {
        uint8_t opb = lb[p];
        if (opb == 0xE8) {                                    /* direct call */
            t.kind = CF_DIR_CALL; t.termPc = lpc; t.termRet = lpc + lsz;
        } else if (opb == 0xC3 || opb == 0xC2) {              /* ret */
            t.kind = CF_RET; t.termPc = lpc;
        } else if (opb == 0xFF && (p + 1) < lsz) {            /* grp5 */
            uint8_t ext = (lb[p + 1] >> 3) & 7;
            if (ext == 2 || ext == 3) {                       /* call r/m */
                t.kind = CF_IND_CALL; t.termPc = lpc; t.termRet = lpc + lsz;
            } else if (ext == 4 || ext == 5) {                /* jmp r/m */
                t.kind = CF_IND_JMP; t.termPc = lpc;
            }
        } else if (opb == 0xE9 || opb == 0xEB) {              /* direct jmp (1.12.1) */
            t.kind = CF_DIR_JMP; t.termPc = lpc;
        }
    }
}

static void loadTraceBlocks(const char* tracePath) {
    std::string bp = std::string(tracePath) + ".blocks";
    std::ifstream f(bp, std::ios::binary);
    if (!f) { info("[trace] no %s: compute blocks replay as synthetic blocks (instruction count and size only)", bp.c_str()); return; }
    uint64_t recs = 0;
    while (f) {
        uint64_t vaddr = 0; uint32_t n = 0, bytes = 0;
        if (!f.read(reinterpret_cast<char*>(&vaddr), 8)) break;
        f.read(reinterpret_cast<char*>(&n), 4); f.read(reinterpret_cast<char*>(&bytes), 4);
        if (!f || n == 0 || n > 1024) break;
        TraceBlock b; b.n = n; b.bytes = bytes; b.lens.resize(n); b.data.resize((size_t)n * 16);
        f.read(reinterpret_cast<char*>(b.lens.data()), n); f.read(reinterpret_cast<char*>(b.data.data()), (size_t)n * 16);
        if (!f) break;
        recs++;
        g_blocks.emplace(std::make_pair(vaddr, n), std::move(b));   // the first translation of an (address, length) wins
    }
    info("[trace] %lu block records read from %s (%lu distinct blocks); the decoder the execution path uses decodes them",
         (unsigned long)recs, bp.c_str(), (unsigned long)g_blocks.size());
}

static const CachedBlock& blockFor(uint64_t addr, uint32_t nInsns, uint32_t bytes) {
    const std::pair<uint64_t, uint32_t> key(addr, nInsns);
    auto it = g_bblCache.find(key);
    if (it != g_bblCache.end()) return it->second;
    CachedBlock cb;
    auto bi = g_blocks.find(key);
    if (g_decodeBlocks && bi != g_blocks.end() && bi->second.n > 0 && bi->second.n <= 1024) {
        const TraceBlock& b = bi->second;
        cb.bbl = x86dec::createDecodedBblInfo(addr, reinterpret_cast<const uint8_t (*)[16]>(b.data.data()), b.lens.data(), b.n, b.bytes);
        classifyTerminator(b, addr, cb.term);   // 1.11.100 (H40)
        g_decodedBlocks++;
    } else {
        cb.bbl = createSimpleBblInfo(nInsns > 0 ? nInsns : 1, bytes > 0 ? bytes : (nInsns > 0 ? nInsns : 1) * 4);
        g_syntheticBlocks++;
    }
    return g_bblCache.emplace(key, cb).first->second;
}

/* 1.11.100 (H40): the per-thread pending terminator, resolved by the next
 * block's address and fed through branchPtr as the execution plugin's
 * tb_exec_cb feeds it (same gates: an out-of-order or in-order core bound to
 * the thread, the PIMID_OOO_NOBRANCH / PIMID_INORDER_NOBRANCH escapes). With no
 * core bound (the thread rejoins with this block) the state is left as it is,
 * exactly as there. */
static bool     g_brPending[MAX_THREADS];
static uint64_t g_brPc[MAX_THREADS], g_brTaken[MAX_THREADS], g_brFall[MAX_THREADS];
static uint8_t  g_ctrlPending[MAX_THREADS];
static uint64_t g_ctrlPc[MAX_THREADS], g_ctrlRet[MAX_THREADS];
static uint64_t g_brFed = 0, g_ctrlFed = 0, g_roiInsns = 0, g_roiInsnsHeader = 0, g_feedDropped = 0;
static bool g_oooNoBranch = false, g_inorderNoBranch = false;
static bool g_dropFeedOnce[MAX_THREADS];   // set by a leave: the execution path's first block after a leave is not fed (H40)
static void feedTerminator(uint32_t tid, uint64_t nextAddr, const BlockTerm& t) {
    bool feed = false;
    if (cores[tid]) {
        if (cores[tid]->asOOOCore()) feed = !g_oooNoBranch;
        else if (cores[tid]->asInOrderCore()) feed = !g_inorderNoBranch;
    }
    if (!feed) return;
    if (g_dropFeedOnce[tid]) { g_dropFeedOnce[tid] = false; g_feedDropped++; return; }   // the pending state stays, as there
    if (g_brPending[tid]) {
        fPtrs[tid].branchPtr(tid, g_brPc[tid], (nextAddr == g_brTaken[tid]) ? 1 : 0, g_brTaken[tid], g_brFall[tid]);
        g_brFed++;
    }
    if (g_ctrlPending[tid]) {
        fPtrs[tid].branchPtr(tid, g_ctrlPc[tid], g_ctrlPending[tid], nextAddr, g_ctrlRet[tid]);
        g_ctrlPending[tid] = 0;
        g_ctrlFed++;
    }
    if (t.cond) { g_brPending[tid] = true; g_brPc[tid] = t.brPc; g_brTaken[tid] = t.brTaken; g_brFall[tid] = t.brFall; }
    else g_brPending[tid] = false;
    if (t.kind) { g_ctrlPending[tid] = t.kind; g_ctrlPc[tid] = t.termPc; g_ctrlRet[tid] = t.termRet; }
}

/* 1.11.100 (review H38): the host and device core masks of the run, built
 * as the execution plugin builds them (ALU and null cores are device
 * elements), and each thread's domain. A thread whose events carry
 * FLAG_DEVICE_DOMAIN (an offload region, the ROI in device scope) runs on
 * the device mask, as the execution path migrates it. */
static g_vector<bool> g_hostMask, g_deviceMask;
static bool g_hasDevice = false, g_hasHost = false;
static bool thread_on_device[MAX_THREADS];
static uint64_t g_migrations = 0;
static void buildDomainMasks() {
    g_hostMask.resize(zinfo->numCores, false); g_deviceMask.resize(zinfo->numCores, false);
    for (uint32_t i = 0; i < zinfo->numCores; i++) {
        if (dynamic_cast<ALUCore*>(zinfo->cores[i]) || dynamic_cast<NullCore*>(zinfo->cores[i])) g_deviceMask[i] = true; else g_hostMask[i] = true;
    }
    for (auto b : g_deviceMask) if (b) g_hasDevice = true;
    for (auto b : g_hostMask) if (b) g_hasHost = true;
    /* a device-only or host-only machine: every thread uses the one mask */
    if (!g_hasDevice) g_deviceMask = g_hostMask;
    if (!g_hasHost) g_hostMask = g_deviceMask;
}
static uint64_t g_barriersTaken = 0, g_barriersSkipped = 0, g_roiBegins = 0, g_roiEnds = 0;
static bool g_roiDeviceWindow = false, g_cosimNoOffload = false;   // 1.11.100 (H43): the ROI window is the offload window

/* ---- CID/Core management (mirrors qemu_zsim_plugin.cpp) ---- */

static inline void clearCid(uint32_t tid) {
    cids[tid] = INVALID_CID;
    cores[tid] = nullptr;
}

static inline void setCid(uint32_t tid, uint32_t cid) {
    cids[tid] = cid;
    cores[tid] = zinfo->cores[cid];
}

uint32_t getCid(uint32_t tid) {
    return cids[tid];
}

/* ---- Termination checking ---- */

static void CheckForTermination() {
    if (zinfo->terminationConditionMet) return;

    if (zinfo->maxPhases && zinfo->numPhases >= zinfo->maxPhases) {
        zinfo->terminationConditionMet = true;
        info("Max phases reached (%ld)", zinfo->numPhases);
        return;
    }

    if (zinfo->maxTotalInstrs) {
        uint64_t totalInstrs = 0;
        for (uint32_t i = 0; i < zinfo->numCores; i++) {
            totalInstrs += zinfo->cores[i]->getInstrs();
        }
        if (totalInstrs >= zinfo->maxTotalInstrs) {
            zinfo->terminationConditionMet = true;
            info("Max total instructions reached (%ld)", totalInstrs);
            return;
        }
    }

    if (zinfo->externalTermPending) {
        zinfo->terminationConditionMet = true;
        info("Terminating due to external notification");
    }
}

/* ---- Phase actions ---- */

void EndOfPhaseActions() {
    zinfo->profSimTime->transition(PROF_WEAVE);
    CheckForTermination();
    zinfo->contentionSim->simulatePhase(zinfo->globPhaseCycles + zinfo->phaseLength);

    zinfo->eventQueue->tick();
    zinfo->profSimTime->transition(PROF_BOUND);
}

/* ---- Barrier (phase synchronization) ---- */

/* ---- SimEnd ---- */

void SimEnd() {
    if (__sync_bool_compare_and_swap(&perProcessEndFlag, 0, 1) == false) {
        return;  /* Already ended */
    }

    info("Dumping termination stats");
    zinfo->trigger = 20000;
    for (StatsBackend* backend : *(zinfo->statsBackends)) {
        /* 1.11.100 (H41): as the execution plugin's dumpTerminationStats --
         * replay the controller's pending epochs and fold its totals before
         * the dump. Without it the trace path reported a replay with zero
         * requests and zero epochs for the ROI (the pre-ROI epochs were
         * replayed at roi_begin; the ROI's own were never replayed). */
        if (zinfo->dramReplay) zinfo->dramReplay->finalize();
        backend->dump(false);
    }

    if (zinfo->garnetNetwork) {
        // 1.11.92 (F11): replay the final phase's pending records first.
        zinfo->garnetNetwork->drainPendingRecords(false, zinfo->phaseLength,
                                                  zinfo->numPhases + 1);
        zinfo->garnetNetwork->setTotalCycles(zinfo->globPhaseCycles);
        std::string garnetStatsPath = std::string(zinfo->outputDir) + "/garnet_stats.txt";
        zinfo->garnetNetwork->writeStatsFile(garnetStatsPath.c_str());
        zinfo->garnetNetwork->printStats();
    }

    if (zinfo->sched) zinfo->sched->notifyTermination();
}

/* ---- NOP function pointers (for unscheduled threads) ---- */

static void NopLoad(THREADID, ADDRINT) {}
static void NopStore(THREADID, ADDRINT) {}
static void NopBbl(THREADID, ADDRINT, BblInfo*) {}
static void NopBranch(THREADID, ADDRINT, BOOL, ADDRINT, ADDRINT) {}
static void NopPredLoad(THREADID, ADDRINT, BOOL) {}
static void NopPredStore(THREADID, ADDRINT, BOOL) {}

static InstrFuncPtrs nopPtrs = {NopLoad, NopStore, NopBbl, NopBranch,
                                 NopPredLoad, NopPredStore, FPTR_NOP, {0}};

/* ---- Join function pointers ---- */

static void Join(uint32_t tid);  // Forward declaration

static void JoinLoad(THREADID tid, ADDRINT addr) {
    Join(tid);
    fPtrs[tid].loadPtr(tid, addr);
}

static void JoinStore(THREADID tid, ADDRINT addr) {
    Join(tid);
    fPtrs[tid].storePtr(tid, addr);
}

static void JoinBbl(THREADID tid, ADDRINT addr, BblInfo* bbl) {
    Join(tid);
    fPtrs[tid].bblPtr(tid, addr, bbl);
}

static InstrFuncPtrs joinPtrs = {JoinLoad, JoinStore, JoinBbl, NopBranch,
                                  NopPredLoad, NopPredStore, FPTR_JOIN, {0}};

/* 1.11.100 (H42): the execution path's phase barrier, driven by ONE host
 * thread. Every thread stays joined to its core; a core that crosses the
 * phase end PARKS its thread (TakeBarrier counts the crossing; the thread's
 * later events are deferred) and the driver ends the phase when every joined
 * thread is parked -- the barrier's own condition. The old driver ran one
 * thread at a time, left the previous thread's core at every switch, and let
 * the single active core end the phase: the other cores lost the rest of
 * every phase they had not reached, which inflated the in-order replay's
 * cycles by 10% on the 16-thread probe (one thread: exact). A thread with no
 * event for IDLE_LIMIT events of the others is blocked (a futex wait) and
 * leaves its core, as the execution path's watchdog forces it to. */
static std::deque<PimidTraceEvent> g_deferred[MAX_THREADS];
static uint32_t g_crossings[MAX_THREADS];
static uint64_t g_lastReadSeq[MAX_THREADS];
static uint64_t g_eventsRead = 0, g_phaseEnds = 0, g_deferredEvents = 0, g_deferredPeak = 0, g_idleLeaves = 0;
static const uint64_t IDLE_LIMIT = 262144;
static uint64_t g_eventsProcessed = 0, g_memReads = 0, g_memWrites = 0, g_computes = 0, g_barriers = 0, g_skippedEvents = 0, g_hierCycles = 0;

static inline bool joinedThread(uint32_t t) { uint32_t c = getCid(t); return c != INVALID_CID && c != UNINITIALIZED_CID; }
static inline bool parkedThread(uint32_t t) { return g_crossings[t] > 0; }
static void leaveThread(uint32_t tid) {
    uint32_t cid = getCid(tid);
    if (cid == INVALID_CID || cid == UNINITIALIZED_CID) return;
    zinfo->sched->leave(procIdx, tid, cid);
    fPtrs[tid] = joinPtrs;
    clearCid(tid);
    g_dropFeedOnce[tid] = true;
}
static bool anyJoined() { for (uint32_t t = 0; t < zinfo->numCores; t++) if (joinedThread(t)) return true; return false; }
static bool allJoinedParked() { for (uint32_t t = 0; t < zinfo->numCores; t++) if (joinedThread(t) && !parkedThread(t)) return false; return true; }
static bool anyDeferred() { for (uint32_t t = 0; t < zinfo->numCores; t++) if (!g_deferred[t].empty()) return true; return false; }
/* what Scheduler::callback does when the last thread arrives at the barrier:
 * the simulator's end-of-phase actions (the weave), then the phase counters */
static void endPhase() {
    EndOfPhaseActions();
    zinfo->numPhases++;
    zinfo->globPhaseCycles += zinfo->phaseLength;
    for (uint32_t t = 0; t < zinfo->numCores; t++) if (g_crossings[t] > 0) g_crossings[t]--;
    g_phaseEnds++;
}
/* joined threads with no event before the cut / the end of the file / for
 * IDLE_LIMIT events are blocked: they leave, as under the execution path's
 * watchdog, so the phase can end */
static void leaveIdleThreads(bool everyEmpty) {
    for (uint32_t t = 0; t < zinfo->numCores; t++)
        if (joinedThread(t) && !parkedThread(t) && g_deferred[t].empty() &&
            (everyEmpty || g_eventsRead - g_lastReadSeq[t] > IDLE_LIMIT)) { leaveThread(t); g_idleLeaves++; }
}

uint32_t TakeBarrier(uint32_t tid, uint32_t cid) {
    /* 1.11.100 (H42): the core crossed the phase end. The execution path's
     * thread blocks in the scheduler's barrier until every thread arrives;
     * one host thread cannot block, so the thread is parked (one crossing
     * per call: a core that crosses two phase ends in one block waits for
     * two) and the driver ends the phase when all joined threads are parked.
     * The scheduler's sync is not called: the barrier never ends a phase on
     * its own (checkEndPhase needs a waiting thread). The (since retired,
     * 1.12.1) SimpleCore::BblFunc's
     * debug assertion that the global phase advanced per crossing does not
     * hold for a second crossing in one block (release builds). */
    g_crossings[tid]++;
    if (zinfo->terminationConditionMet) {
        info("Termination condition met, tid %d leaving", tid);
        leaveThread(tid);
        /* INVALID_CID: the callers' phase-end loops break instead of calling
         * TakeBarrier again on a LEFT thread. */
        return INVALID_CID;
    }
    return cid;
}

static void Join(uint32_t tid) {
    /* 1.11.100 (H42): every thread stays joined (one per core). A thread
     * beyond its mask's cores would wait in the scheduler for a context,
     * which one host thread cannot do: refuse instead of hanging. */
    {
        const g_vector<bool>& m = thread_on_device[tid] ? g_deviceMask : g_hostMask;
        uint32_t cap = 0, used = 0;
        for (uint32_t c = 0; c < zinfo->numCores; c++) if (m[c]) cap++;
        for (uint32_t t = 0; t < zinfo->numCores; t++) { uint32_t c = getCid(t); if (c != INVALID_CID && c != UNINITIALIZED_CID && c < zinfo->numCores && m[c]) used++; }
        if (used >= cap) panic("[trace] thread %u needs a core but all %u cores of its %s mask hold a thread: the replay runs one thread per core (H42)", tid, cap, thread_on_device[tid] ? "device" : "host");
    }

    uint32_t cid = zinfo->sched->join(procIdx, tid);
    setCid(tid, cid);

    if (unlikely(zinfo->terminationConditionMet)) {
        info("Caught termination on join, tid %d", tid);
        zinfo->sched->leave(procIdx, tid, cid);
        clearCid(tid);
        return;
    }

    fPtrs[tid] = cores[tid]->GetFuncPtrs();
}

/* ---- Thread initialization ---- */

static void initThread(uint32_t tid) {
    if (thread_initialized[tid]) return;
    thread_initialized[tid] = true;
    /* 1.11.100 (H38): the mask of the thread's domain, not the whole process */
    zinfo->sched->start(procIdx, tid, thread_on_device[tid] ? g_deviceMask : g_hostMask);
    fPtrs[tid] = joinPtrs;
    clearCid(tid);
    info("Trace thread %d initialized (domain=%s)", tid, thread_on_device[tid] ? "DEVICE" : "HOST");
}

/* 1.11.100 (H38): move a thread between the host and device masks, as the
 * execution plugin's migrateThreadToDomain does: leave, finish, restart. */
static void migrateThread(uint32_t tid, bool toDevice) {
    if (thread_on_device[tid] == toDevice) return;
    thread_on_device[tid] = toDevice;
    if (!(g_hasDevice && g_hasHost)) return;   // one mask: nothing to move between
    if (thread_initialized[tid]) {
        uint32_t cid = getCid(tid);
        if (cid != INVALID_CID && cid != UNINITIALIZED_CID) {
            zinfo->sched->leave(procIdx, tid, cid);
        }
        zinfo->sched->finish(procIdx, tid);
        thread_initialized[tid] = false;
        clearCid(tid);
    }
    g_migrations++;
    initThread(tid);
}

/* 1.11.100 (H36): roi_begin as the execution path does it -- every core's
 * baseline, the power-gating window, the traffic-counter groups, the fabric
 * and the controller replay; roi_end ends the simulation (device scope). */
static void traceRoiBegin() {
    for (uint32_t c = 0; c < zinfo->numCores; c++) if (zinfo->cores[c]) zinfo->cores[c]->markRoiBegin();
    zinfo->pgres.markRoi(zinfo->numPhases, zinfo->globPhaseCycles);
    uint32_t groups = 0;
    if (zinfo->roiRebaseStats) for (AggregateStat* g : *zinfo->roiRebaseStats) { if (g) { g->roiRebase(); groups++; } }
    if (zinfo->garnetNetwork) {
        zinfo->garnetNetwork->drainPendingRecords(false, zinfo->phaseLength, zinfo->numPhases);
        zinfo->garnetNetwork->markRoiBegin();
    }
    if (zinfo->dramReplay) zinfo->dramReplay->markRoi();
    zinfo->hierarchy.mpiNocBaselined = true;
    zinfo->hierarchy.mpiNocRoiBasePhase = zinfo->numPhases;
    g_roiBegins++;
    info("[roi] trace replay: roi_begin -- %u cores baselined, %u stat groups rebased (1.11.100 H36)", zinfo->numCores, groups);
}

/* ---- BblInfo cache for synthetic compute events ---- */

static BblInfo* makeSyntheticBbl(uint64_t instrCount) {
    uint32_t instrs = (instrCount > 0) ? static_cast<uint32_t>(instrCount) : 1;
    uint32_t bytes = instrs * 4;  /* ~4 bytes per instruction estimate */
    return createSimpleBblInfo(instrs, bytes);
}

/* ---- Hierarchy latency (delegated to hierarchy_util.h) ---- */

static uint64_t computeHierarchyLatency(uint32_t src_pe, uint32_t dst_pe,
                                          uint32_t coreId = 0) {
    if (!zinfo->hierarchy.enabled || src_pe == dst_pe) return 0;

    // Simple model hierarchy traversal
    return computePEtoPELatency(
        src_pe, dst_pe,
        zinfo->hierarchy.levelLatency, zinfo->hierarchy.bridgeLatency,
        zinfo->hierarchy.placementLevel, zinfo->hierarchy.subarraysPerBank,
        zinfo->hierarchy.banksPerBG, zinfo->hierarchy.bgPerChip,
        zinfo->hierarchy.peMemMapSize > 0 ? zinfo->hierarchy.peMemMapOffsets : nullptr,
        zinfo->hierarchy.peMemMapSize > 0 ? zinfo->hierarchy.peMemMapData : nullptr,
        zinfo->hierarchy.peMemMapSize,
        zinfo->hierarchy.connectionMode,
        zinfo->hierarchy.localLinkLatency,
        zinfo->hierarchy.chipsPerRank,
        zinfo->hierarchy.ranksPerChannel);
}

/* ---- Stats output (written as key=value for PIMID to parse) ---- */

static void writeSimpleStats(const char* outputDir, uint64_t hierCycles = 0) {
    char path[512];
    snprintf(path, sizeof(path), "%s/zsim_trace_stats.txt", outputDir);

    FILE* fp = fopen(path, "w");
    if (!fp) {
        warn("Could not write stats to %s", path);
        return;
    }

    uint64_t totalInstrs = 0;
    uint64_t totalCycles = 0;
    for (uint32_t i = 0; i < zinfo->numCores; i++) {
        uint64_t ci = zinfo->cores[i]->getInstrs();
        uint64_t cc = zinfo->cores[i]->getCycles();
        totalInstrs += ci;
        if (cc > totalCycles) totalCycles = cc;
        fprintf(fp, "core.%u.instrs = %lu\n", i, ci);
        fprintf(fp, "core.%u.cycles = %lu\n", i, cc);
        if (ci > 0) {
            fprintf(fp, "core.%u.ipc = %.4f\n", i, (double)ci / (double)cc);
        }
    }

    fprintf(fp, "total.instrs = %lu\n", totalInstrs);
    fprintf(fp, "total.cycles = %lu\n", totalCycles);
    if (totalInstrs > 0 && totalCycles > 0) {
        fprintf(fp, "total.ipc = %.4f\n", (double)totalInstrs / (double)totalCycles);
    }
    fprintf(fp, "sim.phases = %lu\n", zinfo->numPhases);
    fprintf(fp, "sim.phase_cycles = %lu\n", zinfo->globPhaseCycles);
    if (hierCycles > 0)
        fprintf(fp, "hierarchy.total_cycles = %lu\n", hierCycles);

    fclose(fp);
    info("Stats written to %s", path);
}

/* ---- Library entry point ---- */

/* ---- 1.11.100 (H42): one event, on its thread's core ---- */
static void dispatchEvent(const PimidTraceEvent& event) {
    uint32_t tid = event.pe_id % zinfo->numCores;
    /* 1.11.100 (H38): the event's domain decides the thread's mask. In
     * device scope the ROI is the offload boundary: every in-ROI event is
     * a device event. */
    {
        /* 1.11.100 (H43): in system scope the ROI opens the device window as
         * the execution co-simulation opens it (cosimRoiBeginOffload: the
         * opener's domain flips to DEVICE at roi_begin and threads spawned
         * inside inherit it; PIMID_COSIM_NO_OFFLOAD keeps the host); the
         * WORK brackets flag their events in the trace itself. The
         * co-simulation's flush, launch and transfer charges at the boundary
         * are not replayed (OPEN). */
        const bool devEvent = (event.flags & FLAG_DEVICE_DOMAIN) != 0 || (!g_hasHost) || g_roiDeviceWindow;
        if (devEvent != thread_on_device[tid]) migrateThread(tid, devEvent);
    }
    if (!thread_initialized[tid]) {
        initThread(tid);
    }

    /* Ensure thread is joined (scheduled on a core) */
    if (fPtrs[tid].type == FPTR_JOIN) {
        Join(tid);
        if (zinfo->terminationConditionMet) return;
    }

    /* 1.11.100 (H40): the previous block's terminator resolves against
     * THIS block's start address and is fed to the core BEFORE the
     * block's bbl(), as the execution plugin's tb_exec_cb feeds it. Fed
     * AFTER the join: the driver runs one thread at a time and unbinds
     * the previous thread's core at every switch, which is the driver's
     * own mechanism, not a leave of the program (fed before the join,
     * 39% of the ROI's branches never reached the predictor on the
     * probe). The program's own leaves -- the recorded syscalls -- drop
     * the first block's feed as the execution path drops it (H39). */
    const CachedBlock* curBlock = nullptr;
    if (event.event_type == EVT_COMPUTE_INT || event.event_type == EVT_COMPUTE_FP || event.event_type == EVT_COMPUTE_VECTOR) {
        curBlock = &blockFor(event.address, (uint32_t)event.aux_data, event.size);
        feedTerminator(tid, event.address, curBlock->term);
        if (g_roiBegins > 0 && g_roiEnds == 0) g_roiInsns += event.aux_data;   // the trace's own ROI instruction count, file order
    }

    switch (event.event_type) {
        case EVT_MEM_READ:
            fPtrs[tid].loadPtr(tid, event.address);
            g_memReads++;
            break;

        case EVT_MEM_WRITE:
            fPtrs[tid].storePtr(tid, event.address);
            g_memWrites++;
            break;

        case EVT_MEM_ATOMIC:
            fPtrs[tid].loadPtr(tid, event.address);
            fPtrs[tid].storePtr(tid, event.address);
            g_memReads++;
            g_memWrites++;
            break;

        case EVT_COMPUTE_INT:
        case EVT_COMPUTE_FP:
        case EVT_COMPUTE_VECTOR: {
            /* 1.11.100 (C6/H37): the executed block, decoded from its bytes */
            fPtrs[tid].bblPtr(tid, event.address, curBlock->bbl);
            g_computes++;
            break;
        }
        case EVT_ROI_BEGIN:   // 1.11.100 (H36)
            g_roiDeviceWindow = !g_cosimNoOffload;   // H43: the offload window opens here (co-sim)
            traceRoiBegin();
            break;
        case EVT_ROI_END: {   // 1.11.100 (H36): the statistics freeze here, as at the execution path's roi_end
            g_roiDeviceWindow = false;
            g_roiEnds++;
            info("[roi] trace replay: roi_end -- the simulation ends here (device scope) (1.11.100 H36)");
            zinfo->terminationConditionMet = true;
            break;
        }

        case EVT_PIM_COMPUTE: {
            /* PIM compute: local only, no data movement */
            BblInfo* bbl = makeSyntheticBbl(event.aux_data > 0 ? event.aux_data : 1);
            fPtrs[tid].bblPtr(tid, event.address, bbl);
            if (zinfo->terminationConditionMet) break;
            if (event.address != 0) {
                fPtrs[tid].loadPtr(tid, event.address);
                g_memReads++;
            }
            g_computes++;
            break;
        }

        case EVT_PIM_GATHER:
        case EVT_PIM_SCATTER:
        case EVT_PIM_REDUCE: {
            /* PIM data movement: compute + memory access + hierarchy latency */
            BblInfo* bbl = makeSyntheticBbl(event.aux_data > 0 ? event.aux_data : 1);
            fPtrs[tid].bblPtr(tid, event.address, bbl);
            if (zinfo->terminationConditionMet) break;
            if (event.address != 0) {
                fPtrs[tid].loadPtr(tid, event.address);
                g_memReads++;
            }
            g_computes++;
            /* Add hierarchy latency for inter-PE data movement */
            if (zinfo->hierarchy.enabled && event.src_node != event.dst_node) {
                uint64_t hlat = computeHierarchyLatency(event.src_node, event.dst_node, tid);
                if (hlat > 0) {
                    BblInfo* hbbl = makeSyntheticBbl(hlat);
                    fPtrs[tid].bblPtr(tid, 0, hbbl);
                    if (zinfo->terminationConditionMet) break;
                    g_hierCycles += hlat;
                }
            }
            break;
        }

        case EVT_NET_SEND:
        case EVT_NET_RECV:
            /* Network events: model as memory accesses to the address.
             * The Garnet NoC will intercept via the memory hierarchy. */
            if (event.event_type == EVT_NET_SEND) {
                fPtrs[tid].storePtr(tid, event.address);
                g_memWrites++;
            } else {
                fPtrs[tid].loadPtr(tid, event.address);
                g_memReads++;
            }
            /* Add hierarchy latency for inter-PE communication */
            if (zinfo->hierarchy.enabled && event.src_node != event.dst_node) {
                uint64_t hlat = computeHierarchyLatency(event.src_node, event.dst_node, tid);
                if (hlat > 0) {
                    BblInfo* hbbl = makeSyntheticBbl(hlat);
                    fPtrs[tid].bblPtr(tid, 0, hbbl);
                    if (zinfo->terminationConditionMet) break;
                    g_hierCycles += hlat;
                }
            }
            break;

        case EVT_BARRIER: {
            /* 1.11.100 (H39, root cause of the in-order/OOO replay crash):
             * in this trace format a BARRIER event is a thread-creating
             * syscall (clone/fork/vfork, qemu_trace_plugin.c syscall_cb).
             * The execution path leaves the core around a syscall and
             * joins again after it; the phase ends only when a core's own
             * clock crosses the phase end (the cores' BblFunc calls
             * TakeBarrier). The old driver called TakeBarrier on the
             * event, forcing a phase end at whatever cycle the core had
             * reached; the recorder's phase start then found a RUNNING
             * core behind the phase end (core_recorder.cpp cSimStart) and
             * every in-order and out-of-order replay died there. Now the
             * thread leaves its core; its next event joins it again
             * (FPTR_JOIN). */
            if (joinedThread(tid)) {
                leaveThread(tid);   // H40: the first block after the leave is not fed, as in execution
                g_barriersTaken++;
            } else {
                g_barriersSkipped++;
            }
            g_barriers++;
            break;
        }

        case EVT_OFFLOAD_START:
        case EVT_OFFLOAD_END:
        case EVT_TASK_START:
        case EVT_TASK_END:
            /* Informational -- no simulation effect in trace replay */
            break;

        default:
            /* OpenMP, MPI, other events: skip in ZSim replay */
            g_skippedEvents++;
            break;
    }

    g_eventsProcessed++;
    if (g_eventsProcessed % 1000000 == 0) info("Processed %lu events...", g_eventsProcessed);
}

int zsim_trace_run(const char* cfgPath, const char* tracePath, const char* outputDir) {
    /* Ensure output directory exists */
    mkdir(outputDir, 0755);

    /* Open trace file and validate header */
    FILE* trace_fp = fopen(tracePath, "rb");
    if (!trace_fp) {
        fprintf(stderr, "Error: Cannot open trace file: %s\n", tracePath);
        return 1;
    }

    PimidTraceHeader header;
    if (fread(&header, sizeof(header), 1, trace_fp) != 1) {
        fprintf(stderr, "Error: Cannot read trace header\n");
        fclose(trace_fp);
        return 1;
    }

    if (header.magic != PIMID_TRACE_MAGIC) {
        fprintf(stderr, "Error: Invalid trace magic: 0x%08X (expected 0x%08X)\n",
                header.magic, PIMID_TRACE_MAGIC);
        fclose(trace_fp);
        return 1;
    }

    fprintf(stdout, "zsim_trace: %s\n", tracePath);
    fprintf(stdout, "  Events:  %lu\n", header.num_events);
    fprintf(stdout, "  PEs:     %lu\n", header.num_pes);
    fprintf(stdout, "  Cycles:  %lu - %lu\n", header.first_cycle, header.last_cycle);
    /* 1.11.100 (H40): the generator's own ROI instruction count, from the
     * header's metadata (file order, under its write lock); the replay's count
     * over the same file must equal it -- both are printed at the end. */
    {
        long pos = ftell(trace_fp);
        char meta[513]; memset(meta, 0, sizeof(meta));
        if (fread(meta, 1, 512, trace_fp) == 512) {
            const char* k = strstr(meta, "roi_insns: ");
            if (k) g_roiInsnsHeader = strtoull(k + 11, nullptr, 10);
        }
        fseek(trace_fp, pos, SEEK_SET);
    }

    /* Seek past YAML metadata to binary events */
    if (fseek(trace_fp, (long)header.header_size, SEEK_SET) != 0) {
        fprintf(stderr, "Error: Cannot seek to event data at offset %lu\n", header.header_size);
        fclose(trace_fp);
        return 1;
    }

    /* Initialize shared memory */
    loadTraceBlocks(tracePath);   // 1.11.100 (C6/H37)
    if (getenv("PIMID_TRACE_NO_DECODE")) { g_decodeBlocks = false; info("[trace] PIMID_TRACE_NO_DECODE: synthetic blocks"); }
    g_oooNoBranch = (getenv("PIMID_OOO_NOBRANCH") != nullptr);           // 1.11.100 (H40): the execution plugin's escapes
    g_inorderNoBranch = (getenv("PIMID_INORDER_NOBRANCH") != nullptr);
    g_cosimNoOffload = (getenv("PIMID_COSIM_NO_OFFLOAD") != nullptr);   // 1.11.100 (H43): the execution co-simulation's baseline knob
    int shmId = gm_init(1 << 28);  /* 256 MB segment, the size the execution plugin uses (1.11.100: the decoded cores need more than the 64 MB that stood here) */

    /* Initialize ZSim simulation hierarchy */
    procIdx = 0;
    for (uint32_t i = 0; i < MAX_THREADS; i++) {
        cids[i] = UNINITIALIZED_CID;
        thread_initialized[i] = false;
    }
    perProcessEndFlag = 0;

    SimInit(cfgPath, outputDir, shmId);
    buildDomainMasks();   // 1.11.100 (H38)
    for (uint32_t t = 0; t < MAX_THREADS; t++) thread_on_device[t] = false;

    lineBits = __builtin_ctz(zinfo->lineSize);
    procMask = ((uint64_t)procIdx) << 32;

    fprintf(stdout, "zsim_trace: ZSim initialized (%u cores, %u MHz, phase=%u)\n",
            zinfo->numCores, zinfo->freqMHz, zinfo->phaseLength);

    /* ---- Event replay loop (1.11.100 H42: the phase barrier in one host thread) ---- */

    PimidTraceEvent event, cut;
    bool haveCut = false;
    uint32_t rr = 0;
    for (;;) {
        if (zinfo->terminationConditionMet) {
            info("Termination condition met after %lu events", g_eventsProcessed);
            break;
        }
        /* 1. a deferred event of a thread that is no longer parked, round-robin over the threads */
        bool did = false;
        for (uint32_t i = 0; i < zinfo->numCores && !did; i++) {
            uint32_t t = (rr + i) % zinfo->numCores;
            if (!parkedThread(t) && !g_deferred[t].empty()) {
                PimidTraceEvent e = g_deferred[t].front(); g_deferred[t].pop_front(); rr = (t + 1) % zinfo->numCores;
                dispatchEvent(e); did = true;
            }
        }
        if (did) { if (anyJoined() && allJoinedParked()) endPhase(); continue; }
        /* 2. an ROI marker is a cut in file order: everything read before it runs first, so the
         *    replay's ROI instruction count and the cores' retired count match the generator's exactly */
        if (haveCut) {
            if (!anyDeferred()) { haveCut = false; dispatchEvent(cut); if (anyJoined() && allJoinedParked()) endPhase(); continue; }
            leaveIdleThreads(true);
            endPhase();
            continue;
        }
        /* 3. the next event of the file */
        if (fread(&event, sizeof(event), 1, trace_fp) != 1) {
            if (anyDeferred()) { leaveIdleThreads(true); endPhase(); continue; }
            break;
        }
        g_eventsRead++;
        uint32_t tid = event.pe_id % zinfo->numCores;
        g_lastReadSeq[tid] = g_eventsRead;
        if (event.event_type == EVT_ROI_BEGIN || event.event_type == EVT_ROI_END) { haveCut = true; cut = event; continue; }
        if (parkedThread(tid)) {
            g_deferred[tid].push_back(event); g_deferredEvents++;
            uint64_t backlog = 0; for (uint32_t t = 0; t < zinfo->numCores; t++) backlog += g_deferred[t].size();
            if (backlog > g_deferredPeak) g_deferredPeak = backlog;
            leaveIdleThreads(false);
            if (anyJoined() && allJoinedParked()) endPhase();
            continue;
        }
        dispatchEvent(event);
        if (anyJoined() && allJoinedParked()) endPhase();
    }

    fclose(trace_fp);

    /* ---- Finalize ---- */

    /* Leave every joined thread first (H42) */
    for (uint32_t tid = 0; tid < zinfo->numCores; tid++) if (joinedThread(tid)) leaveThread(tid);

    /* Finish all initialized threads */
    for (uint32_t tid = 0; tid < zinfo->numCores; tid++) {
        if (thread_initialized[tid]) {
            zinfo->sched->finish(procIdx, tid);
        }
    }

    /* Write simple stats for PIMID to parse */
    writeSimpleStats(outputDir, g_hierCycles);

    /* Dump ZSim stats and clean up */
    SimEnd();

    /* Print summary */
    fprintf(stdout, "\nzsim_trace: Simulation complete\n");
    fprintf(stdout, "  Events processed: %lu\n", g_eventsProcessed);
    fprintf(stdout, "  Memory reads:     %lu\n", g_memReads);
    fprintf(stdout, "  Memory writes:    %lu\n", g_memWrites);
    fprintf(stdout, "  Compute BBLs:     %lu\n", g_computes);
    fprintf(stdout, "  Phases:           %lu ended when every joined thread had crossed; %lu events deferred behind a parked thread (peak backlog %lu); %lu idle leaves\n", g_phaseEnds, g_deferredEvents, g_deferredPeak, g_idleLeaves);
    fprintf(stdout, "  Barriers:         %lu thread-creating syscalls (left the core: %lu, no core bound: %lu)\n", g_barriers, g_barriersTaken, g_barriersSkipped);
    fprintf(stdout, "  Branch feed:      %lu conditional directions, %lu control transfers fed to the cores' predictors (%lu first-after-syscall blocks not fed, as in execution)\n", g_brFed, g_ctrlFed, g_feedDropped);
    fprintf(stdout, "  ROI instructions: %lu replayed; the trace header says %lu (the blocks' instruction counts between the markers, file order; %s)\n",
            g_roiInsns, g_roiInsnsHeader, (g_roiInsns == g_roiInsnsHeader) ? "equal" : "NOT EQUAL");
    fprintf(stdout, "  Blocks:           %lu decoded from the trace's instruction bytes, %lu synthetic\n", g_decodedBlocks, g_syntheticBlocks);
    fprintf(stdout, "  ROI markers:      %lu begin, %lu end; domain migrations: %lu\n", g_roiBegins, g_roiEnds, g_migrations);
    fprintf(stdout, "  Skipped events:   %lu\n", g_skippedEvents);
    if (g_hierCycles > 0)
        fprintf(stdout, "  Hierarchy cycles: %lu\n", g_hierCycles);

    /* SimEnd calls _exit(0) in the QEMU plugin, but for the trace driver
     * we want graceful shutdown, so we return normally if SimEnd didn't
     * call _exit(). Note: SimEnd() above may call _exit(0). If we reach
     * here, it means we should exit gracefully. */
    return 0;
}
