/* stream_triad_omp.c — STREAM Triad: a[i] = b[i] + scalar * c[i]
 * OpenMP parallel version. Standalone, no shared headers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include "zsim_hooks.h"

#define DEFAULT_SIZE 8192

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

int main(int argc, char* argv[]) {
    int N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    float scalar = 3.0f;

    float* a = (float*)malloc(N * sizeof(float));
    float* b = (float*)malloc(N * sizeof(float));
    float* c = (float*)malloc(N * sizeof(float));
    if (!a || !b || !c) { fprintf(stderr, "malloc failed\n"); return 1; }

    for (int i = 0; i < N; i++) {
        b[i] = 1.0f;
        c[i] = 2.0f;
    }

    /* 1.12.18: the thread team starts before the ROI, as message-passing
     * ranks exist before it; the ROI measures the kernel, not the start-up. */
    #pragma omp parallel
    { }
    zsim_roi_begin();
    #pragma omp parallel for
    for (int i = 0; i < N; i++)
        a[i] = b[i] + scalar * c[i];
    zsim_roi_end();

    double checksum = 0.0;
    for (int i = 0; i < N; i++) checksum += a[i];
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(a); free(b); free(c);
    return 0;
}
