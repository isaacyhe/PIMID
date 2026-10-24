/* ft.c -- 3D FFT (NPB FT kernel)
 * Cooley-Tukey radix-2 1D FFTs along each dimension of a 3D grid,
 * then inverse FFT to verify round-trip correctness.
 * Memory pattern: strided butterfly access across 3 dimensions.
 *
 * Usage: ./ft [--size N] [--threads T]
 * Default: grid is 32^3, 1 thread */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>
#include "zsim_hooks.h"

#define DEFAULT_SIZE    32
#define DEFAULT_THREADS 1

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* Simple complex number type (C-compatible, no std::complex) */
typedef struct {
    double re, im;
} cplx;

static cplx cplx_add(cplx a, cplx b) {
    cplx c; c.re = a.re + b.re; c.im = a.im + b.im; return c;
}

static cplx cplx_sub(cplx a, cplx b) {
    cplx c; c.re = a.re - b.re; c.im = a.im - b.im; return c;
}

static cplx cplx_mul(cplx a, cplx b) {
    cplx c;
    c.re = a.re * b.re - a.im * b.im;
    c.im = a.re * b.im + a.im * b.re;
    return c;
}

/* In-place Cooley-Tukey radix-2 DIT FFT on n elements with given stride.
 * sign = -1 for forward, +1 for inverse. */
static void fft1d(cplx* data, int n, int stride, int sign) {
    int i, j, m, step;

    /* Bit-reversal permutation */
    j = 0;
    for (i = 0; i < n - 1; i++) {
        if (i < j) {
            cplx tmp = data[i * stride];
            data[i * stride] = data[j * stride];
            data[j * stride] = tmp;
        }
        m = n >> 1;
        while (m >= 1 && j >= m) {
            j -= m;
            m >>= 1;
        }
        j += m;
    }

    /* Butterfly stages */
    for (step = 2; step <= n; step <<= 1) {
        int half = step >> 1;
        double angle = (double)sign * 2.0 * M_PI / (double)step;
        cplx wn;
        wn.re = cos(angle);
        wn.im = sin(angle);

        for (i = 0; i < n; i += step) {
            cplx w;
            w.re = 1.0; w.im = 0.0;
            for (j = 0; j < half; j++) {
                int idx_e = (i + j) * stride;
                int idx_o = (i + j + half) * stride;
                cplx t = cplx_mul(w, data[idx_o]);
                cplx u = data[idx_e];
                data[idx_e] = cplx_add(u, t);
                data[idx_o] = cplx_sub(u, t);
                w = cplx_mul(w, wn);
            }
        }
    }

    /* Scale for inverse FFT */
    if (sign == 1) {
        double inv = 1.0 / (double)n;
        for (i = 0; i < n; i++) {
            data[i * stride].re *= inv;
            data[i * stride].im *= inv;
        }
    }
}

/* 3D FFT: apply 1D FFTs along each dimension in sequence.
 * Grid layout: data[i * N * N + j * N + k] for dimensions (x, y, z). */
static void fft3d(cplx* data, int N, int sign) {
    /* Pass 1: FFT along z (stride=1, length=N, for each (i,j)) */
    #pragma omp parallel for collapse(2) schedule(static)
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            fft1d(&data[(size_t)i * N * N + (size_t)j * N], N, 1, sign);
        }
    }

    /* Pass 2: FFT along y (stride=N, length=N, for each (i,k)) */
    #pragma omp parallel for collapse(2) schedule(static)
    for (int i = 0; i < N; i++) {
        for (int k = 0; k < N; k++) {
            fft1d(&data[(size_t)i * N * N + k], N, N, sign);
        }
    }

    /* Pass 3: FFT along x (stride=N*N, length=N, for each (j,k)) */
    #pragma omp parallel for collapse(2) schedule(static)
    for (int j = 0; j < N; j++) {
        for (int k = 0; k < N; k++) {
            fft1d(&data[(size_t)j * N + k], N, N * N, sign);
        }
    }
}

static uint32_t lcg_rand(uint32_t* s) {
    *s = *s * 1103515245u + 12345u;
    return (*s >> 16) & 0x7FFF;
}

int main(int argc, char* argv[]) {
    int N       = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    int threads = parse_int_arg(argc, argv, "--threads", DEFAULT_THREADS);

    if (threads < 1) threads = 1;
    omp_set_num_threads(threads);

    /* N must be a power of 2 */
    if (N < 2) N = 2;
    {
        int p = 1;
        while (p < N) p <<= 1;
        N = p;
    }

    size_t total = (size_t)N * N * N;
    cplx* data = (cplx*)malloc(total * sizeof(cplx));
    cplx* orig = (cplx*)malloc(total * sizeof(cplx));
    if (!data || !orig) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    /* Initialize with deterministic pseudo-random data */
    {
        uint32_t seed = 271828u;
        for (size_t idx = 0; idx < total; idx++) {
            data[idx].re = (double)(lcg_rand(&seed) % 1000) / 1000.0 - 0.5;
            data[idx].im = 0.0;
            orig[idx] = data[idx];
        }
    }

    /* --- ROI: Forward 3D FFT + Inverse 3D FFT --- */
    /* 1.12.18: the thread team starts before the ROI, as message-passing
     * ranks exist before it; the ROI measures the kernel, not the start-up. */
    #pragma omp parallel num_threads(threads)
    { }
    zsim_roi_begin();

    fft3d(data, N, -1);  /* forward */
    fft3d(data, N, +1);  /* inverse */

    zsim_roi_end();

    /* Verify round-trip: compare with original (outside ROI) */
    double max_err = 0.0;
    double checksum = 0.0;
    for (size_t idx = 0; idx < total; idx++) {
        double err_re = fabs(data[idx].re - orig[idx].re);
        double err_im = fabs(data[idx].im - orig[idx].im);
        double err = err_re > err_im ? err_re : err_im;
        if (err > max_err) max_err = err;
        checksum += data[idx].re;
    }

    printf("BENCH_GRID: %d^3 (%lu elements)\n", N, (unsigned long)total);
    printf("BENCH_MAX_ERROR: %e\n", max_err);
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(data);
    free(orig);
    return 0;
}
