/* bfs_message_passing.c -- Breadth-First Search on a random graph (CSR), message passing, partitioned graph.
 * 1D vertex partitioning: with B = ceil(V / P), rank r owns the contiguous vertex block [r*B, min(V, (r+1)*B)) and
 * holds only the CSR rows and dist entries of its own vertices, so a rank's memory and work are proportional to
 * V / P. The graph is the one the serial and shared-memory versions build: edge (i, d) is draw number i*deg + d of the
 * splitmix64 stream seeded 42, and a rank starts the stream at its first row, so the graph does not depend on the
 * number of ranks. Level-synchronous search: in each level a rank expands its frontier; a neighbour in its own block
 * it marks at once, every other neighbour it sends to the neighbour's owner (the per-owner counts by MPI_Alltoall, the
 * vertices by one MPI_Sendrecv per rank pair). The owner keeps the received vertices it has not visited yet, and an
 * MPI_Allreduce of the frontier sizes ends the search. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mpi.h>
#include "zsim_hooks.h"

#define DEFAULT_VERTICES 512
#define DEFAULT_DEGREE   8
#define SM64_GAMMA 0x9E3779B97F4A7C15ull

/* The owner of vertex v is v / B, computed without a division. With M = floor((2^64 - 1) / B) + 1 (B >= 2),
 * M * B = 2^64 + e with 0 <= e < B, and the high 64 bits of v * M are floor(v / B + v * e / (B * 2^64)); that is
 * floor(v / B) whenever v * e < 2^64, so it is exact for every v < 2^31 (with B < 2^31). The vertex count is checked
 * against that bound at start (BFS_VERTICES_MAX). B = 1 (V <= P): the owner is v itself (M = 0 marks it). */
#define BFS_VERTICES_MAX 2147483647LL   /* vertex ids 0 .. V - 1 below 2^31 */
static uint64_t bfs_owner_mult(long b) { return b < 2 ? 0 : ~(uint64_t)0 / (uint64_t)b + 1; }
static inline int bfs_owner(int v, uint64_t m) {
    return m ? (int)(((unsigned __int128)(uint32_t)v * m) >> 64) : v;
}

/* splitmix64 (Steele, Lea and Flood, "Fast splittable pseudorandom number generators", OOPSLA 2014): one 64-bit
 * output per call from a 64-bit counter state, seeded as before. A draw over a range R is the output reduced modulo R;
 * the modulo bias is at most R / 2^64 (below 2^-40 for R < 2^24) and is ignored. */
static uint64_t splitmix64(uint64_t* s) {
    uint64_t z = (*s += SM64_GAMMA);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

static long long parse_ll_arg(int argc, char** argv, const char* flag, long long def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return strtoll(argv[i + 1], NULL, 10);
    return def;
}

int main(int argc, char* argv[]) {
    MPI_Init(&argc, &argv);

    int rank, nprocs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    /* The owner computation (bfs_owner) is exact for vertex ids below 2^31: a vertex count outside 1 .. 2^31 - 1 is
     * refused rather than run with wrong owners. */
    const long long Vreq = parse_ll_arg(argc, argv, "--vertices", DEFAULT_VERTICES);
    if (Vreq < 1 || Vreq > BFS_VERTICES_MAX) {
        if (rank == 0)
            fprintf(stderr, "bfs: --vertices %lld refused: a vertex's owner rank is computed by an exact reciprocal "
                    "multiply, exact for vertex ids below 2^31; the vertex count must be 1 .. %lld\n",
                    Vreq, BFS_VERTICES_MAX);
        fflush(stderr);
        MPI_Barrier(MPI_COMM_WORLD);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    int V = (int)Vreq;
    int deg = parse_int_arg(argc, argv, "--degree", DEFAULT_DEGREE);
    int source = parse_int_arg(argc, argv, "--source", 0);

    /* This rank's block of the graph: rows [lo, hi). */
    const uint32_t seed = 42;
    const long B = ((long)V + nprocs - 1) / nprocs;
    const uint64_t BM = bfs_owner_mult(B);   /* owner of v: bfs_owner(v, BM) = v / B */
    const long lo = ((long)rank * B < V) ? (long)rank * B : V;
    const long hi = (lo + B < V) ? lo + B : V;
    const int nloc = (int)(hi - lo);
    const size_t nedge = (size_t)nloc * (size_t)deg;

    int* row_ptr = (int*)malloc((size_t)(nloc + 1) * sizeof(int));
    int* col_idx = (int*)malloc((nedge + 1) * sizeof(int));
    int* counts = (int*)malloc(6 * (size_t)nprocs * sizeof(int));
    if (!row_ptr || !col_idx || !counts) {
        fprintf(stderr, "rank %d: malloc failed\n", rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    int *sc = counts, *rc = counts + nprocs, *so = counts + 2 * nprocs, *ro = counts + 3 * nprocs,
        *ot = counts + 4 * nprocs, *it = counts + 5 * nprocs;
    for (int i = 0; i <= nloc; i++) row_ptr[i] = i * deg;
    uint64_t gs = (uint64_t)seed + (uint64_t)lo * (uint64_t)deg * SM64_GAMMA;   /* the stream at row lo */
    for (size_t e = 0; e < nedge; e++)
        col_idx[e] = (int)(splitmix64(&gs) % (uint64_t)V);

    /* Edges from this block to each owner and (by one exchange) into this block: they size the send buckets and the
     * receive buffer, which no level can exceed. */
    for (int p = 0; p < nprocs; p++) ot[p] = 0;
    for (size_t e = 0; e < nedge; e++) ot[bfs_owner(col_idx[e], BM)]++;
    MPI_Alltoall(ot, 1, MPI_INT, it, 1, MPI_INT, MPI_COMM_WORLD);
    long in_edges = 0;
    for (int p = 0; p < nprocs; p++) in_edges += it[p];
    so[0] = 0;
    for (int p = 1; p < nprocs; p++) so[p] = so[p - 1] + ot[p - 1];

    int* dist = (int*)malloc((size_t)(nloc + 1) * sizeof(int));
    int* frontier = (int*)malloc((size_t)(nloc + 1) * sizeof(int));
    int* next = (int*)malloc((size_t)(nloc + 1) * sizeof(int));
    int* sendbuf = (int*)malloc((nedge + 1) * sizeof(int));
    int* recvbuf = (int*)malloc(((size_t)in_edges + 1) * sizeof(int));
    if (!dist || !frontier || !next || !sendbuf || !recvbuf) {
        fprintf(stderr, "rank %d: malloc failed\n", rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    for (int i = 0; i < nloc; i++) dist[i] = -1;
    int fsize = 0;
    if (source >= lo && source < hi) {
        dist[source - lo] = 0;
        frontier[0] = source;
        fsize = 1;
    }

    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) zsim_roi_begin();

    int level = 0;
    int gsize = 1;   /* the frontier, summed over the ranks: the source */
    while (gsize > 0) {
        level++;

        /* Expand: a neighbour in this rank's block is marked here; every other one goes to its owner's bucket */
        for (int p = 0; p < nprocs; p++) sc[p] = 0;
        int n = 0;
        for (int f = 0; f < fsize; f++) {
            const int u = (int)(frontier[f] - lo);
            for (int e = row_ptr[u]; e < row_ptr[u + 1]; e++) {
                const int v = col_idx[e];
                const int p = bfs_owner(v, BM);
                if (p == rank) {
                    if (dist[v - lo] == -1) { dist[v - lo] = level; next[n++] = v; }
                } else {
                    sendbuf[so[p] + sc[p]++] = v;
                }
            }
        }

        /* Route: the counts, then the vertices, to their owners */
        MPI_Alltoall(sc, 1, MPI_INT, rc, 1, MPI_INT, MPI_COMM_WORLD);
        ro[0] = 0;
        for (int p = 1; p < nprocs; p++) ro[p] = ro[p - 1] + rc[p - 1];
        const int nrecv = ro[nprocs - 1] + rc[nprocs - 1];
        for (int i = 1; i < nprocs; i++) {
            const int dst = (rank + i) % nprocs, src = (rank - i + nprocs) % nprocs;
            MPI_Sendrecv(sendbuf + so[dst], sc[dst], MPI_INT, dst, 0,
                         recvbuf + ro[src], rc[src], MPI_INT, src, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        }

        /* Keep the received vertices not visited yet: the rest of the next frontier */
        for (int i = 0; i < nrecv; i++) {
            const int v = recvbuf[i];
            if (dist[v - lo] == -1) {
                dist[v - lo] = level;
                next[n++] = v;
            }
        }
        int* t = frontier; frontier = next; next = t;
        fsize = n;
        MPI_Allreduce(&fsize, &gsize, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    }

    if (rank == 0) zsim_roi_end();
    /* The closing barrier every MPI kernel has: the device-scope ROI ends at the last rank's arrival. */
    MPI_Barrier(MPI_COMM_WORLD);

    long visited_local = 0, visited = 0;
    for (int i = 0; i < nloc; i++)
        if (dist[i] >= 0) visited_local++;
    MPI_Reduce(&visited_local, &visited, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_WORLD);

    if (rank == 0) {
        printf("BENCH_LEVELS: %d\n", level);
        printf("BENCH_VISITED: %ld\n", visited);
        printf("BENCH_CHECKSUM: %ld\n", visited);
        printf("BENCH_DONE\n");
    }

    free(row_ptr); free(col_idx); free(counts);
    free(dist); free(frontier); free(next); free(sendbuf); free(recvbuf);
    MPI_Finalize();
    return 0;
}
