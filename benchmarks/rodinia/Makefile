# Rodinia Benchmarks Makefile -- 10 OpenMP benchmarks
BENCHDIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
ZSIM_HOOKS := $(BENCHDIR)/../../external/zsim/misc/hooks

CXX      := g++
CXXFLAGS := -std=c++11 -O2 -Wall -I$(ZSIM_HOOKS)
OMPFLAGS := -fopenmp
LDFLAGS  := -lpthread -lm

BENCHMARKS := hotspot needle pathfinder srad kmeans lud backprop lavamd particlefilter myocyte
BINS := $(foreach b,$(BENCHMARKS),$(BENCHDIR)/$(b)/$(b))

.PHONY: all clean

all: $(BINS)

# Each benchmark: <name>/<name>.c -> <name>/<name>
define BENCH_RULE
$(BENCHDIR)/$(1)/$(1): $(BENCHDIR)/$(1)/$(1).c
	$(CXX) $(CXXFLAGS) $(OMPFLAGS) $(LDFLAGS) -o $$@ $$<
endef
$(foreach b,$(BENCHMARKS),$(eval $(call BENCH_RULE,$(b))))

clean:
	rm -f $(BINS)
