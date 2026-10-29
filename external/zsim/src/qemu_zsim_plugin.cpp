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

#include "common/debug_checks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>
#include <sys/syscall.h>
#include <sys/mman.h>   // (1.12.107): MFD_CLOEXEC
#include <fcntl.h>      // (1.12.107): O_ACCMODE, O_CLOEXEC
#include <errno.h>
#include <linux/futex.h>   // (1.12.172): FUTEX_WAKE
#include <climits>
#include <dlfcn.h>     // (1.12.157): dlsym of QEMU's pid_g2h (patch 0003)
#include <sys/stat.h>   // (1.12.157): struct stat / statx answers given fixed host-identity fields
#include <map>
#include <atomic>
#include <unordered_map>
#include <map>   // 1.11.97: barrier-generation table
#include <mutex>
#include <deque>
#include <set>
#include <elf.h>      // 1.12.18: libgomp .dynsym (PIM runtime primitives)
#include <string>
#include <vector>

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
#include "dram_epoch_replay.h"   // 1.11.98 (ruling 4 (c))
#include "pe_memory_interface.h"  // 1.12.18 (ruling 2026-10-24): singleProcCut / cutRoiRel
#include "mem_ctrls.h"           // 1.12.18 (bug 7): SimpleMemory::flushWriteback
#include "ooo_core.h"     // CtrlFlowKind codes for the branch/indirect feed
#include "x86_decoder.h"  // minimal x86-64 decoder -> DynUops for the real OOO path

/* ---- Globals (equivalent to zsim.cpp globals) ---- */

GlobSimInfo* zinfo;
uint32_t procIdx;
uint32_t lineBits;
uint64_t procMask;
bool dqDataReadable = false;   // measured DQ data: set at init (user ruling 2026-10-25: measured termination duty)
/* (1.12.137, user ruling 2026-10-27): guest line reads for the measured data statistics through QEMU's guest page table
 * (dq_data_stats.h), on vCPU threads (QEMU's read needs the calling thread's vCPU); elsewhere process_vm_readv. */
static thread_local bool t_pimidVcpu = false;
static int pimidPluginGuestRead(uint64_t addr, uint8_t* dst, uint32_t n) {
    if (!t_pimidVcpu) return -1;
    static thread_local GByteArray* buf = nullptr;
    if (!buf) buf = g_byte_array_sized_new(256);
    if (!qemu_plugin_read_memory_vaddr(addr, buf, n) || buf->len != n) return 0;
    memcpy(dst, buf->data, n);
    return 1;
}
int (*pimidGuestReadHook)(uint64_t, uint8_t*, uint32_t) = nullptr;
Core* cores[MAX_THREADS];

/* 1.9.0 thread-MPI epoch cut: per-core ROI state (0=ACTIVE, 1=QUIESCENT parked in
 * MPI, 2=EXITED, 3=VACATED by a co-sim migration, 4=WAITING: QUIESCENT and
 * blocked on a peer, i.e. a receive waiting for its message or a barrier
 * waiting for its last arrival; see EndOfPhaseActions). Marked at DETERMINISTIC sim
 * events (roi_end / thread-fini / MPI comm brackets / host-device migration),
 * NOT by a wall-dependent advancement heuristic. Read by
 * EndOfPhaseActions to build the consistent cut = min roiRel over ACTIVE cores.
 * Static zero-init => all ACTIVE at start.
 * QUIESCENT, WAITING and VACATED are re-armed to ACTIVE when the core runs again (MPI
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
 * decode runs for EVERY core type (the class census needs it), and alu/null
 * (1.12.1: SimpleCore retired) consume instrs/bytes/census -- their nFp feeds the 1.11.11 soft-float
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
static const uint64_t kFutexWaitSynthPc = 0x2000 + 202;   // (1.12.76): futex WAIT-class ops (see syscall_cb)
/* (1.12.76): a new thread's domain is read by its parent at the clone (the guest's
 * own order), not by the new thread when QEMU first runs it. */
static int g_cloneDomPending[MAX_VCPUS];
static std::mutex g_cloneMu;
static std::map<int, int> g_cloneDom;   // child OS tid -> domain read at the parent's clone
/* (1.12.172): each thread's exit-clear word (CLONE_CHILD_CLEARTID at its clone, or its set_tid_address), by OS tid */
static std::map<int, uint64_t> g_clearTidByHost;
static uint64_t g_cloneClearPending[MAX_VCPUS];
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
static void rtRoiSnapshot();   // 1.12.18: PIM runtime primitives, defined below
static void rtPrintStats();
static bool threadMpiMode();   // 1.11.92: g_mpi_thread_mode, which is declared below
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
#define ZSIM_MAGIC_OP_MPI_TRANSPORT_RANGE 2060   /* 1.12.17 (M1): %rdx -> {base, bytes} of transport memory */
#define ZSIM_MAGIC_OP_SPM_QUERY     2061  /* 1.12.18: %rdx -> pimid_spm_msg_t; returns capacity, staging mode */
#define ZSIM_MAGIC_OP_SPM_BIND      2062  /* 1.12.18: {base, bytes, cmd} of this thread's scratchpad RAM and DMA command block; {0,0} releases */
#define ZSIM_MAGIC_OP_SPM_DMA       2063  /* 1.12.18: {dst, src, bytes, dir}: one DMA block transfer */
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
/* A thread-MPI rank inside a comm bracket is about to block on a peer (a
 * receive whose message has not been posted, a barrier whose last rank has not
 * arrived): its core becomes WAITING (state 4) until COMM_END. */
#define ZSIM_MAGIC_OP_MPI_WAIT_PEER  2068

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

/* 1.11.97 (gate 1207B, MPI barrier rendezvous): the GENERATION of the ROI
 * clock baseline. Every snapshotRoiBaseCyc() moves roiRelCycles() to a new
 * origin; a barrier release published on the previous origin must not be
 * charged on the new one (see the MPI_ADVANCE handler). */
static std::atomic<uint64_t> g_roiBaseGen{0};
/* Per barrier generation k (the k-th MPI_TIME of every rank is the same
 * barrier: the shim issues MPI_TIME only in MPI_Barrier's thread-mode path),
 * the baseline generation in force at its FIRST arrival; and each rank's own
 * count of MPI_TIME calls, i.e. the barrier generation it is in. */
static std::map<uint64_t, uint64_t> g_barFirstBaseGen;
static lock_t g_barGenLock = 0;
static uint64_t g_rankBarGen[MAX_THREADS] = {0};

/* 1.12.18 (bug B3): coherence-flush writebacks issued before the thread-MPI
 * ROI baseline existed (see chargeCoherenceFlush), sent when it is taken. */
static std::mutex g_heldFlushMutex;
struct HeldFlushLine { uint64_t line; uint32_t src; uint64_t offset; };   // 1.12.18: offset into the flush
static std::vector<HeldFlushLine> g_heldFlush;
static void flushHeldWritebacks() {
    std::vector<HeldFlushLine> h;
    { std::lock_guard<std::mutex> g(g_heldFlushMutex); h.swap(g_heldFlush); }
    if (h.empty() || !zinfo->flushHostMems || zinfo->flushHostMems->empty()) return;
    const g_vector<SimpleMemory*>& hm = *zinfo->flushHostMems;
    const uint64_t n = hm.size();
    for (const auto& e : h) {
        const uint32_t src = e.src;
        const uint64_t cyc = (src < zinfo->numCores && zinfo->cores[src]) ? zinfo->cores[src]->getCycles() : 0;
        hm[e.line % n]->flushWriteback(e.line / n, cyc + e.offset, src);   // 1.12.18: paced over the flush
    }
    info("[roi] %lu coherence-flush writeback(s) of the kernel-entry flush that preceded the ROI baseline sent to the host memory controller at the baseline (inside the ROI) [1.12.18 B3]",
         (unsigned long)h.size());
}

/* Snapshot every core's ROI baseline cycle (call right after markRoiBegin). */
static inline void snapshotRoiBaseCyc() {
    g_roiBaseGen.fetch_add(1);   // 1.11.97: a new origin for roiRelCycles()
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
    zinfo->hierarchy.mpiRoiBaseCyc = mpi_roi_base_cyc;   // 1.12.18 (R2, B3): host cores' ROI origin
    zinfo->hierarchy.mpiRoiBaseCycN = MAX_THREADS;
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
    rtRoiSnapshot();   // 1.12.18: the PIM runtime statistics window opens with the traffic counters'
    if (zinfo && zinfo->dramReplay) zinfo->dramReplay->markRoi();   // 1.11.98 (gate 1208A): the replay's statistics window opens with the traffic counters'
    if (zinfo && zinfo->hostDramReplay) zinfo->hostDramReplay->markRoi();   // 1.12.18 (ruling 17 / R0605): the host memory's replay, same window
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
    /* 1.11.92 (F11): replay the pre-ROI records still waiting for a drain
     * BEFORE the counters are zeroed, so they are dropped with the rest of
     * the pre-ROI traffic instead of being replayed -- and counted -- by the
     * first ROI drain. Not under thread-MPI: nothing is recorded there
     * before the ROI baseline, which is set just before this call, so what
     * is pending is ROI traffic. */
    uint64_t pre_replayed = 0;
    if (zinfo->garnetNetwork && !threadMpiMode())
        pre_replayed = zinfo->garnetNetwork->drainPendingRecords(
            false, zinfo->phaseLength, zinfo->numPhases);
    uint64_t pre_flits = 0, pre_pkts = 0;
    if (zinfo->garnetNetwork) {
        pre_flits += zinfo->garnetNetwork->markRoiBegin();
        pre_pkts  += zinfo->garnetNetwork->roiDroppedPackets();
    }
    if (zinfo->systemGarnetNetwork) {
        pre_flits += zinfo->systemGarnetNetwork->markRoiBegin();
        pre_pkts  += zinfo->systemGarnetNetwork->roiDroppedPackets();
    }
    info("[roi] traffic counters rebased at roi_begin (%s): %u stat groups, "
         "pre-ROI memory-controller accesses dropped: %lu, pre-ROI NoC flits dropped: %lu "
         "(%lu packets; %lu pending pre-ROI records replayed first)",
         where, groups, (unsigned long)pre_mem, (unsigned long)pre_flits,
         (unsigned long)pre_pkts, (unsigned long)pre_replayed);
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
/* 1.12.18 (ruling 2026-10-24): single-process run priced with the thread-MPI
 * cut mechanism (PEMemoryInterface::singleProcCut); set at install. */
static bool g_spCut = false;
static bool threadMpiMode() { return g_mpi_thread_mode; }
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
/* (1.12.170): co-simulation thread-MPI ranks that have entered the device window. Rank 0's roi_end closes the window
 * (g_roiClosing) but a rank that has not yet run its kernel still enters it, at its next block or barrier; the
 * window drains only when every rank has entered and left. Until then a rank whose turn came after rank 0's
 * roi_end ran its kernel on its host core (its own scratchpad DMA then had no device PE to run on). */
static bool g_rankEntered[MAX_THREADS] = {false};
static std::atomic<uint32_t> g_ranksEnteredN{0};
static inline void markRankEntered(uint32_t tid) {
    if (tid < MAX_THREADS && !g_rankEntered[tid]) { g_rankEntered[tid] = true; g_ranksEnteredN.fetch_add(1); }
}

/* 1.11.97 (x01-qemu-zsim-plugin-5, ruled): the DEVICE-scope thread-MPI ROI
 * ends at the LAST rank's roi_end, not the opener's.
 *
 * What it did before: every MPI kernel calls zsim_roi_begin/zsim_roi_end on
 * rank 0 only (benchmarks/pim_kernels/x/x_mpi.c, benchmarks/host/x/
 * x_message_passing.c), so g_roiRefCount is 1 and, outside co-sim, the
 * opener's roi_end froze the stats and requested termination on the spot
 * while ranks 1..N-1 were still in their compute (the serial weave keeps
 * them within about a phase of rank 0, and unbalanced kernels -- bfs,
 * spmv_csr -- much further). Co-sim with offload already waited: its
 * window drains on g_deviceWorkers at the closing barrier (above).
 *
 * The workloads do not mark ranks 1..N-1's ends; the closing MPI_Barrier
 * that follows rank 0's roi_end in 12 of the 14 MPI kernels is where every
 * rank's kernel ends in its own program order. In thread mode the barrier
 * first issues MPI_TIME at the rank's ARRIVAL (before it parks in the
 * frozen-clock wait), so arrivals are counted there: when the N-th arrival
 * of a barrier generation lands and the opener's roi_end has already
 * happened (it precedes the opener's own arrival in program order, so it
 * cannot be missed), every rank has finished its kernel and every core
 * still shows its own arrival clock. That instant is the end of the device
 * ROI: the span is the last rank's end, and no rank's cycles include the
 * barrier wait. A barrier generation cannot start counting before the
 * previous one completes (a rank re-enters only after the release), so the
 * counter is reset at each N-th arrival.
 *
 * Kernels with no barrier after roi_end (bfs_mpi.c, bfs_message_passing.c)
 * never complete a closing generation; they fall back to the guest-exit dump
 * (plugin_exit), the same documented fallback the co-sim window uses, and
 * their stats then include the post-ROI tail. Ranks between their MPI_TIME
 * op and their COMM_BEGIN park at the freeze retire a few transport
 * instructions inside the window (mutex lock), a wall-order residue of tens
 * of instructions. */
static std::atomic<bool>     g_devRoiClosing{false};
static std::atomic<uint32_t> g_devBarArrivals{0};
static std::atomic<bool>     g_devRoiFrozen{false};
static inline bool devScopeLastRankRule() {
    return g_mpi_thread_mode && !g_cosim_mode && g_mpi_rank_count > 1;
}

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

/* 1.12.18 (user ruling 2026-10-23, "run time = elapsed wall clock"): the
 * ROI's elapsed time on the global clock (zinfo->roiWall, see zsim.h). A
 * stamp is the phase clock plus the stamping core's offset in its phase --
 * PEMemoryInterface::phaseStamp, the axis the DRAM replay records on -- so
 * a serial run's elapsed time equals its core's cycles exactly and a
 * blocked shared-memory thread's wait is inside it. */
static inline uint64_t roiWallStamp(uint32_t tid) {
    const uint64_t pl = zinfo->phaseLength > 0 ? zinfo->phaseLength : 10000;
    uint64_t off = 0;
    if (tid < MAX_THREADS && thread_initialized[tid]) {
        uint32_t cid = cids[tid];
        if (cid < zinfo->numCores && zinfo->cores[cid]) off = zinfo->cores[cid]->getCycles() % pl;
    }
    return (uint64_t)zinfo->numPhases * pl + off;
}
/* 1.12.18 (user ruling 2026-10-23, "sync to waker"): a release stamp per
 * futex word. FUTEX_WAKE-class ops stamp the word(s) with the waker's clock
 * at the syscall; a FUTEX_WAIT-class op that returns 0 (woken) syncs the
 * waiter's clock to the stamp of the word it waited on, at its next Join.
 * Only device PEs stamp and sync (Core::wakeStampable): their clocks are
 * their own timelines; the host cores re-base to the global phase clock on
 * join already. 1.12.21 (user ruling 2026-10-24): the in-order / OOO cores
 * stamp and sync as well, in their own table (clkStampPut below): a waiter
 * that rejoins inside the phase of its wake still resumes no earlier than
 * the release. A requeued waiter (FUTEX_CMP_REQUEUE / WAKE_OP) syncs to
 * the requeue/wake-op stamp on its original word -- a lower bound on the
 * release it was finally woken by. futex_waitv (449) is not tracked (its
 * words live in a guest array); none of the shipped kernels' runtimes use
 * it under the guest glibc (the diagnosis saw only nr 202). Thread-MPI
 * (g_mpi_thread_mode) is left out: its ranks' waits are modelled by the
 * transport (frozen-clock waits, the rendezvous re-places the clock from
 * deterministic arithmetic, roiWall source 2 = the ranks' own clocks); a
 * futex-driven jump there would make rank time wall-dependent. */
static const uint32_t kWakeSlots = 8192;   // power of two
struct WakeStampSlot { uint64_t addr; uint64_t cyc; uint64_t org; };   // 1.12.18: org = the waker's ROI origin (single-process cut)
static WakeStampSlot g_wakeStamp[kWakeSlots];
static std::mutex g_wakeMu;
static uint64_t g_waitWord[MAX_THREADS];          // armed FUTEX_WAIT word, 0 = none
static uint64_t g_wakeSyncTo[MAX_THREADS];        // pending sync target, 0 = none
static uint64_t g_wakeSyncOrg[MAX_THREADS];       // 1.12.18: the ROI origin of that target's clock
static std::atomic<uint64_t> g_wakeSyncN{0}, g_wakeSyncSkip{0}, g_wakeStampN{0}, g_wakeMissN{0};
static uint64_t g_roiWakeSyncN = 0, g_roiWakeSyncSkip = 0, g_roiWakeStampN = 0, g_roiWakeMissN = 0;
static inline uint32_t wakeSlotOf(uint64_t a) { return (uint32_t)((a >> 2) * 0x9E3779B97F4A7C15ull >> 51) & (kWakeSlots - 1); }
static void wakeStampPut(uint64_t addr, uint64_t cyc, uint64_t org = 0) {
    if (!addr) return;
    std::lock_guard<std::mutex> g(g_wakeMu);
    WakeStampSlot& s = g_wakeStamp[wakeSlotOf(addr)];
    s.addr = addr; s.cyc = cyc; s.org = org;   // the latest release on this word (a collision evicts: no sync, never a wrong one)
    g_wakeStampN.fetch_add(1, std::memory_order_relaxed);
}
/* 1.12.21 (user ruling 2026-10-24): the in-order / OOO cores (lock clock
 * domain 1: host cores, and device PEs of those models) stamp and sync too,
 * in their OWN table (their raw clock is on the global phase axis, not
 * comparable with an ALU PE's own timeline; a separate table also leaves the
 * ALU PEs' stamps and collisions exactly as they were). rel = the waker's
 * ROI-relative clock at the release (single-process cut), the origin the
 * waiter takes with the clock. */
struct ClkStampSlot { uint64_t addr; uint64_t cyc; uint64_t rel; };
static ClkStampSlot g_clkStamp[kWakeSlots];
static uint32_t g_waitDom[MAX_THREADS];           // the waiter's clock domain (2 = ALU PE table, 1 = clk table)
static uint64_t g_clkSyncTo[MAX_THREADS], g_clkSyncRel[MAX_THREADS];
static void clkStampPut(uint64_t addr, uint64_t cyc, uint64_t rel) {
    if (!addr) return;
    std::lock_guard<std::mutex> g(g_wakeMu);
    ClkStampSlot& s = g_clkStamp[wakeSlotOf(addr)];
    s.addr = addr; s.cyc = cyc; s.rel = rel;
    g_wakeStampN.fetch_add(1, std::memory_order_relaxed);
}
static bool clkStampGet(uint64_t addr, uint64_t& cyc, uint64_t& rel) {
    std::lock_guard<std::mutex> g(g_wakeMu);
    const ClkStampSlot& s = g_clkStamp[wakeSlotOf(addr)];
    if (s.addr != addr) return false;
    cyc = s.cyc; rel = s.rel; return true;
}
static inline uint64_t clkRelOf(uint32_t cid, uint64_t raw) {   // the ROI-relative clock a domain-1 release carries
    return (g_spCut && zinfo->hierarchy.mpiNocBaselined) ? PEMemoryInterface::cutRoiRel(cid, raw) : 0;
}
static bool wakeStampGet(uint64_t addr, uint64_t& cyc, uint64_t* org = nullptr) {
    std::lock_guard<std::mutex> g(g_wakeMu);
    const WakeStampSlot& s = g_wakeStamp[wakeSlotOf(addr)];
    if (s.addr != addr) return false;
    cyc = s.cyc; if (org) *org = s.org; return true;
}
/* futex op classes (cmd = op & 0x7f: FUTEX_PRIVATE_FLAG 128 and
 * FUTEX_CLOCK_REALTIME 256 masked off) */
static void wakeSyncOnFutexEntry(uint32_t tid, uint32_t cid, uint64_t uaddr, uint64_t op, uint64_t uaddr2) {
    if (g_mpi_thread_mode) return;   // the transport models thread-MPI waits (see above)
    /* 1.12.18 (ruling 2026-10-24): a single-process thread entering a futex
     * WAIT is SLEEPING: its core is QUIESCENT (exempt from the consistent cut)
     * until the thread joins again (Join). */
    if (g_spCut && cid < MAX_THREADS) {
        const uint32_t c7 = (uint32_t)(op & 0x7f);
        if (c7 == 0 || c7 == 9 || c7 == 6 || c7 == 11 || c7 == 13) {
            uint32_t a = 0;
            g_coreRoiState[cid].compare_exchange_strong(a, 1);
        }
    }
    if (tid < MAX_THREADS && cid < zinfo->numCores && zinfo->cores[cid] && !zinfo->cores[cid]->wakeStampable() &&
        zinfo->cores[cid]->lockClockDomain() == 1) {   // 1.12.21: in-order / OOO core
        Core* co = zinfo->cores[cid];
        const uint64_t raw = co->lockClock();
        switch ((uint32_t)(op & 0x7f)) {
            case 1: case 10: case 7:
                clkStampPut(uaddr, raw, clkRelOf(cid, raw)); break;
            case 3: case 4: case 5: case 12:
                clkStampPut(uaddr, raw, clkRelOf(cid, raw));
                clkStampPut(uaddr2, raw, clkRelOf(cid, raw)); break;
            case 0: case 9: case 6: case 11: case 13:
                g_waitWord[tid] = uaddr; g_waitDom[tid] = 1; break;
            default: break;
        }
        return;
    }
    if (tid >= MAX_THREADS || cid >= zinfo->numCores || !zinfo->cores[cid] || !zinfo->cores[cid]->wakeStampable()) return;
    if (tid < MAX_THREADS) g_waitDom[tid] = 2;   // 1.12.19
    const uint32_t cmd = (uint32_t)(op & 0x7f);
    switch (cmd) {
        case 1: case 10: case 7:          // WAKE, WAKE_BITSET, UNLOCK_PI
            wakeStampPut(uaddr, zinfo->cores[cid]->getCycles(), PEMemoryInterface::spOriginOf(cid)); break;
        case 3: case 4: case 5: case 12:  // REQUEUE, CMP_REQUEUE, WAKE_OP, CMP_REQUEUE_PI
            wakeStampPut(uaddr, zinfo->cores[cid]->getCycles(), PEMemoryInterface::spOriginOf(cid));
            wakeStampPut(uaddr2, zinfo->cores[cid]->getCycles(), PEMemoryInterface::spOriginOf(cid)); break;
        case 0: case 9: case 6: case 11: case 13:   // WAIT, WAIT_BITSET, LOCK_PI, WAIT_REQUEUE_PI, LOCK_PI2
            g_waitWord[tid] = uaddr; break;
        default: break;
    }
}
static void wakeSyncOnFutexReturn(uint32_t tid, int64_t ret) {
    if (tid >= MAX_THREADS) return;
    const uint64_t w = g_waitWord[tid];
    if (!w) return;
    g_waitWord[tid] = 0;
    if (ret != 0) return;   // EAGAIN/ETIMEDOUT/EINTR: not woken, nothing to sync to
    if (g_waitDom[tid] == 1) {   // 1.12.21: an in-order / OOO waiter
        uint64_t c = 0, rel = 0;
        if (clkStampGet(w, c, rel)) { if (c > g_clkSyncTo[tid]) { g_clkSyncTo[tid] = c; g_clkSyncRel[tid] = rel; } }
        else g_wakeMissN.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint64_t c = 0, org = 0;
    if (wakeStampGet(w, c, &org)) { if (c > g_wakeSyncTo[tid]) { g_wakeSyncTo[tid] = c; g_wakeSyncOrg[tid] = org; } }
    else g_wakeMissN.fetch_add(1, std::memory_order_relaxed);
}
static void wakeSyncApply(uint32_t tid, uint32_t cid) {
    if (tid < MAX_THREADS && g_clkSyncTo[tid]) {   // 1.12.21: an in-order / OOO waiter takes its waker's clock and origin
        const uint64_t t = g_clkSyncTo[tid], rel = g_clkSyncRel[tid];
        g_clkSyncTo[tid] = 0;
        if (cid < zinfo->numCores && zinfo->cores[cid] && !zinfo->cores[cid]->wakeStampable() && zinfo->cores[cid]->lockClockDomain() == 1) {
            const uint64_t sk = zinfo->cores[cid]->syncToWaker(t);
            if (sk) { g_wakeSyncN.fetch_add(1, std::memory_order_relaxed); g_wakeSyncSkip.fetch_add(sk, std::memory_order_relaxed); }
            if (sk && g_spCut && zinfo->hierarchy.mpiNocBaselined) PEMemoryInterface::clkAdoptRel(cid, rel);
        }
    }
    if (tid >= MAX_THREADS || !g_wakeSyncTo[tid]) return;
    const uint64_t t = g_wakeSyncTo[tid];
    g_wakeSyncTo[tid] = 0;
    if (cid >= zinfo->numCores || !zinfo->cores[cid] || !zinfo->cores[cid]->wakeStampable()) return;
    const uint64_t sk = zinfo->cores[cid]->syncToWaker(t);
    if (sk) { g_wakeSyncN.fetch_add(1, std::memory_order_relaxed); g_wakeSyncSkip.fetch_add(sk, std::memory_order_relaxed); }
    if (sk && g_spCut && zinfo->hierarchy.mpiNocBaselined) PEMemoryInterface::spSetOrigin(cid, g_wakeSyncOrg[tid]);   // 1.12.18: the clock is the waker's, so is its origin
}
/* 1.12.18 (user ruling 2026-10-23, "model the lock word"): lock handoffs.
 * Sync to waker (above) carries a release only through a futex wake; on a
 * lightly loaded host a contended lock is taken by spinning (CAS/XCHG in user
 * space) without any futex sleep, no handoff was seen, and critical sections
 * overlapped in simulated time (host-load dependent run times). The lock word
 * is now modelled. Atomic read-modify-writes are found at translation time
 * (isAtomicRmw: a LOCK prefix, or XCHG with a memory operand) and tagged
 * (lock_rmw_tag); their mem callbacks deliver the value read and the value
 * written (QEMU reports both halves of an atomic, R then W). Per word (a
 * small hash table holding only words an atomic RMW has CHANGED):
 *   acquire: an atomic RMW that changes the word from its free value while
 *     no thread holds it -> holder := this thread; if the last release was
 *     by ANOTHER thread on the same clock domain (Core::lockClockDomain) and
 *     wrote the value this RMW read, the acquirer's clock := max(own,
 *     release time) (Core::syncToLock: re-based like syncToWaker). The free
 *     value is learned from the first changing RMW (its old value).
 *   release: the holder writes the free value back, by an atomic RMW (XCHG,
 *     LOCK DEC/ADD, CMPXCHG) or a plain store (a spin unlock 'movl $1') ->
 *     stamp (thread, domain, clock, value); holder := none.
 *   anything else (contention marks like libgomp's -1, counter updates, a
 *     failed CAS) changes nothing. An atomic counter therefore never syncs:
 *     its first update makes the first updater the holder and it never
 *     returns to the free value by the holder. A thread re-taking its own
 *     lock never syncs (serial runs are untouched).
 * Conservative and cheap: a table miss or collision (evict) loses a sync,
 * never makes a wrong one; a plain store costs one relaxed load (g_lockKey
 * filter) unless an atomic RMW has touched a word with that address. If an
 * acquirer's callback runs before its releaser's (the two atomics raced on
 * the host) the acquire is taken without a sync and counted (races).
 * Thread-MPI is left out (as for sync to waker): its ranks' clocks are placed
 * by the transport from deterministic arithmetic (frozen-clock waits,
 * rendezvous), and the MPI shim's own atomics are host-schedule driven, so a
 * lock-word jump there would make rank time wall-dependent. Switch:
 * simulation.lock_sync (PIMID_LOCK_SYNC), default on. */
static bool g_lockSync = true;
static int lock_rmw_tag;   // mem-cb userdata of an atomic read-modify-write instruction
static const uint32_t kLockSlots = 4096;   // power of two
static const uint32_t kNoHolder = ~0u;
struct LockWord { uint64_t addr, freeVal, relVal, relClk, relOrg, relRel; uint32_t holder, relTid, relDom; bool relValid; };   // 1.12.21: relRel = a domain-1 releaser's ROI-relative clock   // 1.12.18: relOrg = the releaser's ROI origin
static LockWord g_lockWord[kLockSlots];
static std::atomic<uint64_t> g_lockKey[kLockSlots];   // lock-free filter for the plain-store path
static std::atomic<bool> g_lockAny{false};
static std::mutex g_lockMu;
static uint64_t g_rmwOld[MAX_THREADS];   // the R half of this thread's atomic RMW
static bool g_rmwHaveOld[MAX_THREADS];
static std::atomic<uint64_t> g_lockSyncN{0}, g_lockSyncSkip{0}, g_lockRelN{0}, g_lockAcqN{0}, g_lockRaceN{0};
static uint64_t g_roiLockSyncN = 0, g_roiLockSyncSkip = 0, g_roiLockRelN = 0, g_roiLockAcqN = 0, g_roiLockRaceN = 0;
static inline uint32_t lockSlotOf(uint64_t a) { return (uint32_t)((a >> 2) * 0x9E3779B97F4A7C15ull >> 52) & (kLockSlots - 1); }
static bool isAtomicRmw(const uint8_t* b, size_t n) {
    bool lock = false;
    size_t i = 0;
    for (; i < n; i++) {
        const uint8_t c = b[i];
        if (c == 0xf0) { lock = true; continue; }
        if (c == 0x66 || c == 0x67 || c == 0xf2 || c == 0xf3 || c == 0x26 || c == 0x2e ||
            c == 0x36 || c == 0x3e || c == 0x64 || c == 0x65) continue;
        break;
    }
    if (lock) return true;   // LOCK is only legal on a memory-destination RMW
    if (i < n && b[i] >= 0x40 && b[i] <= 0x4f) i++;   // REX
    return (i + 1 < n) && (b[i] == 0x86 || b[i] == 0x87) && ((b[i + 1] >> 6) != 3);   // XCHG r, m (always locked)
}
static inline uint64_t lockMemVal(qemu_plugin_meminfo_t info) {
    const qemu_plugin_mem_value v = qemu_plugin_mem_get_value(info);
    switch (v.type) {
        case QEMU_PLUGIN_MEM_VALUE_U8:  return v.data.u8;
        case QEMU_PLUGIN_MEM_VALUE_U16: return v.data.u16;
        case QEMU_PLUGIN_MEM_VALUE_U32: return v.data.u32;
        case QEMU_PLUGIN_MEM_VALUE_U64: return v.data.u64;
        default: return v.data.u128.low ^ (v.data.u128.high * 0x9E3779B97F4A7C15ull);   // CMPXCHG16B: equality only
    }
}
static inline Core* lockCoreOf(uint32_t tid) {
    if (tid >= MAX_THREADS || !thread_initialized[tid]) return nullptr;
    const uint32_t cid = cids[tid];
    if (cid >= zinfo->numCores || !zinfo->cores[cid] || !zinfo->cores[cid]->lockClockDomain()) return nullptr;
    return zinfo->cores[cid];
}
static void lockSyncApply(Core* c, uint64_t t, uint32_t cid = ~0u, uint64_t org = 0, uint64_t rel = 0) {
    const uint64_t sk = c->syncToLock(t);
    if (sk) { g_lockSyncN.fetch_add(1, std::memory_order_relaxed); g_lockSyncSkip.fetch_add(sk, std::memory_order_relaxed); }
    if (sk && g_spCut && cid != ~0u && zinfo->hierarchy.mpiNocBaselined) {
        if (c->lockClockDomain() == 1) PEMemoryInterface::clkAdoptRel(cid, rel);   // 1.12.21: an in-order / OOO acquirer
        else PEMemoryInterface::spSetOrigin(cid, org);   // 1.12.18: the releaser's origin
    }
}
/* the R and W halves of a tagged atomic RMW (after the access is timed) */
static void lockRmw(uint32_t tid, uint64_t addr, qemu_plugin_meminfo_t info) {
    if (!qemu_plugin_mem_is_store(info)) { g_rmwOld[tid] = lockMemVal(info); g_rmwHaveOld[tid] = true; return; }
    if (!g_rmwHaveOld[tid]) return;
    g_rmwHaveOld[tid] = false;
    const uint64_t oldv = g_rmwOld[tid], newv = lockMemVal(info);
    if (newv == oldv) return;   // no change (a fence 'lock or $0', a failed CAS on the locked value)
    Core* c = lockCoreOf(tid);
    if (!c) return;
    const uint32_t dom = c->lockClockDomain();
    uint64_t syncTo = 0, syncOrg = 0, syncRel = 0;
    {
        std::lock_guard<std::mutex> g(g_lockMu);
        const uint32_t s = lockSlotOf(addr);
        LockWord& w = g_lockWord[s];
        if (w.addr != addr) {   // first change seen (or a collision: evict) -- an acquire from oldv, nothing to sync to
            w.addr = addr; w.freeVal = oldv; w.holder = tid; w.relValid = false;
            g_lockKey[s].store(addr, std::memory_order_relaxed);
            g_lockAny.store(true, std::memory_order_relaxed);
            g_lockAcqN.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (w.holder == tid && newv == w.freeVal) {   // release by the holder
            w.relValid = true; w.relTid = tid; w.relDom = dom; w.relClk = c->lockClock(); w.relVal = newv;
            w.relOrg = PEMemoryInterface::spOriginOf(cids[tid]);
            w.relRel = (dom == 1) ? clkRelOf(cids[tid], w.relClk) : 0;   // 1.12.19
            w.holder = kNoHolder;
            g_lockRelN.fetch_add(1, std::memory_order_relaxed);
        } else if (oldv == w.freeVal && w.holder != tid) {   // acquire: took the word from its free value
            if (w.holder != kNoHolder) g_lockRaceN.fetch_add(1, std::memory_order_relaxed);   // its release callback is late
            else if (w.relValid && w.relTid != tid && w.relDom == dom && oldv == w.relVal) { syncTo = w.relClk; syncOrg = w.relOrg; syncRel = w.relRel; }
            w.holder = tid;
            g_lockAcqN.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (syncTo) lockSyncApply(c, syncTo, cids[tid], syncOrg, syncRel);
}
/* a plain store: a release if it writes the free value of a word this thread holds */
static void lockStore(uint32_t tid, uint64_t addr, qemu_plugin_meminfo_t info) {
    const uint32_t s = lockSlotOf(addr);
    if (g_lockKey[s].load(std::memory_order_relaxed) != addr) return;   // the cheap filter
    Core* c = lockCoreOf(tid);
    if (!c) return;
    const uint64_t v = lockMemVal(info);
    std::lock_guard<std::mutex> g(g_lockMu);
    LockWord& w = g_lockWord[s];
    if (w.addr != addr || w.holder != tid || v != w.freeVal) return;
    w.relValid = true; w.relTid = tid; w.relDom = c->lockClockDomain(); w.relClk = c->lockClock(); w.relVal = v;
    w.relOrg = PEMemoryInterface::spOriginOf(cids[tid]);   // 1.12.18
    w.relRel = (w.relDom == 1) ? clkRelOf(cids[tid], w.relClk) : 0;   // 1.12.19
    w.holder = kNoHolder;
    g_lockRelN.fetch_add(1, std::memory_order_relaxed);
}

/* the ROI's elapsed time on the roi_begin core's own timeline (diagnostic
 * beside the phase-clock stamps: with the clocks synced to their wakers this
 * is the critical path the wakes carried) */
static uint32_t g_roiTlCid = ~0u;
static uint64_t g_roiTlBegin = 0;

/* (1.12.171): a link serves one flit of its width per cycle, so the ROI cannot be shorter than the service the
 * busiest tree link has had to give the traffic it carried (the network counts it per up-link, at the width the
 * power model prices the link at). The run time closes at that service time when the PEs' own ends lie below it:
 * the link was the bottleneck, and the link duty the report prints stays within 1. The records of the phase not
 * yet drained are included (read only). Network cycles are device cycles. */
static uint64_t roiWallLinkFloor(uint64_t cur, const char* what) {
    uint64_t fl = 0;
    if (zinfo->garnetNetwork) fl = std::max(fl, zinfo->garnetNetwork->linkBusyMaxCyclesProjected());
    if (zinfo->systemGarnetNetwork) fl = std::max(fl, zinfo->systemGarnetNetwork->linkBusyMaxCyclesProjected());
    if (fl > cur) {
        info("[roi] run time raised from %lu to %lu cycles: the busiest tree link needs that long to carry the ROI's traffic (%s)",
             (unsigned long)cur, (unsigned long)fl, what);
        return fl;
    }
    return cur;
}

static void roiWallBegin(uint32_t tid) {   // non-thread-MPI roi_begin (re-opens on a repeat)
    if (tid < MAX_THREADS && thread_initialized[tid] && cids[tid] < zinfo->numCores && zinfo->cores[cids[tid]]) {
        g_roiTlCid = cids[tid]; g_roiTlBegin = zinfo->cores[cids[tid]]->getCycles();
    } else g_roiTlCid = ~0u;
    g_roiWakeSyncN = g_wakeSyncN.load(); g_roiWakeSyncSkip = g_wakeSyncSkip.load();
    g_roiWakeStampN = g_wakeStampN.load(); g_roiWakeMissN = g_wakeMissN.load();
    g_roiLockSyncN = g_lockSyncN.load(); g_roiLockSyncSkip = g_lockSyncSkip.load();   // 1.12.18 (model the lock word)
    g_roiLockRelN = g_lockRelN.load(); g_roiLockAcqN = g_lockAcqN.load(); g_roiLockRaceN = g_lockRaceN.load();
    zinfo->roiWall.ended = false;
    zinfo->roiWall.endStamp = 0;
    zinfo->roiWall.beginStamp = roiWallStamp(tid);
    zinfo->roiWall.source = 1;
}
static void roiWallEnd(uint32_t tid, const char* how) {
    if (zinfo->roiWall.source != 1 || zinfo->roiWall.ended) return;
    const uint64_t e = (tid < MAX_THREADS) ? roiWallStamp(tid)
                     : (uint64_t)zinfo->numPhases * (zinfo->phaseLength > 0 ? zinfo->phaseLength : 10000);
    zinfo->roiWall.endStamp = (e > zinfo->roiWall.beginStamp) ? e : zinfo->roiWall.beginStamp;
    if (!g_spCut) zinfo->roiWall.endStamp = zinfo->roiWall.beginStamp + roiWallLinkFloor(zinfo->roiWall.endStamp - zinfo->roiWall.beginStamp, how);   // (1.12.171)
    zinfo->roiWall.ended = true;
    info("[roi] run time: ROI elapsed %lu cycles on the global phase clock (stamps %lu -> %lu, %s) [1.12.18, ruling 2026-10-23]",
         (unsigned long)zinfo->roiWall.cycles(), (unsigned long)zinfo->roiWall.beginStamp,
         (unsigned long)zinfo->roiWall.endStamp, how);
    {   /* 1.12.18 (user ruling 2026-10-23, "sync to waker"): the wake syncs of the ROI */
        uint64_t tl = 0; bool tlOk = false;
        if (tid < MAX_THREADS && thread_initialized[tid] && cids[tid] == g_roiTlCid && g_roiTlCid < zinfo->numCores) {
            const uint64_t e = zinfo->cores[g_roiTlCid]->getCycles();
            tl = (e > g_roiTlBegin) ? e - g_roiTlBegin : 0; tlOk = true;
        }
        info("[wake-sync] ROI: %lu futex wakes synced a PE clock to its waker (%lu cycles skipped), %lu release stamps, %lu woken waits without a stamp; roi_begin core's own timeline elapsed %s%lu cycles [1.12.18, ruling 2026-10-23]",
             (unsigned long)(g_wakeSyncN.load() - g_roiWakeSyncN), (unsigned long)(g_wakeSyncSkip.load() - g_roiWakeSyncSkip),
             (unsigned long)(g_wakeStampN.load() - g_roiWakeStampN), (unsigned long)(g_wakeMissN.load() - g_roiWakeMissN),
             tlOk ? "" : "(n/a: roi_end on another core) ", (unsigned long)tl);
        /* 1.12.18 (user ruling 2026-10-23, "model the lock word") */
        info("[lock-sync] ROI: %lu lock handoffs synced a core clock to the releaser (%lu cycles skipped), %lu releases, %lu acquires, %lu acquire/release races (%s) [1.12.18, ruling 2026-10-23]",
             (unsigned long)(g_lockSyncN.load() - g_roiLockSyncN), (unsigned long)(g_lockSyncSkip.load() - g_roiLockSyncSkip),
             (unsigned long)(g_lockRelN.load() - g_roiLockRelN), (unsigned long)(g_lockAcqN.load() - g_roiLockAcqN),
             (unsigned long)(g_lockRaceN.load() - g_roiLockRaceN), g_lockSync ? "simulation.lock_sync on" : "off");
    }
    /* 1.12.18 (user ruling 2026-10-24 (a)): a single-process cut run's run time
     * is the latest device PE's ROI end on its own clock -- the axis its NoC
     * and DRAM records carry (PEMemoryInterface::cutRoiRel; co-sim: the device
     * axis from the host doorbell) -- the same rule as thread-MPI source 2.
     * The phase-clock stamps above stay printed (diagnostic). A run where no
     * device PE ran (a no-offload baseline) takes its host cores, on the host
     * clock. */
    if (g_spCut) {
        uint64_t mx = 0, n = 0; uint32_t endCore = ~0u;
        for (int pass = 0; pass < 2 && n == 0; pass++) {
            for (uint32_t c = 0; c < zinfo->numCores; c++) {
                Core* co = zinfo->cores[c];
                if (!co || (pass == 0) != co->wakeStampable()) continue;
                /* 1.12.18 (W9): a core that had not run at the ROI baseline
                 * (snapshot 0: OpenMP workers spawned in the ROI) has origin 0
                 * and is counted; it used to be skipped while its records
                 * were stamped (baseline_DDR5_omp_gemv_16core: 1 of 16 host
                 * cores read, 676 ROI records outside). A core that ran
                 * nothing in the ROI reads 0 below. */
                if (pass == 1 && c >= MAX_THREADS) continue;
                uint64_t v = PEMemoryInterface::cutRoiRel(c, co->pimidRawCycle());   // 1.12.18: the raw clock, as a request carries it (cutRoiRel moves it to the wall-free clock)
                if (pass == 1 && c < PEMemoryInterface::kSpOrigins)   // 1.12.21: or its latest request, if that lies later
                    v = std::max<uint64_t>(v, PEMemoryInterface::clkLastRec()[c].load(std::memory_order_relaxed));
                if (v == 0) continue;   // ran nothing in the ROI
                if (endCore == ~0u || v > mx) { mx = v; endCore = c; }
                n++;
            }
        }
        uint32_t f = 0;
        if (endCore != ~0u && !zinfo->cores[endCore]->wakeStampable())
            for (uint32_t nd = 0; nd < zinfo->nodeMap.numNodes && nd < 16; nd++)
                if (endCore >= zinfo->nodeMap.nodes[nd].coreStart && endCore < zinfo->nodeMap.nodes[nd].coreEnd) f = zinfo->nodeMap.nodes[nd].freqMHz;
        zinfo->roiWall.mpiEndFreqMHz = f;   // 0 = the device PEs' (the replay's own) clock
        if (f == 0) mx = roiWallLinkFloor(mx, how);   // (1.12.171)
        zinfo->roiWall.mpiEnd = mx;
        zinfo->roiWall.source = 3;
        info("[roi] run time: ROI elapsed %lu cycles = the latest %s ROI end on its own clock (%lu core(s), core %d; single-process cut, %s) [1.12.18, ruling 2026-10-24 (a)]",
             (unsigned long)mx, (f == 0) ? "device PE's" : "host core's", (unsigned long)n, (int)endCore, how);
    }
}
/* Thread-MPI: the max over ranks of the ROI-relative end. Co-sim with
 * offload: each rank's closing-barrier marker (the kernel window the
 * 1.12.3 trim prices); otherwise every device core's clock at the cut. */
static void roiWallCloseMpi(const char* how) {
    if (zinfo->roiWall.ended) return;
    uint64_t mx = 0, n = 0;
    bool fromMarkers = false;
    if (g_cosim_mode && !g_cosim_no_offload) {
        for (uint32_t t = 0; t < MAX_THREADS; t++) {
            if (!g_tailMarked[t]) continue;
            uint32_t c = g_tailDeviceCid[t];
            if (c >= zinfo->numCores || !zinfo->cores[c]) continue;
            uint64_t b = zinfo->cores[c]->getRoiBaseCycle();
            uint64_t v = (g_tailMarker[t] > b) ? g_tailMarker[t] - b : 0;
            if (v > mx) mx = v;
            n++;
        }
        fromMarkers = (n > 0);
    }
    /* 1.12.18 (user ruling R2 2026-10-24): the ranks run on whichever cores
     * the run gives them -- the device PEs, or, in a no-offload baseline, the
     * HOST cores. The run time is the same rule on either: the first ROI
     * barrier (the baseline snapshot) to the latest rank's ROI end. A host
     * core keeps no roiBaseCycle of its own; its origin is the snapshot's
     * clock (mpi_roi_base_cyc). The device PEs of a no-offload baseline never
     * run (0 cycles) and were the only cores read here: run time 0. */
    const bool hostRanks = g_cosim_mode && g_cosim_no_offload;
    uint32_t endCore = ~0u;
    if (!fromMarkers) {
        for (uint32_t c = 0; c < zinfo->numCores; c++) {
            Core* co = zinfo->cores[c];
            if (!co) continue;
            const bool isHost = co->asInOrderCore() || co->asOOOCore();
            if (g_cosim_mode && isHost != hostRanks) continue;   // the rank cores only
            uint64_t cy = co->getCycles(), b = PEMemoryInterface::roiOriginOf(c);   // 1.12.18 (W9): an ooo/in-order PE's origin
            if (hostRanks) {
                if (c >= MAX_THREADS || mpi_roi_base_cyc[c] == 0) continue;   // never baselined: ran no rank
                b = mpi_roi_base_cyc[c];
            }
            uint64_t v = (cy > b) ? cy - b : 0;
            if (v > mx || endCore == ~0u) { mx = std::max(mx, v); endCore = c; }
            n++;
        }
    }
    {   /* 1.12.18 (R2): the clock mpiEnd is counted in (the DRAM replay converts it) */
        uint32_t f = 0;
        const uint32_t probe = endCore;
        for (uint32_t nd = 0; probe != ~0u && nd < zinfo->nodeMap.numNodes && nd < 16; nd++)
            if (probe >= zinfo->nodeMap.nodes[nd].coreStart && probe < zinfo->nodeMap.nodes[nd].coreEnd) f = zinfo->nodeMap.nodes[nd].freqMHz;
        zinfo->roiWall.mpiEndFreqMHz = hostRanks ? f : 0;   // device PEs: the replay's own clock
    }
    if (!hostRanks && fromMarkers) {   // (1.12.171): traffic after the kernel window is outside what is priced
        if (zinfo->garnetNetwork) zinfo->garnetNetwork->setRoiEnd(mx);
        if (zinfo->systemGarnetNetwork) zinfo->systemGarnetNetwork->setRoiEnd(mx);
    }
    if (!hostRanks) mx = roiWallLinkFloor(mx, how);   // (1.12.171)
    zinfo->roiWall.mpiEnd = mx;
    zinfo->roiWall.source = 2;
    zinfo->roiWall.ended = true;
    info("[roi] run time: thread-MPI ROI elapsed %lu cycles = max over %lu rank core(s) of the ROI-relative end (%s, %s%s) [1.12.18, ruling 2026-10-23]",
         (unsigned long)mx, (unsigned long)n, fromMarkers ? "closing-barrier markers" : "core clocks at the cut", how,
         hostRanks ? "; no-offload baseline: the HOST cores running the ranks, from the first ROI barrier (ruling R2 2026-10-24)" : "");
}
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

/* 1.11.94 (H11 x01-qemu-zsim-plugin-3): true between a non-thread-MPI co-sim
 * ROI_BEGIN offload (cosimRoiBeginOffload) and its ROI_END. A thread's domain
 * was fixed at birth from g_in_device_region, and ROI_BEGIN moved only the
 * caller, so OpenMP pool workers created by a parallel region BEFORE
 * zsim_roi_begin() (babelstream init, NPB cg/mg/ft, streamcluster) kept the
 * host mask and ran the offloaded kernel on host cores. While this flag is
 * set, every thread of the process that is still DOMAIN_HOST migrates itself
 * to the device domain at its next BBL (insn_exec_cb). The migration has to
 * be performed by the thread itself: sched->leave/finish detach the CALLING
 * thread's core, and another thread may be mid-BBL on that core, so the
 * opener cannot detach its peers. Scoped to the ROI window (not WORK
 * regions), matching the ruling. */
static std::atomic<bool> g_roi_offload_window{false};

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
 * 1.12.16: the vendored qemu-plugin.h is QEMU 11.1.2's (plugin API 7), which declares the register API and
 * includes <glib.h>; the local glib / register shim this file carried for the API-2 header is gone. The names
 * below alias the real types so the code that follows is unchanged. */
typedef GArray PimidGArray;
typedef GByteArray PimidGByteArray;
typedef qemu_plugin_reg_descriptor pimid_qemu_plugin_reg_descriptor;

#define PIMID_GARRAY_INDEX(a, t, i) \
    (((t*)(void*)(a)->data)[(i)])

static std::atomic<struct qemu_plugin_register*> rcx_handle{nullptr};
static std::atomic<struct qemu_plugin_register*> rdx_handle{nullptr};
static std::atomic<struct qemu_plugin_register*> rsp_handle{nullptr};
static std::atomic<struct qemu_plugin_register*> rdi_handle{nullptr};   /* 1.12.18: omp_set_num_threads(n) */   /* 1.12.17: main stack at roi_begin */
static void stackAtRoiBegin(uint32_t tid);   /* 1.12.17: defined with the stack registry below */
static void stackAtRoiBeginReg();
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
            } else if (d->name && strcmp(d->name, "rsp") == 0) {
                rsp_handle.store(d->handle, std::memory_order_release);
            } else if (d->name && strcmp(d->name, "rdi") == 0) {
                rdi_handle.store(d->handle, std::memory_order_release);
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
    if (!qemu_plugin_read_register(h, rcx_buf)) return false;  /* API 7: bool; the size is the buffer length */
    int n = (int)rcx_buf->len;
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

/* (1.12.164) co-simulation: a device kernel's scratchpad opened before the offload.
 * A kernel opens its scratchpad (benchmarks/include/pimid_spm.h) once per thread BEFORE its ROI, and in a
 * co-simulation everything before the ROI runs on the HOST core: the host has no scratchpad, so the query
 * answered 0 and the kernel ran its DRAM path on the device, while a standalone device run of the same
 * program binds the PE's scratchpad (HBM3 histogram, 8 PEs: 206612 vs 37255 PE cycles). The query of a
 * host-side thread in an offloading co-simulation is now answered by the device's PE (the PE the thread
 * will run on has the same scratchpad), the bind is kept, and it is applied to the device PE the thread
 * joins after its migrate-in, so the offloaded kernel runs the code path the standalone run runs. */
struct SpmPending { uint64_t lo, bytes, cmd; };
static SpmPending g_spmPend[MAX_THREADS];
static bool g_spmPendSet[MAX_THREADS];
static Core* g_spmPendOn[MAX_THREADS];
static std::atomic<bool> g_spmPendAny{false};
static Core* spmDeviceTemplate();
static void spmApplyPending(uint32_t tid);

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

/* ---- The DRAM replay cut's wake floor (thread-MPI) ----
 * A WAITING rank (state 4) resumes no earlier than the event that releases it:
 * a message already posted to it (no earlier than its sender's clock at the
 * send), a barrier already released (its latest arrival), or an action a rank
 * that is not waiting has yet to take (no earlier than that rank's clock now).
 * These tables hold the posted ones, on the cut's ROI-relative axis: one entry
 * per message from its SEND to its RECV (per source/destination pair, in
 * order), one per released barrier until its last rank has advanced. An entry
 * made before the last ROI re-baseline is on an older origin; while one is
 * outstanding the floor is 0 (the cut holds at every waiting rank, as before). */
static std::mutex g_cutWakeMu;
struct CutWakeEntry { uint64_t t; uint64_t gen; };
static std::map<std::pair<uint32_t, uint32_t>, std::deque<CutWakeEntry>> g_cutWakeMsgs;
struct CutWakeBar { uint32_t arr = 0, adv = 0; uint64_t maxArr = 0, gen = 0; };
static std::map<uint64_t, CutWakeBar> g_cutWakeBars;
static std::multiset<uint64_t> g_cutWakeFloor;   // the posted entries of the current origin
static uint64_t g_cutWakeGen = 0, g_cutWakeStale = 0;
static inline uint64_t cutAxisClock(uint32_t c) {   // the cut's ROI-relative clock of core c (EndOfPhaseActions)
    Core* co = (c < zinfo->numCores) ? zinfo->cores[c] : nullptr;
    if (!co) return 0;
    const uint64_t base = g_cosim_mode ? co->getRoiBaseCycle() : PEMemoryInterface::roiOriginOf(c);
    const uint64_t cyc = co->getCycles();
    return (cyc > base) ? cyc - base : 0;
}
static void cutWakeRebase() {   // under g_cutWakeMu: entries of an older origin become stale
    const uint64_t g = g_roiBaseGen.load();
    if (g == g_cutWakeGen) return;
    g_cutWakeStale += g_cutWakeFloor.size();
    g_cutWakeFloor.clear();
    g_cutWakeGen = g;
}
static void cutWakeDrop(const CutWakeEntry& e) {   // under g_cutWakeMu
    if (e.gen != g_cutWakeGen) { if (g_cutWakeStale) g_cutWakeStale--; return; }
    auto it = g_cutWakeFloor.find(e.t);
    if (it != g_cutWakeFloor.end()) g_cutWakeFloor.erase(it);
}
static void cutWakeMessage(uint32_t tid, bool recv, uint32_t src, uint32_t dst) {
    std::lock_guard<std::mutex> g(g_cutWakeMu);
    cutWakeRebase();
    auto& q = g_cutWakeMsgs[std::make_pair(src, dst)];
    if (!recv) {
        const uint64_t t = (tid < MAX_THREADS && cids[tid] < MAX_THREADS) ? cutAxisClock(cids[tid]) : 0;
        q.push_back(CutWakeEntry{t, g_cutWakeGen});
        g_cutWakeFloor.insert(t);
    } else if (!q.empty()) {
        cutWakeDrop(q.front());
        q.pop_front();
    }
}
static void cutWakeBarrier(uint32_t tid, uint64_t k, bool arrival) {
    std::lock_guard<std::mutex> g(g_cutWakeMu);
    cutWakeRebase();
    CutWakeBar& b = g_cutWakeBars[k];
    if (arrival) {
        const uint64_t t = (tid < MAX_THREADS && cids[tid] < MAX_THREADS) ? cutAxisClock(cids[tid]) : 0;
        if (b.arr == 0) b.gen = g_cutWakeGen;
        if (t > b.maxArr) b.maxArr = t;
        if (++b.arr == g_mpi_rank_count) {   // released
            if (b.gen == g_cutWakeGen) g_cutWakeFloor.insert(b.maxArr); else g_cutWakeStale++;
        }
    } else if (++b.adv >= g_mpi_rank_count) {   // its last rank has advanced
        if (b.arr >= g_mpi_rank_count) cutWakeDrop(CutWakeEntry{b.maxArr, b.gen});
        g_cutWakeBars.erase(k);
    }
}
static uint64_t cutWakeFloor() {   // the earliest posted wake; ~0 when none
    std::lock_guard<std::mutex> g(g_cutWakeMu);
    cutWakeRebase();
    if (g_cutWakeStale) return 0;
    return g_cutWakeFloor.empty() ? ~0ull : *g_cutWakeFloor.begin();
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
    /* 1.12.17: the same cut also folds the device DRAM epoch replay under
     * message passing (DramEpochReplay cut mode). */
    /* 1.12.18 (ruling 2026-10-24): single-process runs (g_spCut) fold at the
     * same consistent cut after the ROI baseline, on cutRoiRel. */
    const bool foldNoc = (g_mpi_thread_mode || g_spCut) && zinfo->garnetNetwork &&
                         zinfo->garnetNetwork->isCycleAccurate();
    const bool foldDram = (g_mpi_thread_mode || g_spCut) && zinfo->dramReplay &&
                          zinfo->hierarchy.dramEpochReplay && zinfo->dramReplay->cutMode();
    /* 1.12.18 (ruling 17 / R0605): the decoupled host memory's replay folds at
     * the same cut (a host sharing the device's instance folds with it). */
    const bool foldHostDram = (g_mpi_thread_mode || g_spCut) && zinfo->hostDramReplay && zinfo->hostDramReplay->cutMode();
    uint64_t spNocCut = 0; bool spNocFolded = false;   // 1.12.18: for the epoch publish below
    if ((foldNoc || foldDram || foldHostDram) && zinfo->hierarchy.mpiNocBaselined) {
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
        /* 1.12.17: the DRAM replay cut also holds at a QUIESCENT core (parked in
         * an MPI wait). Its clock is frozen there, and the accesses it issues when
         * it resumes are stamped from that point on; exempting it, as the NoC cut
         * does, let the cut run past it, and its later records then belonged to
         * epochs already replayed (smoke t1217a: 4.17 M of 8.09 M stencil records,
         * which then issued as one burst). Only EXITED and VACATED cores are
         * exempt here, so no record can arrive behind this cut. */
        /* (1.12.176): a WAITING core (blocked on a peer) does not hold the DRAM cut at
         * its own clock but at the earliest clock it can resume: no earlier than
         * W = min(the cores that are not waiting, the posted wakes; see
         * cutWakeFloor). So the cut no longer stops for the whole wait of a rank
         * that finished its part early (an MPI reduce's root, a barrier's first
         * arrival), which left the running ranks' records unfolded and their
         * accesses priced by the bootstrap. min over waiting q of max(rr_q, W) is
         * max(min rr_q, W). With no posted wake and no running core the cut holds
         * at the waiting cores, as before. */
        uint64_t cutDram = ~0ull, minWait = ~0ull;
        int dbgCutCore = -1, dbgActive = 0;
        for (uint32_t c = 0; c < zinfo->numCores && c < MAX_THREADS; c++) {
            Core* co = zinfo->cores[c];
            if (!co) continue;
            const uint32_t st = g_coreRoiState[c].load(std::memory_order_acquire);
            if (st != 0 && st != 1 && st != 4) continue;   // EXITED / VACATED: exempt from both cuts
            /* 1.12.18 (W9): device scope (every core a PE): an ooo/in-order PE's
             * ROI origin (PEMemoryInterface::roiOriginOf), the origin its records
             * are stamped against; co-sim keeps the host cores out of the cut
             * as before (roiBaseCycle 0: their clock never pins it). */
            uint64_t base = g_cosim_mode ? co->getRoiBaseCycle() : PEMemoryInterface::roiOriginOf(c);
            uint64_t cyc  = co->getCycles();
            uint64_t rr   = (cyc > base) ? cyc - base : 0;
            if (g_spCut) rr = PEMemoryInterface::cutRoiRel(c, co->pimidRawCycle());   // 1.12.18: the records' axis (W9: the raw clock a request carries)
            if (rr == 0) continue;       // baselined but no ROI traffic yet
            /* 1.12.18 (ruling 2026-10-24): a SLEEPING single-process core (futex
             * wait, QUIESCENT) is exempt from BOTH cuts: it resumes at its waker's
             * clock (sync to waker), not at its own frozen one, so holding the DRAM
             * cut there would stall the replay for the whole sleep. */
            if (g_spCut && st != 0) continue;
            if (st == 4) { if (rr < minWait) minWait = rr; continue; }   // WAITING: bounded below, exempt from the NoC cut
            if (rr < cutDram) cutDram = rr;
            if (st != 0) continue;       // QUIESCENT: exempt from the NoC cut only
            if (rr < cut) { cut = rr; dbgCutCore = (int)c; }
            dbgActive++;
        }
        uint64_t wakeW = ~0ull;
        if (minWait != ~0ull) {
            wakeW = std::min(cutDram, cutWakeFloor());
            cutDram = std::min(cutDram, (wakeW == ~0ull) ? minWait : std::max(minWait, wakeW));
        }
        {   // PIMID_FOLD_DEBUG=1: which core pins the cut, and is it advancing?
            static const bool foldDbg = (getenv("PIMID_FOLD_DEBUG") != nullptr);
            static uint64_t dbgCalls = 0; static uint64_t lastCut = 0; static int lastCore = -1;
            if (foldDbg && ((dbgCalls++ % 100) == 0)) {
                info("[CutDbg] phase=%lu cut=%lu pinnedBy=core%d active=%d cutAdvance=%ld cutDram=%lu minWait=%lu wakeFloor=%lu",
                     (unsigned long)zinfo->numPhases, (unsigned long)cut, dbgCutCore,
                     dbgActive, (long)((cut == ~0ull || lastCut == 0) ? 0 : (int64_t)cut - (int64_t)lastCut),
                     (unsigned long)cutDram, (unsigned long)minWait, (unsigned long)wakeW);
                lastCut = (cut == ~0ull) ? lastCut : cut; lastCore = dbgCutCore;
            }
        }
        if (foldNoc && g_spCut && !zinfo->garnetNetwork->cutAxis()) {   // 1.12.18
            zinfo->garnetNetwork->enterCutAxis();
        }
        if (foldNoc) zinfo->garnetNetwork->foldByCut(cut, zinfo->phaseLength);
        if (foldNoc) { spNocCut = cut; spNocFolded = true; }
        if (foldDram) {   // the epoch length the access hook keys on (PEMemoryInterface::localAccessLatency)
            const uint32_t E = DramEpochReplay::epochPhases();   // (1.12.145, ruling 2026-10-28)
            const uint32_t pl = zinfo->phaseLength > 0 ? zinfo->phaseLength : 10000;
            zinfo->dramReplay->foldByCut(cutDram, (uint64_t)pl * E);
        }
        if (foldHostDram) {   // 1.12.18: as above, the host interfaces' epoch length is the same
            const uint32_t E = DramEpochReplay::epochPhases();   // (1.12.145, ruling 2026-10-28)
            const uint32_t pl = zinfo->phaseLength > 0 ? zinfo->phaseLength : 10000;
            zinfo->hostDramReplay->foldByCut(cutDram, (uint64_t)pl * E);
        }
    }

    /* 1.12.18 (user ruling 2026-10-24, "OMP NoC: epoch-frozen"): single-process
     * runs (no thread-MPI, no shared detailed-MPI NoC) fold the phase that just
     * ended HERE, single-threaded, and publish each completed epoch's measured
     * pair latencies; accesses read that table, which changes only here. */
    {
        static const bool sharedNocRun = (getenv("PIMID_NOC_SHM") != nullptr);
        GarnetNetwork* gn = zinfo->garnetNetwork;
        if (!g_mpi_thread_mode && !sharedNocRun && gn && gn->isCycleAccurate()) {   // single process (g_spCut)
            if (!gn->epochFrozenPublish()) {
                gn->setEpochFrozenPublish(true);
                info("[EpochFeedback] shared-memory run: NoC accesses priced from the epoch-frozen measured pair latencies "
                     "(folded at the phase barrier; an access in epoch k reads epoch k-1; E_phases=%u x %u cycles) [1.12.18, ruling 2026-10-24]",
                     gn->detEpochPhases(), (unsigned)zinfo->phaseLength);
            }
            const uint32_t pl = zinfo->phaseLength > 0 ? zinfo->phaseLength : 10000;
            if (!g_spCut || !gn->cutAxis()) {   // before the ROI baseline: the phase grid
                gn->foldByCut(((uint64_t)zinfo->numPhases + 1) * pl, pl);
                gn->publishCompletedEpochs(zinfo->numPhases);
            } else if (spNocFolded) {   // 1.12.18 (full mechanism): epochs wholly below the consistent cut
                if (spNocCut == ~0ull) gn->publishCompletedEpochs(~0ull - 1);
                else if (spNocCut >= pl) gn->publishCompletedEpochs(spNocCut / pl - 1);
            }
        }
    }

    CheckForTermination();
    zinfo->contentionSim->simulatePhase(zinfo->globPhaseCycles + zinfo->phaseLength);

    zinfo->eventQueue->tick();
    zinfo->profSimTime->transition(PROF_BOUND);
}

/* ---- Barrier (phase synchronization) ---- */

bool pimidPostRoiFree();   // 1.12.18: defined with g_termStatsDumped below

/* (1.12.123) fixed thread order: a thread that leaves its core for a futex wait
 * stays pending until the kernel has queued it on the futex word (or the call
 * has returned), so the next turn cannot touch the word before the kernel
 * compares it. The thread's state is read from /proc (sleeping, in that call,
 * on that address). */
/* (1.12.161): the fixed order holds for the whole simulated run -- from the program's start until the
 * statistics are written -- not only inside the ROI: after a co-sim window closes, the ranks' post-ROI
 * collective (reported as the protocol tail) and the replay's last records still ran in the host's order */
static inline bool detActive();
static std::atomic<uint8_t> g_detSleepWatch[MAX_THREADS];
static int g_detSleepHtid[MAX_THREADS];
static uint64_t g_detSleepNr[MAX_THREADS], g_detSleepArg[MAX_THREADS];
static std::atomic<uint32_t> g_detSleepers{0};
static std::atomic<uint32_t> g_detSleepTop{0};
static bool detReadProc(int htid, const char* what, char* b, size_t sz) {
    char p[64];
    snprintf(p, sizeof p, "/proc/self/task/%d/%s", htid, what);
    const int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const ssize_t n = read(fd, b, sz - 1);
    close(fd);
    if (n <= 0) return false;
    b[n] = 0;
    return true;
}
static bool detAsleep(uint32_t t) {
    char b[512];
    if (!detReadProc(g_detSleepHtid[t], "stat", b, sizeof b)) return false;
    const char* r = strrchr(b, ')');
    if (!r || r[1] != ' ' || r[2] != 'S') return false;
    if (!detReadProc(g_detSleepHtid[t], "syscall", b, sizeof b)) return false;
    char* e = nullptr;
    const unsigned long nr = strtoul(b, &e, 10);
    if (e == b || nr != g_detSleepNr[t]) return false;
    return strtoull(e, nullptr, 16) == g_detSleepArg[t];
}
static void detSettle(int64_t own = 0) {   // wait until every thread this turn woke, created, moved or put to sleep is settled
    /* own: the caller's own re-join, counted already (it cannot settle before the caller leaves) */
    for (int i = 0; g_detPending.load() > own; i++) {
        if (g_detSleepers.load() > 0) {
            const uint32_t top = std::min<uint32_t>(g_detSleepTop.load(), MAX_THREADS);
            for (uint32_t t = 0; t < top; t++)
                if (g_detSleepWatch[t].load() && detAsleep(t) && g_detSleepWatch[t].exchange(0)) {
                    g_detSleepers.fetch_sub(1);
                    if (g_detCredit[t].exchange(0)) g_detPending.fetch_sub(1);
                }
            if (g_detPending.load() <= own) break;
        }
        if (!detActive()) break;   // the statistics are written: nothing is ordered any more
        usleep(5);
        if (i == 4000000) {
            warn("[zsim] fixed thread order: %ld joins did not register in 20 s; continuing", (long)g_detPending.load());
            for (uint32_t t = 0; t < MAX_THREADS; t++) if (g_detSleepWatch[t].exchange(0)) g_detSleepers.fetch_sub(1);
            g_detPending.store(0);
        }
    }
    if (g_detPending.load() <= 0 && zinfo && zinfo->sched) zinfo->sched->detDispatch();   // (1.12.158): the queued rejoins get their cores, in queue order
}

/* (1.12.123) fixed thread order: a turn lasts at most kDetTurnCycles of the core's own
 * clock (then the core with the earliest clock runs), not the rest of the phase:
 * a thread no longer runs up to a whole phase ahead of the threads it woke */
static const uint64_t kDetTurnCycles = 1000;
static uint64_t g_detQ = 0;   // kDetTurnCycles once the fixed order is on
static uint64_t g_detTurnEnd[MAX_THREADS];
static inline void detTurnBegin(uint32_t tid, uint32_t cid) {
    if (g_detQ && tid < MAX_THREADS && cid < zinfo->numCores && zinfo->cores[cid])
        g_detTurnEnd[tid] = zinfo->cores[cid]->lockClock() + g_detQ;
}

uint32_t TakeBarrier(uint32_t tid, uint32_t cid) {
    if (pimidPostRoiFree()) {   // 1.12.18: after the ROI, leave and run free
        zinfo->sched->leave(procIdx, tid, cid);
        clearCid(tid);
        fPtrs[tid] = nopPtrs;
        return INVALID_CID;
    }
    if (g_detOrder && detActive()) detSettle();   // (1.12.123): every thread this turn woke, created, moved or put to sleep is settled
    uint32_t newCid = zinfo->sched->sync(procIdx, tid, cid);
    clearCid(tid);
    setCid(tid, newCid);
    detTurnBegin(tid, newCid);

    if (zinfo->terminationConditionMet && pimidPostRoiFree()) {
        zinfo->sched->leave(procIdx, tid, newCid);
        clearCid(tid);
        fPtrs[tid] = nopPtrs;
        return INVALID_CID;
    }
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
        g_mpiRoiCyc[dcid] = g_tailMarker[tid];   // 1.12.3 (ruling 1 (a)): the kernel-window receipt = the marker
        info("Protocol tail: rank %u PE(cid %u) post-ROI inter-PE collective tail %lu cyc "
             "(device-resident); kernel window %lu cyc", tid, dcid, (unsigned long)tail,
             (unsigned long)g_tailMarker[tid]);
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
    if (!pimid::debugChecks()) return;   // internal diagnostic lines
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
static inline bool detActive() { return !g_termStatsDumped; }   // (1.12.161): see the fixed-order section
/* 1.12.18: after a non-MPI ROI end the stats are written at once and the
 * program runs to its own exit, unsimulated (g_postRoiFree): a thread that
 * reaches a phase barrier or rejoins leaves the scheduler instead of ending
 * the process, and the watchdog waits for the exit. Until now the watchdog
 * (or the first thread at a barrier) ended the process within microseconds
 * of the ROI end, racing the program's own output: shared-memory runs lost
 * their BENCH_CHECKSUM line now and then. A program that has not exited
 * kPostRoiWaitSec after the dump is ended as before. */
static volatile uint32_t g_postRoiFree = 0;
static volatile uint64_t g_postRoiSince = 0;
static const uint64_t kPostRoiWaitSec = 600;
static uint64_t postRoiWallSec() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec;
}
bool pimidPostRoiFree() {
    return g_postRoiFree && g_termStatsDumped && postRoiWallSec() < g_postRoiSince + kPostRoiWaitSec;
}
static void dumpTerminationStats() {
    if (!__sync_bool_compare_and_swap(&g_termStatsDumped, 0, 1)) return;

    recordProtocolTailStats();   // 1.8.7: fill the protocolTail receipt BEFORE the dump
    /* 1.12.18 (ruling 2026-10-23): the run time is closed before the stats
     * and the DRAM replay's window read it. */
    if (g_mpi_thread_mode) roiWallCloseMpi("termination dump");
    else roiWallEnd(MAX_THREADS, "no roi_end before the stats write: closed at the dump's phase");
    info("Dumping termination stats");
    rtPrintStats();   // 1.12.18: PIM runtime primitives, per core
    zinfo->trigger = 20000;
    for (StatsBackend* backend : *(zinfo->statsBackends)) {
        if (zinfo->dramReplay) zinfo->dramReplay->finalize();   // 1.11.98 (ruling 4 (c)): replay the tail, fold the controller totals
        if (zinfo->hostDramReplay) zinfo->hostDramReplay->finalize();   // 1.12.18 (ruling 17 / R0605)
        backend->dump(false);
    }

    dumpGapHistograms();   // 1.11.40 (audit E17): measurement only

    if (zinfo->garnetNetwork) {
        /* 1.11.92 (F11): the records of the phase after the last drain were
         * never replayed, so the final phase was missing from every count.
         * Replay what is pending (thread-MPI: every bucket still held back
         * by the cut) before the stats are written. */
        uint64_t tail = zinfo->garnetNetwork->drainPendingRecords(
            threadMpiMode(), zinfo->phaseLength, zinfo->numPhases + 1);
        if (tail)
            info("[roi] %lu pending NoC records replayed before the final stats write",
                 (unsigned long)tail);
        if (g_spCut && zinfo->garnetNetwork->cutAxis()) {   // 1.12.18 (ruling 2026-10-24)
            uint64_t late = 0, lateEp = 0, stale = 0, reads = 0;
            zinfo->garnetNetwork->frozenCounters(late, lateEp, stale, reads);
            info("[EpochFeedback] single-process cut: %lu NoC record(s) behind a cut already taken, %lu packet(s) joined a later epoch, "
                 "%lu of %lu keyed read(s) took an epoch before the lag [1.12.18, ruling 2026-10-24]",
                 (unsigned long)late, (unsigned long)lateEp, (unsigned long)stale, (unsigned long)reads);
        }
        zinfo->garnetNetwork->setTotalCycles(zinfo->globPhaseCycles);
        std::string garnetStatsPath = std::string(zinfo->outputDir) + "/garnet_stats.txt";
        zinfo->garnetNetwork->writeStatsFile(garnetStatsPath.c_str());
        zinfo->garnetNetwork->printStats();
    }
}

/* 1.11.94: the guest's exit status. The exit/exit_group syscall hook below
 * stores the status the workload exited with; SimEnd() hands it to the host
 * as QEMU's own exit code. It was always 0, so a workload that REFUSED at
 * data preparation (pimid_devorg: PREP REFUSED, exit 1) or failed an
 * assertion came back to the simulator as a completed run, which then parsed
 * and priced the whole run as a measurement. */
static int g_guest_exit_status = 0;
/* (1.12.115): the guest reached exit/exit_group. QEMU also leaves through the
 * plugin's exit when it fails to start the guest (a launch error: "mmap stack",
 * "Cannot allocate vsyscall page", a reserved range too small, a bad image);
 * SimEnd's exit code then said 0 and the launcher read an all-zero run. */
static bool g_guest_exit_seen = false;
/* (1.12.115): the guest executed a block. A run that ends without the guest's own
 * exit (the termination condition: simulation.max_instructions, the end of a
 * message-passing ROI) did run; only a QEMU that never started the guest did not */
static std::atomic<bool> g_guest_ran{false};
static std::atomic<bool> g_roi_begin_seen{false};   // (1.12.115): a ROI_BEGIN marker executed (any mode)
static const int kExitGuestNotStarted = 125;   // the guest QEMU failed before the workload exited
static const int kExitRoiNeverOpened = 124;    // the workload has ROI markers, the ROI never opened

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

    /* (1.12.115): a run that did not happen is not reported as one */
    if (!g_guest_exit_seen && !g_guest_ran.load()) {   // (1.12.115): and it never ran
        fprintf(stderr, "[guest] FATAL: the guest QEMU exited before the workload ran (a launch error is printed above); "
                        "nothing was simulated (exit %d)\n", kExitGuestNotStarted);
        g_guest_exit_status = kExitGuestNotStarted;
    } else if (g_guest_exit_status == 0 && getenv("PIMID_ROI_EXPECTED") && !g_roi_begin_seen.load()) {
        fprintf(stderr, "[guest] FATAL: the workload carries ROI markers but its ROI never opened; "
                        "nothing was measured (exit %d)\n", kExitRoiNeverOpened);
        g_guest_exit_status = kExitRoiNeverOpened;
    }

    /* Terminate the process.  Background threads (contention sim, scheduler
     * watchdog) won't exit on their own, same as the original ZSim behaviour.
     * Use _exit() rather than exit() to avoid re-entering atexit/plugin_exit. */
    _exit(g_guest_exit_status & 0xff);
}

/* ---- Thread management ---- */

/* 1.12.18 (2026-10-24, MPI checksums): SimThreadFini once per SimThreadStart.
 * After a thread-MPI ROI the ranks run to their exit; each rank thread is
 * finished by vcpu_exit_cb, and plugin_exit finished it again (Scheduler::
 * finish assertion, scheduler.h gidMap, then a QEMU-internal SIGSEGV). */
static bool g_simThreadFinished[MAX_THREADS + 1];
static void SimThreadStart(uint32_t tid) {
    info("Thread %d starting", tid);
    if (tid > MAX_THREADS) {
        panic("tid > MAX_THREADS");
    }
    g_simThreadFinished[tid] = false;
    ProcessTreeNode* procTree = zinfo->procArray[procIdx];
    zinfo->sched->start(procIdx, tid, procTree->getMask());

    fPtrs[tid] = joinPtrs;
    clearCid(tid);
}

static void SimThreadFini(uint32_t tid) {
    if (tid <= MAX_THREADS && g_simThreadFinished[tid]) return;   // 1.12.18: finished already (see SimThreadStart)
    if (tid <= MAX_THREADS) g_simThreadFinished[tid] = true;
    // Thread left permanently -> EXITED (exempt from the epoch cut).
    if ((g_mpi_thread_mode || g_spCut) && tid < MAX_THREADS && cids[tid] < MAX_THREADS)   // 1.12.18: single-process cut too
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
    if (!g_pin_device_homes) return device_mask;   // (1.12.76): every mode, not thread-MPI only
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

/* (1.12.76): host homes, as the device homes above: thread vcpu -> host core
 * host_core_list[vcpu % n], one owner per core; a thread that collides keeps the
 * whole host mask. With OMP_PROC_BIND=true / OMP_PLACES=cores (set by PIMID)
 * libgomp binds thread i to place i: this is that placement. */
static g_vector<uint32_t> host_core_list;
static g_vector<uint32_t> host_home_owner;
static g_vector<g_vector<bool>> host_home_mask;
static bool g_host_pin_warned = false;
static const g_vector<bool>& hostMaskFor(uint32_t tid) {
    if (!g_pin_device_homes || tid >= MAX_THREADS || host_core_list.empty()) return host_mask;
    const uint32_t vcpu = tidToVcpu[tid];
    if (vcpu == UNINITIALIZED_CID) return host_mask;
    std::lock_guard<std::mutex> lock(pinMutex);
    const uint32_t idx = vcpu % (uint32_t)host_core_list.size();
    if (host_home_owner[idx] != UNINITIALIZED_CID && host_home_owner[idx] != vcpu) {
        if (!g_host_pin_warned) {
            g_host_pin_warned = true;
            warn("[ZSim] host pinning: vcpu %u and vcpu %u share host core %u (%lu host cores); the later thread "
                 "keeps the whole host mask", vcpu, host_home_owner[idx], host_core_list[idx],
                 (unsigned long)host_core_list.size());
        }
        return host_mask;
    }
    host_home_owner[idx] = vcpu;
    g_vector<bool>& m = host_home_mask[tid];
    if (m.size() != zinfo->numCores) m.resize(zinfo->numCores, false);
    for (uint32_t i = 0; i < zinfo->numCores; i++) m[i] = false;
    m[host_core_list[idx]] = true;
    return m;
}
static void ensureThreadInit(uint32_t tid) {
    if (thread_initialized[tid]) return;
    thread_initialized[tid] = true;

    const g_vector<bool>& mask =
        (thread_domain[tid].load() == DOMAIN_DEVICE) ? deviceMaskFor(tid)
                                                     : hostMaskFor(tid);   // (1.12.76): pinned host home

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
    if (pimidPostRoiFree()) { fPtrs[tid] = nopPtrs; return; }   // 1.12.18: after the ROI, run free
    /* trace only each thread's first few joins -- the startup hang window */
    bool tr = g_cosim_trace && tid < MAX_THREADS && trace_join_count[tid]++ < 6;
    if (tr) COSIMTRACE("tid=%u sched->join enter", tid);
    uint32_t cid = zinfo->sched->join(procIdx, tid);
    if (tr) COSIMTRACE("tid=%u sched->join done cid=%u", tid, cid);
    setCid(tid, cid);
    if (unlikely(g_spmPendAny.load(std::memory_order_relaxed))) spmApplyPending(tid);   // (1.12.164)
    wakeSyncApply(tid, cid);   // 1.12.18 (user ruling 2026-10-23, "sync to waker")
    detTurnBegin(tid, cid);
    if (g_spCut && cid < MAX_THREADS) {   // 1.12.18 (ruling 2026-10-24): awake again -> ACTIVE (after the wake sync)
        uint32_t q = 1;
        g_coreRoiState[cid].compare_exchange_strong(q, 0);
    }

    if (unlikely(zinfo->terminationConditionMet) && pimidPostRoiFree()) {
        zinfo->sched->leave(procIdx, tid, cid);
        clearCid(tid);
        fPtrs[tid] = nopPtrs;
        return;
    }
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

    /* (1.12.76): a thread's tid is its vCPU index, QEMU's cpu index, which the
     * parent assigns at the clone (cpu_get_free_index: one past the highest in
     * use), in the guest's own thread-creation order. Through 1.12.61 the tid was
     * nextTid++ in the order QEMU ran the plugin's vCPU-init callbacks, which QEMU
     * runs asynchronously on each new thread when it first executes, so the
     * host scheduler numbered the threads. */
    if (vcpu_index >= MAX_THREADS) return MAX_THREADS;
    uint32_t tid = vcpu_index;
    if (tid >= nextTid) nextTid = tid + 1;
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
     * call (E8), indirect call (FF /2,/3), indirect jmp (FF /4,/5), ret
     * (C3/C2) or (1.12.1) direct jmp (E9/EB), termKind holds the CtrlFlowKind
     * code (2..6, see ooo_core.h) and
     * termPc/termRetAddr its PC and (for calls) fall-through return address.
     * The actual target is resolved from the NEXT TB's start address, exactly
     * like the conditional-direction wiring above. termKind=0 otherwise.
     * Mutually exclusive with endsInCondBranch (one terminator per TB). */
    uint8_t   termKind;
    uint64_t  termPc;
    uint64_t  termRetAddr;
    /* 1.12.18 (PIM runtime primitives): RtRegion of this block, and at a
     * libgomp entry point its event (bit 15 set; kind | arg << 8). */
    uint8_t   rtRegion;
    uint16_t  rtSym;
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

/* ==== 1.12.18: PIM runtime primitives (user rulings 2026-10-23) ===========
 *
 * "The PIM way, not as full SW overhead": a PE is NOT charged for executing
 * the parallel runtime's own code. Each runtime EVENT is charged the cost of
 * the corresponding PIM runtime primitive instead, as idle cycles (addDelay)
 * on the PE's own clock, in program order -- deterministic. The kernel's own
 * work stays simulated: user code inside a critical section, the reduce
 * arithmetic on user buffers, and every byte a message moves (the sender's
 * buffer read, the Garnet transfer, the receiver's write).
 *
 * Runtime code is "not executed on the PE": its blocks and memory accesses
 * are not fed to the core (no instructions, no cycles, no traffic, no
 * energy), exactly like the process-mode MPI comm window. The thread stays
 * ATTACHED to its core while it runs that code (an eager Join at each
 * dropped block), so the futex leave/join and the wake-sync stamps keep
 * working: a waiter still cannot run ahead of its releaser, and the primitive
 * cost is ADDED to that ordering (release, barrier and launch are charged
 * before the release stamp; an acquire is charged after the acquisition).
 *
 * Two sources of runtime code:
 *  1. libgomp (OpenMP). Recognised by ADDRESS RANGE: the executable mappings
 *     of libgomp.so in /proc/self/maps (guest_base 0). A thread enters the
 *     runtime when it jumps from the workload binary into libgomp; it leaves
 *     when control reaches the workload binary again (the outlined region
 *     body, or the return from the GOMP_* / omp_* call). Code in other
 *     libraries (libc: malloc, futex, pthread_create) is attributed to
 *     whichever side called it. The EVENT is the exported libgomp symbol the
 *     jump lands on (libgomp's .dynsym, read from the mapped file); a return
 *     from a region body into libgomp is the implicit barrier at the end of
 *     the region. No interposer library is needed and every GOMP_* entry
 *     point of any libgomp version is covered by construction; an entry
 *     point without a PIM primitive (tasks, target, teams, cancel) REFUSES.
 *  2. libpimid_mpi (thread-MPI shim). The shim brackets its bookkeeping with
 *     RT_BEGIN / RT_END and announces each protocol event with RT_EVENT.
 *
 * PEs only by default (apply_to: pe): a host CPU really runs libgomp, so
 * host cores keep executing it (apply_to: all extends the model to them).
 *
 * Costs (all overridable, simulation.pim_runtime.*; docs/yaml_reference.md):
 * UPMEM's measured on-chip primitives, the device's PEs treated as one sync
 * domain ("assumes DPU-style atomics across the device's PEs"):
 *  - lock acquire / release: 1 instruction each (UPMEM SDK syslib/mutex.h:
 *    acquire / release), latency 11 cycles per op (revolver pipeline: "same
 *    thread dispatched 11 cycles apart", Gomez-Luna et al., PrIM, IEEE Access
 *    2022, arXiv 2105.03814 Sec. 2.2; uPIMulator Table I).
 *  - barrier: DERIVED from PrIM Appendix Fig. 21 (reduction final step, 1
 *    DPU, int64): (BARRIER tree - HANDS tree) / log2 T = 224 / 504 / 1201 /
 *    2777 cycles at T = 2 / 4 / 8 / 16, interpolated linearly in the
 *    participant count P, extrapolated linearly above 16 (printed).
 *  - OpenMP parallel launch: one barrier (tasklets are resident; a fork/join
 *    is a barrier_wait) -- the barrier table.
 *  - OpenMP reduction combine: PrIM Fig. 21 HANDS tree 304 / 768 / 1392 /
 *    2654 cycles at T = 2 / 4 / 8 / 16 (measured, includes the adds).
 *  - MPI point-to-point protocol: one handshake notify/wait pair (UPMEM SDK
 *    handshake.c, ~10 instructions x 11 cycles = 110), DERIVED.
 *  - MPI collective step: one point-to-point handshake (110); MPI barrier:
 *    the barrier table.
 * Cycles are used as published (UPMEM DPU pipeline cycles, PrIM's DPU at
 * 350 MHz; the alu_core PE is the analogous pipeline); cycle_basis: time
 * converts them through time to the PE clock instead. */
enum RtKind { RTK_OMP_LAUNCH = 0, RTK_OMP_BARRIER, RTK_OMP_ACQUIRE, RTK_OMP_RELEASE, RTK_OMP_DISPATCH,
              RTK_OMP_REDUCTION, RTK_OMP_QUERY, RTK_MPI_P2P, RTK_MPI_COLL_STEP, RTK_MPI_RED_STEP,
              RTK_MPI_BARRIER, RTK_N };
static const char* const kRtKey[RTK_N] = {
    "omp_launch", "omp_barrier", "omp_lock_acquire", "omp_lock_release", "omp_loop_dispatch",
    "omp_reduction", "omp_query", "mpi_p2p_protocol", "mpi_collective_step", "mpi_reduction_step",
    "mpi_barrier" };
enum RtScale { RTS_CONSTANT = 0, RTS_LINEAR, RTS_LOG2 };
enum RtTable { RTT_NONE = 0, RTT_BARRIER, RTT_REDUCTION };
static const uint16_t RT_SYM_UNMAPPED = 0xff;   /* a libgomp entry point with no PIM primitive */
static const uint8_t RT_ARG_NONE = 0, RT_ARG_TEAM_RDX = 1, RT_ARG_ICV_RDI = 2;
enum RtRegion { RTR_OTHER = 0, RTR_USER = 1, RTR_GOMP = 2 };
enum RtState { RTS_UNSET = 0, RTS_KERNEL = 1, RTS_RUNTIME = 2 };

struct RtParams {
    bool enabled = true;
    bool allCores = false;          /* apply_to: all */
    bool timeBasis = false;         /* cycle_basis: time */
    double srcMHz = 350.0;          /* clock the published cycles were measured at (PrIM: 350 MHz DPU) */
    std::vector<std::pair<uint32_t, double>> barTab = { {2, 224}, {4, 504}, {8, 1201}, {16, 2777} };
    std::vector<std::pair<uint32_t, double>> redTab = { {2, 304}, {4, 768}, {8, 1392}, {16, 2654} };
    double cyc[RTK_N]   = { 0, 0, 11, 11, 22, 0, 1, 110, 110, 0, 0 };
    int    scale[RTK_N] = { 0 };
    int    table[RTK_N] = { RTT_BARRIER, RTT_BARRIER, RTT_NONE, RTT_NONE, RTT_NONE,
                            RTT_REDUCTION, RTT_NONE, RTT_NONE, RTT_NONE, RTT_NONE, RTT_BARRIER };
};
static RtParams g_rt;
static uint8_t  g_rtState[MAX_THREADS];          /* RtState per thread */
static bool     g_rtShim[MAX_THREADS];           /* inside a shim RT_BEGIN/RT_END window */
static uint64_t g_rtPend[MAX_THREADS];           /* acquire cycles charged at the runtime exit */
static std::atomic<uint32_t> g_rtTeam{1};        /* team size of the current OpenMP region */
static std::atomic<uint32_t> g_rtIcv{1};         /* nthreads-var (OMP_NUM_THREADS / omp_set_num_threads) */
static std::atomic<bool> g_rtGompLoaded{false};
static std::atomic<bool> g_rtExtrapWarned{false};
/* per core (cid) statistics; *Roi = snapshot at the last roi_begin */
static uint64_t g_rtCnt[MAX_THREADS][RTK_N], g_rtCyc[MAX_THREADS][RTK_N], g_rtDropI[MAX_THREADS], g_rtDropM[MAX_THREADS];
static uint64_t g_rtCntRoi[MAX_THREADS][RTK_N], g_rtCycRoi[MAX_THREADS][RTK_N], g_rtDropIRoi[MAX_THREADS], g_rtDropMRoi[MAX_THREADS];

static double rtInterp(const std::vector<std::pair<uint32_t, double>>& t, uint32_t P, bool* extrap) {
    if (P <= 1 || t.empty()) return 0.0;   /* one participant: nothing to synchronise */
    if (P <= t[0].first) return t[0].second * (double)(P - 1) / (double)(t[0].first > 1 ? t[0].first - 1 : 1);
    for (size_t i = 1; i < t.size(); i++)
        if (P <= t[i].first) {
            const double f = (double)(P - t[i - 1].first) / (double)(t[i].first - t[i - 1].first);
            return t[i - 1].second + f * (t[i].second - t[i - 1].second);
        }
    *extrap = true;
    if (t.size() < 2) return t.back().second;
    const auto& a = t[t.size() - 2]; const auto& b = t.back();
    return b.second + (double)(P - b.first) * (b.second - a.second) / (double)(b.first - a.first);
}
static uint32_t rtPeMHz() {
    return zinfo->hierarchy.nocBandwidthFreqMHz > 0 ? zinfo->hierarchy.nocBandwidthFreqMHz : zinfo->freqMHz;
}
static uint64_t rtCost(int kind, uint32_t P) {
    double c = 0.0;
    bool ex = false;
    if (g_rt.table[kind] == RTT_BARRIER) c = rtInterp(g_rt.barTab, P, &ex);
    else if (g_rt.table[kind] == RTT_REDUCTION) c = rtInterp(g_rt.redTab, P, &ex);
    else {
        double f = 1.0;
        if (g_rt.scale[kind] == RTS_LINEAR) f = (double)P;
        else if (g_rt.scale[kind] == RTS_LOG2) { f = 0.0; for (uint32_t s = 1; s < P; s <<= 1) f += 1.0; }
        c = g_rt.cyc[kind] * f;
    }
    if (ex && !g_rtExtrapWarned.exchange(true))
        info("[pim-runtime] NOTE: a %s event with P=%u participants is beyond PrIM Fig. 21's T <= 16: its cost is "
             "EXTRAPOLATED linearly in P from the last two table points (%.0f cycles)", kRtKey[kind], P, c);
    if (g_rt.timeBasis && g_rt.srcMHz > 0) c = c * (double)rtPeMHz() / g_rt.srcMHz;
    return (uint64_t)(c + 0.5);
}

/* PEs only, unless apply_to: all. A host CPU really runs its runtime. */
static inline bool rtApplies(uint32_t tid) {
    if (!g_rt.enabled || tid >= MAX_THREADS) return false;
    if (g_rt.allCores || !g_cosim_mode) return true;
    return thread_domain[tid].load() == DOMAIN_DEVICE;
}
/* The thread must be on its core to be charged: a dropped block does not go
 * through JoinBbl, so join here (the wake sync is applied by Join). */
static inline bool rtEnsureAttached(uint32_t tid) {
    if (fPtrs[tid].type == FPTR_JOIN) Join(tid);
    return cores[tid] != nullptr && cids[tid] < zinfo->numCores && cids[tid] < MAX_THREADS;
}
static void rtCharge(uint32_t tid, int kind, uint32_t P, bool deferred) {
    const uint64_t c = rtCost(kind, P);
    if (!rtEnsureAttached(tid)) { g_rtPend[tid] += c; return; }
    const uint32_t cid = cids[tid];
    g_rtCnt[cid][kind]++;
    if (deferred) { g_rtPend[tid] += c; return; }
    g_rtCyc[cid][kind] += c;
    if (c) cores[tid]->addDelay((uint32_t)std::min<uint64_t>(c, 0xFFFFFFFFull));
}
static void rtApplyPending(uint32_t tid) {
    if (!g_rtPend[tid] || !rtEnsureAttached(tid)) return;
    const uint64_t c = g_rtPend[tid];
    g_rtPend[tid] = 0;
    g_rtCyc[cids[tid]][RTK_OMP_ACQUIRE] += c;
    cores[tid]->addDelay((uint32_t)std::min<uint64_t>(c, 0xFFFFFFFFull));
}

/* ---- libgomp recognition ---- */
struct RtMap { uint64_t lo, hi; uint8_t region; };
static std::vector<RtMap> g_rtMaps;
static std::unordered_map<uint64_t, uint16_t> g_rtSyms;        /* entry address -> kind | arg << 8 */
static std::unordered_map<uint64_t, std::string> g_rtSymName;
static std::string g_rtExe;
static std::mutex g_rtMu;
static uint32_t g_rtReloads = 0;

static uint16_t rtKindForName(const char* n) {
    auto in = [n](std::initializer_list<const char*> l) { for (const char* s : l) if (!strcmp(n, s)) return true; return false; };
    if (!strcmp(n, "GOMP_parallel_end")) return RTK_OMP_BARRIER;
    if (!strncmp(n, "GOMP_parallel", 13)) return RTK_OMP_LAUNCH | (RT_ARG_TEAM_RDX << 8);   /* (fn, data, num_threads, ...) */
    if (in({"GOMP_barrier", "GOMP_barrier_cancel", "GOMP_loop_end", "GOMP_loop_end_cancel",
            "GOMP_sections_end", "GOMP_sections_end_cancel"})) return RTK_OMP_BARRIER;
    if (in({"GOMP_critical_start", "GOMP_critical_name_start", "GOMP_atomic_start", "GOMP_ordered_start",
            "GOMP_doacross_wait", "GOMP_doacross_ull_wait", "omp_set_lock", "omp_set_nest_lock", "omp_test_lock",
            "omp_test_nest_lock", "omp_set_lock_", "omp_set_nest_lock_", "omp_test_lock_", "omp_test_nest_lock_"}))
        return RTK_OMP_ACQUIRE;
    if (in({"GOMP_critical_end", "GOMP_critical_name_end", "GOMP_atomic_end", "GOMP_ordered_end",
            "GOMP_doacross_post", "GOMP_doacross_ull_post", "omp_unset_lock", "omp_unset_nest_lock",
            "omp_unset_lock_", "omp_unset_nest_lock_"})) return RTK_OMP_RELEASE;
    if (in({"GOMP_taskgroup_reduction_register", "GOMP_taskgroup_reduction_unregister",
            "GOMP_task_reduction_remap", "GOMP_workshare_task_reduction_unregister"})) return RTK_OMP_REDUCTION;
    if (!strncmp(n, "GOMP_loop_", 10) ||
        in({"GOMP_sections_start", "GOMP_sections2_start", "GOMP_sections_next", "GOMP_sections_end_nowait",
            "GOMP_single_start", "GOMP_single_copy_start", "GOMP_single_copy_end"})) return RTK_OMP_DISPATCH;
    if (!strcmp(n, "omp_set_num_threads")) return RTK_OMP_QUERY | (RT_ARG_ICV_RDI << 8);
    if (!strncmp(n, "omp_", 4)) return RTK_OMP_QUERY;
    return RT_SYM_UNMAPPED;   /* GOMP_task*, GOMP_target*, GOMP_teams*, GOMP_cancel*, GOACC_*, ... */
}

/* libgomp's exported functions, from the .dynsym of the mapped file. */
static void rtLoadGompSyms(const std::string& path, uint64_t bias) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) panic("[pim-runtime] cannot read %s to find libgomp's entry points", path.c_str());
    std::vector<uint8_t> d;
    uint8_t buf[65536]; size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + r);
    fclose(f);
    if (d.size() < sizeof(Elf64_Ehdr) || memcmp(d.data(), ELFMAG, SELFMAG) || d[EI_CLASS] != ELFCLASS64)
        panic("[pim-runtime] %s is not a 64-bit ELF file", path.c_str());
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)d.data();
    if (eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) > d.size())
        panic("[pim-runtime] %s: section headers out of range", path.c_str());
    const Elf64_Shdr* sh = (const Elf64_Shdr*)(d.data() + eh->e_shoff);
    uint32_t n = 0;
    for (uint32_t i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_DYNSYM || sh[i].sh_link >= eh->e_shnum) continue;
        const Elf64_Shdr& st = sh[sh[i].sh_link];
        const Elf64_Sym* sy = (const Elf64_Sym*)(d.data() + sh[i].sh_offset);
        const uint64_t ns = sh[i].sh_size / sizeof(Elf64_Sym);
        for (uint64_t k = 0; k < ns; k++) {
            const int ty = ELF64_ST_TYPE(sy[k].st_info);
            if ((ty != STT_FUNC && ty != STT_GNU_IFUNC) || sy[k].st_shndx == SHN_UNDEF || !sy[k].st_value) continue;
            if (sy[k].st_name >= st.sh_size) continue;
            const char* nm = (const char*)(d.data() + st.sh_offset + sy[k].st_name);
            const uint64_t a = bias + sy[k].st_value;
            const uint16_t kd = rtKindForName(nm);
            auto it = g_rtSyms.find(a);
            if (it == g_rtSyms.end() || (it->second & 0xff) == RT_SYM_UNMAPPED) { g_rtSyms[a] = kd; g_rtSymName[a] = nm; }
            n++;
        }
    }
    info("[pim-runtime] libgomp recognised: %s at bias 0x%lx, %u exported functions mapped to PIM runtime events",
         path.c_str(), (unsigned long)bias, n);
}

static std::string rtRealpath(const std::string& p) {
    char b[4096];
    return realpath(p.c_str(), b) ? std::string(b) : p;
}
/* The workload binary: the argument after "--" on QEMU's command line (src/main.cpp launches qemu ... -- binary). */
static void rtFindExe() {
    FILE* f = fopen("/proc/self/cmdline", "rb");
    if (!f) return;
    std::string all; char buf[4096]; size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, r);
    fclose(f);
    std::vector<std::string> av;
    for (size_t p = 0; p < all.size(); ) { size_t e = all.find('\0', p); if (e == std::string::npos) e = all.size(); av.push_back(all.substr(p, e - p)); p = e + 1; }
    for (size_t i = 0; i + 1 < av.size(); i++) if (av[i] == "--") { g_rtExe = rtRealpath(av[i + 1]); return; }
}
/* Caller holds g_rtMu. QEMU linux-user maps the guest's segments without host
 * PROT_EXEC (it translates guest code, it never runs it), so the guest's text
 * is an r--p mapping; mappings are classified by range regardless of their
 * permissions (a translated block's address can only lie in a guest mapping).
 * The simulator's own process may map a libgomp too (r-xp, a host library):
 * every libgomp instance's symbols are loaded, keyed by its own load bias, so
 * the guest's is covered whichever it is. */
static std::vector<uint64_t> g_rtGompBiases;
static void rtReloadMaps() {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return;
    std::vector<RtMap> m;
    char line[1024];
    std::vector<std::pair<std::string, uint64_t>> gomps;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo = 0, hi = 0, off = 0; char perm[8] = {0}; int pos = 0;
        if (sscanf(line, "%lx-%lx %7s %lx %*s %*s%n", &lo, &hi, perm, &off, &pos) < 4) continue;
        const char* p = line + pos; while (*p == ' ' || *p == '\t') p++;
        std::string path(p); while (!path.empty() && (path.back() == '\n' || path.back() == ' ')) path.pop_back();
        if (getenv("PIMID_RT_DEBUG")) info("[pim-runtime] debug map %s", line);
        if (path.empty() || path[0] != '/') continue;   /* anonymous, [heap], [stack], [vdso]: OTHER */
        const size_t sl = path.rfind('/');
        const bool isGomp = path.compare(sl + 1, 7, "libgomp") == 0;
        if (isGomp) {
            bool seen = false;
            for (auto& g : gomps) if (g.second == lo - off) seen = true;
            if (!seen) gomps.push_back({path, lo - off});
        }
        m.push_back({lo, hi, (uint8_t)(isGomp ? RTR_GOMP : (path == g_rtExe ? RTR_USER : RTR_OTHER))});
    }
    fclose(f);
    g_rtMaps.swap(m);
    for (auto& g : gomps) {
        bool loaded = false;
        for (uint64_t b : g_rtGompBiases) if (b == g.second) loaded = true;
        if (loaded) continue;
        g_rtGompBiases.push_back(g.second);
        rtLoadGompSyms(g.first, g.second);
    }
}
/* Translation time: the region of a block and, at a libgomp entry point, its event. */
static void rtClassifyTb(uint64_t va, uint8_t* region, uint16_t* sym) {
    *region = RTR_OTHER; *sym = 0;
    if (getenv("PIMID_RT_DEBUG")) { static std::atomic<int> nd{0}; if (nd.fetch_add(1) < 50) info("[pim-runtime] debug tb 0x%lx", (unsigned long)va); }
    std::lock_guard<std::mutex> g(g_rtMu);
    for (int pass = 0; pass < 2; pass++) {
        for (const RtMap& m : g_rtMaps)
            if (va >= m.lo && va < m.hi) {
                *region = m.region;
                if (m.region == RTR_GOMP && !g_rtGompLoaded.exchange(true)) {   /* the guest's libgomp runs */
                    if (g_rtExe.empty())
                        panic("[pim-runtime] the guest runs libgomp but the workload binary could not be identified from "
                              "QEMU's command line: the runtime cannot be told from the kernel. Set simulation.pim_runtime.enabled: false");
                    for (const RtMap& x : g_rtMaps)
                        if (x.region == RTR_USER)
                            info("[pim-runtime] workload (kernel) mapping [0x%lx, 0x%lx)", (unsigned long)x.lo, (unsigned long)x.hi);
                    info("[pim-runtime] guest libgomp code at 0x%lx: OpenMP runtime events are recognised", (unsigned long)va);
                }
                if (m.region == RTR_GOMP) {
                    auto it = g_rtSyms.find(va);
                    if (it != g_rtSyms.end()) *sym = (uint16_t)(it->second | 0x8000);   /* bit 15: an entry point */
                }
                return;
            }
        if (pass == 0 && g_rtReloads < 100000) { g_rtReloads++; rtReloadMaps(); }
    }
}
static uint8_t rtRegionOf(uint64_t a) {
    std::lock_guard<std::mutex> g(g_rtMu);
    for (const RtMap& m : g_rtMaps) if (a >= m.lo && a < m.hi) return m.region;
    return RTR_OTHER;
}
static void rtOmpEntry(uint32_t tid, uint16_t sym, uint64_t va) {
    const int kind = sym & 0xff;
    const uint8_t arg = (uint8_t)((sym >> 8) & 0x7f);
    if (kind == RT_SYM_UNMAPPED) {
        std::string nm;
        { std::lock_guard<std::mutex> g(g_rtMu); auto it = g_rtSymName.find(va); if (it != g_rtSymName.end()) nm = it->second; }
        panic("[pim-runtime] FATAL: the workload calls libgomp's %s on a PE; it has no PIM runtime primitive (tasks, "
              "target, teams and cancellation are not modelled). Run with simulation.pim_runtime.enabled: false.",
              nm.empty() ? "<unnamed>" : nm.c_str());
    }
    uint64_t v = 0;
    if (arg == RT_ARG_TEAM_RDX) {
        resolveRcxHandle();
        uint64_t nt = 0;
        if (!readRdx(&nt)) nt = 0;
        const uint32_t P = (uint32_t)(nt ? nt : g_rtIcv.load());
        g_rtTeam.store(P ? P : 1);
    } else if (arg == RT_ARG_ICV_RDI) {
        resolveRcxHandle();
        if (readGuestReg(rdi_handle.load(std::memory_order_acquire), &v) && (int32_t)v > 0) g_rtIcv.store((uint32_t)v);
    }
    const uint32_t P = (kind == RTK_OMP_LAUNCH || kind == RTK_OMP_BARRIER || kind == RTK_OMP_REDUCTION) ? g_rtTeam.load() : 1;
    rtCharge(tid, kind, P, kind == RTK_OMP_ACQUIRE);
    /* A TAIL call from a region body (GOMP_critical_end, omp_unset_lock or
     * GOMP_barrier as the body's last statement, compiled to a jmp) returns
     * straight into libgomp: the body ends with this call, so its implicit
     * barrier is charged here (the return address on the stack is in libgomp). */
    uint64_t sp = 0;
    resolveRcxHandle();
    if (readGuestReg(rsp_handle.load(std::memory_order_acquire), &sp) && sp) {
        const uint64_t ret = *(const uint64_t*)(uintptr_t)sp;   /* guest_base 0: guest memory is host memory */
        if (rtRegionOf(ret) == RTR_GOMP) rtCharge(tid, RTK_OMP_BARRIER, g_rtTeam.load(), false);
    }
}
/* Per executed block: the runtime state machine. Returns true when the block
 * is runtime code to be dropped (not fed to the core). */
static bool rtOnTb(uint32_t tid, uint8_t region, uint16_t sym, uint64_t va) {
    const bool app = rtApplies(tid);
    const uint8_t st = g_rtState[tid];
    if (region == RTR_GOMP) {
        if (st != RTS_RUNTIME) {
            if (app && st == RTS_KERNEL) {
                if (sym & 0x8000) rtOmpEntry(tid, (uint16_t)(sym & 0x7fff), va);
                else rtCharge(tid, RTK_OMP_BARRIER, g_rtTeam.load(), false);   /* a region body returned: implicit barrier */
            }
            g_rtState[tid] = RTS_RUNTIME;   /* from UNSET: a new thread starting in the runtime, no event */
        }
    } else if (region == RTR_USER) {
        g_rtState[tid] = RTS_KERNEL;
    }
    const uint8_t now = g_rtState[tid];
    const bool drop = app && (g_rtShim[tid] || now == RTS_RUNTIME ||
                              (now == RTS_UNSET && tid != 0 && g_rtGompLoaded.load(std::memory_order_relaxed)));
    /* leaving the runtime: a deferred acquire is charged after the
     * acquisition (Join has applied the wake sync to the releaser first) */
    if (!drop && g_rtPend[tid]) rtApplyPending(tid);
    return drop;
}
static inline bool rtDropNow(uint32_t tid) {
    if (!g_rt.enabled || !rtApplies(tid)) return false;
    const uint8_t s = g_rtState[tid];
    return g_rtShim[tid] || s == RTS_RUNTIME || (s == RTS_UNSET && tid != 0 && g_rtGompLoaded.load(std::memory_order_relaxed));
}
static void rtRoiSnapshot() {
    memcpy(g_rtCntRoi, g_rtCnt, sizeof g_rtCnt); memcpy(g_rtCycRoi, g_rtCyc, sizeof g_rtCyc);
    memcpy(g_rtDropIRoi, g_rtDropI, sizeof g_rtDropI); memcpy(g_rtDropMRoi, g_rtDropM, sizeof g_rtDropM);
}
static void rtPrintStats() {
    if (!g_rt.enabled || !zinfo) return;
    uint64_t tc[RTK_N] = {0}, ty[RTK_N] = {0}, tI = 0, tM = 0;
    for (uint32_t c = 0; c < zinfo->numCores && c < MAX_THREADS; c++) {
        std::string s; uint64_t cy = 0, ev = 0;
        for (int k = 0; k < RTK_N; k++) {
            const uint64_t n = g_rtCnt[c][k] - g_rtCntRoi[c][k], y = g_rtCyc[c][k] - g_rtCycRoi[c][k];
            tc[k] += n; ty[k] += y; cy += y; ev += n;
            if (n) { char b[96]; snprintf(b, sizeof b, " %s=%lu/%lu", kRtKey[k], (unsigned long)n, (unsigned long)y); s += b; }
        }
        const uint64_t dI = g_rtDropI[c] - g_rtDropIRoi[c], dM = g_rtDropM[c] - g_rtDropMRoi[c];
        tI += dI; tM += dM;
        if (ev || dI || dM)
            info("[pim-runtime] ROI core %u: events (count/cycles):%s | primitive cycles charged %lu | runtime code not executed on the PE: %lu instrs, %lu memory accesses",
                 c, s.empty() ? " none" : s.c_str(), (unsigned long)cy, (unsigned long)dI, (unsigned long)dM);
    }
    std::string s; uint64_t E = 0, Y = 0;
    for (int k = 0; k < RTK_N; k++) {
        E += tc[k]; Y += ty[k];
        if (tc[k]) { char b[96]; snprintf(b, sizeof b, " %s=%lu/%lu", kRtKey[k], (unsigned long)tc[k], (unsigned long)ty[k]); s += b; }
    }
    info("[pim-runtime] ROI total: events=%lu charged=%lu cycles removedInstrs=%lu removedMemAccesses=%lu |%s [1.12.18, ruling 2026-10-23; assumes DPU-style atomics across the device's PEs]",
         (unsigned long)E, (unsigned long)Y, (unsigned long)tI, (unsigned long)tM, s.empty() ? " no events" : s.c_str());
}
static std::vector<std::pair<uint32_t, double>> rtParseTable(const std::string& v, const char* key) {
    std::vector<std::pair<uint32_t, double>> t;
    size_t p = 0;
    while (p < v.size()) {
        size_t e = v.find(',', p); if (e == std::string::npos) e = v.size();
        const std::string it = v.substr(p, e - p);
        const size_t c = it.find(':');
        if (c == std::string::npos) panic("[pim-runtime] %s: entry '%s' is not P:cycles", key, it.c_str());
        t.push_back({(uint32_t)strtoul(it.substr(0, c).c_str(), nullptr, 10), strtod(it.substr(c + 1).c_str(), nullptr)});
        p = e + 1;
    }
    std::sort(t.begin(), t.end());
    if (t.empty() || t[0].first < 2) panic("[pim-runtime] %s needs participant counts >= 2", key);
    return t;
}
/* PIMID_PIM_RUNTIME (src/main.cpp, simulation.pim_runtime): "key=value;..."; only the keys the user set. */
static void rtInit() {
    const char* e = getenv("PIMID_PIM_RUNTIME");
    const std::string env = e ? e : "";
    size_t p = 0;
    while (p < env.size()) {
        size_t q = env.find(';', p); if (q == std::string::npos) q = env.size();
        const std::string kv = env.substr(p, q - p); p = q + 1;
        const size_t eq = kv.find('='); if (eq == std::string::npos) continue;
        const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        if (k == "enabled") g_rt.enabled = (v == "1");
        else if (k == "apply_to") g_rt.allCores = (v == "all");
        else if (k == "cycle_basis") g_rt.timeBasis = (v == "time");
        else if (k == "source_clock_mhz") g_rt.srcMHz = strtod(v.c_str(), nullptr);
        else if (k == "barrier_table") g_rt.barTab = rtParseTable(v, "barrier_table");
        else if (k == "reduction_table") g_rt.redTab = rtParseTable(v, "reduction_table");
        else {
            bool hit = false;
            for (int i = 0; i < RTK_N && !hit; i++) {
                if (k == std::string(kRtKey[i]) + "_cycles") { g_rt.cyc[i] = strtod(v.c_str(), nullptr); g_rt.table[i] = RTT_NONE; hit = true; }
                else if (k == std::string(kRtKey[i]) + "_scaling") {
                    g_rt.scale[i] = (v == "linear") ? RTS_LINEAR : (v == "log2") ? RTS_LOG2 : RTS_CONSTANT; hit = true;
                }
            }
            if (!hit) panic("[pim-runtime] unknown PIMID_PIM_RUNTIME key '%s'", k.c_str());
        }
    }
    {
        const char* nt = getenv("OMP_NUM_THREADS");
        const uint32_t v = nt ? (uint32_t)atoi(nt) : 0;
        g_rtIcv.store(v ? v : (zinfo ? zinfo->numCores : 1));
    }
    if (!g_rt.enabled) { info("[pim-runtime] disabled (simulation.pim_runtime.enabled: false): runtime code is executed and priced like kernel code"); return; }
    rtFindExe();
    std::string s;
    for (int i = 0; i < RTK_N; i++) {
        char b[128];
        if (g_rt.table[i] == RTT_BARRIER) snprintf(b, sizeof b, " %s=barrier_table(P)", kRtKey[i]);
        else if (g_rt.table[i] == RTT_REDUCTION) snprintf(b, sizeof b, " %s=reduction_table(P)", kRtKey[i]);
        else snprintf(b, sizeof b, " %s=%g%s", kRtKey[i], g_rt.cyc[i],
                      g_rt.scale[i] == RTS_LINEAR ? "*P" : g_rt.scale[i] == RTS_LOG2 ? "*ceil(log2 P)" : "");
        s += b;
    }
    std::string bt, rt;
    for (auto& x : g_rt.barTab) { char b[48]; snprintf(b, sizeof b, " %u:%g", x.first, x.second); bt += b; }
    for (auto& x : g_rt.redTab) { char b[48]; snprintf(b, sizeof b, " %u:%g", x.first, x.second); rt += b; }
    info("[pim-runtime] enabled (apply_to=%s): runtime code is not executed on the PE; each runtime event is charged its PIM primitive. "
         "Assumes DPU-style atomics across the device's PEs (UPMEM measured primitives, one sync domain).", g_rt.allCores ? "all" : "pe");
    info("[pim-runtime] costs (cycles):%s; barrier_table{%s } (%sfrom PrIM Fig. 21), reduction_table{%s } (PrIM Fig. 21 HANDS); "
         "cycle_basis=%s (%s)", s.c_str(), bt.c_str(), pimid::provLabel("DERIVED "), rt.c_str(), g_rt.timeBasis ? "time" : "pe_cycles",
         g_rt.timeBasis ? "published DPU cycles at source_clock_mhz converted through time to the PE clock"
                        : "published UPMEM DPU pipeline cycles used as PE cycles");
    info("[pim-runtime] workload binary for the runtime/kernel split: %s", g_rtExe.empty() ? "<not found>" : g_rtExe.c_str());
}
/* Shim ops (src/parallel_abstraction/pimid_mpi.cpp). */
#define ZSIM_MAGIC_OP_RT_BEGIN  2064   /* 1.12.18: runtime bookkeeping follows: not executed on the PE */
#define ZSIM_MAGIC_OP_RT_END    2065
#define ZSIM_MAGIC_OP_RT_EVENT  2066   /* %rdx = kind | participants << 32: charge one primitive */
#define ZSIM_MAGIC_OP_RT_QUERY  2067   /* %rdx -> uint64: 1 if the PIM runtime applies to this thread */
static inline bool isRtMagicOp(uint64_t op) { return op >= ZSIM_MAGIC_OP_RT_BEGIN && op <= ZSIM_MAGIC_OP_RT_QUERY; }
static void handleRtMagicOp(uint64_t op, uint32_t tid) {
    if (tid >= MAX_THREADS || !zinfo) return;
    uint64_t a = 0;
    if (op == ZSIM_MAGIC_OP_RT_QUERY) {
        if (readRdx(&a) && a) *(volatile uint64_t*)a = rtApplies(tid) ? 1 : 0;
        return;
    }
    if (!g_rt.enabled) return;
    if (pimidPostRoiFree()) return;   // 1.12.18 (2026-10-24): no runtime charges after the ROI (see handleMpiMagicOp)
    if (op == ZSIM_MAGIC_OP_RT_BEGIN) { g_rtShim[tid] = true; return; }
    if (op == ZSIM_MAGIC_OP_RT_END)   { g_rtShim[tid] = false; return; }
    if (!readRdx(&a) || !rtApplies(tid)) return;
    const int kind = (int)(a & 0xffffffffu);
    if (kind < RTK_MPI_P2P || kind > RTK_MPI_BARRIER) panic("[pim-runtime] RT_EVENT kind %d is not an MPI event", kind);
    rtCharge(tid, kind, (uint32_t)(a >> 32), false);
}
/* ==== end 1.12.18 PIM runtime primitives ==== */

/* ---- QEMU Callbacks ---- */

/**
 * Memory access callback -- dispatches to ZSim's load/store function pointers.
 */
/* PIMID 1.12.16: QEMU 10 and later run a REP string instruction for up to 65535 iterations inside
 * one block execution. ZSim ends a phase only at a block boundary, so a long memset or memcpy then
 * piled its timing events into one phase and overflowed the 256 MB global heap. PIMID builds QEMU
 * with external/qemu-patches/0001-i386-one-rep-iteration-per-block-with-plugins.patch, under which
 * every iteration is its own block execution (as in QEMU 9.2): at most two memory accesses (MOVS,
 * CMPS) between block callbacks. More than that means the patch is missing, and the run stops. */
static int rep_string_tag;
static uint32_t rep_mem_since_bbl[MAX_VCPUS];
static bool isRepString(const uint8_t* b, size_t n) {
    bool rep = false;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c == 0xf2 || c == 0xf3) { rep = true; continue; }
        if (c == 0x66 || c == 0x67 || c == 0x26 || c == 0x2e || c == 0x36 || c == 0x3e ||
            c == 0x64 || c == 0x65 || (c >= 0x40 && c <= 0x4f)) continue;
        return rep && c >= 0xa4 && c <= 0xaf && c != 0xa8 && c != 0xa9;
    }
    return false;
}

/* (1.12.165): system.page_placement: standalone. Before the ROI baseline, every guest access of an offloading
 * co-simulation claims its page for the element the accessing thread will run on (its home: vcpu mod the
 * device's elements, 1.12.76), as a device run's elements claim the pages they touch before the ROI. The host's
 * pre-ROI requests do not reach the device's memory interfaces (the replay opens at the ROI), so the claim is
 * made here, at the access. */
static uint64_t g_ppLastPage[MAX_VCPUS];
static void pagePlaceClaim(unsigned int vcpu, uint64_t vaddr) {
    if (!g_cosim_mode || g_cosim_no_offload || vcpu >= MAX_VCPUS || vcpu_is_internal[vcpu] || device_core_list.empty()) return;
    const uint64_t pg = (vaddr >> 12) + 1;
    if (g_ppLastPage[vcpu] == pg) return;
    g_ppLastPage[vcpu] = pg;
    const uint32_t cid = device_core_list[vcpu % (uint32_t)device_core_list.size()];
    PEMemoryInterface* mi = (cid < zinfo->numCores && zinfo->cores[cid]) ? zinfo->cores[cid]->firstTouchMI() : nullptr;
    if (mi) mi->claimForPlacement(vaddr);
}

static void mem_cb(unsigned int vcpu_index,
                   qemu_plugin_meminfo_t info,
                   uint64_t vaddr,
                   void *userdata) {
    if (unlikely(zinfo && zinfo->pagePlacementStandalone) && !zinfo->hierarchy.mpiNocBaselined)
        pagePlaceClaim(vcpu_index, vaddr);   // (1.12.165)
    if (!in_roi) return;
    if (vcpu_index < MAX_VCPUS && vcpu_is_internal[vcpu_index]) return;
    if (userdata == &rep_string_tag && vcpu_index < MAX_VCPUS && ++rep_mem_since_bbl[vcpu_index] > 2)
        panic("QEMU ran a REP string instruction for several iterations inside one block execution: this QEMU "
              "lacks external/qemu-patches/0001-i386-one-rep-iteration-per-block-with-plugins.patch "
              "(docs/build.md), without which a long memset or memcpy overflows the ZSim global heap");

    uint32_t tid = getOrAssignTid(vcpu_index);
    if (tid >= MAX_THREADS) return;
    ensureThreadInit(tid);

    /* Inside an MPI comm window: skip counting the transport's own accesses
     * (keeps the core attached but the wall-clock poll invisible). */
    if (mpi_comm_window[tid].load(std::memory_order_acquire)) return;
    /* 1.12.18: runtime code is not executed on the PE (PIM runtime primitives) */
    if (g_rt.enabled && rtDropNow(tid)) { const uint32_t c = cids[tid]; if (c < MAX_THREADS) g_rtDropM[c]++; return; }

    if (vcpu_index < MAX_VCPUS) in_zsim[vcpu_index] = true;
    if (qemu_plugin_mem_is_store(info)) {
        fPtrs[tid].storePtr(tid, vaddr);
    } else {
        fPtrs[tid].loadPtr(tid, vaddr);
    }
    if (g_lockSync) {   // 1.12.18 (user ruling 2026-10-23, "model the lock word"): after the access is timed
        if (userdata == &lock_rmw_tag) lockRmw(tid, vaddr, info);
        else if (g_lockAny.load(std::memory_order_relaxed) && qemu_plugin_mem_is_store(info)) lockStore(tid, vaddr, info);
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
            if (g_detOrder && detActive() && tid < MAX_THREADS) { if (!g_detCredit[tid].exchange(1)) g_detPending.fetch_add(1); }   // (1.12.123): its re-join is awaited
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
            if ((g_mpi_thread_mode || g_spCut) && cid < MAX_THREADS)   // 1.12.18: single-process cut too
                g_coreRoiState[cid].store(3, std::memory_order_release);
            if (g_detOrder && detActive() && tid < MAX_THREADS) { if (!g_detCredit[tid].exchange(1)) g_detPending.fetch_add(1); }   // (1.12.123): its re-join is awaited
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
    if (unlikely(!g_guest_ran.load(std::memory_order_relaxed))) g_guest_ran.store(true);   // (1.12.115)
    if (vcpu_index < MAX_VCPUS) rep_mem_since_bbl[vcpu_index] = 0;
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
    /* 1.11.94 (H11 x01-qemu-zsim-plugin-3): the lazy migrate-in also runs
     * outside thread-MPI, for any thread alive when the ROI window opened
     * (g_roi_offload_window). An OpenMP worker does not pay its own
     * flush+launch: the offload is one kernel launch, charged once on the
     * opener in cosimRoiBeginOffload; and it is not counted in
     * g_deviceWorkers, which only the thread-MPI tail logic reads (OMP
     * workers born inside the window are not counted either). */
    if (unlikely(!g_mpi_thread_mode && g_roi_offload_window.load(std::memory_order_acquire) &&
                 g_cosim_mode && !g_cosim_no_offload &&
                 g_in_device_region.load(std::memory_order_acquire) &&
                 thread_domain[tid].load() == DOMAIN_HOST)) {
        /* (1.12.123) fixed thread order: a thread back from a syscall takes its turn
         * before it moves, so the move happens in turn order */
        if (g_detOrder && thread_initialized[tid] && fPtrs[tid].bblPtr == JoinBbl) Join(tid);
        {
            std::lock_guard<std::mutex> mig(g_migrateMutex);
            detachThreadForMigration(tid, DOMAIN_DEVICE);
        }
        ensureThreadInit(tid);  // device mask; may block: OUTSIDE the lock
        info("Thread %d: ROI window migrate-in (co-sim thread born before roi_begin)", tid);
    }
    if (unlikely(g_mpi_thread_mode && g_cosim_mode && !g_cosim_no_offload &&
                 (g_in_device_region.load(std::memory_order_acquire) ||
                  (g_roiClosing.load() && tid < MAX_THREADS && !g_rankEntered[tid])) &&   // (1.12.170): a late rank
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
            markRankEntered(tid);   // (1.12.170)
        }
        ensureThreadInit(tid);  // may block on the scheduler: OUTSIDE the lock
        info("Thread %d: ROI window migrate-in (co-sim MPI rank, flush+launch charged) [1.8.5]", tid);
    }

    ensureThreadInit(tid);

    /* Inside an MPI comm window: keep the core attached but do not count the
     * transport's own instructions (the wall-clock poll stays invisible). */
    if (mpi_comm_window[tid].load(std::memory_order_acquire)) return;

    /* 1.12.18 (user ruling 2026-10-23): PIM runtime primitives. A block of
     * runtime code is not fed to the core; the thread stays attached. */
    if (g_rt.enabled) {
        if (vcpu_index < MAX_VCPUS) in_zsim[vcpu_index] = true;
        const bool drop = rtOnTb(tid, tud->rtRegion, tud->rtSym, tud->tbAddr);
        if (drop) {
            /* (1.12.161): counted inside the ROI only, as the dropped memory accesses are (mem_cb records nothing
             * outside it): the threads still running after the ROI's end added to the count until the statistics
             * were printed, a host-timed amount (2-16 instructions between two runs of one co-sim cell) */
            if (rtEnsureAttached(tid) && in_roi.load()) g_rtDropI[cids[tid]] += tud->numInsns;
            g_brPending[tid] = false;
            g_ctrlPending[tid] = 0;
        }
        if (vcpu_index < MAX_VCPUS) in_zsim[vcpu_index] = false;
        if (drop) return;
    }

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
     * alu/null stay byte-identical.
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
    if (g_detQ && g_detOrder && detActive() && tid < MAX_THREADS) {   // (1.12.123): the turn ends after kDetTurnCycles
        const uint32_t cid = cids[tid];
        if (cid < zinfo->numCores && zinfo->cores[cid] && fPtrs[tid].bblPtr != JoinBbl) {
            Core* c = zinfo->cores[cid];
            if (g_detTurnEnd[tid] == 0) g_detTurnEnd[tid] = c->lockClock() + g_detQ;
            else if (c->lockClock() >= g_detTurnEnd[tid]) {
                detSettle();   // the threads this turn woke, created or moved join first, so they count below
                if (zinfo->sched->detOthersToRun()) zinfo->sched->detYield(procIdx, tid, cid);
                g_detTurnEnd[tid] = c->lockClock() + g_detQ;
            }
        }
    }
    if (vcpu_index < MAX_VCPUS) in_zsim[vcpu_index] = false;
}

/**
 * Handle an MPI magic op for the given thread.
 * Computes NoC + hierarchy latency and injects it as a synthetic BBL.
 */
static void handleMpiMagicOp(uint64_t op, uint32_t tid) {
    if (tid >= MAX_THREADS) return;
    /* 1.12.18 (2026-10-24, MPI checksums): after the ROI (stats written, the
     * program running to its exit) the MPI timing ops price nothing. The
     * post-ROI checksum reduce of gemv/spmv/stencil/triad/vadd otherwise drove
     * the NoC, contention and clock handlers on finalized state and on cores
     * the rank no longer holds (QEMU SIGSEGV / heap corruption). */
    if (pimidPostRoiFree() && op != ZSIM_MAGIC_OP_MPI_REGISTER && op != ZSIM_MAGIC_OP_MPI_TRANSPORT_RANGE) return;

    if (op == ZSIM_MAGIC_OP_MPI_TRANSPORT_RANGE) {
        /* 1.12.17 (M1): libpimid_mpi registers the memory of its own transport
         * (mailboxes, staging). PE accesses inside it are not priced: the
         * message transfer is priced once, as the injected Garnet message, and
         * the sender's read and the receiver's write of the user buffers are
         * the only memory traffic a message makes. */
        uint64_t addr = 0;
        if (zinfo && readRdx(&addr) && addr != 0) {
            const uint64_t* r = (const uint64_t*)addr;
            const uint64_t lo = r[0], bytes = r[1];
            const uint32_t i = zinfo->mpiTransportRanges;
            if (bytes == 0) return;
            if (i >= GlobSimInfo::kMaxMpiTransportRanges)
                panic("MPI transport: more than %u transport ranges registered", GlobSimInfo::kMaxMpiTransportRanges);
            zinfo->mpiTransportLo[i] = lo;
            zinfo->mpiTransportHi[i] = lo + bytes;
            __sync_synchronize();
            zinfo->mpiTransportRanges = i + 1;
            info("[mpi] transport range %u registered: [0x%lx, 0x%lx) %lu bytes -- PE accesses inside are transport, not priced (1.12.17 M1)",
                 i, (unsigned long)lo, (unsigned long)(lo + bytes), (unsigned long)bytes);
        }
        return;
    }

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

    if (op == ZSIM_MAGIC_OP_MPI_WAIT_PEER) {
        /* (1.12.176): the rank blocks on a peer inside its comm bracket: QUIESCENT ->
         * WAITING (the DRAM cut bounds it by the wake floor, EndOfPhaseActions) */
        if (g_mpi_thread_mode && cids[tid] < MAX_THREADS) {
            uint32_t q = 1;
            g_coreRoiState[cids[tid]].compare_exchange_strong(q, 4);
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
                if (!g_coreRoiState[cids[tid]].compare_exchange_strong(q, 0) && q == 4)
                    g_coreRoiState[cids[tid]].compare_exchange_strong(q, 0);   // WAITING -> ACTIVE
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
        const bool lateRankPre = g_mpi_thread_mode && tid < MAX_THREADS && !g_rankEntered[tid];   // (1.12.170)
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
                     tid < MAX_THREADS && (!g_roiClosing.load() || !g_rankEntered[tid]) &&   // (1.12.170): a late rank enters too
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
                if (g_roiClosing.load()) {
                    g_deviceWorkers.fetch_add(1);   // (1.12.170): a late rank joins the closing window's population
                } else if (!g_in_device_region.exchange(true)) {
                    g_deviceWorkers.store(1);
                } else {
                    g_deviceWorkers.fetch_add(1);
                }
                markRankEntered(tid);   // (1.12.170)
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
                     !(lateRankPre && g_rankEntered[tid]) &&   // (1.12.170): a rank that entered at this very op has not run its kernel
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
            if (g_deviceWorkers.fetch_sub(1) - 1 == 0 && g_ranksEnteredN.load() >= g_mpi_rank_count) {   // (1.12.170): every rank has entered
                /* Last rank out: window close is a bookkeeping event only --
                 * no migration, no dump (heap abort, round 3), no termination
                 * (kill-mid-protocol, rounds 4-6), no in_roi freeze (round 7).
                 * The guest runs to natural exit; the exit path dumps. */
                info("Thread %d: ROI window drained (last rank out)", tid);
                /* 1.12.3 (ruling 1 (a)): the device kernel window closes HERE,
                 * at the last rank's closing-barrier marker. Snapshot the
                 * power-gating residency window (phase index + active phases)
                 * so the priced window ends with the kernel and the tail that
                 * follows stays out of it. Bookkeeping only: no dump, no
                 * termination, no freeze (the 1.8.6 rounds above). */
                zinfo->pgres.markKernelClose(zinfo->numPhases);
                roiWallCloseMpi("co-sim kernel close");   // 1.12.18
                info("Thread %d: device kernel window closed at phase %lu (co-sim MPI, the last rank's marker; the tail is outside the priced window) [1.12.3]",
                     tid, (unsigned long)zinfo->numPhases);
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
            flushHeldWritebacks();   // 1.12.18 (B3)
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
        /* 1.11.97 (gate 1207B): record which barrier generation this arrival
         * belongs to and the ROI-clock origin in force at that generation's
         * first arrival (MPI_ADVANCE checks it). */
        if (tid < MAX_THREADS) {
            const uint64_t k = ++g_rankBarGen[tid];
            futex_lock(&g_barGenLock);
            if (g_barFirstBaseGen.find(k) == g_barFirstBaseGen.end())
                g_barFirstBaseGen[k] = g_roiBaseGen.load();
            futex_unlock(&g_barGenLock);
            if (g_mpi_thread_mode) cutWakeBarrier(tid, k, true);   // (1.12.176): the DRAM cut's wake floor
        }
        if (getenv("PIMID_DEBUG_RDV")) {   /* diagnostic: the clock axis of a barrier arrival */
            static std::atomic<int> nT{0};
            if (nT.fetch_add(1) < 400) {
                uint32_t cid = (tid < MAX_THREADS) ? cids[tid] : tid;
                uint64_t raw = cores[tid] ? cores[tid]->getCycles() : 0;
                info("[rdvdbg] TIME tid=%u cid=%u raw=%lu base[tid]=%lu base[cid]=%lu roiBase=%lu roiRel=%lu",
                     tid, cid, (unsigned long)raw, (unsigned long)mpi_roi_base_cyc[tid],
                     (unsigned long)((cid < MAX_THREADS) ? mpi_roi_base_cyc[cid] : 0),
                     (unsigned long)zinfo->hierarchy.mpiNocRoiBase, (unsigned long)gp->sim_now);
            }
        }
        /* 1.11.97 (x01-qemu-zsim-plugin-5): MPI_TIME is the thread-mode
         * barrier's ARRIVAL op. Device scope counts arrivals per barrier
         * generation; the N-th arrival of the first generation completed
         * after the opener's roi_end is the last rank's roi_end, and the
         * stats freeze there (see g_devRoiClosing). */
        if (devScopeLastRankRule()) {
            uint32_t n = g_devBarArrivals.fetch_add(1) + 1;
            if (n >= g_mpi_rank_count) {
                g_devBarArrivals.store(0);   // generation complete
                if (g_devRoiClosing.load() && !g_devRoiFrozen.exchange(true)) {
                    info("Thread %d: device-scope ROI end at the last rank's arrival "
                         "(%u ranks) [1.11.97]", tid, g_mpi_rank_count);
                    if (tid < MAX_THREADS && cids[tid] < MAX_THREADS)
                        g_coreRoiState[cids[tid]].store(2, std::memory_order_release);
                    in_roi.store(false);
                    dumpTerminationStats();
                    /* 1.12.18 (2026-10-24, MPI checksums): every rank has arrived, the
                     * stats are written; the program now runs to its own exit
                     * unsimulated (pimidPostRoiFree), as after a shared-memory ROI,
                     * so rank 0's BENCH_CHECKSUM is printed. It used to end here at
                     * the next Join (_exit before the print). */
                    g_postRoiSince = postRoiWallSec();
                    __sync_synchronize();
                    g_postRoiFree = 1;
                    if (zinfo->sched) zinfo->sched->releaseForPostRoi();   // (1.12.121): no thread is left waiting in the barrier
                    zinfo->terminationConditionMet = true;
                }
            }
        }
        return;
    }
    if (op == ZSIM_MAGIC_OP_MPI_ADVANCE) {
        uint64_t target = cyclesFromDomain(params.sim_send_time,
                                           params.sim_now_freq_mhz,
                                           coreFreqMHzFor(tid));
        uint64_t cur = roiRelCycles(tid);
        Core* cc = cores[tid];
        /* 1.11.97 (gate 1207B, root cause of the MPI cycle inflation): the
         * shim publishes every rank's arrival (MPI_TIME = roiRelCycles), takes
         * the max as the release, and each rank then ADVANCEs to it. The ROI
         * clock origin can move in between: the ENTRY barrier synthesizes the
         * ROI baseline in the BARRIER op that follows the first rank's
         * ADVANCE (H10 "the entry barrier rebases as designed"), and rank 0's
         * roi_begin re-baselines every core right after. A rank whose ADVANCE
         * comes after such a re-baseline compared a release on the OLD origin
         * (rank 0's whole pre-ROI preparation, 105 M cycles on the ALU gate
         * shape) with its clock on the NEW origin, and was charged the
         * preparation inside the ROI -- rank 0 included, advancing to its own
         * arrival; the offset then leaked into every rank (ALU stencil 4-rank:
         * 1.0 M -> 106 M cycles; which rank re-baselined first is schedule-
         * dependent, so the damage varied run to run). A release is therefore
         * charged only when the origin is the one in force at its barrier
         * generation's first arrival; otherwise it is DISCARDED (the wait it
         * describes is pre-ROI, before the new origin). Also discarded: a
         * thread without a valid core, and anything after termination. */
        bool staleAxis = false;
        if (g_mpi_thread_mode && tid < MAX_THREADS) cutWakeBarrier(tid, g_rankBarGen[tid], false);   // (1.12.176)
        if (tid < MAX_THREADS) {
            const uint64_t k = g_rankBarGen[tid];
            futex_lock(&g_barGenLock);
            auto it = g_barFirstBaseGen.find(k);
            staleAxis = (it == g_barFirstBaseGen.end()) || (it->second != g_roiBaseGen.load());
            while (!g_barFirstBaseGen.empty() && g_barFirstBaseGen.begin()->first + 64 < k)
                g_barFirstBaseGen.erase(g_barFirstBaseGen.begin());
            futex_unlock(&g_barGenLock);
        }
        const uint32_t advCid = (tid < MAX_THREADS) ? cids[tid] : tid;
        const bool noCore = (advCid >= zinfo->numCores) || zinfo->terminationConditionMet;
        /* (1.12.176): the barrier whose BARRIER op synthesizes the ROI baseline (the entry barrier,
         * mpi_roi_baselined still false) releases at a clock of the pre-ROI origin: the max
         * arrival includes rank 0's whole data preparation. Every rank whose ADVANCE comes after
         * the synthesis discards it as stale (above); the one rank whose ADVANCE came first (the
         * barrier's last arrival, which does not wait) used to be charged it: its core ran up to
         * the preparation's length ahead of the others' (25 M cycles on a smoke stream_triad), took
         * no turn until global time caught up, and ran its kernel alone after the others had
         * finished theirs, so their accesses were priced by the bootstrap. Discarded for every
         * rank alike. */
        const bool preRoiRelease = g_mpi_thread_mode && !mpi_roi_baselined;
        if (getenv("PIMID_DEBUG_RDV") && (staleAxis || noCore || preRoiRelease)) {
            static std::atomic<int> nD{0};
            if (nD.fetch_add(1) < 64)
                info("[rdvdbg] ADVANCE tid=%u DISCARDED (%s): release=%lu cur=%lu", tid,
                     staleAxis ? "ROI clock re-baselined since the barrier's first arrival"
                               : (noCore ? "no core / terminated" : "entry barrier: its release is on the pre-ROI origin"),
                     (unsigned long)target, (unsigned long)cur);
        }
        if (staleAxis || noCore || preRoiRelease) return;
        if (getenv("PIMID_DEBUG_RDV")) {   /* diagnostic: what the barrier release charges */
            static std::atomic<int> nA{0};
            if (nA.fetch_add(1) < 400) {
                uint32_t cid = (tid < MAX_THREADS) ? cids[tid] : tid;
                info("[rdvdbg] ADVANCE tid=%u cid=%u release=%lu cur=%lu delta=%ld",
                     tid, cid, (unsigned long)target, (unsigned long)cur, (long)(target - cur));
            }
        }
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
    /* 1.11.97 (review H10): a communication call NEVER baselines the ROI.
     * This block synthesised a per-rank baseline at the first SEND/RECV, so
     * ranks 1..N-1 of gemv_mpi counted their data preparation as ROI work.
     * roi_begin (every rank calls it; in thread mode it sweeps all cores) and
     * the entry barrier's rebase are the two designed baselines. */

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
    if (g_mpi_thread_mode) cutWakeMessage(tid, isRecv, params.src_pe, params.dst_pe);   // (1.12.176): the DRAM cut's wake floor

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

    /* 2. NoC latency (if Garnet is available)
     *
     * 1.11.97 (x01-qemu-zsim-plugin-6, ruled): ONE MESSAGE IS INJECTED ONCE,
     * BY ITS SENDER. Both libpimid_mpi timing ops reach this point for every
     * message -- MPI_SEND from the sending rank, MPI_RECV from the receiving
     * rank -- and both used to put the message's ceil(size/64) packets on the
     * device NoC (thread mode: recordBatchAccess into the batch replay;
     * process mode: live accessNetwork). Every MPI message was therefore
     * charged twice: twice the packets, flits, router traversals and NoC
     * energy. Physically the bytes cross the fabric once, launched by the
     * sender's NI, so the sender injects; the receiver only PRICES the same
     * path to place its rendezvous arrival (send_time + contend_wait + hier
     * + noc) and injects nothing. The shared shm log below was already
     * sender-only (one publication per message); this makes the device NoC
     * agree with it.
     *   thread mode (detailed): the receiver skips recordBatchAccess and
     *     keeps the deterministic RTT probe, the same pricing the sender
     *     uses, so its arrival time is unchanged. Its probe is forced
     *     uncounted; the SENDER's probe still follows the htree-9 knob
     *     PIMID_NOC_PROBE_UNCOUNTED (counted by default).
     *   analytic NoC: the receiver reads the sender-cached getRTT value
     *     through getRTTAnalyticUncounted (same RTT, no packet counted).
     *   process mode (detailed, live): the receiver prices each packet with
     *     the uncounted analytic one-way latency instead of a live
     *     injection; its rank-local network carries only what that rank
     *     sent. Its arrival is now zero-contention analytic where it used to
     *     be its own (second) live injection. Process mode is the per-rank
     *     trace-gen path only; thread mode is the exec-method MPI model. */
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
            if (!isRecv)   // 1.11.97 (plugin-6): sender-side injection only
                for (uint32_t p = 0; p < numPackets; p++)
                    gn->recordBatchAccess(srcNode, dstNode, stamp + p);
            /* Deterministic pricing: static analytic RTT only (the EWMA
             * read is wall-order-dependent; see pe_memory_interface).
             * 1.11.94 (x02-zsim-garnet-htree-9): this is a PROBE -- the
             * message is already counted by the replay above. getRTTProbe
             * counts nothing under PIMID_NOC_PROBE_UNCOUNTED=1 and is
             * getRTT otherwise (the 1.11.93 default). */
            /* 1.11.97 (plugin-6): the receiver's probe is always uncounted. */
            uint32_t rtt = gn->getRTTProbe(
                std::to_string(params.src_pe).c_str(),
                std::to_string(params.dst_pe).c_str(),
                /*uncounted=*/isRecv);
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
                /* 1.11.97 (plugin-6): the receiver prices, it does not inject. */
                uint32_t oneWay = isRecv
                    ? anet->analyticalOneWayUncounted(srcNode, dstNode)
                    : anet->accessNetwork(srcNode, dstNode,
                                          issueTime + totalOneWay);
                totalOneWay += oneWay;
            }
            nocLat = 2 * totalOneWay;  /* RTT */
        } else {
            /* Simple mode: analytical RTT with message size scaling.
             * 1.11.97 (plugin-6): getRTT counts one packet per call, so the
             * receiver reads the sender-cached RTT uncounted instead. */
            nocLat = isRecv
                ? gn->getRTTAnalyticUncounted(
                      std::to_string(params.src_pe).c_str(),
                      std::to_string(params.dst_pe).c_str())
                : gn->getRTT(
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
static inline bool isSpmMagicOp(uint64_t op) {   // 1.12.18
    return op >= ZSIM_MAGIC_OP_SPM_QUERY && op <= ZSIM_MAGIC_OP_SPM_DMA;
}

/* 1.12.18: the scratchpad ops (benchmarks/include/pimid_spm.h). %rdx points to
 * a pimid_spm_msg_t {a, b, c, d, ret0, ret1}; guest and host share the address
 * space in user mode, so it is used directly. A DMA the core accepts is priced
 * by the core (spmDma) and its bytes are moved here, so the transfer executes
 * no guest instructions. */
static void handleSpmMagicOp(uint64_t op, uint32_t tid) {
    uint64_t addr = 0;
    if (!zinfo || tid >= MAX_THREADS || !readRdx(&addr) || addr == 0) return;
    volatile uint64_t* m = (volatile uint64_t*)addr;
    Core* c = cores[tid];
    /* (1.12.164): a host-side thread of an offloading co-simulation opens the device kernel's scratchpad */
    const bool hostSide = g_cosim_mode && !g_cosim_no_offload && thread_domain[tid].load() == DOMAIN_HOST;
    if (op == ZSIM_MAGIC_OP_SPM_QUERY) {
        uint64_t cap = 0, mode = 0;
        if (hostSide) { Core* d = spmDeviceTemplate(); if (d) d->spmQuery(&cap, &mode); }
        else if (c) c->spmQuery(&cap, &mode);
        m[4] = cap; m[5] = mode;
        return;
    }
    if (op == ZSIM_MAGIC_OP_SPM_BIND && hostSide) {
        if (m[1] == 0) {   // close: release the PE it was applied to
            if (g_spmPendOn[tid]) g_spmPendOn[tid]->spmBind(0, 0, 0);
            g_spmPendSet[tid] = false; g_spmPendOn[tid] = nullptr;
            m[4] = 1;
            return;
        }
        uint64_t cap = 0, mode = 0;
        Core* d = spmDeviceTemplate();
        if (d) d->spmQuery(&cap, &mode);
        const bool ok = cap > 0 && m[1] <= cap;
        if (ok) {
            g_spmPend[tid] = SpmPending{m[0], m[1], m[2]};
            g_spmPendSet[tid] = true; g_spmPendOn[tid] = nullptr;
            g_spmPendAny.store(true, std::memory_order_relaxed);
            info("Thread %u: scratchpad [0x%lx, 0x%lx) opened on the host before the offload; bound to the "
                 "device PE the thread joins (co-sim)", tid, (unsigned long)m[0], (unsigned long)(m[0] + m[1]));
        }
        m[4] = ok ? 1 : 0;
        return;
    }
    if (op == ZSIM_MAGIC_OP_SPM_BIND) {
        const bool ok = c && c->spmBind(m[0], m[1], m[2]);
        m[4] = ok ? 1 : 0;
        if (ok && m[1]) info("Thread %u: scratchpad bound [0x%lx, 0x%lx) on core %u", tid,
                             (unsigned long)m[0], (unsigned long)(m[0] + m[1]), c->getSrcId());
        return;
    }
    const uint64_t dst = m[0], src = m[1], bytes = m[2], dir = m[3];
    if (c && c->spmDma(dst, src, bytes, dir == 1)) {
        memmove((void*)dst, (const void*)src, bytes);
        m[4] = 1;
    } else if (hostSide && g_spmPendSet[tid]) {
        /* (1.12.170): a thread that holds a scratchpad opened for the device but stays on the host has no DMA engine
         * to use: the block is moved here and is not priced on a device PE (every thread-MPI rank of an
         * offloading co-simulation moves to its device PE before its kernel, so this is the exceptional path).
         * Answering 0 would abort the guest, which stalls the other ranks in their barriers. */
        memmove((void*)dst, (const void*)src, bytes);
        m[4] = 1;
    } else {
        m[4] = 0;
    }
}

/* (1.12.164): the device's PE answers a host-side scratchpad query (every PE of the device has the same one). */
static Core* spmDeviceTemplate() {
    if (!zinfo || device_core_list.empty()) return nullptr;
    return zinfo->cores[device_core_list[0]];
}
/* (1.12.164): a thread that joins a device PE with a scratchpad opened before the offload binds it there. */
static void spmApplyPending(uint32_t tid) {
    if (tid >= MAX_THREADS || !g_spmPendSet[tid]) return;
    Core* c = cores[tid];
    if (!c || c == g_spmPendOn[tid] || thread_domain[tid].load() != DOMAIN_DEVICE) return;
    const SpmPending& p = g_spmPend[tid];
    if (g_spmPendOn[tid]) g_spmPendOn[tid]->spmBind(0, 0, 0);
    if (c->spmBind(p.lo, p.bytes, p.cmd)) {
        g_spmPendOn[tid] = c;
        info("Thread %u: scratchpad bound [0x%lx, 0x%lx) on core %u (co-sim: opened before the offload)", tid,
             (unsigned long)p.lo, (unsigned long)(p.lo + p.bytes), c->getSrcId());
    } else {
        panic("Thread %u: the scratchpad opened before the offload [0x%lx, +%lu) cannot be bound on core %u",
              tid, (unsigned long)p.lo, (unsigned long)p.bytes, c->getSrcId());
    }
}

static inline bool isMpiMagicOp(uint64_t op) {
    return (op >= ZSIM_MAGIC_OP_MPI_REGISTER && op <= ZSIM_MAGIC_OP_MPI_TRANSPORT_RANGE) /* 1.11.97 (x01-qemu-zsim-plugin-10): MPI_TIME 2058 and MPI_ADVANCE 2059 dispatch; the barrier's exit-time equalisation was dead */
           || op == ZSIM_MAGIC_OP_MPI_WAIT_PEER;
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
/* (user ruling 2026-10-27: serial links calibrated): THE LINK'S ASPM L1. A transfer that finds the
 * host-device link idle for longer than its L1 entry timer finds it in L1 and
 * pays the L1 exit latency first; the idle time up to the timer was spent in L0.
 * One link: the state is global, on the reference (phase) clock the bridge's
 * cycles are in. The counters feed the link's idle energy (xingL1Exits,
 * xingL0IdleNs); the residency in L1 is the rest of the run. */
static std::mutex g_linkStateMutex;
static bool g_linkSeen = false;
static uint64_t g_linkFreeAt = 0;
static uint32_t linkL1ExitCharge(uint32_t xferCycles) {
    if (!zinfo || zinfo->bridge.l1EntryCycles == 0) return 0;
    std::lock_guard<std::mutex> g(g_linkStateMutex);
    const uint64_t now = zinfo->globPhaseCycles;
    uint32_t exitc = 0;
    if (!g_linkSeen || now > g_linkFreeAt + zinfo->bridge.l1EntryCycles) {
        exitc = zinfo->bridge.l1ExitCycles;
        __sync_fetch_and_add(&zinfo->xing.l1Exits, 1);
    }
    if (g_linkSeen && now > g_linkFreeAt) {
        const uint64_t l0 = std::min<uint64_t>(now - g_linkFreeAt, zinfo->bridge.l1EntryCycles);
        __sync_fetch_and_add(&zinfo->xing.l0IdleNs, l0 * 1000ull / std::max<uint32_t>(1, zinfo->freqMHz));
    }
    g_linkSeen = true;
    g_linkFreeAt = std::max(g_linkFreeAt, now) + exitc + xferCycles;
    return exitc;
}

static uint32_t getBridgeLatency(uint32_t transfer_bytes = 0) {
    if (!zinfo || !zinfo->bridge.enabled) return 0;
    uint32_t base = zinfo->bridge.phyLatencyCycles + zinfo->bridge.protocolOverheadCycles;
    uint32_t serCycles = 0;
    /* (user ruling 2026-10-27: serial links calibrated): the link's framing crosses with the payload */
    const uint32_t wire_bytes = transfer_bytes + zinfo->bridge.headerBytes;
    if (zinfo->bridge.bytesPerCycle > 0.0 && wire_bytes > 0) {
        serCycles = static_cast<uint32_t>(
            std::ceil(static_cast<double>(wire_bytes) / zinfo->bridge.bytesPerCycle));
    }
    return base + serCycles + linkL1ExitCharge(base + serCycles);
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
static uint64_t measureAndCleanFlushDelta(uint64_t* cleanedLines, std::vector<Address>* linesOut) {
    *cleanedLines = 0;
    if (!zinfo) return 0;
    std::unordered_set<Address> distinct;
    const g_vector<Cache*>& all = zsimAllCaches();
    /* 1.12.18: the writebacks leave in the WALK's order -- caches in
     * registry order, each cache's lines in line-id (set-major) order, a line
     * at its first sighting -- deterministic, and the order a hardware flush
     * emits them. Address order (the 1.12.18 bug-7 sort) sent each 64 MB
     * per-thread malloc arena's lines in one run: with the PIM device's
     * contiguous per-bank layout (pagesPerUnit) every run is ONE bank, so a
     * controller queue full of one bank's writes drained the whole flush one
     * bank at a time (cosim_DDR5_mpi_gemv: 61,002 lines, 2.8 M DRAM cycles,
     * ~46 per write, the kernel then stalled ~12x). Arenas share their set
     * indices, so the walk alternates between them, and across the banks. */
    for (Cache* c : all) {
        if (!c) continue;
        *cleanedLines += c->flushDirtyLines(distinct, linesOut);
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
                               uint64_t* cleanedLines, uint64_t* walkIndex,
                               std::vector<Address>* linesOut) {
    if (!zinfo || !zinfo->coherence.enabled) return false;
    if (zinfo->coherence.mode != 0) return false;  // separate = cache bypass, no flush
    /* 1.11.62 (ruling 5b): the gate hook. Read once; the fast path is the
     * common case where it is off and this is a predictable load. */
    static const bool flushTrace = (getenv("PIMID_FLUSH_TRACE") != nullptr);
    std::lock_guard<std::mutex> lk(g_flushWalkMutex);
    uint64_t cleaned = 0;
    const uint64_t delta = measureAndCleanFlushDelta(&cleaned, linesOut);
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
    std::vector<Address> wbLines;   // 1.12.18 (bug 7)
    if (!takeFlushWalkDelta(tid, &deltaBytes, &cumulativeBytes, &cleanedLines,
                            &walkIdx, zinfo->flushHostMems ? &wbLines : nullptr))
        return;
    /* 1.12.18: (bug 7) THE WRITEBACKS REACH THE HOST MEMORY CONTROLLER. The
     * walk cleaned these lines, i.e. wrote them back to memory; they were
     * priced array-side only (a 0.5 row-miss stand-in) and never reached the
     * host controller or its replay. Each line now goes to its controller
     * (the SplitAddrMemory rule, line % n, controller-local line / n) at the
     * flush instant, in address order (deterministic), outside every walk
     * lock: one write in the controller's count (McPAT MC memory_writes, the
     * host-originated array class) and, with the host replayed, one write
     * request (source_id 1) whose ACTs and row state the controller counts.
     * The flush runs at the co-sim roi_begin offload, after the traffic
     * counters are rebased, so it lands in the ROI window. */
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
    if (zinfo->flushHostMems && !zinfo->flushHostMems->empty() && !wbLines.empty()) {
        /* 1.12.18: the writebacks leave in the WALK's order
         * (measureAndCleanFlushDelta), not sorted, and PACED over the flush
         * the core is charged for: line i at cyc + i x flushCyc / lines. All
         * at one instant (the bug-7 form) gave every line one replay stamp,
         * and the replay orders equal stamps by address: each 64 MB
         * per-thread malloc arena became one run of writes to ONE bank (the
         * device's contiguous per-bank layout, pagesPerUnit), the controller
         * queue held ~1.4 banks, and cosim_DDR5_mpi_gemv drained its 61,002
         * flush lines one bank at a time (2.7 M DRAM cycles, 0.035 commands
         * per cycle; the kernel then ran ~12x long). */
        const g_vector<SimpleMemory*>& hm = *zinfo->flushHostMems;
        const uint64_t n = hm.size();
        const uint64_t cyc = cores[tid] ? cores[tid]->getCycles() : 0;
        const uint32_t src = getCid(tid);
        /* 1.12.18 (bug B3): thread-MPI co-sim flushes at the KERNEL-ENTRY
         * barrier, and the first rank there flushes (walk #1: the whole
         * pre-ROI footprint, ~61k lines on the smoke gemv) BEFORE the barrier
         * takes the ROI baseline: its writebacks were rebased out of the host
         * controller's counts and never reached the replay (no record before
         * mpiNocBaselined), while shared-memory co-sim flushes after its
         * roi_begin rebase. They are held here and sent the instant the
         * baseline is taken (flushHeldWritebacks), on the flushing core's
         * clock then: inside the ROI, counted and replayed as in phase mode. */
        if (g_mpi_thread_mode && !mpi_roi_baselined) {
            std::lock_guard<std::mutex> g(g_heldFlushMutex);
            for (size_t i = 0; i < wbLines.size(); i++)
                g_heldFlush.push_back(HeldFlushLine{wbLines[i], src, (uint64_t)i * flushCyc / wbLines.size()});
        } else {
            for (size_t i = 0; i < wbLines.size(); i++)
                hm[wbLines[i] % n]->flushWriteback(wbLines[i] / n, cyc + (uint64_t)i * flushCyc / wbLines.size(), src);
        }
        __sync_fetch_and_add(&zinfo->xing.flushMcLines, (uint64_t)wbLines.size());
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
    if (g_spCut && !g_mpi_thread_mode && tid < MAX_THREADS && cids[tid] < zinfo->numCores && zinfo->cores[cids[tid]]) {
        /* 1.12.18 (ruling 2026-10-24): the device's ROI starts at the doorbell
         * on the host's ROI-relative clock (PEMemoryInterface::spDevShift). */
        const uint32_t hc = cids[tid];
        PEMemoryInterface::spDevShift() = PEMemoryInterface::cutRoiRel(hc, zinfo->cores[hc]->pimidRawCycle());   // 1.12.18 (W9): the raw clock (cutRoiRel converts)
    }
    thread_domain[tid].store(DOMAIN_DEVICE);
    // 1.8.4 ROI window: first opener initializes the population; any later
    // opener (kernels where every rank calls roi_begin) just joins it.
    if (!g_in_device_region.exchange(true)) {
        g_deviceWorkers.store(1);
    } else {
        g_deviceWorkers.fetch_add(1);
    }
    if (g_mpi_thread_mode) markRankEntered(tid);   // (1.12.170)
    g_roiClosing.store(false);
    if (!g_mpi_thread_mode)
        g_roi_offload_window.store(true, std::memory_order_release);   // 1.11.94 (H11)
    ++offload_count;
    if (thread_initialized[tid]) {
        uint32_t cid = cids[tid];
        if (cid != INVALID_CID && cid != UNINITIALIZED_CID) {
            if (g_detOrder && detActive() && tid < MAX_THREADS) { if (!g_detCredit[tid].exchange(1)) g_detPending.fetch_add(1); }   // (1.12.123): its re-join is awaited
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
    if (isSpmMagicOp(op)) {   // 1.12.18
        if (tid < MAX_THREADS) {
            ensureThreadInit(tid);
            handleSpmMagicOp(op, tid);
        }
        return;
    }
    if (isRtMagicOp(op)) {   // 1.12.18: PIM runtime primitives (thread-MPI shim)
        if (tid < MAX_THREADS) {
            ensureThreadInit(tid);
            handleRtMagicOp(op, tid);
        }
        return;
    }

    // Extract low 32 bits as opcode, high 32 bits as optional payload size
    uint32_t opcode = static_cast<uint32_t>(op & 0xFFFFFFFF);
    uint32_t payload_size = static_cast<uint32_t>(op >> 32);

    if (opcode == ZSIM_MAGIC_OP_ROI_BEGIN) g_roi_begin_seen = true;   // (1.12.115): see SimEnd
    if (opcode == ZSIM_MAGIC_OP_ROI_BEGIN) stackAtRoiBegin(tid);   // 1.12.17: the main thread's stack
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
                /* 1.11.97 (review H10, refined by gate 1207D): did the ENTRY
                 * barrier already rebase every core? In all thirteen MPI
                 * kernels the first MPI_Barrier is the line before rank 0's
                 * zsim_roi_begin(), after the data preparation, so the
                 * barrier's synthesized baseline IS the kernel entry of every
                 * rank (H10: "the entry barrier rebases as designed"). Sweeping
                 * every core again here caught ranks 1..N-1 at whatever point
                 * of their kernel they had reached when rank 0 got here -- the
                 * 1.6 jitter the comment above warns about -- and dropped those
                 * instructions (gemv_mpi per-rank ROI instructions max/min
                 * 1.015 / 1.087 / 1.199 across three runs; rank 0 exact). The
                 * sweep, the clock snapshot and the traffic rebase therefore
                 * run here only when no barrier has rebased yet (a kernel
                 * with no entry barrier); otherwise rank 0 alone re-baselines
                 * its own core (above). */
                const bool barrierBaselined = mpi_roi_baselined;
                in_roi.store(true);
                mpi_roi_baselined = true;
                if (barrierBaselined) {
                    info("[roi] thread-MPI roi_begin after the entry barrier's baseline: rank %u re-baselined its own core; "
                         "the other ranks keep their kernel-entry baseline (1.11.97, H10 refined)", tid);
                    cosimRoiBeginOffload(tid);
                    return;
                }
                /* 1.11.97 (review H10): no barrier has rebased: the first
                 * roi_begin in thread mode baselines EVERY core, not only the
                 * calling rank's: the other ranks are threads of this process
                 * and would otherwise keep their pre-ROI (data preparation)
                 * cycles in the window. */
                for (uint32_t c = 0; c < zinfo->numCores; c++)
                    if (zinfo->cores[c]) zinfo->cores[c]->markRoiBegin();
                snapshotRoiBaseCyc();
                roiRebaseTrafficCounters("thread-MPI first roi_begin");   // 1.11.90
                flushHeldWritebacks();   // 1.12.18 (B3)
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
        roiWallBegin(tid);   // 1.12.18: run time = elapsed wall clock (before the replay window opens)
        // This rank got a real roi_begin -> don't also synthesize a baseline at
        // the first MPI op (see handleMpiMagicOp). rank 0 takes this path.
        mpi_roi_baselined = true;
        snapshotRoiBaseCyc();
        if (g_spCut) {   // 1.12.18 (ruling 2026-10-24): each device PE's ROI origin starts at its own baseline (PEMemoryInterface::cutRoiRel)
            for (uint32_t c = 0; c < zinfo->numCores; c++)
                if (zinfo->cores[c] && zinfo->cores[c]->wakeStampable()) PEMemoryInterface::spSetOrigin(c, zinfo->cores[c]->getRoiBaseCycle());
            PEMemoryInterface::clkClearOrigins();   // 1.12.21: in-order / OOO cores start on their snapshot origin
        }
        roiRebaseTrafficCounters("roi_begin");   // 1.11.90
        // Co-sim: the ROI IS the offload region. The launching thread (and
        // every thread spawned inside the region) executes on the DEVICE;
        // out-of-ROI code executes on the host. Ordinary workloads are
        // co-sim workloads -- there is no special kind.
        cosimRoiBeginOffload(tid);
    } else if (opcode == ZSIM_MAGIC_OP_ROI_END) {
        if (!g_mpi_thread_mode) roiWallEnd(tid, "roi_end");   // 1.12.18: on the core that ran the ROI, before any migration
        // Co-sim: return the launching thread to a host core BEFORE the
        // termination flag, so the host core rejoin fast-forwards to the
        // global phase clock and host cycles absorb the device execution.
        if (g_cosim_mode && !g_cosim_no_offload && tid < MAX_THREADS) {
            g_in_device_region.store(false);
            g_roi_offload_window.store(false, std::memory_order_release);   // 1.11.94 (H11)
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
                        if (g_detOrder && detActive() && tid < MAX_THREADS) { if (!g_detCredit[tid].exchange(1)) g_detPending.fetch_add(1); }   // (1.12.123): its re-join is awaited
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
                (g_deviceWorkers.load() > 0 || g_ranksEnteredN.load() < g_mpi_rank_count)) {   // (1.12.170): a rank still owes its kernel
                /* 1.8.4 ROI window: the window is closing but other ranks
                 * are still computing on device PEs. The LAST rank out (at
                 * its barrier arrival, handleMpiMagicOp) freezes stats;
                 * freezing here at the opener's roi_end would truncate
                 * every other rank's compute tail. */
                return;
            }
            if (devScopeLastRankRule()) {
                /* 1.11.97 (x01-qemu-zsim-plugin-5): device scope, same
                 * reason. The opener's roi_end only marks the ROI as
                 * closing; the N-th arrival at the next barrier (MPI_TIME
                 * handler) freezes the stats. See g_devRoiClosing. */
                g_devRoiClosing.store(true);
                info("Thread %d: ROI end (device-scope MPI opener): closing, "
                     "stats freeze at the last rank's barrier arrival [1.11.97]", tid);
                return;
            }
            // This rank finished its ROI -> EXITED, so the epoch cut advances over
            // the still-running peers instead of pinning on this (now frozen) core.
            if (tid < MAX_THREADS && cids[tid] < MAX_THREADS)
                g_coreRoiState[cids[tid]].store(2, std::memory_order_release);
            in_roi.store(false);
            dumpTerminationStats();
            /* 1.12.18 (2026-10-24, MPI checksums): the last rank is out of its ROI, the
             * stats are written; the program now runs to its own exit
             * unsimulated (pimidPostRoiFree), as after a shared-memory ROI,
             * so rank 0's BENCH_CHECKSUM is printed. It used to end here at
             * the next Join (_exit before the print). */
            g_postRoiSince = postRoiWallSec();
            __sync_synchronize();
            g_postRoiFree = 1;
            if (zinfo->sched) zinfo->sched->releaseForPostRoi();   // (1.12.121): no thread is left waiting in the barrier
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
            /* 1.12.18: write the stats now; the program then runs to its own
             * exit (pimidPostRoiFree), so its output after the ROI survives. */
            dumpTerminationStats();
            g_postRoiSince = postRoiWallSec();
            __sync_synchronize();
            g_postRoiFree = 1;
            if (zinfo->sched) zinfo->sched->releaseForPostRoi();   // (1.12.121): no thread is left waiting in the barrier
            zinfo->terminationConditionMet = true;
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
                    if (g_detOrder && detActive() && tid < MAX_THREADS) { if (!g_detCredit[tid].exchange(1)) g_detPending.fetch_add(1); }   // (1.12.123): its re-join is awaited
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
                    if (g_detOrder && detActive() && tid < MAX_THREADS) { if (!g_detCredit[tid].exchange(1)) g_detPending.fetch_add(1); }   // (1.12.123): its re-join is awaited
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
    if (isSpmMagicOp(op)) {   // 1.12.18
        ensureThreadInit(tid);
        handleSpmMagicOp(op, tid);
        return;
    }
    if (isRtMagicOp(op)) {   // 1.12.18: PIM runtime primitives (thread-MPI shim)
        ensureThreadInit(tid);
        handleRtMagicOp(op, tid);
        return;
    }
    // Extract low 32 bits as opcode, high 32 bits as optional payload size
    uint32_t opcode = static_cast<uint32_t>(op & 0xFFFFFFFF);
    uint32_t payload_size = static_cast<uint32_t>(op >> 32);

    if (opcode == ZSIM_MAGIC_OP_ROI_BEGIN) g_roi_begin_seen = true;   // (1.12.115): see SimEnd
    if (opcode == ZSIM_MAGIC_OP_ROI_BEGIN) stackAtRoiBegin(tid);   // 1.12.17: the main thread's stack
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
                /* 1.11.97 (H10 refined, gate 1207D): the same rule as the
                 * other magic-op entry point -- when the entry barrier has
                 * already rebased every core at kernel entry, rank 0 keeps its
                 * own re-baseline (above) and nothing else is re-snapshotted;
                 * otherwise every core is baselined here (H10). */
                const bool barrierBaselined = mpi_roi_baselined;
                in_roi.store(true);
                mpi_roi_baselined = true;
                if (barrierBaselined) {
                    info("[roi] thread-MPI roi_begin after the entry barrier's baseline: rank %u re-baselined its own core; "
                         "the other ranks keep their kernel-entry baseline (1.11.97, H10 refined)", tid);
                    cosimRoiBeginOffload(tid);
                    return;
                }
                for (uint32_t c = 0; c < zinfo->numCores; c++)
                    if (zinfo->cores[c]) zinfo->cores[c]->markRoiBegin();   // 1.11.97 (H10): no barrier has rebased
                snapshotRoiBaseCyc();
                roiRebaseTrafficCounters("thread-MPI first roi_begin");   // 1.11.90
                flushHeldWritebacks();   // 1.12.18 (B3)
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
        roiWallBegin(tid);   // 1.12.18: run time = elapsed wall clock (before the replay window opens)
        // This rank got a real roi_begin -> don't also synthesize a baseline at
        // the first MPI op (see handleMpiMagicOp). rank 0 takes this path.
        mpi_roi_baselined = true;
        snapshotRoiBaseCyc();
        if (g_spCut) {   // 1.12.18 (ruling 2026-10-24): each device PE's ROI origin starts at its own baseline (PEMemoryInterface::cutRoiRel)
            for (uint32_t c = 0; c < zinfo->numCores; c++)
                if (zinfo->cores[c] && zinfo->cores[c]->wakeStampable()) PEMemoryInterface::spSetOrigin(c, zinfo->cores[c]->getRoiBaseCycle());
            PEMemoryInterface::clkClearOrigins();   // 1.12.21: in-order / OOO cores start on their snapshot origin
        }
        roiRebaseTrafficCounters("roi_begin");   // 1.11.90
        // Co-sim: ROI = offload region (see the twin handler above).
        cosimRoiBeginOffload(tid);
    } else if (opcode == ZSIM_MAGIC_OP_ROI_END) {
        if (!g_mpi_thread_mode) roiWallEnd(tid, "roi_end");   // 1.12.18: on the core that ran the ROI, before any migration
        if (g_cosim_mode && !g_cosim_no_offload && tid < MAX_THREADS) {
            g_in_device_region.store(false);
            g_roi_offload_window.store(false, std::memory_order_release);   // 1.11.94 (H11)
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
                        if (g_detOrder && detActive() && tid < MAX_THREADS) { if (!g_detCredit[tid].exchange(1)) g_detPending.fetch_add(1); }   // (1.12.123): its re-join is awaited
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
                (g_deviceWorkers.load() > 0 || g_ranksEnteredN.load() < g_mpi_rank_count)) {   // (1.12.170): a rank still owes its kernel
                /* 1.8.4 ROI window: the window is closing but other ranks
                 * are still computing on device PEs. The LAST rank out (at
                 * its barrier arrival, handleMpiMagicOp) freezes stats;
                 * freezing here at the opener's roi_end would truncate
                 * every other rank's compute tail. */
                return;
            }
            if (devScopeLastRankRule()) {
                /* 1.11.97 (x01-qemu-zsim-plugin-5): device scope, same
                 * reason. The opener's roi_end only marks the ROI as
                 * closing; the N-th arrival at the next barrier (MPI_TIME
                 * handler) freezes the stats. See g_devRoiClosing. */
                g_devRoiClosing.store(true);
                info("Thread %d: ROI end (device-scope MPI opener): closing, "
                     "stats freeze at the last rank's barrier arrival [1.11.97]", tid);
                return;
            }
            // This rank finished its ROI -> EXITED, so the epoch cut advances over
            // the still-running peers instead of pinning on this (now frozen) core.
            if (tid < MAX_THREADS && cids[tid] < MAX_THREADS)
                g_coreRoiState[cids[tid]].store(2, std::memory_order_release);
            in_roi.store(false);
            dumpTerminationStats();
            /* 1.12.18 (2026-10-24, MPI checksums): the last rank is out of its ROI, the
             * stats are written; the program now runs to its own exit
             * unsimulated (pimidPostRoiFree), as after a shared-memory ROI,
             * so rank 0's BENCH_CHECKSUM is printed. It used to end here at
             * the next Join (_exit before the print). */
            g_postRoiSince = postRoiWallSec();
            __sync_synchronize();
            g_postRoiFree = 1;
            if (zinfo->sched) zinfo->sched->releaseForPostRoi();   // (1.12.121): no thread is left waiting in the barrier
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
            /* 1.12.18: write the stats now; the program then runs to its own
             * exit (pimidPostRoiFree), so its output after the ROI survives. */
            dumpTerminationStats();
            g_postRoiSince = postRoiWallSec();
            __sync_synchronize();
            g_postRoiFree = 1;
            if (zinfo->sched) zinfo->sched->releaseForPostRoi();   // (1.12.121): no thread is left waiting in the barrier
            zinfo->terminationConditionMet = true;
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
                if (g_detOrder && detActive() && tid < MAX_THREADS) { if (!g_detCredit[tid].exchange(1)) g_detPending.fetch_add(1); }   // (1.12.123): its re-join is awaited
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
                if (g_detOrder && detActive() && tid < MAX_THREADS) { if (!g_detCredit[tid].exchange(1)) g_detPending.fetch_add(1); }   // (1.12.123): its re-join is awaited
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
static void tb_trans_cb(struct qemu_plugin_tb *tb, void *userdata) {
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
    tud->rtRegion = RTR_OTHER;   // 1.12.18: PIM runtime primitives
    tud->rtSym = 0;
    if (g_rt.enabled) rtClassifyTb(tb_addr, &tud->rtRegion, &tud->rtSym);

    /* Branch-predictor wiring (OOO and decode-enabled in-order): classify this
     * TB's TERMINATOR so its outcome can be resolved from the next TB's start
     * address and fed to the core's predictors:
     *  - conditional jcc            -> direction feed (2-level predictor)
     *  - direct call (E8)           -> RAS push (target always predicted)
     *  - indirect call (FF /2,/3)   -> BTB target check + RAS push
     *  - indirect jmp (FF /4,/5)    -> BTB target check
     *  - ret (C3/C2)                -> RAS pop + target check
     *  - direct jmp (E9/EB)         -> CF_DIR_JMP (1.12.1, ticket #114): the
     *    decode resteer of the predictor-less in-order mode (branchPredictor
     *    none); the OOO and the PAg in-order cores ignore it, as they ignored
     *    the unfed jmp before (a fixed target, always predicted).
     * The replay classifier (zsim_trace_driver.cpp classifyTerminator) is a
     * copy of this one and must stay identical (1.11.100 H40).
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
            } else if (opb == 0xE9 || opb == 0xEB) {              /* direct jmp (1.12.1) */
                tud->termKind = CF_DIR_JMP;
                tud->termPc = lpc;
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
                    || isMpiMagicOp(prev_magic_op)
                    || isSpmMagicOp(prev_magic_op)
                    || isRtMagicOp(prev_magic_op)) {   // 1.12.18
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
            /* 1.12.18: a libgomp entry point reads registers (team size, return address) */
            const bool rtRegs = (tud->rtSym & 0x8000) != 0;
            qemu_plugin_register_vcpu_insn_exec_cb(
                insn, insn_exec_cb, rtRegs ? QEMU_PLUGIN_CB_R_REGS : QEMU_PLUGIN_CB_NO_REGS, tud);
            bbl_registered = true;
        }

        /* Register memory callback for every instruction (REP strings tagged for the guard above;
         * 1.12.18: atomic read-modify-writes tagged for the lock-word model, "model the lock word") */
        qemu_plugin_register_vcpu_mem_cb(
            insn, mem_cb, QEMU_PLUGIN_CB_NO_REGS,
            QEMU_PLUGIN_MEM_RW, isRepString(bytes, insn_sz) ? (void *)&rep_string_tag
                                : (g_lockSync && isAtomicRmw(bytes, insn_sz)) ? (void *)&lock_rmw_tag : NULL);
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

/* ---- 1.12.17: guest thread stacks (zsim.h stackLo) ----
 * glibc maps each thread's stack with mmap(MAP_STACK) and clone()s the thread
 * onto it; the main thread's stack is the mapping holding its stack pointer at
 * roi_begin. Guest and host share the address space (guest_base 0), so that
 * mapping is found in /proc/self/maps. */
static std::mutex g_stackMu;
static uint64_t g_stackMapLen[MAX_VCPUS];          /* pending mmap(MAP_STACK) length per vCPU */
static const uint32_t kStackMaps = 256;            /* recent MAP_STACK mappings, a ring */
static uint64_t g_stackMapLo[kStackMaps], g_stackMapHi[kStackMaps];
static uint32_t g_stackMapNext = 0;

/* Caller holds g_stackMu. A stack glibc reuses for a new thread is re-armed. */
static void stackRangeAdd(uint64_t lo, uint64_t hi, uint64_t sp, const char* what) {
    uint32_t n = zinfo->stackRanges, slot = n;
    for (uint32_t i = 0; i < n; i++) {
        if (zinfo->stackLo[i] == lo && zinfo->stackHi[i] == hi) {
            zinfo->stackSp[i] = sp;
            __sync_synchronize();
            zinfo->stackOwner[i] = 0;
            return;
        }
        if (slot == n && zinfo->stackLo[i] == 0 && zinfo->stackHi[i] == 0) slot = i;
    }
    if (slot == n && n >= GlobSimInfo::kMaxStackRanges)
        panic("[stack] more than %u guest thread stacks registered", GlobSimInfo::kMaxStackRanges);
    zinfo->stackSp[slot] = sp;
    zinfo->stackOwner[slot] = 0;
    zinfo->stackHi[slot] = hi;
    zinfo->stackLo[slot] = lo;
    if (lo < zinfo->stackMin) zinfo->stackMin = lo;
    if (hi > zinfo->stackMax) zinfo->stackMax = hi;
    __sync_synchronize();
    if (slot == n) zinfo->stackRanges = n + 1;
    info("[stack] %s stack [0x%lx, 0x%lx) registered: its pages map into the PE that runs on it (1.12.17)",
         what, (unsigned long)lo, (unsigned long)hi);
}

static void stackOnClone(uint64_t sp) {
    std::lock_guard<std::mutex> g(g_stackMu);
    for (uint32_t i = 0; i < kStackMaps; i++)
        if (g_stackMapHi[i] && sp > g_stackMapLo[i] && sp <= g_stackMapHi[i]) {
            stackRangeAdd(g_stackMapLo[i], g_stackMapHi[i], sp, "thread");
            return;
        }
    for (uint32_t i = 0; i < zinfo->stackRanges; i++)
        if (sp > zinfo->stackLo[i] && sp <= zinfo->stackHi[i]) {
            stackRangeAdd(zinfo->stackLo[i], zinfo->stackHi[i], sp, "thread");
            return;
        }
    info("[stack] WARNING: clone onto stack pointer 0x%lx outside any MAP_STACK mapping: "
         "that thread's stack keeps the global address map", (unsigned long)sp);
}

static void stackOnUnmap(uint64_t addr, uint64_t len) {
    std::lock_guard<std::mutex> g(g_stackMu);
    const uint64_t end = addr + len;
    for (uint32_t i = 0; i < kStackMaps; i++)
        if (g_stackMapHi[i] && g_stackMapLo[i] < end && addr < g_stackMapHi[i])
            g_stackMapLo[i] = g_stackMapHi[i] = 0;
    for (uint32_t i = 0; i < zinfo->stackRanges; i++)
        if (zinfo->stackHi[i] && zinfo->stackLo[i] < end && addr < zinfo->stackHi[i]) {
            zinfo->stackOwner[i] = 0;
            zinfo->stackLo[i] = zinfo->stackHi[i] = 0;
        }
}

/* 1.12.18: register the main stack, then the PE running the thread claims
 * the stack under its stack pointer (spmOwnStack). */
static void stackAtRoiBegin(uint32_t tid) {
    stackAtRoiBeginReg();
    uint64_t sp = 0;
    if (zinfo && tid < MAX_THREADS && cores[tid] &&
        readGuestReg(rsp_handle.load(std::memory_order_acquire), &sp) && sp)
        cores[tid]->spmOwnStack(sp);
}

/* The main thread's stack: the mapping that holds its stack pointer. */
static void stackAtRoiBeginReg() {
    uint64_t sp = 0;
    if (!zinfo) return;
    if (!readGuestReg(rsp_handle.load(std::memory_order_acquire), &sp) || sp == 0) {
        info("[stack] WARNING: stack pointer unreadable at roi_begin: the main stack keeps the global address map");
        return;
    }
    std::lock_guard<std::mutex> g(g_stackMu);
    for (uint32_t i = 0; i < zinfo->stackRanges; i++)
        if (sp > zinfo->stackLo[i] && sp <= zinfo->stackHi[i]) return;   /* a registered thread stack */
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) { info("[stack] WARNING: /proc/self/maps unreadable: the main stack keeps the global address map"); return; }
    char line[512];
    bool found = false;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo = 0, hi = 0;
        if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && sp >= lo && sp < hi) {
            stackRangeAdd(lo, hi, sp, "main");
            found = true;
            break;
        }
    }
    fclose(f);
    if (!found)
        info("[stack] WARNING: stack pointer 0x%lx at roi_begin is in no mapping: the main stack keeps the global address map",
             (unsigned long)sp);
}

static uint32_t g_detFutexOp[MAX_VCPUS];   // (1.12.123): the futex op and nr_wake at entry
static uint64_t g_detFutexNr[MAX_VCPUS];
/* (1.12.157): THE GUEST'S FILE STATUS CARRIES NO HOST FILE STATE.
 * stat, fstat, lstat, newfstatat and statx answered with the host file's device, inode, times,
 * block size and block count: the dynamic linker stats every library it opens, glibc's stdio
 * stats a stream at its first use, and the buffers stay in stack lines the measured DQ data
 * statistics read later, so the data-pattern counters followed the host (access times change
 * between two runs; device, inode and change times differ between two copies of a tree). When
 * such a call succeeds, its answer is rewritten in the guest's buffer: device 1 (statx: 0:1),
 * inode = the order in which the guest first saw that host file (1, 2, ...; so two names of one
 * file still compare equal and two files still differ), every time 0, blocks = size / 512 rounded
 * up, statx mount id 0. Type, permissions, links, owner, device number, size and the block size
 * (glibc sizes a stream's buffer by it) stay the host's, except that a status of descriptor 0, 1 or 2 (fstat, or an empty path with
 * AT_EMPTY_PATH) reports size 0: the guest's standard streams are the launcher's log files,
 * whose length follows the log. */
static bool readGuestString(uint64_t addr, std::string* out);   // below
static uint64_t g_statWant[MAX_VCPUS];   // the guest buffer of the stat call in flight (0: none)
static int g_statFd[MAX_VCPUS];          // its descriptor when it names one (else -1)
static std::mutex g_statMu;
static std::map<std::pair<uint64_t, uint64_t>, uint64_t> g_statIno;
static uint64_t statGuestIno(uint64_t dev, uint64_t ino) {
    std::lock_guard<std::mutex> g(g_statMu);
    auto it = g_statIno.find({dev, ino});
    if (it != g_statIno.end()) return it->second;
    const uint64_t n = g_statIno.size() + 1;
    g_statIno[{dev, ino}] = n;
    return n;
}
static void statCallEnter(unsigned int vcpu, int64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    if (vcpu >= MAX_VCPUS) return;
    g_statWant[vcpu] = 0; g_statFd[vcpu] = -1;
    if (num == SYS_stat || num == SYS_lstat) g_statWant[vcpu] = a2;
    else if (num == SYS_fstat) { g_statWant[vcpu] = a2; g_statFd[vcpu] = (int)a1; }
    else if (num == SYS_newfstatat || num == SYS_statx) {
        g_statWant[vcpu] = (num == SYS_newfstatat) ? a3 : a5;
        const uint64_t flags = (num == SYS_newfstatat) ? a4 : a3;
        std::string path;
        if ((flags & AT_EMPTY_PATH) && (!a2 || (readGuestString(a2, &path) && path.empty()))) g_statFd[vcpu] = (int)a1;
    }
    if (g_statWant[vcpu]) g_statWant[vcpu] |= (num == SYS_statx) ? (1ull << 63) : 0;
}
static void statCallReturn(unsigned int vcpu, int64_t ret) {
    if (vcpu >= MAX_VCPUS || !g_statWant[vcpu]) return;
    const bool sx = (g_statWant[vcpu] >> 63) & 1;
    const uint64_t buf = g_statWant[vcpu] & ~(1ull << 63);
    const int fd = g_statFd[vcpu];
    g_statWant[vcpu] = 0;
    if (ret != 0) return;
    const bool stream = fd >= 0 && fd <= 2;
    const size_t len = sx ? sizeof(struct statx) : sizeof(struct stat);   // the x86_64 guest's layouts are the host's
    GByteArray* b = g_byte_array_new();
    if (qemu_plugin_read_memory_vaddr(buf, b, len) && b->len == len) {
        if (sx) {
            struct statx x; memcpy(&x, b->data, sizeof(x));
            const uint64_t ino = statGuestIno(((uint64_t)x.stx_dev_major << 32) | x.stx_dev_minor, x.stx_ino);
            x.stx_dev_major = 0; x.stx_dev_minor = 1; x.stx_ino = ino;
            if (stream) x.stx_size = 0;
            x.stx_blocks = (x.stx_size + 511) / 512;
            memset(&x.stx_atime, 0, sizeof(x.stx_atime)); memset(&x.stx_btime, 0, sizeof(x.stx_btime));
            memset(&x.stx_ctime, 0, sizeof(x.stx_ctime)); memset(&x.stx_mtime, 0, sizeof(x.stx_mtime));
            x.stx_mnt_id = 0;
            memcpy(b->data, &x, sizeof(x));
        } else {
            struct stat t; memcpy(&t, b->data, sizeof(t));
            const uint64_t ino = statGuestIno((uint64_t)t.st_dev, (uint64_t)t.st_ino);
            t.st_dev = 1; t.st_ino = (ino_t)ino;
            if (stream) t.st_size = 0;
            t.st_blocks = (t.st_size + 511) / 512;
            memset(&t.st_atim, 0, sizeof(t.st_atim)); memset(&t.st_mtim, 0, sizeof(t.st_mtim)); memset(&t.st_ctim, 0, sizeof(t.st_ctim));
            memcpy(b->data, &t, sizeof(t));
        }
        qemu_plugin_write_memory_vaddr(buf, b);
    }
    g_byte_array_free(b, TRUE);
}

static void syscall_ret_cb(unsigned int vcpu_index, int64_t num, int64_t ret, void* userdata) {
    statCallReturn(vcpu_index, ret);   // (1.12.157): fixed host-identity fields of a file status
    if (g_detOrder && vcpu_index < MAX_VCPUS) {   // (1.12.123): a futex wait that returned is no longer watched
        const uint32_t t = vcpuToTid[vcpu_index];
        if (t < MAX_THREADS && g_detSleepWatch[t].exchange(0)) g_detSleepers.fetch_sub(1);
    }
    if (g_detOrder && detActive() && vcpu_index < MAX_VCPUS && !vcpu_is_internal[vcpu_index]) {
        /* (1.12.123) fixed thread order: a turn may not end before the threads it
         * woke (FUTEX_WAKE's count) or created (clone) have joined; a woken
         * waiter's or a new thread's next join settles one of them */
        if (num == 56 && ret > 0) g_detPending.fetch_add(1);
        if (num == 202) {
            const uint32_t c = g_detFutexOp[vcpu_index];
            if ((c == 1 || c == 10 || c == 5) && ret > 0) g_detPending.fetch_add(ret);
            else if ((c == 3 || c == 4) && ret > 0) g_detPending.fetch_add((int64_t)std::min<uint64_t>((uint64_t)ret, g_detFutexNr[vcpu_index]));
            else if ((c == 0 || c == 9 || c == 11) && ret == 0) {
                const uint32_t t = vcpuToTid[vcpu_index];
                if (t < MAX_THREADS) g_detCredit[t].store(1);
            }
        }
    }
    if (num == 56 && ret > 0 && vcpu_index < MAX_VCPUS && !vcpu_is_internal[vcpu_index]) {   // (1.12.76): hand the domain to the child
        {
            /* (1.12.124): clone returns the guest's ID of the child; the child looks
             * itself up by its host ID. (1.12.157): QEMU's -tid-base gives IDs in order of
             * first sight (patch 0003), translated back by QEMU's own pid_g2h, which the
             * patch exports to plugins; without it (a QEMU launched without -tid-base,
             * or the patch's first version) the guest's ID is the host's plus the base,
             * and host IDs stay below 4194304. */
            typedef int64_t (*PidG2h)(int64_t);
            static const PidG2h g2h = (PidG2h)dlsym(RTLD_DEFAULT, "pid_g2h");
            static const int64_t kGuestTidBase = 10000000;
            const int hostTid = (int)(g2h ? g2h(ret) : (ret > kGuestTidBase ? ret - kGuestTidBase : ret));
            std::lock_guard<std::mutex> g(g_cloneMu);
            g_cloneDom[hostTid] = g_cloneDomPending[vcpu_index];
            if (g_cloneClearPending[vcpu_index]) g_clearTidByHost[hostTid] = g_cloneClearPending[vcpu_index];   // (1.12.172)
        }
        /* (1.12.123) fixed thread order: the new thread joins before its creator goes
         * on, so new threads take their cores in the order they were created */
        if (g_detOrder && detActive()) detSettle();
    }
    if (num == 202 && vcpu_index < MAX_VCPUS && !vcpu_is_internal[vcpu_index]) {   // 1.12.18 (ruling 2026-10-23, "sync to waker")
        const uint32_t t = vcpuToTid[vcpu_index];
        if (t < MAX_THREADS) wakeSyncOnFutexReturn(t, ret);
        return;
    }
    if (vcpu_index >= MAX_VCPUS || num != 9 /* mmap */) return;
    const uint64_t len = g_stackMapLen[vcpu_index];
    if (!len) return;
    g_stackMapLen[vcpu_index] = 0;
    if (ret < 0 && ret > -4096) return;   /* the mmap failed */
    std::lock_guard<std::mutex> g(g_stackMu);
    g_stackMapLo[g_stackMapNext] = (uint64_t)ret;
    g_stackMapHi[g_stackMapNext] = (uint64_t)ret + len;
    g_stackMapNext = (g_stackMapNext + 1) % kStackMaps;
}

/**
 * vCPU init callback -- assigns tid but defers SimThreadStart until first
 * execution callback, so we know the thread's domain (HOST vs DEVICE).
 */
static void vcpu_init_cb(unsigned int vcpu_index, void *userdata) {
    t_pimidVcpu = true;   // (1.12.137): this thread runs a vCPU (pimidPluginGuestRead)
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
        int dom = g_in_device_region.load() ? DOMAIN_DEVICE : DOMAIN_HOST;
        if (vcpu_index > 0) {   // (1.12.76): the domain its parent read at the clone (the parent records it as the clone returns)
            const int me = (int)syscall(SYS_gettid);
            bool found = false;
            for (int spin = 0; spin < 200000 && !found; spin++) {   // at most ~2 s
                {
                    std::lock_guard<std::mutex> g(g_cloneMu);
                    auto it = g_cloneDom.find(me);
                    if (it != g_cloneDom.end()) { dom = it->second; g_cloneDom.erase(it); found = true; }
                }
                if (!found) usleep(10);
            }
            if (!found) warn("[ZSim] thread %d: no clone record from its parent; its domain is read now", tid);
        }
        thread_domain[tid].store(dom);
        if (g_detOrder && vcpu_index > 0) g_detCredit[tid].store(1);   // (1.12.123): a new thread's first join
    }
}

/**
 * vCPU exit callback -- thread finish.
 */
static void vcpu_exit_cb(unsigned int vcpu_index, void *userdata) {
    if (vcpu_index < MAX_VCPUS && vcpu_is_internal[vcpu_index]) return;
    if (vcpu_index < MAX_VCPUS) {
        uint32_t tid = vcpuToTid[vcpu_index];
        if (tid != UNINITIALIZED_CID && tid < MAX_THREADS) {
            if (!thread_initialized[tid]) return;  // never started
            if (pimidPostRoiFree()) return;   // (1.12.121): after the ROI a thread ends unsimulated (see syscall_cb)
            /* Leave scheduler if still scheduled */
            uint32_t cid = cids[tid];
            if (g_detOrder && cid != INVALID_CID && cid != UNINITIALIZED_CID && !g_simThreadFinished[tid]) {
                /* (1.12.172): leave and finish at once, in the thread's turn (Scheduler::leaveAndFinish) */
                g_simThreadFinished[tid] = true;
                if (g_mpi_thread_mode || g_spCut) g_coreRoiState[cid].store(2, std::memory_order_release);
                zinfo->sched->leaveAndFinish(procIdx, tid, cid);
                cids[tid] = UNINITIALIZED_CID;
                return;
            }
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
/* (1.12.107): THE GUEST SEES THE SIMULATED MACHINE'S PROCESSORS, NOT THE HOST'S.
 * Under qemu-user the guest's processor queries reach the host: its OpenMP
 * runtime (libgomp) and glibc read /sys/devices/system/cpu/possible and
 * online and call sched_getaffinity, and with OMP_PLACES=cores libgomp reads
 * every listed processor's topology files. The host's processor count (88 to
 * 256 on one cluster) therefore sized the runtime's allocations and its
 * startup loops, and a single-core host baseline's results followed the
 * machine it ran on (DDR3 gemv_omp, 1 OOO core: 198950 / 199024 / 198985
 * cycles on three node types). The guest now sees the simulated machine:
 * processors 0..N-1, N = this run's simulated cores (zinfo->numCores):
 *  - sched_getaffinity returns that set (the kernel's rules for the buffer
 *    size: EINVAL when it cannot hold N bits or is not a multiple of 8 bytes);
 *  - sched_setaffinity accepts a set holding one of them and changes nothing
 *    on the host (a guest thread's core is the simulator's: see the fixed home
 *    core of (1.12.76));
 *  - an open of /sys/devices/system/cpu/{possible,online,present,offline},
 *    cpuK/online and cpuK/topology/... (K < N) reads that machine's values
 *    (one thread per core, one package); any other path under
 *    /sys/devices/system/cpu/ is ENOENT, so no host value reaches the guest.
 * Only these queries change; the guest's instructions are still timed by the
 * same models. */
static uint32_t g_guestCpus = 0;   // N, set once in qemu_plugin_install after SimInit
static std::string guestCpuList(uint32_t n) { return n <= 1 ? std::string("0\n") : "0-" + std::to_string(n - 1) + "\n"; }
static bool guestCpuFile(const std::string& path, std::string* content) {
    static const std::string base = "/sys/devices/system/cpu/";
    if (path.compare(0, base.size(), base) != 0) return false;
    const uint32_t n = g_guestCpus;
    std::string r = path.substr(base.size());
    while (!r.empty() && r.back() == '/') r.pop_back();
    if (r == "possible" || r == "online" || r == "present") { *content = guestCpuList(n); return true; }
    if (r == "offline") { *content = "\n"; return true; }
    if (r.compare(0, 3, "cpu") == 0) {
        size_t i = 3; uint64_t k = 0;
        while (i < r.size() && r[i] >= '0' && r[i] <= '9' && i < 13) k = k * 10 + (uint64_t)(r[i++] - '0');
        if (i > 3 && k < n) {
            const std::string f = r.substr(i);
            const std::string ks = std::to_string(k) + "\n";
            if (f == "/online") { *content = "1\n"; return true; }
            if (f == "/topology/thread_siblings_list" || f == "/topology/core_cpus_list") { *content = ks; return true; }
            if (f == "/topology/core_id") { *content = ks; return true; }
            if (f == "/topology/core_siblings_list" || f == "/topology/package_cpus_list" ||
                f == "/topology/die_cpus_list" || f == "/topology/cluster_cpus_list") { *content = guestCpuList(n); return true; }
            if (f == "/topology/physical_package_id" || f == "/topology/die_id" || f == "/topology/cluster_id") { *content = "0\n"; return true; }
        }
    }
    content->clear();
    return true;   // under the processor directory but not one of the above: ENOENT
}
static bool readGuestString(uint64_t addr, std::string* out) {
    out->clear();
    if (!addr) return false;
    GByteArray* b = g_byte_array_new();
    bool ok = false;
    for (size_t i = 0; i < 4096; i++) {   /* byte by byte: an unmapped page ends the read, not the process */
        g_byte_array_set_size(b, 0);
        if (!qemu_plugin_read_memory_vaddr(addr + i, b, 1) || b->len != 1) break;
        const char c = (char)b->data[0];
        if (!c) { ok = true; break; }
        out->push_back(c);
    }
    g_byte_array_free(b, TRUE);
    return ok;
}
/* (1.12.157): THE GUEST NAMES PIMID'S MPI SHIM BY A FIXED PATH.
 * The launcher gave the guest LD_PRELOAD and LD_LIBRARY_PATH with the build directory's own path,
 * and the dynamic linker keeps those strings (the environment on the stack, each library's name in
 * its link map), memory the measured DQ data statistics read: two equal-length copies of a tree
 * gave different counts. The guest now opens the shim as /pimid-shim/libpimid_mpi.so and its MPI
 * library names as /pimid-shim/guest_mpi/...; this plugin opens them from the directory the
 * launcher names in PIMID_GUEST_SHIM_HOST_DIR (the host environment of QEMU, not the guest's). */
static const char kGuestShimPrefix[] = "/pimid-shim/";
static bool guestShimOpen(const std::string& path, uint64_t flags, uint64_t mode, int64_t* sysret) {
    static const char* const hostDir = getenv("PIMID_GUEST_SHIM_HOST_DIR");
    const size_t n = sizeof(kGuestShimPrefix) - 1;
    if (!hostDir || !*hostDir || path.compare(0, n, kGuestShimPrefix) != 0) return false;
    const std::string rest = path.substr(n);
    if (rest.empty() || rest.find("..") != std::string::npos) { *sysret = -ENOENT; return true; }
    const int fd = open((std::string(hostDir) + "/" + rest).c_str(), (int)flags, (mode_t)mode);
    *sysret = fd < 0 ? -errno : fd;
    return true;
}
static bool guestCpuSyscallFilter(unsigned int vcpu_index, int64_t num, uint64_t a1, uint64_t a2,
                                  uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                                  uint64_t a8, int64_t* sysret, void* userdata) {
    (void)vcpu_index; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)userdata;
    const uint32_t n = g_guestCpus;
    if (!n) return false;
    const uint64_t need = ((uint64_t)n + 63) / 64 * 8;   // the kernel's cpumask size for n processors
    if (num == SYS_sched_getaffinity) {   // (pid, len, mask)
        const uint64_t len = a2;
        if (len < need || (len & 7)) { *sysret = -EINVAL; return true; }
        std::vector<unsigned char> m(need, 0);
        for (uint32_t c = 0; c < n; c++) m[c / 8] |= (unsigned char)(1u << (c % 8));
        GByteArray* b = g_byte_array_new();
        g_byte_array_append(b, m.data(), (guint)need);
        const bool ok = a3 && qemu_plugin_write_memory_vaddr(a3, b);
        g_byte_array_free(b, TRUE);
        *sysret = ok ? (int64_t)need : -EFAULT;
        return true;
    }
    if (num == SYS_sched_setaffinity) {   // (pid, len, mask)
        const uint64_t len = a2;
        const uint64_t rd = len < need ? len : need;
        GByteArray* b = g_byte_array_new();
        const bool ok = a3 && rd && qemu_plugin_read_memory_vaddr(a3, b, rd) && b->len == rd;
        bool any = false;
        for (uint32_t c = 0; ok && c < n && c / 8 < rd; c++) if (b->data[c / 8] & (1u << (c % 8))) { any = true; break; }
        g_byte_array_free(b, TRUE);
        *sysret = !ok ? -EFAULT : (any ? 0 : -EINVAL);
        return true;
    }
    uint64_t pathAddr = 0, flags = 0, mode = 0;
    if (num == SYS_openat) { pathAddr = a2; flags = a3; mode = a4; }
    else if (num == SYS_open) { pathAddr = a1; flags = a2; mode = a3; }
    else return false;
    std::string path, content;
    if (!readGuestString(pathAddr, &path) || path.empty() || path[0] != '/') return false;
    if (guestShimOpen(path, flags, mode, sysret)) return true;   // (1.12.157): /pimid-shim/...
    if (!guestCpuFile(path, &content)) return false;
    if (content.empty()) { *sysret = -ENOENT; return true; }
    if ((flags & O_ACCMODE) != O_RDONLY) { *sysret = -EACCES; return true; }
    const int fd = (int)syscall(SYS_memfd_create, "pimid_guest_cpu", (flags & O_CLOEXEC) ? MFD_CLOEXEC : 0u);
    if (fd < 0) { *sysret = -errno; return true; }
    if (write(fd, content.data(), content.size()) != (ssize_t)content.size() || lseek(fd, 0, SEEK_SET) != 0) {
        const int e = errno ? errno : EIO; close(fd); *sysret = -e; return true;
    }
    *sysret = fd;
    return true;
}

static void syscall_cb(unsigned int vcpu_index,
                       int64_t num,
                       uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t a4,
                       uint64_t a5, uint64_t a6,
                       uint64_t a7, uint64_t a8, void *userdata) {
    if (vcpu_index >= MAX_VCPUS) return;
    if (vcpu_is_internal[vcpu_index]) return;
    statCallEnter(vcpu_index, num, a1, a2, a3, a4, a5);   // (1.12.157): a file status call (its answer is rewritten at return)
    /* Guard: skip if we're already inside ZSim code (e.g., reentrant
     * callback from barrier/scheduler futex operations). */
    if (in_zsim[vcpu_index]) return;
    /* 1.12.17: guest thread stacks (MAP_STACK = 0x20000, CLONE_THREAD = 0x10000) */
    if (num == 9 && (a4 & 0x20000)) g_stackMapLen[vcpu_index] = a2;
    else if (num == 56 && (a1 & 0x10000) && a2 && zinfo) stackOnClone(a2);
    if (g_detOrder && num == 202 && vcpu_index < MAX_VCPUS) { g_detFutexOp[vcpu_index] = (uint32_t)(a2 & 0x7f); g_detFutexNr[vcpu_index] = a3; }
    if (num == 56) g_cloneClearPending[vcpu_index] = (a1 & 0x200000) ? a4 : 0;   // (1.12.172): CLONE_CHILD_CLEARTID's word
    if (num == 218) { std::lock_guard<std::mutex> g(g_cloneMu); g_clearTidByHost[(int)syscall(SYS_gettid)] = a1; }   // (1.12.172): set_tid_address
    if (num == 56 && (a1 & 0x10000))   // (1.12.76): the child's domain, as its parent sees it at the clone
        g_cloneDomPending[vcpu_index] = g_in_device_region.load() ? DOMAIN_DEVICE : DOMAIN_HOST;
    else if (num == 11 && zinfo && zinfo->stackRanges) stackOnUnmap(a1, a2);
    if (num == 231) { g_guest_exit_seen = true; g_guest_exit_status = (int)(a1 & 0xff); }   // (1.12.115): the guest's own exit, whatever its thread's state
    uint32_t tid = vcpuToTid[vcpu_index];
    /* (1.12.172): a thread that exits marks its core EXITED (exempt from the epoch cut) here, at its exit call, in
     * its own turn of the fixed order. SimThreadFini, which marked it before, runs when QEMU tears the vCPU down,
     * at a host-timed point: whether a phase barrier's cut still waited on the finished core, and so where the
     * NoC and DRAM replays split their phase buckets, followed the host (1 run in 32 of a 4-core co-sim MPI cell
     * folded one barrier earlier, and the published epoch latencies moved the ranks' post-ROI tail). */
    if (num == 60 && (g_mpi_thread_mode || g_spCut) && tid < MAX_THREADS && cids[tid] < MAX_THREADS)
        g_coreRoiState[cids[tid]].store(2, std::memory_order_release);
    /* (1.12.172): the exit's clear-and-wake of the thread's ID word (what pthread_join waits on) is done here, in the
     * exiting thread's turn, and the threads it wakes are awaited like those of a FUTEX_WAKE. QEMU did it after the
     * thread had left the fixed order, when the host thread was torn down, so the phase in which a joining thread
     * (rank 0 joining the MPI rank threads) woke followed the host (about 1 run in 70 of a co-sim MPI cell moved its
     * post-ROI tail). QEMU's own clear and wake then find the word cleared and no waiter. */
    if (num == 60 && g_detOrder && detActive() && tid < MAX_THREADS) {
        uint64_t w = 0;
        { std::lock_guard<std::mutex> g(g_cloneMu); auto it = g_clearTidByHost.find((int)syscall(SYS_gettid)); if (it != g_clearTidByHost.end()) { w = it->second; g_clearTidByHost.erase(it); } }
        if (w) {
            GByteArray* b = g_byte_array_new(); const uint32_t z = 0;
            g_byte_array_append(b, (const guint8*)&z, 4);
            const bool ok = qemu_plugin_write_memory_vaddr(w, b);
            g_byte_array_free(b, TRUE);
            if (ok) {
                const long n = syscall(SYS_futex, (void*)(uintptr_t)w, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);   // guest_base 0: the guest address is the host's
                if (n > 0) g_detPending.fetch_add(n);
            }
        }
    }
    if (tid == UNINITIALIZED_CID || tid >= MAX_THREADS) return;
    if (!thread_initialized[tid]) return;  // not yet started

    uint32_t cid = cids[tid];
    /* 1.12.18 (2026-10-24, MPI checksums): exit_group after the ROI by a thread
     * that has left the scheduler (post-ROI run to exit): end here with the
     * guest's own status. The return below skipped the exit hook, so QEMU's
     * exit path reached plugin_exit and the guest status was lost. */
    if (num == 231 && pimidPostRoiFree()) {
        g_guest_exit_seen = true;   // (1.12.115)
        g_guest_exit_status = (int)(a1 & 0xff);
        info("Caught exit syscall (231) after the ROI (program ran to its exit unsimulated), terminating simulation, guest status %d", g_guest_exit_status);
        if (g_guest_exit_status != 0)
            info("Guest exit status %d: the workload did not complete normally", g_guest_exit_status);
        SimEnd();
    }
    /* (1.12.121): after the ROI the program runs unsimulated; its syscalls no longer
     * enter the scheduler (a leave there could end a phase and run the end-of-phase
     * work for cores that will never move again, holding the thread, and with it
     * the program's exit and its output after the ROI) */
    if (pimidPostRoiFree()) return;
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
            g_guest_exit_seen = true;   // (1.12.115)
            g_guest_exit_status = (int)(a1 & 0xff);   /* 1.11.94: carried to the host by SimEnd() */
            if (g_guest_exit_status != 0)
                info("Guest exit status %d: the workload did not complete normally", g_guest_exit_status);
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
            /* (1.12.76): a futex wait that will sleep has its own synthetic PC, marked
             * blocking at start-up: it leaves the core at once, as the OS takes a
             * sleeping thread off its CPU. WAIT / WAIT_BITSET / WAIT_REQUEUE_PI
             * sleep only while the word holds the expected value (the kernel
             * returns EAGAIN otherwise), so a wait whose word has already changed
             * keeps the core, as do the WAKE-class ops; the PI lock ops (LOCK_PI,
             * LOCK_PI2) take the blocking path. Guest and host addresses coincide
             * (guest_base 0). */
            if (num == 202) {
                const uint32_t c7 = (uint32_t)(a2 & 0x7f);
                if (c7 == 6 || c7 == 13) synth_pc = kFutexWaitSynthPc;
                else if ((c7 == 0 || c7 == 9 || c7 == 11) && a1 &&
                         *(volatile uint32_t*)(uintptr_t)a1 == (uint32_t)a3) synth_pc = kFutexWaitSynthPc;
            }
            if (num == 202) wakeSyncOnFutexEntry(tid, cid, a1, a2, a5);   // 1.12.18 (ruling 2026-10-23, "sync to waker")
            if (g_detOrder) {   // (1.12.123): before a leave, every thread this turn woke, created or moved has joined
                if (in_roi.load()) {
                    detSettle();
                    if (tid < MAX_THREADS && (synth_pc == kFutexWaitSynthPc || num == 449)) {
                        /* the wait leaves the core: pending until the kernel has queued it */
                        g_detSleepHtid[tid] = (int)syscall(SYS_gettid);
                        g_detSleepNr[tid] = (uint64_t)num;
                        g_detSleepArg[tid] = a1;
                        if (!g_detCredit[tid].exchange(1)) g_detPending.fetch_add(1);
                        uint32_t top = g_detSleepTop.load();
                        while (top < tid + 1 && !g_detSleepTop.compare_exchange_weak(top, tid + 1)) {}
                        g_detSleepers.fetch_add(1);
                        g_detSleepWatch[tid].store(1);
                    }
                }
            }
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
static void plugin_exit(void *userdata) {
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
    qemu_plugin_register_vcpu_init_cb(id, vcpu_init_cb, NULL);
    qemu_plugin_register_vcpu_exit_cb(id, vcpu_exit_cb, NULL);

    /* 1.12.18 (ruling 2026-10-24): an exec-method run; a single-process one
     * (no PIMID_MPI_RANK, no thread-MPI) is priced with the cut mechanism
     * (PEMemoryInterface::singleProcCut, read by SimInit's replay instances). */
    setenv("PIMID_EXEC_CUT", "1", 1);
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
    /* Measured DQ data: a line address is this process's guest address
     * (guest_base 0) only when no other process shares the hierarchy -- with
     * procIdx > 0 the process bits are OR-ed into it, and another process's
     * lines are not in this address space. */
    dqDataReadable = (procIdx == 0 && zinfo->numProcs == 1);
    pimidGuestReadHook = pimidPluginGuestRead;   // (1.12.137, user ruling 2026-10-27)
    perProcessEndFlag = 0;

    /* Compute core-type masks for co-sim domain routing.
     *
     * Valid host cores:   OoO, InOrder  (need cache hierarchy)
     * Invalid for host:   ALU (no cache), Null (no memory modeling)
     * Valid device cores:  ALL four types (ALU, OoO, InOrder, Null)
     *
     * ALU and Null cores are always placed in the device mask.
     * All other core types (OoO, InOrder) go to the host mask.
     * (1.12.1: the Simple core is retired; "Timing" was the in-order core's
     * former name.) */
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
            /* OoO, InOrder -- valid host cores */
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
        info("[ZSim] PE and host-core pinning DISABLED via PIMID_NO_PE_PIN "
             "(threads take cores in the scheduler's order)");
    } else if (!device_core_list.empty()) {
        info("[ZSim] 1.9.20 PE pinning active: %lu device PEs, thread home = "
             "vcpu_index %% %lu (fixed placement; every mode since (1.12.76))",
             (unsigned long)device_core_list.size(),
             (unsigned long)device_core_list.size());
    }

    bool has_device = false;
    for (auto b : device_mask) if (b) { has_device = true; break; }
    /* host homes ((1.12.76)): set below, after the ALU/Null-only promotion */
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

    /* (1.12.76): host homes (hostMaskFor), over the final host mask */
    host_core_list.clear();
    for (uint32_t i = 0; i < zinfo->numCores; i++) if (host_mask[i]) host_core_list.push_back(i);
    host_home_owner.assign(host_core_list.size(), UNINITIALIZED_CID);
    host_home_mask.resize(MAX_THREADS);

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
        g_spCut = PEMemoryInterface::singleProcCut();   // 1.12.18 (ruling 2026-10-24)
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
    /* (1.12.76): a futex wait leaves its core at once. ZSim kept a thread in a
     * syscall on its core ("fake leave") and its watchdog turned the leave into a
     * real one after a stall measured in host time; every shared-memory run had
     * futex blacklisted that way after its first stall. Since (1.12.126) thread-MPI
     * runs do the same (their mailbox waits are still timed by the transport);
     * process-mode MPI ranks keep the old path. */
    if (!getenv("PIMID_MPI_RANK")) {
        zinfo->sched->markBlockingSyscall(procIdx, kFutexWaitSynthPc);   // futex WAIT-class ops
        zinfo->sched->markBlockingSyscall(procIdx, 0x1000 + 449);        // futex_waitv
        info("[ZSim] futex waits leave the core at once (futex WAIT-class ops, futex_waitv) [(1.12.76)]");
        /* (1.12.123): fixed thread order. One core runs at a time; the cores take
         * their turns in the order of their clocks (the global phase axis; the
         * lower id first on a tie), a thread that joins during a turn (woken,
         * created, moved to the device) takes its place in that order within
         * the same phase, a turn ends only when each thread it woke, created,
         * moved or handed a core has joined and each thread it put to sleep is
         * queued in the kernel, and threads waiting for a core are queued by phase
         * and id. Only a futex wait that sleeps gives up the turn early. The
         * simulated time comes from the model as before; the order in which the
         * threads touch shared state no longer comes from the host. */
        zinfo->sched->setFixedOrder();
        g_detTurnStart = []() { if (detActive()) detSettle(); };
        g_detBeforeLeave = [](uint32_t tid) {
            if (detActive()) detSettle((tid < MAX_THREADS && g_detCredit[tid].load()) ? 1 : 0);
        };
        if (!g_mpi_thread_mode) {
            g_detClockOf = [](uint32_t cid) -> uint64_t {
                return (cid < zinfo->numCores && zinfo->cores[cid]) ? zinfo->cores[cid]->lockClock() : 0;
            };
            info("[ZSim] fixed thread order: one core at a time, turns of at most %lu cycles by clock (shared-memory runs) [(1.12.123)]", (unsigned long)kDetTurnCycles);
            g_detQ = kDetTurnCycles;
        } else {
            /* (1.12.126): thread-MPI ranks keep zsim's own turn order -- one rank at a time,
             * a whole phase each, in the order of the seeded shuffle drawn at every
             * phase end -- made repeatable: a rank that joins mid-phase (woken, its
             * message arrived) takes its place by its position in that shuffle, and
             * a turn passes on only once the ranks it woke have joined and the ranks
             * it put to sleep are queued in the kernel (as above). The ranks' clocks
             * stay where the transport puts them (send stamps, rendezvous arrivals,
             * the frozen-clock comm window), so the timing model is unchanged; the
             * order of the ranks' turns, and with it which message a receive takes
             * first and the order of the contention reservations, no longer comes
             * from the host. Turns by clock are not used here: they change the
             * interleaving the transport was calibrated with (in-order and
             * out-of-order rank cores ran up to 60% longer). */
            g_detClockMode = false;
            for (uint32_t i = 0; i < MAX_THREADS; i++) g_detShufKey[i] = (1ull << 32) + i;
            g_detClockOf = [](uint32_t cid) -> uint64_t { return cid < MAX_THREADS ? g_detShufKey[cid] : cid; };
            g_detQ = 0;
            info("[ZSim] fixed rank order: one rank at a time, a phase each, in the seeded shuffle order (thread-MPI) [(1.12.126)]");
        }
    }
    {   /* 1.12.18 (user ruling 2026-10-23, "model the lock word"): simulation.lock_sync, default on;
         * never under thread-MPI (see lockRmw) */
        const char* ls = getenv("PIMID_LOCK_SYNC");
        const bool on = !(ls && atoi(ls) == 0);
        g_lockSync = on && !g_mpi_thread_mode;
        info("[ZSim] lock-word sync (simulation.lock_sync): %s", g_lockSync ? "on" : (on ? "off (thread-MPI)" : "off"));
    }
    rtInit();   // 1.12.18: PIM runtime primitives (simulation.pim_runtime -> PIMID_PIM_RUNTIME)
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
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans_cb, NULL);
    qemu_plugin_register_vcpu_syscall_cb(id, syscall_cb, NULL);
    qemu_plugin_register_vcpu_syscall_ret_cb(id, syscall_ret_cb, NULL);   /* 1.12.17: MAP_STACK mappings */
    g_guestCpus = zinfo->numCores;   /* (1.12.107): the guest's processor queries (guestCpuSyscallFilter) */
    qemu_plugin_register_vcpu_syscall_filter_cb(id, guestCpuSyscallFilter, NULL);
    /* (1.12.157): PIMID seeds the guest (-seed); QEMU answers the guest's getrandom() from that seed only with
     * external/qemu-patches/0005, which exports this flag (without it the host answered, and glibc's malloc
     * stores a getrandom() value in every freed chunk, memory the DQ data statistics read) */
    if (!dlsym(RTLD_DEFAULT, "qemu_getrandom_seeded"))
        panic("this QEMU lacks external/qemu-patches/0005-linux-user-seeded-getrandom.patch (docs/build.md): "
              "the guest's getrandom() would come from the host");
    {
        char gm[256];
        snprintf(gm, sizeof(gm), "[guest] processors: the guest sees %u (0-%u, the simulated cores) in sched_getaffinity and"
                 " /sys/devices/system/cpu, not the host's [(1.12.107)]\n", g_guestCpus, g_guestCpus ? g_guestCpus - 1 : 0);
        qemu_plugin_outs(gm);
    }
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
