/*
 * Whetstone -- Simplified standalone single-threaded FP benchmark
 * 8 modules exercising different floating-point patterns.
 *
 * Compile: g++ -O2 -I<path-to-zsim/misc/hooks> whetstone.c -o whetstone -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
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
/*  Whetstone globals                                                 */
/* ------------------------------------------------------------------ */
static double T  = 0.499975;
static double T1 = 0.50025;
static double T2 = 2.0;

/* Module 6: procedure calls */
static void PA(double E[], double* T_var, double* T2_var) {
    int J;
    *T_var = (E[0] + E[1] + E[2] - E[3]) * *T_var;
    *T2_var = (E[0] + E[1] - E[2] + E[3]) * *T2_var;
    for (J = 0; J < 6; J++) {
        E[0] = (E[0] + E[1] + E[2] - E[3]) * T;
        E[1] = (E[0] + E[1] - E[2] + E[3]) * T;
    }
}

static void P0(double E[], double T_val, double T2_val) {
    /* Wrapper: several calls to PA */
    PA(E, &T_val, &T2_val);
    PA(E, &T_val, &T2_val);
    PA(E, &T_val, &T2_val);
}

static void P3(double X, double Y, double* Z) {
    *Z = (X + Y) * (X - Y);
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */
int main(int argc, char** argv) {
    int loops = parse_int_arg(argc, argv, "--loops", 10000);

    printf("Whetstone (simplified) -- loops=%d\n", loops);

    /* Working variables */
    double X1, X2, X3, X4, X, Y, Z;
    double E1[4];
    int I, J, K, L, N1, N2, N3, N4, N5, N6, N7, N8;
    int J1, K1, L1;

    /* Scale iteration counts by loops */
    N1 = 0;
    N2 = 12 * loops;
    N3 = 14 * loops;
    N4 = 345 * loops;
    N5 = 0;
    N6 = 210 * loops;
    N7 = 32 * loops;
    N8 = 899 * loops;

    /* Initialise */
    X1 = 1.0;
    X2 = -1.0;
    X3 = -1.0;
    X4 = -1.0;

    /* ---- ROI begin ---- */
    zsim_roi_begin();

    /* Module 1: simple FP assignments */
    for (I = 0; I < N1; I++) {
        X1 = (X1 + X2 + X3 - X4) * T;
        X2 = (X1 + X2 - X3 + X4) * T;
        X3 = (X1 - X2 + X3 + X4) * T;
        X4 = (-X1 + X2 + X3 + X4) * T;
    }

    /* Module 2: array elements */
    E1[0] = 1.0;
    E1[1] = -1.0;
    E1[2] = -1.0;
    E1[3] = -1.0;
    for (I = 0; I < N2; I++) {
        E1[0] = (E1[0] + E1[1] + E1[2] - E1[3]) * T;
        E1[1] = (E1[0] + E1[1] - E1[2] + E1[3]) * T;
        E1[2] = (E1[0] - E1[1] + E1[2] + E1[3]) * T;
        E1[3] = (-E1[0] + E1[1] + E1[2] + E1[3]) * T;
    }

    /* Module 3: conditional jumps */
    J1 = 1;
    K1 = 2;
    L1 = 3;
    for (I = 0; I < N3; I++) {
        if (J1 == 1) L1 = 3; else L1 = 2;
        if (K1 == 2) J1 = 1; else J1 = 3;
        if (L1 == 3) K1 = 2; else K1 = 1;
    }

    /* Module 4: integer-to-FP conversion */
    J = 1;
    K = 2;
    L = 3;
    for (I = 0; I < N4; I++) {
        J = J * (K - J) * (L - K);
        K = L * K - (L - J) * K;
        L = (L - K) * (K + J);
        E1[L - 2] = (double)(J + K + L);
        E1[K - 2] = (double)(J * K * L);
    }

    /* Module 5: trig functions */
    X = 0.5;
    Y = 0.5;
    for (I = 0; I < N5; I++) {
        X = T * atan(T2 * sin(X) * cos(X) / (cos(X + Y) + cos(X - Y) - 1.0));
        Y = T * atan(T2 * sin(Y) * cos(Y) / (cos(X + Y) + cos(X - Y) - 1.0));
    }

    /* Module 6: procedure calls */
    E1[0] = 1.0;
    E1[1] = -1.0;
    E1[2] = -1.0;
    E1[3] = -1.0;
    for (I = 0; I < N6; I++) {
        P0(E1, T, T2);
    }

    /* Module 7: array references */
    J = 1;
    K = 2;
    L = 3;
    E1[0] = 1.0;
    E1[1] = 2.0;
    E1[2] = 3.0;
    for (I = 0; I < N7; I++) {
        P3(E1[J - 1], E1[K - 1], &E1[L - 1]);
    }

    /* Module 8: standard functions (sqrt, exp, log) */
    X = 0.75;
    for (I = 0; I < N8; I++) {
        X = sqrt(exp(log(X) / T1));
    }

    zsim_roi_end();
    /* ---- ROI end ---- */

    double checksum = X1 + X2 + X3 + X4 +
                      E1[0] + E1[1] + E1[2] + E1[3] +
                      X + Y + (double)J + (double)K + (double)L;

    printf("X1=%.6f X2=%.6f X3=%.6f X4=%.6f\n", X1, X2, X3, X4);
    printf("E1 = {%.6f, %.6f, %.6f, %.6f}\n", E1[0], E1[1], E1[2], E1[3]);
    printf("X=%.6f Y=%.6f J=%d K=%d L=%d\n", X, Y, J, K, L);
    printf("BENCH_CHECKSUM: %.6f\n", checksum);
    printf("BENCH_DONE\n");

    return 0;
}
