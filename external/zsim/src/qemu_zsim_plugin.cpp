/**
 * QEMU TCG Plugin for ZSim In-Process Simulation
 *
 * Replaces zsim.cpp's Pin-based instrumentation with QEMU user-mode
 * emulation callbacks. Drives the full ZSim core/cache/memory hierarchy.
 *
 * This plugin is loaded by QEMU via:
 *   qemu-x86_64 -plugin libzsim_qemu.so,shmid=N,cfg=<path>,out=<dir> -- workload
 *
 * Architecture:
 *   - qemu_plugin_install() attaches shared memory, calls SimInit()
 *   - tb_trans_cb() creates BblInfo per translation block
 *   - mem_cb() calls Core::load/store via function pointers
 *   - insn_exec_cb() calls Core::bbl, triggers phase barriers
 *   - vcpu_init_cb() registers threads with scheduler
 *   - atexit_cb() calls SimEnd() and dumps stats
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <unordered_map>
#include <mutex>

/* QEMU plugin API (C interface) */
extern "C" {
#include "qemu/qemu-plugin.h"
QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;
}

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
#include "pimid_noc_shm.h"
#include "process_tree.h"
#include "profile_stats.h"
#include "scheduler.h"
#include "stats.h"
#include "cache.h"   // 1.11.40 (N7): zsimAllCaches; 1.11.57 (D001-D003): flushDirtyLines
#include "zsim.h"
#include "ooo_core.h"     // CtrlFlowKind codes for the branch/indirect feed
#include "x86_decoder.h"  // minimal x86-64 decoder -> DynUops for the real OOO path

/* ---- Globals (equivalent to zsim.cpp globals) ---- */

GlobSimInfo* zinfo;
uint32_t procIdx;
uint32_t lineBits;
uint64_t procMask;
Core* cores[MAX_THREADS];

/* 1.9.0 thread-MPI epoch cut: per-core ROI state (0=ACTIVE, 1=QUIESCENT parked in
 * MPI, 2=EXITED, 3=VACATED by a co-sim migration). Marked at DETERMINISTIC sim
 * events (roi_end / thread-fini / MPI comm brackets / host-device migration),
 * NOT by a wall-dependent advancement heuristic. Read by
 * EndOfPhaseActions to build the consistent cut = min roiRel over ACTIVE cores.
 * Static zero-init => all ACTIVE at start.
 * QUIESCENT and VACATED are re-armed to ACTIVE when the core runs again (MPI
 * wait resumed / a thread attaches); EXITED is sticky, since a finished ROI
 * never resumes. */
static std::atomic<uint32_t> g_coreRoiState[MAX_THREADS];
InstrFuncPtrs fPtrs[MAX_THREADS] ATTR_LINE_ALIGNED;

/* tid->cid mapping */
#define INVALID_CID ((uint32_t)-1)
#define UNINITIALIZED_CID ((uint32_t)-2)
static uint32_t cids[MAX_THREADS];

static volatile uint32_t perProcessEndFlag;

/* BblInfo cache: translation block vaddr -> BblInfo* */
static std::unordered_map<uint64_t, BblInfo*> bblCache;
static std::mutex bblCacheMutex;

/* True if any OOO core exists in this config. Only then do we run the x86
 * decoder to populate BblInfo->oooBbl with real DynUops (which ONLY the OOO
 * core reads). When no OOO core is present we keep the byte-identical synthetic
 * BblInfo path, so alu/simple/in_order/null behavior and sim speed are
 * unchanged. Set once in qemu_plugin_install after cores are constructed. */
static bool g_ooo_present = false;
static bool g_ooo_decode_disabled = false;  /* PIMID_OOO_NODECODE=1 escape hatch */

/* ---- Shared detailed-MPI Garnet: rank state in the shared NoC log ----
 * A guest blocked in MPI (comm window open) injects nothing, so it is marked
 * QUIESCENT and exempted from the merged-replay consistent cut; EXITED at
 * plugin teardown. Peers therefore never wait on this rank (deadlock-free).
 * No-op unless the launcher exported PIMID_NOC_SHM (MPI + detailed). */
static void pimid_noc_mark_state(uint32_t st) {
    static PimidNocHdr* h = nullptr;
    static int rank = -1;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const char* nm = getenv("PIMID_NOC_SHM");
        const char* rk = getenv("PIMID_MPI_RANK");
        if (nm && rk) {
            h = pimid_noc_shm_attach(nm);
            rank = atoi(rk);
        }
    }
    if (h && rank >= 0) pimid_noc_set_state(h, (uint32_t)rank, st);
}

/* True if any genuine in-order core exists. The in-order pipeline (in_order_core)
 * consumes the SAME x86-decoded DynUop stream as the OOO core (its oooBbl), so we
 * must also run the decoder when an in-order core is present. (Historical:
 * PIMID_INORDER_NODECODE
 * disables that decode (in-order falls back to the legacy IPC=1 path), mirroring
 * PIMID_OOO_NODECODE. Only OOOCore and InOrderCore read oooBbl; since 1.11.15
 * decode runs for EVERY core type (the class census needs it), and alu/simple/
 * null consume instrs/bytes/census -- their nFp feeds the 1.11.11 soft-float
 * charge when pim.pe.floating_point=false, so decode is timing-visible on
 * those cores ONLY under that non-default config. */
static bool g_inorder_present = false;
static bool g_inorder_nobranch = false;  /* PIMID_INORDER_NOBRANCH=1: skip branch feed */
/* Combined gate: decode TBs into DynUops when EITHER a (non-disabled) OOO or a
 * (non-disabled) in-order core is present. Set once in qemu_plugin_install. */
static bool g_decode_enabled = false;

/* PIMID_OOO_DUMP=1: profile which opcodes fall to the generic/approx uop path,
 * weighted by DYNAMIC execution count, to prioritize decoder coverage. The
 * per-TB approx-key list is stored in the TbUserdata (set at decode, read at
 * execution -> no lock), and dynamic hits accumulate into a lock-free atomic
 * array indexed by the packed decode signature. Printed at plugin exit. Zero
 * overhead when unset. */
static bool g_ooo_dump = false;
static bool g_ooo_nobranch = false;  /* PIMID_OOO_NOBRANCH=1: skip branch-pred feed */
#define OOO_DUMP_SLOTS (4 * 256 * 8)   /* [mapIdx:2][op:8][reg:3] */
static std::atomic<uint64_t> g_dumpDyn[OOO_DUMP_SLOTS];
static std::atomic<uint64_t> g_dumpTotalInsns{0};
static std::atomic<uint64_t> g_dumpApproxInsns{0};
/* Per-TB cache of approx-key arrays for the dump profiler (keyed by tbAddr).
 * Guarded by bblCacheMutex. Declared here so getOrCreateBblInfo can populate it. */
static std::unordered_map<uint64_t, std::pair<uint16_t*, uint32_t>> g_tbApproxCache;

/* Branch-predictor diagnostics: total dynamic conditional branches fed to the
 * OOO predictor (per-core mispredicts live in the OOO stats / zsim.out). */
static std::atomic<uint64_t> g_brDynCount{0};

/* Per-vcpu -> ZSim THREADID mapping */
#define MAX_VCPUS 256
static uint32_t vcpuToTid[MAX_VCPUS];
/* 1.9.26: detect whether ANOTHER thread used this core during a comm window.
 * A plain bind COUNT is wrong: a rank parks at a barrier and rebinds to its OWN
 * core on wake, which bumps the count and makes every rank look like it shares.
 * Track the current owner instead and count only binds by a DIFFERENT thread. */
static uint32_t g_coreOwner[MAX_THREADS];
static uint64_t g_coreForeignBinds[MAX_THREADS] = {};
static uint32_t nextTid = 0;
static std::mutex tidMutex;

/* 1.9.20 (defect #15): deterministic device-PE homes.
 *
 * The PE index -> Garnet node map is fixed at init (mems[i] carries mcId_ = i;
 * PEMemoryInterface computes srcNode = mcId_ % numNodes). The thread -> core map
 * was NOT: Scheduler::schedThread first tries the last-used context if it is
 * still IDLE, else takes freeList.front(), and the free list is ordered by the
 * wall-clock sequence in which cores were released. Ranks cross that path on
 * every host->device migration, so each run drew a different rank -> PE
 * permutation. Same work, different placement => different hop distances =>
 * different latencies (measured: 1.31% sd / 2.85% range on device cycles, n=5).
 *
 * The key MUST be vcpu_index, not tid: tid is handed out as nextTid++ on first
 * callback, i.e. in arrival order, so it is raced itself and pinning by it would
 * only relabel the same nondeterminism. vcpu_index is QEMU's cpu index, fixed by
 * guest thread-creation order.
 *
 * Giving a rank a single-core affinity mask leaves schedThread's selection
 * algorithm untouched -- it simply has no other candidate to race for. */
static uint32_t tidToVcpu[MAX_THREADS];
static g_vector<uint32_t> device_core_list;   // device cids, ascending
static g_vector<uint32_t> device_home_owner;  // device_core_list idx -> owning vcpu
static g_vector<g_vector<bool>> device_home_mask;  // per-tid single-core mask
static bool g_pin_device_homes = true;        // PIMID_NO_PE_PIN=1 disables
static bool g_pin_fallback_warned = false;
/* Ranks migrate concurrently, so the claim below must be atomic: two ranks
 * racing the same free slot would both read it unowned and both pin to it,
 * which is exactly the collision the guard exists to prevent. */
static std::mutex pinMutex;

/* Plugin state */
static qemu_plugin_id_t pluginId;
static char* cfgPath = nullptr;
static char* outputDir = nullptr;
static uint32_t shmId = 0;
static bool useSharedMem = false;

/* Internal thread filtering -- vCPUs created during SimInit() are ZSim's
 * own threads (watchdog, contention sim) and must NOT be instrumented. */
static std::atomic<bool> sim_initialized{false};
static bool vcpu_is_internal[MAX_VCPUS];  // initialized to false in qemu_plugin_install

/* Guard flag: true while executing ZSim code (bblPtr/loadPtr/storePtr).
 * Prevents reentrant syscall_cb from corrupting scheduler/barrier state
 * if QEMU ever delivers a callback during ZSim function pointer dispatch. */
static bool in_zsim[MAX_VCPUS];

/* ROI state -- set by mov $op, %rcx + xchg %rcx, %rcx (zsim_hooks.h magic ops) */
static std::atomic<bool> in_roi{true};              /* true = record; default on for non-ROI workloads */
static void roiRebaseTrafficCounters(const char* where);   // 1.11.90, defined below
// 1.6 thread-MPI: N rank-threads each bracket their own ROI in ONE process.
// Baselines snapshot at the FIRST begin; termination fires at the LAST end.
static std::atomic<int> g_roiRefCount{0};
static std::atomic<uint64_t> roi_transition_count{0};

/* ZSim magic op codes (must match zsim_hooks.h) */
#define ZSIM_MAGIC_OP_ROI_BEGIN     1025
#define ZSIM_MAGIC_OP_ROI_END       1026
#define ZSIM_MAGIC_OP_WORK_BEGIN    1029
#define ZSIM_MAGIC_OP_WORK_END      1030
#define ZSIM_MAGIC_OP_MPI_REGISTER  2048
#define ZSIM_MAGIC_OP_MPI_SEND      2049
#define ZSIM_MAGIC_OP_MPI_RECV      2050
#define ZSIM_MAGIC_OP_MPI_BARRIER   2051
#define ZSIM_MAGIC_OP_REG_DEVBUF    2052  /* register a device-owned buffer range (v1.1.1) */
#define ZSIM_MAGIC_OP_PRICE_PAUSE   2053  /* suspend address-routed pricing (DMA window) */
#define ZSIM_MAGIC_OP_PRICE_RESUME  2054  /* resume address-routed pricing */
#define ZSIM_MAGIC_OP_MPI_COMM_BEGIN 2055 /* open MPI comm window (keep core attached) */
#define ZSIM_MAGIC_OP_MPI_COMM_END   2056 /* close MPI comm window */
#define ZSIM_MAGIC_OP_MPI_CONTEND    2057
/* 1.6 thread-MPI determinism ops: TIME publishes the calling core's ROI-
 * relative sim clock into params.sim_now; ADVANCE snaps the calling core
 * forward to params.sim_send_time (never backward). Together they let the
 * guest barrier compute a max-arrival rendezvous so every rank exits a
 * barrier at the SAME deterministic cycle regardless of futex wake order. */
#define ZSIM_MAGIC_OP_MPI_TIME       2058
#define ZSIM_MAGIC_OP_MPI_ADVANCE    2059 /* charge sender cross-rank contention wait (Phase 2) */

/* Per-thread MPI comm-window flag. While set (bracketed by COMM_BEGIN/END from
 * libpimid_mpi), the rank's blocking transport sleeps do NOT trigger a
 * syscallLeave (the core stays attached, so post-comm compute is counted) and
 * the transport's own instructions are NOT counted (so the wall-clock poll is
 * invisible and per-rank counts stay deterministic). Set only by the MPI
 * transport; OMP and co-sim paths never touch it, so their behavior is
 * byte-for-byte unchanged. */
static std::atomic<bool> mpi_comm_window[MAX_THREADS];
/* 1.6 frozen-clock waits: raw core cycles at COMM_BEGIN (~0 = idle). */
static uint64_t mpi_comm_saved[MAX_THREADS] = {};
static uint32_t mpi_comm_saved_cid[MAX_THREADS];
static uint64_t mpi_comm_saved_fb[MAX_THREADS] = {};

/* Per-rank ROI baseline synthesis (see handleMpiMagicOp): the MPI benchmarks
 * call zsim_roi_begin() ONLY on rank 0, so ranks 1..N-1 never snapshot an ROI
 * baseline and would report whole-program (init-dominated, non-scaling) cycles/
 * instrs. This flag lets the plugin synthesize a per-rank baseline at the first
 * communication op (~kernel entry) for any rank that never saw roi_begin. Each
 * MPI rank is its own process, so a process-global flag is per-rank. */
static bool mpi_roi_baselined = false;

/* Per-core simulated cycle captured at the ROI baseline (markRoiBegin instant).
 * The sim-time rendezvous stamps/compares send-times RELATIVE to this baseline
 * so cross-rank timing is a function of real post-baseline work only -- NOT of
 * the wall-clock-coupled globPhaseCycles floor each rank happens to carry when
 * it reaches its baseline (that floor differs per rank by scheduling jitter, and
 * leaking it into the rendezvous made the cross-rank critical path nondet). */
static uint64_t mpi_roi_base_cyc[MAX_THREADS] = {0};

/* Co-sim mode: a real host and a real device coexist; ROI = offload region.
 * Defined HERE (moved up from its 1.6 site) so roiRelCycles() below can scope
 * the 1.8.7 tid/cid baseline correction to co-sim only. */
static bool g_cosim_mode = false;

/* Snapshot every core's ROI baseline cycle (call right after markRoiBegin). */
static inline void snapshotRoiBaseCyc() {
    uint64_t maxBase = 0;
    for (uint32_t c = 0; c < zinfo->numCores && c < MAX_THREADS; c++)
        if (zinfo->cores[c]) {
            mpi_roi_base_cyc[c] = zinfo->cores[c]->getCycles();
            if (mpi_roi_base_cyc[c] > maxBase) maxBase = mpi_roi_base_cyc[c];
        }
    /* Export for the PE-MI's shared-NoC publishing: records go on the ROI-
     * relative clock (the floor-free cross-rank axis). The max across cores is
     * the running core's baseline (idle cores sit near 0 in a 1-thread rank). */
    zinfo->hierarchy.mpiNocRoiBase = maxBase;
    // 1.9.0 thread-MPI: capture the ROI-baseline global phase so epochs key
    // ROI-relative -- the wall-dependent pre-ROI phase count cannot then shift the
    // ROI epoch boundaries. Set BEFORE mpiNocBaselined so the flag gates cleanly.
    zinfo->hierarchy.mpiNocRoiBasePhase = zinfo->numPhases;
    __sync_synchronize();
    zinfo->hierarchy.mpiNocBaselined = 1;
}

/* 1.11.90: ROI-rebase the TRAFFIC counters. Cores rebase their own instrs
 * and cycles in markRoiBegin (1.x), the PG residency window is opened by
 * pgres.markRoi (1.11.18), but the cache, memory-controller and PE
 * memory-interface counters, and Garnet's flit counters, kept counting from
 * process start -- the plugin records by default -- so the serial pre-ROI
 * array-init traffic was priced over the kernel's wall clock. Idempotent
 * like markRoiBegin (re-snapshots the current instant). Prints what it
 * dropped, once per call, so a log shows the window it reports. */
static void roiRebaseTrafficCounters(const char* where) {
    if (!zinfo) return;
    uint64_t pre_mem = 0;
    uint32_t groups = 0;
    if (zinfo->roiRebaseStats) {
        for (AggregateStat* g : *zinfo->roiRebaseStats) {
            if (!g) continue;
            /* what the "mem" group counted so far, for the log line */
            if (g->name() && strcmp(g->name(), "mem") == 0) {
                for (uint32_t i = 0; i < g->curSize(); i++) {
                    AggregateStat* mc = g->get(i)->asAggregate();
                    if (!mc) continue;
                    for (uint32_t j = 0; j < mc->curSize(); j++) {
                        Counter* c = mc->get(j)->asCounter();
                        if (c && c->name() && (strcmp(c->name(), "rd") == 0 || strcmp(c->name(), "wr") == 0))
                            pre_mem += c->get();
                    }
                }
            }
            g->roiRebase();
            groups++;
        }
    }
    uint64_t pre_flits = 0;
    if (zinfo->garnetNetwork) pre_flits += zinfo->garnetNetwork->markRoiBegin();
    if (zinfo->systemGarnetNetwork) pre_flits += zinfo->systemGarnetNetwork->markRoiBegin();
    info("[roi] traffic counters rebased at roi_begin (%s): %u stat groups, "
         "pre-ROI memory-controller accesses dropped: %lu, pre-ROI NoC flits dropped: %lu",
         where, groups, (unsigned long)pre_mem, (unsigned long)pre_flits);
}
/* This rank's simulated cycle measured from its ROI baseline (floor-free).
 *
 * 1.8.7 tid/cid baseline-aliasing fix (SCOPED to co-sim). snapshotRoiBaseCyc()
 * stores mpi_roi_base_cyc[] indexed by CORE INDEX (mpi_roi_base_cyc[c] =
 * cores[c]->getCycles()). roiRel needs the baseline of the core the RANK is on.
 *
 * In SYSTEM-scope co-sim the two disagree catastrophically: a rank (tid) runs on
 * a DEVICE PE whose cid != tid, and the old read mpi_roi_base_cyc[tid] aliased
 * HOST cores 0..3 -- carrying their ~16.5M pre-ROI (MPI_Init-era) clock -- so
 * roiRel clamped to 0 for tid 0..3. That collapsed the SEND/RECV rendezvous: the
 * receiver's `cur` read 0, so each recv advanced by the FULL send_time and the
 * reduce ROOT ACCUMULATED the senders' stamps (~2.6M) instead of advancing to
 * their max. Reading the baseline of the rank's ACTUAL core (cids[tid]) puts
 * getCycles() and the baseline on the same clock axis and fixes it.
 *
 * That correction is CONFINED to g_cosim_mode. The catastrophic aliasing exists
 * ONLY where host and device cids coexist (system scope). In DEVICE scope every
 * core is a device PE, the old [tid] indexing is already correct, AND it is
 * DETERMINISTIC -- whereas [cids[tid]] would couple roiRel to the run-to-run
 * PE assignment (cids[tid] is not stable across runs, per-PE baselines differ),
 * making device-scope MPI nondeterministic and NOT bit-identical to prior data.
 * So device scope keeps [tid] verbatim (bit-identical to the 1.8.6 baseline).
 *
 * Documented residual (both scopes): mpi_roi_base_cyc is a shared per-CORE-slot
 * table, so a rank's stamp still carries a small per-rank baseline skew from the
 * slot it reads. It is DETERMINISTIC and present in all prior co-sim MPI data; a
 * proper per-RANK baseline table is a 1.9.x redesign candidate. */
/* Per-rank cache of the last baseline read with a VALID cid (co-sim). A rank's
 * cids[tid] is momentarily invalid (INVALID/UNINITIALIZED_CID) during migrate-in
 * and other detached windows; without this cache the co-sim branch would fall
 * back to mpi_roi_base_cyc[tid], which for tid 0..3 is the aliased HOST-core
 * baseline (~16.5M) -- flipping roiRel between the correct value and 0 mid-run.
 * That inconsistency made a rank publish alternating send-time stamps and, rarely
 * (~4%), drove a downstream rendezvous cycle past 2^32 -> contention_sim "event
 * too far into the future". Caching the last valid device baseline keeps roiRel
 * consistent across transient cid-invalid windows. */
static uint64_t g_rankRoiBaseCache[MAX_THREADS] = {0};

static inline uint64_t roiRelCycles(uint32_t tid) {
    uint64_t now = cores[tid] ? cores[tid]->getCycles() : 0;
    uint64_t base;
    if (g_cosim_mode) {
        uint32_t cid = (tid < MAX_THREADS) ? cids[tid] : tid;
        if (cid < zinfo->numCores) {
            base = mpi_roi_base_cyc[cid];
            if (tid < MAX_THREADS) g_rankRoiBaseCache[tid] = base;   // remember last valid
        } else if (tid < MAX_THREADS && g_rankRoiBaseCache[tid] != 0) {
            base = g_rankRoiBaseCache[tid];                          // reuse across invalid window
        } else {
            base = mpi_roi_base_cyc[tid];                            // pre-first-valid fallback
        }
    } else {
        base = mpi_roi_base_cyc[tid];   // device scope: unchanged, deterministic, bit-identical
    }
    return (now > base) ? (now - base) : 0;
}

/* Co-simulation: per-thread domain tracking and core-type masks */
enum SimDomain { DOMAIN_HOST = 0, DOMAIN_DEVICE = 1 };
static std::atomic<int> thread_domain[MAX_THREADS];    // default HOST

/* 1.9.27: the clock rate (MHz) of the core this thread is running on. Cycle
 * counts are only comparable between ranks whose clocks tick at the same rate,
 * so a timestamp published for another rank must carry its rate. In a coupled
 * system-scope run zinfo->freqMHz is max(all node freqs) -- the HOST clock --
 * while the device carries its own; a rank on a device PE must therefore not be
 * stamped with the global value. */
static inline uint32_t coreFreqMHzFor(uint32_t tid) {
    bool onDevice = (tid < MAX_THREADS &&
                     thread_domain[tid].load() == DOMAIN_DEVICE);
    if (onDevice && zinfo->hierarchy.nocBandwidthFreqMHz > 0)
        return zinfo->hierarchy.nocBandwidthFreqMHz;
    return zinfo->freqMHz;
}

/* Convert a cycle count from a source clock domain into this core's domain.
 * Same rate (or unknown source) is an exact no-op, which is every configuration
 * where host and device clocks coincide -- so this cannot move existing
 * results. */
static inline uint64_t cyclesFromDomain(uint64_t cycles, uint32_t srcFreqMHz,
                                        uint32_t dstFreqMHz) {
    if (srcFreqMHz == 0 || dstFreqMHz == 0 || srcFreqMHz == dstFreqMHz)
        return cycles;
    return (uint64_t)((double)cycles * (double)dstFreqMHz / (double)srcFreqMHz);
}

static bool thread_initialized[MAX_THREADS];            // false until ensureThreadInit

/* Thread-based MPI rank emulation active in THIS process (the only
 * exec-method MPI model since 1.8.3). Resolved ONCE at plugin init from the
 * internal launcher channel (PIMID_MPI_RANKS > 1) so hot paths never call
 * getenv; false in per-rank trace-gen processes (PIMID_MPI_RANK set), which
 * keep process-mode semantics. */
static bool g_mpi_thread_mode = false;
/* 1.11.19 (D4): emulated MPI rank count, resolved once at install.
 * 1.11.62 (ruling 5b): the coherence flush no longer DIVIDES by this. Every
 * rank's arrival walks the caches itself and charges the delta it finds, so
 * D4's "the TOTAL is the working set once, independent of rank count" now
 * holds because every walk cleans, not because a footprint is cut into slices.
 * The flush path reads this only to say how many walks a reader should expect
 * (flushWalkersExpected). */
static uint32_t g_mpi_rank_count = 1;

/* 1.8.4 co-sim ROI window (defect #14): population of device workers in the
 * CURRENT window, and whether the window has been closed by the opener's
 * roi_end. The opener (rank 0 / OMP master) counts 1 at ROI_BEGIN; every
 * MPI rank that lazily migrates in counts 1; each migrate-out decrements.
 * Stats freeze when the population drains to ZERO after the close -- the
 * opener's roi_end alone must not truncate the other ranks' compute tails.
 * OMP workers born inside the window are NOT counted: the master's own
 * out-migration drains the population exactly as before (OMP unchanged). */
static std::atomic<int> g_deviceWorkers{0};
static std::atomic<bool> g_roiClosing{false};

/* 1.8.7 fully device-resident co-sim MPI ranks (ZERO migrations).
 *
 * Finalized model: the MPI ranks ARE the device PEs. The post-ROI inter-PE
 * collective (closing barrier + Reduce + Finalize) is device work executed ON
 * THE PE -- rank 0 collects on its PE; the host is only involved at final
 * readback. So NO rank ever migrates at window close (the 1.8.6/1.8.4 residual
 * device->host legs are all removed), and the tail cycles correctly STAY in the
 * PE's reported cycles. Zero migrations = zero race, by construction.
 *
 * The only added machinery is a RECEIPT: each rank records, IN ITS OWN THREAD
 * CONTEXT (plain per-tid array writes, no locks, no scheduler calls), a TAIL
 * MARKER = its PE cycle count at its closing-barrier op (uniform for all ranks,
 * INCLUDING rank 0 -- see below), plus the PE cid. At the termination dump,
 * recordProtocolTailStats() sets g_mpiProtocolTailCyc[cid] = final PE cycles -
 * surfaced as the per-PE 'protocolTail' visibility stat. No ledger move. */
static uint64_t g_tailMarker[MAX_THREADS]   = {0};   // PE cycles at close (post-ADVANCE)
static uint32_t g_tailDeviceCid[MAX_THREADS];         // PE cid at close
static bool     g_tailMarked[MAX_THREADS]   = {false};

/* Record a rank's post-ROI tail marker in its own thread context: the device
 * PE cycle count at the instant the window closes for this rank. No locks, no
 * scheduler calls -- just per-tid array writes. */
static inline void recordTailMarker(uint32_t tid) {
    if (tid >= MAX_THREADS || !thread_initialized[tid]) return;
    uint32_t cid = cids[tid];
    if (cid < zinfo->numCores && zinfo->cores[cid]) {
        g_tailMarker[tid]    = zinfo->cores[cid]->getCycles();
        g_tailDeviceCid[tid] = cid;
        g_tailMarked[tid]    = true;
    }
}

/* Defined below (cost model helpers); used by the kernel-entry migration in
 * handleMpiMagicOp, which precedes them in this file. */
static void chargeCoherenceFlush(uint32_t tid);
static void chargeLaunchCost(uint32_t tid);
static void drainHostCharges(uint32_t tid);

/* Serializes the 1.8.4 window migrations. A barrier release wakes ALL ranks
 * at once and every earlier caller of the charge+leave/finish+re-init
 * sequence (roi_begin/roi_end/WORK markers) was a single thread -- the
 * concurrent stampede corrupted the process heap (glibc unaligned-tcache
 * abort, v184 rounds 3-4). Two events per rank per run: serializing is
 * free. */
static std::mutex g_migrateMutex;
static g_vector<bool> host_mask;     // true for OOO cores
static g_vector<bool> device_mask;   // true for ALU cores
static std::atomic<uint64_t> offload_count{0};
/* (g_cosim_mode is defined earlier, near mpi_roi_base_cyc, so roiRelCycles can
 * scope the tid/cid baseline correction to co-sim.) */
/* NO_OFFLOAD baseline knob (PIMID_COSIM_NO_OFFLOAD=1, read once at init). In a
 * co-sim (system-scope) run the ROI/WORK magic ops become STAT-ONLY: threads
 * never migrate to the device domain, no bridge/PCIe transfer latency is
 * charged, and no offload_count-driven device pricing happens. The workload
 * runs on the host cores end to end; ROI begin/end still delimit the measured
 * region (baseline stat snapshot + termination) exactly as when the knob is
 * OFF. This yields the host-only baseline cells (1/4/16 OOO host cores running
 * the unmodified OMP/MPI kernels) from the SAME binary. */
static bool g_cosim_no_offload = false;
/* Env-gated thread-lifecycle trace for the in-image startup-hang hunt
 * (PIMID_COSIM_TRACE=1): timestamps around scheduler start/join at thread
 * birth and offload migration. Zero overhead when unset. */
static bool g_cosim_trace = false;
#define COSIMTRACE(fmt, ...) do { if (g_cosim_trace) { \
    struct timespec _ts; clock_gettime(CLOCK_MONOTONIC, &_ts); \
    fprintf(stderr, "[cosim-trace %ld.%03ld] " fmt "\n", \
            (long)_ts.tv_sec, _ts.tv_nsec/1000000, ##__VA_ARGS__); } } while (0)
/* True while the process is inside a host->device offload region (between
 * WORK_BEGIN and WORK_END on the launching thread). Threads spawned during
 * this window are device workers and must be born DOMAIN_DEVICE so they map
 * to device PEs rather than host cores. */
static std::atomic<bool> g_in_device_region{false};

/* Per-vCPU pending magic op from mov $imm, %rcx -- persists across TB
 * boundaries.  This handles the rare case where mov and xchg are in different
 * TBs (e.g., page boundary between them).  Per-vCPU to avoid races when
 * multiple vCPUs translate TBs concurrently under MTTCG. */
static std::atomic<uint64_t> pending_magic_op[MAX_THREADS];

/* ---- Runtime %rcx readback for magic ops ----
 *
 * The translation-time instruction decoder only recognizes mov-immediate
 * forms of the %rcx setup.  When zsim_work_begin_sized(size) is called with a
 * RUNTIME size, the compiler builds %rcx by arithmetic (salq/orq) instead of a
 * mov-immediate, so the decoder sees nothing and payload_size is lost.
 *
 * The general, correct source for the magic op value is the ACTUAL %rcx
 * register at the moment the xchg %rcx,%rcx executes.  We read it via
 * qemu_plugin_read_register(), which requires the exec callback to be
 * registered with QEMU_PLUGIN_CB_R_REGS.  The register handle is resolved once
 * (per-vCPU context) and cached; a reusable GByteArray avoids per-op alloc.
 *
 * NOTE: the vendored qemu-plugin.h in this tree (external/zsim/include) is a
 * minimal subset that predates the register-read API and does NOT pull in
 * <glib.h>, whose include dir is also absent from the build flags.  We are
 * constrained to edit only this .cpp, so we locally declare the small glib ABI
 * (matching system glib-2.0 struct layouts) and the QEMU register API.  These
 * symbols are provided at runtime by qemu-x86_64 (which links libglib-2.0 and
 * exports qemu_plugin_get_registers / qemu_plugin_read_register). */
extern "C" {
typedef struct PimidGArray { char* data; unsigned int len; } PimidGArray;
typedef struct PimidGByteArray { uint8_t* data; unsigned int len; } PimidGByteArray;
struct qemu_plugin_register;
typedef struct {
    struct qemu_plugin_register* handle;
    const char* name;
    const char* feature;
} pimid_qemu_plugin_reg_descriptor;

PimidGArray* qemu_plugin_get_registers(void);
int qemu_plugin_read_register(struct qemu_plugin_register* handle,
                              PimidGByteArray* buf);
PimidGByteArray* g_byte_array_sized_new(unsigned int reserved_size);
PimidGByteArray* g_byte_array_set_size(PimidGByteArray* array, unsigned int length);
char* g_array_free(PimidGArray* array, int free_segment);
}

#define PIMID_GARRAY_INDEX(a, t, i) \
    (((t*)(void*)(a)->data)[(i)])

static std::atomic<struct qemu_plugin_register*> rcx_handle{nullptr};
static std::atomic<struct qemu_plugin_register*> rdx_handle{nullptr};
static std::atomic<bool> rcx_lookup_done{false};
static __thread PimidGByteArray* rcx_buf = nullptr;  /* per-host-thread reusable buf */

/* Resolve and cache the %rcx register handle.  Must run in vCPU context (i.e.
 * from within an exec callback registered with QEMU_PLUGIN_CB_R_REGS).  Safe to
 * call repeatedly; only the first successful lookup populates the cache. */
static void resolveRcxHandle() {
    if (rcx_lookup_done.load(std::memory_order_acquire)) return;
    PimidGArray* regs = qemu_plugin_get_registers();
    if (regs) {
        for (unsigned int i = 0; i < regs->len; i++) {
            pimid_qemu_plugin_reg_descriptor* d =
                &PIMID_GARRAY_INDEX(regs, pimid_qemu_plugin_reg_descriptor, i);
            if (d->name && strcmp(d->name, "rcx") == 0) {
                rcx_handle.store(d->handle, std::memory_order_release);
            } else if (d->name && strcmp(d->name, "rdx") == 0) {
                rdx_handle.store(d->handle, std::memory_order_release);
            }
        }
        g_array_free(regs, 1 /* TRUE */);
    }
    rcx_lookup_done.store(true, std::memory_order_release);
}

/* Read the current vCPU's %rcx as a uint64_t.  Returns true and writes *out on
 * success; false if the handle is unavailable or the read fails.  x86-64 is
 * little-endian and qemu_plugin_read_register returns target byte order, so the
 * low byte is first in the buffer. */
static bool readGuestReg(struct qemu_plugin_register* h, uint64_t* out) {
    if (!h) return false;
    if (!rcx_buf) rcx_buf = g_byte_array_sized_new(16);
    g_byte_array_set_size(rcx_buf, 0);
    int n = qemu_plugin_read_register(h, rcx_buf);
    if (n <= 0) return false;
    uint64_t v = 0;
    int bytes = (n < 8) ? n : 8;
    for (int i = 0; i < bytes; i++) {
        v |= ((uint64_t)rcx_buf->data[i]) << (8 * i);
    }
    *out = v;
    return true;
}

static bool readRcx(uint64_t* out) {
    return readGuestReg(rcx_handle.load(std::memory_order_acquire), out);
}

/* Read %rdx -- used to convey the MPI params-block address at MPI_REGISTER. */
static bool readRdx(uint64_t* out) {
    return readGuestReg(rdx_handle.load(std::memory_order_acquire), out);
}

/* NOP function pointers for unscheduled threads */
static void NopLoad(THREADID, ADDRINT) {}
static void NopStore(THREADID, ADDRINT) {}
static void NopBbl(THREADID, ADDRINT, BblInfo*) {}
static void NopBranch(THREADID, ADDRINT, BOOL, ADDRINT, ADDRINT) {}
static void NopPredLoad(THREADID, ADDRINT, BOOL) {}
static void NopPredStore(THREADID, ADDRINT, BOOL) {}

static InstrFuncPtrs nopPtrs = {NopLoad, NopStore, NopBbl, NopBranch,
                                 NopPredLoad, NopPredStore, FPTR_NOP, {0}};

/* Join function pointers -- used when a thread is waiting to be scheduled */
static void JoinLoad(THREADID tid, ADDRINT addr);
static void JoinStore(THREADID tid, ADDRINT addr);
static void JoinBbl(THREADID tid, ADDRINT addr, BblInfo* bbl);

static InstrFuncPtrs joinPtrs = {JoinLoad, JoinStore, JoinBbl, NopBranch,
                                  NopPredLoad, NopPredStore, FPTR_JOIN, {0}};

/* ---- CID/Core management (mirrors zsim.cpp) ---- */

static inline void clearCid(uint32_t tid) {
    cids[tid] = INVALID_CID;
    cores[tid] = nullptr;
}

static inline void setCid(uint32_t tid, uint32_t cid) {
    cids[tid] = cid;
    cores[tid] = zinfo->cores[cid];
    if (cid < MAX_THREADS && g_coreOwner[cid] != tid) {
        g_coreForeignBinds[cid]++;      // 1.9.26: a DIFFERENT thread took this core
        g_coreOwner[cid] = tid;
    }
    // A core marked VACATED by an earlier migration is running again: re-arm it
    // ACTIVE so it re-enters the consistent cut. Only VACATED is lifted here --
    // EXITED (finished ROI) must stay exempt even though its thread keeps
    // executing post-ROI code, and QUIESCENT is lifted by the MPI resume path.
    if (cid < MAX_THREADS) {
        uint32_t vacated = 3;
        g_coreRoiState[cid].compare_exchange_strong(vacated, 0);
    }
}

uint32_t getCid(uint32_t tid) {
    return cids[tid];
}

/* ---- Termination checking (mirrors zsim.cpp) ---- */

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

    if (zinfo->maxMinInstrs) {
        uint64_t minInstrs = zinfo->cores[0]->getInstrs();
        for (uint32_t i = 1; i < zinfo->numCores; i++) {
            uint64_t ci = zinfo->cores[i]->getInstrs();
            if (ci < minInstrs && ci > 0) minInstrs = ci;
        }
        if (minInstrs >= zinfo->maxMinInstrs) {
            zinfo->terminationConditionMet = true;
            info("Max min instructions reached (%ld)", minInstrs);
            return;
        }
    }

    if (zinfo->externalTermPending) {
        zinfo->terminationConditionMet = true;
        info("Terminating due to external notification");
    }
}

/* ---- Phase actions (called by scheduler at end of phase) ---- */

void EndOfPhaseActions() {
    zinfo->profSimTime->transition(PROF_WEAVE);

    if (zinfo->globalPauseFlag) {
        info("Simulation entering global pause");
        zinfo->profSimTime->transition(PROF_FF);
        while (zinfo->globalPauseFlag) usleep(20 * 1000);
        zinfo->profSimTime->transition(PROF_WEAVE);
    }

    /* 1.9.0 thread-MPI deterministic NoC feedback: fold every COMPLETE phase's
     * Garnet records into the per-epoch frozen table HERE -- this is the scheduler
     * sync callback (atSyncFunc), run SINGLE-THREADED at the phase barrier while all
     * cores wait, with numPhases = the phase that just finished (all its accesses
     * recorded). Doing the fold at one deterministic point means a phase's whole
     * record set is replayed together, not split across the drains of whichever
     * access threads raced first -- the root of the 1.8.x epoch-membership
     * nondeterminism. No-op for OMP/process-MPI (they drain inline / via the shm
     * cut). */
    if (g_mpi_thread_mode && zinfo->garnetNetwork &&
        zinfo->garnetNetwork->isCycleAccurate() &&
        zinfo->hierarchy.mpiNocBaselined) {
        // Consistent cut = min per-core roiRel over ACTIVE cores. All cores are
        // synced at this barrier, so their roiRel snapshots are a pure function of
        // simulated state. A core that has EXITED (finished its ROI) or is QUIESCENT
        // (parked in a frozen-clock MPI wait) injects nothing more and is EXEMPT --
        // its state is set at DETERMINISTIC sim events (roi_end / thread-fini / comm
        // brackets), not a wall-dependent advancement test (which glitched at the
        // pre->post ROI-baseline transition where roiRel drops). Records whose whole
        // roiRel-phase bucket is <= cut are final and get folded (foldByCut). No
        // blocking spin -> deadlock-free.
        uint64_t cut = ~0ull;
        int dbgCutCore = -1, dbgActive = 0;
        for (uint32_t c = 0; c < zinfo->numCores && c < MAX_THREADS; c++) {
            Core* co = zinfo->cores[c];
            if (!co) continue;
            if (g_coreRoiState[c].load(std::memory_order_acquire) != 0) continue;
            uint64_t base = co->getRoiBaseCycle();
            uint64_t cyc  = co->getCycles();
            uint64_t rr   = (cyc > base) ? cyc - base : 0;
            if (rr == 0) continue;       // baselined but no ROI traffic yet
            if (rr < cut) { cut = rr; dbgCutCore = (int)c; }
            dbgActive++;
        }
        {   // PIMID_FOLD_DEBUG=1: which core pins the cut, and is it advancing?
            static const bool foldDbg = (getenv("PIMID_FOLD_DEBUG") != nullptr);
            static uint64_t dbgCalls = 0; static uint64_t lastCut = 0; static int lastCore = -1;
            if (foldDbg && ((dbgCalls++ % 100) == 0)) {
                info("[CutDbg] phase=%lu cut=%lu pinnedBy=core%d active=%d cutAdvance=%ld",
                     (unsigned long)zinfo->numPhases, (unsigned long)cut, dbgCutCore,
                     dbgActive, (long)((cut == ~0ull || lastCut == 0) ? 0 : (int64_t)cut - (int64_t)lastCut));
                lastCut = (cut == ~0ull) ? lastCut : cut; lastCore = dbgCutCore;
            }
        }
        zinfo->garnetNetwork->foldByCut(cut, zinfo->phaseLength);
    }

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
        info("Termination condition met, exiting");
        zinfo->sched->leave(procIdx, tid, newCid);
        clearCid(tid);
        SimEnd();
        /* SimEnd calls _exit(0), so we normally never reach here.
         * Return INVALID_CID as safety net for callers' break logic. */
        return INVALID_CID;
    } else {
        fPtrs[tid] = cores[tid]->GetFuncPtrs();
    }

    return newCid;
}

/* Dump the dynamic opcode profile for instructions that fell to the generic/
 * approx uop path (PIMID_OOO_DUMP). Prints the top signatures by execution
 * count so decoder coverage can be prioritized empirically. */
static void dumpApproxProfile() {
    if (!g_ooo_dump) return;
    uint64_t tot = g_dumpTotalInsns.load();
    uint64_t apx = g_dumpApproxInsns.load();
    static const char* mapName[4] = {"1B ", "0F ", "0F38", "0F3A"};
    // Collect nonzero slots
    std::vector<std::pair<uint64_t,uint16_t>> v;
    for (uint32_t s = 0; s < OOO_DUMP_SLOTS; s++) {
        uint64_t c = g_dumpDyn[s].load();
        if (c) v.push_back(std::make_pair(c, (uint16_t)s));
    }
    std::sort(v.begin(), v.end(), [](const std::pair<uint64_t,uint16_t>&a,
                                     const std::pair<uint64_t,uint16_t>&b){return a.first>b.first;});
    info("==== OOO DECODE DUMP: %lu dynamic instrs, %lu approx (%.1f%%), %zu distinct approx opcodes ====",
         (unsigned long)tot, (unsigned long)apx,
         tot ? (100.0*apx/tot) : 0.0, v.size());
    uint32_t lim = v.size() < 80 ? (uint32_t)v.size() : 80;
    for (uint32_t k = 0; k < lim; k++) {
        int mi, op, reg; x86dec::dbgUnpack(v[k].second, mi, op, reg);
        info("  APPROX map=%s op=0x%02x /%d  dyn=%lu (%.2f%%)",
             mapName[mi & 3], op, reg, (unsigned long)v[k].first,
             tot ? (100.0*v[k].first/tot) : 0.0);
    }
    info("==== END OOO DECODE DUMP ====");
}

/* ---- SimEnd ---- */

/* 1.8.7 protocol-tail RECEIPT, computed ONCE just before the stats dump
 * (single-threaded guest-exit path). For each marked co-sim MPI rank (== PE):
 *   tail = final PE cycles - the PE's closing-barrier marker
 * and record it in g_mpiProtocolTailCyc[] for the explicit 'protocolTail' stat.
 * NO ledger move: the tail is device-resident inter-PE collective work and
 * correctly stays in the PE's 'cycles'. This only exposes its magnitude. */
static void recordProtocolTailStats() {
    if (!(g_mpi_thread_mode && g_cosim_mode && !g_cosim_no_offload)) return;
    for (uint32_t tid = 0; tid < MAX_THREADS; tid++) {
        if (!g_tailMarked[tid]) continue;
        uint32_t dcid = g_tailDeviceCid[tid];
        if (dcid >= zinfo->numCores || !zinfo->cores[dcid]) continue;
        uint64_t finalDev = zinfo->cores[dcid]->getCycles();
        uint64_t tail = (finalDev > g_tailMarker[tid]) ? (finalDev - g_tailMarker[tid]) : 0;
        g_mpiProtocolTailCyc[dcid] = tail;   // receipt only (fixed global array), no ledger move
        info("Protocol tail: rank %u PE(cid %u) post-ROI inter-PE collective tail %lu cyc "
             "(device-resident)", tid, dcid, (unsigned long)tail);
    }
}

/* Dump the termination stats exactly once. Split out of SimEnd so an MPI
 * rank can freeze its ROI-scoped stats at ROI END deterministically while the
 * process lives on to finish its MPI protocol (closing barrier, serving
 * reduces as root). The later real SimEnd (natural guest exit) skips the
 * re-dump. */
/* 1.11.40 (audit E17): dump the inter-event gap histograms. They answer the
 * question E17 raised before any residency redesign is attempted: does idle
 * time at a gating-relevant scale actually EXIST in these workloads, and how
 * much of it?
 *
 * 1.11.57 (audit round 3, residual on F006): "MEASUREMENT ONLY -- nothing in
 * the power model reads these" is FALSE of the devMC half and has been since
 * 1.11.51, which reads the devMC buckets back to weight DRAM power-down
 * residency (init.cpp exports them; src/main.cpp replaces mc_r_idle from
 * them). 1.11.55 corrected the PRINTED claim below and left this comment
 * standing, so the file still opened with the retracted sentence. The claim
 * survives only for the NoC half, where no sourced wake penalty exists.
 *
 * The reported thresholds are the wake penalties a real scheme would pay:
 *   tXP    DRAM precharge power-down exit, ~10 ns. At the device clock that is
 *          a handful of cycles; printed in cycles at the run's own frequency
 *          so the reader is not converting in their head.
 *   100 ns / 1 us  reference points either side of it, so the shape of the
 *          distribution is visible rather than a single number.
 * "usable" subtracts the wake penalty from every qualifying gap, which is what
 * the gap actually yields -- entry and exit are paid out of the idle window. */
static void dumpGapHistograms() {
    struct Item { const char* name; GapHist* h; };
    Item items[2] = { {"noc", &zinfo->pgres.nocGaps},
                      {"devMC", &zinfo->pgres.devMCGaps} };
    double mhz = (zinfo->freqMHz > 0) ? (double)zinfo->freqMHz : 1000.0;
    /* Denominator is the ROI span each histogram actually covers, NOT
     * globPhaseCycles. globPhaseCycles is phases x phaseLength over the WHOLE
     * run -- 7086 phases on the detailed cell where the priced window is 1016 --
     * so dividing by it mixed a whole-run denominator with ROI-gated events and
     * reported percentages above 100%. See GapHist's comment. */
    info("[E17 gap histogram] clock %.0f MHz, whole run %lu cyc "
         "(%lu phases); percentages below are against each component's own "
         "ROI span. Since 1.11.51 the devMC buckets ARE read back: they "
         "weight the DRAM power-down residency (audit F006 -- the previous "
         "'no model reads this' is stale). NoC buckets remain "
         "measurement-only: no sourced wake penalty exists for a logic "
         "fabric and none is invented.",
         mhz, (unsigned long)zinfo->globPhaseCycles,
         (unsigned long)zinfo->numPhases);
    for (Item& it : items) {
        GapHist* h = it.h;
        if (h->events == 0) { info("[E17 %s] no events", it.name); continue; }
        uint64_t total = h->spanCycles();
        if (total == 0) { info("[E17 %s] zero span", it.name); continue; }
        info("[E17 %s] ROI span %lu cyc (%.1f%% of the whole run)", it.name,
             (unsigned long)total,
             zinfo->globPhaseCycles ? 100.0 * total / zinfo->globPhaseCycles : 0.0);
        info("[E17 %s] events=%lu samples=%lu (dropped=%lu)", it.name,
             (unsigned long)h->events, (unsigned long)h->samples,
             (unsigned long)(h->events - h->samples));
        uint64_t inGaps = 0;
        for (int b = 0; b < GapHist::kBuckets; b++) {
            if (h->count[b] == 0) continue;
            inGaps += h->cycles[b];
            info("[E17 %s] gap 2^%-2d (%10.1f ns) count=%-10lu cycles=%-12lu",
                 it.name, b, (double)(1ull << b) * 1000.0 / mhz,
                 (unsigned long)h->count[b], (unsigned long)h->cycles[b]);
        }
        /* 1.11.57 (audit round 3, residual on F007): "% of run" against an
         * ROI-SPAN divisor. `total` is spanCycles(), the window the samples
         * cover -- on the detailed cell the surrounding comment cites, that
         * is 1016 phases of 7086, so a reader who took the label at face
         * value was off by ~7x in the reassuring direction. 1.11.55 listed
         * this among its truth-in-comment corrections and fixed only the
         * sibling line fourteen lines above; this one kept the wrong word,
         * which is worse than either -- two lines in one block using "of run"
         * and "of span" for the same divisor. */
        info("[E17 %s] cycles in gaps total=%lu (%.2f%% of span)", it.name,
             (unsigned long)inGaps, total ? 100.0 * inGaps / total : 0.0);
        const double ns_thresh[3] = {10.0, 100.0, 1000.0};
        const char*  ns_name[3]   = {"tXP~10ns", "100ns", "1us"};
        for (int i = 0; i < 3; i++) {
            uint64_t th = (uint64_t)(ns_thresh[i] * mhz / 1000.0);
            if (th < 1) th = 1;
            uint64_t u = h->cyclesInGapsOver(th);
            info("[E17 %s] usable idle above %-9s (%4lu cyc): %-12lu = %.2f%% of span",
                 it.name, ns_name[i], (unsigned long)th, (unsigned long)u,
                 total ? 100.0 * u / total : 0.0);
        }
    }
}

static volatile uint32_t g_termStatsDumped = 0;
static void dumpTerminationStats() {
    if (!__sync_bool_compare_and_swap(&g_termStatsDumped, 0, 1)) return;

    recordProtocolTailStats();   // 1.8.7: fill the protocolTail receipt BEFORE the dump
    info("Dumping termination stats");
    zinfo->trigger = 20000;
    for (StatsBackend* backend : *(zinfo->statsBackends)) {
        backend->dump(false);
    }

    dumpGapHistograms();   // 1.11.40 (audit E17): measurement only

    if (zinfo->garnetNetwork) {
        zinfo->garnetNetwork->setTotalCycles(zinfo->globPhaseCycles);
        std::string garnetStatsPath = std::string(zinfo->outputDir) + "/garnet_stats.txt";
        zinfo->garnetNetwork->writeStatsFile(garnetStatsPath.c_str());
        zinfo->garnetNetwork->printStats();
    }
}

void SimEnd() {
    dumpApproxProfile();
    if (zinfo && zinfo->garnetNetwork) zinfo->garnetNetwork->dumpInjectionCensus();
    if (g_ooo_present) {
        uint64_t mispred = 0;
        for (uint32_t c = 0; c < zinfo->numCores; c++)
            if (zinfo->cores[c] && zinfo->cores[c]->asOOOCore()) {
                // mispredBranches is a private OOO counter surfaced via stats;
                // aggregate from the stats tree is complex here, so report the
                // fed-branch total and let zsim.out carry per-core mispredicts.
            }
        (void)mispred;
        uint64_t br = g_brDynCount.load();
        info("==== OOO BRANCH PREDICTOR: %lu dynamic conditional branches resolved "
             "and fed to the predictor (per-core mispredBranches in zsim.out) ====",
             (unsigned long)br);
    }
    if (__sync_bool_compare_and_swap(&perProcessEndFlag, 0, 1) == false) {
        while (true) {
            struct timespec tm;
            tm.tv_sec = 1;
            tm.tv_nsec = 0;
            nanosleep(&tm, nullptr);
        }
    }

    dumpTerminationStats();

    if (zinfo->mpiStats.messages > 0) {
        info("MPI Stats: %lu messages, %lu total latency cycles, %lu barriers",
             zinfo->mpiStats.messages, zinfo->mpiStats.totalLatency,
             zinfo->mpiStats.barrierCount);
    }

    if (zinfo->coherence.enabled) {
        info("Coherence Stats: %s mode, %lu roi_begin flushes, %lu total flush cycles charged",
             zinfo->coherence.mode == 0 ? "unified" : "separate",
             (unsigned long)zinfo->coherence.flushCount,
             (unsigned long)zinfo->coherence.flushCyclesCharged);
    }

    if (zinfo->sched) zinfo->sched->notifyTermination();

    /* Terminate the process.  Background threads (contention sim, scheduler
     * watchdog) won't exit on their own, same as the original ZSim behaviour.
     * Use _exit() rather than exit() to avoid re-entering atexit/plugin_exit. */
    _exit(0);
}

/* ---- Thread management ---- */

static void SimThreadStart(uint32_t tid) {
    info("Thread %d starting", tid);
    if (tid > MAX_THREADS) {
        panic("tid > MAX_THREADS");
    }
    ProcessTreeNode* procTree = zinfo->procArray[procIdx];
    zinfo->sched->start(procIdx, tid, procTree->getMask());

    fPtrs[tid] = joinPtrs;
    clearCid(tid);
}

static void SimThreadFini(uint32_t tid) {
    // Thread left permanently -> EXITED (exempt from the epoch cut).
    if (g_mpi_thread_mode && tid < MAX_THREADS && cids[tid] < MAX_THREADS)
        g_coreRoiState[cids[tid]].store(2, std::memory_order_release);
    zinfo->sched->finish(procIdx, tid);
    cids[tid] = UNINITIALIZED_CID;
}

/**
 * Deferred thread initialization -- called on first execution callback.
 * Uses the thread's domain to select the appropriate core-type mask.
 */
/* 1.9.20: the device-side mask for this thread -- a single-PE home mask when
 * pinning is on and this rank can own a home outright, else the full device
 * mask (old behaviour). Ownership is permanent for the life of the run: a rank
 * that migrates back to the host and returns later reclaims the same PE, so the
 * placement is stable end-to-end and not just per-migration. */
static const g_vector<bool>& deviceMaskFor(uint32_t tid) {
    if (!g_pin_device_homes || !g_mpi_thread_mode) return device_mask;
    if (tid >= MAX_THREADS || device_core_list.empty()) return device_mask;

    uint32_t vcpu = tidToVcpu[tid];
    if (vcpu == UNINITIALIZED_CID) return device_mask;

    std::lock_guard<std::mutex> lock(pinMutex);
    uint32_t idx = vcpu % (uint32_t)device_core_list.size();
    /* Collision guard: a non-bijective vcpu set (more device threads than PEs,
     * or non-contiguous vcpu indices aliasing under the modulo) would pin two
     * ranks to one PE, and two ranks that must run concurrently -- across an MPI
     * barrier, say -- would then deadlock against each other. Fall back to the
     * unpinned mask for the loser rather than risk that; the run stays correct
     * and merely keeps the old nondeterminism. */
    if (device_home_owner[idx] != UNINITIALIZED_CID &&
        device_home_owner[idx] != vcpu) {
        if (!g_pin_fallback_warned) {
            g_pin_fallback_warned = true;
            warn("[ZSim] 1.9.20 PE pinning: vcpu %u collides with vcpu %u on "
                 "device PE %u (%lu PEs); falling back to the unpinned device "
                 "mask for colliding ranks -- run-to-run placement will NOT be "
                 "deterministic. Set PIMID_NO_PE_PIN=1 to disable pinning.",
                 vcpu, device_home_owner[idx], device_core_list[idx],
                 (unsigned long)device_core_list.size());
        }
        return device_mask;
    }
    device_home_owner[idx] = vcpu;

    g_vector<bool>& m = device_home_mask[tid];
    if (m.size() != zinfo->numCores) m.resize(zinfo->numCores, false);
    for (uint32_t i = 0; i < zinfo->numCores; i++) m[i] = false;
    m[device_core_list[idx]] = true;
    return m;
}

static void ensureThreadInit(uint32_t tid) {
    if (thread_initialized[tid]) return;
    thread_initialized[tid] = true;

    const g_vector<bool>& mask =
        (thread_domain[tid].load() == DOMAIN_DEVICE) ? deviceMaskFor(tid)
                                                     : host_mask;

    info("Thread %d starting (domain=%s)", tid,
         thread_domain[tid].load() == DOMAIN_DEVICE ? "DEVICE" : "HOST");

    if (tid >= MAX_THREADS) {
        panic("tid >= MAX_THREADS");
    }
    COSIMTRACE("tid=%u sched->start enter (domain=%s)", tid,
               thread_domain[tid].load() == DOMAIN_DEVICE ? "DEV" : "HOST");
    zinfo->sched->start(procIdx, tid, mask);
    COSIMTRACE("tid=%u sched->start done", tid);
    fPtrs[tid] = joinPtrs;
    clearCid(tid);
}

/* ---- Join functions (thread waits for scheduler) ---- */

static std::atomic<uint32_t> trace_join_count[MAX_THREADS];

static void Join(uint32_t tid) {
    /* trace only each thread's first few joins -- the startup hang window */
    bool tr = g_cosim_trace && tid < MAX_THREADS && trace_join_count[tid]++ < 6;
    if (tr) COSIMTRACE("tid=%u sched->join enter", tid);
    uint32_t cid = zinfo->sched->join(procIdx, tid);
    if (tr) COSIMTRACE("tid=%u sched->join done cid=%u", tid, cid);
    setCid(tid, cid);

    if (unlikely(zinfo->terminationConditionMet)) {
        info("Caught termination on join, exiting");
        zinfo->sched->leave(procIdx, tid, cid);
        SimEnd();
    }

    fPtrs[tid] = cores[tid]->GetFuncPtrs();
}

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

/* ---- BblInfo cache ---- */

static BblInfo* getOrCreateBblInfo(uint64_t tbAddr, uint32_t numInsns, uint32_t tbBytes,
                                   struct qemu_plugin_tb* tb) {
    std::lock_guard<std::mutex> lock(bblCacheMutex);

    auto it = bblCache.find(tbAddr);
    if (it != bblCache.end()) {
        return it->second;
    }

    BblInfo* bbl = nullptr;
    /* Decode into DynUops + class census (since 1.11.15: for every core
     * type, not just OOO). Cap TB size to keep the transient decode buffers
     * bounded; oversized TBs fall back to the simple path with a ZERO census
     * (they are rare and typically not hot compute loops) -- the wrapper's
     * deficit guard sees that under-coverage and falls back to fractions if
     * it exceeds 5%. */
    if (g_decode_enabled && tb && numInsns > 0 &&
            numInsns <= 1024) {
        static const uint32_t MAXI = 1024;
        // Local per-call buffers (each thread translates independently; this is
        // on the stack, ~17KB, acceptable). Gather bytes+lengths for the decoder.
        static thread_local uint8_t bytesBuf[MAXI][16];
        static thread_local uint8_t lenBuf[MAXI];
        uint32_t n = numInsns;
        for (uint32_t k = 0; k < n; k++) {
            struct qemu_plugin_insn* insn = qemu_plugin_tb_get_insn(tb, k);
            size_t sz = qemu_plugin_insn_size(insn);
            if (sz > 16) sz = 16;
            lenBuf[k] = (uint8_t)sz;
            qemu_plugin_insn_data(insn, bytesBuf[k], 16);
        }
        if (g_ooo_dump) {
            std::vector<uint16_t> keys;
            bbl = x86dec::createDecodedBblInfo(tbAddr, bytesBuf, lenBuf, n, tbBytes, &keys);
            uint16_t* arr = nullptr;
            if (!keys.empty()) {
                arr = (uint16_t*)malloc(keys.size() * sizeof(uint16_t));
                memcpy(arr, keys.data(), keys.size() * sizeof(uint16_t));
            }
            g_tbApproxCache[tbAddr] = std::make_pair(arr, (uint32_t)keys.size());
        } else {
            bbl = x86dec::createDecodedBblInfo(tbAddr, bytesBuf, lenBuf, n, tbBytes);
        }
    } else {
        bbl = createSimpleBblInfo(numInsns, tbBytes);
    }
    bblCache[tbAddr] = bbl;
    return bbl;
}

/* ---- QEMU vcpu -> ZSim tid mapping ---- */

static uint32_t getOrAssignTid(unsigned int vcpu_index) {
    if (vcpu_index < MAX_VCPUS && vcpuToTid[vcpu_index] != UNINITIALIZED_CID) {
        return vcpuToTid[vcpu_index];
    }

    std::lock_guard<std::mutex> lock(tidMutex);
    /* Double-check under lock */
    if (vcpu_index < MAX_VCPUS && vcpuToTid[vcpu_index] != UNINITIALIZED_CID) {
        return vcpuToTid[vcpu_index];
    }

    uint32_t tid = nextTid++;
    if (vcpu_index < MAX_VCPUS) {
        vcpuToTid[vcpu_index] = tid;
    }
    /* Remember the stable identity behind this (raced) tid so the device-home
     * assignment below can key on vcpu_index instead. */
    if (tid < MAX_THREADS) tidToVcpu[tid] = vcpu_index;
    return tid;
}

/* ---- Userdata struct passed from tb_trans to callbacks ---- */

struct TbUserdata {
    BblInfo* bblInfo;
    uint64_t tbAddr;
    /* PIMID_OOO_DUMP only: packed signatures of this TB's approx instructions,
     * plus its total instruction count, for dynamic opcode profiling. */
    uint16_t* approxKeys;
    uint32_t  numApproxKeys;
    uint32_t  numInsns;
    /* Branch-predictor wiring (OOO only): if this TB ends in a conditional
     * jcc, its PC and the two successor addresses, so the real taken/not-taken
     * direction can be resolved from the actual next TB and fed to the OOO
     * core's branch predictor. endsInCondBranch=false otherwise. */
    bool      endsInCondBranch;
    uint64_t  brPc;
    uint64_t  brTakenTarget;
    uint64_t  brFallthrough;
    /* Indirect control-flow wiring: if this TB's last instruction is a direct
     * call (E8), indirect call (FF /2,/3), indirect jmp (FF /4,/5), or ret
     * (C3/C2), termKind holds the CtrlFlowKind code (2..5, see ooo_core.h) and
     * termPc/termRetAddr its PC and (for calls) fall-through return address.
     * The actual target is resolved from the NEXT TB's start address, exactly
     * like the conditional-direction wiring above. termKind=0 otherwise.
     * Mutually exclusive with endsInCondBranch (one terminator per TB). */
    uint8_t   termKind;
    uint64_t  termPc;
    uint64_t  termRetAddr;
};

/* Per-thread pending conditional branch awaiting direction resolution (set when
 * a jcc-terminated TB executes, resolved by the NEXT TB's address). OOO only. */
static bool     g_brPending[MAX_THREADS];
static uint64_t g_brPc[MAX_THREADS];
static uint64_t g_brTaken[MAX_THREADS];
static uint64_t g_brFall[MAX_THREADS];

/* Per-thread pending indirect/call/ret terminator awaiting target resolution
 * (same next-TB mechanism; fed to the core as a CtrlFlowKind code >= 2 through
 * the branchPtr callback). 0 = none pending. */
static uint8_t  g_ctrlPending[MAX_THREADS];
static uint64_t g_ctrlPc[MAX_THREADS];
static uint64_t g_ctrlRet[MAX_THREADS];

/* ---- QEMU Callbacks ---- */

/**
 * Memory access callback -- dispatches to ZSim's load/store function pointers.
 */
static void mem_cb(unsigned int vcpu_index,
                   qemu_plugin_meminfo_t info,
                   uint64_t vaddr,
                   void *userdata) {
    if (!in_roi) return;
    if (vcpu_index < MAX_VCPUS && vcpu_is_internal[vcpu_index]) return;

    uint32_t tid = getOrAssignTid(vcpu_index);
    if (tid >= MAX_THREADS) return;
    ensureThreadInit(tid);

    /* Inside an MPI comm window: skip counting the transport's own accesses
     * (keeps the core attached but the wall-clock poll invisible). */
    if (mpi_comm_window[tid].load(std::memory_order_acquire)) return;

    if (vcpu_index < MAX_VCPUS) in_zsim[vcpu_index] = true;
    if (qemu_plugin_mem_is_store(info)) {
        fPtrs[tid].storePtr(tid, vaddr);
    } else {
        fPtrs[tid].loadPtr(tid, vaddr);
    }
    if (vcpu_index < MAX_VCPUS) in_zsim[vcpu_index] = false;
}

/* Move a thread to the given domain by leaving its current core and
 * re-initializing against the domain's core mask -- the exact sequence the
 * ROI/WORK offload handlers use inline, factored for the 1.8.4 lazy
 * window migration. */
static void migrateThreadToDomain(uint32_t tid, int domain) {
    thread_domain[tid].store(domain);
    if (thread_initialized[tid]) {
        uint32_t cid = cids[tid];
        if (cid != INVALID_CID && cid != UNINITIALIZED_CID) {
            zinfo->sched->leave(procIdx, tid, cid);
        }
        zinfo->sched->finish(procIdx, tid);
        thread_initialized[tid] = false;
        clearCid(tid);
    }
    ensureThreadInit(tid);
}

/* 1.8.4 concurrent-safe split of the above. detach runs under
 * g_migrateMutex (non-blocking bookkeeping only); the re-init -- which can
 * BLOCK on the scheduler's phase machinery -- must run OUTSIDE the lock, or
 * a barrier-release stampede deadlocks (one rank blocks in the join while
 * the phase system waits on ranks queued behind the mutex; v184 round 5). */
static void detachThreadForMigration(uint32_t tid, int domain) {
    thread_domain[tid].store(domain);
    if (thread_initialized[tid]) {
        uint32_t cid = cids[tid];
        if (cid != INVALID_CID && cid != UNINITIALIZED_CID) {
            // The core being vacated keeps its cycle count but will never
            // advance again: no thread runs on it after this migration, so it
            // injects nothing further and its roiRel is final. Mark it EXITED
            // so it drops out of the consistent cut. Without this, a host core
            // that did ROI work before its rank migrated to the device stays
            // ACTIVE at a frozen roiRel and pins the cut forever: no record
            // bucket can then satisfy "whole bucket below the cut", folding
            // stops entirely, and the pending set grows without bound (observed
            // at 64M records and climbing, with the cut unchanged across 71k
            // phases). Set at the migration itself, a deterministic simulation
            // event, not by a wall-dependent advancement test.
            if (g_mpi_thread_mode && cid < MAX_THREADS)
                g_coreRoiState[cid].store(3, std::memory_order_release);
            zinfo->sched->leave(procIdx, tid, cid);
        }
        zinfo->sched->finish(procIdx, tid);
        thread_initialized[tid] = false;
        clearCid(tid);
    }
}

/**
 * Instruction execution callback -- called on first instruction of each TB.
 * Drives the BBL function pointer which triggers phase counting.
 */
static void insn_exec_cb(unsigned int vcpu_index, void *userdata) {
    if (!in_roi) return;
    if (vcpu_index < MAX_VCPUS && vcpu_is_internal[vcpu_index]) return;

    TbUserdata* tud = (TbUserdata*)userdata;
    uint32_t tid = getOrAssignTid(vcpu_index);
    if (tid >= MAX_THREADS) return;

    /* 1.8.4 co-sim ROI window (defect #14): an MPI rank alive BEFORE the
     * window opened was born DOMAIN_HOST and cannot inherit the device
     * domain at birth the way OMP workers forked inside the ROI do. It
     * migrates ITSELF here, at its first BBL after the window opens.
     * Deterministic under the serial weave. */
    if (unlikely(g_mpi_thread_mode && g_cosim_mode && !g_cosim_no_offload &&
                 g_in_device_region.load(std::memory_order_acquire) &&
                 thread_domain[tid].load() == DOMAIN_HOST)) {
        /* 1.8.5: per-rank flush+launch charges reinstated (mirrors the
         * entry-barrier leg). Charge on THIS rank's host core -- still
         * DOMAIN_HOST and scheduled (it has been feeding host BBLs, so it is
         * initialized) -- BEFORE the locked detach and OUTSIDE g_migrateMutex
         * (v184 round 5: drainHostCharges can block the phase system, so it
         * must not run under the migrate mutex). */
        if (thread_initialized[tid]) {
            chargeCoherenceFlush(tid);   // PART A (1.7.2): Case-1 coherence flush
            chargeLaunchCost(tid);       // PART B (1.7.3): kernel launch cost
            drainHostCharges(tid);       // retire flush+launch onto the host timeline
        }
        {
            std::lock_guard<std::mutex> mig(g_migrateMutex);
            detachThreadForMigration(tid, DOMAIN_DEVICE);
            g_deviceWorkers.fetch_add(1);
        }
        ensureThreadInit(tid);  // may block on the scheduler: OUTSIDE the lock
        info("Thread %d: ROI window migrate-in (co-sim MPI rank, flush+launch charged) [1.8.5]", tid);
    }

    ensureThreadInit(tid);

    /* Inside an MPI comm window: keep the core attached but do not count the
     * transport's own instructions (the wall-clock poll stays invisible). */
    if (mpi_comm_window[tid].load(std::memory_order_acquire)) return;

    /* Dynamic opcode profiling (PIMID_OOO_DUMP): tally this TB's approx insns
     * weighted by execution count. Lock-free (per-TB array + atomic slots). */
    if (unlikely(g_ooo_dump)) {
        g_dumpTotalInsns.fetch_add(tud->numInsns, std::memory_order_relaxed);
        g_dumpApproxInsns.fetch_add(tud->numApproxKeys, std::memory_order_relaxed);
        for (uint32_t k = 0; k < tud->numApproxKeys; k++) {
            uint16_t key = tud->approxKeys[k];
            if (key < OOO_DUMP_SLOTS)
                g_dumpDyn[key].fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (vcpu_index < MAX_VCPUS) in_zsim[vcpu_index] = true;
    /* Branch predictor feed: resolve the previous TB's conditional branch using
     * THIS TB's address as the real next-PC, then feed direction+targets to the
     * core BEFORE bbl() (bbl() consumes branchPc when timing the prev BBL). Only
     * OOO and (decode-enabled) in-order cores get branch callbacks, so
     * alu/simple/null stay byte-identical.
     * NOTE: the OOO leg now also checks !g_ooo_decode_disabled -- a no-op today,
     * since endsInCondBranch is only ever set when decode ran for the TB, but it
     * keeps OOO's feed provably unchanged in mixed-core configs where the
     * in-order presence alone enables decode. */
    bool feedBranch = false;
    if (cores[tid]) {
        if (g_ooo_present && !g_ooo_nobranch && !g_ooo_decode_disabled &&
                cores[tid]->asOOOCore()) feedBranch = true;
        else if (g_inorder_present && !g_inorder_nobranch &&
                cores[tid]->asInOrderCore()) feedBranch = true;
    }
    if (feedBranch) {
        if (g_brPending[tid]) {
            bool taken = (tud->tbAddr == g_brTaken[tid]);
            fPtrs[tid].branchPtr(tid, g_brPc[tid], taken ? 1 : 0,
                                 g_brTaken[tid], g_brFall[tid]);
            g_brDynCount.fetch_add(1, std::memory_order_relaxed);
        }
        /* Indirect/call/ret terminator of the previous TB: THIS TB's start is
         * its actual target. Feed as a CtrlFlowKind code (>= 2) through the
         * same branchPtr callback (BOOL carries the kind; takenNpc = resolved
         * target; notTakenNpc = call fall-through/return address). Routed via
         * fPtrs so unscheduled (nop/join) phases drop it exactly like the
         * conditional feed. */
        if (g_ctrlPending[tid]) {
            fPtrs[tid].branchPtr(tid, g_ctrlPc[tid], g_ctrlPending[tid],
                                 tud->tbAddr, g_ctrlRet[tid]);
            g_ctrlPending[tid] = 0;
        }
        if (tud->endsInCondBranch) {
            g_brPending[tid] = true;
            g_brPc[tid] = tud->brPc;
            g_brTaken[tid] = tud->brTakenTarget;
            g_brFall[tid] = tud->brFallthrough;
        } else {
            g_brPending[tid] = false;
        }
        if (tud->termKind) {
            g_ctrlPending[tid] = tud->termKind;
            g_ctrlPc[tid] = tud->termPc;
            g_ctrlRet[tid] = tud->termRetAddr;
        }
    }
    fPtrs[tid].bblPtr(tid, tud->tbAddr, tud->bblInfo);
    if (vcpu_index < MAX_VCPUS) in_zsim[vcpu_index] = false;
}

/**
 * Handle an MPI magic op for the given thread.
 * Computes NoC + hierarchy latency and injects it as a synthetic BBL.
 */
static void handleMpiMagicOp(uint64_t op, uint32_t tid) {
    if (tid >= MAX_THREADS) return;

    if (op == ZSIM_MAGIC_OP_MPI_REGISTER) {
        /* Registration: the caller put the address of its thread-local
         * PimidMpiParams block in %rdx (opcode is in %rcx).  Record it so
         * subsequent MPI_SEND/RECV can read {src_pe, dst_pe, msg_size} and
         * charge the device NoC.  Guest and host share the address space in
         * QEMU user-mode, so the host-side pointer is directly usable. */
        uint64_t addr = 0;
        if (zinfo && readRdx(&addr) && addr != 0) {
            zinfo->mpiParamsAddr[tid] = addr;
            info("Thread %d: MPI_REGISTER (params@%p)", tid, (void*)addr);
        } else {
            info("Thread %d: MPI_REGISTER (no params addr)", tid);
        }
        return;
    }

    if (op == ZSIM_MAGIC_OP_MPI_COMM_BEGIN) {
        /* 1.6 thread mode: FROZEN-CLOCK WAIT. Do NOT attach the window (a
         * frozen-attached core deadlocks the multi-core phase barrier); the
         * thread parks detached, phases advance, and COMM_END rewinds the
         * core by the wait's raw cycle growth -- the wall-dependent rejoin
         * fast-forward is erased and the rendezvous ADVANCE re-places the
         * clock from deterministic arithmetic alone. */
        if (g_mpi_thread_mode) {
            if (tid < MAX_THREADS && cores[tid]) {
                mpi_comm_saved[tid] = cores[tid]->getCycles();
                mpi_comm_saved_cid[tid] = cids[tid];
                mpi_comm_saved_fb[tid] =
                    (cids[tid] < MAX_THREADS) ? g_coreForeignBinds[cids[tid]] : 0;
                cores[tid]->pimidCommParked = true;
                // Parked in a frozen-clock MPI wait -> QUIESCENT (exempt from the
                // epoch cut) so it does not pin the barrier for still-running peers.
                if (cids[tid] < MAX_THREADS)
                    g_coreRoiState[cids[tid]].store(1, std::memory_order_release);
            }
            /* 1.9.0 epoch-frozen feedback: a rank parked in a frozen-clock MPI
             * wait injects nothing, so it must be QUIESCENT -- exempt from the
             * consistent cut AND the epoch bounded-lag stall -- or its stale
             * watermark would pin the epoch barrier on a parked peer. No-op in
             * pure thread mode (no PIMID_NOC_SHM); load-bearing when a detailed
             * process-MPI rank shares the shm NoC. */
            pimid_noc_mark_state(PIMID_NOC_QUIESCENT);
            return;
        }
        /* Process mode: keep the core attached across transport sleeps and
         * stop counting the transport's own instructions until COMM_END.
         * Shared detailed-MPI Garnet: a blocked guest injects nothing -> mark
         * QUIESCENT so the merged-replay cut never waits on this rank. */
        mpi_comm_window[tid].store(true, std::memory_order_release);
        pimid_noc_mark_state(PIMID_NOC_QUIESCENT);
        return;
    }
    if (op == ZSIM_MAGIC_OP_MPI_COMM_END) {
        if (g_mpi_thread_mode) {
            if (tid < MAX_THREADS && cores[tid] && mpi_comm_saved[tid] != ~0ull) {
                cores[tid]->pimidCommParked = false;
                /* 1.9.26: the revert measures the snap as "clock now minus
                 * clock when I blocked", which is only THIS rank's snap if the
                 * core was its alone. On a device PE it is (1:1 after 1.9.20).
                 * On the shared co-sim host core, other ranks ran on the same
                 * clock meanwhile, so the difference is snap PLUS their work and
                 * reverting it destroys that work. Require exclusive ownership:
                 * same core, and no bind by a different thread in between. */
                uint64_t raw = cores[tid]->getCycles();
                bool sameCid = (mpi_comm_saved_cid[tid] == cids[tid]);
                bool exclusive = sameCid && cids[tid] < MAX_THREADS &&
                                 g_coreForeignBinds[cids[tid]] == mpi_comm_saved_fb[tid];
                if (exclusive && raw > mpi_comm_saved[tid])
                    cores[tid]->pimidRewindCycles(raw - mpi_comm_saved[tid]);
                mpi_comm_saved[tid] = ~0ull;
            }
            // Resumed from the MPI wait -> ACTIVE (re-enters the epoch cut). Do not
            // clobber an EXITED core (roi_end already fired): only ACTIVE<-QUIESCENT.
            if (tid < MAX_THREADS && cids[tid] < MAX_THREADS) {
                uint32_t q = 1;
                g_coreRoiState[cids[tid]].compare_exchange_strong(q, 0);
            }
            /* 1.9.0 epoch-frozen feedback: rank resumes injecting -> ACTIVE, so
             * the cut/epoch barrier accounts for it again. Publishing ACTIVE here
             * (paired with QUIESCENT at COMM_BEGIN) is the single most important
             * interaction with the frozen-clock brackets. The COMM_END rewind and
             * the epoch boundaries share the roiRel axis, so the rewind cannot move
             * a later access across an epoch boundary wall-dependently. No-op in
             * pure thread mode (no PIMID_NOC_SHM). */
            pimid_noc_mark_state(PIMID_NOC_ACTIVE);
            return;
        }
        mpi_comm_window[tid].store(false, std::memory_order_release);
        pimid_noc_mark_state(PIMID_NOC_ACTIVE);
        return;
    }

    if (op == ZSIM_MAGIC_OP_REG_DEVBUF) {
        /* Retired (the device model owns its addresses; nothing to register).
         * Kept as an explicit no-op so old binaries don't fall through to
         * the MPI send/recv params path. */
        return;
    }

    if (op == ZSIM_MAGIC_OP_PRICE_PAUSE || op == ZSIM_MAGIC_OP_PRICE_RESUME) {
        /* DMA window: an explicit staging memcpy is about to run whose
         * transfer cost is charged as a sized link transfer -- suspend
         * per-access pricing so the copy loop is not double-charged. */
        if (zinfo && cores[tid]) {
            cores[tid]->setMemPricingPaused(op == ZSIM_MAGIC_OP_PRICE_PAUSE);
        }
        return;
    }

    if (op == ZSIM_MAGIC_OP_MPI_BARRIER) {
        /* 1.8.4 co-sim ROI window (defect #14), KERNEL-ENTRY migration.
         * Rank 0's roi_begin cannot open the window early enough: a parked
         * rank 0 wakes AFTER the other ranks have already raced through
         * their compute (observed in the v184 first-fed-BBL diagnostic).
         * The entry barrier is the one point every rank passes BEFORE its
         * compute IN ITS OWN PROGRAM ORDER -- the same first-barrier =
         * kernel-entry convention the per-rank ROI baseline below already
         * relies on. Each rank charges its own coherence flush + launch on
         * ITS host core, then migrates ITSELF to its device PE. */
        if (unlikely(g_mpi_thread_mode && g_cosim_mode && !g_cosim_no_offload &&
                     !g_roiClosing.load() && tid < MAX_THREADS &&
                     thread_domain[tid].load() == DOMAIN_HOST)) {
            /* 1.8.5: per-rank flush+launch charges reinstated. Each rank
             * charges its OWN host core here -- still DOMAIN_HOST, still
             * scheduled -- BEFORE the locked detach and OUTSIDE g_migrateMutex.
             * v184 round 5: drainHostCharges feeds a synthetic BBL through the
             * phase system, which can BLOCK, so the charges must never run while
             * g_migrateMutex is held; running them here (before detach) is also
             * semantically required -- the rank must be on its host core for the
             * cost to land on the host inside the ROI window. Mirrors
             * cosimRoiBeginOffload's PART A/B. */
            if (thread_initialized[tid]) {
                chargeCoherenceFlush(tid);   // PART A (1.7.2): Case-1 coherence flush
                chargeLaunchCost(tid);       // PART B (1.7.3): kernel launch cost
                drainHostCharges(tid);       // retire flush+launch onto the host timeline
            }
            {
                std::lock_guard<std::mutex> mig(g_migrateMutex);
                detachThreadForMigration(tid, DOMAIN_DEVICE);
                if (!g_in_device_region.exchange(true)) {
                    g_deviceWorkers.store(1);
                } else {
                    g_deviceWorkers.fetch_add(1);
                }
            }
            ensureThreadInit(tid);  // may block on the scheduler: OUTSIDE the lock
            info("Thread %d: ROI window migrate-in at entry barrier (co-sim, flush+launch charged) [1.8.5]", tid);
        }
        /* 1.8.4 co-sim ROI window (defect #14): the post-kernel join. Once
         * the window is closing, each rank leaves the device at ITS OWN
         * barrier arrival -- compute tail fully on-device, MPI protocol
         * work back on the host. The LAST rank out freezes stats exactly
         * where the opener's roi_end used to. (bfs has no closing barrier;
         * it drains via the guest-exit dump path -- documented fallback.) */
        if (unlikely(g_mpi_thread_mode && g_cosim_mode && !g_cosim_no_offload &&
                     g_roiClosing.load() && tid < MAX_THREADS &&
                     thread_domain[tid].load() == DOMAIN_DEVICE &&
                     !g_tailMarked[tid])) {
            /* 1.8.7 guard: skip a rank already tail-marked. The opener (rank 0)
             * marks at its roi_end and -- unlike 1.8.6, where it migrated to the
             * host -- now STAYS DOMAIN_DEVICE, so without this guard it would
             * re-enter here at the closing barrier and double-decrement
             * g_deviceWorkers (and overwrite its roi_end marker). Ranks 1..N-1
             * are unmarked until THIS branch marks them, so they are unaffected. */
            /* 1.8.6 co-sim MPI exit-protocol heap-corruption fix.
             *
             * ROOT CAUSE: the 1.8.4 closing-barrier migrate-out ran the
             * scheduler churn sched->leave()/finish()+re-init (which calls
             * cores[cid]->leave() + deschedule() and re-attaches the rank to a
             * HOST InOrder core) from EVERY drained rank's own vcpu thread, in
             * a barrier-release STAMPEDE. The MPI serial weave runs with
             * sim.parallelism==1, so normally exactly one vcpu thread ever
             * touches simulator state; but a migrate-out FREES the rank's core
             * slot mid-handler (finish) and a peer immediately starts running,
             * so a rank's in-flight leave/re-init overlaps the peer's
             * contention-sim phase pass -- both iterate InOrder-core /
             * event-queue (feMap, glibc-heap) state that is single-threaded by
             * construction. The unsynchronized overlap corrupts the process
             * heap: the endgame "malloc(): unaligned tcache chunk" /
             * "QEMU internal SIGSEGV", the Garnet "No output port for vnet:<garbage>"
             * panic, and (rarely) the contention_sim "event too far into the
             * future" assert -- all firing right after the last rank drains.
             * The symmetric migrate-IN (host->DEVICE ALU cores, no contention
             * sim) never corrupts; only the device->HOST-InOrder leg does.
             * Confirmed by bisection: skipping ONLY this leg makes 12/12 gemv +
             * 6/6 histogram clean; serializing the churn against the phase sim
             * left gemv failing (the re-init Join + guest re-entry stay
             * concurrent), and holding a lock across the Join re-deadlocks the
             * parallelism==1 phase system (v184 round 5).
             *
             * FIX: do NOT migrate the drained rank back to a host core. The
             * per-rank ROI window still opens (migrate-IN offloads every rank,
             * defect #14) and its DEVICE compute is measured exactly; the rank
             * simply finishes the post-ROI MPI protocol (Reduce/Finalize/exit)
             * ON ITS DEVICE PE. That protocol is a tiny, untimed tail (a few
             * hundred cycles of an 8-256 B reduce against a ~10^6-cycle
             * kernel), so pricing it on the ALU core instead of the host core
             * is a <1% accounting shift -- vastly preferable to the heap
             * corruption, and it removes the racy leg entirely rather than
             * masking it. */
            /* 1.8.7: NO migration (1.8.6 behavior preserved -- zero race). Just
             * record the tail marker in this rank's OWN thread context; the tail
             * it now runs on the device PE is attributed device->host at dump. */
            recordTailMarker(tid);
            info("Thread %d: ROI window close (co-sim MPI rank, stays on device PE, tail marked) [1.8.7]", tid);
            if (g_deviceWorkers.fetch_sub(1) - 1 == 0) {
                /* Last rank out: window close is a bookkeeping event only --
                 * no migration, no dump (heap abort, round 3), no termination
                 * (kill-mid-protocol, rounds 4-6), no in_roi freeze (round 7).
                 * The guest runs to natural exit; the exit path dumps. */
                info("Thread %d: ROI window drained (last rank out)", tid);
            }
        }
        /* Per-rank ROI baseline (collective-first benchmarks). Some MPI
         * kernels (e.g. bfs) reach their communication ONLY through
         * collectives/barriers -- their frontier exchange is Allgather/
         * Allgatherv (no MPI_Send/Recv), so the SEND/RECV baseline further
         * down never fires and ranks 1..N-1 keep the ~40M MPI_Init wall-clock
         * floor. Synthesize the baseline here too, on the FIRST barrier the
         * plugin observes. Both bfs and stencil place a sync MPI_Barrier at
         * kernel entry, immediately before rank 0's zsim_roi_begin(), so this
         * lands at ~kernel entry (post-init: MPI_Init emits no barrier magic
         * op, only MPI_REGISTER). rank 0 hits that same barrier first but then
         * re-baselines at its real roi_begin (markRoiBegin is idempotent -- it
         * just re-snapshots the current instant), so rank 0 stays exact; a
         * rank that later also does SEND/RECV skips the block below (flag
         * already set), preserving the 1.3.3 stencil path. */
        if (zinfo && !mpi_roi_baselined) {
            mpi_roi_baselined = true;
            for (uint32_t c = 0; c < zinfo->numCores; c++)
                if (zinfo->cores[c]) zinfo->cores[c]->markRoiBegin();
            zinfo->pgres.markRoi(zinfo->numPhases, zinfo->globPhaseCycles);   // 1.11.18: PG residency window
            snapshotRoiBaseCyc();
            roiRebaseTrafficCounters("first MPI barrier (synthesized baseline)");   // 1.11.90
            if (getenv("PIMID_DEBUG_RDV"))
                info("Thread %d: synthesized per-rank ROI baseline at first "
                     "MPI BARRIER (cyc=%lu)", tid,
                     (unsigned long)roiRelCycles(tid));
        }
        /* Barrier: all ranks synchronize via tree reduction.
         * With Garnet: inject real traffic for log2(N) rounds of exchanges.
         * Without: analytical estimate proportional to rank count. */
        if (zinfo) {
            __sync_fetch_and_add(&zinfo->mpiStats.barrierCount, 1);
            uint32_t barrierLat = 0;
            GarnetNetwork* gn = zinfo->garnetNetwork;

            if (gn && gn->isCycleAccurate()) {
                /* One shared cycle-accurate Garnet network for all PEs/ranks. */
                GarnetNetwork* anet = gn;
                /* Tree-reduction barrier: log2(N) rounds.
                 * In each round, half the nodes send to the other half.
                 * Model this PE's perspective: one send per round. */
                uint32_t N = gn->getNumNodes();
                uint32_t myNode = tid % N;
                uint32_t rounds = 0;
                for (uint32_t s = 1; s < N; s <<= 1) rounds++;
                uint64_t issueTime = gem5::curTickRef();
                for (uint32_t r = 0; r < rounds; r++) {
                    uint32_t partner = myNode ^ (1u << r);
                    if (partner < N) {
                        uint32_t oneWay = anet->accessNetwork(myNode, partner,
                                                             issueTime + barrierLat);
                        barrierLat += 2 * oneWay;  /* RTT per round */
                    }
                }
            } else {
                barrierLat = zinfo->numCores * 2;  /* ~2 cycles per rank */
                if (zinfo->hierarchy.enabled) {
                    barrierLat += zinfo->hierarchy.levelLatency[4];
                }
            }

            if (barrierLat > 0) {
                /* 1.11.47 (FIX-PRE-FLEET L183): bytes = 0. 1.11.7 removed the
                 * phantom instruction-fetch traffic from the flush/launch/WORK
                 * synthetic BBLs and MISSED this one -- the MPI barrier, the
                 * dominant synthetic in thread-MPI co-sim, still carried
                 * bytes = barrierLat*4 and manufactured ifetch traffic
                 * proportional to barrier latency. A wait fetches nothing. */
                BblInfo* bbl = createSimpleBblInfo(barrierLat, 0, true);  // 1.11.16: injected charge
                fPtrs[tid].bblPtr(tid, 0, bbl);
            }

            __sync_fetch_and_add(&zinfo->mpiStats.messages, 1);
            __sync_fetch_and_add(&zinfo->mpiStats.totalLatency, barrierLat);
        }
        return;
    }

    /* MPI_SEND or MPI_RECV -- read params from registered address */
    if (!zinfo || !zinfo->mpiParamsAddr[tid]) return;

    struct MpiParamsBlock {
        uint32_t src_pe, dst_pe;
        uint64_t msg_size, msg_id;
        uint64_t sim_now;          /* plugin -> guest: current sim-time (SEND) */
        uint64_t sim_send_time;    /* guest -> plugin: message send-time (RECV) */
        uint64_t sim_contend_wait; /* guest -> plugin: cross-rank contention wait */
        uint64_t noc_lat;          /* plugin -> guest: real per-message NoC cost
                                    * (Garnet-computed RTT) = the cross-rank
                                    * occupancy reservation duration (SEND) */
        uint32_t noc_detailed;     /* plugin -> guest: 1 if the NoC model is the
                                    * cycle-accurate (detailed) Garnet, 0 for the
                                    * analytical/simple model -> contention is
                                    * applied ONLY when this is 1 (SEND) */
        /* 1.9.27: clock domain of sim_now, in MHz (former padding word, so the
         * layout is unchanged and the guest struct still matches). Ranks
         * exchange timestamps as CYCLE counts, which are comparable only if
         * both clocks tick at the same rate; carrying the rate lets the
         * consumer convert. 0 = unknown -> assume same domain (prior
         * behaviour). */
        uint32_t sim_now_freq_mhz;
    };
    MpiParamsBlock* gp = (MpiParamsBlock*)zinfo->mpiParamsAddr[tid];
    MpiParamsBlock params;
    /* In QEMU user-mode, guest and host share address space for data */
    memcpy(&params, gp, sizeof(params));

    /* MPI_CONTEND (Phase 2): the sender computed a cross-rank queueing wait
     * against the shared occupancy table; charge it as idle CYCLES on this
     * core (the shared outbound link to the destination was busy). Cycles only
     * via addDelay -- instruction counts stay pure compute. */
    if (op == ZSIM_MAGIC_OP_MPI_CONTEND) {
        uint64_t wait = params.sim_contend_wait;
        Core* cc = cores[tid];
        if (cc && wait > 0) {
            cc->addDelay((uint32_t)std::min<uint64_t>(wait, 0xFFFFFFFFull));
        }
        if (zinfo && wait > 0) {
            __sync_fetch_and_add(&zinfo->mpiStats.totalLatency, wait);
        }
        if (getenv("PIMID_DEBUG_CONTEND")) {
            info("Thread %d: MPI_CONTEND src=%u dst=%u size=%lu wait=%lu",
                 tid, params.src_pe, params.dst_pe,
                 (unsigned long)params.msg_size, (unsigned long)wait);
        }
        return;
    }

    /* NOTE (1.8.7 discovered latent issue -- NOT fixed here): MPI_TIME (2058)
     * and MPI_ADVANCE (2059) fall OUTSIDE isMpiMagicOp()'s [MPI_REGISTER 2048,
     * MPI_CONTEND 2057] window, so these two handlers are DEAD -- never
     * dispatched. The MPI_Barrier max-arrival rendezvous ADVANCE therefore never
     * applies; the closing barrier's exit-time equalization is inert. This is a
     * pre-existing dispatch-bound bug, orthogonal to the tid/cid aliasing fixed
     * in this release. Enabling dead code is its own risk (it would suddenly arm
     * an untested advance across every barrier); deferred to the 1.9.x train to
     * assess deliberately rather than flipped on inside a targeted fix. */
    if (op == ZSIM_MAGIC_OP_MPI_TIME) {
        gp->sim_now = roiRelCycles(tid);
        gp->sim_now_freq_mhz = coreFreqMHzFor(tid);
        return;
    }
    if (op == ZSIM_MAGIC_OP_MPI_ADVANCE) {
        uint64_t target = cyclesFromDomain(params.sim_send_time,
                                           params.sim_now_freq_mhz,
                                           coreFreqMHzFor(tid));
        uint64_t cur = roiRelCycles(tid);
        Core* cc = cores[tid];
        if (cc && target > cur) {
            uint64_t delta = target - cur;
            cc->addDelay((uint32_t)std::min<uint64_t>(delta, 0xFFFFFFFFull));
        }
        return;
    }

    const bool isRecv = (op == ZSIM_MAGIC_OP_MPI_RECV);

    /* Per-rank ROI baseline (THE MPI cycle-accounting fix). The benchmarks call
     * zsim_roi_begin() only on rank 0, so ranks 1..N-1 never snapshot an ROI
     * baseline: their reported cycles/instrs are whole-program, dominated by the
     * fixed serial init + the wall-clock-pumped globPhaseCycles floor that forms
     * during MPI_Init -- so every rank>0 reads a fixed ~40M regardless of --size
     * or kernel, while only rank 0 scaled. On the FIRST real communication op
     * (SEND/RECV ~= kernel entry, mirroring rank 0's roi_begin at the compute
     * loop) synthesize the baseline for any rank that never saw roi_begin, so
     * ALL ranks report kernel-relative, workload-scaling cycles/instrs. Each MPI
     * rank is its own process, so mpi_roi_baselined is per-rank; rank 0 sets it
     * in the roi_begin handler and is therefore left untouched here. */
    if (!mpi_roi_baselined) {
        mpi_roi_baselined = true;
        for (uint32_t c = 0; c < zinfo->numCores; c++)
            if (zinfo->cores[c]) zinfo->cores[c]->markRoiBegin();
        zinfo->pgres.markRoi(zinfo->numPhases, zinfo->globPhaseCycles);   // 1.11.18: PG residency window
        snapshotRoiBaseCyc();
        if (getenv("PIMID_DEBUG_RDV"))
            info("Thread %d: synthesized per-rank ROI baseline at first MPI %s "
                 "(cyc=%lu)", tid, isRecv ? "RECV" : "SEND",
                 (unsigned long)(cores[tid] ? cores[tid]->getCycles() : 0));
    }

    /* SEND: publish the current simulated time so the sender can stamp the
     * outgoing message with its send-time (read back by libpimid_mpi), plus
     * tell the guest whether the NoC model is the cycle-accurate (detailed)
     * Garnet -- cross-rank contention is applied ONLY in detailed mode
     * (analytical/simple is the single intentional approximation, and a native
     * run with no plugin never reaches this and leaves the flag at its default
     * 0). The real per-message NoC cost (noc_lat) is filled in below once
     * computed and used by the guest as the occupancy reservation duration.
     *
     * The send-time stamp is the sender core's ROI-baseline-RELATIVE work clock
     * (roiRelCycles = getCycles() - this rank's baseline), NOT globPhaseCycles
     * and NOT the raw getCycles(). Raw getCycles() carries the per-rank wall-
     * clock floor; subtracting the baseline makes the stamp a function of real
     * post-baseline work only, so the receiver's rendezvous (arrival = send_time
     * + latency) resolves deterministically and floor-free across ranks. */
    if (!isRecv) {
        gp->sim_now = roiRelCycles(tid);
        gp->sim_now_freq_mhz = coreFreqMHzFor(tid);
        gp->noc_detailed =
            (zinfo->garnetNetwork && zinfo->garnetNetwork->isCycleAccurate())
                ? 1u : 0u;
        gp->noc_lat = 0;
    }

    if (params.src_pe == params.dst_pe) return;

    /* 1. Hierarchy latency (PE-to-PE LCA routing with M:N mapping) */
    uint32_t hierLat = 0;
    if (zinfo->hierarchy.enabled) {
        hierLat = (uint32_t)computePEtoPELatency(
            params.src_pe, params.dst_pe,
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

    /* 2. NoC latency (if Garnet is available) */
    uint32_t nocLat = 0;
    if (zinfo->garnetNetwork) {
        GarnetNetwork* gn = zinfo->garnetNetwork;
        uint32_t srcNode = params.src_pe % gn->getNumNodes();
        uint32_t dstNode = params.dst_pe % gn->getNumNodes();

        if (gn->isCycleAccurate() && g_mpi_thread_mode) {
            /* 1.6 thread mode: DETERMINISTIC message path. Live accessNetwork
             * injection is order-sensitive (persistent garnetTick_/in-flight
             * state means each RTT depends on the wall interleaving of the 16
             * rank threads). Instead the message's cache-line packets go into
             * the SAME phase-sorted batch replay every memory access uses (so
             * they physically contend, deterministically), and the charged
             * latency comes from the batch-smoothed RTT -- a pure function of
             * the deterministic batch history. Same modeling contract as OMP. */
            uint32_t pl = zinfo->phaseLength > 0 ? zinfo->phaseLength : 10000;
            uint64_t coreCyc = cores[tid] ? cores[tid]->getCycles() : 0;
            uint64_t stamp = (uint64_t)zinfo->numPhases * pl + (coreCyc % pl);
            uint32_t numPackets = std::max(1u, (uint32_t)((params.msg_size + 63) / 64));
            for (uint32_t p = 0; p < numPackets; p++)
                gn->recordBatchAccess(srcNode, dstNode, stamp + p);
            /* Deterministic pricing: static analytic RTT only (the EWMA
             * read is wall-order-dependent; see pe_memory_interface). */
            uint32_t rtt = gn->getRTT(
                std::to_string(params.src_pe).c_str(),
                std::to_string(params.dst_pe).c_str());
            nocLat = rtt + (uint32_t)((params.msg_size / 64) * 2);
        } else if (gn->isCycleAccurate()) {
            /* One shared cycle-accurate Garnet network for all PEs/ranks. */
            GarnetNetwork* anet = gn;
            /* Direct Garnet injection -- real traffic with real contention.
             * Each packet represents a cache-line (64B) transfer.
             * Large messages inject multiple packets sequentially. */
            uint64_t issueTime = gem5::curTickRef();
            uint32_t numPackets = std::max(1u, (uint32_t)((params.msg_size + 63) / 64));
            uint32_t totalOneWay = 0;
            for (uint32_t p = 0; p < numPackets; p++) {
                uint32_t oneWay = anet->accessNetwork(srcNode, dstNode,
                                                     issueTime + totalOneWay);
                totalOneWay += oneWay;
            }
            nocLat = 2 * totalOneWay;  /* RTT */
        } else {
            /* Simple mode: analytical RTT with message size scaling */
            nocLat = gn->getRTT(
                std::to_string(params.src_pe).c_str(),
                std::to_string(params.dst_pe).c_str());
            if (params.msg_size > 64) {
                nocLat += (uint32_t)((params.msg_size / 64) * 2);
            }
        }
    }

    /* SEND: publish the real per-message NoC cost so the guest can use it as the
     * cross-rank occupancy reservation duration (no magic-number flit constant).
     * In detailed mode this is the Garnet-computed RTT; in analytical mode the
     * guest ignores it (noc_detailed==0 -> contention skipped). */
    if (!isRecv) {
        gp->noc_lat = nocLat;

        /* Shared detailed-MPI Garnet: the message's cache-line packets also go
         * into the cross-rank shared log (sender-side only -- one publication
         * per message), so rank-to-rank MPI traffic physically contends with
         * ALL ranks' memory traffic in every rank's merged Garnet replay. */
        static PimidNocHdr* nocShm = nullptr;
        static int nocRank = -1;
        static bool nocTried = false;
        if (!nocTried) {
            nocTried = true;
            const char* nm = getenv("PIMID_NOC_SHM");
            const char* rk = getenv("PIMID_MPI_RANK");
            if (nm && rk) { nocShm = pimid_noc_shm_attach(nm); nocRank = atoi(rk); }
        }
        if (nocShm && nocRank >= 0 && zinfo->garnetNetwork &&
            zinfo->garnetNetwork->isCycleAccurate()) {
            uint32_t nn = zinfo->garnetNetwork->getNumNodes();
            /* src/dst_pe are RANK ids; rank r's memory traffic sources from
             * node r*nodesPerRank (its own PE-group's MI). Scale so message
             * packets share the same source/dest leaves as the rank's memory
             * stream (exact when ranks == PEs, the sweep norm). */
            uint32_t npr = nocShm->nodesPerRank > 0 ? nocShm->nodesPerRank : 1;
            uint32_t srcN = (params.src_pe * npr) % nn;
            uint32_t dstN = (params.dst_pe * npr) % nn;
            uint32_t nPkts = std::max(1u, (uint32_t)((params.msg_size + 63) / 64));
            if (nPkts > PIMID_NOC_MAX_MSG_RECS) nPkts = PIMID_NOC_MAX_MSG_RECS;
            /* ROI-relative clock: the only cross-rank-comparable axis (same
             * axis the PE-MI publishes memory records on). */
            uint64_t cyc = roiRelCycles(tid);
            for (uint32_t p = 0; p < nPkts; p++)
                pimid_noc_publish(nocShm, (uint32_t)nocRank, srcN, dstN, cyc + p);
        }
    }

    /* 3. Charge timing as CYCLES (not instructions) so instr counts reflect
     *    pure compute and stay deterministic.  Use addDelay(), which advances
     *    the core's clock without inflating its instruction count. */
    uint32_t totalLat = nocLat + hierLat;
    uint64_t chargedCycles = totalLat;
    Core* c = cores[tid];
    if (isRecv) {
        /* Sim-time rendezvous: advance the receiver's clock to the
         * deterministic arrival time = send_time + latency. If the receiver is
         * already past that point in sim-time, the message is already available
         * and we charge nothing. Falls back to plain latency when the message
         * carries no stamp (e.g. legacy path). curCyc is the receiver's ROI-
         * baseline-RELATIVE work clock so it compares like-for-like against the
         * sender's baseline-relative send-time stamp (both floor-free). */
        uint64_t curCyc = roiRelCycles(tid);
        /* Phase-2: the receiver's rendezvous arrival includes the cross-rank
         * contention wait stamped by the sender:
         *   arrival = send_time + contend_wait + hierLat + nocLat            */
        uint64_t sendStamp = cyclesFromDomain(params.sim_send_time,
                                              params.sim_now_freq_mhz,
                                              coreFreqMHzFor(tid));
        uint64_t arrival = (params.sim_send_time > 0)
                               ? (sendStamp + params.sim_contend_wait
                                  + (uint64_t)totalLat)
                               : (curCyc + (uint64_t)totalLat);
        uint64_t delta = (arrival > curCyc) ? (arrival - curCyc) : 0;
        /* 1.8.7 cross-axis invariant guard. A single message's rendezvous
         * advance can never exceed the whole ROI simulated so far -- arrival is
         * a floor-free roiRel stamp bounded by the ROI span, and curCyc >= 0. If
         * `delta` blows past that, the sender's send_time and the receiver's
         * curCyc are on MISMATCHED clock axes (the tid/cid baseline aliasing this
         * release fixes): the classic symptom is curCyc collapsing to 0 while the
         * PE's raw clock is well into the ROI, so `delta` degenerates to the full
         * send_time and the ROOT accumulates. Shout LOUDLY and cap rather than
         * advance a PE by a garbage delta. Same defensive-tripwire discipline as
         * the defect-13 thread-MPI cycle-rewind guard and the 1.7.7 co-sim
         * memRespCycle-skew assert. */
        uint64_t roiSpan = zinfo->globPhaseCycles;   // no one message can advance past the whole ROI
        if (delta > roiSpan && roiSpan > 0) {
            uint64_t raw = cores[tid] ? cores[tid]->getCycles() : 0;
            uint32_t bcid = (tid < MAX_THREADS && cids[tid] < zinfo->numCores) ? cids[tid] : tid;
            warn("[1.8.7 cross-axis guard] rank %u MPI_RECV advance delta=%lu exceeds ROI span "
                 "%lu (send_time=%lu cwait=%lu cur=%lu raw=%lu base=%lu) -- clock-axis mismatch; "
                 "capping to ROI span", tid, (unsigned long)delta, (unsigned long)roiSpan,
                 (unsigned long)params.sim_send_time, (unsigned long)params.sim_contend_wait,
                 (unsigned long)curCyc, (unsigned long)raw,
                 (unsigned long)mpi_roi_base_cyc[bcid]);
            delta = roiSpan;
        }
        chargedCycles = delta;
        if (c && delta > 0) c->addDelay((uint32_t)std::min<uint64_t>(delta, 0xFFFFFFFFull));
        if (getenv("PIMID_DEBUG_RDV")) {
            info("Thread %d: MPI_RECV src=%u dst=%u size=%lu send_time=%lu cwait=%lu "
                 "cur=%lu lat=%u arrival=%lu delta=%lu raw_getCyc=%lu roi_base=%lu",
                 tid, params.src_pe,
                 params.dst_pe, (unsigned long)params.msg_size,
                 (unsigned long)params.sim_send_time,
                 (unsigned long)params.sim_contend_wait,
                 (unsigned long)curCyc, totalLat, (unsigned long)arrival,
                 (unsigned long)delta,
                 (unsigned long)(cores[tid] ? cores[tid]->getCycles() : 0),
                 (unsigned long)mpi_roi_base_cyc[(tid < MAX_THREADS && cids[tid] < zinfo->numCores) ? cids[tid] : tid]);
        }
    } else {
        if (c && totalLat > 0) c->addDelay(totalLat);
        if (getenv("PIMID_DEBUG_RDV")) {
            info("Thread %d: MPI_SEND src=%u dst=%u size=%lu sim_now=%lu lat=%u",
                 tid, params.src_pe, params.dst_pe, (unsigned long)params.msg_size,
                 (unsigned long)roiRelCycles(tid), totalLat);
        }
    }

    /* 4. Stats */
    __sync_fetch_and_add(&zinfo->mpiStats.messages, 1);
    __sync_fetch_and_add(&zinfo->mpiStats.totalLatency, chargedCycles);
}

/**
 * Check if a magic op code is an MPI op.
 */
static inline bool isMpiMagicOp(uint64_t op) {
    return op >= ZSIM_MAGIC_OP_MPI_REGISTER && op <= ZSIM_MAGIC_OP_MPI_CONTEND;
}

/**
 * Compute PCIe/CXL transfer latency (SIMPLE or M/D/1 contention model).
 *
 * @param transfer_bytes Data payload size in bytes. 0 means default (64B cache line).
 *
 * SIMPLE (model==0): baseLatencyCycles + serialization(transfer_bytes).
 * MD1 (model==1): phase-based M/D/1 queuing model using bytesPerCycle for
 *   service rate. Offload count per phase drives arrival rate.
 *   Service time scales with actual transfer_bytes.
 */
static uint32_t getPCIeLatency(uint32_t transfer_bytes = 0) {
    if (!zinfo || !zinfo->pcie.enabled || zinfo->pcie.baseLatencyCycles == 0) return 0;

    uint32_t base = zinfo->pcie.baseLatencyCycles;
    double data_bytes = (transfer_bytes > 0) ? static_cast<double>(transfer_bytes) : 64.0;

    // Protocol overhead: header bytes reduce effective throughput
    double total_bytes = data_bytes + zinfo->pcie.headerBytes;

    // Serialization: ceil(total_bytes / bytesPerCycle) -- including protocol overhead
    uint32_t serCycles = 0;
    if (zinfo->pcie.bytesPerCycle > 0.0) {
        serCycles = static_cast<uint32_t>(
            std::ceil(total_bytes / zinfo->pcie.bytesPerCycle));
    }

    // Coherence overhead (average extra latency for coherent access)
    base += zinfo->pcie.coherenceExtraCycles;

    // SIMPLE model: hop count base + M/D/1 queuing contention
    // At zero load: rho~0, latMul~1.0 -> result ~ base+serCycles
    futex_lock(&zinfo->pcie.updateLock);

    zinfo->pcie.curPhaseAccesses++;

    // Phase-based smoothing (same approach as ZSim SimpleMemory M/D/1)
    uint64_t curPhase = zinfo->numPhases;
    if (curPhase != zinfo->pcie.lastPhase) {
        double alpha = 0.5;
        uint32_t phaseLen = zinfo->phaseLength;
        double curRate = (phaseLen > 0)
            ? static_cast<double>(zinfo->pcie.curPhaseAccesses) / phaseLen
            : 0.0;
        zinfo->pcie.smoothedPhaseRate =
            alpha * curRate + (1.0 - alpha) * zinfo->pcie.smoothedPhaseRate;
        zinfo->pcie.curPhaseAccesses = 0;
        zinfo->pcie.lastPhase = curPhase;
    }

    // Service time from actual transfer size
    double serviceTime = (zinfo->pcie.bytesPerCycle > 0.0)
        ? data_bytes / zinfo->pcie.bytesPerCycle
        : static_cast<double>(base);
    if (serviceTime < 1.0) serviceTime = 1.0;
    double serviceRate = 1.0 / serviceTime;

    double rho = zinfo->pcie.smoothedPhaseRate / serviceRate;
    if (rho > 0.95) rho = 0.95;
    if (rho < 0.0) rho = 0.0;

    // P-K formula for M/D/1: E[T] = D * (1 + rho / (2*(1-rho)))
    double latMul = 1.0 + 0.5 * rho / (1.0 - rho);
    uint32_t result = static_cast<uint32_t>((base + serCycles) * latMul + 0.5);

    futex_unlock(&zinfo->pcie.updateLock);
    return result;
}

/**
 * Two-layer bridge transfer latency (1.7.1). Deterministic host<->device
 * boundary charge = bridge latency (phy pipeline + protocol overhead) +
 * size/aggregate-bandwidth serialization. The phy latency is wire +
 * command-interface/MC pipeline (interposer HBM has NO SerDes). No phase M/D/1
 * so the boundary cost is reproducible run-to-run. Supersedes the flat
 * getPCIeLatency for system-scope co-sim; the flat model remains the fallback
 * for legacy configs with no sys.bridge block.
 */
static uint32_t getBridgeLatency(uint32_t transfer_bytes = 0) {
    if (!zinfo || !zinfo->bridge.enabled) return 0;
    uint32_t base = zinfo->bridge.phyLatencyCycles + zinfo->bridge.protocolOverheadCycles;
    uint32_t serCycles = 0;
    if (zinfo->bridge.bytesPerCycle > 0.0 && transfer_bytes > 0) {
        serCycles = static_cast<uint32_t>(
            std::ceil(static_cast<double>(transfer_bytes) / zinfo->bridge.bytesPerCycle));
    }
    return base + serCycles;
}

/**
 * Host<->device boundary transfer charge (cycles). Uses the two-layer bridge
 * model when a sys.bridge block is present (1.7.1), else falls back to the flat
 * getPCIeLatency (backward compat with pre-1.7.1 configs). NO_OFFLOAD baselines
 * never reach here (the g_cosim_no_offload guards return before the WORK path).
 */
static uint32_t boundaryTransferCycles(uint32_t transfer_bytes = 0) {
    if (zinfo && zinfo->bridge.enabled) return getBridgeLatency(transfer_bytes);
    return getPCIeLatency(transfer_bytes);
}

/**
 * Case-1 coherence FLUSH cycles charged on the host core at roi_begin (1.7.2).
 *
 * Unified address space (mode==0): the host writes back dirty INPUT lines and
 * invalidates OUTPUT lines before the device reads DRAM. Analytic upper bound
 * (HANDOFF ISSUE 3 F1): the whole input+output footprint is treated as dirty
 * writeback traffic (conservative-against-PIM), so
 *   cycles = flushFixedCycles + ceil(deltaBytes / writebackBytesPerCycle),
 * where deltaBytes is what THIS arrival's own walk found still in M -- the
 * data dirtied since the previous walk (1.11.62, ruling 5b: every arrival
 * walks and every walk cleans, so each arrival measures its own delta and
 * nothing is divided), and flushFixedCycles is charged PER RANK because each
 * core issues its own flush instruction and stalls on it (1.11.62, ruling 5a).
 * (1.11.59, audit F021: this line still named `footprintBytes`,
 * the configured override that 1.11.40 stopped using and init.cpp now
 * refuses outright -- a formula naming a config key as its input, five
 * releases after the input became a measurement.)
 * Separate address space (mode==1): cache bypass (Case 2) -> NO flush; the
 * 1.7.1 bridge bulk-DMA path already prices the crossing, so return 0.
 * Deterministic (no phase M/D/1) so the boundary cost reproduces exactly.
 * NO_OFFLOAD baselines never reach the co-sim roi_begin block, so they are
 * never charged.
 */
/* 1.11.40 (audit N7, user ruling: the footprint is MEASURED). Walk every cache
 * and sum the lines in M -- exactly the bytes a writeback flush must move.
 *
 * Why this is the right quantity and the constant was not: a flush writes back
 * DIRTY LINES FROM CACHE. It cannot move more than the caches hold. The 16 MiB
 * default was 56.9x the host hierarchy here (32 KB L1D + 256 KB L2), i.e. not
 * an over-estimate but an impossibility, and it consumed 99.4% of the host's
 * simulated cycles on a 4096-element kernel.
 *
 * E (exclusive) lines are deliberately NOT counted: exclusive-but-clean needs
 * no writeback. That distinction is the reason to read MESI state rather than
 * estimate from occupancy or capacity.
 *
 * Registry scope: alu_core PEs carry no cache, so in a co-sim run every
 * registered cache is a host cache. The count is reported so that assumption is
 * visible; a cache-bearing PE type would require making this role-aware. */

/* ------------------------------------------------------------------------
 * 1.11.62 (ruling 5b): THE COHERENCE FLUSH -- ONE WALK AT EVERY RANK'S
 * ARRIVAL. This replaces the 1.11.57 "one epoch, one walk, N equal slices"
 * design. State the design before the code, because what is GONE (the
 * division, the remainder, the epoch open/close bookkeeping) is exactly what
 * made the previous two revisions wrong in ways that did not cancel.
 *
 * The history in two sentences. 1.11.55 measured at every rank's own arrival
 * and therefore counted one shared working set N times; 1.11.57 measured ONCE
 * at the first arrival, divided the footprint by the rank count and handed
 * every rank an equal slice. Audit round 4 then filed what the second design
 * had bought: the single walk happens at the FIRST arrival, so every line that
 * ranks 2..N dirty in their own pre-kernel prologue is invisible to it and --
 * in a corpus where every workload has exactly ONE offload -- is never charged
 * at all (D002); and a rank that missed the previous offload took a STALE
 * slice, measured before the current working set existed and already cleaned
 * out of the caches (D003).
 *
 * THE DESIGN, in four decisions.
 *
 * 1. EVERY ARRIVAL WALKS. When any participating rank reaches the flush point
 *    it performs its OWN measure-and-clean over the whole cache registry:
 *    measureAndCleanFlushDelta() visits every registered cache, takes each
 *    line that is in M to E under that cache's own bottom-cc lock, and unions
 *    the ADDRESSES of those lines into one set. De-duplicating by address
 *    rather than by cache level keeps the number right whatever the hierarchy
 *    does -- inclusive, non-inclusive, silently upgraded, three levels or one
 *    -- and the L1 write filter's wrAddr is dropped in the same pass
 *    (1.11.60, D004) so that a filtered store after the clean really does
 *    re-dirty the line instead of going missing.
 *
 * 2. THE WALK IS THE DEDUP. Because every walk CLEANS, the walk of arrival k
 *    finds exactly what has been dirtied since walk k-1: its DELTA. Arrival k
 *    charges THAT delta's bytes -- no division, no slice, no remainder. The
 *    sum over arrivals is every line that was dirty at some arrival, charged
 *    exactly once, so D4's invariant ("the working set is priced once,
 *    independent of rank count") now holds BY CONSTRUCTION -- cleaning IS the
 *    de-duplication -- rather than by an arithmetic identity that only held if
 *    all N ranks actually arrived. Data dirtied by a late rank AFTER an
 *    earlier walk is charged by the NEXT walker instead of vanishing with an
 *    epoch that never re-opens (D002 fixed). D003 goes with the slices: a rank
 *    that missed the previous offload just walks and charges its own delta,
 *    and no stale measurement survives anywhere to be re-served.
 *    Precisely: what is charged once is each line's DIRTY EPISODE, not each
 *    line address. A line still in M from before walk k is charged by walk k
 *    and by nobody else; a line the guest WRITES AGAIN between walk k and
 *    walk k+1 is charged twice, and it must be -- it is a second modification
 *    and a real second writeback. What can no longer happen is the same
 *    unmodified dirty line being billed to several ranks, which is exactly
 *    what 1.11.55 did and what the 1.11.57 division existed to undo.
 *
 * 3. THE FIXED COST IS PER RANK, AND THAT IS PHYSICAL (ruling 5a; round-4 D001
 *    is thereby ruled INTENDED, not a defect). coherence.flushFixedCycles --
 *    emitted from coherence_flush_fixed_ns -- models ONE CORE issuing its own
 *    flush instruction and stalling on it. Every rank runs on its own host
 *    core and executes that instruction itself, so every rank pays that
 *    latency, exactly as N cores each executing a wbinvd each stall for one.
 *    What must not be replicated is the DATA: N cores flushing one shared
 *    coherent hierarchy write each dirty line back to memory once, and
 *    decision 2 is what guarantees it -- the second core's flush finds the
 *    first core's lines already clean and moves only what was re-dirtied since.
 *    So the fixed term is O(N) because N instructions really execute, while
 *    the byte term is O(working set) because the working set is written back
 *    once. Both halves now model the same event; 1.11.57's asymmetry (a fixed
 *    cost for N flushes beside bytes for one) is resolved in favour of the
 *    physics rather than by dividing the instruction latency N ways.
 *
 * 4. WHAT LOCKING THE WALK NEEDS. Two locks, at different scopes. The
 *    plugin-level mutex g_flushWalkMutex serialises WHO IS WALKING (it used to
 *    serialise who opens an epoch): walks by different ranks are strictly
 *    ordered, so two of them can never interleave and split one line's dirty
 *    bit across two deltas or count it into both. INSIDE a walk each cache is
 *    entered under its OWN bottom-cc lock -- the lock MESICC::startAccess
 *    takes -- one cache at a time, never two at once, with the tag read inside
 *    that lock (Cache::flushDirtyLines). An executing rank blocked on that
 *    cache simply waits, and no lock cycle can form with the hand-over-hand
 *    child -> tcc -> bcc order of the access path, because the walk never
 *    holds two cc locks. The blocking charge, fPtrs[tid].bblPtr(), stays
 *    OUTSIDE every lock (the 1.8.5 constraint: it feeds the phase system and
 *    can block there), so the walk mutex is never held across a blocking call.
 *
 * OMP AND SINGLE-RANK RUNS ARE UNCHANGED. With one participant exactly one
 * caller reaches the flush point per offload, so there is exactly one walk and
 * one fixed cost. That walk covers the same registry and charges the same
 * bytes the 1.11.57 opener charged when it divided a footprint by one and
 * added a zero remainder, and the cycle term is the same fixed + ceil(bytes /
 * writebackBytesPerCycle). Identical by inspection, term for term.
 *
 * STATISTICS. xingFlushBytes keeps its accumulation semantics: it sums the
 * deltas, so its total is what the old design called the epoch footprint F,
 * PLUS the late-dirty data F structurally could not see, PLUS anything
 * re-dirtied between two walks -- three quantities the old total was missing
 * for one reason each, and every one of them is real writeback traffic.
 * Expect the co-sim flush bytes to go UP against 1.11.57, never down; a run
 * whose ranks all arrive together and dirty nothing in between reproduces the
 * old number exactly. Every walk logs its
 * rank, its delta and the running cumulative; PIMID_FLUSH_TRACE puts the same
 * triple on stderr in a fixed machine-readable form so a gate can assert that
 * several walks happen with several ranks, that the second walk's delta
 * excludes the first walk's lines, and that a single-rank run walks once.
 * ------------------------------------------------------------------------ */

/* 1.11.62 (ruling 5b): flush-walk state. g_flushWalkMutex guards all of it AND
 * serialises the walks themselves. What is deliberately absent, versus
 * 1.11.57: the participant count divisor, the per-rank slice, the integer
 * remainder, the epoch id, the per-tid "already took its share" array and the
 * remaining-charges countdown. All six existed only to SHARE one measurement
 * between ranks that were not allowed to make their own; every arrival now
 * makes its own, so there is nothing to share and no epoch to open or close.
 * Nothing outside this file ever read that state (checked: g_flushEpoch* had
 * no reader anywhere in the tree, and zinfo->coherence.footprintMeasured is a
 * diagnostic field that no stat exports and no consumer parses), so no
 * per-offload epoch counter is retained. */
static std::mutex g_flushWalkMutex;
static uint64_t   g_flushWalkBytesTotal = 0;   // cumulative sum of every delta
static uint64_t   g_flushWalkCount      = 0;   // walks performed, for the log

/* Diagnostic only (1.11.62): how many callers are EXPECTED to reach the flush
 * point, so the one-time log line can say how many walks a reader should see.
 * It divides nothing -- that is the whole point of this revision. Only
 * thread-MPI shares a cache registry across ranks; a process-mode rank has its
 * own zsim instance and its own caches, so it is always its own single
 * participant. */
static inline uint32_t flushWalkersExpected() {
    if (!g_mpi_thread_mode) return 1u;
    return (g_mpi_rank_count > 0) ? g_mpi_rank_count : 1u;
}

/* ONE walk: measure and clean in the same locked pass. Caller holds
 * g_flushWalkMutex. Returns the DELTA in bytes -- the distinct line addresses
 * found in M, i.e. everything dirtied since the previous walk, because that
 * walk left the registry clean. *cleanedLines gets the per-level line count,
 * which is legitimately larger (one address held dirty at three levels is one
 * line of charge and three lines of cleaning) and is reported as the different
 * quantity it is (1.11.57, D004). */
static uint64_t measureAndCleanFlushDelta(uint64_t* cleanedLines) {
    *cleanedLines = 0;
    if (!zinfo) return 0;
    std::unordered_set<Address> distinct;
    const g_vector<Cache*>& all = zsimAllCaches();
    for (Cache* c : all) {
        if (!c) continue;
        *cleanedLines += c->flushDirtyLines(distinct);
    }
    static bool said = false;
    if (!said) {
        said = true;
        info("[flush] every arrival at the flush point walks all %d registered "
             "cache(s) itself: distinct dirty line ADDRESSES measured and "
             "cleaned M -> E in one locked pass, so each walk returns the "
             "DELTA dirtied since the previous walk and the deltas sum to the "
             "working set exactly once (1.11.62). No inclusion assumption: a "
             "line held dirty at several levels is one line, and a line dirty "
             "only in an L1D (zsim upgrades E -> M silently in the child) is "
             "still seen. %u walker(s) expected per offload.",
             (int)all.size(), flushWalkersExpected());
    }
    return (uint64_t)distinct.size() * (uint64_t)zinfo->lineSize;
}

/* Perform THIS arrival's walk and hand back its delta. Returns false if the
 * flush is disabled for this run. The mutex is released before the caller
 * touches fPtrs[].bblPtr() -- see decision 4 above. */
static bool takeFlushWalkDelta(uint32_t tid, uint64_t* deltaBytes,
                               uint64_t* cumulativeBytes,
                               uint64_t* cleanedLines, uint64_t* walkIndex) {
    if (!zinfo || !zinfo->coherence.enabled) return false;
    if (zinfo->coherence.mode != 0) return false;  // separate = cache bypass, no flush
    /* 1.11.62 (ruling 5b): the gate hook. Read once; the fast path is the
     * common case where it is off and this is a predictable load. */
    static const bool flushTrace = (getenv("PIMID_FLUSH_TRACE") != nullptr);
    std::lock_guard<std::mutex> lk(g_flushWalkMutex);
    uint64_t cleaned = 0;
    const uint64_t delta = measureAndCleanFlushDelta(&cleaned);
    g_flushWalkBytesTotal += delta;
    g_flushWalkCount++;
    /* 1.11.57 (audit D015), restated for 1.11.62: footprintMeasured is a
     * DIAGNOSTIC field that no stat exports and no consumer reads. It now
     * holds the CUMULATIVE measured dirty bytes -- the sum of every walk's
     * delta, which is the whole working set the flush has written back so far
     * and equals zinfo->xing.flushBytes. Anyone exporting it as a stat must
     * export it beside xing.flushBytes under that name. */
    zinfo->coherence.footprintMeasured = g_flushWalkBytesTotal;
    *deltaBytes      = delta;
    *cumulativeBytes = g_flushWalkBytesTotal;
    *cleanedLines    = cleaned;
    *walkIndex       = g_flushWalkCount;
    if (flushTrace) {
        /* Fixed, machine-readable, one line per walk, emitted under the same
         * mutex as the walk so the ordering on stderr is the walk ordering.
         * `rank` is the zsim tid of the walking thread: one thread per rank
         * reaches this point, and the plugin carries no other rank id. */
        fprintf(stderr, "[flush-walk] rank=%u delta=%llu cumulative=%llu\n",
                (unsigned)tid, (unsigned long long)delta,
                (unsigned long long)g_flushWalkBytesTotal);
        fflush(stderr);
    }
    return true;
}

/**
 * Apply the Case-1 coherence flush charge on the host core running `tid` at
 * roi_begin, BEFORE the device migration (thread still on its host core, so the
 * cost lands on the host and is inside the ROI/task window). Uses the synthetic
 * BblInfo path (like the bridge WORK charge) because the ooo host core does not
 * override addDelay. Accumulates the flush stats.
 */
static void chargeCoherenceFlush(uint32_t tid) {
    uint64_t deltaBytes = 0, cumulativeBytes = 0, cleanedLines = 0, walkIdx = 0;
    if (!takeFlushWalkDelta(tid, &deltaBytes, &cumulativeBytes, &cleanedLines,
                            &walkIdx))
        return;

    /* 1.11.62 (ruling 5a): the FIXED term is charged per rank because each
     * core issues its own flush instruction and stalls on it -- see decision 3
     * of the design block. 1.11.62 (ruling 5b): the DATA term is this
     * arrival's own delta, which is what this rank's flush actually has to
     * move, because the previous walker already wrote back what it found. */
    uint64_t flushCyc = zinfo->coherence.flushFixedCycles;
    if (zinfo->coherence.writebackBytesPerCycle > 0.0 && deltaBytes > 0) {
        flushCyc += (uint64_t)std::ceil(
            (double)deltaBytes / zinfo->coherence.writebackBytesPerCycle);
    }

    /* 1.11.62 (ruling 5b): the bytes are booked whether or not there are
     * cycles to inject. The walk has already CLEANED those lines, so if the
     * accounting were skipped here they could never be counted by any later
     * walk -- the D002 loss, re-created at the exit. Only the timeline
     * injection is conditional (a zero-cycle synthetic BBL is not an event).
     * At the shipped defaults flushFixedCycles is 400, so this branch is not
     * taken in any emitted configuration. */
    if (flushCyc > 0) {
        uint32_t fc = (uint32_t)std::min<uint64_t>(flushCyc, 0xFFFFFFFFull);
        BblInfo* bbl = createSimpleBblInfo(fc, 0, true);  // 1.11.7: no phantom fetch; 1.11.16: injected charge
        fPtrs[tid].bblPtr(tid, 0, bbl);                   // OUTSIDE every lock (1.8.5)
    }
    __sync_fetch_and_add(&zinfo->xing.flushBytes, deltaBytes);
    __sync_fetch_and_add(&zinfo->xing.count, 1);
    __sync_fetch_and_add(&zinfo->coherence.flushCount, 1);
    __sync_fetch_and_add(&zinfo->coherence.flushCyclesCharged, flushCyc);
    /* 1.11.62 (ruling 5b): the log states the three quantities the design
     * rests on -- the rank that walked, the DELTA it charged, and the running
     * cumulative, whose final value is the whole working set priced once.
     * (1.11.57, D004: the per-level cleaned count is a different quantity from
     * the charged bytes and stays under its own name.) */
    info("Thread %d: Case-1 coherence flush, walk #%llu (rank %u delta %llu B "
         "-> %llu cycles, of which %llu fixed; cumulative %llu B over %llu "
         "walk(s), each line charged once because every walk cleans; %llu "
         "cache line(s) taken M -> E across %zu caches at all levels)",
         tid, (unsigned long long)walkIdx, (unsigned)tid,
         (unsigned long long)deltaBytes, (unsigned long long)flushCyc,
         (unsigned long long)zinfo->coherence.flushFixedCycles,
         (unsigned long long)cumulativeBytes, (unsigned long long)walkIdx,
         (unsigned long long)cleanedLines, zsimAllCaches().size());
}

/**
 * Kernel LAUNCH cost (cycles) charged on the host core at the offload doorbell
 * (1.7.3, HANDOFF ISSUE 2). Composed of documented components:
 *   - doorbellCycles: user-mode doorbell write + cmd-packet formation (no syscall)
 *   - dispatchCycles: the HIP/CUDA-launch-analog runtime/OS software cost
 *   - a small cmd packet crossing the bridge (host->device) via getBridgeLatency
 *   - a small ack packet crossing the bridge back (device->host)
 * Anchor: real GPU kernel-launch latency is ~5-20us; the decomposed defaults
 * (~300ns doorbell + ~5us dispatch + ~tens of ns bridge) sit at that band's low
 * end. Deterministic: the fixed cycle terms plus the deterministic 1.7.1 bridge
 * crossings reproduce exactly run-to-run. DDR5 vs HBM3 differ ONLY in the bridge
 * component (phy pipeline + aggregate bandwidth); doorbell/dispatch are shared.
 * NO_OFFLOAD baselines never reach a co-sim doorbell, so they are never charged.
 */
static uint64_t launchCostCycles() {
    if (!zinfo || !zinfo->launch.enabled) return 0;
    uint64_t cyc = (uint64_t)zinfo->launch.doorbellCycles + zinfo->launch.dispatchCycles;
    // cmd packet crosses the bridge to the device; ack packet returns. Reuse the
    // 1.7.1 two-layer bridge crossing so per-tech phy/bandwidth is priced.
    cyc += getBridgeLatency(zinfo->launch.cmdBytes);
    cyc += getBridgeLatency(zinfo->launch.ackBytes);
    return cyc;
}

/**
 * Apply the kernel launch cost on the host core running `tid` at the offload
 * doorbell, BEFORE the device migration (thread still on its host core, so the
 * cost lands on the host and is inside the ROI/task-region window -- exactly
 * like chargeCoherenceFlush). Uses the synthetic BblInfo path because the ooo
 * host core does not override addDelay. Accumulates the launch stats.
 */
static void chargeLaunchCost(uint32_t tid) {
    uint64_t launchCyc = launchCostCycles();
    if (launchCyc == 0) return;
    uint32_t lc = (uint32_t)std::min<uint64_t>(launchCyc, 0xFFFFFFFFull);
    BblInfo* bbl = createSimpleBblInfo(lc, 0, true);  // 1.11.7: no phantom fetch; 1.11.16: injected charge
    fPtrs[tid].bblPtr(tid, 0, bbl);
    /* 1.11.7: the cmd packet crosses h2d, the ack returns d2h -- both are
     * real crossings and count as such. */
    __sync_fetch_and_add(&zinfo->xing.h2dBytes, (uint64_t)zinfo->launch.cmdBytes);
    __sync_fetch_and_add(&zinfo->xing.d2hBytes, (uint64_t)zinfo->launch.ackBytes);
    __sync_fetch_and_add(&zinfo->xing.count, 2);
    __sync_fetch_and_add(&zinfo->launch.launchCount, 1);
    __sync_fetch_and_add(&zinfo->launch.launchCyclesCharged, launchCyc);
    info("Thread %d: kernel launch cost at offload doorbell (doorbell %u + dispatch %u + bridge cmd/ack -> %llu cycles)",
         tid, zinfo->launch.doorbellCycles, zinfo->launch.dispatchCycles,
         (unsigned long long)launchCyc);
}

/**
 * Drain/commit pending synthetic host charges before the host->device migration.
 *
 * The OOO host core commits a BBL's cycles only when the FOLLOWING BBL is
 * decoded (decodeCycle pipeline lag); sched->leave()/finish() at migration does
 * NOT drain the window, so the LAST synthetic BBL issued before migration would
 * be dropped from the host core's cycle count. Without this drain the coherence
 * flush (1.7.2) landed only because the launch BBL happened to follow it, and
 * the launch BBL itself was lost. Issuing one trailing minimal BBL forces the
 * real flush+launch charges to retire onto the host timeline; this 1-instr
 * trailer becomes the new (negligible, ~1 cycle) orphan. Call AFTER the last
 * pre-migration host charge and BEFORE thread_domain flips to DOMAIN_DEVICE.
 */
static void drainHostCharges(uint32_t tid) {
    if (tid >= MAX_THREADS || !thread_initialized[tid]) return;
    BblInfo* bbl = createSimpleBblInfo(1, 4, true);  // 1.11.16: injected trailer
    fPtrs[tid].bblPtr(tid, 0, bbl);
}

/* Co-sim ROI offload entry for a thread that executes zsim_roi_begin() (the
 * opener). Factored out of the ROI_BEGIN handlers because the thread-MPI
 * branch RETURNS before the legacy inline block -- which meant no thread
 * ever opened the device window in thread-MPI runs (defect #14's deeper
 * layer: 'ROI offload begin' count was 0 while 'end' fired). Both paths and
 * both handler twins now share this one sequence. */
static void cosimRoiBeginOffload(uint32_t tid) {
    if (!(g_cosim_mode && !g_cosim_no_offload && tid < MAX_THREADS)) return;
    /* Thread-MPI: this rank already migrated in at the kernel-entry barrier
     * (flush+launch charged there); its roi_begin is just the marker. */
    if (g_mpi_thread_mode && thread_domain[tid].load() == DOMAIN_DEVICE) return;
    // PART A (1.7.2): Case-1 coherence flush charged on the HOST core BEFORE
    // the device migration (thread still on its host core), so the writeback
    // cost lands on the host and inside the ROI/task window.
    chargeCoherenceFlush(tid);
    // PART B (1.7.3): kernel LAUNCH cost charged on the HOST core at the
    // offload doorbell, also before the device migration.
    chargeLaunchCost(tid);
    // Retire the flush+launch synthetic BBLs onto the host timeline before
    // the migration drops the last one.
    drainHostCharges(tid);
    thread_domain[tid].store(DOMAIN_DEVICE);
    // 1.8.4 ROI window: first opener initializes the population; any later
    // opener (kernels where every rank calls roi_begin) just joins it.
    if (!g_in_device_region.exchange(true)) {
        g_deviceWorkers.store(1);
    } else {
        g_deviceWorkers.fetch_add(1);
    }
    g_roiClosing.store(false);
    ++offload_count;
    if (thread_initialized[tid]) {
        uint32_t cid = cids[tid];
        if (cid != INVALID_CID && cid != UNINITIALIZED_CID) {
            zinfo->sched->leave(procIdx, tid, cid);
        }
        zinfo->sched->finish(procIdx, tid);
        thread_initialized[tid] = false;
        clearCid(tid);
    }
    ensureThreadInit(tid);  // device mask
    info("Thread %d: ROI offload begin (co-sim)", tid);
}

/**
 * Magic instruction callback -- dispatches ZSim magic ops detected at
 * translation time.  The opcode (from the preceding mov $imm, %rcx) is
 * passed as userdata.
 */
static void magic_insn_exec_cb(unsigned int vcpu_index, void *userdata) {
    if (vcpu_index < MAX_VCPUS && vcpu_is_internal[vcpu_index]) return;
    uint64_t op = (uint64_t)(uintptr_t)userdata;
    uint32_t tid = getOrAssignTid(vcpu_index);

    /* Authoritative source: read the ACTUAL %rcx at the xchg.  This captures
     * runtime-built magic ops (e.g. zsim_work_begin_sized with a runtime size,
     * where %rcx is built by salq/orq and the translation-time decoder sees
     * nothing).  Falls back to the translation-time decoded value if the
     * register read is unavailable.  This callback is registered R_REGS. */
    resolveRcxHandle();
    uint64_t rcx;
    if (readRcx(&rcx) && rcx != 0) {
        op = rcx;
    }

    if (isMpiMagicOp(op)) {
        if (tid < MAX_THREADS) {
            ensureThreadInit(tid);
            handleMpiMagicOp(op, tid);
        }
        return;
    }

    // Extract low 32 bits as opcode, high 32 bits as optional payload size
    uint32_t opcode = static_cast<uint32_t>(op & 0xFFFFFFFF);
    uint32_t payload_size = static_cast<uint32_t>(op >> 32);

    if (opcode == ZSIM_MAGIC_OP_ROI_BEGIN) {
        // 1.6 thread-MPI: every rank-thread baselines ITS OWN core at ITS
        // OWN roi_begin (the process-mode contract, per-rank deterministic).
        // A first-rank-baselines-all sweep would capture the other 15 cores
        // at wall-arbitrary points -- stamps relative to those baselines
        // carried run-to-run jitter into every rendezvous downstream.
        if (g_mpi_thread_mode) {
            int prev = g_roiRefCount.fetch_add(1);
            uint32_t cid = (tid < MAX_THREADS) ? cids[tid] : INVALID_CID;
            if (cid != INVALID_CID && cid != UNINITIALIZED_CID &&
                cid < zinfo->numCores && zinfo->cores[cid])
                zinfo->cores[cid]->markRoiBegin();
            if (prev == 0) {
                in_roi.store(true);
                mpi_roi_baselined = true;
                snapshotRoiBaseCyc();
                roiRebaseTrafficCounters("thread-MPI first roi_begin");   // 1.11.90
                /* 1.11.54 (audit F008): OPEN THE PG/GAP WINDOW HERE TOO. This
                 * branch returns before the legacy path's
                 * pgres.markRoi(), so under thread-MPI the power-gating
                 * residency counters and the devMC gap histogram were either
                 * armed at a LATER event (the first barrier or send/recv,
                 * which carry their own !mpi_roi_baselined guard -- and this
                 * line has just set that flag) or, when roi_begin precedes
                 * any of them, never armed at all: GapHist::armed stayed 0,
                 * every bucket stayed empty, and gapPowerDownResidency
                 * silently fell back to the phase-granular residency. Two
                 * counters that get divided against each other must come
                 * from one window. */
                zinfo->pgres.markRoi(zinfo->numPhases, zinfo->globPhaseCycles);
            }
            // 1.8.4: the opener must open the co-sim device window HERE too
            // -- this branch returns before the legacy block below.
            cosimRoiBeginOffload(tid);
            return;
        }
        in_roi.store(true);
        // Snapshot each core's ROI baseline so the reported cycles/instrs cover
        // ONLY the kernel (roi_begin..roi_end), excluding the serial pre-ROI
        // array-init/setup that otherwise runs on the launcher PE and dominates.
        /* 1.11.57 (latent F010): ONE THREAD SNAPSHOTS EVERY CORE, and the
         * condition that makes that reproducible is stated rather than
         * assumed. Core::markRoiBegin captures instrs, the mix accumulators
         * and pgAct.activePhases; if another guest thread were retiring BBLs
         * on another core at this instant, that core's baseline would land at
         * a schedule-dependent point and the ROI window would differ run to
         * run. It does not happen in the shipped workloads: they call
         * zsim_roi_begin() from the single main thread BEFORE any parallel
         * region, and the shared-memory variants bracket the parallel region
         * rather than opening the ROI inside it. Nothing tears either -- the
         * fields are aligned 64-bit stores -- so the exposure is a shifted
         * baseline, not a corrupt one. The thread-MPI path does not have this
         * shape at all: it baselines only the calling rank's own core. */
        for (uint32_t c = 0; c < zinfo->numCores; c++)
            if (zinfo->cores[c]) zinfo->cores[c]->markRoiBegin();
        zinfo->pgres.markRoi(zinfo->numPhases, zinfo->globPhaseCycles);   // 1.11.18: PG residency window
        // This rank got a real roi_begin -> don't also synthesize a baseline at
        // the first MPI op (see handleMpiMagicOp). rank 0 takes this path.
        mpi_roi_baselined = true;
        snapshotRoiBaseCyc();
        roiRebaseTrafficCounters("roi_begin");   // 1.11.90
        // Co-sim: the ROI IS the offload region. The launching thread (and
        // every thread spawned inside the region) executes on the DEVICE;
        // out-of-ROI code executes on the host. Ordinary workloads are
        // co-sim workloads -- there is no special kind.
        cosimRoiBeginOffload(tid);
    } else if (opcode == ZSIM_MAGIC_OP_ROI_END) {
        // Co-sim: return the launching thread to a host core BEFORE the
        // termination flag, so the host core rejoin fast-forwards to the
        // global phase clock and host cycles absorb the device execution.
        if (g_cosim_mode && !g_cosim_no_offload && tid < MAX_THREADS) {
            g_in_device_region.store(false);
            if (g_mpi_thread_mode) {
                /* 1.8.7: the MPI opener (rank 0) is a PE-resident rank exactly
                 * like the others -- NO migration (removes even 1.8.6's residual
                 * rank-0 device->host leg; now truly ZERO migrations), and NO
                 * tail marker / g_deviceWorkers decrement HERE. It is tail-marked
                 * UNIFORMLY at the closing-barrier op (post-barrier), identical
                 * to ranks 1..N-1, and drains g_deviceWorkers there too, so its
                 * reduce-ROOT tail is measured from the same point as every peer.
                 * roi_end only flips the window to CLOSING. The OMP master (else
                 * branch) is UNTOUCHED: its workers are joined before roi_end, so
                 * its immediate host migration is validated. */
                info("Thread %d: ROI offload end (co-sim MPI opener, PE-resident, marked at barrier) [1.8.7]", tid);
                g_roiClosing.store(true);
            } else {
                thread_domain[tid].store(DOMAIN_HOST);
                // BUSY-WAIT occupancy (1.7.3, HANDOFF ISSUE 2, CUDA-default -- no IRQ,
                // no polling knob): while the device kernel ran, the host core was NOT
                // free to do other useful work -- it spun on the completion fence. In
                // device-only co-sim this is represented BY CONSTRUCTION: the single
                // launcher thread is either on the host (pre-offload flush+launch, and
                // post-offload readback) or on the device PE running the kernel -- it
                // is NEVER two places at once and NEVER races ahead to do other host
                // work during the device compute (there IS no other work in device-
                // only mode). The task-region measurement window (the makespan across
                // all cores) therefore already includes the device-execution duration
                // via the device PE, and the launch/flush costs charged at the doorbell
                // sit inside that window on the host timeline. The host cannot skip
                // ahead free -- it is self-penalized for the wait, exactly as a CUDA
                // busy-wait would be. (A multi-core host that could overlap independent
                // host work during offload would need an explicit spin charge here;
                // deferred with the multi-core-host baseline ladder.)
                if (thread_initialized[tid]) {
                    uint32_t cid = cids[tid];
                    if (cid != INVALID_CID && cid != UNINITIALIZED_CID) {
                        zinfo->sched->leave(procIdx, tid, cid);
                    }
                    zinfo->sched->finish(procIdx, tid);
                    thread_initialized[tid] = false;
                    clearCid(tid);
                }
                ensureThreadInit(tid);  // host mask
                info("Thread %d: ROI offload end (co-sim)", tid);
                // OMP master leaves the device here; window is CLOSING.
                g_deviceWorkers.fetch_sub(1);
                g_roiClosing.store(true);
            }
            // (MPI opener path set g_roiClosing above and defers its
            //  g_deviceWorkers decrement + tail marker to the closing barrier.)
        }
        // 1.6 thread-MPI: only the LAST rank out freezes stats + terminates;
        // earlier enders still owe MPI protocol work (closing barrier,
        // serving collectives) on live cores.
        if (g_mpi_thread_mode) {
            if (g_roiRefCount.fetch_sub(1) - 1 > 0) return;
            if (g_cosim_mode && !g_cosim_no_offload &&
                g_deviceWorkers.load() > 0) {
                /* 1.8.4 ROI window: the window is closing but other ranks
                 * are still computing on device PEs. The LAST rank out (at
                 * its barrier arrival, handleMpiMagicOp) freezes stats;
                 * freezing here at the opener's roi_end would truncate
                 * every other rank's compute tail. */
                return;
            }
            // This rank finished its ROI -> EXITED, so the epoch cut advances over
            // the still-running peers instead of pinning on this (now frozen) core.
            if (tid < MAX_THREADS && cids[tid] < MAX_THREADS)
                g_coreRoiState[cids[tid]].store(2, std::memory_order_release);
            in_roi.store(false);
            dumpTerminationStats();
            zinfo->terminationConditionMet = true;
            return;
        }
        in_roi.store(false);
        if (getenv("PIMID_MPI_RANK")) {
            /* MPI rank: freeze the ROI-scoped stats NOW (deterministic ROI
             * end) but DO NOT request termination -- this rank still owes MPI
             * protocol work (the closing barrier; serving the reduce as
             * root). The watchdog SimEnd here killed rank 0 mid-protocol at
             * high rank counts: peers then blocked forever on the dead
             * root's full mailbox ring (16 sends succeed, the rest
             * send_WAIT_full -- observed exactly in the 32-rank trace). The
             * process ends naturally at guest exit; SimEnd then skips the
             * already-done dump. At 4/8 ranks the old race was won by
             * timing luck; this makes the semantics deterministic. */
            dumpTerminationStats();
        } else {
            zinfo->terminationConditionMet = true;  // Let watchdog trigger SimEnd()
        }
    } else if (opcode == ZSIM_MAGIC_OP_WORK_BEGIN) {
        if (g_cosim_no_offload) return;  // NO_OFFLOAD baseline: WORK is stat-only (no migrate/charge)
        if (tid < MAX_THREADS) {
            // Kernel LAUNCH cost (1.7.3) charged on the HOST core at the WORK
            // offload doorbell BEFORE the device migration, so it lands on the
            // host inside the task window (the workloads here use ROI markers, but
            // the explicit-work offload path prices the launch identically).
            chargeLaunchCost(tid);
            /* PCIe/CXL offload timing: host -> device transfer latency.
             *
             * 1.11.57 (latent D012): CHARGED ON THE HOST CORE, LIKE ITS
             * MIRROR. This injection used to sit AFTER the detach and the
             * re-attach under device_mask, so the BBL retired on a device PE
             * at the DEVICE clock -- while the cycle count inside it was
             * built by getPCIeLatency from pcie.baseLatencyCycles and
             * pcie.bytesPerCycle, which 1.11.56 (B061) deliberately re-based
             * on the HOST/reference clock, saying in as many words that
             * "these cycles are SPENT by the host cores". At the reference
             * co-sim ratio (host 3000 MHz, device 500 MHz) every h2d transfer
             * was spent 6x longer than the nanoseconds it was converted from.
             * WORK_END already does it correctly -- it restores DOMAIN_HOST
             * and re-attaches before charging -- and that asymmetry between
             * two halves of one transfer is what makes this visible at all.
             * Latent because no shipped guest reaches this opcode: the co-sim
             * message-passing variants that docs/benchmarks.md says call
             * zsim_work_begin_sized do not exist in this tree.
             *
             * 1.11.7: timing-only BBL -- ZERO fetch bytes. The old pcieLat*4
             * manufactured phantom ifetch traffic that landed in the
             * cache/DRAM counters energy is charged from. */
            uint32_t pcieLat = boundaryTransferCycles(payload_size);
            if (pcieLat > 0) {
                BblInfo* bbl = createSimpleBblInfo(pcieLat, 0, true);  // 1.11.16: injected charge
                fPtrs[tid].bblPtr(tid, 0, bbl);
            }
            drainHostCharges(tid);  // retire launch + h2d BBLs before migration drops them
            thread_domain[tid].store(DOMAIN_DEVICE);
            g_in_device_region.store(true);
            uint64_t cnt = ++offload_count;
            /* If thread already running on a host core, leave it and
             * re-initialize with device_mask so it joins a device PE. */
            if (thread_initialized[tid]) {
                uint32_t cid = cids[tid];
                if (cid != INVALID_CID && cid != UNINITIALIZED_CID) {
                    zinfo->sched->leave(procIdx, tid, cid);
                }
                zinfo->sched->finish(procIdx, tid);
                thread_initialized[tid] = false;
                clearCid(tid);
            }
            ensureThreadInit(tid);  // will use device_mask
            __sync_fetch_and_add(&zinfo->xing.h2dBytes, (uint64_t)payload_size);
            __sync_fetch_and_add(&zinfo->xing.count, 1);
            info("Thread %d: WORK_BEGIN (device offload #%lu, %u bytes)", tid,
                 (unsigned long)cnt, payload_size);
        }
        return;
    } else if (opcode == ZSIM_MAGIC_OP_WORK_END) {
        if (g_cosim_no_offload) return;  // NO_OFFLOAD baseline: WORK is stat-only (no migrate/charge)
        g_in_device_region.store(false);
        if (tid < MAX_THREADS) {
            /* Migrate the offloading thread BACK to a host core (mirror of
             * WORK_BEGIN). Without this it stays on a device PE forever, so
             * all post-offload host work (final merge, verification) accrues
             * on device_pes-0 -- and under address-routed pricing is
             * mispriced as device-issued host accesses (host latency plus
             * attach tolls per read). The host-core rejoin also fast-forwards
             * its cycle counter to the global phase clock, so host cycles
             * correctly absorb the device execution it waited for. */
            thread_domain[tid].store(DOMAIN_HOST);
            if (thread_initialized[tid]) {
                uint32_t cid = cids[tid];
                if (cid != INVALID_CID && cid != UNINITIALIZED_CID) {
                    zinfo->sched->leave(procIdx, tid, cid);
                }
                zinfo->sched->finish(procIdx, tid);
                thread_initialized[tid] = false;
                clearCid(tid);
            }
            ensureThreadInit(tid);  // will use host_mask
            /* PCIe/CXL return timing: device->host transfer latency,
             * charged on the host core (it pays for the return DMA wait) */
            uint32_t pcieLat = boundaryTransferCycles(payload_size);
            if (pcieLat > 0) {
                BblInfo* bbl = createSimpleBblInfo(pcieLat, 0, true);  // 1.11.7: no phantom fetch; 1.11.16: injected charge
                fPtrs[tid].bblPtr(tid, 0, bbl);
            }
            __sync_fetch_and_add(&zinfo->xing.d2hBytes, (uint64_t)payload_size);
            __sync_fetch_and_add(&zinfo->xing.count, 1);
            info("Thread %d: WORK_END (%u bytes)", tid, payload_size);
        }
        return;
    } else {
        return;
    }
    uint64_t count = ++roi_transition_count;
    info("ROI %s (transition #%lu)", opcode == ZSIM_MAGIC_OP_ROI_BEGIN ? "BEGIN" : "END",
         (unsigned long)count);
}

/**
 * Execution callback for mov $imm, %rcx at end of a TB -- stores the opcode
 * into the per-vCPU pending slot so the next TB's xchg can consume it.
 */
static void mov_magic_exec_cb(unsigned int vcpu_index, void *userdata) {
    uint64_t op = (uint64_t)(uintptr_t)userdata;
    uint32_t tid = getOrAssignTid(vcpu_index);
    if (tid < MAX_THREADS) {
        pending_magic_op[tid].store(op);
    }
}

/**
 * Execution callback for xchg %rcx, %rcx without a preceding mov in the
 * same TB.  Checks the per-vCPU pending slot for a cross-TB magic op.
 */
static void xchg_pending_exec_cb(unsigned int vcpu_index, void *userdata) {
    (void)userdata;
    if (vcpu_index < MAX_VCPUS && vcpu_is_internal[vcpu_index]) return;
    uint32_t tid = getOrAssignTid(vcpu_index);
    if (tid >= MAX_THREADS) return;
    /* Always drain the pending slot to keep cross-TB mov state consistent. */
    uint64_t op = pending_magic_op[tid].exchange(0);

    /* Authoritative source: read the ACTUAL %rcx at the xchg.  This is the
     * general fix for runtime-built magic ops (the pending slot only ever
     * holds translation-time decoded mov-immediates).  This callback is
     * registered R_REGS. */
    resolveRcxHandle();
    uint64_t rcx;
    if (readRcx(&rcx) && rcx != 0) {
        op = rcx;
    }

    if (isMpiMagicOp(op)) {
        ensureThreadInit(tid);
        handleMpiMagicOp(op, tid);
        return;
    }
    // Extract low 32 bits as opcode, high 32 bits as optional payload size
    uint32_t opcode = static_cast<uint32_t>(op & 0xFFFFFFFF);
    uint32_t payload_size = static_cast<uint32_t>(op >> 32);

    if (opcode == ZSIM_MAGIC_OP_ROI_BEGIN) {
        // 1.6 thread-MPI: every rank-thread baselines ITS OWN core at ITS
        // OWN roi_begin (the process-mode contract, per-rank deterministic).
        // A first-rank-baselines-all sweep would capture the other 15 cores
        // at wall-arbitrary points -- stamps relative to those baselines
        // carried run-to-run jitter into every rendezvous downstream.
        if (g_mpi_thread_mode) {
            int prev = g_roiRefCount.fetch_add(1);
            uint32_t cid = (tid < MAX_THREADS) ? cids[tid] : INVALID_CID;
            if (cid != INVALID_CID && cid != UNINITIALIZED_CID &&
                cid < zinfo->numCores && zinfo->cores[cid])
                zinfo->cores[cid]->markRoiBegin();
            if (prev == 0) {
                in_roi.store(true);
                mpi_roi_baselined = true;
                snapshotRoiBaseCyc();
                roiRebaseTrafficCounters("thread-MPI first roi_begin");   // 1.11.90
                /* 1.11.54 (audit F008): OPEN THE PG/GAP WINDOW HERE TOO. This
                 * branch returns before the legacy path's
                 * pgres.markRoi(), so under thread-MPI the power-gating
                 * residency counters and the devMC gap histogram were either
                 * armed at a LATER event (the first barrier or send/recv,
                 * which carry their own !mpi_roi_baselined guard -- and this
                 * line has just set that flag) or, when roi_begin precedes
                 * any of them, never armed at all: GapHist::armed stayed 0,
                 * every bucket stayed empty, and gapPowerDownResidency
                 * silently fell back to the phase-granular residency. Two
                 * counters that get divided against each other must come
                 * from one window. */
                zinfo->pgres.markRoi(zinfo->numPhases, zinfo->globPhaseCycles);
            }
            // 1.8.4: the opener must open the co-sim device window HERE too
            // -- this branch returns before the legacy block below.
            cosimRoiBeginOffload(tid);
            return;
        }
        in_roi.store(true);
        // Snapshot each core's ROI baseline so the reported cycles/instrs cover
        // ONLY the kernel (roi_begin..roi_end), excluding the serial pre-ROI
        // array-init/setup that otherwise runs on the launcher PE and dominates.
        /* 1.11.57 (latent F010): ONE THREAD SNAPSHOTS EVERY CORE, and the
         * condition that makes that reproducible is stated rather than
         * assumed. Core::markRoiBegin captures instrs, the mix accumulators
         * and pgAct.activePhases; if another guest thread were retiring BBLs
         * on another core at this instant, that core's baseline would land at
         * a schedule-dependent point and the ROI window would differ run to
         * run. It does not happen in the shipped workloads: they call
         * zsim_roi_begin() from the single main thread BEFORE any parallel
         * region, and the shared-memory variants bracket the parallel region
         * rather than opening the ROI inside it. Nothing tears either -- the
         * fields are aligned 64-bit stores -- so the exposure is a shifted
         * baseline, not a corrupt one. The thread-MPI path does not have this
         * shape at all: it baselines only the calling rank's own core. */
        for (uint32_t c = 0; c < zinfo->numCores; c++)
            if (zinfo->cores[c]) zinfo->cores[c]->markRoiBegin();
        zinfo->pgres.markRoi(zinfo->numPhases, zinfo->globPhaseCycles);   // 1.11.18: PG residency window
        // This rank got a real roi_begin -> don't also synthesize a baseline at
        // the first MPI op (see handleMpiMagicOp). rank 0 takes this path.
        mpi_roi_baselined = true;
        snapshotRoiBaseCyc();
        roiRebaseTrafficCounters("roi_begin");   // 1.11.90
        // Co-sim: ROI = offload region (see the twin handler above).
        cosimRoiBeginOffload(tid);
    } else if (opcode == ZSIM_MAGIC_OP_ROI_END) {
        if (g_cosim_mode && !g_cosim_no_offload && tid < MAX_THREADS) {
            g_in_device_region.store(false);
            if (g_mpi_thread_mode) {
                /* 1.8.7: the MPI opener (rank 0) is a PE-resident rank exactly
                 * like the others -- NO migration (removes even 1.8.6's residual
                 * rank-0 device->host leg; now truly ZERO migrations), and NO
                 * tail marker / g_deviceWorkers decrement HERE. It is tail-marked
                 * UNIFORMLY at the closing-barrier op (post-barrier), identical
                 * to ranks 1..N-1, and drains g_deviceWorkers there too, so its
                 * reduce-ROOT tail is measured from the same point as every peer.
                 * roi_end only flips the window to CLOSING. The OMP master (else
                 * branch) is UNTOUCHED: its workers are joined before roi_end, so
                 * its immediate host migration is validated. */
                info("Thread %d: ROI offload end (co-sim MPI opener, PE-resident, marked at barrier) [1.8.7]", tid);
                g_roiClosing.store(true);
            } else {
                thread_domain[tid].store(DOMAIN_HOST);
                // BUSY-WAIT occupancy (1.7.3, HANDOFF ISSUE 2, CUDA-default -- no IRQ,
                // no polling knob): while the device kernel ran, the host core was NOT
                // free to do other useful work -- it spun on the completion fence. In
                // device-only co-sim this is represented BY CONSTRUCTION: the single
                // launcher thread is either on the host (pre-offload flush+launch, and
                // post-offload readback) or on the device PE running the kernel -- it
                // is NEVER two places at once and NEVER races ahead to do other host
                // work during the device compute (there IS no other work in device-
                // only mode). The task-region measurement window (the makespan across
                // all cores) therefore already includes the device-execution duration
                // via the device PE, and the launch/flush costs charged at the doorbell
                // sit inside that window on the host timeline. The host cannot skip
                // ahead free -- it is self-penalized for the wait, exactly as a CUDA
                // busy-wait would be. (A multi-core host that could overlap independent
                // host work during offload would need an explicit spin charge here;
                // deferred with the multi-core-host baseline ladder.)
                if (thread_initialized[tid]) {
                    uint32_t cid = cids[tid];
                    if (cid != INVALID_CID && cid != UNINITIALIZED_CID) {
                        zinfo->sched->leave(procIdx, tid, cid);
                    }
                    zinfo->sched->finish(procIdx, tid);
                    thread_initialized[tid] = false;
                    clearCid(tid);
                }
                ensureThreadInit(tid);  // host mask
                info("Thread %d: ROI offload end (co-sim)", tid);
                // OMP master leaves the device here; window is CLOSING.
                g_deviceWorkers.fetch_sub(1);
                g_roiClosing.store(true);
            }
            // (MPI opener path set g_roiClosing above and defers its
            //  g_deviceWorkers decrement + tail marker to the closing barrier.)
        }
        // 1.6 thread-MPI: only the LAST rank out freezes stats + terminates;
        // earlier enders still owe MPI protocol work (closing barrier,
        // serving collectives) on live cores.
        if (g_mpi_thread_mode) {
            if (g_roiRefCount.fetch_sub(1) - 1 > 0) return;
            if (g_cosim_mode && !g_cosim_no_offload &&
                g_deviceWorkers.load() > 0) {
                /* 1.8.4 ROI window: the window is closing but other ranks
                 * are still computing on device PEs. The LAST rank out (at
                 * its barrier arrival, handleMpiMagicOp) freezes stats;
                 * freezing here at the opener's roi_end would truncate
                 * every other rank's compute tail. */
                return;
            }
            // This rank finished its ROI -> EXITED, so the epoch cut advances over
            // the still-running peers instead of pinning on this (now frozen) core.
            if (tid < MAX_THREADS && cids[tid] < MAX_THREADS)
                g_coreRoiState[cids[tid]].store(2, std::memory_order_release);
            in_roi.store(false);
            dumpTerminationStats();
            zinfo->terminationConditionMet = true;
            return;
        }
        in_roi.store(false);
        if (getenv("PIMID_MPI_RANK")) {
            /* MPI rank: freeze the ROI-scoped stats NOW (deterministic ROI
             * end) but DO NOT request termination -- this rank still owes MPI
             * protocol work (the closing barrier; serving the reduce as
             * root). The watchdog SimEnd here killed rank 0 mid-protocol at
             * high rank counts: peers then blocked forever on the dead
             * root's full mailbox ring (16 sends succeed, the rest
             * send_WAIT_full -- observed exactly in the 32-rank trace). The
             * process ends naturally at guest exit; SimEnd then skips the
             * already-done dump. At 4/8 ranks the old race was won by
             * timing luck; this makes the semantics deterministic. */
            dumpTerminationStats();
        } else {
            zinfo->terminationConditionMet = true;  // Let watchdog trigger SimEnd()
        }
    } else if (opcode == ZSIM_MAGIC_OP_WORK_BEGIN) {
        if (g_cosim_no_offload) return;  // NO_OFFLOAD baseline: WORK is stat-only (no migrate/charge)
        // Kernel LAUNCH cost (1.7.3) charged on the HOST core at the WORK offload
        // doorbell BEFORE the device migration (see the twin handler above).
        chargeLaunchCost(tid);
        /* PCIe/CXL offload timing: host -> device transfer latency, charged
         * on the HOST core -- 1.11.57 (latent D012), see the twin handler
         * above for why the post-migration position spent host-clocked cycles
         * at the device clock. */
        uint32_t pcieLat = boundaryTransferCycles(payload_size);
        if (pcieLat > 0) {
            BblInfo* bbl = createSimpleBblInfo(pcieLat, 0, true);  // 1.11.7: no phantom fetch; 1.11.16: injected charge
            fPtrs[tid].bblPtr(tid, 0, bbl);
        }
        drainHostCharges(tid);  // retire launch + h2d BBLs before migration drops them
        thread_domain[tid].store(DOMAIN_DEVICE);
        g_in_device_region.store(true);
        uint64_t cnt = ++offload_count;
        /* If thread already running on a host core, leave it and
         * re-initialize with device_mask so it joins a device PE. */
        if (thread_initialized[tid]) {
            uint32_t cid = cids[tid];
            if (cid != INVALID_CID && cid != UNINITIALIZED_CID) {
                zinfo->sched->leave(procIdx, tid, cid);
            }
            zinfo->sched->finish(procIdx, tid);
            thread_initialized[tid] = false;
            clearCid(tid);
        }
        ensureThreadInit(tid);
        __sync_fetch_and_add(&zinfo->xing.h2dBytes, (uint64_t)payload_size);
        __sync_fetch_and_add(&zinfo->xing.count, 1);
        info("Thread %d: WORK_BEGIN (device offload #%lu, %u bytes)", tid,
             (unsigned long)cnt, payload_size);
        return;
    } else if (opcode == ZSIM_MAGIC_OP_WORK_END) {
        if (g_cosim_no_offload) return;  // NO_OFFLOAD baseline: WORK is stat-only (no migrate/charge)
        g_in_device_region.store(false);
        /* Migrate back to a host core -- see the WORK_END handler above. */
        thread_domain[tid].store(DOMAIN_HOST);
        if (thread_initialized[tid]) {
            uint32_t cid = cids[tid];
            if (cid != INVALID_CID && cid != UNINITIALIZED_CID) {
                zinfo->sched->leave(procIdx, tid, cid);
            }
            zinfo->sched->finish(procIdx, tid);
            thread_initialized[tid] = false;
            clearCid(tid);
        }
        ensureThreadInit(tid);  // will use host_mask
        /* PCIe/CXL return timing: device->host transfer latency */
        uint32_t pcieLat = boundaryTransferCycles(payload_size);
        if (pcieLat > 0) {
            BblInfo* bbl = createSimpleBblInfo(pcieLat, 0, true);  // 1.11.7: no phantom fetch; 1.11.16: injected charge
            fPtrs[tid].bblPtr(tid, 0, bbl);
        }
        __sync_fetch_and_add(&zinfo->xing.d2hBytes, (uint64_t)payload_size);
        __sync_fetch_and_add(&zinfo->xing.count, 1);
        info("Thread %d: WORK_END (%u bytes)", tid, payload_size);
        return;
    } else {
        return;
    }
    uint64_t count = ++roi_transition_count;
    info("ROI %s (transition #%lu)", opcode == ZSIM_MAGIC_OP_ROI_BEGIN ? "BEGIN" : "END",
         (unsigned long)count);
}

/**
 * Translation block callback -- instruments each TB with:
 *   1. A BBL callback on the first instruction
 *   2. Memory callbacks on each instruction
 */
static void tb_trans_cb(qemu_plugin_id_t id, struct qemu_plugin_tb *tb) {
    size_t n_insns = qemu_plugin_tb_n_insns(tb);
    uint64_t tb_addr = qemu_plugin_tb_vaddr(tb);

    /* Compute total TB size */
    uint32_t tb_bytes = 0;
    for (size_t i = 0; i < n_insns; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        tb_bytes += (uint32_t)qemu_plugin_insn_size(insn);
    }

    /* Get or create BblInfo for this TB (decodes DynUops when OOO is present) */
    BblInfo* bblInfo = getOrCreateBblInfo(tb_addr, (uint32_t)n_insns, tb_bytes, tb);

    /* Allocate persistent userdata for this TB's callbacks.
     * QEMU may re-translate TBs, so we use the cached BblInfo.
     * The TbUserdata is leaked intentionally -- it persists for the
     * lifetime of the process. */
    TbUserdata* tud = (TbUserdata*)malloc(sizeof(TbUserdata));
    tud->bblInfo = bblInfo;
    tud->tbAddr = tb_addr;
    tud->approxKeys = nullptr;
    tud->numApproxKeys = 0;
    tud->numInsns = (uint32_t)n_insns;
    tud->endsInCondBranch = false;
    tud->brPc = tud->brTakenTarget = tud->brFallthrough = 0;
    tud->termKind = 0;
    tud->termPc = tud->termRetAddr = 0;

    /* Branch-predictor wiring (OOO and decode-enabled in-order): classify this
     * TB's TERMINATOR so its outcome can be resolved from the next TB's start
     * address and fed to the core's predictors:
     *  - conditional jcc            -> direction feed (2-level predictor)
     *  - direct call (E8)           -> RAS push (target always predicted)
     *  - indirect call (FF /2,/3)   -> BTB target check + RAS push
     *  - indirect jmp (FF /4,/5)    -> BTB target check
     *  - ret (C3/C2)                -> RAS pop + target check
     * Direct jmp (E9/EB) has a fixed correctly-predicted target: no feed.
     * Since 1.11.15 g_decode_enabled defaults to TRUE for every core type
     * (census decode-always); only the PIMID_NODECODE escape restores the
     * old (ooo && !ooo_nodecode) || (inorder && !inorder_nodecode) gate. */
    if (g_decode_enabled && n_insns > 0) {
        struct qemu_plugin_insn* last = qemu_plugin_tb_get_insn(tb, n_insns - 1);
        uint64_t lpc = qemu_plugin_insn_vaddr(last);
        size_t lsz = qemu_plugin_insn_size(last);
        uint8_t lb[16];
        qemu_plugin_insn_data(last, lb, sizeof(lb));
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
            tud->endsInCondBranch = true;
            tud->brPc = lpc;
            tud->brFallthrough = lpc + lsz;
            tud->brTakenTarget = lpc + lsz + (uint64_t)rel;
        } else if (p < lsz) {
            uint8_t opb = lb[p];
            if (opb == 0xE8) {                                    /* direct call */
                tud->termKind = CF_DIR_CALL;
                tud->termPc = lpc;
                tud->termRetAddr = lpc + lsz;
            } else if (opb == 0xC3 || opb == 0xC2) {              /* ret */
                tud->termKind = CF_RET;
                tud->termPc = lpc;
            } else if (opb == 0xFF && (p + 1) < lsz) {            /* grp5 */
                uint8_t ext = (lb[p + 1] >> 3) & 7;
                if (ext == 2 || ext == 3) {                       /* call r/m */
                    tud->termKind = CF_IND_CALL;
                    tud->termPc = lpc;
                    tud->termRetAddr = lpc + lsz;
                } else if (ext == 4 || ext == 5) {                /* jmp r/m */
                    tud->termKind = CF_IND_JMP;
                    tud->termPc = lpc;
                }
            }
        }
    }

    if (g_ooo_dump) {
        std::lock_guard<std::mutex> lock(bblCacheMutex);
        auto ai = g_tbApproxCache.find(tb_addr);
        if (ai != g_tbApproxCache.end()) {
            tud->approxKeys = ai->second.first;
            tud->numApproxKeys = ai->second.second;
        }
    }

    bool bbl_registered = false;

    /* Track in-TB mov $imm, %rcx -> xchg %rcx, %rcx pairs.
     * prev_magic_op is local (no cross-vCPU race).  Cross-TB communication
     * is deferred to execution time via per-vCPU pending_magic_op[]. */
    uint64_t prev_magic_op = 0;
    struct qemu_plugin_insn *last_magic_mov_insn = NULL;

    for (size_t i = 0; i < n_insns; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        size_t insn_sz = qemu_plugin_insn_size(insn);
        uint8_t insn_buf[16];
        qemu_plugin_insn_data(insn, insn_buf, sizeof(insn_buf));
        const uint8_t *bytes = insn_buf;

        /* Detect xchg %rcx, %rcx (48 87 c9) */
        if (insn_sz == 3 && bytes[0] == 0x48 && bytes[1] == 0x87
                && bytes[2] == 0xc9) {
            uint32_t prev_opcode = static_cast<uint32_t>(prev_magic_op & 0xFFFFFFFF);
            if (prev_opcode == ZSIM_MAGIC_OP_ROI_BEGIN
                    || prev_opcode == ZSIM_MAGIC_OP_ROI_END
                    || prev_opcode == ZSIM_MAGIC_OP_WORK_BEGIN
                    || prev_opcode == ZSIM_MAGIC_OP_WORK_END
                    || isMpiMagicOp(prev_magic_op)) {
                /* In-TB case: mov+xchg in same TB -- opcode known at
                 * translation time, passed as fallback via userdata.  The
                 * callback reads the real %rcx at execution time (R_REGS) as
                 * the authoritative value so runtime-built sizes are captured. */
                qemu_plugin_register_vcpu_insn_exec_cb(
                    insn, magic_insn_exec_cb, QEMU_PLUGIN_CB_R_REGS,
                    (void *)(uintptr_t)prev_magic_op);
            } else {
                /* Cross-TB case: no preceding mov in this TB -- read the real
                 * %rcx at execution time (R_REGS); the per-vCPU pending slot
                 * serves only as a fallback. */
                qemu_plugin_register_vcpu_insn_exec_cb(
                    insn, xchg_pending_exec_cb, QEMU_PLUGIN_CB_R_REGS,
                    NULL);
            }
            prev_magic_op = 0;
            last_magic_mov_insn = NULL;
            continue;
        }

        /* Check if this instruction is mov $imm, %rcx/%ecx (magic op setup).
         * Three possible encodings:
         *   b9 xx xx xx xx          -- mov $imm32, %ecx  (5 bytes)
         *   48 c7 c1 xx xx xx xx   -- mov $imm32, %rcx  (7 bytes)
         *   48 b9 xx*8             -- movabs $imm64, %rcx (10 bytes) */
        prev_magic_op = 0;
        last_magic_mov_insn = NULL;
        if (insn_sz == 5 && bytes[0] == 0xb9) {
            prev_magic_op = (uint32_t)bytes[1] | ((uint32_t)bytes[2] << 8)
                          | ((uint32_t)bytes[3] << 16) | ((uint32_t)bytes[4] << 24);
        } else if (insn_sz == 7 && bytes[0] == 0x48 && bytes[1] == 0xc7
                   && bytes[2] == 0xc1) {
            prev_magic_op = (uint32_t)bytes[3] | ((uint32_t)bytes[4] << 8)
                          | ((uint32_t)bytes[5] << 16) | ((uint32_t)bytes[6] << 24);
        } else if (insn_sz == 10 && bytes[0] == 0x48 && bytes[1] == 0xb9) {
            /* movabs $imm64,%rcx -- read the FULL 8-byte immediate. The high 32
             * bits carry the optional WORK_BEGIN payload size; reading only the
             * low 4 bytes (as before) silently dropped it, so sized transfers
             * were charged as 0 bytes. Cast each byte to uint64_t before shifting
             * past bit 31 to avoid 32-bit overflow. */
            prev_magic_op = (uint64_t)bytes[2]        | ((uint64_t)bytes[3] << 8)
                          | ((uint64_t)bytes[4] << 16) | ((uint64_t)bytes[5] << 24)
                          | ((uint64_t)bytes[6] << 32) | ((uint64_t)bytes[7] << 40)
                          | ((uint64_t)bytes[8] << 48) | ((uint64_t)bytes[9] << 56);
        }
        if (prev_magic_op != 0) {
            last_magic_mov_insn = insn;
        }

        /* Register BBL callback on first non-magic instruction of TB */
        if (!bbl_registered) {
            qemu_plugin_register_vcpu_insn_exec_cb(
                insn, insn_exec_cb, QEMU_PLUGIN_CB_NO_REGS, tud);
            bbl_registered = true;
        }

        /* Register memory callback for every instruction */
        qemu_plugin_register_vcpu_mem_cb(
            insn, mem_cb, QEMU_PLUGIN_CB_NO_REGS,
            QEMU_PLUGIN_MEM_RW, NULL);
    }

    /* If the TB ends with a mov $imm, %rcx that wasn't consumed by an xchg
     * in this TB, register an execution callback to store the opcode into
     * the per-vCPU pending slot for the next TB's xchg to pick up. */
    if (prev_magic_op != 0 && last_magic_mov_insn != NULL) {
        qemu_plugin_register_vcpu_insn_exec_cb(
            last_magic_mov_insn, mov_magic_exec_cb, QEMU_PLUGIN_CB_NO_REGS,
            (void *)(uintptr_t)prev_magic_op);
    }
}

/**
 * vCPU init callback -- assigns tid but defers SimThreadStart until first
 * execution callback, so we know the thread's domain (HOST vs DEVICE).
 */
static void vcpu_init_cb(qemu_plugin_id_t id, unsigned int vcpu_index) {
    /* vCPUs created during SimInit are ZSim-internal (watchdog, contention sim).
     * They must not participate in the phase barrier. */
    if (!sim_initialized && vcpu_index > 0) {
        if (vcpu_index < MAX_VCPUS) {
            vcpu_is_internal[vcpu_index] = true;
        }
        return;  // don't assign TID
    }

    uint32_t tid = getOrAssignTid(vcpu_index);
    if (tid < MAX_THREADS) {
        thread_initialized[tid] = false;
        /* Inherit DEVICE domain if spawned inside an active offload region,
         * so device-worker threads map to device PEs, not host cores. */
        thread_domain[tid].store(
            g_in_device_region.load() ? DOMAIN_DEVICE : DOMAIN_HOST);
    }
}

/**
 * vCPU exit callback -- thread finish.
 */
static void vcpu_exit_cb(qemu_plugin_id_t id, unsigned int vcpu_index) {
    if (vcpu_index < MAX_VCPUS && vcpu_is_internal[vcpu_index]) return;
    if (vcpu_index < MAX_VCPUS) {
        uint32_t tid = vcpuToTid[vcpu_index];
        if (tid != UNINITIALIZED_CID && tid < MAX_THREADS) {
            if (!thread_initialized[tid]) return;  // never started
            /* Leave scheduler if still scheduled */
            uint32_t cid = cids[tid];
            if (cid != INVALID_CID && cid != UNINITIALIZED_CID) {
                zinfo->sched->leave(procIdx, tid, cid);
            }
            SimThreadFini(tid);
        }
    }
}

/**
 * Syscall callback -- scheduler leave/join around blocking syscalls.
 * For thread-creating syscalls (clone/fork), we don't need special
 * handling since QEMU fires vcpu_init for new threads.
 */
static void syscall_cb(qemu_plugin_id_t id,
                       unsigned int vcpu_index,
                       int64_t num,
                       uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t a4,
                       uint64_t a5, uint64_t a6,
                       uint64_t a7, uint64_t a8) {
    if (vcpu_index >= MAX_VCPUS) return;
    if (vcpu_is_internal[vcpu_index]) return;
    /* Guard: skip if we're already inside ZSim code (e.g., reentrant
     * callback from barrier/scheduler futex operations). */
    if (in_zsim[vcpu_index]) return;
    uint32_t tid = vcpuToTid[vcpu_index];
    if (tid == UNINITIALIZED_CID || tid >= MAX_THREADS) return;
    if (!thread_initialized[tid]) return;  // not yet started

    uint32_t cid = cids[tid];
    if (cid == INVALID_CID || cid == UNINITIALIZED_CID) return;

    /* For potentially blocking syscalls, use the scheduler's adaptive
     * syscallLeave mechanism (matching ZSim's Pin-mode behavior):
     * - Short/non-blocking syscalls get a "fake leave" -- the barrier
     *   stays in RUNNING state, avoiding the expensive leave/join cycle.
     * - Truly blocking syscalls are detected by the watchdog thread,
     *   which forces a real leave after a timeout.
     * This prevents barrier state corruption from the real leave/join
     * cycle while still handling blocking syscalls correctly. */
    switch (num) {
        case 60:  /* exit -- single thread */
            /* A worker (non-main) thread finishing must NOT end the sim and
             * must NOT be torn down here: vcpu_exit_cb already does
             * leave()+SimThreadFini() when the vCPU exits. Ending here
             * truncates the other device PEs; finishing here double-frees
             * the scheduler gid (scheduler.h:234 assert). */
            if (tid != 0) return;
            /* main thread: fall through to terminate the whole simulation */
        case 231: /* exit_group -- whole process */
            info("Caught exit syscall (%ld), terminating simulation", (long)num);
            zinfo->sched->leave(procIdx, tid, cid);
            SimEnd();
            break;  /* not reached */
        case 0:   /* read */
        case 1:   /* write */
        case 7:   /* poll */
        case 23:  /* select */
        case 35:  /* nanosleep */
        case 43:  /* accept */
        case 45:  /* recvfrom */
        case 47:  /* recvmsg */
        case 202: /* futex */
        case 230: /* clock_nanosleep -- glibc >= 2.31 implements ALL sleeps
                   * with this, never plain nanosleep(35) */
        case 232: /* epoll_wait */
        case 270: /* pselect6 */
        case 271: /* ppoll -- glibc poll() uses this on modern kernels */
        case 281: /* epoll_pwait */
        case 288: /* accept4 */
        case 449: /* futex_waitv -- glibc >= 2.35 pthread waits use this on
                   * kernels >= 5.16. Without a syscallLeave here a guest
                   * thread blocking in it kept its core slot and starved the
                   * phase barrier: cosim hung at device-thread startup with
                   * image-built (glibc 2.39) workloads on new kernels, while
                   * the same run passed natively with glibc-2.34 binaries
                   * (those fall back to futex(202)). */
        {
            /* MPI comm window: the rank is polling its shm mailbox. Keep the
             * core ATTACHED (no leave) so post-comm compute is counted and
             * cycles do not couple to the wall-clock poll duration. Gated to
             * the bracketed MPI transport only -- OMP/co-sim never set this. */
            if (tid < MAX_THREADS &&
                mpi_comm_window[tid].load(std::memory_order_acquire)) {
                break;
            }
            /* Use syscall number as synthetic PC for the blacklist.
             * Offset by 0x1000 to avoid address 0 (which could confuse
             * the blacklist's unordered_set). */
            uint64_t synth_pc = 0x1000 + (uint64_t)num;
            zinfo->sched->syscallLeave(procIdx, tid, cid, synth_pc,
                                       (int)num, a1, a2);
            /* Whether fake or real leave occurred, set joinPtrs so the
             * next execution callback triggers Join(), which handles
             * both cases: fakeLeave fast-path or full bar.join(). */
            clearCid(tid);
            fPtrs[tid] = joinPtrs;
            break;
        }
        default:
            break;
    }
}

/**
 * Plugin exit -- dump stats and clean up.
 */
static void plugin_exit(qemu_plugin_id_t id, void *userdata) {
    (void)userdata;
    /* Shared detailed-MPI Garnet: mark this rank EXITED first, so peers'
     * merged-replay cuts stop including it immediately (no wait on the dead). */
    pimid_noc_mark_state(PIMID_NOC_EXITED);
    /* Finalize any threads still registered */
    for (uint32_t v = 0; v < MAX_VCPUS; v++) {
        uint32_t tid = vcpuToTid[v];
        if (tid != UNINITIALIZED_CID && tid < MAX_THREADS) {
            if (!thread_initialized[tid]) continue;  // never started
            uint32_t cid = cids[tid];
            if (cid != INVALID_CID && cid != UNINITIALIZED_CID) {
                zinfo->sched->leave(procIdx, tid, cid);
            }
            SimThreadFini(tid);
        }
    }

    SimEnd();

    char msg[256];
    uint64_t totalInstrs = 0;
    for (uint32_t i = 0; i < zinfo->numCores; i++) {
        totalInstrs += zinfo->cores[i]->getInstrs();
    }
    snprintf(msg, sizeof(msg),
             "ZSim+QEMU: simulation complete. %lu total instructions, %lu phases\n",
             (unsigned long)totalInstrs, (unsigned long)zinfo->numPhases);
    qemu_plugin_outs(msg);

    if (roi_transition_count.load() > 0) {
        snprintf(msg, sizeof(msg),
                 "ZSim+QEMU ROI: %lu transitions, final state: %s\n",
                 (unsigned long)roi_transition_count.load(),
                 in_roi.load() ? "recording" : "not recording");
        qemu_plugin_outs(msg);
    }

    uint64_t oc = offload_count.load();
    if (oc > 0) {
        snprintf(msg, sizeof(msg),
                 "ZSim+QEMU co-sim: %lu offload transitions\n",
                 (unsigned long)oc);
        qemu_plugin_outs(msg);
    }
}

/* ---- Plugin Install ---- */

extern "C" QEMU_PLUGIN_EXPORT
int qemu_plugin_install(qemu_plugin_id_t id,
                        const qemu_info_t *info,
                        int argc, char **argv) {
    pluginId = id;

    /* Parse arguments */
    uint32_t argProcIdx = 0;
    for (int i = 0; i < argc; i++) {
        char *arg = argv[i];
        if (strncmp(arg, "shmid=", 6) == 0) {
            shmId = (uint32_t)atoi(arg + 6);
            useSharedMem = true;
        } else if (strncmp(arg, "cfg=", 4) == 0) {
            cfgPath = strdup(arg + 4);
        } else if (strncmp(arg, "out=", 4) == 0) {
            outputDir = strdup(arg + 4);
        } else if (strncmp(arg, "procIdx=", 8) == 0) {
            argProcIdx = (uint32_t)atoi(arg + 8);
        }
    }

    if (!cfgPath) {
        qemu_plugin_outs("ERROR: zsim_qemu plugin requires cfg=<path> argument\n");
        return -1;
    }

    /* Default output directory */
    if (!outputDir) {
        outputDir = strdup("/tmp/zsim_qemu");
    }

    /* Ensure output directory exists */
    char mkdirCmd[512];
    snprintf(mkdirCmd, sizeof(mkdirCmd), "mkdir -p %s", outputDir);
    (void)system(mkdirCmd);

    /* Initialize vcpu->tid mapping and internal-vCPU tracking */
    for (uint32_t i = 0; i < MAX_VCPUS; i++) {
        vcpuToTid[i] = UNINITIALIZED_CID;
        vcpu_is_internal[i] = false;
        in_zsim[i] = false;
    }
    /* 1.9.20: no tid has a known stable identity until getOrAssignTid records it */
    for (uint32_t i = 0; i < MAX_THREADS; i++) tidToVcpu[i] = UNINITIALIZED_CID;
    for (uint32_t i = 0; i < MAX_THREADS; i++) {
        cids[i] = UNINITIALIZED_CID;
    }

    /* Register vcpu init/exit BEFORE SimInit so that internal threads
     * (watchdog, contention sim) created by SimInit() get vcpu_init_cb
     * and are marked as internal vCPUs. */
    qemu_plugin_register_vcpu_init_cb(id, vcpu_init_cb);
    qemu_plugin_register_vcpu_exit_cb(id, vcpu_exit_cb);

    if (argProcIdx == 0) {
        /* Primary process: create shared memory and run SimInit */
        if (useSharedMem) {
            gm_attach(shmId);
        } else {
            int newShmId = gm_init(1 << 28);  /* 256 MB segment */
            (void)newShmId;
        }

        procIdx = 0;
        SimInit(cfgPath, outputDir, shmId);

        /* Write sentinel file so secondary processes know init is done */
        char sentinelPath[512];
        snprintf(sentinelPath, sizeof(sentinelPath), "%s/.zsim_shmid", outputDir);
        FILE* sf = fopen(sentinelPath, "w");
        if (sf) {
            fprintf(sf, "%d\n", gm_get_shmid());
            fclose(sf);
        }
    } else {
        /* Secondary process: wait for primary's sentinel, then attach */
        char sentinelPath[512];
        snprintf(sentinelPath, sizeof(sentinelPath), "%s/.zsim_shmid", outputDir);
        int attached_shmid = -1;
        for (int attempt = 0; attempt < 6000; attempt++) {  /* 60 sec max wait */
            FILE* sf = fopen(sentinelPath, "r");
            if (sf) {
                if (fscanf(sf, "%d", &attached_shmid) == 1 && attached_shmid >= 0) {
                    fclose(sf);
                    break;
                }
                fclose(sf);
            }
            usleep(10000);  /* 10ms poll */
        }
        if (attached_shmid < 0) {
            qemu_plugin_outs("ERROR: Secondary process timed out waiting for primary init\n");
            return -1;
        }

        gm_attach(attached_shmid);
        zinfo = static_cast<GlobSimInfo*>(gm_get_glob_ptr());
        procIdx = argProcIdx;
    }

    sim_initialized = true;  // mark init complete -- subsequent vCPUs are application threads

    lineBits = __builtin_ctz(zinfo->lineSize);
    procMask = ((uint64_t)procIdx) << 32;
    perProcessEndFlag = 0;

    /* Compute core-type masks for co-sim domain routing.
     *
     * Valid host cores:   OoO, Simple, Timing  (need cache hierarchy)
     * Invalid for host:   ALU (no cache), Null (no memory modeling)
     * Valid device cores:  ALL five types (ALU, OoO, Simple, Timing, Null)
     *
     * ALU and Null cores are always placed in the device mask.
     * All other core types (OoO, Simple, Timing) go to the host mask. */
    /* Detect any OOO core -> enable x86 decode of TBs into DynUops so the OOO
     * engine runs its real dataflow pipeline instead of the synthetic path. */
    g_ooo_decode_disabled = (getenv("PIMID_OOO_NODECODE") != nullptr);
    g_ooo_dump = (getenv("PIMID_OOO_DUMP") != nullptr);
    g_ooo_nobranch = (getenv("PIMID_OOO_NOBRANCH") != nullptr);
    g_inorder_nobranch = (getenv("PIMID_INORDER_NOBRANCH") != nullptr);
    for (uint32_t i = 0; i < zinfo->numCores; i++) {
        if (!zinfo->cores[i]) continue;
        if (zinfo->cores[i]->asOOOCore()) g_ooo_present = true;
        if (zinfo->cores[i]->asInOrderCore()) g_inorder_present = true;
    }
    /* 1.11.15 (audit): decode ALWAYS -- the instruction-class census
     * (1.11.10) needs it, and gating it on OOO/InOrder presence left the
     * census identically zero on alu/simple/null cores, i.e. on the default
     * device element and most of the corpus: the counted mix silently fell
     * back to fractions and the FP-without-FPU report could never fire.
     * ALU/simple-core timing is untouched ON DEFAULTS: they consume instrs
     * and bytes (identical in the decoded BblInfo), plus nFp for the 1.11.11
     * soft-float charge -- which was dead code while the census was zero, so
     * a config with pim.pe.floating_point=false AND fp_emulation_cycles>0
     * reports different (more honest) cycles from 1.11.15 on. With the
     * default fpEmulCycles=0 the charge stays inert and timing is
     * bit-identical. The cost is decode work at translation time, once per
     * basic block. PIMID_NODECODE=1 restores the old gating as an escape
     * hatch (value-checked: PIMID_NODECODE=0 means decode-always). */
    const char* nodecode_env = getenv("PIMID_NODECODE");
    /* 1.11.43 (audit E25, user ruling: make the in-order legacy path honest
     * WITH decode). PIMID_INORDER_NODECODE used to control TWO unrelated
     * things: the core's timing model (legacy IPC=1 vs decoded pipeline) AND
     * this decoder gate. The interlock made the legacy path's mix counting
     * and soft-float charge structurally inert -- every BblInfo arrived with
     * zero class counts, so mixAdd() added zeros and the 1.11.11 FP charge
     * (added to that path deliberately) never once executed with nonzero
     * input. The env now selects the TIMING PATH ONLY; classification stays
     * on, so the legacy path counts a real mix and the FP charge is live.
     * With fp_emulation_cycles = 0 (default) legacy timing remains
     * byte-identical: the mix counters do not feed timing.
     * The OOO escape is unchanged -- an OOO core cannot execute without
     * uops, so its decode kill-switch is genuinely about decode. */
    /* 1.11.44: the in-order NODECODE escape hatch is deleted (user ruling);
     * decode is unconditional for in-order. Only OOO retains a kill-switch,
     * because an OOO core genuinely cannot execute without uops and the
     * switch exists for decoder-fault triage, not as a timing mode. */
    (void)nodecode_env;
    g_decode_enabled = !(g_ooo_present && g_ooo_decode_disabled);
    if (g_ooo_present) {
        info("[ZSim] OOO core present: x86 decode -> DynUops enabled (real out-of-order path)%s",
             g_ooo_decode_disabled ? " [DISABLED via PIMID_OOO_NODECODE]" : "");
    }
    if (g_inorder_present) {
        info("[ZSim] In-order core present: x86 decode -> DynUops enabled (real in-order scoreboard)%s",
             "");
    }

    host_mask.resize(zinfo->numCores, false);
    device_mask.resize(zinfo->numCores, false);
    for (uint32_t i = 0; i < zinfo->numCores; i++) {
        if (dynamic_cast<ALUCore*>(zinfo->cores[i])) {
            device_mask[i] = true;
        } else if (dynamic_cast<NullCore*>(zinfo->cores[i])) {
            device_mask[i] = true;
        } else {
            /* OoO, Simple, Timing -- valid host cores */
            host_mask[i] = true;
        }
    }

    /* 1.9.20: ascending list of device PEs + per-PE ownership, for the
     * deterministic rank->PE homes assigned in deviceMaskFor(). */
    device_core_list.clear();
    for (uint32_t i = 0; i < zinfo->numCores; i++) {
        if (device_mask[i]) device_core_list.push_back(i);
    }
    device_home_owner.assign(device_core_list.size(), UNINITIALIZED_CID);
    device_home_mask.resize(MAX_THREADS);
    if (getenv("PIMID_NO_PE_PIN") && atoi(getenv("PIMID_NO_PE_PIN")) != 0) {
        g_pin_device_homes = false;
        info("[ZSim] 1.9.20 PE pinning DISABLED via PIMID_NO_PE_PIN "
             "(rank->PE placement reverts to scheduler order, i.e. run-to-run "
             "nondeterministic)");
    } else if (!device_core_list.empty()) {
        info("[ZSim] 1.9.20 PE pinning active: %lu device PEs, rank->PE home = "
             "vcpu_index %% %lu (deterministic placement)",
             (unsigned long)device_core_list.size(),
             (unsigned long)device_core_list.size());
    }

    bool has_device = false;
    for (auto b : device_mask) if (b) { has_device = true; break; }
    bool has_host = false;
    for (auto b : host_mask) if (b) { has_host = true; break; }

    if (!has_device) {
        /* Non-cosim mode -- no ALU/Null cores found.
         * Place all cores in host mask (unchanged behavior). */
        for (uint32_t i = 0; i < zinfo->numCores; i++) {
            host_mask[i] = true;
        }
    } else if (!has_host) {
        /* ALU/Null-only config (e.g. PUM/PNM exploration).
         * Promote device cores to host so QEMU can execute on them. */
        for (uint32_t i = 0; i < zinfo->numCores; i++) {
            if (device_mask[i]) host_mask[i] = true;
        }
        info("[ZSim] ALU/Null-only config: all device cores promoted to host for QEMU execution");
    }

    /* Initialize co-sim thread state */
    for (uint32_t i = 0; i < MAX_THREADS; i++) {
        thread_initialized[i] = false;
        thread_domain[i].store(DOMAIN_HOST);
    }

    /* Count host vs device cores for reporting */
    uint32_t nHostCores = 0, nDeviceCores = 0;
    for (uint32_t i = 0; i < zinfo->numCores; i++) {
        if (host_mask[i]) nHostCores++;
        if (device_mask[i]) nDeviceCores++;
    }

    /* Co-sim = a real host AND a real device are both present. Then the ROI
     * IS the offload region: any ordinary workload's kernel executes on the
     * device, its out-of-ROI code on the host. No special cosim workloads. */
    g_cosim_mode = (nHostCores > 0 && nDeviceCores > 0 && nDeviceCores < zinfo->numCores);
    if (g_cosim_mode) {
        info("[ZSim] Co-sim mode: ROI = offload region (host runs out-of-ROI code, device runs the kernel)");
    }

    /* Thread-based MPI rank emulation: resolved once, before any guest code
     * (and therefore any magic op) can run. See g_mpi_thread_mode decl. */
    {
        const char* mpi_ranks_env = getenv("PIMID_MPI_RANKS");
        g_mpi_thread_mode = (mpi_ranks_env && atoi(mpi_ranks_env) > 1 &&
                             !getenv("PIMID_MPI_RANK"));
        /* 1.11.19 (user decision D4): remember the rank count.
         * 1.11.62 (ruling 5b): the flush no longer slices by it -- each rank
         * walks the caches at its own arrival and charges the delta that walk
         * finds, so the count is now only the expected walker count in the
         * flush log (see chargeCoherenceFlush). */
        int nr = mpi_ranks_env ? atoi(mpi_ranks_env) : 1;
        g_mpi_rank_count = (nr > 0) ? (uint32_t)nr : 1u;
    }
    if (g_mpi_thread_mode) {
        info("[ZSim] MPI: emulated ranks in-process (deterministic serial weave)");
    }
    g_cosim_trace = (getenv("PIMID_COSIM_TRACE") != nullptr);
    if (g_cosim_trace) {
        info("[ZSim] Co-sim thread-lifecycle trace enabled (PIMID_COSIM_TRACE)");
    }
    g_cosim_no_offload = (getenv("PIMID_COSIM_NO_OFFLOAD") != nullptr);
    if (g_cosim_no_offload) {
        info("[ZSim] NO_OFFLOAD baseline: ROI/WORK markers are stat-only "
             "(no device migration, no transfer charged; host-only run)");
    }

    char msg[512];
    snprintf(msg, sizeof(msg),
             "ZSim+QEMU plugin loaded: %u cores (%u host, %u device), %u MHz, phase=%u\n"
             "  Config: %s\n  Output: %s\n",
             zinfo->numCores, nHostCores, nDeviceCores,
             zinfo->freqMHz, zinfo->phaseLength,
             cfgPath, outputDir);
    qemu_plugin_outs(msg);

    /* Register remaining QEMU callbacks (vcpu_init/exit already registered above) */
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans_cb);
    qemu_plugin_register_vcpu_syscall_cb(id, syscall_cb);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);

    return 0;
}

/* PIMID 1.11.90: component version stamp. The loader in src/main.cpp reads
 * the "@(#)PIMID_COMPONENT" string from this file and refuses a component
 * whose version differs from the binary's (see locatePimidComponent). */
#ifndef PIMID_VERSION
#error "PIMID_VERSION must be defined for the component version stamp"
#endif
extern "C" __attribute__((visibility("default"), used))
const char zsim_qemu_component_stamp[] = "@(#)PIMID_COMPONENT libzsim_qemu.so " PIMID_VERSION;
extern "C" __attribute__((visibility("default")))
const char* zsim_qemu_component_version(void) { return PIMID_VERSION; }
