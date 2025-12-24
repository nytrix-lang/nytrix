#!/usr/bin/env python3
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BUILD = ROOT / "build" / "release"

def run(cmd):
    res = subprocess.run([str(x) for x in cmd], cwd=ROOT)
    if res.returncode:
        raise SystemExit(res.returncode)

def main(argv):
    cmd = argv[1] if len(argv) > 1 else "all"
    jobs = str(os.cpu_count() or 1)
    if cmd in ("help", "-h", "--help"):
        print("Nytrix prototype build")
        print("commands: all, bin, test, clean")
        return 0
    if cmd == "clean":
        import shutil
        shutil.rmtree(ROOT / "build", ignore_errors=True)
        return 0
    run(["cmake", "-S", ROOT, "-B", BUILD, "-DCMAKE_BUILD_TYPE=Release"])
    run(["cmake", "--build", BUILD, "--target", "ny", "--parallel", jobs])
    if cmd == "test":
        run([BUILD / "ny", "--version"])
    return 0

if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
