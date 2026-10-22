"""Intent to a verified KiCad board: the deterministic design compiler (docs/engineering/DESIGN.md).

    text --spec_nlp--> intent EngineeringSpec --decisions--> complete EngineeringSpec --lower--> .copper
         --layout.optimize--> placed .copper --export--> KiCad project --verify--> 11 quality gates

Nothing here is specific to one sentence. A decision comes from the user's text, an explicit answer, the
design profile (documented project defaults) or a named design rule, and every one is reported with its
source. A decision none of these settles is a clarification question, and the run stops. The expansion uses
catalogue data only: the module's pins, its reference circuit and its programming pins, and the LED's model.
copper-core judges the electrical design and every candidate board; KiCad 9.x judges the native files.
"""
from __future__ import annotations

import argparse, contextlib, copy, hashlib, io, json, math, re, shutil, subprocess, tempfile
from pathlib import Path

LED_FUNCTIONS = ('gpio_indicator', 'external_load', 'addressable', 'driver')
SUPPORTED_LED_FUNCTIONS = ('gpio_indicator',)
PROGRAMMING = ('uart_header', 'none')
RULE_DEFAULTS = {'resistor_series': ('E24', 'standard 5% resistor series (IEC 60063)'),
                 'seeds': ('1', 'one deterministic annealing seed'), 'iterations': ('300', 'bounded annealing effort')}
GATES = ('spec', 'topology', 'parts', 'models', 'electrical', 'routing', 'internal_dfm', 'kicad_erc_drc',
         'connectivity', 'manifest', 'determinism')


class Stop(Exception):
    """The run cannot continue: status, message and (for questions) the decisions so far."""
    def __init__(self, status, message, decisions=None):
        super().__init__(message); self.status = status; self.decisions = decisions


# --------------------------------------------------------------------------- inputs
def load_profile(path):
    import copper
    from hwc_kicad import head, one, direct, text_at
    path = Path(copper.existing(path, 'Design profile')).resolve(); tree = copper.load(path)
    if head(tree) != 'design_profile' or text_at(tree, 'version') != '1':
        raise copper.InvalidInput(f'{path}: expected (design_profile (version 1) ...)')
    answers = {}
    for node in (one(tree, 'answers') or [None])[1:]:
        if not isinstance(node, list) or len(node) != 2:
            raise copper.InvalidInput(f'{path}: answers are (KEY VALUE)')
        answers[str(node[0])] = str(node[1])
    return dict(path=path, name=text_at(tree, 'name') or path.stem, provenance=text_at(tree, 'provenance'),
                catalogues=[path.parent / str(c) for c in (one(tree, 'catalogues') or [None])[1:]],
                dfm=(path.parent / text_at(tree, 'dfm')) if text_at(tree, 'dfm') else None, answers=answers)


def parse_answers(text):
    import copper
    out = {}
    for item in filter(None, (t.strip() for t in (text or '').split(','))):
        key, sep, value = item.partition('=')
        if not sep or not key.strip() or not value.strip():
            raise copper.InvalidInput(f'--answers takes key=value pairs separated by commas, got {item!r}')
        out[key.strip()] = value.strip()
    return out


class Decisions:
    """Every choice the compiler makes, with its source; unsettled ones become questions."""
    def __init__(self, answers, profile):
        self.answers, self.profile = answers, profile; self.items = []; self.questions = []; self.used = set()

    def get(self, key):
        if key in self.answers:
            self.used.add(key); return self.answers[key], 'answer', 'given with --answers'
        if self.profile and key in self.profile['answers']:
            return self.profile['answers'][key], 'profile', f"design profile {self.profile['name']}"
        if key in RULE_DEFAULTS:
            value, why = RULE_DEFAULTS[key]; return value, 'rule', why
        return None, None, None

    def decide(self, field, value, source, reason):
        self.items.append(dict(field=field, value=value, source=source, reason=reason)); return value

    def ask(self, field, text, options=()):
        if not any(q['field'] == field for q in self.questions):
            self.questions.append(dict(id=f'q{len(self.questions) + 1}', field=field, text=text, options=list(options),
                                       answer_with=f'--answers {field}=VALUE'))
        return None

    def take(self, key, field, text, options=(), convert=str):
        value, source, why = self.get(key)
        if value is None:
            return self.ask(key, text, options)
        try:
            converted = convert(value)
        except (TypeError, ValueError):
            raise Stop('INVALID_INPUT', f'{key}={value!r} ({why}) is not valid: {text}') from None
        self.decide(field, value, source, why); return converted


def positive(text):
    value = float(text)
    if not (math.isfinite(value) and value > 0):
        raise ValueError(text)
    return value


# --------------------------------------------------------------------------- catalogue helpers
BUNDLED_CATALOGUES = ('mcu_modules.parts', 'indicator.parts')


def merged_catalogue(paths, out):
    """The design's catalogues merged into one file (later files win), validated by copper-core."""
    import copper
    from hwc_kicad import direct
    merged, order = {}, []
    for path in paths:
        for part in direct(copper.load(copper.existing(path, 'Catalogue')), 'part'):
            if str(part[1]) not in merged:
                order.append(str(part[1]))
            merged[str(part[1])] = part
    target = out / 'catalogue.parts'
    copper.write_tree(target, copper.sx('catalogue', *[merged[i] for i in order]))
    copper.validate_catalogue(target, label='merged design catalogue')
    return target


def part_mcu(part):
    from hwc_kicad import one
    return one(part, 'mcu')


def pins_of(mcu, key):
    from hwc_kicad import one
    return [str(p) for p in (one(mcu, key) or [None])[1:]]


def symbol_pin_names(part, symdir):
    import copper
    from hwc_kicad import text_at
    if symdir is None:
        raise copper.ToolUnavailable('KiCad symbol libraries are needed to read the module pins: install KiCad 9.x, set '
                                     'KICAD_SYMBOL_DIR or pass --symbols')
    return {text_at(p, 'number'): text_at(p, 'name') for p in copper.symbol_pins(copper.resolve_symbol(symdir, text_at(part, 'symbol'), {}))}


def net_name(name, fallback):
    clean = re.sub(r'[^A-Za-z0-9_]+', '_', name or '').strip('_')
    return clean if clean and clean[0].isalpha() and clean not in ('GND',) else fallback


def led_forward_voltage(part, current):
    """Vf at `current` from the LED part's own Shockley parameters (the model copper-core solves)."""
    from hwc_kicad import text_at
    isat = float(text_at(part, 'is_a') or 1e-14); n = float(text_at(part, 'ideality') or 1.0)
    vt = 1.380649e-23 * 300.0 / 1.602176634e-19
    return n * vt * math.log(current / isat + 1.0)


def eseries_at_least(series, value, core):
    import copper
    r = subprocess.run([copper.core_path(core), 'eseries', series, repr(value)], capture_output=True, text=True, timeout=30)
    if r.returncode:
        raise copper.InvalidInput(r.stderr.strip() or 'copper-core eseries failed')
    return json.loads(r.stdout)['at_least']


# --------------------------------------------------------------------------- compile: intent -> complete spec
def compile_spec(text, profile, answers, symdir, out, core):
    """Returns (intent, complete spec, decisions, notes, catalogue path). Raises Stop for questions and errors."""
    import copper, spec_ir, spec_nlp
    from hwc_kicad import one, text_at, direct
    intent, parse_errors = spec_nlp.parse(text)
    errors, _ = spec_ir.validate(copy.deepcopy(intent))
    if parse_errors or errors:
        raise Stop('INVALID_INPUT', '; '.join(parse_errors + errors))
    d = Decisions(answers, profile); notes = list(intent.source.get('notes', []))
    if 'catalogues' in answers:
        paths = [Path(p) for p in answers['catalogues'].split(';') if p]; d.used.add('catalogues')
        d.decide('catalogues', answers['catalogues'], 'answer', 'given with --answers')
    elif profile and profile['catalogues']:
        paths = profile['catalogues']; d.decide('catalogues', ';'.join(p.name for p in paths), 'profile', f"design profile {profile['name']}")
    else:
        import copper
        paths = [copper.ROOT / 'profiles' / n for n in BUNDLED_CATALOGUES]
        d.decide('catalogues', ';'.join(BUNDLED_CATALOGUES), 'rule', 'no profile: the bundled module and indicator catalogues')
    catalogue = merged_catalogue(paths, out)
    parts = {str(p[1]): p for p in copper.catalogue(str(catalogue))}
    spec = copy.deepcopy(intent); spec.source = {'kind': 'compiled', 'text': text}; spec.unresolved = []

    mcus = [c for c in spec.components if c.role == 'mcu']; leds = [c for c in spec.components if c.role == 'led']
    others = [c for c in spec.components if c.role not in ('mcu', 'led', 'supply')]
    if len(mcus) != 1 or not leds:
        raise Stop('NOT_IMPLEMENTED', 'the design rules cover one microcontroller with GPIO indicator LEDs; this request has '
                   f'{len(mcus)} MCU(s) and {len(leds)} LED(s)')
    unconnected = [c.ref for c in others if not any(k.ref == c.ref for k in spec.connections)]
    if unconnected:
        raise Stop('NOT_IMPLEMENTED', f'no design rule decides how {", ".join(unconnected)} connect; state the connections')
    u = mcus[0]

    # -- the module: an exact part, never inferred from a family name
    candidates = sorted(i for i, p in parts.items() if part_mcu(p) and (not u.attrs.get('family') or text_at(p, 'family').lower() == u.attrs['family'].value.lower()))
    variant = u.attrs['variant'].value if 'variant' in u.attrs else None
    if 'part' in u.attrs:
        mcu_id = d.decide(f'{u.ref}.part', u.attrs['part'].value, 'text', 'named in the request')
    elif variant and [i for i in candidates if text_at(parts[i], 'mpn').upper() == variant.upper()]:
        mcu_id = d.decide(f'{u.ref}.part', next(i for i in candidates if text_at(parts[i], 'mpn').upper() == variant.upper()),
                          'catalogue', f'the only catalogue part with MPN {variant}')
    else:
        mcu_id = d.take('mcu_part', f'{u.ref}.part', f'Which {u.attrs["family"].value if "family" in u.attrs else "MCU"} module is '
                        f'{u.ref}? A family name does not identify a part, its pins or its footprint.', candidates)
    if mcu_id is not None and mcu_id not in candidates:
        raise Stop('INVALID_INPUT', f'{mcu_id} is not a catalogue MCU part{" of family " + u.attrs["family"].value if "family" in u.attrs else ""} '
                   f'(catalogue MCUs: {", ".join(candidates) or "none"})')

    # -- LEDs: what the LED is for, which part, how much current
    led_parts = sorted(i for i, p in parts.items() if text_at(p, 'role') == 'indicator_led')
    function = d.take('led_function', 'led.function', 'Is the LED a simple GPIO indicator, an external load, an addressable LED or '
                      'a separately driven LED?', LED_FUNCTIONS)
    if function is not None and function not in LED_FUNCTIONS:
        raise Stop('INVALID_INPUT', f'led_function must be one of {", ".join(LED_FUNCTIONS)}')
    if function is not None and function not in SUPPORTED_LED_FUNCTIONS:
        raise Stop('NOT_IMPLEMENTED', f'led_function {function}: only gpio_indicator has a design rule so far')
    led_choice = {}
    for led in leds:
        pid = led.attrs['part'].value if 'part' in led.attrs else d.take('led_part', f'{led.ref}.part', 'Which LED part?', led_parts)
        if pid is not None and pid not in led_parts:
            raise Stop('INVALID_INPUT', f'{pid} is not an indicator LED in the catalogue ({", ".join(led_parts) or "none"})')
        if pid and 'color' in led.attrs and led.attrs['color'].value not in pid:
            raise Stop('INVALID_INPUT', f'{led.ref} is requested {led.attrs["color"].value}, but part {pid} does not say that colour')
        current = next((r.maximum.value for r in spec.requirements if r.kind == 'current' and r.subject == led.ref and r.maximum), None)
        if current is not None:
            d.decide(f'{led.ref}.current_a', current, 'text', 'requested maximum current')
        else:
            current = d.take('led_current', f'{led.ref}.current_a', 'What LED current (A) should the GPIO drive?', convert=positive)
        led_choice[led.ref] = (pid, current)
    spec.requirements = [r for r in spec.requirements if not (r.kind == 'current' and r.subject in led_choice)]

    # -- supply, programming, board, DFM
    supplies = [c for c in spec.components if c.role == 'supply']
    supply_kind = None; volts = None
    if supplies:
        supply_kind = d.decide('supply', 'from the request', 'text', 'a supply is stated')
    else:
        supply_kind = d.take('supply_kind', 'supply.kind', 'How does power enter the board, and at what voltage?', ('header',))
        volts = d.take('supply_voltage', 'supply.voltage_v', 'What is the supply voltage (V)?', convert=positive)
        if supply_kind is not None and supply_kind != 'header':
            raise Stop('NOT_IMPLEMENTED', f'supply_kind {supply_kind}: only power through the programming/power header has a design rule')
    programming = d.take('programming', 'programming', 'How is the module programmed?', PROGRAMMING)
    if programming is not None and programming not in PROGRAMMING:
        raise Stop('INVALID_INPUT', f'programming must be one of {", ".join(PROGRAMMING)}')
    if supply_kind == 'header' and programming == 'none':
        raise Stop('NOT_IMPLEMENTED', 'power through the header needs the header: choose programming=uart_header or state a supply')
    headers = sorted(i for i, p in parts.items() if text_at(p, 'role') == 'programming_header')
    header_id = d.take('programming_part', 'programming.part', 'Which header part carries power and programming?', headers) if programming == 'uart_header' else None
    for axis in ('width', 'height'):
        if axis in spec.board:
            d.decide(f'board.{axis}_mm', spec.board[axis].value * 1000, 'text', 'requested')
        else:
            v = d.take(f'board_{axis}_mm', f'board.{axis}_mm', f'What is the board {axis} (mm)?', convert=positive)
            if v is not None:
                spec.board[axis] = spec_ir.Quantity(v / 1000, 'm', 'defaulted')
    layers = spec.board['layers'].value if 'layers' in spec.board else d.take('layers', 'board.layers', 'How many copper layers?', convert=positive)
    if layers is not None:
        spec.board['layers'] = spec_ir.Quantity(float(layers), '1', 'defaulted' if 'layers' not in intent.board else 'explicit')
    if 'dfm' in spec.profiles:
        dfm = d.decide('dfm', spec.profiles['dfm'].value, 'text', 'named in the request')
    elif 'dfm' in answers:
        d.used.add('dfm'); dfm = d.decide('dfm', answers['dfm'], 'answer', 'given with --answers')
    elif profile and profile['dfm']:
        dfm = d.decide('dfm', str(profile['dfm']), 'profile', f"design profile {profile['name']}")
    else:
        dfm = d.ask('dfm', 'Which fabrication/DFM rule profile (a (rules ...) file)?', ['profiles/conservative.rules', 'profiles/esp32_2layer.rules'])
    series = d.take('resistor_series', 'resistor.series', 'Which resistor series (E24 or E96)?', ('E24', 'E96'))
    unknown = sorted(set(answers) - d.used - {'seeds', 'iterations'})
    if unknown:
        raise Stop('INVALID_INPUT', f'unknown answer key(s): {", ".join(unknown)}')
    # Contradictions that catalogue data alone shows are reported before any library is read.
    if mcu_id is not None:
        early = part_mcu(parts[mcu_id]); lo, hi = (float(x) for x in one(early, 'operating_v')[1:3])
        stated = supplies[0].value.value if supplies and supplies[0].value else volts
        if stated is not None and not lo <= stated <= hi:
            raise Stop('INVALID_INPUT', f'a {stated:g} V supply is outside {mcu_id} operating range {lo:g}..{hi:g} V')
        limit = float(text_at(early, 'max_source_a') or 'nan')
        for led_ref, (_, current) in led_choice.items():
            if current is not None and math.isfinite(limit) and current > limit:
                raise Stop('INVALID_INPUT', f'{led_ref}: {current * 1e3:g} mA exceeds the {limit * 1e3:g} mA per-pin limit of {mcu_id}')
    if d.questions:
        raise Stop('UNVERIFIED', f'{len(d.questions)} decision(s) need an answer', d)

    # -- expansion: everything below comes from catalogue data and named rules
    mcu_part = parts[mcu_id]; mcu = part_mcu(mcu_part); names = symbol_pin_names(mcu_part, symdir)
    taken = {c.ref for c in spec.components}

    def ref(prefix):
        n = 1
        while f'{prefix}{n}' in taken:
            n += 1
        taken.add(f'{prefix}{n}'); return f'{prefix}{n}'

    def add(role, value=None, part=None, why=''):
        r = ref(spec_ir.ROLES[role]['prefix'])
        q = None if value is None else spec_ir.Quantity(value, spec_ir.ROLES[role]['unit'], 'inferred')
        c = spec_ir.Component(r, role, q, provenance='inferred')
        if part:
            c.attrs['part'] = spec_ir.Attr(part, 'catalogue-derived')
        spec.components.append(c); d.decide(f'{r}', f'{role}{"" if value is None else f" {value:g}"}', 'rule', why); return r

    def connect(r, terminal, net):
        spec.connections.append(spec_ir.Connection(r, terminal, net, 'inferred'))

    vdd, gnd = 'VDD', 'GND'
    u.attrs['part'] = spec_ir.Attr(mcu_id, 'catalogue-derived')
    for p in pins_of(mcu, 'supply'):
        connect(u.ref, p, vdd)
    for p in pins_of(mcu, 'ground'):
        connect(u.ref, p, gnd)
    d.decide(f'{u.ref}.power', f'supply pins {pins_of(mcu, "supply")} -> {vdd}, ground pins {pins_of(mcu, "ground")} -> {gnd}', 'catalogue',
             f'{mcu_id} mcu supply/ground pins')
    vmin, vmax = (float(x) for x in one(mcu, 'operating_v')[1:3])
    if supplies:
        vdd = next(k.net for k in spec.connections if k.ref == supplies[0].ref and k.terminal == '1')
        volts = supplies[0].value.value
        spec.connections = [k if not (k.ref == u.ref and k.net == 'VDD') else spec_ir.Connection(k.ref, k.terminal, vdd, k.provenance) for k in spec.connections]
    else:
        v1 = add('supply', volts, None, 'the external supply on the header, modelled as an ideal source for DC')
        connect(v1, '1', vdd); connect(v1, '2', gnd)
    if not vmin <= volts <= vmax:
        raise Stop('INVALID_INPUT', f'a {volts:g} V supply is outside {mcu_id} operating range {vmin:g}..{vmax:g} V')
    reset = text_at(mcu, 'reset')
    pin_net = {}

    def mcu_net(pin):
        if pin not in pin_net:
            pin_net[pin] = net_name(names.get(pin), f'{u.ref}_P{pin}')
            connect(u.ref, pin, pin_net[pin])
        return pin_net[pin]

    reference = one(mcu, 'reference')
    for entry in (reference or [None])[1:]:
        kind = str(entry[0])
        if kind == 'provenance':
            continue
        pin = str(entry[1]); why = f'{mcu_id} reference circuit: {text_at(reference, "provenance")}'
        for value in (float(v) for v in entry[2:]):
            if kind == 'decouple':
                c = add('capacitor', value, None, f'decoupling on supply pin {pin}; {why}'); connect(c, '1', vdd); connect(c, '2', gnd)
            elif kind == 'pull_up':
                r = add('resistor', value, None, f'pull-up on pin {pin} ({names.get(pin, pin)}); {why}'); connect(r, '1', vdd); connect(r, '2', mcu_net(pin))
            elif kind == 'cap_to_ground':
                c = add('capacitor', value, None, f'capacitor to ground on pin {pin} ({names.get(pin, pin)}); {why}'); connect(c, '1', mcu_net(pin)); connect(c, '2', gnd)
    if not reference:
        notes.append(f'{mcu_id} has no reference circuit in the catalogue: no decoupling or reset network was added')
    program = pins_of(mcu, 'program')
    if programming == 'uart_header':
        header = parts[header_id]; hpins = sorted(symbol_pin_names(header, symdir), key=lambda p: [int(t) if t.isdigit() else t for t in re.split(r'(\d+)', p)])
        if len(hpins) != 2 + len(program):
            raise Stop('INVALID_INPUT', f'{header_id} has {len(hpins)} pins; power (2) and {mcu_id} programming pins ({len(program)}) need {2 + len(program)}')
        j = add('connector', None, header_id, f'power and programming header: {vdd}, {gnd}, then {mcu_id} programming pins {program}')
        connect(j, hpins[0], vdd); connect(j, hpins[1], gnd)
        for hp, mp in zip(hpins[2:], program):
            connect(j, hp, mcu_net(mp))
    elif programming == 'none':
        notes.append('no programming header: the module cannot be programmed in circuit (mcu_programming findings)')

    # -- GPIO indicator rule: GPIO -> series resistor -> LED anode; cathode -> GND
    eligible = [g for g in direct(one(mcu, 'gpio'), 'pin') if 'out' in [str(x) for x in g[3:]]
                and str(g[1]) not in {str(s[0]) for s in (one(mcu, 'straps') or [None])[1:]} and str(g[1]) not in program and str(g[1]) != reset]
    eligible.sort(key=lambda g: int(str(g[1])) if str(g[1]).isdigit() else 10 ** 6)
    max_source = float(text_at(mcu, 'max_source_a') or 'nan')
    for led in leds:
        pid, current = led_choice[led.ref]; led.attrs['part'] = spec_ir.Attr(pid, 'catalogue-derived')
        intent_gpio = next((g for g in spec.gpio if g.load == led.ref), None)
        if intent_gpio:
            pin = spec_ir.gpio_pin(mcu_part, intent_gpio.pin)
            if pin is None:
                raise Stop('INVALID_INPUT', f'{intent_gpio.pin} is not a GPIO of {mcu_id}')
            entry = next(g for g in direct(one(mcu, 'gpio'), 'pin') if str(g[1]) == pin)
            if 'out' not in [str(x) for x in entry[3:]]:
                raise Stop('INVALID_INPUT', f'{intent_gpio.pin} (pin {pin}) of {mcu_id} is input-only and cannot drive {led.ref}')
            if pin in pin_net:
                raise Stop('INVALID_INPUT', f'{intent_gpio.pin} (pin {pin}) of {mcu_id} is already used ({pin_net[pin]})')
            d.decide(f'{led.ref}.gpio', intent_gpio.pin, 'text', 'requested')
            strap = next((str(x[1]) for x in (one(mcu, 'straps') or [None])[1:] if str(x[0]) == pin), None)
            if strap:
                notes.append(f'{intent_gpio.pin} is a strapping pin of {mcu_id} ({strap}); the LED and its resistor load it at reset')
        else:
            free = [g for g in eligible if str(g[1]) not in pin_net]
            if not free:
                raise Stop('INVALID_INPUT', f'no free output GPIO of {mcu_id} for {led.ref}')
            pin = str(free[0][1])
            d.decide(f'{led.ref}.gpio', str(free[0][2]), 'rule', f'lowest-numbered output-capable pin of {mcu_id} that is not a strapping, '
                     'programming or reset pin, so the LED cannot disturb boot or programming')
            spec.gpio.append(spec_ir.Gpio(u.ref, str(free[0][2]), led.ref, 'inferred'))
        if math.isfinite(max_source) and current > max_source:
            raise Stop('INVALID_INPUT', f'{led.ref}: {current * 1e3:g} mA exceeds the {max_source * 1e3:g} mA per-pin limit of {mcu_id}')
        drive = mcu_net(pin)
        vf = led_forward_voltage(parts[pid], current); needed = (volts - vf) / current
        if needed <= 0:
            raise Stop('INVALID_INPUT', f'{led.ref}: the {volts:g} V supply is below the LED forward voltage {vf:.3g} V at {current * 1e3:g} mA')
        value = eseries_at_least(series, needed, core)
        r = add('resistor', value, None, f'series resistor for {led.ref}: (Vsupply {volts:g} V - Vf {vf:.4g} V at {current * 1e3:g} mA, from '
                f'{pid} model) / {current * 1e3:g} mA = {needed:.4g} ohm, rounded up to {series} {value:g} ohm so the current stays at or below target')
        anode = f'{led.ref}_A'
        connect(r, '1', drive); connect(r, '2', anode); connect(led.ref, '2', anode); connect(led.ref, '1', gnd)
        d.decide(f'{led.ref}.polarity', f'anode (pin 2) {anode}, cathode (pin 1) {gnd}', 'catalogue', f'{pid} pin map cathode=1, anode=2')

    used = {k.terminal for k in spec.connections if k.ref == u.ref}
    spec.no_connect[u.ref] = sorted((p for p in names if p not in used), key=lambda p: [int(t) if t.isdigit() else t for t in re.split(r'(\d+)', p)])
    d.decide(f'{u.ref}.no_connect', f'{len(spec.no_connect[u.ref])} pins', 'rule', 'every module pin the design does not use is an explicit no-connect')
    spec.profiles['catalogue'] = spec_ir.Attr(catalogue.name, 'defaulted')  # beside the .espec: no machine-specific path
    spec.profiles['dfm'] = spec_ir.Attr(str(Path(dfm).resolve()), 'defaulted')
    if spec.name.provenance != 'explicit':
        spec.name = spec_ir.Attr('esp32_led' if 'ESP32' in (u.attrs.get('family').value if 'family' in u.attrs else '') else 'design', 'defaulted')
    errors, questions = spec_ir.validate(spec)
    if errors or questions:
        raise Stop('ERROR', 'the compiled spec is not complete (a compiler defect): ' + '; '.join(errors + [q.text for q in questions]))
    return intent, spec, d, notes, catalogue


# --------------------------------------------------------------------------- pipeline
def build_once(text, profile, answers, symdir, fpdir, out, core):
    """Spec -> .copper -> placement -> KiCad project in `out`. Returns the stage record (no verification)."""
    import copper, layout, spec_ir
    out.mkdir(parents=True, exist_ok=True)
    intent, spec, d, notes, catalogue = compile_spec(text, profile, answers, symdir, out, core)
    (out / 'intent.espec').write_text(spec_ir.dumps(intent), encoding='utf-8')
    (out / 'design.espec').write_text(spec_ir.dumps(spec), encoding='utf-8')
    if fpdir is None:
        raise copper.ToolUnavailable('KiCad footprint libraries are needed for placement: set KICAD_FOOTPRINT_DIR or pass --footprints')
    tree, lower_notes = spec_ir.lower(spec, str(catalogue), core, base=out)
    copper.write_tree(out / 'design.copper', tree)
    seeds = tuple(int(x) for x in d.take('seeds', 'layout.seeds', 'annealing seeds', convert=lambda v: v).split(';') if x)
    iterations = int(d.take('iterations', 'layout.iterations', 'annealing iterations', convert=lambda v: str(int(v))))
    best, candidates, problem = layout.optimize(tree, fpdir, core, seeds, iterations)
    record = dict(intent=intent, spec=spec, decisions=d, notes=notes + lower_notes, catalogue=catalogue, tree=tree,
                  best=best, candidates=candidates, problem=problem, out=out)
    if not best or not best['feasible']:
        return record
    placed = copy.deepcopy(best['resolved']); placed[:] = [v for v in placed if not (isinstance(v, list) and str(v[0]) == 'geometry')]
    width = best['width_rounds'][-1]['trace_width_mm']
    if width != problem['trace_width']:
        from hwc_kicad import one
        rules = one(placed, 'rules')
        rules[:] = [v for v in rules if not (isinstance(v, list) and str(v[0]) == 'trace_width_mm')] + [copper.sx('trace_width_mm', copper.number(width))]
    copper.write_tree(out / 'placed.copper', placed)
    args = argparse.Namespace(project=str(out / 'placed.copper'), out=str(out / 'build'), core=core, symbol_dir=str(symdir),
                              footprint_dir=str(fpdir), no_route=False, draft=False)
    with contextlib.redirect_stdout(io.StringIO()):
        record['build_rc'] = copper.export_project(args)
    record['manifest'] = json.loads((out / 'build/manifest.json').read_text())
    return record


def artifact_hashes(out):
    files = {}
    for name in ('design.espec', 'design.copper', 'placed.copper'):
        files[name] = hashlib.sha256((out / name).read_bytes()).hexdigest()
    files.update({f'build/{k}': v for k, v in json.loads((out / 'build/manifest.json').read_text())['files'].items()})
    return files


def verify_build(out, cli, core, project):
    import copper
    args = argparse.Namespace(out=str(out), kicad_cli=cli, core=core, project=str(project), format='quiet')
    with contextlib.redirect_stdout(io.StringIO()):
        copper.verify(args)
    return json.loads((Path(out) / 'verification.json').read_text())


def tamper_checks(build, cli, core, project):
    """Verification must reject a modified output and a stale source (run on private copies)."""
    results = {}
    with tempfile.TemporaryDirectory() as d:
        copy_dir = Path(d) / 'build'; shutil.copytree(build, copy_dir)
        pcb = copy_dir / 'design.kicad_pcb'; pcb.write_bytes(pcb.read_bytes() + b' ')
        v = verify_build(copy_dir, None, core, project)
        results['modified_output'] = next(c['status'] for c in v['checks'] if c['check'] == 'output_hashes')
        copy2 = Path(d) / 'build2'; shutil.copytree(build, copy2); stale = Path(d) / 'stale.copper'
        stale.write_bytes(Path(project).read_bytes() + b'\n; edited\n')
        v = verify_build(copy2, None, core, stale)
        results['stale_source'] = next(c['status'] for c in v['checks'] if c['check'] == 'source_freshness')
    return results


def worst(statuses):
    order = ['ERROR', 'FAIL', 'INVALID_INPUT', 'NOT_IMPLEMENTED', 'UNVERIFIED', 'PASS']
    return min(statuses, key=order.index) if statuses else 'UNVERIFIED'


def run(text, out, profile_path=None, answers_text=None, symbol_dir=None, footprint_dir=None, kicad_cli=None, core=None):
    """The acceptance workflow. Returns the report dict (also written to out/design.json)."""
    import copper, layout, spec_ir
    out = Path(out).resolve(); out.mkdir(parents=True, exist_ok=True)
    profile = load_profile(profile_path) if profile_path else None
    answers = parse_answers(answers_text)
    report = dict(request=text, profile=str(profile['path']) if profile else None, answers=answers, gates={}, decisions=[],
                  questions=[], notes=[], files={})
    # Libraries are needed only once every decision is settled: questions come first, wherever they are asked.
    try:
        symdir = copper.discover_symbol_dir(symbol_dir); fpdir = copper.discover_footprint_dir(footprint_dir)
        report['libraries'] = dict(symbols=str(symdir), footprints=str(fpdir))
    except FileNotFoundError as exc:
        symdir = fpdir = None; report['libraries'] = dict(error=str(exc).splitlines()[0])
    try:
        rec = build_once(text, profile, answers, symdir, fpdir, out, core)
    except Stop as exc:
        report.update(status=exc.status, message=str(exc))
        if exc.decisions:
            report['decisions'] = exc.decisions.items; report['questions'] = exc.decisions.questions
        write_report(out, report); return report
    d, spec, best = rec['decisions'], rec['spec'], rec['best']
    report['decisions'] = d.items; report['notes'] = rec['notes']; report['fingerprint'] = spec_ir.fingerprint(spec)
    report['provenance'] = spec_ir.provenance_report(spec)
    g = report['gates']

    def gate(n, name, status, detail):
        g[name] = dict(gate=n, status=status, detail=detail)

    gate(1, 'spec', 'PASS', f'complete spec validates ({len(spec.components)} components, {len(spec.nets())} nets); '
         f'{sum(1 for x in d.items if x["source"] != "text")} decisions not stated in the request are listed')
    # 2 topology: every spec component and connection exists in the lowered design
    data = {c['ref']: c for c in copper.component_data(rec['tree'])}
    missing = [f'{k.ref}.{k.terminal}->{k.net}' for k in spec.connections if data.get(k.ref, {}).get('pins', {}).get(k.terminal) != k.net]
    missing += [c.ref for c in spec.components if c.ref not in data]
    gate(2, 'topology', 'FAIL' if missing else 'PASS', f'missing: {missing}' if missing else 'every requested component and connection is in the design')
    # 3 parts: real KiCad symbol/footprint and pin/pad parity
    problems, unverified = [], []
    for pid in sorted({text_at_part(c) for c in data.values()}):
        part = next(p for p in copper.catalogue(str(rec['catalogue'])) if str(p[1]) == pid)
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / 'p.parts'; copper.write_tree(probe, copper.sx('catalogue', [x for x in part if not (isinstance(x, list) and str(x[0]) in ('alternates', 'catalogue'))]))
            summary = copper.validate_catalogue(probe, core)['parts'][0]
        try:
            problems += [f'{pid}: {p}' for p in copper.library_parity(part, summary, symdir, fpdir)]
        except copper.Unverified as exc:
            unverified.append(f'{pid}: {exc}')
    gate(3, 'parts', 'FAIL' if problems else 'UNVERIFIED' if unverified else 'PASS',
         '; '.join(problems + unverified) or 'every physical part maps to its KiCad symbol and footprint with matching pins and pads')
    # 4 models / 5 electrical: copper-core on the lowered design
    analysis = copper.core_check(rec['tree'], core)
    gaps = [i for i in analysis['issues'] if i['rule'] in copper.MODEL_GAPS]
    boundary = [c['ref'] for c in analysis['components'] if c.get('mcu')]
    gate(4, 'models', 'FAIL' if gaps else 'PASS', '; '.join(i['message'] for i in gaps) or
         f'every device has a DC model; {", ".join(boundary)} is an MCU behavioral boundary (DC constraints only; firmware and silicon not simulated)')
    scopes = copper.report_scopes(analysis); led_checks = []
    for led in [c for c in spec.components if c.role == 'led']:
        series = next(k.ref for k in spec.connections if k.net == f'{led.ref}_A' and k.ref != led.ref)
        calc = next((c for c in analysis['calculations'] if c['ref'] == series), None)
        target = next(x['value'] for x in d.items if x['field'] == f'{led.ref}.current_a')
        if calc:
            led_checks.append(dict(led=led.ref, resistor=series, current_a=calc['current_a'], target_a=float(target), ok=calc['current_a'] <= float(target) * (1 + 1e-9)))
    electrical = 'FAIL' if scopes['electrical'] == 'FAIL' or any(not c['ok'] for c in led_checks) else scopes['electrical']
    gate(5, 'electrical', electrical, dict(dc_solved=analysis['dc_solved'], mna_residual=analysis['mna_residual'], kcl_residual_a=analysis['kcl_residual_a'],
         led_current=led_checks, findings=[f"{i['severity']}:{i['rule']} {i['object']}" for i in analysis['issues'] if i['rule'] in copper.ELECTRICAL_RULES]))
    if not best or not best['feasible']:
        c = layout.brief(best) if best else None
        gate(6, 'routing', 'FAIL', f"no feasible board: {c['unrouted'] if c else '?'} unrouted, hard {c['hard_by_rule'] if c else {}}")
        for n, name in enumerate(GATES[6:], 7):
            gate(n, name, 'UNVERIFIED', 'not reached: no feasible board')
        report['layout'] = dict(chosen=c, candidates=[layout.brief(x) for x in rec['candidates']])
        report.update(status=worst([v['status'] for v in g.values()]), message='no feasible board was found')
        write_report(out, report); return report
    report['layout'] = dict(chosen=layout.brief(best), candidates=[layout.brief(x) for x in rec['candidates']])
    gate(6, 'routing', 'PASS', f"fully connected: {best['routing']['routed_connections']} connections, {best['routing']['length_mm']:.2f} mm, "
         f"{best['routing']['vias']} vias, trace {best['width_rounds'][-1]['trace_width_mm']:.2f} mm ({best['label']})")
    built = json.loads((out / 'build/report.json').read_text()); physical = copper.report_scopes(built)['physical']
    gate(7, 'internal_dfm', physical, [f"{i['severity']}:{i['rule']} {i['object']}" for i in built['issues'] if i['rule'] not in copper.ELECTRICAL_RULES]
         or 'all supported mandatory rules pass')
    cli = copper.discover_cli(kicad_cli); v = verify_build(out / 'build', cli, core, out / 'placed.copper')
    report['verification'] = v; gates = {x['gate']: x for x in v['gates']}
    erc_drc = [gates[k]['status'] for k in ('erc', 'drc') if k in gates]
    gate(8, 'kicad_erc_drc', worst(erc_drc) if len(erc_drc) == 2 else 'UNVERIFIED',
         {k: dict(status=gates[k]['status'], exit_code=gates[k].get('returncode')) for k in ('erc', 'drc') if k in gates} or
         (v['gates'][0].get('message') if v['gates'] else 'KiCad CLI not run'))
    gate(9, 'connectivity', gates['netlist_parity']['status'] if 'netlist_parity' in gates else 'UNVERIFIED',
         gates.get('netlist_parity', {}).get('message', 'KiCad netlist not exported: independent pin/net parity not compared'))
    checks = {c['check']: c['status'] for c in v['checks']}; tamper = tamper_checks(out / 'build', cli, core, out / 'placed.copper')
    ok = checks.get('output_hashes') == 'PASS' and checks.get('source_freshness') == 'PASS' and tamper == {'modified_output': 'FAIL', 'stale_source': 'FAIL'}
    gate(10, 'manifest', 'PASS' if ok else 'FAIL', dict(output_hashes=checks.get('output_hashes'), source_freshness=checks.get('source_freshness'),
         rejects_modified_output=tamper['modified_output'] == 'FAIL', rejects_stale_source=tamper['stale_source'] == 'FAIL'))
    with tempfile.TemporaryDirectory() as tmp:
        again = build_once(text, profile, answers, symdir, fpdir, Path(tmp) / 'run2', core)
        first, second = artifact_hashes(out), artifact_hashes(Path(tmp) / 'run2') if again.get('manifest') else {}
    differ = sorted(k for k in set(first) | set(second) if first.get(k) != second.get(k))
    gate(11, 'determinism', 'FAIL' if differ else 'PASS', f'differs on re-run: {differ}' if differ else
         f'a second run from the same request, profile, catalogue and tools reproduced all {len(first)} artifacts byte for byte')
    report['files'] = {k: str(out / k) for k in ('intent.espec', 'design.espec', 'design.copper', 'placed.copper', 'catalogue.parts')}
    report['files'].update({k: str(out / 'build' / k) for k in ('design.kicad_sch', 'design.kicad_pcb', 'design.kicad_pro', 'bom.csv', 'report.json', 'manifest.json', 'verification.json')})
    report['notes'].append('the board is build/design.kicad_pcb: KiCad\'s native board format (KiCad 6 and later); a legacy ".pcb"/".brd" file is not produced')
    report.update(status=worst([x['status'] for x in g.values()]), message='')
    write_report(out, report); return report


def text_at_part(component):
    return str(component['part'][1])


def write_report(out, report):
    (Path(out) / 'design.json').write_text(json.dumps(report, indent=2, default=str) + '\n', encoding='utf-8')
