/* histogram_mpi.c -- Histogram construction: random scatter into bins
 * MPI domain decomposition. Each rank processes N/nprocs data elements
 * into a local histogram, then MPI_Reduce sums all local histograms.
 *
 * Device-organization aware (PIMID), mirroring histogram_omp.c: the simulator
 * prices each PE access by LOCATION. D1 identity mapping -- rank r owns PE r's
 * slot (enforced ranks == num_pes). Prep (before the ROI, untimed, SPMD): each
 * rank relocates ITS input chunk + its private histogram into ITS own PE slot;
 * the ROI scan is then PE-local. Slot layout: [chunk][local hist]. The final
 * MPI_Reduce (the shared cross-PE step) produces the result and is timed:
 * 1.12.18 (ruling 2026-10-23) moved it inside the ROI. Coarse placement runs
 * the host path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include "zsim_hooks.h"
#include "../../include/pimid_devorg.h"
#include "../../include/pimid_spm.h"

#define DEFAULT_SIZE 16384
#define DEFAULT_BINS 256

static uint32_t bench_rand(uint32_t* s) {
    *s = *s * 1103515245 + 12345;
    return (*s >> 16) & 0x7FFF;
}

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* 1.12.18: hist[data[i]]++ for data[0..n) through the PE scratchpad
 * (pimid_spm.h). When the nbins counters fit, the RAM holds the whole
 * histogram (fetched once, written back once) and the rest of it is one
 * buffer for data tiles, each fetched once. Otherwise it holds two equal
 * buffers, a tile of the bins and a tile of data: each bin tile is fetched and
 * written back once and rescans all the data, counting the values that fall
 * in it. With no scratchpad the tiles are whole ranges and this is the plain
 * scatter loop. */
static void histogram_count(pimid_spm_t* s, int* hist, const int* data, size_t n,
                            size_t nbins) {
    const size_t T2 = pimid_spm_tile(s, 2, sizeof(int));
    const size_t hbytes = s->cap ? (nbins * sizeof(int) + 63) / 64 * 64 : 0;
    const int fits = !s->cap || hbytes + 64 <= s->cap;
    const size_t TB = fits ? nbins : T2;
    const size_t TD = (s->cap && fits) ? (s->cap - hbytes) / 64 * 64 / sizeof(int) : T2;
    const size_t oh = 0, od = s->cap ? (fits ? hbytes : T2 * sizeof(int)) : 0;
    for (size_t b0 = 0; b0 < nbins; b0 += TB) {
        const size_t nb = (nbins - b0 < TB) ? nbins - b0 : TB;
        int* hb = (int*)pimid_spm_in(s, oh, hist + b0, nb * sizeof(int));
        for (size_t i0 = 0; i0 < n; i0 += TD) {
            const size_t m = (n - i0 < TD) ? n - i0 : TD;
            const int* db = (const int*)pimid_spm_in(s, od, data + i0, m * sizeof(int));
            if (nb == nbins) {
                for (size_t i = 0; i < m; i++)
                    hb[db[i]]++;
            } else {
                for (size_t i = 0; i < m; i++) {
                    const size_t d = (size_t)db[i] - b0;
                    if (d < nb) hb[d]++;
                }
            }
        }
        pimid_spm_flush(s, hist + b0, hb, nb * sizeof(int));
    }
}

/* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): need of the largest rank share
 * (rank 0 holds ceil(N/nprocs)) for [chunk][local hist]. */
typedef struct { int nprocs; int nbins; } histogram_mpi_ctx;
static size_t histogram_mpi_need(long n, const void* c) {
    const histogram_mpi_ctx* x = (const histogram_mpi_ctx*)c;
    return ((size_t)((n + x->nprocs - 1) / x->nprocs) + (size_t)x->nbins) * sizeof(int);
}

int main(int argc, char* argv[]) {
    MPI_Init(&argc, &argv);

    int rank, nprocs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    int N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    int nbins = parse_int_arg(argc, argv, "--bins", DEFAULT_BINS);

    /* Device organization + D1 identity-mapping guard: ranks must == num_pes. */
    pimid_devorg_t dev = pimid_devorg_from_args(argc, argv);
    if (nprocs > dev.num_pes) {
        if (rank == 0)
            fprintf(stderr, "pimid_devorg: MPI world size (%d) != num_pes (%d); "
                    "identity mapping requires ranks <= PEs (rank r -> slot r). Aborting.\n",
                    nprocs, dev.num_pes);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    /* All ranks generate the full data array with the same seed,
     * but each rank only processes its chunk */
    uint32_t seed = 42;

    /* Domain decomposition */
    int base = N / nprocs;
    int rem  = N % nprocs;
    int local_start = rank * base + (rank < rem ? rank : rem);
    int local_n = base + (rank < rem ? 1 : 0);

    /* Generate data: advance RNG to local_start, then generate local_n values */
    int* local_data = (int*)malloc(local_n * sizeof(int));
    if (!local_data) { fprintf(stderr, "rank %d: malloc failed\n", rank); MPI_Abort(MPI_COMM_WORLD, 1); }

    /* Advance seed past elements before local_start */
    for (int i = 0; i < local_start; i++)
        bench_rand(&seed);

    for (int i = 0; i < local_n; i++)
        local_data[i] = bench_rand(&seed) % nbins;

    int* local_hist = (int*)calloc(nbins, sizeof(int));
    int* global_hist = NULL;
    if (rank == 0)
        global_hist = (int*)calloc(nbins, sizeof(int));

    if (!local_hist) { fprintf(stderr, "rank %d: malloc failed\n", rank); MPI_Abort(MPI_COMM_WORLD, 1); }

    /* ---- DATA PREP (before ROI, not timed): this rank's chunk + private hist
     * -> its PE slot. Slot layout: [chunk][local hist].
     * Assumes thread-MPI (the default/only supported mode): slot r maps to PE
     * r's units directly. Under the retired PIMID_MPI_PROCESS=1 legacy mode the
     * simulator's per-rank unit rotation (pe_memory_interface.h) would
     * double-shift these prepped slots. */
    int prep = pimid_devorg_needs_prep(&dev);
    size_t need = ((size_t)local_n + (size_t)nbins) * sizeof(int);
    histogram_mpi_ctx need_ctx = { nprocs, nbins };
    size_t slot_bytes = 0;
    int* dbuf = NULL;
    int* dpe = NULL, *hpe = NULL;
    if (prep) {
        /* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): a need larger than the slot
         * is refused; it used to fall back to host layout in silence. */
        slot_bytes = pimid_devorg_slot_bytes(&dev);
        if (need > slot_bytes) {
            char who[32];
            snprintf(who, sizeof who, "rank %d: ", rank);
            pimid_devorg_report_slot_refusal(who, "histogram", &dev, need, slot_bytes, "--size",
                pimid_devorg_largest_fit(histogram_mpi_need, &need_ctx, N, slot_bytes));
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        dbuf = (int*)pimid_devorg_alloc(&dev, &slot_bytes);
        if (!dbuf) {
            fprintf(stderr, "rank %d: devorg: alloc of the device buffer failed; "
                    "running host-layout (no prep)\n", rank);
            prep = 0;
        }
    }
    if (prep) {
        dpe = (int*)pimid_devorg_pe_slot(dbuf, rank, slot_bytes);
        hpe = dpe + local_n;
        memcpy(dpe, local_data, (size_t)local_n * sizeof(int));
        memset(hpe, 0, (size_t)nbins * sizeof(int));
    }

    if (prep)
        fprintf(stderr, "[devorg] PREP ACTIVE: kernel=histogram level=%s pes=%d "
                "rank=%d slot_bytes=%zu\n",
                pimid_devorg_level_name(dev.level), dev.num_pes, rank, slot_bytes);

    pimid_spm_t spm;   /* 1.12.18: this rank's PE scratchpad */
    pimid_spm_open(&spm);

    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) zsim_roi_begin();

    /* Kernel: each rank builds a local histogram from its data chunk
     * (on slot pointers if prepped) */
    /* 1.12.18: no scratchpad -- the 1.12.17 code, unchanged */
    if (!spm.cap) {
        if (prep) {
            for (int i = 0; i < local_n; i++)
                hpe[dpe[i]]++;
        } else {
            for (int i = 0; i < local_n; i++)
                local_hist[local_data[i]]++;
        }
    } else {
        if (prep)
            histogram_count(&spm, hpe, dpe, (size_t)local_n, (size_t)nbins);
        else
            histogram_count(&spm, local_hist, local_data, (size_t)local_n, (size_t)nbins);
    }

    /* 1.12.18 (ruling 2026-10-23): the cross-rank merge that produces the
     * result is part of the kernel and is timed, as the shared-memory merge
     * is: every rank's private histogram (read straight from its PE slot when
     * prepped -- the post-ROI copy back to host layout is gone) is summed into
     * rank 0's global histogram INSIDE the ROI. Both scratchpad branches end
     * here. Every rank calls it before rank 0's zsim_roi_end and the closing
     * barrier, so the last-rank ROI rule still counts every rank's arrival. */
    MPI_Reduce(prep ? hpe : local_hist, global_hist, nbins, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);

    if (rank == 0) zsim_roi_end();
    MPI_Barrier(MPI_COMM_WORLD);
    pimid_spm_close(&spm);

    if (rank == 0) {
        long checksum = 0;
        for (int i = 0; i < nbins; i++) checksum += global_hist[i];
        printf("BENCH_CHECKSUM: %ld\n", checksum);
        printf("BENCH_DONE\n");
        free(global_hist);
    }

    free(local_data); free(local_hist);
    MPI_Finalize();
    return 0;
}
