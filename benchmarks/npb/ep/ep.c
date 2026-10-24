/* ep.c -- Embarrassingly Parallel (NPB EP kernel)
 * Marsaglia polar method for generating Gaussian random pairs,
 * tallied into 10 concentric annular bins by distance from origin.
 * Memory pattern: pure compute, minimal memory (register-heavy).
 *
 * Usage: ./ep [--size N] [--threads T]
 * Default: 1024 pairs, 1 thread */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>
#include "zsim_hooks.h"

#define DEFAULT_SIZE    1024
#define DEFAULT_THREADS 1
#define NUM_BINS        10

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* Simple 64-bit LCG for reproducible uniform [0,1) doubles.
 * Each thread gets its own state via seed + thread_id offset. */
static double uniform_rand(uint64_t* state) {
    *state = *state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)(*state >> 11) / (double)(1ULL << 53);
}

int main(int argc, char* argv[]) {
    int N       = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    int threads = parse_int_arg(argc, argv, "--threads", DEFAULT_THREADS);

    if (threads < 1) threads = 1;
    omp_set_num_threads(threads);

    /* Global counters: q[k] = count of pairs with k <= l < k+1 (l = sqrt(x1^2+x2^2))
     * plus sx = sum of all x1's, sy = sum of all x2's (NPB-style sums) */
    long* q_global = (long*)calloc(NUM_BINS, sizeof(long));
    double sx_global = 0.0, sy_global = 0.0;
    long accepted_global = 0;

    if (!q_global) { fprintf(stderr, "malloc failed\n"); return 1; }

    /* --- ROI: Gaussian pair generation + binning --- */
    /* 1.12.18: the thread team starts before the ROI, as message-passing
     * ranks exist before it; the ROI measures the kernel, not the start-up. */
    #pragma omp parallel num_threads(threads)
    { }
    zsim_roi_begin();

    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        int nt  = omp_get_num_threads();
        int chunk = (N + nt - 1) / nt;
        int lo = tid * chunk;
        int hi = lo + chunk;
        if (hi > N) hi = N;

        uint64_t rng_state = 314159265ULL + (uint64_t)tid * 1000000007ULL;
        long q_local[NUM_BINS];
        double sx_local = 0.0, sy_local = 0.0;
        long accepted_local = 0;
        int k;

        for (k = 0; k < NUM_BINS; k++) q_local[k] = 0;

        for (int i = lo; i < hi; i++) {
            double x1, x2, t;

            /* Marsaglia polar method: reject pairs outside unit circle */
            do {
                x1 = 2.0 * uniform_rand(&rng_state) - 1.0;
                x2 = 2.0 * uniform_rand(&rng_state) - 1.0;
                t = x1 * x1 + x2 * x2;
            } while (t >= 1.0 || t == 0.0);

            /* Transform to Gaussian */
            t = sqrt(-2.0 * log(t) / t);
            x1 = x1 * t;
            x2 = x2 * t;

            /* Accumulate sums */
            sx_local += x1;
            sy_local += x2;
            accepted_local++;

            /* Bin by Euclidean distance from origin */
            {
                double l = sqrt(x1 * x1 + x2 * x2);
                k = (int)l;
                if (k >= 0 && k < NUM_BINS)
                    q_local[k]++;
            }
        }

        /* Merge into global */
        #pragma omp critical
        {
            for (k = 0; k < NUM_BINS; k++)
                q_global[k] += q_local[k];
            sx_global += sx_local;
            sy_global += sy_local;
            accepted_global += accepted_local;
        }
    }

    zsim_roi_end();

    /* Print bin counts (NPB-style output) */
    printf("BENCH_PAIRS: %ld\n", accepted_global);
    printf("BENCH_SX: %.6f\n", sx_global);
    printf("BENCH_SY: %.6f\n", sy_global);
    for (int k = 0; k < NUM_BINS; k++)
        printf("  q[%d] = %ld\n", k, q_global[k]);

    /* Checksum: sum of all bin counts + hash of sx, sy */
    {
        long total = 0;
        for (int k = 0; k < NUM_BINS; k++) total += q_global[k];
        /* Mix in floating-point sums for a more distinctive checksum */
        long cs = total + (long)(sx_global * 1000.0) + (long)(sy_global * 1000.0);
        printf("BENCH_CHECKSUM: %ld\n", cs);
    }
    printf("BENCH_DONE\n");

    free(q_global);
    return 0;
}
