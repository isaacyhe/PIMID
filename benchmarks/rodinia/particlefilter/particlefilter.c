/* particlefilter.c -- Particle filter (Monte Carlo localization)
 * Tracks a 2D position over T frames using N particles.
 * Predict, update weights from synthetic observations, normalize, resample.
 * OpenMP parallel on predict and weight update steps. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>
#include "zsim_hooks.h"

#define DEFAULT_PARTICLES 1000
#define DEFAULT_FRAMES    10
#define NOISE_SCALE       0.5
#define LIKELIHOOD_SIGMA  1.0

static uint32_t bench_rand(uint32_t* s) {
    *s = *s * 1103515245 + 12345;
    return (*s >> 16) & 0x7FFF;
}

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* Convert uniform [0,1) to approximate Gaussian using Box-Muller */
static double rand_gaussian(uint32_t* s) {
    double u1 = (bench_rand(s) + 1.0) / 32768.0;  /* avoid log(0) */
    double u2 = bench_rand(s) / 32767.0;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * 3.14159265358979323846 * u2);
}

int main(int argc, char* argv[]) {
    int N = parse_int_arg(argc, argv, "--particles", DEFAULT_PARTICLES);
    int T = parse_int_arg(argc, argv, "--frames", DEFAULT_FRAMES);

    /* Particle state: position (x, y) and weight */
    double* px = (double*)malloc(N * sizeof(double));
    double* py = (double*)malloc(N * sizeof(double));
    double* weights = (double*)malloc(N * sizeof(double));
    double* cumsum  = (double*)malloc(N * sizeof(double));
    double* new_px  = (double*)malloc(N * sizeof(double));
    double* new_py  = (double*)malloc(N * sizeof(double));

    if (!px || !py || !weights || !cumsum || !new_px || !new_py) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    /* Initialize particles at random positions, uniform weights */
    uint32_t seed = 42;
    for (int i = 0; i < N; i++) {
        px[i] = 10.0 * (bench_rand(&seed) / 32767.0);
        py[i] = 10.0 * (bench_rand(&seed) / 32767.0);
        weights[i] = 1.0 / N;
    }

    /* Separate seed for observations (deterministic sequence) */
    uint32_t obs_seed = 123;

    /* 1.12.18: the thread team starts before the ROI, as message-passing
     * ranks exist before it; the ROI measures the kernel, not the start-up. */
    #pragma omp parallel
    { }
    zsim_roi_begin();

    for (int t = 0; t < T; t++) {
        /* Generate synthetic observation for this frame */
        double obs_x = 5.0 + 2.0 * (bench_rand(&obs_seed) / 32767.0 - 0.5);
        double obs_y = 5.0 + 2.0 * (bench_rand(&obs_seed) / 32767.0 - 0.5);

        /* 1. Predict: add random noise to each particle */
        #pragma omp parallel for
        for (int i = 0; i < N; i++) {
            /* Per-thread seed derived from particle index and frame */
            uint32_t local_seed = seed + (uint32_t)(i * 1000 + t * 7919);
            px[i] += NOISE_SCALE * rand_gaussian(&local_seed);
            py[i] += NOISE_SCALE * rand_gaussian(&local_seed);
        }

        /* 2. Update weights: likelihood based on distance to observation */
        #pragma omp parallel for
        for (int i = 0; i < N; i++) {
            double dx = px[i] - obs_x;
            double dy = py[i] - obs_y;
            double dist2 = dx * dx + dy * dy;
            double likelihood = exp(-dist2 / (2.0 * LIKELIHOOD_SIGMA * LIKELIHOOD_SIGMA));
            weights[i] *= likelihood;
        }

        /* 3. Normalize weights */
        double wsum = 0.0;
        for (int i = 0; i < N; i++)
            wsum += weights[i];
        if (wsum > 0.0) {
            for (int i = 0; i < N; i++)
                weights[i] /= wsum;
        } else {
            /* All weights collapsed -- reinitialize uniformly */
            for (int i = 0; i < N; i++)
                weights[i] = 1.0 / N;
        }

        /* 4. Systematic resampling */
        cumsum[0] = weights[0];
        for (int i = 1; i < N; i++)
            cumsum[i] = cumsum[i - 1] + weights[i];

        double u0 = bench_rand(&seed) / (32767.0 * N);
        int j = 0;
        for (int i = 0; i < N; i++) {
            double u = u0 + (double)i / N;
            while (j < N - 1 && cumsum[j] < u) j++;
            new_px[i] = px[j];
            new_py[i] = py[j];
        }

        /* Copy resampled particles back */
        for (int i = 0; i < N; i++) {
            px[i] = new_px[i];
            py[i] = new_py[i];
            weights[i] = 1.0 / N;
        }
    }

    zsim_roi_end();

    /* Checksum: final mean particle position (x + y) */
    double mean_x = 0.0, mean_y = 0.0;
    for (int i = 0; i < N; i++) {
        mean_x += px[i];
        mean_y += py[i];
    }
    mean_x /= N;
    mean_y /= N;
    printf("BENCH_CHECKSUM: %f\n", mean_x + mean_y);
    printf("BENCH_DONE\n");

    free(px);
    free(py);
    free(weights);
    free(cumsum);
    free(new_px);
    free(new_py);
    return 0;
}
