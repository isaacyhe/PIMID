# Workloads that run standalone on this side (config selects core type/placement).
# Three data models per kernel:
#   serial         -> <k>/<k>_serial          (g++, single thread)
#   shared_memory  -> <k>/<k>_shared_memory   (g++ -fopenmp; OpenMP threads, shared addr space)
#   message_passing-> <k>/<k>_message_passing (mpicxx; forked ranks via libpimid_mpi.so)
# .c sources are compiled as C++ for zsim_hooks.h compatibility (matches the
# original pim_kernels recipe). MPI variants need mpicxx (for <mpi.h>); if it is
# absent they are skipped with a note rather than failing the build.
HERE     := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
HOOKS    := $(HERE)../../external/zsim/misc/hooks
CXX      := g++
# Auto-discover an MPI C++ wrapper: PATH, then the system MPICH/OpenMPI dirs.
MPICXX   := $(shell command -v mpicxx 2>/dev/null || \
             ([ -x /usr/lib64/mpich/bin/mpicxx ] && echo /usr/lib64/mpich/bin/mpicxx) || \
             ([ -x /usr/mpi/gcc/openmpi-4.1.9a1/bin/mpicxx ] && echo /usr/mpi/gcc/openmpi-4.1.9a1/bin/mpicxx) || \
             echo mpicxx)
CXXFLAGS := -std=c++11 -O2 -Wall -I$(HOOKS)
OMPFLAGS := -fopenmp
LDFLAGS  := -lpthread

KERNELS  := stream_triad gemv spmv_csr bfs stencil_2d

SERIAL := $(foreach k,$(KERNELS),$(k)/$(k)_serial)
SM     := $(foreach k,$(KERNELS),$(k)/$(k)_shared_memory)
MP     := $(foreach k,$(KERNELS),$(k)/$(k)_message_passing)

HAS_MPI := $(shell [ -x "$(MPICXX)" ] || command -v $(MPICXX) >/dev/null 2>&1 && echo yes || echo no)

.PHONY: all serial shared_memory message_passing clean
all: serial shared_memory
ifeq ($(HAS_MPI),yes)
all: message_passing
else
	@echo "NOTE: mpicxx not found -- skipping message_passing (forked-MPI) variants"
endif

serial: $(SERIAL)
shared_memory: $(SM)
message_passing: $(MP)

%_serial: %_serial.c
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)
%_shared_memory: %_shared_memory.c
	$(CXX) $(CXXFLAGS) $(OMPFLAGS) -o $@ $< $(LDFLAGS)
%_message_passing: %_message_passing.c
	$(MPICXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(SERIAL) $(SM) $(MP)
