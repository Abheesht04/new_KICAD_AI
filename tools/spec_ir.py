"""EngineeringSpec IR, schema version 1 (docs/engineering/ENGINEERING_SPEC.md).

The first output of every frontend: what the user asked for, with units and provenance.
It is not a schematic, a part choice, a PCB or a solver result. Hand-written `.espec`
files and the constrained-English frontend (spec_nlp.py) produce the same IR; both are
validated here, fingerprinted here and handed to the compiler (lowered to `.copper`) here.

Values are SI. Every value, attribute and connection carries a provenance:
explicit, inferred, defaulted, imported or catalogue-derived. Nothing is invented:
a missing decision becomes a clarification question, an invalid explicit value an error.
"""
from __future__ import annotations

import hashlib
import json
import math
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

from hwc_kicad import Atom as A, Quoted as Q, dump_sexpr, head, parse_sexpr

SCHEMA_VERSION = 1
PROVENANCE = ('explicit', 'inferred', 'defaulted', 'imported', 'catalogue-derived')
UNITS = ('ohm', 'F', 'H', 'V', 'A', 'W', 'Hz', 's', 'm', '1')

TWO_PIN = {'1': '1', '2': '2'}
POLARIZED = {'1': '1', '2': '2', 'cathode': '1', 'k': '1', 'anode': '2', 'a': '2'}
SOURCE_PINS = {'1': '1', '2': '2', '+': '1', '-': '2', 'positive': '1', 'negative': '2', 'pos': '1', 'neg': '2'}

# Role contract: reference prefix, unit of its value (None: no value), whether a value is required,
# how terminal names map to canonical pins (None: any identifier, e.g. MCU GPIO names),
# and the bundled catalogue part it lowers to (None: the user must bind a part).
ROLES = {
    'resistor':       dict(prefix='R', unit='ohm', required=True, positive=True, terminals=TWO_PIN, part='resistor'),
    'capacitor':      dict(prefix='C', unit='F', required=True, positive=True, terminals=TWO_PIN, part='capacitor'),
    'inductor':       dict(prefix='L', unit='H', required=True, positive=True, terminals=TWO_PIN, part='inductor'),
    'diode':          dict(prefix='D', unit=None, required=False, positive=False, terminals=POLARIZED, part='diode'),
    'led':            dict(prefix='D', unit=None, required=False, positive=False, terminals=POLARIZED, part=None),
    'supply':         dict(prefix='V', unit='V', required=True, positive=False, terminals=SOURCE_PINS, part='voltage'),
    'current_source': dict(prefix='I', unit='A', required=True, positive=False, terminals=SOURCE_PINS, part='current'),
    'connector':      dict(prefix='J', unit=None, required=False, positive=False, terminals=None, part=None),
    'mcu':            dict(prefix='U', unit=None, required=False, positive=False, terminals=None, part=None),
    'sensor':         dict(prefix='U', unit=None, required=False, positive=False, terminals=None, part=None),
    'mosfet':         dict(prefix='Q', unit=None, required=False, positive=False, terminals=None, part=None),
    'bjt':            dict(prefix='Q', unit=None, required=False, positive=False, terminals=None, part=None),
}
ATTRIBUTES = ('package', 'color', 'family', 'variant', 'part')
OUTPUTS = ('kicad',)   # kicad: .kicad_sch, .kicad_pcb, .kicad_pro, BOM and reports
TWO_TERMINAL = {r for r, c in ROLES.items() if c['terminals'] in (TWO_PIN, POLARIZED, SOURCE_PINS)}
IDENT = re.compile(r'^[A-Za-z0-9_][A-Za-z0-9_+./-]{0,63}$')
REF = re.compile(r'^[A-Z][A-Z0-9_]{0,15}$')


@dataclass
class Quantity:
    value: float
    unit: str
    provenance: str = 'explicit'


@dataclass
class Attr:
    value: str
    provenance: str = 'explicit'


@dataclass
class Component:
    ref: str
    role: str
    value: Optional[Quantity] = None
    tolerance: Optional[Quantity] = None
    attrs: dict = field(default_factory=dict)
    provenance: str = 'explicit'


@dataclass
class Connection:
    ref: str
    terminal: str
    net: str
    provenance: str = 'explicit'


@dataclass
class Requirement:
    kind: str                     # 'voltage' (on a net) or 'current' (through a component)
    subject: str
    minimum: Optional[Quantity] = None
    maximum: Optional[Quantity] = None
    provenance: str = 'explicit'


@dataclass
class Gpio:
    mcu: str
    pin: str
    load: str
    provenance: str = 'explicit'


@dataclass
class Analysis:
    kind: str                     # dc | ac | transient
    params: dict = field(default_factory=dict)   # name -> Quantity
    provenance: str = 'explicit'


@dataclass
class Question:
    id: str
    field: str
    text: str
    options: list = field(default_factory=list)


@dataclass
class Spec:
    name: Attr = field(default_factory=lambda: Attr('spec', 'defaulted'))
    source: dict = field(default_factory=lambda: {'kind': 'handwritten', 'text': ''})
    components: list = field(default_factory=list)
    connections: list = field(default_factory=list)
    requirements: list = field(default_factory=list)
    gpio: list = field(default_factory=list)
    board: dict = field(default_factory=dict)        # width, height (m), layers ('1')
    analyses: list = field(default_factory=list)
    profiles: dict = field(default_factory=dict)     # catalogue, dfm -> Attr
    unresolved: list = field(default_factory=list)
    no_connect: dict = field(default_factory=dict)   # ref -> sorted pins deliberately left unconnected
    outputs: list = field(default_factory=list)      # requested artifacts, e.g. 'kicad'; not engineering content

    def component(self, ref):
        return next((c for c in self.components if c.ref == ref), None)

    def nets(self):
        return sorted({c.net for c in self.connections})


# --------------------------------------------------------------------------- validation
def validate(spec):
    """Check the spec against the schema. Normalizes terminal names to canonical pins and adds
    clarification questions for missing decisions. Returns (errors, questions)."""
    errors, questions = [], []

    def ask(field_, text, options=()):
        if not any(q.field == field_ for q in questions):
            questions.append(Question(f'q{len(questions) + 1}', field_, text, list(options)))

    refs = {}
    for c in spec.components:
        if not REF.match(c.ref):
            errors.append(f'{c.ref}: references are an uppercase letter followed by letters, digits or _')
        if c.ref in refs:
            errors.append(f'{c.ref}: duplicate component reference')
        refs[c.ref] = c
        role = ROLES.get(c.role)
        if role is None:
            errors.append(f'{c.ref}: unknown role {c.role!r} (known: {", ".join(sorted(ROLES))})')
            continue
        for name, a in c.attrs.items():
            if name not in ATTRIBUTES:
                errors.append(f'{c.ref}: unknown attribute {name!r}')
            elif not a.value or not IDENT.match(a.value):
                errors.append(f'{c.ref}: invalid {name} {a.value!r}')
        if c.value is not None:
            errors.extend(check_quantity(f'{c.ref} value', c.value, role['unit'], role['positive']))
        elif role['required']:
            ask(f'{c.ref}.value', f'What is the {c.role.replace("_", " ")} value of {c.ref} (in {role["unit"]})?')
        if c.tolerance is not None:
            errors.extend(check_quantity(f'{c.ref} tolerance', c.tolerance, '1', False))
            if c.tolerance.unit == '1' and math.isfinite(c.tolerance.value) and not 0 <= c.tolerance.value < 1:
                errors.append(f'{c.ref}: tolerance must be in [0, 1) as a fraction')
        if c.role == 'mcu' and 'variant' not in c.attrs and 'part' not in c.attrs:  # an explicit part identifies it
            family = c.attrs['family'].value if 'family' in c.attrs else 'MCU'
            ask(f'{c.ref}.variant', f'Which exact {family} module or board is {c.ref}? A family name does not identify '
                'a part, its pins or its footprint.', MCU_VARIANTS.get(family.lower(), []))

    seen = {}
    for k in spec.connections:
        c = refs.get(k.ref)
        if not IDENT.match(k.net):
            errors.append(f'{k.ref}: invalid net name {k.net!r}')
        if c is None:
            errors.append(f'connection to unknown component {k.ref}')
            continue
        role = ROLES.get(c.role)
        if role is None:
            continue
        mapping = role['terminals']
        terminal = k.terminal.lower() if mapping is not None else k.terminal
        if mapping is not None:
            if terminal not in mapping:
                errors.append(f'{k.ref}: a {c.role} has no terminal {k.terminal!r} (use {", ".join(sorted(set(mapping)))})')
                continue
            k.terminal = mapping[terminal]
        elif not IDENT.match(k.terminal):
            errors.append(f'{k.ref}: invalid terminal {k.terminal!r}')
            continue
        key = (k.ref, k.terminal)
        if key in seen and seen[key] != k.net:
            errors.append(f'{k.ref} terminal {k.terminal} is connected to both {seen[key]} and {k.net}')
        seen[key] = k.net
    # Remove exact duplicates so that repeating a connection does not change the spec.
    unique = {}
    for k in spec.connections:
        unique.setdefault((k.ref, k.terminal, k.net), k)
    spec.connections = list(unique.values())

    # Questions carried in the spec (from a frontend or the file) stay open until what they ask is stated.
    carried = [q for q in spec.unresolved if not _satisfied(spec, q.field, seen)]
    pending = {q.field.split('.')[0] for q in carried}
    for c in spec.components:
        if c.ref in pending:
            continue
        if c.role in TWO_TERMINAL:
            for pin in ('1', '2'):
                if (c.ref, pin) not in seen:
                    ask(f'{c.ref}.{pin}', f'Which net does terminal {pin} of {c.ref} ({c.role.replace("_", " ")}) connect to?')
        elif ROLES.get(c.role) and not any(k.ref == c.ref for k in spec.connections) and not any(
                g.load == c.ref or g.mcu == c.ref for g in spec.gpio):
            ask(f'{c.ref}.connections', f'How is {c.ref} ({c.role}) connected?')

    nets = set(spec.nets())
    if nets and 'GND' not in nets:
        ask('nets.GND', 'Which net is ground (GND)? Every circuit needs one reference node.', sorted(nets))
    for r in spec.requirements:
        if r.kind == 'voltage' and r.subject not in nets:
            errors.append(f'voltage requirement on unknown net {r.subject}')
        if r.kind == 'current' and r.subject not in refs:
            errors.append(f'current requirement on unknown component {r.subject}')
        if r.kind not in ('voltage', 'current'):
            errors.append(f'unknown requirement kind {r.kind!r}')
        unit = 'V' if r.kind == 'voltage' else 'A'
        for q in (r.minimum, r.maximum):
            if q is not None:
                errors.extend(check_quantity(f'{r.kind} requirement on {r.subject}', q, unit, False))
        if r.minimum is None and r.maximum is None:
            errors.append(f'{r.kind} requirement on {r.subject} has no bound')
        if r.minimum and r.maximum and math.isfinite(r.minimum.value) and math.isfinite(r.maximum.value) \
                and r.minimum.value > r.maximum.value:
            errors.append(f'{r.kind} requirement on {r.subject}: minimum exceeds maximum')
    for g in spec.gpio:
        mcu, load = refs.get(g.mcu), refs.get(g.load)
        if mcu is None or mcu.role != 'mcu':
            errors.append(f'GPIO {g.pin}: {g.mcu} is not an MCU in this spec')
        if load is None:
            errors.append(f'GPIO {g.pin}: drives unknown component {g.load}')
        if not IDENT.match(g.pin):
            errors.append(f'invalid GPIO pin name {g.pin!r}')
    for name, q in spec.board.items():
        if name not in ('width', 'height', 'layers'):
            errors.append(f'unknown board field {name!r}')
            continue
        errors.extend(check_quantity(f'board {name}', q, '1' if name == 'layers' else 'm', True))
        if name == 'layers' and math.isfinite(q.value) and (q.value != int(q.value) or not 1 <= q.value <= 32):
            errors.append('board layers must be a whole number from 1 to 32')
    for a in spec.analyses:
        errors.extend(check_analysis(a))
    for name in spec.profiles:
        if name not in ('catalogue', 'dfm'):
            errors.append(f'unknown profile {name!r}')
    for ref, pins in spec.no_connect.items():
        if ref not in refs:
            errors.append(f'no_connect on unknown component {ref}')
        for pin in pins:
            if not IDENT.match(pin):
                errors.append(f'{ref}: invalid no-connect pin {pin!r}')
            elif (ref, pin) in seen:
                errors.append(f'{ref} pin {pin} is both connected and no-connect')
    for o in spec.outputs:
        if o not in OUTPUTS:
            errors.append(f'unknown output {o!r} (known: {", ".join(OUTPUTS)})')
    for q in carried:
        if not any(x.field == q.field for x in questions):
            questions.append(q)
    return errors, questions


def _satisfied(spec, field_, connected):
    ref, _, what = field_.partition('.')
    if ref == 'nets':
        return what in spec.nets()
    if ref == 'gpio':
        return any(g.pin == what for g in spec.gpio)
    c = spec.component(ref)
    if c is None:
        return False   # e.g. power.input: open until the user removes it with the answer stated
    if what == 'connections':
        return any(k.ref == ref for k in spec.connections)
    if what in ('1', '2'):
        return (ref, what) in connected
    if what == 'polarity':
        return (ref, '1') in connected and (ref, '2') in connected
    if what == 'value':
        return c.value is not None
    return what in c.attrs


MCU_VARIANTS = {
    'esp32': ['ESP32-WROOM-32E module', 'ESP32-S3-WROOM-1 module', 'ESP32-C3-MINI-1 module', 'ESP32-DevKitC board'],
    'stm32': ['an exact STM32 part number'], 'rp2040': ['RP2040 chip', 'Raspberry Pi Pico board'],
}
ANALYSIS_PARAMS = {'dc': {}, 'ac': {'start': 'Hz', 'stop': 'Hz', 'points': '1'},
                   'transient': {'step': 's', 'stop': 's', 'method': None}}


def check_quantity(what, q, unit, positive):
    if q.provenance not in PROVENANCE:
        return [f'{what}: unknown provenance {q.provenance!r}']
    if not math.isfinite(q.value):
        return [f'{what}: {q.value} is not a finite number']
    if unit is None:
        return [f'{what}: this role takes no value']
    if q.unit != unit:
        return [f'{what}: unit {q.unit} where {unit} is required']
    if positive and q.value <= 0:
        return [f'{what}: must be greater than zero (got {q.value:g} {unit})']
    return []


def check_analysis(a):
    if a.kind not in ANALYSIS_PARAMS:
        return [f'unknown analysis {a.kind!r}']
    errors = []
    allowed = ANALYSIS_PARAMS[a.kind]
    for name, q in a.params.items():
        if name not in allowed:
            errors.append(f'{a.kind} analysis: unknown parameter {name!r}')
        elif name == 'method':
            if q.value not in ('be', 'trap'):
                errors.append('transient method must be be or trap')
        else:
            errors.extend(check_quantity(f'{a.kind} {name}', q, allowed[name], True))
    if a.kind == 'ac' and all(k in a.params for k in ('start', 'stop')) and a.params['start'].value > a.params['stop'].value:
        errors.append('ac analysis: start exceeds stop')
    return errors


# --------------------------------------------------------------------------- fingerprint
def semantic(spec):
    """The engineering content of a spec: order-independent, without provenance, name or source."""
    def qv(q):
        return None if q is None else [float(f'{q.value:.12g}'), q.unit]
    return {
        'schema': SCHEMA_VERSION,
        'components': sorted([c.ref, c.role, qv(c.value), qv(c.tolerance), sorted((k, a.value) for k, a in c.attrs.items())]
                             for c in spec.components),
        'connections': sorted([k.ref, k.terminal, k.net] for k in spec.connections),
        'requirements': sorted([r.kind, r.subject, qv(r.minimum), qv(r.maximum)] for r in spec.requirements),
        'gpio': sorted([g.mcu, g.pin, g.load] for g in spec.gpio),
        'board': sorted([k, qv(v)] for k, v in spec.board.items()),
        'analyses': sorted([a.kind, sorted((k, v.value if k == 'method' else qv(v)) for k, v in a.params.items())]
                           for a in spec.analyses),
        'profiles': sorted([k, a.value] for k, a in spec.profiles.items()),
        'no_connect': sorted([r, list(p)] for r, p in spec.no_connect.items()),
    }


def fingerprint(spec):
    return hashlib.sha256(json.dumps(semantic(spec), sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def provenance_report(spec):
    """Every value that was not stated explicitly, so a reader can see what was assumed."""
    out = []
    for c in spec.components:
        if c.provenance != 'explicit':
            out.append({'field': c.ref, 'provenance': c.provenance})
        for name, q in (('value', c.value), ('tolerance', c.tolerance)):
            if q and q.provenance != 'explicit':
                out.append({'field': f'{c.ref}.{name}', 'provenance': q.provenance, 'value': [q.value, q.unit]})
        for name, a in c.attrs.items():
            if a.provenance != 'explicit':
                out.append({'field': f'{c.ref}.{name}', 'provenance': a.provenance, 'value': a.value})
    for k in spec.connections:
        if k.provenance != 'explicit':
            out.append({'field': f'{k.ref}.{k.terminal}', 'provenance': k.provenance, 'value': k.net})
    for name, q in spec.board.items():
        if q.provenance != 'explicit':
            out.append({'field': f'board.{name}', 'provenance': q.provenance, 'value': [q.value, q.unit]})
    if spec.name.provenance != 'explicit':
        out.append({'field': 'name', 'provenance': spec.name.provenance, 'value': spec.name.value})
    return out


def to_json(spec):
    def qj(q):
        return None if q is None else {'value': q.value, 'unit': q.unit, 'provenance': q.provenance}
    return {
        'schema_version': SCHEMA_VERSION, 'name': spec.name.value, 'source': spec.source,
        'components': [{'ref': c.ref, 'role': c.role, 'value': qj(c.value), 'tolerance': qj(c.tolerance),
                        'attributes': {k: {'value': a.value, 'provenance': a.provenance} for k, a in c.attrs.items()},
                        'provenance': c.provenance} for c in spec.components],
        'connections': [vars(k) for k in spec.connections],
        'requirements': [{'kind': r.kind, 'subject': r.subject, 'minimum': qj(r.minimum), 'maximum': qj(r.maximum),
                          'provenance': r.provenance} for r in spec.requirements],
        'gpio': [vars(g) for g in spec.gpio],
        'board': {k: qj(v) for k, v in spec.board.items()},
        'analyses': [{'kind': a.kind, 'params': {k: (v.value if k == 'method' else qj(v)) for k, v in a.params.items()}}
                     for a in spec.analyses],
        'profiles': {k: a.value for k, a in spec.profiles.items()},
    }


# --------------------------------------------------------------------------- .espec text
def _num(v):
    return A(f'{v:.12g}')


def _q(key, q):
    node = [A(key), _num(q.value)] + ([] if q.unit == '1' else [A(q.unit)])   # dimensionless: no unit atom
    if q.provenance != 'explicit':
        node.append(A(q.provenance))
    return node


def _attr(key, a):
    node = [A(key), Q(a.value)]
    if a.provenance != 'explicit':
        node.append(A(a.provenance))
    return node


def dumps(spec):
    """Canonical `.espec` text: components by reference, connections sorted; provenance kept."""
    root = [A('engineering_spec'), [A('version'), A(str(SCHEMA_VERSION))], _attr('name', spec.name)]
    source = [A('source'), A(spec.source.get('kind', 'handwritten'))]
    if spec.source.get('text'):
        source.append(Q(spec.source['text']))
    root.append(source)
    comps = [A('components')]
    for c in sorted(spec.components, key=lambda c: c.ref):
        node = [A('component'), A(c.ref), [A('role'), A(c.role)]]
        if c.value:
            node.append(_q('value', c.value))
        if c.tolerance:
            node.append(_q('tolerance', c.tolerance))
        node.extend(_attr(k, c.attrs[k]) for k in sorted(c.attrs))
        if c.provenance != 'explicit':
            node.append([A('provenance'), A(c.provenance)])
        comps.append(node)
    root.append(comps)
    conns = [A('connections')]
    for k in sorted(spec.connections, key=lambda k: (k.ref, k.terminal, k.net)):
        node = [A('connect'), A(k.ref), A(k.terminal), A(k.net)]
        if k.provenance != 'explicit':
            node.append(A(k.provenance))
        conns.append(node)
    root.append(conns)
    if spec.requirements:
        reqs = [A('requirements')]
        for r in sorted(spec.requirements, key=lambda r: (r.kind, r.subject)):
            node = [A(r.kind), A(r.subject)]
            if r.minimum:
                node.append(_q('min', r.minimum))
            if r.maximum:
                node.append(_q('max', r.maximum))
            reqs.append(node)
        root.append(reqs)
    if spec.gpio:
        root.append([A('gpio')] + [[A('drive'), A(g.mcu), A(g.pin), A(g.load)] for g in sorted(spec.gpio, key=lambda g: (g.mcu, g.pin))])
    if spec.board:
        root.append([A('board')] + [_q(k, spec.board[k]) for k in sorted(spec.board)])
    if spec.analyses:
        node = [A('analyses')]
        for a in sorted(spec.analyses, key=lambda a: a.kind):
            item = [A(a.kind)]
            for k in sorted(a.params):
                v = a.params[k]
                item.append([A(k), A(v.value)] if k == 'method' else _q(k, v))
            node.append(item)
        root.append(node)
    if spec.profiles:
        root.append([A('profiles')] + [_attr(k, spec.profiles[k]) for k in sorted(spec.profiles)])
    if spec.no_connect:
        root.append([A('no_connect')] + [[A('pins'), A(r)] + [A(p) for p in spec.no_connect[r]] for r in sorted(spec.no_connect)])
    if spec.outputs:
        root.append([A('outputs')] + [A(o) for o in spec.outputs])
    if spec.unresolved:
        root.append([A('unresolved')] + [[A('question'), A(q.id), Q(q.field), Q(q.text), [Q(o) for o in q.options]]
                                         for q in spec.unresolved])
    return dump_sexpr(root) + '\n'


class SpecFormatError(ValueError):
    """The .espec text does not follow the schema (INVALID_INPUT)."""


def _fields(node, allowed, where):
    seen = set()
    for item in node:
        if not isinstance(item, list) or not item:
            raise SpecFormatError(f'{where}: unexpected {item!r}')
        key = head(item)
        if key not in allowed:
            raise SpecFormatError(f'{where}: unknown field {key!r}')
        if key in seen and allowed[key] == 'one':
            raise SpecFormatError(f'{where}: duplicate field {key!r}')
        seen.add(key)


def _number(text, where):
    try:
        value = float(str(text))
    except ValueError:
        raise SpecFormatError(f'{where}: {text!r} is not a number') from None
    return value


def _provenance(rest, where):
    if not rest:
        return 'explicit'
    if len(rest) > 1 or str(rest[0]) not in PROVENANCE:
        raise SpecFormatError(f'{where}: expected a provenance ({", ".join(PROVENANCE)}), got {rest!r}')
    return str(rest[0])


def _quantity(node, where):
    """(KEY VALUE UNIT [provenance]); a dimensionless quantity (tolerance, layers, points) omits UNIT."""
    if len(node) < 2:
        raise SpecFormatError(f'{where}: expected ({head(node)} VALUE UNIT [provenance])')
    if len(node) == 2 or str(node[2]) in PROVENANCE:
        return Quantity(_number(node[1], where), '1', _provenance(node[2:], where))
    unit = str(node[2])
    if unit not in UNITS:
        raise SpecFormatError(f'{where}: unknown unit {unit!r} (SI units: {", ".join(UNITS)})')
    return Quantity(_number(node[1], where), unit, _provenance(node[3:], where))


def _attribute(node, where):
    if len(node) < 2:
        raise SpecFormatError(f'{where}: expected ({head(node)} "TEXT" [provenance])')
    return Attr(str(node[1]), _provenance(node[2:], where))


def loads(text, origin='handwritten'):
    """Parse `.espec` text into a Spec. Structural problems raise SpecFormatError; semantic
    checks are done by validate()."""
    try:
        root = parse_sexpr(text)
    except ValueError as exc:
        raise SpecFormatError(f'not an S-expression: {exc}') from None
    if head(root) != 'engineering_spec':
        raise SpecFormatError('expected (engineering_spec (version 1) ...)')
    body = root[1:]
    _fields(body, {'version': 'one', 'name': 'one', 'source': 'one', 'components': 'one', 'connections': 'one',
                   'requirements': 'one', 'gpio': 'one', 'board': 'one', 'analyses': 'one', 'profiles': 'one',
                   'unresolved': 'one', 'no_connect': 'one', 'outputs': 'one'}, 'engineering_spec')
    get = {head(n): n for n in body}
    if 'version' not in get or [str(x) for x in get['version'][1:]] != [str(SCHEMA_VERSION)]:
        raise SpecFormatError(f'unsupported schema version (this reader understands {SCHEMA_VERSION})')
    spec = Spec()
    if 'name' in get:
        spec.name = _attribute(get['name'], 'name')
    if 'source' in get:
        src = get['source']
        spec.source = {'kind': str(src[1]) if len(src) > 1 else origin, 'text': str(src[2]) if len(src) > 2 else ''}
    else:
        spec.source = {'kind': origin, 'text': ''}
    for node in (get.get('components') or [None])[1:]:
        if head(node) != 'component' or len(node) < 2:
            raise SpecFormatError('components: expected (component REF (role ROLE) ...)')
        ref = str(node[1])
        _fields(node[2:], {'role': 'one', 'value': 'one', 'tolerance': 'one', 'provenance': 'one',
                           **{a: 'one' for a in ATTRIBUTES}}, ref)
        items = {head(x): x for x in node[2:]}
        if 'role' not in items or len(items['role']) != 2:
            raise SpecFormatError(f'{ref}: (role ROLE) is required')
        c = Component(ref, str(items['role'][1]))
        if 'value' in items:
            c.value = _quantity(items['value'], f'{ref} value')
        if 'tolerance' in items:
            c.tolerance = _quantity(items['tolerance'], f'{ref} tolerance')
        for a in ATTRIBUTES:
            if a in items:
                c.attrs[a] = _attribute(items[a], f'{ref} {a}')
        if 'provenance' in items:
            c.provenance = _provenance(items['provenance'][1:], f'{ref} provenance')
        spec.components.append(c)
    for node in (get.get('connections') or [None])[1:]:
        if head(node) != 'connect' or len(node) < 4:
            raise SpecFormatError('connections: expected (connect REF TERMINAL NET [provenance])')
        spec.connections.append(Connection(str(node[1]), str(node[2]), str(node[3]), _provenance(node[4:], 'connect')))
    for node in (get.get('requirements') or [None])[1:]:
        if len(node) < 2:
            raise SpecFormatError('requirements: expected (voltage NET (min ...) (max ...)) or (current REF ...)')
        _fields(node[2:], {'min': 'one', 'max': 'one'}, f'{head(node)} requirement')
        items = {head(x): x for x in node[2:]}
        spec.requirements.append(Requirement(str(node[0]), str(node[1]),
                                             _quantity(items['min'], 'min') if 'min' in items else None,
                                             _quantity(items['max'], 'max') if 'max' in items else None))
    for node in (get.get('gpio') or [None])[1:]:
        if head(node) != 'drive' or len(node) != 4:
            raise SpecFormatError('gpio: expected (drive MCU PIN LOAD)')
        spec.gpio.append(Gpio(str(node[1]), str(node[2]), str(node[3])))
    if 'board' in get:
        _fields(get['board'][1:], {'width': 'one', 'height': 'one', 'layers': 'one'}, 'board')
        for node in get['board'][1:]:
            spec.board[head(node)] = _quantity(node, f'board {head(node)}')
    for node in (get.get('analyses') or [None])[1:]:
        if not isinstance(node, list) or not node:
            raise SpecFormatError('analyses: expected (dc) (ac (start ..) ...) (transient ...)')
        a = Analysis(str(node[0]))
        for p in node[1:]:
            if head(p) == 'method' and len(p) == 2:
                a.params['method'] = Attr(str(p[1]))
            else:
                a.params[head(p)] = _quantity(p, f'{a.kind} {head(p)}')
        spec.analyses.append(a)
    if 'profiles' in get:
        _fields(get['profiles'][1:], {'catalogue': 'one', 'dfm': 'one'}, 'profiles')
        for node in get['profiles'][1:]:
            spec.profiles[head(node)] = _attribute(node, f'profile {head(node)}')
    for node in (get.get('no_connect') or [None])[1:]:
        if head(node) != 'pins' or len(node) < 3:
            raise SpecFormatError('no_connect: expected (pins REF PIN ...)')
        spec.no_connect[str(node[1])] = sorted({str(x) for x in node[2:]}, key=_natural)
    spec.outputs = [str(x) for x in (get.get('outputs') or [None])[1:]]
    for node in (get.get('unresolved') or [None])[1:]:
        if head(node) != 'question' or len(node) != 5:
            raise SpecFormatError('unresolved: expected (question ID "FIELD" "TEXT" ("OPTION" ...))')
        spec.unresolved.append(Question(str(node[1]), str(node[2]), str(node[3]), [str(o) for o in node[4]]))
    return spec


def load(path):
    return loads(Path(path).read_text(encoding='utf-8'))


# --------------------------------------------------------------------------- compiler handoff
def _natural(pin):
    return [int(t) if t.isdigit() else t for t in re.split(r'(\d+)', pin)]


def gpio_pin(part, name):
    """Pin number of GPIO `name` (IO4, GPIO4 or 4) in a catalogue part's (mcu (gpio ...)) table, or None."""
    def key(text):
        m = re.fullmatch(r'(?:GPIO|IO)?(\d+)', str(text).upper())
        return m.group(1) if m else str(text).upper()
    for node in part:
        if isinstance(node, list) and head(node) == 'mcu':
            for g in node[1:]:
                if isinstance(g, list) and head(g) == 'gpio':
                    for p in g[1:]:
                        if key(p[2]) == key(name):
                            return str(p[1])
    return None


def lower(spec, catalogue_path=None, core=None, base=None):
    """Deterministic handoff to the engineering compiler: a complete, valid spec becomes a `.copper`
    project tree. Every component binds a catalogue part (explicit (part ID), or the bundled generic
    part for its role); nothing is invented. Returns (tree, notes) where notes list what the spec
    states that `.copper` does not carry. Profile paths in the spec are relative to `base` (the
    .espec file's directory). Raises copper.InvalidInput / copper.Unverified."""
    import copper  # late import: copper.py imports this module

    errors, questions = validate(spec)
    if errors:
        raise copper.InvalidInput('the spec is invalid: ' + '; '.join(errors))
    if questions:
        raise copper.Unverified('the spec has open questions: ' + '; '.join(q.text for q in questions))
    nets = spec.nets()
    if 'GND' not in nets:
        raise copper.Unverified('no GND net: the compiler needs a ground reference')
    def profile(name):
        path = Path(spec.profiles[name].value)
        return str(path if path.is_absolute() or base is None else Path(base) / path)
    catalogue_file = catalogue_path or (profile('catalogue') if 'catalogue' in spec.profiles else None)
    parts = {str(p[1]): p for p in copper.catalogue(catalogue_file, core)}
    sx = copper.sx
    components, notes = [], []
    for c in sorted(spec.components, key=lambda c: c.ref):
        part_id = c.attrs['part'].value if 'part' in c.attrs else ROLES[c.role]['part']
        if part_id is None:
            raise copper.Unverified(f'{c.ref}: no bundled catalogue part implements role {c.role!r}; bind one with (part ID) '
                                    'from a catalogue that defines its symbol, footprint, pin map and model')
        if part_id not in parts:
            raise copper.Unverified(f'{c.ref}: part {part_id!r} is not in the catalogue')
        node = sx('component', A(c.ref), sx('part', A(part_id)))
        if c.value is not None:
            node.append(sx('value', _num(c.value.value)))
        pins = sorted((k.terminal, k.net) for k in spec.connections if k.ref == c.ref)
        node.append(sx('pins', *[sx(t, A(n)) for t, n in pins]))
        if spec.no_connect.get(c.ref):
            node.append(sx('nc', *[A(p) for p in spec.no_connect[c.ref]]))
        # GPIO intent sets the DC scenario: the MCU drives each intended load's pin high (the load on).
        drives = []
        for g in spec.gpio:
            if g.mcu != c.ref:
                continue
            pin = gpio_pin(parts[part_id], g.pin)
            if pin is None:
                raise copper.InvalidInput(f'{c.ref}: {g.pin} is not a GPIO of part {part_id}')
            drives.append(sx(pin, A('high')))
            notes.append(f'DC scenario: {c.ref} drives {g.pin} (pin {pin}) high, so {g.load} is on')
        if drives:
            node.append(sx('drive', *drives))
        if c.tolerance is not None:
            notes.append(f'{c.ref}: tolerance {c.tolerance.value:g} comes from the catalogue part in .copper, '
                         'not from the spec')
        components.append(node)
    board = spec.board
    width = board['width'].value * 1000 if 'width' in board else 40.0
    height = board['height'].value * 1000 if 'height' in board else 30.0
    layers = int(board['layers'].value) if 'layers' in board else 2
    if layers != 2:
        raise copper.Unverified(f'the compiler supports two-layer boards; the spec asks for {layers}')
    tree = sx('copper_project', sx('version', A('2')), sx('name', Q(spec.name.value)),
              sx('nets', *[A(n) for n in ['GND'] + [n for n in nets if n != 'GND']]),
              sx('board', *[_num(x) for x in (width, height, 1.6, 0.035, 2)]),
              sx('parts', *[parts[p] for p in sorted({str(c[2][1]) for c in components})]),
              sx('components', *components))
    if 'width' not in board or 'height' not in board:
        notes.append('board size not stated: defaulted to 40 x 30 mm')
    notes.append('board thickness 1.6 mm, copper 0.035 mm: compiler defaults')
    if 'dfm' in spec.profiles:
        rules = copper.load(copper.existing(profile('dfm'), 'DFM profile'))
        if head(rules) != 'rules':
            raise copper.InvalidInput('the DFM profile must have a (rules ...) root')
        tree.append(rules)
    targets = []
    for r in spec.requirements:
        if r.kind == 'voltage':
            lo = r.minimum.value if r.minimum else -1e12
            hi = r.maximum.value if r.maximum else 1e12
            targets.append(sx('voltage', A(r.subject), _num(lo), _num(hi)))
        else:
            raise copper.Unverified(f'a {r.kind} requirement on {r.subject} cannot be checked by the compiler yet')
    if targets:
        tree.append(sx('targets', *targets))
    for a in spec.analyses:
        notes.append(f'{a.kind} analysis requested: run pcba simulate {a.kind}')
    notes.append('placement is not part of the spec: components have no (at ...) yet')
    return tree, notes
