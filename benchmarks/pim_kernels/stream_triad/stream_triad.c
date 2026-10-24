/* stream_triad.c — STREAM Triad: a[i] = b[i] + scalar * c[i]
 * Serial baseline. Standalone, no shared headers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zsim_hooks.h"
#include "../../include/pimid_spm.h"

#define DEFAULT_SIZE 8192

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* 1.12.18: a[0..n) = b + scalar * c through the PE scratchpad
 * (pimid_spm.h). The RAM holds three equal buffers: a tile of b, a tile of c
 * and a tile of a. Each b and c tile is fetched once and each a tile is
 * written back once. With no scratchpad the tile is the whole range and this
 * is the plain element loop. */
static void stream_triad_range(pimid_spm_t* s, float* a, const float* b, const float* c,
                               float scalar, size_t n) {
    const size_t T = pimid_spm_tile(s, 3, sizeof(float));
    const size_t ob = 0, oc = s->cap ? T * sizeof(float) : 0, oa = s->cap ? 2 * T * sizeof(float) : 0;
    for (size_t i0 = 0; i0 < n; i0 += T) {
        const size_t m = (n - i0 < T) ? n - i0 : T;
        const float* bb = (const float*)pimid_spm_in(s, ob, b + i0, m * sizeof(float));
        const float* cb = (const float*)pimid_spm_in(s, oc, c + i0, m * sizeof(float));
        float* ab = (float*)pimid_spm_out(s, oa, a + i0);
        for (size_t i = 0; i < m; i++)
            ab[i] = bb[i] + scalar * cb[i];
        pimid_spm_flush(s, a + i0, ab, m * sizeof(float));
    }
}

int main(int argc, char* argv[]) {
    int N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    float scalar = 3.0f;

    float* a = (float*)malloc(N * sizeof(float));
    float* b = (float*)malloc(N * sizeof(float));
    float* c = (float*)malloc(N * sizeof(float));
    if (!a || !b || !c) { fprintf(stderr, "malloc failed\n"); return 1; }

    for (int i = 0; i < N; i++) {
        b[i] = 1.0f;
        c[i] = 2.0f;
    }

    pimid_spm_t spm;
    pimid_spm_open(&spm);
    zsim_roi_begin();
    /* 1.12.18: no scratchpad -- the 1.12.17 code, unchanged */
    if (!spm.cap) {
        for (int i = 0; i < N; i++)
            a[i] = b[i] + scalar * c[i];
    } else {
        stream_triad_range(&spm, a, b, c, scalar, (size_t)N);
    }
    zsim_roi_end();
    pimid_spm_close(&spm);

    double checksum = 0.0;
    for (int i = 0; i < N; i++) checksum += a[i];
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(a); free(b); free(c);
    return 0;
}
