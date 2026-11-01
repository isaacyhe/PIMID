/* radix.c -- LSD Radix Sort with R=256 (SPLASH-3 style)
 *
 * LSD radix sort: for each 8-bit digit (4 passes for 32-bit keys):
 *   1. Build per-thread local histograms
 *   2. Prefix sum to compute offsets
 *   3. Parallel permute (scatter)
 * Random scatter/gather during permute phase.
 *
 * Compile: g++ -std=c++11 -O2 -Wall -I<hooks-path> -lpthread -lm radix.c -o radix
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include "zsim_hooks.h"

#define DEFAULT_SIZE    65536
#define DEFAULT_THREADS 4
#define RADIX           256
#define NUM_PASSES      4   /* 4 x 8 bits = 32 bits */

/* ------------------------------------------------------------------ */
/*  Arg parser                                                         */
/* ------------------------------------------------------------------ */
static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* ------------------------------------------------------------------ */
/*  Deterministic generator (splitmix64)                               */
/* ------------------------------------------------------------------ */
/* splitmix64 (Steele, Lea and Flood, "Fast splittable pseudorandom number generators", OOPSLA 2014): one 64-bit
 * output per call from a 64-bit counter state, seeded as before. A draw over a range R is the output reduced modulo R;
 * the modulo bias is at most R / 2^64 (below 2^-40 for R < 2^24) and is ignored. */
static uint64_t splitmix64(uint64_t* s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* ------------------------------------------------------------------ */
/*  Shared state                                                       */
/* ------------------------------------------------------------------ */
static uint32_t* keys_src;
static uint32_t* keys_dst;
static int N;
static int num_threads;
static int current_pass;
static int* local_hist;      /* num_threads * RADIX */
static int global_prefix[RADIX];
static int* thread_prefix;   /* num_threads * RADIX */
static pthread_barrier_t barrier;

/* ------------------------------------------------------------------ */
/*  Worker thread                                                      */
/* ------------------------------------------------------------------ */
static void* radix_worker(void* arg) {
    int tid = (int)(intptr_t)arg;
    int chunk = (N + num_threads - 1) / num_threads;
    int lo = tid * chunk;
    int hi = lo + chunk;
    if (hi > N) hi = N;

    for (int pass = 0; pass < NUM_PASSES; pass++) {
        int shift = pass * 8;
        int* my_hist = &local_hist[tid * RADIX];

        /* Phase 1: Local histogram */
        memset(my_hist, 0, RADIX * sizeof(int));
        for (int i = lo; i < hi; i++) {
            int digit = (keys_src[i] >> shift) & 0xFF;
            my_hist[digit]++;
        }

        pthread_barrier_wait(&barrier);

        /* Phase 2: Thread 0 computes global prefix sum */
        if (tid == 0) {
            /* Sum histograms across threads */
            int total[RADIX];
            memset(total, 0, sizeof(total));
            for (int t = 0; t < num_threads; t++)
                for (int d = 0; d < RADIX; d++)
                    total[d] += local_hist[t * RADIX + d];

            /* Exclusive prefix sum */
            int sum = 0;
            for (int d = 0; d < RADIX; d++) {
                global_prefix[d] = sum;
                sum += total[d];
            }

            /* Per-thread prefix: for each digit, each thread's offset
               is global_prefix[d] + sum of hist[0..t-1][d] */
            for (int d = 0; d < RADIX; d++) {
                int off = global_prefix[d];
                for (int t = 0; t < num_threads; t++) {
                    thread_prefix[t * RADIX + d] = off;
                    off += local_hist[t * RADIX + d];
                }
            }
        }

        pthread_barrier_wait(&barrier);

        /* Phase 3: Permute (scatter) */
        int* my_prefix = &thread_prefix[tid * RADIX];
        for (int i = lo; i < hi; i++) {
            int digit = (keys_src[i] >> shift) & 0xFF;
            keys_dst[my_prefix[digit]] = keys_src[i];
            my_prefix[digit]++;
        }

        pthread_barrier_wait(&barrier);

        /* Swap src/dst for next pass */
        if (tid == 0) {
            uint32_t* tmp = keys_src;
            keys_src = keys_dst;
            keys_dst = tmp;
        }

        pthread_barrier_wait(&barrier);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char** argv) {
    N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    num_threads = parse_int_arg(argc, argv, "--threads", DEFAULT_THREADS);

    printf("Radix Sort (LSD, R=256) -- N=%d, threads=%d\n", N, num_threads);

    /* Allocate */
    keys_src = (uint32_t*)malloc((size_t)N * sizeof(uint32_t));
    keys_dst = (uint32_t*)malloc((size_t)N * sizeof(uint32_t));
    local_hist = (int*)malloc((size_t)num_threads * RADIX * sizeof(int));
    thread_prefix = (int*)malloc((size_t)num_threads * RADIX * sizeof(int));
    if (!keys_src || !keys_dst || !local_hist || !thread_prefix) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    /* Initialize with deterministic data */
    uint64_t gs = 42;   /* the generator state, seeded as before */
    for (int i = 0; i < N; i++)
        keys_src[i] = (uint32_t)(splitmix64(&gs) >> 32);

    /* Setup barrier */
    pthread_barrier_init(&barrier, NULL, (unsigned)num_threads);
    pthread_t* threads = (pthread_t*)malloc((size_t)num_threads * sizeof(pthread_t));

    /* ---- ROI begin ---- */
    zsim_roi_begin();

    for (int t = 1; t < num_threads; t++)
        pthread_create(&threads[t], NULL, radix_worker, (void*)(intptr_t)t);
    radix_worker((void*)(intptr_t)0);

    for (int t = 1; t < num_threads; t++)
        pthread_join(threads[t], NULL);

    zsim_roi_end();
    /* ---- ROI end ---- */

    /* After NUM_PASSES (even), result is in keys_src */
    /* Verify sorted */
    int sorted = 1;
    for (int i = 1; i < N; i++) {
        if (keys_src[i] < keys_src[i - 1]) { sorted = 0; break; }
    }
    printf("SORTED: %s\n", sorted ? "yes" : "no");

    /* Checksum */
    uint64_t checksum = 0;
    for (int i = 0; i < N; i++)
        checksum += keys_src[i];

    printf("BENCH_CHECKSUM: %llu\n", (unsigned long long)checksum);
    printf("BENCH_DONE\n");

    pthread_barrier_destroy(&barrier);
    free(threads);
    free(local_hist);
    free(thread_prefix);
    free(keys_src);
    free(keys_dst);
    return 0;
}
