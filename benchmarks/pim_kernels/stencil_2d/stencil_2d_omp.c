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

#define DEFAULT_SIZE 64
#define DEFAULT_ITERS 5

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): per-PE need of [cur][next]. */
static size_t stencil_2d_omp_need(long n, const void* c) {
    long P = *(const int*)c;
    return 2ull * (size_t)((n + P - 1) / P) * (size_t)n * sizeof(float);
}

int main(int argc, char* argv[]) {
    int N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    int iters = parse_int_arg(argc, argv, "--iters", DEFAULT_ITERS);
    pimid_devorg_t dev = pimid_devorg_from_args(argc, argv);
    int P = dev.num_pes;

    float* grid = (float*)calloc((size_t)N * N, sizeof(float));
    float* tmp = (float*)calloc((size_t)N * N, sizeof(float));
    if (!grid || !tmp) { fprintf(stderr, "malloc failed\n"); return 1; }

    /* Initialize: hot boundary on top row */
    for (int j = 0; j < N; j++) grid[j] = 100.0f;

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
        for (int i = 0; i < N; i++) {
            memcpy(rowsA[i], grid + (size_t)i * N, (size_t)N * sizeof(float));
            memset(rowsB[i], 0, (size_t)N * sizeof(float));
        }

        fprintf(stderr, "[devorg] PREP ACTIVE: kernel=stencil_2d level=%s pes=%d "
                "slot_bytes=%zu\n",
                pimid_devorg_level_name(dev.level), P, slot_bytes);
        zsim_roi_begin();
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
        zsim_roi_end();

        double checksum = 0.0;
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++) checksum += rowsA[i][j];
        printf("BENCH_CHECKSUM: %f\n", checksum);
        printf("BENCH_DONE\n");
        free(rowsA); free(rowsB);
    } else {
        zsim_roi_begin();
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
        zsim_roi_end();

        double checksum = 0.0;
        for (int i = 0; i < N * N; i++) checksum += grid[i];
        printf("BENCH_CHECKSUM: %f\n", checksum);
        printf("BENCH_DONE\n");
    }

    free(grid); free(tmp);
    return 0;
}
