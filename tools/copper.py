#!/usr/bin/env python3
"""Copper V2: deterministic project orchestration and KiCad 9 adapters.
Engineering authority lives in copper-core. This file resolves local libraries,
places/routes candidate geometry, emits native KiCad files, and runs native gates.
No shell execution, network requests, or LLM engineering decisions.
"""
from __future__ import annotations
import argparse, contextlib, copy, csv, hashlib, heapq, io, json, math, os, re, shutil, subprocess, sys, uuid
from pathlib import Path
from hwc_kicad import (Atom as A, Quoted as Q, parse_sexpr, dump_sexpr, direct, one,
                      head, text_at, discover_symbol_dir, discover_footprint_dir, discover_cli)
ROOT=Path(__file__).resolve().parents[1]
NS=uuid.UUID('aa7c164c-f2ec-5244-b7eb-6726f0ee52d8')
# Build statuses. Any analysis warning means required data was missing (unknown rating,
# height or geometry), so the build is UNVERIFIED rather than PASS.
STATUS_PASS='INTERNAL_CHECKS_PASS_NATIVE_KICAD_REQUIRED'
STATUS_UNVERIFIED='INTERNAL_CHECKS_UNVERIFIED_MISSING_DATA'
STATUS_DRAFT='DRAFT_FAILED_CHECKS'
def sha256_file(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
# Outcome classes for the --result-json protocol (see main). Any other ValueError is INVALID_INPUT.
class Unverified(Exception):"""A required tool, model or data source is unavailable (UNVERIFIED)."""
class ToolUnavailable(Unverified,FileNotFoundError):"""A required executable or library directory is missing."""
class InvalidInput(ValueError):"""The input violates a declared contract (INVALID_INPUT)."""
class Failed(ValueError):"""A check ran and failed (FAIL)."""
class CoreError(ValueError):"""copper-core misbehaved, e.g. an unexpected exit code (ERROR)."""
def existing(path,what='Project file'):
    if not Path(path).is_file():raise InvalidInput(f'{what} not found: {path}')
    return path
def uid(s): return str(uuid.uuid5(NS,s))
def sx(tag,*v): return [A(tag),*v]
def number(v): return A(f'{float(v):.9g}')
def write_tree(path,tree):
    path=Path(path);path.parent.mkdir(parents=True,exist_ok=True)
    tmp=path.with_suffix(path.suffix+'.tmp');tmp.write_text(dump_sexpr(tree)+'\n',encoding='utf-8');tmp.replace(path)
def load(path): return parse_sexpr(Path(path).read_text(encoding='utf-8'))
def core_path(explicit=None):
    choices=[explicit,os.environ.get('COPPER_CORE')]+[str(ROOT/x/y) for x in ['build','build/Debug','build/Release','build-local','build-desktop'] for y in ['copper-core','copper-core.exe']]
    for p in choices:
        if p and Path(p).is_file():return str(Path(p).resolve())
    raise ToolUnavailable('Build copper-core first or set COPPER_CORE to its executable path.')
def core_run(project,core=None,mode='analyze',extra=()):
    result=subprocess.run([core_path(core),mode,str(Path(project).resolve()),*map(str,extra)],capture_output=True,text=True,timeout=120)
    if result.returncode not in (0,2) or not result.stdout.strip():
        message=result.stderr.strip() or f'core failed: {result.returncode}'
        raise (InvalidInput if message.startswith('PROJECT ERROR') else CoreError)(message)
    return (json.loads(result.stdout) if mode=='analyze' else result.stdout),result.returncode

BUNDLED_CATALOGUE=ROOT/'profiles/default.parts'
def validate_catalogue(path,core=None,label=None):
    """Schema-check a catalogue file with copper-core (same rules as a project's parts). Returns its summary."""
    result=subprocess.run([core_path(core),'catalogue',str(Path(existing(path,'Catalogue')).resolve())],capture_output=True,text=True,timeout=60)
    if result.returncode:raise InvalidInput(f'{label or path}: '+(result.stderr.strip() or f'catalogue check failed ({result.returncode})'))
    return json.loads(result.stdout)
def catalogue(path=None,core=None):
    """Bundled defaults overlaid by an optional user catalogue. Precedence is deterministic: a user part replaces
    the bundled part with the same id (keeping its position); new ids follow in file order. Each part records
    the catalogue it came from. A malformed catalogue raises before anything is used or written."""
    merged={}
    sources=[(BUNDLED_CATALOGUE,'bundled:default.parts')]+([(Path(path),'user:'+Path(path).name)] if path else [])
    for source,label in sources:
        validate_catalogue(source,core)
        for part in direct(load(source),'part'):
            part=[x for x in copy.deepcopy(part) if not (isinstance(x,list) and head(x)=='catalogue')]
            merged[str(part[1])]=part+[sx('catalogue',Q(label))]
    return list(merged.values())
def model_specs(core=None,_cache={}):
    """Device model specs registered in copper-core (contract v1), by id."""
    key=core_path(core)
    if key not in _cache:
        result=subprocess.run([key,'models'],capture_output=True,text=True,timeout=30)
        if result.returncode:raise CoreError(result.stderr.strip() or 'copper-core models failed')
        _cache[key]={m['id']:m for m in json.loads(result.stdout)['models']}
    return _cache[key]
def check_terminal_pin_names(ref,component,spec,symbol_pins):
    """Reject a pin map that KiCad pin names contradict, e.g. anode mapped to the pin named 'K'.
    Unnamed pins and names no terminal claims are accepted; only a definite swap is an error."""
    def accepts(terminal,name):return terminal['pin_names'] is None or name.upper() in terminal['pin_names'].upper().split('|')
    terminals={t['name']:t for t in spec['terminals']};names={text_at(p,'number'):text_at(p,'name') for p in symbol_pins}
    for terminal,pin in component['terminals'].items():
        name=names.get(pin) or ''
        if name in ('','~') or accepts(terminals[terminal],name):continue
        claimed=[other for other,t in terminals.items() if other!=terminal and t['pin_names'] and accepts(t,name)]
        if claimed:raise InvalidInput(f"{ref}: symbol pin {pin} is named '{name}' ({claimed[0]}) but the part maps it to terminal '{terminal}'; the pin map is swapped")
def resolved_rating(component,key):
    """Known value of a rating/height from the analysis report, or None when UNKNOWN."""
    rating=component['ratings'][key];return rating['value'] if rating['known'] else None
def new_project(args):
    parts=catalogue(args.catalogue,getattr(args,'core',None));template=args.template
    if template=='divider':
        comps=[('V1','voltage',args.vin,[('1','VIN'),('2','GND')],(0,0)),('R1','resistor',args.r,[('1','VIN'),('2','OUT')],(16,12)),('R2','resistor',args.r2,[('1','OUT'),('2','GND')],(24,12)),('J1','header3',0,[('1','VIN'),('2','OUT'),('3','GND')],(8,12))]
    elif template=='rc':
        comps=[('V1','voltage',args.vin,[('1','VIN'),('2','GND')],(0,0)),('R1','resistor',args.r,[('1','VIN'),('2','OUT')],(16,12)),('C1','capacitor',args.c,[('1','OUT'),('2','GND')],(24,12)),('J1','header3',0,[('1','VIN'),('2','OUT'),('3','GND')],(8,12))]
    elif template=='rl':
        comps=[('V1','voltage',args.vin,[('1','VIN'),('2','GND')],(0,0)),('R1','resistor',args.r,[('1','VIN'),('2','OUT')],(16,12)),('L1','inductor',args.l,[('1','OUT'),('2','GND')],(24,12)),('J1','header3',0,[('1','VIN'),('2','OUT'),('3','GND')],(8,12))]
    else:
        comps=[('V1','voltage',args.vin,[('1','VIN'),('2','GND')],(0,0)),('R1','resistor',args.r,[('1','VIN'),('2','OUT')],(16,12)),('D1','diode',0,[('1','GND'),('2','OUT')],(24,12)),('J1','header3',0,[('1','VIN'),('2','OUT'),('3','GND')],(8,12))]
    tree=sx('copper_project',sx('version',A('2')),sx('name',Q(template)),sx('nets',A('GND'),A('VIN'),A('OUT')),sx('board',*map(number,[40,30,1.6,.035,2])),sx('parts',*parts),sx('components',*[sx('component',A(ref),sx('part',A(part)),sx('value',number(value)),sx('pins',*[sx(pin,A(net)) for pin,net in pins]),sx('at',number(pos[0]),number(pos[1]),A('0'))) for ref,part,value,pins,pos in comps]))
    if args.rules:
        rules=load(args.rules)
        if head(rules)!='rules':raise ValueError('Rule profile must have (rules ...) root')
        tree.append(rules)
    if args.target is not None:tree.append(sx('targets',sx('voltage',A('OUT'),number(args.target-args.target_tolerance),number(args.target+args.target_tolerance))))
    if Path(args.out).exists() and not args.force:raise InvalidInput(f'{args.out} exists; use --force to replace it')
    write_tree(args.out,tree);print(f'Created {args.out}. Component values use SI units; ratings come from the selected catalogue.')
    return 0

def component_data(tree):
    parts={str(p[1]):p for p in direct(one(tree,'parts'),'part')};result=[]
    for c in direct(one(tree,'components'),'component'):
        part=parts[text_at(c,'part')];at=one(c,'at') or [None,0,0,0]
        result.append(dict(ref=str(c[1]),part=part,kind=text_at(part,'kind'),value=float(text_at(c,'value',default='0')),symbol=text_at(part,'symbol'),footprint=text_at(part,'footprint'),pins={str(x[0]):str(x[1]) for x in one(c,'pins')[1:]},x=float(at[1]),y=float(at[2]),rotation=float(at[3])))
    return result

def safe_lib_id(value):
    if not re.fullmatch(r'[A-Za-z0-9_+.-]+:[A-Za-z0-9_+().,~ -]+',value):raise ValueError(f'Invalid library ID: {value}')
    return value.split(':',1)
def resolve_symbol(libdir,libid,cache,seen=()):
    if libid in cache:return copy.deepcopy(cache[libid])
    if libid in seen:raise ValueError('Cyclic symbol inheritance')
    lib,name=safe_lib_id(libid);tree=load(libdir/f'{lib}.kicad_sym')
    child=next((copy.deepcopy(s) for s in direct(tree,'symbol') if s[1]==name),None)
    if child is None:raise ValueError(f'Symbol not found: {libid}')
    parent=text_at(child,'extends')
    if parent:
        base=resolve_symbol(libdir,f'{lib}:{parent}',cache,(*seen,libid))
        # Inheritance is flattened; child properties override by key and graphics by unit suffix.
        props={str(v[1]):v for v in direct(base,'property')};props.update({str(v[1]):v for v in direct(child,'property')})
        units={str(v[1]).rsplit('_',2)[-2:][0]+'_'+str(v[1]).rsplit('_',1)[-1]:v for v in direct(base,'symbol')}
        for v in direct(child,'symbol'):units['_'.join(str(v[1]).rsplit('_',2)[-2:])]=v
        other={head(v):v for v in base[2:] if isinstance(v,list) and head(v) not in ('property','symbol','extends')}
        other.update({head(v):v for v in child[2:] if isinstance(v,list) and head(v) not in ('property','symbol','extends')})
        child=sx('symbol',Q(libid),*other.values(),*props.values(),*units.values())
    child[1]=Q(libid)
    for unit in direct(child,'symbol'):unit[1]=Q(name+'_'+'_'.join(str(unit[1]).rsplit('_',2)[-2:]))
    cache[libid]=copy.deepcopy(child);return child

def symbol_pins(symbol):
    result=[]
    for unit in direct(symbol,'symbol'):
        match=re.search(r'_(\d+)_(\d+)$',str(unit[1]))
        if not match:raise ValueError('Malformed symbol unit')
        un,style=map(int,match.groups())
        if un>1:raise Unverified('Multi-unit symbols require an explicit unit mapping; V2 exporter refuses to guess')
        if style>1:continue
        result.extend(direct(unit,'pin'))
    numbers=[text_at(p,'number') for p in result]
    if len(set(numbers))!=len(numbers):raise ValueError('Duplicate/stacked symbol pins need explicit handling')
    return result

def effects(hidden=False):
    e=sx('effects',sx('font',sx('size',A('1.27'),A('1.27'))))
    if hidden:e.append(sx('hide',A('yes')))
    return e

def schematic(tree,components,symbol_dir,stem,bindings=None,specs=None):
    """bindings: report['components'] by ref; specs: model_specs(). When given, pin maps are checked against
    the symbol's pin names (logical terminal -> symbol pin -> footprint pad with the same number)."""
    rootid=uid(stem+'/schematic');lib=sx('lib_symbols');cache={}
    sch=sx('kicad_sch',sx('version',A('20250114')),sx('generator',Q('copper_v2')),sx('uuid',Q(rootid)),sx('paper',Q('A4')),lib)
    physical=[c for c in components if c['kind'] not in ('V','I')]
    for index,c in enumerate(physical):
        definition=resolve_symbol(symbol_dir,c['symbol'],cache);pins=symbol_pins(definition)
        if set(c['pins'])!={text_at(p,'number') for p in pins}:raise ValueError(f"{c['ref']}: project pins differ from symbol pins")
        binding=(bindings or {}).get(c['ref'])
        if binding and binding['model'] and specs:check_terminal_pin_names(c['ref'],binding,specs[binding['model']['id']],pins)
        if not any(s[1]==c['symbol'] for s in lib[1:]):lib.append(definition)
        x,y=50.8+(index%4)*50.8,50.8+(index//4)*50.8
        instance=sx('symbol',sx('lib_id',Q(c['symbol'])),sx('at',number(x),number(y),A('0')),sx('unit',A('1')),sx('exclude_from_sim',A('no')),sx('in_bom',A('yes')),sx('on_board',A('yes')),sx('dnp',A('no')),sx('uuid',Q(uid(stem+'/'+c['ref']))))
        for key,value,offset,hide in [('Reference',c['ref'],-5,False),('Value',f"{c['value']:.9g}",5,False),('Footprint',c['footprint'],0,True)]:
            instance.append(sx('property',Q(key),Q(value),sx('at',number(x+7),number(y+offset),A('0')),effects(hide)))
        instance.append(sx('instances',sx('project',Q(stem),sx('path',Q('/'+rootid),sx('reference',Q(c['ref'])),sx('unit',A('1'))))))
        for pin in pins:
            no=text_at(pin,'number');at=one(pin,'at');px,py=x+float(at[1]),y-float(at[2]);net=c['pins'][no]
            instance.append(sx('pin',Q(no),sx('uuid',Q(uid(stem+'/'+c['ref']+'/pin/'+no)))))
            # Labels sit exactly on actual pin endpoints; symbol coordinates have inverted Y.
            sch.append(sx('label',Q(net),sx('at',number(px),number(py),A('0')),effects(),sx('uuid',Q(uid(stem+'/'+c['ref']+'/label/'+no)))))
        sch.append(instance)
    sch.append(sx('sheet_instances',sx('path',Q('/'),sx('page',Q('1')))))
    sch.append(sx('embedded_fonts',A('no')))
    return sch

def rotate(x,y,angle):
    a=math.radians(angle);return (x*math.cos(a)+y*math.sin(a),-x*math.sin(a)+y*math.cos(a))
def footprint_geometry(c,fpdir,netids,stem):
    lib,name=safe_lib_id(c['footprint']);fp=load(fpdir/f'{lib}.pretty'/f'{name}.kicad_mod')
    if head(fp)!='footprint':raise ValueError('Expected KiCad footprint')
    if one(fp,'zone') or one(fp,'embedded_files'):raise Unverified('Footprint zones/embedded files are outside V2 geometry coverage')
    for obj in fp:
        if isinstance(obj,list) and head(obj)!='pad' and text_at(obj,'layer') in ('F.Cu','B.Cu','*.Cu') and head(obj)!='layer':raise Unverified('Footprint copper graphics are not represented by the supported pad geometry')
    if any(one(fp,key) for key in ('solder_mask_margin','solder_paste_margin','solder_paste_ratio')):raise Unverified('Per-footprint mask/paste overrides require explicit process support')
    fp[1]=Q(c['footprint']);fp[:]=[x for x in fp if not isinstance(x,list) or head(x) not in ('at','uuid','path')]
    fp.extend([sx('at',number(c['x']),number(c['y']),number(c['rotation'])),sx('uuid',Q(uid(stem+'/'+c['ref']+'/footprint'))),sx('path',Q('/'+uid(stem+'/schematic')+'/'+uid(stem+'/'+c['ref'])))])
    found_ref=False
    for prop in direct(fp,'property'):
        if prop[1]=='Reference':prop[2]=Q(c['ref']);found_ref=True
        if prop[1]=='Value':prop[2]=Q(f"{c['value']:.9g}")
    for t in direct(fp,'fp_text'):
        if t[1]=='reference':t[2]=Q(c['ref']);found_ref=True
        if t[1]=='value':t[2]=Q(f"{c['value']:.9g}")
    if not found_ref:fp.append(sx('fp_text',A('reference'),Q(c['ref']),sx('at',A('0'),A('-3')),sx('layer',Q('F.SilkS')),effects()))
    pads=[];mapped=set();geo=[]
    for pad in direct(fp,'pad'):
        no=str(pad[1]);kind=str(pad[2]);shape=str(pad[3]);at=one(pad,'at');size=one(pad,'size');drill=one(pad,'drill');layers=one(pad,'layers')
        if any(one(pad,key) for key in ('solder_mask_margin','solder_paste_margin','solder_paste_margin_ratio','chamfer','chamfer_ratio')):raise Unverified('Pad mask/paste/chamfer overrides are outside supported geometry')
        if not at or not size or not layers:raise ValueError('Pad lacks position, size or layers')
        if (kind=='thru_hole') != bool(drill):raise ValueError('Pad drill/type mismatch')
        if kind not in ('smd','thru_hole') or shape not in ('circle','rect','roundrect','oval'):raise Unverified(f"{c['ref']}:{no}: unsupported pad type/shape {kind}/{shape}")
        if no not in c['pins']:raise ValueError(f"{c['ref']}: physical pad {no} missing from electrical map")
        if drill and (len(drill)!=2 or not re.fullmatch(r'[0-9.]+',str(drill[1]))):raise Unverified('Slotted/offset drills require native geometry support')
        if any(str(x) not in ('F.Cu','F.Paste','F.Mask','*.Cu','*.Mask') for x in layers[1:]):raise Unverified('Only top-side SMD or through-hole pads are supported')
        dx,dy=rotate(float(at[1]),float(at[2]),c['rotation']);x,y=c['x']+dx,c['y']+dy
        angle=c['rotation']+(float(at[3]) if len(at)>3 else 0)
        if angle%90:raise Unverified('Pad rotation must be a multiple of 90 degrees')
        w,h=float(size[1]),float(size[2]);w,h=(h,w) if round(angle/90)%2 else (w,h)
        if w<=0 or h<=0:raise ValueError('Pad dimensions must be positive')
        circular=shape=='circle' and abs(w-h)<1e-9
        # C retains the exact axis-aligned core and rounded-corner radius.
        net=c['pins'][no];mapped.add(no);pad[:]=[v for v in pad if not isinstance(v,list) or head(v) not in ('net','uuid')]
        pad.extend([sx('net',A(str(netids[net])),Q('/'+net)),sx('uuid',Q(uid(stem+'/'+c['ref']+'/pad/'+no+'/'+str(len(pads)))))])
        pads.append(dict(ref=c['ref'],pin=no,net=net,x=x,y=y,w=w,h=h,through=kind=='thru_hole'))
        # diameter is conservative for legacy edge/annular reporting, exact adapter uses width/height.
        diameter=min(w,h)
        corner=min(w,h)/2 if shape=='oval' else (float(text_at(pad,'roundrect_rratio',default='0'))*min(w,h) if shape=='roundrect' else 0)
        if corner<0 or corner>min(w,h)/2:raise ValueError('Invalid rounded pad radius')
        geo.append(sx('pad',A(c['ref']),A(no),A(net),*map(number,[x,y,diameter,float(drill[1]) if drill else 0,0,w,h,0 if circular else 1,corner])))
    if mapped!=set(c['pins']):raise ValueError(f"{c['ref']}: symbol pin/footprint pad mismatch")
    # Use actual courtyard extents; a footprint lacking a courtyard is not silently invented.
    pts=[]
    for obj in fp:
        if isinstance(obj,list) and text_at(obj,'layer')=='F.CrtYd':
            for key in ('start','end','center'):
                at=one(obj,key)
                if at:
                    dx,dy=rotate(float(at[1]),float(at[2]),c['rotation']);pts.append((c['x']+dx,c['y']+dy))
            if head(obj) not in ('fp_rect','fp_line'):raise Unverified('Non-rectilinear courtyard requires explicit geometry support')
    if not pts:raise ValueError(f"{c['ref']}: footprint has no supported courtyard")
    lo=[min(p[i] for p in pts) for i in range(2)];hi=[max(p[i] for p in pts) for i in range(2)]
    geo.append(sx('body',A(c['ref']),*map(number,[(lo[0]+hi[0])/2,(lo[1]+hi[1])/2,hi[0]-lo[0],hi[1]-lo[1],c.get('height',0)])))
    # Regenerate all nested UUIDs deterministically; avoid duplicate library UUIDs.
    def uuids(n,path=''):
        if isinstance(n,list):
            for i,v in enumerate(n):
                if isinstance(v,list) and head(v)=='uuid':v[1]=Q(uid(stem+'/'+c['ref']+'/nested/'+path+str(i)))
                else:uuids(v,path+str(i)+'/')
    uuids(fp);return fp,pads,geo

def route(pads,width,height,trace_width,clearance,edge,via_diameter=.6):
    """Deterministic 2-layer Manhattan A* candidate router, with bounded search.
    Conservative obstacle envelopes; C exact geometry and KiCad DRC judge result.
    """
    grid=.25;nx=round(width/grid);ny=round(height/grid);traces=[];vias=[];occupied=[{},{}]
    radius=trace_width/2+clearance
    def node(p):return (round(p['x']/grid),round(p['y']/grid),0)
    def blocked(x,y,z,net,via=False):
        xx,yy=x*grid,y*grid;r=(via_diameter/2+clearance) if via else radius
        if min(xx,yy,width-xx,height-yy)<edge+r:return True
        for p in pads:
            if p['net']==net or (z==1 and not p['through']):continue
            if abs(xx-p['x'])<=p['w']/2+r and abs(yy-p['y'])<=p['h']/2+r:return True
        reach=math.ceil((r+trace_width/2)/grid)
        for a in range(x-reach,x+reach+1):
            for b in range(y-reach,y+reach+1):
                other=occupied[z].get((a,b))
                if other and other!=net and math.hypot(a-x,b-y)*grid<r+trace_width/2:return True
        for vx,vy,vnet in vias:
            if vnet!=net and math.hypot(xx-vx,yy-vy)<r+via_diameter/2:return True
        return False
    nets=sorted({p['net'] for p in pads})
    for net in nets:
        targets=[p for p in pads if p['net']==net]
        if len(targets)<2:continue
        joined=[targets.pop(0)]
        while targets:
            _,i,j=min((abs(a['x']-b['x'])+abs(a['y']-b['y']),i,j) for i,a in enumerate(joined) for j,b in enumerate(targets))
            a,b=joined[i],targets.pop(j);start,goal=node(a),node(b)
            if blocked(*start,net) or blocked(*goal,net):raise Failed(f'Cannot escape pad on net {net}; edit placement or constraints')
            heap=[(0,0,start)];cost={start:0};prev={};found=False
            while heap:
                _,g,u=heapq.heappop(heap)
                if g!=cost[u]:continue
                if u==goal:found=True;break
                if len(cost)>250000:break
                x,y,z=u
                for v,extra in [((x+1,y,z),1),((x-1,y,z),1),((x,y+1,z),1),((x,y-1,z),1),((x,y,1-z),20)]:
                    xx,yy,zz=v
                    if not (0<=xx<=nx and 0<=yy<=ny) or blocked(xx,yy,zz,net):continue
                    if zz!=z and (blocked(x,y,z,net,True) or blocked(x,y,zz,net,True)):continue
                    ng=g+extra
                    if ng<cost.get(v,1e30):cost[v]=ng;prev[v]=u;h=abs(xx-goal[0])+abs(yy-goal[1])+(20 if zz!=goal[2] else 0);heapq.heappush(heap,(ng+h,ng,v))
            if not found:raise Failed(f'Router could not connect {net}; no route is claimed')
            path=[goal]
            while path[-1]!=start:path.append(prev[path[-1]])
            path.reverse()
            for x,y,z in path:occupied[z][x,y]=net
            # Compress collinear grid runs to avoid thousands of segments.
            points=[path[0]]
            for k in range(1,len(path)-1):
                d1=tuple(path[k][q]-path[k-1][q] for q in range(3));d2=tuple(path[k+1][q]-path[k][q] for q in range(3))
                if d1!=d2:points.append(path[k])
            if len(path)>1:points.append(path[-1])
            traces.append((net,a['x'],a['y'],start[0]*grid,start[1]*grid,trace_width,0))
            for u,v in zip(points,points[1:]):
                if u[2]!=v[2]:vias.append((u[0]*grid,u[1]*grid,net))
                else:traces.append((net,u[0]*grid,u[1]*grid,v[0]*grid,v[1]*grid,trace_width,0 if u[2]==0 else 2))
            traces.append((net,goal[0]*grid,goal[1]*grid,b['x'],b['y'],trace_width,0));joined.append(b)
    return [t for t in traces if math.hypot(t[1]-t[3],t[2]-t[4])>1e-9],sorted(set(vias))

def rule(tree,key,default):return float(text_at(one(tree,'rules'),key,default=str(default)))
def export_project(args):
    project=Path(args.project).resolve();tree=load(project);out=Path(args.out).resolve();out.mkdir(parents=True,exist_ok=True)
    for marker in ('manifest.json','verification.json'):
        (out/marker).unlink(missing_ok=True)
    report,rc=core_run(project,args.core)
    if rc and not getattr(args,'draft',False):raise Failed('Electrical analysis failed; run check to inspect the findings before layout.')
    components=component_data(tree);nets=list(map(str,one(tree,'nets')[1:]));netids={n:i+1 for i,n in enumerate(nets)}
    symdir=discover_symbol_dir(args.symbol_dir);fpdir=discover_footprint_dir(args.footprint_dir)
    bindings={b['ref']:b for b in report['components']}
    for c in components:c['height']=resolved_rating(bindings[c['ref']],'height_mm') or 0  # 0 = not stated in geometry
    stem='design';sch=schematic(tree,components,symdir,stem,bindings,model_specs(args.core))
    dims=list(map(float,(one(tree,'board') or [None,40,30,1.6,.035,2])[1:]));w,h,thick,copper,layers=dims
    board=sx('kicad_pcb',sx('version',A('20241229')),sx('generator',Q('copper_v2')),sx('general',sx('thickness',number(thick))),sx('paper',Q('A4')),sx('layers',*[sx(str(i),Q(name),A(kind)) for i,name,kind in [(0,'F.Cu','signal'),(2,'B.Cu','signal'),(3,'B.Adhes','user'),(5,'F.SilkS','user'),(7,'B.SilkS','user'),(9,'F.Mask','user'),(11,'B.Mask','user'),(13,'Dwgs.User','user'),(25,'Edge.Cuts','user'),(31,'F.CrtYd','user'),(33,'B.CrtYd','user'),(35,'F.Fab','user'),(37,'B.Fab','user'),(1,'F.Adhes','user'),(19,'F.Paste','user'),(21,'B.Paste','user')]]),sx('setup',sx('pad_to_mask_clearance',number(rule(tree,'mask_expansion_mm',.05)))),sx('net',A('0'),Q('')))
    board.extend(sx('net',A(str(i)),Q('/'+n)) for n,i in netids.items())
    board.append(sx('gr_rect',sx('start',A('0'),A('0')),sx('end',number(w),number(h)),sx('stroke',sx('width',A('.05')),sx('type',A('default'))),sx('fill',A('none')),sx('layer',Q('Edge.Cuts')),sx('uuid',Q(uid('outline')))))
    pads=[];geometry=sx('geometry')
    for c in components:
        if c['kind'] in ('V','I'):continue
        fp,pp,gg=footprint_geometry(c,fpdir,netids,stem);board.append(fp);pads.extend(pp);geometry.extend(gg)
    traces=[];vias=[]
    if not args.no_route:traces,vias=route(pads,w,h,rule(tree,'trace_width_mm',.2),rule(tree,'clearance_mm',.2),rule(tree,'edge_mm',.3),rule(tree,'via_diameter_mm',.6))
    for i,(net,x1,y1,x2,y2,width,layer) in enumerate(traces):
        board.append(sx('segment',sx('start',number(x1),number(y1)),sx('end',number(x2),number(y2)),sx('width',number(width)),sx('layer',Q('F.Cu' if layer==0 else 'B.Cu')),sx('net',A(str(netids[net]))),sx('uuid',Q(uid('track/'+str(i))))))
        geometry.append(sx('trace',A(net),*map(number,[x1,y1,x2,y2,width,layer])))
    for i,(x,y,net) in enumerate(vias):
        diameter=rule(tree,'via_diameter_mm',.6);drill=rule(tree,'drill_mm',.3)
        board.append(sx('via',sx('at',number(x),number(y)),sx('size',number(diameter)),sx('drill',number(drill)),sx('layers',Q('F.Cu'),Q('B.Cu')),sx('net',A(str(netids[net]))),sx('uuid',Q(uid('via/'+str(i))))))
        geometry.append(sx('via',A(net),*map(number,[x,y,diameter,drill])))
    tree[:]=[x for x in tree if not isinstance(x,list) or head(x)!='geometry'];tree.append(geometry)
    write_tree(out/'resolved.copper',tree)
    report,rc=core_run(out/'resolved.copper',args.core)
    write_tree(out/'design.kicad_sch',sch);write_tree(out/'design.kicad_pcb',board)
    # Snapshot the exact selected source libraries and footprints, with local tables.
    # Native ERC/DRC can now resolve links without modifying global KiCad settings.
    symtable=sx('sym_lib_table',sx('version',A('7')));fptable=sx('fp_lib_table',sx('version',A('7')))
    for library in sorted({safe_lib_id(c['symbol'])[0] for c in components if c['kind'] not in ('V','I')}):
        target=out/'libraries/symbols'/f'{library}.kicad_sym';target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(symdir/f'{library}.kicad_sym',target)
        symtable.append(sx('lib',sx('name',Q(library)),sx('type',Q('KiCad')),sx('uri',Q('${KIPRJMOD}/libraries/symbols/'+library+'.kicad_sym')),sx('options',Q('')),sx('descr',Q('Exact local library snapshot'))))
    for library in sorted({safe_lib_id(c['footprint'])[0] for c in components if c['kind'] not in ('V','I')}):
        target=out/'libraries/footprints'/f'{library}.pretty';target.mkdir(parents=True,exist_ok=True)
        for c in components:
            if c['kind'] in ('V','I'):continue
            lib,foot=safe_lib_id(c['footprint'])
            if lib==library:shutil.copyfile(fpdir/f'{lib}.pretty'/f'{foot}.kicad_mod',target/f'{foot}.kicad_mod')
        fptable.append(sx('lib',sx('name',Q(library)),sx('type',Q('KiCad')),sx('uri',Q('${KIPRJMOD}/libraries/footprints/'+library+'.pretty')),sx('options',Q('')),sx('descr',Q('Exact selected footprints'))))
    write_tree(out/'sym-lib-table',symtable);write_tree(out/'fp-lib-table',fptable)
    (out/'design.kicad_pro').write_text(json.dumps({'meta':{'filename':'design.kicad_pro','version':1}},indent=2)+'\n')
    (out/'libraries/NOTICE.txt').write_text('Library files are exact snapshots from the selected local KiCad libraries. KiCad official library content is licensed CC-BY-SA-4.0 with the KiCad libraries exception; https://www.kicad.org/libraries/license/ . Custom libraries retain their original authorship and licensing. No ownership transfer is implied.\n')
    shutil.copyfile(ROOT/'docs/v2/KICAD_LIBRARY_LICENSE.md',out/'libraries/KICAD_LIBRARY_LICENSE.md')
    dru='(version 1)\n'
    for rule_name,constraint,limit in [('clearance','clearance',rule(tree,'clearance_mm',.2)),('width','track_width',rule(tree,'trace_width_mm',.2)),('edge','edge_clearance',rule(tree,'edge_mm',.3)),('drill','hole_size',rule(tree,'drill_mm',.3)),('annular','annular_width',rule(tree,'annular_mm',.15)),('hole_gap','hole_clearance',rule(tree,'hole_gap_mm',.25))]:
        dru+=f'(rule "Copper_{rule_name}" (constraint {constraint} (min {limit}mm)))\n'
    (out/'design.kicad_dru').write_text(dru)

    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    with (out/'bom.csv').open('w',newline='',encoding='utf-8') as f:
        writer=csv.writer(f);writer.writerow(['Reference','Part','Value_SI','Symbol','Footprint','Power_W','Voltage_V','Height_mm','Manufacturer','MPN','Datasheet'])
        for c in components:
            if c['kind'] not in ('V','I'):
                rated=[resolved_rating(bindings[c['ref']],k) for k in ('power_w','voltage_v','height_mm')]
                writer.writerow([c['ref'],c['part'][1],c['value'],c['symbol'],c['footprint'],*['UNKNOWN' if v is None else f'{v:.9g}' for v in rated],text_at(c['part'],'manufacturer'),text_at(c['part'],'mpn'),text_at(c['part'],'datasheet')])
    lines=['Copper V2 calculation report',f'DC solved: {report["dc_solved"]}',f'Errors: {report["errors"]}; warnings: {report["warnings"]}']
    for n in report['nodes']:lines.append(f'{n["name"]}: {n["voltage_v"]} V')
    for c in report['calculations']:lines.append(f'{c["ref"]}: {c["formula"]}; V={c["voltage_v"]} V, R={c["resistance_ohm"]} ohm, I={c["current_a"]} A, P={c["power_w"]} W')
    lines.extend(f'{v["severity"].upper()} {v["rule"]} {v["object"]}: {v["message"]}' for v in report['issues']);lines.append(report['coverage'])
    (out/'CALCULATIONS.txt').write_text('\n'.join(lines)+'\n')
    # Hash the actual source, resolved IR and outputs, never a statement of certification.
    files=['resolved.copper','design.kicad_sch','design.kicad_pcb','design.kicad_pro','design.kicad_dru','sym-lib-table','fp-lib-table','bom.csv','report.json']
    files+=sorted(str(p.relative_to(out)) for p in (out/'libraries').rglob('*') if p.is_file())
    unverified=sorted({v['rule'] for v in report['issues'] if v['severity']!='error'})
    try:source_path=os.path.relpath(project,out)
    except ValueError:source_path=str(project)  # Windows: different drive
    manifest={'schema':2,'source_sha256':sha256_file(project),'source_path':Path(source_path).as_posix(),'status':STATUS_DRAFT if rc else (STATUS_UNVERIFIED if unverified else STATUS_PASS),'unverified_rules':unverified,'files':{name:sha256_file(out/name) for name in files},'symbols':str(symdir),'footprints':str(fpdir)}
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    # Lightweight exact board preview for external viewing, independent of desktop renderer.
    import html
    svg=[f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="-2 -2 {w+4} {h+4}"><rect x="0" y="0" width="{w}" height="{h}" rx=".3" fill="#122c2b" stroke="#74d3bb" stroke-width=".1"/>']
    for net,x1,y1,x2,y2,width,layer in traces:svg.append(f'<path d="M{x1} {y1} L{x2} {y2}" stroke="{"#ef8069" if layer==0 else "#6a9edc"}" stroke-width="{width}"/>')
    for p in pads:svg.append(f'<rect x="{p["x"]-p["w"]/2}" y="{p["y"]-p["h"]/2}" width="{p["w"]}" height="{p["h"]}" fill="#edca7c"><title>{html.escape(p["ref"]+"."+p["pin"]+" "+p["net"])}</title></rect>')
    svg.append('</svg>');(out/'board.svg').write_text(''.join(svg))
    print(f'{manifest["status"]}: {out}\n{len(pads)} pads, {len(traces)} tracks, {len(vias)} vias; {report["errors"]} errors, {report["warnings"]} warnings.');return rc

def overall_status(statuses):
    return 'FAIL' if 'FAIL' in statuses else 'UNVERIFIED' if 'UNVERIFIED' in statuses else 'PASS'
def native_gates(out,cli):
    """KiCad 9.x ERC/DRC/netlist plus independent pin/net parity. Returns (version, gates)."""
    version=None;error='KiCad CLI not found; set --kicad-cli'
    if cli:
        try:version=subprocess.run([cli,'version'],capture_output=True,text=True,timeout=30)
        except (OSError,subprocess.SubprocessError) as exc:error=f'Cannot run KiCad CLI {cli}: {exc}'
    if version is None or version.returncode:
        return None,[{'gate':'kicad_cli','status':'UNVERIFIED','message':error if version is None else 'kicad-cli version failed: '+version.stderr.strip()}]
    found=version.stdout.strip()
    if not re.match(r'^9\.',found):return found,[{'gate':'kicad_version','status':'UNVERIFIED','message':f'Native validation targets KiCad 9.x only; found {found}'}]
    gates=[]
    for label,cmd in [('erc',['sch','erc','--format','json','--exit-code-violations','-o',str(out/'erc.json'),str(out/'design.kicad_sch')]),('drc',['pcb','drc','--format','json','--all-track-errors','--schematic-parity','--exit-code-violations','-o',str(out/'drc.json'),str(out/'design.kicad_pcb')]),('netlist',['sch','export','netlist','--format','kicadxml','-o',str(out/'netlist.xml'),str(out/'design.kicad_sch')])]:
        r=subprocess.run([cli,*cmd],capture_output=True,text=True,timeout=180);gates.append({'gate':label,'status':'PASS' if r.returncode==0 else 'FAIL','returncode':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
    # Compare all electrical physical-pin mappings against KiCad's own extracted netlist.
    if gates[-1]['returncode']==0:
        import xml.etree.ElementTree as ET
        root=ET.parse(out/'netlist.xml').getroot();actual={}
        for net in root.findall('./nets/net'):
            for node in net.findall('node'):actual[(node.attrib['ref'],node.attrib['pin'])]=net.attrib['name'].removeprefix('/')
        expected={(c['ref'],pin):net for c in component_data(load(out/'resolved.copper')) if c['kind'] not in ('V','I') for pin,net in c['pins'].items()}
        gates.append({'gate':'netlist_parity','status':'PASS' if expected==actual else 'FAIL','returncode':0 if expected==actual else 2,'message':'Matches electrical IR' if expected==actual else f'Pin/net mismatch: expected {expected}, got {actual}'})
    else:gates.append({'gate':'netlist_parity','status':'UNVERIFIED','message':'KiCad netlist export failed; parity not compared'})
    return found,gates
def verify(args):
    out=Path(args.out).resolve();marker=out/'verification.json'
    marker.unlink(missing_ok=True)  # A failed run must never leave an earlier PASS behind.
    manifest=json.loads(Path(existing(out/'manifest.json','Build manifest')).read_text());checks=[];gates=[];version=None
    def check(name,status,message):checks.append({'check':name,'status':status,'message':message})
    changed=sorted(name for name,digest in manifest['files'].items() if not (out/name).is_file() or sha256_file(out/name)!=digest)
    check('output_hashes','FAIL' if changed else 'PASS',f'Changed since build: {changed}; rebuild' if changed else 'All built files match the manifest')
    if not changed:
        project=getattr(args,'project',None) or (manifest.get('source_path') and str(out/manifest['source_path']))
        if not project:check('source_freshness','UNVERIFIED','Manifest has no source path; pass --project')
        elif not Path(project).is_file():check('source_freshness','UNVERIFIED',f'Source not found: {project}')
        elif sha256_file(project)!=manifest['source_sha256']:check('source_freshness','FAIL',f'{project} changed since build; rebuild')
        else:check('source_freshness','PASS','Source matches the build')
        build=manifest.get('status')
        check('build_status',{STATUS_PASS:'PASS',STATUS_UNVERIFIED:'UNVERIFIED'}.get(build,'FAIL'),f'Build status {build}')
        # Re-run the C engine on the built IR; the stored report must be reproduced exactly.
        try:fresh,rc=core_run(out/'resolved.copper',getattr(args,'core',None))
        except FileNotFoundError as exc:check('internal_reanalysis','UNVERIFIED',str(exc))
        except ValueError as exc:check('internal_reanalysis','FAIL',str(exc))
        else:
            stored=json.loads((out/'report.json').read_text())
            if fresh!=stored:check('internal_reanalysis','FAIL','Re-analysis differs from report.json')
            elif rc or fresh['errors']:check('internal_reanalysis','FAIL',f'{fresh["errors"]} internal errors')
            else:check('internal_reanalysis','PASS','Re-analysis reproduces report.json with no errors')
            missing=sorted({v['rule'] for v in fresh['issues'] if v['severity']!='error'})
            check('data_completeness','UNVERIFIED' if missing else 'PASS',f'Missing data: {missing}' if missing else 'No unknown ratings, heights or geometry')
        version,gates=native_gates(out,discover_cli(args.kicad_cli))
    status=overall_status([c['status'] for c in checks+gates])
    result={'status':status,'pass':status=='PASS','kicad_version':version,'checks':checks,'gates':gates,'hashes':manifest['files']}
    marker.write_text(json.dumps(result,indent=2)+'\n')
    if getattr(args,'format','json')=='json':print(json.dumps(result,indent=2))
    return 0 if status=='PASS' else 2

# ---------------------------------------------------------------------------
# Report scopes, human rendering and the --result-json protocol used by pcba.
# ---------------------------------------------------------------------------
MODEL_GAPS={'unsupported_model','unsupported_analysis'}  # required model or capability absent: UNVERIFIED
ELECTRICAL_RULES={'unsupported_model','unsupported_analysis','model_stamp','mna_failure','open_node','rating_unknown','voltage_derating','power_derating','voltage_target',
                  'corner_capacity','corner_allocation','corner_solve','corner_power','corner_voltage','corner_target'}
def scope_status(issues,incomplete=False):
    if any(i['severity']=='error' for i in issues):return 'FAIL'
    return 'UNVERIFIED' if incomplete or issues else 'PASS'
def report_scopes(report):
    """Electrical vs physical/DFM status of one copper-core report; every other rule is physical."""
    electrical=[i for i in report['issues'] if i['rule'] in ELECTRICAL_RULES]
    physical=[i for i in report['issues'] if i['rule'] not in ELECTRICAL_RULES]
    el=scope_status(electrical)
    # A missing device model means the analysis could not be established, not that it failed.
    if el=='FAIL' and all(i['rule'] in MODEL_GAPS or i['severity']!='error' for i in electrical):el='UNVERIFIED'
    # Trace current budgets need a DC solution; without one the physical scope is incomplete.
    return {'electrical':el,'physical':scope_status(physical,not report['physical_checked'] or not report['dc_solved'])}
def dc_status(report):
    if report['dc_solved']:return 'PASS'
    return 'UNVERIFIED' if any(i['rule'] in MODEL_GAPS for i in report['issues']) else 'FAIL'
def outcome_status(exc):
    if isinstance(exc,(Unverified,FileNotFoundError)):return 'UNVERIFIED'
    if isinstance(exc,(Failed,subprocess.CalledProcessError)):return 'FAIL'
    if isinstance(exc,CoreError):return 'ERROR'
    if isinstance(exc,(ValueError,KeyError,IndexError)):return 'INVALID_INPUT'
    return 'ERROR'
def part_fields(p):
    return {'id':str(p[1]),**{k:text_at(p,k) for k in ('kind','model','symbol','footprint','power_w','voltage_v','tolerance','height_mm','manufacturer','mpn','datasheet','provenance','catalogue')},
            'pinmap':{str(e[0]):str(e[1]) for e in (one(p,'pinmap') or [None])[1:]}}

def render_issues(issues,title='Findings'):
    errors=sum(i['severity']=='error' for i in issues)
    print(f'{title}: {errors} error(s), {len(issues)-errors} warning(s)')
    for i in issues:print(f"  {i['severity'].upper():<7} {i['rule']:<24} {i['object']}: {i['message']}")
def render_report(report,scopes=None):
    print(f"Project: {report['name']}")
    if report['dc_solved']:
        print(f"DC operating point: solved (max residual {report['mna_residual']:.3g}, KCL residual {report['kcl_residual_a']:.3g} A)")
        for n in report['nodes']:print(f"  {n['name']:<12} {n['voltage_v']:.9g} V")
        if report['calculations']:print('Resistor calculations (I=(Va-Vb)/R, P=I*I*R):')
        for c in report['calculations']:print(f"  {c['ref']:<12} V={c['voltage_v']:.6g} V  I={c['current_a']:.6g} A  P={c['power_w']:.6g} W")
        if report['tolerance_corner_count']:print(f"Resistor tolerance corners evaluated: {report['tolerance_corner_count']}")
    else:print('DC operating point: NOT solved')
    print('Device models:')
    for c in report['components']:
        model=f"{c['model']['id']} v{c['model']['version']} ({c['model']['source'].replace('_',' ')})" if c['model'] else ('none (ideal boundary)' if c['kind']=='J' else 'none (analysis blocked)')
        print(f"  {c['ref']:<12} {model}")
    render_issues(report['issues'])
    if scopes:print(f"Electrical: {scopes['electrical']}   Physical/DFM: {scopes['physical']}"+('' if report['physical_checked'] else '  (no board geometry; run project build)'))
    print(f"Coverage: {report['coverage']}")
def render_verification(v):
    print(f"Verification: {v['status']}   KiCad: {v['kicad_version'] or 'not run'}")
    for c in v['checks']:print(f"  {c['status']:<10} {c['check']:<20} {c['message']}")
    for g in v['gates']:print(f"  {g['status']:<10} {g['gate']:<20} {g.get('message') or 'exit code '+str(g.get('returncode'))}")
def render_doctor(checks):
    for c in checks:
        print(f"  {c['status']:<10} {c['check']:<20} {c['detail']}")
        if c['hint'] and c['status']!='PASS':print(f"  {'':<10} {'':<20} -> {c['hint']}")

def doctor(args):
    checks=[]
    def add(name,status,detail,kind='core',hint=''):checks.append({'check':name,'status':status,'kind':kind,'detail':detail,'hint':hint})
    v=sys.version_info
    add('python','PASS' if v>=(3,10) else 'FAIL',f'{sys.executable} ({v.major}.{v.minor}.{v.micro})',hint='Install Python 3.10 or newer')
    try:
        core=core_path(args.core);report,rc=core_run(ROOT/'examples/v2/divider.copper',core)
        add('copper_core','PASS' if rc==0 and report['dc_solved'] else 'FAIL',f'{core} (divider smoke solve {"ok" if report["dc_solved"] else "FAILED"})',hint='Rebuild copper-core')
    except (Unverified,ValueError,OSError,subprocess.SubprocessError) as exc:add('copper_core','FAIL',str(exc),hint='Build the copper-core target or set COPPER_CORE')
    out=Path(args.out)
    try:
        out.mkdir(parents=True,exist_ok=True);probe=out/f'.pcba-write-test-{os.getpid()}';probe.write_text('ok');probe.unlink()
        add('output_dir','PASS',f'{out.resolve()} is writable')
    except OSError as exc:add('output_dir','FAIL',f'{out}: {exc}',hint='Choose a writable directory with --out')
    cli=discover_cli(args.kicad_cli);hint='Install KiCad 9.x and add kicad-cli to PATH, set KICAD_CLI, or pass --kicad-cli'
    if not cli:add('kicad_cli','UNVERIFIED','kicad-cli not found','native',hint)
    else:
        try:r=subprocess.run([cli,'version'],capture_output=True,text=True,timeout=60)
        except (OSError,subprocess.SubprocessError) as exc:add('kicad_cli','UNVERIFIED',f'{cli}: {exc}','native',hint)
        else:
            found=r.stdout.strip()
            if r.returncode==0 and found.startswith('9.'):add('kicad_cli','PASS',f'{cli} (KiCad {found})','native')
            else:add('kicad_cli','UNVERIFIED',f'{cli} reports {found or "no version"}; native validation targets KiCad 9.x only','native',hint)
    for name,find,pattern,option,env in [('symbol_libraries',discover_symbol_dir,'*.kicad_sym',args.symbol_dir,'KICAD_SYMBOL_DIR'),('footprint_libraries',discover_footprint_dir,'*.pretty',args.footprint_dir,'KICAD_FOOTPRINT_DIR')]:
        try:found_dir=find(option);count=len(list(found_dir.glob(pattern)))
        except FileNotFoundError as exc:add(name,'UNVERIFIED',str(exc).splitlines()[0],'native',f'Install the KiCad 9.x libraries or set {env}')
        else:add(name,'PASS' if count else 'UNVERIFIED',f'{found_dir} ({count} libraries)','native',f'Point {env} at a directory with KiCad libraries')
    import ctypes.util
    loader=ctypes.util.find_library('vulkan-1' if os.name=='nt' else 'vulkan')
    add('vulkan_runtime','PASS' if loader else 'UNVERIFIED',loader or 'Vulkan loader not found','optional','Only the Copper Studio desktop needs Vulkan')
    studio=[p for p in (ROOT/'build-desktop/desktop/copper-studio',ROOT/'build-desktop/desktop/copper-studio.exe',ROOT/'build-desktop/desktop/Release/copper-studio.exe') if p.is_file()]
    add('desktop_app','PASS' if studio else 'UNVERIFIED',str(studio[0]) if studio else 'copper-studio not built','optional','cmake -S . -B build-desktop -DCOPPER_BUILD_DESKTOP=ON')
    core=overall_status([c['status'] for c in checks if c['kind']=='core']);native=overall_status([c['status'] for c in checks if c['kind']=='native'])
    status='FAIL' if core=='FAIL' else native
    args.outcome.update(status=status,data={'checks':checks})
    if args.format=='human':render_doctor(checks)
    elif args.format=='json':print(json.dumps(checks,indent=2))
    return 0 if status=='PASS' else 2

def library_scan(args):
    from hwc_kicad import cmd_catalog, cmd_footprints
    try:symdir=discover_symbol_dir(args.symbol_dir);fpdir=discover_footprint_dir(args.footprint_dir)
    except FileNotFoundError as exc:raise ToolUnavailable(str(exc)) from exc
    out=Path(args.out);files={'symbols':out/'kicad_catalog.json','footprints':out/'kicad_footprints.json'}
    cmd_catalog(argparse.Namespace(symbol_dir=str(symdir),out=str(files['symbols']),csv=str(out/'kicad_catalog.csv')))
    cmd_footprints(argparse.Namespace(footprint_dir=str(fpdir),out=str(files['footprints']),csv=str(out/'kicad_footprints.csv')))
    counts={'symbols':json.loads(files['symbols'].read_text(encoding='utf-8'))['symbol_count'],'footprints':json.loads(files['footprints'].read_text(encoding='utf-8'))['footprint_count']}
    status='PASS' if all(counts.values()) else 'UNVERIFIED'
    args.outcome.update(status=status,message='' if status=='PASS' else 'A library directory contained no entries',data={'symbol_dir':str(symdir),'footprint_dir':str(fpdir),'counts':counts,'files':{k:str(v.resolve()) for k,v in files.items()}})
    return 0 if status=='PASS' else 2

def dfm_check(args):
    target=Path(args.target);project=target/'resolved.copper' if target.is_dir() else target
    existing(project,'Board project (a .copper file with geometry, or a build directory)')
    report,_=core_run(project,args.core);scopes=report_scopes(report)
    findings=[i for i in report['issues'] if i['rule'] not in ELECTRICAL_RULES]
    args.outcome.update(status=scopes['physical'],data={'project':str(project.resolve()),'physical_checked':report['physical_checked'],'dc_solved':report['dc_solved'],'findings':findings,'coverage':report['coverage']})
    if args.format=='human':
        print(f'Board: {project}');render_issues(findings,'DFM findings')
        if not report['physical_checked']:print('No board geometry: physical DFM did not run. Run project build, then check the build directory.')
        elif not report['dc_solved']:print('DC solve failed: trace current/drop budgets were not evaluated.')
        print(f"Coverage: {report['coverage']}")
    elif args.format=='json':print(json.dumps(args.outcome['data'],indent=2))
    return 0 if scopes['physical']=='PASS' else 2

AC_RULE_OUTCOME={'unsupported_model':Unverified,'unsupported_analysis':Unverified,'no_excitation':InvalidInput,
                 'invalid_sweep':InvalidInput,'singular':Failed,'operating_point':Failed,'model_stamp':Failed}
def ac_sweep(args):
    """Small-signal AC sweep by copper-core on the project's real circuit; rows keep complex phasors."""
    project=Path(existing(args.project)).resolve()
    result=subprocess.run([core_path(args.core),'ac',str(project),repr(args.start),repr(args.stop),str(args.points),args.sweep],capture_output=True,text=True,timeout=600)
    message=result.stderr.strip()
    if result.returncode==2 and message.startswith('PROJECT ERROR'):raise InvalidInput(message)
    rule=message[3:].split(':',1)[0] if message.startswith('AC ') else ''
    if result.returncode not in (0,2) or (result.returncode==2 and rule not in AC_RULE_OUTCOME):raise CoreError(message or f'copper-core ac failed ({result.returncode})')
    reader=csv.reader(io.StringIO(result.stdout));header=next(reader,[])
    rows=[{name:(value if name=='status' else (float(value) if value else None)) for name,value in zip(header,row)} for row in reader]
    args.outcome['data']={'project':str(project),'sweep':args.sweep,'start_hz':args.start,'stop_hz':args.stop,'points':args.points,
                          'columns':header,'rows':rows,'csv':None}
    if args.out and result.stdout:
        Path(args.out).parent.mkdir(parents=True,exist_ok=True);Path(args.out).write_text(result.stdout);args.outcome['data']['csv']=str(Path(args.out).resolve())
    if args.format=='human' and rows:
        nets=[c[2:-len(')_mag')] for c in header if c.startswith('V(') and c.endswith(')_mag')]
        print(f"{'frequency_hz':>14}"+''.join(f'{"|V("+n+")|":>16}{"phase_deg":>11}' for n in nets))
        for row in rows:
            if row['status']!='ok':print(f"{row['frequency_hz']:>14.6g}  {row['status']}");continue
            print(f"{row['frequency_hz']:>14.6g}"+''.join(f"{row[f'V({n})_mag']:>16.6g}{row[f'V({n})_phase_deg']:>11.3f}" for n in nets))
        if args.out:print(f'CSV with node voltage and branch current phasors: {args.out}')
    elif args.format=='json':sys.stdout.write(result.stdout)
    if rule:raise AC_RULE_OUTCOME[rule](message[3:])
    return 0
def run_command(args):
    o=args.outcome;fmt=args.format
    if args.command=='new':rc=new_project(args);o['data']={'path':str(Path(args.out).resolve())};return rc
    if args.command=='check':
        report,rc=core_run(existing(args.project),args.core);scopes=report_scopes(report)
        o.update(status=dc_status(report) if args.scope=='dc' else overall_status(list(scopes.values())),data={'project':str(Path(args.project).resolve()),'scope':args.scope,'scopes':scopes,'report':report})
        if fmt=='human':render_report(report,None if args.scope=='dc' else scopes)
        else:print(json.dumps(report,indent=2))
        return rc
    if args.command=='transient':
        project=existing(args.project)
        if not (math.isfinite(args.dt) and 0<args.dt<=1000 and 1<=args.steps<=100000):raise InvalidInput('Transient requires 0 < dt <= 1000 s and 1 <= steps <= 100000')
        report,_=core_run(project,args.core)
        if any(i['rule']=='unsupported_model' for i in report['issues']):raise Unverified('An active device lacks a supported model; transient analysis is blocked')
        missing=[f"{c['ref']} ({c['model']['id']})" for c in report['components'] if c['model'] and not c['capabilities']['transient']]
        if missing:raise Unverified('These device models do not support transient analysis: '+', '.join(missing))
        data,rc=core_run(project,args.core,'transient',(args.dt,args.steps))
        rows=list(csv.reader(io.StringIO(data)));header,samples=rows[0],rows[1:]
        if rc or not samples:raise Failed(f'Transient solve failed after {len(samples)} accepted step(s); no successful waveform is claimed')
        o['data']={'project':str(Path(project).resolve()),'method':'backward_euler','initial_state':'zero stored energy','dt_s':args.dt,'steps':args.steps,'columns':header,'final':dict(zip(header,map(float,samples[-1]))),'csv':None}
        if args.out:
            Path(args.out).parent.mkdir(parents=True,exist_ok=True);Path(args.out).write_text(data);o['data']['csv']=str(Path(args.out).resolve())
            print(args.out if fmt=='json' else f'Backward Euler, dt={args.dt:g} s, {len(samples)} steps -> {args.out}')
        else:
            o['data']['rows']=[[float(x) for x in r] for r in samples];sys.stdout.write(data)
        return 0
    if args.command=='ac':return ac_sweep(args)
    if args.command=='build':
        existing(args.project);rc=export_project(args);m=json.loads((Path(args.out)/'manifest.json').read_text())
        o.update(status={STATUS_PASS:'PASS',STATUS_UNVERIFIED:'UNVERIFIED'}.get(m['status'],'FAIL'),data={'out':str(Path(args.out).resolve()),'build_status':m['status'],'unverified_rules':m['unverified_rules'],'native_verification':'NOT_RUN'})
        if fmt=='human':print('Native KiCad verification has not run yet; run project verify on this output directory.')
        return rc
    if args.command in ('verify','fabricate'):
        rc=verify(args);v=json.loads((Path(args.out).resolve()/'verification.json').read_text())
        o.update(status=v['status'],data={'verification':v})
        if fmt=='human':render_verification(v)
        if args.command=='verify':return rc
        if v['status']!='PASS':raise (Failed if v['status']=='FAIL' else Unverified)(f"Native verification is {v['status']}; no fabrication outputs generated")
        out=Path(args.out).resolve();report=json.loads((out/'report.json').read_text())
        if report['errors']:raise Failed('Resolve all internal errors before fabrication export')
        if report['warnings']:raise Unverified('Resolve all internal findings, including unknown ratings, before fabrication export')
        cli=discover_cli(args.kicad_cli);cam=out/'fabrication';cam.mkdir(exist_ok=True)
        for command in [['pcb','export','gerbers','-o',str(cam)+os.sep,str(out/'design.kicad_pcb')],['pcb','export','drill','-o',str(cam)+os.sep,str(out/'design.kicad_pcb')]]:
            subprocess.run([cli,*command],check=True,timeout=180)
        o['data']['cam']={'dir':str(cam),'files':sorted(p.name for p in cam.iterdir())}
        print('CAM files exported to '+str(cam)+'. Engineering review of models, process and assembly remains required.');return 0
    if args.command=='classify':return subprocess.run([core_path(args.core),'classify',args.text],timeout=30).returncode
    if args.command in ('part-add','part-remove'):
        target=Path(args.file)
        if target.resolve()==BUNDLED_CATALOGUE.resolve():raise InvalidInput('profiles/default.parts is the bundled catalogue and is read-only; use your own catalogue file')
        tree=load(target) if target.exists() else sx('catalogue')
        if head(tree)!='catalogue':raise InvalidInput(f'{target} is not a (catalogue ...) file')
        if args.command=='part-remove':
            kept=[x for x in tree if not (isinstance(x,list) and head(x)=='part' and str(x[1])==args.id)]
            if len(kept)==len(tree):raise InvalidInput(f'{args.id} is not in {target}')
            write_tree(target,kept);o['data']={'file':str(target.resolve()),'id':args.id,'removed':True};print('Removed '+args.id+' from '+args.file);return 0
        if any(str(p[1])==args.id for p in direct(tree,'part')):raise InvalidInput(f'duplicate part ID {args.id}')
        if not re.fullmatch(r'[A-Za-z0-9_.-]+',args.id):raise InvalidInput('Invalid part ID')
        for name,value in [('power',args.power),('voltage',args.voltage),('height',args.height)]:
            if value is not None and not (math.isfinite(value) and value>0):raise InvalidInput(f'--{name} must be a positive rating; omit it when unknown')
        if args.tolerance is not None and not (math.isfinite(args.tolerance) and 0<=args.tolerance<1):raise InvalidInput('--tolerance must be in [0, 1)')
        if args.kind not in ('V','I'):safe_lib_id(args.symbol);safe_lib_id(args.footprint)
        part=sx('part',A(args.id),sx('kind',A(args.kind)))
        if args.symbol:part.append(sx('symbol',Q(args.symbol)))
        if args.footprint:part.append(sx('footprint',Q(args.footprint)))
        if args.model:part.append(sx('model',A(args.model)))
        if args.pinmap:
            entries=[e.split('=',1) for e in args.pinmap.split(',')]
            if any(len(e)!=2 or not all(e) for e in entries):raise InvalidInput('--pinmap must be terminal=pin[,terminal=pin...]')
            part.append(sx('pinmap',*[sx(t.strip(),A(pin.strip())) for t,pin in entries]))
        for key,value in [('power_w',args.power),('voltage_v',args.voltage),('tolerance',args.tolerance),('height_mm',args.height)]:
            if value is not None:part.append(sx(key,number(value)))
        for key in ('manufacturer','mpn','datasheet','provenance'):
            if getattr(args,key):part.append(sx(key,Q(getattr(args,key))))
        tree.append(part)
        # Validate the whole resulting catalogue with copper-core before touching the target file.
        target.parent.mkdir(parents=True,exist_ok=True);probe=target.with_name(target.name+'.check')
        try:write_tree(probe,tree);validate_catalogue(probe,args.core,label=target)
        finally:probe.unlink(missing_ok=True)
        write_tree(target,tree);o['data']={'file':str(target.resolve()),'id':args.id};print('Added '+args.id+' to '+args.file);return 0
    if args.command=='catalogue':
        if args.file:existing(args.file,'Catalogue')
        matches=[p for p in catalogue(args.file) if args.search.lower() in dump_sexpr(p).lower()]
        o['data']={'catalogue':str(Path(args.file).resolve()) if args.file else str(ROOT/'profiles/default.parts'),'query':args.search,'parts':[part_fields(p) for p in matches]}
        for p in matches:
            if fmt=='human':f=part_fields(p);print(f"{f['id']:<16} kind={f['kind']:<2} {f['symbol'] or '-':<34} {f['footprint'] or '-'}")
            else:print(dump_sexpr(p))
        if fmt=='human':print(f'{len(matches)} part(s) match')
        return 0
    if args.command=='doctor':return doctor(args)
    if args.command=='library-scan':return library_scan(args)
    if args.command=='dfm':return dfm_check(args)

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--format',choices=['json','human','quiet'],default='json',help='stdout style: json (legacy default), human, or quiet')
    parser.add_argument('--result-json',metavar='FILE',help='write {"status","message","data"} here on every handled exit path (pcba protocol)')
    sub=parser.add_subparsers(dest='command',required=True)
    p=sub.add_parser('new');p.add_argument('template',choices=['divider','rc','rl','diode']);p.add_argument('--out',required=True);p.add_argument('--catalogue');p.add_argument('--rules');p.add_argument('--vin',type=float,default=5);p.add_argument('--r',type=float,default=10000);p.add_argument('--r2',type=float,default=20000);p.add_argument('--c',type=float,default=1e-6);p.add_argument('--l',type=float,default=.01);p.add_argument('--target',type=float);p.add_argument('--target-tolerance',type=float,default=.05);p.add_argument('--force',action='store_true');p.add_argument('--core')
    for cmd in ['check','transient']:
        p=sub.add_parser(cmd);p.add_argument('project');p.add_argument('--core')
        if cmd=='check':p.add_argument('--scope',choices=['all','dc'],default='all',help='dc: status of the DC solve only')
        if cmd=='transient':p.add_argument('--dt',type=float,default=1e-4);p.add_argument('--steps',type=int,default=1000);p.add_argument('--out',help='CSV path; omitted: CSV on stdout')
    p=sub.add_parser('build');p.add_argument('project');p.add_argument('--out',default='out/v2');p.add_argument('--core');p.add_argument('--symbol-dir');p.add_argument('--footprint-dir');p.add_argument('--no-route',action='store_true');p.add_argument('--draft',action='store_true',help='Allow electrical failures to produce an explicitly failed draft, never a verified package')
    for cmd in ['verify','fabricate']:
        p=sub.add_parser(cmd);p.add_argument('--out',default='out/v2');p.add_argument('--kicad-cli');p.add_argument('--core');p.add_argument('--project',help='Source .copper to check for freshness (default: path recorded at build)')
    p=sub.add_parser('classify');p.add_argument('text');p.add_argument('--core')
    p=sub.add_parser('catalogue');p.add_argument('--file');p.add_argument('--search',default='')
    p=sub.add_parser('part-add',help='Unknown ratings are omitted, never written as 0');p.add_argument('--file',required=True);p.add_argument('--id',required=True);p.add_argument('--kind',choices=list('RCLDVIJU'),required=True);p.add_argument('--symbol',default='');p.add_argument('--footprint',default='');p.add_argument('--power',type=float);p.add_argument('--voltage',type=float);p.add_argument('--tolerance',type=float);p.add_argument('--height',type=float);p.add_argument('--manufacturer',default='');p.add_argument('--mpn',default='');p.add_argument('--datasheet',default='')
    p.add_argument('--model',help='device model id (copper-core models); default: the kind default');p.add_argument('--pinmap',help='terminal=symbol_pin,... e.g. cathode=1,anode=2');p.add_argument('--provenance',default='',help='where the ratings come from');p.add_argument('--core')
    p=sub.add_parser('part-remove');p.add_argument('--file',required=True);p.add_argument('--id',required=True)
    p=sub.add_parser('ac',help='small-signal AC sweep; sources need (ac MAGNITUDE PHASE_DEG)');p.add_argument('project');p.add_argument('--start',type=float,required=True);p.add_argument('--stop',type=float,required=True);p.add_argument('--points',type=int,required=True);p.add_argument('--sweep',choices=['log','lin'],default='log');p.add_argument('--out',help='CSV path');p.add_argument('--core')
    p=sub.add_parser('dfm');p.add_argument('target',help='.copper file with geometry, or a build directory');p.add_argument('--core')
    p=sub.add_parser('doctor');p.add_argument('--out',default='out');p.add_argument('--core');p.add_argument('--kicad-cli');p.add_argument('--symbol-dir');p.add_argument('--footprint-dir')
    p=sub.add_parser('library-scan');p.add_argument('--out',default='out/library');p.add_argument('--symbol-dir');p.add_argument('--footprint-dir')

    args=parser.parse_args(argv);args.outcome={'status':None,'message':'','data':None};rc=2
    try:
        with contextlib.redirect_stdout(io.StringIO()) if args.format=='quiet' else contextlib.nullcontext():rc=run_command(args)
    except (Unverified,ValueError,KeyError,IndexError,OSError,subprocess.SubprocessError) as exc:
        print(f'COPPER ERROR: {exc}',file=sys.stderr);rc=2;args.outcome.update(status=outcome_status(exc),message=str(exc))
    if args.outcome['status'] is None:args.outcome['status']='PASS' if rc==0 else 'FAIL'
    if args.result_json:
        # Status is the first key; pcba reads it without a full JSON parser.
        path=Path(args.result_json);tmp=path.with_name(path.name+'.tmp')
        tmp.write_text(json.dumps({'status':args.outcome['status'],'message':args.outcome['message'],'data':args.outcome['data']})+'\n',encoding='utf-8');tmp.replace(path)
    return rc
if __name__=='__main__':raise SystemExit(main())
