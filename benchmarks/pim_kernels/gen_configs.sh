#!/usr/bin/env bash
#
# gen_configs.sh -- Generate all PIM kernel YAML config files.
#
# 9 benchmarks x 3 variants (serial, omp, mpi) x 4 tiers (tiny, small, medium, large) = 108 configs.
#

set -euo pipefail

BASE="benchmarks/pim_kernels"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

COUNT=0

# ---------------------------------------------------------------------------
# Helper: emit a single YAML file
#   emit_yaml <dir> <name> <variant> <tier> <args_string> <max_instrs>
#
#   variant: serial | omp | mpi
#   args_string: YAML list contents, e.g. '"--size", "1024"'
# ---------------------------------------------------------------------------
emit_yaml() {
    local dir="$1" name="$2" variant="$3" tier="$4" args_str="$5" max_instrs="$6"

    # Build filename
    local fname
    case "${variant}_${tier}" in
        serial_small)  fname="${name}.yaml" ;;
        serial_tiny)   fname="${name}_tiny.yaml" ;;
        serial_medium) fname="${name}_medium.yaml" ;;
        serial_large)  fname="${name}_large.yaml" ;;
        omp_small)     fname="${name}_omp.yaml" ;;
        omp_tiny)      fname="${name}_omp_tiny.yaml" ;;
        omp_medium)    fname="${name}_omp_medium.yaml" ;;
        omp_large)     fname="${name}_omp_large.yaml" ;;
        mpi_small)     fname="${name}_mpi.yaml" ;;
        mpi_tiny)      fname="${name}_mpi_tiny.yaml" ;;
        mpi_medium)    fname="${name}_mpi_medium.yaml" ;;
        mpi_large)     fname="${name}_mpi_large.yaml" ;;
    esac

    local binary_suffix=""
    [ "$variant" = "omp" ] && binary_suffix="_omp"
    [ "$variant" = "mpi" ] && binary_suffix="_mpi"

    local outfile="${dir}/${fname}"

    {
        cat <<EOF
scope: device
workload:
  binary: ${BASE}/${name}/${name}${binary_suffix}
  args: [${args_str}]
EOF

        if [ "$variant" = "omp" ]; then
            cat <<EOF
  env:
    OMP_NUM_THREADS: "8"
EOF
        elif [ "$variant" = "mpi" ]; then
            cat <<EOF
  type: mpi
  mpi_ranks: 4
EOF
        fi

        cat <<EOF
pim:
  pe:
    type: alu_core
    count: 8
    frequency_mhz: 1000
    placement:
      level: BANK
  mc:
    type: simple
memory:
  technology: SRAM
noc:
  topology: MESH_2D
  model: simple
simulation:
  max_instructions: ${max_instrs}
EOF
    } > "$outfile"

    COUNT=$((COUNT + 1))
}

# ---------------------------------------------------------------------------
# Per-benchmark generator: calls emit_yaml for all 12 combos (3 variants x 4 tiers)
#   gen_all <dir> <name> <tier> <args_string>  (called 4 times per benchmark)
# ---------------------------------------------------------------------------
gen_all_tier() {
    local dir="$1" name="$2" tier="$3" args_str="$4" max_instrs="$5"
    for variant in serial omp mpi; do
        emit_yaml "$dir" "$name" "$variant" "$tier" "$args_str" "$max_instrs"
    done
}

# max_instructions per tier
MAX_TINY=1000000
MAX_SMALL=10000000
MAX_MEDIUM=100000000
MAX_LARGE=1000000000

# ===== stream_triad =====
D="${SCRIPT_DIR}/stream_triad"
N="stream_triad"
gen_all_tier "$D" "$N" tiny   '"--size", "1024"'       $MAX_TINY
gen_all_tier "$D" "$N" small  '"--size", "8192"'       $MAX_SMALL
gen_all_tier "$D" "$N" medium '"--size", "65536"'      $MAX_MEDIUM
gen_all_tier "$D" "$N" large  '"--size", "524288"'     $MAX_LARGE

# ===== vector_add =====
D="${SCRIPT_DIR}/vector_add"
N="vector_add"
gen_all_tier "$D" "$N" tiny   '"--size", "1024"'       $MAX_TINY
gen_all_tier "$D" "$N" small  '"--size", "8192"'       $MAX_SMALL
gen_all_tier "$D" "$N" medium '"--size", "65536"'      $MAX_MEDIUM
gen_all_tier "$D" "$N" large  '"--size", "524288"'     $MAX_LARGE

# ===== reduction =====
D="${SCRIPT_DIR}/reduction"
N="reduction"
gen_all_tier "$D" "$N" tiny   '"--size", "1024"'       $MAX_TINY
gen_all_tier "$D" "$N" small  '"--size", "16384"'      $MAX_SMALL
gen_all_tier "$D" "$N" medium '"--size", "131072"'     $MAX_MEDIUM
gen_all_tier "$D" "$N" large  '"--size", "1048576"'    $MAX_LARGE

# ===== gemv =====
D="${SCRIPT_DIR}/gemv"
N="gemv"
gen_all_tier "$D" "$N" tiny   '"--size", "64"'         $MAX_TINY
gen_all_tier "$D" "$N" small  '"--size", "256"'        $MAX_SMALL
gen_all_tier "$D" "$N" medium '"--size", "512"'        $MAX_MEDIUM
gen_all_tier "$D" "$N" large  '"--size", "1024"'       $MAX_LARGE

# ===== spmv_csr =====
D="${SCRIPT_DIR}/spmv_csr"
N="spmv_csr"
gen_all_tier "$D" "$N" tiny   '"--size", "256", "--density", "1"'     $MAX_TINY
gen_all_tier "$D" "$N" small  '"--size", "1024", "--density", "1"'    $MAX_SMALL
gen_all_tier "$D" "$N" medium '"--size", "4096", "--density", "1"'    $MAX_MEDIUM
gen_all_tier "$D" "$N" large  '"--size", "16384", "--density", "1"'   $MAX_LARGE

# ===== bfs =====
D="${SCRIPT_DIR}/bfs"
N="bfs"
gen_all_tier "$D" "$N" tiny   '"--vertices", "128", "--degree", "4"'    $MAX_TINY
gen_all_tier "$D" "$N" small  '"--vertices", "512", "--degree", "8"'    $MAX_SMALL
gen_all_tier "$D" "$N" medium '"--vertices", "2048", "--degree", "8"'   $MAX_MEDIUM
gen_all_tier "$D" "$N" large  '"--vertices", "8192", "--degree", "16"'  $MAX_LARGE

# ===== histogram =====
D="${SCRIPT_DIR}/histogram"
N="histogram"
gen_all_tier "$D" "$N" tiny   '"--size", "4096", "--bins", "64"'       $MAX_TINY
gen_all_tier "$D" "$N" small  '"--size", "16384", "--bins", "256"'     $MAX_SMALL
gen_all_tier "$D" "$N" medium '"--size", "65536", "--bins", "1024"'    $MAX_MEDIUM
gen_all_tier "$D" "$N" large  '"--size", "262144", "--bins", "4096"'   $MAX_LARGE

# ===== stencil_2d =====
D="${SCRIPT_DIR}/stencil_2d"
N="stencil_2d"
gen_all_tier "$D" "$N" tiny   '"--size", "32", "--iters", "3"'         $MAX_TINY
gen_all_tier "$D" "$N" small  '"--size", "64", "--iters", "5"'         $MAX_SMALL
gen_all_tier "$D" "$N" medium '"--size", "128", "--iters", "10"'       $MAX_MEDIUM
gen_all_tier "$D" "$N" large  '"--size", "256", "--iters", "20"'       $MAX_LARGE

# ===== pointer_chase ===== (working set = lines x 64 B; steps = dependent loads per thread / rank)
D="${SCRIPT_DIR}/pointer_chase"
N="pointer_chase"
gen_all_tier "$D" "$N" tiny   '"--lines", "4096", "--steps", "2000"'       $MAX_TINY
gen_all_tier "$D" "$N" small  '"--lines", "65536", "--steps", "10000"'     $MAX_SMALL
gen_all_tier "$D" "$N" medium '"--lines", "262144", "--steps", "20000"'    $MAX_MEDIUM
gen_all_tier "$D" "$N" large  '"--lines", "1048576", "--steps", "50000"'   $MAX_LARGE

echo "Generated ${COUNT} YAML config files."
