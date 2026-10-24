/* myocyte.c -- Cardiac myocyte ODE simulation (Hodgkin-Huxley-like model)
 * Simulates N cells, each with 4 state variables (V, m, h, n), over T steps.
 * Forward Euler integration of ionic current ODEs.
 * OpenMP parallel on cells. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>
#include "zsim_hooks.h"

#define DEFAULT_CELLS 1
#define DEFAULT_STEPS 1000
#define DEFAULT_DT    0.005

/* Hodgkin-Huxley parameters (squid giant axon model, classic values) */
#define C_M    1.0      /* membrane capacitance (uF/cm^2) */
#define G_NA   120.0    /* sodium conductance (mS/cm^2) */
#define G_K    36.0     /* potassium conductance (mS/cm^2) */
#define G_L    0.3      /* leak conductance (mS/cm^2) */
#define E_NA   50.0     /* sodium reversal potential (mV) */
#define E_K   -77.0     /* potassium reversal potential (mV) */
#define E_L   -54.387   /* leak reversal potential (mV) */
#define I_STIM 10.0     /* stimulus current (uA/cm^2) */

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

static double parse_double_arg(int argc, char** argv, const char* flag, double def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atof(argv[i + 1]);
    return def;
}

/* Alpha and beta rate functions for gating variables */
static double alpha_m(double V) {
    if (fabs(V + 40.0) < 1e-7)
        return 1.0;  /* limit as V -> -40 */
    return 0.1 * (V + 40.0) / (1.0 - exp(-(V + 40.0) / 10.0));
}

static double beta_m(double V) {
    return 4.0 * exp(-(V + 65.0) / 18.0);
}

static double alpha_h(double V) {
    return 0.07 * exp(-(V + 65.0) / 20.0);
}

static double beta_h(double V) {
    return 1.0 / (1.0 + exp(-(V + 35.0) / 10.0));
}

static double alpha_n(double V) {
    if (fabs(V + 55.0) < 1e-7)
        return 0.1;  /* limit as V -> -55 */
    return 0.01 * (V + 55.0) / (1.0 - exp(-(V + 55.0) / 10.0));
}

static double beta_n(double V) {
    return 0.125 * exp(-(V + 65.0) / 80.0);
}

int main(int argc, char* argv[]) {
    int N_cells = parse_int_arg(argc, argv, "--cells", DEFAULT_CELLS);
    int T_steps = parse_int_arg(argc, argv, "--steps", DEFAULT_STEPS);
    double dt   = parse_double_arg(argc, argv, "--dt", DEFAULT_DT);

    /* State per cell: V, m, h, n */
    double* V = (double*)malloc(N_cells * sizeof(double));
    double* m = (double*)malloc(N_cells * sizeof(double));
    double* h = (double*)malloc(N_cells * sizeof(double));
    double* n = (double*)malloc(N_cells * sizeof(double));

    if (!V || !m || !h || !n) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    /* Initialize all cells to resting state */
    for (int c = 0; c < N_cells; c++) {
        V[c] = -65.0;   /* resting potential (mV) */
        m[c] = 0.05;    /* Na activation gate */
        h[c] = 0.6;     /* Na inactivation gate */
        n[c] = 0.32;    /* K activation gate */
    }

    /* 1.12.18: the thread team starts before the ROI, as message-passing
     * ranks exist before it; the ROI measures the kernel, not the start-up. */
    #pragma omp parallel
    { }
    zsim_roi_begin();

    for (int t = 0; t < T_steps; t++) {
        /* Apply stimulus only during first 10% of simulation */
        double stim = (t < T_steps / 10) ? I_STIM : 0.0;

        #pragma omp parallel for
        for (int c = 0; c < N_cells; c++) {
            double Vc = V[c];
            double mc = m[c];
            double hc = h[c];
            double nc = n[c];

            /* Ionic currents */
            double I_Na = G_NA * mc * mc * mc * hc * (Vc - E_NA);
            double I_K  = G_K  * nc * nc * nc * nc * (Vc - E_K);
            double I_L  = G_L  * (Vc - E_L);

            /* Membrane potential derivative */
            double dVdt = (-I_Na - I_K - I_L + stim) / C_M;

            /* Gating variable derivatives */
            double am = alpha_m(Vc); double bm = beta_m(Vc);
            double ah = alpha_h(Vc); double bh = beta_h(Vc);
            double an = alpha_n(Vc); double bn = beta_n(Vc);

            double dmdt = am * (1.0 - mc) - bm * mc;
            double dhdt = ah * (1.0 - hc) - bh * hc;
            double dndt = an * (1.0 - nc) - bn * nc;

            /* Forward Euler integration */
            V[c] = Vc + dt * dVdt;
            m[c] = mc + dt * dmdt;
            h[c] = hc + dt * dhdt;
            n[c] = nc + dt * dndt;

            /* Clamp gating variables to [0, 1] */
            if (m[c] < 0.0) m[c] = 0.0; if (m[c] > 1.0) m[c] = 1.0;
            if (h[c] < 0.0) h[c] = 0.0; if (h[c] > 1.0) h[c] = 1.0;
            if (n[c] < 0.0) n[c] = 0.0; if (n[c] > 1.0) n[c] = 1.0;
        }
    }

    zsim_roi_end();

    /* Checksum: sum of final voltages across all cells */
    double checksum = 0.0;
    for (int c = 0; c < N_cells; c++) {
        checksum += V[c];
    }
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(V);
    free(m);
    free(h);
    free(n);
    return 0;
}
