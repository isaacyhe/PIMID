#!/usr/bin/env python3
"""check-host-cache-defaults.py [--pimid PATH] [--workload PATH] [--params DIR]

Checks that a host with no `cache:` block (or a partial one) gets the cache record's host hierarchy (params/cache/default.yaml,
section `host`) and that the run log says so. For each case it writes a small system configuration, starts the simulator and
reads the "[params] ... caches (cache record, host core)" line the configuration step prints; the run is stopped at that line.

  1 core, no block      L1D 48 KB 12-way 5 cycles, L1I 32 KB 8-way, L2 2048 KB 16-way 16 cycles, L3 1920 KB 15-way (one slice)
  4 and 16 cores        the same with an L3 of 4 and 16 slices
  6 cores               a power-of-two number of slices (8) is built, and the log says so
  partial block         the keys given keep their values, the rest is the record's (ways that do not fit a size given adapt, or are refused)
  L3 size / none        an explicit total size is shared with the record's ways and latency; `l3: none` (or l3_kb 0) is no L3; nothing else is
  L3 inclusion          the record's (non-inclusive) unless l3_inclusion gives inclusive or non-inclusive; another value, or the key without
                        an L3, is refused
  old host              sizes, ways, latencies and inclusion all given: the old hierarchy

Exit status 0 when every case passes."""
import os, re, shutil, signal, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

def arg(name, default):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default

PIMID = arg("--pimid", os.path.join(ROOT, "build", "pimid"))
WORKLOAD = arg("--workload", os.path.join(ROOT, "benchmarks", "pim_kernels", "pointer_chase", "pointer_chase_omp"))
PARAMS = arg("--params", os.environ.get("PIMID_PARAMS") or os.path.join(ROOT, "params"))

def flow(line):
    return {k: float(v) if "." in v else int(v) for k, v in re.findall(r"(\w+):\s*([0-9.]+)", line.split("{", 1)[1])}

def record():
    rec, section = {"levels": {}, "host": {}}, None
    for ln in open(os.path.join(PARAMS, "cache", "default.yaml")):
        if re.match(r"^(levels|host):", ln):
            section = ln.split(":")[0]; continue
        m = re.match(r"^  (l1d|l1i|l2|l3):\s*\{", ln)
        if m and section:
            rec[section][m.group(1)] = flow(ln)
            inc = re.search(r"inclusion:\s*([a-z-]+)", ln)
            if inc: rec[section][m.group(1)]["inclusion"] = inc.group(1)
    return rec

REC = record()
H, G = REC["host"], REC["levels"]

def slices(cores):
    s = 1
    while s < cores: s *= 2
    return s

TEMPLATE = """system:
  hosts:
    - name: host
      core_type: ooo_core
      num_cores: %(cores)d
      frequency_mhz: 2000
      tech_node_nm: 22
%(cache)s      memory:
        technology: DDR5

  devices:
    - name: device
      attachment: internal
      frequency_mhz: 2000
      tech_node_nm: 22
      pe_type: alu_core
      num_pes: 8
      pim:
        placement:
          level: BANK
        mc:
          type: simple
      memory:
        technology: DDR5
        banks: 16
      noc:
        model: detailed
"""

def record_line(cores):
    s = slices(cores)
    l2 = "%d KB %d-way %d cycles load-to-use (%d beyond the L1D)" % (H["l2"]["kb"], H["l2"]["ways"], H["l2"]["load_to_use_cycles"], H["l2"]["load_to_use_cycles"] - H["l1d"]["load_to_use_cycles"])
    l3 = "%d KB %d-way (%d core(s) x %d KB slices), %s, %d cycles load-to-use" % (H["l3"]["slice_kb"] * s, H["l3"]["ways"], cores, H["l3"]["slice_kb"], H["l3"]["inclusion"],
                                                                                 round(H["l3"]["load_to_use_ns"] * 2))
    return ["L1D %d KB %d-way %d cycles" % (H["l1d"]["kb"], H["l1d"]["ways"], H["l1d"]["load_to_use_cycles"]),
            "L1I %d KB %d-way" % (H["l1i"]["kb"], H["l1i"]["ways"]), "L2 " + l2, "L3 " + l3]

REC_L1D = "L1D %d KB %d-way %d cycles" % (H["l1d"]["kb"], H["l1d"]["ways"], H["l1d"]["load_to_use_cycles"])
L3C = "%d cycles load-to-use" % round(H["l3"]["load_to_use_ns"] * 2)
OLD_HOST = ("      cache:\n        l1d_kb: 32\n        l1d_ways: 8\n        l1d_latency_ns: 0.5\n        l1i_kb: 32\n        l1i_ways: 4\n        l1i_latency_ns: 0.5\n"
            "        l2_kb: 256\n        l2_ways: 8\n        l2_latency_ns: 0.5\n        l3_kb: 32768\n        l3_ways: 16\n        l3_latency_ns: 3.0\n"
            "        l3_inclusion: inclusive\n")
SLICES4 = "(4 core(s) x %d KB slices)" % H["l3"]["slice_kb"]

# (name, cores, cache block, strings the line must hold, notes that must be printed)
CASES = [
    ("no block, 1 core", 1, "", record_line(1), []),
    ("no block, 4 cores", 4, "", record_line(4), []),
    ("no block, 16 cores", 16, "", record_line(16), []),
    ("no block, 6 cores (8 slices)", 6, "", ["L3 %d KB" % (H["l3"]["slice_kb"] * 8), "6 core(s)"], ["slices (%d KB) are built" % (H["l3"]["slice_kb"] * 8)]),
    ("partial: l2_kb 1024", 4, "      cache:\n        l2_kb: 1024\n",
        [REC_L1D, "L2 1024 KB %d-way %d cycles load-to-use" % (H["l2"]["ways"], H["l2"]["load_to_use_cycles"]), "L3 %d KB %d-way (4 core(s)" % (H["l3"]["slice_kb"] * 4, H["l3"]["ways"])], []),
    ("partial: l1d_kb 32 (ways adapt)", 1, "      cache:\n        l1d_kb: 32\n", ["L1D 32 KB 8-way %d cycles" % H["l1d"]["load_to_use_cycles"], "L2 %d KB %d-way" % (H["l2"]["kb"], H["l2"]["ways"])],
        ["L1D 32 KB: the record's 12 ways do not make a power-of-two number of sets; 8-way"]),
    ("L3 size: l3_kb 32768 (ways adapt)", 4, "      cache:\n        l3_kb: 32768\n", ["L3 32768 KB 16-way", L3C], ["L3 32768 KB: the record's 15 ways do not make a power-of-two number of sets; 16-way"]),
    ("L3 size: l3_kb 15360 (15-way kept)", 4, "      cache:\n        l3_kb: 15360\n", ["L3 15360 KB 15-way", L3C], []),
    ("L3 size: l3_kb 11520 (refused)", 4, "      cache:\n        l3_kb: 11520\n", ["FATAL", "L3 11520 KB cannot be built with the record's 15 ways"], []),
    ("L3 size and ways: l3_kb 32768, l3_ways 16", 4, "      cache:\n        l3_kb: 32768\n        l3_ways: 16\n", ["L3 32768 KB 16-way", L3C], []),
    ("L3 none: l3: none", 4, "      cache:\n        l3: none\n", [REC_L1D, "L3: none"], []),
    ("L3 none: l3_kb 0", 4, "      cache:\n        l3_kb: 0\n", ["L3: none"], []),
    ("l3: none with l3_kb (refused)", 4, "      cache:\n        l3: none\n        l3_kb: 4096\n", ["FATAL", "l3: none and l3_kb 4096"], []),
    ("l3: record (refused)", 4, "      cache:\n        l3: record\n", ["FATAL", "the only value is `none`"], []),
    ("L3 inclusion: inclusive", 4, "      cache:\n        l3_inclusion: inclusive\n", [SLICES4 + ", inclusive, " + L3C], []),
    ("L3 inclusion: non-inclusive", 4, "      cache:\n        l3_inclusion: non-inclusive\n", [SLICES4 + ", non-inclusive, " + L3C], []),
    ("L3 inclusion with l3_kb 32768", 4, "      cache:\n        l3_kb: 32768\n        l3_inclusion: inclusive\n", ["L3 32768 KB 16-way, inclusive, " + L3C], []),
    ("l3_inclusion: exclusive (refused)", 4, "      cache:\n        l3_inclusion: exclusive\n", ["FATAL", "the values are `inclusive` and `non-inclusive`"], []),
    ("l3_inclusion without an L3 (refused)", 4, "      cache:\n        l3: none\n        l3_inclusion: inclusive\n", ["FATAL", "has no L3"], []),
    ("old host, sizes + ways + latencies", 4, OLD_HOST,
        ["L1D 32 KB 8-way 1 cycles", "L1I 32 KB 4-way", "L2 256 KB 8-way 1 cycles", "L3 32768 KB 16-way, inclusive, 6 cycles"], []),
]

def run_case(tmp, name, cores, cache, want, want_note):
    cfg = os.path.join(tmp, "case.yaml")
    open(cfg, "w").write(TEMPLATE % {"cores": cores, "cache": cache})
    env = dict(os.environ, PIMID_COSIM_NO_OFFLOAD="1", OMP_NUM_THREADS=str(cores), PIMID_PARAMS=PARAMS)
    cmd = [PIMID, "--method", "exec", "--config", cfg, "--workload", WORKLOAD, "--lines", "64", "--steps", "100", "--workload-type", "openmp"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, start_new_session=True, universal_newlines=True)
    seen, found, notes = [], None, []
    try:
        for ln in p.stdout:
            seen.append(ln)
            if "FATAL" in ln: found = ln; break
            if "caches (cache record, host core)" in ln: found = ln; break
            if "do not make a power-of-two number of sets; " in ln: notes.append(ln)
            if len(seen) > 20000: break
    finally:
        try: os.killpg(p.pid, signal.SIGKILL)
        except Exception: pass
        p.wait()
    ok = found is not None and all(w in found for w in want) and all(any(n in x for x in notes) for n in want_note)
    print("%-34s %s" % (name, "ok" if ok else "FAIL"))
    if not ok:
        print("   expected in the line: %s" % want + ("; notes: %s" % want_note if want_note else ""))
        print("   got:                  %s" % (found.strip() if found else "(no caches line)"))
    return ok

def main():
    for what, path in (("simulator", PIMID), ("cache record", os.path.join(PARAMS, "cache", "default.yaml"))):
        if not os.path.exists(path): print("missing %s: %s" % (what, path)); return 2
    tmp = tempfile.mkdtemp(prefix="hostcache_")
    try:
        results = [run_case(tmp, *c) for c in CASES]
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("%d of %d cases pass" % (sum(results), len(results)))
    return 0 if all(results) else 1

if __name__ == "__main__":
    sys.exit(main())
