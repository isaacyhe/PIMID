/* pointer_chase.c -- dependent-load latency probe (serial).
 * A random cyclic permutation over N cache lines (Sattolo's algorithm, seeded,
 * deterministic) is laid out as a linked list, one 64-byte line per node. The
 * ROI follows the chain for --steps loads; every load depends on the previous
 * one, so ROI cycles / steps is the load-to-use latency the core sees (the
 * application view of memory latency). Use a working set well above the LLC
 * (the default 1M lines = 64 MB). Standalone, no shared headers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "zsim_hooks.h"

#define DEFAULT_LINES (1L << 20)
#define DEFAULT_STEPS 20000L
#define DEFAULT_SEED  12345L

struct line { struct line* next; long pad[7]; };   /* one 64-byte cache line */

static long parse_long_arg(int argc, char** argv, const char* flag, long def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atol(argv[i + 1]);
    return def;
}

static uint64_t xorshift64(uint64_t* s) {
    uint64_t x = *s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return *s = x;
}

int main(int argc, char* argv[]) {
    long N = parse_long_arg(argc, argv, "--lines", DEFAULT_LINES);
    long S = parse_long_arg(argc, argv, "--steps", DEFAULT_STEPS);
    uint64_t seed = (uint64_t)parse_long_arg(argc, argv, "--seed", DEFAULT_SEED) | 1;
    if (N < 2 || S < 1) { fprintf(stderr, "need --lines >= 2 and --steps >= 1\n"); return 1; }

    struct line* L = (struct line*)aligned_alloc(64, (size_t)N * sizeof(struct line));
    uint32_t* perm = (uint32_t*)malloc((size_t)N * sizeof(uint32_t));
    if (!L || !perm) { fprintf(stderr, "malloc failed\n"); return 1; }
    /* Sattolo: a uniformly random permutation with exactly one cycle */
    for (long i = 0; i < N; i++) perm[i] = (uint32_t)i;
    for (long i = N - 1; i > 0; i--) {
        long j = (long)(xorshift64(&seed) % (uint64_t)i);
        uint32_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }
    for (long i = 0; i < N; i++) {
        L[perm[i]].next = &L[perm[(i + 1) % N]];
        for (int k = 0; k < 7; k++) L[perm[i]].pad[k] = 0;
    }
    struct line* p = &L[perm[0]];
    free(perm);

    zsim_roi_begin();
    for (long s = 0; s < S; s++)
        p = p->next;
    zsim_roi_end();

    printf("pointer_chase: lines %ld (%ld MB), steps %ld, threads 1\n", N, N * 64 / 1048576, S);
    printf("BENCH_CHECKSUM: %f\n", (double)(p - L));
    printf("BENCH_DONE\n");
    free(L);
    return 0;
}
