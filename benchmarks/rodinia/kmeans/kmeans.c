/* kmeans.c -- K-means clustering
 * OpenMP parallel version. Clusters N points of D dimensions into K
 * clusters over T iterations (Rodinia kmeans). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>
#include "zsim_hooks.h"

#define DEFAULT_POINTS   1024
#define DEFAULT_CLUSTERS 8
#define DEFAULT_DIMS     16
#define DEFAULT_ITERS    20

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
    int N = parse_int_arg(argc, argv, "--points", DEFAULT_POINTS);
    int K = parse_int_arg(argc, argv, "--clusters", DEFAULT_CLUSTERS);
    int D = parse_int_arg(argc, argv, "--dims", DEFAULT_DIMS);
    int iters = parse_int_arg(argc, argv, "--iters", DEFAULT_ITERS);

    /* Allocate data */
    double* points    = (double*)malloc((size_t)N * D * sizeof(double));
    double* centroids = (double*)malloc((size_t)K * D * sizeof(double));
    double* new_cents = (double*)malloc((size_t)K * D * sizeof(double));
    int*    counts    = (int*)calloc(K, sizeof(int));
    int*    assign    = (int*)malloc(N * sizeof(int));
    if (!points || !centroids || !new_cents || !counts || !assign) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    /* Initialize points from LCG, range [0.0, 100.0) */
    uint32_t seed = 42;
    for (size_t i = 0; i < (size_t)N * D; i++)
        points[i] = (double)bench_rand(&seed) / 32768.0 * 100.0;

    /* Initial centroids = first K points */
    memcpy(centroids, points, (size_t)K * D * sizeof(double));

    /* 1.12.18: the thread team starts before the ROI, as message-passing
     * ranks exist before it; the ROI measures the kernel, not the start-up. */
    #pragma omp parallel
    { }
    zsim_roi_begin();

    for (int t = 0; t < iters; t++) {
        /* Step 1: Assignment -- find nearest centroid for each point */
        #pragma omp parallel for
        for (int i = 0; i < N; i++) {
            double min_dist = 1e30;
            int best = 0;
            for (int k = 0; k < K; k++) {
                double dist = 0.0;
                for (int d = 0; d < D; d++) {
                    double diff = points[(size_t)i * D + d]
                                - centroids[(size_t)k * D + d];
                    dist += diff * diff;
                }
                if (dist < min_dist) {
                    min_dist = dist;
                    best = k;
                }
            }
            assign[i] = best;
        }

        /* Step 2: Clear accumulators */
        memset(new_cents, 0, (size_t)K * D * sizeof(double));
        memset(counts, 0, K * sizeof(int));

        /* Step 3: Accumulate point positions per cluster */
        for (int i = 0; i < N; i++) {
            int k = assign[i];
            counts[k]++;
            for (int d = 0; d < D; d++)
                new_cents[(size_t)k * D + d] += points[(size_t)i * D + d];
        }

        /* Step 4: Update centroids as mean of assigned points */
        for (int k = 0; k < K; k++) {
            if (counts[k] > 0) {
                for (int d = 0; d < D; d++)
                    centroids[(size_t)k * D + d] = new_cents[(size_t)k * D + d]
                                                 / (double)counts[k];
            }
        }
    }

    zsim_roi_end();

    /* Checksum: sum of all final centroid coordinates */
    double checksum = 0.0;
    for (size_t i = 0; i < (size_t)K * D; i++)
        checksum += centroids[i];
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(points);
    free(centroids);
    free(new_cents);
    free(counts);
    free(assign);
    return 0;
}
