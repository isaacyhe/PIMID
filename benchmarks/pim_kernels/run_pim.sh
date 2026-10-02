#!/bin/bash
# Run PIM kernel benchmarks through pimid
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PIMID_BIN="${PIMID_BIN:-pimid}"
TIER="${TIER:-tiny}"
METHOD="${METHOD:-exec}"
PASS=0; FAIL=0; SKIP=0

KERNELS="stream_triad vector_add gemv spmv_csr bfs histogram reduction stencil_2d"
VARIANTS="serial omp"
[ -x "$(command -v mpirun)" ] && VARIANTS="serial omp mpi"

echo "=== PIM Kernel Benchmarks (tier=$TIER) ==="

# Build if needed
make -C "$SCRIPT_DIR" all 2>&1 | tail -1

for kernel in $KERNELS; do
    for variant in $VARIANTS; do
        # Determine binary and config
        if [ "$variant" = "serial" ]; then
            bin="$SCRIPT_DIR/$kernel/$kernel"
            suffix=""
        else
            bin="$SCRIPT_DIR/$kernel/${kernel}_${variant}"
            suffix="_${variant}"
        fi

        # Determine config file
        if [ "$TIER" = "small" ]; then
            cfg="$SCRIPT_DIR/$kernel/${kernel}${suffix}.yaml"
        else
            cfg="$SCRIPT_DIR/$kernel/${kernel}${suffix}_${TIER}.yaml"
        fi

        name="${kernel}${suffix}_${TIER}"

        if [ ! -x "$bin" ]; then
            echo "SKIP $name (binary not built)"
            SKIP=$((SKIP + 1))
            continue
        fi

        if [ ! -f "$cfg" ]; then
            echo "SKIP $name (config not found: $cfg)"
            SKIP=$((SKIP + 1))
            continue
        fi

        echo -n "RUN  $name... "

        # First verify standalone execution
        if ! "$bin" --size 64 2>&1 | grep -q "BENCH_DONE"; then
            echo "FAIL (standalone)"
            FAIL=$((FAIL + 1))
            continue
        fi

        # Run through pimid if available
        if command -v "$PIMID_BIN" &>/dev/null; then
            outdir=$(mktemp -d /tmp/pimid_pim_XXXXXX)
            # 1.11.94 (b03-suites-b-1, ruling H42): pimid has no --output option; it
            # rejected the flag ('Unknown option: --output', rc=1), so every run failed.
            # pimid's own exit status decides PASS; its output goes to $outdir.
            if $PIMID_BIN --method "$METHOD" --config "$cfg" --no-power \
                >"$outdir/stdout.log" 2>"$outdir/stderr.log"; then
                echo "PASS"
                PASS=$((PASS + 1))
            else
                echo "FAIL (pimid)"
                FAIL=$((FAIL + 1))
            fi
            rm -rf "$outdir"
        else
            echo "PASS (standalone only)"
            PASS=$((PASS + 1))
        fi
    done
done

echo ""
echo "PIM Kernels: $PASS pass, $FAIL fail, $SKIP skip"
[ "$FAIL" -eq 0 ] || exit 1
