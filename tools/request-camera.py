#!/usr/bin/env python3
"""Arm a bounded CPU snapshot window in a live RDR2 discovery session."""
import argparse,json,os,tempfile,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('session',type=Path)
p.add_argument('--device',type=int);p.add_argument('--duration-ms',type=int,default=5000);p.add_argument('--samples',type=int,default=256)
a=p.parse_args()
if not 100<=a.duration_ms<=30000 or not 1<=a.samples<=1024:p.error('Duration must be 100..30000 ms; samples 1..1024')
device=a.device
if device is None:
    with (a.session/'events.jsonl').open() as f:
        for line in f:
            try:r=json.loads(line)
            except json.JSONDecodeError:break
            if r['event']=='present_marker':device=r['device']
if device is None:p.error('No presenting device observed; specify --device')
control=a.session/'camera';support=control/f'support-{device}.json'
if not support.is_file():p.error('This device has no camera probe capability; restart with the updated layer')
s=json.loads(support.read_text())
if s.get('schema')!=1 or s.get('device')!=device:p.error('Unsupported camera probe capability')
try:os.kill(s['pid'],0)
except (ProcessLookupError,PermissionError):p.error('The captured process is not accessible/running')
if (control/f'disabled-{device}.txt').exists():p.error('Camera probe disabled: '+(control/f'disabled-{device}.txt').read_text())
request={'id':time.time_ns(),'device':device,'duration_ms':a.duration_ms,'max_samples':a.samples}
fd,name=tempfile.mkstemp(prefix='request-',dir=control)
try:
    with os.fdopen(fd,'w') as f:json.dump(request,f);f.write('\n')
    os.replace(name,control/'request.json')
finally:
    if os.path.exists(name):os.unlink(name)
print(json.dumps({'request':request,'output':str(control/f"samples-{device}-{request['id']}.jsonl")},indent=2))
