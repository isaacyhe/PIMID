/* histogram.c — Histogram construction: random scatter into bins
 * Serial baseline. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zsim_hooks.h"
#include "../../include/pimid_spm.h"

#define DEFAULT_SIZE 16384
#define DEFAULT_BINS 256

static uint32_t bench_rand(uint32_t* s) {
    *s = *s * 1103515245 + 12345;
    return (*s >> 16) & 0x7FFF;
}

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* 1.12.18: hist[data[i]]++ for data[0..n) through the PE scratchpad
 * (pimid_spm.h). When the nbins counters fit, the RAM holds the whole
 * histogram (fetched once, written back once) and the rest of it is one
 * buffer for data tiles, each fetched once. Otherwise it holds two equal
 * buffers, a tile of the bins and a tile of data: each bin tile is fetched and
 * written back once and rescans all the data, counting the values that fall
 * in it. With no scratchpad the tiles are whole ranges and this is the plain
 * scatter loop. */
static void histogram_count(pimid_spm_t* s, int* hist, const int* data, size_t n,
                            size_t nbins) {
    const size_t T2 = pimid_spm_tile(s, 2, sizeof(int));
    const size_t hbytes = s->cap ? (nbins * sizeof(int) + 63) / 64 * 64 : 0;
    const int fits = !s->cap || hbytes + 64 <= s->cap;
    const size_t TB = fits ? nbins : T2;
    const size_t TD = (s->cap && fits) ? (s->cap - hbytes) / 64 * 64 / sizeof(int) : T2;
    const size_t oh = 0, od = s->cap ? (fits ? hbytes : T2 * sizeof(int)) : 0;
    for (size_t b0 = 0; b0 < nbins; b0 += TB) {
        const size_t nb = (nbins - b0 < TB) ? nbins - b0 : TB;
        int* hb = (int*)pimid_spm_in(s, oh, hist + b0, nb * sizeof(int));
        for (size_t i0 = 0; i0 < n; i0 += TD) {
            const size_t m = (n - i0 < TD) ? n - i0 : TD;
            const int* db = (const int*)pimid_spm_in(s, od, data + i0, m * sizeof(int));
            if (nb == nbins) {
                for (size_t i = 0; i < m; i++)
                    hb[db[i]]++;
            } else {
                for (size_t i = 0; i < m; i++) {
                    const size_t d = (size_t)db[i] - b0;
                    if (d < nb) hb[d]++;
                }
            }
        }
        pimid_spm_flush(s, hist + b0, hb, nb * sizeof(int));
    }
}

int main(int argc, char* argv[]) {
    int N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    int nbins = parse_int_arg(argc, argv, "--bins", DEFAULT_BINS);
    uint32_t seed = 42;

    int* data = (int*)malloc(N * sizeof(int));
    int* hist = (int*)calloc(nbins, sizeof(int));
    if (!data || !hist) { fprintf(stderr, "malloc failed\n"); return 1; }

    for (int i = 0; i < N; i++)
        data[i] = bench_rand(&seed) % nbins;

    pimid_spm_t spm;
    pimid_spm_open(&spm);
    zsim_roi_begin();
    /* 1.12.18: no scratchpad -- the 1.12.17 code, unchanged */
    if (!spm.cap) {
        for (int i = 0; i < N; i++)
            hist[data[i]]++;
    } else {
        histogram_count(&spm, hist, data, (size_t)N, (size_t)nbins);
    }
    zsim_roi_end();
    pimid_spm_close(&spm);

    long checksum = 0;
    for (int i = 0; i < nbins; i++) checksum += hist[i];
    printf("BENCH_CHECKSUM: %ld\n", checksum);
    printf("BENCH_DONE\n");

    free(data); free(hist);
    return 0;
}
