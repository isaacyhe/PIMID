/* spmv_csr_mpi.c -- Sparse Matrix-Vector Multiply in CSR format: y = A * x
 * MPI domain decomposition. Rank 0 generates the full CSR matrix, then
 * distributes row ranges to all ranks. Each rank computes its rows of y. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include "zsim_hooks.h"

#define DEFAULT_SIZE 1024

static uint32_t bench_rand(uint32_t* s) {
    *s = *s * 1103515245 + 12345;
    return (*s >> 16) & 0x7FFF;
}

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
    int density_pct = parse_int_arg(argc, argv, "--density", 1);

    /* All ranks generate the same CSR with the same seed (deterministic LCG) */
    uint32_t seed = 42;

    /* Count nonzeros per row */
    int* row_nnz = (int*)calloc(N, sizeof(int));
    if (!row_nnz) { fprintf(stderr, "rank %d: malloc failed\n", rank); MPI_Abort(MPI_COMM_WORLD, 1); }

    int total_nnz = 0;
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            if ((int)(bench_rand(&seed) % 100) < density_pct) {
                row_nnz[i]++;
                total_nnz++;
            }
        }
    }

    /* Allocate CSR arrays */
    int* row_ptr = (int*)malloc((N + 1) * sizeof(int));
    int* col_idx = (int*)malloc(total_nnz * sizeof(int));
    float* values = (float*)malloc(total_nnz * sizeof(float));
    float* x = (float*)malloc(N * sizeof(float));
    if (!row_ptr || !col_idx || !values || !x) {
        fprintf(stderr, "rank %d: malloc failed\n", rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    /* Build CSR structure */
    row_ptr[0] = 0;
    for (int i = 0; i < N; i++)
        row_ptr[i + 1] = row_ptr[i] + row_nnz[i];

    /* Re-generate with same seed to fill col_idx and values */
    seed = 42;
    int* cur = (int*)calloc(N, sizeof(int));
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            if ((int)(bench_rand(&seed) % 100) < density_pct) {
                int pos = row_ptr[i] + cur[i];
                col_idx[pos] = j;
                values[pos] = 1.0f;
                cur[i]++;
            }
        }
    }
    free(cur);
    free(row_nnz);

    for (int i = 0; i < N; i++) x[i] = 1.0f;

    /* Domain decomposition: each rank handles a range of rows */
    int base = N / nprocs;
    int rem  = N % nprocs;
    int local_start = rank * base + (rank < rem ? rank : rem);
    int local_rows  = base + (rank < rem ? 1 : 0);

    float* y_local = (float*)calloc(local_rows, sizeof(float));
    if (!y_local) { fprintf(stderr, "rank %d: malloc failed\n", rank); MPI_Abort(MPI_COMM_WORLD, 1); }

    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) zsim_roi_begin();

    /* Kernel: each rank computes its rows */
    for (int i = 0; i < local_rows; i++) {
        int gi = local_start + i;
        float sum = 0.0f;
        for (int j = row_ptr[gi]; j < row_ptr[gi + 1]; j++)
            sum += values[j] * x[col_idx[j]];
        y_local[i] = sum;
    }

    if (rank == 0) zsim_roi_end();
    MPI_Barrier(MPI_COMM_WORLD);

    /* Checksum: partial sum reduced to rank 0 */
    double local_sum = 0.0;
    for (int i = 0; i < local_rows; i++) local_sum += y_local[i];

    double global_sum = 0.0;
    MPI_Reduce(&local_sum, &global_sum, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

    if (rank == 0) {
        printf("BENCH_NNZ: %d\n", total_nnz);
        printf("BENCH_CHECKSUM: %f\n", global_sum);
        printf("BENCH_DONE\n");
    }

    free(row_ptr); free(col_idx); free(values);
    free(x); free(y_local);
    MPI_Finalize();
    return 0;
}
