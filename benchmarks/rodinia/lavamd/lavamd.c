/* lavamd.c -- Molecular dynamics: particle interactions in a 3D grid of boxes
 * Each box contains ~100 particles. Forces computed between each box and its
 * 26 neighbors + self using simplified Coulomb interaction.
 * OpenMP parallel on box iteration. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>
#include "zsim_hooks.h"

#define DEFAULT_BOXES      4
#define PARTICLES_PER_BOX  100
#define SOFTENING          0.01  /* avoid division by zero */

static uint32_t bench_rand(uint32_t* s) {
    *s = *s * 1103515245 + 12345;
    return (*s >> 16) & 0x7FFF;
}

static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

typedef struct {
    double x, y, z;
} Vec3;

typedef struct {
    double charge;
    Vec3 pos;
} Particle;

int main(int argc, char* argv[]) {
    int boxes_per_dim = parse_int_arg(argc, argv, "--boxes", DEFAULT_BOXES);
    int total_boxes = boxes_per_dim * boxes_per_dim * boxes_per_dim;
    int total_particles = total_boxes * PARTICLES_PER_BOX;

    /* Allocate particle data and force accumulators */
    Particle* particles = (Particle*)malloc(total_particles * sizeof(Particle));
    Vec3* forces = (Vec3*)calloc(total_particles, sizeof(Vec3));
    if (!particles || !forces) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    /* Initialize particle positions and charges from LCG */
    uint32_t seed = 42;
    for (int b = 0; b < total_boxes; b++) {
        /* Box center based on grid position */
        int bz = b / (boxes_per_dim * boxes_per_dim);
        int by = (b / boxes_per_dim) % boxes_per_dim;
        int bx = b % boxes_per_dim;
        double cx = (double)bx + 0.5;
        double cy = (double)by + 0.5;
        double cz = (double)bz + 0.5;

        for (int p = 0; p < PARTICLES_PER_BOX; p++) {
            int idx = b * PARTICLES_PER_BOX + p;
            /* Position within box: center +/- 0.5 */
            particles[idx].pos.x = cx + (bench_rand(&seed) / 32767.0) - 0.5;
            particles[idx].pos.y = cy + (bench_rand(&seed) / 32767.0) - 0.5;
            particles[idx].pos.z = cz + (bench_rand(&seed) / 32767.0) - 0.5;
            /* Charge: -1.0 to 1.0 */
            particles[idx].charge = 2.0 * (bench_rand(&seed) / 32767.0) - 1.0;
        }
    }

    /* 1.12.18: the thread team starts before the ROI, as message-passing
     * ranks exist before it; the ROI measures the kernel, not the start-up. */
    #pragma omp parallel
    { }
    zsim_roi_begin();

    /* For each box, interact with 26 neighbors + self */
    #pragma omp parallel for schedule(dynamic)
    for (int b = 0; b < total_boxes; b++) {
        int bz = b / (boxes_per_dim * boxes_per_dim);
        int by = (b / boxes_per_dim) % boxes_per_dim;
        int bx = b % boxes_per_dim;

        /* Iterate over neighbor offsets */
        for (int dz = -1; dz <= 1; dz++) {
            int nz = bz + dz;
            if (nz < 0 || nz >= boxes_per_dim) continue;
            for (int dy = -1; dy <= 1; dy++) {
                int ny = by + dy;
                if (ny < 0 || ny >= boxes_per_dim) continue;
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = bx + dx;
                    if (nx < 0 || nx >= boxes_per_dim) continue;

                    int nb = nz * boxes_per_dim * boxes_per_dim + ny * boxes_per_dim + nx;

                    /* Compute pairwise interactions between particles in box b and box nb */
                    for (int p = 0; p < PARTICLES_PER_BOX; p++) {
                        int pi = b * PARTICLES_PER_BOX + p;
                        double fx = 0.0, fy = 0.0, fz = 0.0;

                        for (int q = 0; q < PARTICLES_PER_BOX; q++) {
                            int qi = nb * PARTICLES_PER_BOX + q;
                            if (pi == qi) continue;

                            double rx = particles[pi].pos.x - particles[qi].pos.x;
                            double ry = particles[pi].pos.y - particles[qi].pos.y;
                            double rz = particles[pi].pos.z - particles[qi].pos.z;
                            double r2 = rx * rx + ry * ry + rz * rz + SOFTENING;
                            double inv_r = 1.0 / sqrt(r2);
                            double inv_r2 = inv_r * inv_r;

                            /* Coulomb force: F = q1*q2 / r^2, direction = r_hat */
                            double f = particles[pi].charge * particles[qi].charge * inv_r2;
                            fx += f * rx * inv_r;
                            fy += f * ry * inv_r;
                            fz += f * rz * inv_r;
                        }

                        forces[pi].x += fx;
                        forces[pi].y += fy;
                        forces[pi].z += fz;
                    }
                }
            }
        }
    }

    zsim_roi_end();

    /* Checksum: sum of all force magnitudes */
    double checksum = 0.0;
    for (int i = 0; i < total_particles; i++) {
        checksum += sqrt(forces[i].x * forces[i].x +
                         forces[i].y * forces[i].y +
                         forces[i].z * forces[i].z);
    }
    printf("BENCH_CHECKSUM: %f\n", checksum);
    printf("BENCH_DONE\n");

    free(particles);
    free(forces);
    return 0;
}
