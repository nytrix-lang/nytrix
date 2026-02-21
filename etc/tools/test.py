#!/usr/bin/env python3
"""
Nytrix Test Orchestrator
"""
import os, sys, time, glob, re, shlex, argparse, select, subprocess, signal, hashlib, platform, tempfile, json
sys.dont_write_bytecode = True
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor, wait, FIRST_COMPLETED

os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
sys.dont_write_bytecode = True

try:
    import pty
except ImportError:
    pty = None

RESET, GRAY, RED, GREEN, CYAN, YELLOW, BOLD, MAGENTA = "\033[0m", "\033[90m", "\033[31m", "\033[32m", "\033[36m", "\033[33m", "\033[1m", "\033[35m"

from context import (
    host_os, host_machine, is_arm_riscv_machine,
    OK_SYMBOL, c, COLOR_ON, ROOT as CONTEXT_ROOT
)
from utils import (
    env_int, env_bool, strip_ansi, log, log_ok, warn, err
)

IS_WINDOWS = (host_os() == "windows")
IS_MACOS = (host_os() == "macos")
UNICODE_UI = (OK_SYMBOL == "✓")
ROOT = str(CONTEXT_ROOT)

def host_platform_id():
    name = (platform.system() or "").strip().lower()
    if name:
        return name
    return (os.name or "unknown").strip().lower()

def prepend_path(path_value, entry):
    if not entry:
        return path_value or ""
    if not path_value:
        return entry
    parts = path_value.split(os.pathsep)
    norm_entry = os.path.normcase(os.path.normpath(entry))
    for p in parts:
        if os.path.normcase(os.path.normpath(p)) == norm_entry:
            return path_value
    return entry + os.pathsep + path_value

def default_test_cache_root():
    explicit = (os.environ.get("NYTRIX_TEST_CACHE_DIR") or "").strip()
    if explicit:
        return os.path.abspath(os.path.expanduser(explicit))

    if IS_WINDOWS:
        base = (
            (os.environ.get("LOCALAPPDATA") or "").strip()
            or os.path.expanduser(r"~\AppData\Local")
        )
        return os.path.abspath(os.path.join(base, "nytrix", "test-cache"))

    if IS_MACOS:
        base = os.path.expanduser("~/Library/Caches")
        return os.path.abspath(os.path.join(base, "nytrix", "test-cache"))

    xdg = (os.environ.get("XDG_CACHE_HOME") or "").strip()
    base = xdg if xdg else os.path.expanduser("~/.cache")
    return os.path.abspath(os.path.join(base, "nytrix", "test-cache"))

def resolve_test_cache_root():
    candidates = [default_test_cache_root()]
    tmp_fallback = os.path.abspath(os.path.join(tempfile.gettempdir(), "nytrix-test-cache"))
    if tmp_fallback not in candidates:
        candidates.append(tmp_fallback)
    for cand in candidates:
        try:
            os.makedirs(cand, exist_ok=True)
            probe = os.path.join(cand, ".write_probe")
            with open(probe, "w", encoding="utf-8") as f:
                f.write("ok")
            os.remove(probe)
            return cand
        except OSError:
            continue
    return candidates[0]

SECTION_RULE_WIDTH = 54
TEST_CACHE_ROOT = resolve_test_cache_root()
TIMINGS_DB = os.path.join(TEST_CACHE_ROOT, "test_timings.json")
RESULTS_DB = os.path.join(TEST_CACHE_ROOT, "test_results.json")
NATIVE_CACHE_DIR = os.path.join(TEST_CACHE_ROOT, "native")
RESULTS_CACHE_REV = "tcache-v2"
_FILE_DIGEST_CACHE = {}
_STDLIB_SIG_CACHE = None
_COMPILER_SIG_CACHE = None
BENCH_COST_HINTS = {
    "sieve.ny": 5.0, "mandelbrot.ny": 4.0, "float.ny": 3.0, "spectral.ny": 2.5,
    "binary.ny": 2.0, "fibonacci.ny": 1.5, "dict.ny": 1.0, "list.ny": 0.8,
}
RUNTIME_COST_HINTS = {
    "strings.ny": 1.5, "parser.ny": 1.4, "comptime.ny": 1.3, "control.ny": 1.2,
}
STD_COST_HINTS = {
    "time.ny": 6.0, "zlib.ny": 5.0, "socket.ny": 4.5, "core.ny": 4.0,
    "process.ny": 3.5, "sys.ny": 3.2, "io.ny": 3.0, "requests.ny": 2.6,
    "http.ny": 2.4, "bigint.ny": 2.0,
}
THREAD_STRESS_DEFAULT = os.path.join(ROOT, "etc", "tests", "std", "os", "thread.ny")

SUITE_BENCHMARK = "benchmark"
SUITE_RUNTIME = "runtime"
SUITE_STD = "std"

def normalize_suite_key(name):
    n = (name or "").strip().lower()
    if n in ("bench", "benchmark"):
        return SUITE_BENCHMARK
    if n == SUITE_RUNTIME:
        return SUITE_RUNTIME
    if n == SUITE_STD:
        return SUITE_STD
    return n

def apply_real_test_mode():
    if not env_bool("NYTRIX_TEST_REAL", default=False):
        return False
    # Disable all result/native/compiler caches for a true cold-path test run.
    os.environ["NYTRIX_TEST_CACHE"] = "0"
    os.environ["NYTRIX_TEST_NO_NATIVE_CACHE"] = "1"
    os.environ["NYTRIX_JIT_CACHE"] = "0"
    os.environ["NYTRIX_AOT_CACHE"] = "0"
    os.environ["NYTRIX_STD_CACHE"] = "0"
    return True

def print_test_mode_banner(real_mode, use_result_cache):
    native_cache = use_result_cache and not env_bool("NYTRIX_TEST_NO_NATIVE_CACHE", default=False)
    jit_cache = env_bool("NYTRIX_JIT_CACHE", default=True)
    aot_cache = env_bool("NYTRIX_AOT_CACHE", default=True)
    std_cache = env_bool("NYTRIX_STD_CACHE", default=True)
    mode = "real" if real_mode else "default"
    print(
        f"{GRAY}[mode]{RESET} "
        f"{mode} "
        f"result_cache={'on' if use_result_cache else 'off'} "
        f"native_cache={'on' if native_cache else 'off'} "
        f"jit_cache={'on' if jit_cache else 'off'} "
        f"aot_cache={'on' if aot_cache else 'off'} "
        f"std_cache={'on' if std_cache else 'off'}"
    )

def has_llvm_core(include_root):
    if not include_root:
        return False
    return os.path.exists(os.path.join(include_root, "llvm-c", "Core.h"))

def extract_windows_cc_path(raw):
    if not raw:
        return ""
    s = raw.strip()
    if not s:
        return ""
    # Exact path (possibly quoted)
    exact = s.strip('"').strip("'")
    if exact and os.path.exists(exact):
        return os.path.abspath(exact)
    # Tokenized command (e.g. "C:\Program Files\LLVM\bin\clang.exe" -O2)
    try:
        for tok in shlex.split(s, posix=False):
            cand = tok.strip('"').strip("'")
            if cand and cand.lower().endswith(".exe") and os.path.exists(cand):
                return os.path.abspath(cand)
    except ValueError:
        pass
    # Fallback: capture any plausible absolute .exe path from raw text.
    for m in re.finditer(r'([A-Za-z]:[\\/].*?\.exe)', s, re.IGNORECASE):
        cand = m.group(1).strip().strip('"').strip("'")
        if os.path.exists(cand):
            return os.path.abspath(cand)
    return ""

def find_windows_clang():
    env_cc = os.environ.get("NYTRIX_CC") or os.environ.get("CC") or ""
    parsed = extract_windows_cc_path(env_cc)
    if parsed:
        return parsed
    for c in (
        r"C:\PROGRA~1\LLVM\bin\clang.exe",
        r"C:\PROGRA~2\LLVM\bin\clang.exe",
        r"C:\PROGRA~1\LLVM\bin\clang-cl.exe",
        r"C:\PROGRA~2\LLVM\bin\clang-cl.exe",
        r"C:\Program Files\LLVM\bin\clang.exe",
        r"C:\Program Files (x86)\LLVM\bin\clang.exe",
        r"C:\Program Files\LLVM\bin\clang-cl.exe",
        r"C:\Program Files (x86)\LLVM\bin\clang-cl.exe",
    ):
        if os.path.exists(c):
            return c
    return None

WINDOWS_LOCAL_LLVM_INC = ""
WINDOWS_CLANG = None
if IS_WINDOWS:
    _local_inc = os.path.join(ROOT, "build", "third_party", "llvm", "headers", "include")
    if has_llvm_core(_local_inc):
        WINDOWS_LOCAL_LLVM_INC = _local_inc
    WINDOWS_CLANG = find_windows_clang()

def default_timeout_seconds():
    env_timeout = os.environ.get("NYTRIX_TEST_TIMEOUT")
    if env_timeout:
        try:
            parsed = int(env_timeout)
            if parsed > 0:
                return parsed
        except ValueError:
            pass
    if IS_WINDOWS:
        return 60
    if is_arm_riscv_machine():
        return 180
    return 60

PROC_TIMEOUT = default_timeout_seconds()

def default_repl_prompt_timeout():
    raw = (os.environ.get("NYTRIX_TEST_REPL_TIMEOUT") or "").strip()
    if raw:
        try:
            val = int(raw)
            if val > 0:
                return val
        except ValueError:
            pass
    if is_arm_riscv_machine():
        return 25
    if IS_WINDOWS:
        return 12
    return 8

REPL_PROMPT_TIMEOUT = default_repl_prompt_timeout()

def kill_windows_process_tree(pid):
    if not IS_WINDOWS:
        return
    try:
        subprocess.run(
            ["taskkill", "/PID", str(pid), "/T", "/F"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    except Exception:
        pass

def windows_bench_jobs(default_jobs):
    raw = os.environ.get("NYTRIX_BENCH_JOBS", "").strip()
    if raw:
        try:
            val = int(raw)
            if val > 0:
                return val
        except ValueError:
            pass
    # On Windows, keep bench parallelism moderate; each bench runs AOT+REPL+Native phases.
    # Spawning many heavy processes on Windows is expensive; scale with host size.
    logical = os.cpu_count() or 1
    ram_bytes = _read_ram_total_bytes()
    ram_gib = float(ram_bytes) / float(1024 ** 3) if ram_bytes > 0 else 0.0
    cap = 2
    if logical >= 24 and ram_gib >= 24.0:
        cap = 3
    elif logical <= 8 or (ram_gib > 0.0 and ram_gib < 8.0):
        cap = 1
    return max(1, min(default_jobs, cap))

def bench_jobs(default_jobs):
    if IS_WINDOWS:
        return windows_bench_jobs(default_jobs)
    raw = os.environ.get("NYTRIX_BENCH_JOBS", "").strip()
    if raw:
        try:
            val = int(raw)
            if val > 0:
                return val
        except ValueError:
            pass
    # Keep bench workers moderate to avoid subprocess thrash from running
    # compile + repl + native phases in parallel.
    logical = os.cpu_count() or 1
    cap = 8 if logical >= 16 else max(1, logical // 2)
    return max(1, min(default_jobs, cap))

def detect_physical_cores(logical=None):
    logical = logical or (os.cpu_count() or 1)
    try:
        if not IS_WINDOWS and os.path.exists("/proc/cpuinfo"):
            seen = set()
            phys = None
            core = None
            with open("/proc/cpuinfo", "r", encoding="utf-8", errors="ignore") as f:
                for line in f:
                    if line.startswith("physical id"):
                        phys = line.split(":", 1)[1].strip()
                    elif line.startswith("core id"):
                        core = line.split(":", 1)[1].strip()
                    elif line.strip() == "":
                        if phys is not None and core is not None:
                            seen.add((phys, core))
                        phys = None
                        core = None
            if seen:
                return max(1, len(seen))
            # Many ARM/RISC-V Linux boards don't expose physical/core ids in
            # /proc/cpuinfo. Treat logical cores as physical in that case.
            if is_arm_riscv_machine():
                return logical
        if platform.system() == "Darwin":
            out = subprocess.check_output(["sysctl", "-n", "hw.physicalcpu"], text=True).strip()
            if out.isdigit():
                return max(1, int(out))
    except Exception:
        pass
    if IS_WINDOWS:
        return max(1, logical // 2) if logical >= 4 else logical
    return max(1, logical // 2) if logical >= 4 else logical

def smt_factor(logical, physical):
    if physical <= 0:
        return 1.0
    return max(1.0, float(logical) / float(max(1, physical)))

def default_test_jobs_soft_cap(logical):
    # Keep a soft ceiling to avoid subprocess thrash, but allow high-core
    # hosts to scale test throughput far beyond older fixed worker caps.
    if logical >= 64:
        return max(24, int(logical * 0.75))
    if logical >= 48:
        return 40
    if logical >= 32:
        return 32
    if logical >= 16:
        return 24
    return max(1, logical)

def auto_threads(profile, logical, physical, kind):
    p = (profile or "auto").strip().lower()
    if p in ("default", "balanced"):
        p = "auto"
    if p in ("off", "single", "1"):
        return 1
    if p in ("aggressive", "max"):
        return logical
    if p in ("conservative", "safe"):
        if kind == "test":
            if IS_WINDOWS:
                return max(1, min(4, max(1, physical // 2)))
            return max(1, min(16, max(1, physical // 2)))
        return max(1, min(physical, logical // 2 if logical >= 8 else logical))
    if p == "smt":
        if kind == "test":
            if IS_WINDOWS:
                return max(1, min(6, max(1, logical // 4 if logical >= 8 else logical // 2)))
            return max(1, min(24, int(logical * 0.6)))
        return max(1, min(logical, int(logical * 0.85)))
    ratio = smt_factor(logical, physical)
    if kind == "test":
        if IS_WINDOWS:
            base = max(1, physical // 2 if logical >= 12 else physical)
            return max(1, min(6, base))
        # Keep tests fast by default: use almost all workers, but leave room
        # for the scheduler and SMT siblings.
        if logical <= 4:
            # Small non-SMT hosts (e.g. 4-core SBCs) usually run faster at full
            # utilization; SMT-heavy 4-thread hosts still keep one thread free.
            jobs = logical if ratio <= 1.2 else max(1, logical - 1)
        elif ratio >= 1.8:
            reserve = max(1, logical // 8)
            jobs = logical - reserve
            # On heavy SMT hosts, avoid saturating both siblings of most cores.
            smt_budget = physical + max(1, physical // 2)
            jobs = min(jobs, smt_budget)
        else:
            reserve = 1 if logical > 2 else 0
            jobs = logical - reserve
        # Keep a practical ceiling while still scaling on ultramulticore hosts.
        jobs = min(jobs, default_test_jobs_soft_cap(logical))
        # Small Linux ARM/RISC-V boards tend to become unstable under heavy
        # multi-process test pressure. Keep defaults conservative there.
        if host_os() == "linux" and is_arm_riscv_machine():
            # Favor throughput on common 4-core SBCs; callers can lower via env.
            arm_cap_default = min(logical, 4)
            arm_cap = env_int("NYTRIX_TEST_ARM_JOBS_CAP", default=arm_cap_default, minimum=1)
            jobs = min(jobs, arm_cap)
        # Keep low-power multi-core ARM/RISC-V boards from falling to a
        # single test worker due to conservative SMT heuristics.
        if jobs < 2 and logical >= 4 and is_arm_riscv_machine():
            jobs = 2
        return max(1, min(logical, jobs))
    jobs = int(physical * 1.5) if ratio >= 1.8 else int(physical * 1.1)
    return max(1, min(logical, max(physical, jobs)))

def recommended_jobs(logical):
    physical = detect_physical_cores(logical=logical)
    profile = (os.environ.get("NYTRIX_AUTO_THREADS") or "auto").strip().lower()
    jobs = auto_threads(profile, logical, physical, "test")
    auto_cap = env_int("NYTRIX_TEST_AUTO_JOBS_CAP", default=0, minimum=0)
    if auto_cap > 0:
        jobs = min(jobs, auto_cap)
    return jobs

def effective_jobs(requested):
    logical = os.cpu_count() or 1
    recommended = recommended_jobs(logical)
    if requested and requested > 0:
        # Allow explicit override for stress testing.
        if env_bool("NYTRIX_TEST_FORCE_JOBS", default=False):
            return requested
        return min(requested, recommended)
    return recommended

def bench_cost_hint(path):
    return BENCH_COST_HINTS.get(os.path.basename(path).lower(), 0.0)

def suite_cost_hint(suite_key, path):
    suite_key = normalize_suite_key(suite_key)
    base = os.path.basename(path).lower()
    if suite_key == SUITE_BENCHMARK:
        return BENCH_COST_HINTS.get(base, 0.0)
    if suite_key == SUITE_RUNTIME:
        return RUNTIME_COST_HINTS.get(base, 0.0)
    if suite_key == SUITE_STD:
        return STD_COST_HINTS.get(base, 0.0)
    return 0.0

def find_ny_bin(preferred="build/ny"):
    cands = [preferred + "_debug", preferred, "./build/ny_debug", "./build/ny", "ny"]
    if IS_WINDOWS:
        extra = []
        for p in cands:
            if not p.lower().endswith(".exe"):
                extra.append(p + ".exe")
        cands = extra + cands
    for p in cands:
        if os.path.exists(p):
            return os.path.abspath(p)
    raise FileNotFoundError("Nytrix binary not found")

def sanitize_dynamic_loader_env(run_env):
    # Keep tests deterministic by default: host-level preloads can interfere
    # with sanitizer startup and runtime behavior.
    if not env_bool("NYTRIX_TEST_PRESERVE_PRELOAD", default=False):
        run_env.pop("LD_PRELOAD", None)
        run_env.pop("DYLD_INSERT_LIBRARIES", None)
    if "NY_TEST_PRELOAD" in run_env:
        preload = (run_env.get("NY_TEST_PRELOAD") or "").strip()
        if preload:
            run_env["LD_PRELOAD"] = preload
        else:
            run_env.pop("LD_PRELOAD", None)

def run_proc(cmd, input_str=None, cwd=None, env=None, timeout=PROC_TIMEOUT):
    start = time.time()
    run_env = env or os.environ.copy()
    sanitize_dynamic_loader_env(run_env)
    if IS_WINDOWS:
        run_env.setdefault("PYTHONUTF8", "1")
        run_env.setdefault("PYTHONIOENCODING", "utf-8")
        if not has_llvm_core(run_env.get("NYTRIX_LLVM_HEADERS")):
            if WINDOWS_LOCAL_LLVM_INC:
                run_env["NYTRIX_LLVM_HEADERS"] = WINDOWS_LOCAL_LLVM_INC
                run_env.setdefault("NYTRIX_LLVM_INCLUDE", WINDOWS_LOCAL_LLVM_INC)
        cc_raw = run_env.get("NYTRIX_CC") or run_env.get("CC") or ""
        cc_path = extract_windows_cc_path(cc_raw)
        if WINDOWS_CLANG and ((not cc_path) or ("windowsapps" in cc_raw.lower()) or ("program files" in cc_path.lower())):
            cc_path = WINDOWS_CLANG
        if cc_path:
            run_env["NYTRIX_CC"] = cc_path
            run_env["PATH"] = prepend_path(run_env.get("PATH", ""), os.path.dirname(cc_path))
        if run_env.get("NYTRIX_CC"):
            run_env["CC"] = run_env["NYTRIX_CC"]
    if IS_WINDOWS:
        proc = None
        try:
            proc = subprocess.Popen(
                cmd,
                stdin=subprocess.PIPE if input_str is not None else None,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                env=run_env,
                cwd=cwd,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
            out, err = proc.communicate(input=input_str, timeout=timeout)
            return {
                "passed": proc.returncode == 0,
                "duration": time.time() - start,
                "stdout": out or "",
                "stderr": err or "",
                "code": proc.returncode,
            }
        except subprocess.TimeoutExpired as e:
            out = e.stdout if isinstance(e.stdout, str) else ""
            err = e.stderr if isinstance(e.stderr, str) else ""
            if proc is not None:
                kill_windows_process_tree(proc.pid)
                try:
                    out2, err2 = proc.communicate(timeout=5)
                    out += out2 or ""
                    err += err2 or ""
                except Exception:
                    try:
                        proc.kill()
                    except Exception:
                        pass
            return {
                "passed": False,
                "duration": time.time() - start,
                "stdout": out,
                "stderr": err,
                "error": f"timeout after {timeout}s",
                "code": -1,
            }
        except Exception as e:
            if proc is not None:
                try:
                    kill_windows_process_tree(proc.pid)
                except Exception:
                    pass
            return {"passed": False, "duration": time.time() - start, "stdout": "", "stderr": "", "error": str(e), "code": -1}

    try:
        res = subprocess.run(
            cmd,
            input=input_str,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            env=run_env,
            cwd=cwd,
        )
        return {"passed": res.returncode == 0, "duration": time.time() - start, "stdout": res.stdout, "stderr": res.stderr, "code": res.returncode}
    except subprocess.TimeoutExpired as e:
        out = e.stdout if isinstance(e.stdout, str) else ""
        err = e.stderr if isinstance(e.stderr, str) else ""
        return {"passed": False, "duration": time.time() - start, "stdout": out, "stderr": err, "error": f"timeout after {timeout}s", "code": -1}
    except Exception as e:
        return {"passed": False, "duration": time.time() - start, "stdout": "", "stderr": "", "error": str(e), "code": -1}

class ReplSession:
    def __init__(self, bin_path):
        if pty is None:
            raise RuntimeError("pty is not available on this platform")
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            env = os.environ.copy()
            sanitize_dynamic_loader_env(env)
            env.update(
                {
                    "ASAN_OPTIONS": "detect_leaks=0",
                    "TERM": "xterm-256color",
                    # Keep smoke deterministic and avoid inheriting stricter
                    # policy envs from developer shells.
                    "NYTRIX_EFFECT_REQUIRE_KNOWN": "0",
                    "NYTRIX_ALIAS_REQUIRE_KNOWN": "0",
                    "NYTRIX_ALIAS_REQUIRE_NO_ESCAPE": "0",
                }
            )
            os.execvpe(
                bin_path,
                [
                    bin_path,
                    "-i",
                    "--no-effect-require-known",
                    "--no-alias-require-known",
                ],
                env,
            )
    def read_until(self, rx, timeout=REPL_PROMPT_TIMEOUT):
        buf, end = "", time.time() + timeout
        while time.time() < end:
            if self.fd in select.select([self.fd], [], [], 0.1)[0]:
                try:
                    data = os.read(self.fd, 8192).decode(errors="ignore")
                    if not data: break
                    buf += data
                except OSError: break
                if re.search(rx, strip_ansi(buf)): return buf
        raise TimeoutError(f"Timeout waiting for {rx}")
    def send(self, s): os.write(self.fd, (s + "\n").encode())
    def close(self):
        for s in (signal.SIGTERM, signal.SIGKILL):
            try: os.kill(self.pid, s); break
            except OSError: pass
        try: os.close(self.fd)
        except: pass

def disp_path(path):
    return path.replace("\\", "/")

def shorten(path):
    p = disp_path(path)
    root = disp_path(ROOT).rstrip("/") + "/"
    if p.startswith(root):
        p = p[len(root):]
    p = p.replace("etc/tests/", "").replace("test/", "")
    return (p[:20] + "..." + p[-27:]) if len(p) > 50 else p

def mtime_or_zero(path):
    try:
        return os.path.getmtime(path)
    except OSError:
        return 0.0

def native_cache_fresh(src_path, bin_path, exe_path):
    if not env_bool("NYTRIX_TEST_CACHE", default=True):
        return False
    if env_bool("NYTRIX_TEST_NO_NATIVE_CACHE", default=False):
        return False
    if not os.path.exists(exe_path):
        return False
    try:
        return os.path.getsize(exe_path) > 0
    except OSError:
        return False

def file_content_sig(path):
    if not path:
        return ""
    p = os.path.abspath(path)
    try:
        st = os.stat(p)
    except OSError:
        return f"{p}:0:0"
    key = (p, int(getattr(st, "st_mtime_ns", int(st.st_mtime * 1_000_000_000))), int(st.st_size))
    cached = _FILE_DIGEST_CACHE.get(key)
    if cached is not None:
        return cached
    h = hashlib.sha1()
    try:
        with open(p, "rb") as f:
            while True:
                chunk = f.read(1 << 20)
                if not chunk:
                    break
                h.update(chunk)
        sig = f"{p}:{st.st_size}:{h.hexdigest()}"
    except OSError:
        sig = f"{p}:0:0"
    _FILE_DIGEST_CACHE[key] = sig
    return sig

def compiler_source_sig():
    global _COMPILER_SIG_CACHE
    if _COMPILER_SIG_CACHE is not None:
        return _COMPILER_SIG_CACHE

    src_root = os.path.join(ROOT, "src")
    h = hashlib.sha1()
    count = 0

    if os.path.isdir(src_root):
        for dirpath, dirnames, filenames in os.walk(src_root):
            dirnames.sort()
            for fn in sorted(filenames):
                if not fn.endswith((".c", ".h", ".inc", ".def")):
                    continue
                path = os.path.join(dirpath, fn)
                rel = os.path.relpath(path, ROOT).replace("\\", "/")
                try:
                    st = os.stat(path)
                except OSError:
                    continue
                mtime_ns = int(getattr(st, "st_mtime_ns", int(st.st_mtime * 1_000_000_000)))
                h.update(f"{rel}:{mtime_ns}:{int(st.st_size)}|".encode("utf-8", "ignore"))
                count += 1

    cmake_root = os.path.join(ROOT, "CMakeLists.txt")
    try:
        st = os.stat(cmake_root)
        mtime_ns = int(getattr(st, "st_mtime_ns", int(st.st_mtime * 1_000_000_000)))
        h.update(f"CMakeLists.txt:{mtime_ns}:{int(st.st_size)}|".encode("utf-8", "ignore"))
        count += 1
    except OSError:
        pass

    _COMPILER_SIG_CACHE = f"{count}:{h.hexdigest()}" if count > 0 else "missing"
    return _COMPILER_SIG_CACHE

def compiler_cache_sig(bin_path):
    mode = (os.environ.get("NYTRIX_TEST_COMPILER_SIG") or "source").strip().lower()
    if mode in ("binary", "strict"):
        return file_content_sig(bin_path)
    src_sig = compiler_source_sig()
    if src_sig != "missing":
        return src_sig
    return file_content_sig(bin_path)

def stdlib_source_sig():
    global _STDLIB_SIG_CACHE
    if _STDLIB_SIG_CACHE is not None:
        return _STDLIB_SIG_CACHE
    std_root = os.path.join(ROOT, "std")
    if not os.path.isdir(std_root):
        _STDLIB_SIG_CACHE = "missing"
        return _STDLIB_SIG_CACHE
    h = hashlib.sha1()
    count = 0
    for dirpath, dirnames, filenames in os.walk(std_root):
        dirnames.sort()
        for fn in sorted(filenames):
            if not fn.endswith(".ny"):
                continue
            path = os.path.join(dirpath, fn)
            rel = os.path.relpath(path, std_root).replace("\\", "/")
            try:
                st = os.stat(path)
            except OSError:
                continue
            mtime_ns = int(getattr(st, "st_mtime_ns", int(st.st_mtime * 1_000_000_000)))
            h.update(f"{rel}:{mtime_ns}:{int(st.st_size)}|".encode("utf-8", "ignore"))
            count += 1
    _STDLIB_SIG_CACHE = f"{count}:{h.hexdigest()}"
    return _STDLIB_SIG_CACHE

def native_cache_key(src_path, bin_path, opt_profile, ny_flags, do_repl, do_native, env=None):
    env = env or os.environ
    std_prebuilt = env.get("NYTRIX_STD_PREBUILT", "")
    std_build = env.get("NYTRIX_BUILD_STD_PATH", "")
    flags_blob = " ".join(ny_flags) if ny_flags else ""
    parts = [
        RESULTS_CACHE_REV,
        f"platform={host_platform_id()}",
        f"os={os.name}",
        f"arch={host_machine()}",
        f"repl={1 if do_repl else 0}",
        f"native={1 if do_native else 0}",
        file_sig(__file__),
        file_sig(src_path),
        f"compiler={compiler_cache_sig(bin_path)}",
        f"opt={opt_profile or ''}",
        f"flags={flags_blob}",
        f"host_cflags={env.get('NYTRIX_HOST_CFLAGS', '')}",
        f"host_ldflags={env.get('NYTRIX_HOST_LDFLAGS', '')}",
        f"arm_float_abi={env.get('NYTRIX_ARM_FLOAT_ABI', '')}",
        f"host_triple={env.get('NYTRIX_HOST_TRIPLE', '')}",
        f"std_tree={stdlib_source_sig()}",
        f"std_prebuilt={file_sig(std_prebuilt) if std_prebuilt else ''}",
        f"std_build={file_sig(std_build) if std_build else ''}",
    ]
    blob = "|".join(parts).encode("utf-8", "ignore")
    return hashlib.sha1(blob).hexdigest()[:16]

def ensure_arm_hardfloat_env(env):
    if not env or IS_WINDOWS:
        return
    mach = (host_machine() or "").lower()
    arm32 = ("arm" in mach) and ("aarch64" not in mach) and ("arm64" not in mach)
    if not arm32:
        return

    env.setdefault("NYTRIX_ARM_FLOAT_ABI", "hard")

    cflags = (env.get("NYTRIX_HOST_CFLAGS") or "").strip()
    ldflags = (env.get("NYTRIX_HOST_LDFLAGS") or "").strip()

    if "-mfloat-abi=" not in cflags:
        cflags = (cflags + " -mfloat-abi=hard").strip()
    if "-mfloat-abi=" not in ldflags:
        ldflags = (ldflags + " -mfloat-abi=hard").strip()

    if "-mfpu=" not in cflags:
        if "armv6" in mach:
            cflags = (cflags + " -mfpu=vfp -march=armv6").strip()
        elif "armv7" in mach:
            cflags = (cflags + " -mfpu=vfpv3 -march=armv7-a").strip()
        else:
            cflags = (cflags + " -mfpu=vfpv3").strip()
    if "-mfpu=" not in ldflags:
        if "armv6" in mach:
            ldflags = (ldflags + " -mfpu=vfp").strip()
        elif "armv7" in mach:
            ldflags = (ldflags + " -mfpu=vfpv3").strip()
        else:
            ldflags = (ldflags + " -mfpu=vfpv3").strip()

    env["NYTRIX_HOST_CFLAGS"] = cflags
    env["NYTRIX_HOST_LDFLAGS"] = ldflags

    if "NYTRIX_HOST_TRIPLE" not in env:
        if "armv6" in mach:
            env["NYTRIX_HOST_TRIPLE"] = "armv6-unknown-linux-gnueabihf"
        else:
            env["NYTRIX_HOST_TRIPLE"] = "armv7-unknown-linux-gnueabihf"

def load_timings_db():
    try:
        with open(TIMINGS_DB, "r", encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, dict):
            return data
    except Exception:
        pass
    return {}

def save_timings_db(db):
    try:
        os.makedirs(os.path.dirname(TIMINGS_DB), exist_ok=True)
        with open(TIMINGS_DB, "w", encoding="utf-8") as f:
            json.dump(db, f, separators=(",", ":"), sort_keys=True)
    except Exception:
        pass

def load_results_db():
    try:
        with open(RESULTS_DB, "r", encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, dict):
            return data
    except Exception:
        pass
    return {}

def save_results_db(db):
    try:
        os.makedirs(os.path.dirname(RESULTS_DB), exist_ok=True)
        with open(RESULTS_DB, "w", encoding="utf-8") as f:
            json.dump(db, f, separators=(",", ":"), sort_keys=True)
    except Exception:
        pass

def file_sig(path):
    return file_content_sig(path)

def bench_path(path):
    p = disp_path(path)
    return (
        "/benchmark/" in p
        or p.startswith("etc/tests/benchmark/")
        or "/bench/" in p
        or p.startswith("std/bench/")
    )

def runtime_path(path):
    p = disp_path(path)
    return "/runtime/" in p or p.startswith("std/runtime/")

def std_path(path):
    p = disp_path(path)
    return "/std/" in p or p.startswith("std/") or p.startswith("etc/tests/std/")

def default_bench_opt_profile():
    # Bench defaults are tuned for wall-clock test throughput:
    # - desktop/server x86: none is usually fastest for end-to-end suite time
    # - constrained/Windows hosts: stronger optimization can still win
    if IS_WINDOWS or is_arm_riscv_machine():
        return "speed"
    return "none"

def default_bench_repl_phase():
    # On small ARM boards, benchmark REPL phase dominates wall time and adds
    # little signal beyond runtime/std REPL coverage.
    if low_memory_arm_host():
        return False
    return True

def default_runtime_repl_phase():
    # On small ARM boards, runtime REPL phase can dominate wall time.
    if low_memory_arm_host():
        return False
    return True

def default_runtime_opt_profile():
    # Runtime suite focuses on correctness. Favor compile throughput.
    return "none"

def default_std_opt_profile():
    # Std suite is large and compile-heavy. Favor compile throughput.
    return "none"

def low_memory_arm_host():
    if host_os() != "linux" or not is_arm_riscv_machine():
        return False
    total = _read_ram_total_bytes()
    if total <= 0:
        return False
    return total <= int(1.5 * (1024 ** 3))

def default_std_repl_phase():
    # On small ARM boards, std REPL phase dominates wall time while runtime
    # suite already exercises REPL behavior.
    if low_memory_arm_host():
        return False
    return True

def default_bench_native_phase():
    # On small ARM boards, native benchmark runs are expensive and mostly
    # duplicate JIT correctness signal.
    if low_memory_arm_host():
        return False
    return True

def native_phase_enabled(path):
    global_native = env_bool("NYTRIX_TEST_NATIVE", default=True)
    if not global_native:
        return False
    if bench_path(path):
        if "NYTRIX_TEST_BENCHMARK_NATIVE" in os.environ:
            return env_bool("NYTRIX_TEST_BENCHMARK_NATIVE", default=default_bench_native_phase())
        return default_bench_native_phase()
    if runtime_path(path):
        if "NYTRIX_TEST_RUNTIME_NATIVE" in os.environ:
            return env_bool("NYTRIX_TEST_RUNTIME_NATIVE", default=True)
        return True
    if std_path(path):
        if "NYTRIX_TEST_STD_NATIVE" in os.environ:
            return env_bool("NYTRIX_TEST_STD_NATIVE", default=True)
        return True
    return True

def thread_stress_settings(pattern):
    if not env_bool("NYTRIX_THREAD_STRESS", default=False):
        return None
    raw_path = (os.environ.get("NYTRIX_THREAD_STRESS_FILE") or THREAD_STRESS_DEFAULT).strip()
    if not raw_path:
        raw_path = THREAD_STRESS_DEFAULT
    path = os.path.abspath(raw_path) # Changed here
    if not pattern_matches(path, pattern):
        return None
    return {
        "path": path,
        "iters": env_int("NYTRIX_THREAD_STRESS_ITERS", default=1, minimum=1),
        "repl": env_bool("NYTRIX_THREAD_STRESS_REPL", default=False),
    }

def pattern_matches(path, patterns):
    if not patterns:
        return True
    for pat in patterns:
        try:
            if re.search(pat, path):
                return True
        except re.error:
            continue
    return False

def effective_opt_profile_for_path(path):
    profile = (os.environ.get("NYTRIX_OPT_PROFILE") or "").strip().lower()
    if bench_path(path):
        bench_profile = (os.environ.get("NYTRIX_TEST_BENCH_OPT_PROFILE") or "").strip().lower()
        if bench_profile:
            profile = bench_profile
        elif not profile:
            default_bench = default_bench_opt_profile()
            if default_bench:
                profile = default_bench
    elif runtime_path(path):
        runtime_profile = (os.environ.get("NYTRIX_TEST_RUNTIME_OPT_PROFILE") or "").strip().lower()
        if runtime_profile:
            profile = runtime_profile
        elif not profile:
            profile = default_runtime_opt_profile()
    elif std_path(path):
        std_profile = (os.environ.get("NYTRIX_TEST_STD_OPT_PROFILE") or "").strip().lower()
        if std_profile:
            profile = std_profile
        elif not profile:
            profile = default_std_opt_profile()
    return profile

def suite_repl_enabled(suite_name, default_enabled):
    env_name = f"NYTRIX_TEST_{suite_name.upper()}_REPL"
    if env_name in os.environ:
        return env_bool(env_name, default=default_enabled)
    return default_enabled

def repl_phase_enabled_for_path(path, suite_repl_enabled):
    if not suite_repl_enabled:
        return False
    return True

def test_sig(src_path, bin_path, do_repl, opt_profile="", do_native=True):
    parts = [
        RESULTS_CACHE_REV,
        f"platform={host_platform_id()}",
        f"os={os.name}",
        f"arch={host_machine()}",
        f"repl={1 if do_repl else 0}",
        f"native={1 if do_native else 0}",
        file_sig(__file__),
        file_sig(src_path),
        f"compiler={compiler_cache_sig(bin_path)}",
    ]
    std_prebuilt = os.environ.get("NYTRIX_STD_PREBUILT")
    std_build = os.environ.get("NYTRIX_BUILD_STD_PATH")
    if std_prebuilt:
        parts.append(file_sig(std_prebuilt))
    if std_build and std_build != std_prebuilt:
        parts.append(file_sig(std_build))
    extra_flags = (os.environ.get("NYTRIX_TEST_FLAGS") or "").strip()
    if extra_flags:
        parts.append(f"flags={extra_flags}")
    parts.append(f"host_cflags={os.environ.get('NYTRIX_HOST_CFLAGS', '')}")
    parts.append(f"host_ldflags={os.environ.get('NYTRIX_HOST_LDFLAGS', '')}")
    parts.append(f"arm_float_abi={os.environ.get('NYTRIX_ARM_FLOAT_ABI', '')}")
    parts.append(f"host_triple={os.environ.get('NYTRIX_HOST_TRIPLE', '')}")
    if opt_profile:
        parts.append(f"opt={opt_profile}")
    blob = "|".join(parts).encode("utf-8", "ignore")
    return hashlib.sha1(blob).hexdigest()

def pack_phase(res):
    if not isinstance(res, dict):
        return None
    try:
        code = int(res.get("code", 0))
    except Exception:
        code = 0
    out = {
        "passed": bool(res.get("passed", False)),
        "duration": float(res.get("duration", 0.0)),
        "code": code,
        "skipped": bool(res.get("skipped", False)),
    }
    if "compile_dur" in res:
        out["compile_dur"] = float(res.get("compile_dur", 0.0))
    if "run_dur" in res:
        out["run_dur"] = float(res.get("run_dur", 0.0))
    return out

def unpack_phase(phase):
    if not isinstance(phase, dict):
        return {
            "passed": False,
            "duration": 0.0,
            "stdout": "",
            "stderr": "",
            "code": -1,
            "skipped": False,
        }
    try:
        code = int(phase.get("code", 0))
    except Exception:
        code = 0
    out = {
        "passed": bool(phase.get("passed", False)),
        "duration": float(phase.get("duration", 0.0)),
        "stdout": "",
        "stderr": "",
        "code": code,
        "skipped": bool(phase.get("skipped", False)),
    }
    if "compile_dur" in phase:
        out["compile_dur"] = float(phase.get("compile_dur", 0.0))
    if "run_dur" in phase:
        out["run_dur"] = float(phase.get("run_dur", 0.0))
    return out

def total_duration(aot, repl, elf):
    return float(aot.get("duration", 0.0) + ((repl or {}).get("duration", 0.0) if repl else 0.0) + elf.get("duration", 0.0))

def duration_ms(seconds):
    try:
        return int(round(float(seconds) * 1000.0))
    except Exception:
        return 0

def phase_breakdown_ms(aot, repl, elf):
    aot_ms = duration_ms((aot or {}).get("duration", 0.0))
    repl_ms = duration_ms((repl or {}).get("duration", 0.0)) if repl else 0
    elf_ms = duration_ms((elf or {}).get("duration", 0.0))
    elf_compile_ms = duration_ms((elf or {}).get("compile_dur", 0.0))
    elf_run_ms = duration_ms((elf or {}).get("run_dur", 0.0))
    if elf_ms > 0 and elf_compile_ms == 0 and elf_run_ms == 0:
        elf_run_ms = elf_ms
    return {
        "ms": aot_ms + repl_ms + elf_ms,
        "aot_ms": aot_ms,
        "repl_ms": repl_ms,
        "elf_ms": elf_ms,
        "elf_compile_ms": elf_compile_ms,
        "elf_run_ms": elf_run_ms,
        "cached_tests": 0,
    }

def profile_rel_path(path):
    full = disp_path(os.path.abspath(path))
    root = disp_path(os.path.abspath(ROOT)).rstrip("/") + "/"
    if full.startswith(root):
        return full[len(root):]
    return full

def new_profile_data():
    return {
        "rev": "phase-v1",
        "phase_totals": {
            "ms": 0,
            "aot_ms": 0,
            "repl_ms": 0,
            "elf_ms": 0,
            "elf_compile_ms": 0,
            "elf_run_ms": 0,
            "cached_tests": 0,
        },
        "suites": {},
        "tests": {},
    }

def add_phase_totals(dst, src):
    for k in ("ms", "aot_ms", "repl_ms", "elf_ms", "elf_compile_ms", "elf_run_ms"):
        dst[k] = int(dst.get(k, 0)) + int(src.get(k, 0))

def record_profile_entry(profile_data, suite_key, path, aot, repl, elf, cached=False):
    if profile_data is None:
        return
    phase = phase_breakdown_ms(aot, repl, elf)
    rel = profile_rel_path(path)
    suite = profile_data["suites"].setdefault(
        suite_key,
        {
            "tests": 0,
            "sum_ms": 0,
            "aot_ms": 0,
            "repl_ms": 0,
            "elf_ms": 0,
            "elf_compile_ms": 0,
            "elf_run_ms": 0,
            "cached_tests": 0,
        },
    )
    suite["tests"] = int(suite.get("tests", 0)) + 1
    suite["sum_ms"] = int(suite.get("sum_ms", 0)) + phase["ms"]
    add_phase_totals(suite, phase)
    if cached:
        suite["cached_tests"] = int(suite.get("cached_tests", 0)) + 1

    phase_totals = profile_data.get("phase_totals", {})
    add_phase_totals(phase_totals, phase)
    if cached:
        phase_totals["cached_tests"] = int(phase_totals.get("cached_tests", 0)) + 1

    profile_data["tests"][rel] = {
        "suite": suite_key,
        "display": shorten(rel),
        "cached": bool(cached),
        **phase,
    }

def write_profile_json(profile_data, out_path, total_count, total_passed, start_time):
    if profile_data is None or not out_path:
        return
    try:
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
    except Exception:
        pass
    payload = dict(profile_data)
    payload["generated_at"] = int(time.time())
    payload["duration_ms"] = int((time.time() - start_time) * 1000)
    payload["total"] = int(total_count)
    payload["passed"] = int(total_passed)
    payload["failed"] = max(0, int(total_count) - int(total_passed))
    try:
        with open(out_path, "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=2, sort_keys=True)
            f.write("\n")
    except Exception:
        pass

def cache_hit(entry, sig, do_repl):
    if not isinstance(entry, dict):
        return False
    if entry.get("sig") != sig:
        return False
    if entry.get("ok") is not True:
        return False
    if not isinstance(entry.get("aot"), dict) or not isinstance(entry.get("elf"), dict):
        return False
    if do_repl and not isinstance(entry.get("repl"), dict):
        return False
    return True

def split_test_flags():
    global _TEST_FLAGS_CACHE
    try:
        cached = _TEST_FLAGS_CACHE
    except NameError:
        cached = None
    if cached is not None:
        return cached
    raw = (os.environ.get("NYTRIX_TEST_FLAGS") or "").strip()
    if not raw:
        _TEST_FLAGS_CACHE = []
        return _TEST_FLAGS_CACHE
    try:
        _TEST_FLAGS_CACHE = shlex.split(raw)
    except ValueError:
        _TEST_FLAGS_CACHE = raw.split()
    return _TEST_FLAGS_CACHE

def print_section_header(name):
    label = f"[ {name} ]"
    width = SECTION_RULE_WIDTH
    side = max(3, (width - len(label) - 2) // 2)
    tail = max(3, width - len(label) - 2 - side)
    print(f"{GRAY}{'-' * side} {CYAN}{label}{RESET}{GRAY} {'-' * tail}{RESET}")

def _read_cpu_model():
    try:
        if not IS_WINDOWS and os.path.exists("/proc/cpuinfo"):
            with open("/proc/cpuinfo", "r", encoding="utf-8", errors="ignore") as f:
                for raw in f:
                    line = raw.strip()
                    if line.startswith("model name"):
                        parts = line.split(":", 1)
                        if len(parts) == 2 and parts[1].strip():
                            return parts[1].strip()
                    if line.startswith("Hardware"):
                        parts = line.split(":", 1)
                        if len(parts) == 2 and parts[1].strip():
                            return parts[1].strip()
        if host_os() == "macos":
            out = subprocess.check_output(
                ["sysctl", "-n", "machdep.cpu.brand_string"],
                text=True,
            ).strip()
            if out:
                return out
    except Exception:
        pass
    if IS_WINDOWS:
        ident = (os.environ.get("PROCESSOR_IDENTIFIER") or "").strip()
        if ident:
            return ident
    proc = (platform.processor() or "").strip()
    if proc:
        return proc
    return host_machine() or "unknown"

def _read_ram_total_bytes():
    try:
        if not IS_WINDOWS and os.path.exists("/proc/meminfo"):
            with open("/proc/meminfo", "r", encoding="utf-8", errors="ignore") as f:
                for raw in f:
                    if raw.startswith("MemTotal:"):
                        fields = raw.split()
                        if len(fields) >= 2:
                            kb = int(fields[1])
                            if kb > 0:
                                return kb * 1024
        if host_os() == "macos":
            out = subprocess.check_output(["sysctl", "-n", "hw.memsize"], text=True).strip()
            if out.isdigit():
                return int(out)
        if IS_WINDOWS:
            try:
                import ctypes

                class MEMORYSTATUSEX(ctypes.Structure):
                    _fields_ = [
                        ("dwLength", ctypes.c_ulong),
                        ("dwMemoryLoad", ctypes.c_ulong),
                        ("ullTotalPhys", ctypes.c_ulonglong),
                        ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong),
                        ("ullAvailPageFile", ctypes.c_ulonglong),
                        ("ullTotalVirtual", ctypes.c_ulonglong),
                        ("ullAvailVirtual", ctypes.c_ulonglong),
                        ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
                    ]

                stat = MEMORYSTATUSEX()
                stat.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
                if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(stat)):
                    return int(stat.ullTotalPhys)
            except Exception:
                pass
    except Exception:
        pass
    return 0

def _fmt_ram(bytes_total):
    if bytes_total <= 0:
        return "unknown"
    gib = float(bytes_total) / float(1024 ** 3)
    if gib >= 1024.0:
        return f"{gib / 1024.0:.2f} TiB"
    return f"{gib:.1f} GiB"

def _normalize_cpu_model(name):
    s = (name or "").strip()
    if not s:
        return "unknown"
    # Strip generic vendor suffixes to keep the host banner compact.
    s = re.sub(r"\s+processor\s*$", "", s, flags=re.IGNORECASE)
    s = re.sub(r"\s+cpu\s*$", "", s, flags=re.IGNORECASE)
    # Core count is already shown in `cores=physical/logical`.
    s = re.sub(r"\s+\d+\s*[- ]core(?:s)?\s*$", "", s, flags=re.IGNORECASE)
    return s.strip() or "unknown"

def print_benchmark_host_info(logical_jobs, bench_worker_jobs):
    logical = os.cpu_count() or 1
    physical = detect_physical_cores(logical=logical)
    os_name = host_os()
    arch = host_machine() or platform.machine().lower() or "unknown"
    cpu_model = _normalize_cpu_model(_read_cpu_model())
    ram_total = _fmt_ram(_read_ram_total_bytes())
    print(
        f"{GRAY}[host]{RESET} "
        f"os={c('36', os_name)} "
        f"arch={c('36', arch)} "
        f"cpu={c('1', cpu_model)} "
        f"cores={c('33', str(physical))}/{c('33', str(logical))} "
        f"ram={c('35', ram_total)} "
        f"jobs={c('32', str(bench_worker_jobs))}/{c('32', str(logical_jobs))}"
    )

def suite_label(suite_key):
    s = normalize_suite_key(suite_key)
    if s == SUITE_BENCHMARK:
        return "Benchmark"
    if s == SUITE_RUNTIME:
        return "Runtime"
    if s == SUITE_STD:
        return "Std"
    if s == "thread":
        return "Thread"
    return s.capitalize() if s else "Other"

def path_suite_key(path):
    if bench_path(path):
        return SUITE_BENCHMARK
    if runtime_path(path):
        return SUITE_RUNTIME
    if std_path(path):
        return SUITE_STD
    return "other"

def print_timing_summary(suite_summaries, timings):
    if not suite_summaries:
        return
    print_section_header("Timing Summary")
    print(f"{GRAY}{'Suite':<10} {'Tests':>5} {'Pass':>5} {'Total':>8} {'Avg':>7} {'Max':>8}{RESET}")
    ordered = [SUITE_BENCHMARK, SUITE_RUNTIME, SUITE_STD, "thread", "other"]
    for key in ordered:
        row = suite_summaries.get(key, {})
        if not row:
            continue
        tests = int(row.get("tests", 0))
        if tests <= 0:
            continue
        passed = int(row.get("passed", 0))
        sum_ms = int(row.get("sum_ms", 0))
        avg_ms = int(round(sum_ms / max(1, tests)))
        max_ms = int(row.get("max_ms", 0))
        print(f"{suite_label(key):<10} {tests:>5} {passed:>5} {sum_ms:>7}ms {avg_ms:>6}ms {max_ms:>7}ms")

    if timings:
        print(f"{GRAY}Top slow tests:{RESET}")
        top = sorted(timings, key=lambda t: float(t[1]), reverse=True)[:8]
        for idx, (path, secs, suite_key) in enumerate(top, start=1):
            print(f"  {idx}. {int(round(float(secs) * 1000.0)):>6}ms  {shorten(disp_path(path))} [{suite_label(suite_key)}]")

def run_test(path, bin_path, do_repl, opt_profile="", do_native=True):
    ny_flags = split_test_flags()
    run_env = os.environ.copy()
    ensure_arm_hardfloat_env(run_env)
    if opt_profile:
        run_env["NYTRIX_OPT_PROFILE"] = opt_profile
    if env_bool("NYTRIX_ENABLE_TEST_MODE", default=False):
        run_env["NYTRIX_TEST_MODE"] = "1"
    else:
        run_env["NYTRIX_TEST_MODE"] = "0"
    # 1. JIT/AOT Run
    aot = run_proc([bin_path, *ny_flags, path], env=run_env)
    # 2. REPL Run
    repl = None
    if do_repl:
        with open(path, "r", encoding="utf-8") as src:
            # Use one-shot REPL mode for deterministic cross-platform behavior.
            # Interactive (-i) plus stdin piping can diverge by terminal/OS.
            repl_src = src.read()
            repl = run_proc([bin_path, *ny_flags, "-repl"], input_str=repl_src, cwd=os.path.dirname(path), env=run_env)
    # 3. Native artifact Compile & Run (EXE on Windows, Mach-O on macOS, ELF elsewhere)
    if do_native:
        exeext = ".exe" if os.name == "nt" else ""
        native_dir = NATIVE_CACHE_DIR
        os.makedirs(native_dir, exist_ok=True)
        native_key = native_cache_key(path, bin_path, opt_profile, ny_flags, do_repl, do_native, run_env)
        elf_fn = os.path.join(native_dir, f"ny_bin_{native_key}{exeext}")
        reuse_native = native_cache_fresh(path, bin_path, elf_fn)
        if reuse_native:
            comp = {"passed": True, "duration": 0.0, "stdout": "", "stderr": "", "code": 0}
        else:
            comp = run_proc([bin_path, *ny_flags, path, "-no-strip", "-o", elf_fn], env=run_env)
        if comp["passed"] and os.path.exists(elf_fn):
            run = run_proc([elf_fn], cwd=os.path.dirname(path))
            elf = {
                "passed": run["passed"],
                "duration": comp["duration"] + run["duration"],
                "compile_dur": comp.get("duration", 0.0),
                "run_dur": run.get("duration", 0.0),
                "stdout": run.get("stdout", ""),
                "stderr": run.get("stderr", ""),
                "error": run.get("error", ""),
                "code": run.get("code"),
                "skipped": False,
            }
        else:
            elf = {
                "passed": False,
                "duration": comp["duration"],
                "compile_dur": comp.get("duration", 0.0),
                "run_dur": 0.0,
                "stdout": comp.get("stdout", ""),
                "stderr": comp.get("stderr", ""),
                "error": comp.get("error", ""),
                "code": comp.get("code"),
                "skipped": False,
            }
    else:
        elf = {
            "passed": True,
            "duration": 0.0,
            "compile_dur": 0.0,
            "run_dur": 0.0,
            "stdout": "",
            "stderr": "",
            "error": "",
            "code": 0,
            "skipped": True,
        }

    return path, aot, repl, elf

def run_thread_stress(path, bin_path, do_repl, iters, opt_profile=""):
    aot_dur = 0.0
    repl_dur = 0.0
    elf_dur = 0.0
    elf_compile_dur = 0.0
    elf_run_dur = 0.0
    fail_aot = None
    fail_repl = None
    fail_elf = None

    for _ in range(iters):
        _, aot, repl, elf = run_test(path, bin_path, do_repl, opt_profile)
        aot_dur += float(aot.get("duration", 0.0))
        elf_dur += float(elf.get("duration", 0.0))
        elf_compile_dur += float(elf.get("compile_dur", 0.0))
        elf_run_dur += float(elf.get("run_dur", 0.0))
        if do_repl and repl is not None:
            repl_dur += float(repl.get("duration", 0.0))
        if fail_aot is None and not aot.get("passed", False):
            fail_aot = aot
        if do_repl and fail_repl is None and repl is not None and not repl.get("passed", False):
            fail_repl = repl
        if fail_elf is None and not elf.get("passed", False):
            fail_elf = elf
        if fail_aot or fail_repl or fail_elf:
            break

    if fail_aot is None:
        aot_out = {"passed": True, "duration": aot_dur, "stdout": "", "stderr": "", "code": 0}
    else:
        aot_out = dict(fail_aot)
        aot_out["duration"] = aot_dur

    if do_repl:
        if fail_repl is None:
            repl_out = {"passed": True, "duration": repl_dur, "stdout": "", "stderr": "", "code": 0}
        else:
            repl_out = dict(fail_repl)
            repl_out["duration"] = repl_dur
    else:
        repl_out = None

    if fail_elf is None:
        elf_out = {
            "passed": True,
            "duration": elf_dur,
            "compile_dur": elf_compile_dur,
            "run_dur": elf_run_dur,
            "stdout": "",
            "stderr": "",
            "code": 0,
        }
    else:
        elf_out = dict(fail_elf)
        elf_out["duration"] = elf_dur
        elf_out["compile_dur"] = elf_compile_dur
        elf_out["run_dur"] = elf_run_dur

    label = f"{path} (x{iters})" if iters > 1 else path
    return label, aot_out, repl_out, elf_out

def print_res(path, aot, repl, elf, idx, total, cached=False):
    path_disp = disp_path(path)
    if UNICODE_UI:
        ok_mark = f"{GREEN}✓{RESET}"
        fail_mark = f"{RED}✗{RESET}"
        skip_mark = f"{YELLOW}·{RESET}"
    else:
        ok_mark = f"{GREEN}OK{RESET}"
        fail_mark = f"{RED}XX{RESET}"
        skip_mark = f"{YELLOW}--{RESET}"
    native_label = "EXE" if IS_WINDOWS else ("Mach-O" if IS_MACOS else "ELF")
    m_aot = skip_mark if aot.get("skipped", False) else (ok_mark if aot["passed"] else fail_mark)
    m_rep = skip_mark if (repl is None or repl.get("skipped", False)) else (ok_mark if repl["passed"] else fail_mark)
    m_elf = skip_mark if elf.get("skipped", False) else (ok_mark if elf["passed"] else fail_mark)

    if cached:
        status = f"[{m_aot}/{m_rep}/{m_elf}] {GRAY}cache{RESET}"
    else:
        dur = int(total_duration(aot, repl, elf) * 1000)
        status = f"[{m_aot}/{m_rep}/{m_elf}] {GRAY}{dur:>4}ms{RESET}"
    print(f"{GRAY}{idx*100//total:>3}%{RESET} {status} {shorten(path_disp)}")

    def fail_detail(r):
        msg = f"{r.get('stdout','')}{r.get('stderr','')}"
        if r.get("error"):
            msg += r["error"]
        return msg

    if not aot["passed"] and not aot.get("skipped", False): print(f"{RED}JIT FAIL{RESET} (code {aot.get('code','?')}) {path_disp}: {fail_detail(aot)}")
    if repl and not repl["passed"] and not repl.get("skipped", False): print(f"{RED}REPL FAIL{RESET} (code {repl.get('code','?')}) {path_disp}: {fail_detail(repl)}")
    if not elf["passed"] and not elf.get("skipped", False): print(f"{RED}{native_label} FAIL{RESET} (code {elf.get('code','?')}) {path_disp}: {fail_detail(elf)}")

    return aot["passed"] and (not repl or repl["passed"]) and elf["passed"]

def run_smoke(bin_path):
    if pty is None:
        print(f"{YELLOW}Skipping REPL smoke test: pty is unavailable on this platform{RESET}")
        return True
    RX = r'ny(!)?\s*>'
    try:
        repl = ReplSession(bin_path)
    except (RuntimeError, OSError) as e:
        print(f"{YELLOW}Skipping REPL smoke test: {e}{RESET}")
        return True
    try:
        def payload_lines(plain, cmd):
            out = []
            for l in plain.splitlines():
                s = l.strip()
                if not s:
                    continue
                if s.startswith("ny>") or s.startswith("ny!>"):
                    continue
                if s == cmd:
                    continue
                out.append(s)
            return out

        def interact(cmd, expected_rx):
            print(f"{CYAN}ny>{RESET} {cmd}")
            repl.send(cmd)
            res = ""
            plain = ""
            payload = []
            # Some hosts (notably macOS PTY) can deliver prompt and result in
            # separate chunks; allow a few prompt cycles before asserting.
            for _ in range(4):
                res += repl.read_until(RX)
                plain = strip_ansi(res)
                payload = payload_lines(plain, cmd)
                if expected_rx == RX:
                    break
                if expected_rx and re.search(expected_rx, "\n".join(payload)):
                    break
            if expected_rx and expected_rx != RX and not re.search(expected_rx, "\n".join(payload)):
                raise AssertionError(f"smoke expectation failed for {cmd!r}: /{expected_rx}/")

            for l in payload:
                print(f"  {GRAY}->{RESET} {l}")

        repl.read_until(RX)
        interact("1 + 1", r'2')
        interact("def x = 123", RX)
        interact("x * 2", r'246')
        interact("str_len('hello')", r'5')
        interact("x + 1", r'124')
    finally: repl.close()
    return True

def main():
    p = argparse.ArgumentParser(); p.add_argument("--bin", default="build/ny"); p.add_argument("--jobs", type=int, default=0); p.add_argument("--pattern", action="append"); p.add_argument("--no-smoke", action="store_true"); p.add_argument("--smoke", action="store_true")
    args = p.parse_args(); bin_path = find_ny_bin(args.bin)
    dbin = bin_path + "_debug" if "_debug" not in bin_path and os.path.exists(bin_path + "_debug") else bin_path
    jobs = effective_jobs(args.jobs)
    real_mode = apply_real_test_mode()

    exec_mode = os.environ.get("NYTRIX_TEST_EXECUTOR", "thread").strip().lower()
    use_process_pool = (exec_mode == "process" and not IS_WINDOWS)
    use_result_cache = env_bool("NYTRIX_TEST_CACHE", default=True)
    print_test_mode_banner(real_mode, use_result_cache)
    wait_log_secs = env_int("NYTRIX_TEST_WAIT_LOG_SECS", default=8, minimum=0)
    wait_log_min_age_secs = env_int(
        "NYTRIX_TEST_WAIT_MIN_AGE_SECS",
        default=max(10, wait_log_secs),
        minimum=0,
    )
    wait_log_solo_secs = env_int(
        "NYTRIX_TEST_WAIT_SOLO_SECS",
        default=max(wait_log_min_age_secs, 15),
        minimum=0,
    )
    profile_json_out = (os.environ.get("NYTRIX_TEST_PROFILE_JSON") or "").strip()
    profile_data = new_profile_data() if profile_json_out else None
    timings_db = load_timings_db()
    results_db = load_results_db() if use_result_cache else {}
    
    suites = [
        ("Benchmark", "etc/tests/benchmark/*.ny", bin_path, suite_repl_enabled("benchmark", default_bench_repl_phase())),
        ("Runtime", "etc/tests/runtime/**/*.ny", dbin, suite_repl_enabled("runtime", default_runtime_repl_phase())),
        ("Std", "std/**/*.ny", dbin, suite_repl_enabled("std", default_std_repl_phase())),
    ]
    
    total_passed, total_count, timings = 0, 0, []
    suite_summaries = {}
    start = time.time()
    if args.smoke and not args.no_smoke and not args.pattern:
        print_section_header("Repl")
        run_smoke(bin_path)
    thread_cfg = thread_stress_settings(args.pattern)
    thread_stress_abs = None
    if thread_cfg:
        if os.path.exists(thread_cfg["path"]):
            print_section_header("Thread")
            opt_profile = effective_opt_profile_for_path(thread_cfg["path"])
            tpath, taot, trepl, telf = run_thread_stress(
                thread_cfg["path"],
                dbin,
                thread_cfg["repl"],
                thread_cfg["iters"],
                opt_profile,
            )
            if print_res(tpath, taot, trepl, telf, 1, 1):
                total_passed += 1
                thread_passed = 1
            else:
                thread_passed = 0
            total_count += 1
            thread_total_dur = total_duration(taot, trepl, telf)
            timings.append((thread_cfg["path"], thread_total_dur, "thread"))
            suite_summaries["thread"] = {
                "tests": 1,
                "passed": thread_passed,
                "sum_ms": int(round(thread_total_dur * 1000.0)),
                "max_ms": int(round(thread_total_dur * 1000.0)),
            }
            thread_stress_abs = os.path.abspath(thread_cfg["path"])
            timings_db["thread"] = {disp_path(thread_cfg["path"]): total_duration(taot, trepl, telf)}
            record_profile_entry(profile_data, "thread", thread_cfg["path"], taot, trepl, telf, cached=False)
        else:
            print(f"{YELLOW}Skipping thread stress: file not found: {thread_cfg['path']}{RESET}")

    seen_files = set()
    try:
        for name, pattern, b, repl_mode in suites:
            files = sorted([f for f in glob.glob(pattern, recursive=True) if pattern_matches(f, args.pattern)])
            # Filter out files already seen in previous suites to avoid double counting
            files = [f for f in files if f not in seen_files]
            for f in files: seen_files.add(f)
            
            if thread_stress_abs:
                files = [f for f in files if os.path.abspath(f) != thread_stress_abs]
            if not files: continue
            suite_key = normalize_suite_key(name)
            hist = timings_db.get(suite_key, {})
            if isinstance(hist, dict) and hist:
                # Longest-first scheduling reduces tail latency under fixed workers.
                files.sort(
                    key=lambda p: float(
                        max(
                            hist.get(disp_path(p), 0.0),
                            suite_cost_hint(suite_key, p),
                        )
                    ),
                    reverse=True,
                )
            elif suite_key == SUITE_BENCHMARK:
                files.sort(key=bench_cost_hint, reverse=True)
            else:
                files.sort(key=lambda p: suite_cost_hint(suite_key, p), reverse=True)
            suite_jobs = max(1, min(jobs, len(files)))
            if suite_key == SUITE_BENCHMARK:
                suite_jobs = max(1, min(bench_jobs(suite_jobs), len(files)))
                print_benchmark_host_info(jobs, suite_jobs)
            print_section_header(name)
            suite_timings = {}
            suite_results = results_db.get(suite_key, {}) if isinstance(results_db.get(suite_key), dict) else {}
            pending_specs = []
            completed = 0
            suite_passed = 0
            suite_total = 0
            suite_sum_ms = 0
            suite_max_ms = 0

            for path in files:
                disp = disp_path(path)
                opt_profile = effective_opt_profile_for_path(path)
                do_native = native_phase_enabled(path)
                do_repl = repl_phase_enabled_for_path(path, repl_mode)
                sig = test_sig(path, b, do_repl, opt_profile, do_native)
                entry = suite_results.get(disp)
                if use_result_cache and cache_hit(entry, sig, do_repl):
                    aot = unpack_phase(entry.get("aot"))
                    rep = unpack_phase(entry.get("repl")) if do_repl else None
                    elf = unpack_phase(entry.get("elf"))
                    ok = print_res(path, aot, rep, elf, completed + 1, len(files), cached=True)
                    if ok:
                        total_passed += 1
                        suite_passed += 1
                    hist_dur = float(entry.get("total_dur", total_duration(aot, rep, elf)))
                    suite_timings[disp] = hist_dur
                    total_dur = hist_dur if env_bool("NYTRIX_TEST_TIMINGS_FROM_CACHE", default=False) else 0.0
                    total_count += 1
                    suite_total += 1
                    ms = int(round(total_dur * 1000.0))
                    suite_sum_ms += ms
                    if ms > suite_max_ms:
                        suite_max_ms = ms
                    timings.append((path, total_dur, suite_key))
                    record_profile_entry(profile_data, suite_key, path, aot, rep, elf, cached=True)
                    completed += 1
                    continue
                pending_specs.append((path, sig, opt_profile, do_native, do_repl))

            if pending_specs:
                live_jobs = max(1, min(suite_jobs, len(pending_specs)))
                # Single-worker suites are common on Windows bench runs. Avoid
                # executor setup/teardown overhead and run inline for consistency.
                if live_jobs == 1:
                    for path, sig, opt_profile, do_native, do_repl in pending_specs:
                        path, aot, rep, elf = run_test(path, b, do_repl, opt_profile, do_native)
                        completed += 1
                        ok = print_res(path, aot, rep, elf, completed, len(files))
                        if ok:
                            total_passed += 1
                            suite_passed += 1
                        total_dur = total_duration(aot, rep, elf)
                        disp = disp_path(path)
                        suite_timings[disp] = total_dur
                        total_count += 1
                        suite_total += 1
                        ms = int(round(total_dur * 1000.0))
                        suite_sum_ms += ms
                        if ms > suite_max_ms:
                            suite_max_ms = ms
                        timings.append((path, total_dur, suite_key))
                        record_profile_entry(profile_data, suite_key, path, aot, rep, elf, cached=False)
                        if use_result_cache:
                            suite_results[disp] = {
                                "sig": sig,
                                "ok": bool(ok),
                                "aot": pack_phase(aot),
                                "repl": pack_phase(rep) if rep is not None else None,
                                "elf": pack_phase(elf),
                                "total_dur": total_dur,
                            }
                else:
                    if use_process_pool:
                        try:
                            ex_ctx = ProcessPoolExecutor(max_workers=live_jobs)
                        except (PermissionError, OSError):
                            # Some environments disallow multiprocessing semaphores; use
                            # threads as a robust fallback.
                            ex_ctx = ThreadPoolExecutor(max_workers=live_jobs)
                    else:
                        ex_ctx = ThreadPoolExecutor(max_workers=live_jobs)
                    with ex_ctx as ex:
                        pending = set()
                        fut_meta = {}
                        submit_idx = 0

                        while submit_idx < len(pending_specs) and len(pending) < live_jobs:
                            path, sig, opt_profile, do_native, do_repl = pending_specs[submit_idx]
                            fut = ex.submit(run_test, path, b, do_repl, opt_profile, do_native)
                            pending.add(fut)
                            fut_meta[fut] = (path, time.time(), sig)
                            submit_idx += 1

                        last_wait_log = time.time()
                        while pending:
                            if wait_log_secs > 0:
                                now = time.time()
                                next_log = last_wait_log + wait_log_secs
                                timeout = min(5.0, max(0.05, next_log - now))
                            else:
                                timeout = 5
                            done, pending = wait(pending, timeout=timeout, return_when=FIRST_COMPLETED)
                            now = time.time()
                            if wait_log_secs > 0 and pending and (now - last_wait_log) >= wait_log_secs:
                                oldest = min(pending, key=lambda fut: fut_meta.get(fut, ("", now, ""))[1])
                                oldest_path, oldest_start, _ = fut_meta.get(oldest, ("<unknown>", now, ""))
                                queued = max(0, len(pending_specs) - submit_idx)
                                oldest_age = int(now - oldest_start)
                                is_solo = (len(pending) == 1 and queued == 0)
                                min_age = wait_log_solo_secs if is_solo else wait_log_min_age_secs
                                if oldest_age >= min_age:
                                    if queued:
                                        print(
                                            f"{GRAY}... running {len(pending)} test(s), {queued} queued; "
                                            f"oldest: {shorten(oldest_path)} ({oldest_age}s, timeout {PROC_TIMEOUT}s){RESET}"
                                        )
                                    else:
                                        print(
                                            f"{GRAY}... running {len(pending)} test(s); "
                                            f"oldest: {shorten(oldest_path)} ({oldest_age}s, timeout {PROC_TIMEOUT}s){RESET}"
                                        )
                                last_wait_log = now
                            if not done:
                                continue
                            for fut in done:
                                meta = fut_meta.pop(fut, None)
                                if meta is None:
                                    continue
                                _, _, sig = meta
                                path, aot, rep, elf = fut.result()
                                completed += 1
                                ok = print_res(path, aot, rep, elf, completed, len(files))
                                if ok:
                                    total_passed += 1
                                    suite_passed += 1
                                total_dur = total_duration(aot, rep, elf)
                                disp = disp_path(path)
                                suite_timings[disp] = total_dur
                                total_count += 1
                                suite_total += 1
                                ms = int(round(total_dur * 1000.0))
                                suite_sum_ms += ms
                                if ms > suite_max_ms:
                                    suite_max_ms = ms
                                timings.append((path, total_dur, suite_key))
                                record_profile_entry(profile_data, suite_key, path, aot, rep, elf, cached=False)
                                if use_result_cache:
                                    suite_results[disp] = {
                                        "sig": sig,
                                        "ok": bool(ok),
                                        "aot": pack_phase(aot),
                                        "repl": pack_phase(rep) if rep is not None else None,
                                        "elf": pack_phase(elf),
                                        "total_dur": total_dur,
                                    }

                                while submit_idx < len(pending_specs) and len(pending) < live_jobs:
                                    npath, nsig, nopt_profile, ndo_native, ndo_repl = pending_specs[submit_idx]
                                    nfut = ex.submit(run_test, npath, b, ndo_repl, nopt_profile, ndo_native)
                                    pending.add(nfut)
                                    fut_meta[nfut] = (npath, time.time(), nsig)
                                    submit_idx += 1

            if suite_timings:
                timings_db[suite_key] = suite_timings
            suite_summaries[suite_key] = {
                "tests": suite_total,
                "passed": suite_passed,
                "sum_ms": suite_sum_ms,
                "max_ms": suite_max_ms,
            }
            if use_result_cache:
                results_db[suite_key] = suite_results
    finally:
        # Persist partial progress so interrupted runs still warm the cache.
        save_timings_db(timings_db)
        if use_result_cache:
            save_results_db(results_db)
        if profile_json_out:
            write_profile_json(profile_data, profile_json_out, total_count, total_passed, start)
    print_timing_summary(suite_summaries, timings)
    print(f"{GRAY}{'-' * SECTION_RULE_WIDTH}{RESET}\nTotal: {total_count} | {GREEN if total_passed==total_count else RED}{total_passed} passed{RESET} | {total_count-total_passed} failed in {int((time.time()-start)*1000)}ms")
    if total_passed != total_count: sys.exit(1)

if __name__ == "__main__":
    try: main()
    except KeyboardInterrupt: sys.exit(1)
