#!/usr/bin/env python3
"""
conv.py — Nytrix Unified Conversion Tool
=========================================
Supported modes (subcommands):

  texi   Convert Texinfo docs → man page or Markdown
  c      Convert C headers   → Nytrix `extern fn` declarations

Usage:
    python3 conv.py texi <file.texi> --name NAME [--format man|md] [--section N] [--out FILE]
    python3 conv.py c [options] [file.h ...]

Texi examples:
    python3 conv.py texi docs/ny.texi --name ny --format man
    python3 conv.py texi docs/std.texi --name std --format md --out docs/std.md

C examples:
    python3 conv.py c build/cache/glfw/include/GLFW/glfw3.h \\
        --filter glfwW --link glfw --platform linux
    python3 conv.py c build/cache/RGFW/RGFW.h \\
        --filter RGFW_ --strip RGFW_ --link rgfw
    python3 conv.py c /usr/include/X11/Xlib.h \\
        --filter X --link X11 --platform linux
    cat header.h | python3 conv.py c --stdin
"""
import re, sys, argparse, pathlib
sys.dont_write_bytecode = True

# ══════════════════════════════════════════════════════════════════════════════
#  TEXINFO CONVERSION  (texi → man / md)
# ══════════════════════════════════════════════════════════════════════════════

def _texi_handle_table(m):
    body = m.group(2).strip()
    body = re.sub(r'@item\s+(.*)', r'.TP\n.B \1', body)
    return body

def _texi_handle_itemize(m):
    body = m.group(1).strip()
    body = re.sub(r'@item\s+', r'.IP \(bu 4\n', body)
    return body

def convert_man(texi_path, man_name, section="1"):
    """Convert a Texinfo file to a groff man page string."""
    with open(texi_path, 'r') as f:
        content = f.read()

    def handle_ref(m):
        args = [a.strip() for a in m.group(1).split(',')]
        if len(args) >= 3 and args[2]:
            return f"\\fB{args[2]}\\fP({section})"
        if len(args) >= 1 and args[0]:
            return f"\\fB{args[0]}\\fP"
        return ""

    title = ""
    m = re.search(r'@settitle\s+(.*)', content)
    if m:
        title = m.group(1).strip()

    out = [f'.TH {man_name.upper()} {section} "" "{title}"']
    content = re.sub(r'\\input texinfo.*?\n', '', content)
    content = re.sub(r'@setfilename.*?\n', '', content)
    content = re.sub(r'@settitle.*?\n', '', content)

    copying = ""
    m = re.search(r'@copying\n(.*?)\n@end copying', content, re.DOTALL)
    if m:
        copying = m.group(1).strip()
        copying = re.sub(r'@copyright\{\}', '(C)', copying)
        content = re.sub(r'@copying\n.*?\n@end copying', '', content, flags=re.DOTALL)

    content = re.sub(r'@node.*?\n', '', content)
    content = re.sub(r'@menu\n.*?\n@end menu', '', content, flags=re.DOTALL)
    content = re.sub(r'@dircategory.*?\n', '', content)
    content = re.sub(r'@direntry\n.*?\n@end direntry', '', content, flags=re.DOTALL)
    content = re.sub(r'@titlepage\n.*?\n@end titlepage', '', content, flags=re.DOTALL)
    content = re.sub(r'@contents\n', '', content)
    content = re.sub(r'@appendix.*?\n', '', content)
    content = re.sub(r'@printindex.*?\n', '', content)

    m = re.search(r'@top\s+(.*)', content)
    if m:
        name_desc = m.group(1).strip()
        if ' - ' not in name_desc:
            name_desc = f"{man_name} - {name_desc}"
        content = re.sub(r'@top\s+.*', r'.SH NAME\n' + name_desc, content, count=1)

    content = re.sub(r'@chapter\s+(.*)', lambda m: f'.SH {m.group(1).upper()}', content)
    content = re.sub(r'@section\s+(.*)', lambda m: f'.SH {m.group(1).upper()}', content)
    content = re.sub(r'@subsection\s+(.*)', r'.SS \1', content)
    content = re.sub(r'@table\s+(@\w+)\n(.*?)\n@end table', _texi_handle_table, content, flags=re.DOTALL)
    content = re.sub(r'@table\s+(@asis)\n(.*?)\n@end table', _texi_handle_table, content, flags=re.DOTALL)
    content = re.sub(r'@itemize\s+@bullet\n(.*?)\n@end itemize', _texi_handle_itemize, content, flags=re.DOTALL)

    content = content.replace('@@', '\x03')
    content = content.replace('@{', '\x01')
    content = content.replace('@}', '\x02')
    content = re.sub(r'@code\{(.*?)\}', r'\\fB\1\\fP', content)
    content = re.sub(r'@command\{(.*?)\}', r'\\fB\1\\fP', content)
    content = re.sub(r'@file\{(.*?)\}', r'\\fI\1\\fP', content)
    content = re.sub(r'@var\{(.*?)\}', r'\\fI\1\\fP', content)
    content = re.sub(r'@emph\{(.*?)\}', r'\\fI\1\\fP', content)
    content = re.sub(r'@samp\{(.*?)\}', r'\\fI\1\\fP', content)
    content = re.sub(r'@option\{(.*?)\}', r'\\fB\1\\fP', content)
    content = re.sub(r'@env\{(.*?)\}', r'\\fB\1\\fP', content)
    content = re.sub(r'@ref\{(.*?)\}', handle_ref, content)
    content = content.replace('\x01', '{')
    content = content.replace('\x02', '}')
    content = content.replace('\x03', '@')
    content = re.sub(r'@example\n(.*?)\n@end example', r'.nf\n\1\n.fi', content, flags=re.DOTALL)
    content = re.sub(r'@bye.*', '', content, flags=re.DOTALL)
    content = re.sub(r'^Next:.*$', '', content, flags=re.MULTILINE)
    content = re.sub(r'^Previous:.*$', '', content, flags=re.MULTILINE)
    content = re.sub(r'^Up:.*$', '', content, flags=re.MULTILINE)
    content = re.sub(r'^\s*\[Contents\]\s*\[Index\]\s*$', '', content, flags=re.MULTILINE)
    content = re.sub(r'\n{3,}', '\n\n', content)
    out.append(content.strip())
    if copying:
        out.append(".SH LICENSE")
        out.append(copying)
    return "\n".join(out)

def convert_md(texi_path, title):
    """Convert a Texinfo file to Markdown string."""
    with open(texi_path, "r") as f:
        content = f.read()

    content = re.sub(r'\\input texinfo.*?\n', '', content)
    content = re.sub(r'@setfilename.*?\n', '', content)
    content = re.sub(r'@settitle.*?\n', '', content)
    content = re.sub(r'@copying\n.*?\n@end copying', '', content, flags=re.DOTALL)
    content = re.sub(r'@node.*?\n', '', content)
    content = re.sub(r'@menu\n.*?\n@end menu', '', content, flags=re.DOTALL)
    content = re.sub(r'@dircategory.*?\n', '', content)
    content = re.sub(r'@direntry\n.*?\n@end direntry', '', content, flags=re.DOTALL)
    content = re.sub(r'@titlepage\n.*?\n@end titlepage', '', content, flags=re.DOTALL)
    content = re.sub(r'@contents\n', '', content)
    content = re.sub(r'@appendix.*?\n', '', content)
    content = re.sub(r'@printindex.*?\n', '', content)
    content = re.sub(r'@top\s+.*\n', f'# {title}\n', content)
    content = re.sub(r'@chapter\s+(.*)', r'# \1', content)
    content = re.sub(r'@section\s+(.*)', r'## \1', content)
    content = re.sub(r'@subsection\s+(.*)', r'### \1', content)
    content = re.sub(r'@table\s+(@\w+)\n(.*?)\n@end table',
                     lambda m: re.sub(r'@item\s+(.*)', r'- `\1`', m.group(2).strip()) + "\n",
                     content, flags=re.DOTALL)
    content = re.sub(r'@itemize\s+@bullet\n(.*?)\n@end itemize',
                     lambda m: re.sub(r'@item\s+', r'- ', m.group(1).strip()),
                     content, flags=re.DOTALL)
    content = content.replace('@@', '\x03')
    content = content.replace('@{', '\x01')
    content = content.replace('@}', '\x02')
    content = re.sub(r'@code\{(.*?)\}', r'`\1`', content)
    content = re.sub(r'@command\{(.*?)\}', r'`\1`', content)
    content = re.sub(r'@file\{(.*?)\}', r'`\1`', content)
    content = re.sub(r'@var\{(.*?)\}', r'`\1`', content)
    content = re.sub(r'@emph\{(.*?)\}', r'*\1*', content)
    content = re.sub(r'@samp\{(.*?)\}', r'`\1`', content)
    content = re.sub(r'@option\{(.*?)\}', r'`\1`', content)
    content = re.sub(r'@env\{(.*?)\}', r'`\1`', content)
    content = re.sub(r'@ref\{(.*?)\}', r'\1', content)
    content = content.replace('\x01', '{')
    content = content.replace('\x02', '}')
    content = content.replace('\x03', '@')
    content = re.sub(r'@example\n(.*?)\n@end example', r'```text\n\1\n```', content, flags=re.DOTALL)
    content = re.sub(r'@bye.*', '', content, flags=re.DOTALL)
    content = re.sub(r'\n{3,}', '\n\n', content)
    content = re.sub(r'\n([#]{1,3} )', r'\n\n\1', content)
    return content.strip() + "\n"

def run_conv(input_path, name, format="man", section="1"):
    """Dispatch texi conversion. format ∈ {'man', 'md'}."""
    if format == "md":
        return convert_md(input_path, name)
    return convert_man(input_path, name, section)


# ══════════════════════════════════════════════════════════════════════════════
#  C → NYTRIX CONVERSION  (c → extern fn)
# ══════════════════════════════════════════════════════════════════════════════

# C → Ny type table ──────────────────────────────────────────────────────────
C_TO_NY: dict[str, str] = {
    "void": "", "void*": "ptr",
    "int": "i32", "unsigned": "u32", "unsigned int": "u32",
    "signed int": "i32", "long": "i64", "unsigned long": "u64",
    "long long": "i64", "unsigned long long": "u64",
    "short": "i16", "unsigned short": "u16",
    "char": "i8", "unsigned char": "u8", "signed char": "i8",
    "int8_t": "i8",   "uint8_t": "u8",
    "int16_t": "i16", "uint16_t": "u16",
    "int32_t": "i32", "uint32_t": "u32",
    "int64_t": "i64", "uint64_t": "u64",
    "size_t": "u64", "ptrdiff_t": "i64",
    "intptr_t": "i64", "uintptr_t": "u64",
    "float": "f32", "double": "f64",
    "_Bool": "i32", "bool": "i32",
    # X11
    "Display": "ptr", "Window": "u64", "Atom": "u64", "Cursor": "u64",
    "Pixmap": "u64", "XID": "u64", "VisualID": "u64", "Drawable": "u64",
    # Win32
    "HWND": "ptr", "HDC": "ptr", "HINSTANCE": "ptr", "HICON": "ptr",
    "HCURSOR": "ptr", "HMENU": "ptr", "HANDLE": "ptr",
    "DWORD": "u32", "WORD": "u16", "BYTE": "u8", "BOOL": "i32",
    "LPVOID": "ptr", "LPCWSTR": "ptr", "LPCSTR": "ptr",
    "WPARAM": "u64", "LPARAM": "i64", "LRESULT": "i64",
    # Cocoa / ObjC
    "id": "ptr", "SEL": "ptr", "Class": "ptr", "IMP": "ptr",
    "CGFloat": "f64", "NSInteger": "i64", "NSUInteger": "u64",
    "CGDirectDisplayID": "u32",
    # Wayland
    "wl_display": "ptr", "wl_registry": "ptr", "wl_surface": "ptr",
    "wl_compositor": "ptr", "wl_buffer": "ptr",
}

_C_QUALIFIERS = re.compile(
    r'\b(const|volatile|restrict|__restrict|inline|extern|static'
    r'|__declspec\s*\([^)]*\)|__attribute__\s*\([^)]*\)'
    r'|GLFWAPI|RGFWDEF|WINAPI|APIENTRY|__cdecl|__stdcall'
    r'|__forceinline|_Nonnull|_Nullable)\b'
)

_NY_RESERVED = {'in', 'out', 'type', 'len', 'mod', 'use', 'def', 'mut',
                'fn', 'if', 'else', 'return', 'while', 'for', 'true', 'false'}

_C_SIZES: dict[str, int] = {
    'char': 1, 'short': 2, 'int': 4, 'long': 8, 'float': 4, 'double': 8,
    'uint8_t': 1, 'uint16_t': 2, 'uint32_t': 4, 'uint64_t': 8,
    'int8_t': 1, 'int16_t': 2, 'int32_t': 4, 'int64_t': 8,
    'size_t': 8, 'ptrdiff_t': 8,
}

_RE_C_FUNC = re.compile(
    r'''
    (?:(?:extern|static|inline|__inline|GLFWAPI|RGFWDEF|WINAPI|APIENTRY
         |__cdecl|__stdcall|__forceinline
         |__declspec\s*\([^)]*\)
         |__attribute__\s*\([^)]*\))\s*)*
    ([\w\s\*]+?)               # return type  (group 1)
    [ \t]+
    (?:\*[ \t]*)?              # optional pointer-to-fn prefix
    ([A-Za-z_]\w*)             # function name (group 2)
    [ \t]*
    \(([^)]*)\)                # params       (group 3)
    [ \t]*;
    ''',
    re.VERBOSE | re.MULTILINE
)


def _strip_c_qualifiers(s: str) -> str:
    s = _C_QUALIFIERS.sub('', s)
    return re.sub(r'\s+', ' ', s).strip()

def c_to_ny_type(ctype: str) -> str:
    """Convert a raw C type string (may include qualifiers, stars) to a Ny type."""
    s = _strip_c_qualifiers(ctype)
    stars = s.count('*')
    s = s.replace('*', '').strip()
    if stars:
        return "ptr"
    if s in C_TO_NY:
        t = C_TO_NY[s]
        return t if t else "ptr"
    for key in sorted(C_TO_NY, key=len, reverse=True):
        if re.fullmatch(re.escape(key), s, re.IGNORECASE):
            t = C_TO_NY[key]
            return t if t else "ptr"
    return "ptr"  # unknown struct/typedef

def parse_c_params(raw: str) -> list[tuple[str, str]]:
    """Return [(ny_type, ny_name), ...] from a raw C parameter list string."""
    raw = raw.strip()
    if not raw or raw == 'void':
        return []
    depth, buf, segments = 0, [], []
    for ch in raw:
        if ch == '(':  depth += 1
        elif ch == ')': depth -= 1
        if ch == ',' and depth == 0:
            segments.append(''.join(buf).strip())
            buf = []
        else:
            buf.append(ch)
    if buf:
        segments.append(''.join(buf).strip())

    result = []
    for idx, seg in enumerate(segments):
        seg = seg.strip()
        if not seg or seg == '...':
            continue
        if '(' in seg:                          # function pointer param → ptr
            result.append(('ptr', f'fn{idx}'))
            continue
        seg_clean = _strip_c_qualifiers(seg)
        tokens = re.findall(r'\b[A-Za-z_]\w*\b', seg_clean.replace('*', ' '))
        if not tokens:
            result.append(('ptr', f'a{idx}'))
            continue
        name = tokens[-1]
        raw_type = re.sub(r'\b' + re.escape(name) + r'\b', '', seg_clean).strip()
        if not raw_type:
            raw_type = name
            name = f'a{idx}'
        raw_type += '*' * seg.count('*')
        ny_type = c_to_ny_type(raw_type)
        if name in _NY_RESERVED:
            name = '_' + name
        result.append((ny_type, name))
    return result

def struct_layout_comment(fields_raw: str, struct_name: str) -> str:
    """Emit a Ny comment block with byte offsets for struct fields (x86-64)."""
    lines = [f";; {struct_name} layout (x86-64, all pointers = 8 bytes):"]
    offset = 0
    for field in fields_raw.strip().split(';'):
        field = field.strip()
        if not field:
            continue
        field = _strip_c_qualifiers(field)
        stars = field.count('*')
        field = field.replace('*', '').strip()
        toks = field.split()
        if len(toks) < 2:
            continue
        fname = toks[-1]
        ftype = ' '.join(toks[:-1])
        if stars:
            size, ny_t = 8, "ptr"
        else:
            size  = _C_SIZES.get(ftype, 4)
            ny_t  = c_to_ny_type(ftype)
        align = min(size, 8)
        if offset % align:
            offset += align - (offset % align)
        lines.append(f";;   +{offset:<4} {ny_t:<6}  {fname}")
        offset += size
    lines.append(f";;   total: ~{offset} bytes")
    return '\n'.join(lines)

def preprocess_c(src: str) -> str:
    """Remove C comments, preprocessor directives, and string literals."""
    src = re.sub(r'/\*.*?\*/', ' ', src, flags=re.DOTALL)
    src = re.sub(r'//[^\n]*', '', src)
    src = re.sub(r'"[^"\\]*(?:\\.[^"\\]*)*"', '""', src)
    src = re.sub(r'^\s*#[^\n]*', '', src, flags=re.MULTILINE)
    return src



# ══════════════════════════════════════════════════════════════════════════════
#  CONVERT C → NY  (main conversion loop)
# ══════════════════════════════════════════════════════════════════════════════

def convert_c_to_ny(
    src: str,
    filter_prefix: str = '',
    strip_prefix: str = '',
    emit_structs: bool = False,
) -> tuple[list[str], dict]:
    """
    Convert preprocessed C source to Nytrix extern fn lines.
    Returns (lines, stats).
    """
    lines: list[str] = []
    stats = {'functions': 0, 'skipped': 0, 'structs': 0}

    # Structs (optional)
    if emit_structs:
        for m in re.finditer(
            r'typedef\s+struct\s+\w*\s*\{([^}]*)\}\s*(\w+)\s*;', src, re.DOTALL
        ):
            name = m.group(2)
            if filter_prefix and not name.startswith(filter_prefix):
                continue
            lines.append(struct_layout_comment(m.group(1), name))
            lines.append('')
            stats['structs'] += 1

    # Functions
    seen: set[str] = set()
    for m in _RE_C_FUNC.finditer(src):
        ret_raw    = m.group(1).strip()
        name       = m.group(2).strip()
        params_raw = m.group(3)

        if filter_prefix and not name.startswith(filter_prefix):
            stats['skipped'] += 1
            continue
        if name in seen:
            continue
        seen.add(name)

        ny_name = name
        if strip_prefix and ny_name.startswith(strip_prefix):
            ny_name = ny_name[len(strip_prefix):]

        ret_clean = _strip_c_qualifiers(ret_raw).replace('*', '').strip()
        is_void   = ret_clean == 'void' and '*' not in ret_raw
        ny_ret    = '' if is_void else c_to_ny_type(ret_raw)
        params    = parse_c_params(params_raw)

        param_str = ', '.join(f'{pname}: {ptype}' for ptype, pname in params)
        ret_str   = f': {ny_ret}' if ny_ret else ''
        lines.append(f'   extern fn {ny_name}({param_str}){ret_str}')
        stats['functions'] += 1

    return lines, stats


# ══════════════════════════════════════════════════════════════════════════════
#  UNIFIED CLI
# ══════════════════════════════════════════════════════════════════════════════

def _main_c(argv=None):
    """Entry point for the `c` subcommand (also called by the c_to_ny.py shim)."""
    import argparse, pathlib, sys as _sys
    ap = argparse.ArgumentParser(
        prog='conv.py c',
        description='C header → Nytrix extern fn declarations',
    )
    ap.add_argument('files', nargs='*', help='C header files to process')
    ap.add_argument('--stdin',    action='store_true', help='read from stdin')
    ap.add_argument('--filter',   default='', metavar='PREFIX',
                    help='only emit names starting with PREFIX')
    ap.add_argument('--strip',    default='', metavar='PREFIX',
                    help='strip PREFIX from emitted Ny names')
    ap.add_argument('--link',     default='', metavar='LIB',
                    help='emit  #link "LIB"  at top')
    ap.add_argument('--platform', default='', metavar='P',
                    choices=['linux', 'windows', 'macos', ''],
                    help='wrap output in comptime platform guard')
    ap.add_argument('--structs',  action='store_true',
                    help='also emit struct layout comments')
    ap.add_argument('--out',      default='', metavar='FILE',
                    help='write output to FILE instead of stdout')
    ap.add_argument('--dry-run',  action='store_true',
                    help='parse only; print stats, no output')
    args = ap.parse_args(argv)

    # Collect sources
    sources: list[tuple[str, str]] = []
    if args.stdin or not args.files:
        sources.append(('stdin', _sys.stdin.read()))
    for fpath in args.files:
        p = pathlib.Path(fpath)
        if not p.exists():
            print(f'[warn] not found: {fpath}', file=_sys.stderr)
            continue
        sources.append((p.name, p.read_text(errors='replace')))

    indent = '   ' if args.platform else ''
    total  = {'functions': 0, 'skipped': 0, 'structs': 0}
    out_lines = [';; Auto-generated by conv.py c']
    if args.files:
        out_lines.append(f';; Sources: {", ".join(args.files)}')
    out_lines.append('')

    if args.platform:
        out_lines.append(f'if(comptime{{ __os_name() == "{args.platform}" }}){{')
    if args.link:
        out_lines.append(f'{indent}#link "{args.link}"')
        out_lines.append('')

    for fname, src in sources:
        clean = preprocess_c(src)
        fn_lines, stats = convert_c_to_ny(
            clean,
            filter_prefix=args.filter,
            strip_prefix=args.strip,
            emit_structs=args.structs,
        )
        for k in stats:
            total[k] += stats[k]
        if fn_lines:
            out_lines.append(f'{indent};; ── {fname} ──')
            for ln in fn_lines:
                out_lines.append(f'{indent}{ln.strip()}' if indent else ln)
            out_lines.append('')

    if args.platform:
        out_lines.append('}')

    print(
        f'[conv c] functions={total["functions"]}  '
        f'structs={total["structs"]}  skipped={total["skipped"]}',
        file=_sys.stderr,
    )
    if args.dry_run:
        return

    output = '\n'.join(out_lines)
    if args.out:
        pathlib.Path(args.out).write_text(output)
        print(f'[conv c] wrote {args.out}', file=_sys.stderr)
    else:
        print(output)


def _main_texi(argv=None):
    """Entry point for the `texi` subcommand."""
    import argparse, pathlib, sys as _sys
    ap = argparse.ArgumentParser(
        prog='conv.py texi',
        description='Texinfo → man page or Markdown',
    )
    ap.add_argument('input', help='path to .texi file')
    ap.add_argument('--name',    required=True, metavar='NAME',
                    help='tool name used in .TH header / # title')
    ap.add_argument('--format',  default='man', choices=['man', 'md'],
                    help='output format (default: man)')
    ap.add_argument('--section', default='1', metavar='N',
                    help='man page section (default: 1)')
    ap.add_argument('--out',     default='', metavar='FILE',
                    help='write to FILE instead of stdout')
    args = ap.parse_args(argv)

    result = run_conv(args.input, args.name, format=args.format, section=args.section)
    if args.out:
        pathlib.Path(args.out).write_text(result)
        print(f'[conv texi] wrote {args.out}', file=_sys.stderr)
    else:
        print(result)


# ══════════════════════════════════════════════════════════════════════════════
#  BIND ENGINE — full .ny module generator
# ══════════════════════════════════════════════════════════════════════════════

# ── Regex helpers ─────────────────────────────────────────────────────────────

# Object-like macro:  #define NAME   value
_RE_MACRO_OBJ = re.compile(
    r'^\s*#\s*define\s+'
    r'([A-Za-z_]\w*)'            # group 1: name
    r'\s+'
    r'([^\\\n]+)',               # group 2: value (single line, not a fn-macro)
    re.MULTILINE,
)

# Function-like macro: #define NAME(args...) body
_RE_MACRO_FN = re.compile(
    r'^\s*#\s*define\s+'
    r'([A-Za-z_]\w*)'
    r'\(([^)]*)\)'               # group 2: param list
    r'\s*'
    r'([^\\\n]*(?:\\\n[^\\\n]*)*)',  # group 3: body (possibly multi-line with \)
    re.MULTILINE,
)

# Typedef struct / anonymous struct with tag
_RE_STRUCT = re.compile(
    r'typedef\s+struct\s*(?P<tag>\w+)?\s*\{(?P<body>[^}]*)\}\s*(?P<alias>\w+)\s*;',
    re.DOTALL,
)

# Enum  typedef enum { ... } Name;
_RE_ENUM = re.compile(
    r'typedef\s+enum\s*(?:\w+)?\s*\{(?P<body>[^}]*)\}\s*(?P<alias>\w+)\s*;',
    re.DOTALL,
)

# Variadic function (has ... in params)
_RE_VARIADIC = re.compile(r'\.\.\.')


def _is_numeric(s: str) -> bool:
    """True if s looks like a C numeric literal (single token, no whitespace)."""
    s = s.strip()
    if ' ' in s and not re.match(r'^[0-9A-Fa-fx+\-*/^|&~()\s]+$', s):
        return False
    s = s.rstrip('uUlLfF')
    try:
        float(s)
        return True
    except ValueError:
        pass
    try:
        int(s, 0)
        return True
    except ValueError:
        return False


def _eval_macro_value(raw: str) -> str | None:
    """
    Try to convert a C macro value to a Nytrix literal.
    Returns None when the value is too complex or looks like a comment/description.
    """
    v = raw.strip().rstrip(';').strip()
    # Strip outer parens
    while v.startswith('(') and v.endswith(')'):
        inner = v[1:-1].strip()
        # Make sure parens were balanced
        if inner.count('(') == inner.count(')'):
            v = inner
        else:
            break

    # Reject if it looks like a prose description (multiple words, no ops)
    words = v.split()
    if len(words) > 4 and not re.search(r'[+\-*/|&^<>~]', v):
        return None
    # Reject if it starts with a word character followed by another word
    # (looks like macro-name + description rather than expression)
    if re.match(r'^[A-Za-z_]\w*\s+[a-z]', v):
        return None

    # String literal → keep as-is
    if v.startswith('"') or v.startswith("'"):
        return v

    # Simple cast like (int)42 or (unsigned)0xFF or (RGFW_bool)1 → strip cast
    cast = re.match(
        r'^\((?:(?:unsigned|signed|const)\s+)?[A-Za-z_]\w*\s*\*?\)\s*(.+)$', v
    )
    if cast:
        inner = cast.group(1).strip()
        if _is_numeric(inner) or inner in ('0', '1'):
            v = inner

    # Hex / decimal / float (single token)
    if _is_numeric(v):
        clean = v.rstrip('uUlLfF')
        return clean if clean else v

    # C bit/arithmetic expression — convert ops to Nytrix equivalents
    # Only accept if it's purely made of: digits, hex, names, parens, ops
    if re.match(r'^[0-9A-Za-z_\s|&^~<>+\-*/()x]+$', v):
        # Reject if it has too many alphabetic words (description, not expression)
        alpha_words = re.findall(r'[A-Za-z_]\w*', v)
        # Allow known numeric suffixes and type names
        _allowed = {'u','l','f','ul','ll','ull','true','false','NULL','nullptr'}
        non_trivial = [w for w in alpha_words if w.lower() not in _allowed]
        if len(non_trivial) > 2:
            return None  # looks like an expression involving other macros — skip
        ny = v
        ny = re.sub(r'<<', ' bshl ', ny)
        ny = re.sub(r'>>', ' bshr ', ny)
        ny = re.sub(r'\|', ' bor ', ny)
        ny = re.sub(r'&(?![&])', ' band ', ny)
        ny = re.sub(r'\s+', ' ', ny).strip()
        return ny

    return None  # too complex


def extract_macros(raw_src: str) -> list[tuple[str, str, str]]:
    """
    Extract C macros from raw (un-preprocessed) source.
    Returns list of (kind, name, value_or_body) where kind ∈ {'const','fn','skip'}.
    """
    results: list[tuple[str, str, str]] = []
    seen: set[str] = set()

    # Function-like macros first (more specific regex)
    for m in _RE_MACRO_FN.finditer(raw_src):
        name = m.group(1)
        params = m.group(2)
        body = re.sub(r'\\\n', ' ', m.group(3)).strip()
        if name in seen:
            continue
        seen.add(name)
        results.append(('fn', name, f'({params}) → {body}'))

    # Object-like macros
    for m in _RE_MACRO_OBJ.finditer(raw_src):
        name = m.group(1)
        val_raw = m.group(2).strip()
        if name in seen:
            continue
        # Skip include guards, _H suffixes, etc.
        if name.endswith('_H') or name.endswith('_H_') or name.startswith('__'):
            continue
        seen.add(name)
        ny_val = _eval_macro_value(val_raw)
        if ny_val is not None:
            results.append(('const', name, ny_val))
        else:
            results.append(('skip', name, val_raw))

    return results


def _expand_multi_decl(body: str) -> str:
    """
    Expand C struct multi-declarations like  `i32 r, g, b, a;`
    into individual declarations `i32 r; i32 g; i32 b; i32 a;`.
    """
    result = []
    for stmt in body.split(';'):
        stmt = stmt.strip()
        if not stmt:
            continue
        # Match: optional qualifiers + base_type + name1, name2, ...
        m = re.match(
            r'^((?:(?:const|volatile|unsigned|signed|struct|enum|union)\s+)*'
            r'(?:[A-Za-z_]\w*(?:\s+[A-Za-z_]\w*)*))'  # base type (may include typedef name)
            r'\s*\*?\s*'
            r'([A-Za-z_]\w*(?:\s*,\s*\*?[A-Za-z_]\w*)+)$',  # name1, name2, ...
            stmt,
        )
        if m:
            base_type = m.group(1).strip()
            names = [n.strip().lstrip('*').strip() for n in m.group(2).split(',')]
            for n in names:
                result.append(f'{base_type} {n}')
        else:
            result.append(stmt)
    return ', '.join(result)


def extract_structs(src: str) -> list[dict]:
    """
    Extract typedef structs from preprocessed source.
    Returns list of {name, fields: [(ny_type, field_name), ...]}.
    Handles multi-declaration fields like  `i32 x, y, w, h;`.
    """
    structs = []
    for m in _RE_STRUCT.finditer(src):
        name = m.group('alias')
        body = m.group('body')
        expanded = _expand_multi_decl(body)
        fields = parse_c_params(expanded)
        structs.append({'name': name, 'fields': fields})
    return structs


def _eval_c_int(expr: str, known: dict[str, int] | None = None) -> int | None:
    """Try to evaluate a simple C integer expression. Returns None if too complex."""
    s = expr.strip().rstrip('uUlLfF')
    # Strip parens
    while s.startswith('(') and s.endswith(')'):
        inner = s[1:-1].strip()
        if inner.count('(') == inner.count(')'):
            s = inner
        else:
            break
    # Direct int literal
    try:
        return int(s, 0)
    except ValueError:
        pass
    # Known macro name
    if known and s in known:
        return known[s]
    # Simple binary expression: a OP b
    for op, fn in [(' << ', lambda a, b: a << b),
                   (' >> ', lambda a, b: a >> b),
                   (' | ', lambda a, b: a | b),
                   (' & ', lambda a, b: a & b),
                   (' + ', lambda a, b: a + b),
                   (' - ', lambda a, b: a - b),
                   (' * ', lambda a, b: a * b)]:
        if op in s:
            parts = s.split(op, 1)
            a = _eval_c_int(parts[0], known)
            b = _eval_c_int(parts[1], known)
            if a is not None and b is not None:
                try:
                    return fn(a, b)
                except Exception:
                    return None
    # Unary ~
    if s.startswith('~'):
        inner = _eval_c_int(s[1:], known)
        return (~inner) & 0xFFFFFFFFFFFFFFFF if inner is not None else None
    return None


def extract_enums(src: str) -> list[dict]:
    """
    Extract typedef enums → list of {name, values: [(member, int_val)]}.
    Evaluates expressions like  (1 << 3)  and tracks running counter.
    """
    enums = []
    for m in _RE_ENUM.finditer(src):
        alias = m.group('alias')
        body  = m.group('body')
        values: list[tuple[str, int]] = []
        counter = 0
        known: dict[str, int] = {}

        for entry in body.split(','):
            entry = entry.strip()
            if not entry:
                continue
            if '=' in entry:
                parts  = entry.split('=', 1)
                member = parts[0].strip()
                rhs    = parts[1].strip()
                val    = _eval_c_int(rhs, known)
                counter = val if val is not None else counter
            else:
                member = entry
            if re.match(r'^[A-Za-z_]\w*$', member):
                values.append((member, counter))
                known[member] = counter
            counter += 1

        enums.append({'name': alias, 'values': values})
    return enums

def _has_variadic(params_raw: str) -> bool:
    return bool(_RE_VARIADIC.search(params_raw))


def _ny_sanitize_name(name: str) -> str:
    """Make a C name safe as a Nytrix identifier."""
    _NY_RESERVED_BIND = {
        'in','out','type','len','mod','use','def','mut','fn','if','else',
        'return','while','for','true','false','and','or','not','end',
    }
    if name in _NY_RESERVED_BIND:
        return '_' + name
    return name


# ── Wrapper generator ─────────────────────────────────────────────────────────

def _build_typed_wrapper(
    ny_name: str,
    orig_c_name: str,
    params: list[tuple[str, str]],  # [(ny_type, ny_name), ...]
    ny_ret: str,
    is_variadic: bool,
    strip_prefix: str,
) -> list[str]:
    """
    Build a Nytrix wrapper function that calls through _lib (dlopen handle).

    Typed variant:
        fn foo(a: i32, b: ptr): i32 {
            def _f = dlsym(_lib, "foo")
            call2(_f, a, b)
        }

    Variadic variant (C '...' args):
        fn foo(fmt: ptr, ...args): ptr {
            def _f = dlsym(_lib, "foo")
            ffi_call(_f, [fmt] + list(args))
        }
    """
    lines = []

    # Build parameter signature
    if is_variadic:
        fixed_params = [f'{pname}: {ptype}' for ptype, pname in params]
        sig_parts = fixed_params + ['...args']
    else:
        sig_parts = [f'{pname}: {ptype}' for ptype, pname in params]

    ret_str = f': {ny_ret}' if ny_ret else ''
    lines.append(f'fn {ny_name}({", ".join(sig_parts)}){ret_str}{{')
    lines.append(f'   def _f = dlsym(_lib, "{orig_c_name}")')

    if is_variadic:
        fixed_arg_list = '[' + ', '.join(pname for _, pname in params) + ']'
        if params:
            lines.append(f'   ffi_call(_f, {fixed_arg_list} + list(args))')
        else:
            lines.append(f'   ffi_call(_f, list(args))')
    else:
        n = len(params)
        arg_list = ', '.join(f'_f' + ', ' + ', '.join(pname for _, pname in params) if i == 0 else '' for i, _ in enumerate([None]))
        # Use explicit callN
        args_str = ', '.join(pname for _, pname in params)
        call = f'call{n}(_f, {args_str})' if args_str else f'call0(_f)'
        if ny_ret:
            lines.append(f'   {call}')
        else:
            lines.append(f'   {call}')

    lines.append('}')
    return lines


def generate_bind_module(
    sources: list[tuple[str, str]],   # [(filename, raw_source), ...]
    module_name: str,
    lib_name: str,
    filter_prefix: str = '',
    strip_prefix: str = '',
    emit_structs: bool = True,
    emit_enums: bool = True,
    emit_macros: bool = True,
    emit_ffi_wrappers: bool = True,   # dynamic dlopen wrappers
    emit_static_externs: bool = False, # also emit static extern fn block
    platform: str = '',
) -> tuple[str, dict]:
    """
    Generate a complete Nytrix binding module.
    Returns (source_string, stats).
    """
    stats: dict = {
        'functions': 0, 'variadic': 0, 'macros_const': 0,
        'macros_fn': 0, 'macros_skip': 0, 'structs': 0, 'enums': 0,
    }

    L: list[str] = []  # output lines

    # ── Header ───────────────────────────────────────────────────────────────
    L.append(f';; Auto-generated by conv.py bind')
    L.append(f';; Sources: {", ".join(f for f, _ in sources)}')
    if lib_name:
        L.append(f';; Library: {lib_name}')
    L.append('')

    # Collect everything first so we can build the module export list
    all_consts:  list[tuple[str, str]]        = []  # (ny_name, ny_val)
    all_enums_e: list[dict]                   = []
    all_structs: list[dict]                   = []
    all_fns:     list[tuple]                  = []  # (ny_name, orig, params, ny_ret, is_var)
    all_macro_fns: list[tuple[str, str, str]] = []  # (name, params_raw, body)

    seen_fns: set[str] = set()

    for fname, raw in sources:
        # Extract macros from RAW source (before stripping #define)
        if emit_macros:
            for kind, mname, mval in extract_macros(raw):
                if filter_prefix and not mname.startswith(filter_prefix):
                    continue
                ny_name = mname
                if strip_prefix and ny_name.startswith(strip_prefix):
                    ny_name = ny_name[len(strip_prefix):]
                ny_name = _ny_sanitize_name(ny_name)
                if kind == 'const':
                    all_consts.append((ny_name, mval))
                    stats['macros_const'] += 1
                elif kind == 'fn':
                    all_macro_fns.append((ny_name, mname, mval))
                    stats['macros_fn'] += 1
                else:
                    stats['macros_skip'] += 1

        clean = preprocess_c(raw)

        # Structs
        if emit_structs:
            for s in extract_structs(clean):
                if filter_prefix and not s['name'].startswith(filter_prefix):
                    continue
                all_structs.append(s)
                stats['structs'] += 1

        # Enums
        if emit_enums:
            for e in extract_enums(clean):
                all_enums_e.append(e)
                stats['enums'] += 1

        # Functions
        for m in _RE_C_FUNC.finditer(clean):
            ret_raw    = m.group(1).strip()
            orig_name  = m.group(2).strip()
            params_raw = m.group(3)

            if filter_prefix and not orig_name.startswith(filter_prefix):
                continue
            if orig_name in seen_fns:
                continue
            seen_fns.add(orig_name)

            ny_name = orig_name
            if strip_prefix and ny_name.startswith(strip_prefix):
                ny_name = ny_name[len(strip_prefix):]
            ny_name = _ny_sanitize_name(ny_name)

            ret_clean = _strip_c_qualifiers(ret_raw).replace('*', '').strip()
            is_void   = ret_clean == 'void' and '*' not in ret_raw
            ny_ret    = '' if is_void else c_to_ny_type(ret_raw)
            is_var    = _has_variadic(params_raw)
            # Strip '...' from params before parsing
            params_clean = re.sub(r',?\s*\.\.\.', '', params_raw)
            params = parse_c_params(params_clean)

            all_fns.append((ny_name, orig_name, params, ny_ret, is_var))
            stats['functions'] += 1
            if is_var:
                stats['variadic'] += 1

    # ── module declaration ────────────────────────────────────────────────────
    exports: list[str] = []

    if lib_name:
        exports.append('load, unload, is_loaded')
    exports += [name for name, _ in all_consts]
    for e in all_enums_e:
        exports += [v for v, _ in e['values']]
    for s in all_structs:
        sn = s['name']
        if filter_prefix and not sn.startswith(filter_prefix):
            continue
        ny_sn = sn[len(strip_prefix):] if strip_prefix and sn.startswith(strip_prefix) else sn
        exports.append(f'{ny_sn}_new')
        exports += [f'{ny_sn}_get_{fn}' for _, fn in s['fields']]
        exports += [f'{ny_sn}_set_{fn}' for _, fn in s['fields']]
    exports += [ny_name for ny_name, *_ in all_fns]

    # Deduplicate preserving order
    seen_ex: set[str] = set()
    unique_exports: list[str] = []
    for e in exports:
        if e not in seen_ex:
            seen_ex.add(e)
            unique_exports.append(e)

    L.append(f'module {module_name} (')
    # Wrap exports at ~80 chars
    line_buf, line_len = '   ', 3
    for i, ex in enumerate(unique_exports):
        sep = ', ' if i < len(unique_exports) - 1 else ''
        chunk = ex + sep
        if line_len + len(chunk) > 90 and line_len > 3:
            L.append(line_buf)
            line_buf, line_len = '   ' + chunk, 3 + len(chunk)
        else:
            line_buf += chunk
            line_len += len(chunk)
    if line_buf.strip():
        L.append(line_buf)
    L.append(')')
    L.append('')
    L.append('use std.core *')
    L.append('use std.os.ffi *')
    L.append('')

    # ── Library loader ────────────────────────────────────────────────────────
    if lib_name:
        L.append(';; ── Library handle ──────────────────────────────────────────────────────────')
        L.append('mut _lib = 0')
        L.append('mut _sym = dict()')   # symbol cache
        L.append('')
        # Platform-aware lib name
        if platform:
            L.append(f'def _LIB_NAME = if(comptime{{__os_name()=="windows"}}){{ "{lib_name}.dll" }}')
            L.append(f'               elif(comptime{{__os_name()=="macos"}}){{ "lib{lib_name}.dylib" }}')
            L.append(f'               else {{ "lib{lib_name}.so" }}')
        else:
            L.append(f'def _LIB_NAME = "{lib_name}"')
        L.append('')
        L.append('fn load(path=""){')
        L.append('   "Open the shared library. Pass an explicit path or leave blank to auto-discover."')
        L.append('   if(_lib){ return true }')
        L.append('   _lib = dlopen_any(if(str_len(path)>0){ path } else { _LIB_NAME }, RTLD_LAZY())')
        L.append('   _sym = dict()')
        L.append('   _lib != 0')
        L.append('}')
        L.append('')
        L.append('fn unload(){')
        L.append('   "Close the shared library handle."')
        L.append('   if(_lib){ dlclose(_lib)   _lib = 0   _sym = dict() }')
        L.append('}')
        L.append('')
        L.append('fn is_loaded(){ _lib != 0 }')
        L.append('')
        L.append(';; ── Symbol resolver (cached) ─────────────────────────────────────────────────')
        L.append('fn _sym_get(name){')
        L.append('   if(dict_has(_sym, name)){ return dict_get(_sym, name, 0) }')
        L.append('   def f = dlsym(_lib, name)')
        L.append('   _sym = dict_set(_sym, name, f)')
        L.append('   f')
        L.append('}')
        L.append('')

    # ── Static #link externs (optional) ──────────────────────────────────────
    if emit_static_externs and all_fns:
        L.append(';; ── Static externs (alternative to dlopen) ─────────────────────────────────')
        if lib_name:
            L.append(f';; #link "{lib_name}"')
        L.append(';; Uncomment to use static linking instead of dlopen:')
        L.append(';;')
        indent = '   ' if platform else ''
        if platform:
            L.append(f';; if(comptime{{__os_name()=="{platform}"}}){{')
        for ny_name, orig, params, ny_ret, is_var in all_fns:
            ret_str = f': {ny_ret}' if ny_ret else ''
            if is_var:
                param_str = ', '.join(f'{pn}: {pt}' for pt, pn in params)
                if param_str:
                    param_str += ', ...'
                else:
                    param_str = '...'
            else:
                param_str = ', '.join(f'{pn}: {pt}' for pt, pn in params)
            L.append(f';;    extern fn {orig}({param_str}){ret_str}')
        if platform:
            L.append(f';;  }}')
        L.append('')

    # ── Macro constants ───────────────────────────────────────────────────────
    if all_consts:
        L.append(';; ── Constants (from #define) ────────────────────────────────────────────────')
        for ny_name, ny_val in all_consts:
            L.append(f'def {ny_name} = {ny_val}')
        L.append('')

    # ── Enums ─────────────────────────────────────────────────────────────────
    if all_enums_e:
        L.append(';; ── Enum values ─────────────────────────────────────────────────────────────')
        for e in all_enums_e:
            L.append(f';; {e["name"]}')
            for member, val in e['values']:
                ny_member = member
                if strip_prefix and ny_member.startswith(strip_prefix):
                    ny_member = ny_member[len(strip_prefix):]
                L.append(f'def {_ny_sanitize_name(ny_member)} = {val}')
        L.append('')

    # ── Struct accessors ──────────────────────────────────────────────────────
    if all_structs:
        L.append(';; ── Struct accessors ────────────────────────────────────────────────────────')
        for s in all_structs:
            sn = s['name']
            ny_sn = sn[len(strip_prefix):] if strip_prefix and sn.startswith(strip_prefix) else sn
            ny_sn = _ny_sanitize_name(ny_sn)
            fields = s['fields']  # [(ny_type, field_name)]
            # Compute offsets using _C_SIZES
            L.append(f';; {sn}')
            total = 0
            offsets: list[tuple[int, str, str]] = []  # (byte_off, ny_type, fname)
            for ny_type, fname in fields:
                size = 8 if ny_type == 'ptr' else _C_SIZES.get(ny_type, 4)
                align = min(size, 8)
                if total % align:
                    total += align - (total % align)
                offsets.append((total, ny_type, fname))
                total += size

            # _new  → malloc + zero
            L.append(f'fn {ny_sn}_new(){{')
            L.append(f'   "Allocate and zero a new {sn} struct."')
            L.append(f'   def p=malloc({total})   memset(p,0,{total})   p')
            L.append(f'}}')

            for off, ny_type, fname in offsets:
                safe_fn = _ny_sanitize_name(fname)

                # Map ny_type → (getter_expr, setter_fn)
                # All Nytrix load/store take (ptr, byte_offset)
                _READ = {
                    'ptr': ('load64', 'store64'),
                    'i64': ('load64', 'store64'),
                    'u64': ('load64', 'store64'),
                    'i32': ('load32', 'store32'),
                    'u32': ('load32', 'store32'),
                    'i16': ('load16', 'store16'),
                    'u16': ('load16', 'store16'),
                    'i8':  ('load8',  'store8'),
                    'u8':  ('load8',  'store8'),
                    'f32': ('load32_f32', 'store32_f32'),
                    'f64': ('load64_f64', 'store64_f64'),
                }
                read_fn, store_fn = _READ.get(ny_type, ('load32', 'store32'))

                L.append(f'fn {ny_sn}_get_{safe_fn}(p){{ {read_fn}(p,{off}) }}')
                L.append(f'fn {ny_sn}_set_{safe_fn}(p,v){{ {store_fn}(p,v,{off}) }}')
            L.append('')

    # ── Macro function wrappers ───────────────────────────────────────────────
    if all_macro_fns:
        L.append(';; ── Macro function wrappers ─────────────────────────────────────────────────')
        L.append(';; These expand C function-like macros as Nytrix functions.')
        for ny_name, orig_c, spec in all_macro_fns:
            # spec is like "(a, b) → body"
            m = re.match(r'\(([^)]*)\)\s*→\s*(.*)', spec)
            if not m:
                L.append(f';; {ny_name}: {spec}  (skipped — complex body)')
                continue
            params_raw = m.group(1)
            body       = m.group(2).strip()
            params = [p.strip() for p in params_raw.split(',') if p.strip()]
            # Try converting simple C body to Nytrix
            ny_body = body
            # Replace token-paste ##
            ny_body = re.sub(r'\s*##\s*', '', ny_body)
            # Replace C logical ops
            ny_body = ny_body.replace('&&', ' and ').replace('||', ' or ').replace('!', 'not ')
            # Replace cast notation (type)x → x
            ny_body = re.sub(r'\([a-zA-Z_ *]+\)', '', ny_body)
            ny_body = ny_body.strip()
            sig = ', '.join(_ny_sanitize_name(p) for p in params)
            L.append(f'fn {ny_name}({sig}){{')
            L.append(f'   ;; C macro: {orig_c}')
            L.append(f'   {ny_body}')
            L.append(f'}}')
        L.append('')

    # ── Function wrappers (dlopen) ────────────────────────────────────────────
    if emit_ffi_wrappers and all_fns:
        L.append(';; ── Function bindings ───────────────────────────────────────────────────────')
        resolver = '_sym_get' if lib_name else 'dlsym(_lib,'
        for ny_name, orig, params, ny_ret, is_var in all_fns:
            ret_str = f': {ny_ret}' if ny_ret else ''
            if is_var:
                fixed = ', '.join(f'{pn}: {pt}' for pt, pn in params)
                sig = (fixed + ', ...args') if fixed else '...args'
            else:
                sig = ', '.join(f'{pn}: {pt}' for pt, pn in params)

            L.append(f'fn {ny_name}({sig}){ret_str}{{')
            # Cached symbol resolve
            if lib_name:
                L.append(f'   def _f = _sym_get("{orig}")')
            else:
                L.append(f'   def _f = dlsym(_lib, "{orig}")')

            if is_var:
                fixed_list = '[' + ', '.join(pn for _, pn in params) + ']'
                if params:
                    L.append(f'   ffi_call(_f, {fixed_list} + list(args))')
                else:
                    L.append(f'   ffi_call(_f, list(args))')
            else:
                n = len(params)
                arg_str = ', '.join(pn for _, pn in params)
                if n == 0:
                    L.append(f'   call0(_f)')
                else:
                    L.append(f'   call{n}(_f, {arg_str})')

            L.append(f'}}')
        L.append('')

    return '\n'.join(L), stats


# ── CLI for bind ───────────────────────────────────────────────────────────────

def _main_bind(argv=None):
    """
    Entry point for `conv.py bind`.

    Generates a complete Nytrix binding module from one or more C headers:
      - Module declaration with all exports
      - dlopen/dlclose loader (load / unload / is_loaded)
      - #define constants → def NAME = val
      - typedef enum members → def NAME = val
      - typedef struct field accessors → _new / _get_FIELD / _set_FIELD
      - Function-like macros → fn wrappers with best-effort body translation
      - C functions → fn wrappers via ffi_call / callN (variadic-aware)
      - Optional static extern fn block (commented out)

    Examples
    --------
    # Full SDL2 binding module
    python3 etc/tools/conv.py bind /usr/include/SDL2/SDL.h \\
        --module my.sdl2 --lib SDL2 --filter SDL --strip SDL_

    # Minimal static-link style (no dlopen, just extern fn)
    python3 etc/tools/conv.py bind glfw3.h \\
        --module my.glfw --lib glfw --filter glfw --strip glfw \\
        --static --no-ffi-wrappers --platform linux

    # Struct + enum only (no functions)
    python3 etc/tools/conv.py bind vulkan/vulkan.h \\
        --module my.vk --filter Vk --strip Vk --no-ffi-wrappers
    """
    import argparse, pathlib, sys as _sys
    ap = argparse.ArgumentParser(
        prog='conv.py bind',
        description='C header → full Nytrix binding module',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=_main_bind.__doc__,
    )
    ap.add_argument('files', nargs='*', help='C header files to process')
    ap.add_argument('--stdin',    action='store_true', help='read header from stdin')
    ap.add_argument('--module',   required=True, metavar='MOD',
                    help='Nytrix module name (e.g. my.glfw)')
    ap.add_argument('--lib',      default='', metavar='LIB',
                    help='shared library name for dlopen (e.g. glfw, SDL2)')
    ap.add_argument('--filter',   default='', metavar='PREFIX',
                    help='only emit names starting with PREFIX')
    ap.add_argument('--strip',    default='', metavar='PREFIX',
                    help='strip PREFIX from Nytrix names')
    ap.add_argument('--platform', default='', metavar='P',
                    choices=['linux', 'windows', 'macos', ''],
                    help='target platform for lib name hints')
    ap.add_argument('--structs',        action='store_true', default=True,
                    help='emit struct accessors (default: on)')
    ap.add_argument('--no-structs',     action='store_false', dest='structs')
    ap.add_argument('--enums',          action='store_true', default=True,
                    help='emit enum constants (default: on)')
    ap.add_argument('--no-enums',       action='store_false', dest='enums')
    ap.add_argument('--macros',         action='store_true', default=True,
                    help='emit #define constants and macro wrappers (default: on)')
    ap.add_argument('--no-macros',      action='store_false', dest='macros')
    ap.add_argument('--ffi-wrappers',   action='store_true', default=True,
                    help='emit dlopen function wrappers (default: on)')
    ap.add_argument('--no-ffi-wrappers',action='store_false', dest='ffi_wrappers')
    ap.add_argument('--static',         action='store_true',
                    help='also emit commented static extern fn block')
    ap.add_argument('--out',      default='', metavar='FILE',
                    help='write to FILE instead of stdout')
    ap.add_argument('--dry-run',  action='store_true',
                    help='parse only; print stats')
    args = ap.parse_args(argv)

    # Collect sources
    sources: list[tuple[str, str]] = []
    if args.stdin or not args.files:
        sources.append(('stdin', _sys.stdin.read()))
    for fpath in args.files:
        p = pathlib.Path(fpath)
        if not p.exists():
            print(f'[warn] not found: {fpath}', file=_sys.stderr)
            continue
        sources.append((p.name, p.read_text(errors='replace')))

    output, stats = generate_bind_module(
        sources,
        module_name       = args.module,
        lib_name          = args.lib,
        filter_prefix     = args.filter,
        strip_prefix      = args.strip,
        emit_structs      = args.structs,
        emit_enums        = args.enums,
        emit_macros       = args.macros,
        emit_ffi_wrappers = args.ffi_wrappers,
        emit_static_externs = args.static,
        platform          = args.platform,
    )

    print(
        f'[conv bind] functions={stats["functions"]}  variadic={stats["variadic"]}  '
        f'macros={stats["macros_const"]}  macro_fns={stats["macros_fn"]}  '
        f'skipped_macros={stats["macros_skip"]}  structs={stats["structs"]}  '
        f'enums={stats["enums"]}',
        file=_sys.stderr,
    )

    if args.dry_run:
        return

    if args.out:
        pathlib.Path(args.out).write_text(output)
        print(f'[conv bind] wrote {args.out}', file=_sys.stderr)
    else:
        print(output)


def main():
    import argparse, sys as _sys
    ap = argparse.ArgumentParser(
        prog='conv.py',
        description=(
            'Nytrix unified conversion tool.\n'
            'Subcommands:\n'
            '  c     C header  → bare extern fn declarations\n'
            '  bind  C header  → full .ny binding module\n'
            '  texi  Texinfo   → man page or Markdown\n'
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument('subcommand', choices=['c', 'bind', 'texi'], help='conversion mode')
    ap.add_argument('rest', nargs=argparse.REMAINDER)
    args = ap.parse_args()

    if args.subcommand == 'c':
        _main_c(args.rest)
    elif args.subcommand == 'bind':
        _main_bind(args.rest)
    else:
        _main_texi(args.rest)


if __name__ == '__main__':
    main()
