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

struct MessageSlot {
    int       src;
    int       tag;
    uint32_t  size;       /* bytes used in payload */
    uint32_t  chunk_idx;  /* 0..n_chunks-1 */
    uint32_t  n_chunks;
    uint32_t  msg_id;
    uint64_t  sim_send_time;    /* sender's simulated send-time (cycles) */
    uint64_t  sim_contend_wait; /* cross-rank contention wait (cycles, Phase 2) */
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

static size_t dtype_size(MPI_Datatype dt) {
    switch (dt) {
        case MPI_CHAR: case MPI_BYTE: return 1;
        case MPI_INT: case MPI_UNSIGNED: case MPI_FLOAT: return 4;
        case MPI_DOUBLE: case MPI_LONG: case MPI_UNSIGNED_LONG:
        case MPI_LONG_LONG: return 8;
        default: return 4;
    }
}

/* Open/close the comm window. Outside instrumentation these are NOPs. */
static inline void mpi_comm_begin(void) { zsim_magic_op(ZSIM_MAGIC_OP_MPI_COMM_BEGIN); }
static inline void mpi_comm_end(void)   { zsim_magic_op(ZSIM_MAGIC_OP_MPI_COMM_END); }

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
static void inject_timing_recv(int src_pe, int dst_pe, uint64_t msg_size,
                               uint64_t send_time, uint64_t contend_wait) {
    tl_mpi_params.src_pe           = (uint32_t)src_pe;
    tl_mpi_params.dst_pe           = (uint32_t)dst_pe;
    tl_mpi_params.msg_size         = msg_size;
    tl_mpi_params.sim_send_time    = send_time;
    tl_mpi_params.sim_contend_wait = contend_wait;
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

/* Push one chunk to the dest mailbox. Blocks if ring is full. */
static int send_chunk(int dest, int tag, const char* data, uint32_t size,
                      uint32_t chunk_idx, uint32_t n_chunks, uint32_t msg_id,
                      uint64_t send_time, uint64_t contend_wait) {
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
        mpi_comm_begin();   /* frozen-clock bracket: plugin rewinds at END */
        pthread_mutex_lock(&mb->mtx);
        while (mb->count >= PIMID_MPI_RING_SLOTS)
            pthread_cond_wait(&mb->can_send, &mb->mtx);
        MessageSlot* slot = &mb->slots[mb->tail];
        slot->src           = g_rank;
        slot->tag           = tag;
        slot->size          = size;
        slot->chunk_idx     = chunk_idx;
        slot->n_chunks      = n_chunks;
        slot->msg_id           = msg_id;
        slot->sim_send_time    = send_time;
        slot->sim_contend_wait = contend_wait;
        if (data && size > 0) memcpy(slot->payload, data, size);
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

int MPI_Send(const void *buf, int count, MPI_Datatype datatype,
             int dest, int tag, MPI_Comm comm) {
    (void)comm;
    if (g_nranks <= 1) return MPI_SUCCESS;
    if (dest < 0 || dest >= g_nranks) return MPI_SUCCESS;

    size_t nbytes = (size_t)count * dtype_size(datatype);
    const char* p = (const char*)buf;

    /* Emit the SEND timing op FIRST: it charges send latency on this core and
     * (under instrumentation) writes the current simulated time into
     * tl_mpi_params.sim_now, which we then stamp onto every chunk so the
     * receiver can rendezvous in simulated time. msg_id is incremented here. */
    inject_timing_send(g_rank, dest, nbytes);
    uint64_t send_time = tl_mpi_params.sim_now;
    uint32_t msg_id    = (uint32_t)tl_mpi_params.msg_id;

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
    uint64_t contend_wait = 0;
    if (tl_mpi_params.noc_detailed) {
        uint64_t dur = tl_mpi_params.noc_lat;
        mpi_comm_begin();
        contend_wait = contend_reserve(dest, send_time, dur);
        inject_contend(contend_wait);
        mpi_comm_end();
    }

    if (nbytes == 0) {
        send_chunk(dest, tag, nullptr, 0, 0, 1, msg_id, send_time, contend_wait);
    } else {
        uint32_t n_chunks = (uint32_t)((nbytes + PIMID_MPI_MAX_MSG_BYTES - 1)
                                       / PIMID_MPI_MAX_MSG_BYTES);
        size_t left = nbytes;
        for (uint32_t i = 0; i < n_chunks; i++) {
            uint32_t take = (uint32_t)((left < PIMID_MPI_MAX_MSG_BYTES) ? left : PIMID_MPI_MAX_MSG_BYTES);
            send_chunk(dest, tag, p, take, i, n_chunks, msg_id, send_time, contend_wait);
            p += take; left -= take;
        }
    }

    return MPI_SUCCESS;
}

int MPI_Recv(void *buf, int count, MPI_Datatype datatype,
             int source, int tag, MPI_Comm comm, MPI_Status *status) {
    (void)comm; (void)status; (void)source; (void)tag;
    if (g_nranks <= 1) return MPI_SUCCESS;
    /* MPI_PROC_NULL (-1) recv is a no-op in real MPI; without this guard an
     * edge rank (e.g. stencil halo exchange) blocks forever on a mailbox no
     * rank will ever send to. Mirrors the dest guard in MPI_Send. */
    if (source < 0 || source >= g_nranks) return MPI_SUCCESS;

    size_t nbytes = (size_t)count * dtype_size(datatype);
    char* p = (char*)buf;
    size_t written = 0;
    int last_src = -1;

    /* Receive until we've assembled a complete message (1+ chunks). For
     * the FIFO-matching policy this implementation has always used, we
     * receive chunks of the next message in order. */
    MessageSlot slot;
    uint64_t send_time = 0;
    uint64_t contend_wait = 0;
    int want = g_thread_mode ? source : -1;   /* thread mode: honor source */
    do {
        recv_slot(&slot, want);
        if (last_src < 0) { last_src = slot.src; want = g_thread_mode ? slot.src : want; }
        send_time    = slot.sim_send_time;     /* all chunks share the stamp */
        contend_wait = slot.sim_contend_wait;  /* and the contention wait */
        size_t space = (nbytes > written) ? (nbytes - written) : 0;
        size_t take = (slot.size < space) ? slot.size : space;
        if (p && take > 0) memcpy(p + written, slot.payload, take);
        written += take;
    } while (slot.chunk_idx + 1 < slot.n_chunks);

    inject_timing_recv(last_src, g_rank, written, send_time, contend_wait);
    return MPI_SUCCESS;
}

int MPI_Sendrecv(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                 int dest, int sendtag,
                 void *recvbuf, int recvcount, MPI_Datatype recvtype,
                 int source, int recvtag,
                 MPI_Comm comm, MPI_Status *status) {
    MPI_Send(sendbuf, sendcount, sendtype, dest, sendtag, comm);
    MPI_Recv(recvbuf, recvcount, recvtype, source, recvtag, comm, status);
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
        } else {
            mpi_comm_begin();   /* frozen-clock bracket */
            while (g_shared->barrier_gen == gen)
                pthread_cond_wait(&g_shared->bar_cv, &g_shared->bar_mtx);
            mpi_comm_end();
        }
        uint64_t release = g_shared->bar_release;
        pthread_mutex_unlock(&g_shared->bar_mtx);
        tl_mpi_params.sim_send_time = release;
        zsim_magic_op(ZSIM_MAGIC_OP_MPI_ADVANCE);
        zsim_magic_op(ZSIM_MAGIC_OP_MPI_BARRIER);
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

int MPI_Bcast(void *buffer, int count, MPI_Datatype datatype,
              int root, MPI_Comm comm) {
    size_t nbytes = (size_t)count * dtype_size(datatype);
    if (g_rank == root) {
        for (int i = 0; i < g_nranks; i++) {
            if (i != root) MPI_Send(buffer, count, datatype, i, 0, comm);
        }
    } else {
        MPI_Recv(buffer, count, datatype, root, 0, comm, MPI_STATUS_IGNORE);
    }
    (void)nbytes;
    return MPI_SUCCESS;
}

int MPI_Reduce(const void *sendbuf, void *recvbuf, int count,
               MPI_Datatype datatype, MPI_Op op, int root, MPI_Comm comm) {
    (void)op;
    size_t nbytes = (size_t)count * dtype_size(datatype);

    if (g_rank != root) {
        MPI_Send(sendbuf, count, datatype, root, 0, comm);
    } else {
        if (sendbuf && recvbuf) memcpy(recvbuf, sendbuf, nbytes);
        /* Receive from each non-root rank in turn. Reduction op not applied
         * -- this is a timing-accurate simulation, not a numerics oracle.
         * Workloads that need actual reduction values should compute them
         * on rank 0 from the received chunks. */
        char* tmp = (char*)malloc(nbytes ? nbytes : 1);
        for (int i = 0; i < g_nranks; i++) {
            if (i != root) {
                MPI_Recv(tmp, count, datatype, i, 0, comm, MPI_STATUS_IGNORE);
            }
        }
        free(tmp);
    }
    return MPI_SUCCESS;
}

int MPI_Allreduce(const void *sendbuf, void *recvbuf, int count,
                  MPI_Datatype datatype, MPI_Op op, MPI_Comm comm) {
    MPI_Reduce(sendbuf, recvbuf, count, datatype, op, 0, comm);
    MPI_Bcast(recvbuf, count, datatype, 0, comm);
    return MPI_SUCCESS;
}

int MPI_Scatter(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                void *recvbuf, int recvcount, MPI_Datatype recvtype,
                int root, MPI_Comm comm) {
    size_t send_sz = (size_t)sendcount * dtype_size(sendtype);
    if (g_rank == root) {
        const char* sbuf = (const char*)sendbuf;
        for (int i = 0; i < g_nranks; i++) {
            if (i == root) {
                if (recvbuf && sbuf) memcpy(recvbuf, sbuf + (size_t)i * send_sz, send_sz);
            } else {
                MPI_Send(sbuf + (size_t)i * send_sz, sendcount, sendtype, i, 0, comm);
            }
        }
    } else {
        MPI_Recv(recvbuf, recvcount, recvtype, root, 0, comm, MPI_STATUS_IGNORE);
    }
    return MPI_SUCCESS;
}

int MPI_Gather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
               void *recvbuf, int recvcount, MPI_Datatype recvtype,
               int root, MPI_Comm comm) {
    size_t recv_sz = (size_t)recvcount * dtype_size(recvtype);

    if (g_rank != root) {
        MPI_Send(sendbuf, sendcount, sendtype, root, 0, comm);
    } else {
        char* rbuf = (char*)recvbuf;
        if (sendbuf && rbuf) {
            size_t send_sz = (size_t)sendcount * dtype_size(sendtype);
            memcpy(rbuf + (size_t)root * recv_sz, sendbuf, send_sz);
        }
        for (int i = 0; i < g_nranks; i++) {
            if (i != root) {
                MPI_Recv(rbuf + (size_t)i * recv_sz, recvcount, recvtype,
                         i, 0, comm, MPI_STATUS_IGNORE);
            }
        }
    }
    return MPI_SUCCESS;
}

int MPI_Alltoall(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                 void *recvbuf, int recvcount, MPI_Datatype recvtype,
                 MPI_Comm comm) {
    size_t send_sz = (size_t)sendcount * dtype_size(sendtype);
    size_t recv_sz = (size_t)recvcount * dtype_size(recvtype);
    const char* sbuf = (const char*)sendbuf;
    char* rbuf = (char*)recvbuf;

    for (int i = 0; i < g_nranks; i++) {
        if (i == g_rank) {
            if (sbuf && rbuf) {
                memcpy(rbuf + (size_t)i * recv_sz,
                       sbuf + (size_t)i * send_sz, send_sz);
            }
        } else {
            MPI_Send(sbuf + (size_t)i * send_sz, sendcount, sendtype, i, 0, comm);
        }
    }
    for (int i = 0; i < g_nranks; i++) {
        if (i != g_rank) {
            MPI_Recv(rbuf + (size_t)i * recv_sz, recvcount, recvtype,
                     i, 0, comm, MPI_STATUS_IGNORE);
        }
    }
    return MPI_SUCCESS;
}

int MPI_Gatherv(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                void *recvbuf, const int *recvcounts, const int *displs,
                MPI_Datatype recvtype, int root, MPI_Comm comm) {
    size_t rtsz = dtype_size(recvtype);
    if (g_rank != root) {
        MPI_Send(sendbuf, sendcount, sendtype, root, 0, comm);
    } else {
        char* rbuf = (char*)recvbuf;
        for (int i = 0; i < g_nranks; i++) {
            if (i == root) {
                if (sendbuf && rbuf)
                    memcpy(rbuf + (size_t)displs[i] * rtsz, sendbuf,
                           (size_t)sendcount * dtype_size(sendtype));
            } else {
                /* FIFO-matched recv (source-agnostic, like the other
                 * collectives): every rank rebuilds the same recvcounts/displs
                 * from the preceding Allgather + Bcast, so all ranks agree on
                 * the layout and the dedup stays consistent. Timing shim, not a
                 * numerics oracle. */
                MPI_Recv(rbuf + (size_t)displs[i] * rtsz, recvcounts[i], recvtype,
                         i, 0, comm, MPI_STATUS_IGNORE);
            }
        }
    }
    return MPI_SUCCESS;
}

int MPI_Allgather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                  void *recvbuf, int recvcount, MPI_Datatype recvtype,
                  MPI_Comm comm) {
    /* Gather every rank's contribution onto rank 0, then broadcast the full
     * concatenation back so all ranks hold it. Built on the shim's Send/Recv
     * so it participates in the sim-time rendezvous / NoC-contention model. */
    MPI_Gather(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, 0, comm);
    MPI_Bcast(recvbuf, g_nranks * recvcount, recvtype, 0, comm);
    return MPI_SUCCESS;
}

int MPI_Allgatherv(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                   void *recvbuf, const int *recvcounts, const int *displs,
                   MPI_Datatype recvtype, MPI_Comm comm) {
    MPI_Gatherv(sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs,
                recvtype, 0, comm);
    /* Broadcast the assembled buffer. displs are contiguous from 0 in the
     * common case (e.g. bfs frontier gather), so [0,total) covers all data. */
    int total = 0;
    for (int i = 0; i < g_nranks; i++) total += recvcounts[i];
    MPI_Bcast(recvbuf, total, recvtype, 0, comm);
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
