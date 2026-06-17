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
    for name in ("clang", "cc", "gcc"):
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
    if not os.environ.get("CC"):
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

def _optional_dep_exists(name: str) -> bool:
    if _pkg_exists(name):
        return True
    if name == "z3":
        return bool(which("z3"))
    return False

def _gmp_available() -> bool:
    env_inc = (os.environ.get("NYTRIX_GMP_INCLUDE") or os.environ.get("GMP_INCLUDE_DIR") or "").strip()
    env_lib = (os.environ.get("NYTRIX_GMP_LIBRARY") or os.environ.get("GMP_LIBRARY") or "").strip()
    if host_os() == "windows":
        if env_inc and env_lib and (_windows_env_path(env_inc) / "gmp.h").exists() and _windows_env_path(env_lib).exists():
            return True
        if _windows_find_vcpkg_gmp(_windows_vcpkg_root()) or _windows_find_msys2_gmp():
            return True
        return False
    if _pkg_exists("gmp"):
        return True
    if env_inc and (Path(env_inc) / "gmp.h").exists():
        return True
    if env_inc and env_lib and Path(env_lib).exists():
        return True
    candidates = [
        Path("/usr/include/gmp.h"),
        Path("/usr/local/include/gmp.h"),
        Path("/opt/homebrew/include/gmp.h"),
        Path(r"C:\vcpkg\installed\x64-windows\include\gmp.h"),
        Path(r"C:\msys64\mingw64\include\gmp.h"),
    ]
    return any(p.exists() for p in candidates)

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
            "libgmp-dev",
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
            "gmp",
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
            "gmp-devel",
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
            log("DEPS", "skipping optional std/native deps; set NYTRIX_INSTALL_STD_DEPS=1 or run ./make deps later")
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
                "gmp",
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
    os.environ["MSYSTEM_PREFIX"] = _windows_cmake_path(prefix)
    os.environ["MINGW_PREFIX"] = _windows_cmake_path(prefix)
    _prepend_env_path("PATH", bin_dir)
    _prepend_env_path("CMAKE_PREFIX_PATH", prefix)
    _prepend_env_path("PKG_CONFIG_PATH", prefix / "lib" / "pkgconfig")
    _prepend_env_path("PKG_CONFIG_PATH", prefix / "share" / "pkgconfig")
    os.environ.setdefault("ZLIB_ROOT", _windows_cmake_path(prefix))
    cc = _windows_tool_path(bin_dir, "clang")
    cxx = _windows_tool_path(bin_dir, "clang++")
    if cc:
        os.environ.setdefault("CC", str(cc))
    if cxx:
        os.environ.setdefault("CXX", str(cxx))
    pkg_config = _windows_tool_path(bin_dir, "pkg-config") or _windows_tool_path(bin_dir, "pkgconf")
    if pkg_config:
        os.environ.setdefault("PKG_CONFIG", str(pkg_config))

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
        pkgs.append(f"{pkg_prefix}-gmp")
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
        _windows_install_msys2_deps(["llvm", "clang", "cmake", "ninja", "pkg-config", "gmp"])
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

def _windows_find_vcpkg_gmp(vcpkg_root: Path) -> tuple[Path, Path] | None:
    triplet = (os.environ.get("VCPKG_DEFAULT_TRIPLET") or "x64-windows").strip() or "x64-windows"
    installs = [ROOT / "vcpkg_installed" / triplet, vcpkg_root / "installed" / triplet]
    for install in installs:
        include_dir = install / "include"
        if not (include_dir / "gmp.h").exists():
            continue
        for lib_name in ("gmp.lib", "libgmp.lib"):
            lib_path = install / "lib" / lib_name
            if lib_path.exists():
                return include_dir, lib_path
    return None

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

def _windows_find_msys2_gmp() -> tuple[Path, Path] | None:
    gnu_ok = _windows_prefers_gnu_toolchain()
    for root in _windows_msys_prefixes():
        include_dir = root / "include"
        if not (include_dir / "gmp.h").exists():
            continue
        lib_dir = root / "lib"
        names = ["gmp.lib", "libgmp.lib"]
        if gnu_ok:
            names.extend(["libgmp.dll.a", "libgmp.a"])
        else:
            # Plain cmd.exe still commonly uses the MSYS2/UCRT LLVM toolchain.
            # Accept its GMP import/static libraries so users do not need to
            # start inside an MSYS2 shell just to configure the build.
            names.extend(["libgmp.dll.a", "libgmp.a"])
        for lib_name in names:
            lib_path = lib_dir / lib_name
            if lib_path.exists():
                return include_dir, lib_path
    return None

def _windows_configure_gmp_env(include_dir: Path, library: Path) -> None:
    os.environ["NYTRIX_GMP_INCLUDE"] = str(include_dir)
    os.environ["NYTRIX_GMP_LIBRARY"] = str(library)
    os.environ["GMP_INCLUDE_DIR"] = str(include_dir)
    os.environ["GMP_LIBRARY"] = str(library)

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

def _windows_ensure_gmp() -> None:
    if host_os() != "windows":
        return
    env_inc = (os.environ.get("NYTRIX_GMP_INCLUDE") or os.environ.get("GMP_INCLUDE_DIR") or "").strip()
    env_lib = (os.environ.get("NYTRIX_GMP_LIBRARY") or os.environ.get("GMP_LIBRARY") or "").strip()
    if env_inc and env_lib and (_windows_env_path(env_inc) / "gmp.h").exists() and _windows_env_path(env_lib).exists():
        _windows_configure_gmp_env(_windows_env_path(env_inc), _windows_env_path(env_lib))
        return
    if _windows_prefers_gnu_toolchain():
        found = _windows_find_msys2_gmp()
        if found:
            _windows_configure_gmp_env(*found)
            return
    vcpkg_root = _windows_vcpkg_root()
    found = _windows_find_vcpkg_gmp(vcpkg_root)
    if found:
        _windows_configure_gmp_env(*found)
        return
    found = _windows_find_msys2_gmp()
    if found:
        _windows_configure_gmp_env(*found)
        return
    provider = (os.environ.get("NYTRIX_WINDOWS_GMP_PROVIDER") or "system").strip().lower()
    if provider not in ("vcpkg", "vcpkg-build"):
        raise SystemExit(
            "GMP headers/library not found for Windows. Install a prebuilt package "
            "(MSYS2 UCRT: mingw-w64-ucrt-x86_64-gmp) or set NYTRIX_GMP_INCLUDE and "
            "NYTRIX_GMP_LIBRARY. Set NYTRIX_WINDOWS_GMP_PROVIDER=vcpkg only when a slow "
            "vcpkg source build is acceptable."
        )
    vcpkg = vcpkg_root / "vcpkg.exe"
    if not vcpkg.exists():
        raise SystemExit("GMP headers not found and vcpkg is unavailable; install GMP or set NYTRIX_GMP_INCLUDE/NYTRIX_GMP_LIBRARY.")
    triplet = (os.environ.get("VCPKG_DEFAULT_TRIPLET") or "x64-windows").strip() or "x64-windows"
    step(f"deps: vcpkg install gmp:{triplet}")
    manifest = ROOT / "vcpkg.json"
    created_manifest = False
    if not manifest.exists():
        baseline = _windows_vcpkg_builtin_baseline(vcpkg_root)
        if not baseline:
            raise SystemExit("vcpkg manifest mode requires a builtin-baseline; set VCPKG_BUILTIN_BASELINE or use a git checkout of vcpkg.")
        manifest.write_text(
            '{\n'
            '  "name": "nytrix-local-deps",\n'
            '  "version-string": "0.1.0",\n'
            f'  "builtin-baseline": "{baseline}",\n'
            '  "dependencies": ["gmp"]\n'
            '}\n',
            encoding="utf-8",
        )
        created_manifest = True
    try:
        run([str(vcpkg), "install", "--triplet", triplet])
    finally:
        if created_manifest:
            manifest.unlink(missing_ok=True)
    found = _windows_find_vcpkg_gmp(vcpkg_root)
    if not found:
        raise SystemExit(f"vcpkg installed gmp:{triplet}, but gmp.h/libgmp were not found under {vcpkg_root}")
    _windows_configure_gmp_env(*found)

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
    if not which("llvm-config") and host_os() != "windows":
        missing.append("llvm")
    if host_os() == "windows":
        if not (which("clang") or Path(r"C:\Program Files\LLVM\bin\clang.exe").exists()):
            missing.append("llvm")
        if not which("cmake"):
            missing.append("cmake")
        if require_git and not which("git"):
            missing.append("git")
    if not _gmp_available():
        missing.append("gmp")
    missing = _dedupe(missing)

    if not missing:
        _windows_ensure_llvm()
        _windows_ensure_gmp()
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
                    pkgs += [f"libclang-{v}-dev"]
                else:
                    pkgs += ["libclang-dev"]
            else:
                pkgs += ["clang", "llvm-dev", "libclang-dev"]
            pkgs += ["libgmp-dev"]
            step("deps: apt update")
            run(["sudo", "apt", "update"])
            step("deps: apt install")
            run(["sudo", "apt", "install", "-y", *pkgs])
            _install_optional_std_deps(force_optional_prompt)
            return
        if distro in ("arch", "manjaro") or "arch" in like:
            step("deps: pacman install")
            run(["sudo", "pacman", "-Sy", "--noconfirm", "base-devel", "python", "clang", "cmake", "ninja", "git", "gdb", "llvm", "pkgconf", "gmp", "zlib"])
            _install_optional_std_deps(force_optional_prompt)
            return
        if distro in ("fedora", "rhel", "centos", "rocky") or "fedora" in like or "rhel" in like:
            step("deps: dnf install")
            run(["sudo", "dnf", "install", "-y", "@development-tools", "clang", "llvm-devel", "cmake", "ninja-build", "git", "gdb", "pkgconf-pkg-config", "gmp-devel", "zlib-devel"])
            _install_optional_std_deps(force_optional_prompt)
            return
    if os_name == "macos":
        if not which("brew"):
            err("brew not found; install Homebrew first.")
            raise SystemExit(1)
        step("deps: brew install")
        pkgs: list[str] = []
        for dep, pkg in (("cmake", "cmake"), ("ninja", "ninja"), ("pkg-config", "pkg-config"), ("gmp", "gmp")):
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
                    _windows_run_install(cmd)
        _windows_ensure_llvm()
        _windows_ensure_gmp()
        _install_optional_std_deps(force_optional_prompt)
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
    if mem_gib > 0.0:
        auto = min(auto, max(1, int(mem_gib / 2.0)))
    auto = min(auto, 24)
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

def cmake_configure(build_root: Path, kind: str) -> Path:
    configure_macos_llvm_env()
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
            # rpath so the built binary finds libs at runtime without LD_LIBRARY_PATH.
            extra_ldflags.append(f"-Wl,-rpath,'$ORIGIN/../vendor/lib/host'")
    host_cflags = cmake_flag_list(os.environ.get("NYTRIX_HOST_CFLAGS") or "")
    raw_ldflags = os.environ.get("NYTRIX_HOST_LDFLAGS") or ""
    if extra_ldflags:
        raw_ldflags = (raw_ldflags + " " + " ".join(extra_ldflags)).strip()
    host_ldflags = cmake_flag_list(raw_ldflags)
    cache_matches_flags = True
    if cache.exists():
        cache_matches_flags = (
            cmake_cache_value(bdir, "NYTRIX_HOST_CFLAGS", "") == host_cflags and
            cmake_cache_value(bdir, "NYTRIX_HOST_LDFLAGS", "") == host_ldflags
        )
    if cache.exists() and cmake_configure_complete(bdir) and cache_matches_flags:
        boot_ok(f"cmake ({kind}) up to date (unchanged)")
        return bdir
    if cache.exists() and not cmake_configure_complete(bdir):
        cache.unlink(missing_ok=True)
        shutil.rmtree(bdir / "CMakeFiles", ignore_errors=True)
    boot_step(f"cmake configure ({kind})")
    cfg = "Debug" if kind in ("debug", "asan", "ubsan") else "Release"
    cmd = [
        "cmake", "-S", str(ROOT), "-B", str(bdir),
        f"-DCMAKE_BUILD_TYPE={cfg}", "-DNYTRIX_FAST_BUILD=ON",
        f"-DNYTRIX_HOST_CFLAGS={host_cflags}",
        f"-DNYTRIX_HOST_LDFLAGS={host_ldflags}",
    ]
    if host_os() == "windows":
        cc = _windows_cmake_tool(os.environ.get("CC") or "")
        cxx = _windows_cmake_tool(os.environ.get("CXX") or "")
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

def ny_fast_run_args(args: list[str]) -> list[str]:
    has_run = any(a == "-run" or a.startswith("--run") for a in args)
    if not has_run:
        return args
    explicit_mode = any(
        a in ("--jit", "-O0", "-O1", "-O2", "-O3", "-g")
        or a.startswith("--profile=")
        or a.startswith("--run=")
        or a.startswith("-passes=")
        for a in args
    )
    if explicit_mode:
        return args
    return ["--run=jit" if a == "-run" else a for a in args]

NY_VALUE_OPTS = {
    "-o", "--output", "-timeout", "--std-path", "--bundle-std", "--bundle-symbols",
    "--emit-artifact", "--emit-ir", "--emit-bc", "--emit-asm", "--dump-dir",
    "--entry-name", "--extract-at", "--extract-lang", "--host-triple",
    "--host-cflags", "--host-ldflags", "--arm-float-abi", "--dwarf-version", "--gpu",
    "--gpu-backend", "--gpu-offload", "--gpu-min-work", "--accel-target",
    "--accel-object", "--gpu-target", "--parallel", "--threads",
    "--parallel-min-work", "--heap", "--mode", "--max-errors", "--warn",
    "--profile", "-passes",
}

NY_PREFIX_VALUE_OPTS = (
    "--output=", "--std-path=", "--bundle-std=", "--bundle-symbols=",
    "--emit-artifact=", "--emit-ir=", "--emit-bc=", "--emit-asm=", "--dump-dir=",
    "--entry-name=", "--extract-at=", "--extract-lang=", "--host-triple=",
    "--host-cflags=", "--host-ldflags=", "--arm-float-abi=", "--dwarf-version=", "--gpu=",
    "--gpu-backend=", "--gpu-offload=", "--gpu-min-work=", "--accel-target=",
    "--accel-object=", "--gpu-target=", "--parallel=", "--threads=",
    "--parallel-min-work=", "--heap=", "--mode=", "--max-errors=", "--warn=",
    "--profile=", "-passes=",
)

NY_RUN_CACHE_BLOCKERS = {
    "--jit", "-emit-only", "-o", "--output", "-i", "--interactive", "--repl",
    "-c", "-e", "--eval", "-ic", "-ci", "--eval-repl", "-dump-ast",
    "--expand", "--expand-json", "-dump-llvm", "-dump-tokens", "--extract-code",
    "--extract-json", "-dump-docs", "-dump-funcs", "-dump-symbols", "-dump-stats",
    "-prof", "--prof", "-verify", "-g", "--emit-ir", "--emit-bc", "--emit-asm",
    "--dump-on-error", "--dump-diagnose", "-trace",
}

NY_SUBCOMMANDS = {"fmt", "test", "doc", "web", "perf", "make", "pkg", "get", "install", "new", "c2ny", "ny-lsp"}

def _ny_arg_takes_value(arg: str) -> bool:
    return arg in NY_VALUE_OPTS

def _ny_source_index(args: list[str]) -> int:
    skip = False
    for i, arg in enumerate(args):
        if skip:
            skip = False
            continue
        if _ny_arg_takes_value(arg):
            skip = True
            continue
        if arg.startswith(NY_PREFIX_VALUE_OPTS):
            continue
        if arg.startswith("-L") or arg.startswith("-l"):
            continue
        if arg.startswith("-"):
            continue
        if arg in NY_SUBCOMMANDS:
            return -1
        p = (ROOT / arg).resolve() if not Path(arg).is_absolute() else Path(arg)
        if arg.endswith(".ny") or p.exists():
            return i
    return -1

_NY_USE_RE = re.compile(r"^\s*use\s+([A-Za-z_][A-Za-z0-9_.]*)")

def _ny_module_path(mod: str) -> Path | None:
    if not mod.startswith("std."):
        return None
    rel = Path(*mod[4:].split("."))
    for cand in (ROOT / "lib" / rel.with_suffix(".ny"), ROOT / "lib" / rel / "mod.ny"):
        if cand.exists():
            return cand.resolve()
    return None

def _ny_import_graph(source: Path) -> list[Path]:
    seen_files: set[Path] = set()
    stack = [source.resolve()]
    while stack:
        path = stack.pop()
        if path in seen_files or not path.exists():
            continue
        seen_files.add(path)
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        for line in text.splitlines():
            m = _NY_USE_RE.match(line)
            if not m:
                continue
            dep = _ny_module_path(m.group(1))
            if dep is not None and dep not in seen_files:
                stack.append(dep)
    return sorted(seen_files)

def _hash_file_identity(h: "hashlib._Hash", path: Path) -> None:
    try:
        st = path.stat()
    except OSError:
        return
    rel = str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path)
    h.update(rel.encode("utf-8", "ignore"))
    h.update(str(st.st_size).encode())
    h.update(str(st.st_mtime_ns).encode())

def _ny_run_cache_key(launch: str, args: list[str], source: Path, build_root: Path, kind: str) -> str:
    h = hashlib.sha256()
    h.update(b"ny-run-cache-v3\0")
    h.update((" ".join(args[:_ny_source_index(args) + 1])).encode("utf-8", "ignore"))
    for name in (
        "NYTRIX_STD_PATH",
        "NYTRIX_BUILD_STD_PATH",
        "NYTRIX_HOST_TRIPLE",
        "NYTRIX_HOST_CFLAGS",
        "NYTRIX_HOST_LDFLAGS",
        "NYTRIX_ARM_FLOAT_ABI",
    ):
        h.update(name.encode())
        h.update(b"=")
        h.update((os.environ.get(name) or "").encode("utf-8", "ignore"))
        h.update(b"\0")
    for path in [
        Path(launch),
        cmake_build_dir(build_root, kind) / "std.ny",
        *(Path(os.environ[name]).expanduser() for name in ("NYTRIX_STD_PATH", "NYTRIX_BUILD_STD_PATH") if os.environ.get(name)),
        *_ny_import_graph(source),
    ]:
        _hash_file_identity(h, path)
    return h.hexdigest()[:24]

def run_ny_cached(build_root: Path, kind: str, args: list[str]) -> int | None:
    if not _env_flag("NYTRIX_MAKE_NY_RUN_CACHE", True):
        return None
    if "-run" not in args:
        return None
    if any(a.startswith("--run=") for a in args):
        return None
    if any(a in NY_RUN_CACHE_BLOCKERS or a in ("-O1", "-O2", "-O3", "-O0") or a.startswith("--profile=") or a.startswith("-passes=") for a in args):
        return None
    src_i = _ny_source_index(args)
    if src_i < 0:
        return None
    source_arg = args[src_i]
    source = (ROOT / source_arg).resolve() if not Path(source_arg).is_absolute() else Path(source_arg)
    if not source.exists():
        return None
    binp = resolve_tool_bin(build_root, kind, "ny")
    launch = tool_launch_path(binp)
    key = _ny_run_cache_key(launch, args, source, build_root, kind)
    cache_dir = build_root / "cache" / "make-run" / key
    exe = ".exe" if host_os() == "windows" else ""
    cached = cache_dir / f"ny-run{exe}"
    env = os.environ.copy()
    env.setdefault("NYTRIX_STD_CACHE", "1")
    env.setdefault("NYTRIX_STD_BC_CACHE_AUTO", "1")
    program_args = args[src_i + 1:]
    if program_args and program_args[0] == "--":
        program_args = program_args[1:]
    if not cached.exists():
        cache_dir.mkdir(parents=True, exist_ok=True)
        compile_front = [a for a in args[:src_i] if a != "-run"]
        compile_args = [launch, "--profile=compile", *compile_front, "-o", str(cached), source_arg]
        rc = subprocess.Popen(compile_args, cwd=str(ROOT), env=env).wait()
        if rc != 0:
            cached.unlink(missing_ok=True)
            return rc
        chmod_executable(cached)
    else:
        log("CACHE", f"ny -run using {cached.relative_to(ROOT)}")
    try:
        return subprocess.Popen([str(cached), *program_args], cwd=str(ROOT), env=env).wait()
    except KeyboardInterrupt:
        return 130

CROSS_PRESETS = {
    "linux-x64": ("x86_64-linux-gnu", "qemu-x86_64", ""),
    "linux-x86_64": ("x86_64-linux-gnu", "qemu-x86_64", ""),
    "x86_64-linux": ("x86_64-linux-gnu", "qemu-x86_64", ""),
    "linux-arm64": ("aarch64-linux-gnu", "qemu-aarch64", ""),
    "linux-aarch64": ("aarch64-linux-gnu", "qemu-aarch64", ""),
    "aarch64-linux": ("aarch64-linux-gnu", "qemu-aarch64", ""),
    "linux-armhf": ("arm-linux-gnueabihf", "qemu-arm", "hard"),
    "linux-arm": ("arm-linux-gnueabihf", "qemu-arm", "hard"),
    "linux-riscv64": ("riscv64-linux-gnu", "qemu-riscv64", ""),
    "riscv64-linux": ("riscv64-linux-gnu", "qemu-riscv64", ""),
    "windows-x64": ("x86_64-w64-windows-gnu", "wine", ""),
    "windows-x86_64": ("x86_64-w64-windows-gnu", "wine", ""),
}

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

def _demo_keywords_from_source(source: str) -> list[str]:
    path = ROOT / source
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []
    for raw in lines[:24]:
        line = raw.strip()
        low = line.lower()
        if low.startswith(";; keywords:"):
            tail = line.split(":", 1)[1].strip()
            return [w.strip().lower() for w in tail.replace(",", " ").split() if w.strip()]
        if line and not line.startswith("#!") and not line.startswith(";;"):
            break
    return []

def _demo_comment_title(source: str) -> str:
    path = ROOT / source
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return ""
    for raw in lines[:24]:
        line = raw.strip()
        low = line.lower()
        if low.startswith(";; keywords:"):
            continue
        if line.startswith(";;"):
            text = line[2:].strip()
            if not text:
                continue
            if " - http" in text:
                text = text.split(" - http", 1)[0].strip()
            if len(text) <= 48:
                return text.rstrip(".")
        elif line and not line.startswith("#!"):
            break
    return ""

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

def _parse_web_demo_args(args: list[str], build_root: Path) -> dict[str, object]:
    out_dir = build_root / "wasm"
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
    print("  ./make web-demos --out build/wasm")
    print("")
    print("Flags:")
    print("  --out DIR         output directory")
    print("  --no-ny-wasm      skip compiling manifest Ny sources")
    print("  --require-ny-wasm fail unless every manifest source emits wasm")
    print("  --clean           remove the output directory before writing")

def _demo_wasm_name(demo_id: str) -> str:
    safe = "".join(ch if ch.isalnum() or ch in ("-", "_") else "_" for ch in demo_id.strip())
    return (safe or "demo") + ".wasm"

def _tail_text(value: object, limit: int = 4000) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        value = value.decode("utf-8", "replace")
    return str(value)[-limit:]

WASM_DEFAULT_EXPORTS = (
    "_ny_top_entry",
    "main",
    "ny_web_init",
    "ny_web_main",
    "ny_web_frame",
    "ny_web_render",
)

def _wasm_toolchain() -> tuple[str | None, str]:
    clang = which("clang")
    if not clang:
        return None, "clang missing"
    if "WebAssembly" not in _llvm_targets_built():
        return None, "LLVM WebAssembly backend not built"
    if not which("wasm-ld"):
        return None, "wasm-ld missing"
    return clang, ""

def _ny_wasm_env(build_root: Path, kind: str) -> tuple[Path, Path, dict[str, str]]:
    ny_bin = resolve_tool_bin(build_root, kind, "ny")
    std_file = cmake_build_dir(build_root, kind) / "std.ny"
    env = os.environ.copy()
    env.setdefault("NYTRIX_ROOT", str(ROOT))
    env["NYTRIX_STD_CACHE"] = "0"
    env["NYTRIX_STD_BC_CACHE_AUTO"] = "0"
    if std_file.exists():
        env["NYTRIX_STD_PATH"] = str(std_file)
        env["NYTRIX_BUILD_STD_PATH"] = str(std_file)
    return ny_bin, std_file, env

def _resolve_wasm_path(path: Path | str) -> Path:
    p = Path(path)
    if not p.is_absolute():
        p = ROOT / p
    return p

def _compile_ny_to_wasm(
    build_root: Path,
    kind: str,
    source: Path,
    wasm: Path,
    ir: Path,
    exports: tuple[str, ...] = WASM_DEFAULT_EXPORTS,
    allow_undefined: bool = True,
    initial_memory: int = 16777216,
    max_memory: int = 67108864,
    opt: str = "-O2",
    step_timeout: int = 120,
) -> dict[str, object]:
    clang, tool_err = _wasm_toolchain()
    if tool_err:
        return {"ok": False, "stage": "toolchain", "detail": tool_err}
    try:
        ny_bin, std_file, env = _ny_wasm_env(build_root, kind)
    except SystemExit as exc:
        return {"ok": False, "stage": "toolchain", "detail": str(exc)}
    source = _resolve_wasm_path(source)
    wasm = _resolve_wasm_path(wasm)
    ir = _resolve_wasm_path(ir)
    if not source.exists():
        return {"ok": False, "stage": "source", "detail": f"missing source {source}"}
    wasm.parent.mkdir(parents=True, exist_ok=True)
    ir.parent.mkdir(parents=True, exist_ok=True)
    ny_cmd = [
        str(ny_bin),
        "--host-triple=wasm32-unknown-unknown",
        f"--emit-ir={ir}",
        "-emit-only",
        str(source),
    ]
    if std_file.exists():
        ny_cmd.insert(1, f"--std-path={std_file}")
    try:
        ny_res = subprocess.run(ny_cmd, cwd=str(ROOT), env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=step_timeout)
    except subprocess.TimeoutExpired as exc:
        return {"ok": False, "stage": "ny", "detail": "ny ir timed out", "output": _tail_text(exc.stdout), "timeout": step_timeout}
    if ny_res.returncode != 0 or not ir.exists():
        return {"ok": False, "stage": "ny", "detail": "ny ir failed", "output": _tail_text(ny_res.stdout)}
    clang_cmd = [
        str(clang),
        "--target=wasm32",
        opt,
        "-nostdlib",
        "-Wl,--no-entry",
        "-Wl,--export-memory",
    ]
    clang_cmd.extend(f"-Wl,--export-if-defined={name}" for name in exports if name)
    if allow_undefined:
        clang_cmd.append("-Wl,--allow-undefined")
    clang_cmd.extend([
        f"-Wl,--initial-memory={initial_memory}",
        f"-Wl,--max-memory={max_memory}",
        "-o",
        str(wasm),
        str(ir),
    ])
    try:
        clang_res = subprocess.run(clang_cmd, cwd=str(ROOT), text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=step_timeout)
    except subprocess.TimeoutExpired as exc:
        return {"ok": False, "stage": "clang", "detail": "wasm link timed out", "output": _tail_text(exc.stdout), "timeout": step_timeout}
    if clang_res.returncode != 0 or not wasm.exists():
        return {"ok": False, "stage": "clang", "detail": "wasm link failed", "output": _tail_text(clang_res.stdout)}
    return {"ok": True, "source": str(source), "ir": str(ir), "wasm": str(wasm)}

def _build_ny_demo_wasm(out_dir: Path, build_root: Path, kind: str, manifest: list[dict[str, object]]) -> tuple[int, int, str]:
    wasm_dir = out_dir / "wasm"
    ir_dir = out_dir / "ny-ir"
    wasm_dir.mkdir(parents=True, exist_ok=True)
    ir_dir.mkdir(parents=True, exist_ok=True)
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
        ir = ir_dir / (Path(_demo_wasm_name(demo_id)).with_suffix(".ll").name)
        wasm = wasm_dir / _demo_wasm_name(demo_id)
        res = _compile_ny_to_wasm(build_root, kind, Path(source), wasm, ir, step_timeout=step_timeout)
        if not bool(res.get("ok", False)):
            failed += 1
            item["wasmStatus"] = str(res.get("detail", "wasm failed"))
            report.append({
                "id": demo_id,
                "source": source,
                "ok": False,
                "stage": str(res.get("stage", "")),
                "detail": str(res.get("detail", "")),
                "output": _tail_text(res.get("output", "")),
            })
            continue
        built += 1
        item["wasm"] = "wasm/" + wasm.name
        item["wasmBase64"] = base64.b64encode(wasm.read_bytes()).decode("ascii")
        item["wasmKind"] = "ny"
        item["wasmStatus"] = "ok"
        report.append({"id": demo_id, "source": source, "ok": True, "wasm": item["wasm"]})
    (out_dir / "ny-wasm-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return built, failed, f"{built}/{len([x for x in manifest if str(x.get('source', '')).strip()])} manifest Ny sources emitted wasm"

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
    ny_built = 0
    ny_failed = 0
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

def print_wasm_help() -> None:
    print(c("1;36", "Nytrix wasm compiler"))
    print("")
    print("Usage:")
    print("  ./make wasm path/to/app.ny")
    print("  ./make wasm path/to/app.ny --out build/wasm/app.wasm")
    print("")
    print("Flags:")
    print("  -o, --out FILE      output wasm file")
    print("  --ir FILE           output LLVM IR file")
    print("  --timeout SEC       per-stage timeout")
    print("  --export NAME       export an extra symbol")
    print("  --no-undefined      reject unresolved host imports at link time")

def _parse_wasm_args(args: list[str], build_root: Path) -> dict[str, object]:
    if not args or any(a in ("-h", "--help") for a in args):
        return {"help": True}
    source: Path | None = None
    out_path: Path | None = None
    ir_path: Path | None = None
    exports = list(WASM_DEFAULT_EXPORTS)
    allow_undefined = True
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
        if a.startswith("-"):
            raise SystemExit(f"wasm: unknown option {a}")
        if source is not None:
            raise SystemExit(f"wasm: unexpected extra argument {a}")
        source = Path(a)
        i += 1
    if source is None:
        raise SystemExit("wasm: missing Ny source")
    src_abs = _resolve_wasm_path(source)
    stem = src_abs.stem or "app"
    if out_path is None:
        out_path = build_root / "wasm" / (stem + ".wasm")
    if out_path.suffix.lower() != ".wasm":
        out_path = out_path / (stem + ".wasm")
    if ir_path is None:
        ir_path = build_root / "wasm-ir" / (stem + ".ll")
    return {
        "help": False,
        "source": src_abs,
        "out": _resolve_wasm_path(out_path),
        "ir": _resolve_wasm_path(ir_path),
        "exports": tuple(dict.fromkeys(exports)),
        "allow_undefined": allow_undefined,
        "timeout": max(1, timeout_sec),
    }

def run_wasm(build_root: Path, kind: str, args: list[str]) -> int:
    cfg = _parse_wasm_args(args, build_root)
    if bool(cfg.get("help", False)):
        print_wasm_help()
        return 0
    res = _compile_ny_to_wasm(
        build_root,
        kind,
        cfg["source"],  # type: ignore[arg-type]
        cfg["out"],     # type: ignore[arg-type]
        cfg["ir"],      # type: ignore[arg-type]
        exports=cfg["exports"],  # type: ignore[arg-type]
        allow_undefined=bool(cfg.get("allow_undefined", True)),
        step_timeout=int(cfg.get("timeout", 120)),
    )
    if not bool(res.get("ok", False)):
        detail = str(res.get("detail", "wasm failed"))
        output = _tail_text(res.get("output", ""), 1600)
        if output:
            print(output)
        raise SystemExit("wasm: " + detail)
    ok("wasm: " + _rel_or_abs(Path(str(res["wasm"]))))
    log("WASM", "ir: " + _rel_or_abs(Path(str(res["ir"]))))
    return 0

def _cross_slug(triple: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.+-]+", "-", triple).strip("-") or "target"

def _cross_qemu_for_triple(triple: str) -> str:
    t = triple.lower()
    if "w64-windows" in t or "windows" in t:
        return "wine"
    if t.startswith("aarch64") or t.startswith("arm64"):
        return "qemu-aarch64"
    if t.startswith("arm"):
        return "qemu-arm"
    if t.startswith("riscv64"):
        return "qemu-riscv64"
    if t.startswith("x86_64") or t.startswith("amd64"):
        return "qemu-x86_64"
    if t.startswith("i386") or t.startswith("i686"):
        return "qemu-i386"
    return ""

def _cross_target(raw: str) -> tuple[str, str, str]:
    key = raw.strip().lower()
    if key in CROSS_PRESETS:
        return CROSS_PRESETS[key]
    return raw, _cross_qemu_for_triple(raw), ""

def _cross_is_windows(triple: str) -> bool:
    t = (triple or "").lower()
    return "windows" in t or "mingw" in t or "w64" in t

def _cross_default_output(build_root: Path, triple: str, source: Path) -> Path:
    name = source.with_suffix("").name
    if _cross_is_windows(triple) and not name.lower().endswith(".exe"):
        name += ".exe"
    return build_root / "cache" / "cross" / _cross_slug(triple) / name

def _cross_first_word(raw: str) -> str:
    raw = (raw or "").strip()
    if not raw:
        return ""
    try:
        parts = shlex.split(raw)
    except ValueError:
        parts = raw.split()
    return parts[0] if parts else ""

def _cross_tool_exists(raw: str) -> bool:
    tool = _cross_first_word(raw)
    if not tool:
        return False
    if "/" in tool or "\\" in tool:
        return Path(tool).expanduser().exists()
    return bool(_tool_path(tool))

_LLVM_TARGETS_BUILT_CACHE: set[str] | None = None

def _llvm_targets_built() -> set[str]:
    global _LLVM_TARGETS_BUILT_CACHE
    if _LLVM_TARGETS_BUILT_CACHE is not None:
        return _LLVM_TARGETS_BUILT_CACHE
    res = run_capture(["llvm-config", "--targets-built"])
    if res.returncode != 0:
        _LLVM_TARGETS_BUILT_CACHE = set()
    else:
        _LLVM_TARGETS_BUILT_CACHE = {item.strip() for item in res.stdout.split() if item.strip()}
    return _LLVM_TARGETS_BUILT_CACHE

def _cross_llvm_backend_for_triple(triple: str) -> str:
    arch = (triple or "").split("-", 1)[0].lower()
    if arch in ("x86_64", "amd64", "i386", "i486", "i586", "i686"):
        return "X86"
    if arch in ("aarch64", "arm64"):
        return "AArch64"
    if arch.startswith("arm") or arch in ("thumb", "thumbv7"):
        return "ARM"
    if arch.startswith("riscv"):
        return "RISCV"
    if arch.startswith("ppc") or arch.startswith("powerpc"):
        return "PowerPC"
    if arch.startswith("wasm"):
        return "WebAssembly"
    return ""

def _cross_llvm_backend_issue(triple: str) -> str:
    backend = _cross_llvm_backend_for_triple(triple)
    built = _llvm_targets_built()
    if backend and built and backend not in built:
        return f"LLVM backend {backend} not built"
    return ""

def _mingw_prefix_for_triple(triple: str) -> str:
    t = (triple or "").lower()
    if t.startswith(("i686", "i386", "x86-w64")):
        return "i686-w64-mingw32"
    return "x86_64-w64-mingw32"

def _mingw_sysroot_for_triple(triple: str, explicit: str = "") -> str:
    for raw in (
        explicit,
        os.environ.get("NYTRIX_MINGW_SYSROOT", ""),
        os.environ.get("MINGW_SYSROOT", ""),
        os.environ.get("NYTRIX_CROSS_SYSROOT", ""),
    ):
        value = (raw or "").strip()
        if value:
            return os.path.expanduser(os.path.expandvars(value))
    prefix = _mingw_prefix_for_triple(triple)
    for path in (Path("/usr") / prefix, Path("/usr/local") / prefix):
        if (path / "include").is_dir() and (path / "lib").is_dir():
            return str(path)
    return ""

def _mingw_select_cc(triple: str, requested: str = "") -> tuple[str, str]:
    candidates: list[tuple[str, str]] = []
    if requested.strip():
        candidates.append((requested.strip(), "argument"))
    for env_name in ("NYTRIX_MINGW_CC", "NY_MINGW_CC"):
        value = (os.environ.get(env_name) or "").strip()
        if value:
            candidates.append((value, env_name))
    env_cc = (os.environ.get("NYTRIX_CC") or "").strip()
    if env_cc and "mingw" in env_cc.lower():
        candidates.append((env_cc, "NYTRIX_CC"))
    prefix = _mingw_prefix_for_triple(triple)
    candidates.extend([
        (f"{prefix}-gcc", "PATH"),
        (f"{prefix}-clang", "PATH"),
    ])
    for cc, source in candidates:
        if _cross_tool_exists(cc):
            return cc, source
    return "", ""

def _cross_compiler_needs_target_flag(cc: str, triple: str) -> bool:
    tool = Path(_cross_first_word(cc)).name.lower()
    prefix = (triple or "").lower()
    if prefix and tool.startswith(prefix + "-"):
        return False
    if _cross_is_windows(triple) and "w64-mingw32" in tool:
        return False
    return True

def _cross_is_linux(triple: str) -> bool:
    t = (triple or "").lower()
    return "linux" in t and not _cross_is_windows(t)

def _cross_is_native_linux(triple: str) -> bool:
    if host_os() != "linux" or not _cross_is_linux(triple):
        return False
    host_arch = (platform.machine() or "").lower()
    target_arch = (triple or "").split("-", 1)[0].lower()
    if host_arch in ("x86_64", "amd64"):
        return target_arch in ("x86_64", "amd64")
    if host_arch in ("aarch64", "arm64"):
        return target_arch in ("aarch64", "arm64")
    return host_arch == target_arch

def _linux_cross_sysroot_for_triple(triple: str, explicit: str = "") -> str:
    for raw in (
        explicit,
        os.environ.get("NYTRIX_CROSS_SYSROOT", ""),
        os.environ.get("NYTRIX_SYSROOT", ""),
    ):
        value = (raw or "").strip()
        if value:
            return os.path.expanduser(os.path.expandvars(value))
    if _cross_is_native_linux(triple):
        return ""
    for path in (Path("/usr") / triple, Path("/usr/local") / triple, Path("/opt") / triple):
        if (path / "lib").is_dir() or (path / "usr" / "lib").is_dir():
            return str(path)
    return ""

def _linux_cross_file_any(sysroot: str, triple: str, rels: tuple[str, ...]) -> bool:
    if not sysroot:
        return False
    base = Path(sysroot)
    triple_arch = (triple or "").split("-", 1)[0]
    expanded: list[Path] = []
    for rel in rels:
        expanded.append(base / rel)
        if rel.startswith("lib/"):
            expanded.append(base / "lib" / triple_arch / rel[4:])
            expanded.append(base / "usr" / "lib" / triple_arch / rel[4:])
        if rel.startswith("include/"):
            expanded.append(base / "usr" / rel)
    return any(path.exists() for path in expanded)

def _linux_select_cc(triple: str, requested: str = "") -> tuple[str, str]:
    candidates: list[tuple[str, str]] = []
    if requested.strip():
        candidates.append((requested.strip(), "argument"))
    for env_name in ("NYTRIX_CROSS_CC", "NYTRIX_CC"):
        value = (os.environ.get(env_name) or "").strip()
        if value:
            candidates.append((value, env_name))
    candidates.extend([
        (f"{triple}-gcc", "PATH"),
        (f"{triple}-clang", "PATH"),
    ])
    for cc, source in candidates:
        if _cross_tool_exists(cc):
            return cc, source
    return "", ""

def _linux_cross_missing(triple: str) -> list[str]:
    if not _cross_is_linux(triple) or _cross_is_native_linux(triple):
        return []
    missing: list[str] = []
    if not _linux_cross_sysroot_for_triple(triple):
        missing.append(f"sysroot for {triple}")
        return missing
    sysroot = _linux_cross_sysroot_for_triple(triple)
    if not _linux_cross_file_any(sysroot, triple, ("include/gmp.h",)):
        missing.append("target gmp headers")
    if not _linux_cross_file_any(sysroot, triple, ("lib/libgmp.so", "lib/libgmp.a")):
        missing.append("target gmp library")
    if not _linux_cross_file_any(sysroot, triple, ("include/zlib.h",)):
        missing.append("target zlib headers")
    if not _linux_cross_file_any(sysroot, triple, ("lib/libz.so", "lib/libz.a")):
        missing.append("target zlib library")
    cc, _ = _linux_select_cc(triple)
    if not cc and not _tool_path("clang"):
        missing.append(f"clang or {triple}-gcc")
    return missing

def _cross_compile_issues(triple: str) -> list[str]:
    issues: list[str] = []
    backend_issue = _cross_llvm_backend_issue(triple)
    if backend_issue:
        issues.append(backend_issue)
    if _cross_is_windows(triple):
        issues.extend(_mingw_runtime_missing(triple))
    else:
        issues.extend(_linux_cross_missing(triple))
    return issues

def _cross_runner_issue(runner: str) -> str:
    if not runner:
        return ""
    return "" if _tool_path(runner) else f"runner {runner} missing"

def _cross_status_detail(triple: str, runner: str) -> tuple[bool, str]:
    issues = _cross_compile_issues(triple)
    runner_issue = _cross_runner_issue(runner)
    if runner_issue:
        issues.append(runner_issue)
    if issues:
        return False, "missing " + ", ".join(issues)
    detail = "ready"
    if _cross_is_windows(triple):
        detail = _mingw_status_detail(triple)
    elif _cross_is_linux(triple) and not _cross_is_native_linux(triple):
        sysroot = _linux_cross_sysroot_for_triple(triple)
        cc, source = _linux_select_cc(triple)
        pieces: list[str] = []
        if cc:
            pieces.append(f"cc={cc} ({source})")
        elif _tool_path("clang"):
            pieces.append("cc=clang")
        if sysroot:
            pieces.append(f"sysroot={sysroot}")
        detail = ", ".join(pieces) if pieces else detail
    return True, detail

def _mingw_file_any(sysroot: str, rels: tuple[str, ...]) -> bool:
    if not sysroot:
        return False
    base = Path(sysroot)
    return any((base / rel).exists() for rel in rels)

def _mingw_runtime_missing(triple: str) -> list[str]:
    missing: list[str] = []
    cc, _ = _mingw_select_cc(triple)
    if not cc:
        missing.append(f"{_mingw_prefix_for_triple(triple)}-gcc")
    sysroot = _mingw_sysroot_for_triple(triple)
    if not _mingw_file_any(sysroot, ("include/gmp.h",)):
        missing.append("mingw gmp headers")
    if not _mingw_file_any(sysroot, ("lib/libgmp.dll.a", "lib/libgmp.a")):
        missing.append("mingw gmp library")
    if not _mingw_file_any(sysroot, ("include/zlib.h",)):
        missing.append("mingw zlib headers")
    if not _mingw_file_any(sysroot, ("lib/libz.dll.a", "lib/libz.a")):
        missing.append("mingw zlib library")
    return missing

def _mingw_status_detail(triple: str) -> str:
    cc, source = _mingw_select_cc(triple)
    sysroot = _mingw_sysroot_for_triple(triple)
    missing = _mingw_runtime_missing(triple)
    if not cc:
        return "missing compiler"
    detail = f"{cc} ({source})"
    if sysroot:
        detail += f", sysroot={sysroot}"
    if missing:
        detail += "; missing " + ", ".join(missing)
    return detail

def _arg_needs_value(args: list[str], i: int, name: str) -> str:
    if i + 1 >= len(args):
        raise SystemExit(f"make cross: missing value for {name}")
    return args[i + 1]

def _merge_flag_words(base: str, extra: str) -> str:
    base = (base or "").strip()
    extra = (extra or "").strip()
    if base and extra:
        return base + " " + extra
    return base or extra

def _parse_cross_args(args: list[str]) -> dict[str, object]:
    target = ""
    output = ""
    sysroot = ""
    qemu = ""
    cc = ""
    extra_cflags = ""
    extra_ldflags = ""
    ny_args: list[str] = []
    program_args: list[str] = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--":
            program_args = args[i + 1:]
            break
        if a in ("-h", "--help"):
            return {"help": True}
        if a in ("--target", "-target"):
            target = _arg_needs_value(args, i, a)
            i += 2
            continue
        if a.startswith("--target="):
            target = a.split("=", 1)[1]
            i += 1
            continue
        if a == "--sysroot":
            sysroot = _arg_needs_value(args, i, a)
            i += 2
            continue
        if a.startswith("--sysroot="):
            sysroot = a.split("=", 1)[1]
            i += 1
            continue
        if a == "--qemu":
            qemu = _arg_needs_value(args, i, a)
            i += 2
            continue
        if a.startswith("--qemu="):
            qemu = a.split("=", 1)[1]
            i += 1
            continue
        if a == "--cc":
            cc = _arg_needs_value(args, i, a)
            i += 2
            continue
        if a.startswith("--cc="):
            cc = a.split("=", 1)[1]
            i += 1
            continue
        if a == "--host-cflags":
            extra_cflags = _merge_flag_words(extra_cflags, _arg_needs_value(args, i, a))
            i += 2
            continue
        if a.startswith("--host-cflags="):
            extra_cflags = _merge_flag_words(extra_cflags, a.split("=", 1)[1])
            i += 1
            continue
        if a == "--host-ldflags":
            extra_ldflags = _merge_flag_words(extra_ldflags, _arg_needs_value(args, i, a))
            i += 2
            continue
        if a.startswith("--host-ldflags="):
            extra_ldflags = _merge_flag_words(extra_ldflags, a.split("=", 1)[1])
            i += 1
            continue
        if a in ("-o", "--output"):
            output = _arg_needs_value(args, i, a)
            i += 2
            continue
        if a.startswith("--output="):
            output = a.split("=", 1)[1]
            i += 1
            continue
        if a == "-run" or a.startswith("--run="):
            i += 1
            continue
        if not target and not a.startswith("-"):
            maybe = a.lower()
            if maybe in CROSS_PRESETS or "-" in maybe:
                target = a
                i += 1
                continue
        ny_args.append(a)
        i += 1
    return {
        "help": False,
        "target": target,
        "output": output,
        "sysroot": sysroot,
        "qemu": qemu,
        "cc": cc,
        "cflags": extra_cflags,
        "ldflags": extra_ldflags,
        "ny_args": ny_args,
        "program_args": program_args,
    }

def print_cross_help() -> None:
    print(c("1;36", "Nytrix cross targets"))
    print("")
    print("Usage:")
    print("  ./make cross TARGET file.ny")
    print("  ./make cross-run TARGET file.ny -- program args")
    print("  ./make cross --target aarch64-linux-gnu --sysroot /opt/sysroot file.ny")
    print("  ./make cross-run windows-x64 hello.ny")
    print("")
    print("Presets:")
    ready: list[tuple[str, str, str, str]] = []
    setup: list[tuple[str, str, str, str]] = []
    for name, (triple, runner, _) in sorted(CROSS_PRESETS.items()):
        ok_now, detail = _cross_status_detail(triple, runner)
        row = (name, triple, runner or "none", detail)
        if ok_now:
            ready.append(row)
        else:
            setup.append(row)
    if ready:
        print(c("32", "  ready now"))
        for name, triple, runner, detail in ready:
            print(f"    {name:<18} {triple:<24} runner={runner:<14} {detail}")
    if setup:
        print(c("33", "  setup needed"))
        for name, triple, runner, detail in setup:
            print(f"    {name:<18} {triple:<24} runner={runner:<14} {detail}")
    print("")
    print("Flags:")
    print("  --target T       target triple or preset")
    print("  --sysroot DIR    pass --sysroot to clang and -L DIR to qemu")
    print("  --cc PATH        set NYTRIX_CC for the native runtime compiler")
    print("  --qemu PATH      override qemu/wine runner for cross-run")
    print("  --host-cflags F  append target C flags")
    print("  --host-ldflags F append target linker flags")
    print("")
    print("Config/env:")
    print("  NYTRIX_MINGW_CC       override MinGW compiler for windows-x64")
    print("  NYTRIX_MINGW_SYSROOT  override MinGW sysroot")
    print("  NYTRIX_STD_OVERLAY    path-list of project std/lib module override roots")

def _rel_or_abs(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(ROOT))
    except Exception:
        return str(path)

def _tool_path(name: str) -> str:
    return shutil.which(name) or ""

def _tool_status(name: str) -> str:
    path = _tool_path(name)
    return path if path else c("33", "missing")

def _runner_names() -> list[str]:
    names: list[str] = []
    for _, runner, _ in CROSS_PRESETS.values():
        if runner and runner not in names:
            names.append(runner)
    return names

def _missing_runners() -> list[str]:
    return [name for name in _runner_names() if not _tool_path(name)]

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

def _linux_soft_runner_packages(distro: str, like: str, missing: list[str]) -> list[str]:
    need_qemu = any(name.startswith("qemu-") for name in missing)
    need_wine = "wine" in missing
    pkgs: list[str] = []
    if distro in ("debian", "ubuntu", "linuxmint", "pop", "raspbian") or "debian" in like:
        if need_qemu:
            pkgs.append("qemu-user")
        if need_wine:
            pkgs.append("wine64" if apt_has_pkg("wine64") else "wine")
        return pkgs
    if distro in ("arch", "manjaro") or "arch" in like:
        if need_qemu:
            pkgs.append("qemu-user")
        if need_wine:
            pkgs.append("wine")
        return pkgs
    if distro in ("fedora", "rhel", "centos", "rocky") or "fedora" in like or "rhel" in like:
        if need_qemu:
            pkgs.append("qemu-user")
        if need_wine:
            pkgs.append("wine")
        return pkgs
    return []

def _linux_mingw_packages(distro: str, like: str) -> list[str]:
    if distro in ("debian", "ubuntu", "linuxmint", "pop", "raspbian") or "debian" in like:
        return [
            "gcc-mingw-w64-x86-64",
            "binutils-mingw-w64-x86-64",
            "libz-mingw-w64-dev",
            "libgmp-mingw-w64-dev",
        ]
    if distro in ("arch", "manjaro") or "arch" in like:
        return [
            "mingw-w64-gcc",
            "mingw-w64-zlib",
            "mingw-w64-gmp",
        ]
    if distro in ("fedora", "rhel", "centos", "rocky") or "fedora" in like or "rhel" in like:
        return [
            "mingw64-gcc",
            "mingw64-binutils",
            "mingw64-zlib",
            "mingw64-gmp",
        ]
    return []

def _arch_aur_helper() -> str:
    return which("yay") or which("paru")

def _arch_install_packages(pkgs: list[str]) -> None:
    repo_pkgs: list[str] = []
    aur_pkgs: list[str] = []
    for pkg in pkgs:
        if pkg in ("mingw-w64-gmp", "mingw-w64-zlib"):
            aur_pkgs.append(pkg)
        else:
            repo_pkgs.append(pkg)
    if repo_pkgs:
        step("soft deps: pacman install")
        run(["sudo", "pacman", "-Sy", "--noconfirm", *_dedupe(repo_pkgs)])
    if aur_pkgs:
        helper = _arch_aur_helper()
        if helper:
            step(f"soft deps: {Path(helper).name} install")
            run([helper, "-S", "--noconfirm", *_dedupe(aur_pkgs)])
        else:
            log("DEPS", "AUR helper not found; install manually: " + " ".join(_dedupe(aur_pkgs)))

def install_soft_deps() -> None:
    missing = _missing_runners()
    mingw_missing = _mingw_runtime_missing("x86_64-w64-windows-gnu")
    if not missing and not mingw_missing:
        return
    os_name = host_os()
    if os_name == "linux":
        info = read_os_release()
        distro, like = info.get("ID", "").lower(), info.get("ID_LIKE", "").lower()
        pkgs = _linux_soft_runner_packages(distro, like, missing)
        if mingw_missing:
            pkgs.extend(_linux_mingw_packages(distro, like))
        pkgs = _dedupe(pkgs)
        if not pkgs:
            if missing:
                log("DEPS", "missing optional runners: " + ", ".join(missing))
            if mingw_missing:
                log("DEPS", "missing MinGW pieces: " + ", ".join(mingw_missing))
            log("DEPS", f"runner auto-install not configured for distro: {distro or os_name}")
            return
        if distro in ("debian", "ubuntu", "linuxmint", "pop", "raspbian") or "debian" in like:
            step("soft deps: apt update")
            run(["sudo", "apt", "update"])
            step("soft deps: apt install")
            run(["sudo", "apt", "install", "-y", *pkgs])
            return
        if distro in ("arch", "manjaro") or "arch" in like:
            _arch_install_packages(pkgs)
            return
        if distro in ("fedora", "rhel", "centos", "rocky") or "fedora" in like or "rhel" in like:
            step("soft deps: dnf install")
            run(["sudo", "dnf", "install", "-y", *pkgs])
            return
    if os_name == "macos":
        if not which("brew"):
            log("DEPS", "brew not found; install qemu/wine/mingw-w64 manually if needed")
            return
        pkgs: list[str] = []
        if any(name.startswith("qemu-") for name in missing):
            pkgs.append("qemu")
        if mingw_missing:
            pkgs.append("mingw-w64")
        if "wine" in missing:
            log("DEPS", "wine is not installed automatically on macOS; install a suitable wine package if needed")
        if pkgs:
            step("soft deps: brew install")
            run(["brew", "install", *_dedupe(pkgs)])
        return
    if missing:
        log("DEPS", "missing optional runners: " + ", ".join(missing))
    if mingw_missing:
        log("DEPS", "missing MinGW pieces: " + ", ".join(mingw_missing))
    log("DEPS", f"runner auto-install not implemented for host: {os_name}")

def _print_kv(key: str, value: str) -> None:
    print(f"  {c('36', key):<28} {value}")

def _built_tool_status(build_root: Path, kind: str, name: str) -> str:
    try:
        path = resolve_tool_bin(build_root, kind, name)
        return _rel_or_abs(path)
    except SystemExit:
        return c("33", "missing (run ./make all)")

def run_make_env(build_root: Path, kind: str, jobs: int, jobs_note: str) -> int:
    bdir = cmake_build_dir(build_root, kind)
    print(c("1;36", "Nytrix environment"))
    _print_kv("root", str(ROOT))
    _print_kv("host", f"{host_os()} {platform.machine() or 'unknown'}")
    _print_kv("build.kind", kind)
    _print_kv("build.dir", _rel_or_abs(bdir))
    _print_kv("build.jobs", f"{jobs} ({jobs_note})")
    _print_kv("cache.dir", _rel_or_abs(build_root / "cache"))
    _print_kv("ccache.dir", _rel_or_abs(build_root / "cache" / "ccache"))
    _print_kv("python.cache", os.environ.get("PYTHONPYCACHEPREFIX", ""))
    _print_kv("config.loaded", os.environ.get("NYTRIX_CONFIG_LOADED", "none"))
    print("")
    print(c("1", "Tools"))
    for name in ("ny", "ny-fmt", "ny-test", "ny-doc", "ny-perf"):
        _print_kv(name, _built_tool_status(build_root, kind, name))
    for name in ("cmake", "ninja", "clang", "llvm-config", "pkg-config", "pkgconf", "git", "gdb"):
        _print_kv(name, _tool_status(name))
    print("")
    print(c("1", "Overrides"))
    for name in (
        "BUILD_DIR",
        "NYTRIX_HOST_TRIPLE",
        "NYTRIX_HOST_CFLAGS",
        "NYTRIX_HOST_LDFLAGS",
        "NYTRIX_CC",
        "CC",
        "CXX",
        "LLVM_CONFIG",
        "PKG_CONFIG_PATH",
        "NYTRIX_STD_PATH",
        "NYTRIX_BUILD_STD_PATH",
        "NYTRIX_BUILD_JOBS",
        "NYTRIX_CONFIG",
        "NY_CONFIG",
        "NYTRIX_PKG_HOME",
        "NYTRIX_PKG_PATH",
        "NYTRIX_PKG_REGISTRY",
        "NYTRIX_MINGW_CC",
        "NYTRIX_MINGW_SYSROOT",
        "MINGW_SYSROOT",
        "NYTRIX_CROSS_SYSROOT",
        "NYTRIX_STD_OVERLAY",
    ):
        value = os.environ.get(name)
        if value:
            _print_kv(name, value)
    return 0

def run_make_targets() -> int:
    print(c("1;36", "Nytrix target presets"))
    print("")
    print(f"  {c('1', 'preset'):<22} {c('1', 'triple'):<28} {c('1', 'runner'):<34} {c('1', 'status')}")
    for name, (triple, runner, _) in sorted(CROSS_PRESETS.items()):
        runner_status = "none"
        if runner:
            runner_status = runner
            path = _tool_path(runner)
            if path:
                runner_status += f" ({path})"
            else:
                runner_status += " (missing)"
        ok_now, detail = _cross_status_detail(triple, runner)
        status = ("ok: " if ok_now else "setup: ") + detail
        print(f"  {name:<22} {triple:<28} {runner_status:<34} {status}")
    print("")
    print("Use: ./make cross <preset> file.ny")
    print("Run: ./make cross-run <preset> file.ny -- args")
    return 0

def _doctor_mark(ok_value: bool, required: bool) -> str:
    if ok_value:
        return c("32", "ok")
    if required:
        return c("31", "fail")
    return c("33", "warn")

def _doctor_check(label: str, ok_value: bool, detail: str = "", required: bool = True) -> int:
    print(f"  {_doctor_mark(ok_value, required):<12} {label:<22} {detail}")
    return 1 if required and not ok_value else 0

def print_doctor_help() -> None:
    print(c("1;36", "Nytrix doctor"))
    print("")
    print("Usage:")
    print("  ./make doctor")
    print("  ./make doctor --install")
    print("")
    print("Plain doctor is read-only. --install installs required deps, optional")
    print("std/native deps, qemu/wine runners, and MinGW cross pieces.")

def run_make_doctor(build_root: Path, kind: str, args: list[str]) -> int:
    install = False
    for a in args:
        if a in ("-h", "--help"):
            print_doctor_help()
            return 0
        if a in ("--install", "--fix"):
            install = True
            continue
        raise SystemExit(f"make doctor: unknown option {a}")
    if install:
        prev = os.environ.get("NYTRIX_INSTALL_STD_DEPS")
        os.environ["NYTRIX_INSTALL_STD_DEPS"] = "1"
        try:
            ensure_deps(force_optional_prompt=True, require_git=True)
            install_soft_deps()
        finally:
            _set_env_value("NYTRIX_INSTALL_STD_DEPS", prev)
    bdir = cmake_build_dir(build_root, kind)
    cache_dir = build_root / "cache"
    ccache_dir = cache_dir / "ccache"
    failures = 0
    print(c("1;36", "Nytrix doctor"))
    failures += _doctor_check("project root", ROOT.exists(), str(ROOT))
    failures += _doctor_check("build dir", ensure_dir_writable(build_root), _rel_or_abs(build_root))
    failures += _doctor_check("cache dir", ensure_dir_writable(cache_dir), _rel_or_abs(cache_dir))
    failures += _doctor_check("ccache dir", ensure_dir_writable(ccache_dir), _rel_or_abs(ccache_dir))
    _doctor_check("config", bool(os.environ.get("NYTRIX_CONFIG_LOADED", "")), os.environ.get("NYTRIX_CONFIG_LOADED", "none"), required=False)
    print("")
    print(c("1", "Required tools"))
    for name in ("cmake", "clang"):
        failures += _doctor_check(name, bool(_tool_path(name)), _tool_status(name))
    failures += _doctor_check("ninja", bool(_tool_path("ninja")), _tool_status("ninja"))
    failures += _doctor_check("pkg-config", bool(_tool_path("pkg-config") or _tool_path("pkgconf")), _tool_status("pkg-config") if _tool_path("pkg-config") else _tool_status("pkgconf"))
    failures += _doctor_check("llvm-config", bool(_tool_path("llvm-config")) or host_os() == "windows", _tool_status("llvm-config"), required=(host_os() != "windows"))
    failures += _doctor_check("gmp", _gmp_available(), "headers/library discoverable")
    print("")
    print(c("1", "Optional std/native deps"))
    optional_missing = _detect_optional_std_missing()
    if optional_missing:
        _doctor_check("stdlib extras", False, f"{len(optional_missing)} missing; run ./make doctor --install", required=False)
        for item in optional_missing[:12]:
            print(f"  {c('33', 'warn'):<12} {'':<22} {item}")
        if len(optional_missing) > 12:
            print(f"  {c('33', 'warn'):<12} {'':<22} ... {len(optional_missing) - 12} more")
    else:
        _doctor_check("stdlib extras", True, "all detected")
    print("")
    print(c("1", "Built tools"))
    for name in ("ny", "ny-fmt", "ny-test"):
        path = _built_tool_status(build_root, kind, name)
        failures += _doctor_check(name, "missing" not in path, path, required=False)
    std_path = bdir / "std.ny"
    failures += _doctor_check("std.ny", std_path.exists(), _rel_or_abs(std_path), required=False)
    print("")
    print(c("1", "Vendored libs"))
    vendor_lib = build_root / "vendor" / "lib" / "host"
    if vendor_lib.is_dir() and any(vendor_lib.glob("*.so*")):
        ldd = which("ldd")
        missing_deps: list[str] = []
        for so in sorted(vendor_lib.glob("*.so*")):
            if not so.is_file() or so.is_symlink():
                continue
            if not ldd:
                break
            res = subprocess.run([ldd, str(so)], capture_output=True, text=True, timeout=30)
            for line in res.stdout.splitlines():
                if "not found" in line and "=>" in line:
                    lib_name = line.split("=>")[0].strip()
                    missing_deps.append(f"{so.name}: {lib_name}")
        if ldd is None:
            _doctor_check("vendor libs", True, "ldd not available, skip check", required=False)
        elif missing_deps:
            _doctor_check("vendor libs", False, f"{len(missing_deps)} unresolved", required=True)
            for d in missing_deps[:8]:
                print(f"  {c('33', 'warn'):<12} {'':<22} {d}")
            if len(missing_deps) > 8:
                print(f"  {c('33', 'warn'):<12} {'':<22} ... {len(missing_deps) - 8} more")
        else:
            _doctor_check("vendor libs", True, "all deps satisfied", required=False)
    print("")
    print(c("1", "Runners"))
    for name in _runner_names():
        _doctor_check(name, bool(_tool_path(name)), _tool_status(name), required=False)
    if host_os() == "linux":
        display = os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY") or ""
        _doctor_check("display", bool(display), display or "no DISPLAY/WAYLAND_DISPLAY for UI apps", required=False)
    print("")
    print(c("1", "Cross toolchains"))
    seen_triples: set[str] = set()
    for _, (triple, runner, _) in sorted(CROSS_PRESETS.items()):
        if triple in seen_triples:
            continue
        seen_triples.add(triple)
        ok_now, detail = _cross_status_detail(triple, runner)
        _doctor_check(triple, ok_now, detail, required=False)
    if _mingw_runtime_missing("x86_64-w64-windows-gnu"):
        _doctor_check("mingw install", False, "run ./make doctor --install or set NYTRIX_MINGW_CC/NYTRIX_MINGW_SYSROOT", required=False)
    print("")
    if failures:
        print(c("31", f"{failures} required check(s) failed"))
        return 1
    ok("doctor checks passed")
    return 0

def run_cross(build_root: Path, kind: str, args: list[str], run_after: bool) -> int:
    cfg = _parse_cross_args(args)
    if bool(cfg.get("help", False)) or not str(cfg.get("target", "")):
        print_cross_help()
        return 0 if bool(cfg.get("help", False)) else 2
    ny_args = list(cfg.get("ny_args", []))
    src_i = _ny_source_index(ny_args)
    if src_i < 0:
        raise SystemExit("make cross: pass a .ny source file after the target")
    target_raw = str(cfg.get("target", ""))
    triple, runner_name, arm_abi = _cross_target(target_raw)
    source_arg = ny_args[src_i]
    source = (ROOT / source_arg).resolve() if not Path(source_arg).is_absolute() else Path(source_arg)
    out_raw = str(cfg.get("output", ""))
    out_path = Path(out_raw) if out_raw else _cross_default_output(build_root, triple, source)
    if not out_path.is_absolute():
        out_path = ROOT / out_path
    out_path.parent.mkdir(parents=True, exist_ok=True)
    sysroot = str(cfg.get("sysroot", "")).strip()
    cc = str(cfg.get("cc", "")).strip()
    cc_source = ""
    compile_issues: list[str] = []
    backend_issue = _cross_llvm_backend_issue(triple)
    if backend_issue:
        compile_issues.append(backend_issue)
    if _cross_is_windows(triple):
        cc, cc_source = _mingw_select_cc(triple, cc)
        if not cc:
            raise SystemExit(
                "make cross: windows-x64 needs MinGW-w64; run ./make doctor --install "
                "or set NYTRIX_MINGW_CC"
            )
        if not sysroot:
            sysroot = _mingw_sysroot_for_triple(triple)
        missing = _mingw_runtime_missing(triple)
        if missing and not _env_flag("NYTRIX_CROSS_ALLOW_MISSING_MINGW", False):
            raise SystemExit(
                "make cross: windows-x64 missing " + ", ".join(missing) +
                "; run ./make doctor --install or set NYTRIX_MINGW_SYSROOT"
            )
    elif _cross_is_linux(triple):
        if not sysroot:
            sysroot = _linux_cross_sysroot_for_triple(triple)
        if not cc:
            cc, cc_source = _linux_select_cc(triple, "")
        if not _cross_is_native_linux(triple):
            if not sysroot:
                compile_issues.append(f"sysroot for {triple}")
            else:
                if not _linux_cross_file_any(sysroot, triple, ("include/gmp.h",)):
                    compile_issues.append("target gmp headers")
                if not _linux_cross_file_any(sysroot, triple, ("lib/libgmp.so", "lib/libgmp.a")):
                    compile_issues.append("target gmp library")
                if not _linux_cross_file_any(sysroot, triple, ("include/zlib.h",)):
                    compile_issues.append("target zlib headers")
                if not _linux_cross_file_any(sysroot, triple, ("lib/libz.so", "lib/libz.a")):
                    compile_issues.append("target zlib library")
            if not cc and not _tool_path("clang"):
                compile_issues.append(f"clang or {triple}-gcc")
    if compile_issues and not _env_flag("NYTRIX_CROSS_ALLOW_MISSING", False):
        raise SystemExit(
            f"make cross: {target_raw} unavailable: " + ", ".join(compile_issues) +
            "; run ./make targets or ./make doctor"
        )
    if run_after:
        runner = str(cfg.get("qemu", "")).strip() or runner_name
        runner_issue = _cross_runner_issue(runner)
        if runner_issue and not _env_flag("NYTRIX_CROSS_ALLOW_MISSING", False):
            raise SystemExit(f"make cross-run: {target_raw} unavailable: {runner_issue}")
    needs_target = _cross_compiler_needs_target_flag(cc, triple)
    target_flag = f"--target={triple}"
    target_cflags = target_flag if needs_target else ""
    target_ldflags = target_flag if needs_target else ""
    if sysroot and (needs_target or bool(str(cfg.get("sysroot", "")).strip())):
        target_cflags = _merge_flag_words(target_cflags, f"--sysroot={sysroot}")
        target_ldflags = _merge_flag_words(target_ldflags, f"--sysroot={sysroot}")
    target_cflags = _merge_flag_words(target_cflags, str(cfg.get("cflags", "")))
    target_ldflags = _merge_flag_words(target_ldflags, str(cfg.get("ldflags", "")))
    binp = resolve_tool_bin(build_root, kind, "ny")
    launch = tool_launch_path(binp)
    compile_front = ny_args[:src_i]
    compile_args = [
        launch,
        "--profile=compile",
        "--host-triple", triple,
        "--host-cflags", target_cflags,
        "--host-ldflags", target_ldflags,
    ]
    if arm_abi:
        compile_args.extend(["--arm-float-abi", arm_abi])
    compile_args.extend([*compile_front, "-o", str(out_path), source_arg])
    env = os.environ.copy()
    ccache_dir = build_root / "cache" / "ccache"
    ccache_dir.mkdir(parents=True, exist_ok=True)
    env.setdefault("CCACHE_DIR", str(ccache_dir))
    if cc:
        env["NYTRIX_CC"] = cc
    if cc_source:
        log("CROSS", f"{target_raw} -> {triple}; cc={cc} ({cc_source})")
    else:
        log("CROSS", f"{target_raw} -> {triple}")
    rc = subprocess.Popen(compile_args, cwd=str(ROOT), env=env).wait()
    if rc != 0:
        return rc
    chmod_executable(out_path)
    ok(f"cross binary: {out_path.relative_to(ROOT) if out_path.is_relative_to(ROOT) else out_path}")
    if not run_after:
        return 0
    runner = str(cfg.get("qemu", "")).strip() or runner_name
    if not runner:
        log("CROSS", f"no runner preset for {triple}; compile completed")
        return 0
    runner_path = shutil.which(runner) or (runner if Path(runner).exists() else "")
    if not runner_path:
        log("CROSS", f"runner '{runner}' not found; install it to execute {triple} binaries")
        return 0
    run_cmd = [runner_path]
    if sysroot and Path(runner_path).name.startswith("qemu-"):
        run_cmd.extend(["-L", sysroot])
    run_cmd.extend([str(out_path), *list(cfg.get("program_args", []))])
    log("CROSS", "run " + " ".join(shlex.quote(x) for x in run_cmd))
    return subprocess.Popen(run_cmd, cwd=str(ROOT), env=env).wait()

PROFILE_TIME_RE = re.compile(r"^\s*([A-Za-z][A-Za-z ]+):\s+([0-9]+(?:\.[0-9]+)?)s\s*$")

def print_profile_help() -> None:
    print(c("1;36", "Nytrix profile tooling"))
    print("")
    print("Usage:")
    print("  ./make profile time [--runs N] -- <ny args>")
    print("  ./make profile compile [--runs N] file.ny")
    print("  ./make profile perf [--out perf.data] -- <ny args>")
    print("  ./make profile report [perf.data]")
    print("  ./make profile gdb -- <ny args>")
    print("  ./make profile asan|ubsan [ny-test args]")
    print("  ./make profile fuzz [ny-test args]")
    print("  ./make profile afl -- <afl-fuzz args>")
    print("")
    print("Examples:")
    print("  ./make profile compile --runs 5 etc/projects/ui/editor.ny")
    print("  ./make profile perf -- --profile=compile -emit-only etc/projects/ui/editor.ny")
    print("  ./make profile gdb -- -time -run etc/tests/rt/comptime.ny")

def _strip_dashdash(args: list[str]) -> list[str]:
    return args[1:] if args and args[0] == "--" else args

def _profile_has_time(args: list[str]) -> bool:
    return any(a == "-time" or a == "--time" for a in args)

def _profile_ny_env(build_root: Path, kind: str) -> dict[str, str]:
    env = os.environ.copy()
    env.setdefault("NYTRIX_ROOT", str(ROOT))
    env.setdefault("NYTRIX_STD_CACHE", "1")
    env.setdefault("NYTRIX_STD_BC_CACHE_AUTO", "1")
    env.setdefault("NYTRIX_JIT_CACHE", "1")
    if _env_flag("NYTRIX_MAKE_USE_PREBUILT_STD", False):
        std_file = cmake_build_dir(build_root, kind) / "std.ny"
        if std_file.exists():
            std_path = str(std_file)
            env.setdefault("NYTRIX_STD_PATH", std_path)
            env.setdefault("NYTRIX_BUILD_STD_PATH", std_path)
            env.setdefault("NYTRIX_STD_PREBUILT", std_path)
    return env

def _profile_parse_time_args(args: list[str]) -> tuple[int, bool, list[str]]:
    runs = 3
    show_output = False
    out: list[str] = []
    i = 0
    while i < len(args):
        a = args[i]
        if a in ("--runs", "-n"):
            i += 1
            if i >= len(args):
                raise SystemExit("make profile time: missing value for --runs")
            runs = max(1, int(args[i]))
        elif a.startswith("--runs="):
            runs = max(1, int(a.split("=", 1)[1]))
        elif a == "--show-output":
            show_output = True
        elif a == "--":
            out.extend(args[i + 1:])
            break
        else:
            out.append(a)
        i += 1
    return runs, show_output, out

def _profile_parse_metric(output: str) -> dict[str, float]:
    metrics: dict[str, float] = {}
    for line in output.splitlines():
        m = PROFILE_TIME_RE.match(line)
        if not m:
            continue
        key = re.sub(r"\s+", "_", m.group(1).strip().lower())
        try:
            metrics[key] = float(m.group(2))
        except ValueError:
            pass
    return metrics

def _profile_time_summary(values: list[float]) -> str:
    if not values:
        return "n/a"
    mean = sum(values) / len(values)
    return f"min={min(values):.4f}s mean={mean:.4f}s max={max(values):.4f}s"

def run_profile_time(build_root: Path, kind: str, args: list[str]) -> int:
    runs, show_output, ny_args = _profile_parse_time_args(args)
    ny_args = _strip_dashdash(ny_args)
    if not ny_args:
        print_profile_help()
        return 2
    if not _profile_has_time(ny_args):
        ny_args = ["-time", *ny_args]
    ny_bin = resolve_tool_bin(build_root, kind, "ny")
    env = _profile_ny_env(build_root, kind)
    totals: list[float] = []
    codegen: list[float] = []
    for idx in range(1, runs + 1):
        started = time.perf_counter()
        res = subprocess.run([str(ny_bin), *ny_args], cwd=str(ROOT), env=env,
                             text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        elapsed = time.perf_counter() - started
        if show_output or res.returncode != 0:
            sys.stdout.write(res.stdout)
        metrics = _profile_parse_metric(res.stdout)
        total = metrics.get("total_time", elapsed)
        cg_time = metrics.get("codegen")
        totals.append(total)
        if cg_time is not None:
            codegen.append(cg_time)
        cg_txt = f" codegen={cg_time:.4f}s" if cg_time is not None else ""
        print(f"profile time run {idx}/{runs}: total={total:.4f}s{cg_txt}")
        if res.returncode != 0:
            return res.returncode
    print("profile time summary:")
    print(f"  total   {_profile_time_summary(totals)}")
    if codegen:
        print(f"  codegen {_profile_time_summary(codegen)}")
    return 0

def run_profile_compile(build_root: Path, kind: str, args: list[str]) -> int:
    if not args or args[0] in ("-h", "--help"):
        print_profile_help()
        return 0
    return run_profile_time(build_root, kind, ["--runs", "3", "--profile=compile", "-emit-only", *args])

def _profile_parse_perf_args(args: list[str]) -> tuple[Path, str, float, list[str]]:
    out = _nytrix_cache_root("profiles") / f"ny-perf-{int(time.time())}.data"
    callgraph = "dwarf"
    limit = 1.0
    ny_args: list[str] = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--out":
            i += 1
            if i >= len(args):
                raise SystemExit("make profile perf: missing value for --out")
            out = Path(args[i]).expanduser()
        elif a.startswith("--out="):
            out = Path(a.split("=", 1)[1]).expanduser()
        elif a == "--callgraph":
            i += 1
            if i >= len(args):
                raise SystemExit("make profile perf: missing value for --callgraph")
            callgraph = args[i]
        elif a.startswith("--callgraph="):
            callgraph = a.split("=", 1)[1]
        elif a == "--limit":
            i += 1
            if i >= len(args):
                raise SystemExit("make profile perf: missing value for --limit")
            limit = float(args[i])
        elif a.startswith("--limit="):
            limit = float(a.split("=", 1)[1])
        elif a == "--":
            ny_args.extend(args[i + 1:])
            break
        else:
            ny_args.append(a)
        i += 1
    return out, callgraph, limit, _strip_dashdash(ny_args)

def run_profile_report(args: list[str]) -> int:
    limit = "1"
    perf_file = ""
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--limit":
            i += 1
            if i >= len(args):
                raise SystemExit("make profile report: missing value for --limit")
            limit = args[i]
        elif a.startswith("--limit="):
            limit = a.split("=", 1)[1]
        elif not perf_file:
            perf_file = a
        i += 1
    if not perf_file:
        cands = sorted(_nytrix_cache_root("profiles").glob("*.data"),
                       key=lambda p: p.stat().st_mtime if p.exists() else 0,
                       reverse=True)
        if cands:
            perf_file = str(cands[0])
    if not perf_file:
        raise SystemExit("make profile report: pass a perf.data file or run profile perf first")
    return subprocess.run([
        "perf", "report", "--stdio", "-i", perf_file, "--sort", "symbol",
        "--no-children", "--percent-limit", limit,
    ], cwd=str(ROOT)).returncode

def run_profile_perf(build_root: Path, kind: str, args: list[str]) -> int:
    if not shutil.which("perf"):
        raise SystemExit("make profile perf: perf not found")
    out, callgraph, limit, ny_args = _profile_parse_perf_args(args)
    if not ny_args:
        print_profile_help()
        return 2
    if not _profile_has_time(ny_args):
        ny_args = ["-time", *ny_args]
    out.parent.mkdir(parents=True, exist_ok=True)
    ny_bin = resolve_tool_bin(build_root, kind, "ny")
    env = _profile_ny_env(build_root, kind)
    cmd = ["perf", "record", "-g", "--call-graph", callgraph, "-o", str(out), "--",
           str(ny_bin), *ny_args]
    log("PROFILE", " ".join(shlex.quote(x) for x in cmd))
    rc = subprocess.run(cmd, cwd=str(ROOT), env=env).returncode
    if rc != 0:
        return rc
    ok(f"perf data: {out}")
    return run_profile_report(["--limit", str(limit), str(out)])

def run_profile_gdb(build_root: Path, kind: str, args: list[str]) -> int:
    if not shutil.which("gdb"):
        raise SystemExit("make profile gdb: gdb not found")
    ny_args = _strip_dashdash(args)
    if not ny_args:
        print_profile_help()
        return 2
    ny_bin = resolve_tool_bin(build_root, kind, "ny")
    return subprocess.run(["gdb", "--args", str(ny_bin), *ny_args], cwd=str(ROOT)).returncode

def run_make_profile(build_root: Path, kind: str, jobs: int, args: list[str]) -> int:
    if not args or args[0] in ("-h", "--help"):
        print_profile_help()
        return 0
    mode = args[0]
    rest = args[1:]
    if mode in ("time", "bench"):
        return run_profile_time(build_root, kind, rest)
    if mode in ("compile", "comptime"):
        return run_profile_compile(build_root, kind, rest)
    if mode in ("perf", "callgraph"):
        return run_profile_perf(build_root, kind, rest)
    if mode == "report":
        return run_profile_report(rest)
    if mode == "gdb":
        return run_profile_gdb(build_root, kind, rest)
    if mode == "gprof":
        cmake_build(build_root, kind, ["ny", "ny-perf"], jobs)
        return run_tool(build_root, kind, "ny-perf", ["profile", *rest])
    if mode in ("asan", "ubsan"):
        base_host_cflags = os.environ.get("NYTRIX_HOST_CFLAGS")
        base_host_ldflags = os.environ.get("NYTRIX_HOST_LDFLAGS")
        base_skip_optional_gates = os.environ.get("NYTRIX_SKIP_OPTIONAL_GATES")
        base_test_cache = os.environ.get("NYTRIX_TEST_CACHE")
        base_test_cold = os.environ.get("NYTRIX_TEST_COLD")
        san_kind = configure_command_environment(
            mode, kind, base_host_cflags, base_host_ldflags,
            base_skip_optional_gates, base_test_cache, base_test_cold,
        )
        cmake_build(build_root, san_kind, ["ny", "ny-test"], jobs)
        return run_test(build_root, san_kind, jobs, rest)
    if mode == "fuzz":
        cmake_build(build_root, kind, ["ny", "ny-test", "ny-fuzz"], jobs)
        if rest:
            return run_tool(build_root, kind, "ny-fuzz", rest)
        return run_tool(build_root, kind, "ny-fuzz", ["validate-shapes", "etc/tests/fuzz"])
    if mode == "afl":
        afl = shutil.which("afl-fuzz")
        if not afl:
            raise SystemExit("make profile afl: afl-fuzz not found")
        afl_args = _strip_dashdash(rest)
        if not afl_args:
            raise SystemExit("make profile afl: pass afl-fuzz args after --")
        return subprocess.run([afl, *afl_args], cwd=str(ROOT)).returncode
    raise SystemExit(f"make profile: unknown mode '{mode}'")

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
    log("TEST", f"make test: full matrix with jit/repl/native and benchmarks; result_cache {cache_mode}, exec_cache {exec_cache_mode}, std_cache {std_cache_mode} (set NYTRIX_TEST_EXEC_CACHE=1 to enable binary caches)")
    test_timeout_s = int(os.environ.get("NYTRIX_TEST_TIMEOUT") or "1800")  # 30 min default
    step(f"run tests: bin=ny jobs={test_jobs} timeout={test_timeout_s}s")
    rc = run_tool(build_root, kind, "ny-test", ["--bin", str(ny_bin), "--jobs", str(test_jobs), *extra], timeout=float(test_timeout_s))
    elapsed_ms = int((time.perf_counter() - started) * 1000.0)
    if rc == 0:
        ok(f"test suite completed in {elapsed_ms}ms")
    else:
        log("TEST", f"test suite failed after {elapsed_ms}ms")
    return rc

def parse(argv: list[str]) -> tuple[list[str], list[str], int, bool, bool, bool, bool, str | None, bool | None]:
    known = {"all", "bin", "bin-static", "tar", "vendor", "fmt", "std", "std_bc", "test", "repl", "fuzz", "docs", "web-demos", "wasm", "c2ny", "install", "uninstall", "clean", "debug", "tidy", "perf", "profile", "gprof", "asan", "ubsan", "optcheck", "analyze", "check", "fb", "ny", "run", "release", "static", "deps", "cross", "cross-run", "env", "targets", "doctor"}

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
    version = False
    color_mode: str | None = None
    bootstrap_logs: bool | None = None
    i = 0
    had_unknown_nonflag = False
    while i < len(argv):
        a = argv[i]
        if a in ("-h", "--help"):
            if source_passthrough:
                extra.append(a)
            else:
                help_flag = True
        elif a == "help":
            help_flag = True
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
        elif a in ("static", "vendor", "cross", "cross-run", "doctor", "profile", "web-demos", "wasm"):
            cmds.append(a)
            extra.extend(argv[i + 1 :])
            break
        elif a == "ny":
            cmds.append("ny")
            extra.extend(argv[i + 1 :])
            break
        elif a in known:
            cmds.append("ny" if a == "run" else a)
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
    return cmds, extra, jobs, verbose, help_flag, version, kind_debug, color_mode, bootstrap_logs

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
            ("fmt/check/tidy", "format, parse-check, or tidy source"),
            ("test/fuzz", "run tests and smoke fuzzing"),
            ("asan/ubsan", "run tests under sanitizer builds"),
            ("profile", "time, perf, gdb, sanitizer, and fuzz wrappers"),
            ("perf/gprof", "run performance tooling"),
        )),
        ("Run", (
            ("ny/repl/run", "launch the compiler, REPL, or cached -run flow"),
            ("docs", "build documentation portal"),
            ("wasm", "compile a Ny source file to WebAssembly"),
            ("web-demos", "build the browser WebGL/Wasm demo portal"),
        )),
        ("Inspect", (
            ("env", "print effective paths, tools, and overrides"),
            ("targets", "list cross presets and runner status"),
            ("doctor", "diagnose setup; use --install to install known deps"),
        )),
        ("Cross", (
            ("cross", "compile for a target preset or triple"),
            ("cross-run", "compile, then run through qemu/wine when present"),
        )),
    )
    for title, rows in groups:
        print(c("1", title + ":"))
        for cmd, desc in rows:
            print(f"  {c('36', cmd)}{' ' * max(1, 20 - len(cmd))}{desc}")
    print("")
    print(c("1", "Options:"))
    for opt, desc in (
        ("-j, --jobs N", "parallel build jobs"),
        ("-v, --verbose", "print subcommands"),
        ("-h, --help", "show this help"),
        ("--version", "print version"),
        ("--color {auto,always,never}", "control colored output"),
        ("--no-color", "disable colored output"),
        ("--bootstrap-logs", "show bootstrap status"),
        ("--no-bootstrap-logs", "hide bootstrap status"),
    ):
        print(f"  {c('32', opt)}{' ' * max(1, 34 - len(opt))}{desc}")
    print("")
    print(c("1", "Ny/runtime passthrough:"))
    print(f"  {c('36', './make ny <file.ny> [ny flags] [program args]')}  run a Ny source")
    print(f"  {c('36', './make <file.ny> [ny flags] [program args]')}     shorthand for ./make ny")
    print(f"  {c('36', './make -trace ny <file.ny> ...')}              pass compiler flags before the source")
    print(f"  {c('36', './make ny <file.ny> -v -gl')}                pass UI/app flags after the source")
    print(f"  {c('36', './make ny <file.ny> -- [args]')}             force the rest to be program args")
    print("")
    print(c("1", "Examples:"))
    print("  ./make doctor")
    print("  ./make doctor --install")
    print("  ./make targets")
    print("  ./make ny etc/projects/ui/term.ny -h")
    print("  ./make etc/projects/ui/term.ny -v -vk btop")
    print("  ./make -trace ny etc/projects/ui/engine.ny -vk")
    print("  ./make wasm etc/projects/os/args.ny --out build/wasm/args.wasm")
    print("  ./make bin-static")
    print("  ./make static libs build/static")
    print("  ./make web-demos")
    print("  ./make cross-run linux-x64 etc/projects/os/args.ny -- one two")

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
    # Check GMP
    for p in ("/usr/lib/libgmp.a", "/usr/local/lib/libgmp.a"):
        if Path(p).exists():
            break
    else:
        for path in Path("/usr/lib").glob("libgmp*.a"):
            break
        else:
            missing.append("libgmp.a (libgmp-dev not providing static lib)")
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

    # ---- Detect bundled LLVM version from .so files ----
    bundled_llvm_major = _detect_bundled_llvm_major(lib_dir)
    if bundled_llvm_major:
        log("VENDOR", f"detected bundled LLVM {bundled_llvm_major} from libLLVM.so.*")
    else:
        log("VENDOR", "no bundled LLVM .so found; skipping LLVM/Clang headers")

    vendored_bin = vendor_dir / "bin"
    vendored_include = vendor_dir / "include"

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
    for name in ("src", "lib", "etc"):
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
    cmds, extra, requested_jobs, verbose, want_help, want_version, debug_kind, cli_color_mode, cli_bootstrap_logs = parse(sys.argv[1:])
    COLOR = apply_cli_color_mode(cli_color_mode)
    ensure_project_scripts_executable()
    if want_help:
        print_help()
        return 0
    if want_version:
        print("Nytrix Build Tool (python bootstrap)")
        return 0

    kind = "debug" if debug_kind else "release"
    build_root, notice = resolve_build_dir()
    first_repl_bootstrap = bootstrap_needed_for_repl(build_root, kind, cmds)
    inspect_cmds = {"env", "targets", "doctor"}
    tool_style_cmds = {"fmt", "analyze", "check", "tidy", "test", "perf", "profile", "docs", "web-demos", "wasm", "ny", "repl", "gprof", "asan", "ubsan", "fuzz", "cross", "cross-run", "static", "bin-static", "tar", "vendor", *inspect_cmds}
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
        log("BUILD", "auto-activated vendored LLVM (python PATH/LLVM_CONFIG/NYTRIX_LLVM_INCLUDE)")

    deps_free_cmds = {*inspect_cmds, "static", "bin-static", "tar", "vendor"}
    if not all(c in deps_free_cmds for c in cmds):
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
            rc = run_make_env(build_root, active_kind, jobs, jobs_note)
            if rc != 0:
                return rc
            continue
        if cmd == "targets":
            rc = run_make_targets()
            if rc != 0:
                return rc
            continue
        if cmd == "doctor":
            rc = run_make_doctor(build_root, active_kind, extra)
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
        elif cmd in ("fmt", "analyze", "check", "tidy"):
            targets = ["ny-fmt"]
        elif cmd in ("test", "asan", "ubsan", "fuzz"):
            targets = ["ny", "ny-test"]
        elif cmd in ("cross", "cross-run"):
            targets = ["ny"]
        elif cmd == "profile":
            targets = ["ny"]
        elif cmd == "docs":
            targets = ["ny", "std", "ny-doc"]
        elif cmd in ("web-demos", "wasm"):
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
            if cmd in ("ny", "repl") and not cmake_build_has_work(build_root, active_kind, targets):
                clean_bad_tool_build(build_root, active_kind, "ny")
            if cmd in ("ny", "repl") and not cmake_build_has_work(build_root, active_kind, targets):
                pass
            else:
                repl_build_visible = cmd in ("ny", "repl")
                old_quiet = QUIET_BOOTSTRAP
                if repl_build_visible:
                    QUIET_BOOTSTRAP = False
                    boot_notice("ny binary missing: compiling before launch")
                try:
                    cmake_build(build_root, active_kind, targets, jobs)
                    if repl_build_visible:
                        boot_notice("ny compiled; launching")
                finally:
                    if repl_build_visible:
                        QUIET_BOOTSTRAP = old_quiet

        if cmd in ("all", "bin", "std", "std_bc"):
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
            rc = run_tool(build_root, active_kind, "ny-fmt", ["--tidy", *extra])
        elif cmd == "perf":
            rc = run_tool(build_root, active_kind, "ny-perf", extra)
        elif cmd == "docs":
            std_file = str(cmake_build_dir(build_root, active_kind) / "std.ny")
            out_dir = str(build_root / "docs")
            rc = run_tool(build_root, active_kind, "ny-doc", [std_file, "-o", out_dir, *extra])
        elif cmd == "web-demos":
            rc = run_web_demos(build_root, active_kind, extra)
        elif cmd == "c2ny":
            if not extra:
                nyt_err("c2ny", "usage: ./make c2ny <file.c> [-o <out.ny>]")
                raise SystemExit(1)
            rc = run_tool(build_root, active_kind, "ny-fmt", ["--c2ny", *extra])
        elif cmd == "wasm":
            rc = run_wasm(build_root, active_kind, extra)
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
                cached_rc = run_ny_cached(build_root, active_kind, extra)
                rc = cached_rc if cached_rc is not None else run_tool(build_root, active_kind, "ny", ny_fast_run_args(extra))
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
            rc = run_cross(build_root, active_kind, extra, False)
        elif cmd == "cross-run":
            rc = run_cross(build_root, active_kind, extra, True)
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
                rc = run_tool(build_root, active_kind, "ny-fuzz", ["validate-shapes", "etc/tests/fuzz"])
        elif cmd in ("optcheck", "fb"):
            raise SystemExit(f"make: command '{cmd}' is not yet ported to native C path")
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
