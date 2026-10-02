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

#define DEFAULT_SIZE 256

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): per-PE need of the prep layout
 * [A rows][x] for size N on P PEs (largest share = ceil(N/P) rows). */
static size_t gemv_omp_need(long n, const void* ctx) {
    long P = *(const int*)ctx;
    size_t rows = (size_t)((n + P - 1) / P);
    return (rows * (size_t)n + (size_t)n) * sizeof(float);
}

int main(int argc, char* argv[]) {
    int N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    pimid_devorg_t dev = pimid_devorg_from_args(argc, argv);
    int P = dev.num_pes;

    float* A = (float*)malloc((size_t)N * N * sizeof(float));
    float* x = (float*)malloc((size_t)N * sizeof(float));
    float* y = (float*)calloc((size_t)N, sizeof(float));
    if (!A || !x || !y) { fprintf(stderr, "malloc failed\n"); return 1; }

    for (int i = 0; i < N; i++) {
        x[i] = 1.0f;
        for (int j = 0; j < N; j++)
            A[(size_t)i * N + j] = (float)((i + j) % 7);
    }

    int prep = pimid_devorg_needs_prep(&dev);
    int rows_per_pe = (N + P - 1) / P;

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
        for (int pe = 0; pe < P; pe++) {
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
        zsim_roi_end();
        (void)slot_floats; (void)Adev;   /* Adev's raw alloc is freed at process exit */
    } else {
        /* Coarse / host-shared placement: no relocate; run on host layout. */
        zsim_roi_begin();
        #pragma omp parallel for
        for (int i = 0; i < N; i++) {
            float sum = 0.0f;
            for (int j = 0; j < N; j++)
                sum += A[(size_t)i * N + j] * x[j];
            y[i] = sum;
        }
        zsim_roi_end();
    }

    double checksum = 0.0;
    for (int i = 0; i < N; i++) checksum += y[i];
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(A); free(x); free(y);
    return 0;
}
