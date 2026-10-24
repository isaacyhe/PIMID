/* reduction_mpi.c — Streaming sum reduction: result = sum(a[i])
 * MPI domain decomposition. Each rank sums its chunk, then MPI_Reduce
 * aggregates partial sums to rank 0 (inside the ROI since 1.12.18). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include "zsim_hooks.h"
#include "../../include/pimid_spm.h"

#define DEFAULT_SIZE 16384

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* 1.12.18: sum(a[0..n)) through the PE scratchpad (pimid_spm.h). The RAM
 * holds one buffer: a tile of a, each fetched once. The one running sum adds
 * the elements in index order across tiles (no per-tile partial sums), so the
 * result is bit-identical to the plain loop. With no scratchpad the tile is
 * the whole range and this is the plain loop. */
static double reduction_sum(pimid_spm_t* s, const float* a, size_t n) {
    const size_t T = pimid_spm_tile(s, 1, sizeof(float));
    double sum = 0.0;
    for (size_t i0 = 0; i0 < n; i0 += T) {
        const size_t m = (n - i0 < T) ? n - i0 : T;
        const float* ab = (const float*)pimid_spm_in(s, 0, a + i0, m * sizeof(float));
        for (size_t i = 0; i < m; i++)
            sum += ab[i];
    }
    return sum;
}

int main(int argc, char* argv[]) {
    MPI_Init(&argc, &argv);

    int rank, nprocs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    int N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);

    /* Domain decomposition */
    int base = N / nprocs;
    int rem  = N % nprocs;
    int local_start = rank * base + (rank < rem ? rank : rem);
    int local_n = base + (rank < rem ? 1 : 0);

    float* a = (float*)malloc(local_n * sizeof(float));
    if (!a) { fprintf(stderr, "rank %d: malloc failed\n", rank); MPI_Abort(MPI_COMM_WORLD, 1); }

    /* Initialize with same values as serial version */
    for (int i = 0; i < local_n; i++) {
        int gi = local_start + i;
        a[i] = (float)(gi % 100) * 0.01f;
    }

    pimid_spm_t spm;   /* 1.12.18: this rank's PE scratchpad */
    pimid_spm_open(&spm);

    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) zsim_roi_begin();

    /* Kernel: each rank computes partial sum */
    double local_sum = 0.0;
    /* 1.12.18: no scratchpad -- the 1.12.17 code, unchanged */
    if (!spm.cap) {
        for (int i = 0; i < local_n; i++)
            local_sum += a[i];
    } else {
        local_sum = reduction_sum(&spm, a, (size_t)local_n);
    }

    /* 1.12.18 (ruling 2026-10-23): the cross-rank reduction that produces the
     * result is timed, as the OpenMP reduction(+:sum) clause is: partial sums
     * are reduced to rank 0 INSIDE the ROI (both scratchpad branches end
     * here). Every rank calls it before rank 0's zsim_roi_end and the closing
     * barrier, so the last-rank ROI rule still counts every rank's arrival. */
    double global_sum = 0.0;
    MPI_Reduce(&local_sum, &global_sum, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

    if (rank == 0) zsim_roi_end();
    MPI_Barrier(MPI_COMM_WORLD);
    pimid_spm_close(&spm);

    if (rank == 0) {
        printf("BENCH_CHECKSUM: %f\n", global_sum);
        printf("BENCH_DONE\n");
    }

    free(a);
    MPI_Finalize();
    return 0;
}
