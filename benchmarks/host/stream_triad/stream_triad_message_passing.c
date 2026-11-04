/* stream_triad_mpi.c -- STREAM Triad: a[i] = b[i] + scalar * c[i]
 * MPI domain decomposition. Each rank processes N/nprocs elements. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include "zsim_hooks.h"

#define DEFAULT_SIZE 8192

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

int main(int argc, char* argv[]) {
    MPI_Init(&argc, &argv);

    int rank, nprocs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    int N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    float scalar = 3.0f;

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

    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) zsim_roi_begin();

    /* Kernel: each rank processes its chunk */
    for (int i = 0; i < local_n; i++)
        a[i] = b[i] + scalar * c[i];

    if (rank == 0) zsim_roi_end();
    MPI_Barrier(MPI_COMM_WORLD);

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
