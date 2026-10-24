/* is.c -- Integer Sort (NPB IS kernel)
 * Bucket sort with NPB-style key generation and partial verification.
 * Memory pattern: random scatter (histogram-like bin counting).
 *
 * Usage: ./is [--size N] [--threads T]
 * Default: 65536 keys, 1 thread */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>
#include "zsim_hooks.h"

#define DEFAULT_SIZE    65536
#define DEFAULT_THREADS 1
#define NUM_BUCKETS     1024
#define MAX_KEY         (NUM_BUCKETS * 64)  /* keys in [0, MAX_KEY) */

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* NPB-style key generation: produces keys clustered around a mean
 * using a simple linear congruential generator + triangular distribution. */
static uint32_t lcg_rand(uint32_t* s) {
    *s = *s * 1103515245u + 12345u;
    return (*s >> 16) & 0x7FFF;
}

static void generate_keys(int* keys, int N, uint32_t seed) {
    /* Generate keys with a distribution that mimics NPB IS:
     * sum of 4 uniform samples scaled to [0, MAX_KEY). */
    for (int i = 0; i < N; i++) {
        uint32_t sum = 0;
        for (int k = 0; k < 4; k++)
            sum += lcg_rand(&seed);
        keys[i] = (int)((sum % (uint32_t)MAX_KEY));
    }
}

int main(int argc, char* argv[]) {
    int N       = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    int threads = parse_int_arg(argc, argv, "--threads", DEFAULT_THREADS);

    if (threads < 1) threads = 1;
    omp_set_num_threads(threads);

    int* keys    = (int*)malloc((size_t)N * sizeof(int));
    int* sorted  = (int*)malloc((size_t)N * sizeof(int));
    int* buckets = (int*)calloc(MAX_KEY, sizeof(int));
    if (!keys || !sorted || !buckets) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    /* Key generation (outside ROI) */
    generate_keys(keys, N, 314159u);

    /* --- ROI: bucket sort --- */
    /* 1.12.18: the thread team starts before the ROI, as message-passing
     * ranks exist before it; the ROI measures the kernel, not the start-up. */
    #pragma omp parallel num_threads(threads)
    { }
    zsim_roi_begin();

    /* Phase 1: count occurrences (histogram scatter) */
    if (threads > 1) {
        /* Each thread builds a private histogram, then merge */
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            int nt  = omp_get_num_threads();
            int* local_hist = (int*)calloc(MAX_KEY, sizeof(int));

            int chunk = (N + nt - 1) / nt;
            int lo = tid * chunk;
            int hi = lo + chunk;
            if (hi > N) hi = N;

            for (int i = lo; i < hi; i++)
                local_hist[keys[i]]++;

            /* Merge into global buckets */
            #pragma omp critical
            {
                for (int j = 0; j < MAX_KEY; j++)
                    buckets[j] += local_hist[j];
            }
            free(local_hist);
        }
    } else {
        for (int i = 0; i < N; i++)
            buckets[keys[i]]++;
    }

    /* Phase 2: prefix sum to get ranks */
    {
        int total = 0;
        for (int i = 0; i < MAX_KEY; i++) {
            int count = buckets[i];
            buckets[i] = total;
            total += count;
        }
    }

    /* Phase 3: scatter keys into sorted order */
    for (int i = 0; i < N; i++) {
        int k = keys[i];
        sorted[buckets[k]] = k;
        buckets[k]++;
    }

    zsim_roi_end();

    /* Partial verification: check sorted order + compute checksum */
    long checksum = 0;
    for (int i = 0; i < N; i++)
        checksum += (long)sorted[i];

    /* Verify sorted order */
    int sorted_ok = 1;
    for (int i = 1; i < N; i++) {
        if (sorted[i] < sorted[i - 1]) {
            sorted_ok = 0;
            break;
        }
    }

    printf("BENCH_SORTED: %s\n", sorted_ok ? "PASS" : "FAIL");
    printf("BENCH_CHECKSUM: %ld\n", checksum);
    printf("BENCH_DONE\n");

    free(keys);
    free(sorted);
    free(buckets);
    return 0;
}
