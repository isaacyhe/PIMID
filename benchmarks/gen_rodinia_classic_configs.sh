#!/usr/bin/env bash
# gen_rodinia_classic_configs.sh
# Generates all YAML config files for Rodinia (10 benchmarks x 4 tiers = 40)
# and Classic (6 benchmarks x 4 tiers = 24), total = 64 configs.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RODINIA_DIR="${SCRIPT_DIR}/rodinia"
CLASSIC_DIR="${SCRIPT_DIR}/classic"

# max_instructions per tier
declare -A MAX_INSTRS
MAX_INSTRS[tiny]=1000000
MAX_INSTRS[small]=10000000
MAX_INSTRS[medium]=100000000
MAX_INSTRS[large]=1000000000

COUNT=0

# -- Helper: write a Rodinia YAML ----------------------------------------------
write_rodinia() {
    local name="$1"
    local tier="$2"
    shift 2
    local args=("$@")

    local dir="${RODINIA_DIR}/${name}"
    local file
    if [[ "$tier" == "small" ]]; then
        file="${dir}/${name}.yaml"
    else
        file="${dir}/${name}_${tier}.yaml"
    fi

    local max_instrs="${MAX_INSTRS[$tier]}"

    # Build args list as YAML array items
    local args_yaml=""
    for a in "${args[@]}"; do
        if [[ -n "$args_yaml" ]]; then
            args_yaml="${args_yaml}, \"${a}\""
        else
            args_yaml="\"${a}\""
        fi
    done

    # 1.11.94 (b02-suites-a-1, ruling H47): noc.model is analytical. The
    # loader accepts only analytical | detailed and refused 'simple' (rc=1),
    # so none of the 40 Rodinia configs could load.
    cat > "$file" <<EOF
scope: device
workload:
  binary: benchmarks/rodinia/${name}/${name}
  args: [${args_yaml}]
  env:
    OMP_NUM_THREADS: "8"
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
  model: analytical
simulation:
  max_instructions: ${max_instrs}
EOF

    COUNT=$((COUNT + 1))
    echo "  [${COUNT}] ${file#${SCRIPT_DIR}/}"
}

# -- Helper: write a Classic YAML ----------------------------------------------
write_classic() {
    local name="$1"
    local tier="$2"
    shift 2
    local args=("$@")

    local dir="${CLASSIC_DIR}/${name}"
    local file
    if [[ "$tier" == "small" ]]; then
        file="${dir}/${name}.yaml"
    else
        file="${dir}/${name}_${tier}.yaml"
    fi

    local max_instrs="${MAX_INSTRS[$tier]}"

    # Build args list as YAML array items
    local args_yaml=""
    for a in "${args[@]}"; do
        if [[ -n "$args_yaml" ]]; then
            args_yaml="${args_yaml}, \"${a}\""
        else
            args_yaml="\"${a}\""
        fi
    done

    cat > "$file" <<EOF
scope: device
workload:
  binary: benchmarks/classic/${name}/${name}
  args: [${args_yaml}]
pim:
  pe:
    type: alu_core
    count: 1
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

    COUNT=$((COUNT + 1))
    echo "  [${COUNT}] ${file#${SCRIPT_DIR}/}"
}

# ==============================================================================
# Rodinia benchmarks (10 x 4 = 40)
# ==============================================================================
echo "=== Generating Rodinia configs ==="

# hotspot: --rows --cols --iters
write_rodinia hotspot tiny   "--rows" "64"   "--cols" "64"   "--iters" "1"
write_rodinia hotspot small  "--rows" "256"  "--cols" "256"  "--iters" "10"
write_rodinia hotspot medium "--rows" "512"  "--cols" "512"  "--iters" "100"
write_rodinia hotspot large  "--rows" "1024" "--cols" "1024" "--iters" "1000"

# needle: --size
write_rodinia needle tiny   "--size" "64"
write_rodinia needle small  "--size" "256"
write_rodinia needle medium "--size" "1024"
write_rodinia needle large  "--size" "4096"

# pathfinder: --rows --cols
write_rodinia pathfinder tiny   "--rows" "10"  "--cols" "100"
write_rodinia pathfinder small  "--rows" "100" "--cols" "1000"
write_rodinia pathfinder medium "--rows" "100" "--cols" "10000"
write_rodinia pathfinder large  "--rows" "100" "--cols" "100000"

# srad: --rows --cols --iters
write_rodinia srad tiny   "--rows" "64"   "--cols" "64"   "--iters" "1"
write_rodinia srad small  "--rows" "256"  "--cols" "256"  "--iters" "10"
write_rodinia srad medium "--rows" "512"  "--cols" "512"  "--iters" "50"
write_rodinia srad large  "--rows" "1024" "--cols" "1024" "--iters" "100"

# kmeans: --points --clusters --dims --iters
write_rodinia kmeans tiny   "--points" "256"   "--clusters" "4"  "--dims" "16" "--iters" "10"
write_rodinia kmeans small  "--points" "1024"  "--clusters" "8"  "--dims" "16" "--iters" "20"
write_rodinia kmeans medium "--points" "4096"  "--clusters" "16" "--dims" "32" "--iters" "20"
write_rodinia kmeans large  "--points" "16384" "--clusters" "32" "--dims" "64" "--iters" "20"

# lud: --size
write_rodinia lud tiny   "--size" "64"
write_rodinia lud small  "--size" "256"
write_rodinia lud medium "--size" "512"
write_rodinia lud large  "--size" "1024"

# backprop: --input
write_rodinia backprop tiny   "--input" "1024"
write_rodinia backprop small  "--input" "16384"
write_rodinia backprop medium "--input" "65536"
write_rodinia backprop large  "--input" "262144"

# lavamd: --boxes
write_rodinia lavamd tiny   "--boxes" "2"
write_rodinia lavamd small  "--boxes" "4"
write_rodinia lavamd medium "--boxes" "6"
write_rodinia lavamd large  "--boxes" "10"

# particlefilter: --particles --frames
write_rodinia particlefilter tiny   "--particles" "100"   "--frames" "5"
write_rodinia particlefilter small  "--particles" "1000"  "--frames" "10"
write_rodinia particlefilter medium "--particles" "5000"  "--frames" "50"
write_rodinia particlefilter large  "--particles" "10000" "--frames" "100"

# myocyte: --cells --steps
write_rodinia myocyte tiny   "--cells" "1"  "--steps" "100"
write_rodinia myocyte small  "--cells" "1"  "--steps" "1000"
write_rodinia myocyte medium "--cells" "4"  "--steps" "1000"
write_rodinia myocyte large  "--cells" "16" "--steps" "5000"

# ==============================================================================
# Classic benchmarks (6 x 4 = 24)
# ==============================================================================
echo ""
echo "=== Generating Classic configs ==="

# dhrystone: --iters
write_classic dhrystone tiny   "--iters" "100"
write_classic dhrystone small  "--iters" "10000"
write_classic dhrystone medium "--iters" "1000000"
write_classic dhrystone large  "--iters" "10000000"

# whetstone: --loops
write_classic whetstone tiny   "--loops" "100"
write_classic whetstone small  "--loops" "10000"
write_classic whetstone medium "--loops" "1000000"
write_classic whetstone large  "--loops" "10000000"

# binary_search: --size --lookups
write_classic binary_search tiny   "--size" "1024"     "--lookups" "100"
write_classic binary_search small  "--size" "65536"    "--lookups" "10000"
write_classic binary_search medium "--size" "1048576"  "--lookups" "100000"
write_classic binary_search large  "--size" "16777216" "--lookups" "1000000"

# quicksort: --size
write_classic quicksort tiny   "--size" "1024"
write_classic quicksort small  "--size" "65536"
write_classic quicksort medium "--size" "1048576"
write_classic quicksort large  "--size" "16777216"

# sha256: --size
write_classic sha256 tiny   "--size" "1024"
write_classic sha256 small  "--size" "65536"
write_classic sha256 medium "--size" "1048576"
write_classic sha256 large  "--size" "16777216"

# naive_matmul: --size
write_classic naive_matmul tiny   "--size" "32"
write_classic naive_matmul small  "--size" "128"
write_classic naive_matmul medium "--size" "256"
write_classic naive_matmul large  "--size" "512"

# ==============================================================================
echo ""
echo "=== Done: ${COUNT} YAML config files generated ==="
