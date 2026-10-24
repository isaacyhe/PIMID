/* needle.c -- Needleman-Wunsch sequence alignment (dynamic programming)
 * OpenMP parallel version. Aligns two sequences of length N using
 * anti-diagonal wavefront parallelism (Rodinia needle). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include "zsim_hooks.h"

#define DEFAULT_SIZE 256

/* Scoring */
#define MATCH_SCORE    2
#define MISMATCH_SCORE (-1)
#define GAP_PENALTY    (-1)

static uint32_t bench_rand(uint32_t* s) {
    *s = *s * 1103515245 + 12345;
    return (*s >> 16) & 0x7FFF;
}

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

static inline int max3(int a, int b, int c) {
    int m = a;
    if (b > m) m = b;
    if (c > m) m = c;
    return m;
}

int main(int argc, char* argv[]) {
    int N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    const char alphabet[] = "ACGT";
    uint32_t seed = 42;

    /* Generate two random sequences */
    char* seqA = (char*)malloc((N + 1) * sizeof(char));
    char* seqB = (char*)malloc((N + 1) * sizeof(char));
    if (!seqA || !seqB) { fprintf(stderr, "malloc failed\n"); return 1; }
    for (int i = 0; i < N; i++) seqA[i] = alphabet[bench_rand(&seed) % 4];
    for (int i = 0; i < N; i++) seqB[i] = alphabet[bench_rand(&seed) % 4];
    seqA[N] = '\0';
    seqB[N] = '\0';

    /* Score matrix: (N+1) x (N+1) */
    size_t dim = (size_t)(N + 1);
    int* score = (int*)malloc(dim * dim * sizeof(int));
    if (!score) { fprintf(stderr, "malloc failed\n"); return 1; }

    /* Initialize borders: gap penalties */
    for (int i = 0; i <= N; i++) {
        score[i * dim + 0] = i * GAP_PENALTY;
        score[0 * dim + i] = i * GAP_PENALTY;
    }

    /* 1.12.18: the thread team starts before the ROI, as message-passing
     * ranks exist before it; the ROI measures the kernel, not the start-up. */
    #pragma omp parallel
    { }
    zsim_roi_begin();

    /* Fill score matrix using anti-diagonal wavefront */
    /* Total anti-diagonals: 2*N - 1 (for the inner (1..N) x (1..N) region) */
    for (int diag = 0; diag < 2 * N - 1; diag++) {
        /* Elements on this anti-diagonal: i + j - 2 = diag, where i,j in [1,N] */
        int i_start = (diag < N) ? 1 : diag - N + 2;
        int i_end   = (diag < N) ? diag + 1 : N;
        int count   = i_end - i_start + 1;

        #pragma omp parallel for
        for (int k = 0; k < count; k++) {
            int i = i_start + k;
            int j = diag + 2 - i;  /* i + j = diag + 2 */

            int s = (seqA[i - 1] == seqB[j - 1]) ? MATCH_SCORE : MISMATCH_SCORE;
            int diag_score = score[(i - 1) * dim + (j - 1)] + s;
            int up_score   = score[(i - 1) * dim + j] + GAP_PENALTY;
            int left_score = score[i * dim + (j - 1)] + GAP_PENALTY;

            score[i * dim + j] = max3(diag_score, up_score, left_score);
        }
    }

    zsim_roi_end();

    /* Final alignment score */
    int alignment_score = score[N * dim + N];
    printf("BENCH_CHECKSUM: %d\n", alignment_score);
    printf("BENCH_DONE\n");

    free(seqA);
    free(seqB);
    free(score);
    return 0;
}
