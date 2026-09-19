#!/usr/bin/env python3
import importlib.util
import json
import math
import struct
import tempfile
from pathlib import Path

spec = importlib.util.spec_from_file_location('compare', Path(__file__).with_name('compare-camera.py'))
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)


def sample(number, angle, translation, bad_inverse=False):
    values = [0.] * 116
    def put(offset, rows):
        values[offset//4:offset//4+16] = [rows[r][c] for c in range(4) for r in range(4)]
    c, s = math.cos(angle), math.sin(angle)
    rotation = [[c,-s,0,0],[s,c,0,0],[0,0,1,0],[0,0,0,1]]
    put(0, [[1,0,0,number*100],[0,1,0,0],[0,0,1,0],[0,0,0,1]])  # Unrelated object movement.
    put(64, rotation)
    put(128, [[1,0,number*.0001,0],[0,2,0,0],[0,0,.000015,.15000225],[0,0,-1,0]])
    put(192, list(zip(*rotation)))
    if bad_inverse:values[48] = 20.
    values[64:68] = [translation,2,3,1]
    return {'event':'cpu_snapshot_before_submit','submission':number,'sample':number,
            'binding':29,'present_marker':number,'bytes_hex':struct.pack('<116f',*values).hex(),
            'read_method':'pipe_copy_from_user'}


with tempfile.TemporaryDirectory() as tmp:
    p = Path(tmp)/'window.jsonl'
    rows = [sample(1,0,0),sample(2,math.pi/2,5),sample(3,0,0,True),sample(4,0,0)]
    rows += [{'event':'submission_result','submission':i,'result':0} for i in (1,2,3)]
    rows += [{'event':'end','samples':4}]
    p.write_text(''.join(json.dumps(r)+'\n' for r in rows))
    report = compare.summarize(p)
    assert report['complete'] and len(report['candidate_groups']) == 1
    g = report['candidate_groups'][0]
    assert g['samples'] == 2 and abs(g['rotation_max_angle_from_first_degrees']-90) < .001
    assert g['vector_at_256_component_ranges'][0] == [0,5]
    assert g['implied_aspect_ratio'] == 2
    assert report['rejected'] == {'inverse_rotation_mismatch':1,'submission_not_confirmed_successful':1}
    rows[1] = sample(2,0,5)
    p.write_text(''.join(json.dumps(r)+'\n' for r in rows))
    assert compare.summarize(p)['candidate_groups'][0]['rotation_max_angle_from_first_degrees'] == 0
    with p.open('a') as f:f.write('{broken')
    assert not compare.summarize(p)['complete']
print('PASS: known rotation/translation, unrelated object movement, jitter grouping, inverse and submission rejection, partial output')
