import sys
sys.dont_write_bytecode = True
import os
import platform
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

def host_os():
    sysname = platform.system()
    if sysname == "Windows":
        return "windows"
    if sysname == "Darwin":
        return "macos"
    if sysname == "Linux":
        return "linux"
    return "unknown"

def host_machine():
    return (platform.machine() or "").lower()

def is_arm_riscv_machine():
    mach = host_machine()
    return ("arm" in mach or "aarch64" in mach or "riscv" in mach)

def enable_windows_utf8_console():
    if platform.system() != "Windows":
        return
    try:
        import ctypes
        k32 = ctypes.windll.kernel32
        k32.SetConsoleCP(65001)
        k32.SetConsoleOutputCP(65001)
        # Enable ANSI escape processing in classic conhost
        h_out = k32.GetStdHandle(-11)  # STD_OUTPUT_HANDLE
        mode = ctypes.c_uint()
        if h_out and k32.GetConsoleMode(h_out, ctypes.byref(mode)):
            k32.SetConsoleMode(h_out, mode.value | 0x0004)  # ENABLE_VIRTUAL_TERMINAL_PROCESSING
        h_err = k32.GetStdHandle(-12)  # STD_ERROR_HANDLE
        mode_err = ctypes.c_uint()
        if h_err and k32.GetConsoleMode(h_err, ctypes.byref(mode_err)):
            k32.SetConsoleMode(h_err, mode_err.value | 0x0004)
    except Exception:
        pass
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

enable_windows_utf8_console()

def supports_glyph(glyph):
    enc = sys.stdout.encoding or "utf-8"
    try:
        glyph.encode(enc)
        return True
    except Exception:
        return False

def ok_symbol():
    mode = (os.environ.get("NYTRIX_UI_SYMBOLS") or "").strip().lower()
    if mode == "ascii":
        return "OK"
    if mode == "utf":
        return "✓"
    # Unified Unix/Windows fallback: check glyph support or default to UTF
    if platform.system() == "Windows":
        return "✓"
    return "✓" if supports_glyph("✓") else "OK"

OK_SYMBOL = ok_symbol()

def color_enabled():
    if os.environ.get("NO_COLOR"):
        return False
    if os.environ.get("FORCE_COLOR") or os.environ.get("CLICOLOR_FORCE"):
        return True
    if not sys.stdout.isatty():
        return False
    term = os.environ.get("TERM", "")
    if term == "dumb":
        return False
    return True

COLOR_ON = color_enabled()

def c(code, s):
    if not COLOR_ON: return s
    # Support nesting: restore the outer color after any inner reset
    inner = str(s).replace("\033[0m", f"\033[0m\033[{code}m")
    return f"\033[{code}m{inner}\033[0m"

def first_unwritable_path(root: Path, max_checks: int = 256):
    if not root.exists():
        return None
    try:
        if not os.access(root, os.W_OK):
            return root
    except OSError:
        return root

    probe_roots = [
        root / "release" / "CMakeFiles",
        root / "debug" / "CMakeFiles",
        root / "CMakeFiles",
        root,
    ]
    seen = set()
    checked = 0
    for base in probe_roots:
        if not base.exists():
            continue
        try:
            key = str(base.resolve())
        except OSError:
            key = str(base)
        if key in seen:
            continue
        seen.add(key)
        try:
            iterator = base.rglob("*")
        except OSError:
            continue
        for p in iterator:
            checked += 1
            if checked > max_checks:
                return None
            try:
                if p.exists() and not os.access(p, os.W_OK):
                    return p
            except OSError:
                return p
    return None

def ensure_dir_writable(path: Path) -> bool:
    try:
        path.mkdir(parents=True, exist_ok=True)
    except OSError:
        return False
    probe = path / ".nytrix_write_probe"
    try:
        probe.write_text("ok", encoding="utf-8")
        probe.unlink()
        return True
    except OSError:
        try:
            probe.unlink()
        except OSError:
            pass
        return False

def resolve_build_dir():
    raw_env = os.environ.get("BUILD_DIR")
    if raw_env:
        env_dir = Path(raw_env).expanduser().resolve()
        # Keep sanitizer builds in the main build tree.
        if env_dir.name in {
            ".asan-build",
            "asan-build",
            ".ubsan-build",
            "ubsan-build",
            ".san-build",
            "san-build",
        }:
            return (env_dir.parent / "build").resolve(), ""
        return env_dir, ""

    default_dir = Path("build").resolve()
    blocked = first_unwritable_path(default_dir)
    if blocked is None and ensure_dir_writable(default_dir):
        return default_dir, ""
    if blocked is None:
        blocked = default_dir

    fallback_dir = (Path.home() / ".cache" / "nytrix-build").resolve()
    if ensure_dir_writable(fallback_dir):
        note = (
            f"default build dir not writable ({blocked}); "
            f"using {fallback_dir}"
        )
        return fallback_dir, note
    return default_dir, ""

BUILD_DIR, BUILD_DIR_NOTICE = resolve_build_dir()
