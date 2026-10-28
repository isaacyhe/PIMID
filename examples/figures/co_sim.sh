#!/usr/bin/env bash
# Host-device co-simulation (the "co-sim" demonstration).
#
# This runs PIMID's REAL coupled co-simulation: ONE simulation per cell, with
# the host and the device both live in the same process (a configuration with
# system.hosts[]; the nodes decide, 1.12.9).
# There are NO co-sim-specific workloads -- an ordinary pim_kernels binary is
# launched; its ROI offload region executes on the DEVICE model (PEs + the
# device's own in-memory network + memory technology, identical to a
# standalone device-scope run), while the out-of-ROI code (input generation,
# result handling) executes on the HOST model (cores + caches + host memory).
# Boundary crossings are charged with the host-device link and memory models.
#
# The device offloaded in co-simulation IS the device: its ROI cycle count
# agrees with a standalone device-scope run of the same kernel. This script
# demonstrates that equivalence per cell (the "parity" column).
#
# Run from the repository root after building:
#     bash examples/figures/co_sim.sh
# Output: results_co_sim/co_sim.csv
#   columns: tech, kernel, cosim_device_cycles, cosim_host_cycles,
#            device_scope_cycles, parity_pct, checksum_match, rc
set -uo pipefail
PIMID="${PIMID:-./build/pimid}"
OUT="${OUT:-results_co_sim}"
mkdir -p "$OUT/cfg" "$OUT/logs"

TECHS="${TECHS:-HBM3 DDR4}"
KERNELS="${KERNELS:-histogram stencil_2d}"
NPES=8
FREQ=2000

# Small sizes keep the demonstration fast; scale up for figure-quality data.
declare -A KARGS=(
  [histogram]="--size 2048 --bins 256"
  [stencil_2d]="--size 64 --iters 5"
  [stream_triad]="--size 16384"
  [gemv]="--size 256"
)
kbin() { echo "benchmarks/pim_kernels/$1/$1_omp"; }

CSV="$OUT/co_sim.csv"
echo "tech,kernel,cosim_device_cycles,cosim_host_cycles,device_scope_cycles,parity_pct,checksum_match,rc" > "$CSV"

for t in $TECHS; do
  for k in $KERNELS; do
    args="${KARGS[$k]}"

    # ---- coupled co-simulation: host + device in one run (system.hosts[] present) ----
    cfg="$OUT/cfg/cosim_${t}_${k}.yaml"
    cat > "$cfg" <<YAML
system:
  hosts:
    - name: host
      core_type: in_order_core
      num_cores: 1
      frequency_mhz: ${FREQ}
      tech_node_nm: 22
      cache: { l1d_kb: 32, l1i_kb: 32, l2_kb: 256 }
      memory: { technology: DDR5 }
  devices:
    - name: device
      attachment: internal       # device IS the host's main memory (no external
                                 # link); for a discrete device add a links:
                                 # entry (pcie_gen5 / cxl_3_0 / interposer ...)
      frequency_mhz: ${FREQ}
      tech_node_nm: 22
      pe_type: alu_core
      num_pes: ${NPES}
      pim:
        placement: { level: BANK }
        mc: { type: simple }
      memory: { technology: ${t}, banks: 16 }
      noc: { model: detailed }   # a DRAM device's network is its tier tree (an in-die
                                 # MESH_2D is refused on the detailed path, 1.11.103)
  network:
    topology: crossbar
    model: detailed
YAML
    clog="$OUT/logs/cosim_${t}_${k}.log"
    "$PIMID" --method exec --config "$cfg" \
      --workload "$(kbin "$k")" $args --workload-type openmp > "$clog" 2>&1
    rc=$?
    cdev=$(awk '/device_pes-0:/{f=1} f&&/cycles:/{print $2; exit}' "$clog")
    chost=$(awk '/host_cores-0:/{f=1} f&&/cycles:/{print $2; exit}' "$clog")
    ccsum=$(grep -oE 'BENCH_CHECKSUM: [-0-9]+' "$clog" | head -1 | grep -oE '[-0-9]+$')

    # ---- standalone device-scope run of the same kernel (parity baseline) ----
    dcfg="$OUT/cfg/dev_${t}_${k}.yaml"
    cat > "$dcfg" <<YAML
workload:
  binary: $(kbin "$k")
  args: [$(printf '"%s", ' $args | sed 's/, $//')]
system:
  devices:
    - name: device
      pim:
        pe: { type: alu_core, count: ${NPES}, frequency_mhz: ${FREQ} }
        placement: { level: BANK }
        mc: { type: simple }
      memory:
        technology: ${t}
        banks: 16
      noc:
        model: detailed
YAML
    dlog="$OUT/logs/dev_${t}_${k}.log"
    "$PIMID" --method exec --config "$dcfg" --workload-type openmp > "$dlog" 2>&1
    ddev=$(grep -oE 'cycles: [0-9]+ # Simulated cycles' "$dlog" | head -1 | grep -oE '[0-9]+')
    dcsum=$(grep -oE 'BENCH_CHECKSUM: [-0-9]+' "$dlog" | head -1 | grep -oE '[-0-9]+$')

    # parity: coupled device cycles vs standalone device cycles
    parity="NA"
    if [ -n "${cdev:-}" ] && [ -n "${ddev:-}" ] && [ "${ddev:-0}" -ne 0 ]; then
      parity=$(awk -v a="$cdev" -v b="$ddev" 'BEGIN{ printf "%.1f", 100.0*(a-b)/b }')
    fi
    cmatch="NA"
    [ -n "${ccsum:-}" ] && [ -n "${dcsum:-}" ] && \
      { [ "$ccsum" = "$dcsum" ] && cmatch="yes" || cmatch="NO"; }

    echo "${t},${k},${cdev:-NA},${chost:-NA},${ddev:-NA},${parity},${cmatch},${rc}" >> "$CSV"
    echo "  ${t} ${k}: cosim_dev=${cdev:-FAIL} host=${chost:-NA} dev_scope=${ddev:-NA} parity=${parity}% checksum=${cmatch}"
  done
done
echo "DONE -> $CSV"
echo "PASS criterion: |parity_pct| within a few percent AND checksum_match=yes for every row."