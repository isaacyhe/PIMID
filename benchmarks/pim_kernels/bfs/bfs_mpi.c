/* bfs_mpi.c -- Breadth-First Search on random graph (adjacency list via CSR)
 * MPI parallelization. All ranks share full graph and dist[]. Frontier
 * expansion is split across ranks; Allgatherv collects candidates, then
 * all ranks perform identical dedup to maintain consistent state.
 *
 * Device-organization aware (PIMID), mirroring bfs_omp.c's choice of WHICH
 * arrays get slotted: the CSR (row_ptr, col_idx) and dist go in the PE slot;
 * the frontier/candidate work queues stay host-side (they ARE the shared
 * structure). D1 identity mapping -- rank r owns PE r's slot (enforced ranks ==
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

/* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): need of the full replicated
 * [row_ptr V+1][col_idx V*deg][dist V] every rank slots. */
static size_t bfs_mpi_need(long v, const void* c) {
    int deg = *(const int*)c;
    return ((size_t)(v + 1) + (size_t)v * (size_t)deg + (size_t)v) * sizeof(int);
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
    size_t cand_cap = (size_t)((V + nprocs - 1) / nprocs) * (size_t)deg;
    if (cand_cap < (size_t)V) cand_cap = (size_t)V;
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
     * dist -> its PE slot. Slot layout: [row_ptr V+1][col_idx V*deg][dist V].
     * rp/ci/ds are the working pointers (slot-resident when prepped).
     * Assumes thread-MPI (the default/only supported mode): slot r maps to PE
     * r's units directly. Under the retired PIMID_MPI_PROCESS=1 legacy mode the
     * simulator's per-rank unit rotation (pe_memory_interface.h) would
     * double-shift these prepped slots. */
    int prep = pimid_devorg_needs_prep(&dev);
    size_t need = ((size_t)(V + 1) + (size_t)V * deg + (size_t)V) * sizeof(int);
    size_t slot_bytes = 0;
    int* dbuf = NULL;
    int* rp = row_ptr;
    int* ci = col_idx;
    int* ds = dist;
    if (prep) {
        /* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): a need larger than the slot
         * is refused; it used to fall back to host layout in silence. */
        slot_bytes = pimid_devorg_slot_bytes(&dev);
        if (need > slot_bytes) {
            char who[32];
            snprintf(who, sizeof who, "rank %d: ", rank);
            pimid_devorg_report_slot_refusal(who, "bfs", &dev, need, slot_bytes, "--vertices",
                pimid_devorg_largest_fit(bfs_mpi_need, &deg, V, slot_bytes));
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
        memcpy(rp, row_ptr, (size_t)(V + 1) * sizeof(int));
        memcpy(ci, col_idx, (size_t)V * deg * sizeof(int));
        memcpy(ds, dist, (size_t)V * sizeof(int));
    }

    if (prep)
        fprintf(stderr, "[devorg] PREP ACTIVE: kernel=bfs level=%s pes=%d "
                "rank=%d slot_bytes=%zu\n",
                pimid_devorg_level_name(dev.level), dev.num_pes, rank, slot_bytes);

    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) zsim_roi_begin();

    int level = 0;
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
            int u = frontier[f];
            for (int e = rp[u]; e < rp[u + 1]; e++) {
                int v = ci[e];
                if (ds[v] == -1)
                    candidates[ncand++] = v;
            }
        }

        /* Gather all candidates */
        MPI_Allgather(&ncand, 1, MPI_INT, recvcounts, 1, MPI_INT, MPI_COMM_WORLD);
        displs[0] = 0;
        for (int i = 1; i < nprocs; i++)
            displs[i] = displs[i - 1] + recvcounts[i - 1];
        int total_cand = displs[nprocs - 1] + recvcounts[nprocs - 1];

        MPI_Allgatherv(candidates, ncand, MPI_INT,
                       all_candidates, recvcounts, displs, MPI_INT,
                       MPI_COMM_WORLD);

        /* All ranks deduplicate identically */
        int new_fsize = 0;
        for (int i = 0; i < total_cand; i++) {
            int v = all_candidates[i];
            if (ds[v] == -1) {
                ds[v] = level;
                frontier[new_fsize++] = v;
            }
        }
        fsize = new_fsize;
    }

    if (rank == 0) zsim_roi_end();

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
