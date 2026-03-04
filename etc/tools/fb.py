#!/usr/bin/env python3
"""
Nytrix Framebuffer Renderer.
"""
from __future__ import annotations
import sys
sys.dont_write_bytecode = True
import struct
import os
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
import argparse
from pathlib import Path

DUMP_SEARCH_PATHS = [
    "build/release/fb_dump.tga",
    "fb_dump.tga",
    "build/fb_dump.tga",
    "build/debug/fb_dump.tga",
    "screenshot.tga",
    "snapshot.tga"
]

def load_tga(p):
    try:
        data = Path(p).read_bytes()
    except Exception as e:
        print(f"Error reading {p}: {e}")
        sys.exit(1)
    
    if len(data) < 18:
        print(f"Invalid TGA header in {p}")
        sys.exit(1)
        
    idl, cm, typ = data[0], data[1], data[2]
    if typ not in (2, 3):
        print(f"Unsupported TGA type {typ} (only 2=RGB and 3=Gray supported)")
        sys.exit(1)
        
    w, h = struct.unpack_from("<HH", data, 12)
    bpp, desc = data[16], data[17]
    ch = bpp // 8
    if ch < 3:
        print(f"Unsupported BPP {bpp} (only 24/32 supported)")
        sys.exit(1)
        
    off = 18 + idl + (cm * 3 if cm else 0)
    px_data = data[off:]
    top_down = bool(desc & 0x20)
    
    pixels = []
    for y in range(h):
        ry = y if top_down else (h - 1 - y)
        row = []
        base = ry * w * ch
        for x in range(w):
            i = base + x * ch
            if i + 2 >= len(px_data): break
            b, g, r = px_data[i], px_data[i+1], px_data[i+2]
            row.append((r, g, b))
        if row: pixels.append(row)
    return w, h, pixels

def render(px, cols, rows, ascii_mode):
    h = len(px)
    if h == 0: return
    w = len(px[0])
    sw = max(1, w // cols)
    sh = max(Sh := 1, h // rows)
    sh = Sh if sh == 1 else sh # Fix for division by zero or similar
    sh = max(1, h // rows)
    
    for ry in range(rows):
        py = min(ry * sh, h - 1)
        line = []
        for rx in range(cols):
            pxi = min(rx * sw, w - 1)
            r, g, b = px[py][pxi]
            if ascii_mode:
                v = (r * 299 + g * 587 + b * 114) // 1000
                ch = " .:-=+*#%@"[v * 10 // 256]
                line.append(ch * 2)
            else:
                line.append(f"\x1b[48;2;{r};{g};{b}m  ")
        print("".join(line) + ("\x1b[0m" if not ascii_mode else ""))

def main(argv=None):
    ap = argparse.ArgumentParser(description="Nytrix FB Debugger")
    ap.add_argument("file", nargs="?", default="dump", help="TGA file to load or 'dump' to search automatically")
    ap.add_argument("-c", "--cols", type=int, default=120)
    ap.add_argument("-r", "--rows", type=int, default=40)
    ap.add_argument("--ascii", action="store_true")
    # Filter out execution keywords when chained from ./make
    filtered_argv = []
    if argv:
        for arg in argv:
            if arg in ("ny", "-run", "--run", "run", "perf", "gate", "matrix"): continue
            if arg.endswith(".ny") or arg.endswith(".nyh"): continue
            # If it's the ny binary itself, skip it
            if arg.endswith("/ny") or arg == "./ny": continue
            # Only keep it if it looks like a flag or a file that actually exists
            if arg.startswith("-") or Path(arg).is_file():
                # But don't keep it if it's a known non-TGA file we just filtered
                filtered_argv.append(arg)
            
    a = ap.parse_args(filtered_argv)

    target = a.file
    if target == "dump":
        for p in DUMP_SEARCH_PATHS:
            if Path(p).exists():
                target = p
                break
        if target == "dump":
            paths_str = ", ".join(DUMP_SEARCH_PATHS)
            print(f"Error: No framebuffer dump found (searched: {paths_str})")
            sys.exit(1)
    
    if not Path(target).exists():
        print(f"Error: File not found: {target}")
        sys.exit(1)
        
    w, h, px = load_tga(target)
    print(f"FB: {target} ({w}x{h}) -> Terminal ({a.cols}x{a.rows})")
    render(px, a.cols, a.rows, a.ascii)

if __name__ == "__main__":
    main()
