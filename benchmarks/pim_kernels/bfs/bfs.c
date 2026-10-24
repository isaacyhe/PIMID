/* bfs.c — Breadth-First Search on random graph (adjacency list via CSR)
 * Serial baseline. Generates random graph with given vertex count and degree. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zsim_hooks.h"
#include "../../include/pimid_spm.h"

#define DEFAULT_VERTICES 512
#define DEFAULT_DEGREE   8

static uint32_t bench_rand(uint32_t* s) {
    *s = *s * 1103515245 + 12345;
    return (*s >> 16) & 0x7FFF;
}

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* 1.12.18: the serial claim rule for bfs_expand: test-and-set dist[v], and
 * append the vertex to the next frontier when it was unvisited. */
typedef struct { const int* row_ptr; const int* col_idx; int* dist; int level; } bfs_ctx_t;

static inline const int* bfs_adj(const bfs_ctx_t* c, int u, size_t* n) {
    const int e0 = c->row_ptr[u], e1 = c->row_ptr[u + 1];
    *n = (size_t)(e1 - e0);
    return c->col_idx + e0;
}

static inline int bfs_visit(const bfs_ctx_t* c, int v) {
    if (c->dist[v] == -1) {
        c->dist[v] = c->level;
        return 1;
    }
    return 0;
}

/* 1.12.18: expand frontier[0..nf) by one level through the PE scratchpad
 * (pimid_spm.h). The RAM holds three equal buffers: a tile of the frontier, a
 * tile of one vertex's adjacency row (col_idx) and a tile of the output list
 * (the vertices bfs_visit asks to append). Each frontier entry and each
 * adjacency row is fetched once; the output list is written back a full tile
 * at a time and at the end. The row_ptr[u] lookup and the dist[v] test are
 * irregular and stay direct DRAM accesses (bfs_adj / bfs_visit, the part each
 * variant defines for its own claim rule and layout). With no scratchpad the
 * tiles are the whole range and this is the plain frontier loop. Returns the
 * number of vertices appended to out. */
static size_t bfs_expand(pimid_spm_t* s0, const bfs_ctx_t* c0, const int* frontier, size_t nf,
                         int* out) {
    const bfs_ctx_t cl = *c0, *c = &cl;   /* local copies: no store below can alias them */
    pimid_spm_t sl = *s0, *s = &sl;
    const size_t T = pimid_spm_tile(s, 3, sizeof(int));
    const size_t of = 0, oa = s->cap ? T * sizeof(int) : 0, oo = s->cap ? 2 * T * sizeof(int) : 0;
    size_t nout = 0, no = 0;
    int* ob = (int*)pimid_spm_out(s, oo, out);
    for (size_t fb = 0; fb < nf; fb += T) {
        const size_t nb = (nf - fb < T) ? nf - fb : T;
        const int* fl = (const int*)pimid_spm_in(s, of, frontier + fb, nb * sizeof(int));
        for (size_t f = 0; f < nb; f++) {
            size_t ne;
            const int* adj = bfs_adj(c, fl[f], &ne);
            for (size_t eb = 0; eb < ne; eb += T) {
                const size_t na = (ne - eb < T) ? ne - eb : T;
                const int* ab = (const int*)pimid_spm_in(s, oa, adj + eb, na * sizeof(int));
                for (size_t e = 0; e < na; e++) {
                    const int v = ab[e];
                    if (bfs_visit(c, v)) {
                        ob[no++] = v;
                        if (no == T) {   /* out tile full (only with a scratchpad) */
                            pimid_spm_flush(s, out + nout, ob, no * sizeof(int));
                            nout += no; no = 0;
                            ob = (int*)pimid_spm_out(s, oo, out + nout);
                        }
                    }
                }
            }
        }
    }
    pimid_spm_flush(s, out + nout, ob, no * sizeof(int));
    return nout + no;
}

int main(int argc, char* argv[]) {
    int V = parse_int_arg(argc, argv, "--vertices", DEFAULT_VERTICES);
    int deg = parse_int_arg(argc, argv, "--degree", DEFAULT_DEGREE);
    int source = parse_int_arg(argc, argv, "--source", 0);
    uint32_t seed = 42;

    int total_edges = V * deg;
    int* row_ptr = (int*)malloc((V + 1) * sizeof(int));
    int* col_idx = (int*)malloc(total_edges * sizeof(int));
    int* dist = (int*)malloc(V * sizeof(int));
    int* frontier = (int*)malloc(V * sizeof(int));
    int* next_frontier = (int*)malloc(V * sizeof(int));
    if (!row_ptr || !col_idx || !dist || !frontier || !next_frontier) {
        fprintf(stderr, "malloc failed\n"); return 1;
    }

    /* Build random graph (CSR) */
    for (int i = 0; i <= V; i++) row_ptr[i] = i * deg;
    for (int i = 0; i < V; i++) {
        for (int d = 0; d < deg; d++)
            col_idx[i * deg + d] = bench_rand(&seed) % V;
    }

    /* Init distances */
    for (int i = 0; i < V; i++) dist[i] = -1;
    dist[source] = 0;
    frontier[0] = source;
    int fsize = 1;

    pimid_spm_t spm;
    pimid_spm_open(&spm);
    zsim_roi_begin();
    int level = 0;
    /* 1.12.18: no scratchpad -- the 1.12.17 code, unchanged */
    if (!spm.cap) {
        while (fsize > 0) {
            level++;
            int nsize = 0;
            for (int f = 0; f < fsize; f++) {
                int u = frontier[f];
                for (int e = row_ptr[u]; e < row_ptr[u + 1]; e++) {
                    int v = col_idx[e];
                    if (dist[v] == -1) {
                        dist[v] = level;
                        next_frontier[nsize++] = v;
                    }
                }
            }
            /* Swap frontiers */
            int* tmp = frontier;
            frontier = next_frontier;
            next_frontier = tmp;
            fsize = nsize;
        }
    } else {
        while (fsize > 0) {
            level++;
            const bfs_ctx_t ctx = { row_ptr, col_idx, dist, level };
            int nsize = (int)bfs_expand(&spm, &ctx, frontier, (size_t)fsize, next_frontier);
            /* Swap frontiers */
            int* tmp = frontier;
            frontier = next_frontier;
            next_frontier = tmp;
            fsize = nsize;
        }
    }
    zsim_roi_end();
    pimid_spm_close(&spm);

    long visited = 0;
    for (int i = 0; i < V; i++)
        if (dist[i] >= 0) visited++;

    printf("BENCH_LEVELS: %d\n", level);
    printf("BENCH_VISITED: %ld\n", visited);
    printf("BENCH_CHECKSUM: %ld\n", visited);
    printf("BENCH_DONE\n");

    free(row_ptr); free(col_idx); free(dist);
    free(frontier); free(next_frontier);
    return 0;
}
