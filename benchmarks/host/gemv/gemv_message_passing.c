/* gemv_mpi.c -- General Matrix-Vector Multiply: y = A * x
 * MPI domain decomposition. Rows of A distributed across ranks.
 * Each rank needs the full x vector (broadcast from rank 0). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include "zsim_hooks.h"

#define DEFAULT_SIZE 256

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

    /* Domain decomposition: distribute rows across ranks */
    int base = N / nprocs;
    int rem  = N % nprocs;
    int local_rows = base + (rank < rem ? 1 : 0);
    int local_start = rank * base + (rank < rem ? rank : rem);

    /* Each rank allocates its local rows of A, full x, and local y */
    float* A_local = (float*)malloc((size_t)local_rows * N * sizeof(float));
    float* x = (float*)malloc(N * sizeof(float));
    float* y_local = (float*)calloc(local_rows, sizeof(float));
    if (!A_local || !x || !y_local) {
        fprintf(stderr, "rank %d: malloc failed\n", rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    /* Initialize: same pattern as serial (rank builds its rows) */
    for (int i = 0; i < local_rows; i++) {
        int gi = local_start + i;
        for (int j = 0; j < N; j++)
            A_local[i * N + j] = (float)((gi + j) % 7);
    }

    /* x initialized on all ranks (same values) */
    for (int i = 0; i < N; i++)
        x[i] = 1.0f;

    /* Broadcast x to all ranks (in case future versions init only on rank 0) */
    MPI_Bcast(x, N, MPI_FLOAT, 0, MPI_COMM_WORLD);

    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) zsim_roi_begin();

    /* Kernel: each rank computes its rows of y */
    for (int i = 0; i < local_rows; i++) {
        float sum = 0.0f;
        for (int j = 0; j < N; j++)
            sum += A_local[i * N + j] * x[j];
        y_local[i] = sum;
    }

    if (rank == 0) zsim_roi_end();
    MPI_Barrier(MPI_COMM_WORLD);

    /* Checksum: each rank computes partial checksum, reduce to rank 0 */
    double local_sum = 0.0;
    for (int i = 0; i < local_rows; i++) local_sum += y_local[i];

    double global_sum = 0.0;
    MPI_Reduce(&local_sum, &global_sum, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

    if (rank == 0) {
        printf("BENCH_CHECKSUM: %f\n", global_sum);
        printf("BENCH_DONE\n");
    }

    free(A_local); free(x); free(y_local);
    MPI_Finalize();
    return 0;
}
