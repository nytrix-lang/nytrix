#!/usr/bin/env python3
"""
GPU offload toggle matrix verifier.

Runs the Nytrix binary across GPU-related CLI combinations and validates
that std.os reports consistent state and policy decisions.
"""
import sys
sys.dont_write_bytecode = True

import argparse
import os
import subprocess
from dataclasses import dataclass, field
from typing import Dict, List, Optional

PROBE_CODE = r"""
use std.os *
use std.core.dict *
use std.str.io *

def st = gpu_offload_status(16384)
def pst = parallel_status(16384)
def line = gpu_mode() + "|" + gpu_backend() + "|" + gpu_offload() + "|" + to_str(gpu_min_work()) + "|" + to_str(gpu_async()) + "|" + to_str(gpu_fast_math()) + "|" + to_str(gpu_available()) + "|" + to_str(dict_get(st, "policy_selected", false)) + "|" + to_str(dict_get(st, "active", false)) + "|" + dict_get(st, "reason", "") + "|" + dict_get(st, "selected_backend", "") + "|" + parallel_mode() + "|" + to_str(parallel_threads()) + "|" + to_str(parallel_min_work()) + "|" + to_str(dict_get(pst, "selected", false)) + "|" + dict_get(pst, "reason", "") + "|" + to_str(dict_get(pst, "effective_threads", 1))
print(line)
"""

FIELDS = [
    "mode",
    "backend",
    "offload",
    "min_work",
    "async",
    "fast_math",
    "available",
    "policy_selected",
    "active",
    "reason",
    "selected_backend",
    "parallel_mode",
    "parallel_threads",
    "parallel_min_work",
    "parallel_selected",
    "parallel_reason",
    "parallel_effective_threads",
]

@dataclass
class Case:
    name: str
    flags: List[str] = field(default_factory=list)
    expected: Dict[str, str] = field(default_factory=dict)
    env_overrides: Optional[Dict[str, str]] = None

CASES = [
    Case("default"),
    Case("gpu_off", ["--gpu=off"], {"mode": "off"}),
    Case("parallel_off", ["--parallel=off"], {"parallel_mode": "off", "parallel_selected": "false"}),
    Case(
        "opencl_on",
        [
            "--gpu=opencl",
            "--gpu-backend=opencl",
            "--gpu-offload=on",
            "--gpu-min-work=512",
            "--gpu-async",
        ],
        {
            "mode": "opencl",
            "backend": "opencl",
            "offload": "on",
            "min_work": "512",
            "async": "true",
        },
    ),
    Case(
        "cuda_force",
        [
            "--gpu-backend=cuda",
            "--gpu-offload=force",
            "--gpu-min-work=4096",
            "--gpu-fast-math",
        ],
        {
            "backend": "cuda",
            "offload": "force",
            "min_work": "4096",
            "fast_math": "true",
        },
    ),
    Case(
        "hip_on_alias",
        ["--gpu-target=hip", "--gpu-offload=on", "--gpu-min-work=1024"],
        {"backend": "hip", "offload": "on", "min_work": "1024"},
    ),
    Case(
        "backend_none",
        ["--gpu-backend=none", "--gpu-offload=force"],
        {"backend": "none", "offload": "force"},
    ),
    Case(
        "env_force_unavailable",
        ["--gpu-backend=cuda", "--gpu-offload=force"],
        {"backend": "cuda", "offload": "force", "available": "false"},
        env_overrides={"NYTRIX_GPU_AVAILABLE": "0"},
    ),
    Case(
        "parallel_threads_on",
        ["--parallel=threads", "--threads=8", "--parallel-min-work=1024"],
        {"parallel_mode": "threads", "parallel_threads": "8", "parallel_min_work": "1024", "parallel_selected": "true"},
    ),
    Case(
        "parallel_below_min_work",
        ["--parallel=threads", "--threads=8", "--parallel-min-work=65536"],
        {"parallel_mode": "threads", "parallel_threads": "8", "parallel_min_work": "65536", "parallel_selected": "false", "parallel_reason": "below_min_work"},
    ),
]

def parse_probe_line(stdout: str) -> Dict[str, str]:
    lines = [ln.strip() for ln in stdout.splitlines() if ln.strip()]
    if not lines:
        raise ValueError("no output from probe")
    parts = lines[-1].split("|")
    if len(parts) != len(FIELDS):
        raise ValueError(f"probe output malformed: {lines[-1]!r}")
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

def run_case(ny_bin: str, case: Case) -> Dict[str, str]:
    cmd = [ny_bin, *case.flags, "-c", PROBE_CODE]
    env = os.environ.copy()
    if case.env_overrides:
        env.update(case.env_overrides)
    sanitize_dynamic_loader_env(env)
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if proc.returncode != 0:
        raise RuntimeError(
            f"{case.name} failed (code {proc.returncode})\n"
            f"cmd: {' '.join(cmd)}\n"
            f"stdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr}"
        )
    values = parse_probe_line(proc.stdout)
    for k, v in case.expected.items():
        got = values.get(k, "")
        if got != v:
            raise AssertionError(
                f"{case.name}: expected {k}={v!r}, got {got!r}\n"
                f"flags: {' '.join(case.flags)}\n"
                f"probe: {values}"
            )
    # Cross-case invariants for current runtime.
    if values.get("active") not in ("true", "false"):
        raise AssertionError(f"{case.name}: active is not boolean ({values.get('active')!r})")
    if values.get("reason", "") == "":
        raise AssertionError(f"{case.name}: reason is empty")
    if values.get("parallel_selected") not in ("true", "false"):
        raise AssertionError(f"{case.name}: parallel_selected is not boolean ({values.get('parallel_selected')!r})")
    if values.get("parallel_reason", "") == "":
        raise AssertionError(f"{case.name}: parallel_reason is empty")
    if values.get("parallel_effective_threads", "0").isdigit():
        if int(values.get("parallel_effective_threads", "0")) < 1:
            raise AssertionError(f"{case.name}: parallel_effective_threads must be >= 1")
    return values

def main() -> int:
    ap = argparse.ArgumentParser(description="Nytrix GPU toggle matrix test")
    ap.add_argument("--bin", default="build/release/ny", help="Path to ny binary")
    args = ap.parse_args()

    ny_bin = os.path.abspath(args.bin)
    if not os.path.exists(ny_bin):
        print(f"error: binary not found: {ny_bin}", file=sys.stderr)
        return 1

    print(f"GPU matrix using {ny_bin}")
    failures = 0
    for case in CASES:
        try:
            vals = run_case(ny_bin, case)
            print(
                f"[ok] {case.name:20s} "
                f"mode={vals['mode']:<6s} backend={vals['backend']:<7s} "
                f"offload={vals['offload']:<5s} available={vals['available']:<5s} "
                f"policy={vals['policy_selected']:<5s} active={vals['active']:<5s} "
                f"reason={vals['reason']} "
                f"parallel={vals['parallel_mode']}/{vals['parallel_selected']} "
                f"threads={vals['parallel_threads']}/{vals['parallel_effective_threads']}"
            )
        except Exception as ex:
            failures += 1
            print(f"[xx] {case.name}: {ex}", file=sys.stderr)

    if failures:
        print(f"GPU matrix failed: {failures} case(s)", file=sys.stderr)
        return 1
    print("GPU matrix passed")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
