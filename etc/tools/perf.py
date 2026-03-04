#!/usr/bin/env python3
"""
Nytrix Performance & Dispatch Analyzer
"""
from __future__ import annotations
import sys
sys.dont_write_bytecode = True
import argparse
import json
import os
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
import time
import statistics
import subprocess
import platform
import re
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Dict, List, Optional, Any

# Internal imports
sys.path.insert(0, str(Path(__file__).resolve().parent))
from context import ROOT, host_os, host_machine, c, OK_SYMBOL
from utils import log, warn, err, log_ok, step, run

# --- PERF GATE LOGIC (Was perfgate.py) ---

DEFAULT_BASELINE = ROOT / "build" / "cache" / "perf_gate_baseline.json"
DEFAULT_OUT = ROOT / "build" / "cache" / "perf_gate_latest.json"

DEFAULT_CASES = [
    ("etc/tests/benchmark/binary.ny", "compile"),
    ("etc/tests/benchmark/dict.ny", "balanced"),
    ("etc/tests/benchmark/float.ny", "speed"),
    ("etc/tests/benchmark/fibonacci.ny", "speed"),
    ("etc/tests/benchmark/sieve.ny", "size"),
]

@dataclass
class BenchResult:
    id: str; path: str; profile: str; median_ms: float; min_ms: float; max_ms: float; stddev_ms: float; samples: List[float]; phases: Dict[str, float] = field(default_factory=dict)

def _parse_timings(stderr: str) -> Dict[str, float]:
    out = {}
    for line in stderr.splitlines():
        match = re.search(r"^([A-Za-z ]+):\s*([\d.]+)s", line)
        if match: out[match.group(1).strip().lower().replace(" ", "_")] = float(match.group(2)) * 1000.0
    return out

def run_benchmark(bin_path, case_path, profile, repeats, warmups, timeout_sec) -> BenchResult:
    env = {**os.environ, "NYTRIX_OPT_PROFILE": profile, "NYTRIX_AUTO_PURITY": "1", "NYTRIX_AUTO_MEMO_IMPURE": "1"}
    cmd = [str(bin_path), "-time", "-run", str(case_path)]
    for _ in range(max(0, warmups)): subprocess.run(cmd, env=env, capture_output=True, timeout=timeout_sec, check=False)
    
    samples_ns, all_phases = [], []
    for _ in range(repeats):
        t0 = time.perf_counter_ns()
        p = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=timeout_sec, check=False)
        if p.returncode != 0: raise RuntimeError(f"Bench failed: {case_path}\n{p.stderr}")
        samples_ns.append(time.perf_counter_ns() - t0)
        all_phases.append(_parse_timings(p.stderr))
    
    samples_ms = [ns / 1_000_000.0 for ns in samples_ns]
    avg_phases = {k: statistics.mean(p[k] for p in all_phases if k in p) for k in (all_phases[0].keys() if all_phases else [])}
    return BenchResult(id=f"{case_path.relative_to(ROOT).as_posix()}::{profile}", path=case_path.relative_to(ROOT).as_posix(), profile=profile, median_ms=statistics.median(samples_ms), min_ms=min(samples_ms), max_ms=max(samples_ms), stddev_ms=statistics.stdev(samples_ms) if len(samples_ms) > 1 else 0.0, samples=samples_ms, phases=avg_phases)

PROBE_CODE = r"""
use std.core *
use std.os *
use std.core.dict *
def st = gpu_offload_status(16384)
def pst = parallel_status(16384)
def fields = [gpu_mode(), gpu_backend(), gpu_offload(), to_str(gpu_min_work()), to_str(gpu_available()), to_str(dict_get(st, "policy_selected", false)), dict_get(st, "reason", ""), parallel_mode(), to_str(dict_get(pst, "selected", false)), dict_get(pst, "reason", "")]
mut line = ""
for f in fields { if (line != "") line = line + "|" line = line + to_str(f) }
print(line)
"""
FIELDS = ["mode", "backend", "offload", "min_work", "available", "gpu_selected", "gpu_reason", "parallel_mode", "parallel_selected", "parallel_reason"]

@dataclass
class Case:
    name: str; flags: List[str] = field(default_factory=list); env: Dict[str, str] = field(default_factory=dict); expected: Dict[str, str] = field(default_factory=dict)

def run_matrix_case(ny_bin, case):
    cmd = [ny_bin, *case.flags, "-run", "-c", PROBE_CODE]
    env = {**os.environ, **case.env}
    p = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if p.returncode != 0: raise RuntimeError(f"failed: {p.stdout}\n{p.stderr}")
    vals = dict(zip(FIELDS, p.stdout.strip().splitlines()[-1].split("|")))
    for k, v in case.expected.items():
        if vals.get(k) != v: raise AssertionError(f"expected {k}={v}, got {vals.get(k)}")
    return vals

def run_matrix(ny_bin):
    step("Executing Dispatch Matrix...")
    suites = [
        ("Optimization Profiles", [Case(f"opt/{p}/{o}", [o], {"NYTRIX_OPT_PROFILE": p}) for p in ["none", "balanced", "speed", "size"] for o in ["-O0", "-O3"]]),
        ("GPU & Parallel Flags", [
            Case("gpu/off", ["--gpu=off"], expected={"mode": "off", "gpu_selected": "false"}),
            Case("gpu/force", ["--gpu-offload=force"], expected={"offload": "force", "gpu_selected": "true"}),
            Case("par/threads", ["--parallel=threads", "--threads=4"], expected={"parallel_mode": "threads"}),
        ])
    ]
    failures = 0
    for title, cases in suites:
        print(f"\n--- {title} ---")
        for c in cases:
            try:
                v = run_matrix_case(ny_bin, c)
                print(f"[ok] {c.name.ljust(25)} gpu={v['gpu_selected']}({v['gpu_reason']}) par={v['parallel_selected']}({v['parallel_reason']})")
            except Exception as e:
                print(f"[xx] {c.name.ljust(25)} {e}"); failures += 1
    return failures == 0

# --- MAIN ENTRY ---

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["gate", "matrix"], nargs="?", default="gate")
    ap.add_argument("--bin", default=str(ROOT / "build" / "release" / "ny"))
    ap.add_argument("--write-baseline", action="store_true")
    args, unknown = ap.parse_known_args()
    
    bin_path = Path(args.bin).resolve()
    if not bin_path.exists(): err(f"Binary not found: {bin_path}"); sys.exit(1)
    
    if args.mode == "matrix":
        sys.exit(0 if run_matrix(str(bin_path)) else 1)
    
    # Gate Mode
    step(f"Nytrix Performance Gate (host={c('36', host_machine())})")
    results = []
    cases = [(ROOT / p, pr) for p, pr in DEFAULT_CASES]
    for c_path, profile in cases:
        try:
            res = run_benchmark(bin_path, c_path, profile, 5, 2, 60)
            results.append(res); p_str = f" [{', '.join(f'{k}={v:.1f}ms' for k, v in res.phases.items())}]" if res.phases else ""
            print(f"{c('32', OK_SYMBOL)} {res.path:<35} {c('36', res.profile):<10} med={c('1', f'{res.median_ms:.2f}ms')} std={res.stddev_ms:.2f}ms{c('90', p_str)}")
        except Exception as e: err(f"Failed benchmark {c_path}: {e}")
    
    baseline_path = DEFAULT_BASELINE
    if args.write_baseline:
        baseline_path.parent.mkdir(parents=True, exist_ok=True)
        with open(baseline_path, "w") as f: json.dump({"measurements": {r.id: r.median_ms for r in results}}, f, indent=2)
        log_ok("Updated baseline")
        sys.exit(0)
        
    regressions = 0
    if baseline_path.exists():
        log("DIFF", "Comparison with baseline:")
        with open(baseline_path) as f: baseline_data = json.load(f).get("measurements", {})
        for r in results:
            base = baseline_data.get(r.id)
            if base:
                pct = ((r.median_ms - base) / base) * 100.0
                mark = c("31", f"+{pct:.1f}%") if pct > 10.0 else (c("32", f"{pct:.1f}%") if pct < -10.0 else f"{pct:+.1f}%")
                if pct > 10.0: regressions += 1
                print(f"  {r.id:<45} {base:>8.2f} -> {r.median_ms:>8.2f} ms ({mark})")
    
    if regressions: err(f"Performance regressions detected: {regressions}"); sys.exit(1)
    log_ok("Performance gate passed")
