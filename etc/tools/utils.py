import sys
sys.dont_write_bytecode = True
import hashlib
import os
import shutil
import subprocess
import shlex
import time
import re
import signal
from pathlib import Path

from context import c, OK_SYMBOL, host_os

def log(action, msg):
    # Action in vibrant Purple/Magenta (35)
    tag = c("1;35", action)
    print(f"{tag} {msg}", flush=True)

def step(msg):
    # Arrow in vibrant Cyan (36)
    print(f"{c('1;36', '→')} {c('36', msg)}", flush=True)

def log_ok(msg):
    # Checkmark in vibrant Green (32)
    print(f"{c('32', OK_SYMBOL)} {c('32', msg)}", flush=True)

def warn(msg):
    print(f"{c('33', msg)}", flush=True)

def err(msg):
    print(f"{c('31', msg)}", flush=True)

def env_int(name, default=0, minimum=0):
    raw = (os.environ.get(name) or "").strip()
    try:
        v = int(raw)
        return max(v, minimum)
    except (ValueError, TypeError):
        return default

def env_bool(name, default=False):
    raw = (os.environ.get(name) or "").strip().lower()
    if not raw:
        return default
    return raw in ("1", "true", "yes", "on", "y")

def which(cmd):
    return shutil.which(cmd) is not None

def file_sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()

def cmake_path(p):
    if not p:
        return p
    return str(p).replace("\\", "/")

def prepend_path_value(existing, new_path):
    if not new_path:
        return existing
    sep = ";" if host_os() == "windows" else ":"
    cur = existing or ""
    parts = [p for p in cur.split(sep) if p]
    norm_new = os.path.normcase(os.path.normpath(new_path))
    for p in parts:
        if os.path.normcase(os.path.normpath(p)) == norm_new:
            return cur
    return new_path if not cur else (new_path + sep + cur)

def run(cmd, env=None, shell=False, suppress_contains=None, line_filter=None):
    if isinstance(cmd, str):
        if shell:
            try:
                subprocess.check_call(cmd, env=env, shell=True)
                return
            except subprocess.CalledProcessError as e:
                # Handle Ctrl+C (130) or SIGINT (128 + 2)
                if e.returncode in (130, 128 + signal.SIGINT):
                    raise KeyboardInterrupt()
                raise
        cmd = shlex.split(cmd)
    
    # If filtering output, perform manual pipe read loop
    if (suppress_contains or line_filter) and not shell:
        popen_kwargs = {
            "env": env,
            "shell": False,
            "stdout": subprocess.PIPE,
            "stderr": subprocess.STDOUT,
            "text": True,
        }
        if host_os() == "windows":
            popen_kwargs["encoding"] = "utf-8"
            popen_kwargs["errors"] = "replace"
        
        proc = subprocess.Popen(cmd, **popen_kwargs)
        try:
            assert proc.stdout is not None
            for raw in proc.stdout:
                line = raw.rstrip("\n").replace("\r", "")
                if not line:
                    continue
                if suppress_contains and any(tok in line for tok in suppress_contains):
                    continue
                if line_filter:
                    line = line_filter(line)
                    if line is None:
                        continue
                print(line, flush=True)
        except KeyboardInterrupt:
            try:
                proc.terminate()
            except Exception:
                pass
            raise
        rc = proc.wait()
        if rc != 0:
            if host_os() == "windows" and rc == 3221225781:
                warn("Process failed with 0xC0000135 (DLL not found).")
                warn("Ensure LLVM runtime DLLs are available.")
            if rc in (130, 128 + signal.SIGINT):
                raise KeyboardInterrupt()
            raise subprocess.CalledProcessError(rc, cmd)
        return

    # Normal case: just run it
    try:
        subprocess.check_call(cmd, env=env, shell=shell)
    except subprocess.CalledProcessError as e:
        if host_os() == "windows" and e.returncode == 3221225781:
            warn("Process failed with 0xC0000135 (DLL not found).")
            warn("Ensure LLVM runtime DLLs are available.")
        if e.returncode in (130, 128 + signal.SIGINT):
            raise KeyboardInterrupt()
        raise

def run_capture(cmd, env=None, shell=False):
    kwargs = {"env": env, "shell": shell, "capture_output": True, "text": True}
    if host_os() == "windows":
        kwargs["encoding"] = "utf-8"
        kwargs["errors"] = "replace"
    return subprocess.run(cmd, **kwargs)

def write_if_changed(path: Path, text: str) -> bool:
    try:
        if path.read_text(encoding="utf-8") == text:
            return False
    except (FileNotFoundError, IOError):
        pass
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return True

def strip_ansi(text):
    if not text:
        return ""
    import re
    return re.sub(r'\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])', '', text)
def ir_stats(path):
    try: text = Path(path).read_text(encoding="utf-8", errors="ignore")
    except: return {}
    fn = len(re.findall(r"^define\b", text, flags=re.MULTILINE))
    alloca = len(re.findall(r"\balloca\b", text))
    phi = len(re.findall(r"\bphi\b", text))
    inst = 0
    in_fn = False
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("define "): in_fn = True; continue
        if in_fn and line == "}": in_fn = False; continue
        if not in_fn: continue
        if not line or line.startswith(";") or line.endswith(":"): continue
        if raw.startswith("  "): inst += 1
    return {"fn": fn, "inst": inst, "alloca": alloca, "phi": phi}
