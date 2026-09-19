#!/usr/bin/env python3
"""Summarize bounded startup breadcrumbs; an unmatched entry alone is not proof of a hang."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('log',type=Path)
a=p.parse_args()
pattern=re.compile(r'tick=(\d+) tid=(\d+) api=(\w+) call=(\d+) (enter|exit|leave) result=(-?\d+)$')
opened={};finished=Counter();issues=[];durations=[];failures=[]
for line in a.log.read_text().splitlines():
    m=pattern.fullmatch(line)
    if not m:issues.append('Unrecognized or incomplete line');continue
    tick,tid,api,call,phase,result=m.groups();tick=int(tick);result=int(result)
    key=(api,int(call),int(tid))
    if phase=='enter':
        if key in opened:issues.append('Duplicate entry: '+str(key))
        opened[key]=tick
    elif key in opened:
        start=opened.pop(key);finished[api]+=1;durations.append(dict(api=api,call=int(call),elapsed_ms=tick-start))
        if phase=='exit' and result<0:failures.append(dict(api=api,result=result))
    else:issues.append('Unmatched completion: '+str(key))
print(json.dumps(dict(completed=finished,unmatched_entries=[dict(api=k[0],call=k[1],tid=k[2],tick_ms=v) for k,v in opened.items()],
    slowest_completed=sorted(durations,key=lambda d:d['elapsed_ms'],reverse=True)[:8],vulkan_failures=failures,parse_issues=issues,
    limits='First 32 calls per API only. Unmatched entries may still be running; no inference about later calls or game responsiveness.'),indent=2))
