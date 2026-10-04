/* bfs_mpi.c — Breadth-First Search on random graph (adjacency list via CSR)
 * MPI parallelization. All ranks share full graph and dist[]. Frontier
 * expansion is split across ranks; Allgatherv collects candidates, then
 * all ranks perform identical dedup to maintain consistent state. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include "zsim_hooks.h"

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

int main(int argc, char* argv[]) {
    MPI_Init(&argc, &argv);

    int rank, nprocs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    int V = parse_int_arg(argc, argv, "--vertices", DEFAULT_VERTICES);
    int deg = parse_int_arg(argc, argv, "--degree", DEFAULT_DEGREE);
    int source = parse_int_arg(argc, argv, "--source", 0);

    /* All ranks generate the same graph with the same seed */
    uint32_t seed = 42;
    int total_edges = V * deg;
    int* row_ptr = (int*)malloc((V + 1) * sizeof(int));
    int* col_idx = (int*)malloc(total_edges * sizeof(int));
    int* dist = (int*)malloc(V * sizeof(int));
    int* frontier = (int*)malloc(V * sizeof(int));
    int* candidates = (int*)malloc(V * sizeof(int));
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
            for (int e = row_ptr[u]; e < row_ptr[u + 1]; e++) {
                int v = col_idx[e];
                if (dist[v] == -1)
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
            if (dist[v] == -1) {
                dist[v] = level;
                frontier[new_fsize++] = v;
            }
        }
        fsize = new_fsize;
    }

    if (rank == 0) zsim_roi_end();
    /* 1.11.98 (user ruling 2026-10-03): the closing barrier every other MPI
     * kernel has, so the device-scope ROI ends at the last rank's arrival
     * (1.11.97 last-rank rule) instead of at guest exit. */
    MPI_Barrier(MPI_COMM_WORLD);

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
