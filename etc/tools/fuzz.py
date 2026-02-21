import sys
sys.dont_write_bytecode = True
import argparse
import concurrent.futures
import os
import random
import subprocess
import time
import tempfile
from dataclasses import dataclass
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))

from context import ROOT
from utils import log, log_ok, step, warn

def rand_ident(rng):
    letters = "abcdefghijklmnopqrstuvwxyz"
    return rng.choice(letters) + "".join(rng.choice(letters) for _ in range(rng.randint(1, 7)))

def gen_expr(rng):
    ints = [str(rng.randint(-20, 200)) for _ in range(6)]
    atoms = ints + ['"x"', '"abc"', "true", "false"]
    a = rng.choice(atoms)
    b = rng.choice(atoms)
    op = rng.choice(["+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">="])
    return f"({a} {op} {b})"

def gen_snippet(rng):
    x = rand_ident(rng)
    y = rand_ident(rng)
    z = rand_ident(rng)
    return "\n".join(
        [
            f"def {x} = {rng.randint(0, 9)}",
            f"def {y} = {gen_expr(rng)}",
            f"if({x} < 5){{ def {z} = {gen_expr(rng)} }} else {{ def {z} = {gen_expr(rng)} }}",
            f"print({z})",
        ]
    )

def gen_mem_snippet(rng):
    n = rng.randint(8, 128)
    idx = rng.randint(0, max(0, n - 1))
    return "\n".join(
        [
            "use std.core *",
            f"def p = malloc({n})",
            f"store8(p, 65, {idx})",
            f"def v = load8(p, {idx})",
            "if(v != 65){ panic('mem mismatch') }",
            "free(p)",
        ]
    )

def gen_slice_snippet(rng):
    a = rng.randint(0, 3)
    b = rng.randint(4, 9)
    step = rng.choice([1, 2, 3])
    text = rng.choice(['"hello_world"', '"abcdefghij"', '"nytrix_slice_test"'])
    return "\n".join(
        [
            f"def s = {text}",
            f"def p = s[{a}:{b}:{step}]",
            "print(p)",
        ]
    )

def gen_import_snippet(rng):
    mod = rng.choice(["std.core", "std.str", "std.core.reflect"])
    return "\n".join(
        [
            f"use {mod} *",
            f"def x = {rng.randint(1, 20)}",
            "print(x)",
        ]
    )

def gen_layout_snippet(rng):
    name = "P" + rand_ident(rng)
    x = rand_ident(rng)
    y = rand_ident(rng)
    return "\n".join(
        [
            f"layout {name} {{",
            f"    int: {x},",
            f"    int: {y}",
            "}",
            'print("ok")',
        ]
    )

def run_case_proc(ny_bin, src, timeout_s):
    p = subprocess.run(
        [ny_bin, "-c", src],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout_s,
    )
    err_out = p.stderr or ""
    crashed = p.returncode < 0 or "Caught signal" in err_out or "AddressSanitizer" in err_out
    panicked = "Panic:" in err_out
    return crashed, panicked, p.returncode, p.stdout, p.stderr

@dataclass
class CaseResult:
    index: int
    seed: int
    src: str
    crashed: bool
    panicked: bool
    timed_out: bool
    returncode: int
    stdout: str
    stderr: str

def gen_case(index, base_seed, mode):
    case_seed = (base_seed * 0x9E3779B1 + index * 0x85EBCA6B) & 0xFFFFFFFF
    rng = random.Random(case_seed)
    if mode == "parser":
        src = rng.choice([gen_snippet, gen_slice_snippet, gen_layout_snippet])(rng)
    elif mode == "memory":
        src = gen_mem_snippet(rng)
    elif mode == "imports":
        src = gen_import_snippet(rng)
    elif mode == "slices":
        src = gen_slice_snippet(rng)
    else:
        pick = rng.randint(0, 99)
        if pick < 35:
            src = gen_snippet(rng)
        elif pick < 60:
            src = gen_mem_snippet(rng)
        elif pick < 78:
            src = gen_slice_snippet(rng)
        elif pick < 90:
            src = gen_import_snippet(rng)
        else:
            src = gen_layout_snippet(rng)
    return case_seed, src

def run_case_index(ny_bin, timeout_s, index, base_seed, mode):
    case_seed, src = gen_case(index, base_seed, mode)
    try:
        crashed, panicked, rc, out, err_out = run_case_proc(ny_bin, src, timeout_s)
        timed_out = False
    except subprocess.TimeoutExpired:
        crashed = True
        panicked = False
        timed_out = True
        rc, out, err_out = 124, "", "timeout"
    return CaseResult(
        index=index,
        seed=case_seed,
        src=src,
        crashed=crashed,
        panicked=panicked,
        timed_out=timed_out,
        returncode=rc,
        stdout=out,
        stderr=err_out,
    )

def save_crash_artifacts(seed, bad_cases, max_saved):
    out_dir = Path(tempfile.gettempdir()) / f"ny_fuzz_{seed}"
    out_dir.mkdir(parents=True, exist_ok=True)
    keep = bad_cases[:max_saved]
    for r in keep:
        base = out_dir / f"case_{r.index:06d}_seed_{r.seed}"
        (base.with_suffix(".ny")).write_text(r.src, encoding="utf-8")
        (base.with_suffix(".stderr.txt")).write_text(r.stderr or "", encoding="utf-8")
        (base.with_suffix(".stdout.txt")).write_text(r.stdout or "", encoding="utf-8")
    
    summary_path = out_dir / "summary.txt"
    with open(summary_path, "w", encoding="utf-8") as f:
        f.write(f"seed={seed}\n")
        f.write(f"bad_cases={len(bad_cases)}\n")
        f.write(f"saved={len(keep)}\n")
        if bad_cases:
            first = bad_results[0]
            f.write(f"first_case={first.index}\n")
    return out_dir

def run_fuzz_harness(bin_path, iterations=200, jobs=1, timeout_s=1.2, mode="mixed", fail_on_panic=False):
    seed = int(time.time())
    bin_path = str(Path(bin_path).resolve())
    
    log("FUZZ", f"seed={seed} iterations={iterations} jobs={jobs} mode={mode}")
    
    bad_results = []
    total = 0
    crashes = 0
    panics = 0
    timeouts = 0
    
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        futures = [
            ex.submit(run_case_index, bin_path, timeout_s, i, seed, mode)
            for i in range(iterations)
        ]
        for fut in concurrent.futures.as_completed(futures):
            r = fut.result()
            total += 1
            if r.crashed: crashes += 1
            if r.panicked: panics += 1
            if r.timed_out: timeouts += 1
            
            is_bad = r.crashed or r.timed_out or (fail_on_panic and r.panicked)
            if is_bad:
                bad_results.append(r)
                warn(f"fail at case {r.index} seed={r.seed} rc={r.returncode}")

    log_ok(f"done total={total} crashes={crashes} timeouts={timeouts} panics={panics}")
    
    if bad_results:
        bad_results.sort(key=lambda x: x.index)
        # save_crash_artifacts logic simplified for here
        return 1
    return 0
