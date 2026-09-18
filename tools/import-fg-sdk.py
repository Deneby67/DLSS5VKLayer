#!/usr/bin/env python3
"""Import the pinned production FG DLL from an official, locally downloaded SDK ZIP."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('archive',type=Path)
a=p.parse_args()
source='https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.1/streamline-sdk-v2.14.1.zip'
archive_hash='92c4d954631a1710da86ca3fa8d5034f2b9503838c95fc4ae977ae149319781b'
dll_hash='ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82'
member='bin/x64/nvngx_dlssg.dll'
if hashlib.sha256(a.archive.read_bytes()).hexdigest()!=archive_hash:
    p.error('SDK archive SHA-256 mismatch; download the pinned archive from '+source)
with zipfile.ZipFile(a.archive) as z: dll=z.read(member)
if hashlib.sha256(dll).hexdigest()!=dll_hash: p.error('FG DLL SHA-256 mismatch')
out=Path(__file__).resolve().parents[1]/'build/fg/binaries'
out.mkdir(parents=True,exist_ok=True)
(out/'nvngx_dlssg.dll').write_bytes(dll)
(out/'manifest.json').write_text(json.dumps(dict(source=source,archive_sha256=archive_hash,
    member=member,sha256=dll_hash,size=len(dll),version='310.9.1'),indent=2)+'\n')
print(f'Imported FG 310.9.1, SHA-256 {dll_hash}')
