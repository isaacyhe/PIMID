/*
 * Naive Matrix Multiply -- C = A * B, triple-loop i-j-k
 * Single-threaded, no blocking, no optimisation.
 *
 * Compile: g++ -O2 -I<path-to-zsim/misc/hooks> naive_matmul.c -o naive_matmul
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zsim_hooks.h"

/* ------------------------------------------------------------------ */
/*  Arg parser                                                        */
/* ------------------------------------------------------------------ */
static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* ------------------------------------------------------------------ */
/*  LCG                                                               */
/* ------------------------------------------------------------------ */
static uint32_t bench_rand(uint32_t* s) {
    *s = *s * 1103515245 + 12345;
    return (*s >> 16) & 0x7FFF;
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */
int main(int argc, char** argv) {
    int N = parse_int_arg(argc, argv, "--size", 128);

    printf("Naive MatMul (i-j-k) -- size=%dx%d\n", N, N);

    size_t mat_bytes = (size_t)N * (size_t)N * sizeof(float);

    float* A = (float*)malloc(mat_bytes);
    float* B = (float*)malloc(mat_bytes);
    float* C = (float*)calloc((size_t)N * (size_t)N, sizeof(float));

    if (!A || !B || !C) { fprintf(stderr, "malloc failed\n"); return 1; }

    /* Init A and B from LCG: values in [0.0, 1.0) */
    uint32_t seed = 42;
    for (int i = 0; i < N * N; i++)
        A[i] = (float)bench_rand(&seed) / 32768.0f;
    for (int i = 0; i < N * N; i++)
        B[i] = (float)bench_rand(&seed) / 32768.0f;

    /* ---- ROI begin ---- */
    zsim_roi_begin();

    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            float sum = 0.0f;
            for (int k = 0; k < N; k++) {
                sum += A[i * N + k] * B[k * N + j];
            }
            C[i * N + j] = sum;
        }
    }

    zsim_roi_end();
    /* ---- ROI end ---- */

    /* Checksum: sum of all elements of C */
    double checksum = 0.0;
    for (int i = 0; i < N * N; i++)
        checksum += (double)C[i];

    printf("BENCH_CHECKSUM: %.6f\n", checksum);
    printf("BENCH_DONE\n");

    free(C);
    free(B);
    free(A);
    return 0;
}
