#!/usr/bin/env python3
"""
Perf gate helper for Nytrix.

Runs a small benchmark suite with selected optimizer profiles, records median
times, and optionally compares against a baseline with regression thresholds.
"""
from __future__ import annotations

import sys
sys.dont_write_bytecode = True

import argparse
import datetime as dt
import json
import os
import platform
import statistics
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BASELINE = ROOT / "build" / "cache" / "perf_gate_baseline.json"
DEFAULT_OUT = ROOT / "build" / "cache" / "perf_gate_latest.json"

DEFAULT_CASES = [
    ("etc/tests/benchmark/binary.ny", "compile"),
    ("etc/tests/benchmark/dict.ny", "balanced"),
    ("etc/tests/benchmark/float.ny", "speed"),
    ("etc/tests/benchmark/fibonacci.ny", "speed"),
    ("etc/tests/benchmark/sieve.ny", "size"),
]

def parse_case(raw: str) -> tuple[str, str]:
    value = (raw or "").strip()
    if not value:
        raise ValueError("empty case")
    if ":" in value:
        path, profile = value.rsplit(":", 1)
        path = path.strip()
        profile = profile.strip().lower()
        if not path:
            raise ValueError(f"invalid case path in '{raw}'")
        if not profile:
            profile = "balanced"
        return path, profile
    return value, "balanced"

def resolve_cases(raw_cases: list[str] | None) -> list[tuple[Path, str]]:
    parsed: list[tuple[str, str]] = []
    if raw_cases:
        for raw in raw_cases:
            parsed.append(parse_case(raw))
    else:
        parsed = list(DEFAULT_CASES)

    out: list[tuple[Path, str]] = []
    for rel_path, profile in parsed:
        p = Path(rel_path)
        if not p.is_absolute():
            p = ROOT / p
        p = p.resolve()
        if not p.exists():
            raise FileNotFoundError(f"benchmark case not found: {p}")
        out.append((p, profile))
    return out

def run_case(
    bin_path: Path,
    case_path: Path,
    profile: str,
    repeats: int,
    warmups: int,
    timeout_sec: int,
) -> dict:
    env = os.environ.copy()
    env["NYTRIX_OPT_PROFILE"] = profile
    env.setdefault("NYTRIX_AUTO_PURITY", "1")
    env.setdefault("NYTRIX_AUTO_MEMO_IMPURE", "1")

    cmd = [str(bin_path), str(case_path)]

    for _ in range(max(0, warmups)):
        warm = subprocess.run(
            cmd,
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=timeout_sec,
            check=False,
        )
        if warm.returncode != 0:
            raise RuntimeError(
                f"warmup failed: case={case_path} profile={profile} code={warm.returncode}"
            )

    samples: list[float] = []
    for _ in range(repeats):
        t0 = time.perf_counter()
        run = subprocess.run(
            cmd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout_sec,
            check=False,
        )
        dt_ms = (time.perf_counter() - t0) * 1000.0
        if run.returncode != 0:
            msg = (
                f"benchmark run failed: case={case_path} profile={profile} "
                f"code={run.returncode}\nstdout:\n{run.stdout}\nstderr:\n{run.stderr}"
            )
            raise RuntimeError(msg)
        samples.append(dt_ms)

    median_ms = statistics.median(samples) if samples else 0.0
    return {
        "id": f"{case_path.relative_to(ROOT).as_posix()}::{profile}",
        "path": case_path.relative_to(ROOT).as_posix(),
        "profile": profile,
        "median_ms": median_ms,
        "samples_ms": samples,
    }

def load_baseline(path: Path) -> dict:
    if not path.exists():
        return {}
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        raise RuntimeError(f"failed to parse baseline json: {path}: {exc}") from exc
    m = raw.get("measurements")
    if isinstance(m, dict):
        out = {}
        for k, v in m.items():
            try:
                out[str(k)] = float(v)
            except (TypeError, ValueError):
                continue
        return out
    return {}

def save_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")

def main() -> int:
    ap = argparse.ArgumentParser(description="Nytrix perf regression gate")
    ap.add_argument("--bin", default=str(ROOT / "build" / "release" / "ny"))
    ap.add_argument("--baseline", default=str(DEFAULT_BASELINE))
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--case", action="append", help="path[:profile] (repeatable)")
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--warmups", type=int, default=1)
    ap.add_argument("--timeout", type=int, default=120, help="per-run timeout seconds")
    ap.add_argument("--max-regression-pct", type=float, default=10.0)
    ap.add_argument("--min-regression-ms", type=float, default=5.0)
    ap.add_argument(
        "--write-baseline",
        action="store_true",
        help="write current measurements to baseline path",
    )
    args = ap.parse_args()

    bin_path = Path(args.bin).resolve()
    if not bin_path.exists():
        print(f"error: binary not found: {bin_path}", file=sys.stderr)
        return 2
    if args.repeats < 1:
        print("error: --repeats must be >= 1", file=sys.stderr)
        return 2
    if args.warmups < 0:
        print("error: --warmups must be >= 0", file=sys.stderr)
        return 2

    cases = resolve_cases(args.case)
    print(f"Perf gate using {bin_path}")

    measurements = []
    for case_path, profile in cases:
        result = run_case(
            bin_path=bin_path,
            case_path=case_path,
            profile=profile,
            repeats=args.repeats,
            warmups=args.warmups,
            timeout_sec=args.timeout,
        )
        measurements.append(result)
        print(
            f"[ok] {result['path']:<36} profile={profile:<8} "
            f"median={result['median_ms']:.2f}ms"
        )

    payload = {
        "schema": 1,
        "created_at_utc": dt.datetime.now(dt.timezone.utc)
        .replace(microsecond=0)
        .isoformat()
        .replace("+00:00", "Z"),
        "host": {
            "system": platform.system(),
            "machine": platform.machine(),
            "python": platform.python_version(),
        },
        "bin": str(bin_path),
        "repeats": args.repeats,
        "warmups": args.warmups,
        "measurements": {m["id"]: m["median_ms"] for m in measurements},
        "cases": measurements,
    }
    save_json(Path(args.out), payload)
    print(f"Wrote latest perf snapshot: {args.out}")

    baseline_path = Path(args.baseline).resolve()
    if args.write_baseline:
        save_json(baseline_path, payload)
        print(f"Wrote baseline: {baseline_path}")
        return 0

    baseline = load_baseline(baseline_path)
    if not baseline:
        print(f"No baseline at {baseline_path}; skipping regression gate.")
        print("Tip: run with --write-baseline once on your reference machine.")
        return 0

    failures = []
    max_ratio = 1.0 + (args.max_regression_pct / 100.0)
    for m in measurements:
        case_id = m["id"]
        cur = float(m["median_ms"])
        base = baseline.get(case_id)
        if base is None or base <= 0:
            continue
        ratio = cur / base
        delta = cur - base
        if ratio > max_ratio and delta > args.min_regression_ms:
            failures.append((case_id, base, cur, delta, ratio))

    if failures:
        print("Perf regressions detected:")
        for case_id, base, cur, delta, ratio in failures:
            pct = (ratio - 1.0) * 100.0
            print(
                f"  - {case_id}: baseline={base:.2f}ms current={cur:.2f}ms "
                f"delta=+{delta:.2f}ms ({pct:.2f}%)"
            )
        return 1

    print("Perf gate passed.")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
