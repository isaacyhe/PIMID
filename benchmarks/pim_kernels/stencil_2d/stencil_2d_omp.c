/* stencil_2d_omp.c -- 2D 5-point Jacobi stencil (nearest-neighbor average)
 * OpenMP parallel version. Grid is N*N, runs for iter iterations.
 *
 * Device-organization aware (PIMID): the simulator prices each PE access by
 * LOCATION. Prep (before the ROI, untimed) places PE p's row block -- both the
 * current and next grids -- in PE p's own unit slot; the ROI then reads its own
 * rows locally and only the block-boundary halo rows from the neighbor PEs'
 * slots (genuine near-neighbor traffic, which is the stencil's real
 * communication pattern). Row-pointer indirection keeps the arithmetic
 * identical to the host-layout path. Coarse placement runs the original path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include "zsim_hooks.h"
#include "../../include/pimid_devorg.h"
#include "../../include/pimid_spm.h"

#define DEFAULT_SIZE 64
#define DEFAULT_ITERS 5

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* 1.12.18: one interior row out[0..n) of the 5-point stencil from the input
 * rows up, mid and dn, through the PE scratchpad (pimid_spm.h). The RAM holds
 * four equal buffers: a segment of up, a segment of mid (with one halo column
 * on each side), a segment of dn and a segment of out. Each column segment of
 * the three input rows is fetched once, so a sweep fetches every input row
 * three times (as the row above, the row itself and the row below), and each
 * out segment is written back once. With no scratchpad the segment is the
 * whole row and this is the plain row loop. */
static void stencil_row(pimid_spm_t* s, const float* up, const float* mid, const float* dn,
                        float* out, size_t n) {
    const size_t T = pimid_spm_tile(s, 4, sizeof(float));
    const size_t W = T - 2;   /* out columns per segment: mid needs W + 2 */
    const size_t ou = 0, om = s->cap ? T * sizeof(float) : 0;
    const size_t od = s->cap ? 2 * T * sizeof(float) : 0, oo = s->cap ? 3 * T * sizeof(float) : 0;
    for (size_t cb = 0; cb < n; cb += W) {
        const size_t ce = (n - cb < W) ? n : cb + W;                    /* out columns [cb, ce) */
        const size_t lo = cb ? cb - 1 : 0, hi = (ce < n) ? ce + 1 : n;  /* mid columns [lo, hi) */
        const size_t j0 = cb ? cb : 1, j1 = (ce < n) ? ce : n - 1;      /* interior [j0, j1) */
        const size_t nj = (j1 > j0) ? j1 - j0 : 0;
        const float* ub = (const float*)pimid_spm_in(s, ou, up + j0, nj * sizeof(float));
        const float* mb = (const float*)pimid_spm_in(s, om, mid + lo, (hi - lo) * sizeof(float));
        const float* db = (const float*)pimid_spm_in(s, od, dn + j0, nj * sizeof(float));
        float* ob = (float*)pimid_spm_out(s, oo, out + cb);
        if (cb == 0) ob[0] = mb[0];
        if (ce == n) ob[n - 1 - cb] = mb[n - 1 - lo];
        for (size_t j = j0; j < j0 + nj; j++) {
            ob[j - cb] = 0.25f * (
                ub[j - j0] + db[j - j0] +
                mb[j - 1 - lo] + mb[j + 1 - lo]);
        }
        pimid_spm_flush(s, out + cb, ob, (ce - cb) * sizeof(float));
    }
}

/* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): per-PE need of [cur][next]. */
static size_t stencil_2d_omp_need(long n, const void* c) {
    long P = *(const int*)c;
    return 2ull * (size_t)((n + P - 1) / P) * (size_t)n * sizeof(float);
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
    int N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    int iters = parse_int_arg(argc, argv, "--iters", DEFAULT_ITERS);
    pimid_devorg_t dev = pimid_devorg_from_args(argc, argv);
    int P = dev.num_pes;

    float* grid = (float*)malloc((size_t)N * N * sizeof(float));   /* zeroed below */
    float* tmp = (float*)malloc((size_t)N * N * sizeof(float));
    if (!grid || !tmp) { fprintf(stderr, "malloc failed\n"); return 1; }

    int prep = pimid_devorg_needs_prep(&dev);
    int rpp = (N + P - 1) / P;                       /* rows per PE block */
    size_t need = 2ull * rpp * N * sizeof(float);    /* [cur block][next block] */

    size_t slot_bytes = 0;
    char* base = NULL;
    if (prep) {
        /* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): a need larger than the slot
         * is refused; it used to fall back to host layout in silence. */
        slot_bytes = pimid_devorg_slot_bytes(&dev);
        if (need > slot_bytes) {
            pimid_devorg_report_slot_refusal("", "stencil_2d", &dev, need, slot_bytes, "--size",
                pimid_devorg_largest_fit(stencil_2d_omp_need, &P, N, slot_bytes));
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
    /* Host layout: both ROI paths split the interior rows [1, N-1) as
     * schedule(static) over T threads; the master keeps the two boundary rows
     * (the ROI copies them on the master). Devorg prep: PE p's row block
     * [p*rpp, (p+1)*rpp), which thread p copies into its slot. */
    if (prep) {
        #pragma omp parallel num_threads(T)
        {
            int lo, hi;
            ft_share(N, omp_get_thread_num(), omp_get_num_threads(), rpp, 0, &lo, &hi);
            if (hi > lo) {
                memset(grid + (size_t)lo * N, 0, (size_t)(hi - lo) * N * sizeof(float));
                memset(tmp + (size_t)lo * N, 0, (size_t)(hi - lo) * N * sizeof(float));
            }
        }
    } else {
        memset(grid, 0, (size_t)N * sizeof(float));
        memset(tmp, 0, (size_t)N * sizeof(float));
        memset(grid + (size_t)(N - 1) * N, 0, (size_t)N * sizeof(float));
        memset(tmp + (size_t)(N - 1) * N, 0, (size_t)N * sizeof(float));
        #pragma omp parallel for schedule(static) num_threads(T)
        for (int i = 1; i < N - 1; i++) {
            memset(grid + (size_t)i * N, 0, (size_t)N * sizeof(float));
            memset(tmp + (size_t)i * N, 0, (size_t)N * sizeof(float));
        }
    }

    /* Initialize: hot boundary on top row */
    for (int j = 0; j < N; j++) grid[j] = 100.0f;

    if (prep) {
        /* ---- DATA PREP (before ROI, not timed): PE p's slot = [cur][next];
         * rowsA/rowsB give O(1) row lookup across slots. */
        omp_set_num_threads(P);
        float** rowsA = (float**)malloc((size_t)N * sizeof(float*));
        float** rowsB = (float**)malloc((size_t)N * sizeof(float*));
        if (!rowsA || !rowsB) { fprintf(stderr, "malloc failed\n"); return 1; }
        for (int i = 0; i < N; i++) {
            int p = i / rpp; if (p >= P) p = P - 1;
            int off = i - p * rpp;
            float* slot = (float*)pimid_devorg_pe_slot(base, p, slot_bytes);
            rowsA[i] = slot + (size_t)off * N;
            rowsB[i] = slot + (size_t)rpp * N + (size_t)off * N;
        }
        /* 1.12.18: first-touch placement -- each thread initialises the share it computes on, so its pages live in its PE's memory. */
        /* Thread p fills PE p's slot: the rows i with i / rpp == p (the
         * table above; the clamp to P - 1 never fires, rpp * P >= N). */
        #pragma omp parallel num_threads(P)
        {
            const int me = omp_get_thread_num(), nt = omp_get_num_threads();
            for (int i = 0; i < N; i++) {
                int p = i / rpp; if (p >= P) p = P - 1;
                if (p % nt != me) continue;
                memcpy(rowsA[i], grid + (size_t)i * N, (size_t)N * sizeof(float));
                memset(rowsB[i], 0, (size_t)N * sizeof(float));
            }
        }

        fprintf(stderr, "[devorg] PREP ACTIVE: kernel=stencil_2d level=%s pes=%d "
                "slot_bytes=%zu\n",
                pimid_devorg_level_name(dev.level), P, slot_bytes);
        zsim_roi_begin();
        /* 1.12.18: no scratchpad -- the 1.12.17 code, unchanged */
        if (!spms[0].cap) {
            for (int t = 0; t < iters; t++) {
                /* Preserve Dirichlet boundary rows */
                memcpy(rowsB[0], rowsA[0], (size_t)N * sizeof(float));
                memcpy(rowsB[N - 1], rowsA[N - 1], (size_t)N * sizeof(float));
                #pragma omp parallel for
                for (int i = 1; i < N - 1; i++) {
                    rowsB[i][0] = rowsA[i][0];
                    rowsB[i][N - 1] = rowsA[i][N - 1];
                    for (int j = 1; j < N - 1; j++) {
                        rowsB[i][j] = 0.25f * (
                            rowsA[i - 1][j] + rowsA[i + 1][j] +
                            rowsA[i][j - 1] + rowsA[i][j + 1]);
                    }
                }
                /* Swap row-pointer tables */
                float** s = rowsA; rowsA = rowsB; rowsB = s;
            }
        } else {
            for (int t = 0; t < iters; t++) {
                /* Preserve Dirichlet boundary rows */
                memcpy(rowsB[0], rowsA[0], (size_t)N * sizeof(float));
                memcpy(rowsB[N - 1], rowsA[N - 1], (size_t)N * sizeof(float));
                #pragma omp parallel for num_threads(T)
                for (int i = 1; i < N - 1; i++)
                    stencil_row(&spms[omp_get_thread_num()], rowsA[i - 1], rowsA[i], rowsA[i + 1],
                                rowsB[i], (size_t)N);
                /* Swap row-pointer tables */
                float** s = rowsA; rowsA = rowsB; rowsB = s;
            }
        }
        zsim_roi_end();

        double checksum = 0.0;
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++) checksum += rowsA[i][j];
        printf("BENCH_CHECKSUM: %f\n", checksum);
        printf("BENCH_DONE\n");
        free(rowsA); free(rowsB);
    } else {
        zsim_roi_begin();
        /* 1.12.18: no scratchpad -- the 1.12.17 code, unchanged */
        if (!spms[0].cap) {
            for (int t = 0; t < iters; t++) {
                /* Preserve Dirichlet boundary rows */
                memcpy(&tmp[0], &grid[0], N * sizeof(float));
                memcpy(&tmp[(N - 1) * N], &grid[(N - 1) * N], N * sizeof(float));
                #pragma omp parallel for
                for (int i = 1; i < N - 1; i++) {
                    tmp[i * N + 0] = grid[i * N + 0];
                    tmp[i * N + (N - 1)] = grid[i * N + (N - 1)];
                    for (int j = 1; j < N - 1; j++) {
                        tmp[i * N + j] = 0.25f * (
                            grid[(i - 1) * N + j] + grid[(i + 1) * N + j] +
                            grid[i * N + (j - 1)] + grid[i * N + (j + 1)]);
                    }
                }
                /* Swap */
                float* s = grid; grid = tmp; tmp = s;
            }
        } else {
            for (int t = 0; t < iters; t++) {
                /* Preserve Dirichlet boundary rows */
                memcpy(&tmp[0], &grid[0], N * sizeof(float));
                memcpy(&tmp[(N - 1) * N], &grid[(N - 1) * N], N * sizeof(float));
                #pragma omp parallel for num_threads(T)
                for (int i = 1; i < N - 1; i++)
                    stencil_row(&spms[omp_get_thread_num()], &grid[(i - 1) * N], &grid[i * N],
                                &grid[(i + 1) * N], &tmp[i * N], (size_t)N);
                /* Swap */
                float* s = grid; grid = tmp; tmp = s;
            }
        }
        zsim_roi_end();

        double checksum = 0.0;
        for (int i = 0; i < N * N; i++) checksum += grid[i];
        printf("BENCH_CHECKSUM: %f\n", checksum);
        printf("BENCH_DONE\n");
    }

    #pragma omp parallel num_threads(T)
    pimid_spm_close(&spms[omp_get_thread_num()]);
    free(spms);

    free(grid); free(tmp);
    return 0;
}
