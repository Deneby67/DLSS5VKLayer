#!/usr/bin/env python3
"""Exercise conservative target selection without proprietary game captures."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile

spec = importlib.util.spec_from_file_location('targets', Path(__file__).with_name('analyze-render-targets.py'))
targets = importlib.util.module_from_spec(spec)
spec.loader.exec_module(targets)
evidence = {'vertex_shader': {'sha256': 'a' * 64}, 'paired_fragment_shader': {'sha256': 'b' * 64}}
attachments = [dict(role='color' if i < 5 else 'depth_stencil', attachment_slot=i if i < 5 else 0,
                    view_format=f, image_format=f, valid=True, extent=[32, 24, 1],
                    image_generation=100+i, view_generation=200+i, base_mip=0, mips=1,
                    base_layer=0, layers=1, usage=23 if i < 5 else 39,
                    aspect=1 if i < 5 else 6, store=0)
               for i, f in enumerate([43, 37, 37, 37, 83, 130])]
link = {'stages': [dict(stage=s, sha256=h, entry='VkMain', specialized=False)
                   for s, h in [(1, 'a'*64), (16, 'b'*64)]],
        'render_targets': dict(status='tracked attachment references', dynamic_rendering=False,
                               render_pass_generation=1, framebuffer_generation=2, subpass=0,
                               render_area=[0, 0, 32, 24], attachments=attachments)}
assert targets.select(link, evidence)[1] is None
for field, value, reason in [('valid', False, 'incomplete_or_stale_attachments'),
                             ('view_format', 37, 'different_attachment_layout'),
                             ('base_mip', 1, 'unverified_subresource_or_image'),
                             ('extent', [64, 24, 1], 'different_extent_or_render_area')]:
    bad = copy.deepcopy(link)
    bad['render_targets']['attachments'][4][field] = value
    assert targets.select(bad, evidence)[1] == reason
bad = copy.deepcopy(link)
bad['stages'][1]['sha256'] = 'c'*64
assert targets.select(bad, evidence)[1] == 'different_shader_pair'
bad = copy.deepcopy(link)
bad['stages'][0]['specialized'] = True
assert targets.select(bad, evidence)[1] == 'unverified_stage_variant'

with tempfile.TemporaryDirectory() as tmp:
    files = []
    def write(session, request, result=0):
        path = Path(tmp)/session/'camera'/f'{request}.jsonl'
        path.parent.mkdir(parents=True, exist_ok=True)
        rows = [dict(event='begin'),
                dict(event='cpu_snapshot_before_submit', submission=1, sample=1,
                     present_marker=20, draw_links=[link]),
                dict(event='submission_result', submission=1, result=result),
                dict(event='end', samples=1)]
        for r in rows:
            r.update(device=444, request=request)
        path.write_text(''.join(json.dumps(r)+'\n' for r in rows))
        return path
    files = [write('first', 1), write('first', 2), write('second', 1)]
    report = targets.summarize(files + [files[0]], evidence)
    assert len(report['target_groups']) == 2  # Same generations in different processes never join.
    assert [g['distinct_samples'] for g in report['target_groups']] == [2, 1]
    assert all(g['both_have_transfer_src_usage'] and g['both_declare_store'] for g in report['target_groups'])
    assert report['fg_ready'] is False and report['runnable'] is False
    failed = write('failed', 1, -4)
    assert not targets.summarize([failed], evidence)['target_groups']
    with files[0].open('a') as f:
        f.write('{unfinished')
    assert not targets.summarize([files[0]], evidence)['windows'][0]['complete']
print('PASS: shader/attachment/subresource gates, submission rejection, process isolation, incomplete windows')
