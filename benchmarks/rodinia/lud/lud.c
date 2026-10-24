/* lud.c -- LU Decomposition (in-place, no pivoting)
 * Doolittle algorithm with OpenMP parallel submatrix update.
 * Decomposes an N*N matrix A into L*U in place. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>
#include "zsim_hooks.h"

#define DEFAULT_SIZE 256

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

    double* A = (double*)malloc((size_t)N * N * sizeof(double));
    if (!A) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    /* Initialize matrix from LCG (values 1.0-10.0), add N to diagonal for stability */
    uint32_t seed = 42;
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            double val = 1.0 + 9.0 * (bench_rand(&seed) / 32767.0);
            if (i == j) val += (double)N;
            A[i * N + j] = val;
        }
    }

    /* 1.12.18: the thread team starts before the ROI, as message-passing
     * ranks exist before it; the ROI measures the kernel, not the start-up. */
    #pragma omp parallel
    { }
    zsim_roi_begin();

    /* Doolittle LU decomposition in place */
    for (int k = 0; k < N; k++) {
        /* Update row k: divide by pivot */
        double pivot = A[k * N + k];
        for (int j = k + 1; j < N; j++) {
            A[k * N + j] /= pivot;
        }

        /* Update submatrix A[i][j] for i,j > k */
        #pragma omp parallel for
        for (int i = k + 1; i < N; i++) {
            double factor = A[i * N + k];
            for (int j = k + 1; j < N; j++) {
                A[i * N + j] -= factor * A[k * N + j];
            }
        }
    }

    zsim_roi_end();

    /* Checksum: sum of diagonal elements */
    double checksum = 0.0;
    for (int i = 0; i < N; i++) {
        checksum += A[i * N + i];
    }
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(A);
    return 0;
}
