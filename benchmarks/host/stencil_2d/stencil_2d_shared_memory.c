/* stencil_2d_omp.c — 2D 5-point Jacobi stencil (nearest-neighbor average)
 * OpenMP parallel version. Grid is N*N, runs for iter iterations. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include "zsim_hooks.h"

#define DEFAULT_SIZE 64
#define DEFAULT_ITERS 5

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

int main(int argc, char* argv[]) {
    int N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    int iters = parse_int_arg(argc, argv, "--iters", DEFAULT_ITERS);

    float* grid = (float*)calloc((size_t)N * N, sizeof(float));
    float* tmp = (float*)calloc((size_t)N * N, sizeof(float));
    if (!grid || !tmp) { fprintf(stderr, "malloc failed\n"); return 1; }

    /* Initialize: hot boundary on top row */
    for (int j = 0; j < N; j++) grid[j] = 100.0f;

    /* 1.12.18: the thread team starts before the ROI, as message-passing
     * ranks exist before it; the ROI measures the kernel, not the start-up. */
    #pragma omp parallel
    { }
    zsim_roi_begin();
    for (int t = 0; t < iters; t++) {
        /* Preserve Dirichlet boundary rows */
        memcpy(&tmp[0], &grid[0], N * sizeof(float));
        memcpy(&tmp[(N - 1) * N], &grid[(N - 1) * N], N * sizeof(float));
        #pragma omp parallel for
        for (int i = 1; i < N - 1; i++) {
            tmp[i * N + 0] = grid[i * N + 0];
            tmp[i * N + (N - 1)] = grid[i * N + (N - 1)];
            for (int j = 1; j < N - 1; j++) {
                tmp[i * N + j] = 0.25f * (
                    grid[(i - 1) * N + j] + grid[(i + 1) * N + j] +
                    grid[i * N + (j - 1)] + grid[i * N + (j + 1)]);
            }
        }
        /* Swap */
        float* s = grid; grid = tmp; tmp = s;
    }
    zsim_roi_end();

    double checksum = 0.0;
    for (int i = 0; i < N * N; i++) checksum += grid[i];
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(grid); free(tmp);
    return 0;
}
