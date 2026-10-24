/* stencil_2d.c — 2D 5-point Jacobi stencil (nearest-neighbor average)
 * Serial baseline. Grid is N×N, runs for iter iterations. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zsim_hooks.h"
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

int main(int argc, char* argv[]) {
    int N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    int iters = parse_int_arg(argc, argv, "--iters", DEFAULT_ITERS);

    float* grid = (float*)calloc((size_t)N * N, sizeof(float));
    float* tmp = (float*)calloc((size_t)N * N, sizeof(float));
    if (!grid || !tmp) { fprintf(stderr, "malloc failed\n"); return 1; }

    /* Initialize: hot boundary on top row */
    for (int j = 0; j < N; j++) grid[j] = 100.0f;

    pimid_spm_t spm;
    pimid_spm_open(&spm);
    zsim_roi_begin();
    /* 1.12.18: no scratchpad -- the 1.12.17 code, unchanged */
    if (!spm.cap) {
        for (int t = 0; t < iters; t++) {
            /* Preserve Dirichlet boundary rows */
            memcpy(&tmp[0], &grid[0], N * sizeof(float));
            memcpy(&tmp[(N - 1) * N], &grid[(N - 1) * N], N * sizeof(float));
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
            for (int i = 1; i < N - 1; i++)
                stencil_row(&spm, &grid[(i - 1) * N], &grid[i * N], &grid[(i + 1) * N],
                            &tmp[i * N], (size_t)N);
            /* Swap */
            float* s = grid; grid = tmp; tmp = s;
        }
    }
    zsim_roi_end();
    pimid_spm_close(&spm);

    double checksum = 0.0;
    for (int i = 0; i < N * N; i++) checksum += grid[i];
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(grid); free(tmp);
    return 0;
}
