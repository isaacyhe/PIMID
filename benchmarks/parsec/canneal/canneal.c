/*
 * Simulated Annealing -- Netlist placement optimization (PARSEC canneal)
 * Random element swaps accepted via Metropolis criterion. Geometric cooling.
 * Cache-hostile random access pattern.
 *
 * Compile: g++ -std=c++11 -O2 -Wall -I<path-to-zsim/misc/hooks> -lpthread -lm canneal.c -o canneal
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <pthread.h>
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
/*  Generators: splitmix64 (element draws) and an LCG (the rest)      */
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

static uint32_t bench_rand(uint32_t* s) {
    *s = *s * 1103515245 + 12345;
    return (*s >> 16) & 0x7FFF;
}

/* ------------------------------------------------------------------ */
/*  Netlist data structures                                           */
/* ------------------------------------------------------------------ */
#define MAX_NETS_PER_ELEM 6

typedef struct {
    int x, y;                         /* placement position */
    int nets[MAX_NETS_PER_ELEM];      /* connected net IDs (-1 = unused) */
    int num_nets;
} Element;

typedef struct {
    int elems[4];                     /* elements in this net (-1 = unused) */
    int num_elems;
} Net;

/* ------------------------------------------------------------------ */
/*  Globals (shared across threads)                                   */
/* ------------------------------------------------------------------ */
static Element* g_elems;
static Net*     g_nets;
static int      g_num_elems;
static int      g_num_nets;
static int      g_grid_side;
static int      g_swaps_per_temp;
static double   g_temperature;

static pthread_barrier_t g_barrier;
static volatile int g_total_accepted;

/* ------------------------------------------------------------------ */
/*  Wire length of a single net (half-perimeter bounding box)         */
/* ------------------------------------------------------------------ */
static int net_cost(int net_id) {
    Net* n = &g_nets[net_id];
    int min_x = 1 << 30, max_x = -(1 << 30);
    int min_y = 1 << 30, max_y = -(1 << 30);
    for (int i = 0; i < n->num_elems; i++) {
        int eid = n->elems[i];
        if (g_elems[eid].x < min_x) min_x = g_elems[eid].x;
        if (g_elems[eid].x > max_x) max_x = g_elems[eid].x;
        if (g_elems[eid].y < min_y) min_y = g_elems[eid].y;
        if (g_elems[eid].y > max_y) max_y = g_elems[eid].y;
    }
    return (max_x - min_x) + (max_y - min_y);
}

/* ------------------------------------------------------------------ */
/*  Cost delta for swapping two elements                              */
/* ------------------------------------------------------------------ */
static int swap_delta(int a, int b) {
    /* Sum cost of all affected nets before swap */
    int cost_before = 0;
    for (int i = 0; i < g_elems[a].num_nets; i++)
        cost_before += net_cost(g_elems[a].nets[i]);
    for (int i = 0; i < g_elems[b].num_nets; i++)
        cost_before += net_cost(g_elems[b].nets[i]);

    /* Perform swap */
    int tx = g_elems[a].x; g_elems[a].x = g_elems[b].x; g_elems[b].x = tx;
    int ty = g_elems[a].y; g_elems[a].y = g_elems[b].y; g_elems[b].y = ty;

    /* Sum cost after swap */
    int cost_after = 0;
    for (int i = 0; i < g_elems[a].num_nets; i++)
        cost_after += net_cost(g_elems[a].nets[i]);
    for (int i = 0; i < g_elems[b].num_nets; i++)
        cost_after += net_cost(g_elems[b].nets[i]);

    return cost_after - cost_before;
    /* Note: swap stays -- caller undoes if rejected */
}

static void undo_swap(int a, int b) {
    int tx = g_elems[a].x; g_elems[a].x = g_elems[b].x; g_elems[b].x = tx;
    int ty = g_elems[a].y; g_elems[a].y = g_elems[b].y; g_elems[b].y = ty;
}

/* ------------------------------------------------------------------ */
/*  Thread context                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    int id;
    int num_threads;
    int accepted;
    uint32_t seed;
    uint64_t eseed;   /* element draws (64-bit generator), seeded like seed */
} ThreadCtx;

static void* worker(void* arg) {
    ThreadCtx* ctx = (ThreadCtx*)arg;
    int my_swaps = g_swaps_per_temp / ctx->num_threads;
    double temp = g_temperature;
    double cooling = 0.95;
    int temp_steps = 40;

    for (int step = 0; step < temp_steps; step++) {
        int local_accepted = 0;
        for (int s = 0; s < my_swaps; s++) {
            int a = (int)(splitmix64(&ctx->eseed) % (uint64_t)g_num_elems);
            int b = (int)(splitmix64(&ctx->eseed) % (uint64_t)g_num_elems);
            if (a == b) continue;

            int delta = swap_delta(a, b);
            if (delta <= 0) {
                /* Accept improving or neutral move */
                local_accepted++;
            } else {
                /* Metropolis criterion */
                double prob = exp(-(double)delta / temp);
                double r = (double)(bench_rand(&ctx->seed)) / 32768.0;
                if (r < prob) {
                    local_accepted++;
                } else {
                    undo_swap(a, b);
                }
            }
        }
        ctx->accepted += local_accepted;

        /* Synchronize threads between temperature steps */
        pthread_barrier_wait(&g_barrier);
        temp *= cooling;
        pthread_barrier_wait(&g_barrier);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */
int main(int argc, char** argv) {
    g_num_elems = parse_int_arg(argc, argv, "--size", 4096);
    int num_threads = parse_int_arg(argc, argv, "--threads", 1);

    /* Derive grid and net count */
    g_grid_side = (int)ceil(sqrt((double)g_num_elems));
    g_num_nets  = g_num_elems * 2;
    g_swaps_per_temp = g_num_elems * 3;
    g_temperature = 2000.0;

    printf("Simulated Annealing (canneal) -- elems=%d nets=%d grid=%dx%d threads=%d\n",
           g_num_elems, g_num_nets, g_grid_side, g_grid_side, num_threads);

    /* Allocate */
    g_elems = (Element*)calloc((size_t)g_num_elems, sizeof(Element));
    g_nets  = (Net*)calloc((size_t)g_num_nets, sizeof(Net));
    if (!g_elems || !g_nets) { fprintf(stderr, "malloc failed\n"); return 1; }

    /* Initialize elements: random positions */
    uint32_t seed = 42;
    uint64_t eseed = 42;   /* element draws (64-bit generator) */
    for (int i = 0; i < g_num_elems; i++) {
        g_elems[i].x = (int)(bench_rand(&seed) % (uint32_t)g_grid_side);
        g_elems[i].y = (int)(bench_rand(&seed) % (uint32_t)g_grid_side);
        g_elems[i].num_nets = 0;
        for (int j = 0; j < MAX_NETS_PER_ELEM; j++)
            g_elems[i].nets[j] = -1;
    }

    /* Initialize nets: each net connects 2-4 random elements */
    for (int i = 0; i < g_num_nets; i++) {
        int ne = 2 + (int)(bench_rand(&seed) % 3); /* 2..4 */
        if (ne > 4) ne = 4;
        g_nets[i].num_elems = ne;
        for (int j = 0; j < ne; j++) {
            int eid = (int)(splitmix64(&eseed) % (uint64_t)g_num_elems);
            g_nets[i].elems[j] = eid;
            /* Add net to element (if room) */
            if (g_elems[eid].num_nets < MAX_NETS_PER_ELEM) {
                g_elems[eid].nets[g_elems[eid].num_nets] = i;
                g_elems[eid].num_nets++;
            }
        }
        for (int j = ne; j < 4; j++)
            g_nets[i].elems[j] = -1;
    }

    pthread_barrier_init(&g_barrier, NULL, (unsigned)num_threads);

    /* ---- ROI begin ---- */
    zsim_roi_begin();

    pthread_t*  threads = (pthread_t*)malloc((size_t)num_threads * sizeof(pthread_t));
    ThreadCtx*  ctxs    = (ThreadCtx*)malloc((size_t)num_threads * sizeof(ThreadCtx));

    for (int t = 0; t < num_threads; t++) {
        ctxs[t].id          = t;
        ctxs[t].num_threads = num_threads;
        ctxs[t].accepted    = 0;
        ctxs[t].seed        = (uint32_t)(42 + t * 1000);
        ctxs[t].eseed       = (uint64_t)(42 + t * 1000);
        pthread_create(&threads[t], NULL, worker, &ctxs[t]);
    }
    for (int t = 0; t < num_threads; t++)
        pthread_join(threads[t], NULL);

    zsim_roi_end();
    /* ---- ROI end ---- */

    /* Compute total cost and checksum */
    long total_cost = 0;
    for (int i = 0; i < g_num_nets; i++)
        total_cost += net_cost(i);

    int total_accepted = 0;
    for (int t = 0; t < num_threads; t++)
        total_accepted += ctxs[t].accepted;

    long checksum = total_cost + (long)total_accepted;

    printf("Final wire cost: %ld  Accepted swaps: %d\n", total_cost, total_accepted);
    printf("BENCH_CHECKSUM: %ld\n", checksum);
    printf("BENCH_DONE\n");

    pthread_barrier_destroy(&g_barrier);
    free(ctxs);
    free(threads);
    free(g_nets);
    free(g_elems);
    return 0;
}
