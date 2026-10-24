/* stream_triad_mpi.c -- STREAM Triad: a[i] = b[i] + scalar * c[i]
 * MPI domain decomposition. Each rank processes N/nprocs elements.
 *
 * Device-organization aware (PIMID), mirroring stream_triad_omp.c: the
 * simulator prices each PE access by LOCATION. D1 identity mapping -- rank r
 * owns PE r's slot (enforced ranks == num_pes). Prep (before the ROI, untimed,
 * SPMD): each rank relocates ITS b + c chunk into ITS own PE slot; the ROI
 * writes a's chunk there. Slot layout: [a chunk][b chunk][c chunk]. Coarse /
 * host-shared placement runs the original host-layout path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include "zsim_hooks.h"
#include "../../include/pimid_devorg.h"
#include "../../include/pimid_spm.h"

#define DEFAULT_SIZE 8192

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* 1.12.18: a[0..n) = b + scalar * c through the PE scratchpad
 * (pimid_spm.h). The RAM holds three equal buffers: a tile of b, a tile of c
 * and a tile of a. Each b and c tile is fetched once and each a tile is
 * written back once. With no scratchpad the tile is the whole range and this
 * is the plain element loop. */
static void stream_triad_range(pimid_spm_t* s, float* a, const float* b, const float* c,
                               float scalar, size_t n) {
    const size_t T = pimid_spm_tile(s, 3, sizeof(float));
    const size_t ob = 0, oc = s->cap ? T * sizeof(float) : 0, oa = s->cap ? 2 * T * sizeof(float) : 0;
    for (size_t i0 = 0; i0 < n; i0 += T) {
        const size_t m = (n - i0 < T) ? n - i0 : T;
        const float* bb = (const float*)pimid_spm_in(s, ob, b + i0, m * sizeof(float));
        const float* cb = (const float*)pimid_spm_in(s, oc, c + i0, m * sizeof(float));
        float* ab = (float*)pimid_spm_out(s, oa, a + i0);
        for (size_t i = 0; i < m; i++)
            ab[i] = bb[i] + scalar * cb[i];
        pimid_spm_flush(s, a + i0, ab, m * sizeof(float));
    }
}

/* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): need of the largest rank share
 * [a][b][c] chunks, chunk = ceil(N/nprocs). */
static size_t stream_triad_mpi_need(long n, const void* c) {
    long np = *(const int*)c;
    return 3ull * (size_t)((n + np - 1) / np) * sizeof(float);
}

int main(int argc, char* argv[]) {
    MPI_Init(&argc, &argv);

    int rank, nprocs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    int N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    float scalar = 3.0f;

    /* Device organization + D1 identity-mapping guard: ranks must == num_pes. */
    pimid_devorg_t dev = pimid_devorg_from_args(argc, argv);
    if (nprocs > dev.num_pes) {
        if (rank == 0)
            fprintf(stderr, "pimid_devorg: MPI world size (%d) != num_pes (%d); "
                    "identity mapping requires ranks <= PEs (rank r -> slot r). Aborting.\n",
                    nprocs, dev.num_pes);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    /* Domain decomposition: each rank handles a contiguous chunk */
    int base = N / nprocs;
    int rem  = N % nprocs;
    int local_n = base + (rank < rem ? 1 : 0);

    float* a = (float*)malloc(local_n * sizeof(float));
    float* b = (float*)malloc(local_n * sizeof(float));
    float* c = (float*)malloc(local_n * sizeof(float));
    if (!a || !b || !c) { fprintf(stderr, "rank %d: malloc failed\n", rank); MPI_Abort(MPI_COMM_WORLD, 1); }

    for (int i = 0; i < local_n; i++) {
        b[i] = 1.0f;
        c[i] = 2.0f;
    }

    /* ---- DATA PREP (before ROI, not timed): this rank's chunk -> its PE slot.
     * Slot layout: [a chunk][b chunk][c chunk], each local_n floats.
     * Assumes thread-MPI (the default/only supported mode): slot r maps to PE
     * r's units directly. Under the retired PIMID_MPI_PROCESS=1 legacy mode the
     * simulator's per-rank unit rotation (pe_memory_interface.h) would
     * double-shift these prepped slots. */
    int prep = pimid_devorg_needs_prep(&dev);
    size_t need = 3ull * (size_t)local_n * sizeof(float);
    size_t slot_bytes = 0;
    float* dbuf = NULL;
    float* ape = NULL, *bpe = NULL, *cpe = NULL;
    if (prep) {
        /* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): a need larger than the slot
         * is refused; it used to fall back to host layout in silence. */
        slot_bytes = pimid_devorg_slot_bytes(&dev);
        if (need > slot_bytes) {
            char who[32];
            snprintf(who, sizeof who, "rank %d: ", rank);
            pimid_devorg_report_slot_refusal(who, "stream_triad", &dev, need, slot_bytes, "--size",
                pimid_devorg_largest_fit(stream_triad_mpi_need, &nprocs, N, slot_bytes));
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        dbuf = (float*)pimid_devorg_alloc(&dev, &slot_bytes);
        if (!dbuf) {
            fprintf(stderr, "rank %d: devorg: alloc of the device buffer failed; "
                    "running host-layout (no prep)\n", rank);
            prep = 0;
        }
    }
    if (prep) {
        ape = (float*)pimid_devorg_pe_slot(dbuf, rank, slot_bytes);
        bpe = ape + (size_t)local_n;
        cpe = bpe + (size_t)local_n;
        memcpy(bpe, b, (size_t)local_n * sizeof(float));
        memcpy(cpe, c, (size_t)local_n * sizeof(float));
    }

    if (prep)
        fprintf(stderr, "[devorg] PREP ACTIVE: kernel=stream_triad level=%s pes=%d "
                "rank=%d slot_bytes=%zu\n",
                pimid_devorg_level_name(dev.level), dev.num_pes, rank, slot_bytes);

    pimid_spm_t spm;   /* 1.12.18: this rank's PE scratchpad */
    pimid_spm_open(&spm);

    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) zsim_roi_begin();

    /* Kernel: each rank processes its chunk (on slot pointers if prepped) */
    /* 1.12.18: no scratchpad -- the 1.12.17 code, unchanged */
    if (!spm.cap) {
        if (prep) {
            for (int i = 0; i < local_n; i++)
                ape[i] = bpe[i] + scalar * cpe[i];
        } else {
            for (int i = 0; i < local_n; i++)
                a[i] = b[i] + scalar * c[i];
        }
    } else {
        if (prep)
            stream_triad_range(&spm, ape, bpe, cpe, scalar, (size_t)local_n);
        else
            stream_triad_range(&spm, a, b, c, scalar, (size_t)local_n);
    }

    if (rank == 0) zsim_roi_end();
    MPI_Barrier(MPI_COMM_WORLD);
    pimid_spm_close(&spm);

    /* Gather this rank's chunk of a back to host layout (post-ROI, not timed). */
    if (prep)
        memcpy(a, ape, (size_t)local_n * sizeof(float));

    /* Checksum: each rank computes partial sum, reduce to rank 0 */
    double local_sum = 0.0;
    for (int i = 0; i < local_n; i++) local_sum += a[i];

    double global_sum = 0.0;
    MPI_Reduce(&local_sum, &global_sum, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

    if (rank == 0) {
        printf("BENCH_CHECKSUM: %f\n", global_sum);
        printf("BENCH_DONE\n");
    }

    free(a); free(b); free(c);
    MPI_Finalize();
    return 0;
}
