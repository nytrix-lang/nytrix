#!/usr/bin/env -S python3 -B
from __future__ import annotations
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
os.environ.setdefault("PYTHONPYCACHEPREFIX", str(Path(tempfile.gettempdir()) / "nytrix-pycache"))
sys.pycache_prefix = os.environ["PYTHONPYCACHEPREFIX"]

ROOT = Path(__file__).resolve().parent
QUIET_BOOTSTRAP = False
LOADED_CONFIGS: list[Path] = []

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

def which(name: str) -> str:
    return shutil.which(name) or ""

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

def cmake_configure(build_root: Path, kind: str) -> Path:
    configure_macos_llvm_env()
    bdir = cmake_build_dir(build_root, kind)
    bdir.mkdir(parents=True, exist_ok=True)
    cache = bdir / "CMakeCache.txt"
    host_cflags = cmake_flag_list(os.environ.get("NYTRIX_HOST_CFLAGS") or "")
    host_ldflags = cmake_flag_list(os.environ.get("NYTRIX_HOST_LDFLAGS") or "")
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
    run(cmd, quiet=QUIET_BOOTSTRAP)
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

NY_SUBCOMMANDS = {"fmt", "test", "doc", "web", "perf", "make", "pkg", "get", "install", "new", "ny-lsp"}

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
        cmake_build(build_root, kind, ["ny", "ny-test"], jobs)
        return run_tool(build_root, kind, "ny-test", ["--smoke", *rest])
    if mode == "afl":
        afl = shutil.which("afl-fuzz")
        if not afl:
            raise SystemExit("make profile afl: afl-fuzz not found")
        afl_args = _strip_dashdash(rest)
        if not afl_args:
            raise SystemExit("make profile afl: pass afl-fuzz args after --")
        return subprocess.run([afl, *afl_args], cwd=str(ROOT)).returncode
    raise SystemExit(f"make profile: unknown mode '{mode}'")

def run_tool(build_root: Path, kind: str, name: str, args: list[str]) -> int:
    binp = resolve_tool_bin(build_root, kind, name)
    launch = tool_launch_path(binp)
    env = os.environ.copy()
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
    proc = subprocess.Popen([launch, *args], cwd=str(ROOT), env=env)
    interrupted = False
    rc = 0
    try:
        rc = proc.wait()
        return rc
    except KeyboardInterrupt:
        interrupted = True
        try:
            if proc.poll() is None:
                proc.send_signal(signal.SIGINT)
        except Exception:
            pass
        try:
            rc = proc.wait(timeout=2.0)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
            rc = 130
        return rc if isinstance(rc, int) and rc != 0 else 130
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
    step(f"run tests: bin=ny jobs={test_jobs} timeout=auto")
    rc = run_tool(build_root, kind, "ny-test", ["--bin", str(ny_bin), "--jobs", str(test_jobs), *extra])
    elapsed_ms = int((time.perf_counter() - started) * 1000.0)
    if rc == 0:
        ok(f"test suite completed in {elapsed_ms}ms")
    else:
        log("TEST", f"test suite failed after {elapsed_ms}ms")
    return rc

def parse(argv: list[str]) -> tuple[list[str], list[str], int, bool, bool, bool, bool, str | None, bool | None]:
    known = {"all", "bin", "fmt", "std", "std_bc", "test", "repl", "fuzz", "docs", "install", "uninstall", "clean", "debug", "tidy", "perf", "profile", "gprof", "asan", "ubsan", "optcheck", "analyze", "check", "fb", "ny", "run", "release", "deps", "cross", "cross-run", "env", "targets", "doctor"}
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
            help_flag = True
        elif a == "help":
            help_flag = True
        elif a == "--version":
            version = True
        elif a in ("-v", "--verbose"):
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
        elif a in ("cross", "cross-run", "doctor", "profile"):
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
        if extra and not had_unknown_nonflag:
            cmds = ["ny"]
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
    print(c("1", "Examples:"))
    print("  ./make doctor")
    print("  ./make doctor --install")
    print("  ./make targets")
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
    tool_style_cmds = {"fmt", "analyze", "check", "tidy", "test", "perf", "profile", "docs", "ny", "repl", "gprof", "asan", "ubsan", "fuzz", "cross", "cross-run", *inspect_cmds}
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
    if not all(c in inspect_cmds for c in cmds):
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
            targets = ["ny", "std", "ny-fmt", "ny-perf", "ny-test", "ny-doc", "ny-make"]
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
        if cmd not in ("uninstall",):
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
            rc = run_tool(build_root, active_kind, "ny-test", ["--smoke"])
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
