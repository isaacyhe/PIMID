/* backprop.c -- Neural network backpropagation (1 hidden layer)
 * Forward pass (sigmoid activation) then backward pass with weight update.
 * OpenMP parallel on input-to-hidden matmul and weight updates.
 * Layer sizes follow Rodinia backprop: --input input units, 16 hidden units,
 * 1 output unit (bpnn_create(layer_size, 16, 1)). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>
#include "zsim_hooks.h"

#define DEFAULT_INPUT 16384
#define LEARNING_RATE 0.3
/* 1.11.94 (b02-suites-a-4, ruling H46): Rodinia's backprop has a fixed 16
 * hidden units. This port used n_input/16, so w_ih grew as n_input^2/16:
 * 32 GiB at the large tier (--input 262144, malloc failed) and an int index
 * i*n_hidden+j that overflowed past INT_MAX. */
#define N_HIDDEN      16

static uint32_t bench_rand(uint32_t* s) {
    *s = *s * 1103515245 + 12345;
    return (*s >> 16) & 0x7FFF;
}

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

static double sigmoid(double x) {
    return 1.0 / (1.0 + exp(-x));
}

int main(int argc, char* argv[]) {
    int n_input  = parse_int_arg(argc, argv, "--input", DEFAULT_INPUT);
    int n_hidden = N_HIDDEN;
    int n_output = 1;

    if (n_input < 1) {
        fprintf(stderr, "backprop: --input must be >= 1 (got %d)\n", n_input);
        return 1;
    }

    /* Allocate arrays */
    /* index arithmetic below is size_t (1.11.94, ruling H46) */
    double* input       = (double*)malloc((size_t)n_input * sizeof(double));
    double* hidden      = (double*)malloc(n_hidden * sizeof(double));
    double* output      = (double*)malloc(n_output * sizeof(double));
    double* w_ih        = (double*)malloc((size_t)n_input * n_hidden * sizeof(double));  /* input-to-hidden weights */
    double* w_ho        = (double*)malloc((size_t)n_hidden * n_output * sizeof(double)); /* hidden-to-output weights */
    double* hidden_bias = (double*)malloc(n_hidden * sizeof(double));
    double* output_bias = (double*)malloc(n_output * sizeof(double));
    double* hidden_delta = (double*)malloc(n_hidden * sizeof(double));
    double* output_delta = (double*)malloc(n_output * sizeof(double));

    if (!input || !hidden || !output || !w_ih || !w_ho ||
        !hidden_bias || !output_bias || !hidden_delta || !output_delta) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    /* Initialize weights from LCG (range -0.5 to 0.5) */
    uint32_t seed = 42;
    for (size_t i = 0; i < (size_t)n_input * n_hidden; i++)
        w_ih[i] = (bench_rand(&seed) / 32767.0) - 0.5;
    for (size_t i = 0; i < (size_t)n_hidden * n_output; i++)
        w_ho[i] = (bench_rand(&seed) / 32767.0) - 0.5;
    for (int i = 0; i < n_hidden; i++)
        hidden_bias[i] = (bench_rand(&seed) / 32767.0) - 0.5;
    for (int i = 0; i < n_output; i++)
        output_bias[i] = (bench_rand(&seed) / 32767.0) - 0.5;

    /* Initialize input from LCG */
    for (int i = 0; i < n_input; i++)
        input[i] = bench_rand(&seed) / 32767.0;

    /* Target output (synthetic) */
    double target = 0.5;

    zsim_roi_begin();

    /* === Forward pass: input -> hidden (sigmoid) === */
    #pragma omp parallel for
    for (int j = 0; j < n_hidden; j++) {
        double sum = hidden_bias[j];
        for (int i = 0; i < n_input; i++) {
            sum += input[i] * w_ih[(size_t)i * n_hidden + j];
        }
        hidden[j] = sigmoid(sum);
    }

    /* Forward pass: hidden -> output (sigmoid) */
    for (int k = 0; k < n_output; k++) {
        double sum = output_bias[k];
        for (int j = 0; j < n_hidden; j++) {
            sum += hidden[j] * w_ho[(size_t)j * n_output + k];
        }
        output[k] = sigmoid(sum);
    }

    /* === Backward pass: compute output error === */
    for (int k = 0; k < n_output; k++) {
        double o = output[k];
        output_delta[k] = o * (1.0 - o) * (target - o);
    }

    /* Backprop to hidden layer */
    #pragma omp parallel for
    for (int j = 0; j < n_hidden; j++) {
        double sum = 0.0;
        for (int k = 0; k < n_output; k++) {
            sum += output_delta[k] * w_ho[(size_t)j * n_output + k];
        }
        hidden_delta[j] = hidden[j] * (1.0 - hidden[j]) * sum;
    }

    /* Update hidden-to-output weights */
    for (int j = 0; j < n_hidden; j++) {
        for (int k = 0; k < n_output; k++) {
            w_ho[(size_t)j * n_output + k] += LEARNING_RATE * output_delta[k] * hidden[j];
        }
    }
    for (int k = 0; k < n_output; k++) {
        output_bias[k] += LEARNING_RATE * output_delta[k];
    }

    /* Update input-to-hidden weights */
    #pragma omp parallel for
    for (int i = 0; i < n_input; i++) {
        for (int j = 0; j < n_hidden; j++) {
            w_ih[(size_t)i * n_hidden + j] += LEARNING_RATE * hidden_delta[j] * input[i];
        }
    }
    #pragma omp parallel for
    for (int j = 0; j < n_hidden; j++) {
        hidden_bias[j] += LEARNING_RATE * hidden_delta[j];
    }

    zsim_roi_end();

    /* Checksum: sum of all hidden weights after training */
    double checksum = 0.0;
    for (size_t i = 0; i < (size_t)n_input * n_hidden; i++)
        checksum += w_ih[i];
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(input);
    free(hidden);
    free(output);
    free(w_ih);
    free(w_ho);
    free(hidden_bias);
    free(output_bias);
    free(hidden_delta);
    free(output_delta);
    return 0;
}
