import sys
sys.dont_write_bytecode = True

import argparse
import re
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from context import ROOT
from utils import log, log_ok, err

DEFAULT_PATHS = ("std", "etc/tests")

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

    return "\n".join(compact).rstrip() + "\n"

def _iter_ny_files(paths):
    for raw in paths:
        path = (ROOT / raw).resolve() if not Path(raw).is_absolute() else Path(raw).resolve()
        if not path.exists():
            continue
        if path.is_file():
            if path.suffix == ".ny":
                yield path
            continue
        for file_path in sorted(path.rglob("*.ny")):
            if file_path.is_file():
                yield file_path

def run_nyfmt(paths=None, check=False):
    paths = tuple(paths or DEFAULT_PATHS)
    files = list(dict.fromkeys(_iter_ny_files(paths)))
    if not files:
        err("nyfmt: no .ny files found")
        return 1

    changed = []
    for file_path in files:
        old = file_path.read_text(encoding="utf-8")
        new = format_ny_text(old)
        if new != old:
            changed.append(file_path)
            if not check:
                file_path.write_text(new, encoding="utf-8")

    if check:
        if changed:
            err(f"nyfmt: {len(changed)} file(s) need formatting")
            for path in changed:
                try:
                    rel = path.relative_to(ROOT)
                except ValueError:
                    rel = path
                err(f"  - {rel}")
            return 1
        log_ok(f"nyfmt check passed ({len(files)} files)")
        return 0

    log_ok(f"nyfmt formatted {len(changed)} file(s) across {len(files)} file(s) scanned")
    return 0

def main():
    parser = argparse.ArgumentParser(description="Nytrix .ny formatter")
    parser.add_argument("paths", nargs="*", default=list(DEFAULT_PATHS), help="paths to format (default: std etc/tests)")
    parser.add_argument("--check", action="store_true", help="check only, do not write")
    args = parser.parse_args()

    mode = "check" if args.check else "write"
    log("NYFMT", f"mode={mode} paths={','.join(args.paths)}")
    return run_nyfmt(paths=args.paths, check=args.check)

if __name__ == "__main__":
    raise SystemExit(main())
