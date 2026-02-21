import sys
sys.dont_write_bytecode = True
import os
import shutil
import re
import time
import subprocess
from pathlib import Path

from context import ROOT, host_os, c, OK_SYMBOL
from utils import run, run_capture, step, warn, err, log_ok, env_int, env_bool, strip_ansi, ir_stats
from cmake import cmake_build_dir

from tidy import run_tidy
from std import run_std_bundle
from fuzz import run_fuzz_harness
from conv import run_conv
from web import run_web_gen

os.environ["PYTHONDONTWRITEBYTECODE"] = "1"

def _py(*args):
    return [sys.executable, "-B", *[str(a) for a in args]]

def bin_paths(build_dir, kind):
    bdir = cmake_build_dir(build_dir, kind)
    exeext = ".exe" if host_os() == "windows" else ""
    return bdir/f"ny{exeext}", bdir/f"ny_debug{exeext}", bdir/f"ny-lsp{exeext}", bdir/"std.ny"

def resolve_primary_bin(build_dir, kind):
    rel, dbg, _, _ = bin_paths(build_dir, kind)
    if kind == "debug":
        return dbg if dbg.exists() else cmake_build_dir(build_dir, "debug")/("ny_debug.exe" if host_os()=="windows" else "ny_debug")
    return rel if rel.exists() else cmake_build_dir(build_dir, "release")/("ny.exe" if host_os()=="windows" else "ny")

def _append_flag_blob(existing, extras):
    base = (existing or "").strip()
    add = " ".join(x for x in extras if x).strip()
    if not add:
        return base
    if not base:
        return add
    return f"{base} {add}".strip()

def _run_make_subcommand(command, jobs=0, env=None, extra_args=None):
    cmd = _py(ROOT / "make", command)
    if jobs:
        cmd += ["-j", str(jobs)]
    if extra_args:
        cmd.extend(extra_args)
    run(cmd, env=env)

def _parse_env_float(name, default):
    raw = (os.environ.get(name) or "").strip()
    if not raw:
        return float(default)
    try:
        return float(raw)
    except ValueError:
        return float(default)

def _sanitizer_env(build_dir, sanitizer):
    env = os.environ.copy()
    for k in (
        "SAN",
        "NYTRIX_SAN",
        "PERF",
        "NYTRIX_PERF",
    ):
        env.pop(k, None)
    env["NYTRIX_SKIP_OPTIONAL_GATES"] = "1"
    san = (sanitizer or "").strip().lower()
    if san == "asan":
        c_extras = ["-fsanitize=address", "-fno-omit-frame-pointer", "-g3"]
        ld_extras = ["-fsanitize=address"]
        env.setdefault("ASAN_OPTIONS", "detect_leaks=0:strict_init_order=1:check_initialization_order=1")
    elif san == "ubsan":
        c_extras = ["-fsanitize=undefined", "-fno-omit-frame-pointer", "-g3", "-fno-sanitize-recover=undefined"]
        ld_extras = ["-fsanitize=undefined"]
        env.setdefault("UBSAN_OPTIONS", "print_stacktrace=1:halt_on_error=1")
    else:
        raise ValueError(f"unsupported sanitizer: {sanitizer}")

    env["NYTRIX_HOST_CFLAGS"] = _append_flag_blob(env.get("NYTRIX_HOST_CFLAGS"), c_extras)
    env["NYTRIX_HOST_LDFLAGS"] = _append_flag_blob(env.get("NYTRIX_HOST_LDFLAGS"), ld_extras)
    env["BUILD_DIR"] = str((Path(build_dir) / san).resolve())
    env["NYTRIX_TEST_CACHE"] = "0"
    env.setdefault("NYTRIX_TEST_EXECUTOR", "thread")
    return env

def run_test(build_dir, test_kind, std_bundle, test_jobs=0, timeout="auto", unknown=None):
    test_bin = resolve_primary_bin(build_dir, test_kind).resolve()
    
    env = os.environ.copy()
    env["NYTRIX_BUILD_STD_PATH"] = str(std_bundle.resolve())
    env.setdefault("NYTRIX_TEST_CACHE", "1")
    
    if host_os() == "windows":
        env["PYTHONUTF8"] = "1"
        env["PYTHONIOENCODING"] = "utf-8"
    else:
        env.setdefault("NYTRIX_USE_LLD", "1" if shutil.which("ld.lld") else "0")

    bin_disp = c('1;36', test_bin.name)
    jobs_disp = c('1;33', str(test_jobs))
    timeout_disp = c('1;35', timeout)
    step(f"run tests: bin={bin_disp} jobs={jobs_disp} timeout={timeout_disp}")
    
    # We still use the external script for now as it's very large, 
    # but we moved it to etc/tools/test.py
    cmd = [
        *_py(ROOT/"etc"/"tools"/"test.py"),
        "--bin", str(test_bin), "--jobs", str(test_jobs)
    ]
    if unknown:
        cmd.extend(unknown)

    run(cmd, env=env)

def run_fuzz(build_dir, jobs=0, iterations=0, timeout_s=0.0, mode=""):
    bin_debug = resolve_primary_bin(build_dir, "debug")
    if not Path(bin_debug).exists():
        fallback = resolve_primary_bin(build_dir, "release")
        warn(f"debug fuzz binary missing; using {Path(fallback).name} fallback")
        bin_debug = fallback
    jobs = jobs or 24
    iterations = iterations or env_int("NYTRIX_FUZZ_ITERS", default=200, minimum=1)
    timeout_s = timeout_s if timeout_s and timeout_s > 0 else _parse_env_float("NYTRIX_FUZZ_TIMEOUT", 1.2)
    mode = (mode or os.environ.get("NYTRIX_FUZZ_MODE") or "mixed").strip().lower() or "mixed"
    fail_on_panic = env_bool("NYTRIX_FUZZ_FAIL_ON_PANIC", default=False)

    rc = run_fuzz_harness(
        bin_debug,
        iterations=iterations,
        jobs=jobs,
        timeout_s=timeout_s,
        mode=mode,
        fail_on_panic=fail_on_panic,
    )
    if rc:
        raise SystemExit(rc)

def run_repl(build_dir, kind="release", unknown=None):
    ny_bin = resolve_primary_bin(build_dir, kind)
    cmd = [str(ny_bin), "-i"]
    extra = list(unknown or [])

    has_effect_policy_flag = any(
        a in ("--effect-require-known", "--no-effect-require-known")
        for a in extra
    )
    has_alias_policy_flag = any(
        a in ("--alias-require-known", "--no-alias-require-known", "--alias-require-no-escape")
        for a in extra
    )
    if not has_effect_policy_flag:
        cmd.append("--no-effect-require-known")
    if not has_alias_policy_flag:
        cmd.append("--no-alias-require-known")

    if extra:
        cmd.extend(extra)
    step(f"run repl: bin={ny_bin.name}")
    # On Windows, os.execv() does not properly transfer the Win32 console
    # to the child process, so ReadConsoleInput fails immediately.
    # Use subprocess.run() on Windows so the console is fully inherited.
    import sys
    if sys.platform == "win32":
        import subprocess
        result = subprocess.run(cmd)
        raise SystemExit(result.returncode)
    # On POSIX, replace the Python wrapper so ny handles Ctrl-C natively.
    os.execv(str(ny_bin), cmd)

def run_std(build_dir, kind="release"):
    bdir = cmake_build_dir(build_dir, kind)
    bundle_path = bdir / "std.ny"
    run_std_bundle(bundle_path)

def run_fmt(unknown=None):
    cmd = [*_py(ROOT / "etc" / "tools" / "nyfmt.py")]
    if unknown:
        cmd.extend(unknown)
    run(cmd)

def run_docs(build_dir):
    # Just an example of how web fits in
    bdir = cmake_build_dir(build_dir, "release")
    bundle_path = bdir / "std.ny"
    run_web_gen(bundle_path, build_dir / "docs")

def run_install(build_dir, kind="release"):
    bdir = cmake_build_dir(build_dir, kind)
    exeext = ".exe" if host_os() == "windows" else ""
    required = [bdir / f"ny{exeext}", bdir / f"ny-lsp{exeext}", bdir / "std.ny"]
    if host_os() == "linux":
        required.append(bdir / "libnytrixrt.so")
    elif host_os() == "macos":
        required.append(bdir / "libnytrixrt.dylib")
    missing = [p for p in required if not p.exists()]
    if missing:
        err("install artifacts are missing:")
        for p in missing:
            err(f"  - {p}")
        err("build first with `python3 make all` (or rerun install without sudo once).")
        raise SystemExit(1)

    env = os.environ.copy()
    cmd = ["cmake", "--install", str(bdir)]
    prefix = (env.get("PREFIX") or "").strip()
    if prefix:
        cmd += ["--prefix", prefix]
    step(f"install: dir={bdir}")
    try:
        run(cmd, env=env)
    except subprocess.CalledProcessError as e:
        default_prefix = "C:/nytrix" if host_os() == "windows" else "/usr"
        target_prefix = prefix or default_prefix
        err(f"install failed for prefix: {target_prefix}")
        if host_os() != "windows":
            err("hint: use sudo with python3 for system prefixes, or install user-local:")
            err("  sudo python3 make install")
            err("  PREFIX=$HOME/.local python3 make install")
        raise SystemExit(e.returncode)
    log_ok("installed")
    if host_os() != "windows":
        default_prefix = "/usr"
        target_prefix = prefix or default_prefix
        if target_prefix not in ("/usr", "/usr/local"):
            bin_dir = Path(target_prefix) / "bin"
            lib_dir = Path(target_prefix) / "lib"
            root_dir = Path(target_prefix) / "share" / "nytrix"
            warn(f"PATH hint: export PATH=\"{bin_dir}:$PATH\"")
            warn(f"Library hint: export LD_LIBRARY_PATH=\"{lib_dir}:$LD_LIBRARY_PATH\"")
            warn(f"Root hint: export NYTRIX_ROOT=\"{root_dir}\"")

def run_uninstall(build_dir, kind="release"):
    bdir = cmake_build_dir(build_dir, kind)
    manifest = bdir / "install_manifest.txt"
    if not manifest.exists():
        err(f"install manifest not found: {manifest}")
        err("run `make install` first with the same build profile")
        raise SystemExit(1)

    removed = 0
    failed = 0
    for raw in manifest.read_text(encoding="utf-8", errors="ignore").splitlines():
        p = Path(raw.strip())
        if not p:
            continue
        try:
            if p.is_file() or p.is_symlink():
                p.unlink()
                removed += 1
            elif p.is_dir():
                shutil.rmtree(p)
                removed += 1
        except OSError:
            failed += 1

    log_ok(f"uninstalled ({removed} path(s) removed, {failed} failed)")

def run_perf(build_dir, kind="release", unknown=None):
    ny_bin = resolve_primary_bin(build_dir, kind).resolve()
    cmd = [*_py(ROOT / "etc" / "tools" / "perfgate.py"), "--bin", str(ny_bin)]
    if unknown:
        cmd.extend(unknown)
    step(f"run perf gate: bin={ny_bin.name}")
    run(cmd)

def run_sanitizer(build_dir, sanitizer, jobs=0, unknown=None):
    san = (sanitizer or "").strip().lower()
    if san not in ("asan", "ubsan"):
        raise SystemExit(f"unsupported sanitizer command: {sanitizer}")
    env = _sanitizer_env(build_dir, san)
    extra_args = list(unknown or [])
    if not extra_args and not env_bool("NYTRIX_SANITIZER_FULL", default=False):
        pattern = (os.environ.get("NYTRIX_SANITIZER_PATTERN") or "").strip()
        if not pattern:
            pattern = "etc/tests/runtime/|std/"
        extra_args += ["--pattern", pattern]

    step(f"run {san} gate: build_dir={env['BUILD_DIR']}")
    _run_make_subcommand("test", jobs=jobs, env=env, extra_args=extra_args)

def run_asan(build_dir, jobs=0, unknown=None):
    run_sanitizer(build_dir, "asan", jobs=jobs, unknown=unknown)

def run_ubsan(build_dir, jobs=0, unknown=None):
    run_sanitizer(build_dir, "ubsan", jobs=jobs, unknown=unknown)

def run_optcheck(build_dir):
    bin_debug = resolve_primary_bin(build_dir, "debug")
    out_dir = build_dir/"cache"/"optcheck"
    out_dir.mkdir(parents=True, exist_ok=True)
    
    src = out_dir/"optcheck.ny"
    src.write_text(
        "fn loop_sum(n){\n  mut i=0\n  mut s=0\n  while(i<n){ s+=i; i+=1 }\n  s\n}\n\n"
        "fn main(){\n  def a = loop_sum(200)\n  def b = loop_sum(120)\n  if(a > b){ a-b } else { b-a }\n}\n",
        encoding="utf-8"
    )
    
    profiles = ("none", "compile", "balanced", "speed", "size")
    print(c("36", "Optimization Check (LLVM IR)"))
    print(c("90", "-"*64))
    
    for profile in profiles:
        ir_path = out_dir/f"{profile}.ll"
        env = os.environ.copy()
        env["NYTRIX_OPT_PROFILE"] = profile
        env["NYTRIX_EMIT_IR_POSTOPT_PATH"] = str(ir_path)
        t0 = time.time()
        run([str(bin_debug), str(src), "--emit-ir="+str(ir_path)], env=env)
        dt = int((time.time()-t0)*1000)
        st = ir_stats(ir_path)
        print(f"  {profile:<8} {dt:>4}ms inst={st.get('inst',0)}")

    log_ok("optcheck complete")
