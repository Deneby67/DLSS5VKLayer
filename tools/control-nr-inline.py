#!/usr/bin/env python3
"""Enable/disable the installed native-loader NR adapter for one verified live RDR2 session."""
import argparse,json,os,re,tempfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('log',type=Path,help='Per-launch adapter.log under ~/.local/state/dlssnr/native-inline')
p.add_argument('mode',choices=('status','on','off'),default='status',nargs='?')
a=p.parse_args();log=a.log.resolve();root=(Path.home()/'.local/state/dlssnr/native-inline').resolve()
if log.name!='adapter.log' or not log.is_relative_to(root):p.error('Expected a private native-inline launch log')
text=log.read_text(errors='replace')
ready=set(re.findall(r'\[nr-inline\] arm-ready pid=(\d+) token=(\d+);',text))
if len(ready)!=1:
    if a.mode=='status':print(json.dumps(dict(ready=False,reason='No unique SR evaluation session has announced its control token')));raise SystemExit(0)
    p.error('No unique arm-ready session: start native DLSS in the game scene first')
winpid,token=next(iter(ready));stat=log.stat();live=[]
for proc in Path('/proc').iterdir():
    if not proc.name.isdigit():continue
    try:
        if (proc/'comm').read_text().strip().lower()!='rdr2.exe':continue
        env=(proc/'environ').read_bytes().split(b'\0')
        if ('DLSSNR_ARM_FILE=Z:'+str(log.parent/'arm.txt')).encode() not in env:continue
        for fd in (proc/'fd').iterdir():
            try:
                s=fd.stat()
                if (s.st_dev,s.st_ino)==(stat.st_dev,stat.st_ino):live.append(int(proc.name));break
            except (OSError,ProcessLookupError):pass
    except (OSError,ProcessLookupError):pass
if len(live)!=1:p.error('Log is not held open by exactly one matching live RDR2 process')
arm=log.parent/'arm.txt'
if a.mode!='status':
    fd,tmp=tempfile.mkstemp(dir=log.parent,prefix='.arm-')
    try:
        with os.fdopen(fd,'w') as f:f.write(f'{winpid} {token} {int(a.mode=="on")}\n');f.flush();os.fsync(f.fileno())
        os.replace(tmp,arm)
    finally:
        if os.path.exists(tmp):os.unlink(tmp)
request=arm.read_text().strip() if arm.exists() else None
states=re.findall(r'\[nr-inline\] arm-state (enabled|disabled)',text)
print(json.dumps(dict(linux_pid=live[0],windows_pid=int(winpid),ready=True,
    requested_on=request==f'{winpid} {token} 1',last_observed_state=states[-1] if states else 'disabled',
    nr_recorded='recorded NR-before-SR calls=' in text,
    note='Request polled on SR evaluation, at most every 250 ms; CPU recording is not proof of GPU completion.'),indent=2))
