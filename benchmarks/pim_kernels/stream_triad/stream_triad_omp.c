/* stream_triad_omp.c -- STREAM Triad: a[i] = b[i] + scalar * c[i]
 * OpenMP parallel version.
 *
 * Device-organization aware (PIMID): the simulator prices each PE access by
 * LOCATION (its own unit = fast, elsewhere = far). To get near-data speed the
 * benchmark must EXPLICITLY relocate each PE's slice into that PE's own unit --
 * the simulator never moves data for you. We do that here, before the ROI, from
 * the device org (placement level, PE count, units, pages-per-unit) the harness
 * passes on the command line. The relocate is plain setup (not timed); only the
 * compute ROI is measured. Coarse/host-shared placement needs no relocate. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include "zsim_hooks.h"
#include "../../include/pimid_devorg.h"
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

/* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): per-PE need of the prep layout
 * [a chunk][b chunk][c chunk] for size N on P PEs (chunk = ceil(N/P)). */
static size_t stream_triad_omp_need(long n, const void* ctx) {
    long P = *(const int*)ctx;
    return 3u * (size_t)((n + P - 1) / P) * sizeof(float);
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
    float scalar = 3.0f;
    pimid_devorg_t dev = pimid_devorg_from_args(argc, argv);
    int P = dev.num_pes;

    float* a = (float*)malloc(N * sizeof(float));
    float* b = (float*)malloc(N * sizeof(float));
    float* c = (float*)malloc(N * sizeof(float));
    if (!a || !b || !c) { fprintf(stderr, "malloc failed\n"); return 1; }

    int prep = pimid_devorg_needs_prep(&dev);
    int chunk = (N + P - 1) / P;

    /* 1.12.18: each thread opens its PE's scratchpad before the ROI. */
    if (prep) omp_set_num_threads(P);
    int T = prep ? P : omp_get_max_threads();
    pimid_spm_t* spms = (pimid_spm_t*)calloc((size_t)T, sizeof(pimid_spm_t));
    if (!spms) { fprintf(stderr, "malloc failed\n"); return 1; }
    #pragma omp parallel num_threads(T)
    pimid_spm_open(&spms[omp_get_thread_num()]);

    /* 1.12.18: first-touch placement -- each thread initialises the share it computes on, so its pages live in its PE's memory. */
    /* Host layout: the ROI's split, [N*t/T, N*(t+1)/T) with a scratchpad,
     * else schedule(static) over T threads. Devorg prep: PE pe's chunk
     * [pe*chunk, (pe+1)*chunk), which thread pe copies into its slot. a, which
     * the ROI writes, is touched here too (every element is overwritten). */
    #pragma omp parallel num_threads(T)
    {
        int lo, hi;
        ft_share(N, omp_get_thread_num(), omp_get_num_threads(), prep ? chunk : 0,
                 spms[0].cap != 0, &lo, &hi);
        for (int i = lo; i < hi; i++) {
            a[i] = 0.0f;
            b[i] = 1.0f;
            c[i] = 2.0f;
        }
    }

    if (prep) {
        /* ---- DATA PREP (device-org aware, before ROI, not timed) ----
         * a, b, c are each PARTITIONED by element range: PE pe owns elements
         * [pe*chunk, (pe+1)*chunk). Nothing is read by all PEs (the scalar is a
         * plain register value), so there is no replication. Relocate PE pe's
         * chunk of b + c into PE pe's own unit slot; a's chunk is written there
         * by the ROI. Slot layout: [a chunk][b chunk][c chunk]. */
        omp_set_num_threads(P);
        /* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): refuse when PE pe's three
         * chunks do not fit its slot (c overlapped PE pe+1's a before). */
        size_t slot_bytes = pimid_devorg_slot_bytes(&dev);
        size_t need = stream_triad_omp_need(N, &P);
        if (need > slot_bytes) {
            pimid_devorg_report_slot_refusal("", "stream_triad", &dev, need, slot_bytes, "--size",
                pimid_devorg_largest_fit(stream_triad_omp_need, &P, N, slot_bytes));
            return 1;
        }
        float* dbuf = (float*)pimid_devorg_alloc(&dev, &slot_bytes);
        if (!dbuf) { fprintf(stderr, "devorg alloc failed\n"); return 1; }
        /* 1.12.18: first-touch placement -- each thread initialises the share it computes on, so its pages live in its PE's memory. */
        /* Thread pe fills PE pe's slot (the ROI's schedule(static, 1) over pe
         * runs PE pe's chunk on thread pe). The a chunk is zeroed here so the
         * whole slot is first touched by its PE; the ROI overwrites it. */
        #pragma omp parallel num_threads(P)
        for (int pe = omp_get_thread_num(); pe < P; pe += omp_get_num_threads()) {
            int i0 = pe * chunk;
            int i1 = i0 + chunk; if (i1 > N) i1 = N;
            if (i0 >= N) continue;
            float* ape = (float*)pimid_devorg_pe_slot(dbuf, pe, slot_bytes);
            float* bpe = ape + (size_t)chunk;          /* b chunk after a chunk */
            float* cpe = bpe + (size_t)chunk;          /* c chunk after b chunk */
            memset(ape, 0, (size_t)(i1 - i0) * sizeof(float));
            memcpy(bpe, b + i0, (size_t)(i1 - i0) * sizeof(float));
            memcpy(cpe, c + i0, (size_t)(i1 - i0) * sizeof(float));
        }

        fprintf(stderr, "[devorg] PREP ACTIVE: kernel=stream_triad level=%s pes=%d "
                "slot_bytes=%zu\n",
                pimid_devorg_level_name(dev.level), P, slot_bytes);
        zsim_roi_begin();
        /* 1.12.18: no scratchpad -- the 1.12.17 code, unchanged */
        if (!spms[0].cap) {
            #pragma omp parallel for schedule(static, 1)
            for (int pe = 0; pe < P; pe++) {
                int i0 = pe * chunk;
                int i1 = i0 + chunk; if (i1 > N) i1 = N;
                float* ape = (float*)pimid_devorg_pe_slot(dbuf, pe, slot_bytes);
                float* bpe = ape + (size_t)chunk;
                float* cpe = bpe + (size_t)chunk;
                for (int i = i0; i < i1; i++) {
                    int k = i - i0;
                    ape[k] = bpe[k] + scalar * cpe[k];
                }
            }
        } else {
            #pragma omp parallel for schedule(static, 1)
            for (int pe = 0; pe < P; pe++) {
                int i0 = pe * chunk;
                int i1 = i0 + chunk; if (i1 > N) i1 = N;
                float* ape = (float*)pimid_devorg_pe_slot(dbuf, pe, slot_bytes);
                float* bpe = ape + (size_t)chunk;
                float* cpe = bpe + (size_t)chunk;
                if (i1 > i0)
                    stream_triad_range(&spms[omp_get_thread_num()], ape, bpe, cpe, scalar, (size_t)(i1 - i0));
            }
        }
        zsim_roi_end();

        /* Gather each PE's chunk of a back to host layout for the checksum
         * (post-ROI, not timed). */
        for (int pe = 0; pe < P; pe++) {
            int i0 = pe * chunk;
            int i1 = i0 + chunk; if (i1 > N) i1 = N;
            if (i0 >= N) continue;
            float* ape = (float*)pimid_devorg_pe_slot(dbuf, pe, slot_bytes);
            memcpy(a + i0, ape, (size_t)(i1 - i0) * sizeof(float));
        }
        (void)dbuf;   /* dbuf's raw alloc is freed at process exit */
    } else {
        /* Coarse / host-shared placement: no relocate; run on host layout. */
        zsim_roi_begin();
        /* 1.12.18: no scratchpad -- the 1.12.17 code, unchanged */
        if (!spms[0].cap) {
            #pragma omp parallel for
            for (int i = 0; i < N; i++)
                a[i] = b[i] + scalar * c[i];
        } else {
            #pragma omp parallel num_threads(T)
            {
                const int t = omp_get_thread_num();
                const int i0 = (int)(((long)N * t) / T), i1 = (int)(((long)N * (t + 1)) / T);
                if (i1 > i0)
                    stream_triad_range(&spms[t], a + i0, b + i0, c + i0, scalar, (size_t)(i1 - i0));
            }
        }
        zsim_roi_end();
    }

    #pragma omp parallel num_threads(T)
    pimid_spm_close(&spms[omp_get_thread_num()]);
    free(spms);

    double checksum = 0.0;
    for (int i = 0; i < N; i++) checksum += a[i];
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(a); free(b); free(c);
    return 0;
}
