/*
 * Quicksort — In-place iterative (stack-based) quicksort benchmark
 * Lomuto partition scheme, single-threaded.
 *
 * Compile: g++ -O2 -I<path-to-zsim/misc/hooks> quicksort.c -o quicksort
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
/*  Generator (splitmix64)                                            */
/* ------------------------------------------------------------------ */
/* splitmix64 (Steele, Lea and Flood, "Fast splittable pseudorandom number generators", OOPSLA 2014): one 64-bit
 * output per call from a 64-bit counter state, seeded as before. A draw over a range R is the output reduced modulo R;
 * the modulo bias is at most R / 2^64 (below 2^-40 for R < 2^24) and is ignored. */
static uint64_t splitmix64(uint64_t* s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* ------------------------------------------------------------------ */
/*  Lomuto partition                                                   */
/* ------------------------------------------------------------------ */
static int partition(int* arr, int lo, int hi) {
    int pivot = arr[hi];
    int i = lo - 1;
    for (int j = lo; j < hi; j++) {
        if (arr[j] <= pivot) {
            i++;
            int tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
        }
    }
    int tmp = arr[i + 1]; arr[i + 1] = arr[hi]; arr[hi] = tmp;
    return i + 1;
}

/* ------------------------------------------------------------------ */
/*  Iterative quicksort (explicit stack)                               */
/* ------------------------------------------------------------------ */
static void quicksort(int* arr, int n) {
    if (n <= 1) return;

    /* Stack of (lo, hi) pairs.  log2(N) * 2 is sufficient for
       balanced partitions; worst-case N but that is rare with random data. */
    int stack_size = 64;  /* enough for N up to ~2^63 balanced */
    int* stack = (int*)malloc((size_t)stack_size * 2 * sizeof(int));
    int top = -1;

    /* Push initial range */
    stack[++top] = 0;
    stack[++top] = n - 1;

    while (top >= 0) {
        int hi = stack[top--];
        int lo = stack[top--];

        if (lo >= hi) continue;

        int p = partition(arr, lo, hi);

        /* Push larger subarray first (tail-call optimisation for stack depth) */
        if (p - lo > hi - p) {
            /* Left is larger: push left first, then right */
            if (lo < p - 1) {
                stack[++top] = lo;
                stack[++top] = p - 1;
            }
            if (p + 1 < hi) {
                stack[++top] = p + 1;
                stack[++top] = hi;
            }
        } else {
            /* Right is larger: push right first, then left */
            if (p + 1 < hi) {
                stack[++top] = p + 1;
                stack[++top] = hi;
            }
            if (lo < p - 1) {
                stack[++top] = lo;
                stack[++top] = p - 1;
            }
        }
    }

    free(stack);
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */
int main(int argc, char** argv) {
    int size = parse_int_arg(argc, argv, "--size", 65536);

    printf("Quicksort (iterative Lomuto) — size=%d\n", size);

    /* Init array from the generator */
    int* arr = (int*)malloc((size_t)size * sizeof(int));
    if (!arr) { fprintf(stderr, "malloc failed\n"); return 1; }
    uint64_t gs = 42;   /* the generator state, seeded as before */
    for (int i = 0; i < size; i++)
        arr[i] = (int)(splitmix64(&gs) >> 33);

    /* ---- ROI begin ---- */
    zsim_roi_begin();

    quicksort(arr, size);

    zsim_roi_end();
    /* ---- ROI end ---- */

    /* Verify sorted */
    int sorted = 1;
    for (int i = 1; i < size; i++) {
        if (arr[i] < arr[i - 1]) { sorted = 0; break; }
    }
    printf("SORTED: %s\n", sorted ? "yes" : "no");

    /* Checksum: sum of first 10 + last 10 elements */
    long first_sum = 0, last_sum = 0;
    int n_check = (size < 10) ? size : 10;
    for (int i = 0; i < n_check; i++)
        first_sum += arr[i];
    for (int i = size - n_check; i < size; i++)
        last_sum += arr[i];
    long checksum = first_sum + last_sum;

    printf("First 10 sum: %ld  Last 10 sum: %ld\n", first_sum, last_sum);
    printf("BENCH_CHECKSUM: %ld\n", checksum);
    printf("BENCH_DONE\n");

    free(arr);
    return 0;
}
