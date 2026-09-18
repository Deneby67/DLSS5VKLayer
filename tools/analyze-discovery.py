#!/usr/bin/env python3
"""Summarize metadata candidates, never produce a validated game profile."""
import argparse, collections, hashlib, json, struct
from pathlib import Path

def reflect(path):
    blob=path.read_bytes()
    if len(blob)<20 or len(blob)%4 or len(blob)>16<<20: raise ValueError('invalid SPIR-V size')
    if hashlib.sha256(blob).hexdigest()!=path.stem: raise ValueError('shader SHA-256 mismatch')
    words=struct.unpack('<%dI'%(len(blob)//4),blob)
    if words[0]!=0x07230203: raise ValueError('invalid SPIR-V magic')
    names={}; decorations={}; members={}; types={}; variables=[]
    def string(w): return struct.pack('<%dI'%len(w),*w).split(b'\0')[0].decode('utf-8','replace')
    pos=5
    while pos<len(words):
        n,op=words[pos]>>16,words[pos]&65535
        if n<1 or pos+n>len(words): raise ValueError('truncated SPIR-V instruction')
        x=words[pos+1:pos+n]; pos+=n
        if op==5 and len(x)>=2: names[x[0]]=string(x[1:]) # OpName
        elif op==6 and len(x)>=3: members.setdefault((x[0],x[1]),{})['name']=string(x[2:])
        elif op==71 and len(x)>=2: decorations.setdefault(x[0],{})[x[1]]=list(x[2:])
        elif op==72 and len(x)>=3: members.setdefault((x[0],x[1]),{})[x[2]]=list(x[3:])
        elif 19<=op<=33 and x: types[x[0]]=(op,x[1:])
        elif op==59 and len(x)>=3: variables.append(x[:3])
    descriptors=[]
    for type_id,var,storage in variables:
        dec=decorations.get(var,{})
        if 33 not in dec or 34 not in dec: continue # Binding, DescriptorSet
        item={'variable_id':var,'name':names.get(var,''),'set':dec[34][0],
              'binding':dec[33][0],'storage_class':storage,'matrix_members':[]}
        pointer=types.get(type_id,(None,()))
        if pointer[0]==32 and len(pointer[1])>=2:
            struct_id=pointer[1][1]; st=types.get(struct_id,(None,()))
            if st[0]==30:
                for i,member_type in enumerate(st[1]):
                    m=types.get(member_type,(None,())); md=members.get((struct_id,i),{})
                    if m[0]==24: # Matrix type; purpose cannot be inferred from this.
                        item['matrix_members'].append({'member':i,'name':md.get('name',''),
                            'offset':md.get(35,[None])[0],'matrix_stride':md.get(7,[None])[0],
                            'row_major':4 in md,'column_major':5 in md,'columns':m[1][1]})
        descriptors.append(item)
    return {'sha256':path.stem,'descriptors':descriptors,
            'reflection_limits':['direct decorations only','top-level matrix members only','no camera semantics or values']}

def analyze(session):
    counts=collections.Counter(); shaders=set(); omitted=set(); depths=[]; motion=[]; pipeline_shaders=collections.Counter()
    limits=[]; last_seq=0; markers=0; partial=False
    with (session/'events.jsonl').open() as f:
        for line in f:
            try: r=json.loads(line)
            except json.JSONDecodeError:
                partial=True; break # Active capture may have a trailing partial write.
            if r['seq']!=last_seq+1: raise ValueError('event sequence gap')
            last_seq=r['seq']; markers=max(markers,r.get('present_marker',0)); event=r['event']; counts[event]+=1
            if event=='session':
                if r['schema']!=1: raise ValueError('unsupported discovery schema')
                limits=r['limitations']
            elif event=='shader':
                if r.get('binary_saved',True): shaders.add(r['sha256'])
                else: omitted.add(r['sha256'])
            elif event in ('graphics_pipeline','compute_pipeline'):
                for stage in r.get('stages',[r.get('stage',{})]):
                    if stage.get('sha256'): pipeline_shaders[stage['sha256']]+=1
            elif event=='image':
                if r['usage']&0x20: depths.append(r) # DEPTH_STENCIL_ATTACHMENT
                # Only a format/usage heuristic: RG16/RG32 float or RG16 SNORM.
                if r['format'] in (78,83,103) and r['usage']&(0x10|0x8): motion.append(r)
    status=session/'stopped.txt'
    reflected=[reflect(session/'shaders'/(h+'.spv')) for h in sorted(shaders)]
    matrix_blocks={}
    for shader in reflected:
        for descriptor in shader['descriptors']:
            if not descriptor['matrix_members']: continue
            signature=(descriptor['set'],descriptor['binding'],descriptor['storage_class'],
                tuple((m['offset'],m['matrix_stride'],m['row_major'],m['column_major'],m['columns']) for m in descriptor['matrix_members']))
            if signature not in matrix_blocks:
                matrix_blocks[signature]={'set':descriptor['set'],'binding':descriptor['binding'],
                    'storage_class':descriptor['storage_class'],'members':descriptor['matrix_members'],
                    'shader_count':0,'example_shader_hashes':[],'camera_verified':False}
            block=matrix_blocks[signature]; block['shader_count']+=1
            if len(block['example_shader_hashes'])<3: block['example_shader_hashes'].append(shader['sha256'])
    return {'schema':1,'validated_profile':False,'gpu_contents_captured':False,
        'present_markers_seen':markers,'event_counts':dict(counts),'trailing_partial_line':partial,
        'stopped_reason':status.read_text() if status.exists() else None,'limitations':limits,
        'depth_attachment_candidates':depths,'motion_format_candidates_unverified':motion,
        'shader_pipeline_references':dict(pipeline_shaders),
        'shaders':reflected,'shader_binaries_omitted':sorted(omitted-shaders),
        'matrix_block_candidates':sorted(matrix_blocks.values(),key=lambda b:-b['shader_count']),
        'next_required':['command submission/resource history','depth/motion readback validation',
                         'camera buffer values under movement and camera cuts','exact game executable hash']}
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('session',type=Path); p.add_argument('--output',type=Path)
    a=p.parse_args(); report=analyze(a.session)
    if a.output: a.output.write_text(json.dumps(report,indent=2)+'\n')
    else: print(json.dumps(report,indent=2))
