/* hotspot.c -- 2D thermal simulation: iterative heat equation solver
 * OpenMP parallel version. Grid is rows*cols, runs for iters time steps.
 * Models heat diffusion with per-cell power injection (Rodinia hotspot). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>
#include "zsim_hooks.h"

#define DEFAULT_ROWS  256
#define DEFAULT_COLS  256
#define DEFAULT_ITERS 10

/* Thermal parameters (simplified from Rodinia) */
#define AMB_TEMP      80.0
#define CHIP_HEIGHT   0.016   /* meters */
#define CHIP_WIDTH    0.016
#define T_CHIP        0.0005  /* chip thickness */
#define K_SI          100.0   /* silicon thermal conductivity W/(m*K) */
#define C_SI          1.75e6  /* silicon volumetric heat capacity J/(m^3*K) */
#define CONV_COEFF    0.001   /* convective heat transfer coefficient */

static uint32_t bench_rand(uint32_t* s) {
    *s = *s * 1103515245 + 12345;
    return (*s >> 16) & 0x7FFF;
}

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

int main(int argc, char* argv[]) {
    int rows = parse_int_arg(argc, argv, "--rows", DEFAULT_ROWS);
    int cols = parse_int_arg(argc, argv, "--cols", DEFAULT_COLS);
    int iters = parse_int_arg(argc, argv, "--iters", DEFAULT_ITERS);

    size_t ncells = (size_t)rows * cols;
    double* temp   = (double*)malloc(ncells * sizeof(double));
    double* temp2  = (double*)malloc(ncells * sizeof(double));
    double* power  = (double*)malloc(ncells * sizeof(double));
    if (!temp || !temp2 || !power) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    /* Grid spacing */
    double grid_height = CHIP_HEIGHT / rows;
    double grid_width  = CHIP_WIDTH  / cols;

    /* Thermal capacitance and conductance */
    double cap = C_SI * T_CHIP * grid_height * grid_width;
    double Rx  = grid_width  / (2.0 * K_SI * T_CHIP * grid_height);
    double Ry  = grid_height / (2.0 * K_SI * T_CHIP * grid_width);
    double Rz  = T_CHIP / (K_SI * grid_height * grid_width);
    double max_slope = K_SI * T_CHIP / (0.5 * cap)
                       * (1.0 / (grid_height * grid_height)
                        + 1.0 / (grid_width * grid_width));
    double step = 0.001 / max_slope;  /* time step from stability constraint */

    /* Initialize: temperature to ambient, power from LCG */
    uint32_t seed = 42;
    for (size_t i = 0; i < ncells; i++) {
        temp[i]  = AMB_TEMP;
        temp2[i] = AMB_TEMP;
        power[i] = (double)(bench_rand(&seed) % 51);  /* 0..50 */
    }

    /* 1.12.18: the thread team starts before the ROI, as message-passing
     * ranks exist before it; the ROI measures the kernel, not the start-up. */
    #pragma omp parallel
    { }
    zsim_roi_begin();
    for (int t = 0; t < iters; t++) {
        #pragma omp parallel for
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                int idx = r * cols + c;
                double old = temp[idx];

                /* Neighbor temperatures with clamped boundaries */
                double tN = (r > 0)        ? temp[(r - 1) * cols + c] : old;
                double tS = (r < rows - 1) ? temp[(r + 1) * cols + c] : old;
                double tW = (c > 0)        ? temp[r * cols + (c - 1)] : old;
                double tE = (c < cols - 1) ? temp[r * cols + (c + 1)] : old;

                /* Heat diffusion from neighbors */
                double lateral = (tN + tS - 2.0 * old) / (Ry * cap)
                               + (tW + tE - 2.0 * old) / (Rx * cap);

                /* Convective cooling to ambient */
                double vertical = (AMB_TEMP - old) / (Rz * cap);

                /* Power injection */
                double pwr = power[idx];

                temp2[idx] = old + step * (pwr / cap + lateral + vertical);
            }
        }
        /* Swap buffers */
        double* s = temp;
        temp = temp2;
        temp2 = s;
    }
    zsim_roi_end();

    double checksum = 0.0;
    for (size_t i = 0; i < ncells; i++)
        checksum += temp[i];
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(temp);
    free(temp2);
    free(power);
    return 0;
}
