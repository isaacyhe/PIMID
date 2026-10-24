/* spmv_csr.c — Sparse Matrix-Vector Multiply in CSR format: y = A * x
 * Serial baseline. Generates random sparse matrix with ~1% density. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zsim_hooks.h"
#include "../../include/pimid_spm.h"

#define DEFAULT_SIZE 1024

static uint32_t bench_rand(uint32_t* s) {
    *s = *s * 1103515245 + 12345;
    return (*s >> 16) & 0x7FFF;
}

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* 1.12.18: y[0..nrows) = A[0..nrows) * x for the CSR rows whose row_ptr
 * entries start at row_ptr (col_idx / values are indexed globally), through
 * the PE scratchpad (pimid_spm.h). The RAM holds four equal buffers: a tile of
 * row_ptr, a tile of col_idx, a tile of values and a tile of y. Each row block
 * fetches its row_ptr entries once, streams its nonzeros through the col_idx
 * and values tiles (each nonzero fetched once, a row may span tiles) and
 * writes its y block back once. The x[col] gather is irregular and stays a
 * direct DRAM access. With no scratchpad the tiles are the whole range and
 * this is the plain row loop. */
static void spmv_rows(pimid_spm_t* s, const int* row_ptr, const int* col_idx, const float* values,
                      const float* x, float* y, size_t nrows) {
    const size_t T = pimid_spm_tile(s, 4, sizeof(int));
    const size_t R = T - 1;   /* rows per block: row_ptr needs R + 1 entries */
    const size_t orp = 0, oc = s->cap ? T * sizeof(int) : 0;
    const size_t ov = s->cap ? 2 * T * sizeof(int) : 0, oy = s->cap ? 3 * T * sizeof(int) : 0;
    for (size_t rb = 0; rb < nrows; rb += R) {
        const size_t nr = (nrows - rb < R) ? nrows - rb : R;
        const int* rp = (const int*)pimid_spm_in(s, orp, row_ptr + rb, (nr + 1) * sizeof(int));
        float* yb = (float*)pimid_spm_out(s, oy, y + rb);
        const int jlast = rp[nr];
        int jb = rp[0], je = rp[0];   /* nonzeros [jb, je) are in the RAM */
        const int* cb = col_idx;
        const float* vb = values;
        for (size_t r = 0; r < nr; r++) {
            float sum = 0.0f;
            int j = rp[r];
            const int jend = rp[r + 1];
            while (j < jend) {
                if (j == je) {
                    jb = j;
                    je = ((size_t)(jlast - jb) < T) ? jlast : jb + (int)T;
                    cb = (const int*)pimid_spm_in(s, oc, col_idx + jb, (size_t)(je - jb) * sizeof(int));
                    vb = (const float*)pimid_spm_in(s, ov, values + jb, (size_t)(je - jb) * sizeof(float));
                }
                const int e = (jend < je) ? jend : je;
                for (; j < e; j++)
                    sum += vb[j - jb] * x[cb[j - jb]];
            }
            yb[r] = sum;
        }
        pimid_spm_flush(s, y + rb, yb, nr * sizeof(float));
    }
}

int main(int argc, char* argv[]) {
    int N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    int density_pct = parse_int_arg(argc, argv, "--density", 1);
    uint32_t seed = 42;

    /* Count nonzeros per row first */
    int* row_nnz = (int*)calloc(N, sizeof(int));
    if (!row_nnz) { fprintf(stderr, "malloc failed\n"); return 1; }

    int total_nnz = 0;
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            if ((int)(bench_rand(&seed) % 100) < density_pct) {
                row_nnz[i]++;
                total_nnz++;
            }
        }
    }

    /* Allocate CSR arrays */
    int* row_ptr = (int*)malloc((N + 1) * sizeof(int));
    int* col_idx = (int*)malloc(total_nnz * sizeof(int));
    float* values = (float*)malloc(total_nnz * sizeof(float));
    float* x = (float*)malloc(N * sizeof(float));
    float* y = (float*)calloc(N, sizeof(float));
    if (!row_ptr || !col_idx || !values || !x || !y) {
        fprintf(stderr, "malloc failed\n"); return 1;
    }

    /* Build CSR structure */
    row_ptr[0] = 0;
    for (int i = 0; i < N; i++)
        row_ptr[i + 1] = row_ptr[i] + row_nnz[i];

    /* Re-generate with same seed to fill col_idx and values */
    seed = 42;
    int* cur = (int*)calloc(N, sizeof(int));
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            if ((int)(bench_rand(&seed) % 100) < density_pct) {
                int pos = row_ptr[i] + cur[i];
                col_idx[pos] = j;
                values[pos] = 1.0f;
                cur[i]++;
            }
        }
    }
    free(cur);
    free(row_nnz);

    for (int i = 0; i < N; i++) x[i] = 1.0f;

    pimid_spm_t spm;
    pimid_spm_open(&spm);
    zsim_roi_begin();
    /* 1.12.18: no scratchpad -- the 1.12.17 code, unchanged */
    if (!spm.cap) {
        for (int i = 0; i < N; i++) {
            float sum = 0.0f;
            for (int j = row_ptr[i]; j < row_ptr[i + 1]; j++)
                sum += values[j] * x[col_idx[j]];
            y[i] = sum;
        }
    } else {
        spmv_rows(&spm, row_ptr, col_idx, values, x, y, (size_t)N);
    }
    zsim_roi_end();
    pimid_spm_close(&spm);

    double checksum = 0.0;
    for (int i = 0; i < N; i++) checksum += y[i];
    printf("BENCH_NNZ: %d\n", total_nnz);
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(row_ptr); free(col_idx); free(values);
    free(x); free(y);
    return 0;
}
