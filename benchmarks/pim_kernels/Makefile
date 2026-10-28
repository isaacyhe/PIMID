# PIM Kernels Makefile — builds 8 benchmarks × 3 variants (serial, OpenMP, MPI)
# All .c files compiled as C++ (g++) for zsim_hooks.h compatibility.

BENCHDIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
ZSIM_HOOKS := $(BENCHDIR)/../../external/zsim/misc/hooks

CXX      := g++
MPICXX   := mpicxx
CXXFLAGS := -std=c++11 -O2 -Wall -I$(ZSIM_HOOKS)
OMPFLAGS := -fopenmp
# 1.12.18 (ruling 2026-10-24, bind at load): every symbol is bound before main
# (no lazy-binding resolution inside the ROI); the simulator also sets
# LD_BIND_NOW=1 for the guest.
LDFLAGS  := -Wl,-z,now -lpthread

KERNELS := stream_triad vector_add gemv spmv_csr bfs histogram reduction stencil_2d pointer_chase

# Targets per kernel
SERIAL_BINS := $(foreach k,$(KERNELS),$(BENCHDIR)/$(k)/$(k))
OMP_BINS    := $(foreach k,$(KERNELS),$(BENCHDIR)/$(k)/$(k)_omp)
MPI_BINS    := $(foreach k,$(KERNELS),$(BENCHDIR)/$(k)/$(k)_mpi)

# Check if mpicxx is available. `which $(MPICXX) && echo yes || echo no`
# captured by $(shell) yields "/usr/bin/mpicxx\nyes" (since $(shell) collects
# combined stdout), so $(HAS_MPI) was never literally "yes" and MPI builds
# silently never ran. Use command -v with stdout-suppressed so only the
# yes/no signal is captured.
HAS_MPI := $(shell command -v $(MPICXX) >/dev/null 2>&1 && echo yes || echo no)

.PHONY: all clean serial omp mpi

all: serial omp
ifeq ($(HAS_MPI),yes)
all: mpi
else
	@echo "NOTE: mpicxx not found, skipping MPI variants"
endif

serial: $(SERIAL_BINS)
omp: $(OMP_BINS)
mpi: $(MPI_BINS)

# Serial: <kernel>/<kernel>.c -> <kernel>/<kernel>
define SERIAL_RULE
$(BENCHDIR)/$(1)/$(1): $(BENCHDIR)/$(1)/$(1).c
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $$@ $$<
endef
$(foreach k,$(KERNELS),$(eval $(call SERIAL_RULE,$(k))))

# OpenMP: <kernel>/<kernel>_omp.c -> <kernel>/<kernel>_omp
define OMP_RULE
$(BENCHDIR)/$(1)/$(1)_omp: $(BENCHDIR)/$(1)/$(1)_omp.c
	$(CXX) $(CXXFLAGS) $(OMPFLAGS) $(LDFLAGS) -o $$@ $$<
endef
$(foreach k,$(KERNELS),$(eval $(call OMP_RULE,$(k))))

# MPI: <kernel>/<kernel>_mpi.c -> <kernel>/<kernel>_mpi
define MPI_RULE
$(BENCHDIR)/$(1)/$(1)_mpi: $(BENCHDIR)/$(1)/$(1)_mpi.c
	$(MPICXX) $(CXXFLAGS) $(LDFLAGS) -o $$@ $$<
endef
$(foreach k,$(KERNELS),$(eval $(call MPI_RULE,$(k))))

clean:
	rm -f $(SERIAL_BINS) $(OMP_BINS) $(MPI_BINS)
