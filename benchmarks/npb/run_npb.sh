#!/bin/bash
# NPB Benchmark Runner for PIMID
# Builds and runs self-contained NPB benchmarks through pimid.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PIMID_BIN="${PIMID_BIN:-pimid}"
PASS=0; FAIL=0; SKIP=0

# Build benchmarks if needed
echo "=== Building NPB benchmarks ==="
make -C "$SCRIPT_DIR" -j"$(nproc)" 2>&1 | tail -3 || true
echo ""

echo "=== NPB Benchmarks ==="

for yaml in "$SCRIPT_DIR"/configs/*.yaml; do
    name=$(basename "$yaml" .yaml)
    # Check if binary exists
    binary=$(grep "binary:" "$yaml" | head -1 | awk '{print $2}')
    if [ ! -x "$binary" ]; then
        echo "SKIP $name (binary not found: $binary)"
        SKIP=$((SKIP + 1))
        continue
    fi
    echo -n "RUN  $name... "
    outdir=$(mktemp -d /tmp/pimid_npb_XXXXXX)
    # 1.11.94 (b03-suites-b-1, ruling H42): pimid has no --output option; it
    # rejected the flag ('Unknown option: --output', rc=1), so every run failed.
    # pimid's own exit status decides PASS; its output goes to $outdir.
    if $PIMID_BIN --method exec --config "$yaml" --no-power \
        >"$outdir/stdout.log" 2>"$outdir/stderr.log"; then
        echo "PASS"
        PASS=$((PASS + 1))
    else
        echo "FAIL"
        FAIL=$((FAIL + 1))
    fi
    rm -rf "$outdir"
done

echo "NPB: $PASS pass, $FAIL fail, $SKIP skip"
