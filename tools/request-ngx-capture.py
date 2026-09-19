#!/usr/bin/env python3
"""Request bounded NGX parameter observations from an already loaded RDR2 proxy."""
import argparse
import json
import os
from pathlib import Path
import tempfile
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('log', type=Path, help='NGX JSONL log held open by the live RDR2 process')
p.add_argument('--samples', type=int, default=32)
p.add_argument('--duration-ms', type=int, default=5000)
a = p.parse_args()
if not 1 <= a.samples <= 256 or not 100 <= a.duration_ms <= 30000:
    p.error('Use 1..256 samples and 100..30000 milliseconds')
log = a.log.resolve(strict=True)
records = []
for line in log.read_text().splitlines():
    try:
        records.append(json.loads(line))
    except json.JSONDecodeError:
        continue  # A writer may currently be appending the final record.
pid = records[0].get('pid') if records else None
if (not isinstance(pid, int) or pid <= 0 or not log.name.startswith(f'ngx-{pid}-') or
        not any(r.get('schema') == 1 and r.get('pid') == pid and
                r.get('event') == 'intercept' and
                r.get('export') == 'NVSDK_NGX_VULKAN_EvaluateFeature' for r in records)):
    p.error('Log does not confirm an NGX evaluation hook')
# GetCurrentProcessId is a Wine/Windows PID, not the Linux /proc PID. Establish
# ownership using the open log inode before writing its Windows-PID control file.
identity = log.stat()
owners = []
for proc in Path('/proc').iterdir():
    if not proc.name.isdecimal():
        continue
    try:
        if proc.stat().st_uid != os.getuid() or (proc/'comm').read_text().strip().lower() != 'rdr2.exe':
            continue
        for entry in (proc/'fd').iterdir():
            try:
                target = entry.stat()
                if (target.st_dev, target.st_ino) == (identity.st_dev, identity.st_ino):
                    owners.append(int(proc.name))
                    break
            except OSError:
                continue
    except OSError:
        continue
if len(owners) != 1:
    p.error('Log must be held open by exactly one live RDR2.exe owned by this user')
request = time.time_ns()
fd, tmp = tempfile.mkstemp(dir=log.parent, prefix='request-')
try:
    with os.fdopen(fd, 'w') as f: f.write(f'{request} {a.samples} {a.duration_ms}\n')
    os.replace(tmp, log.parent/f'request-{pid}.txt')
finally:
    if os.path.exists(tmp): os.unlink(tmp)
print(f'Request {request}: at most {a.samples} evaluations over {a.duration_ms} ms; Windows PID {pid}, Linux PID {owners[0]}')
