#!/usr/bin/env python3
"""Fetch the pinned, tested official NVIDIA DLSS SR release (no binaries in git)."""
import hashlib
import json
from pathlib import Path
import urllib.request

repo=Path(__file__).resolve().parents[1]
source=json.loads((repo/'profiles/research/dlss-sr-nvidia-310.9.1.json').read_text())
out=repo/'build/dlss-sr';out.mkdir(parents=True,exist_ok=True)
request=urllib.request.Request('https://api.github.com/repos/NVIDIA/DLSS/releases/latest',headers={'User-Agent':'DLSS5VKLayer'})
with urllib.request.urlopen(request,timeout=30) as response:latest=json.load(response)
if latest['tag_name']!=source['tag']:
    raise SystemExit('New NVIDIA release '+latest['tag_name']+' needs preset and Vulkan validation before updating the pin')
dll=out/'nvngx_dlss.dll'
if dll.exists() and hashlib.sha256(dll.read_bytes()).hexdigest()==source['sha256']:
    print('Already verified:',source['version'])
else:
    with urllib.request.urlopen(source['url'],timeout=60) as response:data=response.read()
    if len(data)!=source['bytes'] or hashlib.sha256(data).hexdigest()!=source['sha256']:
        raise SystemExit('NVIDIA download did not match the pinned release')
    temporary=dll.with_suffix('.tmp');temporary.write_bytes(data);temporary.replace(dll)
(out/'source.json').write_text(json.dumps(source,indent=2)+'\n')
print(json.dumps(source,indent=2))
