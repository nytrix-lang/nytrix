#!/usr/bin/env python3
"""
Audit Nytrix `.ny` files for missing function docstrings.
"""
from __future__ import annotations
import argparse
import json
import re
import sys
sys.dont_write_bytecode = True
import os
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
from dataclasses import dataclass, asdict
from pathlib import Path

FN_RE = re.compile(r"^\s*fn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(")

@dataclass
class MissingDoc:
    path: str
    line: int
    name: str
    signature: str

def iter_ny_files(paths: list[str]) -> list[Path]:
    out: list[Path] = []
    seen: set[Path] = set()
    for raw in paths:
        p = Path(raw)
        if p.is_file() and p.suffix == ".ny":
            rp = p.resolve()
            if rp not in seen:
                out.append(p)
                seen.add(rp)
            continue
        if p.is_dir():
            for child in sorted(p.rglob("*.ny")):
                rp = child.resolve()
                if rp not in seen:
                    out.append(child)
                    seen.add(rp)
    return out

def has_inline_doc(line: str) -> bool:
    if "{" not in line:
        return False
    body = line.split("{", 1)[1].lstrip()
    return body.startswith('"')

def has_following_doc(lines: list[str], start_idx: int) -> bool:
    idx = start_idx + 1
    while idx < len(lines) and not lines[idx].strip():
        idx += 1
    if idx >= len(lines):
        return False
    return lines[idx].lstrip().startswith('"')

def scan_file(path: Path) -> tuple[int, list[MissingDoc]]:
    lines = path.read_text().splitlines()
    total = 0
    missing: list[MissingDoc] = []
    for i, line in enumerate(lines):
        match = FN_RE.match(line)
        if not match:
            continue
        total += 1
        if has_inline_doc(line) or has_following_doc(lines, i):
            continue
        missing.append(
            MissingDoc(
                path=str(path),
                line=i + 1,
                name=match.group(1),
                signature=line.strip(),
            )
        )
    return total, missing

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", default=["lib"], help="Files or directories to scan")
    parser.add_argument("--json", action="store_true", help="Emit JSON instead of text")
    parser.add_argument("--top", type=int, default=10, help="Max missing items to show per file in text mode")
    parser.add_argument("--fail-on-missing", action="store_true", help="Exit with code 1 when missing docstrings are found")
    args = parser.parse_args()

    files = iter_ny_files(args.paths)
    total_fns = 0
    missing: list[MissingDoc] = []
    per_file: dict[str, list[MissingDoc]] = {}
    for path in files:
        fn_total, file_missing = scan_file(path)
        total_fns += fn_total
        if file_missing:
            per_file[str(path)] = file_missing
            missing.extend(file_missing)

    if args.json:
        print(
            json.dumps(
                {
                    "files_scanned": len(files),
                    "functions_scanned": total_fns,
                    "missing_count": len(missing),
                    "missing": [asdict(item) for item in missing],
                },
                indent=2,
            )
        )
    else:
        print(f"Scanned {len(files)} files")
        print(f"Scanned {total_fns} functions")
        print(f"Missing docstrings: {len(missing)}")
        for path in sorted(per_file):
            entries = per_file[path]
            print(path)
            for item in entries[: args.top]:
                print(f"  {item.line}: {item.signature}")
            if len(entries) > args.top:
                print(f"  ... {len(entries) - args.top} more")

    return 1 if args.fail_on_missing and missing else 0

if __name__ == "__main__":
    sys.exit(main())
