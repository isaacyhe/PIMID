#!/usr/bin/env bash
# DRAM technology sweep (the "dram_sweep" figure): 7 DRAM standards x
# 5 kernels x {shared-memory (OpenMP), message-passing (MPI)} on 16
# in-memory PEs at BANK placement, cycle-accurate network model, with power.
#
# Run from the repository root after building:   bash examples/figures/dram_sweep.sh
# Output: results_dram_sweep/dram_sweep.csv  (one row per cell)
set -uo pipefail
PIMID="${PIMID:-./build/pimid}"
OUT="${OUT:-results_dram_sweep}"
mkdir -p "$OUT/cfg" "$OUT/logs"

TECHS="${TECHS:-DDR3 DDR4 DDR5 LPDDR5 GDDR6 HBM2 HBM3}"
MODELS="${MODELS:-omp mpi}"
KERNELS="${KERNELS:-stream_triad gemv spmv_csr bfs stencil_2d}"
declare -A ARGS=(
  [stream_triad]='"--size", "65536"'
  [gemv]='"--size", "512"'
  [spmv_csr]='"--size", "4096", "--density", "1"'
  [bfs]='"--vertices", "2048", "--degree", "8"'
  [stencil_2d]='"--size", "128", "--iters", "10"'
)

CSV="$OUT/dram_sweep.csv"
echo "tech,exec_model,kernel,cycles,total_power_W,total_energy_nJ,rc" > "$CSV"

for t in $TECHS; do
 for m in $MODELS; do
  for k in $KERNELS; do
    cfg="$OUT/cfg/${t}_${m}_${k}.yaml"
    cat > "$cfg" <<YAML
workload:
  binary: benchmarks/pim_kernels/${k}/${k}_${m}
  args: [${ARGS[$k]}]
system:
  devices:
    - name: device
      pim:
        pe: { type: alu_core, count: 16, frequency_mhz: 500 }
        placement: { level: BANK }
        mc: { type: simple }
      memory:
        technology: ${t}
        banks: 16
      noc:
        model: detailed
YAML
    log="$OUT/logs/${t}_${m}_${k}.log"
    if [ "$m" = "mpi" ]; then
      "$PIMID" --method exec --config "$cfg" --workload-type mpi --mpi-ranks 16 \
        --power --power-report standard > "$log" 2>&1
    else
      "$PIMID" --method exec --config "$cfg" --workload-type openmp \
        --power --power-report standard > "$log" 2>&1
    fi
    rc=$?
    # OMP reports "cycles: N"; MPI reports the critical path as "(max: N)"
    if [ "$m" = "mpi" ]; then
      cyc=$(grep -oE '\(max: [0-9]+\)' "$log" | head -1 | grep -oE '[0-9]+')
    else
      cyc=$(grep -oE 'cycles: [0-9]+ # Simulated cycles' "$log" | head -1 | grep -oE '[0-9]+')
    fi
    pw=$(grep -oE 'Total Power: [0-9.]+' "$log" | head -1 | grep -oE '[0-9.]+' | head -1)
    en="NA"
    [ -n "${cyc:-}" ] && [ -n "${pw:-}" ] && \
      en=$(awk -v p="$pw" -v c="$cyc" 'BEGIN{ printf "%.3f", p * (c/500e6) * 1e9 }')
    echo "${t},${m},${k},${cyc:-NA},${pw:-NA},${en},${rc}" >> "$CSV"
    echo "  ${t} ${m} ${k}: cycles=${cyc:-FAIL} rc=${rc}"
  done
 done
done
echo "DONE -> $CSV"
