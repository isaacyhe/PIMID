/* pimid_devorg.h -- EXPLICIT device-organization awareness for PIM benchmarks.
 *
 * The PIMID simulator prices a device memory access purely by LOCATION: data in
 * the accessing PE's own placement-level unit is CLOSE (fast local path); data in
 * another unit is FAR (crosses the network). It does NOT move, cache, translate,
 * or auto-prep anything -- that is the BENCHMARK's job.
 *
 * So a benchmark that wants near-data speed must EXPLICITLY relocate each PE's
 * working set into that PE's own device unit. To do that it must know the device
 * organization -- placement level, PE count, unit count, and pages-per-unit --
 * which the harness passes on the command line. This header parses that org and
 * gives the address math the simulator uses:
 *
 *     unit(addr) = (page(addr) / pagesPerUnit) % totalUnits,   page = addr >> 12
 *     PE i owns the contiguous unit range [i*unitsPerPe, (i+1)*unitsPerPe)
 *
 * A buffer aligned to (totalUnits * pagesPerUnit * 4096) has its byte 0 at unit 0;
 * PE i's slot at offset i*unitsPerPe*pagesPerUnit*4096 therefore maps to unit
 * i*unitsPerPe = PE i's first owned unit -> served LOCAL.
 *
 * COARSE / host-shared placement (a PE already sees the whole memory in host
 * layout) needs no relocation: needs_prep() returns 0 and the kernel runs on the
 * host-laid-out data directly.
 *
 * The relocate is ordinary setup code -- run it BEFORE zsim_roi_begin(), just like
 * the data init. In device-only mode there is no host, but the benchmark must
 * still DO the relocate so the ROI's accesses land local; the relocate itself is
 * NOT part of the measured ROI (only the compute ROI is measured).
 */
#ifndef PIMID_DEVORG_H_
#define PIMID_DEVORG_H_

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define PIMID_PAGE_BYTES 4096u

/* 1.9.23: HOST parallelism vs DEVICE parallelism.
 *
 * These are different quantities and may differ: the host phase (data
 * generation) runs on host CORES, the device phase runs on PEs. A kernel that
 * sizes its host phase by the PE count oversubscribes a smaller host (16 ranks
 * on a 1-core host); one that leaves it serial under-uses a larger one.
 *
 * pimid_host_cores() reports the host core count PIMID exported, falling back
 * to 1 when unset (device-only runs, or a legacy launcher). Use it for the
 * HOST phase; keep using dev.num_pes for per-PE placement and the ROI. */
static inline int pimid_host_cores(void) {
    const char* e = getenv("PIMID_HOST_CORES");
    int v = e ? atoi(e) : 0;
    return (v > 0) ? v : 1;
}

/* The device PE count as PIMID sees it. Prefer dev.num_pes from --pes when you
 * already have a pimid_devorg_t; this is for code that only needs the count. */
static inline int pimid_device_pes(void) {
    const char* e = getenv("PIMID_DEVICE_PES");
    int v = e ? atoi(e) : 0;
    return (v > 0) ? v : 1;
}

/* placement levels, matching the simulator's pe_hierarchy_level */
enum {
    PIMID_LVL_SUBARRAY = 0,
    PIMID_LVL_BANK = 1,
    PIMID_LVL_BANK_GROUP = 2,
    PIMID_LVL_CHIP = 3,
    PIMID_LVL_RANK = 4,
    PIMID_LVL_CHANNEL = 5,
    PIMID_LVL_LOGIC_DIE = 6
};

typedef struct {
    int level;          /* PIMID_LVL_* */
    int num_pes;        /* PEs sharing the device */
    uint64_t total_units;   /* placement-level units across the whole device */
    uint64_t pages_per_unit;/* 4KB pages per unit */
    uint64_t units_per_pe;  /* total_units / num_pes (>=1) */
} pimid_devorg_t;

static inline int pimid__arg_int(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

static inline int pimid__level_from_name(const char* s, int def) {
    if (!s) return def;
    if (!strcmp(s, "SUBARRAY"))   return PIMID_LVL_SUBARRAY;
    if (!strcmp(s, "BANK"))       return PIMID_LVL_BANK;
    if (!strcmp(s, "BANK_GROUP")) return PIMID_LVL_BANK_GROUP;
    if (!strcmp(s, "CHIP"))       return PIMID_LVL_CHIP;
    if (!strcmp(s, "RANK"))       return PIMID_LVL_RANK;
    if (!strcmp(s, "CHANNEL"))    return PIMID_LVL_CHANNEL;
    if (!strcmp(s, "LOGIC_DIE"))  return PIMID_LVL_LOGIC_DIE;
    return def;
}

/* Parse device org from argv. The harness passes:
 *   --placement <LEVEL> --pes <N> --total-units <U> --pages-per-unit <P>
 * Missing values fall back to a single host-shared unit (no prep). */
static inline pimid_devorg_t pimid_devorg_from_args(int argc, char** argv) {
    pimid_devorg_t d;
    const char* lvl = NULL;
    for (int i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "--placement")) { lvl = argv[i + 1]; break; }
    d.level = pimid__level_from_name(lvl, PIMID_LVL_CHANNEL);
    d.num_pes = pimid__arg_int(argc, argv, "--pes", 1);
    if (d.num_pes < 1) d.num_pes = 1;
    d.total_units = (uint64_t)pimid__arg_int(argc, argv, "--total-units", d.num_pes);
    if (d.total_units < 1) d.total_units = d.num_pes;
    d.pages_per_unit = (uint64_t)pimid__arg_int(argc, argv, "--pages-per-unit", 32);
    if (d.pages_per_unit < 1) d.pages_per_unit = 1;
    d.units_per_pe = d.total_units / (uint64_t)d.num_pes;
    if (d.units_per_pe < 1) d.units_per_pe = 1;
    return d;
}

/* CHANNEL / LOGIC_DIE are the host-shared aggregation levels: a PE sees the whole
 * device in host layout, so no relocation is needed (the simulator already serves
 * these local). The finer, device-private levels (subarray..rank) need the
 * benchmark to relocate each PE's slice into its own unit. */
static inline int pimid_devorg_needs_prep(const pimid_devorg_t* d) {
    return (d->num_pes > 1)
        && (d->level < PIMID_LVL_CHANNEL)
        && (d->units_per_pe < d->total_units);
}

/* Human-readable placement-level name (for unambiguous PREP status lines). */
static inline const char* pimid_devorg_level_name(int lvl) {
    switch (lvl) {
        case PIMID_LVL_SUBARRAY:   return "SUBARRAY";
        case PIMID_LVL_BANK:       return "BANK";
        case PIMID_LVL_BANK_GROUP: return "BANK_GROUP";
        case PIMID_LVL_CHIP:       return "CHIP";
        case PIMID_LVL_RANK:       return "RANK";
        case PIMID_LVL_CHANNEL:    return "CHANNEL";
        case PIMID_LVL_LOGIC_DIE:  return "LOGIC_DIE";
        default:                   return "UNKNOWN";
    }
}

/* Bytes per PE slot: the address span of a PE's owned units. */
static inline size_t pimid_devorg_slot_bytes(const pimid_devorg_t* d) {
    return (size_t)d->units_per_pe * (size_t)d->pages_per_unit * PIMID_PAGE_BYTES;
}

/* Allocate a device buffer of num_pes PE-local slots whose byte 0 maps to unit 0,
 * so slot i maps to PE i's units. The guest virtual address the simulator sees
 * is the returned base, aligned to one org "period" (period =
 * totalUnits*pagesPerUnit*4096), so unit(base) = (base/4096 / pagesPerUnit) %
 * totalUnits == 0. Works for any totalUnits (no power-of-two requirement).
 * The mapping is intentionally not returned/freed -- benchmark processes are
 * short-lived. Returns NULL when the mapping fails. *slot_bytes_out gets the
 * per-slot span.
 *
 * 1.11.94 (b01-pim-kernels-6, ruling H41): exact sizing with MAP_NORESERVE.
 * Before this the buffer was malloc(slot*num_pes + period + 4 KiB): about twice
 * the device's address span (DDR4 BANK 8 PEs 16 GiB, DDR5 32 GiB, HBM2 64 GiB,
 * HBM3 256 GiB), committed under heuristic overcommit, so it failed on any host
 * whose RAM+swap was smaller and the kernels lost their prep (or exited). Now
 * one anonymous MAP_NORESERVE mapping of span + period is reserved (no commit
 * charge), the base is aligned up to a period boundary, and the head before the
 * base and the tail after base + span are unmapped. What stays mapped is exactly
 * span = slot*num_pes bytes; pages are committed only when touched. */
static inline void* pimid_devorg_alloc(const pimid_devorg_t* d, size_t* slot_bytes_out) {
    size_t slot = pimid_devorg_slot_bytes(d);
    size_t period = (size_t)d->total_units * (size_t)d->pages_per_unit * PIMID_PAGE_BYTES;
    size_t span = slot * (size_t)d->num_pes;
    if (slot_bytes_out) *slot_bytes_out = slot;
    if (slot == 0 || span / slot != (size_t)d->num_pes) return NULL;   /* overflow */
    if (span > SIZE_MAX - period) return NULL;                         /* overflow */
    size_t len = span + period;   /* worst-case slack to reach a period boundary */
    void* m = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) return NULL;
    uintptr_t addr = (uintptr_t)m;
    uintptr_t aligned = ((addr + period - 1) / period) * period;  /* -> page maps to unit 0 */
    size_t head = (size_t)(aligned - addr);       /* multiple of 4096: mmap and period are */
    size_t tail = len - head - span;
    if (head) munmap(m, head);
    if (tail) munmap((char*)aligned + span, tail);
    return (void*)aligned;
}

/* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): a PE's prepared working set
 * must fit the PE's slot. A kernel whose per-PE need exceeds slot_bytes would
 * spill into the next PE's slot (gemv_omp / stream_triad_omp: overlapping
 * slots, wrong results, heap overflow) or, in the kernels that checked, fall
 * back to host layout in silence, so the cell measured something other than
 * the prepared kernel. Every devorg kernel now refuses instead: it prints the
 * need, the slot and the largest size argument that fits, and exits non-zero.
 *
 * need_fn(size, ctx) is the kernel's per-PE need (largest share) as a function
 * of its size argument; it must be non-decreasing in size. */
typedef size_t (*pimid_devorg_need_fn)(long size, const void* ctx);

/* Largest size in [0, size] whose need fits slot (0 when none does). */
static inline long pimid_devorg_largest_fit(pimid_devorg_need_fn fn, const void* ctx,
                                            long size, size_t slot) {
    long lo = 0, hi = size;          /* fn(hi) > slot is the caller's premise */
    while (hi - lo > 1) {
        long mid = lo + (hi - lo) / 2;
        if (fn(mid, ctx) <= slot) lo = mid; else hi = mid;
    }
    return lo;
}

/* Print the refusal (one line). The caller then exits non-zero (OMP/serial:
 * return 1; MPI: MPI_Abort). who is "" or e.g. "rank 3: ". */
static inline void pimid_devorg_report_slot_refusal(const char* who, const char* kernel,
                                                    const pimid_devorg_t* d,
                                                    size_t need, size_t slot,
                                                    const char* size_flag, long fit) {
    fprintf(stderr, "%spimid_devorg: PREP REFUSED: kernel=%s level=%s pes=%d per-PE "
            "need %zu B > slot %zu B (%llu unit(s) x %llu pages x 4096 B); ",
            who, kernel, pimid_devorg_level_name(d->level), d->num_pes, need, slot,
            (unsigned long long)d->units_per_pe, (unsigned long long)d->pages_per_unit);
    if (fit > 0)
        fprintf(stderr, "%s %ld is the largest that fits this geometry\n", size_flag, fit);
    else
        fprintf(stderr, "no %s fits this geometry\n", size_flag);
}

/* Byte pointer to PE i's slot within a pimid_devorg_alloc buffer. */
static inline void* pimid_devorg_pe_slot(void* base, int pe, size_t slot_bytes) {
    return (void*)((char*)base + (size_t)pe * slot_bytes);
}

#endif /* PIMID_DEVORG_H_ */
