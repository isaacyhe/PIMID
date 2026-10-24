/* gemv.c — General Matrix-Vector Multiply: y = A * x
 * Serial baseline. A is N×N, x and y are N-vectors. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zsim_hooks.h"
#include "../../include/pimid_spm.h"

#define DEFAULT_SIZE 256

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* 1.12.18: y[0..nrows) = A[0..nrows) * x through the PE scratchpad
 * (pimid_spm.h). The RAM holds three equal buffers: a tile of x, a tile of
 * one A row and a tile of y. Each row block fetches its x tiles once and each
 * A row segment once, and writes its y block back once. With no scratchpad
 * the tile is the whole range and this is the plain row loop. */
static void gemv_rows(pimid_spm_t* s, const float* A, const float* x, float* y,
                      size_t nrows, size_t n) {
    const size_t T = pimid_spm_tile(s, 3, sizeof(float));
    const size_t ox = 0, oa = s->cap ? T * sizeof(float) : 0, oy = s->cap ? 2 * T * sizeof(float) : 0;
    for (size_t rb = 0; rb < nrows; rb += T) {
        const size_t nr = (nrows - rb < T) ? nrows - rb : T;
        float* yb = (float*)pimid_spm_out(s, oy, y + rb);
        for (size_t cb = 0; cb < n; cb += T) {
            const size_t nc = (n - cb < T) ? n - cb : T;
            const float* xb = (const float*)pimid_spm_in(s, ox, x + cb, nc * sizeof(float));
            for (size_t r = 0; r < nr; r++) {
                const float* ab = (const float*)pimid_spm_in(s, oa, A + (rb + r) * n + cb, nc * sizeof(float));
                float sum = 0.0f;
                for (size_t c = 0; c < nc; c++)
                    sum += ab[c] * xb[c];
                if (cb == 0) yb[r] = sum; else yb[r] += sum;
            }
        }
        pimid_spm_flush(s, y + rb, yb, nr * sizeof(float));
    }
}

int main(int argc, char* argv[]) {
    int N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);

    float* A = (float*)malloc((size_t)N * N * sizeof(float));
    float* x = (float*)malloc(N * sizeof(float));
    float* y = (float*)calloc(N, sizeof(float));
    if (!A || !x || !y) { fprintf(stderr, "malloc failed\n"); return 1; }

    for (int i = 0; i < N; i++) {
        x[i] = 1.0f;
        for (int j = 0; j < N; j++)
            A[i * N + j] = (float)((i + j) % 7);
    }

    pimid_spm_t spm;
    pimid_spm_open(&spm);
    zsim_roi_begin();
    /* 1.12.18: no scratchpad -- the 1.12.17 code, unchanged */
    if (!spm.cap) {
        for (int i = 0; i < N; i++) {
            float sum = 0.0f;
            for (int j = 0; j < N; j++)
                sum += A[i * N + j] * x[j];
            y[i] = sum;
        }
    } else {
        gemv_rows(&spm, A, x, y, (size_t)N, (size_t)N);
    }
    zsim_roi_end();
    pimid_spm_close(&spm);

    double checksum = 0.0;
    for (int i = 0; i < N; i++) checksum += y[i];
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(A); free(x); free(y);
    return 0;
}
