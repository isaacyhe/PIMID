/* stencil_2d_mpi.c -- 2D 5-point Jacobi stencil (nearest-neighbor average)
 * MPI domain decomposition. Rows of the N*N grid are distributed across
 * ranks. Ghost rows are exchanged via MPI_Sendrecv between neighbors.
 *
 * Device-organization aware (PIMID), mirroring stencil_2d_omp.c: the simulator
 * prices each PE access by LOCATION. D1 identity mapping -- rank r owns PE r's
 * slot (enforced ranks == num_pes). Prep (before the ROI, untimed, SPMD): each
 * rank relocates ITS grid + tmp (both including the +2 ghost rows -- D3: halo
 * rows live INSIDE this rank's slot) into ITS own PE slot; the ROI then reads
 * its own rows locally. Slot layout: [grid alloc_rows][tmp alloc_rows].
 * MPI_Sendrecv halo exchange is unchanged (it writes into the slot's ghost
 * rows). Coarse placement runs the original host-layout path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include "zsim_hooks.h"
#include "../../include/pimid_devorg.h"

#define DEFAULT_SIZE  64
#define DEFAULT_ITERS 5

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): need of the largest rank share
 * [grid alloc_rows][tmp alloc_rows], alloc_rows = ceil(N/nprocs) + 2. */
static size_t stencil_2d_mpi_need(long n, const void* c) {
    long np = *(const int*)c;
    return 2ull * (size_t)((n + np - 1) / np + 2) * (size_t)n * sizeof(float);
}

int main(int argc, char* argv[]) {
    MPI_Init(&argc, &argv);

    int rank, nprocs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    int N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);
    int iters = parse_int_arg(argc, argv, "--iters", DEFAULT_ITERS);

    /* Device organization + D1 identity-mapping guard: ranks must == num_pes. */
    pimid_devorg_t dev = pimid_devorg_from_args(argc, argv);
    if (nprocs > dev.num_pes) {
        if (rank == 0)
            fprintf(stderr, "pimid_devorg: MPI world size (%d) != num_pes (%d); "
                    "identity mapping requires ranks <= PEs (rank r -> slot r). Aborting.\n",
                    nprocs, dev.num_pes);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    /* Domain decomposition: distribute rows across ranks.
     * Each rank owns local_rows rows of the grid but allocates
     * local_rows+2 rows to hold ghost rows (top and bottom). */
    int base = N / nprocs;
    int rem  = N % nprocs;
    int local_start = rank * base + (rank < rem ? rank : rem);
    int local_rows  = base + (rank < rem ? 1 : 0);

    /* Allocate grid with ghost rows: row 0 = top ghost, rows 1..local_rows = data,
     * row local_rows+1 = bottom ghost. grid_host/tmp_host own the allocations
     * (freed at the end); grid/tmp are the compute pointers (repointed into the
     * PE slot when prepped). */
    int alloc_rows = local_rows + 2;
    float* grid_host = (float*)calloc((size_t)alloc_rows * N, sizeof(float));
    float* tmp_host  = (float*)calloc((size_t)alloc_rows * N, sizeof(float));
    if (!grid_host || !tmp_host) { fprintf(stderr, "rank %d: malloc failed\n", rank); MPI_Abort(MPI_COMM_WORLD, 1); }
    float* grid = grid_host;
    float* tmp  = tmp_host;

    /* Initialize: hot boundary on global top row (row 0).
     * If this rank owns global row 0, set it in local row 1. */
    if (local_start == 0) {
        for (int j = 0; j < N; j++)
            grid[1 * N + j] = 100.0f;
    }

    int up_rank   = (rank > 0) ? rank - 1 : MPI_PROC_NULL;
    int down_rank = (rank < nprocs - 1) ? rank + 1 : MPI_PROC_NULL;

    /* ---- DATA PREP (before ROI, not timed): grid + tmp (each alloc_rows*N,
     * including the +2 ghost rows -- D3) -> this rank's PE slot.
     * Slot layout: [grid alloc_rows][tmp alloc_rows].
     * Assumes thread-MPI (the default/only supported mode): slot r maps to PE
     * r's units directly. Under the retired PIMID_MPI_PROCESS=1 legacy mode the
     * simulator's per-rank unit rotation (pe_memory_interface.h) would
     * double-shift these prepped slots. */
    int prep = pimid_devorg_needs_prep(&dev);
    size_t need = 2ull * (size_t)alloc_rows * N * sizeof(float);
    size_t slot_bytes = 0;
    float* dbuf = NULL;
    if (prep) {
        /* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): a need larger than the slot
         * is refused; it used to fall back to host layout in silence. */
        slot_bytes = pimid_devorg_slot_bytes(&dev);
        if (need > slot_bytes) {
            char who[32];
            snprintf(who, sizeof who, "rank %d: ", rank);
            pimid_devorg_report_slot_refusal(who, "stencil_2d", &dev, need, slot_bytes, "--size",
                pimid_devorg_largest_fit(stencil_2d_mpi_need, &nprocs, N, slot_bytes));
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        dbuf = (float*)pimid_devorg_alloc(&dev, &slot_bytes);
        if (!dbuf) {
            fprintf(stderr, "rank %d: devorg: alloc of the device buffer failed; "
                    "running host-layout (no prep)\n", rank);
            prep = 0;
        }
    }
    if (prep) {
        float* gslot = (float*)pimid_devorg_pe_slot(dbuf, rank, slot_bytes);
        float* tslot = gslot + (size_t)alloc_rows * N;
        memcpy(gslot, grid_host, (size_t)alloc_rows * N * sizeof(float));
        memcpy(tslot, tmp_host,  (size_t)alloc_rows * N * sizeof(float));
        grid = gslot;
        tmp  = tslot;
    }

    if (prep)
        fprintf(stderr, "[devorg] PREP ACTIVE: kernel=stencil_2d level=%s pes=%d "
                "rank=%d slot_bytes=%zu\n",
                pimid_devorg_level_name(dev.level), dev.num_pes, rank, slot_bytes);

    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) zsim_roi_begin();

    for (int t = 0; t < iters; t++) {
        /* Exchange ghost rows with neighbors.
         * Send my top data row (row 1) to up_rank's bottom ghost.
         * Receive from up_rank into my top ghost (row 0). */
        MPI_Sendrecv(&grid[1 * N], N, MPI_FLOAT, up_rank, 0,
                     &grid[0 * N], N, MPI_FLOAT, up_rank, 1,
                     MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        /* Send my bottom data row (row local_rows) to down_rank's top ghost.
         * Receive from down_rank into my bottom ghost (row local_rows+1). */
        MPI_Sendrecv(&grid[local_rows * N], N, MPI_FLOAT, down_rank, 1,
                     &grid[(local_rows + 1) * N], N, MPI_FLOAT, down_rank, 0,
                     MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        /* Stencil computation on interior points.
         * Local rows 1..local_rows map to global rows local_start..local_start+local_rows-1.
         * Skip global boundary rows (row 0 and row N-1). */
        for (int li = 1; li <= local_rows; li++) {
            int gi = local_start + (li - 1);  /* global row index */
            if (gi == 0 || gi == N - 1) {
                /* Copy boundary rows unchanged */
                memcpy(&tmp[li * N], &grid[li * N], N * sizeof(float));
                continue;
            }
            /* Column boundaries stay zero */
            tmp[li * N + 0] = grid[li * N + 0];
            tmp[li * N + (N - 1)] = grid[li * N + (N - 1)];
            for (int j = 1; j < N - 1; j++) {
                tmp[li * N + j] = 0.25f * (
                    grid[(li - 1) * N + j] + grid[(li + 1) * N + j] +
                    grid[li * N + (j - 1)] + grid[li * N + (j + 1)]);
            }
        }

        /* Swap grid and tmp */
        float* s = grid; grid = tmp; tmp = s;
    }

    if (rank == 0) zsim_roi_end();
    MPI_Barrier(MPI_COMM_WORLD);

    /* Checksum: each rank sums its local data rows (rows 1..local_rows) */
    double local_sum = 0.0;
    for (int li = 1; li <= local_rows; li++)
        for (int j = 0; j < N; j++)
            local_sum += grid[li * N + j];

    double global_sum = 0.0;
    MPI_Reduce(&local_sum, &global_sum, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

    if (rank == 0) {
        printf("BENCH_CHECKSUM: %f\n", global_sum);
        printf("BENCH_DONE\n");
    }

    free(grid_host); free(tmp_host);
    MPI_Finalize();
    return 0;
}
