/* barnes.c -- Barnes-Hut N-body simulation (SPLASH-3 style)
 *
 * Octree-based gravitational N-body: build octree from particle positions,
 * compute forces via tree traversal (theta=0.5 opening criterion), update
 * velocities and positions for a few timesteps. The root cell is refitted to
 * the bodies' bounding cube every step and the total mass is 1, as in SPLASH-2.
 * Pointer-chase tree traversal (irregular, cache-unfriendly).
 *
 * Compile: g++ -std=c++11 -O2 -Wall -I<hooks-path> -lpthread -lm barnes.c -o barnes
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <pthread.h>
#include "zsim_hooks.h"

#define DEFAULT_SIZE    1024
#define DEFAULT_THREADS 4
#define TIMESTEPS       3
#define DT              0.01
#define THETA           0.5
#define SOFTENING       0.01
#define MAX_NODES       (DEFAULT_SIZE * 40)
/* 1.11.94 (b02-suites-a-3, ruling C1): depth bound for insert_body. The root
 * is refitted to the bodies every step, so a depth this large is reached only
 * when two bodies coincide to within 2^-MAX_TREE_DEPTH of the root side; the
 * build then refuses instead of recursing until the stack overflows. */
#define MAX_TREE_DEPTH  128

/* ------------------------------------------------------------------ */
/*  Arg parser                                                         */
/* ------------------------------------------------------------------ */
static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* ------------------------------------------------------------------ */
/*  Deterministic LCG                                                  */
/* ------------------------------------------------------------------ */
static uint32_t bench_rand(uint32_t* s) {
    *s = *s * 1103515245 + 12345;
    return (*s >> 16) & 0x7FFF;
}

/* ------------------------------------------------------------------ */
/*  Data structures                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    double x, y, z;
    double vx, vy, vz;
    double fx, fy, fz;
    double mass;
} body_t;

/* Octree node: either a leaf (one body) or internal (8 children) */
typedef struct octree_node {
    double cx, cy, cz;         /* center of mass */
    double total_mass;
    double size;               /* side length of this cell */
    double ox, oy, oz;         /* origin (lower corner) */
    int body_index;            /* -1 if internal or empty */
    int children[8];           /* indices into node pool, -1 if empty */
    int is_leaf;
} octree_node_t;

static body_t* bodies;
static int N;
static int num_threads;
static pthread_barrier_t barrier;

/* Octree pool (rebuilt each timestep) */
static octree_node_t* nodes;
static int node_count;
static int max_nodes;
static pthread_mutex_t tree_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/*  Octree construction (serial for correctness)                       */
/* ------------------------------------------------------------------ */
static int alloc_node(double ox, double oy, double oz, double size) {
    if (node_count >= max_nodes) return -1;
    int idx = node_count++;
    nodes[idx].ox = ox;  nodes[idx].oy = oy;  nodes[idx].oz = oz;
    nodes[idx].size = size;
    nodes[idx].total_mass = 0;
    nodes[idx].cx = nodes[idx].cy = nodes[idx].cz = 0;
    nodes[idx].body_index = -1;
    nodes[idx].is_leaf = 1;
    for (int c = 0; c < 8; c++) nodes[idx].children[c] = -1;
    return idx;
}

static int octant(octree_node_t* n, double x, double y, double z) {
    double mx = n->ox + n->size * 0.5;
    double my = n->oy + n->size * 0.5;
    double mz = n->oz + n->size * 0.5;
    return ((x >= mx) ? 1 : 0) | ((y >= my) ? 2 : 0) | ((z >= mz) ? 4 : 0);
}

static void child_origin(octree_node_t* n, int oct, double* cx, double* cy, double* cz) {
    double hs = n->size * 0.5;
    *cx = n->ox + ((oct & 1) ? hs : 0);
    *cy = n->oy + ((oct & 2) ? hs : 0);
    *cz = n->oz + ((oct & 4) ? hs : 0);
}

/* 1.11.94 (b02-suites-a-3, ruling C1): a full node pool or an unbounded
 * subdivision is a refusal, not a silent drop. Before this, alloc_node()
 * returning -1 dropped the body from the tree without a word, and bodies that
 * had left the fixed root cell recursed until the stack overflowed
 * (barnes_large, N=16384: SIGSEGV rc=139). */
static void tree_refuse(const char* what, int bi) {
    fprintf(stderr, "barnes: octree build refused: %s (body %d, N=%d, "
            "nodes %d of %d)\n", what, bi, N, node_count, max_nodes);
    exit(1);
}

static void insert_body(int node_idx, int bi, int depth) {
    if (depth > MAX_TREE_DEPTH)
        tree_refuse("subdivision deeper than MAX_TREE_DEPTH (coincident bodies)", bi);
    octree_node_t* n = &nodes[node_idx];
    if (n->is_leaf && n->body_index == -1) {
        /* Empty leaf: just place body here */
        n->body_index = bi;
        return;
    }
    if (n->is_leaf && n->body_index >= 0) {
        /* Occupied leaf: subdivide */
        int old_bi = n->body_index;
        n->body_index = -1;
        n->is_leaf = 0;
        /* Re-insert old body */
        int oc = octant(n, bodies[old_bi].x, bodies[old_bi].y, bodies[old_bi].z);
        double cx, cy, cz;
        child_origin(n, oc, &cx, &cy, &cz);
        int child = alloc_node(cx, cy, cz, n->size * 0.5);
        if (child < 0) tree_refuse("node pool exhausted", old_bi);
        n = &nodes[node_idx]; /* pool might have moved */
        n->children[oc] = child;
        insert_body(child, old_bi, depth + 1);
    }
    /* Internal node: insert into correct octant */
    n = &nodes[node_idx];
    int oc = octant(n, bodies[bi].x, bodies[bi].y, bodies[bi].z);
    if (n->children[oc] < 0) {
        double cx, cy, cz;
        child_origin(n, oc, &cx, &cy, &cz);
        int child = alloc_node(cx, cy, cz, n->size * 0.5);
        if (child < 0) tree_refuse("node pool exhausted", bi);
        n = &nodes[node_idx];
        n->children[oc] = child;
    }
    insert_body(n->children[oc], bi, depth + 1);
}

static void compute_com(int node_idx) {
    octree_node_t* n = &nodes[node_idx];
    if (n->is_leaf) {
        if (n->body_index >= 0) {
            body_t* b = &bodies[n->body_index];
            n->total_mass = b->mass;
            n->cx = b->x; n->cy = b->y; n->cz = b->z;
        }
        return;
    }
    double mx = 0, my = 0, mz = 0, mt = 0;
    for (int c = 0; c < 8; c++) {
        if (n->children[c] >= 0) {
            compute_com(n->children[c]);
            octree_node_t* ch = &nodes[n->children[c]];
            mx += ch->cx * ch->total_mass;
            my += ch->cy * ch->total_mass;
            mz += ch->cz * ch->total_mass;
            mt += ch->total_mass;
        }
    }
    if (mt > 0) { n->cx = mx / mt; n->cy = my / mt; n->cz = mz / mt; }
    n->total_mass = mt;
}

/* 1.11.94 (b02-suites-a-3, ruling C1): the root cell is the bodies' bounding
 * cube, refitted every timestep, as SPLASH-2 barnes does (setbound(): min/max
 * over all bodies, side = largest extent, root slightly larger so a body on the
 * max face still falls inside). The old root was fixed at [-0.5,1.5]^3; at
 * N=16384 15645 of 16384 bodies had left it after one step, so the tree held
 * bodies in cells that did not contain them. */
static int alloc_root(void) {
    double lo[3] = { bodies[0].x, bodies[0].y, bodies[0].z };
    double hi[3] = { lo[0], lo[1], lo[2] };
    for (int i = 1; i < N; i++) {
        double p[3] = { bodies[i].x, bodies[i].y, bodies[i].z };
        for (int k = 0; k < 3; k++) {
            if (p[k] < lo[k]) lo[k] = p[k];
            if (p[k] > hi[k]) hi[k] = p[k];
        }
    }
    double side = 0;
    for (int k = 0; k < 3; k++)
        if (hi[k] - lo[k] > side) side = hi[k] - lo[k];
    if (side <= 0) side = 1.0;   /* N == 1: any cube contains the body */
    side *= 1.00002;              /* SPLASH-2 setbound() margin */
    double ox = 0.5 * (lo[0] + hi[0]) - 0.5 * side;
    double oy = 0.5 * (lo[1] + hi[1]) - 0.5 * side;
    double oz = 0.5 * (lo[2] + hi[2]) - 0.5 * side;
    int root = alloc_node(ox, oy, oz, side);
    if (root < 0) tree_refuse("node pool exhausted", 0);
    return root;
}

/* ------------------------------------------------------------------ */
/*  Force computation (parallel tree walk)                             */
/* ------------------------------------------------------------------ */
static void compute_force_from_node(int bi, int node_idx) {
    octree_node_t* n = &nodes[node_idx];
    if (n->total_mass == 0) return;
    if (n->is_leaf && n->body_index == bi) return;

    double dx = n->cx - bodies[bi].x;
    double dy = n->cy - bodies[bi].y;
    double dz = n->cz - bodies[bi].z;
    double dist2 = dx * dx + dy * dy + dz * dz + SOFTENING * SOFTENING;
    double dist = sqrt(dist2);

    /* Opening criterion: if leaf or s/d < theta, treat as point mass */
    if (n->is_leaf || (n->size / dist < THETA)) {
        double inv_dist3 = 1.0 / (dist2 * dist);
        double f = n->total_mass * inv_dist3;
        bodies[bi].fx += f * dx;
        bodies[bi].fy += f * dy;
        bodies[bi].fz += f * dz;
    } else {
        /* Recurse into children */
        for (int c = 0; c < 8; c++)
            if (n->children[c] >= 0)
                compute_force_from_node(bi, n->children[c]);
    }
}

typedef struct {
    int tid;
    int root;
} worker_arg_t;

static void* force_worker(void* arg) {
    worker_arg_t* wa = (worker_arg_t*)arg;
    int tid = wa->tid;
    int root = wa->root;
    int chunk = (N + num_threads - 1) / num_threads;
    int lo = tid * chunk;
    int hi = lo + chunk;
    if (hi > N) hi = N;

    for (int i = lo; i < hi; i++) {
        bodies[i].fx = bodies[i].fy = bodies[i].fz = 0;
        compute_force_from_node(i, root);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char** argv) {
    N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    num_threads = parse_int_arg(argc, argv, "--threads", DEFAULT_THREADS);

    if (N < 1 || num_threads < 1) {
        fprintf(stderr, "barnes: --size and --threads must be >= 1 (got %d, %d)\n",
                N, num_threads);
        return 1;
    }
    printf("Barnes-Hut N-body -- N=%d, threads=%d, timesteps=%d\n", N, num_threads, TIMESTEPS);

    max_nodes = N * 40;
    /* 1.11.94 (b02-suites-a-3): gate injection hook. Random inputs never fill
     * a 40*N pool once the root is refitted, so the refusal path is exercised
     * by shrinking the pool: PIMID_BARNES_POOL_FAULT=<nodes>. */
    {
        const char* e = getenv("PIMID_BARNES_POOL_FAULT");
        if (e && atoi(e) > 0) max_nodes = atoi(e);
    }
    bodies = (body_t*)calloc((size_t)N, sizeof(body_t));
    nodes = (octree_node_t*)malloc((size_t)max_nodes * sizeof(octree_node_t));
    if (!bodies || !nodes) { fprintf(stderr, "malloc failed\n"); return 1; }

    /* Initialize bodies in unit cube */
    uint32_t seed = 42;
    for (int i = 0; i < N; i++) {
        bodies[i].x = (double)bench_rand(&seed) / 32768.0;
        bodies[i].y = (double)bench_rand(&seed) / 32768.0;
        bodies[i].z = (double)bench_rand(&seed) / 32768.0;
        bodies[i].vx = bodies[i].vy = bodies[i].vz = 0;
        /* 1.11.94 (b02-suites-a-3, ruling C1): total mass 1 (each body
         * 1/N), as SPLASH-2's Plummer model normalises it. With mass 1.0 each
         * and G = 1, accelerations grew with N and the system flew apart
         * within one DT at the large size. */
        bodies[i].mass = 1.0 / (double)N;
    }

    /* 1.11.94 (b02-suites-a-3): gate injection hook for the depth refusal:
     * PIMID_BARNES_COINCIDE_FAULT=1 puts body 1 on body 0. */
    {
        const char* e = getenv("PIMID_BARNES_COINCIDE_FAULT");
        if (e && atoi(e) > 0 && N > 1) {
            bodies[1].x = bodies[0].x; bodies[1].y = bodies[0].y; bodies[1].z = bodies[0].z;
        }
    }

    pthread_t* threads = (pthread_t*)malloc((size_t)num_threads * sizeof(pthread_t));
    worker_arg_t* wargs = (worker_arg_t*)malloc((size_t)num_threads * sizeof(worker_arg_t));

    /* ---- ROI begin ---- */
    zsim_roi_begin();

    for (int step = 0; step < TIMESTEPS; step++) {
        /* Build octree (serial) */
        node_count = 0;
        int root = alloc_root();   /* bounding cube of the bodies, this step */
        for (int i = 0; i < N; i++)
            insert_body(root, i, 0);
        compute_com(root);

        /* Compute forces (parallel) */
        for (int t = 0; t < num_threads; t++) {
            wargs[t].tid = t;
            wargs[t].root = root;
        }
        for (int t = 1; t < num_threads; t++)
            pthread_create(&threads[t], NULL, force_worker, &wargs[t]);
        force_worker(&wargs[0]);
        for (int t = 1; t < num_threads; t++)
            pthread_join(threads[t], NULL);

        /* Update velocities and positions (serial, lightweight) */
        for (int i = 0; i < N; i++) {
            bodies[i].vx += bodies[i].fx * DT;
            bodies[i].vy += bodies[i].fy * DT;
            bodies[i].vz += bodies[i].fz * DT;
            bodies[i].x += bodies[i].vx * DT;
            bodies[i].y += bodies[i].vy * DT;
            bodies[i].z += bodies[i].vz * DT;
        }
    }

    zsim_roi_end();
    /* ---- ROI end ---- */

    /* Checksum: sum of final positions */
    double checksum = 0.0;
    for (int i = 0; i < N; i++)
        checksum += bodies[i].x + bodies[i].y + bodies[i].z;

    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(wargs);
    free(threads);
    free(nodes);
    free(bodies);
    return 0;
}
