# PIMID Cache Warehouse

## What it is and why

Several PIMID backends produce their numbers by running a **slow, deterministic
design-space search**. NVSim characterization is the canonical example: given a
memory type, capacity, process node, and per-access word width, it searches
cell/array organizations to
report read/write latency, energy, leakage, and area. The search can take on the
order of seconds per point and always returns the same answer for the same
inputs.

The **cache warehouse** memoizes those results. The first time a
`(backend, parameters)` point is needed it is computed and persisted; every later
run that asks for the same point reads it back instead of re-running the search.
Because the underlying computation is deterministic, the cache is safe: a hit is
indistinguishable from a fresh run except for being much faster.

The warehouse is *centralized* -- one root directory shared across runs and
backends -- so characterization work done by one experiment is reused by every
other.

## Cache modes

The cache behaves according to a **mode**:

| Mode  | Reads from cache | Writes to cache | Use when                                              |
|-------|:----------------:|:---------------:|-------------------------------------------------------|
| `rw`  | yes              | yes             | Default. Read if present, compute + store on a miss.  |
| `ro`  | yes              | no              | Reproduce results without mutating a shared warehouse.|
| `wo`  | no               | yes             | Force a recompute and **overwrite** the cached value. Use to refresh stale entries. |
| `off` | no               | no              | Bypass the cache entirely; always recompute, never store. |

Notes:

- `wo` ("write-only") still computes everything from scratch -- it just skips the
  read so it always recomputes, and it stores the fresh result, superseding any
  previous one. This is the mode to use when refreshing entries flagged by
  `pimid-cache stale`.
- `off` is equivalent to having no warehouse at all for that run.

## Configuration surfaces and precedence

The cache mode and warehouse location can be set three ways. **Precedence,
highest first: CLI > environment > YAML config > built-in default.**

1. **CLI flags**
   - `--cache <mode>` -- one of `rw`, `ro`, `wo`, `off`.
   - `--cache-dir <path>` -- warehouse root.

2. **Environment variables**
   - `PIMID_CACHE_MODE` -- `rw` | `ro` | `wo` | `off`.
   - `PIMID_CACHE_DIR` -- warehouse root.
   - `PIMID_CACHE_DISABLE` -- if set to a truthy value, forces the cache off
     (equivalent to `--cache off`).

3. **YAML config**
   ```yaml
   cache:
     mode: rw          # rw | ro | wo | off
     dir: ~/.cache/pimid
     enabled: true     # false is equivalent to mode: off
   ```

4. **Defaults**
   - mode: `rw`
   - dir: `<pimid>/cache`, located from the running binary (since 1.11.52;
     `~/.cache/pimid` before -- see the per-user store below)

### The pre-1.11.52 per-user store

`~/.cache/pimid` is read as a READ-ONLY fallback: an NVSim query that misses
the warehouse looks there, and an entry it finds is copied into the warehouse
("migrated cached characterization into the tree cache"). Since 1.12.2 an
entry is migrated only if it passes every check a warehouse entry passes and
is in the current format -- full-precision values (`<value_format>g17`,
1.11.94) and the subarray geometry (`<subarray_rows>` / `<subarray_cols>`,
1.11.102). An older entry is not copied: the run prints

    [NVSimWrapper] legacy cache entry <path> is pre-1.11.102 (no subarray geometry) [and 6-significant-digit (...)]: not migrated; recharacterizing

and the query is recharacterized and stored in the warehouse. Through 1.12.1
the entry was copied first and checked after, so a stale one was planted in
the warehouse and served from there on (a PCM cell then refused: "NVSim
reported no wordlines per mat").

### Back-compat

The previous, NVSim-specific environment variables are still honored:

- `PIMID_NVSIM_CACHE_DIR` -- legacy location of the NVSim cache. The warehouse
  treats its **parent directory** as the warehouse root (the legacy dir was the
  per-backend `nvsim/` subdir). Honored only when the generic
  `PIMID_CACHE_DIR` is unset.
- `PIMID_NVSIM_CACHE_DISABLE` -- legacy disable switch, treated like
  `PIMID_CACHE_DISABLE`.

The generic `PIMID_CACHE_*` variables take precedence over the legacy
`PIMID_NVSIM_*` ones.

## On-disk layout

```
<root>/                       warehouse root (default <pimid>/cache)
|-- index.jsonl               append-only manifest (one JSON object per line)
|-- nvsim/                     per-backend artifact subdir
|   |-- nvm_t1_c8388608_n22.xml
|   `-- ...
`-- <other-backend>/          one subdir per backend
```

### Manifest: `index.jsonl`

The manifest is **append-only**: one JSON object per line, never rewritten in
place by normal runs. Each record describes one cached point:

```json
{
  "backend": "nvsim",
  "key": "t1_c8388608_n22",
  "tool_version": "1.0.0",
  "timestamp": 1780000000,
  "params": {
    "nvm_type": 1,
    "capacity_bytes": 8388608,
    "process_node_nm": 22
  },
  "value": {
    "read_latency_s": 1.2e-9,
    "write_latency_s": 3.4e-9,
    "read_energy_nj": 0.1,
    "write_energy_nj": 0.2,
    "leakage_mw": 5.0,
    "area_mm2": 0.05
  }
}
```

Field meanings:

| Field          | Meaning                                                              |
|----------------|---------------------------------------------------------------------|
| `backend`      | Backend that produced the record (e.g. `nvsim`); also its subdir.   |
| `key`          | Stable identity of the cached point within the backend.             |
| `tool_version` | Version of the producing tool; used to detect stale entries.        |
| `timestamp`    | Unix epoch seconds when the record was written.                     |
| `params`       | Input parameters that define the point.                             |
| `value`        | The cached result.                                                  |

Because the manifest is append-only, the **same `(backend, key)` may appear on
several lines**. The line with the highest `timestamp` (equivalently, the last
occurrence) is **authoritative**; earlier lines are superseded history. The
`pimid-cache prune` command compacts this history.

## The `pimid-cache` tool

A standalone Python 3 (stdlib-only) inspector/maintenance CLI lives at
`scripts/pimid-cache`. It resolves the warehouse root using
`--dir`/`--cache-dir` > `$PIMID_CACHE_DIR` > (legacy parent of
`$PIMID_NVSIM_CACHE_DIR`) > `~/.cache/pimid`.

### Subcommands

- `pimid-cache path` -- print the resolved root and manifest path.
- `pimid-cache list [--backend B]` -- table of unique `(backend, key)` entries
  (latest each): backend, key, tool_version, human timestamp, key params.
- `pimid-cache show <key> [--backend B]` -- pretty-printed full latest record.
- `pimid-cache stale [--current-version X] [--backend B]` -- entries whose
  `tool_version` differs from the current version (default `1.0.0`). These are
  refresh candidates -- re-run them with `--cache wo`.
- `pimid-cache prune [--dry-run] [--force] [--stale] [--backend B] [--current-version X]`
  -- compact the manifest by removing superseded/duplicate (and malformed) lines,
  keeping the latest line per key. With `--stale`, also drop entries whose
  version mismatches and delete their artifact files when locatable. **Defaults
  to a dry run**; pass `--force` to actually apply. All operations are confined
  to the resolved warehouse root.

The `--dir`/`--cache-dir` option is global and may appear before the subcommand.

### Examples

```bash
# Where is the warehouse?
pimid-cache path

# What's cached?
pimid-cache list
pimid-cache list --backend nvsim

# Inspect one point.
pimid-cache show t1_c8388608_n22

# Find entries produced by an old tool version.
pimid-cache stale
pimid-cache stale --current-version 1.1.0

# Refresh a stale point (recompute + overwrite):
#   re-run the relevant PIMID experiment with:  --cache wo

# See what compaction would do, then apply it.
pimid-cache prune                 # dry-run: superseded/duplicate/malformed lines
pimid-cache prune --force         # apply

# Also evict stale entries and their artifacts.
pimid-cache prune --stale                 # dry-run
pimid-cache prune --stale --force         # apply

# Operate on a non-default warehouse.
pimid-cache --dir /scratch/pimid-cache list
PIMID_CACHE_DIR=/scratch/pimid-cache pimid-cache list
```

A missing or empty warehouse is handled gracefully -- the tool prints a
"warehouse empty / not found" notice rather than crashing, and malformed
manifest lines are skipped with a warning.
