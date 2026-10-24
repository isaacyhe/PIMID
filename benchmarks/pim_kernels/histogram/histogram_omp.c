/* histogram_omp.c -- Histogram construction: random scatter into bins
 * OpenMP parallel version. Private histograms per thread, merged at end.
 *
 * Device-organization aware (PIMID): the simulator prices each PE access by
 * LOCATION (own unit = fast, elsewhere = far). Near-data speed requires the
 * benchmark to EXPLICITLY relocate each PE's slice into that PE's own unit --
 * the simulator never moves data. Prep here (before the ROI, untimed):
 * PE p's slot holds [its input chunk][its private histogram]; the ROI scan is
 * then PE-local, and only the final merge into the shared host histogram
 * crosses. 1.12.18 (ruling 2026-10-23): that merge is PARALLEL -- after a
 * barrier, thread t sums ITS slice of the bins across every thread's partial
 * histogram (no omp critical). Coarse/host-shared placement runs the host
 * layout. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include "zsim_hooks.h"
#include "../../include/pimid_devorg.h"
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

/* 1.12.18 (ruling 2026-10-23): the parallel merge of one thread's slice
 * hist[b0, b0 + n) = sum over q < np of parts[q][b0, b0 + n), through the PE
 * scratchpad. The RAM holds two equal buffers: a tile of the result (summed in
 * the RAM, written back once) and a tile of one partial (each fetched once).
 * The counts are integers, so the result is bit-identical to any order of the
 * adds (the omp critical merge it replaces). */
static void histogram_merge(pimid_spm_t* s, int* hist, int* const* parts, int np,
                            size_t b0, size_t n) {
    const size_t T2 = pimid_spm_tile(s, 2, sizeof(int));
    const size_t op = s->cap ? T2 * sizeof(int) : 0;
    for (size_t o = 0; o < n; o += T2) {
        const size_t m = (n - o < T2) ? n - o : T2;
        int* acc = (int*)pimid_spm_out(s, 0, hist + b0 + o);
        for (size_t i = 0; i < m; i++)
            acc[i] = 0;
        for (int q = 0; q < np; q++) {
            const int* pb = (const int*)pimid_spm_in(s, op, parts[q] + b0 + o, m * sizeof(int));
            for (size_t i = 0; i < m; i++)
                acc[i] += pb[i];
        }
        pimid_spm_flush(s, hist + b0 + o, acc, m * sizeof(int));
    }
}

/* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): per-PE need of [chunk][hist]. */
typedef struct { int P; int nbins; } histogram_omp_ctx;
static size_t histogram_omp_need(long n, const void* c) {
    const histogram_omp_ctx* x = (const histogram_omp_ctx*)c;
    return ((size_t)((n + x->P - 1) / x->P) + (size_t)x->nbins) * sizeof(int);
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
    int nbins = parse_int_arg(argc, argv, "--bins", DEFAULT_BINS);
    uint32_t seed = 42;
    pimid_devorg_t dev = pimid_devorg_from_args(argc, argv);
    int P = dev.num_pes;

    int* data = (int*)malloc((size_t)N * sizeof(int));
    int* hist = (int*)calloc(nbins, sizeof(int));
    if (!data || !hist) { fprintf(stderr, "malloc failed\n"); return 1; }

    int prep = pimid_devorg_needs_prep(&dev);
    int chunk = (N + P - 1) / P;
    size_t need = ((size_t)chunk + (size_t)nbins) * sizeof(int);
    histogram_omp_ctx need_ctx = { P, nbins };

    size_t slot_bytes = 0;
    char* base = NULL;
    if (prep) {
        /* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): a need larger than the slot
         * is refused; it used to fall back to host layout in silence. */
        slot_bytes = pimid_devorg_slot_bytes(&dev);
        if (need > slot_bytes) {
            pimid_devorg_report_slot_refusal("", "histogram", &dev, need, slot_bytes, "--size",
                pimid_devorg_largest_fit(histogram_omp_need, &need_ctx, N, slot_bytes));
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

    /* 1.12.18 (ruling 2026-10-23): parts[q] = partial histogram q, read by
     * every thread in the parallel merge. Devorg prep: PE q's slot histogram
     * (filled here, before the ROI) for the np PEs that hold data; host
     * layout: each thread's own calloc'd histogram, published in the ROI. */
    int** parts = (int**)calloc((size_t)T, sizeof(int*));
    if (!parts) { fprintf(stderr, "malloc failed\n"); return 1; }

    /* 1.12.18: first-touch placement -- each thread initialises the share it computes on, so its pages live in its PE's memory. */
    /* data is split as the ROI splits it: host layout [N*t/T, N*(t+1)/T)
     * with a scratchpad, else schedule(static) over T threads; devorg prep:
     * PE p's chunk [p*chunk, (p+1)*chunk), which thread p copies into its
     * slot. The values come from one sequential random stream, so each thread
     * first touches (zeroes) its share and the master then writes the values
     * into the placed pages. hist (nbins shared counters, merged by every
     * thread) is not partitioned and stays calloc'd. */
    #pragma omp parallel num_threads(T)
    {
        int lo, hi;
        ft_share(N, omp_get_thread_num(), omp_get_num_threads(), prep ? chunk : 0,
                 spms[0].cap != 0, &lo, &hi);
        if (hi > lo)
            memset(data + lo, 0, (size_t)(hi - lo) * sizeof(int));
    }
    for (int i = 0; i < N; i++)
        data[i] = bench_rand(&seed) % nbins;

    if (prep) {
        /* ---- DATA PREP (before ROI, not timed): PE p's slot = [chunk][hist] */
        omp_set_num_threads(P);
        /* 1.12.18: first-touch placement -- each thread initialises the share it computes on, so its pages live in its PE's memory. */
        /* Thread p fills PE p's slot (the ROI's schedule(static, 1) over p runs
         * PE p's chunk on thread p). */
        #pragma omp parallel num_threads(P)
        for (int p = omp_get_thread_num(); p < P; p += omp_get_num_threads()) {
            int i0 = p * chunk, i1 = i0 + chunk; if (i1 > N) i1 = N;
            if (i0 >= N) continue;
            int* dpe = (int*)pimid_devorg_pe_slot(base, p, slot_bytes);
            memcpy(dpe, data + i0, (size_t)(i1 - i0) * sizeof(int));
            memset(dpe + chunk, 0, (size_t)nbins * sizeof(int));  /* local hist */
        }

        fprintf(stderr, "[devorg] PREP ACTIVE: kernel=histogram level=%s pes=%d "
                "slot_bytes=%zu\n",
                pimid_devorg_level_name(dev.level), P, slot_bytes);
        /* 1.12.18: the PEs that hold data (the others have no partial). */
        int np = chunk ? (N + chunk - 1) / chunk : 0;
        if (np > P) np = P;
        for (int q = 0; q < np; q++)
            parts[q] = (int*)pimid_devorg_pe_slot(base, q, slot_bytes) + chunk;
        zsim_roi_begin();
        /* 1.12.18: no scratchpad -- the 1.12.17 count (thread p runs PE p's
         * chunk, as schedule(static, 1) gave it); then a barrier and the
         * PARALLEL merge: thread t sums ITS slice of the bins across the np
         * partials (ruling 2026-10-23; was an omp critical loop). */
        if (!spms[0].cap) {
            #pragma omp parallel num_threads(P)
            {
                const int t = omp_get_thread_num(), nt = omp_get_num_threads();
                for (int p = t; p < P; p += nt) {
                    int i0 = p * chunk, i1 = i0 + chunk; if (i1 > N) i1 = N;
                    if (i0 >= N) continue;
                    int* dpe = (int*)pimid_devorg_pe_slot(base, p, slot_bytes);
                    int* local_hist = dpe + chunk;
                    int n = i1 - i0;
                    for (int i = 0; i < n; i++)
                        local_hist[dpe[i]]++;
                }
                #pragma omp barrier
                const int b0 = (int)(((long)nbins * t) / nt), b1 = (int)(((long)nbins * (t + 1)) / nt);
                for (int b = b0; b < b1; b++) {
                    int s = 0;
                    for (int q = 0; q < np; q++)
                        s += parts[q][b];
                    hist[b] = s;
                }
            }
        } else {
            /* 1.12.18: scratchpad -- the same count and parallel merge, each
             * through the thread's PE scratchpad. */
            #pragma omp parallel num_threads(P)
            {
                const int t = omp_get_thread_num(), nt = omp_get_num_threads();
                for (int p = t; p < P; p += nt) {
                    int i0 = p * chunk, i1 = i0 + chunk; if (i1 > N) i1 = N;
                    if (i0 >= N) continue;
                    int* dpe = (int*)pimid_devorg_pe_slot(base, p, slot_bytes);
                    int* local_hist = dpe + chunk;
                    int n = i1 - i0;
                    histogram_count(&spms[t], local_hist, dpe, (size_t)n, (size_t)nbins);
                }
                #pragma omp barrier
                const int b0 = (int)(((long)nbins * t) / nt), b1 = (int)(((long)nbins * (t + 1)) / nt);
                if (b1 > b0)
                    histogram_merge(&spms[t], hist, parts, np, (size_t)b0, (size_t)(b1 - b0));
            }
        }
        zsim_roi_end();
    } else {
        zsim_roi_begin();
        /* 1.12.18: no scratchpad -- the 1.12.17 count; each thread publishes
         * its private histogram in parts[], and after the barrier the merge is
         * PARALLEL: thread t sums ITS slice of the bins across all threads'
         * partials (ruling 2026-10-23; was an omp critical loop). A second
         * barrier keeps every partial alive until all slices are summed. */
        if (!spms[0].cap) {
            #pragma omp parallel num_threads(T)
            {
                const int t = omp_get_thread_num(), nt = omp_get_num_threads();
                int* local_hist = (int*)calloc(nbins, sizeof(int));
                parts[t] = local_hist;

                #pragma omp for
                for (int i = 0; i < N; i++)
                    local_hist[data[i]]++;
                /* (the implicit barrier of omp for: every partial is complete) */

                const int b0 = (int)(((long)nbins * t) / nt), b1 = (int)(((long)nbins * (t + 1)) / nt);
                for (int b = b0; b < b1; b++) {
                    int s = 0;
                    for (int q = 0; q < nt; q++)
                        s += parts[q][b];
                    hist[b] = s;
                }

                #pragma omp barrier
                free(local_hist);
            }
        } else {
            /* 1.12.18: scratchpad -- the same count and parallel merge, each
             * through the thread's PE scratchpad. */
            #pragma omp parallel num_threads(T)
            {
                const int t = omp_get_thread_num(), nt = omp_get_num_threads();
                int* local_hist = (int*)calloc(nbins, sizeof(int));
                parts[t] = local_hist;

                const int i0 = (int)(((long)N * t) / T), i1 = (int)(((long)N * (t + 1)) / T);
                if (i1 > i0)
                    histogram_count(&spms[t], local_hist, data + i0, (size_t)(i1 - i0), (size_t)nbins);

                #pragma omp barrier
                const int b0 = (int)(((long)nbins * t) / nt), b1 = (int)(((long)nbins * (t + 1)) / nt);
                if (b1 > b0)
                    histogram_merge(&spms[t], hist, parts, nt, (size_t)b0, (size_t)(b1 - b0));

                #pragma omp barrier
                free(local_hist);
            }
        }
        zsim_roi_end();
    }

    #pragma omp parallel num_threads(T)
    pimid_spm_close(&spms[omp_get_thread_num()]);
    free(spms);
    free(parts);

    long checksum = 0;
    for (int i = 0; i < nbins; i++) checksum += hist[i];
    printf("BENCH_CHECKSUM: %ld\n", checksum);
    printf("BENCH_DONE\n");

    free(data); free(hist);
    return 0;
}
