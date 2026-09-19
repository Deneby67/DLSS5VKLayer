#!/usr/bin/env python3
"""GPU integration tests for metadata capture and fail-open behavior (no presentation)."""
import argparse, hashlib, importlib.util, json, os, re, struct, subprocess, tempfile
import sys
sys.dont_write_bytecode=True
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--layer',type=Path,required=True)
p.add_argument('--probe',type=Path,required=True)
p.add_argument('--validation',type=Path,help='Khronos validation manifest with an absolute library path')
a=p.parse_args()
repo=Path(__file__).resolve().parents[1]
draw_words=[int(x,16) for x in re.findall(r'0x([0-9a-f]{8})u',(repo/'test_layer/camera_draw_spv.h').read_text())]
draw_shader_hash=hashlib.sha256(struct.pack('<%dI'%len(draw_words),*draw_words)).hexdigest()
spec=importlib.util.spec_from_file_location('discovery_analysis',repo/'tools/analyze-discovery.py')
analysis=importlib.util.module_from_spec(spec); spec.loader.exec_module(analysis)
with tempfile.TemporaryDirectory(prefix='fg-discovery-test-') as tmp:
    root=Path(tmp); manifests=root/'manifests'; manifests.mkdir()
    (manifests/'discovery.json').write_text(json.dumps({'file_format_version':'1.0.0','layer':{
        'name':'VK_LAYER_DLSSFG_discovery_test','type':'GLOBAL','library_path':str(a.layer.resolve()),'api_version':'1.3.0',
        'implementation_version':'1','description':'Discovery test'}}))
    env=os.environ.copy(); env.update(VK_LAYER_PATH=str(manifests),VK_INSTANCE_LAYERS='VK_LAYER_DLSSFG_discovery_test',
        VKLayer_DLSS5='0',DLSSNR_IDLE_REPAINT='0',DISABLE_VULKAN_RENDERDOC_CAPTURE_1_46='1')
    env.pop('ENABLE_VULKAN_RENDERDOC_CAPTURE',None)
    env.pop('VK_LOADER_LAYERS_DISABLE',None)
    if a.validation:
        validation=json.loads(a.validation.read_text())
        library=Path(validation['layer']['library_path'])
        assert library.is_absolute() and library.is_file()
        (manifests/'validation.json').write_text(json.dumps(validation))
        env['VK_INSTANCE_LAYERS']+=':VK_LAYER_KHRONOS_validation'
        env['DLSSFG_TEST_VALIDATION']='1'
    def run(name,path,mode='normal'):
        if path is None: env.pop('DLSSFG_DISCOVERY_DIR',None)
        else: env['DLSSFG_DISCOVERY_DIR']=str(path)
        r=subprocess.run([name,mode],executable=str(a.probe.resolve()),env=env,text=True,
            stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=60)
        print(name,mode,r.returncode,r.stdout[-1600:],flush=True)
        assert r.returncode==0
        assert 'Validation Error' not in r.stdout and 'VUID-' not in r.stdout
    for name in ['Launcher.exe','SocialClubHelper.exe','RDR2.exe.bak','/RDR2.exe/Launcher.exe']:
        run(name,root/'excluded'); assert not (root/'excluded').exists()
    run('RDR2.exe',None)
    env['DLSSFG_TEST_DRAW_ASSOCIATIONS']='1'
    run('RDR2.exe','/dev/null/impossible','camera-pass')
    env.pop('DLSSFG_TEST_DRAW_ASSOCIATIONS',None)
    run(r'Z:\Games\RDR2.exe',root/'good')
    session=next((root/'good').iterdir())
    records=[json.loads(x) for x in (session/'events.jsonl').read_text().splitlines()]
    assert not (session/'stopped.txt').exists()
    assert [r['seq'] for r in records]==list(range(1,len(records)+1))
    by=lambda event:[r for r in records if r['event']==event]
    assert len(by('shader'))==2 and len(list((session/'shaders').iterdir()))==1
    for shader in by('shader'):
        blob=(session/'shaders'/(shader['sha256']+'.spv')).read_bytes()
        assert hashlib.sha256(blob).hexdigest()==shader['sha256'] and len(blob)==shader['bytes']
    assert by('compute_pipeline')[0]['stage']['sha256']==by('shader')[0]['sha256']
    ids=[r['id'] for r in records if 'id' in r]; assert len(ids)==len(set(ids))
    assert len(by('buffer'))==129 and len(by('descriptor_set'))==3
    assert [r['set']['id'] for r in by('descriptor_write')]==[r['id'] for r in by('descriptor_set')]
    assert all(r['resources'][0]['buffer']['id']==by('buffer')[0]['id'] for r in by('descriptor_write'))
    assert by('image_view')[0]['image']['id']==by('image')[0]['id']
    assert by('device_destroy') and not by('session')[0]['gpu_contents_captured']
    report=analysis.analyze(session)
    assert report['validated_profile'] is False and report['gpu_contents_captured'] is False
    assert len(report['depth_attachment_candidates'])==1 and not report['motion_format_candidates_unverified']
    # Independently inspect a shipped shader whose source declares three bindings.
    blob=(repo/'layer_linux/src/scaling/bcus_Shader_Vk.spv').read_bytes()
    fixture=root/(hashlib.sha256(blob).hexdigest()+'.spv'); fixture.write_bytes(blob)
    assert {(d['set'],d['binding']) for d in analysis.reflect(fixture)['descriptors']}=={(0,0),(0,1),(0,2)}
    fixture.write_bytes(blob+b'xxxx')
    try: analysis.reflect(fixture)
    except ValueError: pass
    else: raise AssertionError('corrupt shader accepted')
    run('rdr2.EXE',root/'limit','limit')
    limited=next((root/'limit').iterdir())
    assert not (limited/'stopped.txt').exists()
    limited_records=[json.loads(x) for x in (limited/'events.jsonl').read_text().splitlines()]
    assert sum(r['event']=='descriptor_budget_exhausted' for r in limited_records)==1
    assert limited_records[-1]['event']=='device_destroy'
    assert [r['seq'] for r in limited_records]==list(range(1,len(limited_records)+1))
    assert (limited/'events.jsonl').stat().st_size<=64<<20
    env['DLSSFG_SHADER_LIMIT_MIB']='0'
    run('RDR2.exe',root/'shader-limit')
    capped=next((root/'shader-limit').iterdir())
    assert not (capped/'stopped.txt').exists()
    cr=[json.loads(x) for x in (capped/'events.jsonl').read_text().splitlines()]
    assert sum(r['event']=='shader_budget_exhausted' for r in cr)==1
    assert len([r for r in cr if r['event']=='shader' and not r['binary_saved']])==2
    assert not list((capped/'shaders').iterdir()) and cr[-1]['event']=='device_destroy'
    assert len(analysis.analyze(capped)['shader_binaries_omitted'])==1
    env.pop('DLSSFG_SHADER_LIMIT_MIB',None)
    env['DLSSFG_METADATA_LIMIT_MIB']='1'
    for camera_name in ('camera','camera-device-local','camera-draw'):
        if camera_name!='camera':env['DLSSFG_TEST_DEVICE_LOCAL_CAMERA']='1'
        else:env.pop('DLSSFG_TEST_DEVICE_LOCAL_CAMERA',None)
        if camera_name=='camera-draw':env['DLSSFG_TEST_DRAW_ASSOCIATIONS']='1'
        else:env.pop('DLSSFG_TEST_DRAW_ASSOCIATIONS',None)
        run('RDR2.exe',root/camera_name,'camera')
        camera=next((root/camera_name).iterdir())
        assert (camera/'stopped.txt').read_text()=='metadata limit reached'
        rows=[json.loads(x) for x in next((camera/'camera').glob('samples-*.jsonl')).read_text().splitlines()]
        samples=[r for r in rows if r['event']=='cpu_snapshot_before_submit']
        assert len(samples)==3, len(samples)
        assert all(s['read_method'] in ('process_vm_readv','pipe_copy_from_user') for s in samples)
        for base,s in zip([1,2,3],samples):
            assert bytes.fromhex(s['bytes_hex'])==struct.pack('<116f',*(base+i*.25 for i in range(116)))
            assert s['offset']==256 and s['binding']==29 and not s['gpu_completion_verified'] and not s['camera_verified']
            assert any(r['event']=='submission_result' and r['submission']==s['submission'] and r['result']==0 for r in rows)
            if camera_name=='camera-draw':
                assert len(s['draw_links'])==1
                link=s['draw_links'][0]
                assert link['kind']=='direct' and link['recorded_calls']==1
                assert link['set_index']==0 and not link['gpu_execution_verified']
                assert link['stages'][0]['stage']==1 and link['stages'][0]['sha256']==draw_shader_hash
            else:assert s['draw_links']==[]
        assert rows[-1]['event']=='end' and rows[-1]['samples']==3
        assert rows[-1]['misses']['memory not mapped']>=1
        assert rows[-1]['misses']['no tracked bound sets in sampled submission']>=1
print('PASS: process scope, disabled mode, I/O failure fallback, hashes/dedup, resource generations, log cap')
