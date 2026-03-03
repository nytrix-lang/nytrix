#!/usr/bin/env python3
"""
Nytrix Standard Library Tools
"""
import sys
sys.dont_write_bytecode = True
import os
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
import argparse
import re
import heapq
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))

from context import ROOT, c
from utils import log, log_ok, write_if_changed

_MODULE_DECL_RE = re.compile(r'^\s*module\s+([a-zA-Z_][a-zA-Z0-9_\.]*)\b')
_USE_DECL_RE = re.compile(r'^\s*use\s+([a-zA-Z_][a-zA-Z0-9_\.]*)\b')

def _declared_module_name(content):
    for raw in content.splitlines():
        line = raw.strip()
        if not line or line.startswith(";") or line.startswith("#"):
            continue
        m = _MODULE_DECL_RE.match(line)
        if m:
            return m.group(1)
    return ""

def _used_modules(content):
    used = []
    seen = set()
    for raw in content.splitlines():
        line = raw.strip()
        if not line or line.startswith(";") or line.startswith("#"):
            continue
        m = _USE_DECL_RE.match(line)
        if not m:
            continue
        mod = m.group(1)
        if mod in seen:
            continue
        seen.add(mod)
        used.append(mod)
    return used

def run_std_bundle(bundle_path=None):
    def file_priority(f):
        path_s = f.as_posix()
        if "os/sys.ny" in path_s: return 1
        if "str/mod.ny" in path_s: return 2
        if "str/io.ny" in path_s: return 3
        if "core/reflect.ny" in path_s: return 4
        if "core/error.ny" in path_s: return 5
        if "core/list.ny" in path_s: return 6
        if "core/dict.ny" in path_s: return 7
        if "core/set.ny" in path_s: return 8
        if "core/mod.ny" in path_s: return 10
        if f.name == "mod.ny": return 20
        return 30
    
    lib_dir = ROOT / "lib"
    if not lib_dir.exists():
        log("STD", "Error: lib/ directory not found")
        return 1
        
    files = []
    for f in lib_dir.rglob("*.ny"):
        if f.name.endswith("_test.ny"):
            continue

        files.append(f)
    if not files:
        log("STD", "Warning: No .ny files in lib/")
        return 0
        
    sorted_files = sorted(list(set(files)), key=lambda f: (file_priority(f.relative_to(ROOT)), str(f)))

    file_recs = []
    for f in sorted_files:
        content = f.read_text(encoding="utf-8")
        rel_path = f.relative_to(lib_dir)
        parts = list(rel_path.parts)
        if parts[-1] == "mod.ny":
            parts.pop()
        else:
            parts[-1] = parts[-1].replace(".ny", "")
        fallback_mod_name = "lib." + ".".join(parts)
        declared_mod_name = _declared_module_name(content)
        full_mod_name = declared_mod_name if declared_mod_name else fallback_mod_name
        file_recs.append({
            "file": f,
            "content": content,
            "rel_path": rel_path,
            "module": full_mod_name,
            "uses": _used_modules(content),
            "sort_key": (file_priority(f.relative_to(ROOT)), str(f)),
        })

    mod_to_idx = {}
    for i, rec in enumerate(file_recs):
        mod_to_idx.setdefault(rec["module"], i)

    n = len(file_recs)
    deps = [set() for _ in range(n)]
    indeg = [0] * n
    forward = [set() for _ in range(n)]
    for i, rec in enumerate(file_recs):
        for dep_mod in rec["uses"]:
            dep_idx = mod_to_idx.get(dep_mod)
            if dep_idx is None or dep_idx == i:
                continue
            if dep_idx in deps[i]:
                continue
            deps[i].add(dep_idx)
            indeg[i] += 1
            forward[dep_idx].add(i)

    heap = []
    for i in range(n):
        if indeg[i] == 0:
            heapq.heappush(heap, (file_recs[i]["sort_key"], i))

    ordered_idxs = []
    done = [False] * n
    while len(ordered_idxs) < n:
        if not heap:
            for i in sorted(range(n), key=lambda j: file_recs[j]["sort_key"]):
                if not done[i]:
                    heapq.heappush(heap, (file_recs[i]["sort_key"], i))
                    break

        _, i = heapq.heappop(heap)
        if done[i]:
            continue
        done[i] = True
        ordered_idxs.append(i)
        for nxt in forward[i]:
            indeg[nxt] -= 1
            if indeg[nxt] <= 0 and not done[nxt]:
                heapq.heappush(heap, (file_recs[nxt]["sort_key"], nxt))

    bundled_output_lines = []

    symbol_map = {}
    
    for i in ordered_idxs:
        rec = file_recs[i]
        f = rec["file"]
        content = rec["content"]
        rel_path = rec["rel_path"]
        full_mod_name = rec["module"]
        
        bundled_output_lines.append(f"#line 1 \"lib/{rel_path.as_posix()}\"\n")
        # Only prepend module declaration if it doesn't already have one
        if not _declared_module_name(content):
            bundled_output_lines.append(f"module {full_mod_name} *\n")
        
        bundled_output_lines.append(content)
        bundled_output_lines.append("\n")
        bundled_output_lines.append(f"use {full_mod_name} *\n") # Make symbols available to subsequent modules in the bundle
        bundled_output_lines.append("\n")
        
        for line in content.split('\n'):
            line = line.strip()
            if line.startswith('fn ') or line.startswith('@extern'):
                rem = line
                if rem.startswith('@extern'):
                    rem = rem[7:].strip()
                if rem.startswith('fn '):
                    name_part = rem[3:].split('(')[0].strip()
                    # Allow __ intrinsics but skip _ private
                    is_intrinsic = name_part.startswith('__')
                    is_private = name_part.startswith('_') and not is_intrinsic
                    if not is_private and name_part:
                        symbol_map[f"{full_mod_name}.{name_part}"] = full_mod_name
            elif line.startswith('def '):
                def_match = re.match(r'def\s+([a-zA-Z_][a-zA-Z0-9_]*)\s*=', line)
                if def_match:
                    name_part = def_match.group(1).strip()
                    if not name_part.startswith('_'):
                        symbol_map[f"{full_mod_name}.{name_part}"] = full_mod_name
    if not bundle_path:
        bundle_path = ROOT / "build" / "release" / "std.ny"
    else:
        bundle_path = Path(bundle_path).resolve()
        
    bundle_path.parent.mkdir(exist_ok=True, parents=True)
    bundle_text = '\n'.join(bundled_output_lines)
    bundle_changed = write_if_changed(bundle_path, bundle_text)
    
    sym_path = bundle_path.parent / "std_symbols.h"
    sym_lines = [
        "#pragma once",
        "typedef struct { const char *sym; const char *mod; } nt_std_symbol;",
        "static const nt_std_symbol nt_std_symbols[] = {",
    ]
    for sym in sorted(symbol_map.keys()):
        mod = symbol_map[sym]
        sym_lines.append(f'    {{"{sym}", "{mod}"}},')
    sym_lines.extend([
        "    {0, 0}",
        "};",
        "",
    ])
    sym_changed = write_if_changed(sym_path, "\n".join(sym_lines))
    
    if bundle_changed or sym_changed:
        try:
            bundle_disp = bundle_path.relative_to(ROOT)
        except ValueError:
            bundle_disp = bundle_path
        try:
            sym_disp = sym_path.relative_to(ROOT)
        except ValueError:
            sym_disp = sym_path
        modules_disp = c('1;36', str(len(files)))
        symbols_disp = c('1;33', str(len(symbol_map)))
        bundle_path_disp = c('1', str(bundle_disp))
        sym_path_disp = c('1', str(sym_disp))
        
        log_ok(f"Bundled {modules_disp} lib modules -> {bundle_path_disp}")
        log_ok(f"Generated {symbols_disp} symbols -> {sym_path_disp}")
    else:
        pass
        
    return 0

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Bundle lib/*.ny into a single std file and symbol index",
    )
    ap.add_argument(
        "bundle_path",
        nargs="?",
        default=None,
        help="Output bundle path (default: build/release/std.ny)",
    )
    args = ap.parse_args(argv)
    return run_std_bundle(args.bundle_path)

if __name__ == "__main__":
    sys.exit(main())
