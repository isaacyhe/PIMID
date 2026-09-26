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
 *   2. SimInit(cfg, outputDir) → creates cores, caches, MCs, network
 *   3. Read PIMID trace header + events
 *   4. Event loop: dispatch to ZSim core function pointers
 *   5. SimEnd() → dump stats
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

/* tid→cid mapping */
#define INVALID_CID ((uint32_t)-1)
#define UNINITIALIZED_CID ((uint32_t)-2)
static uint32_t cids[MAX_THREADS];

static volatile uint32_t perProcessEndFlag;

/* Thread state */
static bool thread_initialized[MAX_THREADS];

/* Single-active-thread protocol: at most one thread RUNNING in the barrier
 * at any time, preventing deadlock in the single-threaded trace driver. */
static constexpr uint32_t INVALID_TID = 0xFFFFFFFF;
static uint32_t activeTraceTid = INVALID_TID;

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
};

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

uint32_t TakeBarrier(uint32_t tid, uint32_t cid) {
    uint32_t newCid = zinfo->sched->sync(procIdx, tid, cid);
    clearCid(tid);
    setCid(tid, newCid);

    if (zinfo->terminationConditionMet) {
        info("Termination condition met, tid %d leaving", tid);
        zinfo->sched->leave(procIdx, tid, newCid);
        clearCid(tid);
        /* Return INVALID_CID so callers (SimpleCore::BblFunc, ALUCore, etc.)
         * see newCid != cid and break out of their phase-end loops instead
         * of calling TakeBarrier again on a LEFT thread. */
        return INVALID_CID;
    }

    fPtrs[tid] = cores[tid]->GetFuncPtrs();
    return newCid;
}

/* ---- SimEnd ---- */

void SimEnd() {
    if (__sync_bool_compare_and_swap(&perProcessEndFlag, 0, 1) == false) {
        return;  /* Already ended */
    }

    info("Dumping termination stats");
    zinfo->trigger = 20000;
    for (StatsBackend* backend : *(zinfo->statsBackends)) {
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

static void Join(uint32_t tid) {
    /* Leave the previously active thread before joining a new one.
     * This ensures at most ONE thread is RUNNING in the barrier,
     * preventing deadlock when a phase boundary is hit. */
    if (activeTraceTid != INVALID_TID && activeTraceTid != tid) {
        uint32_t prevCid = getCid(activeTraceTid);
        if (prevCid != INVALID_CID && prevCid != UNINITIALIZED_CID) {
            zinfo->sched->leave(procIdx, activeTraceTid, prevCid);
            fPtrs[activeTraceTid] = joinPtrs;
            clearCid(activeTraceTid);
        }
    }

    uint32_t cid = zinfo->sched->join(procIdx, tid);
    setCid(tid, cid);
    activeTraceTid = tid;

    if (unlikely(zinfo->terminationConditionMet)) {
        info("Caught termination on join, tid %d", tid);
        zinfo->sched->leave(procIdx, tid, cid);
        activeTraceTid = INVALID_TID;
        return;
    }

    fPtrs[tid] = cores[tid]->GetFuncPtrs();
}

/* ---- Thread initialization ---- */

static void initThread(uint32_t tid) {
    if (thread_initialized[tid]) return;
    thread_initialized[tid] = true;

    ProcessTreeNode* procTree = zinfo->procArray[procIdx];
    zinfo->sched->start(procIdx, tid, procTree->getMask());
    fPtrs[tid] = joinPtrs;
    clearCid(tid);

    info("Trace thread %d initialized", tid);
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

    /* Seek past YAML metadata to binary events */
    if (fseek(trace_fp, (long)header.header_size, SEEK_SET) != 0) {
        fprintf(stderr, "Error: Cannot seek to event data at offset %lu\n", header.header_size);
        fclose(trace_fp);
        return 1;
    }

    /* Initialize shared memory */
    int shmId = gm_init(1 << 26);  /* 64 MB segment — OOO/Timing cores need more */

    /* Initialize ZSim simulation hierarchy */
    procIdx = 0;
    for (uint32_t i = 0; i < MAX_THREADS; i++) {
        cids[i] = UNINITIALIZED_CID;
        thread_initialized[i] = false;
    }
    perProcessEndFlag = 0;

    SimInit(cfgPath, outputDir, shmId);

    lineBits = __builtin_ctz(zinfo->lineSize);
    procMask = ((uint64_t)procIdx) << 32;

    fprintf(stdout, "zsim_trace: ZSim initialized (%u cores, %u MHz, phase=%u)\n",
            zinfo->numCores, zinfo->freqMHz, zinfo->phaseLength);

    /* ---- Event replay loop ---- */

    PimidTraceEvent event;
    uint64_t eventsProcessed = 0;
    uint64_t memReads = 0, memWrites = 0, computes = 0, barriers = 0;
    uint64_t skippedEvents = 0;
    uint64_t hierCycles = 0;

    while (fread(&event, sizeof(event), 1, trace_fp) == 1) {
        if (zinfo->terminationConditionMet) {
            info("Termination condition met after %lu events", eventsProcessed);
            break;
        }

        uint32_t tid = event.pe_id % zinfo->numCores;

        if (!thread_initialized[tid]) {
            initThread(tid);
        }

        /* Ensure thread is joined (scheduled on a core) */
        if (fPtrs[tid].type == FPTR_JOIN) {
            Join(tid);
            if (zinfo->terminationConditionMet) break;
        }

        switch (event.event_type) {
            case EVT_MEM_READ:
                fPtrs[tid].loadPtr(tid, event.address);
                memReads++;
                break;

            case EVT_MEM_WRITE:
                fPtrs[tid].storePtr(tid, event.address);
                memWrites++;
                break;

            case EVT_MEM_ATOMIC:
                fPtrs[tid].loadPtr(tid, event.address);
                fPtrs[tid].storePtr(tid, event.address);
                memReads++;
                memWrites++;
                break;

            case EVT_COMPUTE_INT:
            case EVT_COMPUTE_FP:
            case EVT_COMPUTE_VECTOR: {
                BblInfo* bbl = makeSyntheticBbl(event.aux_data);
                fPtrs[tid].bblPtr(tid, event.address, bbl);
                computes++;
                break;
            }

            case EVT_PIM_COMPUTE: {
                /* PIM compute: local only, no data movement */
                BblInfo* bbl = makeSyntheticBbl(event.aux_data > 0 ? event.aux_data : 1);
                fPtrs[tid].bblPtr(tid, event.address, bbl);
                if (zinfo->terminationConditionMet) break;
                if (event.address != 0) {
                    fPtrs[tid].loadPtr(tid, event.address);
                    memReads++;
                }
                computes++;
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
                    memReads++;
                }
                computes++;
                /* Add hierarchy latency for inter-PE data movement */
                if (zinfo->hierarchy.enabled && event.src_node != event.dst_node) {
                    uint64_t hlat = computeHierarchyLatency(event.src_node, event.dst_node, tid);
                    if (hlat > 0) {
                        BblInfo* hbbl = makeSyntheticBbl(hlat);
                        fPtrs[tid].bblPtr(tid, 0, hbbl);
                        if (zinfo->terminationConditionMet) break;
                        hierCycles += hlat;
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
                    memWrites++;
                } else {
                    fPtrs[tid].loadPtr(tid, event.address);
                    memReads++;
                }
                /* Add hierarchy latency for inter-PE communication */
                if (zinfo->hierarchy.enabled && event.src_node != event.dst_node) {
                    uint64_t hlat = computeHierarchyLatency(event.src_node, event.dst_node, tid);
                    if (hlat > 0) {
                        BblInfo* hbbl = makeSyntheticBbl(hlat);
                        fPtrs[tid].bblPtr(tid, 0, hbbl);
                        if (zinfo->terminationConditionMet) break;
                        hierCycles += hlat;
                    }
                }
                break;

            case EVT_BARRIER: {
                /* Force a phase boundary */
                uint32_t cid = getCid(tid);
                if (cid != INVALID_CID) {
                    TakeBarrier(tid, cid);
                }
                barriers++;
                break;
            }

            case EVT_OFFLOAD_START:
            case EVT_OFFLOAD_END:
            case EVT_TASK_START:
            case EVT_TASK_END:
                /* Informational — no simulation effect in trace replay */
                break;

            default:
                /* OpenMP, MPI, other events: skip in ZSim replay */
                skippedEvents++;
                break;
        }

        eventsProcessed++;

        /* Progress reporting every 1M events */
        if (eventsProcessed % 1000000 == 0) {
            info("Processed %lu events...", eventsProcessed);
        }
    }

    fclose(trace_fp);

    /* ---- Finalize ---- */

    /* Leave the currently active thread first */
    if (activeTraceTid != INVALID_TID) {
        uint32_t cid = getCid(activeTraceTid);
        if (cid != INVALID_CID && cid != UNINITIALIZED_CID)
            zinfo->sched->leave(procIdx, activeTraceTid, cid);
        activeTraceTid = INVALID_TID;
    }

    /* Finish all initialized threads */
    for (uint32_t tid = 0; tid < zinfo->numCores; tid++) {
        if (thread_initialized[tid]) {
            zinfo->sched->finish(procIdx, tid);
        }
    }

    /* Write simple stats for PIMID to parse */
    writeSimpleStats(outputDir, hierCycles);

    /* Dump ZSim stats and clean up */
    SimEnd();

    /* Print summary */
    fprintf(stdout, "\nzsim_trace: Simulation complete\n");
    fprintf(stdout, "  Events processed: %lu\n", eventsProcessed);
    fprintf(stdout, "  Memory reads:     %lu\n", memReads);
    fprintf(stdout, "  Memory writes:    %lu\n", memWrites);
    fprintf(stdout, "  Compute BBLs:     %lu\n", computes);
    fprintf(stdout, "  Barriers:         %lu\n", barriers);
    fprintf(stdout, "  Skipped events:   %lu\n", skippedEvents);
    if (hierCycles > 0)
        fprintf(stdout, "  Hierarchy cycles: %lu\n", hierCycles);

    /* SimEnd calls _exit(0) in the QEMU plugin, but for the trace driver
     * we want graceful shutdown, so we return normally if SimEnd didn't
     * call _exit(). Note: SimEnd() above may call _exit(0). If we reach
     * here, it means we should exit gracefully. */
    return 0;
}
