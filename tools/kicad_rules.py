"""KiCad design rules <-> Copper rule profiles (docs/engineering/KICAD_RULES.md).

One table maps each Copper limit to KiCad's board constraint (.kicad_pro board.design_settings.rules), its custom-rule
constraint (.kicad_dru) and its board setup field (.kicad_pcb setup). Export writes a build's profile into all three
so KiCad DRC checks the same limits as copper-core; import reads a KiCad project back into a (rules ...) profile with
the source and date of every limit. Whatever KiCad checks that one global profile cannot express (conditional or
layer rules, maximums, other constraint types, hole-to-copper clearance) is reported, never dropped silently.
"""
from __future__ import annotations
import datetime, hashlib, json, re, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from hwc_kicad import Atom as A, Quoted as Q, SExprError, direct, head, one, parse_sexpr

# field, copper-core default, (low, high) accepted by copper-core, .kicad_pro rules key, .kicad_dru constraint, setup key
LIMITS = [
    ('trace_width_mm', .2, (.05, 10), 'min_track_width', 'track_width', None),
    ('clearance_mm', .2, (.01, 10), 'min_clearance', 'clearance', None),
    ('edge_mm', .3, (.01, 20), 'min_copper_edge_clearance', 'edge_clearance', None),
    ('via_diameter_mm', .6, (.1, 10), 'min_via_diameter', 'via_diameter', None),
    ('drill_mm', .3, (.05, 10), 'min_through_hole_diameter', 'hole_size', None),
    ('annular_mm', .15, (.01, 5), 'min_via_annular_width', 'annular_width', None),
    ('hole_gap_mm', .25, (.01, 10), 'min_hole_to_hole', 'hole_to_hole', None),
    ('courtyard_gap_mm', .25, (0, 10), None, 'courtyard_clearance', None),
    ('mask_expansion_mm', .05, (0, 1), None, None, 'pad_to_mask_clearance'),
    ('mask_web_mm', .1, (0, 2), None, None, 'solder_mask_min_width'),
]
# KiCad board constraints with no Copper limit: checked by KiCad DRC only.
UNMAPPED_PRO = {'min_hole_clearance': 'hole-to-copper clearance', 'min_silk_clearance': 'silkscreen clearance',
                'solder_mask_to_copper_clearance': 'solder mask to copper clearance', 'min_text_height': 'text height',
                'min_text_thickness': 'text thickness', 'min_microvia_diameter': 'microvia diameter',
                'min_microvia_drill': 'microvia drill', 'min_groove_width': 'groove width', 'min_connection': 'minimum connection width'}
UNITS = {'mm': 1.0, 'mil': .0254, 'mils': .0254, 'th': .0254, 'in': 25.4, 'um': .001}


def value(tree, field):
    for f, default, *_ in LIMITS:
        if f == field:
            node = one(one(tree, 'rules'), field)
            return float(node[1]) if node is not None else default
    raise KeyError(field)


# ------------------------------------------------------------------ export (a build's profile -> KiCad)
def export(tree):
    """(kicad_pro dict, kicad_dru text, setup {key: mm}) for a project tree's (rules ...) profile."""
    rules = {pro: value(tree, f) for f, _, _, pro, _, _ in LIMITS if pro}
    pro = {'meta': {'filename': 'design.kicad_pro', 'version': 1}, 'board': {'design_settings': {'rules': rules}}}
    dru = '(version 1)\n' + ''.join(f'(rule "Copper_{f.removesuffix("_mm")}" (constraint {c} (min {value(tree, f):g}mm)))\n'
                                    for f, _, _, _, c, _ in LIMITS if c)
    # Nets routed wider than the minimum (routing (width NET MM)): KiCad checks each one's width as copper-core does.
    for w in direct(one(tree, 'routing'), 'width'):
        dru += f'(rule "Copper_net_width_{w[1]}" (condition "A.NetName == \'/{w[1]}\'") (constraint track_width (min {float(w[2]):g}mm)))\n'
    return pro, dru, {s: value(tree, f) for f, _, _, _, _, s in LIMITS if s}


# ------------------------------------------------------------------ import (a KiCad project -> profile)
def length(text, where):
    m = re.fullmatch(r'\s*(-?\d+(?:\.\d*)?(?:[eE][+-]?\d+)?|-?\.\d+)\s*([a-zA-Z]*)\s*', str(text))
    if not m or (m.group(2) and m.group(2).lower() not in UNITS):
        raise ValueError(f'{where}: cannot read the length {text!r} (units: {", ".join(UNITS)})')
    return float(m.group(1)) * UNITS[m.group(2).lower() or 'mm']


def read_dru(path):
    """[{name, constraints: [(type, {min|max|opt: mm})], condition, layer, severity}] of a .kicad_dru file."""
    text = '\n'.join(line for line in Path(path).read_text(encoding='utf-8-sig').splitlines() if not line.lstrip().startswith('#'))
    try:
        root = parse_sexpr(f'(kicad_dru {text})')
    except SExprError as exc:
        raise ValueError(f'{path}: {exc}') from exc
    version = one(root, 'version')
    if version is None or str(version[1]) != '1':
        raise ValueError(f'{path}: expected (version 1), the KiCad 9 custom rules format')
    rules = []
    for r in direct(root, 'rule'):
        name = str(r[1]) if len(r) > 1 else ''
        constraints = []
        for c in direct(r, 'constraint'):
            limits = {}
            for k in ('min', 'max', 'opt'):
                node = one(c, k)
                if node is not None:
                    limits[k] = length(node[1], f'{path}: rule {name!r} {c[1]} {k}') if len(node) > 1 else None
            constraints.append((str(c[1]), limits))
        cond, layer, sev = one(r, 'condition'), one(r, 'layer'), one(r, 'severity')
        rules.append({'name': name, 'constraints': constraints, 'condition': str(cond[1]) if cond is not None else None,
                      'layer': str(layer[1]) if layer is not None else None, 'severity': str(sev[1]) if sev is not None else None})
    return rules


def project_files(target):
    """(project dir, .kicad_pro, .kicad_dru or None, .kicad_pcb or None) for a .kicad_pro path or a project folder."""
    target = Path(target)
    if target.is_dir():
        pros = sorted(target.glob('*.kicad_pro'))
        if len(pros) != 1:
            raise ValueError(f'{target}: expected one .kicad_pro, found {len(pros)}; pass the .kicad_pro file')
        target = pros[0]
    if target.suffix != '.kicad_pro' or not target.is_file():
        raise ValueError(f'KiCad project not found: {target}')
    found = [target.with_suffix(s) for s in ('.kicad_dru', '.kicad_pcb')]
    return target.parent, target, *[p if p.is_file() else None for p in found]


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def import_project(target, base=None, date=None):
    """KiCad project -> ((rules ...) tree, report). The value of each limit is the strictest KiCad applies to every
    object: the board constraint, the largest net-class clearance and every unconditional custom rule. A board
    constraint of 0 means KiCad checks nothing; that limit then comes from --base or copper-core's default."""
    folder, pro_path, dru_path, pcb_path = project_files(target)
    date = date or datetime.date.today().isoformat()
    try:
        pro = json.loads(pro_path.read_text(encoding='utf-8-sig'))
    except ValueError as exc:
        raise ValueError(f'{pro_path}: not JSON ({exc})') from exc
    rel = lambda p: p.name
    candidates = {f: [] for f, *_ in LIMITS}        # field -> [(mm, source)]
    report = {'project': str(pro_path.resolve()), 'date': date, 'files': {}, 'limits': {}, 'not_imported': [], 'net_classes': []}
    for p in (pro_path, dru_path, pcb_path):
        if p:
            report['files'][rel(p)] = sha256(p)
    board = ((pro.get('board') or {}).get('design_settings') or {}).get('rules') or {}
    for f, _, _, key, _, _ in LIMITS:
        if key and key in board:
            mm = float(board[key])
            if mm > 0:
                candidates[f].append((mm, f'{rel(pro_path)}: board.design_settings.rules.{key}'))
            else:
                report['not_imported'].append({'source': f'{rel(pro_path)}: board.design_settings.rules.{key}', 'value': mm,
                                               'reason': 'KiCad checks no minimum (0)'})
    for key, what in UNMAPPED_PRO.items():
        if float(board.get(key) or 0) > 0:
            report['not_imported'].append({'source': f'{rel(pro_path)}: board.design_settings.rules.{key}', 'value': float(board[key]),
                                           'reason': f'{what}: KiCad DRC checks it; copper-core has no such limit', 'enforced_by_kicad': True})
    for cls in (pro.get('net_settings') or {}).get('classes') or []:
        row = {k: cls.get(k) for k in ('name', 'clearance', 'track_width', 'via_diameter', 'via_drill')}
        report['net_classes'].append(row)
        if row['clearance']:
            candidates['clearance_mm'].append((float(row['clearance']), f'{rel(pro_path)}: net_settings.classes[{row["name"]}].clearance'))
    if dru_path:
        by_constraint = {c: f for f, _, _, _, c, _ in LIMITS if c}
        for r in read_dru(dru_path):
            for kind, limits in r['constraints']:
                src = f'{rel(dru_path)}: rule "{r["name"]}" {kind}'
                field = by_constraint.get(kind)
                why = ('severity ignore' if r['severity'] == 'ignore' else
                       'has a condition' if r['condition'] else 'limited to layer ' + r['layer'] if r['layer'] else
                       f'constraint type {kind} has no Copper limit' if field is None else
                       'has no min' if limits.get('min') is None else None)
                if why:
                    report['not_imported'].append({'source': src, 'reason': f'{why}: one global profile cannot express it',
                                                   'enforced_by_kicad': r['severity'] != 'ignore', 'condition': r['condition'], 'layer': r['layer']})
                    continue
                candidates[field].append((limits['min'], src))
                for k in ('max', 'opt'):
                    if limits.get(k) is not None:
                        report['not_imported'].append({'source': f'{src} {k}', 'value': limits[k], 'enforced_by_kicad': k == 'max',
                                                       'reason': f'{k} limits are not part of a Copper profile'})
    if pcb_path:
        setup = one(parse_sexpr(pcb_path.read_text(encoding='utf-8')), 'setup')
        for f, _, _, _, _, key in LIMITS:
            node = one(setup, key) if key else None
            if node is not None:
                candidates[f].append((float(node[1]), f'{rel(pcb_path)}: setup.{key}'))
    base_rules = parse_sexpr(Path(base).read_text(encoding='utf-8')) if base else None
    if base_rules is not None and head(base_rules) != 'rules':
        raise ValueError(f'{base}: a base profile has a (rules ...) root')
    rules, provenance = [A('rules')], [A('provenance')]
    for f, default, (low, high), *_ in LIMITS:
        if candidates[f]:
            mm, src = max(candidates[f], key=lambda c: c[0])        # the strictest; ties keep the first source
            if not low <= mm <= high:
                raise ValueError(f'{src}: {mm:g} mm is outside the range copper-core accepts for {f} ({low:g} to {high:g})')
            report['limits'][f] = {'value': mm, 'source': src, 'considered': [{'value': v, 'source': s} for v, s in candidates[f]]}
        elif base_rules is not None and one(base_rules, f) is not None:
            mm, src = float(one(base_rules, f)[1]), f'base profile {Path(base).name}'
            report['limits'][f] = {'value': mm, 'source': src, 'considered': []}
        else:
            report['limits'][f] = {'value': default, 'source': 'copper-core default (not in the KiCad project)', 'considered': []}
            continue
        rules.append([A(f), A(f'{mm:g}')])
        provenance.append([A(f), Q(src), Q(date)])
    if base_rules is not None:          # limits KiCad has no notion of (current, deratings, height) come from the base
        mapped = {f for f, *_ in LIMITS}
        for node in base_rules[1:]:
            if isinstance(node, list) and str(node[0]) not in mapped | {'provenance'}:
                rules.append(node)
                provenance.append([A(str(node[0])), Q(f'base profile {Path(base).name}'), Q(date)])
    rules.append(provenance)
    report['status'] = 'UNVERIFIED' if any(n.get('enforced_by_kicad') for n in report['not_imported']) else 'PASS'
    return rules, report
