/* gemv_omp.c -- General Matrix-Vector Multiply: y = A * x
 * OpenMP parallel version. A is N*N, x and y are N-vectors.
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

/* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): per-PE need of the prep layout
 * [A rows][x] for size N on P PEs (largest share = ceil(N/P) rows). */
static size_t gemv_omp_need(long n, const void* ctx) {
    long P = *(const int*)ctx;
    size_t rows = (size_t)((n + P - 1) / P);
    return (rows * (size_t)n + (size_t)n) * sizeof(float);
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
    pimid_devorg_t dev = pimid_devorg_from_args(argc, argv);
    int P = dev.num_pes;

    float* A = (float*)malloc((size_t)N * N * sizeof(float));
    float* x = (float*)malloc((size_t)N * sizeof(float));
    float* y = (float*)malloc((size_t)N * sizeof(float));   /* zeroed below */
    if (!A || !x || !y) { fprintf(stderr, "malloc failed\n"); return 1; }

    int prep = pimid_devorg_needs_prep(&dev);
    int rows_per_pe = (N + P - 1) / P;

    /* 1.12.18: each thread opens its PE's scratchpad before the ROI. */
    if (prep) omp_set_num_threads(P);
    int T = prep ? P : omp_get_max_threads();
    pimid_spm_t* spms = (pimid_spm_t*)calloc((size_t)T, sizeof(pimid_spm_t));
    if (!spms) { fprintf(stderr, "malloc failed\n"); return 1; }
    #pragma omp parallel num_threads(T)
    pimid_spm_open(&spms[omp_get_thread_num()]);

    /* 1.12.18: first-touch placement -- each thread initialises the share it computes on, so its pages live in its PE's memory. */
    /* Rows of A and y, split as the ROI splits them: host layout
     * [N*t/T, N*(t+1)/T) with a scratchpad, else schedule(static) over T
     * threads; devorg prep: PE pe's row block [pe*rows_per_pe, ...), which
     * thread pe copies into its slot and whose y rows it writes. x is read
     * whole by every thread; it follows the row split (x[i] with row i). */
    #pragma omp parallel num_threads(T)
    {
        int lo, hi;
        ft_share(N, omp_get_thread_num(), omp_get_num_threads(), prep ? rows_per_pe : 0,
                 spms[0].cap != 0, &lo, &hi);
        for (int i = lo; i < hi; i++) {
            x[i] = 1.0f;
            y[i] = 0.0f;
            for (int j = 0; j < N; j++)
                A[(size_t)i * N + j] = (float)((i + j) % 7);
        }
    }

    if (prep) {
        /* ---- DATA PREP (device-org aware, before ROI, not timed) ----
         * Relocate PE pe's rows of A + a private copy of x into PE pe's own unit
         * slot, so the ROI's reads are LOCAL. Slot layout: [A rows][x]. */
        omp_set_num_threads(P);
        /* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): refuse when PE pe's rows + x
         * do not fit its slot (they spilled into the next PE's slot before). */
        size_t slot_bytes = pimid_devorg_slot_bytes(&dev);
        size_t need = gemv_omp_need(N, &P);
        if (need > slot_bytes) {
            pimid_devorg_report_slot_refusal("", "gemv", &dev, need, slot_bytes, "--size",
                pimid_devorg_largest_fit(gemv_omp_need, &P, N, slot_bytes));
            return 1;
        }
        float* Adev = (float*)pimid_devorg_alloc(&dev, &slot_bytes);
        if (!Adev) { fprintf(stderr, "devorg alloc failed\n"); return 1; }
        size_t slot_floats = slot_bytes / sizeof(float);
        /* 1.12.18: first-touch placement -- each thread initialises the share it computes on, so its pages live in its PE's memory. */
        /* Thread pe fills PE pe's slot (the ROI's schedule(static, 1) over pe
         * runs PE pe's rows on thread pe). */
        #pragma omp parallel num_threads(P)
        for (int pe = omp_get_thread_num(); pe < P; pe += omp_get_num_threads()) {
            int r0 = pe * rows_per_pe;
            int r1 = r0 + rows_per_pe; if (r1 > N) r1 = N;
            if (r0 >= N) continue;
            float* Ape = (float*)pimid_devorg_pe_slot(Adev, pe, slot_bytes);
            float* xpe = Ape + (size_t)rows_per_pe * N;   /* x copy after A rows */
            memcpy(Ape, A + (size_t)r0 * N, (size_t)(r1 - r0) * N * sizeof(float));
            memcpy(xpe, x, (size_t)N * sizeof(float));
        }

        fprintf(stderr, "[devorg] PREP ACTIVE: kernel=gemv level=%s pes=%d "
                "slot_bytes=%zu\n",
                pimid_devorg_level_name(dev.level), P, slot_bytes);
        zsim_roi_begin();
        /* 1.12.18: no scratchpad -- the 1.12.17 code, unchanged */
        if (!spms[0].cap) {
            #pragma omp parallel for schedule(static, 1)
            for (int pe = 0; pe < P; pe++) {
                int r0 = pe * rows_per_pe;
                int r1 = r0 + rows_per_pe; if (r1 > N) r1 = N;
                float* Ape = (float*)pimid_devorg_pe_slot(Adev, pe, slot_bytes);
                float* xpe = Ape + (size_t)rows_per_pe * N;
                for (int i = r0; i < r1; i++) {
                    float sum = 0.0f;
                    const float* row = Ape + (size_t)(i - r0) * N;
                    for (int j = 0; j < N; j++)
                        sum += row[j] * xpe[j];
                    y[i] = sum;
                }
            }
        } else {
            #pragma omp parallel for schedule(static, 1)
            for (int pe = 0; pe < P; pe++) {
                int r0 = pe * rows_per_pe;
                int r1 = r0 + rows_per_pe; if (r1 > N) r1 = N;
                float* Ape = (float*)pimid_devorg_pe_slot(Adev, pe, slot_bytes);
                float* xpe = Ape + (size_t)rows_per_pe * N;
                if (r1 > r0)
                    gemv_rows(&spms[omp_get_thread_num()], Ape, xpe, y + r0, (size_t)(r1 - r0), (size_t)N);
            }
        }
        zsim_roi_end();
        (void)slot_floats; (void)Adev;   /* Adev's raw alloc is freed at process exit */
    } else {
        /* Coarse / host-shared placement: no relocate; run on host layout. */
        zsim_roi_begin();
        /* 1.12.18: no scratchpad -- the 1.12.17 code, unchanged */
        if (!spms[0].cap) {
            #pragma omp parallel for
            for (int i = 0; i < N; i++) {
                float sum = 0.0f;
                for (int j = 0; j < N; j++)
                    sum += A[(size_t)i * N + j] * x[j];
                y[i] = sum;
            }
        } else {
            #pragma omp parallel num_threads(T)
            {
                const int t = omp_get_thread_num();
                const int r0 = (int)(((long)N * t) / T), r1 = (int)(((long)N * (t + 1)) / T);
                if (r1 > r0)
                    gemv_rows(&spms[t], A + (size_t)r0 * N, x, y + r0, (size_t)(r1 - r0), (size_t)N);
            }
        }
        zsim_roi_end();
    }

    #pragma omp parallel num_threads(T)
    pimid_spm_close(&spms[omp_get_thread_num()]);
    free(spms);

    double checksum = 0.0;
    for (int i = 0; i < N; i++) checksum += y[i];
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(A); free(x); free(y);
    return 0;
}
