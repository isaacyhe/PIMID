/* bfs_mpi.c -- Breadth-First Search on random graph (adjacency list via CSR)
 * MPI parallelization. All ranks share full graph and dist[]. Frontier
 * expansion is split across ranks; Allgatherv collects candidates, then
 * all ranks perform identical dedup to maintain consistent state.
 *
 * Device-organization aware (PIMID), mirroring bfs_omp.c's choice of WHICH
 * arrays get slotted: the CSR (row_ptr, col_idx) and dist go in the PE slot.
 * 1.12.17: unlike bfs_omp, where the frontier IS the shared structure, every
 * rank here has its OWN private frontier, candidates, all_candidates (the
 * Allgatherv receive buffer), recvcounts and displs, so they go in the rank's
 * PE slot too (they used to stay in the shared heap, which put all ranks'
 * copies on a few heap units). D1 identity mapping -- rank r owns PE r's slot (enforced ranks ==
 * num_pes). STRUCTURAL NOTE: unlike bfs_omp (which owner-partitions the CSR by
 * vertex range because OMP threads share one address space), bfs_mpi replicates
 * the FULL graph + dist on every rank (its Allgatherv model), so each rank
 * SPMD-relocates its OWN full replicated row_ptr/col_idx/dist into its OWN PE
 * slot -- analogous to gemv_mpi slotting its replicated x. The ROI's frontier
 * expansion + dedup then read/write the CSR and dist LOCAL; the cross-PE traffic
 * is the Allgatherv (priced by the NoC model). Arithmetic is identical. Coarse
 * placement runs the host-layout path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include "zsim_hooks.h"
#include "../../include/pimid_devorg.h"
#include "../../include/pimid_spm.h"

#define DEFAULT_VERTICES 512
#define DEFAULT_DEGREE   8

static uint32_t bench_rand(uint32_t* s) {
    *s = *s * 1103515245 + 12345;
    return (*s >> 16) & 0x7FFF;
}

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* 1.12.18: the message-passing claim rule for bfs_expand: test dist[v] only
 * and append every unvisited neighbour to this rank's candidate list (the
 * ranks deduplicate after the Allgatherv, in bfs_dedup). */
typedef struct { const int* row_ptr; const int* col_idx; int* dist; int level; } bfs_ctx_t;

static inline const int* bfs_adj(const bfs_ctx_t* c, int u, size_t* n) {
    const int e0 = c->row_ptr[u], e1 = c->row_ptr[u + 1];
    *n = (size_t)(e1 - e0);
    return c->col_idx + e0;
}

static inline int bfs_visit(const bfs_ctx_t* c, int v) {
    return c->dist[v] == -1;
}

/* 1.12.18: expand frontier[0..nf) by one level through the PE scratchpad
 * (pimid_spm.h). The RAM holds three equal buffers: a tile of the frontier, a
 * tile of one vertex's adjacency row (col_idx) and a tile of the output list
 * (the vertices bfs_visit asks to append). Each frontier entry and each
 * adjacency row is fetched once; the output list is written back a full tile
 * at a time and at the end. The row_ptr[u] lookup and the dist[v] test are
 * irregular and stay direct DRAM accesses (bfs_adj / bfs_visit, the part each
 * variant defines for its own claim rule and layout). With no scratchpad the
 * tiles are the whole range and this is the plain frontier loop. Returns the
 * number of vertices appended to out. */
static size_t bfs_expand(pimid_spm_t* s0, const bfs_ctx_t* c0, const int* frontier, size_t nf,
                         int* out) {
    const bfs_ctx_t cl = *c0, *c = &cl;   /* local copies: no store below can alias them */
    pimid_spm_t sl = *s0, *s = &sl;
    const size_t T = pimid_spm_tile(s, 3, sizeof(int));
    const size_t of = 0, oa = s->cap ? T * sizeof(int) : 0, oo = s->cap ? 2 * T * sizeof(int) : 0;
    size_t nout = 0, no = 0;
    int* ob = (int*)pimid_spm_out(s, oo, out);
    for (size_t fb = 0; fb < nf; fb += T) {
        const size_t nb = (nf - fb < T) ? nf - fb : T;
        const int* fl = (const int*)pimid_spm_in(s, of, frontier + fb, nb * sizeof(int));
        for (size_t f = 0; f < nb; f++) {
            size_t ne;
            const int* adj = bfs_adj(c, fl[f], &ne);
            for (size_t eb = 0; eb < ne; eb += T) {
                const size_t na = (ne - eb < T) ? ne - eb : T;
                const int* ab = (const int*)pimid_spm_in(s, oa, adj + eb, na * sizeof(int));
                for (size_t e = 0; e < na; e++) {
                    const int v = ab[e];
                    if (bfs_visit(c, v)) {
                        ob[no++] = v;
                        if (no == T) {   /* out tile full (only with a scratchpad) */
                            pimid_spm_flush(s, out + nout, ob, no * sizeof(int));
                            nout += no; no = 0;
                            ob = (int*)pimid_spm_out(s, oo, out + nout);
                        }
                    }
                }
            }
        }
    }
    pimid_spm_flush(s, out + nout, ob, no * sizeof(int));
    return nout + no;
}

/* 1.12.18 (message passing only): the identical dedup every rank runs over
 * the gathered candidates cand[0..ncand), through the PE scratchpad. The RAM
 * holds two equal buffers: a tile of the candidates (each fetched once) and a
 * tile of the new frontier (written back a full tile at a time and at the
 * end). The dist[v] test-and-set is irregular and stays a direct DRAM access.
 * cand and out are ordinary memory (cand is the Allgatherv receive buffer;
 * nothing in the RAM is handed to MPI). Returns the new frontier size. */
static size_t bfs_dedup(pimid_spm_t* s0, int* dist, const int* cand, size_t ncand, int* out,
                        int level) {
    pimid_spm_t sl = *s0, *s = &sl;   /* a local copy of the handle */
    const size_t T = pimid_spm_tile(s, 2, sizeof(int));
    const size_t oc = 0, oo = s->cap ? T * sizeof(int) : 0;
    size_t nout = 0, no = 0;
    int* ob = (int*)pimid_spm_out(s, oo, out);
    for (size_t cb = 0; cb < ncand; cb += T) {
        const size_t nc = (ncand - cb < T) ? ncand - cb : T;
        const int* cl = (const int*)pimid_spm_in(s, oc, cand + cb, nc * sizeof(int));
        for (size_t i = 0; i < nc; i++) {
            int v = cl[i];
            if (dist[v] == -1) {
                dist[v] = level;
                ob[no++] = v;
                if (no == T) {   /* out tile full (only with a scratchpad) */
                    pimid_spm_flush(s, out + nout, ob, no * sizeof(int));
                    nout += no; no = 0;
                    ob = (int*)pimid_spm_out(s, oo, out + nout);
                }
            }
        }
    }
    pimid_spm_flush(s, out + nout, ob, no * sizeof(int));
    return nout + no;
}

/* Candidate buffer bound (1.11.94, ruling H43): ceil(V/nprocs)*deg, never below V. */
static size_t bfs_mpi_cand_cap(long v, int deg, int nprocs) {
    size_t cap = (size_t)((v + nprocs - 1) / nprocs) * (size_t)deg;
    return cap < (size_t)v ? (size_t)v : cap;
}

/* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): need of the full replicated
 * [row_ptr V+1][col_idx V*deg][dist V] every rank slots; 1.12.17: plus the rank's
 * private [frontier V][candidates cap][all_candidates V*deg][recvcounts n][displs n]. */
typedef struct { int deg, nprocs; } bfs_mpi_geom_t;
static size_t bfs_mpi_need(long v, const void* c) {
    const bfs_mpi_geom_t* g = (const bfs_mpi_geom_t*)c;
    return ((size_t)(v + 1) + (size_t)v * (size_t)g->deg + (size_t)v
            + (size_t)v + bfs_mpi_cand_cap(v, g->deg, g->nprocs) + (size_t)v * (size_t)g->deg
            + 2 * (size_t)g->nprocs) * sizeof(int);
}

int main(int argc, char* argv[]) {
    MPI_Init(&argc, &argv);

    int rank, nprocs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    int V = parse_int_arg(argc, argv, "--vertices", DEFAULT_VERTICES);
    int deg = parse_int_arg(argc, argv, "--degree", DEFAULT_DEGREE);
    int source = parse_int_arg(argc, argv, "--source", 0);

    /* Device organization + D1 identity-mapping guard: ranks must == num_pes. */
    pimid_devorg_t dev = pimid_devorg_from_args(argc, argv);
    if (nprocs > dev.num_pes) {
        if (rank == 0)
            fprintf(stderr, "pimid_devorg: MPI world size (%d) != num_pes (%d); "
                    "identity mapping requires ranks <= PEs (rank r -> slot r). Aborting.\n",
                    nprocs, dev.num_pes);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    /* All ranks generate the same graph with the same seed */
    uint32_t seed = 42;
    int total_edges = V * deg;
    int* row_ptr = (int*)malloc((V + 1) * sizeof(int));
    int* col_idx = (int*)malloc(total_edges * sizeof(int));
    int* dist = (int*)malloc(V * sizeof(int));
    int* frontier = (int*)malloc(V * sizeof(int));
    /* 1.11.94 (b01-pim-kernels-3, ruling H43): a rank appends every unvisited
     * neighbour of its frontier share without deduplicating, so ncand reaches
     * my_count*deg <= ceil(V/nprocs)*deg, which exceeds V when nprocs < deg
     * (1 rank at 512/8 peaked at 788 > 512: heap overflow). Sized by that
     * bound, never below the old V, so runs with nprocs >= deg keep the same
     * allocation and the ROI is unchanged (a local visited-bitmap dedup would
     * add work to the ROI of every cell). */
    size_t cand_cap = bfs_mpi_cand_cap(V, deg, nprocs);
    int* candidates = (int*)malloc(cand_cap * sizeof(int));
    int* all_candidates = (int*)malloc(V * deg * sizeof(int));
    int* recvcounts = (int*)malloc(nprocs * sizeof(int));
    int* displs = (int*)malloc(nprocs * sizeof(int));
    if (!row_ptr || !col_idx || !dist || !frontier || !candidates ||
        !all_candidates || !recvcounts || !displs) {
        fprintf(stderr, "rank %d: malloc failed\n", rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    for (int i = 0; i <= V; i++) row_ptr[i] = i * deg;
    for (int i = 0; i < V; i++)
        for (int d = 0; d < deg; d++)
            col_idx[i * deg + d] = bench_rand(&seed) % V;

    for (int i = 0; i < V; i++) dist[i] = -1;
    dist[source] = 0;
    frontier[0] = source;
    int fsize = 1;

    /* ---- DATA PREP (before ROI, not timed): this rank's full replicated CSR +
     * dist -> its PE slot, followed by its private work buffers (1.12.17). Slot
     * layout: [row_ptr V+1][col_idx V*deg][dist V][frontier V][candidates cap]
     * [all_candidates V*deg][recvcounts n][displs n]. rp/ci/ds/fr/cd/ac/rc/dp are
     * the working pointers (slot-resident when prepped).
     * Assumes thread-MPI (the default/only supported mode): slot r maps to PE
     * r's units directly. Under the retired PIMID_MPI_PROCESS=1 legacy mode the
     * simulator's per-rank unit rotation (pe_memory_interface.h) would
     * double-shift these prepped slots. */
    int prep = pimid_devorg_needs_prep(&dev);
    bfs_mpi_geom_t geom = { deg, nprocs };
    size_t need = bfs_mpi_need(V, &geom);
    size_t slot_bytes = 0;
    int* dbuf = NULL;
    int* rp = row_ptr;
    int* ci = col_idx;
    int* ds = dist;
    int* fr = frontier;
    int* cd = candidates;
    int* ac = all_candidates;
    int* rc = recvcounts;
    int* dp = displs;
    if (prep) {
        /* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): a need larger than the slot
         * is refused; it used to fall back to host layout in silence. */
        slot_bytes = pimid_devorg_slot_bytes(&dev);
        if (need > slot_bytes) {
            char who[32];
            snprintf(who, sizeof who, "rank %d: ", rank);
            pimid_devorg_report_slot_refusal(who, "bfs", &dev, need, slot_bytes, "--vertices",
                pimid_devorg_largest_fit(bfs_mpi_need, &geom, V, slot_bytes));
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        dbuf = (int*)pimid_devorg_alloc(&dev, &slot_bytes);
        if (!dbuf) {
            fprintf(stderr, "rank %d: devorg: alloc of the device buffer failed; "
                    "running host-layout (no prep)\n", rank);
            prep = 0;
        }
    }
    if (prep) {
        int* slot = (int*)pimid_devorg_pe_slot(dbuf, rank, slot_bytes);
        rp = slot;
        ci = slot + (V + 1);
        ds = slot + (V + 1) + (size_t)V * deg;
        fr = ds + V;
        cd = fr + V;
        ac = cd + cand_cap;
        rc = ac + (size_t)V * deg;
        dp = rc + nprocs;
        memcpy(rp, row_ptr, (size_t)(V + 1) * sizeof(int));
        memcpy(ci, col_idx, (size_t)V * deg * sizeof(int));
        memcpy(ds, dist, (size_t)V * sizeof(int));
        fr[0] = frontier[0];
    }

    if (prep)
        fprintf(stderr, "[devorg] PREP ACTIVE: kernel=bfs level=%s pes=%d "
                "rank=%d slot_bytes=%zu\n",
                pimid_devorg_level_name(dev.level), dev.num_pes, rank, slot_bytes);

    pimid_spm_t spm;   /* 1.12.18: this rank's PE scratchpad */
    pimid_spm_open(&spm);

    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) zsim_roi_begin();

    int level = 0;
    /* 1.12.18: no scratchpad -- the 1.12.17 code, unchanged */
    if (!spm.cap) {
        while (fsize > 0) {
            level++;

            /* Split frontier across ranks */
            int base = fsize / nprocs;
            int rem  = fsize % nprocs;
            int my_start = rank * base + (rank < rem ? rank : rem);
            int my_count = base + (rank < rem ? 1 : 0);

            /* Expand: collect candidate neighbors (may have duplicates) */
            int ncand = 0;
            for (int f = my_start; f < my_start + my_count; f++) {
                int u = fr[f];
                for (int e = rp[u]; e < rp[u + 1]; e++) {
                    int v = ci[e];
                    if (ds[v] == -1)
                        cd[ncand++] = v;
                }
            }

            /* Gather all candidates */
            MPI_Allgather(&ncand, 1, MPI_INT, rc, 1, MPI_INT, MPI_COMM_WORLD);
            dp[0] = 0;
            for (int i = 1; i < nprocs; i++)
                dp[i] = dp[i - 1] + rc[i - 1];
            int total_cand = dp[nprocs - 1] + rc[nprocs - 1];

            MPI_Allgatherv(cd, ncand, MPI_INT,
                           ac, rc, dp, MPI_INT,
                           MPI_COMM_WORLD);

            /* All ranks deduplicate identically */
            int new_fsize = 0;
            for (int i = 0; i < total_cand; i++) {
                int v = ac[i];
                if (ds[v] == -1) {
                    ds[v] = level;
                    fr[new_fsize++] = v;
                }
            }
            fsize = new_fsize;
        }
    } else {
        while (fsize > 0) {
            level++;

            /* Split frontier across ranks */
            int base = fsize / nprocs;
            int rem  = fsize % nprocs;
            int my_start = rank * base + (rank < rem ? rank : rem);
            int my_count = base + (rank < rem ? 1 : 0);

            /* Expand: collect candidate neighbors (may have duplicates); they are
             * flushed to cd (ordinary memory) before the Allgatherv below */
            const bfs_ctx_t ctx = { rp, ci, ds, level };
            int ncand = (int)bfs_expand(&spm, &ctx, fr + my_start, (size_t)my_count, cd);

            /* Gather all candidates */
            MPI_Allgather(&ncand, 1, MPI_INT, rc, 1, MPI_INT, MPI_COMM_WORLD);
            dp[0] = 0;
            for (int i = 1; i < nprocs; i++)
                dp[i] = dp[i - 1] + rc[i - 1];
            int total_cand = dp[nprocs - 1] + rc[nprocs - 1];

            MPI_Allgatherv(cd, ncand, MPI_INT,
                           ac, rc, dp, MPI_INT,
                           MPI_COMM_WORLD);

            /* All ranks deduplicate identically */
            int new_fsize = (int)bfs_dedup(&spm, ds, ac, (size_t)total_cand, fr, level);
            fsize = new_fsize;
        }
    }

    if (rank == 0) zsim_roi_end();
    /* 1.11.98 (user ruling 2026-10-03): the closing barrier every other MPI
     * kernel has. The device-scope ROI ends when the LAST rank arrives here
     * (the last-rank rule of 1.11.97); without it bfs froze its statistics
     * at guest exit, inside rank 0's post-ROI visited-count loop. */
    MPI_Barrier(MPI_COMM_WORLD);
    pimid_spm_close(&spm);

    /* Gather this rank's dist back to host layout for the (untimed) check. */
    if (prep)
        memcpy(dist, ds, (size_t)V * sizeof(int));

    if (rank == 0) {
        long visited = 0;
        for (int i = 0; i < V; i++)
            if (dist[i] >= 0) visited++;

        printf("BENCH_LEVELS: %d\n", level);
        printf("BENCH_VISITED: %ld\n", visited);
        printf("BENCH_CHECKSUM: %ld\n", visited);
        printf("BENCH_DONE\n");
    }

    free(row_ptr); free(col_idx); free(dist);
    free(frontier); free(candidates); free(all_candidates);
    free(recvcounts); free(displs);
    MPI_Finalize();
    return 0;
}
