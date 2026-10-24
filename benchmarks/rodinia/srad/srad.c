/* srad.c -- Speckle Reducing Anisotropic Diffusion (medical imaging)
 * OpenMP parallel version. Image is rows*cols, runs for iters iterations
 * with diffusion coefficient lambda (Rodinia SRAD). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>
#include "zsim_hooks.h"

#define DEFAULT_ROWS   256
#define DEFAULT_COLS   256
#define DEFAULT_ITERS  10
#define DEFAULT_LAMBDA 0.5

static uint32_t bench_rand(uint32_t* s) {
    *s = *s * 1103515245 + 12345;
    return (*s >> 16) & 0x7FFF;
}

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

static double parse_double_arg(int argc, char** argv, const char* flag, double def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atof(argv[i + 1]);
    return def;
}

int main(int argc, char* argv[]) {
    int rows    = parse_int_arg(argc, argv, "--rows", DEFAULT_ROWS);
    int cols    = parse_int_arg(argc, argv, "--cols", DEFAULT_COLS);
    int iters   = parse_int_arg(argc, argv, "--iters", DEFAULT_ITERS);
    double lam  = parse_double_arg(argc, argv, "--lambda", DEFAULT_LAMBDA);

    size_t ncells = (size_t)rows * cols;
    double* image = (double*)malloc(ncells * sizeof(double));
    double* coeff = (double*)malloc(ncells * sizeof(double));
    double* dN    = (double*)malloc(ncells * sizeof(double));
    double* dS    = (double*)malloc(ncells * sizeof(double));
    double* dE    = (double*)malloc(ncells * sizeof(double));
    double* dW    = (double*)malloc(ncells * sizeof(double));
    if (!image || !coeff || !dN || !dS || !dE || !dW) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    /* Initialize image with LCG random values in [0.0, 1.0) */
    uint32_t seed = 42;
    for (size_t i = 0; i < ncells; i++)
        image[i] = (double)bench_rand(&seed) / 32768.0;

    /* 1.12.18: the thread team starts before the ROI, as message-passing
     * ranks exist before it; the ROI measures the kernel, not the start-up. */
    #pragma omp parallel
    { }
    zsim_roi_begin();

    for (int t = 0; t < iters; t++) {
        /* Step 1: Compute statistics for q0 (speckle scale function) */
        double sum = 0.0, sum2 = 0.0;
        #pragma omp parallel for reduction(+:sum,sum2)
        for (int i = 0; i < (int)ncells; i++) {
            sum  += image[i];
            sum2 += image[i] * image[i];
        }
        double mean = sum / (double)ncells;
        double var  = sum2 / (double)ncells - mean * mean;
        if (var < 0.0) var = 0.0;
        double q0 = (mean > 1e-12) ? sqrt(var) / mean : 0.0;
        double q0sq = q0 * q0;

        /* Step 2: Compute directional differences and diffusion coefficient */
        #pragma omp parallel for
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                int idx = r * cols + c;
                double val = image[idx];

                /* Directional differences (clamped at boundaries) */
                dN[idx] = (r > 0)        ? image[(r - 1) * cols + c] - val : 0.0;
                dS[idx] = (r < rows - 1) ? image[(r + 1) * cols + c] - val : 0.0;
                dW[idx] = (c > 0)        ? image[r * cols + (c - 1)] - val : 0.0;
                dE[idx] = (c < cols - 1) ? image[r * cols + (c + 1)] - val : 0.0;

                /* Gradient magnitude squared (normalized) */
                double grad2 = (dN[idx] * dN[idx] + dS[idx] * dS[idx]
                              + dE[idx] * dE[idx] + dW[idx] * dW[idx]);
                double denom = val * val;
                double qsq = (denom > 1e-12) ? (0.5 * grad2) / denom : 0.0;

                /* Diffusion coefficient: edge-stopping function */
                double num = qsq - q0sq;
                double den = q0sq * (1.0 + q0sq);
                double csq = (den > 1e-12) ? num / den : 0.0;
                coeff[idx] = 1.0 / (1.0 + csq);
            }
        }

        /* Step 3: Update image using diffusion equation */
        #pragma omp parallel for
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                int idx = r * cols + c;

                /* Diffusion coefficients of neighbors */
                double cN = coeff[idx];
                double cS = (r < rows - 1) ? coeff[(r + 1) * cols + c] : coeff[idx];
                double cW = coeff[idx];
                double cE = (c < cols - 1) ? coeff[r * cols + (c + 1)] : coeff[idx];

                /* Divergence */
                double divergence = cN * dN[idx] + cS * dS[idx]
                                  + cW * dW[idx] + cE * dE[idx];

                image[idx] += lam * divergence;
            }
        }
    }

    zsim_roi_end();

    /* Checksum: sum of all pixel values */
    double checksum = 0.0;
    for (size_t i = 0; i < ncells; i++)
        checksum += image[i];
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(image);
    free(coeff);
    free(dN);
    free(dS);
    free(dE);
    free(dW);
    return 0;
}
