/* bfs_mpi.c -- Breadth-First Search on a random graph (CSR), message passing, partitioned graph.
 * 1D vertex partitioning: with B = ceil(V / P), rank r owns the contiguous vertex block [r*B, min(V, (r+1)*B)). It
 * holds the CSR rows of its own vertices and their dist entries, and nothing of the other blocks, so a rank's memory
 * and work are proportional to V / P. The graph is the one the serial and OpenMP versions build: edge (i, d) is draw
 * number i*deg + d of the splitmix64 stream seeded 42, and a rank starts the stream at its first row (the generator
 * is a counter, so draw k has state seed + k * gamma), so the graph does not depend on the number of ranks.
 * Level-synchronous search: in each level a rank expands its frontier (its vertices found in the previous level). A
 * neighbour in its own block it marks at once (dist test-and-set, next frontier); every other neighbour it sends to
 * the neighbour's owner -- the per-owner counts by one MPI_Alltoall, the vertices by one MPI_Sendrecv per rank pair.
 * The owner keeps the received vertices it has not visited yet, and an MPI_Allreduce of the frontier sizes ends the
 * search.
 *
 * Device-organization aware (PIMID): a rank's rows, dist, frontiers and send and receive buffers go in its PE slot
 * (rank r owns PE r's slot; ranks <= PEs). With a PE scratchpad (pimid_spm.h) the frontier, the adjacency rows and
 * the received vertices are read a tile at a time and the next frontier is written a tile at a time; the appends to
 * the per-owner send buckets and the dist test-and-set are irregular and stay direct accesses. Coarse placement runs
 * the host-layout path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include "zsim_hooks.h"
#include "../../include/pimid_devorg.h"
#include "../../include/pimid_spm.h"

#define DEFAULT_VERTICES 512
#define DEFAULT_DEGREE   8
#define SM64_GAMMA 0x9E3779B97F4A7C15ull

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

static long bfs_block(long v, int nprocs) { return (v + nprocs - 1) / nprocs; }

/* The owner of vertex v is v / B, computed without a division. With M = floor((2^64 - 1) / B) + 1 (B >= 2),
 * M * B = 2^64 + e with 0 <= e < B, and the high 64 bits of v * M are floor(v / B + v * e / (B * 2^64)); that is
 * floor(v / B) whenever v * e < 2^64, so it is exact for every v < 2^31 (with B < 2^31). The vertex count is checked
 * against that bound at start (BFS_VERTICES_MAX). B = 1 (V <= P): the owner is v itself (M = 0 marks it). */
#define BFS_VERTICES_MAX 2147483647LL   /* vertex ids 0 .. V - 1 below 2^31 */
static uint64_t bfs_owner_mult(long b) { return b < 2 ? 0 : ~(uint64_t)0 / (uint64_t)b + 1; }
static inline int bfs_owner(int v, uint64_t m) {
    return m ? (int)(((unsigned __int128)(uint32_t)v * m) >> 64) : v;
}

/* A rank's slot need, in ints: its rows [B+1], its columns [B*deg], its dist [B], two frontiers [B each], the send
 * buckets [B*deg], the receive buffer [the edges into its block; B*deg, their mean, when not known] and five
 * per-rank count arrays [n each]. (The own block's edges never use the buckets or the receive buffer; they are
 * counted for a simple, safe bound.) */
typedef struct { int deg, nprocs; long in_edges; } bfs_mpi_geom_t;
static size_t bfs_mpi_need(long v, const void* c) {
    const bfs_mpi_geom_t* g = (const bfs_mpi_geom_t*)c;
    const size_t b = (size_t)bfs_block(v, g->nprocs), d = (size_t)g->deg;
    const size_t in = g->in_edges >= 0 ? (size_t)g->in_edges : b * d;
    return ((b + 1) + b * d + 3 * b + b * d + in + 5 * (size_t)g->nprocs) * sizeof(int);
}

/* Expand frontier[0..nf) (global ids, all in [lo, lo+nloc)): a neighbour in this rank's block (owner me) not yet
 * visited gets dist = level and goes to the next frontier out[]; every other neighbour is appended to its owner's
 * bucket, sb[so[p] + sc[p]++] (p = bfs_owner(v, bm)). With a scratchpad the RAM holds two equal buffers, a tile of
 * the frontier and a tile of one adjacency row; each is fetched once. Returns the number of vertices appended to
 * out. */
static size_t bfs_route(pimid_spm_t* s0, const int* rp, const int* ci, int* dist, const int* frontier, size_t nf,
                        long lo, uint64_t bm, int me, int level, int* out, int* sb, const int* so, int* sc) {
    pimid_spm_t sl = *s0, *s = &sl;   /* a local copy of the handle */
    const size_t T = pimid_spm_tile(s, 2, sizeof(int));
    const size_t of = 0, oa = s->cap ? T * sizeof(int) : 0;
    size_t nout = 0;
    for (size_t fb = 0; fb < nf; fb += T) {
        const size_t nb = (nf - fb < T) ? nf - fb : T;
        const int* fl = (const int*)pimid_spm_in(s, of, frontier + fb, nb * sizeof(int));
        for (size_t f = 0; f < nb; f++) {
            const int u = (int)(fl[f] - lo);
            const int e0 = rp[u], ne = rp[u + 1] - e0;
            for (int eb = 0; eb < ne; eb += (int)T) {
                const int na = (ne - eb < (int)T) ? ne - eb : (int)T;
                const int* ab = (const int*)pimid_spm_in(s, oa, ci + e0 + eb, (size_t)na * sizeof(int));
                for (int e = 0; e < na; e++) {
                    const int v = ab[e];
                    const int p = bfs_owner(v, bm);
                    if (p == me) {
                        if (dist[v - lo] == -1) { dist[v - lo] = level; out[nout++] = v; }
                    } else {
                        sb[so[p] + sc[p]++] = v;
                    }
                }
            }
        }
    }
    return nout;
}

/* Keep the received vertices rb[0..n) not yet visited: dist[v - lo] = level and append v to out. With a scratchpad
 * the RAM holds a tile of the received vertices (each fetched once) and a tile of the next frontier (written back a
 * full tile at a time and at the end). Returns the next frontier's size. */
static size_t bfs_keep(pimid_spm_t* s0, int* dist, long lo, const int* rb, size_t n, int* out, int level) {
    pimid_spm_t sl = *s0, *s = &sl;   /* a local copy of the handle */
    const size_t T = pimid_spm_tile(s, 2, sizeof(int));
    const size_t oc = 0, oo = s->cap ? T * sizeof(int) : 0;
    size_t nout = 0, no = 0;
    int* ob = (int*)pimid_spm_out(s, oo, out);
    for (size_t cb = 0; cb < n; cb += T) {
        const size_t nc = (n - cb < T) ? n - cb : T;
        const int* cl = (const int*)pimid_spm_in(s, oc, rb + cb, nc * sizeof(int));
        for (size_t i = 0; i < nc; i++) {
            const int v = cl[i];
            if (dist[v - lo] == -1) {
                dist[v - lo] = level;
                ob[no++] = v;
                if (no == T) {   /* out tile full (only with a scratchpad) */
                    pimid_spm_flush(s, out + nout, ob, no * sizeof(int));
                    nout += no; no = 0;
                    ob = (int*)pimid_spm_out(s, oo, out + nout);
                }
            }
        }
    }
    pimid_spm_flush(s, out + nout, ob, no * sizeof(int));
    return nout + no;
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

    /* Device organization + D1 identity-mapping guard: ranks must be <= num_pes. */
    pimid_devorg_t dev = pimid_devorg_from_args(argc, argv);
    if (nprocs > dev.num_pes) {
        if (rank == 0)
            fprintf(stderr, "pimid_devorg: MPI world size (%d) != num_pes (%d); "
                    "identity mapping requires ranks <= PEs (rank r -> slot r). Aborting.\n",
                    nprocs, dev.num_pes);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    /* This rank's block of the graph: rows [lo, hi). */
    const uint32_t seed = 42;
    const long B = bfs_block(V, nprocs);
    const uint64_t BM = bfs_owner_mult(B);   /* owner of v: bfs_owner(v, BM) = v / B */
    const long lo = ((long)rank * B < V) ? (long)rank * B : V;
    const long hi = (lo + B < V) ? lo + B : V;
    const int nloc = (int)(hi - lo);
    const size_t nedge = (size_t)nloc * (size_t)deg;

    int* row_ptr = (int*)malloc((size_t)(nloc + 1) * sizeof(int));
    int* col_idx = (int*)malloc((nedge + 1) * sizeof(int));
    int* outtot = (int*)malloc((size_t)nprocs * sizeof(int));
    int* intot = (int*)malloc((size_t)nprocs * sizeof(int));
    if (!row_ptr || !col_idx || !outtot || !intot) {
        fprintf(stderr, "rank %d: malloc failed\n", rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    for (int i = 0; i <= nloc; i++) row_ptr[i] = i * deg;
    uint64_t gs = (uint64_t)seed + (uint64_t)lo * (uint64_t)deg * SM64_GAMMA;   /* the stream at row lo */
    for (size_t e = 0; e < nedge; e++)
        col_idx[e] = (int)(splitmix64(&gs) % (uint64_t)V);

    /* Edges from this block to each owner, and (by one exchange) into this block from each rank: they size the
     * send buckets and the receive buffer, which no level can exceed. */
    for (int p = 0; p < nprocs; p++) outtot[p] = 0;
    for (size_t e = 0; e < nedge; e++) outtot[bfs_owner(col_idx[e], BM)]++;
    MPI_Alltoall(outtot, 1, MPI_INT, intot, 1, MPI_INT, MPI_COMM_WORLD);
    long in_edges = 0;
    for (int p = 0; p < nprocs; p++) in_edges += intot[p];

    int* dist = (int*)malloc((size_t)(nloc + 1) * sizeof(int));
    int* frontier = (int*)malloc((size_t)(nloc + 1) * sizeof(int));
    int* next = (int*)malloc((size_t)(nloc + 1) * sizeof(int));
    int* sendbuf = (int*)malloc((nedge + 1) * sizeof(int));
    int* recvbuf = (int*)malloc(((size_t)in_edges + 1) * sizeof(int));
    int* counts = (int*)malloc(5 * (size_t)nprocs * sizeof(int));
    if (!dist || !frontier || !next || !sendbuf || !recvbuf || !counts) {
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

    /* ---- DATA PREP (before ROI, not timed): this rank's block and buffers -> its PE slot. Slot layout:
     * [row_ptr nloc+1][col_idx nloc*deg][dist nloc][frontier nloc][next nloc][sendbuf nloc*deg][recvbuf in_edges]
     * [sc n][rc n][so n][ro n][outtot n]. Assumes thread-MPI: slot r maps to PE r's units directly. */
    int prep = pimid_devorg_needs_prep(&dev);
    bfs_mpi_geom_t geom = { deg, nprocs, in_edges };
    size_t need = bfs_mpi_need(V, &geom);
    size_t slot_bytes = 0;
    int* dbuf = NULL;
    int *rp = row_ptr, *ci = col_idx, *ds = dist, *fr = frontier, *nx = next, *sb = sendbuf, *rb = recvbuf;
    int *sc = counts, *rc = counts + nprocs, *so = counts + 2 * nprocs, *ro = counts + 3 * nprocs,
        *ot = counts + 4 * nprocs;
    memcpy(ot, outtot, (size_t)nprocs * sizeof(int));
    if (prep) {
        /* A need larger than the slot is refused. */
        slot_bytes = pimid_devorg_slot_bytes(&dev);
        if (need > slot_bytes) {
            char who[32];
            bfs_mpi_geom_t mean = { deg, nprocs, -1 };
            snprintf(who, sizeof who, "rank %d: ", rank);
            pimid_devorg_report_slot_refusal(who, "bfs", &dev, need, slot_bytes, "--vertices",
                pimid_devorg_largest_fit(bfs_mpi_need, &mean, V, slot_bytes));
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        dbuf = (int*)pimid_devorg_alloc(&dev, &slot_bytes);
        if (!dbuf) {
            fprintf(stderr, "rank %d: devorg: alloc of the device buffer failed; "
                    "running host-layout (no prep)\n", rank);
            prep = 0;
        }
    }
    if (prep) {
        int* slot = (int*)pimid_devorg_pe_slot(dbuf, rank, slot_bytes);
        rp = slot;
        ci = rp + (nloc + 1);
        ds = ci + nedge;
        fr = ds + nloc;
        nx = fr + nloc;
        sb = nx + nloc;
        rb = sb + nedge;
        sc = rb + in_edges;
        rc = sc + nprocs;
        so = rc + nprocs;
        ro = so + nprocs;
        ot = ro + nprocs;
        memcpy(rp, row_ptr, (size_t)(nloc + 1) * sizeof(int));
        memcpy(ci, col_idx, nedge * sizeof(int));
        memcpy(ds, dist, (size_t)nloc * sizeof(int));
        memcpy(ot, outtot, (size_t)nprocs * sizeof(int));
        if (fsize) fr[0] = frontier[0];
    }
    so[0] = 0;
    for (int p = 1; p < nprocs; p++) so[p] = so[p - 1] + ot[p - 1];

    if (prep)
        fprintf(stderr, "[devorg] PREP ACTIVE: kernel=bfs level=%s pes=%d "
                "rank=%d slot_bytes=%zu\n",
                pimid_devorg_level_name(dev.level), dev.num_pes, rank, slot_bytes);

    pimid_spm_t spm;   /* this rank's PE scratchpad */
    pimid_spm_open(&spm);

    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) zsim_roi_begin();

    int level = 0;
    int gsize = 1;   /* the frontier, summed over the ranks: the source */
    while (gsize > 0) {
        level++;

        /* Expand: a neighbour in this rank's block is marked here; every other one goes to its owner's bucket */
        for (int p = 0; p < nprocs; p++) sc[p] = 0;
        int n = 0;
        if (!spm.cap) {
            for (int f = 0; f < fsize; f++) {
                const int u = (int)(fr[f] - lo);
                for (int e = rp[u]; e < rp[u + 1]; e++) {
                    const int v = ci[e];
                    const int p = bfs_owner(v, BM);
                    if (p == rank) {
                        if (ds[v - lo] == -1) { ds[v - lo] = level; nx[n++] = v; }
                    } else {
                        sb[so[p] + sc[p]++] = v;
                    }
                }
            }
        } else {
            n = (int)bfs_route(&spm, rp, ci, ds, fr, (size_t)fsize, lo, BM, rank, level, nx, sb, so, sc);
        }

        /* Route: the counts, then the vertices, to their owners */
        MPI_Alltoall(sc, 1, MPI_INT, rc, 1, MPI_INT, MPI_COMM_WORLD);
        ro[0] = 0;
        for (int p = 1; p < nprocs; p++) ro[p] = ro[p - 1] + rc[p - 1];
        const int nrecv = ro[nprocs - 1] + rc[nprocs - 1];
        for (int i = 1; i < nprocs; i++) {
            const int dst = (rank + i) % nprocs, src = (rank - i + nprocs) % nprocs;
            MPI_Sendrecv(sb + so[dst], sc[dst], MPI_INT, dst, 0,
                         rb + ro[src], rc[src], MPI_INT, src, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        }

        /* Keep the received vertices not visited yet: the rest of the next frontier */
        if (!spm.cap) {
            for (int i = 0; i < nrecv; i++) {
                const int v = rb[i];
                if (ds[v - lo] == -1) {
                    ds[v - lo] = level;
                    nx[n++] = v;
                }
            }
        } else {
            n += (int)bfs_keep(&spm, ds, lo, rb, (size_t)nrecv, nx + n, level);
        }
        int* t = fr; fr = nx; nx = t;
        fsize = n;
        MPI_Allreduce(&fsize, &gsize, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    }

    if (rank == 0) zsim_roi_end();
    /* The closing barrier every MPI kernel has: the device-scope ROI ends when the last rank arrives here. */
    MPI_Barrier(MPI_COMM_WORLD);
    pimid_spm_close(&spm);

    /* The (untimed) check: the vertices reached, summed over the ranks. */
    long visited_local = 0, visited = 0;
    for (int i = 0; i < nloc; i++)
        if (ds[i] >= 0) visited_local++;
    MPI_Reduce(&visited_local, &visited, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_WORLD);

    if (rank == 0) {
        printf("BENCH_LEVELS: %d\n", level);
        printf("BENCH_VISITED: %ld\n", visited);
        printf("BENCH_CHECKSUM: %ld\n", visited);
        printf("BENCH_DONE\n");
    }

    free(row_ptr); free(col_idx); free(outtot); free(intot);
    free(dist); free(frontier); free(next); free(sendbuf); free(recvbuf); free(counts);
    MPI_Finalize();
    return 0;
}
