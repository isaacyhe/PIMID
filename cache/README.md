# cache/ -- generated tool characterizations

This directory ships EMPTY and fills at runtime. It is the simulator's own
store for external-tool results that are expensive to produce and identical
every time they are asked for:

    cache/nvsim/   NVSim array characterizations, one XML per design key
    cache/index.jsonl   append-only manifest (what was stored, when, by which
                        tool version)

WHY IT EXISTS. One NVSim characterization explores the tool's full design
space -- this tree's own logs report 584,492 to 1,160,943 valid design
points, each a complete bank build and circuit solve. Regenerating the same
design in every sweep cell would dominate a fleet run, so a design is
characterized once and reused.

KEY DISCIPLINE. The filename IS the key: every input that changes the
characterization must appear in it (type, capacity, node, access width,
device corner). An input that changes the physics but not the key would
serve one design's numbers for another's question -- the failure that put
`device_corner` into the key in 1.11.49. The cell file's CONTENT is part of
the key too: the `_h<hash>` suffix is a hash of the file's key lines (the
lines NVSim reads) and of the characterization's calibration revision, and
the entry stores it (`<cell_hash>`) and is refused on load if it differs.

To pre-generate entries instead of paying for them inside a simulation job:

    build/nvsim_warm <type 0|1|2> <capacity_bytes> <node_nm> <width_bits> [corner]

Relocate the store with `--cache-dir <path>` or `PIMID_CACHE_DIR`; disable it
with `PIMID_CACHE_MODE=off`. Entries are reproducible outputs of the vendored
tools, so this directory can be deleted at any time -- the cost of doing so
is recomputation, not correctness.
