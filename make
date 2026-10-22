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
import textwrap
import time
import unicodedata
from dataclasses import dataclass, field
from pathlib import Path

if os.name == "nt":
    import msvcrt
else:
    import fcntl

sys.dont_write_bytecode = True
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
os.environ["MESA_VK_IGNORE_CONFORMANCE_WARNING"] = "true"

ROOT = Path(__file__).resolve().parent
os.environ.setdefault("PYTHONPYCACHEPREFIX", str(ROOT / "build" / "cache" / "python"))
sys.pycache_prefix = os.environ["PYTHONPYCACHEPREFIX"]

QUIET_BOOTSTRAP = False
LOADED_CONFIGS: list[Path] = []
ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")

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
    # Windows dependency pass below chooses a coherent LLVM toolchain
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
    if glyph_style_from_env() == "ascii":
        return False
    enc = sys.stdout.encoding or "utf-8"
    try:
        glyph.encode(enc)
        return True
    except Exception:
        return False

def glyph_style_from_env() -> str:
    """Select presentation glyphs independently of color and TTY detection."""
    raw = (os.environ.get("NYTRIX_GLYPHS") or
           os.environ.get("NYTRIX_UI_SYMBOLS") or "").strip().lower()
    if not raw and (os.environ.get("MAKE_ASCII") or "").strip().lower() in {
        "1", "true", "yes", "on"
    }:
        raw = "ascii"
    return "ascii" if raw in {"ascii", "plain", "fallback"} else "unicode"

def parse_glyph_mode(raw: str) -> str:
    value = (raw or "").strip().lower()
    if value in {"unicode", "utf8", "utf-8", "rich"}:
        return "unicode"
    if value in {"ascii", "plain", "fallback"}:
        return "ascii"
    raise SystemExit(f"make: invalid glyph mode '{raw}' (expected unicode|ascii)")

def apply_cli_glyph_mode(mode: str | None) -> str:
    if mode is not None:
        os.environ["NYTRIX_GLYPHS"] = parse_glyph_mode(mode)
    return glyph_style_from_env()

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


# Nerd-font glyph registry (preferred, ASCII fallback).  Rendered via
# Renderer.icon so ascii glyph mode and non-UTF-8 terminals stay clean.
_ICONS = {
    "git": ("\u2387", "git"),        # 󰘬 branch
    "clock": ("\u25f7", "o"),        #  clock
    "todo": ("\u2611", "[ ]"),            # ✔ checklist
    "palette": ("\u25d0", "#"),       # 󰏘 palette
    "file": ("\u25a5", "f"),          # 󰈙 file
    "build": ("\u2699", "=="),        # 󰯗 hammer
    "check": ("\u2714", "v"),         # 󰄬 check
    "flask": ("\u26a0", "!"),             # ⚠ quick pass
    "play": ("\u25b6", ">"),              # ▶ run
    "search": ("\u25c9", "o"),            # ◉ inspect
    "globe": ("\u25ce", "*"),             # ◎ cross
    "wrench": ("\u2692", "-"),            # ⚒ options
    "book": ("\u25a4", "b"),              # ▤ docs
}

def _icon_for_section(title: str) -> str | None:
    lowered = title.strip().lower()
    if "git" in lowered or "repository" in lowered:
        return "git"
    if "recent commits" in lowered or "history" in lowered:
        return "clock"
    if "backlog" in lowered or "todo" in lowered:
        return "todo"
    if "source lines" in lowered or "composition" in lowered:
        return "palette"
    if "largest" in lowered:
        return "file"
    if lowered.startswith("build"):
        return "build"
    if lowered.startswith("check") or lowered.startswith("test"):
        return "check"
    if lowered.startswith("run") or lowered.startswith("runtime") or \
            "passthrough" in lowered:
        return "play"
    if lowered.startswith("inspect") or "environment" in lowered:
        return "search"
    if "toolchain" in lowered or "override" in lowered or "configure" in lowered:
        return "wrench"
    if lowered.startswith("cross"):
        return "globe"
    if "options" in lowered or "configure" in lowered:
        return "wrench"
    if "help" in lowered or "docs" in lowered or "example" in lowered:
        return "book"
    if lowered.startswith("error") or lowered.startswith("fuzz") or \
            "fmt" in lowered or "lint" in lowered or "audit" in lowered:
        return "flask"
    if "timing" in lowered:
        return "clock"
    return None

@dataclass(frozen=True)
class Theme:
    brand: str = "1;35"
    accent: str = "1;36"
    success: str = "1;32"
    warning: str = "1;33"
    error: str = "1;31"
    muted: str = "2"
    subtle: str = "2"
    rule: str = "2;35"
    accent_soft: str = "36"
    error_soft: str = "31"

@dataclass
class Renderer:
    """Small, shared terminal renderer for every ./make message."""
    color: bool = True
    verbose: bool = False
    theme: Theme = field(default_factory=Theme)
    glyph_style: str = field(default_factory=lambda: glyph_style_from_env())

    def supports_glyph(self, glyph: str) -> bool:
        if self.glyph_style == "ascii":
            return False
        try:
            glyph.encode(sys.stdout.encoding or "utf-8")
            return True
        except Exception:
            return False

    def glyph(self, preferred: str, fallback: str) -> str:
        return preferred if self.supports_glyph(preferred) else fallback

    def paint(self, code: str, text: str) -> str:
        return f"\033[{code}m{text}\033[0m" if self.color else text

    @staticmethod
    def strip_ansi(text: str) -> str:
        return ANSI_RE.sub("", text)

    def visible_width(self, text: str) -> int:
        return sum(self._cell_width(char) for char in self.strip_ansi(text))

    @staticmethod
    def _cell_width(char: str) -> int:
        """Approximate terminal cell width without a third-party wcwidth dependency."""
        if char in "\r\n\t" or unicodedata.combining(char):
            return 0
        if unicodedata.category(char) in {"Cf", "Cc", "Mn", "Me"}:
            return 0
        return 2 if unicodedata.east_asian_width(char) in {"W", "F"} else 1

    def wrap_ansi(self, text: str, width: int) -> list[str]:
        """Wrap colored text by display columns, preserving hard breaks and SGR."""
        tokens = re.findall(r"\x1b\[[0-9;?]*[ -/]*[@-~]|[^\s\x1b]+|[ \t]+|\r?\n|\x1b.", text)
        lines: list[str] = []
        line = ""
        columns = 0
        active_styles: list[str] = []

        def close_line() -> None:
            nonlocal line, columns
            lines.append(line.rstrip() + ("\033[0m" if active_styles else ""))
            line = "".join(active_styles)
            columns = 0

        for token in tokens:
            if token.startswith("\x1b"):
                line += token
                if token.startswith("\x1b[") and token.endswith("m"):
                    params = token[2:-1] or "0"
                    codes = [int(code or "0") for code in params.split(";")
                             if code.isdigit() or code == ""]
                    if 0 in codes:
                        active_styles.clear()
                    else:
                        resets = {22, 23, 24, 25, 27, 28, 29, 39, 49}
                        reset_groups = {
                            22: {1, 2}, 23: {3}, 24: {4}, 25: {5, 6},
                            27: {7}, 28: {8}, 29: {9}, 39: set(range(30, 38)) | set(range(90, 98)) | {38},
                            49: {48},
                        }
                        for reset in codes:
                            if reset in resets:
                                removed = reset_groups[reset]
                                active_styles[:] = [
                                    style for style in active_styles
                                    if not any(int(code or "0") in removed
                                               for code in style[2:-1].split(";")
                                               if code.isdigit() or code == "")
                                ]
                        if not all(code == 0 for code in codes):
                            active_styles.append(token)
                continue
            if token in ("\n", "\r\n"):
                close_line()
                continue
            if token.isspace():
                token_width = sum(self._cell_width(char) for char in token)
                if columns + token_width <= width:
                    line += token
                else:
                    close_line()
                continue
            token_width = sum(self._cell_width(char) for char in token)
            if columns and columns + token_width > width:
                close_line()
            line += token
            columns += token_width
        if line:
            lines.append(line + ("\033[0m" if active_styles else ""))
        return lines or [""]

    @staticmethod
    def width() -> int:
        try:
            width = shutil.get_terminal_size((96, 24)).columns
        except OSError:
            width = 96
        return max(40, min(width, 112))

    def rule(self, width: int | None = None) -> str:
        return self.glyph("─", "-") * max(1, width or self.width())

    def icon(self, name: str, *, rich: bool = False) -> str:
        """A themed nerd-font glyph with an ASCII fallback.  ``rich`` headers
        drop the glyph entirely when it cannot render (ascii mode), rather
        than printing a noisy fallback."""
        preferred, fallback = _ICONS.get(name, ("", ""))
        if rich and not self.supports_glyph(preferred):
            return ""
        return self.glyph(preferred, fallback)

    def mark(self, kind: str) -> str:
        marks = {
            "ok": ("✓", "OK"), "fail": ("✗", "X"),
            "warn": ("▲", "!"), "step": ("›", ">"),
            "bullet": ("◆", "*"), "note": ("•", "*"),
            "hint": ("↳", "->"),
        }
        preferred, fallback = marks[kind]
        return self.glyph(preferred, fallback)

    def emit_wrapped(self, prefix: str, message: str, *, stream=None,
                     code: str | None = None) -> None:
        stream = stream or sys.stdout
        available = max(18, self.width() - self.visible_width(prefix))
        lines = self.wrap_ansi(str(message), available)
        continuation = " " * self.visible_width(prefix)
        for index, line in enumerate(lines):
            rendered = self.paint(code, line) if code else line
            print((prefix if index == 0 else continuation) + rendered,
                  file=stream, flush=True)

    def log(self, name: str, message: str, role: str = "brand") -> None:
        code = getattr(self.theme, role, self.theme.brand)
        mark = self.paint(code, self.mark("bullet"))
        label = name.upper()
        icon = _icon_for_section(label)
        glyph_text = self.icon(icon, rich=True) if icon else ""
        glyph = f"{self.paint(code, glyph_text)} " if glyph_text else ""
        self.emit_wrapped(f"{mark} {glyph}{self.paint(code, label)}  ", message)

    def step(self, message: str) -> None:
        self.emit_wrapped(f"{self.paint(self.theme.accent, self.mark('step'))} ", message)

    def ok(self, message: str) -> None:
        self.emit_wrapped(f"{self.paint(self.theme.success, self.mark('ok'))} ", message)

    def error(self, message: str) -> None:
        self.emit_wrapped(
            f"{self.paint(self.theme.error, self.mark('fail'))} "
            f"{self.paint(self.theme.error, 'error')} "
            f"{self.paint(self.theme.muted, self.glyph('·', ':'))} ",
            message, stream=sys.stderr,
        )

    def hint(self, message: str) -> None:
        self.emit_wrapped(f"  {self.paint(self.theme.muted, self.mark('hint'))} ",
                          message, code=self.theme.muted)

    def kv(self, label: str, value: object, *, code: str | None = None) -> None:
        """Render one compact, wrapping key/value row for command reports."""
        label_text = str(label)
        label_width = 22
        if len(label_text) > label_width:
            label_text = label_text[: label_width - 1] + "~"
        prefix = (
            "  " + self.paint(self.theme.accent_soft, label_text.ljust(label_width))
            + " " + self.paint(self.theme.muted, self.glyph("·", ":")) + " "
        )
        self.emit_wrapped(prefix, str(value), code=code)

    def header(self, title: str, subtitle: str | None = None) -> None:
        leader = f"{self.paint('1;35', self.mark('bullet'))} {self.paint('1;35', 'nytrix')}"
        if title:
            leader += f"  {self.paint('2', self.glyph('·', '/'))}  {self.paint('1;36', title)}"
        print(leader)
        if subtitle:
            for line in textwrap.wrap(subtitle, width=max(24, self.width() - 2),
                                      break_long_words=True,
                                      break_on_hyphens=False) or [subtitle]:
                print(f"  {self.paint('2', line)}")
        print(self.paint('2;35', self.rule()), flush=True)

    def section(self, title: str, icon: str | None = None) -> None:
        name = icon or _icon_for_section(title)
        glyph_text = self.icon(name, rich=True) if name else ""
        glyph = f"{self.paint('1;36', glyph_text)} " if glyph_text else ""
        prefix = (f"{self.paint('1;35', self.mark('bullet'))} "
                  f"{glyph}{self.paint('1;35', title)} ")
        remaining = max(3, self.width() - self.visible_width(prefix))
        print(prefix + self.paint('2', self.rule(remaining)), flush=True)

COLOR = color_on()
UI = Renderer(color=COLOR, glyph_style=glyph_style_from_env())

def c(code: str, s: str) -> str:
    return UI.paint(code, s)

def log(tag: str, msg: str) -> None:
    UI.log(tag, msg)

def step(msg: str) -> None:
    UI.step(msg)

def ok(msg: str) -> None:
    UI.ok(msg)

def err(msg: str) -> None:
    UI.error(msg)

def kv(label: str, value: object, *, code: str | None = None) -> None:
    UI.kv(label, value, code=code)

def _cmd_display(cmd: list[str] | str, shell: bool = False) -> str:
    if isinstance(cmd, str):
        return cmd
    return " ".join(shlex.quote(str(x)) for x in cmd)


def make_verbose_level() -> int:
    """Stacked -v depth (1..5): echoes, toolchain facts, live counters,
    full compile lines, raw passthrough."""
    raw = (os.environ.get("NYTRIX_MAKE_VERBOSE") or "").strip()
    try:
        return max(0, min(5, int(raw)))
    except ValueError:
        return 1 if raw else 0

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
    UI.emit_wrapped(
        f"{c(UI.theme.accent, UI.mark('step'))} ", msg,
        stream=sys.stderr, code=UI.theme.accent_soft,
    )

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

def bench_tools_current(build_root: Path, kind: str) -> bool:
    """Prove the existing tools current from Ninja inputs and header deps.

    This read-only check avoids taking the global build lock when another
    invocation is generating core.bc. An uncertain or stale graph builds
    through the ordinary locked path.
    """
    bdir = cmake_build_dir(build_root, kind)
    if os.name == "nt" or not (bdir / "build.ninja").is_file():
        return False
    ninja = which("ninja")
    if not ninja:
        return False
    for env_key in ("NYTRIX_HOST_CFLAGS", "NYTRIX_HOST_LDFLAGS", "NYTRIX_USE_LLVM"):
        if env_key in os.environ:
            value = cmake_flag_list(os.environ[env_key]) if env_key != "NYTRIX_USE_LLVM" else os.environ[env_key]
            if cmake_cache_value(bdir, env_key, "") != value:
                return False
    proof_path = bdir / ".nytrix-bench-build-inputs.json"
    try:
        proof = json.loads(proof_path.read_text())
        if proof.get("schema") == 1 and all(
            Path(name).stat().st_mtime_ns == value
            for name, value in proof.get("inputs", {}).items()
        ) and proof.get("inputs"):
            return True
    except (OSError, ValueError, TypeError):
        pass
    try:
        stamps: dict[Path, int] = {}
        def stamp(path: Path) -> int:
            path = path if path.is_absolute() else bdir / path
            if path not in stamps:
                stamps[path] = path.stat().st_mtime_ns
            return stamps[path]
        for name in ("build.ninja", ".ninja_deps", "CMakeCache.txt"):
            stamp(bdir / name)
        # A new source file changes its parent directory, even before Ninja
        # regenerates the globbed graph or learns its header dependencies.
        stamp(ROOT / "src")
        for directory in (ROOT / "src").rglob("*"):
            if directory.is_dir():
                stamp(directory)
        inputs: dict[str, set[str]] = {}
        for tool in ("ny-full", "ny-test", "ny"):
            stamp(bdir / tool)
            result = subprocess.run([ninja, "-C", str(bdir), "-t", "inputs", tool],
                                    capture_output=True, text=True, timeout=5)
            if result.returncode:
                return False
            inputs[tool] = set(result.stdout.splitlines())
        thresholds: dict[str, int] = {}
        for tool in ("ny-full", "ny-test", "ny"):
            limit = stamp(bdir / tool)
            # The launcher has an order-only dependency on ny-full; its own
            # executable need not be relinked whenever the compiler changes.
            leaves = inputs[tool]
            if tool == "ny":
                leaves = leaves - inputs["ny-full"] - {"ny-full"}
            if stamp(ROOT / "CMakeLists.txt") > max(stamp(bdir / "ny-full"), stamp(bdir / "ny-test")):
                return False
            for name in leaves:
                path = Path(name)
                if name.endswith(".o"):
                    thresholds[name] = min(thresholds.get(name, limit), limit)
                resolved = path if path.is_absolute() else bdir / path
                if resolved.is_file() and stamp(resolved) > limit:
                    return False
        deps = subprocess.run([ninja, "-C", str(bdir), "-t", "deps"],
                              capture_output=True, text=True, timeout=5)
        if deps.returncode:
            return False
        checked: set[str] = set()
        current = ""
        for line in deps.stdout.splitlines():
            if line and not line.startswith(" "):
                current = line.split(": #deps ", 1)[0]
                if current in thresholds:
                    if not line.endswith("(VALID)"):
                        return False
                    checked.add(current)
            elif current in thresholds and line.startswith("    "):
                if stamp(Path(line.strip())) > stamp(Path(current)):
                    return False
        if checked != set(thresholds):
            return False
        proof = {"schema": 1, "inputs": {str(path): value for path, value in stamps.items()}}
        temporary = proof_path.with_name(f"{proof_path.name}.{os.getpid()}.tmp")
        try:
            temporary.write_text(json.dumps(proof, separators=(",", ":")))
            temporary.replace(proof_path)
        finally:
            temporary.unlink(missing_ok=True)
        return True
    except (OSError, subprocess.SubprocessError):
        return False


def copy_stable_tool(source: Path, destination: Path) -> None:
    """Copy an executable only while its identity stays unchanged."""
    for _ in range(3):
        before = source.stat()
        shutil.copy2(source, destination)
        after = source.stat()
        if ((before.st_ino, before.st_size, before.st_mtime_ns) ==
                (after.st_ino, after.st_size, after.st_mtime_ns)):
            return
    destination.unlink(missing_ok=True)
    raise SystemExit(f"compiler changed repeatedly while snapshotting {source}; rerun benchmark")

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

def _running_as_root() -> bool:
    """Return True when this process already has administrative/root privileges."""
    if host_os() == "windows":
        return False
    geteuid = getattr(os, "geteuid", None)
    if geteuid is None:
        return False
    try:
        return geteuid() == 0
    except Exception:
        return False


def _privileged_command(cmd: list[str], *, action: str = "install dependencies") -> list[str]:
    """
    Return a command suitable for an operation that needs root privileges.

    Root containers commonly do not ship sudo, so never require sudo when
    euid == 0.  For an unprivileged user prefer sudo, then doas.  If neither
    is available, fail with an actionable error rather than a traceback.
    """
    if not cmd:
        raise ValueError("empty privileged command")

    if host_os() == "windows" or _running_as_root():
        return cmd

    sudo = which("sudo")
    if sudo:
        return [sudo, *cmd]

    doas = which("doas")
    if doas:
        return [doas, *cmd]

    err(f"{action} requires administrator/root privileges.")
    err("Re-run as root, install sudo/doas, or set NYTRIX_AUTO_DEPS=0 and install dependencies manually.")
    raise SystemExit(1)


def _run_privileged(cmd: list[str], *, action: str = "install dependencies", quiet: bool = False) -> None:
    run(_privileged_command(cmd, action=action), quiet=quiet)


def _require_program(name: str, *, context: str) -> str:
    """Resolve a required executable or stop with a concise diagnostic."""
    path = which(name)
    if path:
        return path
    err(f"{context}: required command not found: {name}")
    raise SystemExit(127)


def run(cmd: list[str] | str, *, shell: bool = False, env: dict[str, str] | None = None, quiet: bool = False) -> None:
    merged = os.environ.copy()
    if env:
        merged.update(env)
    merged.setdefault("NYTRIX_ROOT", str(ROOT))
    # Keep every Python helper and subprocess protocol UTF-8 even when the
    # parent shell uses a legacy locale.  Invalid tool output is preserved as
    # replacement text instead of aborting the frontend while rendering it.
    merged.setdefault("PYTHONUTF8", "1")
    merged.setdefault("PYTHONIOENCODING", "utf-8")
    show_cmd = _env_flag("NYTRIX_MAKE_COMMANDS", False)
    started = time.perf_counter()
    if show_cmd and not quiet:
        step("$ " + _cmd_display(cmd, shell))
    if COLOR:
        merged.setdefault("FORCE_COLOR", "1")
        merged.setdefault("CLICOLOR_FORCE", "1")
    if quiet:
        try:
            res = subprocess.run(
                cmd,
                cwd=str(ROOT),
                shell=shell,
                env=merged,
                text=True,
                encoding="utf-8",
                errors="replace",
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
        except FileNotFoundError as exc:
            program = cmd[0] if isinstance(cmd, list) and cmd else str(cmd)
            err(f"command not found: {program}")
            raise SystemExit(127) from exc
        if res.returncode != 0:
            if res.stdout:
                sys.stderr.write(res.stdout)
            raise subprocess.CalledProcessError(res.returncode, cmd)
        if show_cmd:
            ok("done in " + _fmt_elapsed(time.perf_counter() - started))
        return
    try:
        proc = subprocess.Popen(
            cmd,
            cwd=str(ROOT),
            shell=shell,
            env=merged,
            text=True,
            encoding="utf-8",
            errors="replace",
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=1,
        )
    except FileNotFoundError as exc:
        program = cmd[0] if isinstance(cmd, list) and cmd else str(cmd)
        err(f"command not found: {program}")
        raise SystemExit(127) from exc
    assert proc.stdout is not None
    for line in proc.stdout:
        print(color_build_line(line.rstrip("\n")), file=sys.stderr, flush=True)
    rc = proc.wait()
    if rc != 0:
        raise subprocess.CalledProcessError(rc, cmd)
    if show_cmd:
        ok("done in " + _fmt_elapsed(time.perf_counter() - started))

NINJA_PROGRESS_RE = re.compile(r"^(\s*)\[([^\]]*)\]\s*(.*)$")

# Rolling build counters (compiles/links per streamed build). Filled by
# color_build_line, reported by cmake_build at -vvv+.
_BUILD_COUNTERS = {"objects": 0, "links": 0, "generate": 0}

_COMPILER_RE = re.compile(r"^\s*(?:cd [^&]*&&\s*)?(/?\S*(?:clang(?:\+\+)?|gcc|g\+\+|cc|c\+\+)(?:-\d+)?(?:\.exe)?)\s+(.*)$")

def compact_compile_cmdline(cmd: str) -> str:
    """One sleek line for a full compiler invocation: compiler, the source
    it compiles, the object it emits, and a summarized flag set."""
    m = _COMPILER_RE.match(cmd)
    if not m:
        return None
    exe, rest = m.group(1), m.group(2)
    name = os.path.basename(exe)
    try:
        tokens = shlex.split(rest)
    except ValueError:
        tokens = rest.split()
    src = obj = None
    seen_flags: list[str] = []
    include_dirs = 0
    defines = 0
    i = 0
    while i < len(tokens):
        t = tokens[i]
        if t == "-o" and i + 1 < len(tokens):
            obj = tokens[i + 1]
            i += 2
            continue
        if t == "-c" and i + 1 < len(tokens):
            src = tokens[i + 1]
            i += 2
            continue
        if t.startswith("-I"):
            include_dirs += 1
            i += 1
            continue
        if t.startswith("-D"):
            defines += 1
            if len(seen_flags) < 6 and t not in ("-DNDEBUG",):
                seen_flags.append(t)
            i += 1
            continue
        if t.startswith(("-O", "-std=", "-march=", "-mcpu=", "-g", "-fopenmp",
                         "-Wall", "-Wextra", "-Wno", "-Werror", "-flto",
                         "-fno-omit", "-fsanitize", "-fPIC", "-fpic", "-pipe")):
            if t not in seen_flags and len(seen_flags) < 6:
                seen_flags.append(t)
            i += 1
            continue
        i += 1
    short = lambda pth: os.path.basename(pth) if len(pth) > 42 else pth
    tail: list[str] = [c("90", f"-I×{include_dirs}")] if include_dirs else []
    if defines > 1:
        tail.append(c("90", f"-D×{defines}"))
    tail += [c("36", flag) for flag in seen_flags]
    parts = [c("1;36", name)]
    if src:
        parts += [c("1;37", src)]
    if obj:
        parts += [c("90", "→"), c("1;32", short(obj))]
    if tail:
        parts.append(c("90", "·"))
    return " ".join(parts + tail)

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
    if low.startswith(("building c object", "building cxx object",
                       "building asm object", "compiling")):
        _BUILD_COUNTERS["objects"] += 1
    elif low.startswith(("linking",)):
        _BUILD_COUNTERS["links"] += 1
    elif low.startswith("generating"):
        _BUILD_COUNTERS["generate"] += 1
    if make_verbose_level() >= 4:
        compacted = compact_compile_cmdline(stripped)
        if compacted:
            return compacted
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

def color_ninja_progress(status: str, rest: str = "") -> str:
    parts = status.split()
    if len(parts) >= 2 and "/" in parts[0]:
        cur, total = parts[0].split("/", 1)
        extra = " " + c("2", " ".join(parts[1:])) if parts[1:] else ""
        return c("90", "[") + c("1;36", cur) + c("90", "/") + c("37", total) \
            + c("90", "]") + extra
    return c("90", "[") + c("1;36", status) + c("90", "]")

def color_source_dir(path: str) -> str:
    palette = (
        ("src/code/", "34"),
        ("src/code/lang/parse/", "35"),
        ("src/cmd/ny/repl/", "36"),
        ("src/base/", "32"),
        ("src/code/backend/wire/", "33"),
        ("src/code/lang/runtime/", "31"),
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
    lead, status, rest = m.groups()
    progress = color_ninja_progress(status, rest)
    if rest.startswith("Linking "):
        body = color_link_line(rest)
    else:
        body = color_build_action(rest)
    return lead + progress + (" " if rest else "") + body

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
        merged.setdefault("PYTHONUTF8", "1")
        merged.setdefault("PYTHONIOENCODING", "utf-8")
        return subprocess.run(
            cmd, cwd=str(ROOT), shell=shell, text=True,
            encoding="utf-8", errors="replace",
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=merged,
        )
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
        ("libwebp", "libwebp / core.parse.img.webp"),
        ("libturbojpeg", "libturbojpeg / core.parse.img.jpeg"),
        ("libpng", "libpng / core.parse.img.png"),
        ("freetype2", "freetype / core.os.ui.font.truetype"),
        ("fontconfig", "fontconfig / font discovery"),
        ("librsvg-2.0", "librsvg / core.parse.img.svg"),
        ("cairo", "cairo / core.parse.img.svg"),
        ("sndfile", "libsndfile / core.os.sound"),
        ("alsa", "ALSA / core.os.sound"),
        ("libpulse", "PulseAudio / core.os.sound"),
        ("jack", "JACK / core.os.sound"),
        ("x11", "X11 window backend"),
        ("xi", "XInput"),
        ("xfixes", "XFixes"),
        ("xcursor", "XCursor"),
        ("xrandr", "XRandR"),
        ("wayland-client", "Wayland window backend"),
        ("wayland-cursor", "Wayland cursor"),
        ("xkbcommon", "keyboard handling"),
        ("vulkan", "Vulkan renderer"),
        ("z3", "Z3 / core.math.smt backend"),
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
            _run_privileged(["apt", "update"], action="update apt package metadata")
            step("std deps: apt install")
            _run_privileged(["apt", "install", "-y", *pkgs], action="install optional Nytrix dependencies")
            return
        if distro in ("arch", "manjaro") or "arch" in like:
            step("std deps: pacman install")
            _run_privileged(["pacman", "-S", "--needed", "--noconfirm", *pkgs], action="install optional Nytrix dependencies")
            return
        if distro in ("fedora", "rhel", "centos", "rocky") or "fedora" in like or "rhel" in like:
            step("std deps: dnf install")
            _run_privileged(["dnf", "install", "-y", *pkgs], action="install optional Nytrix dependencies")
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
        "/usr/lib/llvm-23/include",
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

def _linux_required_packages(distro: str, like: str, missing: list[str]) -> list[str]:
    """Translate the detected missing tools into the smallest distro package set."""
    m = set(missing)
    pkgs: list[str] = []

    is_debian = distro in ("debian", "ubuntu", "linuxmint", "pop", "raspbian") or "debian" in like
    is_arch = distro in ("arch", "manjaro") or "arch" in like
    is_fedora = distro in ("fedora", "rhel", "centos", "rocky") or "fedora" in like or "rhel" in like

    if is_debian:
        if "cmake" in m:
            pkgs.append("cmake")
        if "ninja" in m:
            pkgs.append("ninja-build")
        if "pkg-config" in m:
            pkgs.append("pkg-config")
        if "git" in m:
            pkgs.append("git")

        if "clang" in m or "llvm" in m:
            v = apt_best_llvm_ver()
            if v > 0:
                if "clang" in m or "llvm" in m:
                    pkgs.append(f"clang-{v}")
                if "llvm" in m:
                    pkgs += [f"llvm-{v}", f"llvm-{v}-dev", f"llvm-{v}-runtime"]
                    if apt_has_pkg(f"libclang-{v}-dev"):
                        pkgs.append(f"libclang-{v}-dev")
                    elif apt_has_pkg("libclang-dev"):
                        pkgs.append("libclang-dev")
            else:
                if "clang" in m or "llvm" in m:
                    pkgs.append("clang")
                if "llvm" in m:
                    pkgs += ["llvm-dev", "libclang-dev"]
        return _dedupe(pkgs)

    if is_arch:
        mapping = {
            "cmake": "cmake",
            "ninja": "ninja",
            "pkg-config": "pkgconf",
            "git": "git",
            "clang": "clang",
            "llvm": "llvm",
        }
        for dep in ("cmake", "ninja", "pkg-config", "git", "clang", "llvm"):
            if dep in m:
                pkgs.append(mapping[dep])
        # clang-c/Index.h is shipped with clang on Arch; ensure it accompanies
        # llvm when LLVM development headers are the missing part.
        if "llvm" in m:
            pkgs.append("clang")
        return _dedupe(pkgs)

    if is_fedora:
        mapping = {
            "cmake": "cmake",
            "ninja": "ninja-build",
            "pkg-config": "pkgconf-pkg-config",
            "git": "git",
            "clang": "clang",
        }
        for dep in ("cmake", "ninja", "pkg-config", "git", "clang"):
            if dep in m:
                pkgs.append(mapping[dep])
        if "llvm" in m:
            pkgs += ["llvm-devel", "clang", "clang-devel"]
        return _dedupe(pkgs)

    return []


def ensure_deps(force_optional_prompt: bool = False, require_git: bool = False) -> None:
    if host_os() == "macos":
        configure_macos_llvm_env()
    if host_os() == "windows":
        prefix = _windows_msys2_target_prefix()
        if prefix:
            _windows_configure_msys2_env(prefix)
    missing: list[str] = []
    if not which("cmake"):
        missing.append("cmake")
    if not which("clang") or not which("clang++"):
        missing.append("clang")
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
        pkgs = _linux_required_packages(distro, like, missing)

        if not pkgs:
            err("Missing build dependencies: " + ", ".join(missing))
            err(f"No automatic package mapping is available for Linux distro: {distro or 'unknown'}")
            err("Install the missing tools manually or set NYTRIX_AUTO_DEPS=0.")
            raise SystemExit(1)

        if distro in ("debian", "ubuntu", "linuxmint", "pop", "raspbian") or "debian" in like:
            _require_program("apt", context="deps")
            step("deps: apt update")
            _run_privileged(["apt", "update"], action="update apt package metadata")
            step("deps: apt install " + " ".join(pkgs))
            _run_privileged(["apt", "install", "-y", *pkgs], action="install Nytrix build dependencies")
            _install_optional_std_deps(force_optional_prompt)
            _clean_stale_cmake_caches()
            return

        if distro in ("arch", "manjaro") or "arch" in like:
            _require_program("pacman", context="deps")
            step("deps: pacman install " + " ".join(pkgs))
            _run_privileged(
                ["pacman", "-S", "--needed", "--noconfirm", *pkgs],
                action="install Nytrix build dependencies",
            )
            _install_optional_std_deps(force_optional_prompt)
            _clean_stale_cmake_caches()
            return

        if distro in ("fedora", "rhel", "centos", "rocky") or "fedora" in like or "rhel" in like:
            _require_program("dnf", context="deps")
            step("deps: dnf install " + " ".join(pkgs))
            _run_privileged(["dnf", "install", "-y", *pkgs], action="install Nytrix build dependencies")
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
            # Prefer the rolling formula; configure_macos_llvm_env still
            # selects an already-installed versioned prefix first.
            pkgs[:0] = ["llvm", "lld"]
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
    # Size the build against memory that is actually available *right now*,
    # not total RAM: the desktop and any concurrent workload already hold
    # the difference, and hosts without swap hang when a full parallel
    # build overshoots.  Keep 8 GiB for the system and budget ~4 GiB per
    # compile job (the largest TUs spike hard at -O2).
    avail_gib = 0.0
    if host_os() == "linux":
        try:
            for ln in Path("/proc/meminfo").read_text(encoding="utf-8", errors="ignore").splitlines():
                if ln.startswith("MemAvailable:"):
                    kib = int((ln.split() or ["0", "0"])[1])
                    avail_gib = kib / (1024 * 1024)
                    break
        except Exception:
            pass
    if avail_gib > 0.0:
        reserve = 8.0
        per_job = 4.0
        cap = max(1, int((avail_gib - reserve) / per_job))
        if jobs > cap:
            jobs = cap
            return jobs, (f"auto jobs={jobs} capped by available RAM "
                          f"({avail_gib:.1f} GiB free, {reserve:.0f} reserved, ~{per_job:.0f} GiB/job); "
                          f"override with -j or NYTRIX_BUILD_JOBS")
    return jobs, f"auto jobs={jobs} using 75% of {cpu} cores (available RAM={avail_gib:.1f} GiB); override with -j or NYTRIX_BUILD_JOBS"

def host_available_mem_gib() -> float:
    if host_os() != "linux":
        return 0.0
    try:
        for ln in Path("/proc/meminfo").read_text(encoding="utf-8", errors="ignore").splitlines():
            if ln.startswith("MemAvailable:"):
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
    # Fixture jobs are independent processes. Use every logical CPU and let
    # the live-RAM budget below set the practical limit.
    auto = max(1, cpu)
    avail_gib = host_available_mem_gib()
    # Keep 8 GiB available for the system and budget 2.5 GiB per worker. This
    # covers the measured 2.2 GiB peak fixture RSS while leaving headroom for
    # compiler and runtime variation. Explicit -j and
    # NYTRIX_TEST_JOBS win.
    if avail_gib > 0.0:
        auto = min(auto, max(1, int((avail_gib - 8.0) / 2.5)))
    else:
        auto = min(auto, 2)
    return auto

def resolve_bench_jobs(requested: int = 0) -> int:
    """Choose bounded fixture concurrency for throughput benchmark runs."""
    bench_worker_gib = 1.5
    desired = requested
    if desired <= 0:
        env_v = (os.environ.get("NYTRIX_BENCH_JOBS") or "").strip()
        if env_v.isdigit() and int(env_v) > 0:
            desired = int(env_v)
        else:
            desired = os.cpu_count() or 1
    desired = min(96, max(1, desired))
    available = host_available_mem_gib()
    if available > 0.0:
        memory_cap = max(1, int((available - 8.0) / bench_worker_gib))
        desired = min(desired, memory_cap)
    return desired

def acquire_bench_run_lock(build_root: Path, kind: str):
    """Serialize benchmark suites that share the same generated toolchain.

    Individual fixtures use isolated temporary paths, but concurrent suites
    multiply their worker pools and can make compiler subprocesses fail under
    memory and process pressure. Keep one suite per build tree active at once.
    """
    bdir = cmake_build_dir(build_root, kind)
    bdir.mkdir(parents=True, exist_ok=True)
    lock = (bdir / ".nytrix-bench-run.lock").open("a+b")
    if os.name == "nt":
        lock.seek(0, os.SEEK_END)
        if lock.tell() == 0:
            lock.write(b"\0")
            lock.flush()
        while True:
            try:
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
                return lock
            except OSError:
                time.sleep(0.1)
    try:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print(f"{c(UI.theme.accent, UI.mark('step'))} BENCH · waiting for the active benchmark suite in {bdir}",
              file=sys.stderr, flush=True)
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
    return lock

def configure_macos_llvm_env() -> None:
    if host_os() != "macos":
        return
    configure_macos_tool_path()
    if os.environ.get("LLVM_CONFIG"):
        return
    prefixes = [
        Path("/opt/homebrew/opt/llvm@23"),
        Path("/usr/local/opt/llvm@23"),
        Path("/opt/homebrew/opt/llvm@22"),
        Path("/usr/local/opt/llvm@22"),
        Path("/opt/homebrew/opt/llvm@21"),
        Path("/usr/local/opt/llvm@21"),
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
    cfg = "Debug" if kind in ("debug", "asan", "ubsan", "coverage") else "Release"
    cmd = [
        "cmake", "-S", str(ROOT), "-B", str(bdir),
        f"-DCMAKE_BUILD_TYPE={cfg}", "-DNYTRIX_FAST_BUILD=ON",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        f"-DNYTRIX_HOST_CFLAGS={host_cflags}",
        f"-DNYTRIX_HOST_LDFLAGS={host_ldflags}",
    ]
    if "NYTRIX_USE_LLVM" in os.environ:
        cmd.append(f"-DNYTRIX_USE_LLVM={os.environ['NYTRIX_USE_LLVM']}")
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


def _first_line(cmd: list[str]) -> str:
    res = run_capture(cmd)
    out = (res.stdout or "").strip().splitlines()
    return out[0].strip() if out else ""

def _cc() -> str:
    return os.environ.get("NYTRIX_CC") or os.environ.get("CC") or "cc"

def show_build_introspection(build_root: Path, kind: str, jobs: int) -> None:
    """-vv: themed toolchain facts; -vvvvv: also active env overrides."""
    dot = UI.glyph("·", "|")
    UI.section("Toolchain", icon="build")
    compiler = _cc()
    ver = _first_line([compiler, "--version"]) or "unknown"
    triple = _first_line([compiler, "-dumpmachine"]) or host_os()
    UI.kv("compiler", f"{ver} {dot} target {triple}", code="1;36")
    cmake_ver = _first_line(["cmake", "--version"]).replace("cmake version ", "")
    ninja_ver = _first_line(["ninja", "--version"])
    UI.kv("generator", f"ninja {ninja_ver or '?'} {dot} cmake {cmake_ver or '?'}")
    ccache = which("ccache")
    UI.kv("ccache", ccache or "not found", code="1;32" if ccache else "2")
    UI.kv("jobs / dir", f"{jobs} {dot} {build_root.name}/{kind}")
    if make_verbose_level() >= 5:
        overrides = sorted((k, v) for k, v in os.environ.items()
                           if k.startswith("NYTRIX_") and v)
        if overrides:
            UI.kv("env", f"{len(overrides)} NYTRIX_* overrides", code="1;33")
            for key, value in overrides[:14]:
                shown = value if len(value) <= 60 else value[:57] + "..."
                UI.kv(key, shown, code="2")

def build_counters_summary(elapsed: float) -> None:
    stat = _BUILD_COUNTERS
    if stat["objects"] or stat["links"]:
        dot = UI.glyph("·", "|")
        parts = [f"{stat['objects']} objects" if stat["objects"] else "",
                 f"{stat['links']} link{'s' if stat['links'] != 1 else ''}" if stat["links"] else "",
                 f"{elapsed:.1f}s"]
        log("BUILD", " ".join(p for p in parts if p))

def cmake_build(
    build_root: Path,
    kind: str,
    targets: list[str],
    jobs: int,
    *,
    snapshot_tools: dict[str, Path] | None = None,
    snapshot_files: dict[Path, Path] | None = None,
) -> Path:
    bdir = cmake_build_dir(build_root, kind)
    bdir.mkdir(parents=True, exist_ok=True)
    lock_path = bdir / ".nytrix-cmake-build.lock"
    with lock_path.open("a+b") as build_lock:
        if os.name == "nt":
            build_lock.seek(0, os.SEEK_END)
            if build_lock.tell() == 0:
                build_lock.write(b"\0")
                build_lock.flush()
            while True:
                try:
                    build_lock.seek(0)
                    msvcrt.locking(build_lock.fileno(), msvcrt.LK_NBLCK, 1)
                    break
                except OSError:
                    time.sleep(0.1)
        else:
            try:
                fcntl.flock(build_lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                print(f"{c(UI.theme.accent, UI.mark('step'))} BUILD · waiting for the ongoing build in {bdir}",
                      file=sys.stderr, flush=True)
                fcntl.flock(build_lock.fileno(), fcntl.LOCK_EX)
        try:
            bdir = cmake_configure(build_root, kind)
            windows_stop_locked_build_targets(bdir, targets)
            boot_step(f"build {kind}: {', '.join(targets)}")
            level = make_verbose_level()
            if level >= 2:
                show_build_introspection(build_root, kind, jobs)
            build_cmd = ["cmake", "--build", str(bdir)]
            if targets:
                build_cmd.extend(["--target", *targets])
            build_cmd.extend(["-j", str(max(1, jobs))])
            if level >= 4:
                build_cmd.append("-v")
            build_env = {"NINJA_STATUS": "[%f/%t %es %r] "} if level >= 3 else None
            _BUILD_COUNTERS.update(objects=0, links=0, generate=0)
            started = time.perf_counter()
            run(build_cmd, quiet=QUIET_BOOTSTRAP, env=build_env)
            if snapshot_tools:
                for tool, snapshot in snapshot_tools.items():
                    shutil.copy2(resolve_tool_bin(build_root, kind, tool), snapshot)
            if snapshot_files:
                # Capture generated artifacts while still holding the build
                # lock: a concurrent `make` invocation can otherwise replace
                # them between this build and any later identity check.
                for source, snapshot in snapshot_files.items():
                    if source.is_file():
                        shutil.copy2(source, snapshot)
            if level >= 3:
                build_counters_summary(time.perf_counter() - started)
            boot_ok(f"build {kind}: {', '.join(targets)} complete")
            return bdir
        finally:
            if os.name == "nt":
                build_lock.seek(0)
                msvcrt.locking(build_lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(build_lock.fileno(), fcntl.LOCK_UN)

def stdlib_needs_regeneration(build_root: Path, kind: str) -> bool:
    """Return whether the generated core.ny is older than any library source."""
    generated = cmake_build_dir(build_root, kind) / "core.ny"
    if not generated.exists():
        return True
    try:
        generated_mtime = generated.stat().st_mtime_ns
        return any(path.stat().st_mtime_ns > generated_mtime
                   for path in (ROOT / "lib").rglob("*.ny")
                   if path.is_file())
    except OSError:
        return True

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

def install_prefix_writable(prefix: Path, bdir: Path) -> bool:
    leaves = (
        cmake_cache_value(bdir, "CMAKE_INSTALL_BINDIR", "bin"),
        cmake_cache_value(bdir, "CMAKE_INSTALL_LIBDIR", "lib"),
        cmake_cache_value(bdir, "CMAKE_INSTALL_DATADIR", "share") or "share",
        cmake_cache_value(bdir, "CMAKE_INSTALL_INCLUDEDIR", "include"),
    )
    return all(path_tree_writable(Path(leaf) if Path(leaf).is_absolute()
                                  else prefix / leaf)
               for leaf in leaves)

def sudo_noninteractive_ok(sudo: str) -> bool:
    return subprocess.run([sudo, "-n", "true"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0

def cmake_install(
    build_root: Path, kind: str, jobs: int, extra: list[str] | None = None
) -> int:
    bdir = cmake_build_dir(build_root, kind)
    prefix = Path(cmake_cache_value(bdir, "CMAKE_INSTALL_PREFIX", "/usr/local"))
    cmd = ["cmake", "--install", str(bdir.resolve())]
    install_args = list(extra or [])
    for i, arg in enumerate(install_args):
        if arg == "--prefix" and i + 1 < len(install_args):
            prefix = Path(install_args[i + 1])
        elif arg.startswith("--prefix="):
            prefix = Path(arg.split("=", 1)[1])
    cmd.extend(install_args)
    staged = bool(os.environ.get("DESTDIR"))
    sudo_cmd: list[str] | None = None
    if (not staged and host_os() != "windows" and hasattr(os, "geteuid")
            and os.geteuid() != 0 and not install_prefix_writable(prefix, bdir)):
        sudo = shutil.which("sudo")
        if sudo and sys.stdin.isatty():
            boot_notice(f"install prefix {prefix} needs root; running sudo cmake --install")
            sudo_cmd = [sudo]
        elif sudo and sudo_noninteractive_ok(sudo):
            boot_notice(f"install prefix {prefix} needs root; running sudo -n cmake --install")
            sudo_cmd = [sudo, "-n"]
        else:
            err(f"make: install prefix {prefix} is not writable; run: sudo ./make install")
            return 1
    # `cmake --install` does not build first. Build the configured install set
    # so a clean tree has ny-full, generated core.ny, and every optional tool
    # named by the install rules before CMake evaluates its install script.
    cmake_build(build_root, kind, [], jobs)
    if sudo_cmd:
        return subprocess.run([*sudo_cmd, *cmd], cwd=str(ROOT)).returncode
    return subprocess.run(cmd, cwd=str(ROOT)).returncode

def resolve_tool_bin(build_root: Path, kind: str, name: str) -> Path:
    exe = ".exe" if host_os() == "windows" else ""
    cands = [
        cmake_build_dir(build_root, kind) / f"{name}{exe}",
        cmake_build_dir(build_root, "release") / f"{name}{exe}",
    ]
    # Debug CMake configurations use distinct output names so a debug binary
    # cannot be mistaken for the release launcher.  Resolve that target name
    # here for every tool that has a debug configuration; callers should not
    # need to know the generator/configuration-specific filename.
    if kind in ("debug", "asan", "ubsan", "coverage"):
        debug_name = f"{name}_debug{exe}"
        cands.insert(0, cmake_build_dir(build_root, kind) / debug_name)
        if name == "ny":
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
    path = ROOT / "etc" / "tests" / "lang" / "interop" / "web" / "tests.json"
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
        shape_name = str(item.get("shape", "")).strip()
        if shape_name and not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_-]*", shape_name):
            raise SystemExit(f"web-test: fixture {fixture_id} has an invalid shape name")
        required = item.get("required", [])
        required_regex = item.get("required_regex", [])
        forbidden = item.get("forbidden", ["runtime error"])
        if not isinstance(required, list) or not isinstance(required_regex, list) or not isinstance(forbidden, list):
            raise SystemExit(f"web-test: fixture {fixture_id} markers must be lists")
        if not isinstance(item.get("skip_headless", False), bool):
            raise SystemExit(f"web-test: fixture {fixture_id} skip_headless must be boolean")
        item["id"] = fixture_id
        item["source"] = source
        item["shape"] = shape_name
        item["route"] = route
        item["virtual_time_ms"] = int(item.get("virtual_time_ms", 8000))
        item["required"] = [str(marker) for marker in required]
        item["required_regex"] = [str(pattern) for pattern in required_regex]
        item["forbidden"] = [str(marker) for marker in forbidden]
        item["skip_headless"] = bool(item.get("skip_headless", False))
        seen.add(fixture_id)
        out.append(item)
    return out

def _materialize_web_fixture_source(
    fixture: Path, build_root: Path, fixture_id: str, shape_name: str = ""
) -> Path:
    """Return a .ny source path for a browser fixture, extracting .nshape data."""
    if fixture.suffix == ".ny":
        return fixture
    if fixture.suffix != ".nshape":
        raise SystemExit(f"web-test: unsupported fixture source format: {fixture}")
    try:
        lines = fixture.read_text(encoding="utf-8").splitlines(keepends=True)
    except OSError as exc:
        raise SystemExit(f"web-test: cannot read fixture source: {fixture}: {exc}") from exc

    shape_header = re.compile(r"^\s*shape\s+([A-Za-z_][A-Za-z0-9_-]*)\s*\{")
    source_header = re.compile(
        r"^\s*source\s+ny\s+<<\s*(?:'([A-Za-z0-9_]+)'|\"([A-Za-z0-9_]+)\"|([A-Za-z0-9_]+))\s*$"
    )
    available_shapes = [
        match.group(1)
        for line in lines
        if (match := shape_header.match(line)) is not None
    ]
    selected_shape = shape_name
    if not selected_shape:
        if len(available_shapes) != 1:
            raise SystemExit(
                f"web-test: {fixture} has {len(available_shapes)} shapes; name one in tests.json"
            )
        selected_shape = available_shapes[0]
    if selected_shape not in available_shapes:
        raise SystemExit(
            f"web-test: shape {selected_shape!r} is not in {fixture.relative_to(ROOT)}"
        )

    current_shape = ""
    source_matches = 0
    source_lines: list[str] = []
    i = 0
    while i < len(lines):
        shape_match = shape_header.match(lines[i])
        if shape_match is not None:
            current_shape = shape_match.group(1)
        source_match = source_header.match(lines[i])
        if source_match is None:
            i += 1
            continue
        marker = next(value for value in source_match.groups() if value is not None)
        body: list[str] = []
        i += 1
        while i < len(lines) and lines[i].strip() != marker:
            body.append(lines[i])
            i += 1
        if i == len(lines):
            raise SystemExit(f"web-test: unterminated {marker} source block in {fixture}")
        i += 1
        if current_shape == selected_shape:
            source_matches += 1
            source_lines = body
    if source_matches != 1:
        raise SystemExit(
            f"web-test: expected one Ny source in shape {selected_shape!r} of {fixture}, found {source_matches}"
        )

    safe_id = re.sub(r"[^A-Za-z0-9_.-]+", "-", fixture_id).strip(".-") or "fixture"
    output_dir = build_root / "web" / "sources"
    output_dir.mkdir(parents=True, exist_ok=True)
    output = output_dir / f"{safe_id}.ny"
    output.write_text("".join(source_lines), encoding="utf-8")
    return output

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
    "env.core.os.ui.render.end_frame,"
    "env.core.os.time.msleep,env.rt_msleep_ms"
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
    env.setdefault("NYTRIX_CORE_PATH", str(cmake_build_dir(build_root, kind) / "core.ny"))
    try:
        result = subprocess.run(
            [str(ny_bin), "--wasm", "--parallel=off", "-emit-only",
             f"--emit-wasm={wasm}", str(source)],
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
                    if clipboard and browser_kind == "chromium":
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
                # Firefox does not implement Playwright's clipboard permission
                # grants. The fixture exercises the real gesture path below;
                # keep the context's browser defaults on that engine.
                if clipboard and browser_kind == "chromium":
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
        fixture = ROOT / str(item["source"])
        source = _materialize_web_fixture_source(
            fixture, build_root, fixture_id, str(item.get("shape", ""))
        )
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
    negative = _materialize_web_fixture_source(
        ROOT / "etc" / "tests" / "lang" / "interop" / "web" / "portability.nshape",
        build_root, "unsupported-process", "test_unsupported_process",
    )
    try:
        run_web_check(build_root, kind, [str(negative)])
    except SystemExit:
        negative_report = build_root / "web" / "check" / "unsupported-process.web-report.json"
        try:
            negative_data = json.loads(negative_report.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise SystemExit("web-test: native-process rejection did not write a valid report") from exc
        if (negative_data.get("ok") is not False or
                negative_data.get("unsupported") != ["core.os.process.run"]):
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
    audio = _materialize_web_fixture_source(
        ROOT / "etc" / "tests" / "lang" / "interop" / "web" / "audio.nshape",
        build_root, "audio-init", "test_audio_init",
    )
    if run_web_check(build_root, kind, [str(audio)]) != 0:
        return 1
    audio_report = build_root / "web" / "check" / "audio-init.web-report.json"
    try:
        audio_data = json.loads(audio_report.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit("web-test: audio check did not write a valid report") from exc
    if audio_data.get("ok") is not True or audio_data.get("unsupported") != []:
        raise SystemExit("web-test: browser audio lifecycle imports are not fully hosted")
    requests = _materialize_web_fixture_source(
        ROOT / "etc" / "tests" / "lang" / "interop" / "web" / "window.nshape",
        build_root, "window-requests", "test_window_requests",
    )
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
    decode = _materialize_web_fixture_source(
        ROOT / "etc" / "tests" / "lang" / "interop" / "web" / "audio.nshape",
        build_root, "audio-decode", "test_audio_decode",
    )
    decode_assets = ROOT / "etc" / "tests" / "lang" / "interop" / "web" / "assets"
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
    asset_read = _materialize_web_fixture_source(
        ROOT / "etc" / "tests" / "lang" / "interop" / "web" / "filesystem.nshape",
        build_root, "filesystem-asset-read", "test_filesystem_asset_read",
    )
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
        raise SystemExit("web-test: packaged asset was not readable through core.os.file_read")
    ok("web-test: packaged asset read through core.os.file_read")
    persistence = _materialize_web_fixture_source(
        ROOT / "etc" / "tests" / "lang" / "interop" / "web" / "filesystem.nshape",
        build_root, "filesystem-persistence", "test_filesystem_persistence",
    )
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
    renderer3d = _materialize_web_fixture_source(
        ROOT / "etc" / "tests" / "lang" / "interop" / "web" / "renderer.nshape",
        build_root, "renderer-3d", "test_renderer_3d",
    )
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
    pointer = _materialize_web_fixture_source(
        ROOT / "etc" / "tests" / "lang" / "interop" / "web" / "input.nshape",
        build_root, "input-pointer", "test_input_pointer",
    )
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
    touch = _materialize_web_fixture_source(
        ROOT / "etc" / "tests" / "lang" / "interop" / "web" / "input.nshape",
        build_root, "input-touch", "test_input_touch",
    )
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
    gamepad = _materialize_web_fixture_source(
        ROOT / "etc" / "tests" / "lang" / "interop" / "web" / "input.nshape",
        build_root, "input-gamepad", "test_input_gamepad",
    )
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
    framebuf = _materialize_web_fixture_source(
        ROOT / "etc" / "tests" / "lang" / "interop" / "web" / "renderer.nshape",
        build_root, "renderer-framebuffer", "test_renderer_framebuffer",
    )
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
    texture = _materialize_web_fixture_source(
        ROOT / "etc" / "tests" / "lang" / "interop" / "web" / "renderer.nshape",
        build_root, "renderer-2d-texture", "test_renderer_2d_texture",
    )
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
    if name.startswith("core.os.process."):
        return "native process"
    if name.startswith("core.os.ui.window."):
        return "native window"
    if name.startswith("core.os.ui.render.viewer.") or name.startswith("core.os.ui.render.dump."):
        return "native renderer tooling"
    return "browser host gap"

def _is_deferred_native_web_import(name: str) -> bool:
    return (name in {
                "_setjmp", "poll", "xkb_state_mod_index_is_active",
                # Native branches reachable from portable facades can leave
                # their POSIX calls in Wasm even when the target selects the
                # browser implementation. They remain trap-on-call imports in
                # wasm.js; only the portable browser adapter is executable.
                "close", "connect", "fclose", "fopen", "gethostbyname",
                "recv", "send", "socket",
            } or
            re.match(r"^(?:FT_|egl[A-Z]|gl[A-Z]|vk[A-Z]|wl_[a-z]|jack_[a-z]|pa_[a-z]|snd_[a-z])", name) is not None)

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
    # Runtime helper symbols are implementation details of the canonical Ny
    # ABI. The browser runner supplies a compatibility fallback for those
    # symbols; portability errors must be reserved for actual browser-host
    # APIs (process/window/Vulkan/etc.), which are reported by source-level
    # checks above and by this host-name set.
    # The shared renderer module can retain native backend entry points in
    # dead functions. wasm.js supplies trap-on-call imports for these names:
    # browser adapters must handle live rendering, and accidentally reaching
    # one of these native paths stays observable at runtime.
    deferred_native = sorted(
        name for name in imports
        if _is_deferred_native_web_import(name)
    )
    missing = sorted(name for name in imports - _web_host_import_names()
                     if not name.startswith("rt_") and
                     name != "memset" and not name.startswith("X") and
                     name not in deferred_native)
    # The Wasm module imports runtime helpers, not the original Ny API names.
    # Preserve a source-level rejection for native-only APIs so diagnostics and
    # the portability contract do not degrade into an implementation-detail
    # list such as rt_any_add/rt_tbuf_get.  The low-level imports remain in the
    # report as host_unsupported for debugging.
    source_text = source.read_text(encoding="utf-8", errors="replace")
    source_unsupported: list[str] = []
    if re.search(r"(?m)^\s*use\s+(?:std|core)\.os\.process\b", source_text):
        if re.search(r"\b(?:process\.)?run\s*\(", source_text):
            source_unsupported.append("core.os.process.run")
    unsupported = source_unsupported or missing
    report = {
        "source": _rel_or_abs(source),
        "target": cfg["target"],
        "capabilities": WEB_WASM_BARE_CAPABILITIES,
        "wasm": _rel_or_abs(wasm),
        "imports": sorted(imports),
        "supported": sorted(imports - set(missing) - set(deferred_native)),
        "deferred_native": deferred_native,
        "host_unsupported": missing,
        "unsupported": unsupported,
        "ok": not unsupported,
    }
    report_path = out_dir / (source.stem + ".web-report.json")
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if unsupported:
        print("web-check: unsupported browser imports:")
        groups: dict[str, list[str]] = {}
        for name in unsupported:
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
    deferred_native = {
        name for name in imports
        if _is_deferred_native_web_import(name)
    }
    missing = sorted(name for name in imports - _web_host_import_names()
                     if not name.startswith("rt_") and
                     name != "memset" and not name.startswith("X") and
                     name not in deferred_native)
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
        cmake_build(build_root, kind, ["ny", "ny-perf", "ny-test"], jobs)
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
        # bundled core.ny forces broad FFI/pkg-config probing before the prompt.
        interactive_repl = len(args) > 0 and args[0] == "-i"
        if not interactive_repl and _env_flag("NYTRIX_MAKE_USE_PREBUILT_STD", False):
            std_file = cmake_build_dir(build_root, kind) / "core.ny"
            if std_file.exists():
                std_path = str(std_file)
                env.setdefault("NYTRIX_CORE_PATH", std_path)
                env.setdefault("NYTRIX_BUILD_STD_PATH", std_path)
                env.setdefault("NYTRIX_STD_PREBUILT", std_path)
        if args and Path(args[0]).name == "engine.ny":
            env.setdefault("NYTRIX_JIT_CACHE", "0")
        env.setdefault("NYTRIX_STD_CACHE", "1")
        env.setdefault("NYTRIX_STD_BC_CACHE_AUTO", "1")
        env.setdefault("NYTRIX_JIT_CACHE", "1")
        # Do not execute a stale bundled core.bc after a library edit.  The
        # core.bc target is intentionally expensive, so normal `make ny`
        # falls back to the fresh generated core.ny until it is rebuilt.
        std_bc = cmake_build_dir(build_root, kind) / "core.bc"
        std_ny = cmake_build_dir(build_root, kind) / "core.ny"
        if std_ny.exists() and (not std_bc.exists() or
                                std_ny.stat().st_mtime_ns > std_bc.stat().st_mtime_ns):
            env["NYTRIX_STD_BC_CACHE_AUTO"] = "0"
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


_CHECK_DIAGNOSTIC_RE = re.compile(
    r"^(?P<path>.*?):(?P<line>\d+):(?P<column>\d+):\s+"
    r"\[(?P<code>[A-Z0-9]+)\]\s+(?P<level>warning|error|note):\s+(?P<message>.*)$"
)
_CHECK_SUMMARY_RE = re.compile(
    r"^\s{2}(?P<label>[a-z][a-z ]*?)\s+·\s+(?P<value>.*)$"
)
_CHECK_SUMMARY_LABELS = {
    "files", "functions", "std docs", "shape", "hygiene", "issues",
    "pressure", "similarity", "findings", "bugs", "syntax", "types", "smells",
}


def _check_report_dir() -> Path:
    configured = (os.environ.get("NYTRIX_CHECK_REPORT_DIR") or "").strip()
    root = Path(configured).expanduser() if configured else ROOT / "build" / "check-reports"
    if not root.is_absolute():
        root = ROOT / root
    root.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    candidate = root / stamp
    serial = 1
    while candidate.exists():
        candidate = root / f"{stamp}-{serial}"
        serial += 1
    candidate.mkdir()
    return candidate


def _check_report_path(report_dir: Path, phase: str) -> Path:
    return report_dir / f"{phase}.log"


def _check_report_summary(phase: str, output: str, report_path: Path, rc: int,
                          elapsed: float) -> None:
    clean = ANSI_RE.sub("", output or "")
    lines = clean.splitlines()
    title = next((line.strip() for line in lines if line.startswith("◆ ")), "")
    summaries: dict[str, str] = {}
    diagnostics: list[str] = []
    high_signal_total = 0
    for line in lines:
        summary = _CHECK_SUMMARY_RE.match(line)
        if summary:
            label = summary.group("label").strip()
            if label in _CHECK_SUMMARY_LABELS and label not in summaries:
                summaries[label] = summary.group("value").strip()
        diagnostic = _CHECK_DIAGNOSTIC_RE.match(line)
        if diagnostic and diagnostic.group("level") in ("warning", "error"):
            high_signal_total += 1
            if len(diagnostics) < 6:
                diagnostics.append(
                    f"{diagnostic.group('path')}:{diagnostic.group('line')} "
                    f"[{diagnostic.group('code')}] {diagnostic.group('message')}"
                )

    phase_title = {
        "tidy": "FORMAT",
        "check": "PARSE CHECK",
        "audit": "AUDIT",
    }.get(phase, phase.upper())
    if rc != 0:
        err(f"{phase_title} failed with exit {rc}")
    else:
        ok(f"{phase_title.lower()} complete in {_fmt_elapsed(elapsed)}")
    if title:
        kv("report", title.removeprefix("◆ "))
    preferred = {
        "tidy": ("files", "issues"),
        "check": ("files", "functions", "shape", "hygiene", "issues"),
        "audit": ("files", "pressure", "similarity", "findings", "bugs", "syntax", "types", "smells"),
    }.get(phase, tuple(summaries))
    for label in preferred:
        if label in summaries:
            kv(label, summaries[label])
    if diagnostics:
        UI.section("high-signal findings")
        for item in diagnostics:
            UI.emit_wrapped("  " + UI.paint(UI.theme.warning, UI.mark("warn")) + " ", item,
                            code=UI.theme.warning)
        if high_signal_total > len(diagnostics):
            kv("more", f"{high_signal_total - len(diagnostics)} additional warnings/errors; see full report")
    elif summaries.get("issues") or summaries.get("bugs"):
        kv("detail", "findings are summarized above; see full report for every note")
    elif not title and not summaries and not diagnostics:
        first_line = next((line.strip() for line in lines if line.strip()), "")
        if first_line:
            kv("output", first_line)
    try:
        relative = report_path.relative_to(ROOT)
    except ValueError:
        relative = report_path
    kv("full report", relative)


def run_check_tool(build_root: Path, kind: str, phase: str, args: list[str],
                   report_dir: Path) -> int:
    """Run one quality phase without flooding the terminal with its report."""
    binp = resolve_tool_bin(build_root, kind, "ny-fmt")
    launch = tool_launch_path(binp)
    env = _vendor_env(build_root)
    env["NYTRIX_TOOL_COLOR"] = "never"
    env["NO_COLOR"] = "1"
    started = time.perf_counter()
    try:
        result = subprocess.run(
            [launch, *args], cwd=str(ROOT), env=env, text=True,
            encoding="utf-8", errors="replace", stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        output = result.stdout or ""
        rc = result.returncode
    except OSError as exc:
        output = f"{type(exc).__name__}: {exc}\n"
        rc = 127
    report_path = _check_report_path(report_dir, phase)
    try:
        report_path.write_text(output, encoding="utf-8")
    except OSError as exc:
        err(f"could not write {report_path}: {exc}")
    _check_report_summary(phase, output, report_path, rc,
                          time.perf_counter() - started)
    return rc

def default_fuzz_shape_dir() -> str:
    for rel in ("build/cache/tests/shapes", "etc/tests/code/probes", "etc/tests"):
        if (ROOT / rel).exists():
            return rel
    return "build/cache/tests/fuzz/shapes"

def _bench_parse_native(text: str) -> dict:
    """Parse the bench result table into {name: native_time_string}."""
    rows = {}
    in_table = False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("benchmark") and "status" in stripped.split():
            in_table = True
            continue
        if not in_table:
            continue
        parts = line.split()
        if len(parts) < 7 or parts[1] != "ok":
            continue
        rows[parts[0]] = parts[4]
    return rows


def _bench_to_us(text: str):
    """Convert a bench duration cell to microseconds; None if not comparative."""
    text = text.strip()
    if text.startswith("<") or text == "n/a":
        return None
    try:
        if text.endswith("µs") or text.endswith("us"):
            return float(text.rstrip("usµ")) * 1.0
        if text.endswith("ms"):
            return float(text[:-2]) * 1000.0
        if text.endswith("s"):
            return float(text[:-1]) * 1_000_000.0
    except ValueError:
        return None
    return None


def bench_compare(build_root: Path, kind: str, extra: list[str],
                  runs: int = 3, threshold_pct: float = 25.0,
                  save: bool = False) -> int:
    """Multi-run bench mode: median-of-N native times, stored baseline,
    fail-closed regression threshold.  Single smoke runs measure compile+exec
    noise as much as performance; this is the comparative mode."""
    import json
    import statistics
    binp = resolve_tool_bin(build_root, kind, "ny-test")
    launch = tool_launch_path(binp)
    env = _vendor_env(build_root)
    env["NYTRIX_TEST_COLD"] = "1"
    baseline_override = (os.environ.get("NYTRIX_BENCH_BASELINE") or "").strip()
    if baseline_override:
        baseline_path = Path(baseline_override).expanduser()
    elif _env_flag("CI", False):
        # CI machines are deliberately not interchangeable: use the runner's
        # advertised OS/architecture (or an explicit NYTRIX_BENCH_RUNNER) as
        # the baseline namespace.  This keeps a Linux-x86 regression from
        # being compared with an ARM or Windows sample while preserving the
        # old single-file path for local development.
        runner = (os.environ.get("NYTRIX_BENCH_RUNNER") or
                  os.environ.get("RUNNER_NAME") or
                  os.environ.get("GITHUB_RUNNER_OS") or
                  os.environ.get("RUNNER_OS") or platform.system())
        arch = (os.environ.get("RUNNER_ARCH") or platform.machine())
        key = re.sub(r"[^A-Za-z0-9_.-]+", "_", f"{runner}-{arch}").strip("._-")
        baseline_path = cmake_build_dir(build_root, kind) / "bench-baselines" / f"{key}.json"
    else:
        baseline_path = cmake_build_dir(build_root, kind) / "bench-baseline.json"
    baseline_path.parent.mkdir(parents=True, exist_ok=True)
    samples: dict = {}
    for run in range(runs):
        step(f"bench-compare run {run + 1}/{runs}")
        proc = subprocess.run([launch, "--bench", "--color=never", *extra],
                              cwd=str(ROOT), env=env,
                              capture_output=True, text=True, timeout=1800)
        rows = _bench_parse_native(proc.stdout + "\n" + proc.stderr)
        if not rows:
            log("BENCH", f"run {run + 1}: no rows parsed (rc={proc.returncode})")
            return 1
        for name, native in rows.items():
            us = _bench_to_us(native)
            if us is None:
                continue
            samples.setdefault(name, []).append(us)
    medians = {name: statistics.median(vals)
               for name, vals in samples.items()
               if len(vals) == runs}
    if save or not baseline_path.exists():
        payload = {"runs": runs, "threshold_pct": threshold_pct,
                   "native_us": medians}
        baseline_path.write_text(json.dumps(payload, indent=1, sort_keys=True))
        log("BENCH", f"baseline saved: {baseline_path} ({len(medians)} rows)")
        return 0
    old = json.loads(baseline_path.read_text()).get("native_us", {})
    regressions = []
    for name in sorted(set(old) & set(medians)):
        delta = (medians[name] - old[name]) / old[name] * 100.0
        mark = ""
        if delta > threshold_pct:
            mark = "  REGRESSION"
            regressions.append(name)
        elif delta < -threshold_pct:
            mark = "  (improved)"
        log("BENCH", f"{name:24s} {old[name]:10.1f}us -> {medians[name]:10.1f}us  {delta:+7.1f}%{mark}")
    missing = sorted(set(old) - set(medians))
    if missing:
        log("BENCH", f"rows missing from this run (not compared): {', '.join(missing)}")
    if regressions:
        log("BENCH", f"FAIL: {len(regressions)} row(s) regressed more than {threshold_pct}%: {', '.join(regressions)}")
        return 1
    log("BENCH", f"no regression beyond {threshold_pct}% across {len(medians)} compared rows")
    return 0


def run_shapes_audit_cool(
    build_root: Path, kind: str, shape_dir: str, timeout: float | None
) -> int:
    """Run ny-test fuzz validate-shapes but print a cooler human line, not raw JSON.

    Captures the {"ok":...} JSON, then logs a compact summary. Raw JSON is
    only shown on parse failure or when the audit reports errors, plus with
    NYTRIX_TEST_DEBUG_VERBOSE=1 for machine consumers.
    """
    binp = resolve_tool_bin(build_root, kind, "ny-test")
    launch = tool_launch_path(binp)
    env = _vendor_env(build_root)
    try:
        proc = subprocess.run([launch, "fuzz", "validate-shapes", shape_dir],
                              cwd=str(ROOT), env=env,
                              capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        log("TEST", f"shapes audit timed out after {timeout}s")
        return 124
    out = (proc.stdout or "").strip()
    payload = None
    try:
        # validate-shapes prints a single JSON object on stdout.
        for line in out.splitlines():
            line = line.strip()
            if line.startswith("{") and line.endswith("}"):
                payload = json.loads(line)
                break
    except (ValueError, json.JSONDecodeError):
        payload = None
    verbose = _env_flag("NYTRIX_TEST_DEBUG_VERBOSE", False)
    if payload is None:
        # Couldn't parse: fall back to raw output so nothing is hidden.
        if out:
            print(out)
        return proc.returncode or 1
    audit_ok = payload.get("ok", False)
    count = payload.get("count", "?")
    errors = payload.get("errors", "?")
    fixtures = payload.get("fixtures", "?")
    embedded = payload.get("embedded_sources", "?")
    gens = payload.get("generators", {}) or {}
    gen_bits = []
    for key in ("typed", "optimizer", "torture", "stress", "program", "error"):
        try:
            n = int(gens.get(key, 0))
        except (TypeError, ValueError):
            n = 0
        if n:
            gen_bits.append(f"{key} {n}")
    gen_str = (", ".join(gen_bits)) if gen_bits else "no generators"
    if audit_ok and not errors:
        ok(f"shapes audit: {count} files · {fixtures} fixtures · {embedded} embedded sources ({gen_str})")
    else:
        log("TEST", f"shapes audit: {count} files · {errors} errors · {fixtures} fixtures ({gen_str})")
        first = payload.get("first_error")
        if first:
            log("TEST", f"shapes first error: {first}")
    if verbose or (not audit_ok):
        print(out)
    return proc.returncode


def _run_test_impl(build_root: Path, kind: str, jobs: int, extra: list[str]) -> int:
    started = time.perf_counter()
    # Help short-circuit: never run the suite, shapes audit, or benchmarks
    # for -h/--help. Just show ny-test help and return.
    if any(a in ("-h", "--help", "help") for a in extra):
        return run_tool(build_root, kind, "ny-test", ["--help"], timeout=30)
    # --cold flag: force a cold run without result/execution caches so every
    # fixture re-executes. The shared stdlib bitcode is a build artifact, not
    # an execution cache, so reuse it to avoid recompiling core.ny per fixture.
    # Accepts --cold / --no-cold; env NYTRIX_TEST_COLD=1 still works.
    extra = list(extra)
    cold_flag: bool | None = None
    if "--cold" in extra:
        cold_flag = True
        extra = [a for a in extra if a != "--cold"]
    if "--no-cold" in extra:
        cold_flag = False
        extra = [a for a in extra if a != "--no-cold"]
    if cold_flag:
        os.environ["NYTRIX_TEST_COLD"] = "1"
    elif cold_flag is False:
        os.environ["NYTRIX_TEST_COLD"] = "0"
    no_bench = "--no-bench" in extra
    with_bench = "--with-bench" in extra
    if no_bench and with_bench:
        raise SystemExit("test: choose either --with-bench or --no-bench")
    if no_bench or with_bench:
        extra = [a for a in extra if a not in ("--no-bench", "--with-bench")]
    with_preflight = "--with-preflight" in extra
    if with_preflight:
        extra = [a for a in extra if a != "--with-preflight"]
    # Keep the normal functional suite focused and predictable.  The timed
    # C/Ny benchmark matrix is opt-in via --with-bench or ./make bench.
    if with_bench:
        os.environ["NYTRIX_TEST_NO_BENCH"] = "0"
    else:
        os.environ["NYTRIX_TEST_NO_BENCH"] = "1"
    # Fail-fast stops queuing new fixtures on the first failure. Keep the
    # configured worker count so a slow early fixture does not serialize the
    # whole suite.
    test_jobs = resolve_test_jobs(jobs)
    cold = (os.environ.get("NYTRIX_TEST_COLD") or "").strip().lower() in ("1", "true", "yes", "on")
    pattern_run = any(
        arg == "--pattern" or arg.startswith("--pattern=") for arg in extra
    )
    # A positional fixture or directory is a focused run too.  In particular,
    # `./make test etc/tests/bench/datastructures` must not append the unrelated
    # 66-fixture C/Ny benchmark suite after the selected fixtures finish.
    value_options = {
        "--bin", "--jobs", "--timeout", "--std", "--core-bc", "--std-bc",
        "--triple", "--emulator", "--pattern", "--bench-run",
        "--bench-warmup", "--bench-opt", "--bench-tier", "--bench-engine",
        "--bench-target", "--bench-cache", "--bench-compiler",
        "--bench-compile-profile", "--bench-timeout", "--bench-out-csv",
        "--bench-out-json", "--bench-out-md", "--keep-artifacts",
        "--list-bench", "--list-meta", "--reader-atoms",
    }
    test_root = (ROOT / "etc/tests").resolve()
    explicit_test_path = False
    skip_value = False
    for arg in extra:
        if skip_value:
            skip_value = False
            continue
        if arg in value_options:
            skip_value = True
            continue
        if arg.startswith("-") or "=" in arg:
            continue
        try:
            candidate = Path(arg).resolve()
            candidate.relative_to(test_root)
            explicit_test_path = candidate.exists()
            if explicit_test_path:
                break
        except (OSError, ValueError):
            pass
    bench_mode_run = "--bench" in extra or "--bench-is" in extra
    focused_run = pattern_run or explicit_test_path or bench_mode_run
    explicit_std_bc = any(
        arg in ("--core-bc", "--std-bc")
        or arg.startswith(("--core-bc=", "--std-bc=")) for arg in extra
    )
    std_bc_setting = (
        os.environ.get("NYTRIX_TEST_USE_STD_BC", "auto").strip().lower()
    )
    existing_std_bc = cmake_build_dir(build_root, kind) / "core.bc"
    use_shared_std_bc = (
        std_bc_setting in ("1", "true", "yes", "on")
        or (
            std_bc_setting in ("", "auto")
            and (not focused_run or existing_std_bc.is_file())
        )
    )
    shared_std_enabled = (
        not explicit_std_bc
        and not any(arg == "--no-stdlib" for arg in extra)
        and use_shared_std_bc
    )
    # Full suites amortize the shared bitcode build across many fixtures.
    # Focused runs reuse an existing core.bc, but avoid a one-off build when
    # none exists. Force shared bitcode for focused runs with
    # NYTRIX_TEST_USE_STD_BC=1.
    build_shared_std_bc = shared_std_enabled
    if (
        focused_run
        and std_bc_setting in ("", "auto")
        and not explicit_std_bc
        and not any(arg == "--no-stdlib" for arg in extra)
        and not existing_std_bc.is_file()
    ):
        log(
            "TEST",
            "filtered selection: skipping core.bc prebuild "
            "(override: NYTRIX_TEST_USE_STD_BC=1)",
        )
    step("build test tools and core library")
    targets = ["ny", "ny-full", "ny-test", "core"]
    cmake_build(build_root, kind, targets, test_jobs)
    ny_bin = resolve_tool_bin(build_root, kind, "ny")
    std_bundle = cmake_build_dir(build_root, kind) / "core.ny"
    std_bundle_path = str(std_bundle.resolve()) if std_bundle.is_file() else None
    std_bc = cmake_build_dir(build_root, kind) / "core.bc"
    trace_dir = _nytrix_cache_root("test-trace") / f"run-{os.getpid()}"
    shutil.rmtree(trace_dir, ignore_errors=True)
    trace_dir.mkdir(parents=True, exist_ok=True)
    full_bin = resolve_tool_bin(build_root, kind, "ny-full")
    suite_full_bin = full_bin.with_name(
        f"{full_bin.stem}-test-{os.getpid()}{full_bin.suffix}"
    )
    shared_std_args: list[str] = []
    core_bc_inputs = std_bc.with_name("core.bc.inputs")
    core_bc_signature = std_bc.with_name("core.bc.signature")

    def sha256_file(path: Path) -> str:
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()

    if with_preflight and os.name == "nt":
        # The optional Windows preflight exercises case-insensitive module
        # identity comparison, which the POSIX driver cannot cover.
        step("build Windows module identity driver")
        identity_build = subprocess.run(
            ["cmake", "--build", str(build_root), "--target",
             "ny-module-identity-test", "--config", "Release",
             "--parallel", str(test_jobs)],
            cwd=str(ROOT),
        )
        if identity_build.returncode != 0:
            err("Windows module identity driver build failed")
            return identity_build.returncode
        identity_candidates = (
            build_root / "ny-module-identity-test.exe",
            build_root / "Release" / "ny-module-identity-test.exe",
        )
        identity_bin = next(
            (candidate for candidate in identity_candidates if candidate.is_file()),
            None,
        )
        if identity_bin is None:
            err("Windows module identity driver missing from build output")
            return 1
        step("run Windows module identity driver")
        identity_run = subprocess.run([str(identity_bin)], cwd=str(ROOT))
        if identity_run.returncode != 0:
            err("Windows module identity driver failed")
            return identity_run.returncode
    elif with_preflight and not focused_run and "NYTRIX_USE_LLVM:BOOL=ON" in (
        cmake_build_dir(build_root, kind) / "CMakeCache.txt"
    ).read_text(errors="replace"):
        # Keep machine-code scaling checks on the routine (unfocused) test
        # path only. Focused --pattern runs skip the rebuild + ~300ms run:
        # fixtures themselves exercise the backend, and the driver would be
        # ~20% overhead on a 1-2s focused invocation.
        # The native driver isolates large-function lowering, register allocation,
        # and ELF object emission from source parsing and runtime costs.
        step("build backend scaling driver")
        scaling_build = subprocess.run(
            ["cmake", "--build", str(cmake_build_dir(build_root, kind)), "--target",
             "ny-backend-scaling-test", "--parallel", str(test_jobs)],
            cwd=str(ROOT),
        )
        if scaling_build.returncode != 0:
            err("backend scaling driver build failed")
            return scaling_build.returncode
        scaling_candidates = (
            cmake_build_dir(build_root, kind) / "ny-backend-scaling-test",
            cmake_build_dir(build_root, kind) / "Release" / "ny-backend-scaling-test",
        )
        scaling_bin = next(
            (candidate for candidate in scaling_candidates if candidate.is_file()),
            None,
        )
        if scaling_bin is None:
            err("backend scaling driver missing from build output")
            return 1
        step("run backend scaling driver")
        scaling_run = subprocess.run([str(scaling_bin)], cwd=str(ROOT))
        if scaling_run.returncode != 0:
            err("backend scaling driver failed")
            return scaling_run.returncode
    if with_preflight and not focused_run:
        step("run semantic artifact and legacy expansion checks")
        artifact_run = subprocess.run(
            ["ctest", "--test-dir", str(cmake_build_dir(build_root, kind)),
             "--output-on-failure", "-C", "Release", "-R",
             "^(semantic-effect-artifact|legacy-derive-expansion)$"],
            cwd=str(ROOT),
        )
        if artifact_run.returncode != 0:
            return artifact_run.returncode
    elif not focused_run:
        log("TEST", "skipping auxiliary preflight drivers and CTests by default; use --with-preflight")
    suite_core_ny = trace_dir / "core.ny.shared"
    suite_core_bc = trace_dir / "core.bc.shared"
    suite_core_signature = trace_dir / "core.bc.signature.shared"
    suite_core_inputs = trace_dir / "core.bc.inputs.shared"
    suite_core_links = trace_dir / "core.bc.links.shared"
    if build_shared_std_bc:
        # These test-only builds can refresh ny-full after the initial tool
        # build. Generate/check core.bc afterward so its fingerprint matches
        # the compiler that fixtures will actually execute. Snapshot the
        # generated core artifacts while the build lock is still held: the
        # identity check below must compare the exact bytes this build
        # produced, not the live paths a concurrent `make` may have already
        # replaced (which would otherwise force a full core-from-source
        # recompile in every fixture).
        # Pin the compiler first, then build core.bc with that exact snapshot.
        # The core_bc target normally fingerprints the live ny-full path; a
        # concurrent build can replace that path between compilation and the
        # fixture snapshot, making the new artifact unusable for this suite.
        cmake_build(
            build_root, kind, ["ny-full"], test_jobs,
            snapshot_tools={"ny-full": suite_full_bin},
        )
        previous_core_bc_compiler = os.environ.get("NYTRIX_CORE_BC_COMPILER")
        os.environ["NYTRIX_CORE_BC_COMPILER"] = str(suite_full_bin.resolve())
        try:
            cmake_build(
                build_root, kind, ["core_bc"], test_jobs,
                snapshot_files={
                    std_bundle: suite_core_ny,
                    std_bc: suite_core_bc,
                    core_bc_signature: suite_core_signature,
                    core_bc_inputs: suite_core_inputs,
                    std_bc.with_name(std_bc.name + ".links"): suite_core_links,
                },
            )
        finally:
            if previous_core_bc_compiler is None:
                os.environ.pop("NYTRIX_CORE_BC_COMPILER", None)
            else:
                os.environ["NYTRIX_CORE_BC_COMPILER"] = previous_core_bc_compiler
    else:
        # Fixture workers and generated-compiler tests must not execute a
        # binary while another make invocation is replacing ny-full.
        cmake_build(
            build_root, kind, ["ny-full"], test_jobs,
            snapshot_tools={"ny-full": suite_full_bin},
        )
    # The test-only compiler/core build above can refresh the standard bundle
    # after the initial tool build. Rebuild and snapshot both halves of the
    # launcher/compiler pair after that refresh so their embedded stdlib
    # identities describe the same source tree.
    cmake_build(
        build_root, kind, ["ny-full", "ny"], test_jobs,
        snapshot_tools={"ny-full": suite_full_bin},
    )
    if build_shared_std_bc:
        try:
            checker_name = "ny-core-bc-check.exe" if os.name == "nt" else "ny-core-bc-check"
            checker = cmake_build_dir(build_root, kind) / checker_name
            cache_check = subprocess.run(
                [
                    str(checker),
                    str(suite_core_bc),
                    str(suite_core_ny),
                    str(suite_core_signature),
                    str(suite_core_inputs),
                ],
                cwd=str(ROOT),
                # The generated checker links libcrypto and the core hash
                # module can use that backend.  Its pure Ny SHA-256 fallback
                # currently trips native dynamic-list lowering in this tiny
                # checker, which incorrectly makes every cold test compile
                # the standard library from source.
                env={**os.environ, "NY_HASH_NATIVE": "1"},
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=30,
                check=False,
            )
            std_bc_is_current = (
                suite_core_bc.is_file()
                and suite_core_bc.stat().st_size > 0
                and cache_check.returncode == 0
            )
        except (OSError, subprocess.TimeoutExpired):
            std_bc_is_current = False
        if std_bc_is_current:
            # Keep this suite on one implementation even if another build
            # publishes a newer shared artifact while fixtures are running.
            suite_std_bc = trace_dir / "core.bc"
            shutil.copy2(suite_core_bc, suite_std_bc)
            if suite_core_links.is_file():
                shutil.copy2(suite_core_links, suite_std_bc.with_name("core.bc.links"))
            shared_std_args = ["--core-bc", str(suite_std_bc.resolve())]
            extra = [*extra, *shared_std_args]
            log("TEST", f"shared core bytecode: {std_bc}")
    using_std_bc = explicit_std_bc or bool(shared_std_args)
    if using_std_bc and std_bundle_path:
        # Do not also inject the monolithic core.ny source bundle when fixtures
        # receive core.bc. The loader treats NYTRIX_STD_PREBUILT as source to
        # parse, so setting both makes every cold fixture parse the full bundle.
        for key in ("NYTRIX_STD_PREBUILT", "NYTRIX_BUILD_STD_PATH", "NYTRIX_CORE_PATH", "NYTRIX_STD_PATH"):
            if os.environ.get(key) == std_bundle_path:
                os.environ.pop(key, None)
        if _env_flag("NYTRIX_STD_PREFER_PREBUILT", False):
            os.environ.pop("NYTRIX_STD_PREFER_PREBUILT", None)
    if build_shared_std_bc and not shared_std_args:
        log("TEST", "shared core bytecode unavailable; fixtures will compile core from source")
    os.environ["NYTRIX_TEST_TRACE_DIR"] = str(trace_dir)
    os.environ["NYTRIX_TEST_NY_FULL_BIN"] = str(suite_full_bin)
    suite_ny_bin = ny_bin.with_name(
        f"{ny_bin.stem}-test-{os.getpid()}{ny_bin.suffix}"
    )
    copy_stable_tool(ny_bin, suite_ny_bin)
    # The launcher and compiler are snapshotted as a pair for fixture runs.
    # Reject a stale pair before it can turn one identity mismatch into dozens
    # of unrelated runtime failures.
    identity_env = os.environ.copy()
    identity_env["NYTRIX_NY_FULL_BIN"] = str(suite_full_bin)
    try:
        identity = subprocess.run(
            [str(suite_ny_bin), "--version-id"],
            cwd=str(ROOT), env=identity_env, capture_output=True, text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        suite_ny_bin.unlink(missing_ok=True)
        err(f"cannot verify test launcher/compiler build identities: {exc}")
        return 1
    identities = dict(
        line.split(" ", 1) for line in identity.stdout.splitlines()
        if line.startswith(("ny ", "ny-full "))
    )
    if (
        identity.returncode != 0
        or identities.get("ny") != identities.get("ny-full")
    ):
        err(
            "test launcher/compiler build identities differ: "
            f"launcher={identities.get('ny', 'unknown')} "
            f"compiler={identities.get('ny-full', 'unknown')}"
        )
        suite_ny_bin.unlink(missing_ok=True)
        return 1
    os.environ["NYTRIX_TEST_PROFILE_JSON"] = str(trace_dir / "profile.json")
    # Failure replay reruns failed fixtures and can dominate a long suite.
    # Keep the original fixture output authoritative; replay is opt-in.
    # Explicit CLI settings override the environment default.
    if any(arg in ("--debug-failures", "--with-replay") for arg in extra):
        os.environ["NYTRIX_TEST_DEBUG_FAILURES"] = "1"
    elif "--no-debug-failures" in extra:
        os.environ["NYTRIX_TEST_DEBUG_FAILURES"] = "0"
    os.environ.setdefault("NYTRIX_TEST_DEBUG_FAILURES", "0")
    os.environ.setdefault("NYTRIX_TEST_DEBUGGER_ALL", "0")
    os.environ.setdefault(
        "NYTRIX_TEST_DIAG_DIR", str(build_root / "diagnostics" / "tests")
    )
    os.environ["NYTRIX_TEST_INCLUDE_BENCHMARK"] = "1"
    # Benchmark fixtures use full stress inputs, so run their correctness
    # checks through the native executor by default. The shared core.bc is
    # built once above; this no longer recompiles the whole stdlib per fixture.
    # Set NYTRIX_TEST_BENCHMARK_NATIVE=0 to exercise the default VM route.
    os.environ.setdefault("NYTRIX_TEST_BENCHMARK_NATIVE", "1")
    # Runtime and stdlib fixtures exercise their declared executor/matrix.
    # Forcing native-only here made every row in large CLI flag matrices
    # compile native code, turning a quick argument check into minutes of
    # repeated codegen. Keep the extra native sweep available as an explicit
    # opt-in for developers who want it.
    os.environ.setdefault("NYTRIX_TEST_RUNTIME_NATIVE", "0")
    os.environ.setdefault("NYTRIX_TEST_STD_NATIVE", "0")
    os.environ["NYTRIX_TEST_BENCHMARK_REPL"] = "1"
    os.environ["NYTRIX_TEST_RUNTIME_REPL"] = "1"
    os.environ["NYTRIX_TEST_STD_REPL"] = "1"
    if cold:
        os.environ["NYTRIX_TEST_CACHE"] = "0"
    else:
        os.environ.setdefault("NYTRIX_TEST_CACHE", "1")
    exec_cache_value = os.environ.get("NYTRIX_TEST_EXEC_CACHE")
    exec_cache = not cold and (exec_cache_value is None or
                               exec_cache_value.strip().lower() in ("1", "true", "yes", "on"))
    os.environ["NYTRIX_TEST_NO_NATIVE_CACHE"] = "1" if (cold or not exec_cache) else "0"
    os.environ["NYTRIX_AOT_CACHE"] = "1" if (exec_cache and not cold) else "0"
    os.environ["NYTRIX_JIT_CACHE"] = "1" if (exec_cache and not cold) else "0"
    # UI/audio/clipboard modules are optional host-resource probes.  Their
    # imports can spend the entire per-fixture budget in Vulkan/X11/audio
    # discovery, so keep them out of the normal language/compiler gate; run
    # them explicitly with NYTRIX_TEST_SKIP_SYSTEM_STDLIB=0.
    os.environ.setdefault("NYTRIX_TEST_SKIP_SYSTEM_STDLIB", "1")
    # A cold correctness run must rerun every fixture and bypass result/JIT/AOT
    # caches, but the standard-library source cache is a compile artifact. It
    # is shared across otherwise independent fixtures, so disabling it makes
    # every fixture rebuild the same core modules. Keep it enabled by default;
    # tests of uncached stdlib compilation can opt out explicitly.
    std_cache_enabled = (
        os.environ.get("NYTRIX_TEST_STD_CACHE", "1").strip().lower()
        not in ("0", "false", "no", "off")
    )
    os.environ["NYTRIX_STD_CACHE"] = "1" if std_cache_enabled else "0"
    cache_mode = "on" if _env_flag("NYTRIX_TEST_CACHE", True) else "off"
    exec_cache_mode = "on" if (exec_cache and not cold) else "off"
    std_cache_mode = "on" if std_cache_enabled else "off"
    diagnostics_mode = "on" if _env_flag("NYTRIX_TEST_DEBUG_FAILURES", True) else "off"
    debugger_mode = "on" if _env_flag("NYTRIX_TEST_DEBUGGER_ALL", True) else "crashes-only"
    log("TEST", f"make test: runtime/repl/error/benchmark fixtures; result_cache {cache_mode}, exec_cache {exec_cache_mode} (default on; --cold disables), std_cache {std_cache_mode}; failure_diagnostics {diagnostics_mode}, debugger {debugger_mode}")
    if cache_mode == "on":
        # Loud, impossible-to-miss notice: a cached run is not evidence.  The
        # 2026-09-19 audit caught the default run reporting green while the
        # cold run was red because every row was a result-cache hit.
        log("TEST", "\033[33mWARNING\033[0m result cache is ON: rows may be cache hits, "
             "not executions. A green line here does not prove the compiler "
             "works; run ./make test --cold (or NYTRIX_TEST_COLD=1 ./make test, or ./make check) "
             "for the fail-closed count.")
    # Individual fixtures remain bounded by NYTRIX_TEST_TIMEOUT. The full
    # suite intentionally has no outer deadline: healthy cold runs can exceed
    # thirty minutes on large fixture sets.
    os.environ.setdefault("NYTRIX_TEST_TIMEOUT", "60")
    suite_timeout = None
    step(f"run tests: bin=ny jobs={test_jobs} suite_timeout=unbounded")
    # `--no-progress` controls this Python bootstrapper.  ny-test deliberately
    # has no corresponding flag, so keep this global presentation option out
    # of its fixture-runner argv.
    test_extra = [arg for arg in extra if arg != "--no-progress"]
    if _env_flag("NYTRIX_CHECK_COMPACT", False) and "--failures-only" not in test_extra:
        # Keep the quality gate compact only when explicitly requested. The
        # default check run prints each fixture result so its coverage is
        # visible; complete logs are still retained by the runner.
        test_extra.append("--failures-only")
    try:
        rc = run_tool(
            build_root, kind, "ny-test",
            ["--bin", str(suite_ny_bin), "--jobs", str(test_jobs), *test_extra],
            timeout=suite_timeout,
        )
    finally:
        suite_ny_bin.unlink(missing_ok=True)
    # validate-shapes is part of the unfiltered functional gate on every host;
    # focused fixture selections skip this full-tree audit to keep iteration scoped.
    # Printed cooler via run_shapes_audit_cool (human summary, raw JSON only
    # on error/verbose).
    if rc == 0 and not focused_run:
        rc = run_shapes_audit_cool(
            build_root, kind, default_fuzz_shape_dir(), suite_timeout
        )
    elif rc == 0 and focused_run and not bench_mode_run:
        log("TEST", "skipping the full shapes audit for a focused test selection")
    suite_rc = rc
    # A failed/expired fixture suite is not a valid benchmark baseline.  Do
    # not start the expensive full benchmark pass after it; this also prevents a
    # compile-timeout sweep from consuming another suite-sized budget and
    # evicting useful diagnostics under memory pressure.
    if rc != 0:
        bench_rc = 0
        log("TEST", "skipping benchmarks because the fixture suite failed or timed out")
    elif focused_run:
        bench_rc = 0
        log(
            "TEST",
            "skipping the automatic full benchmark suite for a focused run; "
            "use ./make bench for explicit performance measurements",
        )
    elif with_bench and not _env_flag("NYTRIX_TEST_NO_BENCH", False):
        step("run benchmarks: C/Ny parity and performance (set NYTRIX_TEST_NO_BENCH=1 to skip)")
        # Timed benchmark fixtures run serially by default to keep samples
        # comparable. Test-suite fixture parallelism is unrelated and must not
        # silently become benchmark fixture concurrency. Users can explicitly
        # opt into that trade-off with NYTRIX_TEST_BENCH_JOBS.
        bench_env = (os.environ.get("NYTRIX_TEST_BENCH_JOBS") or "").strip()
        bench_jobs = 1
        if bench_env:
            try:
                v = int(bench_env)
                if v >= 1:
                    bench_jobs = v
            except Exception:
                pass
        bench_rc = run_tool(
            build_root, kind, "ny-test",
            ["--bin", str(ny_bin), "--bench", "--jobs", str(bench_jobs),
             *shared_std_args],
            timeout=suite_timeout,
        )
    else:
        bench_rc = 0
        log("TEST", "skipping timed benchmarks by default; use --with-bench or ./make bench")
    elapsed_ms = int((time.perf_counter() - started) * 1000.0)
    if suite_rc == 0 and bench_rc == 0:
        ok(f"test suite completed in {elapsed_ms}ms")
    else:
        log("TEST", f"test suite failed after {elapsed_ms}ms (suite={suite_rc}, bench={bench_rc})")
    try:
        suite_full_bin.unlink()
    except OSError:
        pass
    return suite_rc or bench_rc


def run_test(build_root: Path, kind: str, jobs: int, extra: list[str]) -> int:
    return _run_test_impl(build_root, kind, jobs, extra)


def check_source_hygiene() -> int:
    """Reject accidental binary source and temporary print-debugging probes."""
    source_roots = (ROOT / "src", ROOT / "lib", ROOT / "etc" / "tests")
    suffixes = {".c", ".h", ".ny"}
    debug_print = re.compile(
        r'\b(?:printf|fprintf)\s*\(\s*"(?:DEBUG|XXX|TODO DEBUG)'
        r'|\bprint\s*\(\s*"DEBUG'
    )
    nul_files: list[Path] = []
    debug_files: list[tuple[Path, int, str]] = []
    for root in source_roots:
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix not in suffixes:
                continue
            try:
                data = path.read_bytes()
            except OSError as exc:
                err(f"CHECK unable to read {path.relative_to(ROOT)}: {exc}")
                return 1
            if b"\x00" in data:
                nul_files.append(path)
                continue
            lines = data.decode("utf-8", errors="replace").splitlines()
            for line_no, line in enumerate(lines, 1):
                if debug_print.search(line):
                    debug_files.append((path, line_no, line.strip()))
    if nul_files:
        err("CHECK source hygiene failed: NUL byte(s) found in")
        for path in nul_files:
            err(f"  {path.relative_to(ROOT)}")
        return 1
    if debug_files:
        err("CHECK source hygiene failed: temporary print debugging found")
        for path, line_no, line in debug_files:
            err(f"  {path.relative_to(ROOT)}:{line_no}: {line}")
        err("Use NY_TRACE_* or compiler diagnostic switches instead.")
        return 1
    ok("source hygiene")
    return 0

_CLOC_LANGUAGES: dict[str, tuple[str, ...]] = {
    "Nytrix": (".ny", ".nshape"),
    "C": (".c", ".h"),
    "C++": (".cc", ".cpp", ".cxx", ".hh", ".hpp", ".hxx"),
    "Python": (".py",),
    "JavaScript/TypeScript": (".js", ".mjs", ".cjs", ".ts", ".tsx"),
    "GLSL": (".glsl", ".vert", ".frag", ".geom", ".comp"),
    "Shell": (".sh", ".bash", ".zsh"),
    "Rust": (".rs",),
    "Go": (".go",),
    "Docs": (".md", ".rst", ".texi", ".adoc"),
    "Config": (".cmake", ".toml", ".yaml", ".yml", ".json", ".xml", ".mk"),
}

_CLOC_BASENAMES: dict[str, str] = {
    "make": "Python",
    "CMakeLists.txt": "Config",
    "Makefile": "Config",
    "Dockerfile": "Config",
}

_CLOC_IGNORED_PARTS = {
    ".git", ".codex", "build", "dist", "node_modules", "target", "tmp", "vendor",
}

def _repo_git(args: list[str]) -> subprocess.CompletedProcess[str]:
    return run_capture(["git", *args])

def _stats_display_path(path: str) -> str:
    """Keep unusual filenames from breaking the one-line stats display."""
    return json.dumps(path, ensure_ascii=False) if not path.isprintable() else path

def _backlog_stats() -> dict[str, object] | None:
    """Summarize the public contributor work queue."""
    todo = ROOT / "docs" / "TODO.md"
    if not todo.is_file():
        return None
    sections: list[dict[str, object]] = []
    current: dict[str, object] | None = None
    try:
        text = todo.read_text(encoding="utf-8")
    except OSError:
        return None
    for line in text.splitlines():
        if line.startswith("## "):
            current = {"name": line[3:].strip(), "open": 0, "done": 0,
                       "open_titles": []}
            sections.append(current)
        elif current is not None:
            stripped = line.lstrip()
            if stripped.startswith("- [ ]"):
                current["open"] = int(current["open"]) + 1
                title = stripped[5:].strip().replace("**:", ":").replace("**", "")
                if len(current["open_titles"]) < 3:
                    current["open_titles"].append(title)
            elif stripped.startswith("- [x]"):
                current["done"] = int(current["done"]) + 1
    if not sections:
        return None
    return {
        "path": "docs/TODO.md",
        "sections": sections,
        "open": sum(int(s["open"]) for s in sections),
        "done": sum(int(s["done"]) for s in sections),
    }

def collect_repo_stats(include_cloc: bool) -> dict[str, object]:
    # NUL-delimited porcelain keeps filenames with spaces, tabs, newlines, or
    # ` -> ` unambiguous. Rename records carry a second path field.
    status = _repo_git(["status", "--porcelain=v1", "-z", "--branch", "--untracked-files=all"])
    if status.returncode != 0:
        raise SystemExit("stats: git status failed; run this command inside a Git worktree")
    records = status.stdout.split("\0")
    branch_line = records[0] if records and records[0].startswith("## ") else "## HEAD"
    branch = branch_line[3:].split("...", 1)[0]
    changed: list[str] = []
    staged = unstaged = untracked = conflicted = 0
    index = 1 if branch_line.startswith("## ") else 0
    while index < len(records):
        line = records[index]
        index += 1
        if len(line) < 3:
            continue
        x, y = line[0], line[1]
        path = line[3:]
        changed.append(path)
        if "R" in (x, y) or "C" in (x, y):
            # `-z` emits the source path as the following record.
            index += 1
        if x == "?" and y == "?":
            untracked += 1
        else:
            staged += x != " "
            unstaged += y != " "
            conflicted += x == "U" or y == "U"

    head = _repo_git(["log", "-1", "--format=%h%x09%s"])
    commits = _repo_git(["rev-list", "--count", "HEAD"])
    shortstat = _repo_git(["diff", "--shortstat", "HEAD"])
    numstat = _repo_git(["diff", "--numstat", "HEAD"])
    diff_files = diff_added = diff_removed = 0
    for row in numstat.stdout.splitlines():
        columns = row.split("\t", 2)
        if len(columns) != 3:
            continue
        diff_files += 1
        if columns[0].isdigit():
            diff_added += int(columns[0])
        if columns[1].isdigit():
            diff_removed += int(columns[1])

    upstream = _repo_git(["rev-list", "--left-right", "--count", "HEAD...@{upstream}"])
    ahead_behind = None
    if upstream.returncode == 0:
        counts = upstream.stdout.split()
        if len(counts) == 2 and all(item.isdigit() for item in counts):
            ahead_behind = {"ahead": int(counts[0]), "behind": int(counts[1])}

    recent = _repo_git(["log", "-5", "--date=short", "--format=%h%x09%ad%x09%s"])
    report: dict[str, object] = {
        "branch": branch,
        "head": head.stdout.strip().split("\t", 1),
        "commit_count": int(commits.stdout.strip()) if commits.returncode == 0 and commits.stdout.strip().isdigit() else None,
        "working_tree": {
            "changed": len(changed), "staged": staged, "unstaged": unstaged,
            "untracked": untracked, "conflicted": conflicted,
        },
        "diff": {"files": diff_files, "added": diff_added, "removed": diff_removed,
                 "summary": shortstat.stdout.strip()},
        "ahead_behind": ahead_behind,
        "changed_paths": changed[:8],
        "recent_commits": [line.split("\t", 2) for line in recent.stdout.splitlines()],
    }
    report["backlog"] = _backlog_stats()
    if include_cloc:
        listing = _repo_git(["ls-files", "--cached", "--others", "--exclude-standard", "-z"])
        if listing.returncode != 0:
            raise SystemExit("stats: git could not enumerate source files")
        suffix_to_lang = {suffix: lang for lang, suffixes in _CLOC_LANGUAGES.items()
                          for suffix in suffixes}
        counts: dict[str, dict[str, int]] = {}
        source_files: list[dict[str, object]] = []
        seen: set[str] = set()
        for raw_path in listing.stdout.split("\0"):
            if not raw_path or raw_path in seen:
                continue
            seen.add(raw_path)
            relative = Path(raw_path)
            if any(part in _CLOC_IGNORED_PARTS for part in relative.parts):
                continue
            lang = _CLOC_BASENAMES.get(relative.name)
            if relative.name.startswith("Dockerfile."):
                lang = "Config"
            lang = lang or suffix_to_lang.get(relative.suffix.lower())
            if not lang:
                continue
            try:
                data = (ROOT / relative).read_bytes()
            except OSError:
                continue
            if b"\0" in data[:8192]:
                continue
            lines_count = data.count(b"\n") + bool(data and not data.endswith(b"\n"))
            entry = counts.setdefault(lang, {"files": 0, "lines": 0})
            entry["files"] += 1
            entry["lines"] += int(lines_count)
            source_files.append({
                "path": relative.as_posix(), "language": lang,
                "lines": int(lines_count),
            })
        report["cloc"] = {
            "files": sum(item["files"] for item in counts.values()),
            "lines": sum(item["lines"] for item in counts.values()),
            "languages": counts,
            "largest_files": sorted(source_files,
                                    key=lambda item: (-int(item["lines"]), str(item["path"])))[:8],
        }
    return report

def print_repo_stats(report: dict[str, object], include_cloc: bool) -> None:
    dot = UI.glyph("·", "|")
    UI.header("repository stats", f"Git state {dot} change summary" +
              (f" {dot} source lines" if include_cloc else ""))
    UI.section("Git")
    UI.kv("branch", report["branch"], code="1;36")
    head = report["head"]
    UI.kv("HEAD", f"{head[0]} {dot} {head[1]}" if isinstance(head, list) and len(head) > 1 else "unavailable")
    commits = report["commit_count"]
    UI.kv("history", f"{commits:,} commits" if isinstance(commits, int) else "unavailable")
    tree = report["working_tree"]
    UI.kv("worktree", f"{tree['changed']:,} changed paths {dot} {tree['conflicted']:,} conflicts")
    staged_text = c("1;32", f"{tree['staged']:,}")
    unstaged_text = c("1;33", f"{tree['unstaged']:,}")
    untracked_text = c("1;36", f"{tree['untracked']:,}")
    UI.kv("state", f"{staged_text} staged {dot} {unstaged_text} unstaged {dot} "
          f"{untracked_text} untracked")
    diff = report["diff"]
    net = diff["added"] - diff["removed"]
    net_text = f"net {net:+,}" if net else "net 0"
    added_text = c("1;32", f"+{diff['added']:,}")
    removed_text = c("1;31", f"-{diff['removed']:,}")
    UI.kv("diff vs HEAD", f"{diff['files']:,} files {dot} "
          f"{added_text} / {removed_text} lines {dot} {net_text}")
    tracking = report["ahead_behind"]
    if isinstance(tracking, dict):
        ahead_text = c("1;32", f"{UI.glyph('↑', '^')} {tracking['ahead']}")
        behind_text = c("1;33", f"{UI.glyph('↓', 'v')} {tracking['behind']}")
        UI.kv("upstream", f"{ahead_text} ahead {dot} {behind_text} behind")
    paths = report["changed_paths"]
    if paths:
        more = f" {dot} ... +{tree['changed'] - len(paths)} more" if tree["changed"] > len(paths) else ""
        UI.kv("changed paths", f" {dot} ".join(_stats_display_path(path) for path in paths) + more)

    commits_list = report["recent_commits"]
    if commits_list:
        UI.section("Recent commits")
        for commit in commits_list:
            if len(commit) == 3:
                prefix = f"  {c('95', commit[0])}  {c('90', commit[1])}  "
                UI.emit_wrapped(prefix, commit[2])

    backlog = report.get("backlog")
    if isinstance(backlog, dict):
        UI.section("Backlog")
        UI.kv(str(backlog.get("path", "TODO.md")),
              f"{backlog['open']} open {dot} {backlog['done']} done")
        for entry in backlog["sections"]:
            opens = int(entry["open"])
            if not opens:
                continue
            name = str(entry["name"])
            done = int(entry["done"])
            count_text = c("1;33", f"{opens} open") + (f" {dot} " + c("1;32", f"{done} done") if done else "")
            UI.kv(name, count_text)
            for title in entry["open_titles"]:
                UI.emit_wrapped("      " + c("2", "· "), title)

    if include_cloc:
        cloc = report["cloc"]
        UI.section("Source lines")
        UI.kv("inventory", f"{cloc['files']:,} files {dot} {cloc['lines']:,} physical lines")
        languages = cloc["languages"]
        ordered = sorted(languages.items(), key=lambda row: row[1]["lines"], reverse=True)
        max_lines = max((counts["lines"] for _, counts in ordered), default=0)
        bar_width = min(18, max(8, (UI.width() - 66)))
        bar_colors = ("1;36", "1;32", "1;34", "1;35", "1;33", "1;96", "1;92", "1;94")
        for index, (language, counts) in enumerate(ordered):
            share = counts["lines"] / cloc["lines"] if cloc["lines"] else 0.0
            filled = int(bar_width * counts["lines"] / max_lines) if max_lines else 0
            color = bar_colors[index % len(bar_colors)]
            bar = UI.paint(color, "█" * filled) + UI.paint("2", "░" * (bar_width - filled))
            if not UI.supports_glyph("█"):
                bar = UI.paint(color, "#" * filled) + UI.paint("2", "." * (bar_width - filled))
            UI.kv(language, f"{bar}  {counts['lines']:>9,} lines  "
                  f"{counts['files']:>4,} files  {share:>5.1%}")
        largest_files = cloc["largest_files"]
        if largest_files:
            UI.section("Largest source files")
            for item in largest_files:
                line_count = c("1;36", format(item["lines"], ",").rjust(9))
                language = c("2", str(item["language"]))
                prefix = f"  {line_count}  {language}  "
                UI.emit_wrapped(prefix, _stats_display_path(str(item["path"])))

def run_stats(args: list[str]) -> int:
    if any(arg in ("-h", "--help") for arg in args):
        print_command_help("stats")
        return 0
    unknown = [arg for arg in args if arg not in ("--cloc", "--no-cloc", "--json")]
    if unknown:
        raise SystemExit("stats: unknown option(s): " + " ".join(unknown))
    if "--cloc" in args and "--no-cloc" in args:
        raise SystemExit("stats: choose either --cloc or --no-cloc")
    include_cloc = "--no-cloc" not in args
    report = collect_repo_stats(include_cloc)
    if "--json" in args:
        print(json.dumps(report, indent=2, ensure_ascii=False))
    else:
        print_repo_stats(report, include_cloc)
    return 0

def run_optcheck(build_root: Path, kind: str, args: list[str]) -> int:
    """Run native optimization correctness nshape tests."""
    if args and args[0] in ("-h", "--help"):
        print("Usage: ./make optcheck")
        print("  Runs optimizer nshape tests from etc/tests/code/intermediate/opt/")
        print("  Add new kernels there as .nshape files with --native-result-oracle.")
        return 0
    if args:
        raise SystemExit("optcheck: no arguments are supported")
    test_bin = resolve_tool_bin(build_root, kind, "ny-test")
    env = _vendor_env(build_root)
    env.setdefault("NYTRIX_STD_CACHE", "0")
    env.setdefault("NYTRIX_STD_BC_CACHE_AUTO", "0")
    opt_dir = ROOT / "etc" / "tests" / "native" / "optimizer" / "optcheck"
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

def parse(argv: list[str]) -> tuple[list[str], list[str], int, bool, bool, bool, bool, str | None, bool | None, str | None]:
    known = {"all", "bin", "bin-static", "tar", "vendor", "fmt", "std", "std_bc", "core", "core_bc", "test", "repl", "fuzz", "bench", "docs", "web", "web-demos", "web-check", "web-test", "wasm", "c2ny", "py2ny", "install", "uninstall", "clean", "debug", "tidy", "audit", "perf", "profile", "gprof", "asan", "ubsan", "coverage", "optcheck", "analyze", "check", "hooks", "fb", "ny", "run", "release", "static", "deps", "cross", "cross-run", "env", "targets", "doctor", "bench-compare", "stats"}
    _PASSTHROUGH = {"fmt", "analyze", "tidy", "audit", "test", "perf", "profile", "docs", "web", "web-demos", "web-check", "web-test", "wasm", "ny", "repl", "gprof", "asan", "ubsan", "fuzz", "bench", "cross", "cross-run", "static", "bin-static", "tar", "vendor", "env", "targets", "doctor", "stats"}

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
    verbose = 0
    help_flag = False
    help_target: str | None = None
    version = False
    color_mode: str | None = None
    glyph_mode: str | None = None
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
            if i + 1 < len(argv) and argv[i + 1] in known:
                help_flag = True
                help_target = argv[i + 1]
                i += 1
            elif last_cmd and last_cmd in _PASSTHROUGH:
                extra.append(a)
            else:
                help_flag = True
                if last_cmd:
                    help_target = last_cmd
        elif a == "--version":
            version = True
        elif a in ("-v", "--verbose") or (
                a.startswith("-v") and a != "-v"
                and set(a[1:]) <= set("v")):
            if source_passthrough:
                extra.append(a)
            else:
                verbose += 1
        elif a == "--bootstrap-logs":
            bootstrap_logs = True
        elif a == "--no-bootstrap-logs":
            bootstrap_logs = False
        elif a == "--no-color":
            color_mode = "never"
        elif a == "--glyphs" or a.startswith("--glyphs="):
            if a.startswith("--glyphs="):
                v = a.split("=", 1)[1]
            else:
                i += 1
                if i >= len(argv):
                    raise SystemExit("make: missing value for --glyphs")
                v = argv[i]
            glyph_mode = parse_glyph_mode(v)
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
    return cmds, extra, jobs, verbose, help_flag, help_target, version, kind_debug, color_mode, bootstrap_logs, glyph_mode

# Per-command help registry. Each entry: (usage, summary, sections) where
# sections is a list of (heading, rows) and rows are (left, right) text pairs.
_COMMAND_USAGE: dict[str, tuple[str, str, list[tuple[str, list[tuple[str, str]]]]]] = {
    "stats": (
        "Usage: ./make stats [--cloc | --no-cloc] [--json]",
        "summarize Git history, worktree changes, and repository source size",
        [
            ("Report", [
                ("Git", "branch, HEAD, commit count, staged/unstaged/untracked files, diff size, and recent commits"),
                ("--cloc", "show per-language and largest-file physical source-line counts (default)"),
                ("--no-cloc", "skip source counting for a faster Git-only summary"),
                ("--json", "emit the same repository facts as machine-readable JSON"),
            ]),
            ("Examples", [
                ("./make stats", "show Git, worktree, and source-line summary"),
                ("./make stats --no-cloc", "show a faster Git-only summary"),
                ("./make stats --cloc --json", "emit all facts as JSON"),
            ]),
        ],
    ),
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
                ("-v, --verbose", "subcommands; stack it: -vv toolchain · -vvv live build stats ·"
                      " -vvvv compile lines · -vvvvv raw + env"),
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
                ("-v, --verbose", "subcommands; stack it: -vv toolchain · -vvv live build stats ·"
                      " -vvvv compile lines · -vvvvv raw + env"),
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
                ("Bundle", "copies the auto-detected LLVM/runtime shared libraries beside the tools"),
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
        "Usage: ./make check [test options] [--no-test-suite]",
        "run the fail-closed repository quality gate (hygiene, tidy, format, audit, build, and tests)",
        [
            ("What it does", [
                ("1", "reject NUL bytes and temporary print-debugging probes"),
                ("2", "run ny-fmt --tidy"),
                ("3", "verify formatting and parser checks"),
                ("4", "run the source audit"),
                ("5", "build the tools required by the selected check mode"),
                ("6", "run ./make test --with-stdlib --with-replay --with-bench --cold; any failure stops the command"),
                ("--no-test-suite", "skip the full suite for fast build-and-source checks, such as CI"),
                ("Report", "prints every fixture result; full phase logs go to build/check-reports/<run>"),
            ]),
            ("Environment", [
                ("NYTRIX_CHECK_COMPACT=1", "show only failing fixture rows in the terminal"),
                ("NYTRIX_CHECK_REPORT_DIR", "override the full quality-report directory"),
            ]),
            ("Examples", [
                ("./make check", "run the complete cold gate with stdlib tests, failure replay, and benchmarks"),
                ("./make check --no-test-suite", "run hygiene, format, audit, and build checks only"),
                ("NYTRIX_CHECK_COMPACT=1 ./make check", "show only failing fixture rows"),
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
        "run the Nytrix test suite; auxiliary checks and timed benchmarks are opt-in",
        [
            ("What it does", [
                ("Suite", "runs ny-test over etc/tests/lang, etc/tests/code, etc/tests/bench, and lib"),
                ("Bench", "timed C/Ny benchmarks require --with-bench (or use ./make bench)"),
                ("Preflight", "backend scaling, semantic artifact, and legacy expansion checks require --with-preflight"),
                ("Native", "native-only sweep is opt-in: NYTRIX_TEST_RUNTIME_NATIVE=1 or NYTRIX_TEST_STD_NATIVE=1"),
                ("Shapes", "unfiltered runs validate-shapes over etc/tests/code/probes; focused --pattern runs skip the full audit"),
            ]),
            ("ny-test options", [
                ("--cold / --no-cold", "rerun fixtures with result/native/JIT/AOT caches off; keep reusable stdlib compile cache (set NYTRIX_TEST_STD_CACHE=0 to disable)"),
                ("--with-bench", "append the timed C/Ny performance suite to an unfiltered test run"),
                ("--with-preflight", "run auxiliary backend scaling, semantic artifact, and legacy expansion checks"),
                ("--no-bench", "compatibility alias: keep the functional suite free of timed benchmarks"),
                ("-h, --help", "show ny-test help only (never runs the suite, shapes audit, or benchmarks)"),
                ("--list-bench", "list bench fixture stems (discovery)"),
                ("--bench", "run the C-vs-Ny benchmark suite instead of the test suite"),
                ("--bench-run N", "timed samples per benchmark (default 3)"),
                ("--bench-warmup N", "warm-up runs per benchmark (default 1)"),
                ("--bench-opt LEVEL", "compiler opt flag, e.g. O2 (default)"),
                ("--bench-target T", "native backend target (default x86_64)"),
                ("--bench-out-csv F", "write benchmark results to CSV"),
                ("--bench-out-json F", "write benchmark results to JSON"),
                ("--bench-out-md F", "write benchmark results to Markdown"),
                ("--smoke / --no-smoke", "restrict/allow the full suite"),
                ("--with-stdlib / --no-stdlib", "include lib/ fixtures"),
                ("--jobs N", "parallel fixture jobs"),
                (
                    "--fail-fast",
                    "stop queueing fixtures after the first failure; keep the configured parallel jobs",
                ),
                ("--timeout SEC", "per-fixture timeout cap (default 60s)"),
                ("--bench-timeout SEC", "benchmark fixture cap (default 30s)"),
                ("--pattern PAT", "run only fixtures whose path contains PAT"),
                ("--phase-times", "report per-phase timing"),
                ("--trace-ir", "trace NYIR through fixtures"),
                ("--failures-only", "only print failing fixtures"),
                ("--debug-failures", "replay failures with trace, IR, assembly, and debugger artifacts"),
                ("--with-replay", "show failure replays and diagnostic artifacts at the end of the run"),
                ("--debugger-all", "run GDB/LLDB replay for every failed fixture, not only signal crashes"),
            ]),
            ("Env overrides", [
                ("NYTRIX_TEST_COLD=1", "cold fixture execution (same as --cold)"),
                ("NYTRIX_TEST_CACHE=0", "disable result caching without disabling standard-source caches"),
                ("NYTRIX_TEST_EXEC_CACHE=0", "disable execution caches during test (enabled by default; --cold disables them)"),
                ("NYTRIX_TEST_TIMEOUT=NNN", "default per-fixture timeout (60s)"),
                ("NYTRIX_TEST_JOBS=N", "automatic fixture workers; default is bounded by CPU and available RAM"),
                ("NYTRIX_TEST_BENCH_TIMEOUT=NNN", "override benchmark fixture timeout"),
                ("NYTRIX_TEST_DIAG_DIR", "failure artifact root (default build/diagnostics/tests)"),
                ("NYTRIX_TEST_DEBUG_FAILURES=0", "disable automatic failure replay artifacts"),
                ("NYTRIX_TEST_DEBUGGER_ALL=0", "disable debugger replay while keeping trace/asm dumps"),
                ("NYTRIX_TEST_DEBUG_VERBOSE=1", "opt into full debugger maps/disassembly; compact is the default"),
            ]),
            ("Examples", [
                ("./make test", "run the whole suite"),
                ("./make test --cold", "rerun all fixtures without result or execution caches"),
                ("./make test --cold --with-stdlib --no-bench", "cold functional suite including stdlib; skip timed performance benchmarks"),
                ("./make test --with-preflight", "run auxiliary backend, semantic artifact, and legacy expansion checks"),
                (
                    "./make test --with-stdlib --fail-fast",
                    "run the stdlib suite serially; stop at the first failure and reuse valid cached passes",
                ),
                ("./make test --pattern dict", "run only the matching fixtures; skip full-tree audits and benchmarks"),
                ("./make test etc/tests/code/abi/calls", "run only that folder; skip full-tree audits and benchmarks"),
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
                ("--bench-run N", "timed samples per fixture (default 3)"),
                ("--bench-warmup N", "warm-up runs before timing (default 1)"),
                ("--bench-opt LEVEL", "compiler opt level 0..3 (default 2)"),
                ("--bench-target T", "restrict to a sub-target of fixtures"),
                ("--bench-compile-profile P", "compiler opt profile (default/speed/balanced/peak); peak enables the proven fast int paths (~100x on int loops)"),
                ("--bench-show-ir", "dump the optimized NYIR for each fixture"),
                ("--bench-show-asm", "dump x86-64 asm for each fixture"),
                ("--bench-show-passes", "show per-pass NYIR transformation stats"),
                ("--bench-profile", "print per-phase compile timing (JIT profile)"),
                ("--bench-ny-verbose", "pass -vvvvvv to diagnostic compiler invocations for selected fixtures"),
                ("--bench-code-metrics", "collect code size and static metrics with a diagnostic compile"),
                ("--no-build", "run snapshots of the current benchmark tools without rebuilding"),
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
                ("validate-shapes DIR", "validate the deterministic test shapes (default: etc/tests/code/probes)"),
                ("afl", "launch afl-fuzz with the given arguments"),
                ("<args>", "pass straight to ny-test fuzz"),
            ]),
            ("Examples", [
                ("./make fuzz", "validate default shapes"),
                ("./make fuzz validate-shapes etc/tests/code/probes", "validate shapes"),
                ("./make fuzz afl -- -i in -o out -- bin", "launch afl-fuzz"),
            ]),
        ],
    ),
    "asan": ("Usage: ./make asan", "build with Address Sanitizer and run the test suite", []),
    "ubsan": ("Usage: ./make ubsan", "build with Undefined Behavior Sanitizer and run the test suite", []),
    "coverage": (
        "Usage: ./make coverage [file.ny ... | --test [ny-test args]]",
        "build a --coverage instrumented compiler, report peak RSS, and emit lcov HTML into build/coverage/coverage-html. "
        "Default runs the memory-safety smoke; pass --test to run the ny-test suite against the instrumented binary "
        "(e.g. ./make coverage --test --pattern runtime --failures-only). Parallel compile jobs are capped at 4 unless --jobs is given.",
        [],
    ),
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
                ("fuzz [DIR]", "run ny-test fuzz validate-shapes"),
                ("afl --", "launch afl-fuzz with the given args"),
            ]),
        ],
    ),
    "optcheck": (
        "Usage: ./make optcheck",
        "run optimizer nshape tests from etc/tests/code/intermediate/opt/",
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
        "build and install Nytrix into the configured prefix",
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
    "hooks": (
        "Usage: ./make hooks",
        "install the repository-managed Git hooks for this checkout",
        [
            ("What it does", [
                ("Git config", "sets local core.hooksPath to .githooks"),
                ("Portable", "uses Git's POSIX hook runner, including Git Bash on Windows"),
                ("Pre-commit", "checks staged whitespace, source hygiene, and formatting"),
                ("Commit message", "checks Conventional Commit type and a 72-character subject limit"),
                ("Pre-push", "runs the fail-closed ./make check quality gate"),
            ]),
        ],
    ),
}

def _print_help_rows(rows: list[tuple[str, str]], indent: str = "  ") -> None:
    # Keep a stable two-column layout, but reserve enough room for the right
    # column.  A raw max(label) made one long option push every continuation
    # line far to the right and made narrow terminals especially hard to read.
    lefts = [l for l, _ in rows if l]
    terminal = UI.width()
    content = max(1, terminal - len(indent))
    longest = max((len(l) for l in lefts), default=1)
    gap = 2
    width = min(longest, max(18, min(38, content - 24 - gap)))
    description_width = max(18, content - width - gap)
    continuation = indent + " " * (width + gap)
    for left, right in rows:
        if not left:
            print(indent + c("90", right))
            continue
        if len(left) <= width:
            wrapped = textwrap.wrap(
                right, width=description_width, break_long_words=True,
                break_on_hyphens=False,
            ) or [""]
            prefix = f"{indent}{c('36', left)}{' ' * (width - len(left) + gap)}"
            print(prefix + c("2", wrapped[0]))
            for line in wrapped[1:]:
                print(continuation + c("2", line))
        else:
            # Left label itself exceeds the column width: put it on its own line.
            print(f"{indent}{c('36', left)}")
            wrapped = textwrap.wrap(
                right, width=max(18, content - 2), break_long_words=True,
                break_on_hyphens=False,
            ) or [""]
            print(f"{indent}  {c('2', wrapped[0])}")
            for line in wrapped[1:]:
                print(f"{indent}  {c('2', line)}")

def print_command_help(cmd: str) -> None:
    UI.header("help", f"Nytrix build frontend · command {cmd}")
    if cmd not in _COMMAND_USAGE:
        UI.hint(f"./make {cmd}")
        UI.hint("run ./make --help for the full command list")
        return
    usage, summary, sections = _COMMAND_USAGE[cmd]
    if usage.lower().startswith("usage:"):
        UI.emit_wrapped(f"{c('1', 'Usage:')} ", usage[6:].strip())
    else:
        UI.emit_wrapped(f"{c('1', 'Usage:')} ", "./make " + usage)
    UI.hint(summary)
    for title, rows in sections:
        UI.section(title)
        _print_help_rows(rows)
    UI.section("Global options")
    _print_help_rows([
        ("-j, --jobs N", "parallel build jobs"),
        ("-v, --verbose", "subcommands; stack it: -vv toolchain · -vvv live build stats ·"
                      " -vvvv compile lines · -vvvvv raw + env"),
        ("--color MODE", "auto | always | never"),
        ("--glyphs MODE", "unicode | ascii"),
        ("--no-color", "disable colored output"),
        ("--bootstrap-logs", "show bootstrap status"),
        ("--debug", "switch to the debug build configuration when supported"),
        ("-h, --help", "show this command's help"),
    ])
    UI.hint("sub-commands share this driver; run ./make <cmd> --help for focused help")

def print_help() -> None:
    UI.header("build frontend", "One entry point for build, quality, diagnostics, packaging, and portable artifacts.")
    UI.emit_wrapped(f"{c('1', 'Usage:')} ", "./make <command> [options]")
    UI.hint("./make help <command>")
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
            ("core/core_bc", "bundle core library source or bitcode"),
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
        UI.section(title)
        _print_help_rows(rows)
    UI.section("Global options")
    _print_help_rows([
        ("-j, --jobs N", "parallel build jobs"),
        ("-v, --verbose", "subcommands; stack it: -vv toolchain · -vvv live build stats ·"
                      " -vvvv compile lines · -vvvvv raw + env"),
        ("-h, --help", "show this help"),
        ("--version", "print version"),
        ("--color MODE", "auto | always | never"),
        ("--glyphs MODE", "unicode | ascii"),
        ("--no-color", "disable colored output"),
        ("--bootstrap-logs", "show bootstrap status"),
        ("--no-bootstrap-logs", "hide bootstrap status"),
    ])
    UI.section("Environment")
    _print_help_rows([
        ("NYTRIX_GLYPHS", "unicode (default) or ascii; also accepts MAKE_ASCII=1"),
        ("NYTRIX_TOOL_COLOR", "auto, always, or never for the shared native/Python UI"),
        ("NYTRIX_MAKE_BOOTSTRAP_LOGS", "show or hide configure/build bootstrap messages"),
    ])
    UI.section("Ny/runtime passthrough")
    UI.hint("These target the ny command directly:")
    _print_help_rows([
        ("./make ny <file.ny> [flags]", "run a Ny source with compiler flags before/after the file"),
        ("./make <file.ny> [flags]", "shorthand for ./make ny"),
        ("./make -trace ny <file.ny> ...", "pass compiler flags before the source"),
        ("./make ny <file.ny> -v -gl", "pass UI/app flags after the source"),
        ("./make ny <file.ny> -- [args]", "force the rest to be program args"),
    ])
    UI.section("Per-command help")
    UI.hint("run ./make <command> --help for command-specific options and examples")
    UI.section("Examples")
    for example in (
        "./make doctor",
        "./make targets",
        "./make cross aarch64-linux-gnu file.ny",
        "./make ny etc/projects/ui/term.ny -h",
        "./make etc/projects/ui/term.ny -v -vk btop",
        "./make -trace ny etc/projects/ui/engine.ny -vk",
        "./make wasm etc/projects/args.ny --out build/web/wasm/args.wasm",
        "./make bin-static",
        "./make static libs build/static",
        "./make web-demos",
    ):
        UI.emit_wrapped("  ", example)

def _set_env_value(name: str, value: str | None) -> None:
    if value is None:
        os.environ.pop(name, None)
    else:
        os.environ[name] = value

def _append_flags(base: str | None, extra: str) -> str:
    return ((base or "") + " " + extra).strip()

def _strip_dashdash(args: list[str]) -> list[str]:
    while args and args[0] == "--":
        args = args[1:]
    return args

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
    if cmd == "coverage":
        _set_env_value(
            "NYTRIX_HOST_CFLAGS",
            _append_flags(base_host_cflags, "--coverage -O0 -g"),
        )
        _set_env_value(
            "NYTRIX_HOST_LDFLAGS",
            _append_flags(base_host_ldflags, "--coverage"),
        )
        os.environ["NYTRIX_TEST_CACHE"] = "0"
        os.environ["NYTRIX_TEST_COLD"] = "1"
        return "coverage"
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
    for tool in ("llvm-config-23", "llvm-config-22", "llvm-config-21", "llvm-config-20", "llvm-config-19", "llvm-config-18", "llvm-config-17", "llvm-config-16", "llvm-config"):
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

def _elf_soname(path: Path) -> str:
    """Return the ELF DT_SONAME for path, or an empty string."""
    if host_os() != "linux":
        return ""
    readelf = which("readelf")
    if not readelf:
        return ""
    try:
        res = subprocess.run(
            [readelf, "-d", str(path)],
            capture_output=True,
            text=True,
            timeout=10,
        )
    except Exception:
        return ""
    if res.returncode != 0:
        return ""
    for line in res.stdout.splitlines():
        if "(SONAME)" not in line:
            continue
        m = re.search(r"\[(.*?)\]", line)
        if m:
            return m.group(1).strip()
    return ""


def _hardlink_alias(src: Path, dst: Path) -> None:
    """Make dst a hardlink to src. Never create a symlink or duplicate copy."""
    src = Path(src)
    dst = Path(dst)
    if src == dst:
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.is_symlink():
        dst.unlink()
    if dst.exists():
        try:
            if os.path.samefile(src, dst):
                return
        except OSError:
            pass
        dst.unlink()
    try:
        os.link(src, dst)
    except OSError as exc:
        raise SystemExit(
            f"vendor: failed to hardlink {dst.name} to {src.name}: {exc}"
        )


def _copy_one_shared_lib(
    src: Path,
    dst_dir: Path,
    needed_name: str | None = None,
) -> Path | None:
    """Copy one shared-library payload compactly.

    The payload is stored under its ELF SONAME when available. Any additional
    filename required by the loader/linker is a hardlink to that same inode.
    Host distro implementation names such as libz.so.1.3.2 are not retained
    unless they are themselves the SONAME/required name.
    """
    try:
        dst_dir.mkdir(parents=True, exist_ok=True)
        real = src.resolve()
        if not real.exists() or not real.is_file():
            return None

        soname = _elf_soname(real)
        canonical_name = soname or needed_name or src.name or real.name
        canonical = dst_dir / canonical_name

        if canonical.is_symlink():
            canonical.unlink()
        if not canonical.exists():
            shutil.copy2(real, canonical)

        if needed_name and needed_name != canonical_name:
            alias = dst_dir / needed_name
            _hardlink_alias(canonical, alias)
            return alias

        return canonical
    except SystemExit:
        raise
    except Exception as exc:
        log("STATIC", f"failed to copy shared lib {src}: {exc}")
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
            "if [ -f \"$_nytrix_here/src/code/lang/runtime/init.c\" ]; then export NYTRIX_RT_SRC=\"$_nytrix_here/src/code/lang/runtime/init.c\"; fi\n"
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
    """Bundle the transitive dynamic dependency closure of binary_path.

    Libraries use one physical payload per inode/SONAME. Required alternate
    names are hardlinks, never symlinks or duplicated copies.
    """
    copied: list[str] = []
    copy_system = not _env_flag("NYTRIX_BUNDLE_NO_SYSTEM_LIBS", False)
    search_dirs = [target_dir, *_runtime_search_dirs()]
    queue: list[Path] = [binary_path]
    seen_files: set[tuple[int, int]] = set()
    seen_missing: set[str] = set()
    unresolved: set[str] = set()

    while queue:
        cur = queue.pop(0)
        try:
            if not cur.exists() or not cur.is_file():
                continue
            st = cur.stat()
            key = (st.st_dev, st.st_ino)
        except Exception:
            continue
        if key in seen_files:
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

        for name in [*missing, *_elf_needed_names(cur)]:
            if not name or name in seen_missing:
                continue
            seen_missing.add(name)
            if not copy_system and _is_system_runtime_lib(name):
                continue
            dep = _find_shared_lib(name, search_dirs)
            if dep is None:
                unresolved.add(name)
                continue
            dst = _copy_one_shared_lib(dep, target_dir, name)
            if not dst:
                unresolved.add(name)
                continue
            if dst.name not in copied:
                copied.append(dst.name)
            queue.append(dst)
            if target_dir not in search_dirs:
                search_dirs.insert(0, target_dir)

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


def _copytree_replace_preserve_hardlinks(src: Path, dst: Path, ignore=None) -> None:
    """Copy a tree while preserving hardlink groups between regular files."""
    if not src.exists():
        return
    if dst.exists():
        shutil.rmtree(dst, ignore_errors=True)

    inode_map: dict[tuple[int, int], Path] = {}

    def copy_file(src_name, dst_name):
        src_path = Path(src_name)
        dst_path = Path(dst_name)
        st = src_path.stat()
        key = (st.st_dev, st.st_ino)
        previous = inode_map.get(key)
        if previous is not None:
            try:
                os.link(previous, dst_path)
            except OSError as exc:
                raise SystemExit(
                    f"tar: could not preserve hardlink {dst_path.name}: {exc}"
                )
            return str(dst_path)
        shutil.copy2(src_path, dst_path)
        inode_map[key] = dst_path
        return str(dst_path)

    shutil.copytree(
        src,
        dst,
        ignore=ignore,
        symlinks=True,
        copy_function=copy_file,
    )


def _materialize_symlinks_as_hardlinks(root: Path) -> None:
    """Convert file symlinks below root to hardlinks; reject directory links."""
    if not root.exists():
        return
    links = [p for p in root.rglob("*") if p.is_symlink()]
    for link in links:
        try:
            target = link.resolve(strict=True)
        except Exception as exc:
            raise SystemExit(f"tar: broken vendor symlink {link}: {exc}")
        if not target.is_file():
            raise SystemExit(f"tar: vendor directory symlink is forbidden: {link}")
        tmp = link.with_name(link.name + ".hardlink.tmp")
        tmp.unlink(missing_ok=True)
        try:
            os.link(target, tmp)
        except OSError as exc:
            raise SystemExit(
                f"tar: cannot materialize {link.name} as hardlink: {exc}"
            )
        link.unlink()
        tmp.replace(link)

    remaining = [p for p in root.rglob("*") if p.is_symlink()]
    if remaining:
        raise SystemExit(
            "tar: vendor bundle still contains symlinks: "
            + ", ".join(str(p.relative_to(root)) for p in remaining[:20])
        )

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
    canonical = lib_dir / "libedit.so.0"
    tmp.replace(canonical)
    try:
        canonical.chmod(0o755)
    except Exception:
        pass
    _hardlink_alias(canonical, lib_dir / "libedit.so")
    log("VENDOR", "replaced host libedit with compact Nytrix LLVM line-editor shim")
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
    for name in ("core.ny", "core.bc", "core_symbols.h"):
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
            targets = ["ny", "core", "ny-fmt", "ny-perf", "ny-test", "ny-doc", "ny-make", "ny-lsp"]
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

    # `std` is a separate generated target because it invokes the compiler;
    # making `ny` depend on it in CMake would create a build cycle. Once the
    # compiler itself is current, refresh the generated standard library
    # before launching a source program so normal `./make ny file.ny` cannot
    # silently execute a stale library bundle.
    if stdlib_needs_regeneration(build_root, kind):
        old_quiet = QUIET_BOOTSTRAP
        try:
            QUIET_BOOTSTRAP = False
            cmake_build(build_root, kind, ["core"], jobs)
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
    for tool in ("llvm-config-23", "llvm-config-22", "llvm-config-21", "llvm-config-20",
                  "llvm-config-19", "llvm-config-18", "llvm-config-17", "llvm-config-16", "llvm-config"):
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

def run_make_vendor(build_root: Path, kind: str, jobs: int, args: list[str]) -> int:
    if args and args[0] in ("-h", "--help", "help"):
        print("usage: make vendor  -- bundle shared build dependencies")
        print()
        print("  Bundles LLVM/Clang/Z3 shared libraries and headers")
        print("  into build/vendor/ for portable source tarballs.")
        print("  Libraries are compact: one payload per SONAME, required")
        print("  alternate names are hardlinks, and no symlinks are emitted.")
        return 0

    os.chdir(str(ROOT))

    release_dir = build_root / "release"
    tool_names = ("ny", "ny-fmt", "ny-perf", "ny-test", "ny-doc", "ny-make", "ny-lsp")
    build_targets = ["ny", "core", "ny-fmt", "ny-perf", "ny-test", "ny-doc", "ny-make", "ny-lsp"]
    if any(not (release_dir / name).exists() for name in tool_names):
        boot_step("vendor: building release compiler/tools first")
        cmake_build(build_root, "release", build_targets, jobs)

    ny_exe = release_dir / "ny"
    if not ny_exe.exists():
        raise SystemExit(f"vendor: release compiler was not produced: {ny_exe}")

    vendor_dir = build_root / "vendor"
    if vendor_dir.exists():
        shutil.rmtree(vendor_dir, ignore_errors=True)

    lib_dir = _vendor_lib_dir(build_root)
    vendored_bin = vendor_dir / "bin"
    vendored_include = vendor_dir / "include"
    lib_dir.mkdir(parents=True, exist_ok=True)

    # The source package carries compiler/build dependencies, not a private libc.
    os.environ["NYTRIX_BUNDLE_NO_SYSTEM_LIBS"] = "1"

    copied: list[str] = []

    def remember(items: list[str]) -> None:
        for item in items:
            if item not in copied:
                copied.append(item)

    # Runtime closure of every shipped build tool, not just ny.
    for tool_name in tool_names:
        exe = release_dir / tool_name
        if exe.exists() and exe.is_file():
            remember(_copy_static_runtime_libs(lib_dir, exe))

    runtime_llvm_major = _detect_bundled_llvm_major(lib_dir)
    system_path = _system_path_excluding_vendor(build_root)
    llvm_config_candidates: list[str] = []

    def add_llvm_config_candidate(raw: str | None) -> None:
        raw = (raw or "").strip()
        if not raw:
            return
        candidate = Path(raw).expanduser()
        if candidate.is_absolute() or "/" in raw or "\\" in raw:
            found = str(candidate) if candidate.exists() else ""
        else:
            found = which(raw, path=system_path)
        if not found:
            return
        try:
            if _same_path(Path(found), vendored_bin / "llvm-config"):
                return
        except Exception:
            pass
        if found not in llvm_config_candidates:
            llvm_config_candidates.append(found)

    add_llvm_config_candidate(os.environ.get("LLVM_CONFIG"))
    if runtime_llvm_major:
        add_llvm_config_candidate(f"llvm-config-{runtime_llvm_major}")
    add_llvm_config_candidate("llvm-config")
    for major in (22, 21, 20, 19, 18, 17, 16):
        add_llvm_config_candidate(f"llvm-config-{major}")

    llvm_config_bin: str | None = None
    llvm_version = ""
    bundled_llvm_major = runtime_llvm_major or ""
    for candidate in llvm_config_candidates:
        result = run_capture([candidate, "--version"])
        version = result.stdout.strip()
        if result.returncode != 0 or not version:
            continue
        major = version.split(".", 1)[0]
        if runtime_llvm_major and major != runtime_llvm_major:
            continue
        llvm_config_bin = candidate
        llvm_version = version
        bundled_llvm_major = major
        break

    if llvm_config_bin:
        log("VENDOR", f"using system llvm-config: {llvm_config_bin} (LLVM {llvm_version})")
    else:
        log("VENDOR", "no usable system llvm-config found")

    llvm_inc_raw = ""
    llvm_libdir_raw = ""
    llvm_cflags_raw = ""
    llvm_cxxflags_raw = ""
    llvm_libs_raw = ""
    llvm_system_libs_raw = ""
    if llvm_config_bin:
        llvm_inc_raw = run_capture([llvm_config_bin, "--includedir"]).stdout.strip()
        llvm_libdir_raw = run_capture([llvm_config_bin, "--libdir"]).stdout.strip()
        llvm_cflags_raw = run_capture([llvm_config_bin, "--cflags"]).stdout.strip()
        llvm_cxxflags_raw = run_capture([llvm_config_bin, "--cxxflags"]).stdout.strip()
        llvm_libs_raw = run_capture([llvm_config_bin, "--libs", "all"]).stdout.strip()
        llvm_system_libs_raw = run_capture([llvm_config_bin, "--system-libs"]).stdout.strip()

    search_dirs: list[Path] = [lib_dir]
    if llvm_libdir_raw:
        search_dirs.append(Path(llvm_libdir_raw))
    search_dirs.extend(_runtime_search_dirs())

    dedup_dirs: list[Path] = []
    seen_dirs: set[str] = set()
    for directory in search_dirs:
        try:
            key = str(directory.resolve())
        except Exception:
            key = str(directory)
        if key in seen_dirs or not directory.exists() or not directory.is_dir():
            continue
        seen_dirs.add(key)
        dedup_dirs.append(directory)
    search_dirs = dedup_dirs

    seeded_names: set[str] = set()

    def seed_shared(src: Path, needed_name: str | None = None) -> Path | None:
        name = needed_name or src.name
        if not name or _is_system_runtime_lib(name):
            return None
        existing = lib_dir / name
        if existing.exists() and not existing.is_symlink():
            return existing
        if name in seeded_names:
            return existing if existing.exists() else None
        try:
            if not src.exists():
                return None
        except Exception:
            return None
        seeded_names.add(name)
        dst = _copy_one_shared_lib(src, lib_dir, name)
        if dst is None:
            return None
        if dst.name not in copied:
            copied.append(dst.name)
        remember(_copy_static_runtime_libs(lib_dir, dst))
        return dst

    def seed_soname(name: str) -> Path | None:
        if not name or _is_system_runtime_lib(name):
            return None
        existing = lib_dir / name
        if existing.exists() and not existing.is_symlink():
            return existing
        dep = _find_shared_lib(name, search_dirs)
        if dep is None:
            return None
        return seed_shared(dep, name)

    # llvm-config describes the build-time LLVM closure even when ny itself was
    # linked in a way that hides those libraries from ldd(ny).
    llvm_link_flags = " ".join(
        value for value in (llvm_libs_raw, llvm_system_libs_raw) if value
    )
    llvm_link_names: set[str] = set()
    if llvm_link_flags:
        try:
            link_tokens = shlex.split(llvm_link_flags)
        except ValueError:
            link_tokens = llvm_link_flags.split()
        for token in link_tokens:
            if token.startswith("-L") and len(token) > 2:
                directory = Path(token[2:]).expanduser()
                if directory.exists() and directory.is_dir():
                    try:
                        key = str(directory.resolve())
                    except Exception:
                        key = str(directory)
                    if key not in seen_dirs:
                        seen_dirs.add(key)
                        search_dirs.insert(0, directory)
                continue
            if token.startswith("-l") and len(token) > 2:
                soname = "lib" + token[2:] + ".so"
                llvm_link_names.add(soname)
                seed_soname(soname)
                continue
            if ".so" in token:
                p = Path(token)
                if p.is_absolute() and p.exists():
                    seed_shared(p, p.name)

    if bundled_llvm_major and not any(name.startswith("libLLVM") for name in llvm_link_names):
        seed_soname(f"libLLVM-{bundled_llvm_major}.so")

    # Clang-family build libraries are not guaranteed to be listed by llvm-config.
    for name in ("libclang.so", "libclang-cpp.so", "libLTO.so", "libomp.so"):
        seed_soname(name)

    pkg_tool = which("pkg-config") or which("pkgconf")

    def pkg_value(package: str, variable: str) -> str:
        if not pkg_tool:
            return ""
        result = run_capture([pkg_tool, f"--variable={variable}", package])
        if result.returncode != 0:
            return ""
        value = result.stdout.strip()
        return value.splitlines()[0] if value else ""

    def add_pkg_libdir(package: str) -> None:
        raw = pkg_value(package, "libdir")
        if not raw:
            return
        directory = Path(raw)
        if not directory.exists() or not directory.is_dir():
            return
        try:
            key = str(directory.resolve())
        except Exception:
            key = str(directory)
        if key not in seen_dirs:
            seen_dirs.add(key)
            search_dirs.insert(0, directory)

    add_pkg_libdir("z3")
    seed_soname("libz3.so")

    if not bundled_llvm_major:
        bundled_llvm_major = _detect_bundled_llvm_major(lib_dir)
    if bundled_llvm_major:
        log("VENDOR", f"bundled LLVM major: {bundled_llvm_major}")

    z3_include_candidates: list[Path] = []
    z3_pkg_include = pkg_value("z3", "includedir")
    if z3_pkg_include:
        z3_include_candidates.append(Path(z3_pkg_include))
    z3_include_candidates += [Path("/usr/include"), Path("/usr/local/include")]
    z3_header = next(
        (d / "z3.h" for d in z3_include_candidates if (d / "z3.h").exists()),
        None,
    )
    have_z3 = any(p.name.startswith("libz3.so") for p in lib_dir.glob("libz3.so*"))
    if z3_header is not None and have_z3:
        vendored_include.mkdir(parents=True, exist_ok=True)
        count = 0
        for header in sorted(z3_header.parent.glob("z3*.h")):
            _copy2_if_different(header, vendored_include / header.name)
            count += 1
        log("VENDOR", f"bundled {count} Z3 public headers")
    elif have_z3:
        log("VENDOR", "libz3 bundled but z3.h was not found")

    if llvm_config_bin and bundled_llvm_major and llvm_inc_raw:
        boot_step("vendor: bundling LLVM/Clang headers")
        llvm_include = Path(llvm_inc_raw)
        for sub in ("llvm-c", "clang-c", "llvm", "clang"):
            src = llvm_include / sub
            if src.exists():
                _copytree_replace(src, vendored_include / sub)

        clang_builtin_candidates = [
            llvm_include.parent / "lib" / "clang" / bundled_llvm_major / "include",
            Path("/usr/lib") / "clang" / bundled_llvm_major / "include",
            Path("/usr/lib64") / "clang" / bundled_llvm_major / "include",
            Path("/usr/local/lib") / "clang" / bundled_llvm_major / "include",
        ]
        if llvm_libdir_raw:
            clang_builtin_candidates.append(
                Path(llvm_libdir_raw) / "clang" / bundled_llvm_major / "include"
            )
        for candidate in clang_builtin_candidates:
            if candidate.exists() and candidate.is_dir() and any(candidate.iterdir()):
                _copytree_replace(candidate, vendored_include / "clang-builtins")
                break

        vendored_bin.mkdir(parents=True, exist_ok=True)
        config_script = (
            "#!/usr/bin/env sh\n"
            "set -eu\n"
            'here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/..\n'
            f"llvm_version={shlex.quote(llvm_version)}\n"
            f"llvm_cflags={shlex.quote(llvm_cflags_raw)}\n"
            f"llvm_cxxflags={shlex.quote(llvm_cxxflags_raw)}\n"
            f"llvm_libs={shlex.quote(llvm_libs_raw)}\n"
            f"llvm_system_libs={shlex.quote(llvm_system_libs_raw)}\n"
            'out=""\n'
            'emit() { if [ -n "$out" ]; then out="$out $1"; else out="$1"; fi; }\n'
            'for arg in "$@"; do\n'
            '  case "$arg" in\n'
            '    --version) echo "$llvm_version"; exit 0 ;;\n'
            '    --prefix) echo "$here"; exit 0 ;;\n'
            '    --includedir) echo "$here/include"; exit 0 ;;\n'
            '    --libdir) echo "$here/lib/host"; exit 0 ;;\n'
            '    --cflags) printf \'%s\\n\' "$llvm_cflags" | sed "s|-I[^ ]*|-I$here/include|g"; exit 0 ;;\n'
            '    --cxxflags) printf \'%s\\n\' "$llvm_cxxflags" | sed "s|-I[^ ]*|-I$here/include|g"; exit 0 ;;\n'
            '    --ldflags) emit "-L$here/lib/host -Wl,-rpath-link,$here/lib/host" ;;\n'
            '    --libs) emit "$llvm_libs" ;;\n'
            '    --system-libs) emit "$llvm_system_libs" ;;\n'
            '    --shared-mode) echo shared; exit 0 ;;\n'
            '    --link-shared|--link-static|all) : ;;\n'
            '  esac\n'
            'done\n'
            'if [ -n "$out" ]; then printf \'%s\\n\' "$out"; exit 0; fi\n'
            'exit 1\n'
        )
        config_path = vendored_bin / "llvm-config"
        config_path.write_text(config_script, encoding="utf-8")
        config_path.chmod(0o755)
        log("VENDOR", f"vendored llvm-config ({llvm_version}) + LLVM/Clang headers")
    else:
        log("VENDOR", "no matching llvm-config; LLVM/Clang headers not bundled")

    _check_static_bundle_glibc_floor(lib_dir, "vendor")

    symlinks = [p for p in vendor_dir.rglob("*") if p.is_symlink()]
    if symlinks:
        raise SystemExit(
            "vendor: symlinks are forbidden: "
            + ", ".join(str(p.relative_to(vendor_dir)) for p in symlinks[:20])
        )

    library_paths = [
        p for p in lib_dir.glob("*.so*") if p.is_file() and not p.is_symlink()
    ]
    inode_groups: dict[tuple[int, int], list[str]] = {}
    for p in library_paths:
        st = p.stat()
        inode_groups.setdefault((st.st_dev, st.st_ino), []).append(p.name)

    log(
        "VENDOR",
        f"{len(inode_groups)} unique shared-library payloads, "
        f"{len(library_paths)} required filenames",
    )
    for names in sorted(inode_groups.values(), key=lambda xs: sorted(xs)[0]):
        if len(names) > 1:
            log("VENDOR", "  hardlink names: " + " = ".join(sorted(names)))

    env_path = vendor_dir / "env.sh"
    env_lines = [
        "#!/usr/bin/env sh",
        "# Nytrix vendored compiler dependencies.",
        '_nytrix_here=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE:-$0}")" && pwd)',
        'export LD_LIBRARY_PATH="$_nytrix_here/lib/host${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"',
        'export LIBRARY_PATH="$_nytrix_here/lib/host${LIBRARY_PATH:+:$LIBRARY_PATH}"',
        'export CPATH="$_nytrix_here/include${CPATH:+:$CPATH}"',
    ]
    if (vendored_bin / "llvm-config").exists():
        env_lines += [
            'export PATH="$_nytrix_here/bin${PATH:+:$PATH}"',
            'export LLVM_CONFIG="$_nytrix_here/bin/llvm-config"',
            'export NYTRIX_LLVM_INCLUDE="$_nytrix_here/include"',
        ]
    env_lines.append("echo 'Nytrix vendor libs loaded'")
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
        print("    source + docs + compact vendored LLVM/Clang/Z3 build deps")
        print("  --with-binaries: creates build/dist/nytrix-static.tar.gz")
        print("    also includes build/static/ with portable binaries/runtime libs")
        print("  Vendor libraries contain no symlinks and preserve hardlink deduplication.")
        print("  NYTRIX_GLIBC_FLOOR=2.38 controls the portability floor for packaged libs.")
        print("  NYTRIX_TAR_WITH_BINARIES=1 makes --with-binaries the default.")
        return 0

    dist_dir = build_root / "dist"
    package_name = "nytrix-static" if with_binaries else "nytrix-source"
    package_dir = dist_dir / package_name
    shutil.rmtree(package_dir, ignore_errors=True)
    package_dir.mkdir(parents=True, exist_ok=True)

    # Always regenerate vendor: a partially populated/stale vendor directory must
    # never make tar silently skip dependency discovery.
    boot_step("tar: refreshing compact vendored build dependencies")
    rc = run_make_vendor(build_root, kind, jobs, [])
    if rc != 0:
        return rc

    package_vendor = package_dir / "build" / "vendor"
    _copytree_replace_preserve_hardlinks(build_root / "vendor", package_vendor)
    package_vendor_lib = package_vendor / "lib" / "host"
    _repair_tar_vendor_glibc_floor(package_vendor_lib)

    # Compatibility repair must not reintroduce symlinks.
    _materialize_symlinks_as_hardlinks(package_vendor)

    if with_binaries:
        rc = run_make_static(build_root, kind, jobs, ["bin"])
        if rc != 0:
            return rc
        _copytree_replace(build_root / "static", package_dir / "build" / "static")

    # Source tree. Do not package .git history; it is large and not needed to build.
    for name in ("src", "lib", "etc", "docs", ".github", ".githooks"):
        _copytree_replace(ROOT / name, package_dir / name, ignore=_tar_source_ignore)
    for name in (
        "make", "CMakeLists.txt", ".clangd", "README.md", "LICENSE", ".editorconfig"
    ):
        _copy_release_file(ROOT / name, package_dir / name)

    # Fail closed if the vendor subtree contains any symbolic links.
    vendor_links = [p for p in package_vendor.rglob("*") if p.is_symlink()]
    if vendor_links:
        raise SystemExit(
            "tar: refusing vendor subtree containing symlinks: "
            + ", ".join(str(p.relative_to(package_vendor)) for p in vendor_links[:20])
        )

    archive_base = dist_dir / package_name
    tar_path = _make_tar_gz_fast(archive_base, dist_dir, package_name)
    ok(f"tar ready: {_rel_or_abs(tar_path)}")
    return 0

def main() -> int:
    global COLOR, QUIET_BOOTSTRAP
    (cmds, extra, requested_jobs, verbose, want_help, help_target, want_version,
     debug_kind, cli_color_mode, cli_bootstrap_logs, cli_glyph_mode) = parse(sys.argv[1:])
    COLOR = apply_cli_color_mode(cli_color_mode)
    UI.color = COLOR
    UI.glyph_style = apply_cli_glyph_mode(cli_glyph_mode)
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
    inspect_cmds = {"env", "targets", "doctor", "hooks", "stats"}
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
        os.environ["NYTRIX_MAKE_VERBOSE"] = str(verbose)
    else:
        os.environ.pop("NYTRIX_MAKE_VERBOSE", None)
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
        if cmd == "stats":
            rc = run_stats(extra)
            if rc != 0:
                return rc
            continue
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
        if cmd == "hooks":
            hook_dir = ROOT / ".githooks"
            hook_names = ("pre-commit", "commit-msg", "pre-merge-commit",
                          "pre-applypatch", "pre-push")
            hooks = [hook_dir / name for name in hook_names]
            missing = [str(hook.relative_to(ROOT)) for hook in hooks if not hook.is_file()]
            if missing:
                raise SystemExit("make hooks: missing " + ", ".join(missing))
            if host_os() != "windows":
                for hook in hooks:
                    chmod_executable(hook)
            rc = subprocess.run(
                ["git", "config", "--local", "core.hooksPath", ".githooks"],
                cwd=str(ROOT), env=os.environ,
            ).returncode
            if rc != 0:
                return rc
            ok("Git hooks installed: core.hooksPath=.githooks")
            continue
        if cmd == "clean":
            shutil.rmtree(build_root, ignore_errors=True)
            log("CLEAN", f"removed {build_root}")
            continue
        if cmd == "deps":
            continue

        targets = ["ny"]
        if cmd in ("all", "bin"):
            targets = ["ny", "core", "ny-fmt", "ny-perf", "ny-test", "ny-doc", "ny-make", "ny-lsp"]
        elif cmd in ("fmt", "analyze", "tidy", "audit"):
            targets = ["ny-fmt"]
        elif cmd == "check":
            targets = (
                ["ny", "ny-fmt"]
                if "--no-test-suite" in extra
                else ["ny", "ny-full", "ny-test", "ny-fmt"]
            )
        elif cmd in ("test", "asan", "ubsan"):
            targets = ["ny", "ny-full", "ny-test"]
        elif cmd == "fuzz":
            targets = ["ny", "ny-test"]
        elif cmd == "bench":
            targets = ["ny", "ny-test"]
            # `run_test` treats the benchmark suite as a focused run and uses
            # generated core.ny by default. Building core.bc here would spend
            # minutes compiling a shared artifact that the runner then ignores.
            # Keep that optimization available only when explicitly requested.
            shared_core_setting = os.environ.get(
                "NYTRIX_TEST_USE_STD_BC", "auto"
            ).strip().lower()
            if shared_core_setting in ("1", "true", "yes", "on"):
                targets.append("core_bc")
        elif cmd in ("cross", "cross-run"):
            targets = ["ny"]
        elif cmd == "profile":
            targets = ["ny"]
        elif cmd == "docs":
            targets = ["ny", "core", "ny-doc"]
        elif cmd in ("web", "web-demos", "web-check", "web-test", "wasm"):
            targets = ["ny", "core"]
        elif cmd in ("std", "core"):
            targets = ["core"]
        elif cmd in ("std_bc", "core_bc"):
            targets = ["core_bc"]
        elif cmd == "install":
            targets = ["ny", "ny-lsp", "core", "ny-fmt", "ny-perf", "ny-test", "ny-doc", "ny-make"]
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
                if cmd == "bench" and "--no-build" in extra:
                    pass
                elif (cmd != "bench" or "core_bc" in targets or
                      not bench_tools_current(build_root, active_kind)):
                    cmake_build(build_root, active_kind, targets, jobs)
                if cmd == "ny" and stdlib_needs_regeneration(build_root, active_kind):
                    cmake_build(build_root, active_kind, ["core"], jobs)
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
        if cmd in ("bin", "std", "std_bc", "core", "core_bc"):
            continue
        if cmd == "test":
            rc = run_test(build_root, active_kind, requested_jobs, extra)
        elif cmd == "fmt":
            rc = run_tool(build_root, active_kind, "ny-fmt", extra)
        elif cmd == "analyze":
            rc = run_tool(build_root, active_kind, "ny-fmt", ["--analyze", *extra])
        elif cmd == "check":
            # This is the single fail-closed quality gate used by pre-push and
            # documented for contributors. Keep every stage explicit: a clean
            # formatter pass alone must not be mistaken for a tested build.
            report_dir = _check_report_dir()
            try:
                report_label = report_dir.relative_to(ROOT)
            except ValueError:
                report_label = report_dir
            kv("check reports", report_label)
            step("check source hygiene")
            rc = check_source_hygiene()
            if rc == 0:
                rc = run_check_tool(build_root, active_kind, "tidy", ["--tidy"], report_dir)
            if rc == 0:
                rc = run_check_tool(build_root, active_kind, "check", ["--check"], report_dir)
            if rc == 0:
                rc = run_check_tool(build_root, active_kind, "audit", ["--audit"], report_dir)
            if rc == 0:
                if "--no-test-suite" in extra:
                    step("skip full test suite (--no-test-suite)")
                else:
                    # A warm result-cache run is not evidence that changed
                    # compiler code still works. Keep the default gate cold,
                    # include stdlib/replay coverage, and show benchmark
                    # results as part of the same live run.
                    os.environ["NYTRIX_TEST_COLD"] = "1"
                    os.environ.setdefault("NYTRIX_CHECK_COMPACT", "0")
                    check_test_args = [
                        arg for arg in extra if arg != "--no-test-suite"
                    ]
                    if "--with-stdlib" not in check_test_args and "--no-stdlib" not in check_test_args:
                        check_test_args.append("--with-stdlib")
                    if "--with-replay" not in check_test_args:
                        check_test_args.append("--with-replay")
                    if "--with-bench" not in check_test_args and "--no-bench" not in check_test_args:
                        check_test_args.append("--with-bench")
                    if "--cold" not in check_test_args:
                        check_test_args.append("--cold")
                    log(
                        "TEST",
                        "check gate: " + shlex.join(["./make", "test", *check_test_args]),
                    )
                    rc = run_test(
                        build_root, active_kind, requested_jobs, check_test_args
                    )
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
            std_file = str(cmake_build_dir(build_root, active_kind) / "core.ny")
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
            rc = cmake_install(build_root, active_kind, jobs, extra)
        elif cmd == "uninstall":
            manifest = cmake_build_dir(build_root, active_kind) / "install_manifest.txt"
            if not manifest.exists():
                raise SystemExit(f"make: install manifest not found: {manifest}")
            destdir = os.environ.get("DESTDIR", "")
            failed = 0
            removed = 0
            for ln in manifest.read_text(encoding="utf-8", errors="ignore").splitlines():
                p = Path(ln.strip())
                if not p:
                    continue
                if destdir and p.is_absolute():
                    p = Path(destdir) / p.relative_to(p.anchor)
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
        elif cmd == "coverage":
            # An instrumented Debug build compiles every TU at -O0 with gcov
            # bookkeeping; cap parallel compile jobs unless the user asked for
            # a specific count so a coverage run cannot saturate host RAM.
            cov_jobs = requested_jobs or min(jobs, 4)
            import resource
            cov_dir = cmake_build_dir(build_root, active_kind)
            suite_mode = bool(extra) and extra[0] == "--test"
            cmake_build(build_root, active_kind,
                        ["ny-full", "ny-test"] if suite_mode else ["ny-full"],
                        cov_jobs)
            # CMake emits ny-full_debug for the instrumented Debug build.
            # Resolve the built target through the shared artifact resolver so
            # coverage works across Debug/Release and platform generators.
            ny_cov_bin = resolve_tool_bin(build_root, active_kind, "ny-full")
            lcov = shutil.which("lcov")
            if lcov:
                subprocess.run([lcov, "--zerocounters", "--directory", str(cov_dir)],
                               cwd=str(ROOT))
            rc = 0
            if suite_mode:
                # Run the real test harness against the instrumented binary;
                # every fixture it executes contributes gcda counters, so one
                # capture afterwards covers the whole selected suite.
                test_args = _strip_dashdash(extra[1:])
                ny_test_bin = resolve_tool_bin(build_root, active_kind, "ny-test")
                before = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
                one = subprocess.run([str(ny_test_bin), "--bin", str(ny_cov_bin),
                                      *test_args], cwd=str(ROOT))
                after = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
                print(f"[coverage] suite: exit={one.returncode} "
                      f"peak child RSS={max(after, before) / 1024:.1f} MiB")
                rc = one.returncode
            else:
                if not extra:
                    before = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
                    ny_test_bin = resolve_tool_bin(build_root, active_kind, "ny-test")
                    one = subprocess.run(
                        [str(ny_test_bin), "--bin", str(ny_cov_bin),
                         "--pattern", "memory-safety-smoke"],
                        cwd=str(ROOT))
                    after = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
                    peak_kb = max(after, before)
                    print(f"[coverage] memory-safety smoke: exit={one.returncode} "
                          f"peak child RSS={peak_kb / 1024:.1f} MiB")
                    if one.returncode != 0:
                        rc = one.returncode
                for smoke in extra:
                    before = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
                    if smoke.endswith(".nshape"):
                        ny_test_bin = resolve_tool_bin(build_root, active_kind, "ny-test")
                        one = subprocess.run([str(ny_test_bin), "--bin", str(ny_cov_bin),
                                              "--pattern", smoke], cwd=str(ROOT))
                    else:
                        one = subprocess.run([str(ny_cov_bin), smoke], cwd=str(ROOT))
                    after = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
                    peak_kb = max(after, before)
                    print(f"[coverage] {smoke}: exit={one.returncode} peak child RSS={peak_kb / 1024:.1f} MiB")
                    if one.returncode != 0:
                        rc = one.returncode
            genhtml = shutil.which("genhtml")
            if lcov and genhtml:
                subprocess.run([lcov, "--capture", "--directory", str(cov_dir),
                                "--output-file", str(cov_dir / "coverage.raw.info")], cwd=str(ROOT))
                subprocess.run([lcov, "--remove", str(cov_dir / "coverage.raw.info"),
                                "/usr/*", "--output-file", str(cov_dir / "coverage.info")], cwd=str(ROOT))
                subprocess.run([genhtml, str(cov_dir / "coverage.info"),
                                "--output-directory", str(cov_dir / "coverage-html")], cwd=str(ROOT))
                print(f"[coverage] HTML report: {cov_dir / 'coverage-html' / 'index.html'}")
            else:
                print("[coverage] lcov/genhtml not found; .gcda files are in the build dir (install lcov for an HTML report)")
            if rc != 0:
                return rc
            return 0
        elif cmd == "fuzz":
            cmake_build(build_root, active_kind, ["ny", "ny-test"], requested_jobs)
            if extra and extra[0] == "afl":
                afl = shutil.which("afl-fuzz")
                if not afl:
                    raise SystemExit("make fuzz afl: afl-fuzz not found")
                afl_args = _strip_dashdash(extra[1:])
                if not afl_args:
                    raise SystemExit("make fuzz afl: pass afl-fuzz args after --")
                rc = subprocess.run([afl, *afl_args], cwd=str(ROOT)).returncode
            elif extra:
                rc = run_tool(build_root, active_kind, "ny-test", ["fuzz", *extra])
            else:
                rc = run_tool(build_root, active_kind, "ny-test", ["fuzz", "validate-shapes", default_fuzz_shape_dir()])
        elif cmd == "bench":
            bench_run_lock = acquire_bench_run_lock(build_root, active_kind)
            bench_extra = [arg for arg in extra if arg not in ("--no-progress", "--no-build")]
            if requested_jobs is not None and requested_jobs > 0:
                bench_extra.extend(["--jobs", str(requested_jobs)])
            elif not any(arg == "--jobs" or arg.startswith("--jobs=") or arg == "-j"
                         for arg in bench_extra):
                # Maximize suite throughput while respecting current free RAM.
                # Parallel fixture timing is noisier than a serial run; use
                # --jobs 1 when collecting publication-quality measurements.
                bench_jobs = resolve_bench_jobs()
                bench_extra.extend(["--jobs", str(bench_jobs)])
            if not os.environ.get("OMP_NUM_THREADS"):
                os.environ["OMP_NUM_THREADS"] = "1"
            if not os.environ.get("OMP_THREAD_LIMIT"):
                os.environ["OMP_THREAD_LIMIT"] = "1"
            explicit_core_bc = any(
                arg in ("--core-bc", "--std-bc")
                or arg.startswith(("--core-bc=", "--std-bc="))
                for arg in bench_extra
            )
            shared_core_enabled = (
                not explicit_core_bc
                and "--no-stdlib" not in bench_extra
                and os.environ.get("NYTRIX_TEST_USE_STD_BC", "auto").strip().lower()
                in ("1", "true", "yes", "on")
            )
            # Pin the compiler and shared core.bc for the whole run. A
            # concurrent `cmake --build` (another make invocation) relinks
            # ny-full and regenerates core.bc in place; without a snapshot the
            # launcher can exec a half-written compiler and individual Ny
            # routes fail nondeterministically, producing phantom FAIL rows.
            ny_bin = resolve_tool_bin(build_root, active_kind, "ny")
            full_bin = resolve_tool_bin(build_root, active_kind, "ny-full")
            trace_dir = _nytrix_cache_root("test-trace") / f"bench-{os.getpid()}"
            shutil.rmtree(trace_dir, ignore_errors=True)
            trace_dir.mkdir(parents=True, exist_ok=True)
            suite_ny_bin = ny_bin.with_name(
                f"{ny_bin.stem}-bench-{os.getpid()}{ny_bin.suffix}"
            )
            suite_full_bin = full_bin.with_name(
                f"{full_bin.stem}-bench-{os.getpid()}{full_bin.suffix}"
            )
            test_bin = resolve_tool_bin(build_root, active_kind, "ny-test")
            suite_test_bin = test_bin.with_name(
                f"{test_bin.stem}-bench-{os.getpid()}{test_bin.suffix}"
            )
            if shared_core_enabled:
                shared_bc = cmake_build_dir(build_root, active_kind) / "core.bc"
                if shared_bc.is_file():
                    core_ny = cmake_build_dir(build_root, active_kind) / "core.ny"
                    core_ny_path = str(core_ny.resolve())
                    for key in ("NYTRIX_STD_PREBUILT", "NYTRIX_BUILD_STD_PATH",
                                "NYTRIX_CORE_PATH", "NYTRIX_STD_PATH"):
                        if os.environ.get(key) == core_ny_path:
                            os.environ.pop(key, None)
                    if _env_flag("NYTRIX_STD_PREFER_PREBUILT", False):
                        os.environ.pop("NYTRIX_STD_PREFER_PREBUILT", None)
                    suite_core_bc = trace_dir / "core.bc"
                    shutil.copy2(shared_bc, suite_core_bc)
                    links = shared_bc.with_name("core.bc.links")
                    if links.is_file():
                        shutil.copy2(links, suite_core_bc.with_name("core.bc.links"))
                    bench_extra.extend(["--core-bc", str(suite_core_bc.resolve())])
            try:
                if full_bin.is_file():
                    copy_stable_tool(full_bin, suite_full_bin)
                    os.environ["NYTRIX_TEST_NY_FULL_BIN"] = str(suite_full_bin)
                copy_stable_tool(ny_bin, suite_ny_bin)
                copy_stable_tool(test_bin, suite_test_bin)
                rc = subprocess.run(
                    [str(suite_test_bin), "--bin", str(suite_ny_bin), "--bench", *bench_extra],
                    cwd=str(ROOT), env=_vendor_env(build_root),
                ).returncode
            finally:
                try:
                    suite_ny_bin.unlink(missing_ok=True)
                    suite_full_bin.unlink(missing_ok=True)
                    suite_test_bin.unlink(missing_ok=True)
                except OSError:
                    pass
                bench_run_lock.close()
        elif cmd == "bench-compare":
            # Multi-run comparative mode: median-of-N native times against a
            # stored baseline with a regression threshold.  A single bench
            # run is a smoke test, not a measurement.
            bench_extra = [arg for arg in extra if arg != "--no-progress"]
            save = "--save-baseline" in bench_extra
            bench_extra = [a for a in bench_extra if a != "--save-baseline"]
            runs = 3
            threshold = 25.0
            rest = []
            for a in bench_extra:
                if a.startswith("--runs="):
                    runs = max(1, int(a.split("=", 1)[1]))
                elif a.startswith("--threshold="):
                    threshold = float(a.split("=", 1)[1])
                else:
                    rest.append(a)
            rc = bench_compare(build_root, active_kind, rest, runs=runs,
                               threshold_pct=threshold, save=save)
        elif cmd == "optcheck":
            rc = run_optcheck(build_root, active_kind, extra)
        elif cmd == "fb":
            raise SystemExit(
                f"make: command '{cmd}' is not implemented on the native C path.\n"
                "  For optimization correctness, use: ./make test\n"
                "  For fuzz/shape validation, use:  ./make fuzz [validate-shapes etc/tests/code/probes]\n"
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
    except BrokenPipeError:
        # Piping reports through tools such as `head` is a normal CLI use.
        # Redirect fd 1 before interpreter shutdown so buffered stdout does
        # not emit a second BrokenPipeError traceback while flushing.
        try:
            devnull = os.open(os.devnull, os.O_WRONLY)
            os.dup2(devnull, sys.stdout.fileno())
            os.close(devnull)
        except OSError:
            pass
        raise SystemExit(0)
