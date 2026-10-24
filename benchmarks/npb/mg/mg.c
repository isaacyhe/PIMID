/* mg.c -- Multi-Grid V-cycle (NPB MG kernel)
 * V-cycle multigrid on a 3D grid: restrict -> smooth -> prolongate -> smooth.
 * Memory pattern: 3D stencil at multiple scales.
 *
 * Usage: ./mg [--size N] [--threads T]
 * Default: grid is 32^3, 1 thread */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>
#include "zsim_hooks.h"

#define DEFAULT_SIZE    32
#define DEFAULT_THREADS 1
#define V_CYCLES        4       /* number of V-cycles */
#define SMOOTH_ITERS    3       /* weighted Jacobi smoothing sweeps per level */
#define OMEGA           0.8     /* Jacobi relaxation weight */

/* Maximum multigrid levels (32^3 -> 16^3 -> 8^3 -> 4^3 = 4 levels) */
#define MAX_LEVELS 8

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* 3D indexing macro */
#define IDX(i, j, k, n) ((size_t)(i) * (n) * (n) + (size_t)(j) * (n) + (size_t)(k))

/* Weighted Jacobi smoothing on a 3D grid of size n^3.
 * Solves A*u = f where A is the 7-point Laplacian. */
static void smooth(double* u, const double* f, double* tmp, int n) {
    int it;
    for (it = 0; it < SMOOTH_ITERS; it++) {
        #pragma omp parallel for collapse(2) schedule(static)
        for (int i = 1; i < n - 1; i++) {
            for (int j = 1; j < n - 1; j++) {
                for (int k = 1; k < n - 1; k++) {
                    double neighbors =
                        u[IDX(i-1,j,k,n)] + u[IDX(i+1,j,k,n)] +
                        u[IDX(i,j-1,k,n)] + u[IDX(i,j+1,k,n)] +
                        u[IDX(i,j,k-1,n)] + u[IDX(i,j,k+1,n)];
                    tmp[IDX(i,j,k,n)] = (1.0 - OMEGA) * u[IDX(i,j,k,n)]
                        + OMEGA * (f[IDX(i,j,k,n)] + neighbors) / 6.0;
                }
            }
        }
        /* Swap u <-> tmp */
        {
            double* s = u;
            /* Copy tmp back to u for the next iteration or final result.
             * We copy rather than swap pointers since the caller owns u. */
            size_t sz = (size_t)n * n * n;
            memcpy(u, tmp, sz * sizeof(double));
            (void)s;
        }
    }
}

/* Compute residual: r = f - A*u (7-point Laplacian) */
static void residual(double* r, const double* u, const double* f, int n) {
    #pragma omp parallel for collapse(2) schedule(static)
    for (int i = 1; i < n - 1; i++) {
        for (int j = 1; j < n - 1; j++) {
            for (int k = 1; k < n - 1; k++) {
                double Au = 6.0 * u[IDX(i,j,k,n)]
                    - u[IDX(i-1,j,k,n)] - u[IDX(i+1,j,k,n)]
                    - u[IDX(i,j-1,k,n)] - u[IDX(i,j+1,k,n)]
                    - u[IDX(i,j,k-1,n)] - u[IDX(i,j,k+1,n)];
                r[IDX(i,j,k,n)] = f[IDX(i,j,k,n)] - Au;
            }
        }
    }
    /* Zero boundaries of r */
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            r[IDX(0,j,i,n)] = 0; r[IDX(n-1,j,i,n)] = 0;
            r[IDX(i,0,j,n)] = 0; r[IDX(i,n-1,j,n)] = 0;
            r[IDX(i,j,0,n)] = 0; r[IDX(i,j,n-1,n)] = 0;
        }
}

/* Full-weighting restriction: fine (nf^3) -> coarse (nc^3), nc = nf/2 */
static void restrict_grid(double* coarse, const double* fine, int nf) {
    int nc = nf / 2;
    #pragma omp parallel for collapse(2) schedule(static)
    for (int i = 1; i < nc - 1; i++) {
        for (int j = 1; j < nc - 1; j++) {
            for (int k = 1; k < nc - 1; k++) {
                int fi = 2 * i, fj = 2 * j, fk = 2 * k;
                /* Simple injection with nearest-neighbor average */
                coarse[IDX(i,j,k,nc)] = 0.125 * (
                    fine[IDX(fi,fj,fk,nf)]     + fine[IDX(fi+1,fj,fk,nf)] +
                    fine[IDX(fi,fj+1,fk,nf)]   + fine[IDX(fi+1,fj+1,fk,nf)] +
                    fine[IDX(fi,fj,fk+1,nf)]   + fine[IDX(fi+1,fj,fk+1,nf)] +
                    fine[IDX(fi,fj+1,fk+1,nf)] + fine[IDX(fi+1,fj+1,fk+1,nf)]);
            }
        }
    }
}

/* Trilinear prolongation: coarse (nc^3) -> fine (nf^3), nf = 2*nc */
static void prolongate(double* fine, const double* coarse, int nc) {
    int nf = 2 * nc;
    #pragma omp parallel for collapse(2) schedule(static)
    for (int i = 1; i < nc - 1; i++) {
        for (int j = 1; j < nc - 1; j++) {
            for (int k = 1; k < nc - 1; k++) {
                double val = coarse[IDX(i,j,k,nc)];
                int fi = 2 * i, fj = 2 * j, fk = 2 * k;
                /* Simple injection: add correction to all 8 fine-grid children */
                fine[IDX(fi,  fj,  fk,  nf)] += val;
                fine[IDX(fi+1,fj,  fk,  nf)] += val;
                fine[IDX(fi,  fj+1,fk,  nf)] += val;
                fine[IDX(fi+1,fj+1,fk,  nf)] += val;
                fine[IDX(fi,  fj,  fk+1,nf)] += val;
                fine[IDX(fi+1,fj,  fk+1,nf)] += val;
                fine[IDX(fi,  fj+1,fk+1,nf)] += val;
                fine[IDX(fi+1,fj+1,fk+1,nf)] += val;
            }
        }
    }
}

int main(int argc, char* argv[]) {
    int N       = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    int threads = parse_int_arg(argc, argv, "--threads", DEFAULT_THREADS);

    if (threads < 1) threads = 1;
    omp_set_num_threads(threads);

    /* N must be a power of 2 and >= 4 */
    if (N < 4) N = 4;
    {
        int p = 1;
        while (p < N) p <<= 1;
        N = p;
    }

    /* Count levels */
    int nlevels = 0;
    {
        int sz = N;
        while (sz >= 4 && nlevels < MAX_LEVELS) {
            nlevels++;
            sz /= 2;
        }
    }

    /* Allocate grids for each level:
     * u[level] = solution, f[level] = RHS, r[level] = residual, tmp[level] = scratch */
    double* u[MAX_LEVELS];
    double* f[MAX_LEVELS];
    double* r[MAX_LEVELS];
    double* tmp[MAX_LEVELS];
    int sizes[MAX_LEVELS];
    {
        int sz = N;
        for (int lev = 0; lev < nlevels; lev++) {
            sizes[lev] = sz;
            size_t total = (size_t)sz * sz * sz;
            u[lev]   = (double*)calloc(total, sizeof(double));
            f[lev]   = (double*)calloc(total, sizeof(double));
            r[lev]   = (double*)calloc(total, sizeof(double));
            tmp[lev] = (double*)calloc(total, sizeof(double));
            if (!u[lev] || !f[lev] || !r[lev] || !tmp[lev]) {
                fprintf(stderr, "malloc failed at level %d (size %d)\n", lev, sz);
                return 1;
            }
            sz /= 2;
        }
    }

    /* Initialize RHS: f[0] with a simple source term */
    {
        int n = sizes[0];
        double h = 1.0 / (double)(n - 1);
        for (int i = 1; i < n - 1; i++)
            for (int j = 1; j < n - 1; j++)
                for (int k = 1; k < n - 1; k++) {
                    double x = i * h, y = j * h, z = k * h;
                    f[0][IDX(i,j,k,n)] = sin(3.14159 * x) * sin(3.14159 * y) * sin(3.14159 * z);
                }
    }

    /* --- ROI: V-cycle multigrid iterations --- */
    /* 1.12.18: the thread team starts before the ROI, as message-passing
     * ranks exist before it; the ROI measures the kernel, not the start-up. */
    #pragma omp parallel num_threads(threads)
    { }
    zsim_roi_begin();

    for (int vc = 0; vc < V_CYCLES; vc++) {
        /* Downward leg: smooth + compute residual + restrict */
        for (int lev = 0; lev < nlevels - 1; lev++) {
            int n = sizes[lev];
            smooth(u[lev], f[lev], tmp[lev], n);
            residual(r[lev], u[lev], f[lev], n);
            restrict_grid(f[lev + 1], r[lev], n);
            /* Zero the coarse-grid solution before solving */
            memset(u[lev + 1], 0, (size_t)sizes[lev+1] * sizes[lev+1] * sizes[lev+1] * sizeof(double));
        }

        /* Solve on coarsest level (just smooth many times) */
        {
            int lev = nlevels - 1;
            int n = sizes[lev];
            for (int s = 0; s < SMOOTH_ITERS * 4; s++) {
                smooth(u[lev], f[lev], tmp[lev], n);
            }
        }

        /* Upward leg: prolongate + smooth */
        for (int lev = nlevels - 2; lev >= 0; lev--) {
            prolongate(u[lev], u[lev + 1], sizes[lev + 1]);
            smooth(u[lev], f[lev], tmp[lev], sizes[lev]);
        }
    }

    zsim_roi_end();

    /* Compute norm and checksum outside ROI */
    {
        int n = sizes[0];
        double norm = 0.0, checksum = 0.0;
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
                for (int k = 0; k < n; k++) {
                    double v = u[0][IDX(i,j,k,n)];
                    norm += v * v;
                    checksum += v;
                }
        norm = sqrt(norm);
        printf("BENCH_NORM: %e\n", norm);
        printf("BENCH_CHECKSUM: %f\n", checksum);
    }
    printf("BENCH_DONE\n");

    for (int lev = 0; lev < nlevels; lev++) {
        free(u[lev]); free(f[lev]); free(r[lev]); free(tmp[lev]);
    }
    return 0;
}
