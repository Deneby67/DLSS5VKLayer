#!/usr/bin/env python3
"""Inspect CPU snapshots; no heuristic here enables a game profile."""
import argparse,json,math,struct
from pathlib import Path
OFFSETS=(0,64,128,192,272,336,400)
def determinant(rows):
    m=[row[:] for row in rows];result=1.
    for c in range(4):
        pivot=max(range(c,4),key=lambda r:abs(m[r][c]))
        if abs(m[pivot][c])<1e-12:return 0.
        if pivot!=c:m[c],m[pivot]=m[pivot],m[c];result=-result
        value=m[c][c];result*=value
        for r in range(c+1,4):
            factor=m[r][c]/value
            for k in range(c+1,4):m[r][k]-=factor*m[c][k]
    return result
def analyze(path):
    samples=[];results={};end=None;partial=False
    for line in path.read_text().splitlines():
        try:r=json.loads(line)
        except json.JSONDecodeError:partial=True;break
        if r['event']=='cpu_snapshot_before_submit':samples.append(r)
        elif r['event']=='submission_result':results[r['submission']]=r['result']
        elif r['event']=='end':end=r
    decoded=[]
    for s in samples:
        data=bytes.fromhex(s['bytes_hex'])
        if len(data)!=464:raise ValueError('unexpected sample size')
        matrices={}
        for offset in OFFSETS:
            values=struct.unpack_from('<16f',data,offset);finite=all(math.isfinite(v) for v in values)
            rows=[[values[column*4+row] for column in range(4)] for row in range(4)]
            matrices[str(offset)]={'finite':finite,'rows':rows if finite else None,
                                  'determinant':determinant(rows) if finite else None}
        decoded.append({'sample':s['sample'],'binding':s['binding'],'buffer_generation':s['buffer_generation'],
                        'offset':s['offset'],'submission':s['submission'],'submission_result':results.get(s['submission']),
                        'present_marker':s['present_marker'],'matrices':matrices})
    return {'schema':1,'camera_verified':False,'gpu_completion_verified':False,'matrix_order':'column-major, assumed from prior SPIR-V',
            'samples':decoded,'successful_submit_samples':sum(s['submission_result']==0 for s in decoded),
            'complete':end is not None and not partial,'end':end,
            'limitations':['A successful queue submission does not prove the shader consumed this slice',
                           'GPU writes and concurrent host writes are not tracked; CPU observations can be stale or torn',
                           'Matrix purpose, jitter and frame identity require independent validation']}
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('samples',type=Path);p.add_argument('--output',type=Path)
    a=p.parse_args();r=analyze(a.samples);text=json.dumps(r,indent=2,allow_nan=False)+'\n'
    if a.output:a.output.write_text(text)
    else:print(text,end='')
