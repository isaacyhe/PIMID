# PIMID Documentation

| Document | Covers |
|---|---|
| [architecture.md](architecture.md) | Simulation engine: QEMU + ZSim plugin, methods (exec / trace / trace-gen / synthetic), scopes, two-Garnet hierarchy |
| [memory.md](memory.md) | 11 memory technologies, backend models (Ramulator2 / CACTI / NVSim), PE placement levels, characterization cache |
| [network.md](network.md) | The two NoC models (analytical closed form + detailed Garnet), 8 topologies, DRAM-as-Garnet-tree, synthetic traffic |
| [cores.md](cores.md) | The four PE core models (and the scalar single-issue in-order PE), MLP intensity, ALU scaling factors |
| [cosim.md](cosim.md) | Host-device co-simulation: system model, the six boundary decisions (launch / coherence / host NoC / bridge / memory topology / host pricing), NO_OFFLOAD baseline, task-region window, MPI semantics |
| [cosim_calibration.md](cosim_calibration.md) | Co-sim calibration: three-tier evidence, measured anchors + sources, source->sim provenance, the total-measured/split-inferred host-path note |
| [benchmarks.md](benchmarks.md) | 48-benchmark suite, data-model variants, NPB details, building and running |
| [examples.md](examples.md) | The example YAML config set and what each group covers |
| [power.md](power.md) | McPAT power/area integration |
| [build.md](build.md) | Building from source, custom QEMU, no-root/HPC notes, Docker, reproducibility |
| [yaml_reference.md](yaml_reference.md) | Complete configuration key reference |
| [dram_specs.md](dram_specs.md) | Per-technology JEDEC organization mapping |
| [memory_spec_structs.md](memory_spec_structs.md) | C++ architecture-spec structs (internal port bitwidths, scaling studies) |
| [cache_warehouse.md](cache_warehouse.md) | Characterization cache warehouse internals |
| [external.md](external.md) | Vendored external simulators (QEMU, ZSim, Ramulator2, CACTI, NVSim, McPAT, Garnet) |
| [external_models.md](external_models.md) | Integrating your own network/memory model via the adapter interface |
| [changelog.md](changelog.md) | Release + defect ledger, newest first. 
