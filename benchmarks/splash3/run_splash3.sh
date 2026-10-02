#!/bin/bash
# SPLASH-3 Benchmark Runner for PIMID
# Runs self-contained SPLASH-3 benchmarks through pimid.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PIMID_BIN="${PIMID_BIN:-pimid}"
TIER="${TIER:-tiny}"          # tiny, small (default yamls), medium, large
METHOD="${METHOD:-exec}"
PASS=0; FAIL=0; SKIP=0

# ---------- color helpers ----------
red()   { printf '\033[1;31m%s\033[0m' "$*"; }
green() { printf '\033[1;32m%s\033[0m' "$*"; }
yellow(){ printf '\033[1;33m%s\033[0m' "$*"; }

BENCH_LIST="fft radix lu cholesky barnes ocean water_nsquared water_spatial fmm radiosity raytrace volrend"

# ---------- Step 1: Build benchmarks if needed ----------
echo "=== Building SPLASH-3 benchmarks ==="
make -C "$SCRIPT_DIR" -j"$(nproc)" 2>&1 | tail -3 || true
echo ""

# ---------- Step 2: Run benchmarks ----------
echo "=== SPLASH-3 Benchmarks (tier=$TIER, method=$METHOD) ==="
echo ""

# Determine yaml suffix
yaml_suffix=""
case "$TIER" in
    tiny)   yaml_suffix="_tiny" ;;
    small)  yaml_suffix="" ;;       # small = default (no suffix)
    medium) yaml_suffix="_medium" ;;
    large)  yaml_suffix="_large" ;;
    all)    yaml_suffix="ALL" ;;    # run all tiers
    *)      echo "ERROR: unknown tier '$TIER' (use: tiny, small, medium, large, all)"; exit 1 ;;
esac

run_one() {
    local yaml="$1"
    local name
    name=$(basename "$yaml" .yaml)

    # Extract binary path from YAML (relative to project root)
    local binary
    binary=$(grep '^\s*binary:' "$yaml" | head -1 | sed 's/.*binary:\s*//')

    # Check if binary exists (try both relative and absolute)
    local resolved_bin=""
    if [ -x "$binary" ]; then
        resolved_bin="$binary"
    elif [ -x "$SCRIPT_DIR/../../$binary" ]; then
        resolved_bin="$SCRIPT_DIR/../../$binary"
    elif [ -x "$SCRIPT_DIR/../../../$binary" ]; then
        resolved_bin="$SCRIPT_DIR/../../../$binary"
    fi

    if [ -z "$resolved_bin" ]; then
        printf "  %-35s %s\n" "$name" "$(yellow SKIP) (binary not found: $binary)"
        SKIP=$((SKIP + 1))
        return
    fi

    printf "  %-35s " "$name"
    local outdir
    outdir=$(mktemp -d /tmp/pimid_splash3_XXXXXX)

    local rc=0
    # 1.11.94 (b03-suites-b-1, ruling H42): pimid has no --output option; it
    # rejected the flag ('Unknown option: --output', rc=1), so every run failed.
    # pimid's own exit status decides PASS; its output goes to $outdir.
    if $PIMID_BIN --method "$METHOD" --config "$yaml" --no-power \
        >"$outdir/stdout.log" 2>"$outdir/stderr.log"; then
        echo "$(green PASS)"
        PASS=$((PASS + 1))
    else
        rc=$?
        echo "$(red FAIL) (exit=$rc)"
        FAIL=$((FAIL + 1))
        if [ -f "$outdir/stderr.log" ]; then
            tail -3 "$outdir/stderr.log" | sed 's/^/    /'
        fi
    fi
    rm -rf "$outdir"
}

run_tier() {
    local suffix="$1"
    local configs_dir="$SCRIPT_DIR/configs"

    for bench in $BENCH_LIST; do
        local yaml
        if [ -z "$suffix" ]; then
            yaml="$configs_dir/${bench}.yaml"
        else
            yaml="$configs_dir/${bench}${suffix}.yaml"
        fi
        if [ ! -f "$yaml" ]; then
            printf "  %-35s %s\n" "${bench}${suffix}" "$(yellow SKIP) (config not found)"
            SKIP=$((SKIP + 1))
            continue
        fi
        run_one "$yaml"
    done
    return 0
}

if [ "$yaml_suffix" = "ALL" ]; then
    for tier_s in _tiny "" _medium _large; do
        tier_label="small"
        case "$tier_s" in
            _tiny)   tier_label="tiny" ;;
            "")      tier_label="small" ;;
            _medium) tier_label="medium" ;;
            _large)  tier_label="large" ;;
        esac
        echo "--- Tier: $tier_label ---"
        run_tier "$tier_s"
        echo ""
    done
else
    run_tier "$yaml_suffix"
fi

# ---------- Summary ----------
echo ""
echo "============================================"
TOTAL=$((PASS + FAIL + SKIP))
echo "SPLASH-3: $TOTAL total, $(green "$PASS pass"), $(red "$FAIL fail"), $(yellow "$SKIP skip")"
echo "============================================"

# Exit with failure if any test failed
[ "$FAIL" -eq 0 ]
