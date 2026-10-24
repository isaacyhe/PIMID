/* gemv_omp.c — General Matrix-Vector Multiply: y = A * x
 * OpenMP parallel version. A is N*N, x and y are N-vectors. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include "zsim_hooks.h"

#define DEFAULT_SIZE 256

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

int main(int argc, char* argv[]) {
    int N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);

    float* A = (float*)malloc((size_t)N * N * sizeof(float));
    float* x = (float*)malloc(N * sizeof(float));
    float* y = (float*)calloc(N, sizeof(float));
    if (!A || !x || !y) { fprintf(stderr, "malloc failed\n"); return 1; }

    for (int i = 0; i < N; i++) {
        x[i] = 1.0f;
        for (int j = 0; j < N; j++)
            A[i * N + j] = (float)((i + j) % 7);
    }

    /* 1.12.18: the thread team starts before the ROI, as message-passing
     * ranks exist before it; the ROI measures the kernel, not the start-up. */
    #pragma omp parallel
    { }
    zsim_roi_begin();
    #pragma omp parallel for
    for (int i = 0; i < N; i++) {
        float sum = 0.0f;
        for (int j = 0; j < N; j++)
            sum += A[i * N + j] * x[j];
        y[i] = sum;
    }
    zsim_roi_end();

    double checksum = 0.0;
    for (int i = 0; i < N; i++) checksum += y[i];
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(A); free(x); free(y);
    return 0;
}
