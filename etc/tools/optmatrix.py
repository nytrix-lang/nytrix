#!/usr/bin/env python3
"""
Optimization feature matrix verifier.

Validates combinations of:
- LLVM opt level flags (-O0..-O3)
- NYTRIX_OPT_PROFILE (none/compile/balanced/speed/size/default)
- GPU/parallel policy toggles
"""
import sys
sys.dont_write_bytecode = True

import argparse
import os
import subprocess
from dataclasses import dataclass, field
from typing import Dict, List

PROBE_CODE = r"""
use std.math.vector *
use std.os *
use std.core.dict *
use std.str.io *

def a = vec4(1, 2, 3, 4)
def b = vec4(10, 20, 30, 40)
def c = add(a, b)
def d = dot(c, c)
assert(d == 3630, "vector math invariant")

def gst = gpu_offload_status(16384)
def pst = parallel_status(16384)
def line = to_str(d) + "|" + to_str(dict_get(gst, "policy_selected", false)) + "|" + dict_get(gst, "reason", "") + "|" + to_str(dict_get(pst, "selected", false)) + "|" + dict_get(pst, "reason", "")
print(line)
"""

FIELDS = ["dot", "gpu_selected", "gpu_reason", "parallel_selected", "parallel_reason"]
PROFILES = ["default", "none", "compile", "balanced", "speed", "size"]
OPT_FLAGS = ["-O0", "-O1", "-O2", "-O3"]

@dataclass
class ToggleCase:
    name: str
    flags: List[str] = field(default_factory=list)
    expected: Dict[str, str] = field(default_factory=dict)

TOGGLES = [
    ToggleCase("default"),
    ToggleCase(
        "cpu_only",
        ["--gpu=off", "--parallel=off"],
        {"gpu_selected": "false", "parallel_selected": "false"},
    ),
    ToggleCase(
        "aggressive",
        ["--gpu-backend=hip", "--gpu-offload=force", "--parallel=threads", "--threads=8", "--parallel-min-work=1024"],
        {"parallel_selected": "true"},
    ),
]

def parse_probe(stdout: str) -> Dict[str, str]:
    lines = [ln.strip() for ln in stdout.splitlines() if ln.strip()]
    if not lines:
        raise ValueError("probe produced no output")
    parts = lines[-1].split("|")
    if len(parts) != len(FIELDS):
        raise ValueError(f"malformed probe line: {lines[-1]!r}")
    return dict(zip(FIELDS, parts))

def sanitize_dynamic_loader_env(run_env: Dict[str, str]) -> None:
    # Keep matrix runs deterministic: host preloads can break sanitizer startup.
    preserve = (os.environ.get("NYTRIX_TEST_PRESERVE_PRELOAD") or "").strip().lower()
    if preserve not in {"1", "true", "yes", "on"}:
        run_env.pop("LD_PRELOAD", None)
        run_env.pop("DYLD_INSERT_LIBRARIES", None)
    preload = (run_env.get("NY_TEST_PRELOAD") or "").strip()
    if preload:
        run_env["LD_PRELOAD"] = preload
    else:
        run_env.pop("LD_PRELOAD", None)

def run_case(ny_bin: str, profile: str, opt_flag: str, toggle: ToggleCase) -> Dict[str, str]:
    cmd = [ny_bin, opt_flag, *toggle.flags, "-c", PROBE_CODE]
    env = os.environ.copy()
    env["NYTRIX_OPT_PROFILE"] = profile
    sanitize_dynamic_loader_env(env)
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if proc.returncode != 0:
        raise RuntimeError(
            f"profile={profile} opt={opt_flag} toggle={toggle.name} failed ({proc.returncode})\n"
            f"cmd: {' '.join(cmd)}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    vals = parse_probe(proc.stdout)
    if vals.get("dot") != "3630":
        raise AssertionError(
            f"profile={profile} opt={opt_flag} toggle={toggle.name}: expected dot=3630 got {vals.get('dot')!r}"
        )
    if vals.get("gpu_selected") not in ("true", "false"):
        raise AssertionError(f"gpu_selected not boolean: {vals.get('gpu_selected')!r}")
    if vals.get("parallel_selected") not in ("true", "false"):
        raise AssertionError(f"parallel_selected not boolean: {vals.get('parallel_selected')!r}")
    if vals.get("gpu_reason", "") == "":
        raise AssertionError("gpu_reason empty")
    if vals.get("parallel_reason", "") == "":
        raise AssertionError("parallel_reason empty")
    for k, exp in toggle.expected.items():
        got = vals.get(k, "")
        if got != exp:
            raise AssertionError(
                f"profile={profile} opt={opt_flag} toggle={toggle.name}: expected {k}={exp!r}, got {got!r}"
            )
    return vals

def main() -> int:
    ap = argparse.ArgumentParser(description="Nytrix optimization feature matrix test")
    ap.add_argument("--bin", default="build/release/ny", help="Path to ny binary")
    args = ap.parse_args()

    ny_bin = os.path.abspath(args.bin)
    if not os.path.exists(ny_bin):
        print(f"error: binary not found: {ny_bin}", file=sys.stderr)
        return 1

    print(f"Optimization matrix using {ny_bin}")
    failures = 0
    for profile in PROFILES:
        for opt_flag in OPT_FLAGS:
            for toggle in TOGGLES:
                label = f"{profile}/{opt_flag}/{toggle.name}"
                try:
                    vals = run_case(ny_bin, profile, opt_flag, toggle)
                    print(
                        f"[ok] {label:30s} "
                        f"gpu={vals['gpu_selected']}({vals['gpu_reason']}) "
                        f"par={vals['parallel_selected']}({vals['parallel_reason']})"
                    )
                except Exception as ex:
                    failures += 1
                    print(f"[xx] {label}: {ex}", file=sys.stderr)

    if failures:
        print(f"Optimization matrix failed: {failures} case(s)", file=sys.stderr)
        return 1
    print("Optimization matrix passed")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
