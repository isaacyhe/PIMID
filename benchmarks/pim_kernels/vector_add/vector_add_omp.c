/* vector_add_omp.c — Vector Addition: c[i] = a[i] + b[i]
 * OpenMP parallel version. Standalone, no shared headers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include "zsim_hooks.h"
#include "../../include/pimid_spm.h"

#define DEFAULT_SIZE 8192

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* 1.12.18: c[0..n) = a + b through the PE scratchpad (pimid_spm.h). The
 * RAM holds three equal buffers: a tile of a, a tile of b and a tile of c.
 * Each a and b tile is fetched once and each c tile is written back once.
 * With no scratchpad the tile is the whole range and this is the plain
 * element loop. */
static void vector_add_range(pimid_spm_t* s, float* c, const float* a, const float* b,
                             size_t n) {
    const size_t T = pimid_spm_tile(s, 3, sizeof(float));
    const size_t oa = 0, ob = s->cap ? T * sizeof(float) : 0, oc = s->cap ? 2 * T * sizeof(float) : 0;
    for (size_t i0 = 0; i0 < n; i0 += T) {
        const size_t m = (n - i0 < T) ? n - i0 : T;
        const float* ab = (const float*)pimid_spm_in(s, oa, a + i0, m * sizeof(float));
        const float* bb = (const float*)pimid_spm_in(s, ob, b + i0, m * sizeof(float));
        float* cb = (float*)pimid_spm_out(s, oc, c + i0);
        for (size_t i = 0; i < m; i++)
            cb[i] = ab[i] + bb[i];
        pimid_spm_flush(s, c + i0, cb, m * sizeof(float));
    }
}

/* 1.12.18: first-touch placement helper. [*lo, *hi) is thread t's share of
 * [0, n) among nt threads: blk > 0 = blocks of blk (the devorg prep's per-PE
 * ranges [t*blk, (t+1)*blk)), else cap != 0 = the scratchpad path's
 * [n*t/nt, n*(t+1)/nt), else schedule(static) as libgomp splits it (n/nt
 * each, the first n%nt threads one more -- the split reduction_omp.c's
 * scratchpad path already relies on). */
static void ft_share(int n, int t, int nt, int blk, int cap, int* lo, int* hi) {
    long a, b;
    if (blk > 0) {
        a = (long)t * blk; b = a + blk;
    } else if (cap) {
        a = ((long)n * t) / nt; b = ((long)n * (t + 1)) / nt;
    } else {
        const long q = n / nt, r = n % nt;
        a = t * q + (t < r ? t : r); b = a + q + (t < r ? 1 : 0);
    }
    *lo = (int)(a < n ? a : n);
    *hi = (int)(b < n ? b : n);
}

int main(int argc, char* argv[]) {
    int N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);

    float* a = (float*)malloc(N * sizeof(float));
    float* b = (float*)malloc(N * sizeof(float));
    float* c = (float*)malloc(N * sizeof(float));
    if (!a || !b || !c) { fprintf(stderr, "malloc failed\n"); return 1; }

    /* 1.12.18: each thread opens its PE's scratchpad before the ROI. */
    int T = omp_get_max_threads();
    pimid_spm_t* spms = (pimid_spm_t*)calloc((size_t)T, sizeof(pimid_spm_t));
    if (!spms) { fprintf(stderr, "malloc failed\n"); return 1; }
    #pragma omp parallel num_threads(T)
    pimid_spm_open(&spms[omp_get_thread_num()]);

    /* 1.12.18: first-touch placement -- each thread initialises the share it computes on, so its pages live in its PE's memory. */
    /* The split is the ROI's: [N*t/T, N*(t+1)/T) with a scratchpad, else
     * schedule(static) over T threads. c, which the ROI writes, is touched
     * here too (the ROI overwrites every element). */
    #pragma omp parallel num_threads(T)
    {
        int lo, hi;
        ft_share(N, omp_get_thread_num(), omp_get_num_threads(), 0, spms[0].cap != 0, &lo, &hi);
        for (int i = lo; i < hi; i++) {
            a[i] = (float)i;
            b[i] = (float)(N - i);
            c[i] = 0.0f;
        }
    }

    zsim_roi_begin();
    /* 1.12.18: no scratchpad -- the 1.12.17 code, unchanged */
    if (!spms[0].cap) {
        #pragma omp parallel for
        for (int i = 0; i < N; i++)
            c[i] = a[i] + b[i];
    } else {
        #pragma omp parallel num_threads(T)
        {
            const int t = omp_get_thread_num();
            const int i0 = (int)(((long)N * t) / T), i1 = (int)(((long)N * (t + 1)) / T);
            if (i1 > i0)
                vector_add_range(&spms[t], c + i0, a + i0, b + i0, (size_t)(i1 - i0));
        }
    }
    zsim_roi_end();

    #pragma omp parallel num_threads(T)
    pimid_spm_close(&spms[omp_get_thread_num()]);
    free(spms);

    double checksum = 0.0;
    for (int i = 0; i < N; i++) checksum += c[i];
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(a); free(b); free(c);
    return 0;
}
