/*
 * Binary Search -- sorted array + random lookups
 * Single-threaded benchmark for cache/branch-prediction stress.
 *
 * Compile: g++ -O2 -I<path-to-zsim/misc/hooks> binary_search.c -o binary_search
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "zsim_hooks.h"

/* ------------------------------------------------------------------ */
/*  Arg parser                                                        */
/* ------------------------------------------------------------------ */
static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* ------------------------------------------------------------------ */
/*  LCG                                                               */
/* ------------------------------------------------------------------ */
static uint32_t bench_rand(uint32_t* s) {
    *s = *s * 1103515245 + 12345;
    return (*s >> 16) & 0x7FFF;
}

/* ------------------------------------------------------------------ */
/*  Binary search (iterative)                                         */
/* ------------------------------------------------------------------ */
static int binary_search(const int* arr, int n, int key) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (arr[mid] == key)      return mid;
        else if (arr[mid] < key)  lo = mid + 1;
        else                      hi = mid - 1;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */
int main(int argc, char** argv) {
    int size    = parse_int_arg(argc, argv, "--size", 65536);
    int lookups = parse_int_arg(argc, argv, "--lookups", 10000);

    printf("Binary Search -- size=%d  lookups=%d\n", size, lookups);

    /* Build sorted array: 0, 2, 4, 6, ..., 2*(size-1) */
    int* arr = (int*)malloc((size_t)size * sizeof(int));
    if (!arr) { fprintf(stderr, "malloc failed\n"); return 1; }
    for (int i = 0; i < size; i++)
        arr[i] = 2 * i;

    /* Generate random lookup keys */
    int* keys = (int*)malloc((size_t)lookups * sizeof(int));
    if (!keys) { fprintf(stderr, "malloc failed\n"); return 1; }
    uint32_t seed = 42;
    int range = 2 * size;  /* keys in [0, 2*size) */
    for (int i = 0; i < lookups; i++) {
        /* Combine two 15-bit outputs for wider range */
        uint32_t r = bench_rand(&seed);
        r = (r << 15) | bench_rand(&seed);
        keys[i] = (int)(r % (uint32_t)range);
    }

    /* ---- ROI begin ---- */
    zsim_roi_begin();

    int found_count = 0;
    for (int i = 0; i < lookups; i++) {
        int idx = binary_search(arr, size, keys[i]);
        if (idx >= 0) found_count++;
    }

    zsim_roi_end();
    /* ---- ROI end ---- */

    printf("Found: %d / %d\n", found_count, lookups);
    printf("BENCH_CHECKSUM: %d\n", found_count);
    printf("BENCH_DONE\n");

    free(keys);
    free(arr);
    return 0;
}
