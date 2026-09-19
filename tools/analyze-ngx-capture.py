#!/usr/bin/env python3
"""Validate bounded NGX windows and emit address-free research evidence, never a runnable profile."""
import argparse
from collections import Counter
import json
from pathlib import Path


def distinct(values):
    return [json.loads(v) for v in sorted({json.dumps(v, sort_keys=True, allow_nan=False) for v in values})]


def summarize(rows):
    if not rows or any(r.get('schema') != 1 or r.get('pid') != rows[0].get('pid') for r in rows):
        raise ValueError('Expected one schema-1 process session')
    if sum(r.get('event') == 'attach_wait' for r in rows) != 1:
        raise ValueError('Expected one attachment session')
    created, windows, active = {}, [], None
    seen_requests = set()
    for row in rows:
        event = row.get('event')
        if event == 'create' and row.get('result') == 1:
            created[(row['provider'], row['handle'])] = (row['handle_generation'], row['feature'])
        elif event == 'release' and row.get('result') == 1:
            created.pop((row['provider'], row['handle']), None)
        elif event == 'capture_begin':
            if active is not None or row['request'] in seen_requests:
                raise ValueError('Overlapping or reused capture request')
            seen_requests.add(row['request'])
            active = {'begin': row, 'before': {}, 'after': {}}
        elif event in ('evaluate_before', 'evaluate_after'):
            if active is None or row['request'] != active['begin']['request']:
                raise ValueError('Evaluation outside its capture window')
            target = active['before' if event == 'evaluate_before' else 'after']
            if row['call'] in target:
                raise ValueError('Duplicate evaluation record')
            if event == 'evaluate_before':
                identity = created.get((row['provider'], row['handle']))
                if identity != (row['handle_generation'], row['feature']) or row['feature'] != 1:
                    raise ValueError('Evaluation is not linked to a known SR creation')
                if row.get('mode') != 'observe_only':
                    raise ValueError('Unsupported observation mode')
            elif row['call'] not in active['before']:
                raise ValueError('Evaluation result precedes its input')
            target[row['call']] = row
        elif event == 'capture_end':
            if active is None or row['request'] != active['begin']['request']:
                raise ValueError('Unmatched capture end')
            before, after = active['before'], active['after']
            limit = active['begin']['max_samples']
            if (not before or before.keys() != after.keys() or len(before) > limit or
                    row['reason'] not in ('duration', 'sample_limit') or
                    (row['reason'] == 'sample_limit' and len(before) != limit)):
                raise ValueError('Incomplete or aborted evaluation window')
            active['end'] = row
            windows.append(active)
            active = None
    if active is not None or not windows:
        raise ValueError('No complete capture or an unfinished window remains')
    accepted, results = [], Counter()
    for w in windows:
        for call, before in w['before'].items():
            result = w['after'][call]['result']
            results[hex(result)] += 1
            if result == 1:
                accepted.append(before['before'])
    if not accepted:
        raise ValueError('No successful SR evaluations')
    resources = {}
    for name in sorted({k for s in accepted for k in s['resources']}):
        descriptors = []
        for s in accepted:
            item = s['resources'].get(name, {})
            resource = item.get('resource', {})
            # Explicit allowlist: never export pointers, handles or process IDs.
            descriptors.append({'result': item.get('result'), **{
                k: resource[k] for k in ('status', 'extent', 'format', 'aspect', 'base_mip',
                                        'mips', 'base_layer', 'layers', 'read_write') if k in resource}})
        resources[name] = distinct(descriptors)
    scalar_names = sorted({k for s in accepted for k in s['values']})
    matrix_names = sorted({k for s in accepted for k in s['optional_matrices']})
    return {
        'schema': 1, 'kind': 'ngx_sr_metadata_evidence', 'runnable': False, 'fg_ready': False,
        'mode': 'observe_only', 'intended_pipeline': ['NR', 'SR', 'FG'],
        'complete_windows': len(windows), 'successful_evaluations': len(accepted),
        'evaluation_results': dict(results), 'verified_feature_id': 1,
        'resources': resources,
        'values': {k: distinct([{'result': s['values'].get(k, {}).get('result'),
                                **({'value': s['values'][k]['value']} if 'value' in s['values'].get(k, {}) else {})}
                               for s in accepted]) for k in scalar_names},
        'optional_matrix_results': {k: distinct([s['optional_matrices'].get(k, {}).get('result')
                                                 for s in accepted]) for k in matrix_names},
        'limitations': [
            'Resource descriptors are observed; pixels and GPU completion are not verified.',
            'NGX call identity does not establish the corresponding presented frame.',
            'Windows NGX resource handles must not be treated as native Linux handles.',
            'Missing matrices have no inferred substitutes; camera semantics remain unverified.',
            'No processing stage, resource preservation or presentation pacing is enabled.']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        report = summarize([json.loads(line) for line in args.log.read_text().splitlines()])
        output = json.dumps(report, indent=2, allow_nan=False) + '\n'
    except (ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    if args.output:
        args.output.write_text(output)
    else:
        print(output, end='')
