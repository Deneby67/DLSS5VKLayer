#!/usr/bin/env python3
"""Check that provisional resource joins cannot cross layout/object generations."""
import importlib.util,json,sys,tempfile
from pathlib import Path
sys.dont_write_bytecode=True
spec=importlib.util.spec_from_file_location('relate',Path(__file__).with_name('relate-discovery.py'))
mod=importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
# Same driver handle reused by a different image; joins must use recorded ids.
records=[
 {'event':'image','id':1,'extent':[100,80,1],'format':83},
 {'event':'image_view','id':2,'image':{'id':1,'handle':999}},
 {'event':'image','id':3,'extent':[1,1,1],'format':37},
 {'event':'image_view','id':4,'image':{'id':3,'handle':999}},
 {'event':'buffer','id':5,'bytes':2048},
 {'event':'pipeline_layout','id':6,'sets':[{'id':10}]},
 {'event':'graphics_pipeline','id':7,'layout':{'id':6},'stages':[{'stage':1,'sha256':'known'}]},
 {'event':'descriptor_set','id':8,'layout':{'id':10}},
 {'event':'descriptor_set','id':9,'layout':{'id':11}},
 {'event':'descriptor_write','set':{'id':8},'binding':29,'type':6,'resources':[{'buffer':{'id':5},'offset':256,'range':464}]},
 {'event':'descriptor_write','set':{'id':9},'binding':29,'type':6,'resources':[{'buffer':{'id':5},'offset':512,'range':464}]},
 {'event':'descriptor_write','set':{'id':8},'binding':96,'type':2,'resources':[{'view':{'id':2}}]},
 {'event':'descriptor_write','set':{'id':8},'binding':96,'type':2,'resources':[{'view':{'id':4}}]},
 {'event':'descriptor_copy','source':{'id':8},'target':{'id':9}},
 {'event':'render_pass','id':12,'subpasses':[{'depth':0}]},
 {'event':'framebuffer','id':13,'render_pass':{'id':12},'views':[{'id':2}]},
]
report={'shaders':[{'sha256':'known','descriptors':[{'set':0,'binding':29,'matrix_members':[{'offset':128}]}]}],
        'depth_attachment_candidates':[],'motion_format_candidates_unverified':[records[0]]}
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp)
    (p/'events.jsonl').write_text(''.join(json.dumps(dict(r,seq=i+1,present_marker=i))+'\n' for i,r in enumerate(records)))
    r=mod.relate(p,report)
    assert len(r['matrix_buffer_candidates'])==1
    c=r['matrix_buffer_candidates'][0]
    assert (c['buffer_id'],c['offset'],c['range'],c['writes'])==(5,256,464,1)
    assert c['camera_verified'] is False and r['validated_profile'] is False
    assert len(r['sampled_image_candidates'])==1 and r['sampled_image_candidates'][0]['image']['id']==1
    assert r['sampled_image_candidates'][0]['writes']==1
    assert r['descriptor_copy_count']==1
    assert r['framebuffer_depth_candidates'][0]['executed'] is None
print('PASS: exact layout identity, object generations, descriptor copies remain unresolved, no execution/semantic claims')
