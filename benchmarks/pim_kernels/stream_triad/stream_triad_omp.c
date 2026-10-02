/* stream_triad_omp.c -- STREAM Triad: a[i] = b[i] + scalar * c[i]
 * OpenMP parallel version.
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

#define DEFAULT_SIZE 8192

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): per-PE need of the prep layout
 * [a chunk][b chunk][c chunk] for size N on P PEs (chunk = ceil(N/P)). */
static size_t stream_triad_omp_need(long n, const void* ctx) {
    long P = *(const int*)ctx;
    return 3u * (size_t)((n + P - 1) / P) * sizeof(float);
}

int main(int argc, char* argv[]) {
    int N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    float scalar = 3.0f;
    pimid_devorg_t dev = pimid_devorg_from_args(argc, argv);
    int P = dev.num_pes;

    float* a = (float*)malloc(N * sizeof(float));
    float* b = (float*)malloc(N * sizeof(float));
    float* c = (float*)malloc(N * sizeof(float));
    if (!a || !b || !c) { fprintf(stderr, "malloc failed\n"); return 1; }

    for (int i = 0; i < N; i++) {
        b[i] = 1.0f;
        c[i] = 2.0f;
    }

    int prep = pimid_devorg_needs_prep(&dev);
    int chunk = (N + P - 1) / P;

    if (prep) {
        /* ---- DATA PREP (device-org aware, before ROI, not timed) ----
         * a, b, c are each PARTITIONED by element range: PE pe owns elements
         * [pe*chunk, (pe+1)*chunk). Nothing is read by all PEs (the scalar is a
         * plain register value), so there is no replication. Relocate PE pe's
         * chunk of b + c into PE pe's own unit slot; a's chunk is written there
         * by the ROI. Slot layout: [a chunk][b chunk][c chunk]. */
        omp_set_num_threads(P);
        /* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): refuse when PE pe's three
         * chunks do not fit its slot (c overlapped PE pe+1's a before). */
        size_t slot_bytes = pimid_devorg_slot_bytes(&dev);
        size_t need = stream_triad_omp_need(N, &P);
        if (need > slot_bytes) {
            pimid_devorg_report_slot_refusal("", "stream_triad", &dev, need, slot_bytes, "--size",
                pimid_devorg_largest_fit(stream_triad_omp_need, &P, N, slot_bytes));
            return 1;
        }
        float* dbuf = (float*)pimid_devorg_alloc(&dev, &slot_bytes);
        if (!dbuf) { fprintf(stderr, "devorg alloc failed\n"); return 1; }
        for (int pe = 0; pe < P; pe++) {
            int i0 = pe * chunk;
            int i1 = i0 + chunk; if (i1 > N) i1 = N;
            if (i0 >= N) continue;
            float* ape = (float*)pimid_devorg_pe_slot(dbuf, pe, slot_bytes);
            float* bpe = ape + (size_t)chunk;          /* b chunk after a chunk */
            float* cpe = bpe + (size_t)chunk;          /* c chunk after b chunk */
            memcpy(bpe, b + i0, (size_t)(i1 - i0) * sizeof(float));
            memcpy(cpe, c + i0, (size_t)(i1 - i0) * sizeof(float));
        }

        fprintf(stderr, "[devorg] PREP ACTIVE: kernel=stream_triad level=%s pes=%d "
                "slot_bytes=%zu\n",
                pimid_devorg_level_name(dev.level), P, slot_bytes);
        zsim_roi_begin();
        #pragma omp parallel for schedule(static, 1)
        for (int pe = 0; pe < P; pe++) {
            int i0 = pe * chunk;
            int i1 = i0 + chunk; if (i1 > N) i1 = N;
            float* ape = (float*)pimid_devorg_pe_slot(dbuf, pe, slot_bytes);
            float* bpe = ape + (size_t)chunk;
            float* cpe = bpe + (size_t)chunk;
            for (int i = i0; i < i1; i++) {
                int k = i - i0;
                ape[k] = bpe[k] + scalar * cpe[k];
            }
        }
        zsim_roi_end();

        /* Gather each PE's chunk of a back to host layout for the checksum
         * (post-ROI, not timed). */
        for (int pe = 0; pe < P; pe++) {
            int i0 = pe * chunk;
            int i1 = i0 + chunk; if (i1 > N) i1 = N;
            if (i0 >= N) continue;
            float* ape = (float*)pimid_devorg_pe_slot(dbuf, pe, slot_bytes);
            memcpy(a + i0, ape, (size_t)(i1 - i0) * sizeof(float));
        }
        (void)dbuf;   /* dbuf's raw alloc is freed at process exit */
    } else {
        /* Coarse / host-shared placement: no relocate; run on host layout. */
        zsim_roi_begin();
        #pragma omp parallel for
        for (int i = 0; i < N; i++)
            a[i] = b[i] + scalar * c[i];
        zsim_roi_end();
    }

    double checksum = 0.0;
    for (int i = 0; i < N; i++) checksum += a[i];
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(a); free(b); free(c);
    return 0;
}
