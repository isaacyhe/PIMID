# Classic Single-Thread Benchmarks Makefile -- 6 benchmarks
BENCHDIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
ZSIM_HOOKS := $(BENCHDIR)/../../external/zsim/misc/hooks

CXX      := g++
CXXFLAGS := -std=c++11 -O2 -Wall -I$(ZSIM_HOOKS)
LDFLAGS  := -lpthread -lm

BENCHMARKS := dhrystone whetstone binary_search quicksort sha256 naive_matmul
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
