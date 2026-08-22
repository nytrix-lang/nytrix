#!/usr/bin/env -S python3 -B
from __future__ import annotations
import base64
import json
import os
import hashlib
import platform
import re
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.dont_write_bytecode = True
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
os.environ["MESA_VK_IGNORE_CONFORMANCE_WARNING"] = "true"

os.environ.setdefault("PYTHONPYCACHEPREFIX", str(Path(tempfile.gettempdir()) / "nytrix-pycache"))
sys.pycache_prefix = os.environ["PYTHONPYCACHEPREFIX"]

ROOT = Path(__file__).resolve().parent
QUIET_BOOTSTRAP = False
LOADED_CONFIGS: list[Path] = []

def _select_default_cc() -> str:
    for name in ("clang-20", "clang-19", "clang-18", "clang", "cc", "gcc"):
        path = shutil.which(name)
        if path:
            return path
    return ""

def apply_builtin_env_defaults() -> None:
    """Apply the old top-level env.sh defaults inside ./make itself."""
    os.environ.setdefault("NYTRIX_ROOT", str(ROOT))
    rt_init = ROOT / "src" / "rt" / "init.c"
    if rt_init.exists():
        os.environ.setdefault("NYTRIX_RT_SRC", str(rt_init))
    sysname = platform.system()
    is_win = sysname == "Windows" or sysname.startswith(("MSYS_NT", "MINGW", "CYGWIN_NT"))
    # On Windows, do not snapshot the first clang on PATH here.  The real
    # Windows dependency pass below chooses a coherent LLVM/GMP toolchain
    # (usually one MSYS2/UCRT prefix).  Grabbing Program Files LLVM before
    # that can mix native clang with MSYS2 headers and break system headers.
    if not is_win and not os.environ.get("CC"):
        cc = _select_default_cc()
        if cc:
            os.environ["CC"] = cc

def _config_file_candidates() -> list[Path]:
    out: list[Path] = []
    explicit = (os.environ.get("NYTRIX_CONFIG") or os.environ.get("NY_CONFIG") or "").strip()
    if explicit:
        out.append(Path(explicit).expanduser())
    out.append(ROOT / ".nytrix" / "config")
    out.append(ROOT / "nytrix.config")
    xdg = (os.environ.get("XDG_CONFIG_HOME") or "").strip()
    homes: list[Path] = []
    if xdg:
        homes.append(Path(xdg).expanduser())
    home = Path.home()
    homes.append(home / ".config")
    for base in homes:
        out.append(base / "nytrix" / "config")
        out.append(base / "ny" / "config")
    dedup: list[Path] = []
    seen: set[str] = set()
    for path in out:
        key = str(path)
        if key in seen:
            continue
        seen.add(key)
        dedup.append(path)
    return dedup

def _config_key_ok(key: str) -> bool:
    if not key or not (key[0].isalpha() or key[0] == "_"):
        return False
    return all(ch.isalnum() or ch == "_" for ch in key)

def _config_value(raw: str) -> str:
    value = raw.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
        value = value[1:-1]
    value = value.replace("${ROOT}", str(ROOT)).replace("$ROOT", str(ROOT))
    return os.path.expanduser(os.path.expandvars(value))

def _load_config_file(path: Path) -> None:
    try:
        text = path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return
    loaded = False
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("#") or s.startswith(";"):
            continue
        loaded = True
        if s.startswith("export "):
            s = s[7:].strip()
        if "=" not in s:
            continue
        key, value = s.split("=", 1)
        key = key.strip()
        if not _config_key_ok(key):
            continue
        if key not in os.environ:
            os.environ[key] = _config_value(value)
    if loaded:
        LOADED_CONFIGS.append(path)

def load_default_config() -> None:
    for path in _config_file_candidates():
        _load_config_file(path)
    if LOADED_CONFIGS:
        os.environ.setdefault("NYTRIX_CONFIG_LOADED", ";".join(str(p) for p in LOADED_CONFIGS))

load_default_config()
apply_builtin_env_defaults()

def has_shebang(path: Path) -> bool:
    try:
        with path.open("rb") as f:
            return f.read(2) == b"#!"
    except OSError:
        return False

def chmod_executable(path: Path) -> None:
    mode = path.stat().st_mode
    wanted = mode | 0o111
    if mode != wanted:
        path.chmod(wanted)

def ensure_project_scripts_executable() -> None:
    if host_os() == "windows":
        return
    for path in ROOT.iterdir():
        if path.is_file() and has_shebang(path):
            chmod_executable(path)
    projects = ROOT / "etc" / "projects"
    if not projects.is_dir():
        return
    for path in projects.rglob("*.ny"):
        if not path.is_file():
            continue
        chmod_executable(path)

def host_os() -> str:
    s = platform.system()
    if s == "Linux":
        return "linux"
    if s == "Darwin":
        return "macos"
    if s == "Windows" or s.startswith(("MSYS_NT", "MINGW", "CYGWIN_NT")):
        return "windows"
    return "unknown"

def supports_glyph(glyph: str) -> bool:
    enc = sys.stdout.encoding or "utf-8"
    try:
        glyph.encode(enc)
        return True
    except Exception:
        return False

def color_on() -> bool:
    tool_mode = os.environ.get("NYTRIX_TOOL_COLOR")
    if tool_mode:
        try:
            mode = parse_color_mode(tool_mode)
            if mode == "always":
                return True
            if mode == "never":
                return False
        except SystemExit:
            pass
    if os.environ.get("NO_COLOR"):
        return False
    if os.environ.get("FORCE_COLOR") or os.environ.get("CLICOLOR_FORCE"):
        return True
    return (sys.stdout.isatty() or sys.stderr.isatty()) and os.environ.get("TERM", "") != "dumb"

def parse_color_mode(raw: str) -> str:
    v = (raw or "").strip().lower()
    if v in ("always", "on", "1", "true", "yes"):
        return "always"
    if v in ("never", "off", "0", "false", "no"):
        return "never"
    if v in ("auto", "tty", "default"):
        return "auto"
    raise SystemExit(f"make: invalid color mode '{raw}' (expected auto|always|never)")

def apply_cli_color_mode(mode: str | None) -> bool:
    if mode == "always":
        os.environ["NYTRIX_TOOL_COLOR"] = "always"
        os.environ["FORCE_COLOR"] = "1"
        os.environ["CLICOLOR_FORCE"] = "1"
        os.environ.pop("NO_COLOR", None)
        return True
    if mode == "never":
        os.environ["NYTRIX_TOOL_COLOR"] = "never"
        os.environ["NO_COLOR"] = "1"
        os.environ.pop("FORCE_COLOR", None)
        os.environ.pop("CLICOLOR_FORCE", None)
        return False
    if mode == "auto":
        os.environ["NYTRIX_TOOL_COLOR"] = "auto"
        os.environ.pop("NO_COLOR", None)
        os.environ.pop("FORCE_COLOR", None)
        os.environ.pop("CLICOLOR_FORCE", None)
        return color_on()
    return color_on()

COLOR = color_on()
ASCII_SYMBOLS = (os.environ.get("NYTRIX_UI_SYMBOLS", "").strip().lower() == "ascii")
OK = "OK" if ASCII_SYMBOLS else ("✓" if supports_glyph("✓") else "OK")
ARROW = "->" if ASCII_SYMBOLS else ("→" if supports_glyph("→") else "->")

def c(code: str, s: str) -> str:
    if not COLOR:
        return s
    return f"\033[{code}m{s}\033[0m"

def log(tag: str, msg: str) -> None:
    print(f"{c('1;35', tag)} {msg}", flush=True)

def step(msg: str) -> None:
    print(f"{c('1;36', ARROW)} {c('36', msg)}", flush=True)

def ok(msg: str) -> None:
    print(f"{c('32', OK)} {c('32', msg)}", flush=True)

def err(msg: str) -> None:
    print(msg, file=sys.stderr)

def _cmd_display(cmd: list[str] | str, shell: bool = False) -> str:
    if isinstance(cmd, str):
        return cmd
    return " ".join(shlex.quote(str(x)) for x in cmd)

def _fmt_elapsed(seconds: float) -> str:
    if seconds < 1:
        return f"{int(seconds * 1000)}ms"
    if seconds < 60:
        return f"{seconds:.1f}s"
    mins = int(seconds // 60)
    return f"{mins}m{seconds - mins * 60:04.1f}s"

def boot_log(tag: str, msg: str) -> None:
    if QUIET_BOOTSTRAP:
        return
    log(tag, msg)

def boot_step(msg: str) -> None:
    if QUIET_BOOTSTRAP:
        return
    step(msg)

def boot_ok(msg: str) -> None:
    if QUIET_BOOTSTRAP:
        return
    ok(msg)

def boot_notice(msg: str) -> None:
    if QUIET_BOOTSTRAP:
        return
    # Keep first-run bootstrap status visible even when the launched tool owns
    # stdout (REPL/help/UI). Stderr preserves the user-facing execution stream.
    print(f"{c('1;36', ARROW)} {c('36', msg)}", file=sys.stderr, flush=True)

def cmake_build_has_work(build_root: Path, kind: str, targets: list[str]) -> bool:
    bdir = cmake_build_dir(build_root, kind)
    if not (bdir / "CMakeCache.txt").exists():
        return True
    exe = ".exe" if host_os() == "windows" else ""
    for target in targets:
        if target in ("ny", "ny-fmt", "ny-perf", "ny-test", "ny-doc", "ny-make", "ny-lsp"):
            if not (bdir / f"{target}{exe}").exists():
                return True
    return False

def _vendor_env(build_root: Path) -> dict[str, str]:
    env = os.environ.copy()
    vendor_dir = _detect_vendor_lib_dir(build_root)
    if vendor_dir:
        old = env.get("LD_LIBRARY_PATH", "")
        env["LD_LIBRARY_PATH"] = f"{vendor_dir}{':' + old if old else ''}"
    return env

def tool_smoke_ok(build_root: Path, kind: str, name: str) -> bool:
    if host_os() == "windows":
        return True
    try:
        binp = resolve_tool_bin(build_root, kind, name)
    except SystemExit:
        return False
    launch = tool_launch_path(binp)
    probe = "--version" if name == "ny" else "--help"
    try:
        res = subprocess.run(
            [launch, probe],
            cwd=str(ROOT),
            env=_vendor_env(build_root),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=4,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    # 132 is the common shell code for SIGILL; negative means direct signal.
    return not (res.returncode == 132 or res.returncode < 0)

def clean_bad_tool_build(build_root: Path, kind: str, name: str) -> None:
    if tool_smoke_ok(build_root, kind, name):
        return
    bdir = cmake_build_dir(build_root, kind)
    boot_notice(f"stale/cpu-incompatible {name} binary detected; cleaning {bdir.name} before rebuild")
    shutil.rmtree(bdir, ignore_errors=True)

def cached_run_binary_ok(path: Path) -> bool:
    """Return False only if a cached ny-run binary dies on a signal when started.

    A cached executable is an arbitrary user program, so we cannot probe it with
    --version like a named tool.  We only reject it when launching it crashes
    the process with a signal (SIGILL=132, or any negative returncode) -- the
    same CPU-incompatibility signal clean_bad_tool_build watches for.  A normal
    non-zero exit (e.g. the program rejecting unknown args) is accepted; we
    never want to discard a good cache entry because the user's program printed
    usage and exited 1.
    """
    try:
        res = subprocess.run(
            [str(path), "--nytrix-cache-smoke"],
            cwd=str(ROOT),
            env=os.environ.copy(),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=4,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    return not (res.returncode == 132 or res.returncode < 0)

def restore_tty_visuals() -> None:
    if not sys.stdout.isatty():
        return
    if host_os() != "windows":
        try:
            tty = open("/dev/tty", "rb+", buffering=0)
            try:
                subprocess.run(["stty", "sane"], stdin=tty, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            finally:
                tty.close()
        except Exception:
            pass
    try:
        sys.stdout.write("\033[0m\033[?25h\033[?7h\033[?1049l\033[?2004l\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[2K\r")
        sys.stdout.flush()
    except Exception:
        pass

def run(cmd: list[str] | str, *, shell: bool = False, env: dict[str, str] | None = None, quiet: bool = False) -> None:
    merged = os.environ.copy()
    if env:
        merged.update(env)
    merged.setdefault("NYTRIX_ROOT", str(ROOT))
    show_cmd = _env_flag("NYTRIX_MAKE_COMMANDS", False)
    started = time.perf_counter()
    if show_cmd and not quiet:
        step("$ " + _cmd_display(cmd, shell))
    if COLOR:
        merged.setdefault("FORCE_COLOR", "1")
        merged.setdefault("CLICOLOR_FORCE", "1")
    if quiet:
        res = subprocess.run(
            cmd,
            cwd=str(ROOT),
            shell=shell,
            env=merged,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if res.returncode != 0:
            if res.stdout:
                sys.stderr.write(res.stdout)
            raise subprocess.CalledProcessError(res.returncode, cmd)
        if show_cmd:
            ok("done in " + _fmt_elapsed(time.perf_counter() - started))
        return
    proc = subprocess.Popen(
        cmd,
        cwd=str(ROOT),
        shell=shell,
        env=merged,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=1,
    )
    assert proc.stdout is not None
    for line in proc.stdout:
        print(color_build_line(line.rstrip("\n")), file=sys.stderr, flush=True)
    rc = proc.wait()
    if rc != 0:
        raise subprocess.CalledProcessError(rc, cmd)
    if show_cmd:
        ok("done in " + _fmt_elapsed(time.perf_counter() - started))

NINJA_PROGRESS_RE = re.compile(r"^(\s*)\[(\d+)/(\d+)\]\s*(.*)$")

def color_build_line(line: str) -> str:
    if not COLOR or not line:
        return line
    low = line.lower()
    stripped = line.lstrip()
    if stripped.startswith("ninja: warning:"):
        return c("33;1", "ninja: warning:") + c("33", stripped[len("ninja: warning:"):])
    if stripped.startswith("ninja:"):
        return c("36;1", "ninja:") + c("90", stripped[len("ninja:"):])
    if "warning:" in low or " warning" in low:
        return c("33;1", line)
    if "error:" in low or " failed" in low or low.startswith("failed"):
        return c("31;1", line)
    if stripped.startswith("FAILED:") or stripped.startswith("ninja: build stopped"):
        return c("31;1", line)
    if NINJA_PROGRESS_RE.match(line):
        return color_ninja_line(line)
    if stripped.startswith("--"):
        return color_cmake_line(line)
    if "building c object" in low or "building cxx object" in low:
        return color_build_action(line)
    if "linking" in low:
        return color_link_line(line)
    if "no work to do" in low:
        return c("32", line)
    return line

def color_path_token(token: str) -> str:
    if not token:
        return token
    if token.startswith("CMakeFiles/") and ".dir/" in token:
        return color_cmake_object_path(token)
    suffixes = (
        ".c", ".cc", ".cpp", ".h", ".hpp", ".o", ".obj", ".a", ".so", ".dylib", ".dll", ".exe",
        ".ny", ".json", ".txt", ".cmake",
    )
    if token.startswith(("/", "./", "../")) or "/" in token or "\\" in token or token.endswith(suffixes):
        parts = token.replace("\\", "/").rsplit("/", 1)
        if len(parts) == 2:
            return c("90", parts[0] + "/") + c("36", parts[1])
        return c("36", token)
    return c("36", token)

def color_target_name(target: str) -> str:
    if target == "nytrix_compiler":
        return c("1;35", target)
    if target == "nytrix_runtime":
        return c("1;33", target)
    if target == "ny":
        return c("1;32", target)
    if target.startswith("ny-"):
        return c("1;36", target)
    return c("1;34", target)

def color_lang(lang: str) -> str:
    colors = {
        "C": "1;32",
        "CXX": "1;35",
        "ASM": "1;33",
        "RC": "1;34",
    }
    return c(colors.get(lang, "1;36"), lang)

def color_ninja_progress(cur: str, total: str) -> str:
    return c("90", "[") + c("1;36", cur) + c("90", "/") + c("37", total) + c("90", "]")

def color_source_dir(path: str) -> str:
    palette = (
        ("src/code/", "34"),
        ("src/parse/", "35"),
        ("src/repl/", "36"),
        ("src/base/", "32"),
        ("src/wire/", "33"),
        ("src/rt/", "31"),
        ("src/cmd/", "36"),
    )
    for prefix, color in palette:
        if path.startswith(prefix):
            return c(color, path)
    return c("37", path)

def color_source_file(file_name: str) -> str:
    for obj_suffix in (".c.o", ".cc.o", ".cpp.o", ".c.obj", ".cc.obj", ".cpp.obj"):
        if file_name.endswith(obj_suffix):
            src = file_name[: -2] if obj_suffix.endswith(".o") else file_name[: -4]
            obj = file_name[len(src):]
            return c("1;37", src) + c("90", obj)
    return c("1;37", file_name)

def color_cmake_object_path(token: str) -> str:
    target_part, source = token.split(".dir/", 1)
    target = target_part[len("CMakeFiles/"):]
    if "/" in source or "\\" in source:
        source = source.replace("\\", "/")
        directory, file_name = source.rsplit("/", 1)
        source_html = color_source_dir(directory + "/") + color_source_file(file_name)
    else:
        source_html = color_source_file(source)
    return c("90", "CMakeFiles/") + color_target_name(target) + c("90", ".dir/") + source_html

def color_build_action(rest: str) -> str:
    build_prefixes = (
        ("Building CXX object ", "CXX"),
        ("Building C object ", "C"),
        ("Building ASM object ", "ASM"),
        ("Building RC object ", "RC"),
    )
    for prefix, lang in build_prefixes:
        if rest.startswith(prefix):
            path = rest[len(prefix):]
            return f"{c('1;34', 'Building')} {color_lang(lang)} {c('90', 'object')} {color_path_token(path)}"
    if rest.startswith("Generating "):
        return f"{c('1;33', 'Generating')} {color_path_token(rest[len('Generating '):])}"
    return rest

def color_link_line(rest: str) -> str:
    if not rest.startswith("Linking "):
        return c("35;1", rest)
    bits = rest.split()
    if len(bits) >= 4:
        artifact = " ".join(bits[3:])
        return f"{c('1;35', bits[0])} {color_lang(bits[1])} {c('90', bits[2])} {color_path_token(artifact)}"
    return c("35;1", rest)

def color_ninja_line(line: str) -> str:
    m = NINJA_PROGRESS_RE.match(line)
    if not m:
        return c("36", line)
    lead, cur, total, rest = m.groups()
    if rest.startswith("Linking "):
        body = color_link_line(rest)
    else:
        body = color_build_action(rest)
    return lead + color_ninja_progress(cur, total) + (" " if rest else "") + body

def color_cmake_line(line: str) -> str:
    lead_len = len(line) - len(line.lstrip())
    lead = line[:lead_len]
    stripped = line[lead_len:]
    if not stripped.startswith("--"):
        return c("34", line)
    body = stripped[2:].lstrip()
    prefix = lead + c("90", "--") + " "
    low = body.lower()
    if "done" in low or "success" in low or body.startswith("Found "):
        tone = "32"
    elif body.startswith(("Detecting ", "Performing Test ", "Checking ", "Looking for ")):
        tone = "90"
    elif body.startswith(("Configuring", "Generating", "Build files")):
        tone = "34"
    else:
        tone = "36"
    if ":" in body:
        left, right = body.split(":", 1)
        return prefix + c(tone + ";1", left + ":") + c("36", right)
    words = body.split(" ", 1)
    if len(words) == 2:
        return prefix + c(tone + ";1", words[0]) + " " + c(tone, words[1])
    return prefix + c(tone, body)

def run_capture(cmd: list[str] | str, *, shell: bool = False) -> subprocess.CompletedProcess[str]:
    try:
        merged = os.environ.copy()
        merged.setdefault("NYTRIX_ROOT", str(ROOT))
        return subprocess.run(cmd, cwd=str(ROOT), shell=shell, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=merged)
    except FileNotFoundError as exc:
        return subprocess.CompletedProcess(cmd, 127, "", str(exc))

def which(name: str, path: str | None = None) -> str:
    return shutil.which(name, path=path) or ""

def configure_macos_tool_path() -> None:
    if host_os() != "macos":
        return
    current = os.environ.get("PATH", "")
    seen = {p for p in current.split(os.pathsep) if p}
    extra: list[str] = []
    for path in (
        Path("/opt/homebrew/bin"),
        Path("/usr/local/bin"),
        Path("/opt/homebrew/sbin"),
        Path("/usr/local/sbin"),
    ):
        s = str(path)
        if path.exists() and s not in seen:
            extra.append(s)
            seen.add(s)
    if extra:
        os.environ["PATH"] = os.pathsep.join([*extra, current] if current else extra)

def _env_flag(name: str, default: bool) -> bool:
    raw = (os.environ.get(name) or "").strip().lower()
    if not raw:
        return default
    if raw in ("1", "true", "yes", "on", "y"):
        return True
    if raw in ("0", "false", "no", "off", "n"):
        return False
    return default

def _pkg_exists(name: str) -> bool:
    pkg_tool = which("pkg-config") or which("pkgconf")
    if not pkg_tool:
        return False
    return run_capture([pkg_tool, "--exists", name]).returncode == 0

def _gmp_available() -> bool:
    if _pkg_exists("gmp"):
        return True
    if which("llvm-config"):
        res = run_capture(["llvm-config", "--libs", "all"])
        if res.returncode == 0 and "-lgmp" in res.stdout:
            return True
    for p in ("/usr/include/gmp.h", "/usr/local/include/gmp.h", "/opt/homebrew/include/gmp.h"):
        if Path(p).exists():
            return True
    return False

def _optional_dep_exists(name: str) -> bool:
    if _pkg_exists(name):
        return True
    if name == "z3":
        return bool(which("z3"))
    return False

def _dedupe(items: list[str]) -> list[str]:
    out: list[str] = []
    seen: set[str] = set()
    for item in items:
        if not item or item in seen:
            continue
        seen.add(item)
        out.append(item)
    return out

def _ask_yes_no(prompt: str, default: bool = False) -> bool:
    if not sys.stdin.isatty():
        return default
    suffix = "[Y/n]" if default else "[y/N]"
    try:
        answer = input(f"{prompt} {suffix} ").strip().lower()
    except EOFError:
        return default
    if not answer:
        return default
    return answer in ("1", "true", "yes", "on", "y")

def prepend_path_value(existing: str, value: str) -> str:
    if not value:
        return existing
    parts = [p for p in (existing or "").split(os.pathsep) if p]
    if value in parts:
        parts.remove(value)
    return os.pathsep.join([value, *parts])

def read_os_release() -> dict[str, str]:
    out: dict[str, str] = {}
    p = Path("/etc/os-release")
    if not p.exists():
        return out
    try:
        for line in p.read_text(encoding="utf-8", errors="ignore").splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip().strip('"')
    except Exception:
        return {}
    return out

def apt_best_llvm_ver() -> int:
    res = run_capture("apt-cache search '^llvm-[0-9]+$'", shell=True)
    if res.returncode != 0:
        res = run_capture("apt-cache search llvm-", shell=True)
    vers: set[int] = set()
    for line in (res.stdout or "").splitlines():
        name = (line.split() or [""])[0]
        if not name.startswith("llvm-"):
            continue
        try:
            vers.add(int(name.split("-", 1)[1]))
        except Exception:
            pass
    return max(vers) if vers else 0

def apt_has_pkg(name: str) -> bool:
    res = run_capture(["apt-cache", "show", name])
    return res.returncode == 0 and bool((res.stdout or "").strip())

def _optional_std_dep_checks() -> list[tuple[str, str]]:
    return [
        ("libwebp", "libwebp / std.core.parse.img.webp"),
        ("libturbojpeg", "libturbojpeg / std.core.parse.img.jpeg"),
        ("libpng", "libpng / std.core.parse.img.png"),
        ("freetype2", "freetype / std.os.ui.font.truetype"),
        ("fontconfig", "fontconfig / font discovery"),
        ("librsvg-2.0", "librsvg / std.core.parse.img.svg"),
        ("cairo", "cairo / std.core.parse.img.svg"),
        ("sndfile", "libsndfile / std.os.sound"),
        ("alsa", "ALSA / std.os.sound"),
        ("libpulse", "PulseAudio / std.os.sound"),
        ("jack", "JACK / std.os.sound"),
        ("x11", "X11 window backend"),
        ("xi", "XInput"),
        ("xfixes", "XFixes"),
        ("xcursor", "XCursor"),
        ("xrandr", "XRandR"),
        ("wayland-client", "Wayland window backend"),
        ("wayland-cursor", "Wayland cursor"),
        ("xkbcommon", "keyboard handling"),
        ("vulkan", "Vulkan renderer"),
        ("z3", "Z3 / std.math.smt backend"),
    ]

def _detect_optional_std_missing() -> list[str]:
    missing: list[str] = []
    for pkg, label in _optional_std_dep_checks():
        if not _optional_dep_exists(pkg):
            missing.append(label)
    return missing

def _linux_optional_std_packages(distro: str, like: str) -> list[str]:
    if distro in ("debian", "ubuntu", "linuxmint", "pop", "raspbian") or "debian" in like:
        return [
            "pkg-config",

            "libwebp-dev",
            "libturbojpeg0-dev",
            "libpng-dev",
            "libfreetype-dev",
            "libfontconfig1-dev",
            "librsvg2-dev",
            "libcairo2-dev",
            "libsndfile1-dev",
            "libasound2-dev",
            "libpulse-dev",
            "libjack-jackd2-dev",
            "libx11-dev",
            "libxi-dev",
            "libxfixes-dev",
            "libxcursor-dev",
            "libxrandr-dev",
            "libwayland-dev",
            "libxkbcommon-dev",
            "libvulkan-dev",
            "libz3-dev",
        ]
    if distro in ("arch", "manjaro") or "arch" in like:
        return [
            "pkgconf",

            "libwebp",
            "libjpeg-turbo",
            "libpng",
            "freetype2",
            "fontconfig",
            "librsvg",
            "cairo",
            "libsndfile",
            "alsa-lib",
            "libpulse",
            "jack2",
            "libx11",
            "libxi",
            "libxfixes",
            "libxcursor",
            "libxrandr",
            "wayland",
            "libxkbcommon",
            "vulkan-headers",
            "z3",
        ]
    if distro in ("fedora", "rhel", "centos", "rocky") or "fedora" in like or "rhel" in like:
        return [
            "pkgconf-pkg-config",

            "libwebp-devel",
            "libjpeg-turbo-devel",
            "libpng-devel",
            "freetype-devel",
            "fontconfig-devel",
            "librsvg2-devel",
            "cairo-devel",
            "libsndfile-devel",
            "alsa-lib-devel",
            "pulseaudio-libs-devel",
            "jack-audio-connection-kit-devel",
            "libX11-devel",
            "libXi-devel",
            "libXfixes-devel",
            "libXcursor-devel",
            "libXrandr-devel",
            "wayland-devel",
            "libxkbcommon-devel",
            "vulkan-loader-devel",
            "z3-devel",
        ]
    return []

def _install_optional_std_deps(force_prompt: bool = False) -> None:
    missing = _detect_optional_std_missing()
    if not missing:
        return
    mode = (os.environ.get("NYTRIX_INSTALL_STD_DEPS") or "ask").strip().lower()
    want = False
    if mode in ("1", "true", "yes", "on", "y"):
        want = True
    elif mode in ("0", "false", "no", "off", "n"):
        want = False
    elif force_prompt or sys.stdin.isatty():
        log("DEPS", "optional std/native deps missing:")
        for item in missing:
            print(f"  - {item}", flush=True)
        want = _ask_yes_no("Install optional std/native deps used by the standard library?", False)
    if not want:
        if missing:
            # The skip is allowed (these deps are optional and a minimal CI may
            # legitimately lack them), but it must never be silent: a non-TTY
            # build that exits 0 while missing std/native deps would otherwise
            # ship a stdlib with quietly-disabled features.  Emit a prominent,
            # machine-parseable marker to stderr so logs and CI cannot miss it.
            log("DEPS", "skipping optional std/native deps; set NYTRIX_INSTALL_STD_DEPS=1 or run ./make deps later")
            err(f"NYTRIX_MISSING_STD_DEPS={','.join(missing)}")
            err("NYTRIX_STD_DEPS_SKIPPED=1 (stdlib features that need these deps will be disabled)")
        return

    os_name = host_os()
    if os_name == "linux":
        info = read_os_release()
        distro, like = info.get("ID", "").lower(), info.get("ID_LIKE", "").lower()
        pkgs = _dedupe(_linux_optional_std_packages(distro, like))
        if not pkgs:
            err(f"Unable to auto-install optional std/native deps for distro: {distro or os_name}")
            raise SystemExit(1)
        if distro in ("debian", "ubuntu", "linuxmint", "pop", "raspbian") or "debian" in like:
            step("std deps: apt update")
            run(["sudo", "apt", "update"])
            step("std deps: apt install")
            run(["sudo", "apt", "install", "-y", *pkgs])
            return
        if distro in ("arch", "manjaro") or "arch" in like:
            step("std deps: pacman install")
            run(["sudo", "pacman", "-Sy", "--noconfirm", *pkgs])
            return
        if distro in ("fedora", "rhel", "centos", "rocky") or "fedora" in like or "rhel" in like:
            step("std deps: dnf install")
            run(["sudo", "dnf", "install", "-y", *pkgs])
            return
    if os_name == "macos":
        if not which("brew"):
            err("brew not found; install Homebrew first.")
            raise SystemExit(1)
        step("std deps: brew install")
        run(
            [
                "brew",
                "install",
                "pkg-config",

                "webp",
                "jpeg-turbo",
                "libpng",
                "freetype",
                "fontconfig",
                "librsvg",
                "cairo",
                "libsndfile",
                "jack",
                "molten-vk",
                "z3",
            ]
        )
        return
    log("DEPS", f"optional std/native deps auto-install not yet implemented for host: {os_name}")

def _nytrix_cache_root(subdir: str) -> Path:
    override = (os.environ.get("NYTRIX_CACHE_DIR") or "").strip()
    if override:
        return Path(override).expanduser().resolve() / subdir
    if host_os() == "windows":
        base = (os.environ.get("LOCALAPPDATA") or "").strip()
        if base:
            return Path(base).expanduser().resolve() / "nytrix" / subdir
    elif host_os() == "macos":
        return Path.home().resolve() / "Library" / "Caches" / "nytrix" / subdir
    else:
        xdg = (os.environ.get("XDG_CACHE_HOME") or "").strip()
        if xdg:
            return Path(xdg).expanduser().resolve() / "nytrix" / subdir
    return Path.home().resolve() / ".cache" / "nytrix" / subdir

def _windows_cmd_exists(name: str) -> bool:
    if host_os() != "windows":
        return False
    res = subprocess.run(["cmd", "/d", "/c", "where", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return res.returncode == 0

def _windows_run_install(cmdline: str) -> None:
    res = run_capture(cmdline, shell=True)
    if res.returncode == 0:
        return
    out = (res.stdout or "") + (res.stderr or "")
    if "already installed" in out or "No available upgrade" in out:
        return
    raise SystemExit(f"Dependency install failed: {cmdline}")

def _windows_deps_provider() -> str:
    raw = (os.environ.get("NYTRIX_WINDOWS_DEPS_PROVIDER") or "msys2").strip().lower()
    if raw in ("native", "system", "choco", "winget"):
        return "native"
    return "msys2"

def _windows_env_path(raw: str) -> Path:
    raw = (raw or "").strip().strip('"')
    if not raw:
        return Path()
    if platform.system() == "Windows" and raw.startswith("/") and not raw.startswith("//"):
        cands = [which("cygpath"), r"C:\msys64\usr\bin\cygpath.exe", r"C:\tools\msys64\usr\bin\cygpath.exe"]
        for cand in cands:
            if not cand or not Path(cand).exists():
                continue
            res = run_capture([cand, "-w", raw])
            if res.returncode == 0 and res.stdout.strip():
                return Path(res.stdout.strip().splitlines()[0])
        parts = raw.strip("/").split("/", 1)
        if parts and parts[0].lower() in ("ucrt64", "clang64", "mingw64", "clangarm64"):
            p = Path(r"C:\msys64") / parts[0]
            if len(parts) > 1:
                p = p / parts[1]
            return p
    return Path(raw)

def _prepend_env_path(name: str, value: Path) -> None:
    if not value:
        return
    text = _windows_cmake_path(value) if name in ("CMAKE_PREFIX_PATH", "PKG_CONFIG_PATH") else str(value)
    os.environ[name] = prepend_path_value(os.environ.get(name, ""), text)

def _windows_cmake_path(path: Path) -> str:
    return str(path).replace("\\", "/")

def _windows_tool_path(bin_dir: Path, name: str) -> Path | None:
    names = [name] if name.endswith(".exe") else [f"{name}.exe", name]
    for item in names:
        p = bin_dir / item
        if p.exists():
            return p
    return None

def _windows_cmake_tool(raw: str) -> str:
    raw = (raw or "").strip()
    if not raw:
        return ""
    path_like = any(sep in raw for sep in ("/", "\\")) or bool(re.match(r"^[A-Za-z]:", raw))
    if not path_like:
        return raw
    path = _windows_env_path(raw)
    tool = _windows_tool_path(path.parent, path.name) if path.name else None
    return _windows_cmake_path(tool or path)

def _windows_is_msys2_shell() -> bool:
    return bool((os.environ.get("MSYSTEM") or os.environ.get("MSYSTEM_PREFIX") or os.environ.get("MINGW_PREFIX") or "").strip())

def _windows_msys2_system() -> str:
    raw = (os.environ.get("NYTRIX_MSYS2_SYSTEM") or os.environ.get("MSYSTEM") or "").strip().upper()
    valid = {"UCRT64", "CLANG64", "MINGW64", "CLANGARM64"}
    if raw in valid:
        return raw
    machine = platform.machine().lower()
    return "CLANGARM64" if machine in ("arm64", "aarch64") else "UCRT64"

def _windows_msys2_layout() -> tuple[str, str]:
    system = _windows_msys2_system()
    layouts = {
        "UCRT64": ("ucrt64", "mingw-w64-ucrt-x86_64"),
        "CLANG64": ("clang64", "mingw-w64-clang-x86_64"),
        "MINGW64": ("mingw64", "mingw-w64-x86_64"),
        "CLANGARM64": ("clangarm64", "mingw-w64-clang-aarch64"),
    }
    return layouts.get(system, layouts["UCRT64"])

def _windows_msys2_root_candidates() -> list[Path]:
    raw: list[str] = []
    for key in ("NYTRIX_MSYS2_ROOT", "MSYS2_ROOT"):
        value = (os.environ.get(key) or "").strip()
        if value:
            raw.append(value)
    for key in ("MSYSTEM_PREFIX", "MINGW_PREFIX"):
        value = (os.environ.get(key) or "").strip()
        if value:
            prefix = _windows_env_path(value)
            if prefix.name.lower() in ("ucrt64", "clang64", "mingw64", "clangarm64"):
                raw.append(str(prefix.parent))
    raw.extend([r"C:\msys64", r"C:\tools\msys64"])
    out: list[Path] = []
    seen: set[str] = set()
    for item in raw:
        p = _windows_env_path(item)
        if not p:
            continue
        key = str(p).lower()
        if key in seen:
            continue
        seen.add(key)
        out.append(p)
    return out

def _windows_find_msys2_root() -> Path | None:
    for root in _windows_msys2_root_candidates():
        if (root / "usr" / "bin" / "bash.exe").exists() or (root / "usr" / "bin" / "bash").exists():
            return root
    return None

def _windows_bash_has_pacman(bash: Path) -> bool:
    probe = (
        "command -v pacman >/dev/null 2>&1 && "
        "{ [ -n \"${MSYSTEM:-}\" ] || uname -o 2>/dev/null | grep -Eiq 'msys|mingw|cygwin'; }"
    )
    return run_capture([str(bash), "-lc", probe]).returncode == 0

def _windows_is_wsl_bash(path: Path) -> bool:
    p = str(path).replace("/", "\\").lower()
    return p.endswith("\\windows\\system32\\bash.exe") or p.endswith("\\windows\\sysnative\\bash.exe")

def _windows_find_msys2_bash() -> Path | None:
    cands: list[Path] = []
    for root in _windows_msys2_root_candidates():
        cands.append(root / "usr" / "bin" / "bash.exe")
        cands.append(root / "usr" / "bin" / "bash")
    path_bash = which("bash")
    if path_bash:
        bash = Path(path_bash)
        if not _windows_is_wsl_bash(bash):
            cands.append(bash)
    seen: set[str] = set()
    for bash in cands:
        key = str(bash).lower()
        if key in seen:
            continue
        seen.add(key)
        if not bash.exists():
            continue
        if _windows_bash_has_pacman(bash):
            return bash
    return None

def _windows_install_msys2_base() -> None:
    if _windows_find_msys2_bash():
        return
    if _windows_is_msys2_shell():
        raise SystemExit("MSYS2 shell detected, but pacman was not found. Reinstall MSYS2 or run from a UCRT64/CLANG64 shell.")
    attempted: list[str] = []
    if _windows_cmd_exists("winget") or which("winget"):
        cmd = (
            "winget install -e --id MSYS2.MSYS2 --accept-source-agreements "
            "--accept-package-agreements --disable-interactivity"
        )
        attempted.append(cmd)
        try:
            step("deps: winget install MSYS2")
            _windows_run_install(cmd)
        except SystemExit:
            pass
    if not _windows_find_msys2_bash() and (_windows_cmd_exists("choco") or which("choco")):
        cmd = "choco install msys2 -y --no-progress --accept-license"
        attempted.append(cmd)
        try:
            step("deps: choco install MSYS2")
            _windows_run_install(cmd)
        except SystemExit:
            pass
    if not _windows_find_msys2_bash():
        hint = "; ".join(attempted) if attempted else "winget or choco was not found"
        raise SystemExit(f"MSYS2 was not found and could not be installed automatically ({hint}). Install MSYS2 or set NYTRIX_MSYS2_ROOT.")

def _windows_msys2_target_prefix() -> Path | None:
    for key in ("MINGW_PREFIX", "MSYSTEM_PREFIX"):
        value = (os.environ.get(key) or "").strip()
        if value:
            prefix = _windows_env_path(value)
            if (prefix / "bin").exists():
                return prefix
    subdir, _pkg_prefix = _windows_msys2_layout()
    root = _windows_find_msys2_root()
    if root:
        return root / subdir
    return None

def _windows_configure_msys2_env(prefix: Path) -> None:
    bin_dir = prefix / "bin"
    if not bin_dir.exists():
        return
    prefix_name = prefix.name.lower()
    msystem = {
        "ucrt64": "UCRT64",
        "clang64": "CLANG64",
        "mingw64": "MINGW64",
        "clangarm64": "CLANGARM64",
    }.get(prefix_name)
    if msystem:
        os.environ["MSYSTEM"] = msystem
    os.environ["MSYSTEM_PREFIX"] = _windows_cmake_path(prefix)
    os.environ["MINGW_PREFIX"] = _windows_cmake_path(prefix)
    _prepend_env_path("PATH", bin_dir)
    _prepend_env_path("CMAKE_PREFIX_PATH", prefix)
    _prepend_env_path("PKG_CONFIG_PATH", prefix / "lib" / "pkgconfig")
    _prepend_env_path("PKG_CONFIG_PATH", prefix / "share" / "pkgconfig")
    os.environ["ZLIB_ROOT"] = _windows_cmake_path(prefix)

    # Keep the C/C++ compiler, llvm-config, pkg-config and import libraries
    # from the same MSYS2 prefix.  This prevents the bad mix seen from cmd.exe:
    # Program Files LLVM clang + C:/msys64/ucrt64 headers/libs.
    keep = _env_flag("NYTRIX_WINDOWS_KEEP_TOOLCHAIN", False) or _windows_deps_provider() == "native"
    cc = _windows_tool_path(bin_dir, "clang") or _windows_tool_path(bin_dir, "gcc")
    cxx = _windows_tool_path(bin_dir, "clang++") or _windows_tool_path(bin_dir, "g++")
    if cc and not keep:
        os.environ["CC"] = str(cc)
        os.environ["CMAKE_C_COMPILER"] = _windows_cmake_path(cc)
    elif cc:
        os.environ.setdefault("CC", str(cc))
    if cxx and not keep:
        os.environ["CXX"] = str(cxx)
        os.environ["CMAKE_CXX_COMPILER"] = _windows_cmake_path(cxx)
    elif cxx:
        os.environ.setdefault("CXX", str(cxx))

    pkg_config = _windows_tool_path(bin_dir, "pkg-config") or _windows_tool_path(bin_dir, "pkgconf")
    if pkg_config:
        os.environ["PKG_CONFIG"] = str(pkg_config)

def _windows_msys2_packages_for(missing: list[str]) -> list[str]:
    _subdir, pkg_prefix = _windows_msys2_layout()
    m = set(missing)
    pkgs: list[str] = []
    if "llvm" in m or "clang" in m:
        pkgs += [f"{pkg_prefix}-clang", f"{pkg_prefix}-llvm", f"{pkg_prefix}-zlib"]
    if "cmake" in m:
        pkgs.append(f"{pkg_prefix}-cmake")
    if "ninja" in m:
        pkgs.append(f"{pkg_prefix}-ninja")
    if "pkg-config" in m:
        pkgs.append(f"{pkg_prefix}-pkgconf")
    if "gmp" in m:
        pass
    if "git" in m:
        pkgs.append("git")
    return _dedupe(pkgs)

def _windows_install_msys2_deps(missing: list[str]) -> None:
    pkgs = _windows_msys2_packages_for(missing)
    if not pkgs:
        return
    _windows_install_msys2_base()
    bash = _windows_find_msys2_bash()
    if not bash:
        raise SystemExit("MSYS2 is installed, but pacman could not be launched.")
    step("deps: msys2 pacman install")
    pkg_args = " ".join(shlex.quote(p) for p in pkgs)
    run([str(bash), "-lc", f"pacman -Sy --needed --noconfirm {pkg_args}"])
    prefix = _windows_msys2_target_prefix()
    if prefix:
        _windows_configure_msys2_env(prefix)

def _windows_install_cmds(missing: list[str]) -> list[str]:
    out: list[str] = []
    m = set(missing)
    if "llvm" in m or "clang" in m:
        out.append(
            '(where clang >nul 2>nul || where clang-cl >nul 2>nul) || '
            '(choco install llvm -y --no-progress --accept-license || '
            'winget install -e --id LLVM.LLVM --accept-source-agreements '
            '--accept-package-agreements --disable-interactivity)'
        )
    if "cmake" in m:
        out.append(
            'where cmake >nul 2>nul || '
            '(choco install cmake -y --no-progress --accept-license || '
            'winget install -e --id Kitware.CMake --accept-source-agreements '
            '--accept-package-agreements --disable-interactivity)'
        )
    if "ninja" in m:
        out.append(
            'where ninja >nul 2>nul || '
            '(choco install ninja -y --no-progress --accept-license || '
            'winget install -e --id Ninja-build.Ninja --accept-source-agreements '
            '--accept-package-agreements --disable-interactivity)'
        )
    if "git" in m:
        out.append(
            'where git >nul 2>nul || '
            '(choco install git -y --no-progress --accept-license || '
            'winget install -e --id Git.Git --accept-source-agreements '
            '--accept-package-agreements --disable-interactivity)'
        )
    return out

def _windows_has_llvm_headers(llvm_root: Path) -> bool:
    include_dir = llvm_root / "include"
    return (include_dir / "llvm-c" / "Core.h").exists() and (include_dir / "clang-c" / "Index.h").exists()

def _windows_has_llvm_install(llvm_root: Path) -> bool:
    return (llvm_root / "bin" / "clang.exe").exists() and _windows_has_llvm_headers(llvm_root)

def _windows_configure_llvm_env(llvm_root: Path) -> None:
    if not _windows_has_llvm_headers(llvm_root):
        raise SystemExit(f"LLVM root is incomplete: {llvm_root} is missing llvm-c/Core.h or clang-c/Index.h")
    bin_dir = llvm_root / "bin"
    include_dir = llvm_root / "include"
    if llvm_root.name.lower() in ("ucrt64", "clang64", "mingw64", "clangarm64"):
        _windows_configure_msys2_env(llvm_root)
    else:
        _prepend_env_path("PATH", bin_dir)
        _prepend_env_path("CMAKE_PREFIX_PATH", llvm_root)
        if not _env_flag("NYTRIX_WINDOWS_KEEP_TOOLCHAIN", False):
            cc = _windows_tool_path(bin_dir, "clang") or _windows_tool_path(bin_dir, "clang-cl")
            cxx = _windows_tool_path(bin_dir, "clang++") or _windows_tool_path(bin_dir, "clang-cl")
            if cc:
                os.environ["CC"] = str(cc)
                os.environ["CMAKE_C_COMPILER"] = _windows_cmake_path(cc)
            if cxx:
                os.environ["CXX"] = str(cxx)
                os.environ["CMAKE_CXX_COMPILER"] = _windows_cmake_path(cxx)
    os.environ["LLVM_ROOT"] = str(llvm_root)
    os.environ["NYTRIX_LLVM_INCLUDE"] = str(include_dir)
    os.environ["NYTRIX_LLVM_HEADERS"] = str(include_dir)
    cfg = _windows_tool_path(bin_dir, "llvm-config")
    if cfg:
        os.environ["LLVM_CONFIG"] = str(cfg)

def _windows_find_msys2_llvm() -> Path | None:
    for root in _windows_msys_prefixes():
        if not _windows_has_llvm_headers(root):
            continue
        if (root / "bin" / "llvm-config.exe").exists() or (root / "bin" / "llvm-config").exists():
            return root
    return None

def _windows_llvm_root_from_config(config: str) -> Path | None:
    if not config:
        return None
    cfg = _windows_env_path(config)
    if cfg.name.lower().startswith("llvm-config") and cfg.parent.name.lower() == "bin":
        root = cfg.parent.parent
        if _windows_has_llvm_headers(root):
            return root
    res = run_capture([str(cfg), "--prefix"])
    if res.returncode == 0 and res.stdout.strip():
        root = _windows_env_path(res.stdout.strip().splitlines()[0])
        if _windows_has_llvm_headers(root):
            return root
    return None

def _windows_ensure_llvm() -> None:
    if host_os() != "windows":
        return
    env_include = (os.environ.get("NYTRIX_LLVM_INCLUDE") or os.environ.get("NYTRIX_LLVM_HEADERS") or "").strip()
    if env_include:
        include_dir = _windows_env_path(env_include)
        if (include_dir / "llvm-c" / "Core.h").exists() and (include_dir / "clang-c" / "Index.h").exists():
            _windows_configure_llvm_env(include_dir.parent)
            return
    prefix = _windows_msys2_target_prefix()
    if prefix and _windows_has_llvm_headers(prefix):
        _windows_configure_llvm_env(prefix)
        return
    env_config = (os.environ.get("LLVM_CONFIG") or "").strip()
    root = _windows_llvm_root_from_config(env_config)
    if root:
        _windows_configure_llvm_env(root)
        return
    root = _windows_find_msys2_llvm()
    if root:
        _windows_configure_llvm_env(root)
        return
    path_config = which("llvm-config") or which("llvm-config.exe")
    root = _windows_llvm_root_from_config(path_config)
    if root:
        _windows_configure_llvm_env(root)
        return
    env_root = (os.environ.get("LLVM_ROOT") or "").strip()
    if env_root and _windows_has_llvm_install(_windows_env_path(env_root)):
        _windows_configure_llvm_env(_windows_env_path(env_root))
        return
    program_files = Path(r"C:\Program Files\LLVM")
    if _windows_has_llvm_install(program_files):
        _windows_configure_llvm_env(program_files)
        return
    if _env_flag("NYTRIX_AUTO_DEPS", True) and _windows_deps_provider() == "msys2":
        _windows_install_msys2_deps(["llvm", "clang", "cmake", "ninja", "pkg-config"])
        root = _windows_find_msys2_llvm()
        if root:
            _windows_configure_llvm_env(root)
            return
    if _windows_bootstrap_llvm_from_source():
        return
    raise SystemExit(
        "LLVM/Clang development headers not found for Windows. Install MSYS2 UCRT "
        "mingw-w64-ucrt-x86_64-llvm and mingw-w64-ucrt-x86_64-clang, or set "
        "LLVM_CONFIG/NYTRIX_LLVM_INCLUDE to a complete LLVM dev install."
    )

def _windows_vcpkg_root() -> Path:
    raw = (os.environ.get("VCPKG_ROOT") or "").strip()
    if raw:
        return _windows_env_path(raw)
    return Path(r"C:\vcpkg")

def _windows_msys_prefixes() -> list[Path]:
    raw: list[str] = []
    for key in ("MSYSTEM_PREFIX", "MINGW_PREFIX"):
        value = (os.environ.get(key) or "").strip()
        if value:
            raw.append(value)
    subdir, _pkg_prefix = _windows_msys2_layout()
    for root in _windows_msys2_root_candidates():
        raw.append(str(root / subdir))
        raw.extend(str(root / d) for d in ("ucrt64", "clang64", "mingw64", "clangarm64"))
    out: list[Path] = []
    seen: set[str] = set()
    for item in raw:
        p = _windows_env_path(item)
        key = str(p).lower()
        if key in seen:
            continue
        seen.add(key)
        out.append(p)
    return out

def _windows_prefers_gnu_toolchain() -> bool:
    if (os.environ.get("MSYSTEM") or os.environ.get("MSYSTEM_PREFIX") or os.environ.get("MINGW_PREFIX")):
        return True
    for key in ("CMAKE_C_COMPILER", "CC"):
        value = (os.environ.get(key) or "").strip().lower().replace("\\", "/")
        if "mingw" in value or "ucrt64" in value or "clang64" in value or value.endswith("/gcc") or value.endswith("/clang"):
            return True
    return False

def _windows_vcpkg_builtin_baseline(vcpkg_root: Path) -> str:
    raw = (os.environ.get("VCPKG_BUILTIN_BASELINE") or os.environ.get("NYTRIX_VCPKG_BASELINE") or "").strip()
    if raw:
        baseline = raw
    else:
        res = run_capture(["git", "-C", str(vcpkg_root), "rev-parse", "HEAD"])
        baseline = (res.stdout or "").strip().splitlines()[0] if res.returncode == 0 and res.stdout.strip() else ""
    if not baseline:
        res = run_capture(["git", "ls-remote", "https://github.com/microsoft/vcpkg.git", "HEAD"])
        first = (res.stdout or "").strip().splitlines()[0] if res.returncode == 0 and res.stdout.strip() else ""
        baseline = first.split()[0] if first else ""
    hexdigits = "0123456789abcdefABCDEF"
    if len(baseline) == 40 and all(ch in hexdigits for ch in baseline):
        return baseline
    return ""

def _windows_bootstrap_llvm_from_source() -> bool:
    if host_os() != "windows":
        return False
    mode = (os.environ.get("NYTRIX_WINDOWS_LLVM_FROM_SOURCE") or "0").strip().lower()
    if mode in ("0", "false", "off", "no", "n"):
        return False
    if not (which("git") and which("cmake")):
        return False

    # If a complete LLVM dev install is already visible, do not force source build
    # unless explicitly requested. A bare clang.exe is not enough because CMake
    # needs llvm-c/Core.h and clang-c/Index.h.
    if mode in ("auto", "1", "true", "on", "yes", "y"):
        visible = _windows_find_msys2_llvm()
        if visible:
            _windows_configure_llvm_env(visible)
            return True
        configured = _windows_llvm_root_from_config((os.environ.get("LLVM_CONFIG") or "").strip() or which("llvm-config") or which("llvm-config.exe"))
        if configured:
            _windows_configure_llvm_env(configured)
            return True

    cache_root = Path((os.environ.get("NYTRIX_WINDOWS_LLVM_ROOT") or "").strip() or _nytrix_cache_root("llvm-src"))
    src_root = cache_root / "llvm-project"
    build_root = cache_root / "build"
    install_root = cache_root / "install"
    ref = (os.environ.get("NYTRIX_WINDOWS_LLVM_REF") or "llvmorg-18.1.8").strip() or "llvmorg-18.1.8"
    jobs = max(1, int((os.environ.get("NYTRIX_WINDOWS_LLVM_JOBS") or "0").strip() or "0") or (os.cpu_count() or 8))

    if _windows_has_llvm_install(install_root):
        log("DEPS", f"using cached Windows LLVM source build ({install_root})")
        _windows_configure_llvm_env(install_root)
        return True

    if not _windows_cmd_exists("cl"):
        raise SystemExit(
            "Windows LLVM source bootstrap requires MSVC tools (`cl`). "
            "Run from Developer PowerShell/Developer Command Prompt."
        )

    cache_root.mkdir(parents=True, exist_ok=True)
    if (src_root / ".git").exists():
        step(f"LLVM: updating source checkout ({ref})")
        run(["git", "-C", str(src_root), "fetch", "--depth", "1", "origin", ref])
        run(["git", "-C", str(src_root), "checkout", "--force", "FETCH_HEAD"])
    else:
        step(f"LLVM: cloning source ({ref})")
        run(["git", "clone", "--depth", "1", "--branch", ref, "https://github.com/llvm/llvm-project.git", str(src_root)])

    step("LLVM: configuring source build")
    build_root.mkdir(parents=True, exist_ok=True)
    cfg = [
        "cmake",
        "-S",
        str(src_root / "llvm"),
        "-B",
        str(build_root),
        "-G",
        "Ninja" if which("ninja") else "NMake Makefiles",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_INSTALL_PREFIX={install_root}",
        "-DLLVM_ENABLE_PROJECTS=clang;lld",
        "-DLLVM_TARGETS_TO_BUILD=host",
        "-DLLVM_INCLUDE_TESTS=OFF",
        "-DLLVM_INCLUDE_BENCHMARKS=OFF",
        "-DLLVM_INCLUDE_EXAMPLES=OFF",
        "-DLLVM_INCLUDE_BINDINGS=OFF",
        "-DLLVM_BUILD_TOOLS=ON",
        "-DLLVM_ENABLE_ZLIB=OFF",
        "-DLLVM_ENABLE_ZSTD=OFF",
        "-DLLVM_ENABLE_TERMINFO=OFF",
    ]
    run(cfg)

    step(f"LLVM: building/installing (jobs={jobs})")
    run(["cmake", "--build", str(build_root), "--target", "install", "--config", "Release", "-j", str(jobs)])

    if not _windows_has_llvm_install(install_root):
        raise SystemExit(f"Windows LLVM source build finished but install is incomplete: {install_root}")
    _windows_configure_llvm_env(install_root)
    ok(f"Windows LLVM source build ready: {install_root}")
    return True
def _clean_stale_cmake_caches() -> None:
    build_root = ROOT / "build"
    for bdir in (build_root / "release", build_root / "debug", build_root / "asan", build_root / "ubsan", build_root / "static"):
        cache = bdir / "CMakeCache.txt"
        if cache.exists():
            cache.unlink(missing_ok=True)
            shutil.rmtree(bdir / "CMakeFiles", ignore_errors=True)



def _check_llvm_headers() -> tuple[bool, str | None]:
    """Check if LLVM/Clang development headers exist. Returns (found, include_dir)."""
    import subprocess
    include_dirs = [
        "/usr/include",
        "/usr/local/include",
        "/opt/homebrew/include",
        "/usr/lib/llvm-22/include",
        "/usr/lib/llvm-21/include",
        "/usr/lib/llvm-20/include",
        "/usr/lib/llvm-19/include",
        "/usr/lib/llvm-18/include",
        "/usr/lib/llvm-17/include",
        "/usr/lib/llvm-16/include",
        "/usr/lib/llvm-15/include",
        "/usr/lib/llvm-14/include",
    ]
    
    # Also check from llvm-config if available
    llvm_config = os.environ.get("LLVM_CONFIG") or shutil.which("llvm-config")
    if llvm_config:
        try:
            result = subprocess.run(
                [llvm_config, "--includedir"],
                capture_output=True, text=True, timeout=5
            )
            if result.returncode == 0:
                include_dirs.insert(0, result.stdout.strip())
        except Exception:
            pass
    
    # Check LLVM_CONFIG env var with versioned configs
    if "LLVM_CONFIG" in os.environ:
        try:
            result = subprocess.run(
                [os.environ["LLVM_CONFIG"], "--includedir"],
                capture_output=True, text=True, timeout=5
            )
            if result.returncode == 0:
                include_dirs.insert(0, result.stdout.strip())
        except Exception:
            pass
    
    # Deduplicate
    seen = set()
    unique_dirs = []
    for d in include_dirs:
        if d not in seen:
            seen.add(d)
            unique_dirs.append(d)
    
    for inc_dir in unique_dirs:
        llvm_core = Path(inc_dir) / "llvm-c" / "Core.h"
        clang_index = Path(inc_dir) / "clang-c" / "Index.h"
        if llvm_core.exists() and clang_index.exists():
            return True, str(inc_dir)
    
    return False, None

def ensure_deps(force_optional_prompt: bool = False, require_git: bool = False) -> None:
    if host_os() == "macos":
        configure_macos_llvm_env()
    if host_os() == "windows":
        prefix = _windows_msys2_target_prefix()
        if prefix:
            _windows_configure_msys2_env(prefix)
    missing: list[str] = []
    for t in ("cmake", "clang"):
        if not which(t):
            missing.append(t)
    if require_git and not which("git"):
        missing.append("git")
    if not (which("pkg-config") or which("pkgconf")):
        missing.append("pkg-config")
    if not which("ninja"):
        missing.append("ninja")
    has_llvm_config = False
    if os.environ.get("LLVM_CONFIG") or which("llvm-config"):
        has_llvm_config = True
    else:
        for ver in range(25, 13, -1):
            cand = which(f"llvm-config-{ver}")
            if cand:
                has_llvm_config = True
                os.environ["LLVM_CONFIG"] = cand
                break
    
    # Also verify LLVM/Clang development headers exist (not just llvm-config binary)
    llvm_headers_ok, _ = _check_llvm_headers()
    if not llvm_headers_ok and host_os() != "windows":
        missing.append("llvm")
    elif not has_llvm_config and host_os() != "windows":
        missing.append("llvm")
    if host_os() == "windows":
        if not (which("clang") or Path(r"C:\Program Files\LLVM\bin\clang.exe").exists()):
            missing.append("llvm")
        if not which("cmake"):
            missing.append("cmake")
        if require_git and not which("git"):
            missing.append("git")
    missing = _dedupe(missing)

    if not missing:
        _windows_ensure_llvm()
        pass
        if force_optional_prompt:
            _install_optional_std_deps(True)
        return
    auto = _env_flag("NYTRIX_AUTO_DEPS", True)
    if not auto:
        err("Missing build dependencies: " + ", ".join(missing))
        err("Set NYTRIX_AUTO_DEPS=1 to auto-install.")
        raise SystemExit(1)

    os_name = host_os()
    if os_name == "linux":
        info = read_os_release()
        distro, like = info.get("ID", "").lower(), info.get("ID_LIKE", "").lower()
        if distro in ("debian", "ubuntu", "linuxmint", "pop", "raspbian") or "debian" in like:
            v = apt_best_llvm_ver()
            pkgs = ["build-essential", "python3", "cmake", "ninja-build", "git", "gdb", "pkg-config", "zlib1g-dev"]
            if v > 0:
                pkgs += [f"clang-{v}", f"llvm-{v}", f"llvm-{v}-dev", f"llvm-{v}-runtime"]
                if apt_has_pkg(f"libclang-{v}-dev"):
                    pkgs.append(f"libclang-{v}-dev")
                if apt_has_pkg("libclang-dev"):
                    pkgs.append("libclang-dev")
            else:
                pkgs += ["clang", "llvm-dev", "libclang-dev"]
            run(["sudo", "apt", "update"])
            step("deps: apt install")
            run(["sudo", "apt", "install", "-y", *pkgs])
            _install_optional_std_deps(force_optional_prompt)
            _clean_stale_cmake_caches()
            return
        if distro in ("arch", "manjaro") or "arch" in like:
            step("deps: pacman install")
            run(["sudo", "pacman", "-Sy", "--noconfirm", "base-devel", "python", "clang", "cmake", "ninja", "git", "gdb", "llvm", "pkgconf", "zlib"])
            _install_optional_std_deps(force_optional_prompt)
            _clean_stale_cmake_caches()
            return
        if distro in ("fedora", "rhel", "centos", "rocky") or "fedora" in like or "rhel" in like:
            step("deps: dnf install")
            run(["sudo", "dnf", "install", "-y", "@development-tools", "clang", "llvm-devel", "cmake", "ninja-build", "git", "gdb", "pkgconf-pkg-config", "zlib-devel"])
            _install_optional_std_deps(force_optional_prompt)
            _clean_stale_cmake_caches()
            return
    if os_name == "macos":
        if not which("brew"):
            err("brew not found; install Homebrew first.")
            raise SystemExit(1)
        step("deps: brew install")
        pkgs: list[str] = []
        for dep, pkg in (("cmake", "cmake"), ("ninja", "ninja"), ("pkg-config", "pkg-config")):
            if dep in missing:
                pkgs.append(pkg)
        if "git" in missing:
            pkgs.append("git")
        if "llvm" in missing or "clang" in missing:
            pkgs[:0] = ["llvm@20", "lld@20"]
        if pkgs:
            run(["brew", "install", *_dedupe(pkgs)])
        configure_macos_llvm_env()
        _install_optional_std_deps(force_optional_prompt)
        _clean_stale_cmake_caches()
        return
    if os_name == "windows":
        if _windows_deps_provider() == "msys2":
            if missing:
                log("DEPS", "installing Windows dependencies with MSYS2/UCRT64")
                _windows_install_msys2_deps(missing)
        else:
            cmds = _windows_install_cmds(missing)
            if cmds:
                log("DEPS", "installing Windows dependencies")
                for cmd in cmds:
                    run(cmd, shell=True)
        _install_optional_std_deps(force_optional_prompt)
        _clean_stale_cmake_caches()
        return
    err(f"Unable to auto-install dependencies for host: {os_name}")
    raise SystemExit(1)

def ensure_dir_writable(path: Path) -> bool:
    try:
        path.mkdir(parents=True, exist_ok=True)
        p = path / ".nytrix_write_probe"
        p.write_text("ok", encoding="utf-8")
        p.unlink(missing_ok=True)
        return True
    except Exception:
        return False

def resolve_build_dir() -> tuple[Path, str]:
    raw = (os.environ.get("BUILD_DIR") or "").strip()
    if raw:
        p = Path(raw).expanduser().resolve()
        if p.name in {".asan-build", "asan-build", ".ubsan-build", "ubsan-build", ".san-build", "san-build"}:
            return (p.parent / "build").resolve(), ""
        return p, ""
    d = (ROOT / "build").resolve()
    if ensure_dir_writable(d):
        return d, ""
    fb = (Path.home() / ".cache" / "nytrix-build").resolve()
    if ensure_dir_writable(fb):
        return fb, f"default build dir not writable ({d}); using {fb}"
    return d, ""

def resolve_jobs(requested: int) -> tuple[int, str]:
    if requested > 0:
        return requested, "user-specified"
    env_jobs = (os.environ.get("NYTRIX_BUILD_JOBS") or "").strip()
    if env_jobs.isdigit() and int(env_jobs) > 0:
        v = int(env_jobs)
        return v, f"jobs={v} from NYTRIX_BUILD_JOBS"
    cpu = os.cpu_count() or 1
    jobs = max(1, int(cpu * 0.75))
    if cpu >= 2:
        jobs = max(2, jobs)
    mem_gib = 0.0
    if host_os() == "linux":
        try:
            for ln in Path("/proc/meminfo").read_text(encoding="utf-8", errors="ignore").splitlines():
                if ln.startswith("MemTotal:"):
                    kib = int((ln.split() or ["0", "0"])[1])
                    mem_gib = kib / (1024 * 1024)
                    break
        except Exception:
            pass
    if mem_gib > 0.0:
        cap = max(1, int(mem_gib / 1.5))
        if jobs > cap:
            jobs = cap
            return jobs, f"auto jobs={jobs} capped by RAM ({mem_gib:.1f} GiB); override with -j or NYTRIX_BUILD_JOBS"
    return jobs, f"auto jobs={jobs} using 75% of {cpu} cores (RAM={mem_gib:.1f} GiB); override with -j or NYTRIX_BUILD_JOBS"

def host_mem_gib() -> float:
    if host_os() != "linux":
        return 0.0
    try:
        for ln in Path("/proc/meminfo").read_text(encoding="utf-8", errors="ignore").splitlines():
            if ln.startswith("MemTotal:"):
                kib = int((ln.split() or ["0", "0"])[1])
                return kib / (1024 * 1024)
    except Exception:
        pass
    return 0.0

def resolve_test_jobs(cli_jobs: int) -> int:
    env_v = (os.environ.get("NYTRIX_TEST_JOBS") or "").strip()
    if env_v:
        try:
            v = int(env_v)
            if v >= 0:
                return v
        except Exception:
            pass
    if cli_jobs > 0:
        return cli_jobs
    cpu = os.cpu_count() or 1
    auto = max(1, int(cpu * 0.5))
    if cpu >= 2:
        auto = max(2, auto)
    mem_gib = host_mem_gib()
    # A large stdlib LLVM compile can peak above 2.5 GiB, and several phases
    # overlap. Reserve 6 GiB per automatic worker so a sweep cannot consume the
    # whole machine. Explicit job settings remain available for controlled CI.
    if mem_gib > 0.0:
        auto = min(auto, max(1, int(mem_gib / 6.0)))
    auto = min(auto, 8)
    return auto

def configure_macos_llvm_env() -> None:
    if host_os() != "macos":
        return
    configure_macos_tool_path()
    if os.environ.get("LLVM_CONFIG"):
        return
    prefixes = [
        Path("/opt/homebrew/opt/llvm@20"),
        Path("/usr/local/opt/llvm@20"),
        Path("/opt/homebrew/opt/llvm@19"),
        Path("/usr/local/opt/llvm@19"),
        Path("/opt/homebrew/opt/llvm@18"),
        Path("/usr/local/opt/llvm@18"),
        Path("/opt/homebrew/opt/llvm"),
        Path("/usr/local/opt/llvm"),
    ]
    for prefix in prefixes:
        cfg = prefix / "bin" / "llvm-config"
        inc = prefix / "include"
        if not cfg.exists() or not (inc / "llvm-c" / "Core.h").exists() or not (inc / "clang-c" / "Index.h").exists():
            continue
        os.environ["LLVM_CONFIG"] = str(cfg)
        os.environ.setdefault("NYTRIX_LLVM_INCLUDE", str(inc))
        os.environ["PATH"] = f"{prefix / 'bin'}{os.pathsep}{os.environ.get('PATH', '')}"
        current_prefix = os.environ.get("CMAKE_PREFIX_PATH", "")
        parts = [str(prefix)]
        if current_prefix:
            parts.append(current_prefix)
        os.environ["CMAKE_PREFIX_PATH"] = os.pathsep.join(parts)
        return

def cmake_build_dir(build_root: Path, kind: str) -> Path:
    if kind == "debug":
        return build_root / "debug"
    if kind == "release":
        return build_root / "release"
    return build_root / kind

def cmake_flag_list(raw: str) -> str:
    raw = (raw or "").strip()
    if not raw:
        return ""
    if ";" in raw:
        return raw
    return ";".join(shlex.split(raw))

def cmake_configure_complete(bdir: Path) -> bool:
    return (
        (bdir / "build.ninja").exists()
        or (bdir / "Makefile").exists()
        or any(bdir.glob("*.sln"))
    )

def _vendor_has_nytrix_libedit_stub(lib_dir: Path) -> bool:
    """Detect the compatibility libedit stub from older tarballs.

    That stub is only meant to satisfy LLVM's unused editline dependency; if it
    sits in LD_LIBRARY_PATH it can also be picked up by host tools such as
    /bin/sh.  Some shells are linked against libedit and then fail during
    process startup with missing symbols like `el_source`.
    """
    if host_os() != "linux" or not lib_dir.is_dir():
        return False
    for p in sorted(lib_dir.glob("libedit.so*")):
        try:
            if "nytrix-stub" in p.name:
                return True
            target = p.resolve(strict=True)
            if "nytrix-stub" in target.name:
                return True
        except Exception:
            try:
                link = os.readlink(p)
                if "nytrix-stub" in link:
                    return True
            except Exception:
                pass
    return False

def _vendor_libedit_shim_ready(lib_dir: Path) -> bool:
    """Return true when the bundled libedit shim has the shell-safe exports."""
    if host_os() != "linux" or not lib_dir.is_dir():
        return False
    readelf = which("readelf")
    if not readelf:
        return False
    candidates = sorted(lib_dir.glob("libedit.so*"))
    for p in candidates:
        try:
            target = p.resolve(strict=True)
        except Exception:
            target = p
        if "nytrix-stub" not in p.name and "nytrix-stub" not in target.name:
            continue
        try:
            res = subprocess.run([readelf, "-Ws", str(target)], capture_output=True, text=True, timeout=5)
        except Exception:
            return False
        text = (res.stdout or "") + (res.stderr or "")
        return (" el_source" in text) and (" el_resize" in text) and (" history_length" in text)
    return False

def _sanitize_vendor_terminal_libs(lib_dir: Path) -> None:
    """Refresh stale terminal-library shims before exporting vendor LD paths."""
    has_stub = _vendor_has_nytrix_libedit_stub(lib_dir)
    needs_edit = _vendor_any_needed(lib_dir, "libedit.so.0")
    has_edit = any(lib_dir.glob("libedit.so*"))
    if has_stub and _vendor_libedit_shim_ready(lib_dir):
        return
    if has_stub or (needs_edit and not has_edit):
        _write_vendor_libedit_stub(lib_dir)

def _detect_vendor_lib_dir(build_root: Path) -> Path | None:
    lib_dir = build_root / "vendor" / "lib" / "host"
    if lib_dir.is_dir() and any(lib_dir.glob("*.so*")):
        _sanitize_vendor_terminal_libs(lib_dir)
        if any(lib_dir.glob("*.so*")):
            return lib_dir
    return None

def _same_cmake_path(a: str, b: str) -> bool:
    def norm(x: str) -> str:
        return x.strip().strip('"').replace("\\", "/").lower()
    return norm(a) == norm(b)

def cmake_configure(build_root: Path, kind: str) -> Path:
    configure_macos_llvm_env()
    if host_os() == "windows":
        _windows_ensure_llvm()
        pass
    bdir = cmake_build_dir(build_root, kind)
    bdir.mkdir(parents=True, exist_ok=True)
    cache = bdir / "CMakeCache.txt"
    # Detect vendored LLVM early so -D flags include rpath-link and -rpath.
    vendor_dir = _detect_vendor_lib_dir(build_root)
    extra_ldflags: list[str] = []
    if vendor_dir:
        vendored_root = vendor_dir.parent.parent
        vendored_llvm_config = vendored_root / "bin" / "llvm-config"
        vendored_include = vendored_root / "include"
        if vendored_llvm_config.exists():
            if vendored_include.exists():
                os.environ.setdefault("NYTRIX_LLVM_INCLUDE", str(vendored_include))
                os.environ.setdefault("LLVM_CONFIG", str(vendored_llvm_config))
                vendored_bin = vendored_root / "bin"
                path = os.environ.get("PATH", "")
                path_entries = path.split(":")
                if str(vendored_bin) not in path_entries:
                    os.environ["PATH"] = f"{vendored_bin}:{path}"
                log("BUILD", f"cmake: vendored LLVM at {_rel_or_abs(vendored_llvm_config)}")
            # rpath-link so the linker resolves transitive .so deps.
            extra_ldflags.append(f"-Wl,-rpath-link,{vendor_dir}")
    host_cflags = cmake_flag_list(os.environ.get("NYTRIX_HOST_CFLAGS") or "")
    raw_ldflags = os.environ.get("NYTRIX_HOST_LDFLAGS") or ""
    if extra_ldflags:
        raw_ldflags = (raw_ldflags + " " + " ".join(extra_ldflags)).strip()
    host_ldflags = cmake_flag_list(raw_ldflags)
    cache_matches_flags = True
    win_cc = ""
    win_cxx = ""
    if host_os() == "windows":
        win_cc = _windows_cmake_tool(os.environ.get("CMAKE_C_COMPILER") or os.environ.get("CC") or "")
        win_cxx = _windows_cmake_tool(os.environ.get("CMAKE_CXX_COMPILER") or os.environ.get("CXX") or "")
    use_llvm = os.environ.get("NYTRIX_USE_LLVM", "ON")
    if cache.exists():
        cache_matches_flags = (
            cmake_cache_value(bdir, "NYTRIX_HOST_CFLAGS", "") == host_cflags and
            cmake_cache_value(bdir, "NYTRIX_HOST_LDFLAGS", "") == host_ldflags and
            (not ("NYTRIX_USE_LLVM" in os.environ) or cmake_cache_value(bdir, "NYTRIX_USE_LLVM", "ON") == use_llvm)
        )
        if host_os() == "windows":
            cached_cc = cmake_cache_value(bdir, "CMAKE_C_COMPILER", "")
            cached_cxx = cmake_cache_value(bdir, "CMAKE_CXX_COMPILER", "")
            cache_matches_flags = (
                cache_matches_flags and
                (not win_cc or _same_cmake_path(cached_cc, win_cc)) and
                (not win_cxx or _same_cmake_path(cached_cxx, win_cxx))
            )
    if cache.exists() and cmake_configure_complete(bdir) and cache_matches_flags:
        boot_ok(f"cmake ({kind}) up to date (unchanged)")
        return bdir
    if cache.exists() and (not cmake_configure_complete(bdir) or not cache_matches_flags):
        cache.unlink(missing_ok=True)
        shutil.rmtree(bdir / "CMakeFiles", ignore_errors=True)
    boot_step(f"cmake configure ({kind})")
    cfg = "Debug" if kind in ("debug", "asan", "ubsan") else "Release"
    cmd = [
        "cmake", "-S", str(ROOT), "-B", str(bdir),
        f"-DCMAKE_BUILD_TYPE={cfg}", "-DNYTRIX_FAST_BUILD=ON",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        f"-DNYTRIX_HOST_CFLAGS={host_cflags}",
        f"-DNYTRIX_HOST_LDFLAGS={host_ldflags}",
    ]
    if "NYTRIX_USE_LLVM" in os.environ:
        cmd.append(f"-DNYTRIX_USE_LLVM={os.environ['NYTRIX_USE_LLVM']}")
    if "NYTRIX_USE_GMP" in os.environ:
        cmd.append(f"-DNYTRIX_USE_GMP={os.environ['NYTRIX_USE_GMP']}")
    if host_os() == "windows":
        cc = win_cc or _windows_cmake_tool(os.environ.get("CC") or "")
        cxx = win_cxx or _windows_cmake_tool(os.environ.get("CXX") or "")
        if cc:
            cmd.append(f"-DCMAKE_C_COMPILER={cc}")
        if cxx:
            cmd.append(f"-DCMAKE_CXX_COMPILER={cxx}")
    if which("ninja"):
        cmd += ["-G", "Ninja"]
    cmake_env = None
    if vendor_dir:
        cmake_env = os.environ.copy()
        old = cmake_env.get("LD_LIBRARY_PATH", "")
        cmake_env["LD_LIBRARY_PATH"] = f"{vendor_dir}{':' + old if old else ''}"
    run(cmd, quiet=QUIET_BOOTSTRAP, env=cmake_env)
    boot_ok(f"cmake ({kind}) configured")
    return bdir

def windows_stop_locked_build_targets(bdir: Path, targets: list[str]) -> None:
    if host_os() != "windows":
        return
    exes: list[Path] = []
    for target in targets:
        if target in ("ny", "ny-fmt", "ny-perf", "ny-test", "ny-doc", "ny-make", "ny-lsp"):
            path = bdir / f"{target}.exe"
            if path.exists():
                exes.append(path.resolve())
    if not exes:
        return
    quoted = []
    for path in exes:
        s = str(path).replace("'", "''")
        quoted.append(f"'{s}'")
    paths = "@(" + ",".join(quoted) + ")"
    script = (
        f"$paths = {paths}; "
        "Get-Process -ErrorAction SilentlyContinue | "
        "Where-Object { $_.Path -and ($paths -contains $_.Path) } | "
        "Stop-Process -Force -ErrorAction SilentlyContinue"
    )
    subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
        cwd=str(ROOT),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

def cmake_build(build_root: Path, kind: str, targets: list[str], jobs: int) -> Path:
    bdir = cmake_configure(build_root, kind)
    windows_stop_locked_build_targets(bdir, targets)
    boot_step(f"build {kind}: {', '.join(targets)}")
    run(["cmake", "--build", str(bdir), "--target", *targets, "-j", str(max(1, jobs))], quiet=QUIET_BOOTSTRAP)
    boot_ok(f"build {kind}: {', '.join(targets)} complete")
    return bdir

def cmake_cache_value(bdir: Path, key: str, default: str = "") -> str:
    cache = bdir / "CMakeCache.txt"
    if not cache.exists():
        return default
    prefix = f"{key}:"
    for line in cache.read_text(encoding="utf-8", errors="ignore").splitlines():
        if line.startswith(prefix) and "=" in line:
            return line.split("=", 1)[1].strip()
    return default

def path_tree_writable(path: Path) -> bool:
    probe = path
    while not probe.exists() and probe != probe.parent:
        probe = probe.parent
    return os.access(str(probe), os.W_OK | os.X_OK)

def install_prefix_writable(prefix: Path) -> bool:
    return all(path_tree_writable(prefix / leaf) for leaf in ("bin", "lib", "share"))

def sudo_noninteractive_ok(sudo: str) -> bool:
    return subprocess.run([sudo, "-n", "true"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0

def cmake_install(build_root: Path, kind: str) -> int:
    bdir = cmake_build_dir(build_root, kind)
    prefix = Path(cmake_cache_value(bdir, "CMAKE_INSTALL_PREFIX", "/usr/local"))
    cmd = ["cmake", "--install", str(bdir.resolve())]
    if host_os() != "windows" and hasattr(os, "geteuid") and os.geteuid() != 0 and not install_prefix_writable(prefix):
        sudo = shutil.which("sudo")
        if sudo and sys.stdin.isatty():
            boot_notice(f"install prefix {prefix} needs root; running sudo cmake --install")
            return subprocess.run([sudo, *cmd], cwd=str(ROOT)).returncode
        if sudo and sudo_noninteractive_ok(sudo):
            boot_notice(f"install prefix {prefix} needs root; running sudo -n cmake --install")
            return subprocess.run([sudo, "-n", *cmd], cwd=str(ROOT)).returncode
        err(f"make: install prefix {prefix} is not writable; run: sudo ./make install")
        return 1
    return subprocess.run(cmd, cwd=str(ROOT)).returncode

def resolve_tool_bin(build_root: Path, kind: str, name: str) -> Path:
    exe = ".exe" if host_os() == "windows" else ""
    cands = [
        cmake_build_dir(build_root, kind) / f"{name}{exe}",
        cmake_build_dir(build_root, "release") / f"{name}{exe}",
    ]
    if name == "ny" and kind in ("debug", "asan", "ubsan"):
        cands.insert(0, cmake_build_dir(build_root, kind) / f"ny_debug{exe}")
    for p in cands:
        if p.exists() and os.access(str(p), os.X_OK):
            return p
    raise SystemExit(f"make: tool not found: {name}")

def tool_launch_path(path: Path) -> str:
    try:
        rel = path.resolve().relative_to(ROOT)
    except ValueError:
        return str(path)
    return os.path.join(".", str(rel))

def bootstrap_needed_for_repl(build_root: Path, kind: str, cmds: list[str]) -> bool:
    if not any(c in ("ny", "repl") for c in cmds):
        return False
    bdir = cmake_build_dir(build_root, kind)
    exe = ".exe" if host_os() == "windows" else ""
    return not (bdir / "CMakeCache.txt").exists() or not (bdir / f"ny{exe}").exists()

WEB_DEMO_ASSET_DIR = ROOT / "etc" / "assets" / "website" / "wasm"
WEB_DEMO_STATIC_ASSETS = (
    "index.html",
    "web.css",
    "wasm.js",
)
WEB_DEMO_SHARED_ASSETS = (
    "logo.svg",
    "favicon.svg",
)
WEB_DEMO_FONT_ASSETS = (
    (ROOT / "etc" / "assets" / "fonts" / "monocraft.ttf", Path("assets") / "monocraft.ttf"),
)
WEB_WASM_BARE_TARGET = {
    "kind": "wasm-bare",
    "host": "browser",
    "graphics": "webgl2",
}
WEB_WASM_BARE_CAPABILITIES = {
    "webgl2": True,
    "webgl3dBaseline": True,
    "keyboard": True,
    "mouse": True,
    "frameLoop": True,
    "assetPreload": True,
    "fullscreen": False,
    "fullscreenRequest": True,
    "pointerLock": False,
    "pointerLockRequest": True,
    "touch": True,
    "gamepad": True,
    "audio": True,
    "audioLifecycle": True,
    "filesystem": False,
    "network": False,
    "threads": False,
    "nativeWindow": False,
    "vulkan": False,
}

def _web_target_descriptor(raw: str, command: str) -> dict[str, str]:
    """Resolve the browser target supported by ny's Wasm emitter."""
    target = raw.strip().lower()
    if target == "wasm-bare":
        return dict(WEB_WASM_BARE_TARGET)
    raise SystemExit(
        f"{command}: unknown browser target {raw!r} "
        "(only wasm-bare is supported; use ny --wasm for the canonical emitter)"
    )

def _parse_web_check_args(args: list[str], build_root: Path) -> dict[str, object]:
    """Accept web-check target selection without teaching the general wasm parser."""
    target = "wasm-bare"
    wasm_args: list[str] = []
    i = 0
    while i < len(args):
        arg = args[i]
        if arg == "--target":
            if i + 1 >= len(args):
                raise SystemExit("web-check: missing value for --target")
            target = args[i + 1]
            i += 2
            continue
        if arg.startswith("--target="):
            target = arg.split("=", 1)[1]
            i += 1
            continue
        wasm_args.append(arg)
        i += 1
    cfg = _parse_wasm_args(wasm_args, build_root)
    if not bool(cfg.get("help", False)):
        cfg["target"] = _web_target_descriptor(target, "web-check")
    return cfg

def _demo_id_from_source(source: str) -> str:
    path = Path(source)
    parts = list(path.parts)
    if len(parts) >= 3 and parts[0] == "etc" and parts[1] == "projects":
        parts = parts[2:]
    if parts and parts[-1].endswith(".ny"):
        parts[-1] = parts[-1][:-3]
    raw = "-".join(parts) or "demo"
    out = []
    last_dash = False
    for ch in raw.lower():
        ok = ch.isalnum()
        if ok:
            out.append(ch)
            last_dash = False
        elif not last_dash:
            out.append("-")
            last_dash = True
    return ("".join(out).strip("-") or "demo")

def _demo_title_from_source(source: str) -> str:
    comment_title = _demo_comment_title(source)
    if comment_title:
        return comment_title
    stem = Path(source).stem.replace("_", " ").replace("-", " ").strip()
    words = [w for w in stem.split() if w]
    return " ".join(w[:1].upper() + w[1:] for w in words) or "Demo"

def _demo_area_from_source(source: str) -> str:
    path = Path(source)
    parts = list(path.parts)
    if len(parts) >= 4 and parts[0] == "etc" and parts[1] == "projects":
        return parts[2].upper()
    return "projects"

def _ny_doc_meta(source: str) -> dict[str, object]:
    """Read source metadata through ny-doc rather than a second parser."""
    binary = Path(os.environ.get("NYTRIX_NY_DOC", ROOT / "build" / "release" / "ny-doc"))
    if not binary.exists():
        return {}
    path = _resolve_wasm_path(source) if "_resolve_wasm_path" in globals() else ROOT / source
    try:
        result = subprocess.run(
            [str(binary), "meta", str(path)],
            cwd=str(ROOT),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=10,
            check=False,
        )
        if result.returncode == 0:
            value = json.loads(result.stdout)
            return value if isinstance(value, dict) else {}
    except (OSError, subprocess.TimeoutExpired, json.JSONDecodeError):
        pass
    return {}

def _demo_keywords_from_source(source: str) -> list[str]:
    value = _ny_doc_meta(source).get("keywords", [])
    return [str(item) for item in value] if isinstance(value, list) else []

def _demo_comment_title(source: str) -> str:
    value = _ny_doc_meta(source).get("title", "")
    return str(value).strip() if value else ""

def _demo_mode_from_source(source: str) -> str:
    area = _demo_area_from_source(source).lower()
    for kw in _demo_keywords_from_source(source):
        if kw not in (area, "example", "demo", "nytrix"):
            return kw
    return area

def _load_web_demo_manifest() -> list[dict[str, object]]:
    path = WEB_DEMO_ASSET_DIR / "demos.json"
    raw_items: list[dict[str, object]] = []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        data = []
    except json.JSONDecodeError as exc:
        raise SystemExit(f"web-demos: invalid {path.relative_to(ROOT)}: {exc}") from exc
    if not isinstance(data, list):
        raise SystemExit("web-demos: demos.json must contain a list")
    for idx, item in enumerate(data):
        if not isinstance(item, dict):
            raise SystemExit(f"web-demos: manifest item {idx} is not an object")
        raw_items.append(dict(item))
    seen_ids: set[str] = set()
    out: list[dict[str, object]] = []
    for idx, item in enumerate(raw_items):
        demo_id = str(item.get("id", "")).strip()
        source = str(item.get("source", "")).strip().replace("\\", "/")
        wasm = str(item.get("wasm", "")).strip().replace("\\", "/")
        if not source and not wasm:
            raise SystemExit(f"web-demos: manifest item {idx} needs source or wasm")
        if not demo_id:
            demo_id = _demo_id_from_source(source or wasm)
        if demo_id in seen_ids:
            raise SystemExit(f"web-demos: duplicate demo id {demo_id}")
        seen_ids.add(demo_id)
        item["id"] = demo_id
        if source:
            item["source"] = source
        if wasm:
            item["wasm"] = wasm
        item["title"] = str(item.get("title", "")).strip() or (_demo_title_from_source(source) if source else Path(wasm).stem)
        item["area"] = str(item.get("area", "")).strip() or (_demo_area_from_source(source) if source else "WASM")
        item["mode"] = str(item.get("mode", "")).strip() or (_demo_mode_from_source(source) if source else "browser")
        out.append(item)
    return out

def _load_web_test_manifest() -> list[dict[str, object]]:
    path = ROOT / "etc" / "tests" / "native" / "web" / "tests.json"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit(f"web-test: invalid {path.relative_to(ROOT)}: {exc}") from exc
    if not isinstance(data, list):
        raise SystemExit("web-test: tests.json must contain a list")
    seen: set[str] = set()
    out: list[dict[str, object]] = []
    for index, raw in enumerate(data):
        if not isinstance(raw, dict):
            raise SystemExit(f"web-test: manifest item {index} is not an object")
        item = dict(raw)
        fixture_id = str(item.get("id", "")).strip()
        source = str(item.get("source", "")).strip().replace("\\", "/")
        if not fixture_id or not source:
            raise SystemExit(f"web-test: manifest item {index} needs id and source")
        if fixture_id in seen:
            raise SystemExit(f"web-test: duplicate fixture id {fixture_id}")
        if not (ROOT / source).is_file():
            raise SystemExit(f"web-test: fixture source does not exist: {source}")
        route = str(item.get("route", "app")).strip() or "app"
        required = item.get("required", [])
        required_regex = item.get("required_regex", [])
        forbidden = item.get("forbidden", ["runtime error"])
        if not isinstance(required, list) or not isinstance(required_regex, list) or not isinstance(forbidden, list):
            raise SystemExit(f"web-test: fixture {fixture_id} markers must be lists")
        if not isinstance(item.get("skip_headless", False), bool):
            raise SystemExit(f"web-test: fixture {fixture_id} skip_headless must be boolean")
        item["id"] = fixture_id
        item["source"] = source
        item["route"] = route
        item["virtual_time_ms"] = int(item.get("virtual_time_ms", 8000))
        item["required"] = [str(marker) for marker in required]
        item["required_regex"] = [str(pattern) for pattern in required_regex]
        item["forbidden"] = [str(marker) for marker in forbidden]
        item["skip_headless"] = bool(item.get("skip_headless", False))
        seen.add(fixture_id)
        out.append(item)
    return out

def _parse_web_demo_args(args: list[str], build_root: Path) -> dict[str, object]:
    out_dir = build_root / "web" / "demos"
    compile_ny_wasm = True
    require_ny_wasm = True
    clean = False
    i = 0
    while i < len(args):
        a = args[i]
        if a in ("-h", "--help"):
            return {"help": True}
        if a == "--out":
            if i + 1 >= len(args):
                raise SystemExit("web-demos: missing value for --out")
            out_dir = Path(args[i + 1])
            i += 2
            continue
        if a.startswith("--out="):
            out_dir = Path(a.split("=", 1)[1])
            i += 1
            continue
        if a == "--no-ny-wasm":
            compile_ny_wasm = False
            i += 1
            continue
        if a == "--require-ny-wasm":
            require_ny_wasm = True
            i += 1
            continue
        if a == "--clean":
            clean = True
            i += 1
            continue
        raise SystemExit(f"web-demos: unknown option {a}")
    if not out_dir.is_absolute():
        out_dir = ROOT / out_dir
    return {
        "help": False,
        "out": out_dir,
        "compile_ny_wasm": compile_ny_wasm,
        "require_ny_wasm": require_ny_wasm,
        "clean": clean,
    }

def print_web_demos_help() -> None:
    print(c("1;36", "Nytrix wasm runner"))
    print("")
    print("Usage:")
    print("  ./make web-demos")
    print("  ./make web-demos --out build/web/demos")
    print("")
    print("Flags:")
    print("  --out DIR         output directory")
    print("  --no-ny-wasm      skip compiling manifest Ny sources")
    print("  --require-ny-wasm fail unless every manifest source emits wasm")
    print("  --clean           remove the output directory before writing")

def _demo_wasm_name(demo_id: str) -> str:
    safe = "".join(ch if ch.isalnum() or ch in ("-", "_") else "_" for ch in demo_id.strip())
    return (safe or "demo") + ".wasm"

def _extract_nshape_ny_source(source: Path, out_dir: Path, demo_id: str) -> tuple[Path | None, str]:
    text = source.read_text(encoding="utf-8")
    m = re.search(r"(?m)^[ \t]*source[ \t]+ny[ \t]+<<'([^']+)'[ \t]*\n", text)
    if not m:
        return None, "missing source ny heredoc"
    tag = m.group(1)
    start = m.end()
    end_re = re.compile(r"(?m)^" + re.escape(tag) + r"[ \t]*$")
    end_m = end_re.search(text, start)
    if not end_m:
        return None, f"unterminated source ny heredoc {tag}"
    safe = Path(_demo_wasm_name(demo_id)).with_suffix(".ny").name
    extracted_dir = out_dir / "ny-src"
    extracted_dir.mkdir(parents=True, exist_ok=True)
    extracted = extracted_dir / safe
    extracted.write_text(text[start:end_m.start()], encoding="utf-8")
    return extracted, ""

def _tail_text(value: object, limit: int = 4000) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        value = value.decode("utf-8", "replace")
    return str(value)[-limit:]
def _resolve_wasm_path(path: Path | str) -> Path:
    resolved = Path(path)
    if not resolved.is_absolute():
        resolved = ROOT / resolved
    return resolved.resolve()


WASM_DEFAULT_EXPORTS = (
    "_ny_top_entry",
    "main",
    "ny_web_init",
    "ny_web_main",
    "ny_web_frame",
    "ny_web_render",
)

WASM_ASYNCIFY_IMPORTS = (
    "env.std.os.ui.render.end_frame,"
    "env.std.os.time.msleep"
)

def _run_ny_wasm(
    build_root: Path,
    kind: str,
    source: Path,
    wasm: Path,
    step_timeout: int = 120,
) -> dict[str, object]:
    """Compile one source through ny's canonical Wasm emitter."""
    try:
        ny_bin = resolve_tool_bin(build_root, kind, "ny")
    except SystemExit as exc:
        return {"ok": False, "stage": "toolchain", "detail": str(exc)}
    source = _resolve_wasm_path(source)
    wasm = _resolve_wasm_path(wasm)
    wasm.parent.mkdir(parents=True, exist_ok=True)
    if not source.exists():
        return {"ok": False, "stage": "source", "detail": f"missing source {source}"}
    env = os.environ.copy()
    env.setdefault("NYTRIX_STDLIB", str(cmake_build_dir(build_root, kind) / "std.ny"))
    try:
        result = subprocess.run(
            [str(ny_bin), "--wasm", "--parallel=off", f"--emit-wasm={wasm}", str(source)],
            cwd=str(ROOT),
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=step_timeout,
        )
    except subprocess.TimeoutExpired as exc:
        return {"ok": False, "stage": "ny", "detail": "ny wasm compilation timed out",
                "output": _tail_text(exc.stdout), "timeout": step_timeout}
    output = _tail_text(result.stdout)
    if result.returncode != 0 or not wasm.exists():
        return {"ok": False, "stage": "ny", "detail": "ny wasm compilation failed",
                "output": output}
    return {"ok": True, "source": str(source), "wasm": str(wasm), "output": output}

def _instrument_wasm_asyncify(wasm: Path, step_timeout: int = 120) -> dict[str, object]:
    """Apply browser asyncify after ny has emitted the Wasm module."""
    wasm_opt = which("wasm-opt")
    if not wasm_opt:
        return {"ok": False, "stage": "toolchain", "detail": "wasm-opt missing (install Binaryen for browser frame scheduling)"}
    tmp = wasm.with_suffix(".asyncify.wasm")
    try:
        result = subprocess.run(
            [wasm_opt, str(wasm), "--asyncify",
             "--pass-arg=asyncify-imports@" + WASM_ASYNCIFY_IMPORTS,
             "-o", str(tmp)],
            cwd=str(ROOT),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=step_timeout,
        )
    except subprocess.TimeoutExpired as exc:
        return {"ok": False, "stage": "asyncify", "detail": "wasm-opt asyncify timed out",
                "output": _tail_text(exc.stdout), "timeout": step_timeout}
    if result.returncode != 0 or not tmp.exists():
        return {"ok": False, "stage": "asyncify", "detail": "wasm-opt asyncify failed",
                "output": _tail_text(result.stdout)}
    tmp.replace(wasm)
    return {"ok": True}

def _build_ny_demo_wasm(out_dir: Path, build_root: Path, kind: str, manifest: list[dict[str, object]]) -> tuple[int, int, str]:
    wasm_dir = out_dir / "wasm"
    wasm_dir.mkdir(parents=True, exist_ok=True)
    report: list[dict[str, object]] = []
    built = 0
    failed = 0
    step_timeout = int((os.environ.get("NYTRIX_WASM_STEP_TIMEOUT") or "120").strip() or "120")
    for item in manifest:
        demo_id = str(item.get("id", "demo"))
        source = str(item.get("source", "")).strip()
        if not source:
            report.append({"id": demo_id, "ok": True, "wasm": str(item.get("wasm", "")), "source": ""})
            continue
        compile_source = Path(source)
        if compile_source.suffix == ".nshape":
            nshape_source = compile_source
            if not nshape_source.is_absolute():
                nshape_source = ROOT / nshape_source
            nshape_source = nshape_source.resolve()
            if not nshape_source.exists():
                failed += 1
                report.append({"id": demo_id, "source": source, "ok": False,
                               "stage": "source", "detail": f"missing source {nshape_source}",
                               "output": ""})
                continue
            compile_source, extract_err = _extract_nshape_ny_source(nshape_source, out_dir, demo_id)
            if compile_source is None:
                failed += 1
                item["wasmStatus"] = extract_err
                report.append({"id": demo_id, "source": source, "ok": False,
                               "stage": "source", "detail": extract_err, "output": ""})
                continue
        wasm = wasm_dir / _demo_wasm_name(demo_id)
        result = _run_ny_wasm(build_root, kind, compile_source, wasm, step_timeout)
        if not bool(result.get("ok", False)):
            failed += 1
            item["wasmStatus"] = str(result.get("detail", "wasm failed"))
            report.append({"id": demo_id, "source": source, "ok": False,
                           "stage": str(result.get("stage", "ny")),
                           "detail": str(result.get("detail", "wasm failed")),
                           "output": _tail_text(result.get("output", ""))})
            continue
        if bool(item.get("asyncify", False)):
            async_result = _instrument_wasm_asyncify(wasm, step_timeout)
            if not bool(async_result.get("ok", False)):
                failed += 1
                item["wasmStatus"] = str(async_result.get("detail", "asyncify failed"))
                report.append({"id": demo_id, "source": source, "ok": False,
                               "stage": str(async_result.get("stage", "asyncify")),
                               "detail": str(async_result.get("detail", "asyncify failed")),
                               "output": _tail_text(async_result.get("output", ""))})
                continue
        built += 1
        item["wasm"] = "wasm/" + wasm.name
        item["wasmBase64"] = base64.b64encode(wasm.read_bytes()).decode("ascii")
        item["wasmKind"] = "ny"
        item["wasmStatus"] = "ok"
        report.append({"id": demo_id, "source": source, "ok": True, "wasm": item["wasm"]})
    (out_dir / "ny-wasm-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    source_count = len([x for x in manifest if str(x.get("source", "")).strip()])
    return built, failed, f"{built}/{source_count} manifest Ny sources emitted wasm"


def run_web_demos(build_root: Path, kind: str, args: list[str]) -> int:
    cfg = _parse_web_demo_args(args, build_root)
    if bool(cfg.get("help", False)):
        print_web_demos_help()
        return 0
    out_dir = cfg["out"]
    assert isinstance(out_dir, Path)
    if bool(cfg.get("clean", False)):
        shutil.rmtree(out_dir, ignore_errors=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    manifest = _load_web_demo_manifest()
    for stale_name in ("web-demos.css", "web_demos.css"):
        stale = out_dir / stale_name
        if stale.exists():
            stale.unlink()
    for name in WEB_DEMO_STATIC_ASSETS:
        src = WEB_DEMO_ASSET_DIR / name
        if not src.exists():
            raise SystemExit(f"web-demos: missing {src.relative_to(ROOT)}")
        shutil.copy2(src, out_dir / name)
    for name in WEB_DEMO_SHARED_ASSETS:
        src = WEB_DEMO_ASSET_DIR.parent / name
        if not src.exists():
            raise SystemExit(f"web-demos: missing {src.relative_to(ROOT)}")
        shutil.copy2(src, out_dir / name)
    ny_built = ny_failed = 0
    ny_detail = "disabled"
    if bool(cfg.get("compile_ny_wasm", True)):
        ny_built, ny_failed, ny_detail = _build_ny_demo_wasm(out_dir, build_root, kind, manifest)
    if ny_failed and bool(cfg.get("require_ny_wasm", False)):
        raise SystemExit("web-demos: " + ny_detail)
    (out_dir / "demos-data.js").write_text(
        "window.NYTRIX_WEB_DEMOS = " + json.dumps(manifest, indent=2) + ";\n",
        encoding="utf-8",
    )
    if bool(cfg.get("compile_ny_wasm", True)):
        if ny_built:
            ok("web runner ny wasm: " + ny_detail)
        else:
            log("WEB", "no manifest Ny sources compiled (" + ny_detail + ")")
    ok("web runner: " + _rel_or_abs(out_dir / "index.html"))
    print("Serve or open: " + _rel_or_abs(out_dir / "index.html"))
    return 0

def run_web_test(build_root: Path, kind: str, args: list[str]) -> int:
    """Prove a portable app runs and native-only imports fail before packaging."""
    if args and args[0] in ("-h", "--help"):
        print("Usage: ./make web-test [--headless]")
        print("Builds the demo runner and deployable Pong app, then checks WebGL2 assets in a headless browser.")
        print("--headless skips manifest fixtures that require a live browser lifecycle.")
        return 0
    if args and args != ["--headless"]:
        raise SystemExit("web-test: expected no options or --headless")
    headless_mode = args == ["--headless"] or os.environ.get(
        "NYTRIX_WEB_HEADLESS", ""
    ).strip().lower() in ("1", "true", "yes", "on")
    requested_browser = os.environ.get("NYTRIX_BROWSER", "").strip()
    browser_names = (requested_browser,) if requested_browser else (
        "chromium", "chromium-browser", "google-chrome")
    browser = None if requested_browser == "firefox" else next(
        (which(name) for name in browser_names if name and which(name)), None)
    if not browser and (not requested_browser or requested_browser == "firefox"):
        bundled = sorted(Path.home().glob(".cache/ms-playwright/firefox-*/firefox/firefox"))
        if bundled and bundled[-1].is_file():
            browser = str(bundled[-1])
    if not browser:
        hint = "; install Playwright Firefox with `playwright install firefox`" if requested_browser == "firefox" else ""
        raise SystemExit("web-test: no supported browser found (set NYTRIX_BROWSER or install Chromium/Firefox)" + hint)
    browser_kind = "firefox" if Path(browser).name.startswith("firefox") else "chromium"
    try:
        browser_probe = subprocess.run(
            [browser, "--version"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise SystemExit(f"web-test: browser cannot start: {browser}: {exc}") from exc
    if browser_probe.returncode != 0:
        detail = _tail_text(browser_probe.stdout, 1200).replace("\n", " ").strip()
        raise SystemExit(
            "web-test: browser cannot start; repair the browser installation"
            + (f": {detail}" if detail else "")
        )
    print("WEB browser: " + _tail_text(browser_probe.stdout, 200).strip())

    def run_browser(url: str, virtual_time_ms: int, real_time: bool = False, clipboard: bool = False) -> subprocess.CompletedProcess[str]:
        """Return the rendered DOM using Chromium or Firefox."""
        # Chromium's --dump-dom virtual-time mode does not terminate while
        # these apps keep requestAnimationFrame work pending. Drive Chromium
        # through the runner's existing real-time Playwright path instead so
        # lifecycle completion is bounded and the live DOM remains asserted.
        if real_time or browser_kind == "chromium":
            try:
                from playwright.sync_api import TimeoutError as PlaywrightTimeoutError
                from playwright.sync_api import sync_playwright
            except ImportError as exc:
                raise SystemExit("web-test: browser execution requires Python Playwright") from exc
            try:
                with sync_playwright() as playwright:
                    launcher = playwright.chromium if browser_kind == "chromium" else playwright.firefox
                    launch_args = [
                        "--no-sandbox", "--no-proxy-server", "--enable-webgl",
                        "--ignore-gpu-blocklist", "--enable-unsafe-swiftshader",
                        "--use-gl=angle", "--use-angle=swiftshader",
                    ] if browser_kind == "chromium" else []
                    instance = launcher.launch(headless=True, executable_path=browser, args=launch_args)
                    context_kwargs = {"viewport": None}
                    if clipboard:
                        context_kwargs["permissions"] = ["clipboard-read", "clipboard-write"]
                    context = instance.new_context(**context_kwargs)
                    page = context.new_page()
                    page.goto(url, wait_until="load", timeout=30000)
                    if clipboard:
                        page.wait_for_timeout(1000)
                        for _ in range(15):
                            page.locator("#glCanvas").click(force=True)
                            page.wait_for_timeout(1000)
                    page.wait_for_timeout(virtual_time_ms)
                    dom = page.content()
                    context.close()
                    instance.close()
                    return subprocess.CompletedProcess([browser, url], 0, dom, "")
            except PlaywrightTimeoutError as exc:
                raise subprocess.TimeoutExpired([browser, url], 30) from exc
            except Exception as exc:
                raise SystemExit(f"web-test: browser could not run the page: {exc}") from exc
        try:
            from playwright.sync_api import TimeoutError as PlaywrightTimeoutError
            from playwright.sync_api import sync_playwright
        except ImportError as exc:
            raise SystemExit("web-test: Firefox requires the optional Python Playwright package") from exc
        try:
            with sync_playwright() as playwright:
                instance = playwright.firefox.launch(headless=True, executable_path=browser)
                # Firefox 150 rejects the legacy isMobile field that older
                # Playwright versions send when creating the default context.
                # A non-emulated context keeps the browser test real and avoids
                # that protocol mismatch.
                context_kwargs = {"viewport": None}
                if clipboard:
                    context_kwargs["permissions"] = ["clipboard-read", "clipboard-write"]
                context = instance.new_context(**context_kwargs)
                page = context.new_page()
                page.goto(url, wait_until="load", timeout=30000)
                if clipboard:
                    page.wait_for_timeout(1000)
                    for _ in range(15):
                        page.locator("#glCanvas").click(force=True)
                        page.wait_for_timeout(1000)
                page.wait_for_timeout(virtual_time_ms)
                dom = page.content()
                context.close()
                instance.close()
                return subprocess.CompletedProcess([browser, url], 0, dom, "")
        except PlaywrightTimeoutError as exc:
            raise subprocess.TimeoutExpired([browser, url], 30) from exc
        except Exception as exc:
            raise SystemExit(f"web-test: browser could not run the page: {exc}") from exc

    def run_browser_package(package_dir: Path, virtual_time_ms: int) -> subprocess.CompletedProcess[str]:
        """Serve one packaged browser app and return its rendered DOM."""
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.bind(("127.0.0.1", 0))
            port = int(probe.getsockname()[1])
        server = subprocess.Popen(
            [sys.executable, "-m", "http.server", str(port), "--bind", "127.0.0.1",
             "--directory", str(package_dir)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            time.sleep(0.25)
            return run_browser(f"http://127.0.0.1:{port}/index.html#app", virtual_time_ms)
        finally:
            server.terminate()
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill()

    def run_manifest_fixture(item: dict[str, object]) -> str | None:
        fixture_id = str(item["id"])
        if headless_mode and bool(item.get("skip_headless", False)):
            print(f"WEB skip {fixture_id}: live browser lifecycle required")
            return None
        source = ROOT / str(item["source"])
        fixture_dir = web_dir / fixture_id
        if run_web_check(build_root, kind, [str(source)]) != 0:
            raise SystemExit(f"web-test: portability check failed for {fixture_id}")
        if run_web(build_root, kind, [str(source), "--out", str(fixture_dir)]) != 0:
            raise SystemExit(f"web-test: build failed for {fixture_id}")
        websocket_server = None
        websocket_echo = bool(item.get("websocket_echo", False))
        if websocket_echo:
            handler = (
                "def handler(connection):\n"
                "    for message in connection:\n"
                "        connection.send(message)\n"
            )
            server_code = (
                "from websockets.sync.server import serve\n"
                + handler +
                "with serve(handler, '127.0.0.1', 18790, compression=None) as server:\n"
                "    server.serve_forever()\n"
            )
            websocket_server = subprocess.Popen(
                [sys.executable, "-c", server_code],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            time.sleep(0.25)
            if websocket_server.poll() is not None:
                raise SystemExit(
                    "web-test: websocket echo fixture requires the Python websockets package"
                )
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.bind(("127.0.0.1", 0))
            port = int(probe.getsockname()[1])
        server = subprocess.Popen(
            [sys.executable, "-m", "http.server", str(port), "--bind", "127.0.0.1", "--directory", str(fixture_dir)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        try:
            time.sleep(0.25)
            result = run_browser(
                f"http://127.0.0.1:{port}/index.html#{item['route']}",
                int(item["virtual_time_ms"]),
                bool(item.get("real_time_browser", False) or item.get("clipboard", False)),
                bool(item.get("clipboard", False)),
            )
        except subprocess.TimeoutExpired as exc:
            raise SystemExit(f"web-test: browser timed out for {fixture_id}") from exc
        finally:
            server.terminate()
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill()
            if websocket_server is not None:
                websocket_server.terminate()
                try:
                    websocket_server.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    websocket_server.kill()
        dom = result.stdout or ""
        required = [str(marker) for marker in item["required"]]
        required_regex = [str(pattern) for pattern in item["required_regex"]]
        forbidden = [str(marker) for marker in item["forbidden"]]
        missing = [marker for marker in required if marker not in dom]
        missing_regex = [pattern for pattern in required_regex if re.search(pattern, dom) is None]
        forbidden_found = [marker for marker in forbidden if marker in dom]
        if result.returncode != 0 or missing or missing_regex or forbidden_found:
            print(
                f"web-test: fixture evidence: returncode={result.returncode} "
                f"missing={missing} missing_regex={missing_regex} forbidden={forbidden_found}"
            )
            output = _tail_text(dom, 3000)
            if output:
                print(output)
            raise SystemExit(f"web-test: browser fixture failed: {fixture_id}")
        return dom
    web_dir = build_root / "web"
    shutil.rmtree(web_dir, ignore_errors=True)
    web_dir.mkdir(parents=True, exist_ok=True)
    out_dir = web_dir / "demos"
    if run_web_demos(build_root, kind, ["--out", str(out_dir), "--clean", "--require-ny-wasm"]) != 0:
        return 1
    negative = ROOT / "etc" / "tests" / "native" / "web" / "unsupported-process.ny"
    try:
        run_web_check(build_root, kind, [str(negative)])
    except SystemExit:
        negative_report = build_root / "web" / "check" / "unsupported-process.web-report.json"
        try:
            negative_data = json.loads(negative_report.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise SystemExit("web-test: native-process rejection did not write a valid report") from exc
        if (negative_data.get("ok") is not False or
                negative_data.get("unsupported") != ["std.os.process.run"]):
            raise SystemExit("web-test: native-process rejection report lost its exact unsupported import")
    else:
        raise SystemExit("web-test: native process fixture unexpectedly passed browser portability analysis")
    app_dir = build_root / "web" / "app"
    if run_web(build_root, kind, ["etc/projects/ui/pong.ny", "--out", str(app_dir),
                                  "--assets", "etc/assets/fonts", "--preload-all"]) != 0:
        return 1
    try:
        app_report = json.loads((app_dir / "web-report.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit("web-test: browser app did not write a valid web report") from exc
    capabilities = app_report.get("capabilities", {})
    if any(capabilities.get(name) is not True for name in ("touch", "gamepad", "audio")):
        raise SystemExit("web-test: proven browser capabilities are missing from the target manifest")
    if not (app_dir / "assets" / "monocraft.ttf").is_file():
        raise SystemExit("web-test: runner did not package its declared Monocraft font")
    if not (app_dir / "assets.data").is_file() or not (app_dir / "assets.data.json").is_file():
        raise SystemExit("web-test: browser assets were not packed into assets.data")
    try:
        asset_index = json.loads((app_dir / "assets.data.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit("web-test: browser asset pack index is not valid JSON") from exc
    expected_fonts = sorted(path.relative_to(ROOT).as_posix() for path in (ROOT / "etc" / "assets" / "fonts").rglob("*") if path.is_file())
    if sorted(item.get("path", "") for item in asset_index.get("assets", [])) != expected_fonts:
        raise SystemExit("web-test: --preload-all did not package the complete asset root")
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = int(probe.getsockname()[1])
    server = subprocess.Popen(
        [sys.executable, "-m", "http.server", str(port), "--bind", "127.0.0.1", "--directory", str(app_dir)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(0.25)
        result = run_browser(f"http://127.0.0.1:{port}/index.html#app", 5000)
    except subprocess.TimeoutExpired:
        raise SystemExit("web-test: browser timed out")
    finally:
        server.terminate()
        try:
            server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            server.kill()
    dom = result.stdout if 'result' in locals() else ""
    audio = ROOT / "etc" / "tests" / "native" / "web" / "audio-init.ny"
    if run_web_check(build_root, kind, [str(audio)]) != 0:
        return 1
    audio_report = build_root / "web" / "check" / "audio-init.web-report.json"
    try:
        audio_data = json.loads(audio_report.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit("web-test: audio check did not write a valid report") from exc
    if audio_data.get("ok") is not True or audio_data.get("unsupported") != []:
        raise SystemExit("web-test: browser audio lifecycle imports are not fully hosted")
    requests = ROOT / "etc" / "tests" / "native" / "web" / "window-requests.ny"
    if run_web_check(build_root, kind, [str(requests)]) != 0:
        return 1
    request_report = build_root / "web" / "check" / "window-requests.web-report.json"
    try:
        request_data = json.loads(request_report.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit("web-test: browser window-request check did not write a valid report") from exc
    if request_data.get("ok") is not True or request_data.get("unsupported") != []:
        raise SystemExit("web-test: fullscreen/pointer-lock request imports are not fully hosted")
    required = ("id=\"webglStatus\">WebGL2", "browser runnable", "id=\"audioStatus\">")
    rejected = ("runtime error", "Load failed", "unsupported import", "WebGL2 missing")
    presented = re.search(r'data-presented="[1-9][0-9]*"', dom) is not None
    # Chromium exposes a deterministic Canvas2D readback marker in headless
    # mode. Firefox presents the same WebGL2 frames but does not expose that
    # readback marker in its headless compositor, so presentation is the
    # portable browser-level proof there.
    visible = ('data-frame-pixels="1"' in dom or
               (browser_kind == "firefox" and presented))
    assets_loaded = re.search(r'data-assets-loaded="[1-9][0-9]*"', dom) is not None
    visible_document = 'data-visible="1"' in dom
    audio_state = re.search(r'data-audio-state="(ready|suspended|running)"', dom) is not None
    nearest_present = 'data-present-filter="nearest"' in dom
    canvas_size = re.search(r'data-canvas-size="([0-9]+x[0-9]+)"', dom)
    framebuffer = re.search(r'data-framebuffer="([0-9]+x[0-9]+)"', dom)
    resized = canvas_size is not None and framebuffer is not None and canvas_size.group(1) == framebuffer.group(1)
    if result.returncode != 0 or not presented or not visible or not assets_loaded or not visible_document or not audio_state or not nearest_present or not resized or any(marker not in dom for marker in required) or any(marker in dom for marker in rejected):
        output = _tail_text(dom, 3000)
        if output:
            print(output)
        raise SystemExit("web-test: packaged Pong did not reach the WebGL2 browser runnable state")
    ok("web-test: packaged Pong reached WebGL2 with loaded assets")
    audio_dir = build_root / "web" / "audio"
    if run_web(build_root, kind, [str(audio), "--out", str(audio_dir)]) != 0:
        return 1
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        audio_port = int(probe.getsockname()[1])
    audio_server = subprocess.Popen(
        [sys.executable, "-m", "http.server", str(audio_port), "--bind", "127.0.0.1", "--directory", str(audio_dir)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(0.25)
        audio_result = run_browser(f"http://127.0.0.1:{audio_port}/index.html#app", 5000)
    except subprocess.TimeoutExpired:
        raise SystemExit("web-test: browser timed out while checking audio lifecycle")
    finally:
        audio_server.terminate()
        try:
            audio_server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            audio_server.kill()
    audio_dom = audio_result.stdout if 'audio_result' in locals() else ""
    audio_suspended = 'data-audio-state="suspended"' in audio_dom
    audio_presented = re.search(r'data-presented="[1-9][0-9]*"', audio_dom) is not None
    if audio_result.returncode != 0 or not audio_suspended or not audio_presented or "runtime error" in audio_dom:
        output = _tail_text(audio_dom, 3000)
        if output:
            print(output)
        raise SystemExit("web-test: browser audio did not remain gesture-gated and visible")
    ok("web-test: browser audio is visible and waits for a user gesture")
    decode = ROOT / "etc" / "tests" / "native" / "web" / "audio-decode.ny"
    decode_assets = ROOT / "etc" / "tests" / "native" / "web" / "assets"
    decode_dir = build_root / "web" / "audio-decode"
    if run_web(build_root, kind, [str(decode), "--out", str(decode_dir),
                                  "--assets", str(decode_assets), "--preload-all"]) != 0:
        return 1
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        decode_port = int(probe.getsockname()[1])
    decode_server = subprocess.Popen(
        [sys.executable, "-m", "http.server", str(decode_port), "--bind", "127.0.0.1", "--directory", str(decode_dir)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(0.25)
        decode_result = run_browser(f"http://127.0.0.1:{decode_port}/index.html#app", 32000)
    except subprocess.TimeoutExpired:
        raise SystemExit("web-test: browser timed out while checking audio decode")
    finally:
        decode_server.terminate()
        try:
            decode_server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            decode_server.kill()
    decode_dom = decode_result.stdout if 'decode_result' in locals() else ""
    decode_ok = 'data-audio-decode="1"' in decode_dom
    decode_length = re.search(r'data-audio-decode-length="([1-9][0-9]*)"', decode_dom)
    decode_source = 'data-audio-source-started="1"' in decode_dom
    if decode_result.returncode != 0 or not decode_ok or decode_length is None or not decode_source or "runtime error" in decode_dom:
        output = _tail_text(decode_dom, 3000)
        if output:
            print(output)
        raise SystemExit("web-test: browser did not decode and start the packed audio asset")
    ok("web-test: browser decoded and started the packed audio asset")
    asset_read = ROOT / "etc" / "tests" / "native" / "web" / "filesystem-asset-read.ny"
    if run_web_check(build_root, kind, [str(asset_read)]) != 0:
        return 1
    asset_read_dir = build_root / "web" / "filesystem-asset-read"
    if run_web(build_root, kind, [str(asset_read), "--out", str(asset_read_dir),
                                  "--assets", str(decode_assets), "--preload-all"]) != 0:
        return 1
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        asset_read_port = int(probe.getsockname()[1])
    asset_read_server = subprocess.Popen(
        [sys.executable, "-m", "http.server", str(asset_read_port), "--bind", "127.0.0.1", "--directory", str(asset_read_dir)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(0.25)
        asset_read_result = run_browser(f"http://127.0.0.1:{asset_read_port}/index.html#app", 16000)
    except subprocess.TimeoutExpired:
        raise SystemExit("web-test: browser timed out while checking packaged asset reads")
    finally:
        asset_read_server.terminate()
        try:
            asset_read_server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            asset_read_server.kill()
    asset_read_dom = asset_read_result.stdout if 'asset_read_result' in locals() else ""
    asset_read_ok = re.search(r'data-touch-count="[1-9][0-9]*"', asset_read_dom) is not None
    asset_read_len = re.search(r'data-touch-x="([1-9][0-9]*)"', asset_read_dom)
    if (asset_read_result.returncode != 0 or not asset_read_ok or asset_read_len is None or
            "runtime error" in asset_read_dom):
        output = _tail_text(asset_read_dom, 3000)
        if output:
            print(output)
        raise SystemExit("web-test: packaged asset was not readable through std.os.file_read")
    ok("web-test: packaged asset read through std.os.file_read")
    persistence = ROOT / "etc" / "tests" / "native" / "web" / "filesystem-persistence.ny"
    if run_web_check(build_root, kind, [str(persistence)]) != 0:
        return 1
    persistence_dir = build_root / "web" / "filesystem-persistence"
    if run_web(build_root, kind, [str(persistence), "--out", str(persistence_dir)]) != 0:
        return 1
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        persistence_port = int(probe.getsockname()[1])
    persistence_server = subprocess.Popen(
        [sys.executable, "-m", "http.server", str(persistence_port), "--bind", "127.0.0.1", "--directory", str(persistence_dir)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(0.25)
        persistence_result = run_browser(f"http://127.0.0.1:{persistence_port}/index.html#app", 16000)
    except subprocess.TimeoutExpired:
        raise SystemExit("web-test: browser timed out while checking virtual filesystem persistence")
    finally:
        persistence_server.terminate()
        try:
            persistence_server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            persistence_server.kill()
    persistence_dom = persistence_result.stdout if 'persistence_result' in locals() else ""
    persistence_count = re.search(r'data-touch-count="[1-9][0-9]*"', persistence_dom)
    persistence_written = re.search(r'data-touch-x="([1-9][0-9]*)"', persistence_dom)
    persistence_length = re.search(r'data-touch-y="([1-9][0-9]*)"', persistence_dom)
    persistence_exists = 'data-vfs-exists="1"' in persistence_dom
    persistence_removed = 'data-vfs-removed="1"' in persistence_dom
    if (persistence_result.returncode != 0 or persistence_count is None or persistence_written is None or
            persistence_length is None or not persistence_exists or not persistence_removed or "runtime error" in persistence_dom):
        output = _tail_text(persistence_dom, 3000)
        if output:
            print(output)
        raise SystemExit("web-test: browser virtual filesystem write/read/remove failed")
    ok("web-test: browser virtual filesystem persisted and removed a file")
    test_manifest = _load_web_test_manifest()
    for item in test_manifest:
        if run_manifest_fixture(item) is None:
            continue
        ok(f"web-test: browser fixture passed: {item['id']}")
    renderer3d = ROOT / "etc" / "tests" / "native" / "web" / "renderer-3d.ny"
    target = "wasm-bare"
    if run_web_check(build_root, kind, [str(renderer3d)]) != 0:
        return 1
    renderer3d_dir = build_root / "web" / "renderer-3d-wasm-bare"
    if run_web(build_root, kind, [str(renderer3d), "--out", str(renderer3d_dir)]) != 0:
        return 1
    try:
        renderer3d_result = run_browser_package(renderer3d_dir, 5000)
    except subprocess.TimeoutExpired:
        raise SystemExit(f"web-test: {target} browser timed out while checking the 3D baseline")
    renderer3d_dom = renderer3d_result.stdout
    renderer3d_presented = re.search(r'data-presented="[1-9][0-9]*"', renderer3d_dom) is not None
    if (renderer3d_result.returncode != 0 or not renderer3d_presented or
            'data-webgl3d="1"' not in renderer3d_dom or 'data-webgl3d-alpha="1"' not in renderer3d_dom or "runtime error" in renderer3d_dom or
            "WebGL2 missing" in renderer3d_dom or "Load failed" in renderer3d_dom):
        output = _tail_text(renderer3d_dom, 3000)
        if output:
            print(output)
        raise SystemExit(f"web-test: {target} 3D baseline did not reach the WebGL2 draw path")
    ok(f"web-test: {target} 3D baseline reached the WebGL2 draw path")
    pointer = ROOT / "etc" / "tests" / "native" / "web" / "input-pointer.ny"
    if run_web_check(build_root, kind, [str(pointer)]) != 0:
        return 1
    pointer_dir = build_root / "web" / "input-pointer"
    if run_web(build_root, kind, [str(pointer), "--out", str(pointer_dir)]) != 0:
        return 1
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        pointer_port = int(probe.getsockname()[1])
    pointer_server = subprocess.Popen(
        [sys.executable, "-m", "http.server", str(pointer_port), "--bind", "127.0.0.1", "--directory", str(pointer_dir)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(0.25)
        pointer_result = run_browser(f"http://127.0.0.1:{pointer_port}/index.html#app", 5000)
    except subprocess.TimeoutExpired:
        raise SystemExit("web-test: browser timed out while checking pointer input")
    finally:
        pointer_server.terminate()
        try:
            pointer_server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            pointer_server.kill()
    pointer_dom = pointer_result.stdout if 'pointer_result' in locals() else ""
    pointer_presented = re.search(r'data-presented="[1-9][0-9]*"', pointer_dom) is not None
    if pointer_result.returncode != 0 or not pointer_presented or "runtime error" in pointer_dom:
        output = _tail_text(pointer_dom, 3000)
        if output:
            print(output)
        raise SystemExit("web-test: browser pointer input fixture did not execute")
    ok("web-test: browser pointer input facade executed")
    touch = ROOT / "etc" / "tests" / "native" / "web" / "input-touch.ny"
    if run_web_check(build_root, kind, [str(touch)]) != 0:
        return 1
    touch_dir = build_root / "web" / "input-touch"
    if run_web(build_root, kind, [str(touch), "--out", str(touch_dir)]) != 0:
        return 1
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        touch_port = int(probe.getsockname()[1])
    touch_server = subprocess.Popen(
        [sys.executable, "-m", "http.server", str(touch_port), "--bind", "127.0.0.1", "--directory", str(touch_dir)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(0.25)
        # #touch-selftest makes wasm.js synthesize a TouchEvent sequence; the
        # fixture echoes the observed touch state via test_report_touch into
        # data-touch-* attributes that this regex reads back from --dump-dom.
        touch_result = run_browser(f"http://127.0.0.1:{touch_port}/index.html#touch-selftest", 8000)
    except subprocess.TimeoutExpired:
        raise SystemExit("web-test: browser timed out while checking touch input")
    finally:
        touch_server.terminate()
        try:
            touch_server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            touch_server.kill()
    touch_dom = touch_result.stdout if 'touch_result' in locals() else ""
    touch_presented = re.search(r'data-presented="[1-9][0-9]*"', touch_dom) is not None
    touch_observed = re.search(r'data-touch-count="[1-9][0-9]*"', touch_dom) is not None
    if touch_result.returncode != 0 or not touch_presented or not touch_observed or "runtime error" in touch_dom:
        output = _tail_text(touch_dom, 3000)
        if output:
            print(output)
        raise SystemExit("web-test: browser touch input did not flow through the public facade")
    ok("web-test: browser touch input flowed through the public facade")
    gamepad = ROOT / "etc" / "tests" / "native" / "web" / "input-gamepad.ny"
    if run_web_check(build_root, kind, [str(gamepad)]) != 0:
        return 1
    gamepad_dir = build_root / "web" / "input-gamepad"
    if run_web(build_root, kind, [str(gamepad), "--out", str(gamepad_dir)]) != 0:
        return 1
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        gamepad_port = int(probe.getsockname()[1])
    gamepad_server = subprocess.Popen(
        [sys.executable, "-m", "http.server", str(gamepad_port), "--bind", "127.0.0.1", "--directory", str(gamepad_dir)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(0.25)
        # #gamepad-selftest makes wasm.js inject a standard-mapped fake Gamepad;
        # the fixture maps it through the public facade into data-gamepad-* attrs.
        gamepad_result = run_browser(f"http://127.0.0.1:{gamepad_port}/index.html#gamepad-selftest", 5000)
    except subprocess.TimeoutExpired:
        raise SystemExit("web-test: browser timed out while checking gamepad mapping")
    finally:
        gamepad_server.terminate()
        try:
            gamepad_server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            gamepad_server.kill()
    gamepad_dom = (gamepad_result.stdout if 'gamepad_result' in locals() else "").lower()
    gamepad_count_ok = re.search(r'data-gamepad-count="1"', gamepad_dom) is not None
    gamepad_button_ok = re.search(r'data-gamepad-buttona="1"', gamepad_dom) is not None
    gamepad_leftx_ok = re.search(r'data-gamepad-leftx="0\.5"', gamepad_dom) is not None
    if gamepad_result.returncode != 0 or not gamepad_count_ok or not gamepad_button_ok or not gamepad_leftx_ok or "runtime error" in gamepad_dom:
        output = _tail_text(gamepad_dom, 3000)
        if output:
            print(output)
        raise SystemExit("web-test: browser gamepad mapping did not flow through the public facade")
    ok("web-test: browser gamepad mapping flowed through the public facade")
    framebuf = ROOT / "etc" / "tests" / "native" / "web" / "renderer-framebuffer.ny"
    if run_web_check(build_root, kind, [str(framebuf)]) != 0:
        return 1
    framebuf_dir = build_root / "web" / "renderer-framebuffer"
    if run_web(build_root, kind, [str(framebuf), "--out", str(framebuf_dir)]) != 0:
        return 1
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        framebuf_port = int(probe.getsockname()[1])
    framebuf_server = subprocess.Popen(
        [sys.executable, "-m", "http.server", str(framebuf_port), "--bind", "127.0.0.1", "--directory", str(framebuf_dir)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(0.25)
        framebuf_result = run_browser(f"http://127.0.0.1:{framebuf_port}/index.html#app", 5000)
    except subprocess.TimeoutExpired:
        raise SystemExit("web-test: browser timed out while checking the framebuffer probe")
    finally:
        framebuf_server.terminate()
        try:
            framebuf_server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            framebuf_server.kill()
    framebuf_dom = (framebuf_result.stdout if 'framebuf_result' in locals() else "").lower()
    framebuf_fb = re.search(r'data-framebuffer="(\d+)x(\d+)"', framebuf_dom)
    framebuf_presented = re.search(r'data-presented="[1-9][0-9]*"', framebuf_dom) is not None
    framebuf_probe_ok = bool(
        framebuf_fb and
        re.search(r'\b' + framebuf_fb.group(1) + ' ' + framebuf_fb.group(2) + r'\b', framebuf_dom) is not None
    )
    firefox_framebuf = browser_kind == "firefox" and framebuf_presented
    if (framebuf_result.returncode != 0 or not framebuf_presented or
            not (framebuf_probe_ok or firefox_framebuf) or "runtime error" in framebuf_dom):
        output = _tail_text(framebuf_dom, 3000)
        if output:
            print(output)
        raise SystemExit("web-test: browser framebuffer probe did not round-trip through the host")
    ok("web-test: browser framebuffer probe round-tripped through the host")
    texture = ROOT / "etc" / "tests" / "native" / "web" / "renderer-2d-texture.ny"
    if run_web_check(build_root, kind, [str(texture)]) != 0:
        return 1
    texture_dir = build_root / "web" / "renderer-texture"
    if run_web(build_root, kind, [str(texture), "--out", str(texture_dir)]) != 0:
        return 1
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        texture_port = int(probe.getsockname()[1])
    texture_server = subprocess.Popen(
        [sys.executable, "-m", "http.server", str(texture_port), "--bind", "127.0.0.1", "--directory", str(texture_dir)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(0.25)
        texture_result = run_browser(f"http://127.0.0.1:{texture_port}/index.html#app", 5000)
    except subprocess.TimeoutExpired:
        raise SystemExit("web-test: browser timed out while checking the texture probe")
    finally:
        texture_server.terminate()
        try:
            texture_server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            texture_server.kill()
    texture_dom = (texture_result.stdout if 'texture_result' in locals() else "").lower()
    texture_presented = re.search(r'data-presented="[1-9][0-9]*"', texture_dom) is not None
    texture_triples = [tuple(int(t) for t in g) for g in re.findall(r'\b(\d+) (\d+) (\d+)\b', texture_dom)]
    texture_has_red = any(len(g) == 3 and g[0] >= 250 for g in texture_triples)
    texture_has_dark = any(len(g) == 3 and g[0] <= 4 for g in texture_triples)
    texture_probe_ok = bool(
        re.search(r'\b2 2\b', texture_dom) is not None and
        texture_has_red and texture_has_dark
    )
    if texture_result.returncode != 0 or not texture_presented or not texture_probe_ok or "runtime error" in texture_dom:
        output = _tail_text(texture_dom, 3000)
        if output:
            print(output)
        raise SystemExit("web-test: browser texture probe did not round-trip through the host")
    ok("web-test: browser texture probe round-tripped through the host")
    return 0

def _parse_wasm_args(args: list[str], build_root: Path) -> dict[str, object]:
    if not args or any(a in ("-h", "--help") for a in args):
        return {"help": True}
    source: Path | None = None
    out_path: Path | None = None
    ir_path: Path | None = None
    exports = list(WASM_DEFAULT_EXPORTS)
    allow_undefined = True
    target = "wasm-bare"
    timeout_sec = int((os.environ.get("NYTRIX_WASM_STEP_TIMEOUT") or "120").strip() or "120")
    i = 0
    while i < len(args):
        a = args[i]
        if a in ("-o", "--out"):
            if i + 1 >= len(args):
                raise SystemExit("wasm: missing value for " + a)
            out_path = Path(args[i + 1])
            i += 2
            continue
        if a.startswith("--out="):
            out_path = Path(a.split("=", 1)[1])
            i += 1
            continue
        if a == "--ir":
            if i + 1 >= len(args):
                raise SystemExit("wasm: missing value for --ir")
            ir_path = Path(args[i + 1])
            i += 2
            continue
        if a.startswith("--ir="):
            ir_path = Path(a.split("=", 1)[1])
            i += 1
            continue
        if a == "--timeout":
            if i + 1 >= len(args):
                raise SystemExit("wasm: missing value for --timeout")
            timeout_sec = int(float(args[i + 1]))
            i += 2
            continue
        if a.startswith("--timeout="):
            timeout_sec = int(float(a.split("=", 1)[1]))
            i += 1
            continue
        if a == "--export":
            if i + 1 >= len(args):
                raise SystemExit("wasm: missing value for --export")
            exports.extend(x.strip() for x in args[i + 1].split(",") if x.strip())
            i += 2
            continue
        if a.startswith("--export="):
            exports.extend(x.strip() for x in a.split("=", 1)[1].split(",") if x.strip())
            i += 1
            continue
        if a == "--no-undefined":
            allow_undefined = False
            i += 1
            continue
        if a == "--target":
            if i + 1 >= len(args):
                raise SystemExit("wasm: missing value for --target")
            target = args[i + 1].strip().lower()
            i += 2
            continue
        if a.startswith("--target="):
            target = a.split("=", 1)[1].strip().lower()
            i += 1
            continue
        if a.startswith("-"):
            raise SystemExit(f"wasm: unknown option {a}")
        if source is not None:
            raise SystemExit(f"wasm: unexpected extra argument {a}")
        source = Path(a)
        i += 1
    if source is None:
        raise SystemExit("wasm: missing Ny source")
    if target != "wasm-bare":
        raise SystemExit(f"wasm: unsupported target {target!r} (only wasm-bare is supported)")
    src_abs = Path(source)
    if not src_abs.is_absolute():
        src_abs = ROOT / src_abs
    src_abs = src_abs.resolve()
    stem = src_abs.stem or "app"
    if out_path is None:
        out_path = build_root / "web" / "wasm" / (stem + ".wasm")
    if out_path.suffix.lower() != ".wasm":
        out_path = out_path / (stem + ".wasm")
    if ir_path is None:
        ir_path = build_root / "web" / "ir" / (stem + ".ll")
    return {
        "help": False,
        "source": src_abs,
        "out": (lambda p: (p if p.is_absolute() else ROOT / p).resolve())(out_path),
        "ir": (lambda p: (p if p.is_absolute() else ROOT / p).resolve())(ir_path),
        "exports": tuple(dict.fromkeys(exports)),
        "allow_undefined": allow_undefined,
        "target": target,
        "timeout": max(1, timeout_sec),
    }

def _web_host_import_names() -> set[str]:
    """Return the browser runner's explicitly implemented Wasm host functions."""
    source = (WEB_DEMO_ASSET_DIR / "wasm.js").read_text(encoding="utf-8")
    quoted = re.findall(r'["\']([^"\']+)["\']\s*:', source)
    bare = re.findall(r'^\s*([A-Za-z_][A-Za-z0-9_]*)\s*:', source, flags=re.MULTILINE)
    return set(quoted) | set(bare)

def _ny_wasm_info(
    build_root: Path,
    kind: str,
    wasm: Path,
) -> tuple[set[str] | None, set[str] | None, str]:
    """Query imports and exports through ny's canonical Wasm introspector."""
    try:
        ny_bin = resolve_tool_bin(build_root, kind, "ny")
    except SystemExit as exc:
        return None, None, str(exc)
    result = subprocess.run(
        [str(ny_bin), f"--wasm-info={wasm}"],
        cwd=str(ROOT),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=30,
    )
    if result.returncode != 0:
        return None, None, _tail_text(result.stdout, 1000) or "ny --wasm-info failed"
    imports: set[str] = set()
    for line in result.stdout.splitlines():
        match = re.search(r"<env\.([^>]+)>\s+<-\s+env\.([^\s]+)", line)
        if match:
            imports.add(match.group(2))
    exports = set(re.findall(r'-> "([^"]+)"', result.stdout))
    return imports, exports, ""

def _web_import_category(name: str) -> str:
    if name.startswith("std.os.process."):
        return "native process"
    if name.startswith("std.os.ui.window."):
        return "native window"
    if name.startswith("std.os.ui.render.viewer.") or name.startswith("std.os.ui.render.dump."):
        return "native renderer tooling"
    return "browser host gap"

def run_web_check(build_root: Path, kind: str, args: list[str]) -> int:
    """Compile a Ny source and reject browser imports missing from the WebGL2 host."""
    cfg = _parse_web_check_args(args, build_root)
    if bool(cfg.get("help", False)):
        print("Usage: ./make web-check path/to/app.ny [--target wasm-bare] [--timeout seconds]")
        return 0
    source = cfg["source"]
    assert isinstance(source, Path)
    out_dir = build_root / "web" / "check"
    shutil.rmtree(out_dir, ignore_errors=True)
    if source.suffix == ".nshape":
        extracted, extract_err = _extract_nshape_ny_source(source, out_dir / "extracted", source.stem or "source")
        if extracted is None:
            raise SystemExit("web-check: " + extract_err)
        source = extracted
    out_dir.mkdir(parents=True, exist_ok=True)
    wasm = out_dir / (source.stem + ".wasm")
    result = _run_ny_wasm(
        build_root,
        kind,
        source,
        wasm,
        step_timeout=int(cfg.get("timeout", 120)),
    )
    if not bool(result.get("ok", False)):
        output = _tail_text(result.get("output", ""), 1600)
        if output:
            print(output)
        raise SystemExit("web-check: " + str(result.get("detail", "Wasm compilation failed")))
    imports, exports, detail = _ny_wasm_info(build_root, kind, wasm)
    if imports is None or exports is None:
        raise SystemExit("web-check: " + detail)
    missing = sorted(imports - _web_host_import_names())
    report = {
        "source": _rel_or_abs(source),
        "target": cfg["target"],
        "capabilities": WEB_WASM_BARE_CAPABILITIES,
        "wasm": _rel_or_abs(wasm),
        "imports": sorted(imports),
        "supported": sorted(imports - set(missing)),
        "unsupported": missing,
        "ok": not missing,
    }
    report_path = out_dir / (source.stem + ".web-report.json")
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if missing:
        print("web-check: unsupported browser imports:")
        groups: dict[str, list[str]] = {}
        for name in missing:
            groups.setdefault(_web_import_category(name), []).append(name)
        for category in sorted(groups):
            names = groups[category]
            print(f"  {category}: {len(names)}")
            for name in names[:12]:
                print("    env." + name)
            if len(names) > 12:
                print(f"    ... {len(names) - 12} more (see report)")
        print("web-check: report: " + _rel_or_abs(report_path))
        raise SystemExit("web-check: add a portable adapter or keep this API native-only")
    ok("web-check: " + _rel_or_abs(source) + f" ({len(imports)} host imports supported)")
    log("WEB", "report: " + _rel_or_abs(report_path))
    return 0

def _parse_web_args(args: list[str], build_root: Path) -> dict[str, object]:
    source: Path | None = None
    out_dir: Path | None = None
    assets: list[Path] = []
    preload_all = False
    asyncify = True
    target = "wasm-bare"
    timeout_sec = int((os.environ.get("NYTRIX_WASM_STEP_TIMEOUT") or "120").strip() or "120")
    i = 0
    while i < len(args):
        a = args[i]
        if a in ("-h", "--help"):
            return {"help": True}
        if a in ("-o", "--out"):
            if i + 1 >= len(args):
                raise SystemExit("web: missing value for " + a)
            out_dir = Path(args[i + 1])
            i += 2
            continue
        if a.startswith("--out="):
            out_dir = Path(a.split("=", 1)[1])
            i += 1
            continue
        if a in ("--assets", "--asset-root"):
            if i + 1 >= len(args):
                raise SystemExit("web: missing value for " + a)
            assets.append(Path(args[i + 1]))
            i += 2
            continue
        if a.startswith("--assets=") or a.startswith("--asset-root="):
            assets.append(Path(a.split("=", 1)[1]))
            i += 1
            continue
        if a == "--preload-all":
            preload_all = True
            i += 1
            continue
        if a == "--renderer":
            if i + 1 >= len(args):
                raise SystemExit("web: missing value for --renderer")
            a = "--renderer=" + args[i + 1]
            i += 2
        else:
            i += 1
        if a.startswith("--renderer="):
            if a.split("=", 1)[1].strip().lower() != "webgl2":
                raise SystemExit("web: only --renderer webgl2 is supported")
            continue
        if a.startswith("--target="):
            target = a.split("=", 1)[1]
            continue
        if a == "--target":
            if i >= len(args):
                raise SystemExit("web: missing value for --target")
            target = args[i]
            i += 1
            continue
        if a == "--no-asyncify":
            asyncify = False
            continue
        if a == "--timeout":
            if i >= len(args):
                raise SystemExit("web: missing value for --timeout")
            timeout_sec = int(float(args[i]))
            i += 1
            continue
        if a.startswith("--timeout="):
            timeout_sec = int(float(a.split("=", 1)[1]))
            continue
        if a.startswith("-"):
            raise SystemExit("web: unknown option " + a)
        if source is not None:
            raise SystemExit("web: unexpected extra source " + a)
        source = Path(a)
    if source is None:
        raise SystemExit("web: missing Ny source")
    source = Path(source)
    if not source.is_absolute():
        source = ROOT / source
    source = source.resolve()
    if out_dir is None:
        out_dir = build_root / "web" / (source.stem or "app")
    out_dir = Path(out_dir)
    if not out_dir.is_absolute():
        out_dir = ROOT / out_dir
    out_dir = out_dir.resolve()
    if preload_all and not assets:
        raise SystemExit("web: --preload-all requires at least one --assets directory")
    return {"help": False, "source": source, "out": out_dir, "assets": assets, "preload_all": preload_all,
            "asyncify": asyncify, "timeout": max(1, timeout_sec),
            "target": _web_target_descriptor(target, "web")}

def print_web_help() -> None:
    print(c("1;36", "Nytrix browser build"))
    print("")
    print("  ./make web game.ny --out build/web/game --renderer webgl2")
    print("")
    print("Builds a deployable WebGL2 browser directory through ny --wasm.")
    print("")
    print("Flags:")
    print("  --out DIR            deployment output directory")
    print("  --renderer webgl2    required renderer target (default)")
    print("  --target TARGET      wasm-bare (default)")
    print("  --assets DIR         package an asset root under assets/ (repeatable)")
    print("  --preload-all        package every regular file under each asset root")
    print("  --timeout SECONDS    ny --wasm and browser post-processing limit")

def _copy_web_runner_assets(out_dir: Path) -> None:
    for name in WEB_DEMO_STATIC_ASSETS:
        src = WEB_DEMO_ASSET_DIR / name
        if not src.exists():
            raise SystemExit(f"web: missing {src.relative_to(ROOT)}")
        shutil.copy2(src, out_dir / name)
    for name in WEB_DEMO_SHARED_ASSETS:
        src = WEB_DEMO_ASSET_DIR.parent / name
        if not src.exists():
            raise SystemExit(f"web: missing {src.relative_to(ROOT)}")
        shutil.copy2(src, out_dir / name)
    for src, rel in WEB_DEMO_FONT_ASSETS:
        if not src.exists():
            raise SystemExit(f"web: missing {src.relative_to(ROOT)}")
        dst = out_dir / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)

def run_web(build_root: Path, kind: str, args: list[str]) -> int:
    """Build one portable Ny source into a deployable WebGL2 directory."""
    cfg = _parse_web_args(args, build_root)
    if bool(cfg.get("help", False)):
        print_web_help()
        return 0
    source = cfg["source"]
    out_dir = cfg["out"]
    assert isinstance(source, Path) and isinstance(out_dir, Path)
    compile_source = source
    if source.suffix == ".nshape":
        compile_source, extract_err = _extract_nshape_ny_source(source, out_dir / ".ny-src", source.stem or "app")
        if compile_source is None:
            raise SystemExit("web: " + extract_err)
    out_dir.mkdir(parents=True, exist_ok=True)
    wasm = out_dir / "app.wasm"
    target = cfg["target"]
    assert isinstance(target, dict)
    result = _run_ny_wasm(
        build_root,
        kind,
        compile_source,
        wasm,
        step_timeout=int(cfg["timeout"]),
    )
    if not bool(result.get("ok", False)):
        output = _tail_text(result.get("output", ""), 1600)
        if output:
            print(output)
        raise SystemExit("web: " + str(result.get("detail", "Wasm compilation failed")))
    imports, exports, detail = _ny_wasm_info(build_root, kind, wasm)
    if imports is None or exports is None:
        raise SystemExit("web: " + detail)
    if not bool(cfg["asyncify"]):
        callbacks = {"ny_web_frame", "ny_web_render"}
        if not (callbacks & exports):
            raise SystemExit(
                "web: --no-asyncify requires an exported ny_web_frame or ny_web_render callback; "
                "ordinary main loops must keep Asyncify enabled"
            )
    if bool(cfg["asyncify"]):
        async_result = _instrument_wasm_asyncify(wasm, int(cfg["timeout"]))
        if not bool(async_result.get("ok", False)):
            output = _tail_text(async_result.get("output", ""), 1600)
            if output:
                print(output)
            raise SystemExit("web: " + str(async_result.get("detail", "asyncify failed")))
    missing = sorted(imports - _web_host_import_names())
    if missing:
        raise SystemExit("web: unsupported browser imports: " + ", ".join("env." + name for name in missing[:6]))
    _copy_web_runner_assets(out_dir)
    packaged_assets: list[dict[str, object]] = []
    source_text = source.read_text(encoding="utf-8", errors="replace")
    asset_literals = set(re.findall(r'["\']([^"\']+)["\']', source_text))
    selected_assets: set[Path] = set()
    referenced_assets: set[Path] = set()
    for raw in cfg["assets"]:
        assert isinstance(raw, Path)
        src = Path(raw)
        if not src.is_absolute():
            src = ROOT / src
        src = src.resolve()
        if not src.is_dir():
            raise SystemExit("web: asset root is not a directory: " + _rel_or_abs(src))
        matched = 0
        if bool(cfg["preload_all"]):
            for candidate in src.rglob("*"):
                if not candidate.is_file():
                    continue
                candidate = candidate.resolve()
                try:
                    candidate.relative_to(src)
                except ValueError:
                    continue
                selected_assets.add(candidate)
                matched += 1
        for literal in asset_literals:
            candidate = Path(literal)
            if not candidate.is_absolute():
                candidate = ROOT / candidate
            candidate = candidate.resolve()
            try:
                candidate.relative_to(src)
            except ValueError:
                continue
            if candidate.is_file():
                selected_assets.add(candidate)
                referenced_assets.add(candidate)
                matched += 1
        if not matched:
            qualifier = "files" if bool(cfg["preload_all"]) else "source-referenced files"
            raise SystemExit("web: no " + qualifier + " under asset root: " + _rel_or_abs(src))
    asset_blob = bytearray()
    for src in sorted(selected_assets):
        try:
            rel = src.relative_to(ROOT)
        except ValueError:
            rel = Path("assets") / src.name
        while len(asset_blob) % 16:
            asset_blob.append(0)
        data = src.read_bytes()
        offset = len(asset_blob)
        asset_blob.extend(data)
        packaged_assets.append({
            "path": rel.as_posix(), "offset": offset, "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(), "preload": src in referenced_assets,
        })
    asset_pack: dict[str, object] | None = None
    if packaged_assets:
        pack_path = out_dir / "assets.data"
        pack_path.write_bytes(asset_blob)
        asset_pack = {"format": "nytrix-web-data-v1", "url": "assets.data",
                      "bytes": len(asset_blob), "assets": packaged_assets}
        (out_dir / "assets.data.json").write_text(
            json.dumps(asset_pack, indent=2) + "\n", encoding="utf-8")
    source_display = _rel_or_abs(source)
    demo = {"id": "app", "title": _demo_title_from_source(source_display),
            "area": "APP", "mode": "webgl", "source": source_display,
            "wasm": "app.wasm", "wasmKind": "ny", "asyncify": bool(cfg["asyncify"]),
            "assets": packaged_assets, "assetPack": asset_pack}
    (out_dir / "demos-data.js").write_text("window.NYTRIX_WEB_DEMOS = " + json.dumps([demo], indent=2) + ";\n", encoding="utf-8")
    report = {"source": source_display, "target": target,
              "capabilities": WEB_WASM_BARE_CAPABILITIES,
              "wasm": "app.wasm", "imports": sorted(imports), "unsupported": [], "assets": packaged_assets, "assetPack": asset_pack,
              "asyncify": bool(cfg["asyncify"]), "softDependencies": []}
    (out_dir / "web-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    build_manifest = {"source": source_display, "target": target,
                      "capabilities": WEB_WASM_BARE_CAPABILITIES, "artifact": "app.wasm",
                      "toolchain": {"ny": _rel_or_abs(resolve_tool_bin(build_root, kind, "ny")),
                                    "wasmOpt": which("wasm-opt") or ""},
                      "assets": packaged_assets, "assetPack": asset_pack,
                      "softDependencies": [], "asyncify": bool(cfg["asyncify"])}
    (out_dir / "build-manifest.json").write_text(json.dumps(build_manifest, indent=2) + "\n", encoding="utf-8")
    ok("web: " + _rel_or_abs(out_dir / "index.html"))
    log("WEB", "report: " + _rel_or_abs(out_dir / "web-report.json"))
    return 0

def _rel_or_abs(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(ROOT))
    except Exception:
        return str(path)

_LLVM_TARGETS_BUILT_CACHE: set[str] | None = None

def _tool_path(name: str) -> str:
    return shutil.which(name) or ""

def _tool_status(name: str) -> str:
    path = _tool_path(name)
    return path if path else c("33", "missing")

def _kill_process_group(pid: int) -> None:
    try:
        os.killpg(os.getpgid(pid), signal.SIGTERM)
    except Exception:
        pass
    try:
        os.kill(pid, signal.SIGKILL)
    except Exception:
        pass

def _system_path_excluding_vendor(build_root: Path) -> str:
    vendor_bin = str(build_root / "vendor" / "bin")
    return os.pathsep.join(
        p for p in os.environ.get("PATH", "").split(os.pathsep)
        if p and p != vendor_bin
    ) or "/usr/bin:/bin"

def run_make_targets() -> int:
    """Delegate to ny --targets for cross-compilation target presets."""
    ny_bin = ROOT / "build" / "release" / "ny"
    return subprocess.run([str(ny_bin), "--targets"]).returncode

PROFILE_TIME_RE = re.compile(r"^\s*([A-Za-z][A-Za-z ]+):\s+([0-9]+(?:\.[0-9]+)?)s\s*$")

def run_make_profile(build_root: Path, kind: str, jobs: int, args: list[str]) -> int:
    """Delegate profiling to ny and ny-perf. Time mode uses ny --profile-time."""
    if not args or args[0] in ("-h", "--help"):
        print("Usage: ./make profile <mode> [args]")
        print("Modes: time (ny --profile-time), gprof/asan/ubsan/fuzz (via ny-perf/ny-test)")
        print("  ./make profile time [--runs N] -- <ny args>")
        return 0
    mode = args[0]
    rest = args[1:]
    if mode in ("time", "bench"):
        ny_bin = resolve_tool_bin(build_root, kind, "ny")
        return subprocess.run([str(ny_bin), "--profile-time", *rest]).returncode
    if mode in ("gprof", "fuzz"):
        cmake_build(build_root, kind, ["ny", "ny-perf", "ny-test", "ny-fuzz"], jobs)
        return run_tool(build_root, kind, "ny-perf", ["profile", *rest])
    if mode in ("asan", "ubsan"):
        san_kind = configure_command_environment(mode, kind,
            os.environ.get("NYTRIX_HOST_CFLAGS"), os.environ.get("NYTRIX_HOST_LDFLAGS"),
            os.environ.get("NYTRIX_SKIP_OPTIONAL_GATES"), os.environ.get("NYTRIX_TEST_CACHE"),
            os.environ.get("NYTRIX_TEST_COLD"))
        cmake_build(build_root, san_kind, ["ny", "ny-full", "ny-test"], jobs)
        return run_test(build_root, san_kind, jobs, rest)
    print(f"make profile: mode '{mode}' not yet delegated. Use the full profile pipeline.", file=sys.stderr)
    return 1

def run_tool(build_root: Path, kind: str, name: str, args: list[str], timeout: float | None = None) -> int:
    binp = resolve_tool_bin(build_root, kind, name)
    launch = tool_launch_path(binp)
    env = _vendor_env(build_root)
    interactive_repl = False
    if name == "ny":
        # Keep the pure REPL path minimal. Pointing interactive startup at the
        # bundled std.ny forces broad FFI/pkg-config probing before the prompt.
        interactive_repl = len(args) > 0 and args[0] == "-i"
        if not interactive_repl and _env_flag("NYTRIX_MAKE_USE_PREBUILT_STD", False):
            std_file = cmake_build_dir(build_root, kind) / "std.ny"
            if std_file.exists():
                std_path = str(std_file)
                env.setdefault("NYTRIX_STD_PATH", std_path)
                env.setdefault("NYTRIX_BUILD_STD_PATH", std_path)
                env.setdefault("NYTRIX_STD_PREBUILT", std_path)
        if args and Path(args[0]).name == "engine.ny":
            env.setdefault("NYTRIX_JIT_CACHE", "0")
        env.setdefault("NYTRIX_STD_CACHE", "1")
        env.setdefault("NYTRIX_STD_BC_CACHE_AUTO", "1")
        env.setdefault("NYTRIX_JIT_CACHE", "1")
        if host_os() != "windows" and not interactive_repl and _env_flag("NYTRIX_MAKE_EXEC_TOOL", True):
            try:
                os.chdir(str(ROOT))
                os.execvpe(launch, [launch, *args], env)
            except OSError:
                pass
    # Use process groups so we can kill the entire tree on timeout/interrupt.
    proc = subprocess.Popen([launch, *args], cwd=str(ROOT), env=env,
                            preexec_fn=os.setsid if host_os() != "windows" else None)
    interrupted = False
    rc = 0
    try:
        rc = proc.wait(timeout=timeout)
        return rc
    except subprocess.TimeoutExpired:
        log("TOOL", f"timeout ({timeout}s): killing process group {proc.pid}")
        _kill_process_group(proc.pid)
        rc = 124
        return rc
    except KeyboardInterrupt:
        interrupted = True
        _kill_process_group(proc.pid)
        rc = 130
        return rc
    finally:
        if interrupted or name == "ny" or rc == 130:
            restore_tty_visuals()

def default_fuzz_shape_dir() -> str:
    for rel in ("build/cache/tests/shapes", "etc/tests/shapes", "etc/tests"):
        if (ROOT / rel).exists():
            return rel
    return "build/cache/tests/fuzz/shapes"

def run_test(build_root: Path, kind: str, jobs: int, extra: list[str]) -> int:
    started = time.perf_counter()
    test_jobs = resolve_test_jobs(jobs)
    cold = (os.environ.get("NYTRIX_TEST_COLD") or "").strip().lower() in ("1", "true", "yes", "on")
    ny_bin = resolve_tool_bin(build_root, kind, "ny")
    trace_dir = _nytrix_cache_root("test-trace")
    shutil.rmtree(trace_dir, ignore_errors=True)
    trace_dir.mkdir(parents=True, exist_ok=True)
    os.environ["NYTRIX_TEST_TRACE_DIR"] = str(trace_dir)
    os.environ["NYTRIX_TEST_PROFILE_JSON"] = str(trace_dir / "profile.json")
    os.environ["NYTRIX_TEST_INCLUDE_BENCHMARK"] = "1"
    os.environ["NYTRIX_TEST_NATIVE"] = "1"
    os.environ["NYTRIX_TEST_AOT_REUSE_NATIVE"] = "0"
    os.environ["NYTRIX_TEST_BENCHMARK_NATIVE"] = "1"
    os.environ["NYTRIX_TEST_RUNTIME_NATIVE"] = "1"
    os.environ["NYTRIX_TEST_STD_NATIVE"] = "1"
    os.environ["NYTRIX_TEST_BENCHMARK_REPL"] = "1"
    os.environ["NYTRIX_TEST_RUNTIME_REPL"] = "1"
    os.environ["NYTRIX_TEST_STD_REPL"] = "1"
    os.environ["NYTRIX_TEST_CACHE"] = "0" if cold else "1"
    exec_cache = (os.environ.get("NYTRIX_TEST_EXEC_CACHE") or "").strip().lower() in ("1", "true", "yes", "on")
    os.environ["NYTRIX_TEST_NO_NATIVE_CACHE"] = "1" if (cold or not exec_cache) else "0"
    os.environ["NYTRIX_AOT_CACHE"] = "1" if (exec_cache and not cold) else "0"
    os.environ["NYTRIX_JIT_CACHE"] = "1" if (exec_cache and not cold) else "0"
    os.environ["NYTRIX_STD_CACHE"] = "0" if cold else os.environ.get("NYTRIX_STD_CACHE", "1")
    cache_mode = "off" if cold else "on"
    exec_cache_mode = "on" if (exec_cache and not cold) else "off"
    std_cache_mode = "off" if cold else ("off" if os.environ.get("NYTRIX_STD_CACHE") == "0" else "on")
    log("TEST", f"make test: fixture-flag matrix with runtime/repl/error/bench suites; result_cache {cache_mode}, exec_cache {exec_cache_mode}, std_cache {std_cache_mode} (set NYTRIX_TEST_EXEC_CACHE=1 to enable binary caches)")
    # NYTRIX_TEST_TIMEOUT belongs to ny-test and limits each fixture.  Keep the
    # outer suite deadline independent so a large, healthy suite is not killed
    # after one fixture's allowance (notably on slower Windows runners).
    suite_timeout_s = int(os.environ.get("NYTRIX_TEST_SUITE_TIMEOUT") or "1800")
    step(f"run tests: bin=ny jobs={test_jobs} suite_timeout={suite_timeout_s}s")
    rc = run_tool(build_root, kind, "ny-test", ["--bin", str(ny_bin), "--jobs", str(test_jobs), *extra], timeout=float(suite_timeout_s))
    if rc == 0 and host_os() != "windows":
        rc = run_tool(build_root, kind, "ny-fuzz", ["validate-shapes", "etc/tests/shapes"], timeout=float(suite_timeout_s))
    suite_rc = rc
    if not _env_flag("NYTRIX_TEST_NO_BENCH", False):
        step("run bench: appending the C-vs-Ny benchmark table (NYTRIX_TEST_NO_BENCH=1 to skip)")
        bench_rc = run_tool(build_root, kind, "ny-test", ["--bin", str(ny_bin), "--bench"], timeout=float(suite_timeout_s))
    else:
        bench_rc = 0
    elapsed_ms = int((time.perf_counter() - started) * 1000.0)
    if suite_rc == 0 and bench_rc == 0:
        ok(f"test suite completed in {elapsed_ms}ms")
    else:
        log("TEST", f"test suite failed after {elapsed_ms}ms (suite={suite_rc}, bench={bench_rc})")
    return suite_rc or bench_rc

def run_optcheck(build_root: Path, kind: str, args: list[str]) -> int:
    """Run native optimization correctness nshape tests."""
    if args and args[0] in ("-h", "--help"):
        print("Usage: ./make optcheck")
        print("  Runs native oracle nshape tests from etc/tests/native/optcheck/")
        print("  Add new kernels there as .nshape files with --native-result-oracle.")
        return 0
    if args:
        raise SystemExit("optcheck: no arguments are supported")
    test_bin = resolve_tool_bin(build_root, kind, "ny-test")
    env = _vendor_env(build_root)
    env.setdefault("NYTRIX_STD_CACHE", "0")
    env.setdefault("NYTRIX_STD_BC_CACHE_AUTO", "0")
    opt_dir = ROOT / "etc" / "tests" / "native" / "optcheck"
    if not opt_dir.is_dir():
        raise SystemExit("optcheck: missing fixture directory " + str(opt_dir))
    result = subprocess.run(
        [str(test_bin), "--with-stdlib", "--color=never", str(opt_dir)],
        cwd=str(ROOT),
        env=env,
        timeout=300,
    )
    if result.returncode != 0:
        raise SystemExit("optcheck: ny-test directory run failed")
    ok("optcheck: ny-test completed native oracle directory")
    return 0

def parse(argv: list[str]) -> tuple[list[str], list[str], int, bool, bool, bool, bool, str | None, bool | None]:
    known = {"all", "bin", "bin-static", "tar", "vendor", "fmt", "std", "std_bc", "test", "repl", "fuzz", "bench", "docs", "web", "web-demos", "web-check", "web-test", "wasm", "c2ny", "py2ny", "install", "uninstall", "clean", "debug", "tidy", "audit", "perf", "profile", "gprof", "asan", "ubsan", "optcheck", "analyze", "check", "fb", "ny", "run", "release", "static", "deps", "cross", "cross-run", "env", "targets", "doctor"}
    _PASSTHROUGH = {"fmt", "analyze", "check", "tidy", "audit", "test", "perf", "profile", "docs", "web", "web-demos", "web-check", "web-test", "wasm", "ny", "repl", "gprof", "asan", "ubsan", "fuzz", "bench", "cross", "cross-run", "static", "bin-static", "tar", "vendor", "env", "targets", "doctor"}

    def looks_like_ny_source(arg: str) -> bool:
        if not arg or arg == "--" or arg.startswith("-"):
            return False
        path = Path(arg) if Path(arg).is_absolute() else ROOT / arg
        return arg.endswith(".ny") or path.exists()

    def implicit_ny_invocation(raw: list[str]) -> bool:
        # Allow fast direct usage:
        #   ./make etc/projects/ui/term.ny -h
        #   ./make -trace etc/projects/ui/term.ny -vk
        # without treating -h as make's own help or rejecting the source path as
        # an unknown make target. Stop as soon as an explicit make command appears.
        for item in raw:
            if item == "--":
                continue
            if item in ("help", *known):
                return False
            if looks_like_ny_source(item):
                return True
        return False

    source_passthrough = implicit_ny_invocation(argv)
    cmds: list[str] = []
    extra: list[str] = []
    jobs = 0
    verbose = False
    help_flag = False
    help_target: str | None = None
    version = False
    color_mode: str | None = None
    bootstrap_logs: bool | None = None
    i = 0
    had_unknown_nonflag = False
    last_cmd: str | None = None
    while i < len(argv):
        a = argv[i]
        if a in ("-h", "--help"):
            if source_passthrough or (last_cmd and last_cmd in _PASSTHROUGH):
                extra.append(a)
            else:
                help_flag = True
                if last_cmd:
                    help_target = last_cmd
        elif a == "help":
            if last_cmd and last_cmd in _PASSTHROUGH:
                extra.append(a)
            else:
                help_flag = True
                if last_cmd:
                    help_target = last_cmd
        elif a == "--version":
            version = True
        elif a in ("-v", "--verbose"):
            if source_passthrough:
                extra.append(a)
            else:
                verbose = True
        elif a == "--bootstrap-logs":
            bootstrap_logs = True
        elif a == "--no-bootstrap-logs":
            bootstrap_logs = False
        elif a == "--no-color":
            color_mode = "never"
        elif a == "--color" or a.startswith("--color="):
            if a.startswith("--color="):
                v = a.split("=", 1)[1]
            else:
                i += 1
                if i >= len(argv):
                    raise SystemExit("make: missing value for --color")
                v = argv[i]
            color_mode = parse_color_mode(v)
        elif a in ("-j", "--jobs") or a.startswith("--jobs="):
            if a.startswith("--jobs="):
                v = a.split("=", 1)[1]
            else:
                i += 1
                if i >= len(argv):
                    raise SystemExit("make: missing value for --jobs")
                v = argv[i]
            try:
                jobs = int(v)
            except Exception:
                raise SystemExit(f"make: invalid jobs value: {v}")
        elif a in ("static", "vendor", "cross", "cross-run", "doctor", "profile", "web", "web-demos", "web-check", "web-test", "wasm"):
            cmds.append(a)
            last_cmd = a
            rest = argv[i + 1 :]
            extra.extend(rest)
            break
        elif a == "ny":
            cmds.append("ny")
            last_cmd = "ny"
            rest = argv[i + 1 :]
            extra.extend(rest)
            break
        elif a in known:
            mapped = "ny" if a == "run" else a
            cmds.append(mapped)
            last_cmd = mapped
        else:
            extra.append(a)
            if not a.startswith("-"):
                had_unknown_nonflag = True
        i += 1

    if not cmds:
        source_like = any(looks_like_ny_source(a) for a in extra)
        if extra and (not had_unknown_nonflag or source_like):
            cmds = ["ny"]
            if source_like:
                help_flag = False
        elif not extra:
            cmds = ["all"]
        else:
            raise SystemExit("make: unknown command or target " + " ".join(extra))
    if "debug" in cmds:
        kind_debug = True
        cmds = [c for c in cmds if c != "debug"]
    elif "release" in cmds:
        kind_debug = False
        cmds = [c for c in cmds if c != "release"]
    else:
        kind_debug = False
    return cmds, extra, jobs, verbose, help_flag, help_target, version, kind_debug, color_mode, bootstrap_logs

# Per-command help registry. Each entry: (usage, summary, sections) where
# sections is a list of (heading, rows) and rows are (left, right) text pairs.
_COMMAND_USAGE: dict[str, tuple[str, str, list[tuple[str, list[tuple[str, str]]]]]] = {
    "all": (
        "all",
        "configure and build ny, std, and tools (default target)",
        [
            ("What it does", [
                ("build", "forces the full tool set: ny, std, ny-fmt, ny-perf, ny-test, ny-doc, ny-make, ny-lsp"),
                ("step x2/runs", "runs a conservative ny-fmt --bugs audit over lib/ (diagnostic only)"),
            ]),
            ("Build options", [
                ("-j, --jobs N", "parallel build jobs"),
                ("-v, --verbose", "print subcommands"),
                ("--color MODE", "auto | always | never"),
                ("--bootstrap-logs", "show bootstrap status"),
                ("--debug", "build the debug, not release, configuration"),
                ("--clean", "remove build artifacts first"),
            ]),
            ("Env overrides", [
                ("NYTRIX_HOST_CFLAGS", "extra compiler flags for host build"),
                ("NYTRIX_BUILD_JOBS", "default job count"),
                ("CC / LC", "preferred host C compiler"),
                ("LLVM_CONFIG", "preferred LLVM location"),
            ]),
            ("Examples", [
                ("./make all", "build everything in release"),
                ("./make --debug all", "build everything in debug"),
            ]),
        ],
    ),
    "bin": (
        "Usage: ./make bin [options]",
        "build the ny executable only",
        [
            ("What it does", [
                ("Targets", "builds ny, std, ny-fmt, ny-perf, ny-test, ny-doc, ny-make, ny-lsp"),
                ("Result", "writes the compiler to build/<kind>/ny"),
            ]),
            ("Options", [
                ("-j, --jobs N", "parallel build jobs"),
                ("-v, --verbose", "print subcommands"),
                ("--debug", "build the debug configuration"),
            ]),
            ("Examples", [
                ("./make bin", "build the compiler"),
                ("./make --debug bin", "build the debug compiler"),
            ]),
        ],
    ),
    "static": (
        "Usage: ./make static <subcommand> [args]",
        "bundled/portable build operations (static bin, tar, vendor, check)",
        [
            ("Subcommands", [
                ("bin", "build compiler/tools and bundle runtime .so files into build/static"),
                ("libs [path]", "bundle shared libs beside build/release or a given path"),
                ("check <binary>", "report whether an ELF is static or dynamic"),
                ("ny <file.ny> [flags]", "compile a Ny program with static link flags"),
                ("binstatic", "compile an arbitrary C program with the bundled toolchain"),
            ]),
            ("Environment", [
                ("CC", "the C compiler to use for static builds"),
                ("NYTRIX_ROOT", "repository root"),
            ]),
            ("Examples", [
                ("./make static bin", "build everything statically"),
                ("./make static libs build/release", "refresh bundled shared libs"),
                ("./make static check build/release/ny", "check the ny ELF type"),
            ]),
        ],
    ),
    "bin-static": (
        "Usage: ./make bin-static",
        "alias for ./make static bin",
        [
            ("What it does", [
                ("Result", "builds compiler/tools and bundles runtime .so files into build/static"),
            ]),
        ],
    ),
    "tar": (
        "Usage: ./make tar [--with-binaries]",
        "create a source (or self-contained binary) release tarball",
        [
            ("Options", [
                ("--with-binaries", "also bundle prebuilt portable binaries into nytrix-static tarball"),
                ("--out PATH", "override the output tarball path"),
            ]),
            ("Output", [
                ("build/dist/nytrix-source.tar.gz", "source tarball by default"),
                ("build/dist/nytrix-static.tar.gz", "binary tarball when --with-binaries"),
            ]),
        ],
    ),
    "vendor": (
        "Usage: ./make vendor",
        "bundle shared libraries into build/vendor/ for portability",
        [
            ("What it does", [
                ("Bundle", "copies the auto-detected LLVM/GMP/runtime shared libraries beside the tools"),
            ]),
        ],
    ),
    "fmt": (
        "Usage: ./make fmt [ny-fmt args]",
        "format Nytrix sources with the ny-fmt formatter",
        [
            ("What it does", [
                ("Formatter", "runs ny-fmt over the current sources"),
            ]),
            ("Options", [
                ("--check", "verify formatting without rewriting"),
                ("--audit", "run deep source audit/analysis"),
                ("--bugs", "report suspicious code patterns"),
                ("--limit N", "cap the number of reported findings"),
            ]),
            ("Examples", [
                ("./make fmt", "format sources"),
                ("./make fmt --check", "verify formatting in CI"),
            ]),
        ],
    ),
    "analyze": (
        "Usage: ./make analyze [path]",
        "run ny-fmt in an analysis/report mode",
        [
            ("Options", [
                ("--limit N", "cap reported findings"),
                ("path", "limit analysis to a file or directory"),
            ]),
        ],
    ),
    "check": (
        "Usage: ./make check [path]",
        "run ny-fmt in check (parse/verification) mode",
        [
            ("Examples", [
                ("./make check", "verify the whole tree parses"),
                ("./make check lib/math", "verify a subtree only"),
            ]),
        ],
    ),
    "tidy": (
        "Usage: ./make tidy [ny-fmt args]",
        "run ny-fmt --tidy to clean up diagnostics across sources",
        [
            ("Examples", [
                ("./make tidy", "tidy all sources"),
            ]),
        ],
    ),
    "audit": (
        "Usage: ./make audit [ny-fmt args]",
        "run ny-fmt --audit deep source audit over lib/ and triggers",
        [
            ("What it does", [
                ("Bug scan", "looks for unused bindings, missing errors, obvious smell patterns"),
            ]),
        ],
    ),
    "test": (
        "Usage: ./make test [ny-test options] [patterns]",
        "run the full Nytrix test suite (runtime, native, interop, probe, error, bench)",
        [
            ("What it does", [
                ("Suite", "runs ny-test over etc/tests/runtime|errors|bench|native|interop|shapes and lib"),
                ("Bench", "after a green suite, appends the timed C-vs-Ny benchmark table (NYTRIX_TEST_NO_BENCH=1 to skip)"),
                ("Native", "forces native fixtures/REPL/stdll runs (NYTRIX_TEST_NATIVE=1)"),
                ("Shapes", "after the suite, runs ny-fuzz validate-shapes over etc/tests/shapes"),
            ]),
            ("ny-test options", [
                ("--list-bench", "list bench fixture stems (discovery)"),
                ("--bench", "run the C-vs-Ny benchmark suite instead of the test suite"),
                ("--bench-run N", "timed samples per benchmark (default 5)"),
                ("--bench-warmup N", "warm-up runs per benchmark (default 2)"),
                ("--bench-opt LEVEL", "compiler opt flag, e.g. O2 (default)"),
                ("--bench-target T", "native backend target (default x86_64)"),
                ("--bench-out-csv F", "write benchmark results to CSV"),
                ("--bench-out-js/io F", "write benchmark results to JSON"),
                ("--bench-out-md F", "write benchmark results to Markdown"),
                ("--smoke / --no-smoke", "restrict/allow the full suite"),
                ("--with-stdlib / --no-stdlib", "include lib/ fixtures"),
                ("--jobs N", "parallel fixture jobs"),
                ("--timeout SEC", "per-fixture timeout cap"),
                ("--pattern PAT", "run only fixtures whose path contains PAT"),
                ("--phase-times", "report per-phase timing"),
                ("--trace-ir", "trace NYIR through fixtures"),
                ("--failures-only", "only print failing fixtures"),
            ]),
            ("Env overrides", [
                ("NYTRIX_TEST_COLD=1", "cold run, disable caches"),
                ("NYTRIX_TEST_EXEC_CACHE=1", "enable the binary cache during test"),
                ("NYTRIX_TEST_SUITE_TIMEOUT", "outer suite deadline (default 1800s)"),
                ("NYTRIX_TEST_TIMEOUT=NNN", "default per-fixture timeout"),
            ]),
            ("Examples", [
                ("./make test", "run the whole suite"),
                ("./make test --pattern dict", "run only the dict fixtures"),
                ("./make test --bench", "run the benchmark suite"),
                ("./make test --bench --bench-out-csv results.csv", "benchmark to CSV"),
            ]),
        ],
    ),
    "bench": (
        "Usage: ./make bench [ny-test bench options]",
        "run the public C-vs-value benchmark shapes via ny-test --bench default path",
        [
            ("What it does", [
                ("Runner", "runs the C-vs-Ny benchmark shapes (etc/tests/bench)"),
            ]),
            ("Options", [
                ("--list-bench", "list bench fixture stems without running"),
                ("PATTERN ...", "fixture name substring filter (repeatable)"),
                ("--bench-run N", "timed samples per fixture (default 1; use 3+ for stable statistics)"),
                ("--bench-warmup N", "warm-up runs before timing (default 0; use 1+ for hot-code measurements)"),
                ("--bench-opt LEVEL", "compiler opt level 0..3 (default 2)"),
                ("--bench-target T", "restrict to a sub-target of fixtures"),
                ("--bench-compile-profile P", "compiler opt profile (default/speed/balanced/peak); peak enables the proven fast int paths (~100x on int loops)"),
                ("--bench-show-ir", "dump the optimized NYIR for each fixture"),
                ("--bench-show-asm", "dump x86-64 asm for each fixture"),
                ("--bench-show-passes", "show per-pass NYIR transformation stats"),
                ("--bench-profile", "print per-phase compile timing (JIT profile)"),
                ("--bench-out-csv PATH", "write a CSV results table"),
                ("--bench-out-json PATH", "write a JSON results table"),
                ("--bench-out-md PATH", "write a Markdown results table"),
            ]),
        ],
    ),
    "fuzz": (
        "Usage: ./make fuzz [subcommand] [args]",
        "run shape validation and smoke fuzzing",
        [
            ("Subcommands", [
                ("validate-shapes DIR", "validate the deterministic test shapes (default: etc/tests/shapes)"),
                ("afl", "launch afl-fuzz with the given arguments"),
                ("<args>", "pass straight to ny-fuzz"),
            ]),
            ("Examples", [
                ("./make fuzz", "validate default shapes"),
                ("./make fuzz validate-shapes etc/tests/shapes", "validate shapes"),
                ("./make fuzz afl -- -i in -o out -- bin", "launch afl-fuzz"),
            ]),
        ],
    ),
    "asan": ("Usage: ./make asan", "build with Address Sanitizer and run the test suite", []),
    "ubsan": ("Usage: ./make ubsan", "build with Undefined Behavior Sanitizer and run the test suite", []),
    "perf": (
        "Usage: ./make perf [ny-perf args]",
        "run the performance toolkit",
        [
            ("Subcommands", [
                ("profile", "sample or linuz / flame the running program"),
                ("report", "summarize a stored profile"),
            ]),
        ],
    ),
    "gprof": (
        "Usage: ./make gprof [file.ny]",
        "compile a source with -pg profiling enabled and run it",
        [
            ("What it does", [
                ("Profile", "enables gprof (--gprof) instrumentation and runs the fixture"),
            ]),
        ],
    ),
    "profile": (
        "Usage: ./make profile <mode> [options]",
        "time, compile, perf, GDB, sanitizer, and fuzz wrappers around ny",
        [
            ("Modes", [
                ("time <file.ny>", "time compile-and-run across N runs"),
                ("compile <file.ny>", "time only the compile phase"),
                ("perf <file.ny>", "run under linux perf / callgraph"),
                ("report", "summarize stored profiles"),
                ("gdb <file.ny>", "run under GDB"),
                ("gprof <file.ny>", "profile with gprof"),
                ("asan", "Address-Sanitizer build + test"),
                ("ubsan", "UBSan build + test"),
                ("fuzz [DIR]", "run ny-fuzz validate-shapes"),
                ("afl --", "launch afl-fuzz with the given args"),
            ]),
        ],
    ),
    "optcheck": (
        "Usage: ./make optcheck",
        "run native oracle nshape tests from etc/tests/native/optcheck/",
        [],
    ),
    "docs": (
        "Usage: ./make docs [ny-doc args]",
        "build the documentation portal",
        [
            ("Options", [
                ("-o DIR", "output directory (default build/docs)"),
            ]),
            ("Examples", [
                ("./make docs", "build the docs"),
                ("./make docs -o build/docs/out", "build to a custom dir"),
            ]),
        ],
    ),
    "web": (
        "Usage: ./make web <file.ny> [--out PATH]",
        "compile one Ny source into a deployable WebGL2 browser app",
        [
            ("Options", [
                ("--out PATH", "output deployment directory"),
            ]),
            ("Examples", [
                ("./make web etc/projects/ui/pong.ny", "build a WebGL app"),
            ]),
        ],
    ),
    "wasm": (
        "Usage: ./make wasm <file.ny> [--out PATH]",
        "compile one Ny source file to WebAssembly",
        [
            ("Options", [
                ("--out PATH", "output .wasm path"),
            ]),
        ],
    ),
    "web-check": ("Usage: ./make web-check <file.ny>", "verify a Ny source uses only implemented browser host APIs", []),
    "web-test": ("Usage: ./make web-test", "build and prove a Pong app reaches the WebGL2 browser runner", []),
    "web-demos": ("Usage: ./make web-demos", "build the browser WebGL/Wasm demo portal", []),
    "c2ny": (
        "Usage: ./make c2ny <file.c> [-o out.ny]",
        "translate a C file to Nytrix source via ny-fmt --c2ny",
        [
            ("Examples", [
                ("./make c2ny hello.c -o hello.ny", "translate hello.c"),
            ]),
        ],
    ),
    "py2ny": (
        "Usage: ./make py2ny <file.py> [-o out.ny]",
        "translate a Python file to Nytrix source via ny-fmt --py2ny",
        [],
    ),
    "ny": (
        "Usage: ./make ny [file.ny] [ny flags] [program args]",
        "launch the compiler/REPL; runs a source directly with a cached start",
        [
            ("Arguments", [
                ("file.ny", "source file to run; omit to open the REPL"),
                ("ny flags", "compiler flags passed to ny (see ny --help)"),
                ("-- [args]", "force all following to be program args"),
            ]),
            ("Examples", [
                ("./make ny", "open the REPL"),
                ("./make ny hello.ny", "compile and run hello.ny"),
                ("./make ny engine.ny -v -gl", "run with compiler flags after the source"),
            ]),
        ],
    ),
    "repl": (
        "Usage: ./make repl",
        "launch the interactive REPL with a fast cached start",
        [
            ("Examples", [
                ("./make repl", "start the interactive line-editing REPL"),
            ]),
        ],
    ),
    "run": (
        "Usage: ./make run <file.ny> [args]",
        "shorthand for ./make ny",
        [],
    ),
    "install": (
        "Usage: ./make install",
        "install ny and ny-lsp (and tools) into the system prefix",
        [],
    ),
    "uninstall": (
        "Usage: ./make uninstall",
        "remove the installfiles reported by the install manifest",
        [],
    ),
    "clean": (
        "Usage: ./make clean",
        "remove the build/ directory and generated artifacts",
        [],
    ),
    "deps": (
        "Usage: ./make deps",
        "ensure required build dependencies are installed",
        [],
    ),
    "env": (
        "Usage: ./make env",
        "print effective paths, tools, overrides, and caches",
        [
            ("What it prints", [
                ("Environment", "root, host, build.kind, build.dir, jobs, caches"),
                ("Tools", "ny, ny-fmt, ny-test, ny-doc, ny-perf and cmake/ninja/clang/llvm"),
                ("Overrides", "active NYTRIX_* environment overrides"),
            ]),
        ],
    ),
    "targets": (
        "Usage: ./make targets",
        "show cross-compilation target guidance (delegates to ny --doctor)",
        [],
    ),
    "doctor": (
        "Usage: ./make doctor",
        "delegate to ny --doctor for system toolchain diagnostics",
        [
            ("Examples", [
                ("./make doctor", "report setup issues"),
                ("./make doctor --help", "show ny --doctor help"),
            ]),
        ],
    ),
}

def _print_help_rows(rows: list[tuple[str, str]], indent: str = "  ") -> None:
    # Split rows whose left column is too wide so the two columns stay aligned.
    lefts = [l for l, _ in rows if l]
    width = max(len(l) for l in lefts) if lefts else 1
    for left, right in rows:
        if not left:
            print(indent + c("90", right))
            continue
        pad = max(1, 2 + width - len(left))
        if len(left) <= width:
            print(f"{indent}{c('36', left)}{' ' * pad}{c('2', right)}")
        else:
            # Left label itself exceeds the column width: put it on its own line.
            print(f"{indent}{c('36', left)}")
            print(f"{indent}{' ' * (width + 2)}{c('2', right)}")

def print_command_help(cmd: str) -> None:
    print(c("1;36", f"Nytrix build tool: {cmd}"))
    print(c("90", "-" * 70))
    if cmd not in _COMMAND_USAGE:
        print(f"  {c('36', './make ' + cmd)}")
        print("  (this command is supported; see './make --help' for the full command list)")
        return
    usage, summary, sections = _COMMAND_USAGE[cmd]
    if usage.lower().startswith("usage:"):
        print(f"{c('1', 'Usage:')} {c('1;32', usage[6:].strip())}")
    else:
        print(f"{c('1', 'Usage:')} {c('1;32', './make ' + usage)}")
    print(f"  {c('90', summary)}")
    for title, rows in sections:
        print("")
        print(c("1", title + ":"))
        _print_help_rows(rows)
    print("")
    print(c("1", "Global options:"))
    _print_help_rows([
        ("-j, --jobs N", "parallel build jobs"),
        ("-v, --verbose", "print subcommands"),
        ("--color MODE", "auto | always | never"),
        ("--no-color", "disable colored output"),
        ("--bootstrap-logs", "show bootstrap status"),
        ("--debug", "switch to the debug build configuration when supported"),
        ("-h, --help", "show this command's help"),
    ])
    print("")
    print(f"  {c('90', 'Sub-commands share this driver; run ./make <cmd> --help for each one.')}")

def print_help() -> None:
    print(c("1;36", "Nytrix build tool"))
    print(c("90", "-" * 70))
    print(f"{c('1', 'Usage:')} {c('1;32', './make')} {c('36', '<command>')} {c('32', '[options]')}")
    print("")
    groups = (
        ("Build", (
            ("all", "configure and build ny, std, and tools"),
            ("bin", "build the ny executable only"),
            ("static bin", "build/bundle portable compiler tools in build/static"),
            ("bin-static", "alias for static bin"),
            ("tar", "create build/dist/nytrix-source.tar.gz, or --with-binaries for nytrix-static"),
            ("vendor", "bundle shared libs into build/vendor/ for portability"),
            ("static libs", "refresh bundled shared libs for release/static"),
            ("static check", "check whether an ELF is static or dynamic"),
            ("std/std_bc", "bundle stdlib source or bitcode"),
            ("install/uninstall", "install or remove ny and ny-lsp"),
            ("clean", "remove generated artifacts"),
        )),
        ("Check", (
            ("fmt/check/tidy/audit", "format, parse-check, tidy, or deep source audit"),
            ("test/fuzz", "run tests and smoke fuzzing"),
            ("bench", "run public C-vs-Ny benchmark shapes"),
            ("asan/ubsan", "run tests under sanitizer builds"),
            ("profile", "time, perf, gdb, sanitizer, and fuzz wrappers"),
            ("perf/gprof", "run performance tooling"),
        )),
        ("Run", (
            ("ny/repl/run", "launch the compiler, REPL, or cached -run flow"),
            ("docs", "build documentation portal"),
            ("web", "build one Ny source as a deployable WebGL2 browser app"),
            ("wasm", "compile a Ny source file to WebAssembly"),
            ("web-check", "verify a Ny source uses only implemented browser host APIs"),
            ("web-test", "build and prove Pong reaches the WebGL2 browser runner"),
            ("web-demos", "build the browser WebGL/Wasm demo portal"),
        )),
        ("Inspect", (
            ("env", "print effective paths, tools, and overrides"),
            ("targets", "show cross-compilation target guidance"),
            ("doctor", "delegate to ny --doctor for toolchain diagnostics"),
        )),
        ("Cross", (
            ("cross", "cross-compile (delegates to ny --cross=<triple>)"),
            ("cross-run", "cross-compile and run via QEMU (delegates to ny)"),
        )),
    )
    for title, rows in groups:
        print(c("1", title + ":"))
        _print_help_rows(rows)
    print("")
    print(c("1", "Options:"))
    _print_help_rows([
        ("-j, --jobs N", "parallel build jobs"),
        ("-v, --verbose", "print subcommands"),
        ("-h, --help", "show this help"),
        ("--version", "print version"),
        ("--color MODE", "auto | always | never"),
        ("--no-color", "disable colored output"),
        ("--bootstrap-logs", "show bootstrap status"),
        ("--no-bootstrap-logs", "hide bootstrap status"),
    ])
    print("")
    print(c("1", "Ny/runtime passthrough:"))
    print("  These target the `ny` command directly:")
    _print_help_rows([
        ("./make ny <file.ny> [flags]", "run a Ny source with compiler flags before/after the file"),
        ("./make <file.ny> [flags]", "shorthand for ./make ny"),
        ("./make -trace ny <file.ny> ...", "pass compiler flags before the source"),
        ("./make ny <file.ny> -v -gl", "pass UI/app flags after the source"),
        ("./make ny <file.ny> -- [args]", "force the rest to be program args"),
    ])
    print("")
    print(c("1", "Per-command help:"))
    print("  Run  ./make <command> --help   for command-specific options and examples.")
    print("")
    print(c("1", "Examples:"))
    print("  ./make doctor")
    print("  ./make targets")
    print("  ./make cross aarch64-linux-gnu file.ny")
    print("  ./make ny etc/projects/ui/term.ny -h")
    print("  ./make etc/projects/ui/term.ny -v -vk btop")
    print("  ./make -trace ny etc/projects/ui/engine.ny -vk")
    print("  ./make wasm etc/projects/os/args.ny --out build/web/wasm/args.wasm")
    print("  ./make bin-static")
    print("  ./make static libs build/static")
    print("  ./make web-demos")

def _set_env_value(name: str, value: str | None) -> None:
    if value is None:
        os.environ.pop(name, None)
    else:
        os.environ[name] = value

def _append_flags(base: str | None, extra: str) -> str:
    return ((base or "") + " " + extra).strip()

def configure_command_environment(
    cmd: str,
    base_kind: str,
    base_host_cflags: str | None,
    base_host_ldflags: str | None,
    base_skip_optional_gates: str | None,
    base_test_cache: str | None,
    base_test_cold: str | None,
) -> str:
    _set_env_value("NYTRIX_HOST_CFLAGS", base_host_cflags)
    _set_env_value("NYTRIX_HOST_LDFLAGS", base_host_ldflags)
    _set_env_value("NYTRIX_SKIP_OPTIONAL_GATES", base_skip_optional_gates)
    _set_env_value("NYTRIX_TEST_CACHE", base_test_cache)
    _set_env_value("NYTRIX_TEST_COLD", base_test_cold)
    if cmd == "asan":
        _set_env_value(
            "NYTRIX_HOST_CFLAGS",
            _append_flags(base_host_cflags, "-fsanitize=address -fno-omit-frame-pointer -g3"),
        )
        _set_env_value(
            "NYTRIX_HOST_LDFLAGS",
            _append_flags(base_host_ldflags, "-fsanitize=address"),
        )
        os.environ["NYTRIX_SKIP_OPTIONAL_GATES"] = "1"
        os.environ["NYTRIX_TEST_CACHE"] = "0"
        os.environ["NYTRIX_TEST_COLD"] = "1"
        return "asan"
    if cmd == "ubsan":
        _set_env_value(
            "NYTRIX_HOST_CFLAGS",
            _append_flags(base_host_cflags, "-fsanitize=undefined -fno-omit-frame-pointer -g3 -fno-sanitize-recover=undefined"),
        )
        _set_env_value(
            "NYTRIX_HOST_LDFLAGS",
            _append_flags(base_host_ldflags, "-fsanitize=undefined"),
        )
        os.environ["NYTRIX_SKIP_OPTIONAL_GATES"] = "1"
        os.environ["NYTRIX_TEST_CACHE"] = "0"
        os.environ["NYTRIX_TEST_COLD"] = "1"
        return "ubsan"
    return base_kind

def print_static_help() -> None:
    print(c("1;36", "Nytrix static / portable build"))
    print("")
    print("Usage:")
    print("  ./make static bin                   build compiler/tools and bundle runtime .so files")
    print("  ./make bin-static                   alias for static bin")
    print("  ./make tar                          build build/dist/nytrix-source.tar.gz")
    print("  ./make tar --with-binaries          build runnable build/dist/nytrix-static.tar.gz")
    print("  ./make static libs [path]           bundle shared libs beside build/release or path")
    print("  ./make static check <binary>        check if ELF binary is static/dynamic")
    print("  ./make static ny <file.ny> [flags]  compile Ny program with static link flags")
    print("  ./make static <file.ny> [flags]     shorthand for static ny")
    print("")
    print("Output:")
    print("  build/static/                       portable folder")
    print("  build/static/lib/host/              bundled libLLVM/libclang/libz3/etc")
    print("  build/release/lib/host/             same libs copied beside release build")
    print("  build/static/env.sh                 convenience helpers; prefer ./run-ny")
    print("")
    print("Modes:")
    print("  default/auto                        portable dynamic bundle; avoids fake full-static")
    print("  NYTRIX_STATIC_MODE=full             force -static; falls back to portable bundle if it fails")
    print("  NYTRIX_STATIC_MODE=mostly           use -static-libgcc/-static-libstdc++ only")
    print("")
    print("Env:")
    print("  NYTRIX_STATIC_LDFLAGS               override compiler/tool link flags")
    print("  NYTRIX_BUNDLE_NO_SYSTEM_LIBS=1      make a thin same-distro bundle; omit libc/loader")
    print("  NYTRIX_STATIC_LIB_SEARCH_PATH=a:b   extra directories to find missing .so files")
    print("  NYTRIX_GLIBC_FLOOR=2.38             max allowed GLIBC requirement for bundled .so files")
    print("  NYTRIX_ALLOW_NEW_GLIBC_BUNDLE=1     allow packaging host-newer .so files anyway")

def _elf_is_static(binary: Path) -> tuple[bool, str]:
    try:
        res = subprocess.run(["ldd", str(binary)], capture_output=True, text=True, timeout=5)
        ldd_out = res.stdout.strip()
        ldd_lower = ldd_out.lower()
        if "not a dynamic executable" in ldd_lower or "statically linked" in ldd_lower:
            return True, ldd_out
        if ldd_out.startswith("\t") or "=>" in ldd_out:
            needed = [
                ln.strip().split()[0] for ln in ldd_out.splitlines()
                if "=>" in ln and "not found" not in ln
            ]
            missing = [ln.split()[0] for ln in ldd_out.splitlines() if "not found" in ln]
            missing_names = [m for m in missing if m and not m.startswith("\t")]
            detail = f"dynamic linked: needed={','.join(needed) if needed else 'none'}"
            if missing_names:
                detail += f" missing={','.join(missing_names)}"
            return False, detail
        res2 = subprocess.run(["file", str(binary)], capture_output=True, text=True, timeout=5)
        file_out = res2.stdout.strip()
        if "statically linked" in file_out.lower():
            return True, file_out
        return False, file_out
    except Exception as exc:
        return False, str(exc)

def _elf_needed_names(path: Path) -> list[str]:
    try:
        res = subprocess.run(["readelf", "-d", str(path)], capture_output=True, text=True, timeout=10)
        names: list[str] = []
        for ln in res.stdout.splitlines():
            if "(NEEDED)" not in ln:
                continue
            m = re.search(r"\[(.*?)\]", ln)
            if m:
                names.append(m.group(1))
        return list(dict.fromkeys(names))
    except Exception:
        return []

def _is_system_runtime_lib(name: str) -> bool:
    fname = name.rsplit("/", 1)[-1]
    base = fname.split(".so", 1)[0]
    if fname.startswith(("ld-linux", "ld-musl")):
        return True
    return base in {"libc", "libm", "libdl", "libpthread", "libutil", "librt", "libgcc_s", "libstdc++"}

def _runtime_search_dirs() -> list[Path]:
    dirs: list[Path] = []
    raw = os.environ.get("NYTRIX_STATIC_LIB_SEARCH_PATH") or os.environ.get("NYTRIX_BUNDLE_LIB_PATH") or ""
    for part in raw.split(os.pathsep):
        if part.strip():
            dirs.append(Path(part).expanduser())
    for tool in ("llvm-config-21", "llvm-config", "llvm-config-22", "llvm-config-20", "llvm-config-19", "llvm-config-18", "llvm-config-17", "llvm-config-16"):
        t = which(tool)
        if not t:
            continue
        res = run_capture([t, "--libdir"])
        if res.returncode == 0:
            val = res.stdout.strip().splitlines()[0] if res.stdout.strip() else ""
            if val:
                dirs.append(Path(val))
    for d in (
        "/usr/lib/llvm21/lib", "/usr/lib/llvm-21/lib", "/usr/lib/llvm22/lib", "/usr/lib/llvm-22/lib",
        "/usr/lib", "/usr/lib64", "/lib", "/lib64", "/usr/local/lib", "/usr/local/lib64",
        "/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu",
    ):
        dirs.append(Path(d))
    for pat in ("/usr/lib/llvm*/lib", "/usr/local/llvm*/lib", "/opt/llvm*/lib"):
        try:
            dirs.extend(Path("/").glob(pat.lstrip("/")))
        except Exception:
            pass
    out: list[Path] = []
    seen: set[str] = set()
    for d in dirs:
        try:
            r = str(d.resolve())
        except Exception:
            r = str(d)
        if r not in seen and d.exists() and d.is_dir():
            seen.add(r)
            out.append(d)
    return out

def _find_shared_lib(name: str, search_dirs: list[Path]) -> Path | None:
    # Exact soname first.
    for d in search_dirs:
        p = d / name
        if p.exists():
            return p
    # Then compatible prefix: libLLVM.so.21.1 -> libLLVM.so*, libz3.so.4.16 -> libz3.so*
    stem = name.split(".so", 1)[0] + ".so"
    candidates: list[Path] = []
    for d in search_dirs:
        try:
            candidates.extend(d.glob(stem + "*"))
        except Exception:
            pass
    # Prefer exact-ish versioned files over bare linker scripts/symlinks.
    candidates = [c for c in candidates if c.exists()]
    # If the requested soname has a major version, never fake it with a different major
    # (libLLVM.so.19 copied as libLLVM.so.21.1 will load but then fail symbol versions).
    req_major = ""
    suf = name.split(".so", 1)[1] if ".so" in name else ""
    if suf.startswith("."):
        req_major = suf[1:].split(".", 1)[0]
    filtered: list[Path] = []
    for cnd in candidates:
        if req_major:
            names = [cnd.name]
            try:
                names.append(cnd.resolve().name)
            except Exception:
                pass
            if not any(n == name or n.startswith(stem + "." + req_major) for n in names):
                continue
        filtered.append(cnd)
    candidates = filtered
    candidates.sort(key=lambda x: (0 if x.name == name else 1, -len(x.name)))
    for cnd in candidates:
        # Avoid text linker scripts where possible.
        try:
            f = subprocess.run(["file", "-b", str(cnd)], capture_output=True, text=True, timeout=5).stdout.lower()
            if "ascii text" in f or "linker script" in f:
                continue
        except Exception:
            pass
        return cnd
    return None

def _copy_one_shared_lib(src: Path, dst_dir: Path, needed_name: str | None = None) -> Path | None:
    try:
        dst_dir.mkdir(parents=True, exist_ok=True)
        real = src.resolve() if src.is_symlink() else src
        name = needed_name or src.name
        dst = dst_dir / name
        if not dst.exists():
            shutil.copy2(real, dst)
        # Symlink the real name → needed name to avoid duplicate .so files (~90MB waste).
        if real.name != name:
            real_dst = dst_dir / real.name
            if not real_dst.exists():
                try:
                    os.symlink(name, real_dst)
                except Exception:
                    shutil.copy2(real, real_dst)
        return dst
    except Exception as exc:
        log("STATIC", f"failed to copy {src}: {exc}")
        return None

def _ldd_resolved(path: Path, lib_dir: Path) -> tuple[list[tuple[str, Path]], list[str]]:
    env = os.environ.copy()
    extra = str(lib_dir)
    old = env.get("LD_LIBRARY_PATH", "")
    env["LD_LIBRARY_PATH"] = extra + (os.pathsep + old if old else "")
    found: list[tuple[str, Path]] = []
    missing: list[str] = []
    try:
        res = subprocess.run(["ldd", str(path)], capture_output=True, text=True, timeout=15, env=env)
        for ln in (res.stdout + "\n" + res.stderr).splitlines():
            parts = ln.strip().split()
            if not parts:
                continue
            if len(parts) >= 4 and parts[1] == "=>" and parts[2] == "not" and parts[3] == "found":
                missing.append(parts[0])
            elif len(parts) >= 3 and parts[1] == "=>" and Path(parts[2]).exists():
                found.append((parts[0], Path(parts[2])))
            elif len(parts) >= 1 and parts[0].startswith("/") and Path(parts[0]).exists():
                found.append((Path(parts[0]).name, Path(parts[0])))
    except Exception:
        pass
    return found, list(dict.fromkeys(missing))

def _patch_rpath_for_bundle(exe: Path, lib_dir: Path) -> bool:
    # Off by default for full portable bundles. When glibc/libstdc++ are bundled,
    # RPATH/LD_LIBRARY_PATH can make the host system loader load a mismatched
    # bundled libc and crash. The robust outside-chroot path is run-ny, which
    # invokes the bundled loader explicitly. Opt in only for thin/same-distro
    # bundles.
    if not _env_flag("NYTRIX_BUNDLE_PATCH_RPATH", False):
        return False
    patchelf = which("patchelf")
    if not patchelf:
        return False
    try:
        rel = os.path.relpath(lib_dir, exe.parent)
        rpath = "$ORIGIN" if rel == "." else "$ORIGIN/" + rel.replace(os.sep, "/")
        subprocess.run([patchelf, "--set-rpath", rpath, str(exe)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return True
    except Exception:
        return False

def _write_bundle_env(bundle_dir: Path, lib_dir: Path) -> None:
    try:
        env_path = bundle_dir / "env.sh"
        env_path.write_text(
            "#!/usr/bin/env sh\n"
            "# Source this for convenience helpers only. Do not LD_LIBRARY_PATH the bundled\n"
            "# glibc into a different host loader; use ./run-ny instead.\n"
            "_nytrix_here=$(CDPATH= cd -- \"$(dirname -- \"${BASH_SOURCE:-$0}\")\" && pwd)\n"
            "export NYTRIX_BUNDLE_ROOT=\"$_nytrix_here\"\n"
            "export NYTRIX_ROOT=\"$_nytrix_here\"\n"
            "if [ -f \"$_nytrix_here/src/rt/init.c\" ]; then export NYTRIX_RT_SRC=\"$_nytrix_here/src/rt/init.c\"; fi\n"
            "if [ -z \"${CC:-}\" ]; then\n"
            "  if command -v clang >/dev/null 2>&1; then export CC=clang;\n"
            "  elif command -v cc >/dev/null 2>&1; then export CC=cc;\n"
            "  elif command -v gcc >/dev/null 2>&1; then export CC=gcc; fi\n"
            "fi\n"
            "nytrix() { \"$_nytrix_here/run-ny\" \"$@\"; }\n"
            "echo 'Nytrix bundle loaded: use nytrix <args> or ./run-ny <args>'\n",
            encoding="utf-8",
        )
        env_path.chmod(0o755)
    except Exception:
        pass

def _copy_static_runtime_libs(target_dir: Path, binary_path: Path) -> list[str]:
    """Bundle dynamic dependencies for binary_path into target_dir.

    Unlike the old ldd-only helper, this also resolves missing sonames such as
    libLLVM.so.21.1/libclang.so.21.1/libz3.so.4.16 from LLVM and system lib dirs,
    then walks transitive dependencies. For bin-static/static-bin portability it
    copies the full host runtime by default too (glibc loader, libc, libm,
    libstdc++, libgcc_s, etc). Set NYTRIX_BUNDLE_NO_SYSTEM_LIBS=1 only if you
    intentionally want a thin bundle tied to the target machine's system libc.
    """
    copied: list[str] = []
    copy_system = not _env_flag("NYTRIX_BUNDLE_NO_SYSTEM_LIBS", False)
    search_dirs = [target_dir, *_runtime_search_dirs()]
    queue: list[Path] = [binary_path]
    seen_files: set[str] = set()
    seen_missing: set[str] = set()
    unresolved: set[str] = set()
    while queue:
        cur = queue.pop(0)
        try:
            key = str(cur.resolve())
        except Exception:
            key = str(cur)
        if key in seen_files or not cur.exists():
            continue
        seen_files.add(key)
        found, missing = _ldd_resolved(cur, target_dir)
        for name, dep in found:
            if not copy_system and _is_system_runtime_lib(name):
                continue
            dst = _copy_one_shared_lib(dep, target_dir, name)
            if dst:
                if dst.name not in copied:
                    copied.append(dst.name)
                queue.append(dst)
        # readelf catches libs hidden by ldd failure and lets us search manually.
        for name in [*missing, *_elf_needed_names(cur)]:
            if not name or name in seen_missing:
                continue
            seen_missing.add(name)
            if not copy_system and _is_system_runtime_lib(name):
                continue
            dep = _find_shared_lib(name, search_dirs)
            if dep:
                dst = _copy_one_shared_lib(dep, target_dir, name)
                if dst:
                    if dst.name not in copied:
                        copied.append(dst.name)
                    queue.append(dst)
                    search_dirs.insert(0, target_dir)
            else:
                unresolved.add(name)
    if unresolved:
        log("STATIC", "unresolved shared libs: " + ", ".join(sorted(unresolved)))
    return copied

def _install_loader_compat_paths(bundle_dir: Path, lib_dir: Path) -> None:
    """Make a dynamic binary usable as a tiny chroot root.

    The executable interpreter is usually absolute, e.g. /lib64/ld-linux-x86-64.so.2.
    RPATH/LD_LIBRARY_PATH are not consulted until that loader exists, so mirror the
    bundled loader into lib64/. Also expose lib/host/*.so* through lib/*.so* symlinks
    because plain `chroot build/static /ny` cannot source env.sh.
    """
    # Dynamic loader path required before the binary can even start.
    loaders = list(lib_dir.glob("ld-linux*.so*")) + list(lib_dir.glob("ld-musl*.so*"))
    for ld in loaders:
        for sub in ("lib64", "lib"):
            dst = bundle_dir / sub / ld.name
            try:
                dst.parent.mkdir(parents=True, exist_ok=True)
                _copy2_if_different(ld, dst)
                dst.chmod(0o755)
            except Exception:
                pass
    # Chroot compatibility: default loader search paths include /lib and /lib64,
    # but not /lib/host. Use symlinks to avoid duplicating a 150+MB LLVM .so.
    for lib_subdir, rel_prefix in (("lib", Path("host")), ("usr/lib", Path("..") / ".." / "lib" / "host"), ("usr/lib64", Path("..") / ".." / "lib" / "host")):
        root_lib = bundle_dir / lib_subdir
        root_lib.mkdir(parents=True, exist_ok=True)
        for so in lib_dir.glob("*.so*"):
            if not so.is_file():
                continue
            dst = root_lib / so.name
            try:
                if _same_path(so, dst):
                    continue
                if dst.exists() or dst.is_symlink():
                    dst.unlink()
                os.symlink(rel_prefix / so.name, dst)
            except Exception:
                try:
                    _copy2_if_different(so, dst)
                except Exception:
                    pass
    # Convenience path inside chroot.
    try:
        usr_bin = bundle_dir / "usr" / "bin"
        usr_bin.mkdir(parents=True, exist_ok=True)
        dst = usr_bin / "ny"
        if dst.exists() or dst.is_symlink():
            dst.unlink()
        os.symlink(Path("..") / ".." / "ny", dst)
    except Exception:
        pass

def _write_bundle_launchers(bundle_dir: Path, lib_dir: Path) -> None:
    """Write outside-chroot launchers that force the bundled loader/libs."""
    try:
        loaders = sorted(lib_dir.glob("ld-linux*.so*")) or sorted(lib_dir.glob("ld-musl*.so*"))
        loader = loaders[0] if loaders else None
        sh = bundle_dir / "run-ny"
        if loader:
            rel_loader = os.path.relpath(loader, bundle_dir).replace(os.sep, "/")
            text = "#!/usr/bin/env sh\n"
            text += "set -eu\n"
            text += 'here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
            text += f'exec "$here/{rel_loader}" --library-path "$here/lib/host${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}" "$here/ny" "$@"\n'
        else:
            text = "#!/usr/bin/env sh\n"
            text += 'here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
            text += 'export LD_LIBRARY_PATH="$here/lib/host${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n'
            text += 'exec "$here/ny" "$@"\n'
        sh.write_text(text, encoding="utf-8")
        sh.chmod(0o755)
    except Exception:
        pass

def _bundle_dir_for_binary(binary_path: Path) -> list[str]:
    lib_dir = binary_path.parent / "lib" / "host"
    copied = _copy_static_runtime_libs(lib_dir, binary_path)
    _patch_rpath_for_bundle(binary_path, lib_dir)
    _write_bundle_env(binary_path.parent, lib_dir)
    _install_loader_compat_paths(binary_path.parent, lib_dir)
    _write_bundle_launchers(binary_path.parent, lib_dir)
    return copied

def _same_path(a: Path, b: Path) -> bool:
    try:
        return a.resolve() == b.resolve()
    except Exception:
        return False

def _copy2_if_different(src: Path, dst: Path) -> bool:
    if _same_path(src, dst):
        return False
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    return True

def _copytree_replace(src: Path, dst: Path, ignore=None) -> None:
    if not src.exists():
        return
    if dst.exists():
        shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(src, dst, ignore=ignore, symlinks=True)

def _copy_release_file(src: Path, dst: Path) -> None:
    if src.exists() and src.is_file():
        _copy2_if_different(src, dst)

def _make_tar_gz_fast(archive_base: Path, root_dir: Path, base_dir: str) -> Path:
    tar_path = Path(str(archive_base) + ".tar.gz")
    tar_bin = which("tar")
    if tar_bin and host_os() != "windows":
        attempts: list[tuple[list[str], dict[str, str] | None]] = [
            ([tar_bin, "-C", str(root_dir), "-I", "gzip -1", "-cf", str(tar_path), base_dir], None),
            ([tar_bin, "-C", str(root_dir), "-czf", str(tar_path), base_dir], {"GZIP": "-1"}),
        ]
        last_exc: Exception | None = None
        for cmd, extra_env in attempts:
            try:
                tar_path.unlink(missing_ok=True)
            except Exception:
                pass
            env = os.environ.copy()
            if extra_env:
                env.update(extra_env)
            try:
                subprocess.run(cmd, check=True, env=env)
                return tar_path
            except Exception as exc:
                last_exc = exc
                try:
                    tar_path.unlink(missing_ok=True)
                except Exception:
                    pass
        if last_exc is not None:
            log("TAR", f"system tar failed, falling back to Python archive: {last_exc}")
    return Path(shutil.make_archive(str(archive_base), "gztar", root_dir=root_dir, base_dir=base_dir))

def _write_ny_test_wrapper(path: Path, real_name: str = "ny-test.real") -> None:
    if host_os() == "windows" or not path.exists() or not path.is_file():
        return
    real = path.with_name(real_name)
    try:
        try:
            head = path.read_text(encoding="utf-8", errors="ignore")[:256]
        except Exception:
            head = ""
        if "ny-test.real" in head and real.exists():
            return
        if real.exists():
            real.unlink()
        path.rename(real)
        wrapper = """#!/usr/bin/env sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
for arg in "$@"; do
  case "$arg" in
    --bin|--bin=*) exec "$here/ny-test.real" "$@" ;;
  esac
done
if [ -x "$here/ny" ]; then
  exec "$here/ny-test.real" --bin "$here/ny" "$@"
fi
exec "$here/ny-test.real" "$@"
"""
        path.write_text(wrapper, encoding="utf-8")
        path.chmod(0o755)
        real.chmod(0o755)
    except Exception:
        pass

def _parse_glibc_floor(raw: str | None = None) -> tuple[int, int]:
    s = (raw or os.environ.get("NYTRIX_GLIBC_FLOOR") or "2.38").strip()
    m = re.fullmatch(r"(\d+)\.(\d+)", s)
    if not m:
        raise SystemExit(f"make tar: invalid NYTRIX_GLIBC_FLOOR={s!r}; expected e.g. 2.38")
    return int(m.group(1)), int(m.group(2))

def _glibc_needed_versions(path: Path) -> list[tuple[int, int]]:
    # Only count the ELF Version needs section. The Version definition section in
    # libc.so itself lists provided versions and must not be treated as a host
    # requirement.
    if host_os() != "linux":
        return []
    readelf = which("readelf")
    if not readelf:
        return []
    try:
        res = subprocess.run([readelf, "--version-info", str(path)], capture_output=True, text=True, timeout=10)
    except Exception:
        return []
    text = (res.stdout or "") + (res.stderr or "")
    needs = False
    out: list[tuple[int, int]] = []
    for line in text.splitlines():
        if "Version needs section" in line:
            needs = True
            continue
        if needs and "Version definition section" in line:
            needs = False
        if not needs:
            continue
        for maj, min_ in re.findall(r"GLIBC_(\d+)\.(\d+)", line):
            out.append((int(maj), int(min_)))
    return out

def _glibc_required_max(path: Path) -> tuple[int, int] | None:
    versions = _glibc_needed_versions(path)
    return max(versions) if versions else None

def _elf_needed_names(path: Path) -> list[str]:
    if host_os() != "linux":
        return []
    readelf = which("readelf")
    if not readelf:
        return []
    try:
        res = subprocess.run([readelf, "-d", str(path)], capture_output=True, text=True, timeout=10)
    except Exception:
        return []
    text = (res.stdout or "") + (res.stderr or "")
    return re.findall(r"Shared library: \[([^\]]+)\]", text)

def _vendor_glibc_floor_violations(lib_dir: Path, floor: tuple[int, int]) -> list[tuple[Path, tuple[int, int]]]:
    bad: list[tuple[Path, tuple[int, int]]] = []
    if host_os() != "linux" or not lib_dir.is_dir():
        return bad
    for p in sorted(lib_dir.glob("*.so*")):
        if p.is_symlink() or not p.is_file():
            continue
        req = _glibc_required_max(p)
        if req and req > floor:
            bad.append((p, req))
    return bad

def _remove_vendor_lib_family(lib_dir: Path, pattern: str) -> None:
    for p in sorted(lib_dir.glob(pattern)):
        try:
            p.unlink()
        except Exception:
            pass

def _write_vendor_libedit_stub(lib_dir: Path) -> bool:
    # LLVM links against editline, but Nytrix never uses LLVM's interactive
    # line-editor path. On rolling distros libedit/ncurses can require a newer
    # glibc than the rest of the vendored LLVM bundle. This tiny no-libc ELF
    # stub satisfies the loader for source builds without shipping host terminal
    # libraries in the tarball.
    if host_os() != "linux":
        return False
    cc_raw = (os.environ.get("CC") or _select_default_cc() or "").strip()
    if not cc_raw:
        return False
    src = (
        "typedef struct EditLine EditLine;\n"
        "typedef struct History History;\n"
        "typedef struct LineInfo LineInfo;\n"
        "typedef struct LineInfoW LineInfoW;\n"
        "typedef int wchar_t;\n"
        "typedef unsigned long size_t;\n"
        "int history_base = 1;\n"
        "int history_length = 0;\n"
        "int history_max_entries = 0;\n"
        "int history_offset = 0;\n"
        "int history_expansion_char = 33;\n"
        "int history_subst_char = 94;\n"
        "const char *history_no_expand_chars = \"\\0\";\n"
        "void *history_inhibit_expansion_function = (void *)0;\n"
        "EditLine *el_init(const char *prog, void *fin, void *fout, void *ferr) "
        "{ (void)prog; (void)fin; (void)fout; (void)ferr; return (EditLine *)0; }\n"
        "EditLine *el_init_fd(const char *prog, void *fin, void *fout, void *ferr, int fdin, int fdout, int fderr) "
        "{ (void)fdin; (void)fdout; (void)fderr; return el_init(prog, fin, fout, ferr); }\n"
        "void el_end(EditLine *e) { (void)e; }\n"
        "void el_reset(EditLine *e) { (void)e; }\n"
        "int el_set(EditLine *e, int op, ...) { (void)e; (void)op; return -1; }\n"
        "int el_get(EditLine *e, int op, ...) { (void)e; (void)op; return -1; }\n"
        "int el_wset(EditLine *e, int op, ...) { (void)e; (void)op; return -1; }\n"
        "int el_wget(EditLine *e, int op, ...) { (void)e; (void)op; return -1; }\n"
        "const char *el_gets(EditLine *e, int *count) "
        "{ (void)e; if (count) *count = 0; return (const char *)0; }\n"
        "const wchar_t *el_wgets(EditLine *e, int *count) "
        "{ (void)e; if (count) *count = 0; return (const wchar_t *)0; }\n"
        "int el_getc(EditLine *e, char *c) { (void)e; if (c) *c = 0; return 0; }\n"
        "int el_wgetc(EditLine *e, wchar_t *c) { (void)e; if (c) *c = 0; return 0; }\n"
        "const LineInfo *el_line(EditLine *e) { (void)e; return (const LineInfo *)0; }\n"
        "const LineInfoW *el_wline(EditLine *e) { (void)e; return (const LineInfoW *)0; }\n"
        "int el_insertstr(EditLine *e, const char *s) { (void)e; (void)s; return -1; }\n"
        "int el_winsertstr(EditLine *e, const wchar_t *s) { (void)e; (void)s; return -1; }\n"
        "int el_push(EditLine *e, const char *s) { (void)e; (void)s; return -1; }\n"
        "int el_wpush(EditLine *e, const wchar_t *s) { (void)e; (void)s; return -1; }\n"
        "int el_replacestr(EditLine *e, const char *s) { (void)e; (void)s; return -1; }\n"
        "int el_wreplacestr(EditLine *e, const wchar_t *s) { (void)e; (void)s; return -1; }\n"
        "int el_deletestr(EditLine *e, int n) { (void)e; (void)n; return -1; }\n"
        "int el_deletestr1(EditLine *e, int n) { (void)e; (void)n; return -1; }\n"
        "int el_cursor(EditLine *e, int n) { (void)e; (void)n; return -1; }\n"
        "void el_beep(EditLine *e) { (void)e; }\n"
        "int el_resize(EditLine *e) { (void)e; return 0; }\n"
        "int el_parse(EditLine *e, int argc, const char **argv) { (void)e; (void)argc; (void)argv; return -1; }\n"
        "int el_wparse(EditLine *e, int argc, const wchar_t **argv) { (void)e; (void)argc; (void)argv; return -1; }\n"
        "int el_source(EditLine *e, const char *file) { (void)e; (void)file; return -1; }\n"
        "History *history_init(void) { return (History *)0; }\n"
        "void history_end(History *h) { (void)h; }\n"
        "History *history_winit(void) { return (History *)0; }\n"
        "void history_wend(History *h) { (void)h; }\n"
        "int history(History *h, void *ev, int op, ...) "
        "{ (void)h; (void)ev; (void)op; return -1; }\n"
        "int history_w(History *h, void *ev, int op, ...) "
        "{ (void)h; (void)ev; (void)op; return -1; }\n"
        "char *history_arg_extract(int a, int b, const char *s) { (void)a; (void)b; (void)s; return (char *)0; }\n"
        "int history_expand(char *s, char **o) { (void)s; if (o) *o = (char *)0; return 0; }\n"
        "void *history_get(int i) { (void)i; return (void *)0; }\n"
        "void *history_get_history_state(void) { return (void *)0; }\n"
        "int history_is_stifled(void) { return 0; }\n"
        "void **history_list(void) { return (void **)0; }\n"
        "int history_search(const char *s, int d) { (void)s; (void)d; return -1; }\n"
        "int history_search_pos(const char *s, int d, int p) { (void)s; (void)d; (void)p; return -1; }\n"
        "int history_search_prefix(const char *s, int d) { (void)s; (void)d; return -1; }\n"
        "int history_set_pos(int p) { (void)p; return -1; }\n"
        "char **history_tokenize(const char *s) { (void)s; return (char **)0; }\n"
        "int history_total_bytes(void) { return 0; }\n"
        "int history_truncate_file(const char *f, int n) { (void)f; (void)n; return 0; }\n"
    )
    real = lib_dir / "libedit.so.0.0.nytrix-stub"
    tmp = lib_dir / ".libedit.so.0.0.nytrix-stub.tmp"
    cmd = shlex.split(cc_raw) + [
        "-shared", "-fPIC", "-nostdlib", "-x", "c", "-",
        "-Wl,-soname,libedit.so.0", "-o", str(tmp),
    ]
    try:
        res = subprocess.run(cmd, input=src, text=True, capture_output=True, timeout=30)
    except Exception:
        return False
    if res.returncode != 0 or not tmp.exists():
        try:
            tmp.unlink(missing_ok=True)
        except Exception:
            pass
        return False

    _remove_vendor_lib_family(lib_dir, "libedit.so*")
    tmp.replace(real)
    try:
        real.chmod(0o755)
    except Exception:
        pass
    for link_name, target in (("libedit.so.0", real.name), ("libedit.so", "libedit.so.0")):
        link = lib_dir / link_name
        try:
            link.unlink(missing_ok=True)
            link.symlink_to(target)
        except Exception:
            _copy2_if_different(real, link)
    log("VENDOR", "replaced host libedit with Nytrix LLVM line-editor shim")
    return True

def _vendor_any_needed(lib_dir: Path, soname: str) -> bool:
    for p in sorted(lib_dir.glob("*.so*")):
        if p.is_symlink() or not p.is_file():
            continue
        if p.name.startswith(soname):
            continue
        if soname in _elf_needed_names(p):
            return True
    return False

def _repair_tar_vendor_glibc_floor(lib_dir: Path) -> None:
    if host_os() != "linux" or _env_flag("NYTRIX_ALLOW_NEW_GLIBC_BUNDLE", False):
        return
    if not lib_dir.is_dir():
        return
    floor = _parse_glibc_floor()
    bad = _vendor_glibc_floor_violations(lib_dir, floor)
    if any(p.name.startswith("libedit.so") for p, _ in bad):
        if not _write_vendor_libedit_stub(lib_dir):
            raise SystemExit(
                "make tar: vendored libedit needs newer glibc than "
                f"{floor[0]}.{floor[1]} and the compatibility shim could not be built; "
                "install cc or set NYTRIX_ALLOW_NEW_GLIBC_BUNDLE=1 to keep host libs"
            )

    # After replacing libedit, ncurses is normally no longer referenced by the
    # vendored LLVM closure. Drop an over-new ncurses copy instead of shipping a
    # same-distro terminal library in a portable source archive.
    bad = _vendor_glibc_floor_violations(lib_dir, floor)
    if any(p.name.startswith("libncursesw.so") for p, _ in bad) and not _vendor_any_needed(lib_dir, "libncursesw.so.6"):
        _remove_vendor_lib_family(lib_dir, "libncursesw.so*")
        log("TAR", "removed unused host-newer libncursesw from vendor bundle")

    bad = _vendor_glibc_floor_violations(lib_dir, floor)
    if bad:
        detail = ", ".join(f"{p.name} needs GLIBC_{req[0]}.{req[1]}" for p, req in bad[:8])
        if len(bad) > 8:
            detail += f", ... {len(bad) - 8} more"
        raise SystemExit(
            "make tar: refusing to package host-newer vendored libs "
            f"(floor GLIBC_{floor[0]}.{floor[1]}): {detail}. "
            "Build the tar on an older distro/container or set "
            "NYTRIX_ALLOW_NEW_GLIBC_BUNDLE=1 for a same-host bundle."
        )

def _check_static_bundle_glibc_floor(lib_dir: Path, context: str = "static") -> None:
    if host_os() != "linux" or _env_flag("NYTRIX_ALLOW_NEW_GLIBC_BUNDLE", False):
        return
    # Vendor builds need the exact libs LLVM was linked against (libedit, etc).
    # Skipping the floor check for vendor prevents link failures from missing SONAMEs.
    if context == "vendor":
        log("STATIC", "vendor context: skipping glibc floor prune (vendored LLVM needs host libs)")
        return
    floor = _parse_glibc_floor()
    bad: list[str] = []
    for p in sorted(lib_dir.glob("*.so*")):
        if not p.is_file():
            continue
        versions = _glibc_needed_versions(p)
        if not versions:
            continue
        req = max(versions)
        if req > floor:
            bad.append(str(p.name))
    if bad:
        for name in bad:
            target = lib_dir / name
            try:
                target.unlink()
            except Exception:
                pass
            # Also remove symlink variants (e.g. libedit.so.0 -> libedit.so.0.0.78)
            for sibling in lib_dir.glob(f"{name}*"):
                try:
                    sibling.unlink()
                except Exception:
                    pass
        # Clean up dangling symlinks in the bundle tree
        bundle_root = lib_dir.parent.parent
        for root, dirs, files in os.walk(str(bundle_root)):
            root_p = Path(root)
            for name in files:
                p = root_p / name
                if p.is_symlink() and not p.exists():
                    try:
                        p.unlink()
                    except Exception:
                        pass
        log("STATIC", f"pruned host-newer terminal libs: {', '.join(bad)}")
        if context == "static":
            log("STATIC", "run-ny will use target system libedit/ncurses when needed")
        elif context == "vendor":
            log("STATIC", "WARNING: libedit/ncurses pruned — vendored LLVM needs them. "
                "If build fails with 'cannot find -ledit', install libedit-dev or set "
                "NYTRIX_ALLOW_NEW_GLIBC_BUNDLE=1 to bundle the host's libedit.")

def _stage_static_tools(src_dir: Path, out_dir: Path) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    bin_dir = out_dir / "bin"
    bin_dir.mkdir(parents=True, exist_ok=True)
    names = ["ny", "ny-fmt", "ny-perf", "ny-test", "ny-doc", "ny-make", "ny-lsp"]
    copied = 0
    for name in names:
        src = src_dir / name
        if not src.exists():
            continue
        dst_top = out_dir / name
        dst_bin = bin_dir / name
        _copy2_if_different(src, dst_top)
        _copy2_if_different(src, dst_bin)
        try:
            dst_top.chmod(0o755)
            dst_bin.chmod(0o755)
        except Exception:
            pass
        copied += 1
    for name in ("std.ny", "std.bc", "std_symbols.h"):
        src = src_dir / name
        if src.exists():
            _copy2_if_different(src, out_dir / name)
    # Bundle project runtime assets for moving the folder to another machine.
    for name in ("src", "etc", "lib"):
        src = ROOT / name
        dst = out_dir / name
        if src.exists() and src.is_dir():
            if dst.exists():
                # Preserve lib/host if present; refresh everything else.
                if name == "lib" and (dst / "host").exists():
                    host_tmp = out_dir / ".host.tmp"
                    shutil.rmtree(host_tmp, ignore_errors=True)
                    shutil.move(str(dst / "host"), str(host_tmp))
                    shutil.rmtree(dst, ignore_errors=True)
                    shutil.copytree(src, dst)
                    shutil.move(str(host_tmp), str(dst / "host"))
                else:
                    shutil.rmtree(dst, ignore_errors=True)
                    shutil.copytree(src, dst)
            else:
                shutil.copytree(src, dst)
    for name in ("make", "CMakeLists.txt", ".clangd", "README.md", "LICENSE"):
        src = ROOT / name
        if src.exists():
            shutil.copy2(src, out_dir / name)
    _write_ny_test_wrapper(out_dir / "ny-test")
    _write_ny_test_wrapper(out_dir / "bin" / "ny-test")
    return copied

def _bundle_static_outputs(build_root: Path, src_dir: Path) -> None:
    static_dir = build_root / "static"
    if src_dir.resolve() != static_dir.resolve():
        n = _stage_static_tools(src_dir, static_dir)
        if n == 0:
            raise SystemExit(f"make static bin: no compiler/tools found in {src_dir}")
    else:
        _stage_static_tools(src_dir, static_dir)
    all_copied: list[str] = []
    for d in (static_dir, build_root / "release"):
        if not d.exists() or not d.is_dir():
            continue
        for exe_name in ("ny", "ny-fmt", "ny-perf", "ny-test", "ny-doc", "ny-make", "ny-lsp"):
            exe = d / exe_name
            if exe.exists():
                all_copied.extend(_bundle_dir_for_binary(exe))
        # Also patch/copy bin/ variants in the portable folder.
        bindir = d / "bin"
        if bindir.exists():
            for exe in bindir.iterdir():
                if exe.is_file() and os.access(exe, os.X_OK):
                    lib_dir = d / "lib" / "host"
                    _patch_rpath_for_bundle(exe, lib_dir)
    readme = static_dir / "README_STATIC.txt"
    readme.write_text(
        "Nytrix portable compiler folder\n"
        "\n"
        "Outside a chroot, use the wrapper. It forces the bundled loader/libs:\n"
        "  ./run-ny --help\n"
        "  ./run-ny -ic 'print(\"hello\")'\n"
        "  ./run-ny path/to/file.ny\n"
        "\n"
        "Convenience shell helper:\n"
        "  . ./env.sh\n"
        "  nytrix --help\n"
        "\n"
        "For chroot-style use:\n"
        "  sudo chroot . /ny --help\n"
        "  sudo chroot . /ny -ic 'print(\"hello\")'\n"
        "\n"
        "Plain ./ny is intentionally not the main portable entrypoint: if the host\n"
        "loader is older/different, LD_LIBRARY_PATH/RPATH with bundled glibc can crash.\n"
        "The glibc loader is mirrored into lib64/ and lib/host/*.so* is exposed\n"
        "through lib/, usr/lib/, and usr/lib64/ symlinks for chroot use.\n"
        "Bundled host libraries live in lib/host/.\n",
        encoding="utf-8",
    )
    uniq = list(dict.fromkeys(all_copied))
    if uniq:
        log("STATIC", f"portable libs bundled: {', '.join(uniq)}")
    _check_static_bundle_glibc_floor(static_dir / "lib" / "host")
    ok(f"portable static folder ready: {_rel_or_abs(static_dir)}")

def _check_static_libs_available() -> tuple[bool, list[str]]:
    """Check whether truly static linking is possible by probing for .a files."""
    missing: list[str] = []
    # Check LLVM static libs
    res = run_capture(["llvm-config", "--libdir"])
    if res.returncode == 0:
        llvm_libdir = Path(res.stdout.strip().splitlines()[0])
        if not (llvm_libdir / "libLLVM-21.a").exists() and not (llvm_libdir / "libLLVM.a").exists():
            missing.append("libLLVM-21.a (install llvm-*-dev/static or build LLVM with -DBUILD_SHARED_LIBS=OFF)")
        for clang_lib in ("libclang.a", "libclang-c.a", "libclang-cpp.a"):
            if (llvm_libdir / clang_lib).exists():
                break
        else:
            missing.append("libclang.a / libclang-c.a (llvm/clang static libs not found)")
    else:
        missing.append("llvm-config not found")
    # Check Z3
    for p in ("/usr/lib/libz3.a", "/usr/local/lib/libz3.a"):
        if Path(p).exists():
            break
    else:
        for path in Path("/usr/lib").glob("libz3*.a"):
            break
        else:
            missing.append("libz3.a (libz3-dev or build z3 from source)")
    ok = len(missing) == 0
    return ok, missing

def _llvm_ldflags_static() -> str | None:
    """Check if llvm-config --link-static works, returns ldflags or None."""
    for flag in ("--link-static", "--shared-mode"):
        res = run_capture(["llvm-config", flag])
        if res.returncode == 0 and "static" in res.stdout:
            return "-static"
    return None

def run_make_static(build_root: Path, kind: str, jobs: int, args: list[str]) -> int:
    global QUIET_BOOTSTRAP
    if not args or args[0] in ("-h", "--help", "help"):
        print_static_help()
        return 0

    sub = args[0]
    rest = args[1:]

    # static bin / static all -- build compiler/tools and make a portable folder.
    if sub in ("bin", "all"):
        old_ldflags = os.environ.get("NYTRIX_HOST_LDFLAGS")
        old_cflags = os.environ.get("NYTRIX_HOST_CFLAGS")
        old_quiet = QUIET_BOOTSTRAP
        try:
            static_mode = (os.environ.get("NYTRIX_STATIC_MODE") or "auto").strip().lower()
            static_ldflags_raw = os.environ.get("NYTRIX_STATIC_LDFLAGS", "").strip()
            static_cflags = os.environ.get("NYTRIX_STATIC_CFLAGS", "").strip()
            full_requested = static_mode in ("full", "true", "1", "yes") or " -static" in f" {static_ldflags_raw} "
            if static_ldflags_raw:
                static_ldflags = static_ldflags_raw
            elif full_requested:
                static_ldflags = "-static -static-libgcc -static-libstdc++"
            else:
                # Practical default: LLVM/Clang/Z3 are usually only provided as .so.
                # Produce a portable bundled folder instead of pretending full-static exists.
                static_ldflags = "-static-libgcc -static-libstdc++"

            os.environ["NYTRIX_HOST_LDFLAGS"] = static_ldflags
            if static_cflags:
                os.environ["NYTRIX_HOST_CFLAGS"] = static_cflags
            QUIET_BOOTSTRAP = False
            targets = ["ny", "std", "ny-fmt", "ny-perf", "ny-test", "ny-doc", "ny-make", "ny-lsp"]
            boot_notice(f"static bin: ldflags=({static_ldflags})")
            bdir = cmake_build_dir(build_root, "static")
            try:
                cmake_build(build_root, "static", targets, jobs)
            except subprocess.CalledProcessError:
                if not full_requested:
                    raise
                log("STATIC", "full static link failed; falling back to portable bundled dynamic build")
                _set_env_value("NYTRIX_HOST_LDFLAGS", old_ldflags)
                _set_env_value("NYTRIX_HOST_CFLAGS", old_cflags)
                cmake_build(build_root, "release", targets, jobs)
                bdir = cmake_build_dir(build_root, "release")
            ny_exe = bdir / "ny"
            if ny_exe.exists():
                is_static, detail = _elf_is_static(ny_exe)
                if is_static:
                    ok(f"static bin: {_rel_or_abs(ny_exe)} is truly static")
                else:
                    log("STATIC", f"{_rel_or_abs(ny_exe)} is dynamic; bundling needed .so files")
                    log("LDD", detail)
            _bundle_static_outputs(build_root, bdir)
            return 0
        finally:
            _set_env_value("NYTRIX_HOST_LDFLAGS", old_ldflags)
            _set_env_value("NYTRIX_HOST_CFLAGS", old_cflags)
            QUIET_BOOTSTRAP = old_quiet

    # static check <binary> -- check if ELF is static
    if sub == "check":
        for target in rest:
            p = Path(target) if Path(target).is_absolute() else ROOT / target
            if not p.exists():
                err(f"static check: not found: {p}")
                continue
            is_static, detail = _elf_is_static(p)
            tag = "STATIC static" if is_static else "STATIC dynamic"
            print(f"{c('32' if is_static else '33', tag)}: {_rel_or_abs(p)}")
            log("LDD", detail)
        return 0

    # static libs [path] -- bundle shared libs beside a binary or build dir.
    if sub == "libs":
        search_items = [build_root / "release", build_root / "static"]
        if rest:
            search_items = [Path(p) if Path(p).is_absolute() else ROOT / p for p in rest]
        total_copied: list[str] = []
        for item in search_items:
            if item.is_file():
                total_copied.extend(_bundle_dir_for_binary(item))
                continue
            if not item.is_dir():
                continue
            for exe_name in ("ny", "ny-fmt", "ny-perf", "ny-test", "ny-doc", "ny-make", "ny-lsp"):
                exe_path = item / exe_name
                if exe_path.exists():
                    total_copied.extend(_bundle_dir_for_binary(exe_path))
        uniq = list(dict.fromkeys(total_copied))
        if uniq:
            log("STATIC", f"bundled shared libs: {', '.join(uniq)}")
        else:
            log("STATIC", "no shared libs copied (already static, missing binary, or deps unavailable)")
        return 0

    # static ny <file.ny> [flags] -- compile with static linking
    if sub == "ny":
        ny_args = rest
    else:
        ny_args = [sub] + rest

    # Ensure release compiler exists
    if cmake_build_has_work(build_root, kind, ["ny"]):
        old_quiet = QUIET_BOOTSTRAP
        try:
            QUIET_BOOTSTRAP = False
            cmake_build(build_root, kind, ["ny"], jobs)
        finally:
            QUIET_BOOTSTRAP = old_quiet

    static_ldflags = os.environ.get("NYTRIX_STATIC_LDFLAGS",
                                     "-static -static-libgcc -static-libstdc++")
    return run_tool(build_root, kind, "ny",
                    ["--host-ldflags", static_ldflags, *ny_args])

def _vendor_lib_dir(build_root: Path) -> Path:
    return build_root / "vendor" / "lib" / "host"

def _llvm_major_version_raw() -> str:
    """Return the major version of the system LLVM (e.g. '22'), or empty string."""
    for tool in ("llvm-config", "llvm-config-22", "llvm-config-21", "llvm-config-20",
                  "llvm-config-19", "llvm-config-18", "llvm-config-17", "llvm-config-16"):
        t = which(tool)
        if not t:
            continue
        res = run_capture([t, "--version"])
        if res.returncode == 0:
            ver = res.stdout.strip().split(".")[0]
            if ver.isdigit():
                return ver
    return ""

def _detect_bundled_llvm_major(lib_dir: Path) -> str:
    """Detect the LLVM major version from bundled .so filenames (e.g. '21' from libLLVM.so.21.1)."""
    for f in lib_dir.glob("libLLVM.so.*"):
        m = re.match(r"libLLVM\.so\.(\d+)", f.name)
        if m:
            return m.group(1)
    return ""

def _create_linker_symlinks(lib_dir: Path, llvm_major: str) -> None:
    """Create linker-name symlinks for bundled .so files so -l flags resolve."""
    # Map canonical file -> desired linker symlinks
    symlinks: list[tuple[str, str]] = []
    for f in lib_dir.iterdir():
        if not f.is_file() or f.is_symlink():
            continue
        # libLLVM.so.21.1 -> libLLVM-21.so
        m = re.match(r"libLLVM\.so\.(\d+)(?:\.\d+.*)?", f.name)
        if m and m.group(1) == llvm_major:
            target = f"libLLVM-{llvm_major}.so"
            symlinks.append((target, f.name))
            continue
        # libclang.so.21.1 -> libclang.so
        m = re.match(r"libclang\.so\.(\d+)(?:\.\d+.*)?", f.name)
        if m and m.group(1) == llvm_major:
            symlinks.append(("libclang.so", f.name))
            continue
        # Generic: for most libs, create unversioned symlink if SONAME is versioned.
        # libz.so.1 -> libz.so, libzstd.so.1 -> libzstd.so, etc.
        m = re.match(r"^((?:lib[\w-]+)\.so)\.\d", f.name)
        if m:
            base = m.group(1)
            if not (lib_dir / base).exists() and base not in (n for n, _ in symlinks):
                symlinks.append((base, f.name))
    for link_name, target in symlinks:
        link = lib_dir / link_name
        if link.exists() or link.is_symlink():
            continue
        link.symlink_to(target)
        log("VENDOR", f"  symlink {link_name} -> {target}")

def run_make_vendor(build_root: Path, kind: str, jobs: int, args: list[str]) -> int:
    if args and args[0] in ("-h", "--help", "help"):
        print("usage: make vendor  -- bundle shared libs for build portability")
        print()
        print("  Copies .so files + LLVM/Clang headers needed to build ny")
        print("  into build/vendor/ so ./make tar produces a portable package")
        print("  that can compile from source without system LLVM dev headers.")
        print()
        print("  The release binary must exist first (run ./make bin first).")
        return 0

    os.chdir(str(ROOT))

    # Find the release binary; build it if missing.
    release_dir = build_root / "release"
    ny_exe = release_dir / "ny"
    if not ny_exe.exists():
        boot_step("vendor: building release ny first")
        cmake_build(build_root, kind, ["ny", "std", "ny-fmt", "ny-perf", "ny-test", "ny-doc", "ny-make", "ny-lsp"], jobs)

    vendor_dir = build_root / "vendor"
    lib_dir = _vendor_lib_dir(build_root)
    lib_dir.mkdir(parents=True, exist_ok=True)

    os.environ["NYTRIX_BUNDLE_NO_SYSTEM_LIBS"] = "1"
    copied = _copy_static_runtime_libs(lib_dir, ny_exe)
    _check_static_bundle_glibc_floor(lib_dir, "vendor")

    # Report only what actually survived the glibc floor prune.
    real_names: set[str] = set()
    for f in lib_dir.glob("*.so*"):
        if f.is_symlink() or not f.is_file():
            continue
        real_names.add(f.name)
    actual = sorted(real_names)
    if actual:
        log("VENDOR", f"bundled {len(actual)} shared libs: {', '.join(actual)}")
    else:
        log("VENDOR", "no shared libs copied (already static link?)")

    # Detect bundled LLVM version from .so files
    bundled_llvm_major = _detect_bundled_llvm_major(lib_dir)
    if bundled_llvm_major:
        log("VENDOR", f"detected bundled LLVM {bundled_llvm_major} from libLLVM.so.*")
    else:
        log("VENDOR", "no bundled LLVM .so found; skipping LLVM/Clang headers")

    vendored_bin = vendor_dir / "bin"
    vendored_include = vendor_dir / "include"

    # Z3's shared library is useful to a fresh source build only when its public
    # headers travel with it. Copy the complete public z3*.h family because
    # z3.h includes the generated API and version headers beside it.
    z3_include_candidates = [Path("/usr/include"), Path("/usr/local/include")]
    z3_pc = run_capture(["pkg-config", "--variable=includedir", "z3"])
    if z3_pc.returncode == 0 and z3_pc.stdout.strip():
        z3_include_candidates.insert(0, Path(z3_pc.stdout.strip()))
    z3_header = next((p / "z3.h" for p in z3_include_candidates if (p / "z3.h").exists()), None)
    if z3_header is not None and any(p.name.startswith("libz3.so") for p in lib_dir.glob("libz3.so*")):
        vendored_include.mkdir(parents=True, exist_ok=True)
        copied_z3_headers = 0
        for header in sorted(z3_header.parent.glob("z3*.h")):
            _copy2_if_different(header, vendored_include / header.name)
            copied_z3_headers += 1
        log("VENDOR", f"bundled {copied_z3_headers} Z3 public headers")
    elif any(p.name.startswith("libz3.so") for p in lib_dir.glob("libz3.so*")):
        log("VENDOR", "libz3 was bundled but z3.h was not found; install matching Z3 development headers before creating a source package")

    # Find the llvm-config matching the bundled LLVM version.
    # Must use the SYSTEM llvm-config, not the vendored one (which doesn't have
    # headers yet and reports $here/include as --includedir).
    llvm_config_bin: str | None = None
    if bundled_llvm_major:
        system_path = _system_path_excluding_vendor(build_root)
        for cand in (f"llvm-config-{bundled_llvm_major}", "llvm-config"):
            cand_path = which(cand, path=system_path)
            if cand_path and cand_path != str(vendored_bin / "llvm-config"):
                llvm_config_bin = cand_path
                break
        if llvm_config_bin:
            check_ver = run_capture([llvm_config_bin, "--version"]).stdout.strip().split(".")[0]
            if check_ver != bundled_llvm_major:
                log("VENDOR", f"  {llvm_config_bin} reports LLVM {check_ver}, bundled libs are {bundled_llvm_major} — skipping incompatible headers")
                llvm_config_bin = None

    if llvm_config_bin and bundled_llvm_major:
        boot_step("vendor: bundling LLVM/Clang headers for self-contained build")
        llvm_version = run_capture([llvm_config_bin, "--version"]).stdout.strip()
        llvm_inc_raw = run_capture([llvm_config_bin, "--includedir"]).stdout.strip()
        llvm_cflags_raw = run_capture([llvm_config_bin, "--cflags"]).stdout.strip()
        llvm_libs_raw = run_capture([llvm_config_bin, "--libs", "all", "--system-libs"]).stdout.strip()
        clang_resource = run_capture([llvm_config_bin, "--libdir"]).stdout.strip()

        # Copy LLVM/Clang headers into vendor/include/.
        for sub in ("llvm-c", "clang-c", "llvm", "clang"):
            src = Path(llvm_inc_raw) / sub
            dst = vendored_include / sub
            if src.exists():
                _copytree_replace(src, dst)

        # Copy clang builtin includes.
        for cand in (
            Path(llvm_inc_raw).parent / "lib" / f"clang/{bundled_llvm_major}/include",
            Path("/usr/lib") / f"clang/{bundled_llvm_major}/include",
            Path("/usr/lib64") / f"clang/{bundled_llvm_major}/include",
            Path("/usr/local/lib") / f"clang/{bundled_llvm_major}/include",
            Path(clang_resource).parent / f"clang/{bundled_llvm_major}/include",
        ):
            if cand.exists() and any(cand.iterdir()):
                _copytree_replace(cand, vendored_include / "clang-builtins")
                break

        # Generate vendored llvm-config script.
        vendored_bin.mkdir(parents=True, exist_ok=True)
        config_script = (
            "#!/usr/bin/env sh\n"
            "# Auto-generated by ./make vendor — do not edit\n"
            f'here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/..\n'
            f'llvm_libs="{llvm_libs_raw}"\nllvm_cflags="{llvm_cflags_raw}"\n'
            'case "${1:-}" in\n'
            f'  --version) echo "{llvm_version}" ;;\n'
            f'  --includedir) echo "$here/include" ;;\n'
            '  --cflags)\n'
            '    echo "$llvm_cflags" | sed "s|-I[^ ]*|-I$here/include|g"\n'
            '    ;;\n'
            '  --ldflags) echo "-L$here/lib/host" ;;\n'
            '  --libs)\n'
            f'    echo "$llvm_libs"\n    ;;\n'
            f'  *) exit 1 ;;\nesac\n'
        )
        config_path = vendored_bin / "llvm-config"
        config_path.write_text(config_script, encoding="utf-8")
        config_path.chmod(0o755)
        log("VENDOR", f"vendored llvm-config ({llvm_version}) + LLVM/Clang headers (bundled {bundled_llvm_major})")
    else:
        log("VENDOR", "no matching llvm-config for bundled LLVM; skipping LLVM/Clang headers")

    # Create linker-name symlinks so -l flags resolve against bundled .so files.
    _create_linker_symlinks(lib_dir, bundled_llvm_major)

    # Write vendor env.sh.
    env_path = vendor_dir / "env.sh"
    env_path.parent.mkdir(parents=True, exist_ok=True)
    env_lines = [
        "#!/usr/bin/env sh",
        "# Source this to use vendored shared libs + LLVM for building.",
        '_nytrix_here=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE:-$0}")" && pwd)',
        'export LD_LIBRARY_PATH="$_nytrix_here/lib/host${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"',
    ]
    if vendored_bin.exists() and (vendored_bin / "llvm-config").exists():
        env_lines += [
            'export PATH="$_nytrix_here/bin${PATH:+:$PATH}"',
            'export LLVM_CONFIG="$_nytrix_here/bin/llvm-config"',
            'export NYTRIX_LLVM_INCLUDE="$_nytrix_here/include"',
        ]
    env_lines += ["echo 'Nytrix vendor libs loaded'"]
    env_path.write_text("\n".join(env_lines) + "\n", encoding="utf-8")
    env_path.chmod(0o755)

    ok(f"vendor ready: {_rel_or_abs(vendor_dir)}")
    return 0

def _tar_source_ignore(dir: str, names: list[str]):
    ignored = {
        ".git", ".cache", "tmp", "__pycache__", ".pytest_cache",
        "CMakeFiles", "CMakeCache.txt", "compile_commands.json",
    }
    out = set()
    for name in names:
        if name in ignored:
            out.add(name)
        elif name.endswith((".o", ".a", ".pyc", ".pyo")):
            out.add(name)
    return out

def run_make_tar(build_root: Path, kind: str, jobs: int, args: list[str]) -> int:
    with_binaries = "--with-binaries" in args or _env_flag("NYTRIX_TAR_WITH_BINARIES", False)
    source_only = "--source" in args or "--source-only" in args
    if source_only:
        with_binaries = False
    args = [a for a in args if a not in ("--with-binaries", "--source", "--source-only")]

    if args and args[0] in ("-h", "--help", "help"):
        print("usage: make tar [--source|--with-binaries]")
        print()
        print("  Default: creates build/dist/nytrix-source.tar.gz")
        print("    source code + vendored LLVM build libs, checked against NYTRIX_GLIBC_FLOOR")
        print("  --with-binaries: creates build/dist/nytrix-static.tar.gz")
        print("    includes build/static/ with ./run-ny and bundled runtime libs")
        print("  NYTRIX_GLIBC_FLOOR=2.38 controls the portability floor for packaged libs.")
        print("  NYTRIX_TAR_WITH_BINARIES=1 makes --with-binaries the default.")
        return 0

    dist_dir = build_root / "dist"
    package_name = "nytrix-static" if with_binaries else "nytrix-source"
    package_dir = dist_dir / package_name
    shutil.rmtree(package_dir, ignore_errors=True)
    package_dir.mkdir(parents=True, exist_ok=True)

    # Ensure vendor libs exist (build release + bundle if needed).
    vendor_lib = _vendor_lib_dir(build_root)
    if not vendor_lib.exists() or not any(vendor_lib.glob("*.so*")):
        boot_step("tar: bundling vendored libs first")
        run_make_vendor(build_root, kind, jobs, [])

    # Copy vendored libs into build/vendor/ inside the package, then repair the
    # copy so source tarballs do not ship same-distro terminal libraries that
    # require a newer glibc than the rest of the vendored LLVM closure.
    _copytree_replace(build_root / "vendor", package_dir / "build" / "vendor")
    _repair_tar_vendor_glibc_floor(package_dir / "build" / "vendor" / "lib" / "host")

    if with_binaries:
        # Build and bundle static portable folder.
        rc = run_make_static(build_root, kind, jobs, ["bin"])
        if rc != 0:
            return rc
        _copytree_replace(build_root / "static", package_dir / "build" / "static")

    # Source tree (runtime headers, stdlib source, etc).
    for name in ("src", "lib", "etc", ".github"):
        _copytree_replace(ROOT / name, package_dir / name, ignore=_tar_source_ignore)
    for name in ("make", "CMakeLists.txt", ".clangd", "out.diff", "README.md", "LICENSE"):
        _copy_release_file(ROOT / name, package_dir / name)

    # No top-level env.sh is emitted. ./make now installs the package-local
    # NYTRIX_ROOT/NYTRIX_RT_SRC/CC defaults before running any command.
    archive_base = dist_dir / package_name
    tar_path = _make_tar_gz_fast(archive_base, dist_dir, package_name)
    ok(f"tar ready: {_rel_or_abs(tar_path)}")
    return 0

def main() -> int:
    global COLOR, QUIET_BOOTSTRAP
    cmds, extra, requested_jobs, verbose, want_help, help_target, want_version, debug_kind, cli_color_mode, cli_bootstrap_logs = parse(sys.argv[1:])
    COLOR = apply_cli_color_mode(cli_color_mode)
    ensure_project_scripts_executable()
    if want_help:
        if help_target and help_target in _COMMAND_USAGE:
            print_command_help(help_target)
            return 0
        print_help()
        return 0
    if want_version:
        print("Nytrix Build Tool")
        return 0

    kind = "debug" if debug_kind else "release"
    build_root, notice = resolve_build_dir()
    first_repl_bootstrap = bootstrap_needed_for_repl(build_root, kind, cmds)
    inspect_cmds = {"env", "targets", "doctor"}
    tool_style_cmds = {"fmt", "analyze", "check", "tidy", "audit", "test", "perf", "profile", "docs", "web", "web-demos", "web-check", "web-test", "wasm", "ny", "repl", "gprof", "asan", "ubsan", "fuzz", "bench", "cross", "cross-run", "static", "bin-static", "tar", "vendor", *inspect_cmds}
    all_tool_style = all(c in tool_style_cmds for c in cmds)
    if all_tool_style and not first_repl_bootstrap:
        # Keep tool invocations clean by default (./make fmt/test/ny...) even if env
        # globally enables bootstrap logs. Use --bootstrap-logs to opt in per run.
        use_bootstrap_logs = False
    else:
        default_bootstrap_logs = True
        use_bootstrap_logs = _env_flag("NYTRIX_MAKE_BOOTSTRAP_LOGS", default_bootstrap_logs)
    if cli_bootstrap_logs is not None:
        use_bootstrap_logs = cli_bootstrap_logs
    if verbose:
        use_bootstrap_logs = True
        os.environ["NYTRIX_MAKE_COMMANDS"] = "1"
    QUIET_BOOTSTRAP = not use_bootstrap_logs

    if notice and not QUIET_BOOTSTRAP:
        log("BUILD", notice)
    # Auto-activate vendored LLVM if present — prevents apt update / missing-dep
    # errors when the dist tar already bundles everything needed.
    vendored_bin = build_root / "vendor" / "bin"
    vendored_llvm_config = vendored_bin / "llvm-config"
    if vendored_llvm_config.exists():
        path = os.environ.get("PATH", "")
        if str(vendored_bin) not in path.split(":"):
            os.environ["PATH"] = f"{vendored_bin}:{path}"
        os.environ.setdefault("LLVM_CONFIG", str(vendored_llvm_config))
        vendored_include = build_root / "vendor" / "include"
        if vendored_include.exists():
            os.environ.setdefault("NYTRIX_LLVM_INCLUDE", str(vendored_include))
        log("BUILD", "auto-activated vendored LLVM (PATH/LLVM_CONFIG/NYTRIX_LLVM_INCLUDE)")

    # Commands that don't need LLVM/deps (fast checks, clean, info)
    deps_free_cmds = {*inspect_cmds, "fmt", "analyze", "check", "tidy", "audit",
                      "clean", "deps", "env", "targets", "doctor",
                      "static", "bin-static", "tar", "vendor"}

    # Commands that need LLVM/deps (build, test, bench, etc.)
    build_cmds = {"bin", "test", "bench", "asan", "ubsan", "debug", "release",
                  "perf", "profile", "docs", "web", "web-demos", "wasm",
                  "cross", "cross-run", "fuzz", "repl", "profile", "gprof",
                  "ny", "run", "install", "uninstall", "profile"}

    needs_deps = any(c in build_cmds for c in cmds)
    if needs_deps or not all(c in deps_free_cmds for c in cmds):
        ensure_deps(force_optional_prompt=("deps" in cmds), require_git=("deps" in cmds))
    elif host_os() == "macos":
        configure_macos_tool_path()

    jobs, jobs_note = resolve_jobs(requested_jobs)
    boot_log("HOST", jobs_note)
    base_host_cflags = os.environ.get("NYTRIX_HOST_CFLAGS")
    base_host_ldflags = os.environ.get("NYTRIX_HOST_LDFLAGS")
    base_skip_optional_gates = os.environ.get("NYTRIX_SKIP_OPTIONAL_GATES")
    base_test_cache = os.environ.get("NYTRIX_TEST_CACHE")
    base_test_cold = os.environ.get("NYTRIX_TEST_COLD")

    for cmd in cmds:
        active_kind = configure_command_environment(
            cmd, kind, base_host_cflags, base_host_ldflags,
            base_skip_optional_gates, base_test_cache, base_test_cold,
        )
        if cmd == "env":
            ny_bin = resolve_tool_bin(build_root, active_kind, "ny")
            rc = subprocess.run([str(ny_bin), "--env"], env=os.environ).returncode
            if rc != 0:
                return rc
            continue
        if cmd == "targets":
            rc = run_make_targets()
            if rc != 0:
                return rc
            continue
        if cmd == "doctor":
            ny_bin = resolve_tool_bin(build_root, active_kind, "ny")
            rc = subprocess.run([str(ny_bin), "--doctor", *extra]).returncode
            if rc != 0:
                return rc
            continue
        if cmd == "clean":
            shutil.rmtree(build_root, ignore_errors=True)
            log("CLEAN", f"removed {build_root}")
            continue
        if cmd == "deps":
            continue

        targets = ["ny"]
        if cmd in ("all", "bin"):
            targets = ["ny", "std", "ny-fmt", "ny-perf", "ny-test", "ny-doc", "ny-make", "ny-lsp"]
        elif cmd in ("fmt", "analyze", "check", "tidy", "audit"):
            targets = ["ny-fmt"]
        elif cmd in ("test", "asan", "ubsan"):
            targets = ["ny", "ny-full", "ny-test"]
            if host_os() != "windows":
                targets.append("ny-fuzz")
        elif cmd in ("fuzz", "bench"):
            if host_os() == "windows":
                raise SystemExit("make: fuzz tooling currently requires POSIX process APIs")
            targets = ["ny", "ny-test", "ny-fuzz"]
        elif cmd in ("cross", "cross-run"):
            targets = ["ny"]
        elif cmd == "profile":
            targets = ["ny"]
        elif cmd == "docs":
            targets = ["ny", "std", "ny-doc"]
        elif cmd in ("web", "web-demos", "web-check", "web-test", "wasm"):
            targets = ["ny", "std"]
        elif cmd == "std":
            targets = ["std"]
        elif cmd == "std_bc":
            targets = ["std_bc"]
        elif cmd == "install":
            targets = ["ny", "ny-lsp", "std", "ny-fmt", "ny-perf", "ny-test", "ny-doc", "ny-make"]
            if host_os() != "windows":
                targets.append("nytrixrt")
        elif cmd == "perf":
            targets = ["ny", "ny-perf"]
        if cmd not in ("uninstall", "static", "bin-static", "tar", "vendor"):
            ny_missing = cmd in ("ny", "repl") and cmake_build_has_work(build_root, active_kind, targets)
            if ny_missing:
                clean_bad_tool_build(build_root, active_kind, "ny")
            repl_build_visible = cmd in ("ny", "repl")
            old_quiet = QUIET_BOOTSTRAP
            if repl_build_visible:
                QUIET_BOOTSTRAP = False
                if ny_missing:
                    boot_notice("ny binary missing: compiling before launch")
            try:
                cmake_build(build_root, active_kind, targets, jobs)
                if repl_build_visible and ny_missing:
                    boot_notice("ny compiled; launching")
            finally:
                if repl_build_visible:
                    QUIET_BOOTSTRAP = old_quiet

        if cmd == "all":
            # Keep the ordinary developer build paired with the formatter's
            # conservative bug audit. This is diagnostic-only: ny-fmt findings
            # are reviewed rather than rewritten automatically.
            rc = run_tool(
                build_root, active_kind, "ny-fmt",
                ["--bugs", "--limit", "80", "lib"],
            )
            if rc != 0:
                return rc
            continue
        if cmd in ("bin", "std", "std_bc"):
            continue
        if cmd == "test":
            rc = run_test(build_root, active_kind, requested_jobs, extra)
        elif cmd == "fmt":
            rc = run_tool(build_root, active_kind, "ny-fmt", extra)
        elif cmd == "analyze":
            rc = run_tool(build_root, active_kind, "ny-fmt", ["--analyze", *extra])
        elif cmd == "check":
            rc = run_tool(build_root, active_kind, "ny-fmt", ["--check", *extra])
        elif cmd == "tidy":
            # Strip only NUL bytes from C/H sources so Clang does not
            # crash with "null character ignored".  Keep all valid UTF-8
            # (em-dashes, arrows, checkmarks, etc.).
            import glob as _glob
            _nul = b"\x00"
            for _pattern in ("src/**/*.c", "src/**/*.h"):
                for _f in _glob.glob(str(ROOT / _pattern), recursive=True):
                    with open(_f, "rb") as _fh:
                        _data = _fh.read()
                    if _nul in _data:
                        with open(_f, "wb") as _fh:
                            _fh.write(_data.replace(_nul, b""))
            rc = run_tool(build_root, active_kind, "ny-fmt", ["--tidy", *extra])
        elif cmd == "audit":
            rc = run_tool(build_root, active_kind, "ny-fmt", ["--audit", *extra])
        elif cmd == "perf":
            rc = run_tool(build_root, active_kind, "ny-perf", extra)
        elif cmd == "docs":
            std_file = str(cmake_build_dir(build_root, active_kind) / "std.ny")
            out_dir = str(build_root / "docs")
            rc = run_tool(build_root, active_kind, "ny-doc", [std_file, "-o", out_dir, *extra])
        elif cmd == "web":
            rc = run_web(build_root, active_kind, extra)
        elif cmd == "web-demos":
            rc = run_web_demos(build_root, active_kind, extra)
        elif cmd == "web-check":
            rc = run_web_check(build_root, active_kind, extra)
        elif cmd == "web-test":
            rc = run_web_test(build_root, active_kind, extra)
        elif cmd == "c2ny":
            if not extra:
                nyt_err("c2ny", "usage: ./make c2ny <file.c> [-o <out.ny>]")
                raise SystemExit(1)
            rc = run_tool(build_root, active_kind, "ny-fmt", ["--c2ny", *extra])
        elif cmd == "py2ny":
            if not extra:
                nyt_err("py2ny", "usage: ./make py2ny <file.py> [-o <out.ny>]")
                raise SystemExit(1)
            rc = run_tool(build_root, active_kind, "ny-fmt", ["--py2ny", *extra])
        elif cmd == "wasm":
            ny_bin = resolve_tool_bin(build_root, active_kind, "ny")
            rc = subprocess.run([str(ny_bin), "--wasm", *extra]).returncode
        elif cmd == "install":
            rc = cmake_install(build_root, active_kind)
        elif cmd == "uninstall":
            manifest = cmake_build_dir(build_root, active_kind) / "install_manifest.txt"
            if not manifest.exists():
                raise SystemExit(f"make: install manifest not found: {manifest}")
            failed = 0
            removed = 0
            for ln in manifest.read_text(encoding="utf-8", errors="ignore").splitlines():
                p = Path(ln.strip())
                if not p:
                    continue
                try:
                    if p.is_dir():
                        shutil.rmtree(p, ignore_errors=False)
                    else:
                        p.unlink(missing_ok=True)
                    removed += 1
                except Exception:
                    failed += 1
            ok(f"uninstalled ({removed} removed, {failed} failed)")
            rc = 1 if failed else 0
        elif cmd == "repl":
            rc = run_tool(build_root, active_kind, "ny", ["-i", *extra])
        elif cmd == "ny":
            if extra:
                rc = run_tool(build_root, active_kind, "ny", extra)
            else:
                rc = run_tool(build_root, active_kind, "ny", ["-i"])
        elif cmd == "static":
            rc = run_make_static(build_root, kind, jobs, extra)
        elif cmd == "bin-static":
            rc = run_make_static(build_root, kind, jobs, ["bin", *extra])
        elif cmd == "vendor":
            rc = run_make_vendor(build_root, kind, jobs, extra)
        elif cmd == "tar":
            rc = run_make_tar(build_root, kind, jobs, extra)
        elif cmd == "cross":
            if not extra:
                raise SystemExit("make cross: usage: ./make cross <triple-or-preset> [file.ny] [-- ny-flags]")
            triple = extra[0]
            rest = extra[1:]
            ny_bin = resolve_tool_bin(build_root, active_kind, "ny")
            rc = subprocess.run([str(ny_bin), f"--cross={triple}", *rest]).returncode
        elif cmd == "cross-run":
            if not extra:
                raise SystemExit("make cross-run: usage: ./make cross-run <triple-or-preset> [file.ny] [-- program-args]")
            triple = extra[0]
            rest = extra[1:]
            ny_bin = resolve_tool_bin(build_root, active_kind, "ny")
            rc = subprocess.run([str(ny_bin), f"--cross={triple}", "--cross-run", *rest]).returncode
        elif cmd == "profile":
            rc = run_make_profile(build_root, active_kind, jobs, extra)
        elif cmd == "gprof":
            rc = run_tool(build_root, active_kind, "ny-perf", ["profile", *extra])
        elif cmd == "asan":
            rc = run_test(build_root, active_kind, requested_jobs, extra)
        elif cmd == "ubsan":
            rc = run_test(build_root, active_kind, requested_jobs, extra)
        elif cmd == "fuzz":
            cmake_build(build_root, active_kind, ["ny", "ny-test", "ny-fuzz"], requested_jobs)
            if extra and extra[0] == "afl":
                afl = shutil.which("afl-fuzz")
                if not afl:
                    raise SystemExit("make fuzz afl: afl-fuzz not found")
                afl_args = _strip_dashdash(extra[1:])
                if not afl_args:
                    raise SystemExit("make fuzz afl: pass afl-fuzz args after --")
                rc = subprocess.run([afl, *afl_args], cwd=str(ROOT)).returncode
            elif extra:
                rc = run_tool(build_root, active_kind, "ny-fuzz", extra)
            else:
                rc = run_tool(build_root, active_kind, "ny-fuzz", ["validate-shapes", default_fuzz_shape_dir()])
        elif cmd == "bench":
            rc = run_tool(build_root, active_kind, "ny-test", ["--bench", *extra])
        elif cmd == "optcheck":
            rc = run_optcheck(build_root, active_kind, extra)
        elif cmd == "fb":
            raise SystemExit(
                f"make: command '{cmd}' is not implemented on the native C path.\n"
                "  For optimization correctness, use: ./make test\n"
                "  For fuzz/shape validation, use:  ./make fuzz [validate-shapes etc/tests/shapes]\n"
                "  For benchmarks, use:             ./make bench"
            )
        else:
            raise SystemExit(f"make: unsupported command: {cmd}")

        if rc != 0:
            return rc

    return 0

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        restore_tty_visuals()
        print()
        raise SystemExit(130)
