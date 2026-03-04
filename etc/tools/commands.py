#!/usr/bin/env python3
"""
Nytrix Build Commands
"""
import sys
sys.dont_write_bytecode = True
import os
import shutil
import re
import time
import subprocess
import signal
from pathlib import Path

from context import ROOT, host_os, c, OK_SYMBOL
from utils import log, run, run_capture, step, warn, err, log_ok, env_int, env_bool, strip_ansi, ir_stats
from cmake import cmake_build_dir

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
    if std_bundle is not None:
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

    cmd = [
        *_py(ROOT/"etc"/"tools"/"test.py"),
        "--bin", str(test_bin), "--jobs", str(test_jobs)
    ]
    if unknown:
        cmd.extend(unknown)

    run(cmd, env=env)

def run_fuzz(build_dir, jobs=0, iterations=0, timeout_s=0.0, mode=""):
    from fuzz import run_fuzz_harness

    bin_debug = resolve_primary_bin(build_dir, "debug")
    if not Path(bin_debug).exists():
        fallback = resolve_primary_bin(build_dir, "release")
        warn(f"debug fuzz binary missing; using {Path(fallback).name} fallback")
        bin_debug = fallback
    cpu = os.cpu_count() or 8
    if jobs <= 0:
        jobs = max(4, min(16, cpu // 2))
    else:
        jobs = max(1, min(jobs, max(4, min(16, cpu // 2))))
    iterations = iterations or env_int("NYTRIX_FUZZ_ITERS", default=200, minimum=1)
    timeout_s = timeout_s if timeout_s and timeout_s > 0 else _parse_env_float("NYTRIX_FUZZ_TIMEOUT", 3.0)
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

def run_bin(build_dir, kind="release", unknown=None):
    ny_bin = resolve_primary_bin(build_dir, kind)
    cmd = [str(ny_bin)]
    if unknown:
        cmd.extend(unknown)
    import subprocess
    step(f"run binary: bin={ny_bin.name}")
    
    # We use fork/exec instead of os.execvp directly so that our Python process
    # survives to run subsequent commands (like fb).
    pid = os.fork()
    if pid == 0:
        # Child: Replace with the binary
        try:
            os.execvp(str(ny_bin), cmd)
        except Exception as e:
            err(f"Failed to execute binary: {e}")
            os._exit(1)
    else:
        # Parent: Wait for child to finish
        try:
            _, status = os.waitpid(pid, 0)
            if os.WIFEXITED(status):
                exit_code = os.WEXITSTATUS(status)
                if exit_code != 0:
                    raise SystemExit(exit_code)
            elif os.WIFSIGNALED(status):
                raise SystemExit(128 + os.WTERMSIG(status))
        except KeyboardInterrupt:
            # Try to terminate child gracefully
            try:
                os.kill(pid, signal.SIGINT)
            except OSError:
                pass
            raise SystemExit(130)

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
    import sys
    if sys.platform == "win32":
        import subprocess
        result = subprocess.run(cmd)
        raise SystemExit(result.returncode)
    os.execv(str(ny_bin), cmd)

def run_std(build_dir, kind="release"):
    from cmake import cmake_build
    cmake_build(build_dir, kind, None, None, None, None, target="std_bundle")

def run_docs(build_dir):
    from web import run_web_gen

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

def _analyze_binary(path):
    """Self-implemented ELF size analyzer to replace bloaty dependency."""
    import struct
    import os
    try:
        with open(path, "rb") as f:
            f.seek(0)
            ident = f.read(16)
            if len(ident) < 16 or ident[:4] != b"\x7fELF":
                return None
            
            is_64 = ident[4] == 2
            # Offset of section headers (e_shoff)
            f.seek(40 if is_64 else 32)
            sh_off = struct.unpack("Q" if is_64 else "I", f.read(8 if is_64 else 4))[0]
            # shentsize (58), shnum (60), shstrndx (62)
            f.seek(58 if is_64 else 46) 
            sh_ent_size = struct.unpack("H", f.read(2))[0]
            sh_num = struct.unpack("H", f.read(2))[0]
            sh_str_idx = struct.unpack("H", f.read(2))[0]
            
            # Read names table
            f.seek(sh_off + sh_str_idx * sh_ent_size + (24 if is_64 else 16))
            str_tab_off = struct.unpack("Q" if is_64 else "I", f.read(8 if is_64 else 4))[0]
            str_tab_size = struct.unpack("Q" if is_64 else "I", f.read(8 if is_64 else 4))[0]
            f.seek(str_tab_off)
            str_tab = f.read(str_tab_size)

            cats = {
                "Code": 0, "Read-Only": 0, "Data": 0, "BSS": 0,
                "Reloc": 0, "Debug": 0, "Other": 0
            }
            cats_vm = cats.copy()
            
            # Mapping
            cat_map = {
                ".text": "Code", ".plt": "Code", ".init": "Code", ".fini": "Code",
                ".rodata": "Read-Only", ".eh_frame": "Read-Only", ".eh_frame_hdr": "Read-Only",
                ".data": "Data", ".got": "Data", ".dynamic": "Data", ".data.rel.ro": "Data",
                ".bss": "BSS", ".tbss": "BSS",
                ".rela.dyn": "Reloc", ".rela.plt": "Reloc", ".dynsym": "Reloc", ".dynstr": "Reloc"
            }

            for i in range(sh_num):
                f.seek(sh_off + i * sh_ent_size)
                name_idx = struct.unpack("I", f.read(4))[0]
                sh_type = struct.unpack("I", f.read(4))[0]
                f.seek(sh_off + i * sh_ent_size + (16 if is_64 else 12)) 
                sh_addr = struct.unpack("Q" if is_64 else "I", f.read(8 if is_64 else 4))[0]
                sh_off_val = struct.unpack("Q" if is_64 else "I", f.read(8 if is_64 else 4))[0]
                sh_size = struct.unpack("Q" if is_64 else "I", f.read(8 if is_64 else 4))[0]

                name_end = str_tab.find(b"\x00", name_idx)
                name = str_tab[name_idx:name_end].decode("ascii", "ignore") if name_idx < len(str_tab) else ""
                
                # Determine category
                best_cat = "Other"
                if name.startswith(".debug"): best_cat = "Debug"
                elif name in cat_map: best_cat = cat_map[name]
                elif ".plt" in name: best_cat = "Code"
                elif sh_addr != 0:
                    if sh_type == 8: best_cat = "BSS"
                    else: best_cat = "Data"
                
                # Update sizes
                if sh_type != 8: # NOBITS doesn't take file space
                    cats[best_cat] += sh_size
                if sh_addr != 0:
                    cats_vm[best_cat] += sh_size

            return cats, cats_vm, os.path.getsize(path)
    except Exception:
        return None

def _run_native_profile(build_dir, kind, unknown):
    """Performs instruction-level native profiling of a Nytrix script."""
    if host_os() != "linux":
        err("native profiling (perf) is only supported on Linux.")
        return

    # Extract target script 
    script_path = None
    target_args = []
    
    unk = unknown or []
    # Try finding 'ny <script>'
    if "ny" in unk:
        ny_idx = unk.index("ny")
        if ny_idx + 1 < len(unk):
            script_path = unk[ny_idx + 1]
            target_args = unk[ny_idx + 2:]
    
    # If not found, try the first argument that ends with .ny
    if not script_path:
        for i, val in enumerate(unk):
            if val.endswith(".ny"):
                script_path = val
                target_args = unk[:i] + unk[i+1:]
                break
    
    # Fallback to the first unknown if any, otherwise default
    if not script_path:
        if unk:
            script_path = unk[0]
            target_args = unk[1:]
        else:
            script_path = "etc/tests/ui.ny"
            target_args = []
        
    if not Path(script_path).exists():
        err(f"script not found: {script_path}")
        return

    # 2. Compile to native binary
    ny_bin = resolve_primary_bin(build_dir, "release")
    out_bin = Path(build_dir) / "cache" / "perf" / "target_bin"
    out_bin.parent.mkdir(parents=True, exist_ok=True)
    if out_bin.exists(): out_bin.unlink()


    # Scale benchmarks and ensure libnytrixrt can be found
    p_env = os.environ.copy()
    p_env["NYTRIX_BENCH_SCALE"] = "500"
    p_env["NYTRIX_AUTO_DUMP"] = "1"
    p_env["NYTRIX_DUMP_PATH"] = "build/release/fb_dump.tga"
    p_env["NYTRIX_DWARF_VERSION"] = "2"
    p_env["NYTRIX_DWARF_SPLIT_INLINING"] = "0"
    p_env["NYTRIX_DWARF_PROFILE_INFO"] = "0"
    if not p_env.get("NY_UI_TIMEOUT"):
        p_env["NY_UI_TIMEOUT"] = "1.0"
        
    rt_dir = str(resolve_primary_bin(build_dir, "release").parent)
    cur_ld = p_env.get("LD_LIBRARY_PATH", "")
    p_env["LD_LIBRARY_PATH"] = f"{rt_dir}:{cur_ld}" if cur_ld else rt_dir

    step(f"compile for profiling: {script_path} -> {out_bin.name}")
    # We include -g to ensure symbols are available for perf report.
    # We use CLI flags for DWARF version because the compiler unsets these env vars on startup.
    compile_cmd = [
        str(ny_bin), "-O3", "-g", 
        "--dwarf-version=2", 
        "--no-dwarf-split-inlining", 
        "--no-dwarf-profile-info",
        "-o", str(out_bin), script_path
    ]
    
    proc = subprocess.run(compile_cmd, capture_output=True, text=True, env=p_env)
    if proc.returncode != 0:
        err(f"compilation failed:\n{proc.stdout}\n{proc.stderr}")
        return

    if not out_bin.exists():
        err("compilation failed (no binary produced).")
        return

    try:
        # Records profile
        step("recording profile (perf record)")
        record_cmd = ["perf", "record", "-q", "-g", "-F", "max", str(out_bin)] + target_args
        subprocess.run(record_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=p_env)
        
        # Save a backup but KEEP perf.data for the report below
        if os.path.exists("perf.data"):
            import shutil
            dst = build_dir / "release" / "perf.data.old"
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2("perf.data", str(dst))

        # Binary size analysis (Self-implemented)
        res_size = _analyze_binary(out_bin)
        total_disk_str = "N/A"
        total_vm_str = "N/A"
        size_report = ""
        
        if res_size:
            cats, cats_vm, total_f = res_size
            total_v = sum(cats_vm.values())
            
            def fmt(b):
                if b >= 1024*1024: return f"{b/1024/1024:.2f} MiB"
                if b >= 1024: return f"{b/1024:.1f} KiB"
                return f"{b} B"

            total_disk_str = fmt(total_f)
            total_vm_str = fmt(total_v)
            
            size_report = f"  {c('4;37', 'Category')}        {c('4;37', 'File Size')}    {c('4;37', 'VM Size')}\n"
            sorted_cats = sorted(cats.keys(), key=lambda k: cats[k] + cats_vm[k], reverse=True)
            for cat in sorted_cats:
                f_s, v_s = cats[cat], cats_vm[cat]
                if f_s == 0 and v_s == 0: continue
                # Percentages
                f_p = (f_s / total_f * 100) if total_f > 0 else 0
                v_p = (v_s / total_v * 100) if total_v > 0 else 0
                size_report += f"  {c('37', cat.ljust(15))} {fmt(f_s).rjust(10)} ({f_p:2.0f}%) {fmt(v_s).rjust(10)} ({v_p:2.0f}%)\n"

        # Stdout report (top items)
        if Path("perf.data").exists():
            # Use overhead sort and avoid children for compactness
            # Force LC_ALL=C and --stdio-color never for clean text
            # We use --no-children and --sort overhead,symbol to get a flat table
            report_cmd = ["perf", "report", "-i", "perf.data", "--stdio", "--stdio-color", "never", "--no-children", "-n", "--sort", "overhead,dso,symbol", "--call-graph", "none"]
            res = run_capture(report_cmd, env={"PERF_PAGER": "cat", "LC_ALL": "C"})
            lines = res.stdout.splitlines()
            
            samples = "0"
            events = "N/A"
            hotspots = []
            
            found_header = False
            for ln in lines:
                ln_strip = ln.strip()
                if "Samples:" in ln_strip:
                    samples = ln_strip.split(":")[1].split()[0]
                if "Event count (approx.):" in ln_strip:
                    events = ln_strip.split(":")[1].strip()
                
                if "Overhead" in ln_strip and "Symbol" in ln_strip:
                    found_header = True
                    continue
                
                if found_header and ln_strip and not ln_strip.startswith("#"):
                    # Table row: 23.80%   25679  target_bin  [.] __script_top
                    parts = ln.split()
                    if len(parts) >= 4 and "%" in parts[0]:
                        overhead = parts[0]
                        smps = parts[1]
                        dso = parts[2]
                        # Symbol is the rest, skipping [.] or [k]
                        sym_idx = 4 if len(parts) > 4 and parts[3] in ("[.]", "[k]") else 3
                        sym = " ".join(parts[sym_idx:])
                        if sym.endswith(" - -"): sym = sym[:-4].strip()
                        hotspots.append((overhead, smps, dso, sym))
                    if len(hotspots) >= 20: break

            # PRINT CONSOLIDATED REPORT (No emojis, technical style)
            print(f"\n{c('1;36', 'NYTRIX NATIVE PROFILE')}")
            print(f"{c('90', '-------------------------------------------------------------------------')}")
            print(f"{c('32', 'Binary Size')} : {total_disk_str} (Disk) / {total_vm_str} (VM)")
            print(f"{c('31', 'CPU Cycles')}  : {events}")
            print(f"{c('34', 'Samples')}     : {samples}")
            print(f"\n{c('1', 'Size Breakdown:')}")
            print(size_report)
            print(f"{c('1', 'Top Hotspots:')}")
            if hotspots:
                print(f"  {c('4;37', 'Overhead')}  {c('4;37', 'Samples')}  {c('4;37', 'DSO'.ljust(15))}  {c('4;37', 'Symbol')}")
                for ovr, smps, dso, sym in hotspots:
                    dso_short = dso if len(dso) <= 15 else "..." + dso[-12:]
                    row = f"  {c('1;33', ovr.rjust(8))}  {c('37', smps.rjust(7))}  {c('90', dso_short.ljust(15))}  - {c('37', sym)}"
                    print(row[:160])
            else:
                print(f"  {c('90', '(no samples captured)')}")
            print()
    finally:
        # Cleanup
        # if out_bin.exists(): out_bin.unlink()
        if Path("perf.data").exists(): Path("perf.data").unlink()

def run_perf(build_dir, kind="release", unknown=None):
    # Default to native profiling unless we explicitly want the gate suite
    if not unknown or "ny" in unknown or any(a.endswith(".ny") for a in unknown):
        _run_native_profile(build_dir, kind, unknown)
        return

    # Regression gate suite (multi-benchmark or dispatch matrix)
    ny_bin = resolve_primary_bin(build_dir, kind).resolve()
    cmd = [*_py(ROOT / "etc" / "tools" / "perf.py"), "--bin", str(ny_bin)]
    if unknown:
        cmd.extend(unknown)
    step(f"run perf suite: bin={ny_bin.name}")
    run(cmd)

def run_fmt(unknown=None):
    from fmt import run_fmt as do_fmt
    do_fmt(unknown)

def run_analyze(unknown=None):
    from fmt import run_analyze as do_analyze
    do_analyze(unknown)

def run_check(unknown=None):
    import argparse
    from fmt import run_check as do_check
    p = argparse.ArgumentParser()
    p.add_argument("paths", nargs="*")
    p.add_argument("--fix", action="store_true")
    p.add_argument("-v", "--verbose", action="store_true")
    args, _ = p.parse_known_args(unknown or [])
    do_check(args.paths or None, verbose=args.verbose, fix=args.fix)

def run_sanitizer(build_dir, sanitizer, jobs=0, unknown=None):
    san = (sanitizer or "").strip().lower()
    if san not in ("asan", "ubsan"):
        raise SystemExit(f"unsupported sanitizer command: {sanitizer}")
    env = _sanitizer_env(build_dir, san)
    extra_args = list(unknown or [])
    if not extra_args and not env_bool("NYTRIX_SANITIZER_FULL", default=False):
        pattern = (os.environ.get("NYTRIX_SANITIZER_PATTERN") or "").strip()
        if not pattern:
            pattern = "etc/tests/runtime/|lib/"
        extra_args += ["--pattern", pattern]

    step(f"run {san} gate: build_dir={env['BUILD_DIR']}")
    _run_make_subcommand("test", jobs=jobs, env=env, extra_args=extra_args)

def run_asan(build_dir, jobs=0, unknown=None):
    run_sanitizer(build_dir, "asan", jobs=jobs, unknown=unknown)

def run_ubsan(build_dir, jobs=0, unknown=None):
    run_sanitizer(build_dir, "ubsan", jobs=jobs, unknown=unknown)

def run_fb(unknown=None):
    from fb import main as fb_main
    fb_main(unknown or [])

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
