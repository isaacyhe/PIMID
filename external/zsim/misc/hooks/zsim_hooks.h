#ifndef __ZSIM_HOOKS_H__
#define __ZSIM_HOOKS_H__

#include <stdint.h>
#include <stdio.h>

//Avoid optimizing compilers moving code around this barrier
#define COMPILER_BARRIER() { __asm__ __volatile__("" ::: "memory");}

//These need to be in sync with the simulator
#define ZSIM_MAGIC_OP_ROI_BEGIN         (1025)
#define ZSIM_MAGIC_OP_ROI_END           (1026)
#define ZSIM_MAGIC_OP_REGISTER_THREAD   (1027)
#define ZSIM_MAGIC_OP_HEARTBEAT         (1028)
#define ZSIM_MAGIC_OP_WORK_BEGIN        (1029) //ubik
#define ZSIM_MAGIC_OP_WORK_END          (1030) //ubik

/* PIMID MPI magic ops — unified in-process MPI timing */
#define ZSIM_MAGIC_OP_MPI_REGISTER  (2048)
#define ZSIM_MAGIC_OP_MPI_SEND      (2049)
#define ZSIM_MAGIC_OP_MPI_RECV      (2050)
#define ZSIM_MAGIC_OP_MPI_BARRIER   (2051)
/* Comm-window bracket: keep the rank core attached across transport polls. */
#define ZSIM_MAGIC_OP_MPI_COMM_BEGIN (2055)
#define ZSIM_MAGIC_OP_MPI_COMM_END   (2056)
/* Phase-2 cross-rank NoC contention: charge sender queueing wait. */
#define ZSIM_MAGIC_OP_MPI_CONTEND    (2057)

#ifdef __x86_64__
#define HOOKS_STR  "HOOKS"
static inline void zsim_magic_op(uint64_t op) {
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;" : : "c"(op));
    COMPILER_BARRIER();
}
#else
#define HOOKS_STR  "NOP-HOOKS"
static inline void zsim_magic_op(uint64_t op) {
    //NOP
}
#endif

/* The ROI markers print nothing. A print here runs on the core being measured: under
 * thread-MPI the region starts at the entry barrier's release and ends at the last rank's
 * arrival at the closing barrier, so rank 0's print before ROI_BEGIN and after ROI_END was
 * timed (about 10 k cycles each on a PE). The simulator reports every ROI transition itself.
 * The marker string stays in the binary: the simulator looks for it to know that the
 * program has an ROI. */
static const char zsim_roi_marker[] __attribute__((used)) = "[" HOOKS_STR "] ROI begin";

static inline void zsim_roi_begin() {
    zsim_magic_op(ZSIM_MAGIC_OP_ROI_BEGIN);
}

static inline void zsim_roi_end() {
    zsim_magic_op(ZSIM_MAGIC_OP_ROI_END);
}

static inline void zsim_heartbeat() {
    zsim_magic_op(ZSIM_MAGIC_OP_HEARTBEAT);
}

static inline void zsim_work_begin() { zsim_magic_op(ZSIM_MAGIC_OP_WORK_BEGIN); }
static inline void zsim_work_end() { zsim_magic_op(ZSIM_MAGIC_OP_WORK_END); }

/* Data-size-aware WORK_BEGIN: high 32 bits carry payload size in bytes.
 * When size_bytes > 0, PCIe M/D/1 uses actual transfer size instead of
 * fixed 64B cache line for serialization. size_bytes == 0 → default 64B. */
static inline void zsim_work_begin_sized(uint32_t size_bytes) {
    uint64_t encoded = ((uint64_t)size_bytes << 32) | ZSIM_MAGIC_OP_WORK_BEGIN;
    zsim_magic_op(encoded);
}

/* ---- PIMID MPI parameter block ---- */

struct __attribute__((aligned(64))) PimidMpiParams {
    uint32_t src_pe;
    uint32_t dst_pe;
    uint64_t msg_size;
    uint64_t msg_id;
    uint64_t sim_now;          /* plugin -> guest: current sim-time (SEND) */
    uint64_t sim_send_time;    /* guest -> plugin: message send-time (RECV) */
    uint64_t sim_contend_wait; /* guest -> plugin: cross-rank contention wait */
    uint64_t noc_lat;          /* plugin -> guest: real per-message NoC cost
                                * (Garnet RTT) = occupancy reserve duration */
    uint32_t noc_detailed;     /* plugin -> guest: 1=detailed (cycle-accurate)
                                * Garnet -> apply contention; 0=analytical */
    uint32_t _pad;
};

/* Register this thread's MPI parameter block address with ZSim.
 * Must be called once per thread after MPI_Init. */
static inline void zsim_mpi_register(PimidMpiParams* params) {
    /* The magic op handler reads the params address from RCX.
     * We pass the address as the op value; the handler knows REGISTER
     * means "read RCX as an address". For non-x86, this is a NOP. */
#ifdef __x86_64__
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;" : : "c"(ZSIM_MAGIC_OP_MPI_REGISTER));
    COMPILER_BARRIER();
    /* Now pass the params address via a second magic op with the address */
    (void)params;  /* address passed via separate mechanism — see pimid_mpi.cpp */
#else
    (void)params;
#endif
}

static inline void zsim_mpi_send(void) { zsim_magic_op(ZSIM_MAGIC_OP_MPI_SEND); }
static inline void zsim_mpi_recv(void) { zsim_magic_op(ZSIM_MAGIC_OP_MPI_RECV); }
static inline void zsim_mpi_barrier(void) { zsim_magic_op(ZSIM_MAGIC_OP_MPI_BARRIER); }

/* ---- Host-Device Co-Simulation Offload API ----
 *
 * Semantically modeled on CUDA: kernel launch is a synchronous handoff of
 * control from host to device, executed on the SAME guest thread. The
 * simulator transitions the thread's domain to DEVICE via zsim_work_begin
 * (also injecting host->device PCIe latency), runs the kernel under
 * DEVICE accounting (cycles attributed to ALU PEs), then transitions back
 * to HOST via zsim_work_end (with device->host PCIe latency).
 *
 * Earlier this routine spawned a pthread for the device side, mirroring the
 * traditional Pin-mode ZSim host/device pattern. That created a dynamic-
 * thread admission race: QEMU asynchronously fires vcpu_init_cb for the
 * cloned device pthread, which is admitted to the phase barrier as a HOST
 * participant before WORK_BEGIN can retag it; meanwhile the host thread is
 * parked in pthread_join outside the barrier. Two participants, one able to
 * make progress, deadlock. The fix is to remove the pthread entirely —
 * a CUDA-style synchronous launch needs only a single guest thread doing
 * a domain transition, not a producer/consumer pair fighting over the
 * barrier. No new magic ops were required.
 *
 * Async device kernels (kernel queued, host continues, sync later) are a
 * separate model; if/when added they will be a different API call, not a
 * silent change to this synchronous one.
 */

static inline void pimid_offload_sync(void (*kernel)(void*), void* arg) {
    zsim_work_begin();   /* host thread switches to DOMAIN_DEVICE */
    kernel(arg);         /* runs inline; accounted to device PEs */
    zsim_work_end();     /* back to DOMAIN_HOST */
}

#endif /*__ZSIM_HOOKS_H__*/
