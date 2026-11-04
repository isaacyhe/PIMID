# PIMID Benchmark Suite -- Top-level Makefile
# Builds all 7 benchmark suites (41 benchmarks total, all self-contained).

BENCHDIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

.PHONY: all clean pim_kernels babelstream rodinia classic npb splash3 parsec

all: pim_kernels babelstream rodinia classic npb splash3 parsec

pim_kernels:
	$(MAKE) -C $(BENCHDIR)/pim_kernels

babelstream:
	$(MAKE) -C $(BENCHDIR)/babelstream

rodinia:
	$(MAKE) -C $(BENCHDIR)/rodinia

classic:
	$(MAKE) -C $(BENCHDIR)/classic

npb:
	$(MAKE) -C $(BENCHDIR)/npb

splash3:
	$(MAKE) -C $(BENCHDIR)/splash3

parsec:
	$(MAKE) -C $(BENCHDIR)/parsec

clean:
	$(MAKE) -C $(BENCHDIR)/pim_kernels clean
	$(MAKE) -C $(BENCHDIR)/babelstream clean
	$(MAKE) -C $(BENCHDIR)/rodinia clean
	$(MAKE) -C $(BENCHDIR)/classic clean
	$(MAKE) -C $(BENCHDIR)/npb clean
	$(MAKE) -C $(BENCHDIR)/splash3 clean
	$(MAKE) -C $(BENCHDIR)/parsec clean
