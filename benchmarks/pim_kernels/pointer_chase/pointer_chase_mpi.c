/* pointer_chase_mpi.c -- dependent-load latency probe (MPI).
 * Every rank builds the same random cyclic permutation over N cache lines
 * (Sattolo's algorithm, seeded, deterministic) in its own buffer, one 64-byte
 * line per node, starts at position rank*N/size of the cycle and follows its
 * chain for --steps loads. Every load depends on the previous one, so ROI
 * cycles / steps is the load-to-use latency each rank sees (the application
 * view of memory latency). Use a working set well above the LLC (the default
 * 1M lines = 64 MB per rank). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mpi.h>
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
    MPI_Init(&argc, &argv);
    int rank, nprocs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
    long N = parse_long_arg(argc, argv, "--lines", DEFAULT_LINES);
    long S = parse_long_arg(argc, argv, "--steps", DEFAULT_STEPS);
    uint64_t seed = (uint64_t)parse_long_arg(argc, argv, "--seed", DEFAULT_SEED) | 1;
    if (N < 2 || S < 1) {
        if (rank == 0) fprintf(stderr, "need --lines >= 2 and --steps >= 1\n");
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    struct line* L = (struct line*)aligned_alloc(64, (size_t)N * sizeof(struct line));
    uint32_t* perm = (uint32_t*)malloc((size_t)N * sizeof(uint32_t));
    if (!L || !perm) { fprintf(stderr, "rank %d: malloc failed\n", rank); MPI_Abort(MPI_COMM_WORLD, 1); }
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
    struct line* p = &L[perm[(long)rank * N / nprocs]];
    free(perm);

    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) zsim_roi_begin();
    for (long s = 0; s < S; s++)
        p = p->next;
    if (rank == 0) zsim_roi_end();
    MPI_Barrier(MPI_COMM_WORLD);

    double local = (double)(p - L), global = 0.0;
    MPI_Reduce(&local, &global, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    if (rank == 0) {
        printf("pointer_chase: lines %ld (%ld MB) per rank, steps %ld per rank, ranks %d\n", N, N * 64 / 1048576, S, nprocs);
        printf("BENCH_CHECKSUM: %f\n", global);
        printf("BENCH_DONE\n");
    }
    free(L);
    MPI_Finalize();
    return 0;
}
