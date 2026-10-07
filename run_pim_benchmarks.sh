#!/usr/bin/env bash
# Run every built PIM kernel binary with one shared PIMID YAML configuration.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT_DIR="$REPO_ROOT/benchmarks/pim_kernels"
PIMID_BIN="${PIMID_BIN:-$REPO_ROOT/build/pimid}"

usage() {
    echo "Usage: $0 CONFIG.yaml" >&2
    exit 2
}

[[ $# -eq 1 ]] || usage
CONFIG="$1"
[[ "$CONFIG" = /* ]] || CONFIG="$PWD/$CONFIG"
[[ -f "$CONFIG" ]] || { echo "Configuration not found: $CONFIG" >&2; exit 2; }
[[ -x "$PIMID_BIN" ]] || {
    if command -v "$PIMID_BIN" >/dev/null 2>&1; then
        PIMID_BIN="$(command -v "$PIMID_BIN")"
    else
        echo "PIMID executable not found: $PIMID_BIN (set PIMID_BIN to override)" >&2
        exit 2
    fi
}

CONFIG_NAME="$(basename "$CONFIG")"
CONFIG_STEM="${CONFIG_NAME%.*}"
LOG_DIR="$REPO_ROOT/logs/$CONFIG_STEM"
mkdir -p "$LOG_DIR"
LOG_DIR="$(cd "$LOG_DIR" && pwd)"

TMP_OUTPUT="$(mktemp -d "${TMPDIR:-/tmp}/pimid-batch.XXXXXX")"
trap 'rm -rf "$TMP_OUTPUT"' EXIT

echo "Building PIM kernel benchmarks..."
if ! make -C "$SCRIPT_DIR" all; then
    echo "Failed to build PIM kernel benchmarks" >&2
    exit 1
fi

KERNELS=(stream_triad vector_add gemv spmv_csr bfs histogram reduction stencil_2d)
PASS=0
FAIL=0

for kernel in "${KERNELS[@]}"; do
    for variant in serial omp mpi; do
        if [[ "$variant" == serial ]]; then
            binary="$SCRIPT_DIR/$kernel/$kernel"
        else
            binary="$SCRIPT_DIR/$kernel/${kernel}_${variant}"
        fi
        [[ -x "$binary" ]] || continue

        workload="$(basename "$binary")"
        log="$LOG_DIR/${workload}_${CONFIG_STEM}.log"
        output="$TMP_OUTPUT/$workload"
        echo "RUN  $workload (config: $CONFIG_NAME) -> $log"

        if "$PIMID_BIN" --method "${METHOD:-exec}" --config "$CONFIG" \
            --workload "$binary" --output "$output" --no-power >"$log" 2>&1; then
            echo "PASS $workload"
            PASS=$((PASS + 1))
        else
            status=$?
            echo "FAIL $workload (exit $status; see $log)"
            FAIL=$((FAIL + 1))
        fi
    done
done

if (( PASS + FAIL == 0 )); then
    echo "No built PIM kernel binaries found under $SCRIPT_DIR" >&2
    exit 1
fi

echo "Finished: $PASS passed, $FAIL failed. Logs: $LOG_DIR"
(( FAIL == 0 ))
