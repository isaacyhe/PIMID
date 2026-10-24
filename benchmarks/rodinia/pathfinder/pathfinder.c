/* pathfinder.c -- Dynamic programming on 2D grid: minimum cost path top-to-bottom
 * OpenMP parallel version. Grid is rows*cols, finds min-cost path from
 * first row to last row (Rodinia pathfinder). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include "zsim_hooks.h"

#define DEFAULT_ROWS 100
#define DEFAULT_COLS 1000

static uint32_t bench_rand(uint32_t* s) {
    *s = *s * 1103515245 + 12345;
    return (*s >> 16) & 0x7FFF;
}

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

static inline int min3(int a, int b, int c) {
    int m = a;
    if (b < m) m = b;
    if (c < m) m = c;
    return m;
}

int main(int argc, char* argv[]) {
    int rows = parse_int_arg(argc, argv, "--rows", DEFAULT_ROWS);
    int cols = parse_int_arg(argc, argv, "--cols", DEFAULT_COLS);

    size_t ncells = (size_t)rows * cols;
    int* grid = (int*)malloc(ncells * sizeof(int));
    int* dp_prev = (int*)malloc(cols * sizeof(int));
    int* dp_curr = (int*)malloc(cols * sizeof(int));
    if (!grid || !dp_prev || !dp_curr) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    /* Initialize grid with LCG random values 0..9 */
    uint32_t seed = 42;
    for (size_t i = 0; i < ncells; i++)
        grid[i] = bench_rand(&seed) % 10;

    /* First row of DP = first row of grid */
    for (int j = 0; j < cols; j++)
        dp_prev[j] = grid[j];

    /* 1.12.18: the thread team starts before the ROI, as message-passing
     * ranks exist before it; the ROI measures the kernel, not the start-up. */
    #pragma omp parallel
    { }
    zsim_roi_begin();

    /* Process row by row */
    for (int i = 1; i < rows; i++) {
        #pragma omp parallel for
        for (int j = 0; j < cols; j++) {
            int up    = dp_prev[j];
            int left  = (j > 0)        ? dp_prev[j - 1] : up;
            int right = (j < cols - 1) ? dp_prev[j + 1] : up;

            dp_curr[j] = grid[i * cols + j] + min3(left, up, right);
        }
        /* Swap buffers */
        int* s = dp_prev;
        dp_prev = dp_curr;
        dp_curr = s;
    }

    zsim_roi_end();

    /* Checksum: minimum value in bottom row */
    int min_val = dp_prev[0];
    for (int j = 1; j < cols; j++)
        if (dp_prev[j] < min_val) min_val = dp_prev[j];

    printf("BENCH_CHECKSUM: %d\n", min_val);
    printf("BENCH_DONE\n");

    free(grid);
    free(dp_prev);
    free(dp_curr);
    return 0;
}
