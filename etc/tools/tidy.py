import sys
sys.dont_write_bytecode = True
import os
import re
import subprocess
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))

from context import ROOT
from utils import log, log_ok, which

EXTS = {".c", ".h", ".ny", ".py", ".md"}
TAB = b"   "

def chunks(xs, n):
    for i in range(0, len(xs), n):
        yield xs[i:i + n]

def get_files(dirs):
    # Try git first
    try:
        cmd = ["git", "ls-files", "-z", "--"] + dirs
        out = subprocess.check_output(cmd, cwd=ROOT, stderr=subprocess.DEVNULL)
        fs = [x.decode() for x in out.split(b"\0") if x]
        return [f for f in fs if not f.startswith(".") and "/." not in f]
    except Exception:
        pass
    
    # Fallback to walk
    fs = []
    for d in dirs:
        dp = ROOT / d
        if not dp.is_dir():
            continue
        for r, ds, ns in os.walk(str(dp)):
            ds[:] = [x for x in ds if not x.startswith(".")]
            for n in ns:
                if n.startswith("."):
                    continue
                p = Path(r) / n
                rel = p.relative_to(ROOT).as_posix()
                if "/." in rel:
                    continue
                fs.append(rel)
    return fs

def normalize_file(path, replace_tabs=True):
    p = ROOT / path
    if not p.exists():
        return 0
    
    try:
        with open(p, "rb") as f:
            b = f.read()
    except OSError:
        return 0

    nb = b.replace(b"\r", b"")
    if replace_tabs:
        nb = nb.replace(b"\t", TAB)
    
    # Remove trailing whitespace
    nb = re.sub(rb"[ \t]+(?=\n|\Z)", b"", nb)
    
    if nb != b:
        with open(p, "wb") as f:
            f.write(nb)
        return 1
    return 0

def run_tidy(dirs=None):
    if dirs is None:
        dirs = ["src", "std", "etc/tests"]
    
    log("TIDY", f"tidying {', '.join(dirs)}")
    
    fs = get_files(dirs)
    if not fs:
        return

    # Run clang-format on C/H files
    c_files = [f for f in fs if f.endswith((".c", ".h"))]
    if c_files and which("clang-format"):
        for batch in chunks(c_files, 256):
            cmd = ["clang-format", "-i"] + [str(ROOT / b) for b in batch]
            subprocess.run(cmd, check=False)
    
    # Text normalization
    changed = 0
    for f in fs:
        ext = os.path.splitext(f)[1]
        if ext in EXTS:
            changed += normalize_file(f, replace_tabs=True)
    
    # Special case for Makefile
    if (ROOT / "Makefile").exists():
        changed += normalize_file("Makefile", replace_tabs=False)
        
    if changed:
        log_ok(f"tidy complete ({changed} files normalized)")
    else:
        log_ok("tidy complete")
