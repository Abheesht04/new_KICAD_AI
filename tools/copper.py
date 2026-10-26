#!/usr/bin/env python3
"""Copper V2: deterministic project orchestration and KiCad 9 adapters.
Engineering authority lives in copper-core. This file resolves local libraries,
places/routes candidate geometry, emits native KiCad files, and runs native gates.
No shell execution, network requests, or LLM engineering decisions.
"""
from __future__ import annotations
import argparse, contextlib, copy, csv, hashlib, io, json, math, os, re, shutil, subprocess, sys, uuid
from pathlib import Path
from hwc_kicad import (Atom as A, Quoted as Q, parse_sexpr, dump_sexpr, direct, one,
                      head, text_at, discover_symbol_dir, discover_footprint_dir, discover_cli)
ROOT=Path(__file__).resolve().parents[1]
# One module identity when run as a script, so spec_ir's `import copper` shares these exception classes.
sys.modules.setdefault('copper',sys.modules[__name__])
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
_LIBRARY_TREES={}
def library_tree(path):
    """A KiCad library file parsed once per process (keyed by path, size and mtime), shared and read-only:
    callers that modify what they get must deep-copy it. Symbol libraries are megabytes; re-parsing them per
    symbol dominated design runs."""
    path=Path(path);st=path.stat();key=(str(path.resolve()),st.st_size,st.st_mtime_ns)
    if key not in _LIBRARY_TREES:_LIBRARY_TREES[key]=load(path)
    return _LIBRARY_TREES[key]
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
        result.append(dict(ref=str(c[1]),part=part,kind=text_at(part,'kind'),value=float(text_at(c,'value',default='0')),symbol=text_at(part,'symbol'),footprint=text_at(part,'footprint'),pins={str(x[0]):str(x[1]) for x in one(c,'pins')[1:]},
                           nc=[str(x) for x in (one(c,'nc') or [None])[1:]],x=float(at[1]),y=float(at[2]),rotation=float(at[3])))
    return result

def safe_lib_id(value):
    if not re.fullmatch(r'[A-Za-z0-9_+.-]+:[A-Za-z0-9_+().,~ -]+',value):raise ValueError(f'Invalid library ID: {value}')
    return value.split(':',1)
def resolve_symbol(libdir,libid,cache,seen=()):
    if libid in cache:return copy.deepcopy(cache[libid])
    if libid in seen:raise ValueError('Cyclic symbol inheritance')
    lib,name=safe_lib_id(libid);tree=library_tree(libdir/f'{lib}.kicad_sym')
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
    if any(n.startswith('[') for n in numbers):raise Unverified('Stacked pin numbers such as [1,15] are KiCad 10 library syntax; native support targets KiCad 9.x libraries')
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
    physical=[c for c in components if c['kind'] not in ('V','I')];power_inputs=set()
    for index,c in enumerate(physical):
        definition=resolve_symbol(symbol_dir,c['symbol'],cache);pins=symbol_pins(definition)
        if set(c['pins'])|set(c['nc'])!={text_at(p,'number') for p in pins}:raise ValueError(f"{c['ref']}: project pins and (nc ...) must be exactly the symbol pins")
        binding=(bindings or {}).get(c['ref'])
        if binding and binding['model'] and specs:check_terminal_pin_names(c['ref'],binding,specs[binding['model']['id']],pins)
        if not any(s[1]==c['symbol'] for s in lib[1:]):lib.append(definition)
        x,y=50.8+(index%4)*50.8,50.8+(index//4)*50.8
        instance=sx('symbol',sx('lib_id',Q(c['symbol'])),sx('at',number(x),number(y),A('0')),sx('unit',A('1')),sx('exclude_from_sim',A('no')),sx('in_bom',A('yes')),sx('on_board',A('yes')),sx('dnp',A('no')),sx('uuid',Q(uid(stem+'/'+c['ref']))))
        for key,value,offset,hide in [('Reference',c['ref'],-5,False),('Value',f"{c['value']:.9g}",5,False),('Footprint',c['footprint'],0,True)]:
            instance.append(sx('property',Q(key),Q(value),sx('at',number(x+7),number(y+offset),A('0')),effects(hide)))
        instance.append(sx('instances',sx('project',Q(stem),sx('path',Q('/'+rootid),sx('reference',Q(c['ref'])),sx('unit',A('1'))))))
        for pin in pins:
            no=text_at(pin,'number');at=one(pin,'at');px,py=x+float(at[1]),y-float(at[2])
            instance.append(sx('pin',Q(no),sx('uuid',Q(uid(stem+'/'+c['ref']+'/pin/'+no)))))
            if no in c['nc']:
                # A deliberate no-connect; a pin the symbol already types no_connect needs no flag.
                if str(pin[1])!='no_connect':sch.append(sx('no_connect',sx('at',number(px),number(py)),sx('uuid',Q(uid(stem+'/'+c['ref']+'/nc/'+no)))))
                continue
            net=c['pins'][no];power_inputs.update([net] if str(pin[1])=='power_in' else [])
            # Labels sit exactly on actual pin endpoints; symbol coordinates have inverted Y.
            sch.append(sx('label',Q(net),sx('at',number(px),number(py),A('0')),effects(),sx('uuid',Q(uid(stem+'/'+c['ref']+'/label/'+no)))))
        sch.append(instance)
    # A power input pin must be driven in ERC. The circuit's own source (a V component, which has no symbol)
    # drives these nets, so a PWR_FLAG marks exactly those nets; an undriven power input stays an ERC error.
    sourced={net for c in components if c['kind']=='V' for net in c['pins'].values()}
    for index,net in enumerate(sorted(power_inputs&sourced)):
        flag=resolve_symbol(symbol_dir,'power:PWR_FLAG',cache);ref=f'#FLG{index+1:02d}'
        if not any(s[1]=='power:PWR_FLAG' for s in lib[1:]):lib.append(flag)
        x,y=25.4+index*12.7,25.4;pin=symbol_pins(flag)[0];at=one(pin,'at')
        instance=sx('symbol',sx('lib_id',Q('power:PWR_FLAG')),sx('at',number(x),number(y),A('0')),sx('unit',A('1')),sx('exclude_from_sim',A('no')),sx('in_bom',A('no')),sx('on_board',A('no')),sx('dnp',A('no')),sx('uuid',Q(uid(stem+'/'+ref))))
        for key,value,hide in [('Reference',ref,True),('Value','PWR_FLAG',False),('Footprint','',True)]:instance.append(sx('property',Q(key),Q(value),sx('at',number(x),number(y-3),A('0')),effects(hide)))
        instance.append(sx('pin',Q(text_at(pin,'number')),sx('uuid',Q(uid(stem+'/'+ref+'/pin')))))
        instance.append(sx('instances',sx('project',Q(stem),sx('path',Q('/'+rootid),sx('reference',Q(ref)),sx('unit',A('1'))))))
        sch.append(sx('label',Q(net),sx('at',number(x+float(at[1])),number(y-float(at[2])),A('0')),effects(),sx('uuid',Q(uid(stem+'/'+ref+'/label')))))
        sch.append(instance)
    sch.append(sx('sheet_instances',sx('path',Q('/'),sx('page',Q('1')))))
    sch.append(sx('embedded_fonts',A('no')))
    return sch

def rotate(x,y,angle):
    a=math.radians(angle);return (x*math.cos(a)+y*math.sin(a),-x*math.sin(a)+y*math.cos(a))
KEEPOUT_FLAGS={'tracks':1,'vias':2,'pads':4,'footprints':8}
def footprint_keepout(c,zone):
    """A footprint rule area (keepout) as an axis-aligned board rectangle, taken from the footprint itself.
    Copper zones, non-rectangular areas and areas that keep nothing out are outside supported geometry."""
    rules=one(zone,'keepout')
    if not rules:raise Unverified(f"{c['ref']}: footprint copper zones are outside V2 geometry coverage")
    flags=sum(bit for key,bit in KEEPOUT_FLAGS.items() if text_at(rules,key)=='not_allowed')
    names=[str(x) for x in (one(zone,'layers') or one(zone,'layer') or [None])[1:]]
    layers=(1 if {'F.Cu','*.Cu','F&B.Cu'}&set(names) else 0)|(2 if {'B.Cu','*.Cu','F&B.Cu'}&set(names) else 0)
    if not flags or not layers:raise Unverified(f"{c['ref']}: footprint keepout keeps nothing out on F.Cu/B.Cu; refusing to guess its meaning")
    pts=[rotate(float(xy[1]),float(xy[2]),c['rotation']) for xy in direct(one(one(zone,'polygon') or [None],'pts') or [None],'xy')]
    xs=sorted({round(c['x']+x,9) for x,_ in pts});ys=sorted({round(c['y']+y,9) for _,y in pts})
    if len(pts)!=4 or len(xs)!=2 or len(ys)!=2:raise Unverified(f"{c['ref']}: only rectangular footprint keepouts are supported")
    return dict(x1=xs[0],y1=ys[0],x2=xs[1],y2=ys[1],layers=layers,flags=flags)
def footprint_geometry(c,fpdir,netids,stem,ncnets=None):
    lib,name=safe_lib_id(c['footprint']);fp=copy.deepcopy(library_tree(fpdir/f'{lib}.pretty'/f'{name}.kicad_mod'))  # modified below
    if head(fp)!='footprint':raise ValueError('Expected KiCad footprint')
    if one(fp,'embedded_files'):raise Unverified('Footprint embedded files are outside V2 geometry coverage')
    keepouts=[footprint_keepout(c,z) for z in direct(fp,'zone')]
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
        if no not in c['pins'] and no not in c['nc']:raise ValueError(f"{c['ref']}: physical pad {no} missing from electrical map")
        if drill and (len(drill)!=2 or not re.fullmatch(r'[0-9.]+',str(drill[1]))):raise Unverified('Slotted/offset drills require native geometry support')
        if any(str(x) not in ('F.Cu','F.Paste','F.Mask','*.Cu','*.Mask') for x in layers[1:]):raise Unverified('Only top-side SMD or through-hole pads are supported')
        dx,dy=rotate(float(at[1]),float(at[2]),c['rotation']);x,y=c['x']+dx,c['y']+dy
        angle=c['rotation']+(float(at[3]) if len(at)>3 else 0)
        if angle%90:raise Unverified('Pad rotation must be a multiple of 90 degrees')
        w,h=float(size[1]),float(size[2]);w,h=(h,w) if round(angle/90)%2 else (w,h)
        if w<=0 or h<=0:raise ValueError('Pad dimensions must be positive')
        circular=shape=='circle' and abs(w-h)<1e-9
        # C retains the exact axis-aligned core and rounded-corner radius.
        # An (nc ...) pad has no net ('~'): still copper for clearance, never routed.
        net=c['pins'].get(no,'~');mapped.add(no);pad[:]=[v for v in pad if not isinstance(v,list) or head(v) not in ('net','uuid')]
        if net!='~':pad.append(sx('net',A(str(netids[net])),Q('/'+net)))
        elif ncnets and (c['ref'],no) in ncnets:pad.append(sx('net',A(str(ncnets[c['ref'],no][0])),Q(ncnets[c['ref'],no][1])))
        pad.append(sx('uuid',Q(uid(stem+'/'+c['ref']+'/pad/'+no+'/'+str(len(pads))))))
        pads.append(dict(ref=c['ref'],pin=no,net=net,x=x,y=y,w=w,h=h,through=kind=='thru_hole'))
        # diameter is conservative for legacy edge/annular reporting, exact adapter uses width/height.
        diameter=min(w,h)
        corner=min(w,h)/2 if shape=='oval' else (float(text_at(pad,'roundrect_rratio',default='0'))*min(w,h) if shape=='roundrect' else 0)
        if corner<0 or corner>min(w,h)/2:raise ValueError('Invalid rounded pad radius')
        geo.append(sx('pad',A(c['ref']),A(no),A(net),*map(number,[x,y,diameter,float(drill[1]) if drill else 0,0,w,h,0 if circular else 1,corner])))
    if mapped!=set(c['pins'])|set(c['nc']):raise ValueError(f"{c['ref']}: symbol pin/footprint pad mismatch")
    # Use actual courtyard extents; a footprint lacking a courtyard is not silently invented.
    pts=[]
    for obj in fp:
        if isinstance(obj,list) and text_at(obj,'layer')=='F.CrtYd':
            for key in ('start','end','center'):
                at=one(obj,key)
                if at:
                    dx,dy=rotate(float(at[1]),float(at[2]),c['rotation']);pts.append((c['x']+dx,c['y']+dy))
            if head(obj)=='fp_poly':
                # A polygon courtyard is enclosed by its bounding box: a larger, conservative envelope.
                for xy in direct(one(obj,'pts') or [None],'xy'):
                    dx,dy=rotate(float(xy[1]),float(xy[2]),c['rotation']);pts.append((c['x']+dx,c['y']+dy))
            elif head(obj) not in ('fp_rect','fp_line'):raise Unverified('Non-rectilinear courtyard requires explicit geometry support')
    if not pts:raise ValueError(f"{c['ref']}: footprint has no supported courtyard")
    lo=[min(p[i] for p in pts) for i in range(2)];hi=[max(p[i] for p in pts) for i in range(2)]
    geo.append(sx('body',A(c['ref']),*map(number,[(lo[0]+hi[0])/2,(lo[1]+hi[1])/2,hi[0]-lo[0],hi[1]-lo[1],c.get('height',0)])))
    # Regenerate all nested UUIDs deterministically; avoid duplicate library UUIDs.
    def uuids(n,path=''):
        if isinstance(n,list):
            for i,v in enumerate(n):
                if isinstance(v,list) and head(v)=='uuid':v[1]=Q(uid(stem+'/'+c['ref']+'/nested/'+path+str(i)))
                else:uuids(v,path+str(i)+'/')
    uuids(fp)
    for k in keepouts:geo.append(sx('keepout',A(c['ref']),*map(number,[k['x1'],k['y1'],k['x2'],k['y2'],k['layers'],k['flags']])))
    return fp,pads,geo,keepouts

def route(pads,width,height,trace_width,clearance,edge,via_diameter=.6,keepouts=()):
    """The build's router (tools/layout.py): deterministic 2-layer Manhattan A* with rip-up/reroute.
    Conservative obstacle envelopes; C exact geometry and KiCad DRC judge the result. A connection that
    cannot be routed fails the build: no partial route is claimed."""
    import layout
    traces,vias,stats=layout.route_with_ripup(pads,width,height,trace_width,clearance,edge,via_diameter,keepouts)
    if stats['unrouted']:
        u=stats['unrouted'][0]
        if u['reason']=='pad_escape':raise Failed(f"Cannot escape pad on net {u['net']}; edit placement or constraints")
        raise Failed(f"Router could not connect {u['net']} ({u['connection']}, {u['reason']}); no route is claimed")
    return traces,vias
def core_check(tree,core=None):
    """copper-core analysis of an in-memory project (written to a private temporary file)."""
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        path=Path(d)/'candidate.copper';write_tree(path,tree);report,_=core_run(path,core)
    return report

def rule(tree,key,default):return float(text_at(one(tree,'rules'),key,default=str(default)))
def auto_place(tree,fpdir,core):
    """Placement for a project whose physical parts have none (e.g. lowered from an EngineeringSpec): the same
    deterministic staged search as `place`, re-checked exactly. A partly placed project is refused; mixing given
    and generated positions would be a silent guess. Returns (tree, placement record or None)."""
    import layout
    physical=[str(c[1]) for c in direct(one(tree,'components'),'component')
              if text_at(next(p for p in direct(one(tree,'parts'),'part') if str(p[1])==text_at(c,'part')),'kind') not in ('V','I')]
    unplaced=[r for r in physical if not one(next(c for c in direct(one(tree,'components'),'component') if str(c[1])==r),'at')]
    if not unplaced:return tree,None
    if len(unplaced)!=len(physical):
        raise InvalidInput(f"{', '.join(unplaced)} have no (at ...) placement while other parts do: place all of them, or none (pcba place)")
    best,candidates,problem=layout.optimize(tree,fpdir,core,(1,),300)
    if not best or not best['feasible']:
        c=layout.brief(best) if best else None
        raise Failed('no feasible placement was found for this unplaced project'+(f": best candidate has {c['hard_violations']} hard violation(s) {c['hard_by_rule']} and {c['unrouted']} unrouted connection(s)" if c else ': '+'; '.join(problem['placement_failures'])))
    record={'source':'generated: layout.optimize (seed 1, 300 iterations), exact copper-core re-check','candidate':best['label'],
            'trace_width_mm':best['width_rounds'][-1]['trace_width_mm'],'positions':best['placement']}
    return layout.placed_project(best,problem),record
def export_project(args):
    project=Path(args.project).resolve();tree=load(project);out=Path(args.out).resolve();out.mkdir(parents=True,exist_ok=True)
    for marker in ('manifest.json','verification.json'):
        (out/marker).unlink(missing_ok=True)
    report,rc=core_run(project,args.core)
    if rc and not getattr(args,'draft',False):raise Failed('Electrical analysis failed; run check to inspect the findings before layout.')
    try:tree,placement=auto_place(tree,discover_footprint_dir(args.footprint_dir),args.core)
    except FileNotFoundError as exc:raise ToolUnavailable(str(exc).splitlines()[0]) from exc
    if placement:print(f"No part had a placement: generated one ({placement['candidate']}; deterministic, re-checked by copper-core).")
    components=component_data(tree);nets=list(map(str,one(tree,'nets')[1:]));netids={n:i+1 for i,n in enumerate(nets)}
    symdir=discover_symbol_dir(args.symbol_dir);fpdir=discover_footprint_dir(args.footprint_dir)
    bindings={b['ref']:b for b in report['components']}
    for c in components:c['height']=resolved_rating(bindings[c['ref']],'height_mm') or 0  # 0 = not stated in geometry
    stem='design';sch=schematic(tree,components,symdir,stem,bindings,model_specs(args.core))
    dims=list(map(float,(one(tree,'board') or [None,40,30,1.6,.035,2])[1:]));w,h,thick,copper,layers=dims
    board=sx('kicad_pcb',sx('version',A('20241229')),sx('generator',Q('copper_v2')),sx('general',sx('thickness',number(thick))),sx('paper',Q('A4')),sx('layers',*[sx(str(i),Q(name),A(kind)) for i,name,kind in [(0,'F.Cu','signal'),(2,'B.Cu','signal'),(3,'B.Adhes','user'),(5,'F.SilkS','user'),(7,'B.SilkS','user'),(9,'F.Mask','user'),(11,'B.Mask','user'),(13,'Dwgs.User','user'),(25,'Edge.Cuts','user'),(31,'F.CrtYd','user'),(33,'B.CrtYd','user'),(35,'F.Fab','user'),(37,'B.Fab','user'),(1,'F.Adhes','user'),(19,'F.Paste','user'),(21,'B.Paste','user')]]),sx('setup',sx('pad_to_mask_clearance',number(rule(tree,'mask_expansion_mm',.05)))),sx('net',A('0'),Q('')))
    board.extend(sx('net',A(str(i)),Q('/'+n)) for n,i in netids.items())
    # KiCad gives every unconnected schematic pin its own net, unconnected-(REF-PIN_NAME-PadN); the board uses
    # the same names so schematic parity holds. In the copper IR these pads stay net-less ('~').
    ncnets={}
    for c in components:
        if not c['nc']:continue
        names={text_at(p,'number'):text_at(p,'name') for p in symbol_pins(resolve_symbol(symdir,c['symbol'],{}))}
        for pin in c['nc']:
            name=names.get(pin,'');ncnets[c['ref'],pin]=(len(netids)+len(ncnets)+1,f"unconnected-({c['ref']}-{name}-Pad{pin})" if name not in ('','~') else f"unconnected-({c['ref']}-Pad{pin})")
    board.extend(sx('net',A(str(i)),Q(n)) for i,n in ncnets.values())
    board.append(sx('gr_rect',sx('start',A('0'),A('0')),sx('end',number(w),number(h)),sx('stroke',sx('width',A('.05')),sx('type',A('default'))),sx('fill',A('none')),sx('layer',Q('Edge.Cuts')),sx('uuid',Q(uid('outline')))))
    pads=[];geometry=sx('geometry');keepouts=[]
    for c in components:
        if c['kind'] in ('V','I'):continue
        fp,pp,gg,kk=footprint_geometry(c,fpdir,netids,stem,ncnets);board.append(fp);pads.extend(pp);geometry.extend(gg);keepouts.extend(kk)
    traces=[];vias=[]
    if not args.no_route:traces,vias=route(pads,w,h,rule(tree,'trace_width_mm',.2),rule(tree,'clearance_mm',.2),rule(tree,'edge_mm',.3),rule(tree,'via_diameter_mm',.6),keepouts)
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
    for library in sorted({safe_lib_id(str(sym[1]))[0] for sym in one(sch,'lib_symbols')[1:]}):
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
    manifest={'schema':2,'placement':placement or {'source':'project (at ...) placements'},'source_sha256':sha256_file(project),'source_path':Path(source_path).as_posix(),'status':STATUS_DRAFT if rc else (STATUS_UNVERIFIED if unverified else STATUS_PASS),'unverified_rules':unverified,'files':{name:sha256_file(out/name) for name in files},'symbols':str(symdir),'footprints':str(fpdir)}
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
def native_status(label,returncode,report):
    """Classify one kicad-cli run from its own report, not from stderr or the exit code alone: KiCad's
    --exit-code-violations is non-zero for warnings too. Errors, unconnected items and schematic-parity issues
    are FAIL; warnings alone are UNVERIFIED (listed, never hidden); a missing or unreadable report is
    UNVERIFIED (the check did not complete), whatever stderr said."""
    if label=='netlist':
        ok=returncode==0 and report.is_file() and report.stat().st_size>0
        return {'status':'PASS' if ok else 'UNVERIFIED','message':'' if ok else f'netlist export exited {returncode} without a netlist'}
    try:data=json.loads(report.read_text(encoding='utf-8'))
    except (OSError,ValueError):return {'status':'UNVERIFIED','message':f'kicad-cli exited {returncode} and wrote no readable {report.name}; the check did not complete'}
    violations=[v for sheet in data.get('sheets',[]) for v in sheet.get('violations',[])] if label=='erc' else list(data.get('violations',[]))
    errors=[v for v in violations if v.get('severity')=='error'];warnings=[v for v in violations if v.get('severity')!='error']
    unconnected=data.get('unconnected_items',[]) if label=='drc' else [];parity=data.get('schematic_parity',[]) if label=='drc' else []
    counts={'errors':len(errors),'warnings':len(warnings),'unconnected':len(unconnected),'parity':len(parity),
            'types':sorted({v.get('type','?') for v in violations+parity})}
    if errors or unconnected or parity:status='FAIL'
    elif warnings:status='UNVERIFIED'
    elif returncode==0:status='PASS'
    else:return {'status':'UNVERIFIED','counts':counts,'message':f'kicad-cli exited {returncode} but its report lists no violation'}
    detail='; '.join(f"{v.get('severity')}:{v.get('type')} {v.get('description','')}" for v in (errors+warnings)[:5])
    return {'status':status,'counts':counts,'message':detail or ('no violations' if status=='PASS' else '')}
def board_content(out):
    """The built board must hold what the electrical IR says: every physical part's footprint and every connected
    pin's pad on its net. An empty or partial board is never a product result, whatever DRC says about it."""
    data=[c for c in component_data(load(out/'resolved.copper')) if c['kind'] not in ('V','I')]
    footprints={}
    for fp in direct(load(out/'design.kicad_pcb'),'footprint'):
        ref=next((str(p[2]) for p in direct(fp,'property') if str(p[1])=='Reference'),None)
        footprints[ref]={str(p[1]):(str(one(p,'net')[2]).removeprefix('/') if one(p,'net') else '') for p in direct(fp,'pad')}
    if not data:return {'check':'board_content','status':'UNVERIFIED','message':'the design has no physical parts: a native DRC of an empty board proves nothing'}
    missing=[c['ref'] for c in data if c['ref'] not in footprints]
    wrong=[f"{c['ref']}.{pin}" for c in data if c['ref'] in footprints for pin,net in c['pins'].items() if footprints[c['ref']].get(pin)!=net]
    pads=sum(len(v) for v in footprints.values());nets=len({n for v in footprints.values() for n in v.values() if n and not n.startswith('unconnected-')})
    ok=not missing and not wrong
    return {'check':'board_content','status':'PASS' if ok else 'FAIL','counts':{'footprints':len(footprints),'pads':pads,'nets':nets},
            'message':f'{len(footprints)} footprints, {pads} pads, {nets} named nets, as the electrical IR requires' if ok else f'missing footprints {missing}; pads not on their IR net {wrong[:10]}'}
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
        report=out/('netlist.xml' if label=='netlist' else f'{label}.json')
        report.unlink(missing_ok=True)  # a stale report from an earlier run must never be read as this run's result
        try:r=subprocess.run([cli,*cmd],capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=180)
        except (OSError,subprocess.SubprocessError) as exc:
            gates.append({'gate':label,'status':'UNVERIFIED','returncode':None,'stdout':'','stderr':'','message':f'kicad-cli could not run: {exc}'});continue
        gate={'gate':label,'returncode':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
        gate.update(native_status(label,r.returncode,report));gates.append(gate)
    # Compare all electrical physical-pin mappings against KiCad's own extracted netlist.
    if gates[-1]['status']=='PASS':
        import xml.etree.ElementTree as ET
        root=ET.parse(out/'netlist.xml').getroot();actual={}
        for net in root.findall('./nets/net'):
            for node in net.findall('node'):actual[(node.attrib['ref'],node.attrib['pin'])]=net.attrib['name'].removeprefix('/')
        data=[c for c in component_data(load(out/'resolved.copper')) if c['kind'] not in ('V','I')]
        expected={(c['ref'],pin):net for c in data for pin,net in c['pins'].items()}
        # Power flags (#FLG) are not parts; KiCad names each unconnected (nc ...) pin's own net 'unconnected-...'.
        nc={(c['ref'],pin) for c in data for pin in c['nc']}
        actual={k:v for k,v in actual.items() if not k[0].startswith('#') and not (k in nc and v.startswith('unconnected-'))}
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
        content=board_content(out);checks.append(content)
        if content['status']=='PASS':version,gates=native_gates(out,discover_cli(args.kicad_cli))
        else:gates=[{'gate':'native','status':'UNVERIFIED','message':'native KiCad checks were not run: the board does not hold the designed content (board_content)'}]
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
                  'corner_capacity','corner_allocation','corner_solve','corner_power','corner_voltage','corner_target','boundary_net',
                  # MCU behavioral boundary (src/v2/project.c mcu_checks)
                  'mcu_supply_current','mcu_supply_unknown','mcu_supply_voltage','mcu_output_ideal','mcu_pin_current','mcu_strap_pin',
                  'mcu_pull_unknown','mcu_input_undefined','mcu_reset','mcu_programming'}
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
        for n in report['nodes']:print(f"  {n['name']:<12} "+(f"{n['voltage_v']:.9g} V" if n['voltage_v'] is not None else 'undefined (boundary pins only)'))
        if report['calculations']:print('Resistor calculations (I=(Va-Vb)/R, P=I*I*R):')
        for c in report['calculations']:print(f"  {c['ref']:<12} V={c['voltage_v']:.6g} V  I={c['current_a']:.6g} A  P={c['power_w']:.6g} W")
        if report['tolerance_corner_count']:print(f"Resistor tolerance corners evaluated: {report['tolerance_corner_count']}")
    else:print('DC operating point: NOT solved')
    print('Device models:')
    for c in report['components']:
        model=f"{c['model']['id']} v{c['model']['version']} ({c['model']['source'].replace('_',' ')})" if c['model'] else ('none (ideal boundary)' if c['kind']=='J' else 'MCU behavioral boundary (DC constraints; firmware/silicon not simulated)' if c.get('mcu') else 'none (analysis blocked)')
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

TRANSIENT_RULE_OUTCOME={'unsupported_model':Unverified,'unsupported_analysis':Unverified,'invalid_argument':InvalidInput,
                        'solve_failed':Failed,'model_stamp':Failed,'state_commit':Failed,'allocation_failure':CoreError}
TRANSIENT_METHODS={'be':'backward_euler','trap':'trapezoidal'}
def transient_run(args):
    """Fixed-step transient by copper-core (src/transient.c): transactional steps, zero initial stored energy."""
    project=Path(existing(args.project)).resolve();o=args.outcome;method=getattr(args,'method','be')
    if not (math.isfinite(args.dt) and 0<args.dt<=1000 and 1<=args.steps<=100000):raise InvalidInput('Transient requires 0 < dt <= 1000 s and 1 <= steps <= 100000')
    result=subprocess.run([core_path(args.core),'transient',str(project),repr(args.dt),str(args.steps),method],capture_output=True,text=True,timeout=600)
    message=result.stderr.strip()
    if result.returncode==2 and message.startswith('PROJECT ERROR'):raise InvalidInput(message)
    rule=message[len('TRANSIENT '):].split(':',1)[0] if message.startswith('TRANSIENT ') else ''
    if result.returncode not in (0,2) or (result.returncode==2 and rule not in TRANSIENT_RULE_OUTCOME):raise CoreError(message or f'copper-core transient failed ({result.returncode})')
    rows=list(csv.reader(io.StringIO(result.stdout)));header,samples=(rows[0],rows[1:]) if rows else ([],[])
    defaults=MnaDefaults
    o['data']={'project':str(project),'method':TRANSIENT_METHODS[method],'first_step_method':'backward_euler',
               'initial_state':'zero stored energy; sources switch on at t = 0+','dt_s':args.dt,'steps_requested':args.steps,
               'steps_accepted':len(samples),'tolerances':defaults,'columns':header,'csv':None,
               'final':dict(zip(header,map(float,samples[-1]))) if samples else None}
    if args.out and samples:
        Path(args.out).parent.mkdir(parents=True,exist_ok=True);Path(args.out).write_text(result.stdout);o['data']['csv']=str(Path(args.out).resolve())
    if rule:raise TRANSIENT_RULE_OUTCOME[rule](message[len('TRANSIENT '):]+f' ({len(samples)} step(s) accepted; no successful waveform is claimed)')
    if args.out:print(args.out if args.format=='json' else f"{TRANSIENT_METHODS[method].replace('_',' ').capitalize()} (first step Backward Euler), dt={args.dt:g} s, {len(samples)} steps -> {args.out}")
    else:o['data']['rows']=[[float(x) for x in r] for r in samples];sys.stdout.write(result.stdout)
    return 0
# Newton tolerances used by every analysis (src/mna.c mna_options_default).
MnaDefaults={'max_iterations':100,'absolute_residual':1e-10,'relative_residual':1e-10,'max_voltage_step_v':0.25,
             'absolute_update_v':1e-9,'relative_update':1e-9}
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
IDENTITY_FIELDS={'kind','symbol','footprint','manufacturer','mpn','datasheet','model','pinmap','provenance','catalogue','family','role','alternates'}
def natural(pin):return [int(t) if t.isdigit() else t for t in re.split(r'(\d+)',pin)]
def library_parity(part,summary,symdir,fpdir):
    """Problems of one part against the native KiCad libraries: symbol and footprint exist, symbol pin numbers
    equal footprint pad numbers, the pin map and MCU pins name real pins, and pin names do not contradict it.
    Raises Unverified when the library data itself is outside supported KiCad 9.x content."""
    if text_at(part,'kind') in ('V','I'):return []
    pins=symbol_pins(resolve_symbol(symdir,text_at(part,'symbol'),{}))  # Unverified: unsupported library data
    numbers={text_at(p,'number') for p in pins};names={text_at(p,'number'):text_at(p,'name') for p in pins};types={text_at(p,'number'):str(p[1]) for p in pins}
    problems=[];lib,name=safe_lib_id(text_at(part,'footprint'));path=fpdir/f'{lib}.pretty'/f'{name}.kicad_mod'
    if not path.is_file():problems.append(f"footprint {text_at(part,'footprint')} not found in {fpdir}")
    else:
        pads={str(p[1]) for p in direct(library_tree(path),'pad') if str(p[1])}  # unnumbered pads are mechanical
        if pads!=numbers:problems.append(f'symbol/footprint pad parity: symbol pins without a pad {sorted(numbers-pads,key=natural)}, pads without a symbol pin {sorted(pads-numbers,key=natural)}')
    for terminal,pin in summary['terminals'].items():
        if pin not in numbers:problems.append(f'pin map: terminal {terminal} -> pin {pin}, which the symbol does not have')
    if summary['model'] and not problems:
        try:check_terminal_pin_names(str(part[1]),summary,model_specs()[summary['model']],pins)
        except InvalidInput as exc:problems.append(str(exc))
    mcu=one(part,'mcu')
    if mcu:
        declared=[(k,str(p)) for k in ('supply','ground','program') for p in (one(mcu,k) or [None])[1:]]+[('reset',text_at(mcu,'reset'))]*bool(one(mcu,'reset'))
        declared+=[('gpio',str(g[1])) for g in direct(one(mcu,'gpio') or [None],'pin')]
        for what,pin in declared:
            if pin not in numbers:problems.append(f'mcu {what} pin {pin} is not a symbol pin')
        for k in ('supply','ground'):
            for pin in (str(p) for p in (one(mcu,k) or [None])[1:]):
                if pin in types and types[pin] not in ('power_in','passive'):problems.append(f'mcu {k} pin {pin} is a {types[pin]} pin in the symbol, not a power input')
        for g in direct(one(mcu,'gpio') or [None],'pin'):
            pin,label=str(g[1]),str(g[2])
            if pin in names and label.upper() not in names[pin].upper().split('/'):problems.append(f"mcu gpio pin {pin} is named '{names[pin]}' in the symbol, not {label}")
    return problems
def part_import(args):
    """Validate a catalogue for import (schema, data provenance, native library parity) and, with --into,
    merge it into a user catalogue. Nothing is written unless every part is accepted."""
    source=Path(existing(args.file,'Catalogue to import'));tree=load(source)
    if head(tree)!='catalogue':raise InvalidInput(f'{source} is not a (catalogue ...) file')
    target=Path(args.into) if args.into else None
    if target and target.resolve()==BUNDLED_CATALOGUE.resolve():raise InvalidInput('profiles/default.parts is the bundled catalogue and is read-only')
    existing_ids={str(p[1]) for p in direct(load(BUNDLED_CATALOGUE),'part')}|({str(p[1]) for p in direct(load(target),'part')} if target and target.exists() else set())
    try:symdir=discover_symbol_dir(args.symbol_dir);fpdir=discover_footprint_dir(args.footprint_dir)
    except FileNotFoundError as exc:symdir=fpdir=None;library_error=str(exc).splitlines()[0]
    accepted,rejected,duplicates,unverified,seen=[],[],[],[],set()
    parts=direct(tree,'part')
    for part in parts:
        pid=str(part[1]) if len(part)>1 else '';reasons=[]
        if pid in seen or pid in existing_ids:duplicates.append(pid);continue
        seen.add(pid)
        probe=source.with_name(f'.{source.name}.{pid}.check')
        try:
            write_tree(probe,sx('catalogue',[x for x in part if not (isinstance(x,list) and head(x)=='alternates')]))
            summary=validate_catalogue(probe,args.core,label=pid)['parts'][0]
        except InvalidInput as exc:rejected.append({'id':pid,'reasons':[str(exc)]});continue
        finally:probe.unlink(missing_ok=True)
        data=sorted(head(x) for x in part[2:] if isinstance(x,list) and head(x) not in IDENTITY_FIELDS)
        if data and not text_at(part,'provenance'):reasons.append(f"{', '.join(data)} need (provenance \"...\") naming their source")
        if symdir:
            try:reasons+=library_parity(part,summary,symdir,fpdir)
            except Unverified as exc:unverified.append({'id':pid,'reason':str(exc)});continue
            except (ValueError,OSError) as exc:reasons.append(f"symbol {text_at(part,'symbol')}: {exc}")
        if reasons:rejected.append({'id':pid,'reasons':reasons})
        else:accepted.append(pid)
    known=existing_ids|set(accepted)
    for part in parts:
        for alt in (str(a) for a in (one(part,'alternates') or [None])[1:]):
            if str(part[1]) in accepted and alt not in known:
                accepted.remove(str(part[1]));rejected.append({'id':str(part[1]),'reasons':[f'alternate {alt} is not an accepted or existing part']})
    status='INVALID_INPUT' if rejected or duplicates else 'UNVERIFIED' if not symdir or unverified else 'PASS'
    written=None
    if target and status=='PASS':
        merged=load(target) if target.exists() else sx('catalogue')
        if head(merged)!='catalogue':raise InvalidInput(f'{target} is not a (catalogue ...) file')
        merged.extend(copy.deepcopy(p) for p in parts)
        probe=target.with_name(target.name+'.check');target.parent.mkdir(parents=True,exist_ok=True)
        try:write_tree(probe,merged);validate_catalogue(probe,args.core,label=target)
        finally:probe.unlink(missing_ok=True)
        write_tree(target,merged);written=str(target.resolve())
    message='; '.join(f"{r['id']}: {'; '.join(r['reasons'])}" for r in rejected)+('; ' if rejected and duplicates else '')+(f"duplicate part id(s): {', '.join(duplicates)}" if duplicates else '')
    if status=='UNVERIFIED':message='schema and provenance checked, but native library parity was not: '+(library_error if not symdir else '; '.join(f"{u['id']}: {u['reason']}" for u in unverified))+'; nothing written'
    args.outcome.update(status=status,message=message,data={'file':str(source.resolve()),'accepted':accepted,'rejected':rejected,'duplicates':duplicates,'unverified':unverified,
        'libraries':{'symbols':str(symdir),'footprints':str(fpdir)} if symdir else None,'written':written})
    if args.format=='human':
        for pid in accepted:print(f'  ACCEPTED  {pid}')
        for r in rejected:print(f"  REJECTED  {r['id']}: "+'; '.join(r['reasons']))
        for pid in duplicates:print(f'  DUPLICATE {pid}')
        for u in unverified:print(f"  UNVERIFIED {u['id']}: {u['reason']}")
        print(f'Status: {status}'+(f'; merged into {args.into}' if written else '; nothing written' if target else ''))
    elif args.format=='json':print(json.dumps(args.outcome['data'],indent=2))
    return 0 if status=='PASS' else 2
def layout_command(args):
    """place: staged placement search; route: route the project's own placement. Both re-check every candidate
    with copper-core; only a candidate with no hard violation and no unrouted connection is written."""
    import layout
    project=Path(existing(args.project)).resolve();tree=load(project)
    try:fpdir=discover_footprint_dir(args.footprint_dir)
    except FileNotFoundError as exc:raise ToolUnavailable(str(exc).splitlines()[0]) from exc
    if args.command=='place':
        seeds=tuple(int(s) for s in args.seeds.split(','))
        if not seeds or args.iterations<0 or args.iterations>100000:raise InvalidInput('--seeds needs integers and --iterations 0..100000')
        best,candidates,problem=layout.optimize(tree,fpdir,args.core,seeds,args.iterations)
    else:
        problem=layout.problem_from(tree,fpdir)
        missing=[c['ref'] for c in problem['components'] if c['ref'] not in problem['given']]
        if missing:raise InvalidInput(f'route needs an (at ...) placement for every part; missing: {", ".join(missing)} (run place first)')
        best=layout.evaluate(tree,problem,dict(problem['given']),fpdir,args.core,0,'given placement');candidates=[best]
    feasible=bool(best and best['feasible'])
    status='PASS' if feasible else 'UNVERIFIED' if best and not best['judged'] else 'FAIL'
    data={'project':str(project),'feasible':feasible,'chosen':layout.brief(best),'candidates':[layout.brief(c) for c in candidates],
          'placement_failures':problem.get('placement_failures',[]),'weights':layout.WEIGHTS,'written':None}
    if feasible:
        out=Path(args.out)
        if args.command=='place':
            placed=layout.placed_project(best,problem)
            if out.exists() and not args.force:raise InvalidInput(f'{out} exists; use --force to replace it')
            write_tree(out,placed);data['written']=str(out.resolve())
        else:
            out.mkdir(parents=True,exist_ok=True);write_tree(out/'routed.copper',best['resolved']);data['written']=str((out/'routed.copper').resolve())
    if args.command=='route' or args.report:
        report=Path(args.report) if args.report else Path(args.out)/'layout.json'
        report.parent.mkdir(parents=True,exist_ok=True);report.write_text(json.dumps(data,indent=2)+'\n',encoding='utf-8');data['report']=str(report.resolve())
    c=data['chosen']
    message='' if feasible else ('no feasible board was found' if args.command=='place' else 'the given placement is not feasible')+(
        f": best candidate '{c['label']}' has {c['hard_violations']} hard violation(s) {c['hard_by_rule']} and {c['unrouted']} unrouted connection(s); no board was written" if c else
        ': no legal placement: '+'; '.join(data['placement_failures']))
    args.outcome.update(status=status,message=message,data=data)
    if args.format=='human':
        for cand in data['candidates']:
            mark='*' if c and cand['label']==c['label'] else ' '
            print(f"{mark} {cand['label']:<38} {'feasible' if cand['feasible'] else 'INFEASIBLE':<10} hard={cand['hard_violations']:<3} unrouted={cand['unrouted']:<3} "
                  f"length={cand['length_mm']:.2f} mm vias={cand['vias']} bends={cand['bends']} width={cand['trace_width_mm']:.2f} mm soft={cand['soft_score']:.2f}")
        for f in data['placement_failures']:print(f'  PLACEMENT {f}')
        print(f'Status: {status}'+(f" -> {data['written']}" if data['written'] else '')+(f'; {message}' if message else ''))
    elif args.format=='json':print(json.dumps(data,indent=2))
    return 0 if feasible else 2
def design_command(args):
    """Request text -> verified KiCad project, through the EngineeringSpec and the shared pipeline (tools/design.py)."""
    import design
    out=Path(args.out)
    if out.exists() and any(out.iterdir()) and not args.force:raise InvalidInput(f'{out} is not empty; use --force to reuse it')
    r=design.run(args.text,out,args.profile,args.answers,args.symbol_dir,args.footprint_dir,args.kicad_cli,args.core)
    args.outcome.update(status=r['status'],message=r.get('message',''),data=r)
    if args.format=='human':
        print(f"Request: {r['request']}")
        if r.get('profile'):print(f"Design profile: {r['profile']}")
        if r['decisions']:print('Decisions:')
        for x in r['decisions']:print(f"  {x['source'].upper():<9} {x['field']:<22} {x['value']}   ({x['reason']})")
        for q in r['questions']:print(f"  QUESTION  {q['field']}: {q['text']}"+(f" Options: {', '.join(q['options'])}." if q['options'] else '')+f" Answer with {q['answer_with']}.")
        for n,gate in sorted(((v['gate'],k) for k,v in r['gates'].items())):
            g=r['gates'][gate];detail=g['detail'] if isinstance(g['detail'],str) else json.dumps(g['detail'])
            print(f"  GATE {n:<2} {g['status']:<10} {gate:<14} {detail[:160]}")
        for n in r['notes']:print(f'  NOTE {n}')
        print(f"Status: {r['status']}"+(f"; {r['message']}" if r.get('message') else '')+f"   (report: {Path(args.out)/'design.json'})")
    elif args.format=='json':print(json.dumps(r,indent=2,default=str))
    return 0 if r['status']=='PASS' else 2
def spec_outcome(args,spec,parse_errors=()):
    """Validate an EngineeringSpec (tools/spec_ir.py) and record status: errors INVALID_INPUT, open questions UNVERIFIED."""
    import spec_ir
    errors,questions=spec_ir.validate(spec);errors=list(parse_errors)+errors
    status='INVALID_INPUT' if errors else 'UNVERIFIED' if questions else 'PASS'
    message='; '.join(errors) if errors else f'{len(questions)} open question(s): the spec is not complete' if questions else ''
    args.outcome.update(status=status,message=message,data={'fingerprint':spec_ir.fingerprint(spec),'errors':errors,
        'questions':[vars(q) for q in questions],'provenance':spec_ir.provenance_report(spec),'spec':spec_ir.to_json(spec)})
    if args.format=='human':
        print(f"Spec: {spec.name.value}   {len(spec.components)} component(s), {len(spec.nets())} net(s)   fingerprint {args.outcome['data']['fingerprint'][:16]}")
        for e in errors:print(f'  ERROR    {e}')
        for q in questions:print(f'  QUESTION {q.field}: {q.text}'+(f" (options: {', '.join(q.options)})" if q.options else ''))
        for p in args.outcome['data']['provenance']:print(f"  {p['provenance'].upper():<9} {p['field']}"+(f" = {p['value']}" if 'value' in p else ''))
        print(f'Status: {status}')
    elif args.format=='json':print(json.dumps(args.outcome['data'],indent=2))
    return errors,questions
def spec_command(args):
    import spec_ir,spec_nlp
    if args.command=='spec-parse':
        if args.file and args.text:raise InvalidInput('give the description as text or with --file, not both')
        try:text=Path(existing(args.file,'Description file')).read_text(encoding='utf-8-sig') if args.file else args.text
        except UnicodeDecodeError as exc:raise InvalidInput(f'{args.file} is not UTF-8 text (byte {exc.start}); save it as UTF-8') from None
        spec,parse_errors=spec_nlp.parse(text or '');errors,questions=spec_outcome(args,spec,parse_errors)
        if args.out and not errors:
            if Path(args.out).exists() and not args.force:raise InvalidInput(f'{args.out} exists; use --force to replace it')
            spec.unresolved=questions;path=Path(args.out);path.parent.mkdir(parents=True,exist_ok=True)
            tmp=path.with_name(path.name+'.tmp');tmp.write_text(spec_ir.dumps(spec),encoding='utf-8');tmp.replace(path)
            args.outcome['data']['path']=str(path.resolve())
            if args.format=='human':print(f'Wrote {args.out}'+(' with the open questions recorded under (unresolved ...)' if questions else ''))
        return 0 if args.outcome['status']=='PASS' else 2
    try:spec=spec_ir.load(existing(args.spec,'Spec file'))
    except spec_ir.SpecFormatError as exc:raise InvalidInput(f'{args.spec}: {exc}') from None
    if args.command=='spec-check':spec_outcome(args,spec);return 0 if args.outcome['status']=='PASS' else 2
    if Path(args.out).exists() and not args.force:raise InvalidInput(f'{args.out} exists; use --force to replace it')
    tree,notes=spec_ir.lower(spec,args.catalogue,args.core,base=Path(args.spec).resolve().parent)
    # The compiler's own parser is the authority: write to a probe, load it with copper-core, then publish.
    out=Path(args.out);probe=out.with_name(out.name+'.check.copper')
    try:write_tree(probe,tree);report,_=core_run(probe,args.core)
    finally:probe.unlink(missing_ok=True)
    write_tree(out,tree)
    args.outcome.update(status='PASS',data={'path':str(out.resolve()),'fingerprint':spec_ir.fingerprint(spec),'notes':notes,
        'dc_solved':report['dc_solved'],'provenance':spec_ir.provenance_report(spec)})
    if args.format=='human':
        print(f'Lowered {args.spec} -> {args.out} (accepted by copper-core; DC {"solved" if report["dc_solved"] else "NOT solved"})')
        for n in notes:print(f'  NOTE {n}')
        print("Next: pcba project check "+args.out)
    elif args.format=='json':print(json.dumps(args.outcome['data'],indent=2))
    return 0
def run_command(args):
    o=args.outcome;fmt=args.format
    if args.command.startswith('spec-'):return spec_command(args)
    if args.command=='part-import':return part_import(args)
    if args.command in ('place','route'):return layout_command(args)
    if args.command=='design':return design_command(args)
    if args.command=='new':rc=new_project(args);o['data']={'path':str(Path(args.out).resolve())};return rc
    if args.command=='check':
        report,rc=core_run(existing(args.project),args.core);scopes=report_scopes(report)
        o.update(status=dc_status(report) if args.scope=='dc' else overall_status(list(scopes.values())),data={'project':str(Path(args.project).resolve()),'scope':args.scope,'scopes':scopes,'report':report})
        if fmt=='human':render_report(report,None if args.scope=='dc' else scopes)
        else:print(json.dumps(report,indent=2))
        return rc
    if args.command=='transient':return transient_run(args)
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
        if cmd=='transient':p.add_argument('--dt',type=float,default=1e-4);p.add_argument('--steps',type=int,default=1000);p.add_argument('--method',choices=['be','trap'],default='be',help='be: Backward Euler; trap: trapezoidal (first step Backward Euler)');p.add_argument('--out',help='CSV path; omitted: CSV on stdout')
    p=sub.add_parser('build');p.add_argument('project');p.add_argument('--out',default='out/v2');p.add_argument('--core');p.add_argument('--symbol-dir');p.add_argument('--footprint-dir');p.add_argument('--no-route',action='store_true');p.add_argument('--draft',action='store_true',help='Allow electrical failures to produce an explicitly failed draft, never a verified package')
    for cmd in ['verify','fabricate']:
        p=sub.add_parser(cmd);p.add_argument('--out',default='out/v2');p.add_argument('--kicad-cli');p.add_argument('--core');p.add_argument('--project',help='Source .copper to check for freshness (default: path recorded at build)')
    p=sub.add_parser('classify');p.add_argument('text');p.add_argument('--core')
    p=sub.add_parser('catalogue');p.add_argument('--file');p.add_argument('--search',default='')
    p=sub.add_parser('part-add',help='Unknown ratings are omitted, never written as 0');p.add_argument('--file',required=True);p.add_argument('--id',required=True);p.add_argument('--kind',choices=list('RCLDVIJUQM'),required=True);p.add_argument('--symbol',default='');p.add_argument('--footprint',default='');p.add_argument('--power',type=float);p.add_argument('--voltage',type=float);p.add_argument('--tolerance',type=float);p.add_argument('--height',type=float);p.add_argument('--manufacturer',default='');p.add_argument('--mpn',default='');p.add_argument('--datasheet',default='')
    p.add_argument('--model',help='device model id (copper-core models); default: the kind default');p.add_argument('--pinmap',help='terminal=symbol_pin,... e.g. cathode=1,anode=2');p.add_argument('--provenance',default='',help='where the ratings come from');p.add_argument('--core')
    p=sub.add_parser('part-remove');p.add_argument('--file',required=True);p.add_argument('--id',required=True)
    p=sub.add_parser('design',help='request text -> verified KiCad project (docs/engineering/DESIGN.md)');p.add_argument('text');p.add_argument('--out',required=True);p.add_argument('--profile');p.add_argument('--answers');p.add_argument('--symbol-dir');p.add_argument('--footprint-dir');p.add_argument('--kicad-cli');p.add_argument('--force',action='store_true');p.add_argument('--core')
    p=sub.add_parser('place',help='staged placement search with exact re-check (docs/engineering/LAYOUT.md)');p.add_argument('project');p.add_argument('--out',required=True);p.add_argument('--seeds',default='1,2,3');p.add_argument('--iterations',type=int,default=1500);p.add_argument('--report');p.add_argument('--footprint-dir');p.add_argument('--force',action='store_true');p.add_argument('--core')
    p=sub.add_parser('route',help='route the project placement with rip-up/reroute and exact re-check');p.add_argument('project');p.add_argument('--out',required=True);p.add_argument('--report');p.add_argument('--footprint-dir');p.add_argument('--core')
    p=sub.add_parser('part-import',help='validate a catalogue against copper-core and the native KiCad libraries; --into merges it');p.add_argument('file');p.add_argument('--into');p.add_argument('--symbol-dir');p.add_argument('--footprint-dir');p.add_argument('--core')
    p=sub.add_parser('ac',help='small-signal AC sweep; sources need (ac MAGNITUDE PHASE_DEG)');p.add_argument('project');p.add_argument('--start',type=float,required=True);p.add_argument('--stop',type=float,required=True);p.add_argument('--points',type=int,required=True);p.add_argument('--sweep',choices=['log','lin'],default='log');p.add_argument('--out',help='CSV path');p.add_argument('--core')
    p=sub.add_parser('dfm');p.add_argument('target',help='.copper file with geometry, or a build directory');p.add_argument('--core')
    p=sub.add_parser('doctor');p.add_argument('--out',default='out');p.add_argument('--core');p.add_argument('--kicad-cli');p.add_argument('--symbol-dir');p.add_argument('--footprint-dir')
    p=sub.add_parser('library-scan');p.add_argument('--out',default='out/library');p.add_argument('--symbol-dir');p.add_argument('--footprint-dir')
    p=sub.add_parser('spec-parse',help='constrained English -> EngineeringSpec (docs/engineering/ENGINEERING_SPEC.md)');p.add_argument('text',nargs='?');p.add_argument('--file',help='read the description from a UTF-8 text file');p.add_argument('--out',help='.espec path to write');p.add_argument('--force',action='store_true')
    p=sub.add_parser('spec-check');p.add_argument('spec')
    p=sub.add_parser('spec-lower',help='complete EngineeringSpec -> .copper project');p.add_argument('spec');p.add_argument('--out',required=True);p.add_argument('--catalogue');p.add_argument('--force',action='store_true');p.add_argument('--core')

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
