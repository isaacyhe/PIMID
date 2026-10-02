/* fmm.c -- 2D quadtree tree-walk force kernel (SPLASH-3 "fmm" slot).
 *
 * 1.11.94 (b02-suites-a-2, ruling C2): this is NOT a fast multipole method.
 * There are no multipole or local expansions, no M2M/M2L/L2L translations
 * and no per-particle direct near field. What it does:
 *   - builds a quadtree (leaf capacity MAX_PARTICLES, depth limit MAX_DEPTH;
 *     a leaf at the depth limit keeps every particle it receives),
 *   - computes each cell's total mass and centre of mass bottom-up,
 *   - for each particle (pthreads, static partition) walks the tree from the
 *     root: a cell that passes the opening test (size / r < THETA) or is a
 *     leaf contributes as a point mass at its centre of mass, otherwise the
 *     walk descends into its children. A particle's own leaf is included as
 *     a point mass like any other leaf.
 * It is a Barnes-Hut-style tree walk that stops at leaf level; the name is
 * kept because the suite and its configs are keyed on it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include "zsim_hooks.h"

#define DEFAULT_SIZE    1024
#define DEFAULT_THREADS 4
#define MAX_DEPTH       6
#define MAX_PARTICLES   32   /* max particles per leaf before subdivision */
#define THETA           0.5  /* opening angle for well-separated test */

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

static uint32_t bench_rand(uint32_t* s) {
    *s = *s * 1103515245 + 12345;
    return (*s >> 16) & 0x7FFF;
}

typedef struct {
    double x, y;
    double mass;
    double fx, fy;       /* accumulated force */
    double potential;
} Particle;

/* Quadtree node -- flat array allocation */
typedef struct QNode {
    double cx, cy, size; /* center and half-size */
    double mp_mass;      /* monopole: total mass */
    double mp_x, mp_y;   /* center of mass */
    int children[4];     /* indices into node pool (-1 = none) */
    int* plist;          /* particle indices (leaf only) */
    int pcount;          /* number of particles */
    int pcap;            /* plist capacity (ints) */
    int is_leaf;
} QNode;

static Particle* particles;
static int nparticles, nthreads;
static QNode* nodes;
static int node_count, node_cap;
static pthread_barrier_t barrier;
static pthread_mutex_t node_lock;

static int alloc_node(double cx, double cy, double sz) {
    int idx;
    pthread_mutex_lock(&node_lock);
    if (node_count >= node_cap) {
        node_cap *= 2;
        QNode* p = (QNode*)realloc(nodes, (size_t)node_cap * sizeof(QNode));
        if (!p) {
            fprintf(stderr, "fmm: node pool growth to %d nodes failed\n", node_cap);
            exit(1);
        }
        nodes = p;
    }
    idx = node_count++;
    pthread_mutex_unlock(&node_lock);
    nodes[idx].cx = cx;
    nodes[idx].cy = cy;
    nodes[idx].size = sz;
    nodes[idx].mp_mass = 0;
    nodes[idx].mp_x = 0;
    nodes[idx].mp_y = 0;
    nodes[idx].children[0] = nodes[idx].children[1] = -1;
    nodes[idx].children[2] = nodes[idx].children[3] = -1;
    nodes[idx].plist = NULL;
    nodes[idx].pcount = 0;
    nodes[idx].pcap = 0;
    nodes[idx].is_leaf = 1;
    return idx;
}

/* 1.11.94 (b02-suites-a-2, ruling C2): growable leaf list. A leaf at the
 * depth limit keeps every particle it is given, so nothing is dropped. */
static void plist_push(int ni, int pi) {
    QNode* n = &nodes[ni];
    if (n->pcount == n->pcap) {
        int cap = n->pcap ? 2 * n->pcap : MAX_PARTICLES;
        int* p = (int*)realloc(n->plist, (size_t)cap * sizeof(int));
        if (!p) {
            fprintf(stderr, "fmm: leaf particle list growth to %d entries failed\n", cap);
            exit(1);
        }
        n->plist = p;
        n->pcap = cap;
    }
    n->plist[n->pcount++] = pi;
}

static int child_quadrant(int ni, int pi) {
    const QNode* n = &nodes[ni];
    return (particles[pi].x >= n->cx ? 1 : 0) + (particles[pi].y >= n->cy ? 2 : 0);
}

/* Insert particle into tree (serial, during build phase). The root is depth 0.
 *
 * 1.11.94 (b02-suites-a-2, ruling C2): a full leaf above the depth limit
 * splits and each of its particles is reinserted into the CHILD that contains
 * it, at the child's depth. Before this, the particles were reinserted into
 * the splitting node itself at depth+1; at the depth limit that reached a
 * fallback which appended to the very list being walked (a 32-int buffer):
 * heap overflow and an endless loop on fmm_large (N=65536, SIGSEGV rc=139),
 * and fallback particles on internal nodes were ignored by the force walk.
 * Node pointers are re-taken after alloc_node(), which may realloc the pool. */
static void tree_insert(int ni, int pi, int depth) {
    if (nodes[ni].is_leaf) {
        if (nodes[ni].pcount < MAX_PARTICLES || depth >= MAX_DEPTH) {
            plist_push(ni, pi);
            return;
        }
        /* Subdivide: detach the old list first, so nothing is appended to a
         * list while it is being walked. */
        int* old = nodes[ni].plist;
        int cnt = nodes[ni].pcount;
        nodes[ni].plist = NULL;
        nodes[ni].pcount = 0;
        nodes[ni].pcap = 0;
        nodes[ni].is_leaf = 0;
        double hs = nodes[ni].size * 0.5;
        double offsets[4][2] = {{-hs, -hs}, {hs, -hs}, {-hs, hs}, {hs, hs}};
        for (int c = 0; c < 4; c++) {
            int k = alloc_node(nodes[ni].cx + offsets[c][0],
                               nodes[ni].cy + offsets[c][1], hs);
            nodes[ni].children[c] = k;
        }
        for (int i = 0; i < cnt; i++)
            tree_insert(nodes[ni].children[child_quadrant(ni, old[i])], old[i], depth + 1);
        free(old);
    }
    /* Internal node: all four children exist. */
    tree_insert(nodes[ni].children[child_quadrant(ni, pi)], pi, depth + 1);
}

/* Compute each cell's total mass and centre of mass bottom-up */
static void compute_multipoles(int ni) {
    QNode* n = &nodes[ni];
    if (n->is_leaf) {
        double tm = 0, tx = 0, ty = 0;
        for (int i = 0; i < n->pcount; i++) {
            int pi = n->plist[i];
            double m = particles[pi].mass;
            tm += m;
            tx += m * particles[pi].x;
            ty += m * particles[pi].y;
        }
        n->mp_mass = tm;
        n->mp_x = (tm > 0) ? tx / tm : n->cx;
        n->mp_y = (tm > 0) ? ty / tm : n->cy;
        return;
    }
    double tm = 0, tx = 0, ty = 0;
    for (int c = 0; c < 4; c++) {
        if (n->children[c] < 0) continue;
        compute_multipoles(n->children[c]);
        QNode* ch = &nodes[n->children[c]];
        tm += ch->mp_mass;
        tx += ch->mp_mass * ch->mp_x;
        ty += ch->mp_mass * ch->mp_y;
    }
    n->mp_mass = tm;
    n->mp_x = (tm > 0) ? tx / tm : n->cx;
    n->mp_y = (tm > 0) ? ty / tm : n->cy;
}

/* Evaluate: for each particle, walk the tree (Barnes-Hut style) */
static void eval_force(int ni, int pi) {
    QNode* n = &nodes[ni];
    if (n->mp_mass < 1e-15) return;
    double dx = particles[pi].x - n->mp_x;
    double dy = particles[pi].y - n->mp_y;
    double r2 = dx * dx + dy * dy + 1e-10;
    double r = sqrt(r2);
    /* Opening test: a far cell, or a leaf, acts as its point mass */
    if (n->is_leaf || (n->size / r < THETA)) {
        double f = -particles[pi].mass * n->mp_mass / (r2 * r);
        particles[pi].fx += f * dx;
        particles[pi].fy += f * dy;
        particles[pi].potential += -n->mp_mass / r;
        return;
    }
    /* Otherwise recurse into children */
    for (int c = 0; c < 4; c++) {
        if (n->children[c] >= 0)
            eval_force(n->children[c], pi);
    }
}

typedef struct { int tid; } ThreadArg;

static void* worker(void* arg) {
    int tid = ((ThreadArg*)arg)->tid;
    int lo = (nparticles * tid) / nthreads;
    int hi = (nparticles * (tid + 1)) / nthreads;

    /* Each thread evaluates forces for its partition of particles */
    for (int i = lo; i < hi; i++) {
        particles[i].fx = 0;
        particles[i].fy = 0;
        particles[i].potential = 0;
        eval_force(0, i);
    }
    return NULL;
}

int main(int argc, char* argv[]) {
    nparticles = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    nthreads = parse_int_arg(argc, argv, "--threads", DEFAULT_THREADS);

    particles = (Particle*)malloc((size_t)nparticles * sizeof(Particle));
    node_cap = nparticles * 4 + 256;
    nodes = (QNode*)malloc((size_t)node_cap * sizeof(QNode));
    node_count = 0;
    if (!particles || !nodes) { fprintf(stderr, "malloc failed\n"); return 1; }

    pthread_mutex_init(&node_lock, NULL);
    pthread_barrier_init(&barrier, NULL, (unsigned)nthreads);

    /* Deterministic particle positions */
    uint32_t seed = 42;
    for (int i = 0; i < nparticles; i++) {
        particles[i].x = 10.0 * bench_rand(&seed) / 32768.0;
        particles[i].y = 10.0 * bench_rand(&seed) / 32768.0;
        particles[i].mass = 0.5 + 1.5 * bench_rand(&seed) / 32768.0;
        particles[i].fx = 0;
        particles[i].fy = 0;
        particles[i].potential = 0;
    }

    /* Build quadtree (serial) */
    alloc_node(5.0, 5.0, 5.0); /* root node centered at (5,5), half-size=5 */
    for (int i = 0; i < nparticles; i++)
        tree_insert(0, i, 0);
    compute_multipoles(0);

    /* Parallel force evaluation */
    pthread_t* threads = (pthread_t*)malloc((size_t)nthreads * sizeof(pthread_t));
    ThreadArg* args = (ThreadArg*)malloc((size_t)nthreads * sizeof(ThreadArg));

    zsim_roi_begin();
    for (int t = 1; t < nthreads; t++) {
        args[t].tid = t;
        pthread_create(&threads[t], NULL, worker, &args[t]);
    }
    args[0].tid = 0;
    worker(&args[0]);
    for (int t = 1; t < nthreads; t++)
        pthread_join(threads[t], NULL);
    zsim_roi_end();

    /* Checksum: sum of potentials */
    double checksum = 0.0;
    for (int i = 0; i < nparticles; i++)
        checksum += particles[i].potential;
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    /* Cleanup */
    for (int i = 0; i < node_count; i++)
        if (nodes[i].plist) free(nodes[i].plist);
    pthread_barrier_destroy(&barrier);
    pthread_mutex_destroy(&node_lock);
    free(threads); free(args);
    free(particles); free(nodes);
    return 0;
}
