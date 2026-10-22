"""Constrained-English frontend for the EngineeringSpec IR (docs/engineering/ENGINEERING_SPEC.md).

A new, deterministic implementation: it shares nothing with the retired NLP code. Layers:

  normalize  -> NFKC, micro sign, ohm sign, plus/minus, quotes, whitespace
  clauses    -> sentences split on . ; newlines, commas and "and a/an/the/connect ..."
  scan       -> quantities (SI prefixes, RKM codes such as 4k7 and 3V3), references (R1),
                nets (upper-case names, "ground"), and the remaining lower-case words
  grammar    -> one pattern per clause kind: component, placement, connection, GPIO intent,
                requirement, board, analysis, profile, name
  build      -> a spec_ir.Spec; unknown text is an error, missing decisions are questions

The output is the same IR a hand-written .espec file produces; the frontend never chooses
parts, never invents values, and marks everything it did not read verbatim as inferred or
defaulted.
"""
from __future__ import annotations

import re
import unicodedata

import spec_ir as ir

PREFIX = {'p': 1e-12, 'P': 1e-12, 'n': 1e-9, 'N': 1e-9, 'u': 1e-6, 'U': 1e-6, 'm': 1e-3,
          'k': 1e3, 'K': 1e3, 'M': 1e6, 'G': 1e9}
UNIT = {'ohm': 'ohm', 'ohms': 'ohm', 'f': 'F', 'farad': 'F', 'farads': 'F', 'h': 'H', 'henry': 'H', 'henries': 'H',
        'v': 'V', 'volt': 'V', 'volts': 'V', 'a': 'A', 'amp': 'A', 'amps': 'A', 'ampere': 'A', 'amperes': 'A',
        'w': 'W', 'watt': 'W', 'watts': 'W', 'hz': 'Hz', 'hertz': 'Hz', 's': 's', 'sec': 's', 'second': 's',
        'seconds': 's'}
WHOLE = {'mm': (1e-3, 'm'), 'cm': (1e-2, 'm'), 'mil': (25.4e-6, 'm'), 'mils': (25.4e-6, 'm'), '%': (0.01, '1')}
RKM_UNIT = {'r': 'ohm', 'v': 'V'}

ROLE_WORDS = {
    'resistor': 'resistor', 'capacitor': 'capacitor', 'cap': 'capacitor', 'inductor': 'inductor', 'choke': 'inductor',
    'diode': 'diode', 'led': 'led', 'supply': 'supply', 'power supply': 'supply', 'voltage source': 'supply',
    'battery': 'supply', 'current source': 'current_source', 'connector': 'connector', 'header': 'connector',
    'mcu': 'mcu', 'microcontroller': 'mcu', 'sensor': 'sensor', 'mosfet': 'mosfet', 'transistor': 'bjt', 'bjt': 'bjt',
}
ROLE_ALT = '|'.join(sorted(map(re.escape, ROLE_WORDS), key=len, reverse=True))
FAMILIES = ('esp32', 'stm32', 'rp2040', 'atmega328p')
COLORS = ('red', 'green', 'blue', 'yellow', 'white', 'orange', 'amber')
PACKAGES = ('0201', '0402', '0603', '0805', '1206', '1210', '2512', 'sot-23', 'sod-123', 'sod-323', 'sma', 'smb',
            'to-92', 'to-220', 'soic-8', 'qfn-32')
FILLER = ('smd', 'package', 'in', 'small', 'signal', 'general-purpose', 'standard')
ADJECTIVES = set(COLORS) | set(PACKAGES) | set(FILLER) | {'usb', 'dc'}
RESERVED = {'LED', 'MCU', 'USB', 'DC', 'AC', 'NPN', 'PNP', 'NMOS', 'PMOS', 'PCB', 'SMD', 'IC', 'RKM'} | {f.upper() for f in FAMILIES}
SMALL = {'one': 1, 'two': 2, 'four': 4, 'six': 6}
REF_RE = r'(?<![\w-])[RCLDVIJUQ]\d+(?![\w-])'
POST = r'(?: (?:between|across|from|on|at|driving|in|connected) .*)?'
DECL = r'(?:(?P<ref1>%\w+) is )?(?:(?:an?|one) )?(?P<pre>(?:[#\w][\w.:-]* )*?)'
LEAD = r'^(?:(?:add|use|place|with|plus|also|then|and|there is|there\'s|we need|i need|i want) )*'


def plain(pre):
    """Words before a role that a declaration accepts: values, packages and known adjectives."""
    return all(w.startswith(('#', 'pkg_')) or w in ADJECTIVES for w in pre.split())


class Tok:
    """A scanned quantity: the number with prefix applied, the unit (None when not written)."""
    def __init__(self, raw, value, unit, prefixed=False):
        self.raw, self.value, self.unit, self.prefixed = raw, value, unit, prefixed


def normalize(text):
    text = unicodedata.normalize('NFKC', text)
    for a, b in (('µ', 'u'), ('μ', 'u'), ('Ω', 'ohm'), ('±', ' ± '), ('+/-', ' ± '),
                 ('×', ' x '), ('“', '"'), ('”', '"'), ('‘', "'"), ('’', "'"),
                 ('–', '-'), ('—', '-')):
        text = text.replace(a, b)
    return re.sub(r'[ \t]+', ' ', text).strip()


def clauses(text):
    out = []
    for sentence in re.split(r'[.;](?=\s|$)|\n+', text):
        for part in re.split(r',\s*(?:and\s+|then\s+)?|\s+and\s+(?=(?:an?|one|connect|add|use|place|wire|tie|the|'
                             r'run|simulate)\s|[RCLDVIJUQ]\d+\s)', sentence, flags=re.I):
            part = part.strip(' "\'')
            if part:
                out.append(part)
    return out


def decompose(suffix, spaced):
    """(multiplier, unit or None) for a unit suffix, or None when it is not a unit."""
    low = suffix.lower()
    if low in WHOLE:
        return WHOLE[low]
    if low in UNIT:
        return 1.0, UNIT[low]
    for p in ('meg',):
        if low.startswith(p) and (low[3:] in UNIT or (not low[3:] and not spaced)):
            return 1e6, UNIT.get(low[3:])
    if suffix[0] in PREFIX:
        rest = suffix[1:].lower()
        if rest in UNIT:
            return PREFIX[suffix[0]], UNIT[rest]
        if not rest and not spaced:
            return PREFIX[suffix[0]], None
    if low in ('r',) and not spaced:
        return 1.0, 'ohm'
    return None


QTY = re.compile(r'(?<![\w.-])(?:(?P<a>\d+)(?P<rkm>[rRkKmMuUnNpPvV])(?P<b>\d+)|(?P<num>-?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?'
                 r'|-?(?:[Nn]a[Nn]|[Ii]nf(?:inity)?)(?![A-Za-z]))(?:(?P<sp>\s?)(?P<suf>%|[A-Za-z]+))?)(?![\w.])')


def scan(clause):
    """Shape string of a clause: quantities -> #i, references -> %REF, nets -> @NET, words lower-case."""
    toks = []

    def quantity(m):
        if m.group('rkm'):
            letter = m.group('rkm')
            value = float(f"{m.group('a')}.{m.group('b')}") * (1.0 if letter in 'rRvV' else PREFIX[letter])
            toks.append(Tok(m.group(0), value, RKM_UNIT.get(letter.lower()), letter not in 'rRvV'))
            return f' #{len(toks) - 1} '
        number, suffix = m.group('num'), m.group('suf')
        value = float(number)
        tail = ''
        if suffix:
            parts = decompose(suffix, bool(m.group('sp')))
            if parts is None:
                tail = f"{m.group('sp')}{suffix}"
            else:
                value *= parts[0]
                toks.append(Tok(m.group(0), value, parts[1], parts[0] != 1.0))
                return f' #{len(toks) - 1} '
        toks.append(Tok(number, value, None))
        return f' #{len(toks) - 1} {tail}'

    text = re.sub(r'(?<![\w-])([RCLDVIJUQ]\d+)\.(\w+)', r'\1 pin \2', clause)
    # Chip-size codes are package names, not numbers: 0201..0805 always, 1206/1210/2512 before "package".
    text = re.sub(r'(?<![\w.-])(0201|0402|0603|0805)(?![\w.])|(?<![\w.-])(1206|1210|2512)(?=\s+package\b)',
                  lambda m: f' pkg_{m.group(1) or m.group(2)} ', text)
    text = QTY.sub(quantity, text)
    text = re.sub(REF_RE, lambda m: f' %{m.group(0)} ', text)

    def net(m):
        word = m.group(0)
        if word in RESERVED or re.fullmatch(r'GPIO\d*', word):
            return word.lower()
        return f'@{word}'
    text = re.sub(r'(?<![\w@%#-])[A-Z][A-Z0-9_]*[A-Z0-9_](?![\w-])', net, text)
    words = []
    for w in text.split():
        if w[0] in '@%#':
            words.append(w)
        else:
            w = w.lower().strip(',:()"\'')
            if w in ('ground', 'gnd'):
                w = '@GND'
            if w:
                words.append(w)
    return ' '.join(words), toks


class Frontend:
    def __init__(self, text):
        self.text = text
        self.spec = ir.Spec(source={'kind': 'nlp', 'text': text})
        self.errors = []
        self.declared = set()
        self.auto = {}

    # ------------------------------------------------------------------ helpers
    def error(self, clause, message):
        self.errors.append(f'"{clause}": {message}')

    def new_ref(self, role):
        prefix = ir.ROLES[role]['prefix']
        n = self.auto.get(prefix, 0)
        taken = self.declared | {c.ref for c in self.spec.components}
        while True:
            n += 1
            if f'{prefix}{n}' not in taken:
                self.auto[prefix] = n
                return f'{prefix}{n}'

    def subject(self, phrase, clause):
        """A component named by reference or by 'the <role|family|colour role>'. None (with an error) otherwise."""
        if phrase.startswith('%'):
            ref = phrase[1:]
            if self.spec.component(ref) is None:
                self.error(clause, f'{ref} is not declared')
                return None
            return ref
        words = phrase.split()
        if words and words[0] == 'the':
            words = words[1:]
        key = ' '.join(words)
        color = None
        if words and words[0] in COLORS:
            color, key = words[0], ' '.join(words[1:])
        if key in FAMILIES:
            found = [c for c in self.spec.components if c.attrs.get('family') and c.attrs['family'].value.lower() == key]
        elif key in ROLE_WORDS:
            found = [c for c in self.spec.components if c.role == ROLE_WORDS[key]]
        else:
            self.error(clause, f'"{phrase}" does not name a component')
            return None
        if color:
            found = [c for c in found if c.attrs.get('color') and c.attrs['color'].value == color]
        if len(found) != 1:
            self.error(clause, f'"{phrase}" is ambiguous ({len(found)} matching components); use a reference such as '
                       f'{"/".join(c.ref for c in found) or "R1"}')
            return None
        return found[0].ref

    def node(self, word):
        return word[1:] if word.startswith('@') else None

    def quantity(self, tok, unit, clause, what):
        if tok.unit is None:
            if unit is None:
                self.error(clause, f'{tok.raw}: {what} takes no value')
                return None
            return ir.Quantity(tok.value, unit, 'inferred')
        return ir.Quantity(tok.value, tok.unit)

    def connect(self, ref, terminal, net, provenance='explicit'):
        self.spec.connections.append(ir.Connection(ref, terminal, net, provenance))

    # ------------------------------------------------------------------ grammar
    def parse(self):
        for clause in clauses(normalize(self.text)):
            for ref in re.findall(REF_RE, clause):
                self.declared.add(ref)
        # Pass 1 declares components so that later clauses may refer to them in any order; pass 2 reads the rest.
        rest = []
        for clause in clauses(normalize(self.text)):
            if self.raw_clause(clause):
                continue
            shape, toks = scan(clause)
            if not any(rule(shape, toks, clause) for rule in (self.board, self.layers, self.analysis, self.power,
                                                               self.mcu, self.component)):
                rest.append((shape, toks, clause))
        for shape, toks, clause in rest:
            if not any(rule(shape, toks, clause) for rule in (self.requirement, self.gpio, self.placement, self.connection)):
                self.error(clause, 'not understood; see docs/engineering/ENGINEERING_SPEC.md for the accepted phrases')
        return self.spec, self.errors

    def raw_clause(self, clause):
        m = re.match(r'^(?:the )?(?:project |design |board )?(?:name|called|named|call it)(?: is)?:?\s+(.+)$', clause, re.I)
        if m:
            self.spec.name = ir.Attr(m.group(1).strip('"\' '))
            return True
        m = re.match(r'^use (?:the )?(?:(?P<kind>parts catalogue|parts catalog|catalogue|catalog|dfm rules|dfm profile|dfm)'
                     r')(?: file)?\s+"?(?P<path>[^"]+?)"?$', clause, re.I)
        if m:
            key = 'dfm' if m.group('kind').lower().startswith('dfm') else 'catalogue'
            self.spec.profiles[key] = ir.Attr(m.group('path'))
            return True
        return False

    def component(self, shape, toks, clause):
        m = re.match(LEAD + DECL + rf'(?P<role>{ROLE_ALT})(?: (?P<ref2>%\w+))?(?P<post>{POST})$', shape)
        if not m or not plain(m.group('pre')) or ROLE_WORDS[m.group('role')] == 'supply':
            return False
        return self.declare(ROLE_WORDS[m.group('role')], m, toks, clause, {})

    def mcu(self, shape, toks, clause):
        m = re.match(LEAD + rf'(?:(?P<ref1>%\w+) is )?(?:(?:an?|one) )?(?P<pre>)(?P<family>{"|".join(FAMILIES)})'
                     r'(?P<qual>[\w-]*)(?: (?P<kind>module|board|chip|devkit|dev kit|development board))?'
                     r'(?: (?P<ref2>%\w+))?(?P<post>)$', shape)
        if not m:
            return False
        family = m.group('family')
        attrs = {'family': ir.Attr(family.upper())}
        qual = m.group('qual').strip('-')
        if qual:   # e.g. ESP32-WROOM-32E: names one module; "ESP32 module" alone does not
            raw = re.search(rf'{family}[\w-]*', clause, re.I).group(0).upper()
            attrs['variant'] = ir.Attr(raw)
        return self.declare('mcu', m, toks, clause, attrs)

    def declare(self, role, m, toks, clause, attrs):
        ref = (m.group('ref1') or m.group('ref2') or '')[1:]
        if m.group('ref1') and m.group('ref2'):
            self.error(clause, 'two references for one component')
            return True
        rules = ir.ROLES[role]
        provenance = 'explicit'
        if ref:
            if not ref.startswith(rules['prefix']):
                self.error(clause, f'{ref}: a {role.replace("_", " ")} reference starts with {rules["prefix"]}')
                return True
            if self.spec.component(ref):
                self.error(clause, f'{ref} is declared twice')
                return True
        else:
            ref, provenance = self.new_ref(role), 'defaulted'
        c = ir.Component(ref, role, attrs=dict(attrs), provenance=provenance)
        for word in m.group('pre').split():
            if word.startswith('#'):
                tok = toks[int(word[1:])]
                if tok.unit == '1':
                    if c.tolerance:
                        self.error(clause, 'two tolerances')
                    c.tolerance = ir.Quantity(tok.value, '1')
                elif c.value:
                    self.error(clause, f'two values for {ref} ({c.value.value:g} and {tok.raw})')
                else:
                    c.value = self.quantity(tok, rules['unit'], clause, f'a {role.replace("_", " ")}')
            elif word in COLORS and role == 'led':
                c.attrs['color'] = ir.Attr(word)
            elif word in PACKAGES or word.startswith('pkg_'):
                c.attrs['package'] = ir.Attr(word.removeprefix('pkg_').upper())
            elif word in FILLER or (role == 'supply' and word in ('usb', 'dc')):
                continue
            else:
                self.error(clause, f'"{word}" is not a known attribute of a {role.replace("_", " ")}')
        self.spec.components.append(c)
        post = m.group('post').strip()
        if post:
            self.place(ref, role, post, clause)
        return True

    def place(self, ref, role, post, clause):
        """'between A and B', 'from A to B', 'on A' after a component."""
        pkg = re.match(r'^(?:in (?:an? )?)?(\S+) package(?: (.*))?$', post)
        if pkg and (pkg.group(1) in PACKAGES or pkg.group(1).startswith('pkg_')):
            self.spec.component(ref).attrs['package'] = ir.Attr(pkg.group(1).removeprefix('pkg_').upper())
            post = (pkg.group(2) or '').strip()
            if not post:
                return
        polar = ir.ROLES[role]['terminals'] in (ir.POLARIZED, ir.SOURCE_PINS)
        m = re.match(r'^(?:connected )?(?:between|across) (@\w+) and (@\w+)$', post)
        if m:
            a, b = self.node(m.group(1)), self.node(m.group(2))
            if role in ('diode', 'led'):
                self.spec.unresolved.append(ir.Question('', f'{ref}.polarity', f'{ref} is between {a} and {b}: which net '
                                                        'is its anode on? (say "from ANODE_NET to CATHODE_NET")', [a, b]))
                return
            self.two(ref, role, a, b, 'inferred' if polar else 'explicit')
            return
        m = re.match(r'^(?:connected )?from (@\w+) to (@\w+)$', post)
        if m:
            self.two(ref, role, self.node(m.group(1)), self.node(m.group(2)), 'inferred' if polar else 'explicit')
            return
        m = re.match(r'^(?:connected )?(?:on|at|to|driving) (@\w+)$', post)
        if m and role in ('supply', 'current_source'):
            self.connect(ref, '1', self.node(m.group(1)))
            self.connect(ref, '2', 'GND', 'inferred')
            return
        self.error(clause, f'cannot read the connection "{post}" (use "between NET and NET" or "from NET to NET")')

    def two(self, ref, role, a, b, provenance):
        """First net on terminal 1, second on 2; for a diode/LED "from A to B" puts the anode on A."""
        if role in ('diode', 'led'):
            a, b = b, a
        self.connect(ref, '1', a, provenance)
        self.connect(ref, '2', b, provenance)

    def power(self, shape, toks, clause):
        m = re.match(LEAD + DECL + r'(?P<role>power supply|voltage source|supply|battery|input|power input)'
                     rf'(?: (?P<ref2>%\w+))?(?P<post>{POST})$', shape)
        # "input" alone is not a supply; "a 5 V input" is.
        if m and plain(m.group('pre')) and ('input' not in m.group('role') or m.group('pre').strip()):
            pre = m.group('pre').split()
            if 'usb' in pre and not any(w.startswith('#') for w in pre):
                self.ask_power(clause)
                return True
            return self.declare('supply', m, toks, clause, {})
        m = re.match(r'^(?:it is |the board is |the circuit is )?powered (?:by|from) (?P<what>.+)$', shape)
        if m:
            what = m.group('what')
            q = re.fullmatch(r'(?:an? )?#(\d+)(?: (?:supply|source|input))? (?:on|at) (@\w+)', what)
            if q:
                tok = toks[int(q.group(1))]
                ref = self.new_ref('supply')
                self.spec.components.append(ir.Component(ref, 'supply', self.quantity(tok, 'V', clause, 'a supply'),
                                                         provenance='defaulted'))
                self.connect(ref, '1', self.node(q.group(2)))
                self.connect(ref, '2', 'GND', 'inferred')
            else:
                self.ask_power(clause)
            return True
        return False

    def ask_power(self, clause):
        self.spec.unresolved.append(ir.Question('', 'power.input', f'"{clause}" does not say the supply voltage and '
                                                'the net it drives. State it, e.g. "a 5 V supply on VIN".',
                                                ['a 5 V supply on VIN', 'a 3.3 V supply on VDD']))

    def placement(self, shape, toks, clause):
        m = re.match(r'^(?:place |put |connect )?(?P<subj>%\w+|the(?: \w+){1,2}?) (?:is )?(?P<post>(?:connected )?'
                     r'(?:between|across|from|on) .+)$', shape)
        if not m:
            return False
        ref = self.subject(m.group('subj'), clause)
        if ref:
            self.place(ref, self.spec.component(ref).role, m.group('post'), clause)
        return True

    TERM = r'(?P<subj>%\w+|the(?: \w+){1,2}?)(?: pin| terminal|\'s)? (?P<pin>#\d+|[\w+-]+)'

    def connection(self, shape, toks, clause):
        m = (re.match(r'^(?:connect |wire |tie |join )?' + self.TERM + r' (?:to|is connected to|connects to|goes to|'
                      r'is on|on|->) (?P<net>\S+)$', shape)
             or re.match(r'^(?:connect |wire |tie )(?P<net>\S+) to ' + self.TERM + '$', shape))
        if not m:
            return False
        net = m.group('net')
        if not net.startswith('@'):
            self.error(clause, f'"{net}" is not a net name (nets are upper-case names such as VIN, or "ground")')
            return True
        ref = self.subject(m.group('subj'), clause)
        if ref:
            pin = m.group('pin')
            pin = toks[int(pin[1:])].raw if pin.startswith('#') else pin.upper() if pin.startswith('gpio') else pin
            self.connect(ref, pin, self.node(net))
        return True

    def gpio(self, shape, toks, clause):
        pin = r'(?:gpio ?(?P<n>#\d+)|(?P<g>gpio\d+)|pin (?P<p>[#\w]+))'
        m = (re.match(rf'^(?:(?P<mcu>%\w+|the \w+)(?:\'s)? )?{pin} (?:drives|controls|switches|blinks|lights|turns on)'
                      r' (?P<load>%\w+|the(?: \w+){1,2}?)$', shape)
             or re.match(r'^(?:drive|control|switch|blink|light) (?P<load>%\w+|the(?: \w+){1,2}?) (?:from|with|using|on) '
                         rf'(?:(?P<mcu>%\w+|the \w+)(?:\'s)? )?{pin}$', shape))
        if not m:
            return False
        if m.group('n'):
            name = 'GPIO' + toks[int(m.group('n')[1:])].raw
        elif m.group('g'):
            name = m.group('g').upper()
        else:
            p = m.group('p')
            name = toks[int(p[1:])].raw if p.startswith('#') else p.upper()
        load = self.subject(m.group('load'), clause)
        if m.group('mcu'):
            mcu = self.subject(m.group('mcu'), clause)
        else:
            mcus = [c.ref for c in self.spec.components if c.role == 'mcu']
            if not mcus:
                self.error(clause, f'{name} drives a load but no microcontroller is declared')
                return True
            if len(mcus) > 1:
                self.spec.unresolved.append(ir.Question('', f'gpio.{name}', f'Which microcontroller drives {name}?', mcus))
                return True
            mcu = mcus[0]
        if mcu and load:
            self.spec.gpio.append(ir.Gpio(mcu, name, load))
        return True

    def bound(self, rest, toks, unit, clause):
        def q(i):
            return self.quantity(toks[int(i)], unit, clause, 'a requirement')
        m = re.fullmatch(r'(?:within |in the range )?(?:between |from )?#(\d+) (?:and|to) #(\d+)', rest)
        if m:
            return q(m.group(1)), q(m.group(2))
        m = re.fullmatch(r'#(\d+) ± #(\d+)', rest)
        if m:
            centre, tol = q(m.group(1)), toks[int(m.group(2))]
            if centre is None:
                return None
            delta = tol.value * abs(centre.value) if tol.unit == '1' else tol.value
            if tol.unit not in ('1', unit, None):
                self.error(clause, f'tolerance {tol.raw} must be a percentage or in {unit}')
                return None
            p = 'inferred' if centre.provenance == 'inferred' or tol.unit is None else 'explicit'
            return ir.Quantity(centre.value - delta, unit, p), ir.Quantity(centre.value + delta, unit, p)
        m = re.fullmatch(r'(?P<op>at least|above|over|more than|greater than|at most|below|under|less than|no more than)'
                         r' #(\d+)', rest)
        if m:
            low = m.group('op') in ('at least', 'above', 'over', 'more than', 'greater than')
            return (q(m.group(2)), None) if low else (None, q(m.group(2)))
        m = re.fullmatch(r'(?:exactly )?#(\d+)', rest)
        if m:
            v = q(m.group(1))
            return v, v
        return None

    def requirement(self, shape, toks, clause):
        m = re.match(r'^(?:the )?(?:voltage (?:at|on|of) )?(?P<net>@\w+)(?: voltage)? (?:must|should|shall|has to|needs to)'
                     r' (?:be|stay|remain)(?: kept)? (?P<rest>.+)$', shape)
        kind = 'voltage'
        if not m:
            m = re.match(r'^(?:the )?current (?:through|in|of) (?P<subj>%\w+|the(?: \w+){1,2}?) (?:must|should|shall)'
                         r' (?:be |stay |remain )?(?P<rest>.+)$', shape)
            kind = 'current'
        if not m:
            return False
        bounds = self.bound(m.group('rest'), toks, 'V' if kind == 'voltage' else 'A', clause)
        if bounds is None:
            self.error(clause, 'cannot read the bound (use "between X and Y", "X ± Y", "at least X" or "at most X")')
            return True
        subject = self.node(m.group('net')) if kind == 'voltage' else self.subject(m.group('subj'), clause)
        if subject and None not in (bounds[0], bounds[1]) or (subject and any(bounds)):
            self.spec.requirements.append(ir.Requirement(kind, subject, bounds[0], bounds[1]))
        return True

    def lengths(self, a, b, clause):
        if a.unit is None and b.unit is None:
            self.error(clause, 'board size needs a length unit (mm)')
            return None
        unit_tok = b if b.unit else a
        if unit_tok.unit != 'm' or (a.unit not in (None, 'm')) or (b.unit not in (None, 'm')):
            self.error(clause, 'board size must be a length (mm)')
            return None
        scale = unit_tok.value / float(re.match(r'-?[\d.]+(?:[eE][+-]?\d+)?', unit_tok.raw).group(0))
        out = []
        for t in (a, b):
            out.append(ir.Quantity(t.value, 'm') if t.unit else ir.Quantity(t.value * scale, 'm', 'inferred'))
        return out

    def board(self, shape, toks, clause):
        m = re.match(r'^(?:the |a )?(?:board|pcb)(?: is| size| size is| of| measures| outline)? #(\d+) (?:x|by) #(\d+)'
                     r'(?: (?:with |and )?(?P<l>#\d+|one|two|four|six) ?-?layers?)?$', shape)
        if not m:
            return False
        dims = self.lengths(toks[int(m.group(1))], toks[int(m.group(2))], clause)
        if dims:
            self.spec.board['width'], self.spec.board['height'] = dims
        if m.group('l'):
            self.set_layers(m.group('l'), toks)
        return True

    def layers(self, shape, toks, clause):
        m = re.match(r'^(?:the |a )?(?:board |pcb )?(?:has |uses |with |is )?(?:an? )?(?P<l>#\d+|one|two|four|six) ?-?'
                     r'layers?(?: board| pcb)?$', shape)
        if not m:
            return False
        self.set_layers(m.group('l'), toks)
        return True

    def set_layers(self, word, toks):
        value = toks[int(word[1:])].value if word.startswith('#') else SMALL[word]
        self.spec.board['layers'] = ir.Quantity(value, '1')

    def analysis(self, shape, toks, clause):
        verb = r'^(?:(?:run|simulate|perform|do|check) )?(?:(?:an?|the) )?'
        if re.match(verb + r'(?:dc|operating point|dc operating point)(?: analysis| simulation| solve| operating point)?$', shape):
            self.spec.analyses.append(ir.Analysis('dc'))
            return True
        m = re.match(verb + r'ac(?: analysis| simulation)?(?: sweep)? from #(\d+) to #(\d+)'
                     r'(?: (?:with|in|at|using) #(\d+) points?)?$', shape)
        if m:
            a = ir.Analysis('ac', {'start': self.quantity(toks[int(m.group(1))], 'Hz', clause, 'a frequency'),
                                   'stop': self.quantity(toks[int(m.group(2))], 'Hz', clause, 'a frequency')})
            if m.group(3):
                a.params['points'] = self.quantity(toks[int(m.group(3))], '1', clause, 'a point count')
            if all(a.params.values()):
                self.spec.analyses.append(a)
            return True
        method = r'(?: (?:using|with) (?P<m>backward euler|be|trapezoidal|trap)(?: integration| method)?)?$'
        m = (re.match(verb + r'transient(?: analysis| simulation)?(?: (?:with|at|using))?(?: an?)?(?: time)? (?:step|dt|'
                      r'timestep) (?:of )?#(?P<s>\d+) (?:for|until|to|up to|stopping at) #(?P<t>\d+)' + method, shape)
             or re.match(verb + r'transient(?: analysis| simulation)? (?:for|until|to) #(?P<t>\d+) (?:with|at|using)'
                         r'(?: an?)?(?: time)? (?:step|dt|timestep) (?:of )?#(?P<s>\d+)' + method, shape))
        if m:
            a = ir.Analysis('transient', {'step': self.quantity(toks[int(m.group('s'))], 's', clause, 'a time step'),
                                          'stop': self.quantity(toks[int(m.group('t'))], 's', clause, 'a stop time')})
            if m.group('m'):
                a.params['method'] = ir.Attr('trap' if m.group('m').startswith('trap') else 'be')
            if all(a.params.values()):
                self.spec.analyses.append(a)
            return True
        return False


def parse(text):
    """Text -> (Spec, parse errors). validate() adds schema errors and clarification questions."""
    if not isinstance(text, str) or not text.strip():
        return ir.Spec(source={'kind': 'nlp', 'text': text or ''}), ['the description is empty']
    spec, errors = Frontend(text).parse()
    for i, q in enumerate(spec.unresolved, 1):
        q.id = f'p{i}'
    return spec, errors
