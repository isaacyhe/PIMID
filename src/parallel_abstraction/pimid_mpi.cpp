/**
 * pimid_mpi.cpp -- Multi-process MPI implementation for PIMID.
 *
 * Replaces the previous pthread-mailbox implementation. Each MPI rank now
 * runs as a separate process, with PIMID itself as the launcher (no mpirun
 * dependency). Inter-rank messaging uses POSIX shared memory + pshared
 * mutex/condvar, so workloads parallelize across host pCPUs (the previous
 * in-process design serialized under qemu-user's TCG core lock).
 *
 * Process-level rank assignment is via env vars set by the PIMID launcher:
 *   PIMID_MPI_RANKS  -- total rank count (required, set by launcher)
 *   PIMID_MPI_RANK   -- this process's rank id (0..N-1, set by launcher)
 *   PIMID_MPI_SHM    -- name of the POSIX shm object backing the mailboxes
 *                      (optional; defaults to /pimid_mpi_<pid_of_rank0>)
 *
 * Workloads that never call MPI symbols simply ignore all of this -- each
 * rank runs independently. Workloads that DO call MPI get a working
 * cross-process shm transport, with magic-op latency injection unchanged.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "parallel_abstraction/pimid_mpi.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <atomic>
#include <cstdint>
#include <cerrno>
#include <vector>

/* ---- ZSim/QEMU magic ops (only effective under instrumentation) ---- */

#define COMPILER_BARRIER() { __asm__ __volatile__("" ::: "memory"); }

#define ZSIM_MAGIC_OP_MPI_REGISTER  2048
#define ZSIM_MAGIC_OP_MPI_SEND      2049
#define ZSIM_MAGIC_OP_MPI_RECV      2050
#define ZSIM_MAGIC_OP_MPI_BARRIER   2051
/* Comm-window bracket: while open, the plugin keeps this rank's core ATTACHED
 * across the transport's polling sleeps (no syscallLeave) and does NOT count
 * the transport's own instructions, so post-comm compute is counted and the
 * reported cycle/instr totals are deterministic (wall-clock poll invisible). */
#define ZSIM_MAGIC_OP_MPI_COMM_BEGIN 2055
#define ZSIM_MAGIC_OP_MPI_COMM_END   2056
/* Phase-2 cross-rank NoC contention: after the sender computes its contention
 * wait against the shared occupancy table, it emits this op so the plugin
 * charges the wait as idle CYCLES on the sender's core (the shared outbound
 * link to the destination was busy). The same wait is also stamped onto the
 * message so the receiver's rendezvous arrival accounts for it. */
#define ZSIM_MAGIC_OP_MPI_CONTEND    2057
#define ZSIM_MAGIC_OP_MPI_TIME       2058  /* plugin -> params.sim_now */
#define ZSIM_MAGIC_OP_MPI_ADVANCE    2059  /* core clock -> params.sim_send_time */
#define ZSIM_MAGIC_OP_MPI_WAIT_PEER  2068  /* thread mode: this rank blocks on a peer (inside its comm bracket) */

struct __attribute__((aligned(64))) PimidMpiParams {
    uint32_t src_pe;
    uint32_t dst_pe;
    uint64_t msg_size;
    uint64_t msg_id;
    /* sim-time rendezvous (1.2.3): plugin writes the rank's current simulated
     * time into sim_now on MPI_SEND so the sender can stamp the message; the
     * receiver copies the message's stamp into sim_send_time before MPI_RECV so
     * the plugin can advance the receiver's clock to the deterministic arrival
     * time (send_time + latency) instead of the wall-clock poll duration. */
    uint64_t sim_now;
    uint64_t sim_send_time;
    /* Cross-rank shared-link queueing wait (cycles), computed by libpimid_mpi
     * against the shared occupancy table. On SEND this is charged to the
     * sender's core (MPI_CONTEND); on RECV it is added to the rendezvous
     * arrival = sim_send_time + sim_contend_wait + hierLat + nocLat. */
    uint64_t sim_contend_wait;
    /* Filled by the plugin on SEND: the real per-message NoC cost (Garnet RTT)
     * used as the occupancy reservation duration -- NOT a magic flit constant. */
    uint64_t noc_lat;
    /* Filled by the plugin on SEND: 1 if the NoC model is the cycle-accurate
     * (detailed) Garnet, 0 for analytical/simple (or a native run with no
     * plugin). Cross-rank contention is applied ONLY when this is 1, so the
     * analytical model -- the single intentional approximation -- skips it. */
    uint32_t noc_detailed;
    /* 1.9.27: clock domain of sim_now, in MHz. Ranks exchange timestamps as
     * CYCLE counts, comparable only if both clocks tick at the same rate;
     * carrying the rate lets the consumer convert. Uses the former padding
     * word, so the layout is unchanged. 0 = unknown -> same-domain. */
    uint32_t sim_now_freq_mhz;
};

static thread_local PimidMpiParams tl_mpi_params;

static inline void zsim_magic_op(uint64_t op) {
#ifdef __x86_64__
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;" : : "c"(op));
    COMPILER_BARRIER();
#else
    (void)op;
#endif
}

/* ---- Shared-memory mailbox layout ----
 *
 * Single POSIX shm object holds:
 *   SharedHeader (barrier state, init flags)
 *   Mailbox[N]   -- one inbound queue per rank
 *
 * Each Mailbox is a fixed-size ring of MessageSlots, each MessageSlot
 * holding up to PIMID_MPI_MAX_MSG_BYTES of payload. Messages larger than
 * the slot are chunked (the workload sees a logical Send/Recv, but the
 * transport splits/joins on the wire). For typical PIM benchmarks msg
 * sizes are well under 64 KB; the ring depth tolerates burst traffic.
 */

#define PIMID_MPI_MAX_MSG_BYTES   (64 * 1024)   /* 64 KB per slot */
#define PIMID_MPI_RING_SLOTS      16            /* per mailbox */
#define ZSIM_MAGIC_OP_MPI_TRANSPORT_RANGE 2060   /* 1.12.17 (M1): register transport memory */

struct MessageSlot {
    int       src;
    int       tag;
    uint32_t  size;       /* bytes used in payload */
    uint32_t  chunk_idx;  /* 0..n_chunks-1 */
    uint32_t  n_chunks;
    uint32_t  msg_id;
    uint64_t  sim_send_time;    /* sender's simulated send-time (cycles) */
    uint64_t  sim_contend_wait; /* cross-rank contention wait (cycles, Phase 2) */
    uint64_t  msg_bytes;        /* (1.12.177): the whole message's size (every chunk carries it) */
    char      payload[PIMID_MPI_MAX_MSG_BYTES];
};

/* KERNEL-WAIT-FREE TRANSPORT (2026-06-12, the 1.0.8 freeze fix).
 *
 * The previous transport used process-shared pthread mutex+condvars. Under
 * the simulator, a rank was captured (PIMID_MPI_TRACE) stuck forever in
 * pthread_mutex_lock on the barrier mutex that NO other rank held: its
 * futex wake was lost (15/16 ranks waiting in gen-2, the 16th parked on a
 * free mutex). Whether glibc's pshared paths or the simulator's syscall
 * interception eats the wake, the robust cure is the same: NO kernel
 * blocking waits anywhere in the transport. Critical sections are guarded
 * by a CAS spinlock with usleep backoff; empty/full/barrier waits poll
 * with predicate recheck. A lost wake is impossible by construction (there
 * are no wakes), and polling threads keep re-entering the simulator's
 * scheduler instead of parking in futexes. The 50-200us poll granularity
 * is invisible at simulation timescales (waits are wall-clock-only). */

struct Mailbox {
    volatile uint32_t lk;          /* 0=free, 1=held (CAS spinlock) */
    uint32_t        head;          /* next slot to read */
    uint32_t        tail;          /* next slot to write */
    uint32_t        count;         /* slots currently filled */
    /* 1.6 thread mode ONLY: private (same-process) mutex/cond waits, the OMP
     * discipline -- a waiting rank parks in a futex, the plugin deschedules
     * its core, and the wait costs ZERO simulated cycles. The kernel-wait-free
     * usleep polling below is the PROCESS-mode discipline (pshared futex wakes
     * were lost under the simulator, the 1.0.8 freeze); private futexes are
     * exercised daily by libgomp under this same plugin. In thread mode a
     * polling wait would burn shared phase-clock cycles while attached, which
     * both crawls and re-imports wall-clock into results. */
    pthread_mutex_t mtx;
    pthread_cond_t  can_recv;      /* signaled on enqueue */
    pthread_cond_t  can_send;      /* signaled on dequeue */
    /* Thread mode: source-matched receive. A slot consumed out of FIFO order
     * (receiver asked for a specific src) is tombstoned here; head advances
     * over tombstones. Per-src order is preserved (scan from head returns the
     * earliest enqueued match). Process mode never touches this. */
    uint8_t         consumed[PIMID_MPI_RING_SLOTS];
    MessageSlot     slots[PIMID_MPI_RING_SLOTS];
};

/* Phase-2 cross-rank NoC contention occupancy table.
 *
 * busy_until[p] holds the simulated cycle until which contention point p is
 * occupied. A point is, by default, keyed on the destination node
 * (dst_pe % g_contend_points) so concurrent messages aimed at the same node
 * contend (captures hotspots). The array is fixed-size at the maximum point
 * count; the active modulus g_contend_points (<= MAX) selects the granularity.
 * Protected by occ_lk (CAS spinlock, same wait-free discipline as the
 * mailboxes). Keyed purely on simulated send-time, so it is independent of
 * wall-clock except for the tie order of simultaneous reservations. */
#define PIMID_MPI_CONTEND_MAX_POINTS 256

struct SharedHeader {
    /* Barrier: lock-free generation barrier (atomics only) */
    volatile int     barrier_count;
    volatile int     barrier_gen;
    /* 1.6 thread mode ONLY: cond-parked barrier (see Mailbox comment). */
    pthread_mutex_t  bar_mtx;
    pthread_cond_t   bar_cv;
    /* Max-arrival rendezvous: max over this generation's arrivals (sim time),
     * captured into bar_release by the last arriver. Every rank ADVANCEs its
     * core to bar_release after the wake, so barrier exit time is a pure
     * function of simulated arrivals -- wake order is timing-invisible. */
    uint64_t         bar_max_arr;
    uint64_t         bar_release;
    int              nranks;
    int              _pad;
    /* Cross-rank contention occupancy (Phase 2). */
    volatile uint32_t occ_lk;     /* CAS spinlock guarding busy_until[] */
    uint32_t          _pad2;
    volatile uint64_t busy_until[PIMID_MPI_CONTEND_MAX_POINTS];
    /* Mailboxes follow: g_shared->mailboxes[0..nranks-1] */
    Mailbox          mailboxes[0];
};

static inline void mb_lock(Mailbox* mb) {
    for (int spin = 0; ; spin++) {
        if (__sync_bool_compare_and_swap(&mb->lk, 0u, 1u)) return;
        if (spin < 64) { __asm__ __volatile__("pause"); continue; }
        usleep(50);
    }
}
static inline void mb_unlock(Mailbox* mb) {
    __sync_synchronize();
    mb->lk = 0;
}

/* ---- Cross-rank contention config + occupancy lock ----
 *
 * Cross-rank NoC contention is ALWAYS active in detailed mode (no opt-in env
 * flag): the plugin tells the guest, per SEND, whether the NoC model is the
 * cycle-accurate Garnet (noc_detailed) and supplies the real per-message NoC
 * cost (noc_lat) used as the reservation duration. In analytical/simple mode
 * (the single intentional approximation) noc_detailed is 0 and contention is
 * skipped. Native runs (no plugin) leave noc_detailed 0 -> no contention.
 *
 * The reservation duration is the real Garnet-computed message latency, so the
 * default models an unsaturated network: the per-message duration is small
 * relative to the sim-time spacing between sends, so contention is near-inert
 * for low-occupancy traffic (correct: no queueing when the link is idle) and
 * grows monotonically as traffic collides on the same point.
 *   PIMID_MPI_CONTEND_POINTS   number of contention points (default 64; fewer
 *                              points => more shared => more collisions). Keyed
 *                              per destination (dst_pe % points).
 *   PIMID_DEBUG_CONTEND        1=print each reservation (point/wait/busy) */
static uint32_t g_contend_points = 64;
static int      g_contend_dbg = 0;

static inline void occ_lock(SharedHeader* h) {
    for (int spin = 0; ; spin++) {
        if (__sync_bool_compare_and_swap(&h->occ_lk, 0u, 1u)) return;
        if (spin < 64) { __asm__ __volatile__("pause"); continue; }
        usleep(20);
    }
}
static inline void occ_unlock(SharedHeader* h) {
    __sync_synchronize();
    h->occ_lk = 0;
}

/* ---- Rank-local + process-local state ----
 *
 * 1.6 THREAD MODE: ranks are guest threads of ONE process, so rank identity
 * lives in TLS. In legacy process mode the process has exactly one MPI-calling
 * thread, for which a thread_local behaves identically to the old global. */

static thread_local int g_rank = -1;
static int            g_nranks = 0;
static bool           g_thread_mode = false;  /* set by the 1.6 main harness */
static SharedHeader*  g_shared = nullptr;
static size_t         g_shared_bytes = 0;
static char           g_shm_name[256] = {0};
static int            g_shm_fd = -1;
static bool           g_we_created_shm = false;
static std::atomic<bool> g_initialized{false};
static std::atomic<int>  g_next_request{1};

/* ---- 1.12.18 (user ruling 2026-10-23): PIM runtime primitives ----
 * "The PIM way, not as full SW overhead": the PE is not charged for executing
 * this shim's own code (argument and chunk bookkeeping, mailbox and staging
 * management, algorithm selection, reduction-op decoding, scratch buffers).
 * RtScope brackets it with RT_BEGIN / RT_END; the zsim plugin does not feed
 * those blocks to the core (no instructions, cycles or memory traffic). Each
 * protocol EVENT is charged the PIM primitive instead (RT_EVENT, costs in
 * simulation.pim_runtime). What a message really moves stays priced and
 * outside the window (RtPriced): the read of the sender's buffer, the write
 * of the receiver's buffer, the copies of user data a collective makes, and
 * the reduce arithmetic on user data. The injected Garnet transfer and the
 * rendezvous timing ops are unchanged. Thread mode only, and only while the
 * plugin says the PIM runtime applies to this rank (RT_QUERY: PEs only by
 * default); otherwise the shim runs exactly as before. */
#define ZSIM_MAGIC_OP_RT_BEGIN 2064
#define ZSIM_MAGIC_OP_RT_END   2065
#define ZSIM_MAGIC_OP_RT_EVENT 2066   /* %rdx = kind | participants << 32 */
#define ZSIM_MAGIC_OP_RT_QUERY 2067   /* %rdx -> uint64: 1 if the PIM runtime applies to this thread */
enum { RTK_MPI_P2P = 7, RTK_MPI_COLL_STEP = 8, RTK_MPI_RED_STEP = 9, RTK_MPI_BARRIER = 10 };   /* = the plugin's RtKind */
static bool g_rt_env = false;                 /* simulation.pim_runtime.enabled (PIMID_PIM_RUNTIME), thread mode */
static thread_local bool tl_rt_on = false;    /* the plugin applies the PIM runtime to this rank (queried per call) */
static thread_local int  tl_rt_depth = 0;     /* nesting of RtScope */
static thread_local bool tl_rt_priced = false;
static thread_local int  tl_coll = 0;         /* > 0 inside a collective: its messages are collective steps */
static inline void rt_op_rdx(uint64_t op, uint64_t v) {
#ifdef __x86_64__
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;" : : "c"(op), "d"(v) : "memory");
    COMPILER_BARRIER();
#else
    (void)op; (void)v;
#endif
}
static bool rt_query(void) {
    volatile uint64_t r = 0;
    rt_op_rdx(ZSIM_MAGIC_OP_RT_QUERY, (uint64_t)(uintptr_t)&r);
    return r == 1;   /* natively (no plugin) r stays 0 */
}
struct RtScope {
    bool on = false;
    RtScope() {
        if (!g_rt_env) return;
        if (tl_rt_depth == 0) tl_rt_on = rt_query();
        if (!tl_rt_on) return;
        on = true;
        if (tl_rt_depth++ == 0) zsim_magic_op(ZSIM_MAGIC_OP_RT_BEGIN);
    }
    void close() {
        if (!on) return;
        on = false;
        if (--tl_rt_depth == 0) zsim_magic_op(ZSIM_MAGIC_OP_RT_END);
    }
    ~RtScope() { close(); }
};
struct RtPriced {   /* kernel-side work inside a runtime window: executed and priced on the PE */
    bool on;
    RtPriced() : on(tl_rt_on && tl_rt_depth > 0 && !tl_rt_priced) { if (on) { tl_rt_priced = true; zsim_magic_op(ZSIM_MAGIC_OP_RT_END); } }
    ~RtPriced() { if (on) { tl_rt_priced = false; zsim_magic_op(ZSIM_MAGIC_OP_RT_BEGIN); } }
};
struct CollScope { CollScope() { tl_coll++; } ~CollScope() { tl_coll--; } };
static inline void rt_event(int kind, uint32_t participants) {
    if (g_rt_env && tl_rt_on) rt_op_rdx(ZSIM_MAGIC_OP_RT_EVENT, (uint64_t)(uint32_t)kind | ((uint64_t)participants << 32));
}
static inline void priced_memcpy(void* d, const void* src, size_t n) { RtPriced p; memcpy(d, src, n); }
/* Per-rank scratch for the collectives' data temporaries, preallocated by each
 * rank in MPI_Init and reused (no malloc/free per call). It holds message
 * payload and reduction operands, so it is NOT transport memory: the
 * receiver's write into it and the combine's reads of it stay priced. Its
 * pages are the rank's own (page-aligned, first touched by the rank's PE in
 * MPI_Init, so first-touch placement puts them in that PE's memory, as a PIM
 * runtime keeps a PE's buffers in its own bank). Growth (a larger message than
 * any before) happens inside the runtime window. Off: a std::vector, as before
 * 1.12.18.
 * 1.12.18 (user ruling 2026-10-24, "PIM-way: no host zeroing"): the scratch is
 * PIM-runtime state and is allocated the way a PIM runtime allocates device
 * buffers. It is no longer allocated and zeroed in MPI_Init: in co-sim MPI_Init
 * runs on the HOST before offload, so the 3 x 64 KB memset per rank was host
 * work whose ~49k dirty lines the kernel-entry flush wrote back. The first
 * collective that needs a slot allocates it here, inside the runtime window
 * (RtScope: the allocation is runtime bookkeeping, not fed to the core), at
 * least kScratchInit bytes, without zeroing: every use writes the bytes it
 * later reads (message receive, copy of the caller's data). Its first priced
 * touch is then the rank's PE's own write, so first-touch placement puts it in
 * that PE's memory; the PE's reads and writes of it during collectives stay
 * simulated and priced exactly as before. */
struct RankScratch { char* p[3]; size_t cap[3]; };
static RankScratch* g_scr = nullptr;
static const size_t kScratchInit = 64 * 1024;
static char* scratch_or_vec(int slot, size_t bytes, std::vector<char>& v) {
    if (tl_rt_on && tl_rt_depth > 0 && g_scr && g_rank >= 0) {
        RankScratch& s = g_scr[g_rank];
        if (s.cap[slot] < bytes) {
            size_t c = s.cap[slot] * 2;
            if (c < kScratchInit) c = kScratchInit;   /* 1.12.18 (ruling 2026-10-24): first allocation, device-side, no zeroing */
            if (c < bytes) c = bytes;
            c = (c + 4095) & ~(size_t)4095;
            free(s.p[slot]);
            s.p[slot] = (char*)aligned_alloc(4096, c);   /* own pages: first touched (placed) by this rank's PE */
            if (!s.p[slot]) { fprintf(stderr, "[pimid_mpi] scratch alloc of %zu bytes failed\n", c); abort(); }
            s.cap[slot] = c;
        }
        return s.p[slot];
    }
    v.resize(bytes);
    return v.data();
}

/* Reserve the contention point for a message sent at sim-time send_time that
 * occupies the shared link for `dur` cycles (the real Garnet-computed per-
 * message NoC cost), and return the queueing wait (cycles) the message must
 * absorb because the shared link was still busy with earlier traffic. The
 * reservation advances busy_until[point] to the message's completion time.
 * Returns 0 when no shm is mapped (solo run) or the duration is 0. */
static uint64_t contend_reserve(int dst_pe, uint64_t send_time, uint64_t dur) {
    /* 1.6 thread mode: the ONE in-process Garnet models message contention
     * physically (packets recorded into the deterministic batch replay), so
     * the occupancy table would double-count -- and its tie order is wall-
     * clock, the last source of run-to-run jitter. Process-mode only. */
    if (g_thread_mode) return 0;
    if (g_shared == nullptr || dur == 0) return 0;
    uint32_t pts = g_contend_points;
    if (pts < 1) pts = 1;
    if (pts > PIMID_MPI_CONTEND_MAX_POINTS) pts = PIMID_MPI_CONTEND_MAX_POINTS;
    uint32_t point = (uint32_t)(dst_pe < 0 ? 0 : dst_pe) % pts;

    occ_lock(g_shared);
    uint64_t busy  = g_shared->busy_until[point];
    uint64_t wait  = (busy > send_time) ? (busy - send_time) : 0;
    uint64_t start = (busy > send_time) ? busy : send_time;
    g_shared->busy_until[point] = start + dur;
    occ_unlock(g_shared);

    if (g_contend_dbg) {
        fprintf(stderr, "[contend r%d] dst=%d point=%u send=%lu busy=%lu "
                "dur=%lu wait=%lu\n", g_rank, dst_pe, point,
                (unsigned long)send_time, (unsigned long)busy,
                (unsigned long)dur, (unsigned long)wait);
    }
    return wait;
}

/* ---- Helpers ---- */

/* 1.11.94 (H29 s09-mpi-api-1): decode the datatype handles the workloads
 * actually pass. The shim is LD_PRELOADed under binaries built with the system
 * MPI (the shipped *_mpi kernels are MPICH builds: reduction_mpi passes
 * 0x4c00080b for MPI_DOUBLE), but this function only knew the private handles
 * 1..9 of include/parallel_abstraction/pimid_mpi.h and sized everything else
 * at 4 bytes: MPI_DOUBLE moved half its data, an MPI_CHAR receive wrote 4x
 * its buffer, and the NoC packet count of double traffic was halved.
 *
 * MPICH builtin datatype handle layout (mpi.h of MPICH 4, and
 * MPIR_Datatype_get_basic_size in MPICH's mpir_datatype.h):
 *   bits 30-31  handle kind, 01 = builtin
 *   bits 26-29  object kind, 0011 = datatype      -> (h & 0xfc000000) == 0x4c000000
 *   bits  8-15  the type's size in bytes          -> (h >> 8) & 0xff
 *   bits  0-7   index within the builtin table
 * e.g. MPI_CHAR 0x4c000101 -> 1, MPI_INT 0x4c000405 -> 4, MPI_FLOAT
 * 0x4c00040a -> 4, MPI_DOUBLE 0x4c00080b -> 8, MPI_LONG 0x4c000807 -> 8.
 * A builtin of size 0 (MPI_LB/MPI_UB markers) carries no data and is refused.
 *
 * Anything else -- an OpenMPI pointer handle, a derived/pair MPICH type
 * (MPI_FLOAT_INT 0x8c000000), MPI_DATATYPE_NULL -- is REFUSED: a guessed size
 * corrupts memory and misprices traffic, so the run stops and names the
 * handle. The private table stays for workloads built against pimid_mpi.h.
 * Corpus parity: the in-ROI traffic of the shipped kernels is MPI_INT and
 * MPI_FLOAT, 4 bytes under both the old default and this decode. */
static size_t dtype_size(MPI_Datatype dt) {
    switch (dt) {
        case MPI_CHAR: case MPI_BYTE: return 1;
        case MPI_INT: case MPI_UNSIGNED: case MPI_FLOAT: return 4;
        case MPI_DOUBLE: case MPI_LONG: case MPI_UNSIGNED_LONG:
        case MPI_LONG_LONG: return 8;
        default: break;
    }
    const uint32_t h = (uint32_t)dt;
    if ((h & 0xfc000000u) == 0x4c000000u) {
        const size_t sz = (size_t)((h >> 8) & 0xffu);
        if (sz > 0) return sz;
    }
    fprintf(stderr, "[pimid_mpi] FATAL: MPI datatype handle 0x%08x is not a "
            "type the shim can size (it decodes MPICH builtin datatypes and "
            "the pimid_mpi.h handles 1..9; derived, pair, marker and OpenMPI "
            "types are not supported). Use a builtin MPICH datatype, or "
            "build the workload against MPICH.\n", (unsigned)h);
    abort();
}

/* Open/close the comm window. Outside instrumentation these are NOPs. */
static inline void mpi_comm_begin(void) { zsim_magic_op(ZSIM_MAGIC_OP_MPI_COMM_BEGIN); }
static inline void mpi_comm_end(void)   { zsim_magic_op(ZSIM_MAGIC_OP_MPI_COMM_END); }
/* Thread mode, inside a comm bracket: the rank is about to block until a peer
 * posts its message or arrives at the barrier. The simulator then bounds the
 * rank's next records by that peer's clock instead of holding its memory
 * replay at this rank's own (see the plugin's cut). */
static inline void mpi_wait_peer(int kind, int want_src) {   /* kind 1 = receive (want_src, -1 any), 2 = barrier */
    rt_op_rdx(ZSIM_MAGIC_OP_MPI_WAIT_PEER, ((uint64_t)(uint32_t)kind << 48) | ((uint64_t)(uint16_t)(want_src + 1) << 32) | (uint32_t)g_rank);
}

/* SEND-direction timing op. The plugin charges send latency on this rank's
 * core AND writes the current simulated time back into tl_mpi_params.sim_now,
 * which the caller reads to stamp the outgoing message. */
static void inject_timing_send(int src_pe, int dst_pe, uint64_t msg_size) {
    tl_mpi_params.src_pe   = (uint32_t)src_pe;
    tl_mpi_params.dst_pe   = (uint32_t)dst_pe;
    tl_mpi_params.msg_size = msg_size;
    tl_mpi_params.msg_id++;
    zsim_magic_op(ZSIM_MAGIC_OP_MPI_SEND);
}

/* RECV-direction timing op. The caller supplies the message's send-time stamp
 * and its cross-rank contention wait; the plugin advances this rank's core to
 * the deterministic arrival time (send_time + contend_wait + latency). */
static inline void set_timing_recv(int src_pe, int dst_pe, uint64_t msg_size,
                                   uint64_t send_time, uint64_t contend_wait) {
    tl_mpi_params.src_pe           = (uint32_t)src_pe;
    tl_mpi_params.dst_pe           = (uint32_t)dst_pe;
    tl_mpi_params.msg_size         = msg_size;
    tl_mpi_params.sim_send_time    = send_time;
    tl_mpi_params.sim_contend_wait = contend_wait;
}
static void inject_timing_recv(int src_pe, int dst_pe, uint64_t msg_size,
                               uint64_t send_time, uint64_t contend_wait) {
    set_timing_recv(src_pe, dst_pe, msg_size, send_time, contend_wait);
    zsim_magic_op(ZSIM_MAGIC_OP_MPI_RECV);
}

/* Charge the sender's cross-rank contention wait (shared outbound link busy)
 * as idle cycles on this rank's core. Cycles only -- instr counts stay pure
 * compute. Emitted ONCE per send (wait may be 0) so the counted-instruction
 * stream is independent of how many sends actually queued; bracketed by the
 * comm window so the op itself is not counted -> instr counts stay pure and
 * deterministic regardless of wall-clock contention. The CALLER brackets this
 * (and contend_reserve's lock spin) inside the comm window so neither the op
 * nor the lock-spin/usleep is counted. */
static void inject_contend(uint64_t contend_wait) {
    tl_mpi_params.sim_contend_wait = contend_wait;
    zsim_magic_op(ZSIM_MAGIC_OP_MPI_CONTEND);
}

static void register_mpi_params(void) {
    /* Convey the address of this thread's params block in %rdx while the
     * REGISTER opcode goes in %rcx, so the simulator can read {src_pe, dst_pe,
     * msg_size} from it on every subsequent MPI_SEND/RECV (and charge the NoC).
     * %rdx is caller-clobbered, so this is safe. */
#ifdef __x86_64__
    void* pp = (void*)&tl_mpi_params;
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;"
                         : : "c"((uint64_t)ZSIM_MAGIC_OP_MPI_REGISTER), "d"(pp)
                         : "memory");
    COMPILER_BARRIER();
#endif
}

/* 1.12.17 (M1, user 2026-10-22): the transport's own memory is registered with
 * the simulator, and PE accesses inside it are not priced: a message costs one
 * read of the sender's buffer, the injected Garnet transfer and one write of the
 * receiver's buffer. Thread mode only (the exec-method MPI model). */
static void register_transport_range(const void* base, size_t bytes) {
#ifdef __x86_64__
    static volatile uint64_t r[2];
    r[0] = (uint64_t)(uintptr_t)base;
    r[1] = (uint64_t)bytes;
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;"
                         : : "c"((uint64_t)ZSIM_MAGIC_OP_MPI_TRANSPORT_RANGE), "d"((void*)r)
                         : "memory");
    COMPILER_BARRIER();
#else
    (void)base; (void)bytes;
#endif
}

/* 1.12.17 (M1): thread-mode staging (messages evicted from a full ring while the
 * receiver waits for another source) lives in one transport arena, so it is
 * registered transport memory too: per rank, cap slots, an arrival-order list
 * and a used map. Capacity: every other rank's full ring. Overflow refuses. */
static MessageSlot* g_stage_slots = nullptr;
static int32_t*     g_stage_order = nullptr;   /* [rank][cap]: slot index, arrival order */
static uint32_t*    g_stage_n     = nullptr;   /* [rank]: staged count */
static uint8_t*     g_stage_used  = nullptr;   /* [rank][cap] */
/* (1.12.177): one chunk-sized copy buffer per rank, in the same transport arena: a send reads the
 * sender's buffer into it (priced) before it takes the receiver's mailbox lock, a receive moves
 * the chunk into it under the lock and writes the user buffer from it (priced) after the lock is
 * released, so no mailbox lock is held across priced work */
static char*        g_copy_bufs   = nullptr;   /* [rank][PIMID_MPI_MAX_MSG_BYTES] */
static inline char* copy_buf(int r) { return g_copy_bufs + (size_t)r * PIMID_MPI_MAX_MSG_BYTES; }
static void*        g_stage_arena = nullptr;
static size_t       g_stage_arena_bytes = 0;
static uint32_t     g_stage_cap = 0;

static bool stage_arena_create(int nranks) {
    g_stage_cap = (uint32_t)PIMID_MPI_RING_SLOTS * (uint32_t)nranks;
    const size_t ns = (size_t)nranks * g_stage_cap;
    const size_t b_slots = ns * sizeof(MessageSlot);
    const size_t b_order = ns * sizeof(int32_t);
    const size_t b_n     = (size_t)nranks * sizeof(uint32_t);
    const size_t b_used  = ns;
    const size_t b_copy  = (size_t)nranks * PIMID_MPI_MAX_MSG_BYTES;   /* (1.12.177) */
    g_stage_arena_bytes = b_slots + b_copy + b_order + b_n + b_used;
    g_stage_arena = calloc(1, g_stage_arena_bytes);
    if (!g_stage_arena) {
        fprintf(stderr, "[pimid_mpi] thread-mode staging arena alloc failed (%zu bytes)\n", g_stage_arena_bytes);
        return false;
    }
    char* p = (char*)g_stage_arena;
    g_stage_slots = (MessageSlot*)p; p += b_slots;
    g_copy_bufs   = p;               p += b_copy;
    g_stage_order = (int32_t*)p;     p += b_order;
    g_stage_n     = (uint32_t*)p;    p += b_n;
    g_stage_used  = (uint8_t*)p;
    return true;
}

static size_t shared_bytes_for(int nranks) {
    return sizeof(SharedHeader) + (size_t)nranks * sizeof(Mailbox);
}

/* 1.6 thread mode: the "shared" region is plain process memory -- all ranks
 * live in this address space, so the mailbox/barrier/occupancy layout is used
 * verbatim with no shm object, no attach race, no cross-process lifetime. */
static bool create_heap_region(int nranks) {
    g_shared_bytes = shared_bytes_for(nranks);
    void* p = calloc(1, g_shared_bytes);
    if (!p) {
        fprintf(stderr, "[pimid_mpi] thread-mode region alloc failed (%zu bytes)\n",
                g_shared_bytes);
        return false;
    }
    g_shared = (SharedHeader*)p;
    g_shared->barrier_count = 0;
    g_shared->nranks        = nranks;
    g_shared->occ_lk        = 0;
    for (int i = 0; i < PIMID_MPI_CONTEND_MAX_POINTS; i++)
        g_shared->busy_until[i] = 0;
    pthread_mutex_init(&g_shared->bar_mtx, nullptr);
    pthread_cond_init(&g_shared->bar_cv, nullptr);
    g_shared->bar_max_arr = 0;
    g_shared->bar_release = 0;
    for (int i = 0; i < nranks; i++) {
        Mailbox* mb = &g_shared->mailboxes[i];
        mb->lk = 0;
        mb->head = mb->tail = mb->count = 0;
        memset((void*)mb->consumed, 0, sizeof(mb->consumed));
        pthread_mutex_init(&mb->mtx, nullptr);
        pthread_cond_init(&mb->can_recv, nullptr);
        pthread_cond_init(&mb->can_send, nullptr);
    }
    __sync_synchronize();
    g_shared->barrier_gen = 1;   /* >0 == initialized (same sentinel as shm) */
    if (!stage_arena_create(nranks)) return false;
    register_transport_range(g_shared, g_shared_bytes);
    register_transport_range(g_stage_arena, g_stage_arena_bytes);
    return true;
}

/* Rank 0 creates and initializes the shm segment; other ranks attach.
 * Synchronization on attach: each non-zero rank busy-waits for the
 * "initialized" sentinel in the header (barrier_gen >= 0 after init,
 * we use barrier_gen=-1 to mean uninitialized) since they're spawned
 * concurrently and may race with rank 0. */
static bool open_or_create_shm(int rank, int nranks, const char* shm_name) {
    g_shared_bytes = shared_bytes_for(nranks);

    int oflag = (rank == 0) ? (O_CREAT | O_RDWR) : O_RDWR;
    int retries = (rank == 0) ? 1 : 200;  /* up to ~2s for rank0 to create */
    for (int i = 0; i < retries; i++) {
        g_shm_fd = shm_open(shm_name, oflag, 0600);
        if (g_shm_fd >= 0) break;
        if (rank == 0) {
            fprintf(stderr, "[pimid_mpi rank0] shm_open(%s) failed: %s\n",
                    shm_name, strerror(errno));
            return false;
        }
        usleep(10000);  /* 10ms */
    }
    if (g_shm_fd < 0) {
        fprintf(stderr, "[pimid_mpi rank%d] could not attach to %s after wait\n",
                rank, shm_name);
        return false;
    }

    if (rank == 0) {
        g_we_created_shm = true;
        if (ftruncate(g_shm_fd, (off_t)g_shared_bytes) < 0) {
            fprintf(stderr, "[pimid_mpi rank0] ftruncate failed: %s\n", strerror(errno));
            return false;
        }
    } else {
        /* Wait until rank 0 has finished ftruncating */
        struct stat st;
        for (int i = 0; i < 200; i++) {
            if (fstat(g_shm_fd, &st) == 0 && (size_t)st.st_size >= g_shared_bytes) break;
            usleep(10000);
        }
    }

    void* p = mmap(NULL, g_shared_bytes, PROT_READ | PROT_WRITE,
                   MAP_SHARED, g_shm_fd, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "[pimid_mpi rank%d] mmap failed: %s\n", rank, strerror(errno));
        return false;
    }
    g_shared = (SharedHeader*)p;

    if (rank == 0) {
        /* Initialize barrier + mailbox state (plain fields; no pthread
         * primitives left in the kernel-wait-free transport). */
        g_shared->barrier_count = 0;
        g_shared->barrier_gen   = 0;
        g_shared->nranks        = nranks;
        g_shared->occ_lk        = 0;
        for (int i = 0; i < PIMID_MPI_CONTEND_MAX_POINTS; i++) {
            g_shared->busy_until[i] = 0;
        }
        for (int i = 0; i < nranks; i++) {
            Mailbox* mb = &g_shared->mailboxes[i];
            mb->lk = 0;
            mb->head = mb->tail = mb->count = 0;
        }
        /* Publish "ready" marker -- non-zero ranks busy-wait on this. */
        __sync_synchronize();
        g_shared->barrier_gen = 1;   /* >0 == initialized */
    } else {
        for (int i = 0; i < 200; i++) {
            __sync_synchronize();
            if (g_shared->barrier_gen >= 1) break;
            usleep(10000);
        }
        if (g_shared->nranks != nranks) {
            fprintf(stderr, "[pimid_mpi rank%d] nranks mismatch: %d (env) vs %d (shm)\n",
                    rank, nranks, g_shared->nranks);
        }
    }
    return true;
}

/* ---- Wait-event tracing (PIMID_MPI_TRACE=1; diagnostic only) ----
 * One line per wait ENTER/EXIT to an unbuffered per-rank file, recording the
 * wait site and the protocol state visible at that moment. At a freeze, the
 * union of per-rank traces shows every rank's last protocol action. */
static FILE* g_trace = nullptr;
static void trace_init(int rank) {
    const char* e = getenv("PIMID_MPI_TRACE");
    if (!e || e[0] != '1') return;
    char p[300];
    snprintf(p, sizeof(p), "/tmp/pimid_mpi_trace_%d_rank%d.log",
             (int)getppid(), rank);  /* launcher pid: instances don't clobber */
    g_trace = fopen(p, "w");
    if (g_trace) setvbuf(g_trace, nullptr, _IONBF, 0);
}
#define MPITRACE(fmt, ...) do { if (g_trace) { \
    struct timespec _ts; clock_gettime(CLOCK_MONOTONIC, &_ts); \
    fprintf(g_trace, "%ld.%03ld r%d " fmt "\n", (long)_ts.tv_sec, \
            _ts.tv_nsec/1000000L, g_rank, ##__VA_ARGS__); } } while (0)

/* ---- Send / Recv core ---- */

static void ring_evict_head(Mailbox* mb, int r);   /* 1.12.17, below */

/* Push one chunk to the dest mailbox. A full ring moves its oldest message to the receiver staging (1.12.17). */
static int send_chunk(int dest, int tag, const char* data, uint32_t size,
                      uint32_t chunk_idx, uint32_t n_chunks, uint32_t msg_id,
                      uint64_t send_time, uint64_t contend_wait, uint64_t msg_bytes) {
    Mailbox* mb = &g_shared->mailboxes[dest];
    MPITRACE("send_enter dest=%d tag=%d chunk=%u/%u", dest, tag, chunk_idx, n_chunks);
    /* Bracket the transport (poll + payload copy) so the simulator keeps the
     * core attached without counting the wall-clock poll as compute. */
    if (g_thread_mode) {
        /* Thread mode: cond-parked wait (OMP discipline). The parked thread
         * leaves its simulated core; the wait costs zero simulated cycles. */
        /* v5 wait discipline: ACTIVE PHASE-LOCKSTEP SPIN. No syscalls, no
         * scheduler leave, no comm window: the spinning core advances through
         * simulated phases in lockstep with all cores, so the wait ends at
         * the peer's DETERMINISTIC publish phase -- wall time cannot vote.
         * (cond parking rejoined at wall-dependent phases; window-frozen
         * cores deadlocked the phase barrier. Spin instrs are counted,
         * deterministically; purity refinement tracked for 1.6.1.) */
        /* 1.12.17 (M1): outside the comm window, so the read of the sender's
         * buffer is priced; the copy buffer is registered transport memory, so
         * the write into it is not. (1.12.177): done before the receiver's mailbox
         * lock is taken -- a sender no longer holds that lock across priced work,
         * and a rank blocked on the lock no longer waits on another rank's copy. */
        char* cb = copy_buf(g_rank);
        if (data && size > 0) priced_memcpy(cb, data, size);   /* 1.12.18: RtPriced */
        mpi_comm_begin();   /* frozen-clock bracket: plugin rewinds at END */
        pthread_mutex_lock(&mb->mtx);
        while (mb->count >= PIMID_MPI_RING_SLOTS)
            ring_evict_head(mb, dest);   /* 1.12.17: never block on a full ring */
        MessageSlot* slot = &mb->slots[mb->tail];
        slot->src           = g_rank;
        slot->tag           = tag;
        slot->size          = size;
        slot->chunk_idx     = chunk_idx;
        slot->n_chunks      = n_chunks;
        slot->msg_id           = msg_id;
        slot->sim_send_time    = send_time;
        slot->sim_contend_wait = contend_wait;
        slot->msg_bytes        = msg_bytes;
        if (data && size > 0) memcpy(slot->payload, cb, size);   /* transport to transport, in the window */
        mb->tail = (mb->tail + 1) % PIMID_MPI_RING_SLOTS;
        mb->count++;
        pthread_cond_signal(&mb->can_recv);
        pthread_mutex_unlock(&mb->mtx);
        mpi_comm_end();
        MPITRACE("send_done dest=%d", dest);
        return 0;
    }
    mpi_comm_begin();
    bool waited = false;
    for (;;) {
        mb_lock(mb);
        if (mb->count < PIMID_MPI_RING_SLOTS) break;   /* lock held */
        mb_unlock(mb);
        if (!waited) { MPITRACE("send_WAIT_full dest=%d", dest); waited = true; }
        usleep(200);   /* ring full: poll until the consumer drains a slot */
    }
    MessageSlot* slot = &mb->slots[mb->tail];
    slot->src           = g_rank;
    slot->tag           = tag;
    slot->size          = size;
    slot->chunk_idx     = chunk_idx;
    slot->n_chunks      = n_chunks;
    slot->msg_id           = msg_id;
    slot->sim_send_time    = send_time;
    slot->sim_contend_wait = contend_wait;
    slot->msg_bytes        = msg_bytes;
    if (data && size > 0) memcpy(slot->payload, data, size);
    mb->tail = (mb->tail + 1) % PIMID_MPI_RING_SLOTS;
    mb->count++;
    uint32_t cnow = mb->count;
    mb_unlock(mb);
    mpi_comm_end();
    MPITRACE("send_done dest=%d count_now=%u", dest, cnow);
    return 0;
}

/* Pop one message from the local mailbox. Blocks if empty. Thread mode
 * matches want_src (MPI_ANY_SOURCE = any); process mode keeps its historical
 * FIFO-pop and ignores want_src. */
/* 1.12.17 (M1): thread-mode receive in two halves around the payload copy.
 * recv_acquire_thread finds the next message from want_src (staging first, then
 * the ring, evicting the ring head to staging when the ring is full and the
 * wanted source is absent -- the same matching and FIFO order as recv_slot) and
 * returns its slot with the comm window CLOSED and the mailbox mutex HELD
 * (staging included, since 1.12.17 senders also evict into it).
 * recv_release_thread frees it. */
struct RecvRef { bool staged; int pos; uint32_t ring_idx; };

static MessageSlot* stage_slot_at(int r, uint32_t i) {
    return &g_stage_slots[(size_t)r * g_stage_cap + i];
}

static void stage_push(int r, const MessageSlot* src) {
    uint32_t n = g_stage_n[r];
    if (n >= g_stage_cap) {
        fprintf(stderr, "[pimid_mpi] rank %d: message staging full (%u messages evicted from a full ring and not yet "
                "received); refusing to drop or reorder a message\n", r, n);
        abort();
    }
    uint32_t i = 0;
    uint8_t* used = &g_stage_used[(size_t)r * g_stage_cap];
    while (used[i]) i++;
    used[i] = 1;
    MessageSlot* d = stage_slot_at(r, i);
    d->src = src->src; d->tag = src->tag; d->size = src->size;
    d->chunk_idx = src->chunk_idx; d->n_chunks = src->n_chunks; d->msg_id = src->msg_id;
    d->sim_send_time = src->sim_send_time; d->sim_contend_wait = src->sim_contend_wait;
    d->msg_bytes = src->msg_bytes;
    if (src->size > 0) memcpy(d->payload, src->payload, src->size);
    g_stage_order[(size_t)r * g_stage_cap + n] = (int32_t)i;
    g_stage_n[r] = n + 1;
}

/* Move the ring head -- the oldest message -- of rank r's mailbox to r's
 * staging (mailbox mutex held). Staging stays older than the ring, so each
 * source's messages keep their order. Used by the receiver and, when the ring
 * is full, by a sender (1.12.17), so a send never blocks on a full ring. */
static void ring_evict_head(Mailbox* mb, int r) {
    stage_push(r, &mb->slots[mb->head]);
    mb->consumed[mb->head] = 0;
    mb->head = (mb->head + 1) % PIMID_MPI_RING_SLOTS;
    mb->count--;
    while (mb->count > 0 && mb->consumed[mb->head]) {
        mb->consumed[mb->head] = 0;
        mb->head = (mb->head + 1) % PIMID_MPI_RING_SLOTS;
        mb->count--;
    }
    pthread_cond_broadcast(&mb->can_send);
}

static MessageSlot* recv_acquire_thread(int want_src, RecvRef* ref, bool more = false) {
    Mailbox* mb = &g_shared->mailboxes[g_rank];
    int32_t* order = &g_stage_order[(size_t)g_rank * g_stage_cap];
    mpi_comm_begin();   /* frozen-clock bracket: plugin rewinds at END */
    pthread_mutex_lock(&mb->mtx);
    for (;;) {
        /* Staging first (it is older than the ring); a sender may have moved
         * the wanted message there while this rank waited. */
        for (uint32_t k = 0; k < g_stage_n[g_rank]; k++) {
            MessageSlot* s = stage_slot_at(g_rank, (uint32_t)order[k]);
            if (want_src < 0 || s->src == want_src) {
                ref->staged = true; ref->pos = (int)k; ref->ring_idx = 0;
                mpi_comm_end();
                MPITRACE("recv_done src=%d tag=%d (stage)", s->src, s->tag);
                return s;   /* mailbox mutex held */
            }
        }
        int found = -1;
        for (uint32_t k = 0; k < mb->count; k++) {
            uint32_t idx = (mb->head + k) % PIMID_MPI_RING_SLOTS;
            if (mb->consumed[idx]) continue;
            if (want_src < 0 || mb->slots[idx].src == want_src) { found = (int)idx; break; }
        }
        if (found >= 0) {
            ref->staged = false; ref->pos = -1; ref->ring_idx = (uint32_t)found;
            mpi_comm_end();
            return &mb->slots[found];   /* mailbox mutex held */
        }
        if (mb->count >= PIMID_MPI_RING_SLOTS) {
            ring_evict_head(mb, g_rank);
            MPITRACE("recv_EVICT_to_stage want=%d", want_src);
            continue;
        }
        MPITRACE("recv_WAIT_src=%d", want_src);
        /* (1.12.177): a later chunk of a message already received carries no new arrival -- the rank
         * goes on from its own clock -- so that wait is not announced as a wait on a peer */
        if (!more) mpi_wait_peer(1, want_src);
        pthread_cond_wait(&mb->can_recv, &mb->mtx);
    }
}

struct RecvTiming { int src; uint64_t size; uint64_t send_time; uint64_t contend_wait; };
static void recv_release_thread(const RecvRef* ref, char* keep = nullptr, const char* from = nullptr, size_t n = 0,
                                const RecvTiming* tm = nullptr) {
    Mailbox* mb = &g_shared->mailboxes[g_rank];
    mpi_comm_begin();
    if (keep && n > 0) memcpy(keep, from, n);   /* (1.12.177): the chunk leaves the slot before the slot is freed */
    /* (1.12.177): the rendezvous parameters are written here, in the window: between the end of the wait
     * and the arrival the rank does nothing that is priced, so no access is stamped before the data */
    if (tm) set_timing_recv(tm->src, g_rank, tm->size, tm->send_time, tm->contend_wait);
    if (ref->staged) {
        int32_t* order = &g_stage_order[(size_t)g_rank * g_stage_cap];
        const uint32_t i = (uint32_t)order[ref->pos];
        g_stage_used[(size_t)g_rank * g_stage_cap + i] = 0;
        const uint32_t n = g_stage_n[g_rank];
        for (uint32_t k = (uint32_t)ref->pos; k + 1 < n; k++) order[k] = order[k + 1];
        g_stage_n[g_rank] = n - 1;
        pthread_mutex_unlock(&mb->mtx);
    } else {
        mb->consumed[ref->ring_idx] = 1;
        while (mb->count > 0 && mb->consumed[mb->head]) {
            mb->consumed[mb->head] = 0;
            mb->head = (mb->head + 1) % PIMID_MPI_RING_SLOTS;
            mb->count--;
            pthread_cond_signal(&mb->can_send);
        }
        pthread_mutex_unlock(&mb->mtx);
    }
    mpi_comm_end();
}

static void recv_slot(MessageSlot* out, int want_src = -1) {
    Mailbox* mb = &g_shared->mailboxes[g_rank];
    MPITRACE("recv_enter");
    /* Bracket the transport (poll + payload copy): the simulator keeps the core
     * attached across the polling sleeps without counting them, so the post-recv
     * compute is counted and cycles do not depend on wall-clock poll duration. */
    if (g_thread_mode) {
        /* SOURCE-MATCHED receive: scan from head for the earliest unconsumed
         * slot from want_src (or any). Popping in wall-arrival order made the
         * rendezvous consume varying send_time stamps run-to-run -- the 6%
         * determinism jitter. Matching by source makes the consumed stamp a
         * pure function of the program's communication pattern.
         *
         * UNEXPECTED-MESSAGE STAGING (1.9.6 deadlock fix). The mailbox ring holds
         * only PIMID_MPI_RING_SLOTS (16) slots. A source-matched receiver that
         * wants a specific source can head-of-line block: an all-to-root
         * collective (MPI_Gather/Reduce, the shape bfs runs every frontier level)
         * fills this mailbox with 16 senders the receiver has not asked for yet
         * while the source it IS waiting for is a sender BLOCKED in send_chunk
         * (ring full) and thus can never enqueue -> deadlock. It fires precisely
         * once nranks > RING_SLOTS (bfs 8/16 ranks are safe, 32/64 hang) and grows
         * more likely the more per-level gathers a kernel runs (bfs, not the
         * one-shot-reduce kernels). CURE: this rank keeps a private staging queue;
         * when the ring is full and the wanted source is absent, EVICT the
         * head-most message into staging (freeing a slot, waking a blocked
         * sender). Messages are still matched by source when consumed -- from
         * staging (older) before the ring (newer) so per-source FIFO and the
         * consumed sim_send_time stamp are unchanged -> determinism preserved;
         * only the transport buffering depth changes. Deadlock-free: the receiver
         * always drains the ring, so every sender eventually enqueues. */
        static thread_local std::vector<MessageSlot> tl_stage;
        for (size_t i = 0; i < tl_stage.size(); i++) {
            if (want_src < 0 || tl_stage[i].src == want_src) {
                *out = tl_stage[i];
                tl_stage.erase(tl_stage.begin() + (long)i);
                MPITRACE("recv_done src=%d tag=%d (stage)", out->src, out->tag);
                return;
            }
        }
        mpi_comm_begin();   /* frozen-clock bracket: plugin rewinds at END */
        pthread_mutex_lock(&mb->mtx);
        for (;;) {
            int found = -1;
            for (uint32_t k = 0; k < mb->count; k++) {
                uint32_t idx = (mb->head + k) % PIMID_MPI_RING_SLOTS;
                if (mb->consumed[idx]) continue;
                if (want_src < 0 || mb->slots[idx].src == want_src) {
                    found = (int)idx;
                    break;
                }
            }
            if (found >= 0) {
                *out = mb->slots[found];
                mb->consumed[found] = 1;
                /* Advance head over the consumed prefix. */
                while (mb->count > 0 && mb->consumed[mb->head]) {
                    mb->consumed[mb->head] = 0;
                    mb->head = (mb->head + 1) % PIMID_MPI_RING_SLOTS;
                    mb->count--;
                    pthread_cond_signal(&mb->can_send);
                }
                break;
            }
            /* No match in the ring. If it is FULL, evict the head-most message to
             * staging to break a possible head-of-line deadlock (see above). The
             * head is unconsumed and, since the scan above found no want_src, is
             * never the message we want, so eviction cannot drop it. */
            if (mb->count >= PIMID_MPI_RING_SLOTS) {
                tl_stage.push_back(mb->slots[mb->head]);
                mb->consumed[mb->head] = 0;
                mb->head = (mb->head + 1) % PIMID_MPI_RING_SLOTS;
                mb->count--;
                pthread_cond_signal(&mb->can_send);
                while (mb->count > 0 && mb->consumed[mb->head]) {
                    mb->consumed[mb->head] = 0;
                    mb->head = (mb->head + 1) % PIMID_MPI_RING_SLOTS;
                    mb->count--;
                    pthread_cond_signal(&mb->can_send);
                }
                MPITRACE("recv_EVICT_to_stage src=%d want=%d", tl_stage.back().src, want_src);
                continue;   /* re-scan: ring now has space, blocked sender can enqueue */
            }
            MPITRACE("recv_WAIT_src=%d", want_src);
            mpi_wait_peer(1, want_src);
            pthread_cond_wait(&mb->can_recv, &mb->mtx);
        }
        pthread_mutex_unlock(&mb->mtx);
        mpi_comm_end();
        MPITRACE("recv_done src=%d tag=%d", out->src, out->tag);
        return;
    }
    mpi_comm_begin();
    bool waited = false;
    for (;;) {
        mb_lock(mb);
        if (mb->count > 0) break;   /* lock held */
        mb_unlock(mb);
        if (!waited) { MPITRACE("recv_WAIT_empty"); waited = true; }
        usleep(200);   /* empty: poll until a producer pushes */
    }
    *out = mb->slots[mb->head];
    mb->head = (mb->head + 1) % PIMID_MPI_RING_SLOTS;
    mb->count--;
    uint32_t cnow = mb->count;
    mb_unlock(mb);
    mpi_comm_end();
    MPITRACE("recv_done src=%d tag=%d count_now=%u", out->src, out->tag, cnow);
}

/* ---- MPI API ---- */

extern "C" {

int MPI_Init(int *argc, char ***argv) {
    (void)argc; (void)argv;

    /* 1.6 thread mode: the main harness already assigned this thread's rank
     * (TLS) and built the in-process region BEFORE any rank started. Every
     * rank-thread calls MPI_Init; each just registers its own params block. */
    if (g_thread_mode) {
        trace_init(g_rank);
        register_mpi_params();
        /* 1.12.18 (user ruling 2026-10-24): the rank's collective scratch is NOT
         * allocated or zeroed here (in co-sim this runs on the host before
         * offload); scratch_or_vec allocates it device-side at first use, inside
         * the runtime window. g_scr's entries start empty (calloc). */
        return MPI_SUCCESS;
    }

    if (g_initialized.exchange(true)) return MPI_SUCCESS;

    const char* env_n = getenv("PIMID_MPI_RANKS");
    const char* env_r = getenv("PIMID_MPI_RANK");
    g_nranks = env_n ? atoi(env_n) : 1;
    g_rank   = env_r ? atoi(env_r) : 0;
    if (g_nranks < 1) g_nranks = 1;
    if (g_rank < 0)   g_rank = 0;

    /* Cross-rank contention config (read once). Contention itself is always
     * active in detailed mode -- only the granularity is tunable here. */
    {
        const char* cp = getenv("PIMID_MPI_CONTEND_POINTS");
        if (cp && *cp) {
            long v = atol(cp);
            if (v < 1) v = 1;
            if (v > PIMID_MPI_CONTEND_MAX_POINTS) v = PIMID_MPI_CONTEND_MAX_POINTS;
            g_contend_points = (uint32_t)v;
        }
        const char* cd = getenv("PIMID_DEBUG_CONTEND");
        g_contend_dbg = (cd && cd[0] == '1') ? 1 : 0;
    }

    trace_init(g_rank);

    if (g_nranks == 1) {
        /* Solo run: no shm needed, MPI is a no-op transport. */
        register_mpi_params();
        return MPI_SUCCESS;
    }

    const char* shm_env = getenv("PIMID_MPI_SHM");
    if (shm_env && *shm_env) {
        snprintf(g_shm_name, sizeof(g_shm_name), "%s", shm_env);
    } else {
        /* Fallback: derive a name from PPID (the pimid launcher pid),
         * so siblings of the same launch agree on the same shm. */
        snprintf(g_shm_name, sizeof(g_shm_name), "/pimid_mpi_%d", (int)getppid());
    }

    if (!open_or_create_shm(g_rank, g_nranks, g_shm_name)) {
        return -1;
    }
    register_mpi_params();
    return MPI_SUCCESS;
}

/* (1.12.122): the binaries link the host MPI library only for its API; the guest
 * now loads this library under that library's name (see main.cpp,
 * guestMpiLibDir), so every MPI symbol the binaries use is defined here. */
int MPI_Abort(MPI_Comm comm, int errorcode) {
    (void)comm;
    fprintf(stderr, "[pimid_mpi] rank %d: MPI_Abort(%d)\n", g_rank, errorcode);
    fflush(nullptr);
    _exit(errorcode != 0 ? (errorcode & 0xff) : 1);   /* thread mode: every rank is a thread of this process */
}

int MPI_Finalize(void) {
    /* 1.6 thread mode: the region belongs to the harness (freed after all
     * rank-threads join); a rank finalizing must not unmap under its peers. */
    if (g_thread_mode) return MPI_SUCCESS;

    if (g_shared) {
        munmap(g_shared, g_shared_bytes);
        g_shared = nullptr;
    }
    if (g_shm_fd >= 0) {
        close(g_shm_fd);
        g_shm_fd = -1;
    }
    /* Rank 0 also unlinks the shm name so it doesn't outlive the launch. */
    if (g_we_created_shm && g_shm_name[0]) {
        shm_unlink(g_shm_name);
    }
    g_initialized.store(false);
    return MPI_SUCCESS;
}

int MPI_Comm_rank(MPI_Comm comm, int *rank) {
    (void)comm; *rank = g_rank;
    return MPI_SUCCESS;
}

int MPI_Comm_size(MPI_Comm comm, int *size) {
    (void)comm; *size = g_nranks;
    return MPI_SUCCESS;
}

/* ---- Point-to-point ---- */

/* 1.12.17: byte-level point-to-point under MPI_Send / MPI_Recv / MPI_Sendrecv
 * and the collectives. send_bytes and recv_bytes are the former bodies of
 * MPI_Send and MPI_Recv; xchg_bytes is the exchange step of the collective
 * algorithms (and of MPI_Sendrecv). */
struct SendStamp { uint64_t send_time; uint64_t contend_wait; uint32_t msg_id; };

static SendStamp send_prelude(int dest, size_t nbytes) {
    /* Emit the SEND timing op FIRST: it charges send latency on this core and
     * (under instrumentation) writes the current simulated time into
     * tl_mpi_params.sim_now, which we then stamp onto every chunk so the
     * receiver can rendezvous in simulated time. msg_id is incremented here. */
    inject_timing_send(g_rank, dest, nbytes);
    SendStamp st;
    st.send_time = tl_mpi_params.sim_now;
    st.msg_id    = (uint32_t)tl_mpi_params.msg_id;

    /* Reserve the shared contention point at the message's sim send-time and
     * charge the resulting queueing wait on this (sender) core. The reservation
     * duration is the REAL per-message NoC cost (noc_lat) the plugin just
     * computed from Garnet -- no magic flit constant. The same wait is stamped
     * onto each chunk so the receiver's rendezvous arrival accounts for it too.
     * Keyed on sim send-time -> independent of wall clock. The whole block
     * (occupancy lock spin + CONTEND op) is bracketed by the comm window so none
     * of it is counted -> instr counts stay pure compute and deterministic.
     * Applied ONLY in detailed mode (noc_detailed): analytical/simple -- the one
     * intentional approximation -- and native runs skip it automatically. */
    st.contend_wait = 0;
    if (tl_mpi_params.noc_detailed) {
        uint64_t dur = tl_mpi_params.noc_lat;
        mpi_comm_begin();
        st.contend_wait = contend_reserve(dest, st.send_time, dur);
        inject_contend(st.contend_wait);
        mpi_comm_end();
    }
    return st;
}

static uint32_t chunks_of(size_t nbytes) {
    return nbytes == 0 ? 1u
                       : (uint32_t)((nbytes + PIMID_MPI_MAX_MSG_BYTES - 1) / PIMID_MPI_MAX_MSG_BYTES);
}

static void send_chunk_at(int dest, int tag, const char* p, size_t nbytes,
                          uint32_t i, uint32_t n, const SendStamp& st) {
    const size_t off = (size_t)i * PIMID_MPI_MAX_MSG_BYTES;
    const size_t left = (nbytes > off) ? nbytes - off : 0;
    const uint32_t take = (uint32_t)((left < PIMID_MPI_MAX_MSG_BYTES) ? left : PIMID_MPI_MAX_MSG_BYTES);
    send_chunk(dest, tag, take ? p + off : nullptr, take, i, n, st.msg_id, st.send_time, st.contend_wait, nbytes);
}

static int send_bytes(const void* buf, size_t nbytes, int dest, int tag) {
    if (g_nranks <= 1) return MPI_SUCCESS;
    if (dest < 0 || dest >= g_nranks) return MPI_SUCCESS;
    RtScope rs;   /* 1.12.18: the send's bookkeeping is runtime */
    /* 1.12.18: the protocol primitive, charged BEFORE the send stamp, so the
     * receiver's rendezvous arrival includes it */
    rt_event(tl_coll ? RTK_MPI_COLL_STEP : RTK_MPI_P2P, 2);
    const SendStamp st = send_prelude(dest, nbytes);
    const uint32_t n = chunks_of(nbytes);
    for (uint32_t i = 0; i < n; i++) send_chunk_at(dest, tag, (const char*)buf, nbytes, i, n, st);
    return MPI_SUCCESS;
}

/* Receive progress of one message (thread mode). */
struct RecvState { int want; int src; size_t written; uint64_t send_time; uint64_t contend_wait; bool done; bool timed; };

/* 1.12.17 (M1): only the message bytes move, straight from the slot
 * (registered transport memory, not priced) into the user buffer
 * (the receiver's write, priced, outside the comm window). The old
 * path copied the whole 64 KB MessageSlot onto the stack per chunk. */
static void recv_chunk_thread(RecvState* r, char* p, size_t nbytes) {
    RecvRef ref;
    MessageSlot* s = recv_acquire_thread(r->want, &ref, r->timed);
    if (r->src < 0) { r->src = s->src; r->want = s->src; }
    r->send_time    = s->sim_send_time;     /* all chunks share the stamp */
    r->contend_wait = s->sim_contend_wait;  /* and the contention wait */
    size_t space = (nbytes > r->written) ? (nbytes - r->written) : 0;
    size_t take = (s->size < space) ? s->size : space;
    const size_t total = (s->msg_bytes < nbytes) ? (size_t)s->msg_bytes : nbytes;   /* what the whole receive writes */
    r->done = (s->chunk_idx + 1 >= s->n_chunks);
    char* cb = copy_buf(g_rank);
    const RecvTiming tm = { r->src, (uint64_t)total, r->send_time, r->contend_wait };
    recv_release_thread(&ref, cb, s->payload, take, r->timed ? nullptr : &tm);   /* the mailbox lock is released here */
    /* (1.12.177): the rendezvous comes first -- the clock moves to the message's arrival -- and the
     * payload is written after it: a receive's copy cannot precede the data. It used to copy at
     * the clock it had when it blocked, then advance; those writes were stamped before the
     * message existed (and behind the DRAM replay's cut once the other ranks had moved on). */
    if (!r->timed) { zsim_magic_op(ZSIM_MAGIC_OP_MPI_RECV); r->timed = true; }   /* parameters set in the window */
    if (p && take > 0) priced_memcpy(p + r->written, cb, take);   /* 1.12.18: RtPriced */
    r->written += take;
}

/* Returns the bytes received. */
static size_t recv_bytes(void* buf, size_t nbytes, int source) {
    if (g_nranks <= 1) return 0;
    /* MPI_PROC_NULL (-1) recv is a no-op in real MPI; without this guard an
     * edge rank (e.g. stencil halo exchange) blocks forever on a mailbox no
     * rank will ever send to. Mirrors the dest guard in MPI_Send. */
    if (source < 0 || source >= g_nranks) return 0;
    char* p = (char*)buf;
    if (g_thread_mode) {   /* thread mode: honor source */
        RtScope rs;   /* 1.12.18 */
        RecvState r = { source, -1, 0, 0, 0, false };
        do recv_chunk_thread(&r, p, nbytes); while (!r.done);
        if (!r.timed) inject_timing_recv(r.src, g_rank, r.written, r.send_time, r.contend_wait);   /* (1.12.177): done at the first chunk */
        /* 1.12.18: the protocol primitive, AFTER the rendezvous (added to the ordering) */
        rt_event(tl_coll ? RTK_MPI_COLL_STEP : RTK_MPI_P2P, 2);
        return r.written;
    }
    /* Receive until we've assembled a complete message (1+ chunks). For
     * the FIFO-matching policy this implementation has always used, we
     * receive chunks of the next message in order. */
    size_t written = 0;
    int last_src = -1;
    uint64_t send_time = 0;
    uint64_t contend_wait = 0;
    MessageSlot slot;
    do {
        recv_slot(&slot, -1);
        if (last_src < 0) last_src = slot.src;
        send_time    = slot.sim_send_time;     /* all chunks share the stamp */
        contend_wait = slot.sim_contend_wait;  /* and the contention wait */
        size_t space = (nbytes > written) ? (nbytes - written) : 0;
        size_t take = (slot.size < space) ? slot.size : space;
        if (p && take > 0) memcpy(p + written, slot.payload, take);
        written += take;
    } while (slot.chunk_idx + 1 < slot.n_chunks);

    inject_timing_recv(last_src, g_rank, written, send_time, contend_wait);
    return written;
}

/* One exchange step: send to dest and receive from source, chunk by chunk in
 * a fixed order (send chunk i, then receive chunk i). A step in which every
 * rank sends before it receives (ring, recursive doubling, pairwise) can then
 * never fill a peer's ring and deadlock, and the order does not depend on wall
 * time. The timing ops are those of MPI_Send followed by MPI_Recv. Returns the
 * bytes received. */
static size_t xchg_bytes(const void* sbuf, size_t sbytes, int dest,
                         void* rbuf, size_t rbytes, int source) {
    if (g_nranks <= 1) return 0;
    if (!g_thread_mode || dest < 0 || dest >= g_nranks || source < 0 || source >= g_nranks) {
        send_bytes(sbuf, sbytes, dest, 0);
        return recv_bytes(rbuf, rbytes, source);
    }
    RtScope rs;   /* 1.12.18: one exchange step, one primitive, before the send stamp */
    rt_event(tl_coll ? RTK_MPI_COLL_STEP : RTK_MPI_P2P, 2);
    const SendStamp st = send_prelude(dest, sbytes);
    const uint32_t ns = chunks_of(sbytes);
    RecvState r = { source, -1, 0, 0, 0, false };
    for (uint32_t i = 0; i < ns || !r.done; i++) {
        if (i < ns) send_chunk_at(dest, 0, (const char*)sbuf, sbytes, i, ns, st);
        if (!r.done) recv_chunk_thread(&r, (char*)rbuf, rbytes);
    }
    if (!r.timed) inject_timing_recv(r.src, g_rank, r.written, r.send_time, r.contend_wait);   /* (1.12.177): done at the first chunk */
    return r.written;
}

int MPI_Send(const void *buf, int count, MPI_Datatype datatype,
             int dest, int tag, MPI_Comm comm) {
    (void)comm;
    RtScope rs;   /* 1.12.18: argument bookkeeping is runtime */
    if (g_nranks <= 1) return MPI_SUCCESS;
    if (dest < 0 || dest >= g_nranks) return MPI_SUCCESS;
    return send_bytes(buf, (size_t)count * dtype_size(datatype), dest, tag);
}

int MPI_Recv(void *buf, int count, MPI_Datatype datatype,
             int source, int tag, MPI_Comm comm, MPI_Status *status) {
    (void)comm; (void)status; (void)tag;
    RtScope rs;   /* 1.12.18: argument bookkeeping is runtime */
    if (g_nranks <= 1) return MPI_SUCCESS;
    if (source < 0 || source >= g_nranks) return MPI_SUCCESS;
    recv_bytes(buf, (size_t)count * dtype_size(datatype), source);
    return MPI_SUCCESS;
}

int MPI_Sendrecv(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                 int dest, int sendtag,
                 void *recvbuf, int recvcount, MPI_Datatype recvtype,
                 int source, int recvtag,
                 MPI_Comm comm, MPI_Status *status) {
    (void)sendtag; (void)recvtag; (void)comm; (void)status;
    RtScope rs;   /* 1.12.18: argument bookkeeping is runtime */
    if (g_nranks <= 1) return MPI_SUCCESS;
    const size_t sb = (dest >= 0 && dest < g_nranks) ? (size_t)sendcount * dtype_size(sendtype) : 0;
    const size_t rb = (source >= 0 && source < g_nranks) ? (size_t)recvcount * dtype_size(recvtype) : 0;
    xchg_bytes(sendbuf, sb, dest, recvbuf, rb, source);
    return MPI_SUCCESS;
}

/* ---- Non-blocking (synchronous semantics -- shm is fast enough) ---- */

int MPI_Isend(const void *buf, int count, MPI_Datatype datatype,
              int dest, int tag, MPI_Comm comm, MPI_Request *request) {
    MPI_Send(buf, count, datatype, dest, tag, comm);
    *request = g_next_request.fetch_add(1);
    return MPI_SUCCESS;
}

int MPI_Irecv(void *buf, int count, MPI_Datatype datatype,
              int source, int tag, MPI_Comm comm, MPI_Request *request) {
    MPI_Recv(buf, count, datatype, source, tag, comm, MPI_STATUS_IGNORE);
    *request = g_next_request.fetch_add(1);
    return MPI_SUCCESS;
}

int MPI_Wait(MPI_Request *request, MPI_Status *status) {
    (void)request; (void)status;
    return MPI_SUCCESS;
}

int MPI_Waitall(int count, MPI_Request requests[], MPI_Status statuses[]) {
    (void)count; (void)requests; (void)statuses;
    return MPI_SUCCESS;
}

/* ---- Collectives ---- */

int MPI_Barrier(MPI_Comm comm) {
    (void)comm;
    if (g_nranks <= 1) {
        zsim_magic_op(ZSIM_MAGIC_OP_MPI_BARRIER);
        return MPI_SUCCESS;
    }

    MPITRACE("barrier_enter");
    /* Lock-free generation barrier: snapshot gen BEFORE arriving; the last
     * arriver resets the count and bumps the generation; everyone else
     * polls the generation. No mutex, no condvar, no kernel waits. */
    if (g_thread_mode) {
        RtScope rs;   /* 1.12.18: the barrier's bookkeeping is runtime */
        /* Cond-parked generation barrier + MAX-ARRIVAL RENDEZVOUS: every rank
         * publishes its arrival sim-time; the last arriver freezes the max as
         * the release time; every rank then ADVANCEs its core clock to that
         * release before the (deterministically priced) BARRIER op. Exit time
         * = f(arrivals) only -- futex wake order cannot touch it. */
        zsim_magic_op(ZSIM_MAGIC_OP_MPI_TIME);
        uint64_t my_arr = tl_mpi_params.sim_now;
        pthread_mutex_lock(&g_shared->bar_mtx);
        int gen = g_shared->barrier_gen;
        if (my_arr > g_shared->bar_max_arr) g_shared->bar_max_arr = my_arr;
        if (++g_shared->barrier_count >= g_nranks) {
            g_shared->barrier_count = 0;
            g_shared->bar_release = g_shared->bar_max_arr;
            g_shared->bar_max_arr = 0;
            g_shared->barrier_gen = gen + 1;
            pthread_cond_broadcast(&g_shared->bar_cv);
            tl_mpi_params.sim_send_time = g_shared->bar_release;
        } else {
            mpi_comm_begin();   /* frozen-clock bracket */
            while (g_shared->barrier_gen == gen) {
                mpi_wait_peer(2, -1);
                pthread_cond_wait(&g_shared->bar_cv, &g_shared->bar_mtx);
            }
            /* (1.12.177): the release is taken in the window: a waiter does nothing priced between the end
             * of its wait and its advance to the release */
            tl_mpi_params.sim_send_time = g_shared->bar_release;
            mpi_comm_end();
        }
        pthread_mutex_unlock(&g_shared->bar_mtx);
        zsim_magic_op(ZSIM_MAGIC_OP_MPI_ADVANCE);
        zsim_magic_op(ZSIM_MAGIC_OP_MPI_BARRIER);
        rs.close();
        /* 1.12.18: the barrier primitive, after the release (every rank exits
         * at the max arrival plus the primitive) */
        rt_event(RTK_MPI_BARRIER, (uint32_t)g_nranks);
        return MPI_SUCCESS;
    }
    int gen = g_shared->barrier_gen;
    int pos = __sync_add_and_fetch(&g_shared->barrier_count, 1);
    MPITRACE("barrier_in gen=%d count=%d/%d", gen, pos, g_nranks);
    if (pos >= g_nranks) {
        g_shared->barrier_count = 0;
        __sync_synchronize();
        __sync_add_and_fetch(&g_shared->barrier_gen, 1);
        MPITRACE("barrier_release gen=%d", g_shared->barrier_gen);
    } else {
        /* Keep the core attached across the spin so the wall-clock poll is not
         * counted (and does not desynchronize per-rank instruction counts). */
        mpi_comm_begin();
        while (g_shared->barrier_gen == gen) {
            usleep(100);
        }
        mpi_comm_end();
    }
    MPITRACE("barrier_exit gen=%d", g_shared->barrier_gen);

    zsim_magic_op(ZSIM_MAGIC_OP_MPI_BARRIER);
    return MPI_SUCCESS;
}

/* ---- 1.12.17 (ruling 3a): MPICH's collective algorithms ----
 * The collectives run the algorithms of MPICH, chosen the way MPICH's
 * intra-communicator "auto" selection chooses them with its default
 * thresholds (Thakur, Rabenseifner and Gropp, "Optimization of Collective
 * Communication Operations in MPICH", IJHPCA 19(1):49-66, 2005; the MPICH 4
 * MPIR_CVAR_* defaults below). They used to route all data through rank 0,
 * one rank at a time. MPI_Reduce and MPI_Allreduce now also apply the
 * reduction operation at every step (they used to move the data only), as
 * MPICH does; MPI_Gatherv stays linear, as in MPICH. Every step is built on
 * send_bytes / recv_bytes / xchg_bytes, so it is priced like any message. */
static const size_t kBcastShortMsg    = 12288;    /* MPIR_CVAR_BCAST_SHORT_MSG_SIZE */
static const size_t kBcastLongMsg     = 524288;   /* MPIR_CVAR_BCAST_LONG_MSG_SIZE */
static const int    kBcastMinProcs    = 8;        /* MPIR_CVAR_BCAST_MIN_PROCS */
static const size_t kAllgatherShort   = 81920;    /* MPIR_CVAR_ALLGATHER_SHORT_MSG_SIZE */
static const size_t kAllgatherLong    = 524288;   /* MPIR_CVAR_ALLGATHER_LONG_MSG_SIZE */
static const size_t kReduceShort      = 2048;     /* MPIR_CVAR_REDUCE_SHORT_MSG_SIZE */
static const size_t kAllreduceShort   = 2048;     /* MPIR_CVAR_ALLREDUCE_SHORT_MSG_SIZE */
static const size_t kAlltoallShort    = 256;      /* MPIR_CVAR_ALLTOALL_SHORT_MSG_SIZE */
static const size_t kAlltoallMedium   = 32768;    /* MPIR_CVAR_ALLTOALL_MEDIUM_MSG_SIZE */
static const int    kAlltoallMinProcs = 8;        /* MPICH uses Bruck's alltoall from 8 processes */
static const void* const kInPlace = (const void*)-1;   /* MPICH's MPI_IN_PLACE */

static bool is_pof2(int n) { return n > 0 && (n & (n - 1)) == 0; }
static int pof2_floor(int n) { int p = 1; while (p * 2 <= n) p *= 2; return p; }

/* ---- reduction operations (MPI_Reduce_local) ---- */
enum RedOp { RO_SUM, RO_PROD, RO_MAX, RO_MIN, RO_LAND, RO_LOR, RO_LXOR, RO_BAND, RO_BOR, RO_BXOR };
enum RedType { RT_I8, RT_U8, RT_I16, RT_U16, RT_I32, RT_U32, RT_I64, RT_U64, RT_F32, RT_F64 };

static RedOp red_op(MPI_Op op) {
    switch ((uint32_t)op) {
        /* include/parallel_abstraction/pimid_mpi.h */
        case 1: return RO_SUM;  case 2: return RO_MAX;  case 3: return RO_MIN;  case 4: return RO_PROD;
        case 5: return RO_LAND; case 6: return RO_LOR;  case 7: return RO_BAND; case 8: return RO_BOR;
        /* MPICH mpi.h */
        case 0x58000001u: return RO_MAX;  case 0x58000002u: return RO_MIN;
        case 0x58000003u: return RO_SUM;  case 0x58000004u: return RO_PROD;
        case 0x58000005u: return RO_LAND; case 0x58000006u: return RO_BAND;
        case 0x58000007u: return RO_LOR;  case 0x58000008u: return RO_BOR;
        case 0x58000009u: return RO_LXOR; case 0x5800000au: return RO_BXOR;
        default: break;
    }
    fprintf(stderr, "[pimid_mpi] FATAL: MPI_Op handle 0x%08x is not supported by the shim's reductions "
            "(MPI_SUM/PROD/MAX/MIN/LAND/LOR/LXOR/BAND/BOR/BXOR of MPICH or pimid_mpi.h).\n", (unsigned)op);
    abort();
}

static RedType red_type(MPI_Datatype dt) {
    switch ((uint32_t)dt) {
        /* include/parallel_abstraction/pimid_mpi.h */
        case 1: return RT_I8;  case 2: return RT_I32; case 3: return RT_F32; case 4: return RT_F64;
        case 5: return RT_I64; case 6: return RT_U32; case 7: return RT_U64; case 8: return RT_U8;
        case 9: return RT_I64;
        /* MPICH mpi.h */
        case 0x4c000101u: case 0x4c000118u: case 0x4c000137u: return RT_I8;
        case 0x4c000102u: case 0x4c00010du: case 0x4c00013bu: case 0x4c00013fu: return RT_U8;
        case 0x4c000203u: case 0x4c000238u: return RT_I16;
        case 0x4c000204u: case 0x4c00023cu: return RT_U16;
        case 0x4c000405u: case 0x4c000439u: return RT_I32;
        case 0x4c000406u: case 0x4c00043du: return RT_U32;
        case 0x4c000807u: case 0x4c000809u: case 0x4c00083au: return RT_I64;
        case 0x4c000808u: case 0x4c000819u: case 0x4c00083eu: return RT_U64;
        case 0x4c00040au: return RT_F32;
        case 0x4c00080bu: return RT_F64;
        default: break;
    }
    fprintf(stderr, "[pimid_mpi] FATAL: MPI datatype handle 0x%08x cannot be reduced by the shim "
            "(integer, float and double builtins only).\n", (unsigned)dt);
    abort();
}

extern "C++" {   /* templates inside the shim's extern "C" block */
template <typename T> static void red_arith(RedOp o, const T* in, T* io, size_t n) {
    switch (o) {
        case RO_SUM:  for (size_t i = 0; i < n; i++) io[i] = in[i] + io[i]; return;
        case RO_PROD: for (size_t i = 0; i < n; i++) io[i] = in[i] * io[i]; return;
        case RO_MAX:  for (size_t i = 0; i < n; i++) if (in[i] > io[i]) io[i] = in[i]; return;
        case RO_MIN:  for (size_t i = 0; i < n; i++) if (in[i] < io[i]) io[i] = in[i]; return;
        case RO_LAND: for (size_t i = 0; i < n; i++) io[i] = (T)((in[i] != 0) && (io[i] != 0)); return;
        case RO_LOR:  for (size_t i = 0; i < n; i++) io[i] = (T)((in[i] != 0) || (io[i] != 0)); return;
        case RO_LXOR: for (size_t i = 0; i < n; i++) io[i] = (T)((in[i] != 0) != (io[i] != 0)); return;
        default: break;
    }
    fprintf(stderr, "[pimid_mpi] FATAL: bitwise MPI_Op on a floating-point datatype\n");
    abort();
}
}

extern "C++" {   /* templates inside the shim's extern "C" block */
template <typename T> static void red_int(RedOp o, const T* in, T* io, size_t n) {
    switch (o) {
        case RO_BAND: for (size_t i = 0; i < n; i++) io[i] = in[i] & io[i]; return;
        case RO_BOR:  for (size_t i = 0; i < n; i++) io[i] = in[i] | io[i]; return;
        case RO_BXOR: for (size_t i = 0; i < n; i++) io[i] = in[i] ^ io[i]; return;
        default: red_arith<T>(o, in, io, n); return;
    }
}
}

/* io = in (op) io, over n elements (MPI_Reduce_local; all supported ops commute). */
static void reduce_local(const void* in, void* io, size_t n, MPI_Datatype dt, MPI_Op op) {
    const RedOp o = red_op(op);
    const RedType ty = red_type(dt);
    rt_event(RTK_MPI_RED_STEP, 1);   /* 1.12.18: the combine step's primitive (default 0: the arithmetic below is simulated) */
    RtPriced pr;                     /* 1.12.18: the reduce arithmetic on user data is kernel work */
    switch (ty) {
        case RT_I8:  red_int<int8_t>(o, (const int8_t*)in, (int8_t*)io, n); return;
        case RT_U8:  red_int<uint8_t>(o, (const uint8_t*)in, (uint8_t*)io, n); return;
        case RT_I16: red_int<int16_t>(o, (const int16_t*)in, (int16_t*)io, n); return;
        case RT_U16: red_int<uint16_t>(o, (const uint16_t*)in, (uint16_t*)io, n); return;
        case RT_I32: red_int<int32_t>(o, (const int32_t*)in, (int32_t*)io, n); return;
        case RT_U32: red_int<uint32_t>(o, (const uint32_t*)in, (uint32_t*)io, n); return;
        case RT_I64: red_int<int64_t>(o, (const int64_t*)in, (int64_t*)io, n); return;
        case RT_U64: red_int<uint64_t>(o, (const uint64_t*)in, (uint64_t*)io, n); return;
        case RT_F32: red_arith<float>(o, (const float*)in, (float*)io, n); return;
        case RT_F64: red_arith<double>(o, (const double*)in, (double*)io, n); return;
    }
}

/* ---- MPI_Bcast ---- */
static void bcast_binomial(char* buf, size_t nbytes, int root) {
    const int P = g_nranks, rel = (g_rank - root + P) % P;
    int mask = 1;
    while (mask < P) {
        if (rel & mask) { recv_bytes(buf, nbytes, (g_rank - mask + P) % P); break; }
        mask <<= 1;
    }
    mask >>= 1;
    while (mask > 0) {
        if (rel + mask < P) send_bytes(buf, nbytes, (g_rank + mask) % P, 0);
        mask >>= 1;
    }
}

/* Binomial scatter of P pieces of ss bytes (the last ones short or empty).
 * Returns how many bytes this rank holds from the start of its own piece. */
static size_t bcast_scatter(char* buf, size_t nbytes, size_t ss, int root) {
    const int P = g_nranks, rel = (g_rank - root + P) % P;
    size_t curr = (rel == 0) ? nbytes : 0;
    int mask = 1;
    while (mask < P) {
        if (rel & mask) {
            const size_t off = (size_t)rel * ss;
            curr = (off < nbytes) ? recv_bytes(buf + off, nbytes - off, (g_rank - mask + P) % P) : 0;
            break;
        }
        mask <<= 1;
    }
    mask >>= 1;
    while (mask > 0) {
        if (rel + mask < P) {
            const size_t keep = ss * (size_t)mask;
            if (curr > keep) {
                send_bytes(buf + (size_t)(rel + mask) * ss, curr - keep, (g_rank + mask) % P, 0);
                curr = keep;
            }
        }
        mask >>= 1;
    }
    return curr;
}

static void bcast_scatter_rd_allgather(char* buf, size_t nbytes, int root) {   /* P a power of two */
    const int P = g_nranks, rel = (g_rank - root + P) % P;
    const size_t ss = (nbytes + P - 1) / P;
    size_t curr = bcast_scatter(buf, nbytes, ss, root);
    for (int mask = 1, i = 0; mask < P; mask <<= 1, i++) {
        const int rdst = rel ^ mask, dst = (rdst + root) % P;
        const size_t soff = (size_t)((rel >> i) << i) * ss;
        const size_t roff = (size_t)((rdst >> i) << i) * ss;
        curr += xchg_bytes(buf + (soff < nbytes ? soff : 0), curr, dst,
                           buf + (roff < nbytes ? roff : 0), roff < nbytes ? nbytes - roff : 0, dst);
    }
}

static void bcast_scatter_ring_allgather(char* buf, size_t nbytes, int root) {
    const int P = g_nranks;
    const size_t ss = (nbytes + P - 1) / P;
    bcast_scatter(buf, nbytes, ss, root);
    const int left = (P + g_rank - 1) % P, right = (g_rank + 1) % P;
    int j = g_rank, jnext = left;
    for (int i = 1; i < P; i++) {
        const size_t rj = (size_t)((j - root + P) % P) * ss, rjn = (size_t)((jnext - root + P) % P) * ss;
        const size_t rcount = rj < nbytes ? (nbytes - rj < ss ? nbytes - rj : ss) : 0;
        const size_t lcount = rjn < nbytes ? (nbytes - rjn < ss ? nbytes - rjn : ss) : 0;
        xchg_bytes(buf + (rcount ? rj : 0), rcount, right, buf + (lcount ? rjn : 0), lcount, left);
        j = jnext;
        jnext = (P + jnext - 1) % P;
    }
}

int MPI_Bcast(void *buffer, int count, MPI_Datatype datatype,
              int root, MPI_Comm comm) {
    (void)comm;
    RtScope rs; CollScope cs;   /* 1.12.18: the collective's bookkeeping is runtime; its messages are collective steps */
    if (g_nranks <= 1) return MPI_SUCCESS;
    const size_t nbytes = (size_t)count * dtype_size(datatype);
    char* buf = (char*)buffer;
    if (nbytes < kBcastShortMsg || g_nranks < kBcastMinProcs) bcast_binomial(buf, nbytes, root);
    else if (nbytes < kBcastLongMsg && is_pof2(g_nranks)) bcast_scatter_rd_allgather(buf, nbytes, root);
    else bcast_scatter_ring_allgather(buf, nbytes, root);
    return MPI_SUCCESS;
}

/* ---- MPI_Allgather / MPI_Allgatherv (byte counts and displacements) ---- */
static void allgatherv_rd(char* rbuf, const std::vector<size_t>& cnt, const std::vector<size_t>& dsp) {   /* P a power of two */
    const int P = g_nranks, rank = g_rank;
    /* MPICH gathers into a contiguous buffer laid out by rank; when the
     * displacements already are that layout, the receive buffer is used. */
    std::vector<size_t> pos(P + 1, 0);
    for (int j = 0; j < P; j++) pos[j + 1] = pos[j] + cnt[j];
    bool contiguous = true;
    for (int j = 0; j < P; j++) if (dsp[j] != dsp[0] + pos[j]) contiguous = false;
    std::vector<char> tmp;
    char* w = rbuf + dsp[0];
    if (!contiguous) { w = scratch_or_vec(0, pos[P] ? pos[P] : 1, tmp); priced_memcpy(w + pos[rank], rbuf + dsp[rank], cnt[rank]); }
    size_t curr = cnt[rank];
    for (int mask = 1, i = 0; mask < P; mask <<= 1, i++) {
        const int dst = rank ^ mask, dtr = (dst >> i) << i, mtr = (rank >> i) << i;
        curr += xchg_bytes(w + pos[mtr], curr, dst, w + pos[dtr], pos[P] - pos[dtr], dst);
    }
    if (!contiguous)
        for (int j = 0; j < P; j++) if (j != rank) priced_memcpy(rbuf + dsp[j], w + pos[j], cnt[j]);
}

static void allgatherv_bruck(char* rbuf, const std::vector<size_t>& cnt, const std::vector<size_t>& dsp) {
    const int P = g_nranks, rank = g_rank;
    std::vector<size_t> rpos(P + 1, 0);   /* block j of the work buffer is rank (rank + j) % P's */
    for (int j = 0; j < P; j++) rpos[j + 1] = rpos[j] + cnt[(rank + j) % P];
    char w[kAllgatherShort];              /* total < kAllgatherShort: on this rank's own stack */
    priced_memcpy(w, rbuf + dsp[rank], cnt[rank]);
    int pof2 = 1;
    while (pof2 <= P / 2) {
        xchg_bytes(w, rpos[pof2], (rank - pof2 + P) % P, w + rpos[pof2], rpos[2 * pof2] - rpos[pof2], (rank + pof2) % P);
        pof2 *= 2;
    }
    const int rem = P - pof2;
    if (rem)
        xchg_bytes(w, rpos[rem], (rank - pof2 + P) % P, w + rpos[pof2], rpos[pof2 + rem] - rpos[pof2], (rank + pof2) % P);
    for (int j = 1; j < P; j++) priced_memcpy(rbuf + dsp[(rank + j) % P], w + rpos[j], cnt[(rank + j) % P]);
}

static void allgatherv_ring(char* rbuf, const std::vector<size_t>& cnt, const std::vector<size_t>& dsp) {
    const int P = g_nranks;
    const int left = (P + g_rank - 1) % P, right = (g_rank + 1) % P;
    int j = g_rank, jnext = left;
    for (int i = 1; i < P; i++) {
        xchg_bytes(rbuf + dsp[j], cnt[j], right, rbuf + dsp[jnext], cnt[jnext], left);
        j = jnext;
        jnext = (P + jnext - 1) % P;
    }
}

static void allgatherv_bytes(const void* sendbuf, size_t sbytes, char* rbuf,
                             const std::vector<size_t>& cnt, const std::vector<size_t>& dsp) {
    if (sendbuf != kInPlace && sendbuf && rbuf)
        priced_memcpy(rbuf + dsp[g_rank], sendbuf, sbytes < cnt[g_rank] ? sbytes : cnt[g_rank]);
    if (g_nranks <= 1) return;
    size_t total = 0;
    for (int j = 0; j < g_nranks; j++) total += cnt[j];
    if (total < kAllgatherLong && is_pof2(g_nranks)) allgatherv_rd(rbuf, cnt, dsp);
    else if (total < kAllgatherShort) allgatherv_bruck(rbuf, cnt, dsp);
    else allgatherv_ring(rbuf, cnt, dsp);
}

int MPI_Allgather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                  void *recvbuf, int recvcount, MPI_Datatype recvtype,
                  MPI_Comm comm) {
    (void)comm;
    RtScope rs; CollScope cs;   /* 1.12.18: the collective's bookkeeping is runtime; its messages are collective steps */
    const size_t blk = (size_t)recvcount * dtype_size(recvtype);
    const size_t sb = (sendbuf == kInPlace) ? 0 : (size_t)sendcount * dtype_size(sendtype);
    std::vector<size_t> cnt(g_nranks, blk), dsp(g_nranks);
    for (int j = 0; j < g_nranks; j++) dsp[j] = (size_t)j * blk;
    allgatherv_bytes(sendbuf, sb, (char*)recvbuf, cnt, dsp);
    return MPI_SUCCESS;
}

int MPI_Allgatherv(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                   void *recvbuf, const int *recvcounts, const int *displs,
                   MPI_Datatype recvtype, MPI_Comm comm) {
    (void)comm;
    RtScope rs; CollScope cs;   /* 1.12.18: the collective's bookkeeping is runtime; its messages are collective steps */
    const size_t ext = dtype_size(recvtype);
    const size_t sb = (sendbuf == kInPlace) ? 0 : (size_t)sendcount * dtype_size(sendtype);
    std::vector<size_t> cnt(g_nranks), dsp(g_nranks);
    for (int j = 0; j < g_nranks; j++) { cnt[j] = (size_t)recvcounts[j] * ext; dsp[j] = (size_t)displs[j] * ext; }
    allgatherv_bytes(sendbuf, sb, (char*)recvbuf, cnt, dsp);
    return MPI_SUCCESS;
}

/* ---- MPI_Reduce / MPI_Allreduce ---- */
/* Piece layout of the reduce-scatter (Rabenseifner) algorithms, in elements. */
static void rs_pieces(int count, int pof2, std::vector<int>& cnts, std::vector<int>& disps) {
    cnts.assign(pof2, count / pof2);
    for (int i = 0; i < count % pof2; i++) cnts[i]++;
    disps.assign(pof2, 0);
    for (int i = 1; i < pof2; i++) disps[i] = disps[i - 1] + cnts[i - 1];
}
static size_t rs_sum(const std::vector<int>& cnts, int a, int b) { size_t s = 0; for (int i = a; i < b; i++) s += (size_t)cnts[i]; return s; }

/* The piece range [send_idx, last_idx) survivor newrank holds after the recursive halving below. */
static void halving_final_idx(int newrank, int pof2, int* send_idx_out, int* last_idx_out) {
    int send_idx = 0, recv_idx = 0, last_idx = pof2;
    for (int mask = 1; mask < pof2; ) {
        if (newrank < (newrank ^ mask)) send_idx = recv_idx + pof2 / (mask * 2);
        else recv_idx = send_idx + pof2 / (mask * 2);
        send_idx = recv_idx;
        mask <<= 1;
        if (mask < pof2) last_idx = recv_idx + pof2 / mask;
    }
    *send_idx_out = send_idx;
    *last_idx_out = last_idx;
}

/* Recursive halving among the pof2 survivors (real rank of survivor k: realOf).
 * On return [send_idx, last_idx) of acc holds this rank's reduced pieces. */
extern "C++" {   /* templates inside the shim's extern "C" block */
template <typename RealOf>
static void reduce_scatter_halving(char* acc, char* tmp, int newrank, int pof2, const std::vector<int>& cnts,
                                   const std::vector<int>& disps, size_t ext, MPI_Datatype dt, MPI_Op op,
                                   RealOf realOf, int* send_idx_out, int* last_idx_out) {
    int send_idx = 0, recv_idx = 0, last_idx = pof2;
    for (int mask = 1; mask < pof2; ) {
        const int newdst = newrank ^ mask, dst = realOf(newdst);
        size_t send_cnt, recv_cnt;
        if (newrank < newdst) {
            send_idx = recv_idx + pof2 / (mask * 2);
            send_cnt = rs_sum(cnts, send_idx, last_idx);
            recv_cnt = rs_sum(cnts, recv_idx, send_idx);
        } else {
            recv_idx = send_idx + pof2 / (mask * 2);
            send_cnt = rs_sum(cnts, send_idx, recv_idx);
            recv_cnt = rs_sum(cnts, recv_idx, last_idx);
        }
        xchg_bytes(acc + (size_t)disps[send_idx] * ext, send_cnt * ext, dst,
                   tmp + (size_t)disps[recv_idx] * ext, recv_cnt * ext, dst);
        reduce_local(tmp + (size_t)disps[recv_idx] * ext, acc + (size_t)disps[recv_idx] * ext, recv_cnt, dt, op);
        send_idx = recv_idx;
        mask <<= 1;
        if (mask < pof2) last_idx = recv_idx + pof2 / mask;
    }
    *send_idx_out = send_idx;
    *last_idx_out = last_idx;
}
}

static void allreduce_bytes(char* acc, int count, MPI_Datatype dt, size_t ext, MPI_Op op) {
    const int P = g_nranks, rank = g_rank;
    const size_t n = (size_t)count * ext;
    const int pof2 = pof2_floor(P), rem = P - pof2;
    std::vector<char> tmpv;   /* 1.12.18: per-rank scratch under the PIM runtime */
    char* tmp = scratch_or_vec(0, n ? n : 1, tmpv);
    int newrank;
    if (rank < 2 * rem) {   /* the first 2*rem ranks fold pairwise; the odd one survives */
        if (rank % 2 == 0) { send_bytes(acc, n, rank + 1, 0); newrank = -1; }
        else { recv_bytes(tmp, n, rank - 1); reduce_local(tmp, acc, count, dt, op); newrank = rank / 2; }
    } else newrank = rank - rem;
    auto realOf = [rem](int nr) { return (nr < rem) ? nr * 2 + 1 : nr + rem; };
    if (newrank != -1) {
        if (n <= kAllreduceShort || count < pof2) {   /* recursive doubling */
            for (int mask = 1; mask < pof2; mask <<= 1) {
                const int dst = realOf(newrank ^ mask);
                xchg_bytes(acc, n, dst, tmp, n, dst);
                reduce_local(tmp, acc, count, dt, op);
            }
        } else {                                      /* Rabenseifner: reduce-scatter + allgather */
            std::vector<int> cnts, disps;
            rs_pieces(count, pof2, cnts, disps);
            int send_idx, last_idx;
            reduce_scatter_halving(acc, tmp, newrank, pof2, cnts, disps, ext, dt, op, realOf, &send_idx, &last_idx);
            int recv_idx = 0;
            for (int mask = pof2 >> 1; mask > 0; mask >>= 1) {
                const int newdst = newrank ^ mask, dst = realOf(newdst);
                size_t send_cnt, recv_cnt;
                if (newrank < newdst) {
                    if (mask != pof2 / 2) last_idx = last_idx + pof2 / (mask * 2);
                    recv_idx = send_idx + pof2 / (mask * 2);
                    send_cnt = rs_sum(cnts, send_idx, recv_idx);
                    recv_cnt = rs_sum(cnts, recv_idx, last_idx);
                } else {
                    recv_idx = send_idx - pof2 / (mask * 2);
                    send_cnt = rs_sum(cnts, send_idx, last_idx);
                    recv_cnt = rs_sum(cnts, recv_idx, send_idx);
                }
                xchg_bytes(acc + (size_t)disps[send_idx] * ext, send_cnt * ext, dst,
                           acc + (size_t)disps[recv_idx] * ext, recv_cnt * ext, dst);
                if (newrank > newdst) send_idx = recv_idx;
            }
        }
    }
    if (rank < 2 * rem) {   /* hand the result back to the folded ranks */
        if (rank % 2) send_bytes(acc, n, rank - 1, 0);
        else recv_bytes(acc, n, rank + 1);
    }
}

int MPI_Allreduce(const void *sendbuf, void *recvbuf, int count,
                  MPI_Datatype datatype, MPI_Op op, MPI_Comm comm) {
    (void)comm;
    RtScope rs; CollScope cs;   /* 1.12.18: the collective's bookkeeping is runtime; its messages are collective steps */
    const size_t ext = dtype_size(datatype);
    if (sendbuf != kInPlace && sendbuf && recvbuf) priced_memcpy(recvbuf, sendbuf, (size_t)count * ext);
    if (g_nranks <= 1) return MPI_SUCCESS;
    allreduce_bytes((char*)recvbuf, count, datatype, ext, op);
    return MPI_SUCCESS;
}

static void reduce_binomial(char* acc, int count, MPI_Datatype dt, size_t ext, MPI_Op op, int root) {
    const int P = g_nranks, rel = (g_rank - root + P) % P;
    const size_t n = (size_t)count * ext;
    std::vector<char> tmpv;   /* 1.12.18: per-rank scratch under the PIM runtime */
    char* tmp = scratch_or_vec(0, n ? n : 1, tmpv);
    for (int mask = 1; mask < P; mask <<= 1) {
        if ((mask & rel) == 0) {
            const int src = rel | mask;
            if (src < P) {
                recv_bytes(tmp, n, (src + root) % P);
                reduce_local(tmp, acc, count, dt, op);
            }
        } else {
            send_bytes(acc, n, ((rel & ~mask) + root) % P, 0);
            break;
        }
    }
}

static void reduce_scatter_gather(char* acc, int count, MPI_Datatype dt, size_t ext, MPI_Op op, int root) {
    const int P = g_nranks, rank = g_rank;
    const size_t n = (size_t)count * ext;
    const int pof2 = pof2_floor(P), rem = P - pof2;
    std::vector<char> tmpv;   /* 1.12.18: per-rank scratch under the PIM runtime */
    char* tmp = scratch_or_vec(0, n ? n : 1, tmpv);
    int newrank;
    if (rank < 2 * rem) {   /* the first 2*rem ranks fold pairwise; the even one survives */
        if (rank % 2 != 0) { send_bytes(acc, n, rank - 1, 0); newrank = -1; }
        else { recv_bytes(tmp, n, rank + 1); reduce_local(tmp, acc, count, dt, op); newrank = rank / 2; }
    } else newrank = rank - rem;
    auto realOf = [rem](int nr) { return (nr < rem) ? nr * 2 : nr + rem; };
    std::vector<int> cnts, disps;
    rs_pieces(count, pof2, cnts, disps);
    int send_idx = 0, last_idx = pof2;
    if (newrank != -1)
        reduce_scatter_halving(acc, tmp, newrank, pof2, cnts, disps, ext, dt, op, realOf, &send_idx, &last_idx);
    /* The root's survivor is newroot. A folded (odd) root takes over its
     * partner's role: the partner hands it the pieces it reduced and leaves. */
    const bool rootFolded = (root < 2 * rem) && (root % 2 != 0);
    const int newroot = (root < 2 * rem) ? root / 2 : root - rem;
    if (rootFolded) {
        int ps = 0, pl = pof2;
        halving_final_idx(newroot, pof2, &ps, &pl);
        const size_t off = (size_t)disps[ps] * ext, nb = rs_sum(cnts, ps, pl) * ext;
        if (rank == root) {
            recv_bytes(acc + off, nb, root - 1);
            newrank = newroot; send_idx = ps; last_idx = pl;
        } else if (rank == root - 1) {
            send_bytes(acc + off, nb, root, 0);
            newrank = -1;
        }
    }
    if (newrank == -1) return;
    int j = 0, mask = 1;
    while (mask < pof2) { mask <<= 1; j++; }
    mask >>= 1; j--;
    int recv_idx = 0;
    while (mask > 0) {
        const int newdst = newrank ^ mask;
        int dst = realOf(newdst);
        if (rootFolded && newdst == newroot) dst = root;
        const int newdst_tree_root = (newdst >> j) << j, newroot_tree_root = (newroot >> j) << j;
        size_t send_cnt, recv_cnt;
        if (newrank < newdst) {
            if (mask != pof2 / 2) last_idx = last_idx + pof2 / (mask * 2);
            recv_idx = send_idx + pof2 / (mask * 2);
            send_cnt = rs_sum(cnts, send_idx, recv_idx);
            recv_cnt = rs_sum(cnts, recv_idx, last_idx);
        } else {
            recv_idx = send_idx - pof2 / (mask * 2);
            send_cnt = rs_sum(cnts, send_idx, last_idx);
            recv_cnt = rs_sum(cnts, recv_idx, send_idx);
        }
        if (newdst_tree_root == newroot_tree_root) {
            send_bytes(acc + (size_t)disps[send_idx] * ext, send_cnt * ext, dst, 0);
            break;
        }
        recv_bytes(acc + (size_t)disps[recv_idx] * ext, recv_cnt * ext, dst);
        if (newrank > newdst) send_idx = recv_idx;
        mask >>= 1; j--;
    }
}

int MPI_Reduce(const void *sendbuf, void *recvbuf, int count,
               MPI_Datatype datatype, MPI_Op op, int root, MPI_Comm comm) {
    (void)comm;
    RtScope rs; CollScope cs;   /* 1.12.18: the collective's bookkeeping is runtime; its messages are collective steps */
    const size_t ext = dtype_size(datatype), n = (size_t)count * ext;
    const bool isRoot = (g_rank == root);
    if (g_nranks <= 1) {
        if (sendbuf != kInPlace && sendbuf && recvbuf) priced_memcpy(recvbuf, sendbuf, n);
        return MPI_SUCCESS;
    }
    /* The running result: the root's recvbuf, a scratch buffer elsewhere. */
    std::vector<char> accv;
    char* acc;
    if (isRoot && recvbuf) acc = (char*)recvbuf;
    else acc = scratch_or_vec(1, n ? n : 1, accv);   /* 1.12.18: per-rank scratch under the PIM runtime */
    if (!(isRoot && sendbuf == kInPlace) && sendbuf) priced_memcpy(acc, sendbuf, n);
    if (n > kReduceShort && count >= pof2_floor(g_nranks)) reduce_scatter_gather(acc, count, datatype, ext, op, root);
    else reduce_binomial(acc, count, datatype, ext, op, root);
    return MPI_SUCCESS;
}

/* ---- MPI_Gather / MPI_Scatter (binomial) and MPI_Gatherv (linear) ---- */
int MPI_Gather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
               void *recvbuf, int recvcount, MPI_Datatype recvtype,
               int root, MPI_Comm comm) {
    (void)comm;
    RtScope rs; CollScope cs;   /* 1.12.18: the collective's bookkeeping is runtime; its messages are collective steps */
    const int P = g_nranks, rel = (g_rank - root + P) % P;
    const size_t blk = (g_rank == root) ? (size_t)recvcount * dtype_size(recvtype)
                                        : (size_t)sendcount * dtype_size(sendtype);
    char* rbuf = (char*)recvbuf;
    if (P <= 1) {
        if (sendbuf != kInPlace && sendbuf && rbuf) priced_memcpy(rbuf, sendbuf, blk);
        return MPI_SUCCESS;
    }
    /* This rank's subtree, in relative rank order, starts with its own block. */
    int sub = 1;
    while (sub < P && !(rel & sub)) sub <<= 1;
    if (rel == 0) sub = P;
    else if (sub > P - rel) sub = P - rel;
    std::vector<char> tmp;
    char* w;
    if (rel == 0 && root == 0) w = rbuf;
    else if (sub == 1) w = (char*)sendbuf;   /* a leaf sends its own block */
    else w = scratch_or_vec(0, (size_t)sub * blk, tmp);   /* 1.12.18 */
    const char* own = (sendbuf == kInPlace) ? rbuf + (size_t)root * blk : (const char*)sendbuf;
    if (w != own && w != (char*)sendbuf && own) priced_memcpy(w, own, blk);
    size_t curr = 1;
    for (int mask = 1; mask < P; mask <<= 1) {
        if ((rel & mask) == 0) {
            const int src = rel | mask;
            if (src < P) {
                const size_t nb = (size_t)((mask < P - src) ? mask : P - src);
                recv_bytes(w + (size_t)mask * blk, nb * blk, (src + root) % P);
                curr += nb;
            }
        } else {
            send_bytes(w, curr * blk, ((rel & ~mask) + root) % P, 0);
            break;
        }
    }
    if (rel == 0 && root != 0)
        for (int j = 0; j < P; j++) priced_memcpy(rbuf + (size_t)((j + root) % P) * blk, w + (size_t)j * blk, blk);
    return MPI_SUCCESS;
}

int MPI_Scatter(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                void *recvbuf, int recvcount, MPI_Datatype recvtype,
                int root, MPI_Comm comm) {
    (void)comm;
    RtScope rs; CollScope cs;   /* 1.12.18: the collective's bookkeeping is runtime; its messages are collective steps */
    const int P = g_nranks, rel = (g_rank - root + P) % P;
    const size_t blk = (g_rank == root) ? (size_t)sendcount * dtype_size(sendtype)
                                        : (size_t)recvcount * dtype_size(recvtype);
    const char* sbuf = (const char*)sendbuf;
    if (P <= 1) {
        if (recvbuf != kInPlace && recvbuf && sbuf) priced_memcpy(recvbuf, sbuf, blk);
        return MPI_SUCCESS;
    }
    int sub = 1;
    while (sub < P && !(rel & sub)) sub <<= 1;
    if (rel == 0) sub = P;
    else if (sub > P - rel) sub = P - rel;
    std::vector<char> tmp;
    const char* w;
    size_t curr;
    if (rel == 0) {
        if (root == 0) w = sbuf;
        else {
            char* const t = scratch_or_vec(0, (size_t)P * blk, tmp);   /* 1.12.18 */
            for (int j = 0; j < P; j++) priced_memcpy(t + (size_t)j * blk, sbuf + (size_t)((j + root) % P) * blk, blk);
            w = t;
        }
        curr = (size_t)P;
    } else {
        int mask = 1;
        while (!(rel & mask)) mask <<= 1;
        const int parent = (g_rank - mask + P) % P;
        if (sub == 1) { recv_bytes(recvbuf, blk, parent); return MPI_SUCCESS; }   /* a leaf */
        char* const t = scratch_or_vec(0, (size_t)sub * blk, tmp);   /* 1.12.18 */
        recv_bytes(t, (size_t)sub * blk, parent);
        w = t;
        curr = (size_t)sub;
    }
    int mask = 1;
    while (mask < P && !(rel & mask)) mask <<= 1;
    for (mask >>= 1; mask > 0; mask >>= 1) {
        if (rel + mask < P && curr > (size_t)mask) {
            send_bytes(w + (size_t)mask * blk, (curr - (size_t)mask) * blk, (g_rank + mask) % P, 0);
            curr = (size_t)mask;
        }
    }
    if (recvbuf != kInPlace && recvbuf) priced_memcpy(recvbuf, w, blk);
    return MPI_SUCCESS;
}

int MPI_Gatherv(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                void *recvbuf, const int *recvcounts, const int *displs,
                MPI_Datatype recvtype, int root, MPI_Comm comm) {
    (void)comm;
    RtScope rs; CollScope cs;   /* 1.12.18: the collective's bookkeeping is runtime; its messages are collective steps */
    /* Linear, as MPICH's MPIR_Gatherv_allcomm_linear. */
    size_t rtsz = dtype_size(recvtype);
    if (g_rank != root) {
        MPI_Send(sendbuf, sendcount, sendtype, root, 0, comm);
    } else {
        char* rbuf = (char*)recvbuf;
        for (int i = 0; i < g_nranks; i++) {
            if (i == root) {
                if (sendbuf && sendbuf != kInPlace && rbuf)
                    priced_memcpy(rbuf + (size_t)displs[i] * rtsz, sendbuf,
                           (size_t)sendcount * dtype_size(sendtype));
            } else {
                MPI_Recv(rbuf + (size_t)displs[i] * rtsz, recvcounts[i], recvtype,
                         i, 0, comm, MPI_STATUS_IGNORE);
            }
        }
    }
    return MPI_SUCCESS;
}

/* ---- MPI_Alltoall: Bruck (short), scattered (medium), pairwise (long) ---- */
int MPI_Alltoall(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                 void *recvbuf, int recvcount, MPI_Datatype recvtype,
                 MPI_Comm comm) {
    (void)comm;
    RtScope rs; CollScope cs;   /* 1.12.18: the collective's bookkeeping is runtime; its messages are collective steps */
    const int P = g_nranks, rank = g_rank;
    const size_t sblk = (size_t)sendcount * dtype_size(sendtype);
    const size_t rblk = (size_t)recvcount * dtype_size(recvtype);
    const char* sbuf = (const char*)sendbuf;
    char* rbuf = (char*)recvbuf;
    if (sbuf && rbuf) priced_memcpy(rbuf + (size_t)rank * rblk, sbuf + (size_t)rank * sblk, sblk);
    if (P <= 1) return MPI_SUCCESS;
    if (sblk <= kAlltoallShort && P >= kAlltoallMinProcs) {
        /* Bruck: rotate up by rank, log2(P) exchanges of the blocks whose index
         * has bit k set, then block j came from rank (rank - j). */
        std::vector<char> wv, pkv, rkv;   /* 1.12.18: per-rank scratch under the PIM runtime */
        char* const w = scratch_or_vec(0, (size_t)P * sblk, wv);
        char* const pk = scratch_or_vec(1, (size_t)P * sblk, pkv);
        char* const rk = scratch_or_vec(2, (size_t)P * sblk, rkv);
        for (int j = 0; j < P; j++) priced_memcpy(w + (size_t)j * sblk, sbuf + (size_t)((rank + j) % P) * sblk, sblk);
        for (int pof2 = 1; pof2 < P; pof2 <<= 1) {
            size_t nb = 0;
            for (int j = 0; j < P; j++)
                if (j & pof2) { priced_memcpy(pk + nb * sblk, w + (size_t)j * sblk, sblk); nb++; }
            xchg_bytes(pk, nb * sblk, (rank + pof2) % P, rk, nb * sblk, (rank - pof2 + P) % P);
            nb = 0;
            for (int j = 0; j < P; j++)
                if (j & pof2) { priced_memcpy(w + (size_t)j * sblk, rk + nb * sblk, sblk); nb++; }
        }
        for (int j = 1; j < P; j++) priced_memcpy(rbuf + (size_t)((rank - j + P) % P) * rblk, w + (size_t)j * sblk, rblk);
    } else if (sblk <= kAlltoallMedium) {
        /* Scattered: step i sends to rank - i and receives from rank + i. */
        for (int i = 1; i < P; i++) {
            const int dst = (rank - i + P) % P, src = (rank + i) % P;
            xchg_bytes(sbuf + (size_t)dst * sblk, sblk, dst, rbuf + (size_t)src * rblk, rblk, src);
        }
    } else {
        /* Pairwise exchange (XOR partners when P is a power of two). */
        for (int i = 1; i < P; i++) {
            int src, dst;
            if (is_pof2(P)) src = dst = rank ^ i;
            else { src = (rank - i + P) % P; dst = (rank + i) % P; }
            xchg_bytes(sbuf + (size_t)dst * sblk, sblk, dst, rbuf + (size_t)src * rblk, rblk, src);
        }
    }
    return MPI_SUCCESS;
}

/* ---- Utility ---- */

int MPI_Type_size(MPI_Datatype datatype, int *size) {
    *size = (int)dtype_size(datatype);
    return MPI_SUCCESS;
}

double MPI_Wtime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

}  /* extern "C" */

/* ==== 1.6 thread mode: rank = guest thread, ONE process ====================
 *
 * The app binary is UNTOUCHED standard MPI. This library interposes
 * __libc_start_main (it is preloaded ahead of libc by the PIMID launcher):
 * when the launcher requests >1 rank (PIMID_MPI_RANKS, internal channel),
 * the app's real main() is captured and a harness
 * main runs instead. The harness builds the in-process transport region and
 * spawns one thread per rank, each executing the app's own main(argc, argv)
 * with its rank identity in TLS. To the zsim plugin this is an OMP-shaped
 * process: N guest threads -> N cores, one Garnet, phase-barrier ordering --
 * venue-independent by construction (no shm, no cut, no replay windows).
 */

#define PIMID_MPI_MAX_RANKS 1024

static int (*g_app_main)(int, char**, char**) = nullptr;

struct PimidRankThread {
    int    rank;
    int    argc;
    char** argv;
    char** envp;
    int    ret;
};

static void* pimid_rank_trampoline(void* arg) {
    PimidRankThread* rt = (PimidRankThread*)arg;
    g_rank = rt->rank;   /* TLS: this thread IS rank r from before main() */
    rt->ret = g_app_main(rt->argc, rt->argv, rt->envp);
    return nullptr;
}

static int pimid_thread_main(int argc, char** argv, char** envp) {
    const char* n = getenv("PIMID_MPI_RANKS");
    int nranks = n ? atoi(n) : 1;
    if (nranks < 1) nranks = 1;
    if (nranks > PIMID_MPI_MAX_RANKS) nranks = PIMID_MPI_MAX_RANKS;

    g_thread_mode = true;
    g_nranks = nranks;
    if (!create_heap_region(nranks)) return 1;
    {   /* 1.12.18: PIM runtime primitives (simulation.pim_runtime.enabled, default true) */
        const char* e = getenv("PIMID_PIM_RUNTIME");
        g_rt_env = !(e && strstr(e, "enabled=0"));
        if (g_rt_env) {   /* the buffers themselves are allocated by each rank in MPI_Init */
            g_scr = (RankScratch*)calloc((size_t)nranks, sizeof(RankScratch));
            if (!g_scr) { fprintf(stderr, "[pimid_mpi] scratch table alloc failed\n"); return 1; }
        }
    }

    fprintf(stderr, "[pimid_mpi] thread mode: %d ranks as threads of one "
            "process (in-process transport, no shm)\n", nranks);

    /* Rank 0 runs ON this (the process main) thread -- the OMP master-is-
     * worker-0 shape. Threads == simulated cores exactly: a separate harness
     * thread would occupy a core slot blocked in pthread_join (the scheduler
     * rightly refuses to fake-leave it) and starve one rank forever, which is
     * precisely the hang the first smoke hit. */
    static PimidRankThread rt[PIMID_MPI_MAX_RANKS];
    static pthread_t       th[PIMID_MPI_MAX_RANKS];
    for (int r = 1; r < nranks; r++) {
        rt[r].rank = r; rt[r].argc = argc; rt[r].argv = argv;
        rt[r].envp = envp; rt[r].ret = 0;
        int rc = pthread_create(&th[r], nullptr, pimid_rank_trampoline, &rt[r]);
        if (rc != 0) {
            fprintf(stderr, "[pimid_mpi] pthread_create(rank %d) failed: %s\n",
                    r, strerror(rc));
            return 1;
        }
    }
    g_rank = 0;
    int worst = g_app_main(argc, argv, envp);
    for (int r = 1; r < nranks; r++) {
        pthread_join(th[r], nullptr);
        if (rt[r].ret > worst) worst = rt[r].ret;
    }
    free(g_shared);
    g_shared = nullptr;
    return worst;
}

extern "C" int __libc_start_main(int (*main_fn)(int, char**, char**), int argc,
                                 char** argv,
                                 int (*init_fn)(int, char**, char**),
                                 void (*fini_fn)(void),
                                 void (*rtld_fini)(void), void* stack_end) {
    using real_start_t = int (*)(int (*)(int, char**, char**), int, char**,
                                 int (*)(int, char**, char**), void (*)(void),
                                 void (*)(void), void*);
    real_start_t real = (real_start_t)dlsym(RTLD_NEXT, "__libc_start_main");
    if (!real) {
        fprintf(stderr, "[pimid_mpi] cannot resolve real __libc_start_main\n");
        _exit(127);
    }
    const char* ranks = getenv("PIMID_MPI_RANKS");
    /* Thread-based rank emulation is THE MPI execution model (1.8.3): it
     * engages whenever the launcher requests more than one rank AND this is
     * not a per-rank trace-gen process (which carries PIMID_MPI_RANK).
     * Everything else -- OMP workloads, solo runs, trace-gen ranks -- boots
     * the app untouched. */
    if (!getenv("PIMID_MPI_RANK") && ranks && atoi(ranks) > 1) {
        g_app_main = main_fn;
        return real(pimid_thread_main, argc, argv, init_fn, fini_fn,
                    rtld_fini, stack_end);
    }
    return real(main_fn, argc, argv, init_fn, fini_fn, rtld_fini, stack_end);
}

/* PIMID 1.11.90: component version stamp. The loader in src/main.cpp reads
 * the "@(#)PIMID_COMPONENT" string from this file and refuses a component
 * whose version differs from the binary's (see locatePimidComponent). */
#ifndef PIMID_VERSION
#error "PIMID_VERSION must be defined for the component version stamp"
#endif
extern "C" __attribute__((visibility("default"), used))
const char pimid_mpi_component_stamp[] = "@(#)PIMID_COMPONENT libpimid_mpi.so " PIMID_VERSION;
extern "C" __attribute__((visibility("default")))
const char* pimid_mpi_component_version(void) { return PIMID_VERSION; }
