/* histogram_mpi.c -- Histogram construction: random scatter into bins
 * MPI domain decomposition. Each rank processes N/nprocs data elements
 * into a local histogram, then MPI_Reduce sums all local histograms.
 *
 * Device-organization aware (PIMID), mirroring histogram_omp.c: the simulator
 * prices each PE access by LOCATION. D1 identity mapping -- rank r owns PE r's
 * slot (enforced ranks == num_pes). Prep (before the ROI, untimed, SPMD): each
 * rank relocates ITS input chunk + its private histogram into ITS own PE slot;
 * the ROI scan is then PE-local. Slot layout: [chunk][local hist]. The final
 * MPI_Reduce (the shared cross-PE step) is unchanged. Coarse placement runs the
 * host path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include "zsim_hooks.h"
#include "../../include/pimid_devorg.h"

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

    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) zsim_roi_begin();

    /* Kernel: each rank builds a local histogram from its data chunk
     * (on slot pointers if prepped) */
    if (prep) {
        for (int i = 0; i < local_n; i++)
            hpe[dpe[i]]++;
    } else {
        for (int i = 0; i < local_n; i++)
            local_hist[local_data[i]]++;
    }

    if (rank == 0) zsim_roi_end();
    MPI_Barrier(MPI_COMM_WORLD);

    /* Gather this rank's private histogram back to host layout for the
     * MPI_Reduce (post-ROI, not timed). */
    if (prep)
        memcpy(local_hist, hpe, (size_t)nbins * sizeof(int));

    /* Reduce local histograms to global histogram on rank 0 */
    MPI_Reduce(local_hist, global_hist, nbins, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);

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
