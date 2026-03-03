#!/usr/bin/env python3
"""
Nytrix Code Tidy
"""
import sys
sys.dont_write_bytecode = True
import os
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
import re
import subprocess
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))

from context import ROOT
from utils import log, log_ok, which

import json

EXTS = {".c", ".h", ".ny", ".py", ".md"}
TAB = b"   "
CACHE_PATH = ROOT / "build" / "tidy_cache.json"

def chunks(xs, n):
    for i in range(0, len(xs), n):
        yield xs[i:i + n]

SHORT_KEYWORD_CALL_RE = re.compile(r"\b(if|elif|while|for|match)\s+\(")
FN_DEF_RE = re.compile(r"\bfn\s+([A-Za-z_][A-Za-z0-9_]*)\s+\(")
COMPTIME_BLOCK_RE = re.compile(r"\bcomptime\s+\{")
CLOSE_PAREN_OPEN_BRACE_RE = re.compile(r"\)\s+\{")

def _apply_short_forms(text):
    text = SHORT_KEYWORD_CALL_RE.sub(r"\1(", text)
    text = FN_DEF_RE.sub(r"fn \1(", text)
    text = COMPTIME_BLOCK_RE.sub("comptime{", text)
    text = CLOSE_PAREN_OPEN_BRACE_RE.sub("){", text)
    return text

def _split_comment(line):
    quote = ""
    escaped = False
    for idx, ch in enumerate(line):
        if quote:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == quote:
                quote = ""
            continue
        if ch == "'" or ch == '"':
            quote = ch
            continue
        if ch == ";":
            return line[:idx], line[idx:]
    return line, ""

def _rewrite_outside_quotes(code):
    if not code:
        return code
    out = []
    quote = ""
    escaped = False
    seg_start = 0
    for idx, ch in enumerate(code):
        if quote:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == quote:
                quote = ""
            continue
        if ch == "'" or ch == '"':
            out.append(_apply_short_forms(code[seg_start:idx]))
            out.append(ch)
            quote = ch
            seg_start = idx + 1
    if seg_start == 0:
        return _apply_short_forms(code)
    out.append(_apply_short_forms(code[seg_start:]))
    return "".join(out)

def _format_line(line):
    code, comment = _split_comment(line)
    formatted_code = _rewrite_outside_quotes(code).rstrip()
    if not comment:
        return formatted_code
    if formatted_code.strip():
        return formatted_code + " " + comment.lstrip()
    return code + comment.lstrip()

def format_ny_text(text):
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    lines = text.split("\n")
    formatted = [_format_line(line) for line in lines]

    compact = []
    prev_blank = False
    for line in formatted:
        blank = (line.strip() == "")
        if blank and prev_blank:
            continue
        compact.append(line)
        prev_blank = blank

    while compact and compact[0].strip() == "":
        compact.pop(0)

    if not compact:
        return "\n"
    return "\n".join(compact).rstrip() + "\n"


def get_files(dirs):
    # Try git first
    try:
        cmd = ["git", "ls-files", "-z", "--"] + dirs
        out = subprocess.check_output(cmd, cwd=ROOT, stderr=subprocess.DEVNULL)
        fs = [x.decode() for x in out.split(b"\0") if x]
        return [f for f in fs if not f.startswith(".") and "/." not in f]
    except Exception:
        pass

    # Fallback to walk
    fs = []
    for d in dirs:
        dp = ROOT / d
        if not dp.is_dir():
            continue
        for r, ds, ns in os.walk(str(dp)):
            ds[:] = [x for x in ds if not x.startswith(".")]
            for n in ns:
                if n.startswith("."):
                    continue
                p = Path(r) / n
                rel = p.relative_to(ROOT).as_posix()
                if "/." in rel:
                    continue
                fs.append(rel)
    return fs

def normalize_file(path, replace_tabs=True):
    p = ROOT / path
    if not p.exists():
        return 0

    try:
        with open(p, "rb") as f:
            b = f.read()
    except OSError:
        return 0

    nb = b.replace(b"\r", b"")
    if replace_tabs:
        nb = nb.replace(b"\t", TAB)

    # Standardize formatting and indentation for .ny files
    if path.endswith(".ny"):
        try:
            text = nb.decode("utf-8")
        except UnicodeDecodeError:
            return 0

        text = format_ny_text(text)
        lines = text.split("\n")
        new_lines = []
        for line in lines:
            if not line.strip():
                new_lines.append("")
                continue

            # Count leading spaces
            l_spaces = 0
            for c in line:
                if c == " ": # space
                    l_spaces += 1
                else:
                    break

            content = line[l_spaces:]
            # If it's a multiple of 4, convert to multiple of 3
            if l_spaces > 0 and l_spaces % 4 == 0:
                l_spaces = (l_spaces // 4) * 3

            new_lines.append(" " * l_spaces + content)

        nb = "\n".join(new_lines).encode("utf-8")


    # Remove trailing whitespace
    nb = re.sub(rb"[ \t]+(?=\n|\Z)", b"", nb)

    if nb != b:
        with open(p, "wb") as f:
            f.write(nb)
        return 1
    return 0

def load_cache():
    if not CACHE_PATH.exists():
        return {}
    try:
        return json.loads(CACHE_PATH.read_text())
    except Exception:
        return {}

def save_cache(cache):
    CACHE_PATH.parent.mkdir(parents=True, exist_ok=True)
    CACHE_PATH.write_text(json.dumps(cache, indent=2))

def run_tidy(dirs=None):
    if dirs is None:
        dirs = ["src", "lib", "etc/tests"]

    log("TIDY", f"tidying {', '.join(dirs)}")

    fs = get_files(dirs)
    if not fs:
        return

    cache = load_cache()
    new_cache = {}
    eligible_fs = []

    for f in fs:
        p = ROOT / f
        try:
            st = p.stat()
        except OSError:
            continue
        mtime = str(st.st_mtime)
        size = st.st_size

        # Check cache
        if f in cache and cache[f].get("mtime") == mtime and cache[f].get("size") == size:
            new_cache[f] = cache[f]
            continue

        eligible_fs.append(f)
        new_cache[f] = {"mtime": mtime, "size": size}

    if not eligible_fs:
        log_ok("tidy complete (cached)")
        return

    # Run clang-format on C/H files in eligible list
    c_files = [f for f in eligible_fs if f.endswith((".c", ".h"))]
    if c_files and which("clang-format"):
        for batch in chunks(c_files, 256):
            cmd = ["clang-format", "-i"] + [str(ROOT / b) for b in batch]
            subprocess.run(cmd, check=False)

    # Text normalization
    changed = 0
    for f in eligible_fs:
        ext = os.path.splitext(f)[1]
        if ext in EXTS:
            rt = (ext not in (".c", ".h"))
            if normalize_file(f, replace_tabs=rt):
                changed += 1
                # Update cache info after normalization
                st = (ROOT / f).stat()
                new_cache[f] = {"mtime": str(st.st_mtime), "size": st.st_size}

    # Special case for Makefile
    if (ROOT / "Makefile").exists():
        normalize_file("Makefile", replace_tabs=False)

    save_cache(new_cache)
    if changed:
        log_ok(f"tidy complete ({changed} files normalized)")
    else:
        log_ok("tidy complete")
