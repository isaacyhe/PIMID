/* spmv_csr_omp.c — Sparse Matrix-Vector Multiply in CSR format: y = A * x
 * OpenMP parallel version. Generates random sparse matrix with ~1% density. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
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
    int N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    int density_pct = parse_int_arg(argc, argv, "--density", 1);
    uint32_t seed = 42;

    /* Count nonzeros per row first */
    int* row_nnz = (int*)calloc(N, sizeof(int));
    if (!row_nnz) { fprintf(stderr, "malloc failed\n"); return 1; }

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
    float* y = (float*)calloc(N, sizeof(float));
    if (!row_ptr || !col_idx || !values || !x || !y) {
        fprintf(stderr, "malloc failed\n"); return 1;
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

    /* 1.12.18: the thread team starts before the ROI, as message-passing
     * ranks exist before it; the ROI measures the kernel, not the start-up. */
    #pragma omp parallel
    { }
    zsim_roi_begin();
    #pragma omp parallel for
    for (int i = 0; i < N; i++) {
        float sum = 0.0f;
        for (int j = row_ptr[i]; j < row_ptr[i + 1]; j++)
            sum += values[j] * x[col_idx[j]];
        y[i] = sum;
    }
    zsim_roi_end();

    double checksum = 0.0;
    for (int i = 0; i < N; i++) checksum += y[i];
    printf("BENCH_NNZ: %d\n", total_nnz);
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(row_ptr); free(col_idx); free(values);
    free(x); free(y);
    return 0;
}
