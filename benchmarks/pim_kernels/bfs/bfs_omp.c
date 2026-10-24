/* bfs_omp.c -- Breadth-First Search on random graph (adjacency list via CSR)
 * OpenMP parallel version. Parallel frontier expansion with atomic dist update.
 *
 * Device-organization aware (PIMID): the simulator prices each PE access by
 * LOCATION. Prep (before the ROI, untimed) partitions the CSR by vertex range:
 * PE p's slot holds [row_ptr slice][col_idx slice][dist slice] for its owned
 * vertices. The ROI reads a vertex's edges from the OWNER's slot and CASes
 * dist in the OWNER's slot -- BFS's genuinely irregular cross-PE traffic is
 * then real network distance, while owned-range scans stay local. The shared
 * frontier work queue remains host-side (it IS the shared structure). The
 * arithmetic and atomics are identical to the host-layout path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include "zsim_hooks.h"
#include "../../include/pimid_devorg.h"
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

/* 1.12.18: the shared-memory claim rule for bfs_expand: compare-and-swap
 * dist[v] and append the vertex to the shared next frontier at an atomically
 * taken slot (so nothing goes to bfs_expand's private output list). On the
 * prepped layout a vertex's row and dist live in its OWNER's slot (s_row /
 * s_col / s_dist, vpp vertices per PE); on host layout s_row is NULL. */
typedef struct {
    const int* row_ptr; const int* col_idx; int* dist;
    int* const* s_row; int* const* s_col; int* const* s_dist;
    int vpp, deg, level;
    int* next; int* nsize;
} bfs_ctx_t;

static inline const int* bfs_adj(const bfs_ctx_t* c, int u, size_t* n) {
    if (c->s_row) {
        int pu = u / c->vpp;
        int uo = u - pu * c->vpp;
        int e0 = c->s_row[pu][uo], e1 = c->s_row[pu][uo + 1];
        *n = (size_t)(e1 - e0);
        return c->s_col[pu] + (e0 - pu * c->vpp * c->deg);
    }
    const int e0 = c->row_ptr[u], e1 = c->row_ptr[u + 1];
    *n = (size_t)(e1 - e0);
    return c->col_idx + e0;
}

static inline int bfs_visit(const bfs_ctx_t* c, int v) {
    int* d;
    if (c->s_dist) {
        int pv = v / c->vpp;
        d = &c->s_dist[pv][v - pv * c->vpp];
    } else {
        d = &c->dist[v];
    }
    /* Atomically check-and-set distance for thread safety */
    int expected = -1;
    if (__sync_bool_compare_and_swap(d, expected, c->level)) {
        int pos;
        #pragma omp atomic capture
        pos = (*c->nsize)++;
        c->next[pos] = v;
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

/* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): per-PE need of [row][col][dist]. */
typedef struct { int P; int deg; } bfs_omp_ctx;
static size_t bfs_omp_need(long v, const void* c) {
    const bfs_omp_ctx* x = (const bfs_omp_ctx*)c;
    size_t vpp = (size_t)((v + x->P - 1) / x->P);
    return ((vpp + 1) + vpp * (size_t)x->deg + vpp) * sizeof(int);
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
    int V = parse_int_arg(argc, argv, "--vertices", DEFAULT_VERTICES);
    int deg = parse_int_arg(argc, argv, "--degree", DEFAULT_DEGREE);
    int source = parse_int_arg(argc, argv, "--source", 0);
    uint32_t seed = 42;
    pimid_devorg_t dev = pimid_devorg_from_args(argc, argv);
    int P = dev.num_pes;

    int total_edges = V * deg;
    int* row_ptr = (int*)malloc((V + 1) * sizeof(int));
    int* col_idx = (int*)malloc((size_t)total_edges * sizeof(int));
    int* dist = (int*)malloc(V * sizeof(int));
    int* frontier = (int*)malloc(V * sizeof(int));
    int* next_frontier = (int*)malloc(V * sizeof(int));
    if (!row_ptr || !col_idx || !dist || !frontier || !next_frontier) {
        fprintf(stderr, "malloc failed\n"); return 1;
    }

    int prep = pimid_devorg_needs_prep(&dev);
    int vpp = (V + P - 1) / P;                 /* vertices per PE */
    size_t need = ((size_t)(vpp + 1) + (size_t)vpp * deg + (size_t)vpp)
                  * sizeof(int);               /* [row][col][dist] per slot */
    bfs_omp_ctx need_ctx = { P, deg };

    size_t slot_bytes = 0;
    char* base = NULL;
    if (prep) {
        /* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): a need larger than the slot
         * is refused; it used to fall back to host layout in silence. */
        slot_bytes = pimid_devorg_slot_bytes(&dev);
        if (need > slot_bytes) {
            pimid_devorg_report_slot_refusal("", "bfs", &dev, need, slot_bytes, "--vertices",
                pimid_devorg_largest_fit(bfs_omp_need, &need_ctx, V, slot_bytes));
            return 1;
        }
        base = (char*)pimid_devorg_alloc(&dev, &slot_bytes);
        if (!base) {
            fprintf(stderr, "devorg: alloc of the device buffer failed; "
                            "running host-layout (no prep)\n");
            prep = 0;
        }
    }

    /* 1.12.18: each thread opens its PE's scratchpad before the ROI. */
    if (prep) omp_set_num_threads(P);
    int T = prep ? P : omp_get_max_threads();
    pimid_spm_t* spms = (pimid_spm_t*)calloc((size_t)T, sizeof(pimid_spm_t));
    if (!spms) { fprintf(stderr, "malloc failed\n"); return 1; }
    #pragma omp parallel num_threads(T)
    pimid_spm_open(&spms[omp_get_thread_num()]);

    /* 1.12.18: first-touch placement -- each thread initialises the share it computes on, so its pages live in its PE's memory. */
    /* The ROI splits the frontier, not the vertices, so there is no vertex
     * split to match; the vertices are split in blocks -- devorg prep: PE p's
     * owned range [p*vpp, (p+1)*vpp), which thread p copies into its slot;
     * host layout: schedule(static) over T threads -- and every per-vertex
     * array follows it: row_ptr, dist, the vertex's col_idx row, and the
     * frontier buffers by index. The edges come from one sequential random
     * stream, so each thread first touches (zeroes) its col_idx rows and the
     * master then writes the edges into the placed pages. */
    #pragma omp parallel num_threads(T)
    {
        int lo, hi;
        ft_share(V, omp_get_thread_num(), omp_get_num_threads(), prep ? vpp : 0, 0, &lo, &hi);
        for (int i = lo; i < hi; i++) {
            row_ptr[i] = i * deg;
            dist[i] = -1;
            frontier[i] = 0;
            next_frontier[i] = 0;
        }
        if (hi > lo)
            memset(col_idx + (size_t)lo * deg, 0, (size_t)(hi - lo) * deg * sizeof(int));
    }

    /* Build random graph (CSR) */
    row_ptr[V] = V * deg;
    for (int i = 0; i < V; i++) {
        for (int d = 0; d < deg; d++)
            col_idx[i * deg + d] = bench_rand(&seed) % V;
    }

    /* Init distances */
    dist[source] = 0;
    frontier[0] = source;
    int fsize = 1;

    if (prep) {
        /* ---- DATA PREP (before ROI, not timed): partition CSR by owner. ---- */
        omp_set_num_threads(P);
        int** s_row = (int**)malloc((size_t)P * sizeof(int*));
        int** s_col = (int**)malloc((size_t)P * sizeof(int*));
        int** s_dist = (int**)malloc((size_t)P * sizeof(int*));
        if (!s_row || !s_col || !s_dist) { fprintf(stderr, "malloc failed\n"); return 1; }
        for (int p = 0; p < P; p++) {
            int* slot = (int*)pimid_devorg_pe_slot(base, p, slot_bytes);
            s_row[p] = slot;
            s_col[p] = slot + (vpp + 1);
            s_dist[p] = slot + (vpp + 1) + (size_t)vpp * deg;
        }
        /* 1.12.18: first-touch placement -- each thread initialises the share it computes on, so its pages live in its PE's memory. */
        /* Thread p fills PE p's slot (vertex u's row and dist are read and
         * CASed in slot u / vpp; PE p's slot is the one in its own unit). */
        #pragma omp parallel num_threads(P)
        for (int p = omp_get_thread_num(); p < P; p += omp_get_num_threads()) {
            int v0 = p * vpp, v1 = v0 + vpp; if (v1 > V) v1 = V;
            if (v0 >= V) continue;
            memcpy(s_row[p], row_ptr + v0, (size_t)(v1 - v0 + 1) * sizeof(int));
            memcpy(s_col[p], col_idx + (size_t)v0 * deg,
                   (size_t)(v1 - v0) * deg * sizeof(int));
            memcpy(s_dist[p], dist + v0, (size_t)(v1 - v0) * sizeof(int));
        }

        fprintf(stderr, "[devorg] PREP ACTIVE: kernel=bfs level=%s pes=%d "
                "slot_bytes=%zu\n",
                pimid_devorg_level_name(dev.level), P, slot_bytes);
        zsim_roi_begin();
        int level = 0;
        /* 1.12.18: no scratchpad -- the 1.12.17 code, unchanged */
        if (!spms[0].cap) {
            while (fsize > 0) {
                level++;
                int nsize = 0;
                #pragma omp parallel for
                for (int f = 0; f < fsize; f++) {
                    int u = frontier[f];
                    int pu = u / vpp;
                    int uo = u - pu * vpp;
                    int e0 = s_row[pu][uo], e1 = s_row[pu][uo + 1];
                    for (int e = e0; e < e1; e++) {
                        int v = s_col[pu][e - pu * vpp * deg];
                        int pv = v / vpp;
                        int expected = -1;
                        if (__sync_bool_compare_and_swap(&s_dist[pv][v - pv * vpp],
                                                         expected, level)) {
                            int pos;
                            #pragma omp atomic capture
                            pos = nsize++;
                            next_frontier[pos] = v;
                        }
                    }
                }
                /* Swap frontiers */
                int* t = frontier;
                frontier = next_frontier;
                next_frontier = t;
                fsize = nsize;
            }
        } else {
            while (fsize > 0) {
                level++;
                int nsize = 0;
                const bfs_ctx_t ctx = { row_ptr, col_idx, dist, s_row, s_col, s_dist,
                                        vpp, deg, level, next_frontier, &nsize };
                #pragma omp parallel num_threads(T)
                {
                    const int t = omp_get_thread_num(), nt = omp_get_num_threads();
                    const int f0 = (int)(((long)fsize * t) / nt), f1 = (int)(((long)fsize * (t + 1)) / nt);
                    if (f1 > f0)
                        bfs_expand(&spms[t], &ctx, frontier + f0, (size_t)(f1 - f0), next_frontier);
                }
                /* Swap frontiers */
                int* t = frontier;
                frontier = next_frontier;
                next_frontier = t;
                fsize = nsize;
            }
        }
        zsim_roi_end();

        /* Gather dist back for the (untimed) result check */
        for (int p = 0; p < P; p++) {
            int v0 = p * vpp, v1 = v0 + vpp; if (v1 > V) v1 = V;
            if (v0 >= V) continue;
            memcpy(dist + v0, s_dist[p], (size_t)(v1 - v0) * sizeof(int));
        }
        free(s_row); free(s_col); free(s_dist);

        long visited = 0;
        for (int i = 0; i < V; i++)
            if (dist[i] >= 0) visited++;
        printf("BENCH_LEVELS: %d\n", level);
        printf("BENCH_VISITED: %ld\n", visited);
        printf("BENCH_CHECKSUM: %ld\n", visited);
        printf("BENCH_DONE\n");
    } else {
        zsim_roi_begin();
        int level = 0;
        /* 1.12.18: no scratchpad -- the 1.12.17 code, unchanged */
        if (!spms[0].cap) {
            while (fsize > 0) {
                level++;
                int nsize = 0;
                #pragma omp parallel for
                for (int f = 0; f < fsize; f++) {
                    int u = frontier[f];
                    for (int e = row_ptr[u]; e < row_ptr[u + 1]; e++) {
                        int v = col_idx[e];
                        /* Atomically check-and-set distance for thread safety */
                        int expected = -1;
                        if (__sync_bool_compare_and_swap(&dist[v], expected, level)) {
                            int pos;
                            #pragma omp atomic capture
                            pos = nsize++;
                            next_frontier[pos] = v;
                        }
                    }
                }
                /* Swap frontiers */
                int* t = frontier;
                frontier = next_frontier;
                next_frontier = t;
                fsize = nsize;
            }
        } else {
            while (fsize > 0) {
                level++;
                int nsize = 0;
                const bfs_ctx_t ctx = { row_ptr, col_idx, dist, NULL, NULL, NULL,
                                        vpp, deg, level, next_frontier, &nsize };
                #pragma omp parallel num_threads(T)
                {
                    const int t = omp_get_thread_num(), nt = omp_get_num_threads();
                    const int f0 = (int)(((long)fsize * t) / nt), f1 = (int)(((long)fsize * (t + 1)) / nt);
                    if (f1 > f0)
                        bfs_expand(&spms[t], &ctx, frontier + f0, (size_t)(f1 - f0), next_frontier);
                }
                /* Swap frontiers */
                int* t = frontier;
                frontier = next_frontier;
                next_frontier = t;
                fsize = nsize;
            }
        }
        zsim_roi_end();

        long visited = 0;
        for (int i = 0; i < V; i++)
            if (dist[i] >= 0) visited++;
        printf("BENCH_LEVELS: %d\n", level);
        printf("BENCH_VISITED: %ld\n", visited);
        printf("BENCH_CHECKSUM: %ld\n", visited);
        printf("BENCH_DONE\n");
    }

    #pragma omp parallel num_threads(T)
    pimid_spm_close(&spms[omp_get_thread_num()]);
    free(spms);

    free(row_ptr); free(col_idx); free(dist);
    free(frontier); free(next_frontier);
    return 0;
}
