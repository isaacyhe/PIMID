#!/bin/bash
# Run BabelStream benchmark through pimid
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PIMID_BIN="${PIMID_BIN:-pimid}"
TIER="${TIER:-tiny}"
METHOD="${METHOD:-exec}"
PASS=0; FAIL=0; SKIP=0

echo "=== BabelStream (tier=$TIER) ==="

# Build if needed
make -C "$SCRIPT_DIR" 2>&1 | tail -1

bin="$SCRIPT_DIR/babelstream"
if [ "$TIER" = "small" ]; then
    cfg="$SCRIPT_DIR/configs/babelstream.yaml"
else
    cfg="$SCRIPT_DIR/configs/babelstream_${TIER}.yaml"
fi

if [ ! -x "$bin" ]; then
    echo "SKIP babelstream (binary not built)"
    exit 0
fi

if [ ! -f "$cfg" ]; then
    echo "SKIP babelstream (config not found: $cfg)"
    exit 0
fi

echo -n "RUN  babelstream_${TIER}... "

# Standalone test
if ! OMP_NUM_THREADS=2 "$bin" --arraysize 1024 --numtimes 2 2>&1 | grep -q "BENCH_DONE"; then
    echo "FAIL (standalone)"
    exit 1
fi

# Run through pimid if available
if command -v "$PIMID_BIN" &>/dev/null; then
    outdir=$(mktemp -d /tmp/pimid_babel_XXXXXX)
    # 1.11.94 (b03-suites-b-1, ruling H42): pimid has no --output option; it
    # rejected the flag ('Unknown option: --output', rc=1), so every run failed.
    # pimid's own exit status decides PASS; its output goes to $outdir.
    if $PIMID_BIN --method "$METHOD" --config "$cfg" --no-power \
        >"$outdir/stdout.log" 2>"$outdir/stderr.log"; then
        echo "PASS"
        PASS=1
    else
        echo "FAIL (pimid)"
        FAIL=1
    fi
    rm -rf "$outdir"
else
    echo "PASS (standalone only)"
    PASS=1
fi

echo ""
echo "BabelStream: $PASS pass, $FAIL fail, $SKIP skip"
[ "$FAIL" -eq 0 ] || exit 1
