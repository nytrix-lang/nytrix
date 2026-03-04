#!/usr/bin/env python3
"""
Nytrix Web Tools - Documentation Generator
"""
import os
import sys
import re
import json
import argparse
import socketserver
from pathlib import Path
from http.server import SimpleHTTPRequestHandler, HTTPServer

sys.dont_write_bytecode = True
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
sys.path.insert(0, str(Path(__file__).resolve().parent))

from context import ROOT
from utils import log, log_ok, warn, err, write_if_changed

def extract_body_html(html_text):
    body_match = re.search(r"<body[^>]*>(.*?)</body>", html_text, re.DOTALL | re.IGNORECASE)
    if body_match:
        return body_match.group(1)
    return html_text

def refine_info_html(html_text):
    html_text = html_text.replace(
        '<div class="contents" style="margin-left: 20px;">',
        '<div class="contents">'
    )
    html_text = html_text.replace(
        '<ul class="mini-toc" style="margin-left: 20px;">',
        '<ul class="mini-toc">'
    )
    html_text = re.sub(r'<div class="nav-panel">.*?</div>', '', html_text, flags=re.DOTALL | re.IGNORECASE)
    html_text = re.sub(r'Next:\s.*?(\s|&nbsp;|</p>|<br>)', '', html_text, flags=re.IGNORECASE)
    html_text = re.sub(r'Previous:\s.*?(\s|&nbsp;|</p>|<br>)', '', html_text, flags=re.IGNORECASE)
    html_text = re.sub(r'Up:\s.*?(\s|&nbsp;|</p>|<br>)', '', html_text, flags=re.IGNORECASE)
    html_text = re.sub(r'\[Contents\]\s*\[Index\]', '', html_text, flags=re.IGNORECASE)
    html_text = re.sub(r'\s*<a class="copiable-link"[^>]*>.*?</a>\s*', ' ', html_text, flags=re.DOTALL | re.IGNORECASE)
    return html_text

def deident_code(code_lines):
    if not code_lines:
        return ""
    lines = code_lines.split("\n")
    min_indent = -1
    for line in lines:
        if line.strip():
            indent = len(line) - len(line.lstrip())
            if min_indent == -1 or indent < min_indent:
                min_indent = indent
    if min_indent <= 0:
        return code_lines
    dedented_lines = []
    for line in lines:
        if len(line) >= min_indent:
            dedented_lines.append(line[min_indent:])
        else:
            dedented_lines.append(line)
    return "\n".join(dedented_lines)

def load_markdown_docs(docs_dir):
    docs = []
    dp = Path(docs_dir)
    if not dp.exists():
        return docs
    for f in sorted(dp.iterdir()):
        if f.suffix == ".md":
            docs.append({"name": f.stem, "format": "md", "html": f.read_text(encoding="utf-8")})
        elif f.suffix == ".html":
            html = extract_body_html(f.read_text(encoding="utf-8"))
            html = refine_info_html(html)
            docs.append({"name": f.stem, "format": "html", "html": html})
    return docs

def sanitize_id(name):
    return re.sub(r"[^a-zA-Z0-9_]", "_", name)

def extract_docstring(body_text):
    """
    Extracts a leading string literal docstring from a function body.
    """
    doc_pattern = re.compile(r'^\s*("([^"\\]*(\\.[^"\\]*)*)"|\'([^\'\\]*(\\.[^\'\\]*)*)\')', re.MULTILINE)
    match = doc_pattern.search(body_text)
    if match:
        doc = (match.group(2) or match.group(4) or "").strip()
        remaining = body_text[match.end():]
        return doc, remaining
    return "", body_text

def get_leading_comments(full_content, start_pos):
    """
    Extracts consecutive ;; comments immediately preceding start_pos.
    """
    lines = full_content[:start_pos].splitlines()
    comments = []
    for line in reversed(lines):
        line = line.strip()
        if line.startswith(";;"):
            comments.append(line[2:].strip())
        elif not line or line.startswith(";") or line.startswith("#"):
            if line.startswith(";;"): continue
            if not line: continue
            break
        else:
            break
    if not comments:
        return ""
    return "\n".join(reversed(comments))

def parse_function_body(chunk, start_pos):
    depth = 0
    in_string = False
    in_comment = False
    string_char = None
    escape_next = False
    body_start = -1
    i = start_pos
    while i < len(chunk):
        char = chunk[i]
        if escape_next:
            escape_next = False
            i += 1
            continue
        if char == "\\" and in_string:
            escape_next = True
            i += 1
            continue
        if char == ";" and not in_string:
            in_comment = True
            i += 1
            continue
        if char == "\n" and in_comment:
            in_comment = False
            i += 1
            continue
        if in_comment:
            i += 1
            continue
        if char in ('"', "'") and not in_string:
            in_string = True
            string_char = char
            i += 1
            continue
        if char == string_char and in_string:
            in_string = False
            string_char = None
            i += 1
            continue
        if in_string:
            i += 1
            continue
        if char == "{":
            if depth == 0:
                body_start = i
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0 and body_start != -1:
                return chunk[body_start + 1:i], i + 1
        i += 1
    return None, start_pos

def parse_imports(body_text):
    imports = []
    import_pattern = re.compile(r"use\s+([a-zA-Z0-9_.]+)(?:\s+as\s+([a-zA-Z0-9_]+))?")
    for match in import_pattern.finditer(body_text):
        full_path = match.group(1)
        alias = match.group(2)
        parts = full_path.split(".")
        if len(parts) > 1:
            module_target = ".".join(parts[:-1])
            symbol_target = parts[-1]
        else:
            module_target = ""
            symbol_target = full_path
        imports.append({"full_path": full_path, "module_target": module_target, "symbol_target": symbol_target, "alias": alias})
    return imports

def clean_code_view(code_lines):
    if not code_lines:
        return ""
    lines = code_lines.split("\n")
    while lines and not lines[0].strip():
        lines.pop(0)
    while lines and not lines[-1].strip():
        lines.pop()
    return "\n".join(lines)

def parse_nytrix_docs(bundle_path, docs_dirs=None):
    if not bundle_path or not Path(bundle_path).exists():
        err(f"Error: Bundle file not found: {bundle_path}")
        return []

    content = Path(bundle_path).read_text(encoding="utf-8")
    
    # Split content by #line markers or ;; Module from markers
    marker_pattern = re.compile(r"(?:^#line \d+ \"(?P<path1>.*)\"|^;;\s*Module from\s+(?P<path2>.*))", re.MULTILINE)
    matches = list(marker_pattern.finditer(content))
    
    modules = []
    parsed_count = 0

    if not matches:
        chunks = [(0, len(content), "unknown")]
    else:
        chunks = []
        for i in range(len(matches)):
            m = matches[i]
            fname = m.group('path1') or m.group('path2') or ""
            start = m.end()
            end = matches[i+1].start() if i+1 < len(matches) else len(content)
            chunks.append((start, end, fname))

    for start, end, orig_file in chunks:
        body = content[start:end]
        if not body.strip():
            continue

        mod_match = re.search(r"module\s+([a-zA-Z0-9.]+)", body)
        if not mod_match:
            continue

        mod_name = mod_match.group(1)
        path = mod_name.split(".")

        mod_doc = get_leading_comments(content, start + mod_match.start())
        mod_body_doc_match = re.search(
            r"module\s+[\w\.]+\s*(?:\([^)]*\)\s*)?\{?\s*(\"([^\"\\]*(\\.[^\"\\]*)*)\"|\'([^\'\\]*(\\.[^\'\\]*)*)\')",
            body,
            re.DOTALL,
        )
        if mod_body_doc_match:
            doc_str = (mod_body_doc_match.group(2) or mod_body_doc_match.group(4) or "").strip()
            if mod_doc: mod_doc += "\n\n" + doc_str
            else: mod_doc = doc_str

        symbols = []
        function_ranges = []
        
        # 1. Functions: fn name(args) { ... }
        fn_pattern = re.compile(r"fn\s+([a-zA-Z0-9_.:]+)\s*\((.*?)\)\s*\{", re.DOTALL)
        for fn_match in fn_pattern.finditer(body):
            fn_name = fn_match.group(1)
            fn_args = fn_match.group(2).strip()
            full_sig = f"fn {fn_name}({fn_args})"
            
            fn_body, end_p = parse_function_body(body, fn_match.end() - 1)
            if fn_body is None: continue
            
            function_ranges.append((fn_match.start(), end_p))
            
            doc = get_leading_comments(body, fn_match.start())
            inner_doc, code_after_doc = extract_docstring(fn_body)
            if inner_doc:
                if doc: doc += "\n\n" + inner_doc
                else: doc = inner_doc
                
            code_view = deident_code(code_after_doc)
            code_view = clean_code_view(code_view)
            imports = parse_imports(fn_body)
            
            symbols.append({
                "id": sanitize_id(fn_name), 
                "name": full_sig, 
                "kind": "function", 
                "doc": doc, 
                "code": code_view, 
                "imports": imports
            })

        # 1.5. Structs and Layouts
        struct_pattern = re.compile(r"(?P<kind>struct|layout)\s+([a-zA-Z0-9_]+)\s*(?:\([^)]*\))?\s*\{", re.DOTALL)
        for st_match in struct_pattern.finditer(body):
            if any(r[0] <= st_match.start() <= r[1] for r in function_ranges): continue
            
            kind = st_match.group("kind")
            st_name = st_match.group(2)
            
            st_body, end_p = parse_function_body(body, st_match.end() - 1)
            if st_body is None: continue
            
            doc = get_leading_comments(body, st_match.start())
            if not doc: doc = f"{kind.capitalize()} definition."
            
            symbols.append({
                "id": sanitize_id(st_name),
                "name": f"{kind} {st_name}",
                "kind": kind,
                "doc": doc,
                "code": f"{kind} {st_name} {{\n{deident_code(st_body)}\n}}"
            })

        # 2. Extern functions
        extern_pattern = re.compile(r"extern\s+fn\s+([a-zA-Z0-9_]+)\s*\((.*?)\)(?:\s*:\s*([a-zA-Z0-9_]+))?(?:\s+as\s+\"([^\"]*)\")?", re.MULTILINE)
        for ext_match in extern_pattern.finditer(body):
            # Check if inside a function (unlikely for extern, but still)
            if any(r[0] <= ext_match.start() <= r[1] for r in function_ranges): continue
            
            fn_name = ext_match.group(1)
            fn_args = ext_match.group(2).strip()
            ret_type = ext_match.group(3) or "void"
            alias = ext_match.group(4)
            
            full_sig = f"extern fn {fn_name}({fn_args}): {ret_type}"
            if alias: full_sig += f' as "{alias}"'
            
            doc = get_leading_comments(body, ext_match.start())
            if not doc: doc = "External function."
            
            symbols.append({
                "id": sanitize_id(fn_name),
                "name": full_sig,
                "kind": "extern",
                "doc": doc,
                "code": ext_match.group(0).strip()
            })

        # 3. Constants/Variables: def NAME = VALUE (top-level only)
        const_pattern = re.compile(r"^([ \t]*)(?P<kind>def|mut)\s+([a-zA-Z0-9_]+)\s*=", re.MULTILINE)
        for const_match in const_pattern.finditer(body):
            indent = const_match.group(1)
            if len(indent) > 3: continue # Heuristic for top-level
            if any(r[0] <= const_match.start() <= r[1] for r in function_ranges): continue
            
            const_name = const_match.group(3)
            if const_name == "define": continue
            
            doc = get_leading_comments(body, const_match.start())
            kind_str = const_match.group('kind')
            if not doc: doc = f"{kind_str.capitalize()} definition."
            
            line_end = body.find("\n", const_match.end())
            if line_end == -1: line_end = len(body)
            code_view = body[const_match.start():line_end].strip()
            
            symbols.append({
                "id": sanitize_id(const_name),
                "name": const_name,
                "kind": "constant" if kind_str == "def" else "variable",
                "sig": code_view,
                "doc": doc,
                "code": code_view
            })

        # 4. Enums
        enum_pattern = re.compile(r"enum\s+([a-zA-Z0-9_]+)\s*(?:\([^)]*\))?\s*\{", re.DOTALL)
        for en_match in enum_pattern.finditer(body):
            if any(r[0] <= en_match.start() <= r[1] for r in function_ranges): continue
            
            en_name = en_match.group(1)
            en_body, end_p = parse_function_body(body, en_match.end() - 1)
            if en_body is None: continue
            
            doc = get_leading_comments(body, en_match.start())
            if not doc: doc = "Enum definition."
            
            symbols.append({
                "id": sanitize_id(en_name),
                "name": f"enum {en_name}",
                "kind": "enum",
                "doc": doc,
                "code": f"enum {en_name} {{\n{deident_code(en_body)}\n}}"
            })

        # 5. Aliases
        alias_pattern = re.compile(r"^([ \t]*)alias\s+([a-zA-Z0-9_]+)\s*=", re.MULTILINE)
        for al_match in alias_pattern.finditer(body):
            if any(r[0] <= al_match.start() <= r[1] for r in function_ranges): continue
            
            al_name = al_match.group(2)
            doc = get_leading_comments(body, al_match.start())
            if not doc: doc = "Type alias."
            
            line_end = body.find("\n", al_match.end())
            if line_end == -1: line_end = len(body)
            code_view = body[al_match.start():line_end].strip()
            
            symbols.append({
                "id": sanitize_id(al_name),
                "name": f"alias {al_name}",
                "kind": "alias",
                "doc": doc,
                "code": code_view
            })

        modules.append({
            "name": mod_name, 
            "module_doc": mod_doc, 
            "symbols": symbols, 
            "path": path, 
            "orig_file": orig_file
        })
        parsed_count += 1

    if not docs_dirs:
        docs_dirs = [ROOT / "docs"]
    markdown_docs = []
    seen = set()
    for d in docs_dirs:
        for doc in load_markdown_docs(d):
            if doc["name"] in seen:
                continue
            markdown_docs.append(doc)
            seen.add(doc["name"])

    overview_module = {
        "name": "Overview",
        "module_doc": "Nytrix Standard Library - Reference documentation.",
        "symbols": [],
        "path": ["Home"],
        "markdown_docs": markdown_docs,
    }
    modules.insert(0, overview_module)
    
    # Sort modules by name (except Overview)
    overview = modules[0]
    rest = sorted(modules[1:], key=lambda x: x["name"])
    modules = [overview] + rest
    
    log_ok(f"Parsed {parsed_count} modules with {sum(len(m.get('symbols', [])) for m in modules)} total symbols")
    return modules

class CustomHandler(SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/":
            self.path = "/index.html"
        return SimpleHTTPRequestHandler.do_GET(self)
    def log_message(self, format, *args):
        pass

def run_web_gen(bundle_path, output_dir=None, serve=False, port=8000):
    assets_dir = ROOT / "etc" / "assets" / "website"
    template_path = assets_dir / "web.html"
    js_path = assets_dir / "web.js"
    css_path = assets_dir / "web.css"

    if not template_path.exists():
        err(f"Error: web.html not found at {template_path}")
        return 1

    html_tpl = template_path.read_text(encoding="utf-8")
    js_tpl = js_path.read_text(encoding="utf-8") if js_path.exists() else ""
    css_tpl = css_path.read_text(encoding="utf-8") if css_path.exists() else ""

    log("WEB", f"Source: {bundle_path}")
    docs_dirs = [ROOT / "docs"]
    extra_dir = os.getenv("NYTRIX_WEBDOC_INFO_DIR", "build/cache/nytrix-info")
    if os.path.isdir(extra_dir):
        docs_dirs.append(Path(extra_dir))

    data = parse_nytrix_docs(bundle_path, docs_dirs=docs_dirs)

    if output_dir:
        output_dir = Path(output_dir)
        output_dir.mkdir(parents=True, exist_ok=True)
        html_path = output_dir / "index.html"

        final_html = html_tpl.replace("DATA_PLACEHOLDER", json.dumps(data, indent=2))
        final_html = final_html.replace("SCRIPT_PLACEHOLDER", js_tpl)
        final_html = final_html.replace("CSS_PLACEHOLDER", css_tpl)

        if write_if_changed(html_path, final_html):
            log_ok(f"Documentation written to: {html_path.relative_to(ROOT)}")
        else:
            log_ok("Documentation up to date")

    if serve:
        serve_dir = output_dir if output_dir else Path(bundle_path).parent
        os.chdir(serve_dir)
        print(f"\nServing documentation at http://localhost:{port}")
        print("   Press Ctrl+C to stop\n")
        try:
            with socketserver.TCPServer(("", port), CustomHandler) as httpd:
                httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n✓ Server stopped")
        except OSError:
            err(f"Error: Port {port} already in use")
            return 1

    return 0

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Nytrix Documentation Generator")
    parser.add_argument("input", nargs="?", help="Path to std_bundle.ny")
    parser.add_argument("-o", "--output", help="Output directory")
    parser.add_argument("-s", "--serve", action="store_true", help="Serve local HTTP server")
    parser.add_argument("-p", "--port", type=int, default=8000, help="Port (default: 8000)")
    args = parser.parse_args()

    if not args.input:
        parser.print_help()
        sys.exit(1)

    sys.exit(run_web_gen(args.input, args.output, args.serve, args.port))
