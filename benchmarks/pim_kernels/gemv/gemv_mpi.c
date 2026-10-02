/* gemv_mpi.c -- General Matrix-Vector Multiply: y = A * x
 * MPI domain decomposition. Rows of A distributed across ranks.
 * Each rank needs the full x vector (broadcast from rank 0).
 *
 * Device-organization aware (PIMID), mirroring gemv_omp.c: the simulator prices
 * each PE access by LOCATION. D1 identity mapping -- rank r owns PE r's slot
 * (enforced ranks == num_pes). Prep (before the ROI, untimed, SPMD): each rank
 * relocates ITS rows of A + a private copy of the replicated x vector into ITS
 * own PE slot, so the ROI's reads are LOCAL. Slot layout: [A rows][x]. y stays
 * host-side (as in the OMP sibling). Coarse placement runs the host path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include "zsim_hooks.h"
#include "../../include/pimid_devorg.h"

#define DEFAULT_SIZE 256

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): need of the largest rank share
 * [A rows][x], rows = ceil(N/nprocs). */
static size_t gemv_mpi_need(long n, const void* c) {
    long np = *(const int*)c;
    return ((size_t)((n + np - 1) / np) * (size_t)n + (size_t)n) * sizeof(float);
}

int main(int argc, char* argv[]) {
    MPI_Init(&argc, &argv);

    int rank, nprocs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    int N = parse_int_arg(argc, argv, "--size", DEFAULT_SIZE);

    /* Device organization + D1 identity-mapping guard: ranks must == num_pes. */
    pimid_devorg_t dev = pimid_devorg_from_args(argc, argv);
    if (nprocs > dev.num_pes) {
        if (rank == 0)
            fprintf(stderr, "pimid_devorg: MPI world size (%d) != num_pes (%d); "
                    "identity mapping requires ranks <= PEs (rank r -> slot r). Aborting.\n",
                    nprocs, dev.num_pes);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    /* Domain decomposition: distribute rows across ranks */
    int base = N / nprocs;
    int rem  = N % nprocs;
    int local_rows = base + (rank < rem ? 1 : 0);
    int local_start = rank * base + (rank < rem ? rank : rem);

    /* Each rank allocates its local rows of A, full x, and local y */
    float* A_local = (float*)malloc((size_t)local_rows * N * sizeof(float));
    float* x = (float*)malloc(N * sizeof(float));
    float* y_local = (float*)calloc(local_rows, sizeof(float));
    if (!A_local || !x || !y_local) {
        fprintf(stderr, "rank %d: malloc failed\n", rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    /* Initialize: same pattern as serial (rank builds its rows) */
    for (int i = 0; i < local_rows; i++) {
        int gi = local_start + i;
        for (int j = 0; j < N; j++)
            A_local[i * N + j] = (float)((gi + j) % 7);
    }

    /* x initialized on all ranks (same values) */
    for (int i = 0; i < N; i++)
        x[i] = 1.0f;

    /* Broadcast x to all ranks (in case future versions init only on rank 0) */
    MPI_Bcast(x, N, MPI_FLOAT, 0, MPI_COMM_WORLD);

    /* ---- DATA PREP (before ROI, not timed): this rank's A rows + a private
     * copy of x -> its PE slot. Slot layout: [A rows][x].
     * Assumes thread-MPI (the default/only supported mode): slot r maps to PE
     * r's units directly. Under the retired PIMID_MPI_PROCESS=1 legacy mode the
     * simulator's per-rank unit rotation (pe_memory_interface.h) would
     * double-shift these prepped slots. */
    int prep = pimid_devorg_needs_prep(&dev);
    size_t need = ((size_t)local_rows * N + (size_t)N) * sizeof(float);
    size_t slot_bytes = 0;
    float* Adev = NULL;
    float* Ape = NULL, *xpe = NULL;
    if (prep) {
        /* 1.11.94 (b01-pim-kernels-1/-2, rulings H44/H45): a need larger than the slot
         * is refused; it used to fall back to host layout in silence. */
        slot_bytes = pimid_devorg_slot_bytes(&dev);
        if (need > slot_bytes) {
            char who[32];
            snprintf(who, sizeof who, "rank %d: ", rank);
            pimid_devorg_report_slot_refusal(who, "gemv", &dev, need, slot_bytes, "--size",
                pimid_devorg_largest_fit(gemv_mpi_need, &nprocs, N, slot_bytes));
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        Adev = (float*)pimid_devorg_alloc(&dev, &slot_bytes);
        if (!Adev) {
            fprintf(stderr, "rank %d: devorg: alloc of the device buffer failed; "
                    "running host-layout (no prep)\n", rank);
            prep = 0;
        }
    }
    if (prep) {
        Ape = (float*)pimid_devorg_pe_slot(Adev, rank, slot_bytes);
        xpe = Ape + (size_t)local_rows * N;   /* x copy after A rows */
        memcpy(Ape, A_local, (size_t)local_rows * N * sizeof(float));
        memcpy(xpe, x, (size_t)N * sizeof(float));
    }

    if (prep)
        fprintf(stderr, "[devorg] PREP ACTIVE: kernel=gemv level=%s pes=%d "
                "rank=%d slot_bytes=%zu\n",
                pimid_devorg_level_name(dev.level), dev.num_pes, rank, slot_bytes);

    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) zsim_roi_begin();

    /* Kernel: each rank computes its rows of y (on slot pointers if prepped) */
    if (prep) {
        for (int i = 0; i < local_rows; i++) {
            float sum = 0.0f;
            const float* row = Ape + (size_t)i * N;
            for (int j = 0; j < N; j++)
                sum += row[j] * xpe[j];
            y_local[i] = sum;
        }
    } else {
        for (int i = 0; i < local_rows; i++) {
            float sum = 0.0f;
            for (int j = 0; j < N; j++)
                sum += A_local[i * N + j] * x[j];
            y_local[i] = sum;
        }
    }

    if (rank == 0) zsim_roi_end();
    MPI_Barrier(MPI_COMM_WORLD);

    /* Checksum: each rank computes partial checksum, reduce to rank 0 */
    double local_sum = 0.0;
    for (int i = 0; i < local_rows; i++) local_sum += y_local[i];

    double global_sum = 0.0;
    MPI_Reduce(&local_sum, &global_sum, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

    if (rank == 0) {
        printf("BENCH_CHECKSUM: %f\n", global_sum);
        printf("BENCH_DONE\n");
    }

    free(A_local); free(x); free(y_local);
    MPI_Finalize();
    return 0;
}
