/* stencil_2d_mpi.c -- 2D 5-point Jacobi stencil (nearest-neighbor average)
 * MPI domain decomposition. Rows of the N*N grid are distributed across
 * ranks. Ghost rows are exchanged via MPI_Sendrecv between neighbors. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include "zsim_hooks.h"

#define DEFAULT_SIZE  64
#define DEFAULT_ITERS 5

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
    int iters = parse_int_arg(argc, argv, "--iters", DEFAULT_ITERS);

    /* Domain decomposition: distribute rows across ranks.
     * Each rank owns local_rows rows of the grid but allocates
     * local_rows+2 rows to hold ghost rows (top and bottom). */
    int base = N / nprocs;
    int rem  = N % nprocs;
    int local_start = rank * base + (rank < rem ? rank : rem);
    int local_rows  = base + (rank < rem ? 1 : 0);

    /* Allocate grid with ghost rows: row 0 = top ghost, rows 1..local_rows = data,
     * row local_rows+1 = bottom ghost */
    int alloc_rows = local_rows + 2;
    float* grid = (float*)calloc((size_t)alloc_rows * N, sizeof(float));
    float* tmp  = (float*)calloc((size_t)alloc_rows * N, sizeof(float));
    if (!grid || !tmp) { fprintf(stderr, "rank %d: malloc failed\n", rank); MPI_Abort(MPI_COMM_WORLD, 1); }

    /* Initialize: hot boundary on global top row (row 0).
     * If this rank owns global row 0, set it in local row 1. */
    if (local_start == 0) {
        for (int j = 0; j < N; j++)
            grid[1 * N + j] = 100.0f;
    }

    int up_rank   = (rank > 0) ? rank - 1 : MPI_PROC_NULL;
    int down_rank = (rank < nprocs - 1) ? rank + 1 : MPI_PROC_NULL;

    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) zsim_roi_begin();

    for (int t = 0; t < iters; t++) {
        /* Exchange ghost rows with neighbors.
         * Send my top data row (row 1) to up_rank's bottom ghost.
         * Receive from up_rank into my top ghost (row 0). */
        MPI_Sendrecv(&grid[1 * N], N, MPI_FLOAT, up_rank, 0,
                     &grid[0 * N], N, MPI_FLOAT, up_rank, 1,
                     MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        /* Send my bottom data row (row local_rows) to down_rank's top ghost.
         * Receive from down_rank into my bottom ghost (row local_rows+1). */
        MPI_Sendrecv(&grid[local_rows * N], N, MPI_FLOAT, down_rank, 1,
                     &grid[(local_rows + 1) * N], N, MPI_FLOAT, down_rank, 0,
                     MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        /* Stencil computation on interior points.
         * Local rows 1..local_rows map to global rows local_start..local_start+local_rows-1.
         * Skip global boundary rows (row 0 and row N-1). */
        for (int li = 1; li <= local_rows; li++) {
            int gi = local_start + (li - 1);  /* global row index */
            if (gi == 0 || gi == N - 1) {
                /* Copy boundary rows unchanged */
                memcpy(&tmp[li * N], &grid[li * N], N * sizeof(float));
                continue;
            }
            /* Column boundaries stay zero */
            tmp[li * N + 0] = grid[li * N + 0];
            tmp[li * N + (N - 1)] = grid[li * N + (N - 1)];
            for (int j = 1; j < N - 1; j++) {
                tmp[li * N + j] = 0.25f * (
                    grid[(li - 1) * N + j] + grid[(li + 1) * N + j] +
                    grid[li * N + (j - 1)] + grid[li * N + (j + 1)]);
            }
        }

        /* Swap grid and tmp */
        float* s = grid; grid = tmp; tmp = s;
    }

    if (rank == 0) zsim_roi_end();
    MPI_Barrier(MPI_COMM_WORLD);

    /* Checksum: each rank sums its local data rows (rows 1..local_rows) */
    double local_sum = 0.0;
    for (int li = 1; li <= local_rows; li++)
        for (int j = 0; j < N; j++)
            local_sum += grid[li * N + j];

    double global_sum = 0.0;
    MPI_Reduce(&local_sum, &global_sum, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

    if (rank == 0) {
        printf("BENCH_CHECKSUM: %f\n", global_sum);
        printf("BENCH_DONE\n");
    }

    free(grid); free(tmp);
    MPI_Finalize();
    return 0;
}
