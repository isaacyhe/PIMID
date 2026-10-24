/* vector_add_mpi.c — Vector Addition: c[i] = a[i] + b[i]
 * MPI domain decomposition. Each rank processes N/nprocs elements. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include "zsim_hooks.h"
#include "../../include/pimid_spm.h"

#define DEFAULT_SIZE 8192

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* 1.12.18: c[0..n) = a + b through the PE scratchpad (pimid_spm.h). The
 * RAM holds three equal buffers: a tile of a, a tile of b and a tile of c.
 * Each a and b tile is fetched once and each c tile is written back once.
 * With no scratchpad the tile is the whole range and this is the plain
 * element loop. */
static void vector_add_range(pimid_spm_t* s, float* c, const float* a, const float* b,
                             size_t n) {
    const size_t T = pimid_spm_tile(s, 3, sizeof(float));
    const size_t oa = 0, ob = s->cap ? T * sizeof(float) : 0, oc = s->cap ? 2 * T * sizeof(float) : 0;
    for (size_t i0 = 0; i0 < n; i0 += T) {
        const size_t m = (n - i0 < T) ? n - i0 : T;
        const float* ab = (const float*)pimid_spm_in(s, oa, a + i0, m * sizeof(float));
        const float* bb = (const float*)pimid_spm_in(s, ob, b + i0, m * sizeof(float));
        float* cb = (float*)pimid_spm_out(s, oc, c + i0);
        for (size_t i = 0; i < m; i++)
            cb[i] = ab[i] + bb[i];
        pimid_spm_flush(s, c + i0, cb, m * sizeof(float));
    }
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
    float* b = (float*)malloc(local_n * sizeof(float));
    float* c = (float*)malloc(local_n * sizeof(float));
    if (!a || !b || !c) { fprintf(stderr, "rank %d: malloc failed\n", rank); MPI_Abort(MPI_COMM_WORLD, 1); }

    /* Initialize with same values as serial version */
    for (int i = 0; i < local_n; i++) {
        int gi = local_start + i;
        a[i] = (float)gi;
        b[i] = (float)(N - gi);
    }

    pimid_spm_t spm;   /* 1.12.18: this rank's PE scratchpad */
    pimid_spm_open(&spm);

    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) zsim_roi_begin();

    /* 1.12.18: no scratchpad -- the 1.12.17 code, unchanged */
    if (!spm.cap) {
        for (int i = 0; i < local_n; i++)
            c[i] = a[i] + b[i];
    } else {
        vector_add_range(&spm, c, a, b, (size_t)local_n);
    }

    if (rank == 0) zsim_roi_end();
    MPI_Barrier(MPI_COMM_WORLD);
    pimid_spm_close(&spm);

    /* Checksum: partial sum reduced to rank 0 */
    double local_sum = 0.0;
    for (int i = 0; i < local_n; i++) local_sum += c[i];

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
