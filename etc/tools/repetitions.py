#!/usr/bin/env python3
"""
Find exact and near-duplicate Nytrix `.ny` functions.
"""
from __future__ import annotations
import sys
sys.dont_write_bytecode = True
import os
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
import argparse
import difflib
import itertools
import json
import re
from collections import Counter, defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable

FN_RE = re.compile(r"^\s*fn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(")
TOKEN_RE = re.compile(
    r'"(?:\\.|[^"])*"|[A-Za-z_][A-Za-z0-9_]*|\d+\.\d+|\d+|==|!=|<=|>=|->|\.\.\.|[{}()\[\],.:;+\-*/%&|^!<>=?]'
)

@dataclass(frozen=True)
class FunctionDef:
    name: str
    path: str
    start_line: int
    end_line: int
    lines: int
    text: str
    body_text: str
    exact_norm: str
    fuzzy_norm: str
    tokens: tuple[str, ...]

@dataclass(frozen=True)
class SimilarPair:
    score: float
    shared_ngrams: int
    left: FunctionDef
    right: FunctionDef

def iter_ny_files(paths: Iterable[str]) -> Iterable[Path]:
    for raw in paths:
        path = Path(raw)
        if path.is_file() and path.suffix == ".ny":
            yield path
            continue
        if path.is_dir():
            yield from sorted(path.rglob("*.ny"))

def strip_comments(line: str) -> str:
    out: list[str] = []
    in_string = False
    escaped = False
    for ch in line:
        if in_string:
            out.append(ch)
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
            continue
        if ch == '"':
            in_string = True
            out.append(ch)
            continue
        if ch == ";":
            break
        out.append(ch)
    return "".join(out)

def scan_braces(line: str) -> tuple[int, bool]:
    depth = 0
    in_string = False
    escaped = False
    for ch in strip_comments(line):
        if in_string:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
            continue
        if ch == '"':
            in_string = True
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
    return depth, "{" in strip_comments(line)

def extract_body_text(fn_text: str) -> str:
    start = fn_text.find("{")
    end = fn_text.rfind("}")
    if start == -1 or end == -1 or end <= start:
        return fn_text
    return fn_text[start + 1 : end]

def normalize_exact(text: str) -> str:
    parts = [strip_comments(line).strip() for line in text.splitlines()]
    return " ".join(part for part in parts if part)

def tokenize_fuzzy(text: str) -> tuple[str, ...]:
    cleaned = "\n".join(strip_comments(line) for line in text.splitlines())
    tokens: list[str] = []
    for token in TOKEN_RE.findall(cleaned):
        if token.startswith('"'):
            tokens.append("STR")
        elif token[0].isdigit():
            tokens.append("NUM")
        else:
            tokens.append(token)
    return tuple(tokens)

def extract_functions(path: Path) -> list[FunctionDef]:
    lines = path.read_text().splitlines()
    out: list[FunctionDef] = []
    i = 0
    while i < len(lines):
        match = FN_RE.match(lines[i])
        if not match:
            i += 1
            continue
        start = i
        depth = 0
        saw_open = False
        j = i
        while j < len(lines):
            delta, opened = scan_braces(lines[j])
            depth += delta
            saw_open = saw_open or opened
            if saw_open and depth <= 0:
                break
            j += 1
        fn_text = "\n".join(lines[start : j + 1])
        body_text = extract_body_text(fn_text)
        exact_norm = normalize_exact(body_text)
        tokens = tokenize_fuzzy(body_text)
        fuzzy_norm = " ".join(tokens)
        out.append(
            FunctionDef(
                name=match.group(1),
                path=str(path),
                start_line=start + 1,
                end_line=j + 1,
                lines=(j - start + 1),
                text=fn_text,
                body_text=body_text,
                exact_norm=exact_norm,
                fuzzy_norm=fuzzy_norm,
                tokens=tokens,
            )
        )
        i = j + 1
    return out

def token_ngrams(tokens: tuple[str, ...], n: int) -> set[tuple[str, ...]]:
    if len(tokens) < n:
        return set()
    return {tokens[i : i + n] for i in range(len(tokens) - n + 1)}

def collect_exact_duplicates(funcs: list[FunctionDef], min_tokens: int) -> list[list[FunctionDef]]:
    groups: defaultdict[str, list[FunctionDef]] = defaultdict(list)
    for fn in funcs:
        if fn.exact_norm and len(fn.tokens) >= min_tokens:
            groups[fn.exact_norm].append(fn)
    return sorted(
        (group for group in groups.values() if len(group) > 1),
        key=lambda group: (-len(group), group[0].name, group[0].path),
    )

def collect_same_name_groups(funcs: list[FunctionDef]) -> list[list[FunctionDef]]:
    groups: defaultdict[str, list[FunctionDef]] = defaultdict(list)
    for fn in funcs:
        groups[fn.name].append(fn)
    return sorted(
        (group for group in groups.values() if len(group) > 1),
        key=lambda group: (-len(group), group[0].name, group[0].path),
    )

def collect_similar_pairs(
    funcs: list[FunctionDef], threshold: float, min_lines: int, ngram_size: int, top: int
) -> list[SimilarPair]:
    eligible = [fn for fn in funcs if fn.lines >= min_lines and len(fn.tokens) >= ngram_size]
    buckets: defaultdict[tuple[str, ...], list[int]] = defaultdict(list)
    for idx, fn in enumerate(eligible):
        for gram in token_ngrams(fn.tokens, ngram_size):
            buckets[gram].append(idx)

    pair_hits: Counter[tuple[int, int]] = Counter()
    for indices in buckets.values():
        uniq = sorted(set(indices))
        if len(uniq) < 2 or len(uniq) > 20:
            continue
        for left, right in itertools.combinations(uniq, 2):
            pair_hits[(left, right)] += 1

    pairs: list[SimilarPair] = []
    for (left_idx, right_idx), shared in pair_hits.most_common():
        left = eligible[left_idx]
        right = eligible[right_idx]
        if left.exact_norm == right.exact_norm:
            continue
        longer = max(len(left.tokens), len(right.tokens))
        shorter = min(len(left.tokens), len(right.tokens))
        if shorter == 0 or longer > shorter * 3:
            continue
        score = difflib.SequenceMatcher(None, left.fuzzy_norm, right.fuzzy_norm).ratio()
        if score < threshold:
            continue
        pairs.append(
            SimilarPair(
                score=score,
                shared_ngrams=shared,
                left=left,
                right=right,
            )
        )
        if len(pairs) >= top:
            break
    return pairs

def location(fn: FunctionDef) -> str:
    return f"{fn.path}:{fn.start_line}"

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", default=["lib"], help="Files or directories to scan")
    parser.add_argument("--threshold", type=float, default=0.88, help="Similarity threshold for near-duplicates")
    parser.add_argument("--min-lines", type=int, default=4, help="Minimum function size to include in near-duplicate checks")
    parser.add_argument("--min-tokens", type=int, default=4, help="Minimum body token count to include in exact-duplicate groups")
    parser.add_argument("--ngram-size", type=int, default=6, help="Token n-gram size for candidate generation")
    parser.add_argument("--top", type=int, default=50, help="Maximum near-duplicate pairs to report")
    parser.add_argument("--json", action="store_true", help="Emit JSON instead of text")
    return parser.parse_args()

def render_text(
    funcs: list[FunctionDef],
    exact_groups: list[list[FunctionDef]],
    same_name_groups: list[list[FunctionDef]],
    similar_pairs: list[SimilarPair],
) -> str:
    lines = [
        f"Scanned {len(funcs)} functions",
        f"Exact duplicate bodies: {len(exact_groups)} groups",
        f"Repeated function names: {len(same_name_groups)} groups",
        f"Near-duplicate bodies: {len(similar_pairs)} pairs",
        "",
        "== Exact Duplicate Bodies ==",
    ]
    if not exact_groups:
        lines.append("(none)")
    else:
        for group in exact_groups[:30]:
            lines.append(f"- {group[0].name}: {len(group)} copies")
            for fn in group:
                lines.append(f"  {location(fn)}")
    lines.append("")
    lines.append("== Repeated Function Names ==")
    if not same_name_groups:
        lines.append("(none)")
    else:
        for group in same_name_groups[:40]:
            lines.append(f"- {group[0].name}: {len(group)} definitions")
            for fn in group[:12]:
                lines.append(f"  {location(fn)}")
            if len(group) > 12:
                lines.append("  ...")
    lines.append("")
    lines.append("== Near-Duplicate Bodies ==")
    if not similar_pairs:
        lines.append("(none)")
    else:
        for pair in similar_pairs:
            lines.append(
                f"- score={pair.score:.3f} shared_ngrams={pair.shared_ngrams} "
                f"{pair.left.name} <-> {pair.right.name}"
            )
            lines.append(f"  {location(pair.left)}")
            lines.append(f"  {location(pair.right)}")
    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    files = list(iter_ny_files(args.paths))
    funcs: list[FunctionDef] = []
    for path in files:
        funcs.extend(extract_functions(path))

    exact_groups = collect_exact_duplicates(funcs, args.min_tokens)
    same_name_groups = collect_same_name_groups(funcs)
    similar_pairs = collect_similar_pairs(
        funcs, threshold=args.threshold, min_lines=args.min_lines, ngram_size=args.ngram_size, top=args.top
    )

    if args.json:
        payload = {
            "summary": {
                "functions": len(funcs),
                "exact_duplicate_groups": len(exact_groups),
                "same_name_groups": len(same_name_groups),
                "near_duplicate_pairs": len(similar_pairs),
            },
            "exact_duplicates": [
                [asdict(fn) for fn in group]
                for group in exact_groups
            ],
            "same_name_groups": [
                [asdict(fn) for fn in group]
                for group in same_name_groups
            ],
            "near_duplicates": [
                {
                    "score": pair.score,
                    "shared_ngrams": pair.shared_ngrams,
                    "left": asdict(pair.left),
                    "right": asdict(pair.right),
                }
                for pair in similar_pairs
            ],
        }
        print(json.dumps(payload, indent=2))
    else:
        print(render_text(funcs, exact_groups, same_name_groups, similar_pairs))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
