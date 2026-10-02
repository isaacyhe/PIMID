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

/* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): per-PE need of [row][col][dist]. */
typedef struct { int P; int deg; } bfs_omp_ctx;
static size_t bfs_omp_need(long v, const void* c) {
    const bfs_omp_ctx* x = (const bfs_omp_ctx*)c;
    size_t vpp = (size_t)((v + x->P - 1) / x->P);
    return ((vpp + 1) + vpp * (size_t)x->deg + vpp) * sizeof(int);
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

    if (prep) {
        /* ---- DATA PREP (before ROI, not timed): partition CSR by owner. ---- */
        omp_set_num_threads(P);
        int** s_row = (int**)malloc((size_t)P * sizeof(int*));
        int** s_col = (int**)malloc((size_t)P * sizeof(int*));
        int** s_dist = (int**)malloc((size_t)P * sizeof(int*));
        if (!s_row || !s_col || !s_dist) { fprintf(stderr, "malloc failed\n"); return 1; }
        for (int p = 0; p < P; p++) {
            int v0 = p * vpp, v1 = v0 + vpp; if (v1 > V) v1 = V;
            int* slot = (int*)pimid_devorg_pe_slot(base, p, slot_bytes);
            s_row[p] = slot;
            s_col[p] = slot + (vpp + 1);
            s_dist[p] = slot + (vpp + 1) + (size_t)vpp * deg;
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
        zsim_roi_end();

        long visited = 0;
        for (int i = 0; i < V; i++)
            if (dist[i] >= 0) visited++;
        printf("BENCH_LEVELS: %d\n", level);
        printf("BENCH_VISITED: %ld\n", visited);
        printf("BENCH_CHECKSUM: %ld\n", visited);
        printf("BENCH_DONE\n");
    }

    free(row_ptr); free(col_idx); free(dist);
    free(frontier); free(next_frontier);
    return 0;
}
