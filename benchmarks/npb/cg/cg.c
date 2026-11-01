/* cg.c -- Conjugate Gradient (NPB CG kernel)
 * Sparse matrix-vector multiply with CG iterations on a symmetric
 * positive-definite matrix in CSR format.
 * Memory pattern: indirect gather (SpMV via CSR) + streaming (AXPY/dot).
 *
 * Usage: ./cg [--size N] [--threads T]
 * Default: 1024 rows, 1 thread */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>
#include "zsim_hooks.h"

#define DEFAULT_SIZE    1024
#define DEFAULT_THREADS 1
#define NNZ_PER_ROW     7       /* average nonzeros per row (NPB-like sparsity) */
#define CG_ITERS        15      /* fixed iteration count for benchmarking */
#define DIAG_SHIFT      20.0    /* diagonal dominance shift for SPD */

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* splitmix64 (Steele, Lea and Flood, "Fast splittable pseudorandom number generators", OOPSLA 2014): one 64-bit
 * output per call from a 64-bit counter state, seeded as before. A draw over a range R is the output reduced modulo R;
 * the modulo bias is at most R / 2^64 (below 2^-40 for R < 2^24) and is ignored. */
static uint64_t splitmix64(uint64_t* s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Build a sparse SPD matrix in CSR format with ~NNZ_PER_ROW entries per row.
 * To ensure symmetry and positive definiteness:
 *   - Generate random off-diagonal entries symmetrically
 *   - Add a large diagonal shift */
static void build_spd_csr(int N, int** out_row_ptr, int** out_col_idx,
                           double** out_values, int* out_nnz) {
    /* First pass: count nonzeros per row */
    int* row_nnz = (int*)calloc(N, sizeof(int));
    uint64_t gs = 271828u;   /* the generator state, seeded as before */
    int total_nnz = 0;

    /* Diagonal always present */
    for (int i = 0; i < N; i++) row_nnz[i] = 1;
    total_nnz = N;

    /* Off-diagonal: pick random column indices symmetrically */
    int off_per_row = NNZ_PER_ROW - 1;
    for (int i = 0; i < N; i++) {
        for (int k = 0; k < off_per_row; k++) {
            int j = (int)(splitmix64(&gs) % (uint64_t)N);
            if (j == i) continue;
            row_nnz[i]++;
            row_nnz[j]++;  /* symmetric */
            total_nnz += 2;
        }
    }

    /* Allocate CSR */
    int* row_ptr  = (int*)malloc((N + 1) * sizeof(int));
    int* col_idx  = (int*)malloc(total_nnz * sizeof(int));
    double* vals  = (double*)malloc(total_nnz * sizeof(double));

    /* Prefix sum for row_ptr */
    row_ptr[0] = 0;
    for (int i = 0; i < N; i++)
        row_ptr[i + 1] = row_ptr[i] + row_nnz[i];

    /* Fill: use a current-offset array */
    int* cur = (int*)calloc(N, sizeof(int));

    /* Diagonal entries */
    for (int i = 0; i < N; i++) {
        int pos = row_ptr[i] + cur[i];
        col_idx[pos] = i;
        vals[pos] = DIAG_SHIFT;
        cur[i]++;
    }

    /* Off-diagonal entries (re-seed for reproducibility) */
    gs = 271828u;
    for (int i = 0; i < N; i++) {
        for (int k = 0; k < off_per_row; k++) {
            int j = (int)(splitmix64(&gs) % (uint64_t)N);
            if (j == i) continue;
            double v = 1.0 / (double)(1 + (k % 4));  /* small off-diag values */

            int pos_i = row_ptr[i] + cur[i];
            col_idx[pos_i] = j;
            vals[pos_i] = v;
            cur[i]++;

            int pos_j = row_ptr[j] + cur[j];
            col_idx[pos_j] = i;
            vals[pos_j] = v;
            cur[j]++;
        }
    }

    free(cur);
    free(row_nnz);

    *out_row_ptr = row_ptr;
    *out_col_idx = col_idx;
    *out_values  = vals;
    *out_nnz     = total_nnz;
}

/* SpMV: y = A * x */
static void spmv(int N, const int* row_ptr, const int* col_idx,
                  const double* vals, const double* x, double* y) {
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < N; i++) {
        double sum = 0.0;
        for (int j = row_ptr[i]; j < row_ptr[i + 1]; j++)
            sum += vals[j] * x[col_idx[j]];
        y[i] = sum;
    }
}

/* dot product */
static double dot(int N, const double* a, const double* b) {
    double s = 0.0;
    #pragma omp parallel for reduction(+:s)
    for (int i = 0; i < N; i++)
        s += a[i] * b[i];
    return s;
}

/* axpy: y = y + alpha * x */
static void axpy(int N, double alpha, const double* x, double* y) {
    #pragma omp parallel for
    for (int i = 0; i < N; i++)
        y[i] += alpha * x[i];
}

int main(int argc, char* argv[]) {
    int N       = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    int threads = parse_int_arg(argc, argv, "--threads", DEFAULT_THREADS);

    if (threads < 1) threads = 1;
    omp_set_num_threads(threads);

    /* Build SPD matrix */
    int* row_ptr = NULL;
    int* col_idx = NULL;
    double* vals = NULL;
    int total_nnz = 0;
    build_spd_csr(N, &row_ptr, &col_idx, &vals, &total_nnz);

    /* Allocate CG vectors */
    double* x = (double*)calloc(N, sizeof(double));   /* solution */
    double* b = (double*)malloc(N * sizeof(double));   /* RHS */
    double* r = (double*)malloc(N * sizeof(double));   /* residual */
    double* p = (double*)malloc(N * sizeof(double));   /* search direction */
    double* Ap = (double*)malloc(N * sizeof(double));  /* A * p */
    if (!x || !b || !r || !p || !Ap) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    /* RHS: b = A * ones (so we know the answer should be close to ones) */
    {
        double* ones = (double*)malloc(N * sizeof(double));
        for (int i = 0; i < N; i++) ones[i] = 1.0;
        spmv(N, row_ptr, col_idx, vals, ones, b);
        free(ones);
    }

    /* --- ROI: CG iterations --- */
    zsim_roi_begin();

    /* r = b - A*x (x=0 so r=b), p = r */
    for (int i = 0; i < N; i++) {
        r[i] = b[i];
        p[i] = b[i];
    }

    double rs_old = dot(N, r, r);

    for (int iter = 0; iter < CG_ITERS; iter++) {
        spmv(N, row_ptr, col_idx, vals, p, Ap);

        double pAp = dot(N, p, Ap);
        if (pAp == 0.0) break;
        double alpha = rs_old / pAp;

        /* x = x + alpha * p */
        axpy(N, alpha, p, x);

        /* r = r - alpha * Ap */
        axpy(N, -alpha, Ap, r);

        double rs_new = dot(N, r, r);

        if (rs_new < 1e-30) break;

        double beta = rs_new / rs_old;

        /* p = r + beta * p */
        #pragma omp parallel for
        for (int i = 0; i < N; i++)
            p[i] = r[i] + beta * p[i];

        rs_old = rs_new;
    }

    zsim_roi_end();

    /* Compute residual norm and checksum outside ROI */
    spmv(N, row_ptr, col_idx, vals, x, Ap);  /* reuse Ap as A*x */
    double res_norm = 0.0;
    double checksum = 0.0;
    for (int i = 0; i < N; i++) {
        double diff = Ap[i] - b[i];
        res_norm += diff * diff;
        checksum += x[i];
    }
    res_norm = sqrt(res_norm);

    printf("BENCH_NNZ: %d\n", total_nnz);
    printf("BENCH_RESIDUAL: %e\n", res_norm);
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(row_ptr); free(col_idx); free(vals);
    free(x); free(b); free(r); free(p); free(Ap);
    return 0;
}
