import sys
sys.dont_write_bytecode = True
import re
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))

from context import ROOT, c
from utils import log, log_ok, write_if_changed

def run_std_bundle(bundle_path=None):
    def file_priority(f):
        path_s = f.as_posix()
        # Adjusted paths after merging lib into module root
        if "core/mod.ny" in path_s: return 0
        if "os/sys.ny" in path_s: return 1
        if "str/mod.ny" in path_s: return 2
        if "str/io.ny" in path_s: return 3
        if "core/reflect.ny" in path_s: return 4
        if "core/error.ny" in path_s: return 5
        if "core/list.ny" in path_s: return 6
        if "core/dict.ny" in path_s: return 7
        if "core/set.ny" in path_s: return 8
        if f.name == "mod.ny": return 20
        return 30
    
    std_dir = ROOT / "std"
    if not std_dir.exists():
        log("STD", "Error: std/ directory not found")
        return 1
        
    files = []
    for f in std_dir.rglob("*.ny"):
        rel_path = f.relative_to(std_dir)
        rel_parts = [p.lower() for p in rel_path.parts]

        # Never bundle tests.
        if f.name.endswith("_test.ny"):
            continue

        files.append(f)
    if not files:
        log("STD", "Warning: No .ny files in std/")
        return 0
        
    sorted_files = sorted(list(set(files)), key=lambda f: (file_priority(f.relative_to(ROOT)), str(f)))
    
    bundled_output_lines = []

    symbol_map = {}
    
    for f in sorted_files:
        content = f.read_text(encoding="utf-8")
        rel_path = f.relative_to(std_dir)
        parts = list(rel_path.parts)
        # Test files are filtered out by name, so no need to filter 'lib' anymore.
        if parts[-1] == "mod.ny":
            parts.pop()
        else:
            parts[-1] = parts[-1].replace(".ny", "")
        
        full_mod_name = "std." + ".".join(parts)
        
        bundled_output_lines.append(f";; Module from {rel_path.as_posix().replace('.ny', '')}\n")
        bundled_output_lines.append(f"module {full_mod_name} *\n")
        bundled_output_lines.append(content)
        bundled_output_lines.append("\n")
        bundled_output_lines.append(f"use {full_mod_name} *\n") # Make symbols available to subsequent modules in the bundle
        bundled_output_lines.append("\n")
        
        # When parsing function/def names for std_symbols.h, use the fully qualified name
        # to ensure uniqueness across modules within the bundle.
        # This will prevent collisions in the C-level symbol table for builtins/runtime functions.
        for line in content.split('\n'):
            line = line.strip()
            if line.startswith('fn '):
                # Simple parser for fn name
                name_part = line[3:].split('(')[0].strip()
                if not name_part.startswith('_') and name_part:
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
        "static const nt_std_symbols[] = {",
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
        
        log_ok(f"Bundled {modules_disp} std modules -> {bundle_path_disp}")
        log_ok(f"Generated {symbols_disp} symbols -> {sym_path_disp}")
    else:
        # log("STD", "Std bundle up to date")
        pass
        
    return 0

if __name__ == "__main__":
    p = sys.argv[1] if len(sys.argv) > 1 else None
    sys.exit(run_std_bundle(p))
