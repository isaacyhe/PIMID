# SPLASH-3 Benchmarks -- Self-Contained Implementations
BENCHDIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
ZSIM_HOOKS := $(BENCHDIR)/../../external/zsim/misc/hooks

CXX      := g++
CXXFLAGS := -std=c++11 -O2 -Wall -I$(ZSIM_HOOKS)
LDFLAGS  := -lpthread -lm

BENCHMARKS := fft radix barnes ocean lu water_nsquared water_spatial cholesky fmm radiosity raytrace volrend
BINS := $(foreach b,$(BENCHMARKS),$(BENCHDIR)/$(b)/$(b))

.PHONY: all clean

all: $(BINS)

define BENCH_RULE
$(BENCHDIR)/$(1)/$(1): $(BENCHDIR)/$(1)/$(1).c
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $$@ $$<
endef
$(foreach b,$(BENCHMARKS),$(eval $(call BENCH_RULE,$(b))))

clean:
	rm -f $(BINS)
