import sys
sys.dont_write_bytecode = True
import os, argparse, shutil, tempfile
from pathlib import Path
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"

# Nytrix Build System - Main Entry Point

from context import BUILD_DIR, BUILD_DIR_NOTICE, ROOT, host_os, is_arm_riscv_machine, c
from utils import log, log_ok, warn, err
from detect import configure_toolchain
from deps import (
    ensure_deps,
    find_llvm_c_include_root,
    ensure_llvm_c_headers,
    canonical_llvm_c_include_root,
)
from cmake import cmake_build, ensure_std_bundle_fresh
from commands import (
    run_test,
    run_std,
    run_fmt,
    run_docs,
    run_install,
    run_uninstall,
    run_fuzz,
    run_repl,
    run_perf,
    run_asan,
    run_ubsan,
)

COMMANDS = (
    "all",
    "bin",
    "fmt",
    "std",
    "test",
    "repl",
    "fuzz",
    "docs",
    "install",
    "uninstall",
    "clean",
)

ENV_GATES = (
    ("SAN=1|asan|ubsan|all", "run sanitizer gate(s) after tests"),
    ("PERF=1", "run perf gate after tests"),
)

def _default_test_cache_root():
    explicit = (os.environ.get("NYTRIX_TEST_CACHE_DIR") or "").strip()
    if explicit:
        return Path(os.path.abspath(os.path.expanduser(explicit)))

    os_name = host_os()
    if os_name == "windows":
        base = (os.environ.get("LOCALAPPDATA") or "").strip() or os.path.expanduser(r"~\AppData\Local")
        return Path(os.path.abspath(os.path.join(base, "nytrix", "test-cache")))
    if os_name == "macos":
        base = os.path.expanduser("~/Library/Caches")
        return Path(os.path.abspath(os.path.join(base, "nytrix", "test-cache")))

    xdg = (os.environ.get("XDG_CACHE_HOME") or "").strip()
    base = xdg if xdg else os.path.expanduser("~/.cache")
    return Path(os.path.abspath(os.path.join(base, "nytrix", "test-cache")))

def _test_cache_candidates():
    out = []

    def add(p):
        rp = Path(p).resolve()
        if rp not in out:
            out.append(rp)

    add(_default_test_cache_root())
    add(Path(tempfile.gettempdir()) / "nytrix-test-cache")
    add(ROOT / "build" / "cache")
    return out

def _compiler_cache_candidates():
    out = []

    def add(p):
        rp = Path(p).resolve()
        if rp not in out:
            out.append(rp)

    # JIT cache path used by src/wire/cache.c
    add(Path.home() / ".cache" / "nytrix" / "jit")

    os_name = host_os()
    if os_name == "windows":
        local = (os.environ.get("LOCALAPPDATA") or "").strip()
        if local:
            add(Path(local) / "nytrix" / "jit")
    elif os_name == "macos":
        add(Path.home() / "Library" / "Caches" / "nytrix" / "jit")
    else:
        xdg = (os.environ.get("XDG_CACHE_HOME") or "").strip()
        if xdg:
            add(Path(xdg) / "nytrix" / "jit")
    return out

def _compiler_cache_file_candidates():
    out = []
    tmp = Path(tempfile.gettempdir())
    for pattern in ("ny_std_cache_*.ny", "ny_aot_cache_*"):
        for p in tmp.glob(pattern):
            rp = p.resolve()
            if rp not in out:
                out.append(rp)
    return out

def _safe_remove_path(path: Path):
    try:
        rp = path.resolve()
    except OSError:
        rp = path
    banned = {
        Path("/"),
        Path.home().resolve(),
        ROOT.resolve(),
        Path(tempfile.gettempdir()).resolve(),
    }
    if rp in banned:
        return False
    if not rp.exists() and not rp.is_symlink():
        return False
    if rp.is_symlink() or rp.is_file():
        try:
            rp.unlink()
            return True
        except OSError:
            return False
    try:
        shutil.rmtree(rp, ignore_errors=False)
        return True
    except OSError:
        return False

def _clean_test_caches():
    removed = []
    failed = []
    for cand in _test_cache_candidates():
        exists_before = cand.exists() or cand.is_symlink()
        if _safe_remove_path(cand):
            removed.append(cand)
        elif exists_before:
            failed.append(cand)
    return removed, failed

def _clean_compiler_caches():
    removed = []
    failed = []

    for cand in _compiler_cache_candidates():
        exists_before = cand.exists() or cand.is_symlink()
        if _safe_remove_path(cand):
            removed.append(cand)
        elif exists_before:
            failed.append(cand)

    for cand in _compiler_cache_file_candidates():
        exists_before = cand.exists() or cand.is_symlink()
        if _safe_remove_path(cand):
            removed.append(cand)
        elif exists_before:
            failed.append(cand)

    return removed, failed

def _enable_real_test_env():
    # Force fully uncached test execution.
    os.environ["NYTRIX_TEST_REAL"] = "1"
    os.environ["NYTRIX_TEST_CACHE"] = "0"
    os.environ["NYTRIX_TEST_NO_NATIVE_CACHE"] = "1"
    os.environ["NYTRIX_JIT_CACHE"] = "0"
    os.environ["NYTRIX_AOT_CACHE"] = "0"
    os.environ["NYTRIX_STD_CACHE"] = "0"

def _linux_mem_total_gib():
    if host_os() != "linux":
        return 0.0
    meminfo = Path("/proc/meminfo")
    if not meminfo.exists():
        return 0.0
    try:
        for line in meminfo.read_text(encoding="utf-8", errors="ignore").splitlines():
            if not line.startswith("MemTotal:"):
                continue
            parts = line.split()
            if len(parts) < 2:
                return 0.0
            kib = int(parts[1])
            return float(kib) / (1024.0 * 1024.0)
    except (OSError, ValueError):
        return 0.0
    return 0.0

def _parse_int_env(name, default):
    raw = (os.environ.get(name) or "").strip()
    if not raw:
        return default
    try:
        val = int(raw)
        return val if val > 0 else default
    except ValueError:
        return default

def _resolve_jobs(user_jobs):
    if user_jobs and user_jobs > 0:
        return user_jobs, ""

    logical = os.cpu_count() or 1
    jobs = logical
    note = ""
    if host_os() == "linux" and is_arm_riscv_machine():
        mem_gib = _linux_mem_total_gib()
        default_cap = 4
        if mem_gib > 0.0 and mem_gib <= 4.0:
            default_cap = 3
        if mem_gib > 0.0 and mem_gib <= 2.0:
            default_cap = 2
        cap = _parse_int_env("NYTRIX_ARM_BUILD_JOBS_CAP", default_cap)
        if jobs > cap:
            jobs = cap
            if mem_gib > 0.0:
                note = f"auto jobs capped to {jobs} on ARM (ram={mem_gib:.1f} GiB); override with -j or NYTRIX_ARM_BUILD_JOBS_CAP"
            else:
                note = f"auto jobs capped to {jobs} on ARM; override with -j or NYTRIX_ARM_BUILD_JOBS_CAP"
    return max(1, jobs), note

def resolve_llvm_headers(build_dir, cc, llvm_config, llvm_root):
    inc = find_llvm_c_include_root(llvm_root, llvm_config)
    if not inc:
        inc = ensure_llvm_c_headers(build_dir, cc, llvm_config)
    inc = canonical_llvm_c_include_root(inc)
    if inc:
        os.environ["NYTRIX_LLVM_HEADERS"] = inc
        os.environ["NYTRIX_LLVM_INCLUDE"] = inc
    return inc

def _env_on(*names):
    for name in names:
        raw = (os.environ.get(name) or "").strip().lower()
        if raw:
            return raw not in ("0", "false", "off", "no", "none")
    return False

def _san_mode():
    raw = (os.environ.get("SAN") or os.environ.get("NYTRIX_SAN") or "").strip().lower()
    if not raw or raw in ("0", "false", "off", "no", "none"):
        return ""
    if raw in ("1", "true", "on", "both", "all"):
        return "both"
    if raw in ("asan", "address"):
        return "asan"
    if raw in ("ubsan", "undefined"):
        return "ubsan"
    warn(f"unknown SAN value '{raw}' (expected 1|asan|ubsan|all); ignoring sanitizer gate")
    return ""

def _has_optional_gates():
    return _env_on("PERF", "NYTRIX_PERF") or bool(_san_mode())

def _run_optional_gates(build_dir, kind, jobs, unknown):
    if _env_on("NYTRIX_SKIP_OPTIONAL_GATES"):
        return
    if _env_on("PERF", "NYTRIX_PERF"):
        run_perf(build_dir, kind)
    san_mode = _san_mode()
    if san_mode == "asan":
        run_asan(build_dir, jobs=jobs, unknown=unknown)
    elif san_mode == "ubsan":
        run_ubsan(build_dir, jobs=jobs, unknown=unknown)
    elif san_mode == "both":
        run_asan(build_dir, jobs=jobs, unknown=unknown)
        run_ubsan(build_dir, jobs=jobs, unknown=unknown)

def _print_help():
    print("Nytrix build tool")
    print("Usage: py make [commands...] [options]")
    print("")
    print("Commands:")
    print("  " + ", ".join(COMMANDS))
    print("")
    print("Optional env gates (applied on `test` and `all`):")
    for key, desc in ENV_GATES:
        print(f"  {key:<24} {desc}")

def main():
    parser = argparse.ArgumentParser(description="Nytrix Build Tool")
    parser.add_argument("commands", nargs="*", default=["all"])
    parser.add_argument("-j", "--jobs", type=int, default=0)
    parser.add_argument("-v", "--verbose", action="store_true")
    args, unknown = parser.parse_known_args()
    try:
        if BUILD_DIR_NOTICE:
            log("BUILD_DIR", c('36', BUILD_DIR_NOTICE))

        valid = set(COMMANDS)
        valid.add("help")
        if any(c in ("help", "-h", "--help") for c in args.commands):
            _print_help()
            return 0
        bad = [c for c in args.commands if c not in valid]
        if bad:
            err("unknown command(s): " + ", ".join(bad))
            _print_help()
            return 1

        ensure_deps()
        cc, llvm, root = configure_toolchain()
        llvm_inc = resolve_llvm_headers(BUILD_DIR, cc, llvm, root)

        non_windows_runtime = [] if host_os() == "windows" else ["nytrixrt"]
        build_targets = {
            "all": ["ny", "ny-lsp", *non_windows_runtime],
            "bin": ["ny"],
            "std": ["std_bundle"],
            "test": ["ny"],
            "repl": ["ny"],
            "fuzz": ["ny"],
            "docs": ["ny", "std_bundle"],
            "install": ["ny", "ny-lsp", "std_bundle", *non_windows_runtime],
        }

        clean_seen = False
        for cmd in args.commands:
            if cmd == "clean":
                shutil.rmtree(BUILD_DIR, ignore_errors=True)
                removed_test_caches, failed_test_caches = _clean_test_caches()
                removed_compiler_caches, failed_compiler_caches = _clean_compiler_caches()
                pycache_dirs = 0
                pycache_files = 0
                for d in ROOT.rglob("__pycache__"):
                    if d.is_dir():
                        shutil.rmtree(d, ignore_errors=True)
                        pycache_dirs += 1
                for pat in ("*.pyc", "*.pyo"):
                    for f in ROOT.rglob(pat):
                        try:
                            if f.is_file():
                                f.unlink()
                                pycache_files += 1
                        except OSError:
                            pass
                log(
                    "CLEAN",
                    (
                        f"removed {BUILD_DIR}; "
                        f"python caches: dirs={pycache_dirs} files={pycache_files}; "
                        f"test caches: removed={len(removed_test_caches)} failed={len(failed_test_caches)}; "
                        f"compiler caches: removed={len(removed_compiler_caches)} failed={len(failed_compiler_caches)}"
                    ),
                )
                if failed_test_caches:
                    for p in failed_test_caches:
                        warn(f"could not remove test cache: {p}")
                if failed_compiler_caches:
                    for p in failed_compiler_caches:
                        warn(f"could not remove compiler cache: {p}")
                clean_seen = True
                continue

            kind = "debug" if cmd == "fuzz" else "release"
            n_jobs, jobs_note = _resolve_jobs(args.jobs)
            if jobs_note:
                log("HOST", jobs_note)
            targets = build_targets.get(cmd)

            skip_build = False
            if cmd == "install" and targets:
                bdir = BUILD_DIR / kind
                exeext = ".exe" if host_os() == "windows" else ""
                required = [bdir / f"ny{exeext}", bdir / f"ny-lsp{exeext}", bdir / "std.ny"]
                if host_os() == "linux":
                    required.append(bdir / "libnytrixrt.so")
                elif host_os() == "macos":
                    required.append(bdir / "libnytrixrt.dylib")
                if all(p.exists() for p in required):
                    skip_build = True
                    log_ok("install: reusing existing build artifacts")

            if targets and not skip_build:
                if cmd != "std":
                    ensure_std_bundle_fresh(BUILD_DIR, kind)
                cmake_build(
                    BUILD_DIR,
                    kind,
                    cc,
                    llvm,
                    root,
                    llvm_inc,
                    target=targets,
                    jobs=n_jobs,
                )

            if cmd == "test":
                if clean_seen:
                    _enable_real_test_env()
                    log("TEST", "clean+test detected: forcing real uncached test run")
                elif _env_on("NYTRIX_TEST_REAL"):
                    _enable_real_test_env()
                    log("TEST", "NYTRIX_TEST_REAL=1: forcing real uncached test run")
                std_bundle = BUILD_DIR / kind / "std.ny"
                run_test(BUILD_DIR, kind, std_bundle, n_jobs, unknown=unknown)
                _run_optional_gates(BUILD_DIR, kind, n_jobs, unknown)
            elif cmd == "all":
                if _has_optional_gates():
                    std_bundle = BUILD_DIR / kind / "std.ny"
                    run_test(BUILD_DIR, kind, std_bundle, n_jobs, unknown=unknown)
                    _run_optional_gates(BUILD_DIR, kind, n_jobs, unknown)
            elif cmd == "repl":
                run_repl(BUILD_DIR, kind, unknown=unknown)
            elif cmd == "fuzz":
                run_fuzz(BUILD_DIR, jobs=n_jobs)
            elif cmd == "std":
                run_std(BUILD_DIR)
            elif cmd == "fmt":
                run_fmt(unknown=unknown)
            elif cmd == "docs":
                run_docs(BUILD_DIR)
            elif cmd == "install":
                run_install(BUILD_DIR, kind)
            elif cmd == "uninstall":
                run_uninstall(BUILD_DIR, kind)

        return 0
    except KeyboardInterrupt:
        print()
        warn("interrupted")
        return 130

def run(cmd):
    res = subprocess.run(cmd)
    if res.returncode != 0:
        sys.exit(res.returncode)

if __name__ == "__main__":
    import subprocess
    sys.exit(main())
