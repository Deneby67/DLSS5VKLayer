#!/usr/bin/env python3
"""Summarize draw-linked target references. Never enables FG or captures pixels."""
import argparse
from collections import Counter
import json
from pathlib import Path


def select(link, evidence):
    """Conservative selector for the researched legacy RDR2 G-buffer pass."""
    stages = link.get('stages', [])
    expected = [(1, evidence['vertex_shader']['sha256']),
                (16, evidence['paired_fragment_shader']['sha256'])]
    if len(stages) != 2 or sorted((s.get('stage', 0), s.get('sha256', '')) for s in stages) != expected:
        return None, 'different_shader_pair'
    if any(s.get('specialized', True) or s.get('entry') != 'VkMain' for s in stages):
        return None, 'unverified_stage_variant'
    targets = link.get('render_targets', {})
    if (targets.get('dynamic_rendering', True) or
            targets.get('status') != 'tracked attachment references'):
        return None, 'unresolved_or_nonlegacy_scope'
    attachments = targets.get('attachments', [])
    if len(attachments) != 6 or not all(a.get('valid') is True for a in attachments):
        return None, 'incomplete_or_stale_attachments'
    colors = sorted((a for a in attachments if a.get('role') == 'color'),
                    key=lambda a: a.get('attachment_slot', -1))
    depths = [a for a in attachments if a.get('role') == 'depth_stencil']
    if ([(a.get('attachment_slot'), a.get('view_format')) for a in colors] !=
            list(enumerate([43, 37, 37, 37, 83])) or len(depths) != 1 or
            depths[0].get('view_format') != 130):
        return None, 'different_attachment_layout'
    motion, depth = colors[4], depths[0]
    extent = motion.get('extent')
    if (not isinstance(extent, list) or len(extent) != 3 or extent[2] != 1 or
            not all(isinstance(n, int) and n > 0 for n in extent) or
            any(a.get('extent') != extent for a in attachments) or
            targets.get('render_area') != [0, 0, *extent[:2]]):
        return None, 'different_extent_or_render_area'
    if any(a.get('base_mip') != 0 or a.get('mips') != 1 or
           a.get('base_layer') != 0 or a.get('layers') != 1 or
           a.get('image_format') != a.get('view_format') or
           a.get('image_generation', 0) <= 0 or a.get('view_generation', 0) <= 0
           for a in attachments):
        return None, 'unverified_subresource_or_image'
    if motion.get('aspect') != 1 or depth.get('aspect') != 6:
        return None, 'different_aspects'
    return {'render_pass_generation': targets['render_pass_generation'],
            'framebuffer_generation': targets['framebuffer_generation'],
            'subpass': targets['subpass'], 'render_area': targets['render_area'],
            'motion_candidate': motion, 'depth_candidate': depth}, None


def summarize(paths, evidence):
    windows, groups, rejected = [], {}, Counter()
    # A resource generation is scoped to a device and process session, not a handle
    # or request. Different windows in the SAME session may corroborate identity.
    for path in dict.fromkeys(p.resolve() for p in paths):
        rows, partial = [], False
        for line in path.read_text().splitlines():
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                partial = True
                break
        begins = [r for r in rows if r.get('event') == 'begin']
        ends = [r for r in rows if r.get('event') == 'end']
        samples = [r for r in rows if r.get('event') == 'cpu_snapshot_before_submit']
        identity = lambda r: (r.get('device'), r.get('request'))
        complete = (not partial and len(begins) == len(ends) == 1 and
                    rows[-1] == ends[0] and identity(begins[0]) == identity(ends[0]) and
                    all(identity(r) == identity(begins[0]) for r in rows) and
                    ends[0].get('samples') == len(samples))
        window = {'path': str(path), 'complete': complete, 'samples': len(samples),
                  'successful_submit_samples': 0, 'matching_draw_links': 0}
        windows.append(window)
        if not complete:
            rejected['incomplete_or_mixed_window'] += 1
            continue
        results = {}
        for row in rows:
            if row.get('event') == 'submission_result':
                key = row['submission']
                if key in results and results[key] != row['result']:
                    results[key] = None
                else:
                    results[key] = row['result']
        session = str(path.parent.parent)
        for sample in samples:
            if results.get(sample['submission']) != 0:
                rejected['submission_not_confirmed_successful'] += 1
                continue
            window['successful_submit_samples'] += 1
            for link in sample.get('draw_links', []):
                target, reason = select(link, evidence)
                if target is None:
                    rejected[reason] += 1
                    continue
                window['matching_draw_links'] += 1
                key = (session, sample['device'], json.dumps(target, sort_keys=True))
                group = groups.setdefault(key, {
                    'session': session, 'device': sample['device'], **target,
                    'draw_links': 0, 'sample_ids': set(), 'cpu_present_markers': set(),
                    'windows': set(), 'gpu_contents_verified': False,
                    'gpu_frame_identity_verified': False})
                group['draw_links'] += 1
                group['sample_ids'].add((sample['request'], sample['sample']))
                group['cpu_present_markers'].add(sample['present_marker'])
                group['windows'].add(str(path))
    output = []
    for group in groups.values():
        group['distinct_samples'] = len(group.pop('sample_ids'))
        group['cpu_present_markers'] = sorted(group['cpu_present_markers'])
        group['windows'] = sorted(group['windows'])
        group['both_have_transfer_src_usage'] = all(
            group[k]['usage'] & 1 for k in ('motion_candidate', 'depth_candidate'))
        group['both_declare_store'] = all(
            group[k].get('store') == 0 for k in ('motion_candidate', 'depth_candidate'))
        output.append(group)
    return {'schema': 1, 'kind': 'research_target_references', 'runnable': False,
            'fg_ready': False, 'windows': windows, 'target_groups': output,
            'rejected': dict(rejected),
            'limitations': [
                'Only the researched two-stage legacy pass signature is selected.',
                'CPU present markers are not GPU frame identifiers.',
                'Recorded draws and successful submits do not prove pixel writes.',
                'Transfer usage and store declarations do not establish safe copy timing.',
                'Final layouts, sample count, queue ownership and later writers remain unresolved.',
                'This private report contains session resource generations, never reusable addresses.']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('samples', type=Path, nargs='+')
    parser.add_argument('--evidence', type=Path, default=Path(__file__).resolve().parent.parent /
                        'profiles/research/rdr2-camera-draw-evidence.json')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    text = json.dumps(summarize(args.samples, json.loads(args.evidence.read_text())),
                      indent=2, allow_nan=False) + '\n'
    if args.output:
        # Reports include process-local resource identities and stay private.
        import os
        fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, 'w') as stream:
            os.fchmod(stream.fileno(), 0o600)
            stream.write(text)
    else:
        print(text, end='')
