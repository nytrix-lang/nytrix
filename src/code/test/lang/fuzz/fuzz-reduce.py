#!/usr/bin/env python3
"""Line-based reducer for a failing fuzz-expr.py program.

usage: fuzz-reduce.py NYC SEEDDIR [-O0|-O2]
Deletes lines of SEEDDIR/p.c while GCC still builds and runs it cleanly and
NYC's result still differs; writes SEEDDIR/min.c and prints it.
"""
import os, subprocess, sys

nyc, seeddir = sys.argv[1], sys.argv[2]
opt = sys.argv[3] if len(sys.argv) > 3 else "-O0"
os.chdir(seeddir)
GCC = ["gcc", "-Wall", "-Werror=return-type", "-Werror=uninitialized",
       "-Wno-overflow", "-Wno-unused", "-O1", "-o", "rg", "r.c"]


def result(src, which):
    open("r.c", "w").write(src)
    if which == "gcc":
        if subprocess.run(GCC, capture_output=True).returncode:
            return None
        exe = "./rg"
    else:
        if subprocess.run([nyc, "-c", opt, "r.c", "-o", "r.o"], capture_output=True).returncode:
            return None
        if subprocess.run(["gcc", "-o", "rn", "r.o"], capture_output=True).returncode:
            return None
        exe = "./rn"
    try:
        p = subprocess.run([exe], capture_output=True, text=True, timeout=10)
    except Exception:
        return None
    return p.returncode, p.stdout


def still_bad(src):
    a = result(src, "gcc")
    if not a or a[0]:
        return False
    b = result(src, "nyc")
    return b is not None and a != b


lines = open("p.c").read().split("\n")
if not still_bad("\n".join(lines)):
    sys.exit("seed does not fail at " + opt)
chunk = max(1, len(lines) // 2)
while chunk >= 1:
    changed = True
    while changed:
        changed = False
        i = len(lines) - chunk
        while i >= 0:
            trial = lines[:i] + lines[i + chunk:]
            if still_bad("\n".join(trial)):
                lines, changed = trial, True
            i -= chunk
    chunk //= 2
open("min.c", "w").write("\n".join(lines))
print("\n".join(lines[4:]))
