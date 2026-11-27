#!/usr/bin/env python3
"""Plot NYR's extracted schedule, integer points and byte conflicts as SVG.

Inspired by islplot's domain/map/group visualizations; no imported code or
third-party modules. This is a finite diagnostic, not the legality engine.
For symbolic bounds the dump is the maximum domain. Large domains are clipped
and 3D regions are sliced; dependencies outside the displayed set are omitted.
External regions assume the emitted alias guard has succeeded.
"""
import argparse
from collections import defaultdict
from html import escape
from itertools import product
import json
from pathlib import Path


def models(path):
    for line in path.read_text().splitlines():
        if line.startswith(('NYR_POLY_MODEL ', 'NYR_POLY_CANDIDATE ')):
            m = json.loads(line.split(' ', 1)[1])
            if m['version'] == 1:
                yield line.startswith('NYR_POLY_MODEL '), m


def point_set(stmt, limit, section):
    dims = len(stmt['domain'])
    extents = [d['extent'] for d in stmt['domain']]
    if dims not in (2, 3) or any(not isinstance(n, int) or n < 0 for n in extents):
        raise ValueError('unsupported domain')
    if dims == 3 and not 0 <= section < extents[2]:
        raise ValueError('slice outside domain')
    for i, j in product(*(range(min(n, limit)) for n in extents[:2])):
        t = stmt['triangle']
        if t and not (j < i if t == 1 else j <= i if t == 2 else i < j if t == 3 else i <= j):
            continue
        yield (i, j) if dims == 2 else (i, j, section)


def order(m, p, stmt):
    v = tuple(p[d] for d in m['axes'])
    if m['register_block'] > 1:
        lanes = m['register_block']
        full = stmt['domain'][m['axes'][1]]['extent'] // lanes * lanes
        group = v[1] // lanes if v[1] < full else full // lanes + v[1] - full
        return (v[0], group, v[2], v[1])
    if m['wavefront']:
        return (m['skew'] * v[0] + v[1], v[0])
    if m['tile']:
        return tuple(x // (m['inner_tile'] if k == 1 and m['inner_tile'] else m['tile'])
                     for k, x in enumerate(v)) + v
    return v


def tile_group(m, p, stmt):
    v = tuple(p[d] for d in m['axes'])
    if m['wavefront']:
        return m['skew'] * v[0] + v[1],
    if m['tile']:
        return tuple(x // (m['inner_tile'] if k == 1 and m['inner_tile'] else m['tile'])
                     for k, x in enumerate(v))
    if m['register_block'] > 1:
        return order(m, p, stmt)[:2]
    return v[0],


def conflicts(m, points):
    """Nearest RAW/WAR/WAW edges for the displayed subset, in source order.

    Each byte remembers its last writer and intervening readers. Deduplicate
    edges across bytes. Full source dependences need the compiler/oracle.
    """
    writers, readers, edges = {}, defaultdict(set), set()
    for k, stmt in enumerate(m['statements']):
        for p in points[k]:
            event = k, p
            for a in stmt['accesses']:
                width = a['width']
                if not 1 <= width <= 8:
                    raise ValueError('unsupported byte width')
                off = a['constant'] + sum(c * x for c, x in zip(a['coefficients'], p))
                for b in range(off, off + width):
                    key = a['base'], b
                    prev = writers.get(key)
                    if prev is not None and prev != event:
                        edges.add((prev, event, 'WAW' if a['write'] else 'RAW'))
                    if a['write']:
                        edges.update((r, event, 'WAR') for r in readers.pop(key, ()) if r != event)
                        writers[key] = event
                    else:
                        readers[key].add(event)
    return edges


def render(m, limit=16, section=0, edge_limit=350):
    if not 1 <= len(m['statements']) <= 3:
        raise ValueError('unsupported statement count')
    points = [list(point_set(s, limit, section)) for s in m['statements']]
    events = [(k, p) for k, ps in enumerate(points) for p in ps]
    rank = {event: n for n, event in enumerate(sorted(events, key=lambda e: (
        order(m, e[1], m['statements'][e[0]]), e[0])), 1)}
    edges = sorted(conflicts(m, points))
    bad = [e for e in edges if rank[e[0]] >= rank[e[1]]]
    bad_set = set(bad)
    groups = sorted({tile_group(m, p, m['statements'][k]) for k, p in events})
    group_index = {g: i for i, g in enumerate(groups)}
    palette = ['#bfdbfe', '#99f6e4', '#fde68a', '#ddd6fe', '#fecdd3', '#bae6fd', '#d9f99d', '#fed7aa']
    span = max((max(p[:2]) + 1 for _, p in events), default=1)
    pitch = min(48, max(19, 600 // span))
    size = span * pitch
    panel_width = size + 100
    w, h = panel_width * 2 + 40, size + 150
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">',
           '<rect width="100%" height="100%" fill="#fff"/>',
           '<style>text{font-family:system-ui,sans-serif;fill:#172033}.small{font-size:11px}.title{font-size:17px;font-weight:600}</style>',
           '<defs><marker id="arrow" markerWidth="6" markerHeight="6" refX="5" refY="3" orient="auto"><path d="M0 0L6 3L0 6Z" fill="#64748b"/></marker></defs>']
    def text(x, y, value, cls='small', anchor='start'):
        out.append(f'<text x="{x}" y="{y}" class="{cls}" text-anchor="{anchor}">{escape(str(value))}</text>')
    text(18, 25, f'NYR region {m["key"]} — {len(events)} displayed instances', 'title')
    text(18, 43, f'{len(edges)} displayed conflicts · {len(bad)} sampled inversions · {"maximum symbolic domain" if any(d["symbolic"] for s in m["statements"] for d in s["domain"]) else "fixed domain"}')
    if m['dimensions'] == 3:
        text(w-18, 43, f'axis 2 = {section}', anchor='end')
    source_rank = {e: n for n, e in enumerate(events, 1)}
    for q, title in enumerate(('Original execution order', 'Scheduled order / tile groups')):
        x0, y0 = 45 + q * panel_width, 88
        text(x0, 68, title, 'title')
        def coord(e):
            k, p = e
            shift = (k - (len(points)-1)/2) * min(7, pitch/5)
            return x0 + (p[1] + .5) * pitch + shift, y0 + (p[0] + .5) * pitch + shift
        for k, p in events:
            x, y = x0+p[1]*pitch, y0+p[0]*pitch
            fill = '#e2e8f0' if not q else palette[group_index[tile_group(m, p, m['statements'][k])] % len(palette)]
            out.append(f'<rect x="{x}" y="{y}" width="{pitch-1}" height="{pitch-1}" rx="3" fill="{fill}"/>')
        # Draw all sampled inversions first; otherwise show a bounded edge set.
        for edge in (bad + [e for e in edges if e not in bad_set])[:edge_limit]:
            a, b, kind = edge
            x1, y1 = coord(a); x2, y2 = coord(b)
            if (x1, y1) == (x2, y2):
                continue
            color = '#b91c1c' if edge in bad_set else '#64748b'
            out.append(f'<path d="M{x1} {y1}L{x2} {y2}" stroke="{color}" stroke-width="1" opacity=".55" marker-end="url(#arrow)"><title>{kind}</title></path>')
        for e in events:
            x, y = coord(e)
            n = (rank if q else source_rank)[e]
            out.append(f'<circle cx="{x}" cy="{y}" r="{min(11,pitch*.3)}" fill="#fff" stroke="#334155"><title>Statement {e[0]}, {escape(str(e[1]))}; rank {n}</title></circle>')
            if len(points)==1 and pitch>=24:
                text(x, y+4, n, anchor='middle')
        text(x0+size/2, y0+size+19, 'j (ordinal)', anchor='middle')
        text(x0-10, y0+size/2, 'i', anchor='end')
    text(18, h-24, 'Bounded diagnostic: omitted points/edges are not evidence of legality. External bases are conditional on alias guards.')
    text(18, h-8, f'Prefix limit {limit}, maximum arrows per panel {edge_limit}. Schedule rank counts displayed points only.')
    out.append('</svg>')
    return '\n'.join(out)+'\n'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('dump', type=Path)
    p.add_argument('-o', '--output', type=Path, required=True)
    p.add_argument('--key', help='exact region key')
    p.add_argument('--limit', type=int, default=12, choices=range(2, 25), metavar='2..24')
    p.add_argument('--slice', type=int, default=0)
    p.add_argument('--candidate', action='store_true', help='draw an uncommitted candidate instead')
    args = p.parse_args()
    for committed, model in models(args.dump):
        if committed == args.candidate or (args.key and model['key'] != args.key):
            continue
        args.output.write_text(render(model, args.limit, args.slice))
        return
    p.error('no matching model record')


if __name__ == '__main__':
    main()
