#!/usr/bin/env python3
"""Compare requested CPU uniform windows; mathematical candidates never enable FG."""
import argparse
import collections
import json
import math
import struct
from pathlib import Path


def matrix(values, offset):
    return [[values[offset // 4 + c * 4 + r] for c in range(4)] for r in range(4)]


def max_error(a, b):
    return max(abs(a[r][c] - b[r][c]) for r in range(4) for c in range(4))


def rotation(m):
    if max(abs(m[r][3]) for r in range(3)) > 1e-4:
        return False
    if max(abs(m[3][c] - (1 if c == 3 else 0)) for c in range(4)) > 1e-4:
        return False
    error = max(abs(sum(m[r][k] * m[c][k] for k in range(3)) - (r == c))
                for r in range(3) for c in range(3))
    det = sum(m[0][c] * (m[1][(c+1)%3] * m[2][(c+2)%3] -
                         m[1][(c+2)%3] * m[2][(c+1)%3]) for c in range(3))
    return error < 1e-4 and abs(det - 1) < 1e-4


def perspective(m):
    nonzero = {(0, 0), (1, 1), (0, 2), (1, 2), (2, 2), (2, 3), (3, 2)}
    return (m[0][0] > 0 and m[1][1] > 0 and m[2][3] > 0 and
            abs(m[3][2] + 1) < 1e-6 and
            all(abs(m[r][c]) < 1e-6 for r in range(4) for c in range(4)
                if (r, c) not in nonzero))


def angle_span(entries):
    first = entries[0][2]
    return max(math.degrees(math.acos(max(-1, min(1,
        (sum(first[r][c]*orient[r][c] for r in range(3) for c in range(3))-1)/2))))
        for _, _, orient, _, _ in entries)


def summarize(path):
    rows = []; partial = False
    for line in path.read_text().splitlines():
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            partial = True
            break
    results = {r['submission']: r['result'] for r in rows if r['event'] == 'submission_result'}
    groups = collections.defaultdict(list)
    skipped = collections.Counter()
    samples = [r for r in rows if r['event'] == 'cpu_snapshot_before_submit']
    for row in samples:
        if results.get(row['submission']) != 0:
            skipped['submission_not_confirmed_successful'] += 1
            continue
        blob = bytes.fromhex(row['bytes_hex'])
        if len(blob) != 464:
            raise ValueError('Expected a 464-byte uniform slice')
        values = struct.unpack('<116f', blob)
        relevant = values[16:68]
        if not all(math.isfinite(v) for v in relevant):
            skipped['nonfinite_candidate'] += 1
            continue
        orient, proj, inverse = (matrix(values, o) for o in (64, 128, 192))
        if not rotation(orient) or not perspective(proj):
            skipped['not_perspective_rotation_shape'] += 1
            continue
        inv_error = max_error(inverse, list(map(list, zip(*orient))))
        if inv_error > 1e-4:
            skipped['inverse_rotation_mismatch'] += 1
            continue
        # Ignore temporal jitter when grouping projections, but keep depth terms.
        key = tuple(float(f'{proj[r][c]:.7g}') for r, c in ((0,0),(1,1),(2,2),(2,3)))
        groups[key].append((row, values, orient, proj, inv_error))
    out = []
    for key, entries in sorted(groups.items(), key=lambda item: -len(item[1])):
        spans = [[min(v[64+i] for _,v,_,_,_ in entries), max(v[64+i] for _,v,_,_,_ in entries)]
                 for i in range(4)]
        out.append({
            'projection_key': key, 'samples': len(entries),
            'bindings': dict(collections.Counter(r['binding'] for r,_,_,_,_ in entries)),
            'cpu_present_markers': [min(r['present_marker'] for r,_,_,_,_ in entries),
                                    max(r['present_marker'] for r,_,_,_,_ in entries)],
            'rotation_max_angle_from_first_degrees': angle_span(entries),
            'inverse_rotation_max_error': max(e for _,_,_,_,e in entries),
            'vector_at_256_component_ranges': spans,
            'projection_jitter_term_ranges': [
                [min(p[i][2] for _,_,_,p,_ in entries), max(p[i][2] for _,_,_,p,_ in entries)]
                for i in (0,1)],
            'implied_aspect_ratio': key[1]/key[0],
            'implied_vertical_fov_degrees': math.degrees(2*math.atan(1/key[1])),
        })
    end = next((r for r in reversed(rows) if r['event'] == 'end'), None)
    all_entries = sorted((entry for entries in groups.values() for entry in entries),
                         key=lambda entry: entry[0]['sample'])
    return {'complete': end is not None and not partial, 'samples': len(samples),
            'end': end, 'rejected': dict(skipped), 'candidate_groups': out,
            'all_candidate_rotation_max_angle_from_first_degrees': angle_span(all_entries) if all_entries else None,
            'read_methods': dict(collections.Counter(r.get('read_method', 'unknown') for r in samples))}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('windows', nargs='+', help='label=/absolute/path/to/samples.jsonl')
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    windows = {}
    for value in a.windows:
        label, sep, path = value.partition('=')
        if not sep or not label or label in windows:
            p.error('Each window needs a unique label=path')
        windows[label] = summarize(Path(path))
    result = {'schema': 1, 'camera_verified': False, 'gpu_completion_verified': False,
              'windows': windows,
              'limitations': ['Window labels describe requested user actions, not measured input events',
                              'CPU snapshots can be stale or torn; submissions do not prove consumption',
                              'Offsets 64/128/192 are mathematical candidates only',
                              'Vector at 256 has no established coordinate space or sign',
                              'Present markers are not GPU frame IDs; no cross-frame pairing established',
                              'Projection groups do not identify the consuming shader or render pass']}
    a.output.write_text(json.dumps(result, indent=2, allow_nan=False)+'\n')
    a.output.chmod(0o600)


if __name__ == '__main__':
    main()
