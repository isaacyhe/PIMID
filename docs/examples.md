# Example configs

`examples/` holds runnable YAML configs covering every PIMID feature
dimension; they are also shipped in the Docker image as a smoke set.

## Coverage

| Group | Files | Values |
|---|---|---|
| Memory tech (11) | `tech_*.yaml` | DDR3, DDR4, DDR5, LPDDR5, GDDR6, HBM2, HBM3, SRAM, STT_MRAM, PCM, ReRAM |
| Core / PE type (4, plus the scalar in-order PE) | `core_*.yaml` | ooo_core (out-of-order), in_order_core, in_order_scalar (in_order_core with issue_width 1 and branch_predictor none; replaced simple_core in 1.12.1), alu_core, null_core |
| NoC topology (6) | `topo_*.yaml` | MESH_2D, CROSSBAR, RING, TORUS_2D, FAT_TREE, BUS |
| Network model (2) | `netmodel_*.yaml` | detailed (cycle-accurate Garnet; the default), analytical (hop-count + M/D/1 + MLP closed form, `noc.mlp` knob) |
| Hierarchical DRAM placement (5) | `placement_*.yaml` | SUBARRAY, BANK, BANK_GROUP, CHIP, RANK |
| HBM logic-die placement (2) | `logicdie_*.yaml` | HBM2, HBM3 |

Multi-process MPI, host/device cosim, trace-gen, and power on/off are
selected via CLI flags, not by extra YAMLs.

Subdirectories: `cosim/` (host-device and multi-device reference configs)
and `integration/` (external-model adapter example -- see
[external_models.md](external_models.md)). Per-benchmark run configs live
with each suite under `benchmarks/` (see [benchmarks.md](benchmarks.md)).

### Co-sim configs (`cosim/`)

| File | Covers |
|---|---|
| `host_device_basic.yaml` | Minimal host + PIM device, `attachment: internal` (the device is host main memory). |
| `multi_device.yaml` | Host + PCIe-attached PIM device; the second device is kept commented as the multi-device reference layout. This build prices ONE memory per role (1.11.45 E27), so a second DEVICE node is refused at config load -- see the file header. |
| `baseline_host_1core.yaml` | Host-only NO_OFFLOAD baseline: 1 OoO core @2GHz, no fabric (1-core crossbar is degenerate). |
| `baseline_host_4core.yaml` | Host-only NO_OFFLOAD baseline: 4 OoO cores, analytic crossbar fabric (`hop_cycles: 4`). |
| `baseline_host_16core.yaml` | Host-only NO_OFFLOAD baseline: 16 OoO cores, analytic crossbar fabric. |

The three `baseline_host_*core.yaml` are the conventional-multicore reference
for the co-sim figure. Each is a system-scope config run under
`PIMID_COSIM_NO_OFFLOAD=1`: the ROI/WORK markers become stat-only (no device
migration, no boundary charge), so the unmodified OMP/MPI kernel runs on the
host cores end to end against the host memory tech. The device block is
required by the parser but is inert under the knob. The sweep runner overrides
`memory.technology` per cell (host tech = device tech of the paired co-sim
cell -- no host-tech confound); `DDR5` in the files is a placeholder default.

```bash
PIMID_COSIM_NO_OFFLOAD=1 ./build/pimid --method exec --scope system \
    --config examples/cosim/baseline_host_4core.yaml \
    --workload benchmarks/pim_kernels/gemv/gemv_omp \
    --workload-type openmp --size 512
```

See [cosim.md](cosim.md) and [benchmarks.md](benchmarks.md) for the full
experiment shape.

## Run one

```bash
./build/pimid --method exec --config examples/tech_DDR4.yaml
```

## NVSim characterization is slow

The three NVSim-backed techs (STT_MRAM, PCM, ReRAM) run NVSim's first-pass
characterization on each launch -- that takes ~5+ minutes per run. Either
allow long timeouts when smoke-testing them, or run once and reuse the
cached output (see [cache_warehouse.md](cache_warehouse.md)).
