#!/usr/bin/env python3
"""Request one same-frame NR/HDR snapshot from the installed, running adapter."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import tempfile

spec=importlib.util.spec_from_file_location('nr_control',Path(__file__).with_name('control-nr-inline.py'))
control=importlib.util.module_from_spec(spec);spec.loader.exec_module(control)


def request(home,selected='auto',proc_root=Path('/proc')):
    s=control.status(home,selected,proc_root)
    if s['status']!='recording' or not s.get('requested_on'):
        raise ValueError(s.get('reason','Enable NR in a loaded scene first.'))
    log=Path(s['log'])
    if log.with_name('hdr-capture.request').exists() or log.with_name('hdr-snapshot.json').exists():
        raise ValueError('A snapshot was already requested for this session. Inspect its log/result; do not request a second one.')
    fd,tmp=tempfile.mkstemp(dir=log.parent,prefix='.hdr-request-')
    try:
        with os.fdopen(fd,'w') as f:
            f.write(f'{s["windows_pid"]} {s["token"]}\n');f.flush();os.fsync(f.fileno())
        os.replace(tmp,log.with_name('hdr-capture.request'))
    finally:
        if os.path.exists(tmp):os.unlink(tmp)
    return dict(status='requested',directory=str(log.parent),linux_pid=s['linux_pid'],token=s['token'])


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--log',default='auto')
    a=p.parse_args()
    try:print(json.dumps(request(Path.home(),a.log),indent=2))
    except (OSError,ValueError,KeyError) as e:p.exit(1,str(e)+'\n')

if __name__=='__main__':main()
