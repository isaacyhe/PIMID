/* reduction_omp.c — Streaming sum reduction: result = sum(a[i])
 * OpenMP parallel version. Uses reduction clause. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include "zsim_hooks.h"
#include "../../include/pimid_spm.h"

#define DEFAULT_SIZE 16384

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* 1.12.18: sum(a[0..n)) through the PE scratchpad (pimid_spm.h). The RAM
 * holds one buffer: a tile of a, each fetched once. The one running sum adds
 * the elements in index order across tiles (no per-tile partial sums), so the
 * result is bit-identical to the plain loop. With no scratchpad the tile is
 * the whole range and this is the plain loop. */
static double reduction_sum(pimid_spm_t* s, const float* a, size_t n) {
    const size_t T = pimid_spm_tile(s, 1, sizeof(float));
    double sum = 0.0;
    for (size_t i0 = 0; i0 < n; i0 += T) {
        const size_t m = (n - i0 < T) ? n - i0 : T;
        const float* ab = (const float*)pimid_spm_in(s, 0, a + i0, m * sizeof(float));
        for (size_t i = 0; i < m; i++)
            sum += ab[i];
    }
    return sum;
}

int main(int argc, char* argv[]) {
    int N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);

    float* a = (float*)malloc(N * sizeof(float));
    if (!a) { fprintf(stderr, "malloc failed\n"); return 1; }

    /* 1.12.18: each thread opens its PE's scratchpad before the ROI. */
    int T = omp_get_max_threads();
    pimid_spm_t* spms = (pimid_spm_t*)calloc((size_t)T, sizeof(pimid_spm_t));
    if (!spms) { fprintf(stderr, "malloc failed\n"); return 1; }
    #pragma omp parallel num_threads(T)
    pimid_spm_open(&spms[omp_get_thread_num()]);

    /* 1.12.18: first-touch placement -- each thread initialises the share it computes on, so its pages live in its PE's memory. */
    /* Both ROI paths split [0, N) as schedule(static) over T threads (the
     * scratchpad path's i0/i1 is that split). */
    #pragma omp parallel for schedule(static) num_threads(T)
    for (int i = 0; i < N; i++)
        a[i] = (float)(i % 100) * 0.01f;

    zsim_roi_begin();
    double sum = 0.0;
    /* 1.12.18: no scratchpad -- the 1.12.17 code, unchanged */
    if (!spms[0].cap) {
        #pragma omp parallel for reduction(+:sum)
        for (int i = 0; i < N; i++)
            sum += a[i];
    } else {
        /* Each thread sums the same contiguous range schedule(static) gave it, so
         * the per-thread partial sums are those of the reduction clause before. */
        #pragma omp parallel num_threads(T) reduction(+:sum)
        {
            const int t = omp_get_thread_num();
            const int q = N / T, r = N % T;
            const int i0 = t * q + (t < r ? t : r), i1 = i0 + q + (t < r ? 1 : 0);
            if (i1 > i0)
                sum += reduction_sum(&spms[t], a + i0, (size_t)(i1 - i0));
        }
    }
    zsim_roi_end();

    #pragma omp parallel num_threads(T)
    pimid_spm_close(&spms[omp_get_thread_num()]);
    free(spms);

    printf("BENCH_CHECKSUM: %f\n", sum);
    printf("BENCH_DONE\n");

    free(a);
    return 0;
}
