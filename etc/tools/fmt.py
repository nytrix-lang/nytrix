#!/usr/bin/env python3
"""
Nytrix Code Format & Analysis Tool
"""
from __future__ import annotations
import sys
sys.dont_write_bytecode = True
import os
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
import re
import json
import difflib
import itertools
import subprocess
import concurrent.futures
from pathlib import Path
from collections import Counter, defaultdict
from dataclasses import dataclass, asdict

sys.path.insert(0, str(Path(__file__).resolve().parent))
from context import ROOT
from utils import log, log_ok, which, warn, c, step

EXTS = {".c", ".h", ".ny", ".py", ".md"}
TAB = b"   "
CACHE_PATH = ROOT / "build" / "cache" / "fmt_cache.json"
ANALYSIS_CACHE_PATH = ROOT / "build" / "cache" / "tidy_cache.json"

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
            if escaped: escaped = False
            elif ch == "\\": escaped = True
            elif ch == quote: quote = ""
            continue
        if ch == "'" or ch == '"':
            quote = ch
            continue
        if ch == ";":
            return line[:idx], line[idx:]
    return line, ""

def _rewrite_outside_quotes(code):
    if not code: return code
    out = []
    quote = ""
    escaped = False
    seg_start = 0
    for idx, ch in enumerate(code):
        if quote:
            if escaped: escaped = False
            elif ch == "\\": escaped = True
            elif ch == quote: quote = ""
            continue
        if ch == "'" or ch == '"':
            out.append(_apply_short_forms(code[seg_start:idx]))
            out.append(ch)
            quote = ch
            seg_start = idx + 1
    if seg_start == 0: return _apply_short_forms(code)
    out.append(_apply_short_forms(code[seg_start:]))
    return "".join(out)

def _format_line(line):
    code, comment = _split_comment(line)
    formatted_code = _rewrite_outside_quotes(code).rstrip()
    if not comment: return formatted_code
    if formatted_code.strip(): return formatted_code + " " + comment.lstrip()
    return code + comment.lstrip()

def format_ny_text(text):
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    lines = text.split("\n")
    formatted = [_format_line(line) for line in lines]
    compact = []
    prev_blank = False
    for line in formatted:
        blank = (line.strip() == "")
        if blank and prev_blank: continue
        compact.append(line)
        prev_blank = blank
    while compact and compact[0].strip() == "": compact.pop(0)
    if not compact: return "\n"
    return "\n".join(compact).rstrip() + "\n"

def normalize_file(path, replace_tabs=True):
    p = ROOT / path
    if not p.exists(): return 0
    try:
        with open(p, "rb") as f: b = f.read()
    except OSError: return 0
    nb = b.replace(b"\r", b"")
    if replace_tabs: nb = nb.replace(b"\t", TAB)
    if path.endswith(".ny"):
        try: text = nb.decode("utf-8")
        except UnicodeDecodeError: return 0
        text = format_ny_text(text)
        lines = text.split("\n")
        new_lines = []
        for line in lines:
            if not line.strip():
                new_lines.append("")
                continue
            l_spaces = 0
            for c_ in line:
                if c_ == " ": l_spaces += 1
                else: break
            content = line[l_spaces:]
            if l_spaces > 0 and l_spaces % 4 == 0:
                l_spaces = (l_spaces // 4) * 3
            new_lines.append(" " * l_spaces + content)
        nb = "\n".join(new_lines).encode("utf-8")
    nb = re.sub(rb"[ \t]+(?=\n|\Z)", b"", nb)
    if nb != b:
        with open(p, "wb") as f: f.write(nb)
        return 1
    return 0

def run_fmt(dirs=None):
    if dirs is None: dirs = ["src", "lib", "etc/tests"]
    fs = sorted(set(_iter_files(dirs)))
    if not fs: return
    
    # Cache logic
    cache = {}
    if CACHE_PATH.exists():
        try: cache = json.loads(CACHE_PATH.read_text())
        except: pass
    
    new_cache = {}
    eligible = []
    for f in fs:
        p = ROOT / f
        try: st = p.stat()
        except: continue
        mtime, size = str(st.st_mtime), st.st_size
        if f in cache and cache[f].get("mtime") == mtime and cache[f].get("size") == size:
            new_cache[f] = cache[f]
            continue
        eligible.append(f)
        new_cache[f] = {"mtime": mtime, "size": size}
    
    if not eligible:
        log_ok("fmt complete (cached)")
        return

    log("FMT", f"formatting {len(eligible)} files...")
    
    # clang-format
    c_files = [f for f in eligible if f.endswith((".c", ".h"))]
    if c_files and which("clang-format"):
        for batch in [c_files[i:i + 256] for i in range(0, len(c_files), 256)]:
            subprocess.run(["clang-format", "-i"] + [str(ROOT / b) for b in batch], check=False)
    
    # Custom formatters
    changed = 0
    with concurrent.futures.ProcessPoolExecutor() as executor:
        futures = {executor.submit(normalize_file, f, (os.path.splitext(f)[1] not in (".c", ".h"))): f 
                   for f in eligible if os.path.splitext(f)[1] in EXTS}
        for future in concurrent.futures.as_completed(futures):
            f = futures[future]
            try:
                if future.result():
                    changed += 1
                    st = (ROOT / f).stat()
                    new_cache[f] = {"mtime": str(st.st_mtime), "size": st.st_size}
            except Exception as e: warn(f"fmt failed for {f}: {e}")

    CACHE_PATH.parent.mkdir(parents=True, exist_ok=True)
    CACHE_PATH.write_text(json.dumps(new_cache, indent=2))
    if changed: log_ok(f"fmt complete ({changed} files updated)")
    else: log_ok("fmt complete")

# --- ANALYSIS (Docstrings + Repetitions) ---

FN_RE = re.compile(r"^\s*fn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(")
TOKEN_RE = re.compile(r'"(?:\\.|[^"])*"|[A-Za-z_][A-Za-z0-9_]*|\d+\.\d+|\d+|==|!=|<=|>=|->|\.\.\.|[{}()\[\],.:;+\-*/%&|^!<>=?]')

@dataclass(frozen=True)
class FunctionDef:
    name: str
    path: str
    start_line: int
    end_line: int
    lines: int
    text: str
    tokens: tuple[str, ...]
    exact_norm: str
    fuzzy_norm: str

def _iter_files(paths):
    for raw in paths:
        p = ROOT / raw
        if p.is_file() and p.suffix in (".ny", ".c", ".h", ".py", ".md"):
            yield str(p.relative_to(ROOT))
        elif p.is_dir():
            for child in p.rglob("*"):
                if child.is_file() and child.suffix in (".ny", ".c", ".h", ".py", ".md") and not any(part.startswith(".") for part in child.parts):
                    yield str(child.relative_to(ROOT))

def _scan_braces(line):
    depth = 0
    in_string, escaped = False, False
    code, _ = _split_comment(line)
    for ch in code:
        if in_string:
            if escaped: escaped = False
            elif ch == "\\": escaped = True
            elif ch == '"': in_string = False
            continue
        if ch == '"': in_string = True
        elif ch == "{": depth += 1
        elif ch == "}": depth -= 1
    return depth, "{" in code

def _extract_ny_functions(path):
    lines = (ROOT / path).read_text().splitlines()
    funcs = []
    i = 0
    while i < len(lines):
        match = FN_RE.match(lines[i])
        if not match:
            i += 1
            continue
        start, depth, saw_open = i, 0, False
        j = i
        while j < len(lines):
            delta, opened = _scan_braces(lines[j])
            depth += delta
            saw_open = saw_open or opened
            if saw_open and depth <= 0: break
            j += 1
        fn_text = "\n".join(lines[start : j + 1])
        
        # Docstring check helpers
        has_doc = False
        body_start = fn_text.find("{")
        if body_start != -1:
            body_after = fn_text[body_start+1:].lstrip()
            if body_after.startswith('"'): has_doc = True
        if not has_doc and j + 1 < len(lines):
            nxt = lines[j+1].strip()
            if not nxt and j + 2 < len(lines): nxt = lines[j+2].strip()
            if nxt.startswith('"'): has_doc = True
            
        # Repetition normalization
        body_txt = fn_text[body_start + 1 : fn_text.rfind("}")] if body_start != -1 else fn_text
        exact = " ".join([_split_comment(ln)[0].strip() for ln in body_txt.splitlines() if ln.strip()])
        
        tokens = []
        for t in TOKEN_RE.findall(body_txt):
            if t.startswith('"'): tokens.append("STR")
            elif t[0].isdigit(): tokens.append("NUM")
            else: tokens.append(t)
        
        funcs.append({
            "name": match.group(1), "path": path, "line": start + 1, "end": j + 1,
            "has_doc": has_doc, "exact": exact, "tokens": tuple(tokens), "signature": lines[i].strip()
        })
        i = j + 1
    return funcs

def run_analyze(dirs=None):
    if not dirs: dirs = ["lib", "src", "etc/tests"]
    fs = [f for f in _iter_files(dirs) if f.endswith(".ny")]
    log("ANALYZE", f"auditing {len(fs)} files")
    
    # Analysis cache logic
    cache = {}
    if ANALYSIS_CACHE_PATH.exists():
        try: cache = json.loads(ANALYSIS_CACHE_PATH.read_text())
        except: pass
    
    new_cache = {}
    all_funcs = []
    
    for f in fs:
        p = ROOT / f
        try: st = p.stat()
        except: continue
        mtime, size = str(st.st_mtime), st.st_size
        
        if f in cache and cache[f].get("mtime") == mtime and cache[f].get("size") == size:
            all_funcs.extend(cache[f].get("funcs", []))
            new_cache[f] = cache[f]
            continue
            
        funcs = _extract_ny_functions(f)
        all_funcs.extend(funcs)
        new_cache[f] = {"mtime": mtime, "size": size, "funcs": funcs}

    ANALYSIS_CACHE_PATH.parent.mkdir(parents=True, exist_ok=True)
    ANALYSIS_CACHE_PATH.write_text(json.dumps(new_cache, indent=2))
    
    missing_docs = [f for f in all_funcs if not f["has_doc"]]
    
    # Exact duplicates
    exact_groups = defaultdict(list)
    for f in all_funcs:
        if len(f["tokens"]) > 4: exact_groups[f["exact"]].append(f)
    dupes = sorted([g for g in exact_groups.values() if len(g) > 1], key=lambda x: -len(x))
    
    # Near duplicates
    pairs = []
    eligible = [f for f in all_funcs if len(f["tokens"]) > 10]
    # Simple ngram similarity for speed
    def get_ngrams(tokens, n=5): return {tokens[i:i+n] for i in range(len(tokens)-n+1)}
    for f1, f2 in itertools.combinations(eligible[:200], 2): # Limit to first 200 for speed
        if f1["exact"] == f2["exact"]: continue
        g1, g2 = get_ngrams(f1["tokens"]), get_ngrams(f2["tokens"])
        if not g1 or not g2: continue
        sim = len(g1 & g2) / max(len(g1), len(g2))
        if sim > 0.8: pairs.append((sim, f1, f2))
    pairs.sort(key=lambda x: -x[0])

    # Print Report
    print(f"\n{c('1;36', 'NYTRIX SOURCE ANALYSIS')}")
    print(f"{c('90', '-'*73)}")
    print(f"{c('32', 'Functions')} : {len(all_funcs)}")
    print(f"{c('31', 'Missing Doc')} : {len(missing_docs)}")
    print(f"{c('33', 'Duplicates')}  : {len(dupes)} exact groups, {len(pairs)} near-pairs")
    
    if missing_docs:
        print(f"\n{c('1', 'Missing Docstrings (Top 10):')}")
        for m in missing_docs[:10]:
            print(f"  {c('37', m['path'])}:{m['line']} - {c('90', m['signature'])}")
            
    if dupes:
        print(f"\n{c('1', 'Exact Duplicate Bodies (Top 5):')}")
        for g in dupes[:5]:
            print(f"  {c('1;33', g[0]['name'])} ({len(g)} copies)")
            for m in g[:3]: print(f"    {m['path']}:{m['line']}")
            
    if pairs:
        print(f"\n{c('1', 'Near-Duplicate Bodies (Top 5):')}")
        for sim, f1, f2 in pairs[:5]:
            print(f"  {c('1;33', f'{sim*100:.1f}%')} match: {f1['name']} <-> {f2['name']}")
            print(f"    {f1['path']}:{f1['line']}\n    {f2['path']}:{f2['line']}")
    print()

# ─── Parse-error detector (brace balance + semicolon-swallow) ────────────────

def _ny_tokenize(content):
    """Yield (kind, value, line, col). kind: open/close/semi/str/nl/char."""
    n = len(content)
    i = 0
    line = 1
    col = 1
    while i < n:
        c_ = content[i]
        if c_ == '\n':
            yield ('nl', '\n', line, col)
            line += 1; col = 1; i += 1; continue
        if c_ == '"':
            start_line, start_col = line, col
            j = i + 1
            while j < n and content[j] != '\n':
                if content[j] == '\\' and j+1 < n: j += 2; continue
                if content[j] == '"': break
                j += 1
            end = j+1 if j < n and content[j] == '"' else j
            val = content[i:end]
            yield ('str', val, start_line, start_col)
            col += len(val); i = end; continue
        if c_ == ';':
            start_line, start_col = line, col
            j = i
            while j < n and content[j] != '\n': j += 1
            yield ('semi', content[i:j], start_line, start_col)
            col += (j - i); i = j; continue
        if c_ == '{': yield ('open', '{', line, col)
        elif c_ == '}': yield ('close', '}', line, col)
        else: yield ('char', c_, line, col)
        col += 1; i += 1

def _count_net_braces_in_text(text):
    """Count net '}' - '{' in text, skipping string literals."""
    opens = closes = 0
    j = 0
    while j < len(text):
        ch = text[j]
        if ch == '"':
            j += 1
            while j < len(text) and text[j] != '"':
                if text[j] == '\\' and j+1 < len(text): j += 1
                j += 1
            j += 1; continue
        if ch == '{': opens += 1
        elif ch == '}': closes += 1
        j += 1
    return closes - opens  # positive = net close

def check_file_braces(path):
    """Returns (unclosed_opens, extra_closes, swallows, err_str)."""
    try:
        content = Path(path).read_text()
    except Exception as e:
        return None, None, None, str(e)
    lines = content.split('\n')
    stack = []   # (line, col, ctx)
    extras = []  # (line, col, ctx)
    swallows = []  # (line, col, ctx, net)
    in_comment = False
    for kind, val, ln, col in _ny_tokenize(content):
        if kind == 'nl': in_comment = False; continue
        if in_comment: continue
        if kind == 'semi':
            in_comment = True
            net = _count_net_braces_in_text(val[1:])
            if net > 0:
                ctx = lines[ln-1].strip()[:90]
                swallows.append((ln, col, ctx, net))
            continue
        if kind == 'open':
            stack.append((ln, col, lines[ln-1].strip()[:90]))
        elif kind == 'close':
            if stack: stack.pop()
            else: extras.append((ln, col, lines[ln-1].strip()[:90]))
    return stack, extras, swallows, None

def _fix_swallow_line(line):
    """Remove the first ';' from a line, un-commenting the rest."""
    # Find ';' outside of string literals
    quote = None
    escaped = False
    for i, ch in enumerate(line):
        if quote:
            if escaped: escaped = False
            elif ch == '\\': escaped = True
            elif ch == quote: quote = None
            continue
        if ch == '"': quote = ch; continue
        if ch == ';':
            return line[:i] + line[i+1:]
    return line

def run_check(paths=None, verbose=False, fix=False, skip_dirs=None):
    """Check .ny files for brace balance and semicolon-swallow bugs."""
    skip_dirs = skip_dirs or []
    if paths is None:
        paths = ["lib", "etc/tests"]
    targets = []
    for raw in paths:
        p = ROOT / raw
        if p.is_file():
            targets.append(p)
        elif p.is_dir():
            for child in sorted(p.rglob("*.ny")):
                if not any(sd in str(child) for sd in skip_dirs):
                    targets.append(child)
    if not targets:
        warn("check: no .ny files found")
        return
    log("CHECK", f"scanning {len(targets)} files for parse bugs...")
    failed = fixed = 0
    for path in targets:
        unclosed, extras, swallows, err = check_file_braces(path)
        if err:
            warn(f"  {path.relative_to(ROOT)}: {err}")
            continue
        # Swallows are only real bugs if the file is also unbalanced
        real_swallows = swallows if (unclosed or extras) else []
        issues = bool(unclosed or extras or real_swallows)
        if not issues:
            if verbose:
                try: rel_str = str(path.relative_to(ROOT))
                except ValueError: rel_str = str(path)
                log_ok(f"  ✓ {rel_str}")
            continue
        try:
            rel = path.relative_to(ROOT)
        except ValueError:
            rel = path
        failed += 1
        print(f"\n{c('31', '✗')} {c('1', str(rel))}")
        if real_swallows:
            print(f"  {c('33', f'{len(real_swallows)} semicolon-swallow bug(s)')}  {c('90', '(; comments out })')}:")
            for ln, col, ctx, net in real_swallows[:6]:
                print(f"    {c('36', f'line {ln}:{col}')}  (hides {net} '}}')  {ctx}")
                print(f"    {c('90', 'hint: remove the ; — it starts a comment, the } after it is never seen')}")
            if len(real_swallows) > 6:
                print(f"    {c('90', f'... and {len(real_swallows)-6} more')}")
            if fix:
                lines = path.read_text().splitlines(keepends=True)
                for ln, col, ctx, net in real_swallows:
                    idx = ln - 1
                    if 0 <= idx < len(lines):
                        lines[idx] = _fix_swallow_line(lines[idx])
                path.write_text("".join(lines))
                fixed += 1
                print(f"  {c('32', '→ fixed (semicolons removed)')}")
        if unclosed:
            print(f"  {c('33', f'{len(unclosed)} unclosed')} '{{' :")
            for ln, col, ctx in unclosed[:5]:
                print(f"    {c('36', f'line {ln}:{col}')}  {ctx}")
        if extras:
            print(f"  {c('33', f'{len(extras)} extra')} '}}' :")
            for ln, col, ctx in extras[:5]:
                print(f"    {c('36', f'line {ln}:{col}')}  {ctx}")
    print()
    if failed:
        msg = f"check: {failed} files with issues"
        if fix and fixed: msg += f", {fixed} auto-fixed"
        warn(msg)
    else:
        log_ok(f"check: all {len(targets)} files OK")


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="*")
    parser.add_argument("--analyze", action="store_true")
    parser.add_argument("--check", action="store_true", help="Check for parse bugs")
    parser.add_argument("--fix", action="store_true", help="Auto-fix semicolon-swallow bugs")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    if args.check or args.fix:
        run_check(args.paths or None, verbose=args.verbose, fix=args.fix)
    elif args.analyze: run_analyze(args.paths or None)
    else: run_fmt(args.paths or None)
