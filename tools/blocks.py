"""Reusable circuit blocks (docs/engineering/BLOCKS.md).

A block is declared in an EngineeringSpec, `(blocks (block NAME (PARAM VALUE) ...))`, and expanded when the spec is
lowered into components, connections and no-connects. Every value a block adds has a source:

  standard  a cited specification and the formula applied to the block's parameters (I2C pull-ups, USB-C Rd);
  part      the bound catalogue part's datasheet data, its (reference ...) entry, whose provenance is mandatory;
  param     a block parameter the user stated.

A value with no source is a question; nothing is defaulted, and no part is chosen: parts are catalogue ids the user
names. The checks a block cannot make itself (regulator dropout, current and dissipation) are copper-core's, on the
lowered project.
"""
from __future__ import annotations
import json, math, re, subprocess
from dataclasses import dataclass, field

# I2C-bus specification and user manual (NXP UM10204), "Pull-up resistor sizing": Rp(min) = (VDD - VOL(max)) / IOL,
# Rp(max) = tr / (0.8473 * Cb); characteristics of the SDA and SCL bus lines: VOL(max) = 0.4 V at IOL = 3 mA
# (Standard-mode, Fast-mode) or 20 mA (Fast-mode Plus); tr(max) = 1000 ns, 300 ns, 120 ns.
I2C_SOURCE = 'NXP UM10204 I2C-bus specification, pull-up resistor sizing: Rp(min) = (VDD - VOL)/IOL, Rp(max) = tr/(0.8473 Cb)'
I2C_MODES = {'standard': (1000e-9, 3e-3), 'fast': (300e-9, 3e-3), 'fast_plus': (120e-9, 20e-3)}
I2C_VOL = 0.4
# USB Type-C Cable and Connector Specification (USB-IF): a sink terminates each CC pin with Rd = 5.1 kOhm +/-20 % to ground.
USB_C_RD = 5100.0
USB_C_SOURCE = 'USB Type-C Cable and Connector Specification (USB-IF): sink Rd = 5.1 kOhm +/-20 % from each CC pin to ground'


@dataclass
class Param:
    name: str
    kind: str                 # net | ref | part | pin | choice | quantity
    required: bool
    meaning: str
    choices: tuple = ()
    unit: str = ''


@dataclass
class Block:
    name: str
    summary: str
    params: list
    expand: object = None


@dataclass
class Use:
    """A block in a spec: its name and parameter values (spec_ir.Attr or spec_ir.Quantity)."""
    name: str
    params: dict = field(default_factory=dict)


class Expansion:
    """What a block adds to a spec, and the source of every value."""
    def __init__(self, spec, catalogue, core, series):
        self.spec, self.catalogue, self.core, self.series = spec, catalogue, core, series
        self.records = []
        self.taken = {c.ref for c in spec.components}

    def ref(self, prefix):
        n = 1
        while f'{prefix}{n}' in self.taken:
            n += 1
        self.taken.add(f'{prefix}{n}')
        return f'{prefix}{n}'

    def add(self, block, role, value, source, provenance, part=None):
        import spec_ir
        rules = spec_ir.ROLES[role]; r = self.ref(rules['prefix'])
        c = spec_ir.Component(r, role, None if value is None else spec_ir.Quantity(value, rules['unit'], 'inferred'), provenance='inferred')
        if part:
            c.attrs['part'] = spec_ir.Attr(part, 'catalogue-derived')
        self.spec.components.append(c)
        self.records.append(dict(block=block, ref=r, role=role, value=value, unit=rules['unit'] if value is not None else None,
                                 part=part, source=source, provenance=provenance))
        return r

    def connect(self, ref, terminal, net):
        import spec_ir
        self.spec.connections.append(spec_ir.Connection(ref, str(terminal), net, 'inferred'))

    def part(self, block, part_id):
        import copper
        if part_id not in self.catalogue:
            raise copper.Unverified(f'block {block}: part {part_id!r} is not in the catalogue')
        return self.catalogue[part_id]

    def pin_net(self, ref, pin):
        """The net on a component pin, creating REF_P<pin> when the pin is not connected yet."""
        net = next((k.net for k in self.spec.connections if k.ref == ref and k.terminal == str(pin)), None)
        if net is None:
            net = f'{ref}_P{pin}'; self.connect(ref, pin, net)
        return net

    def supply_voltage(self, net):
        """Volts of the spec's supply driving `net` against GND, or None."""
        for c in self.spec.components:
            if c.role != 'supply' or c.value is None:
                continue
            pins = {k.terminal: k.net for k in self.spec.connections if k.ref == c.ref}
            if pins.get('1') == net and pins.get('2') == 'GND':
                return c.value.value
        return None

    def eseries_at_most(self, value):
        import copper
        r = subprocess.run([copper.core_path(self.core), 'eseries', self.series, repr(value)], capture_output=True, text=True, timeout=60)
        if r.returncode:
            raise copper.InvalidInput(r.stderr.strip() or f'eseries failed for {value}')
        return json.loads(r.stdout)['at_most']


# --------------------------------------------------------------------------- part data
def reference_of(part):
    """A catalogue part's datasheet reference data: (mcu (reference ...)) or (reference ...), or None."""
    from hwc_kicad import one
    mcu = one(part, 'mcu')
    return (one(mcu, 'reference') if mcu is not None else None) or one(part, 'reference')


def entries(reference, key):
    """Values of every (KEY ...) entry, flattened, as strings."""
    from hwc_kicad import direct
    return [str(v) for e in direct(reference, key) for v in e[1:]]


def needed(block, part_id, reference, *keys):
    import copper
    if reference is None:
        raise copper.Unverified(f'block {block}: part {part_id} has no (reference ...) datasheet data')
    missing = [k for k in keys if not entries(reference, k)]
    if missing:
        raise copper.Unverified(f'block {block}: part {part_id} (reference ...) does not state {", ".join(missing)}')


def reference_parts(reference, label, add, connect, supply, ground, pin_net, pin_label=lambda p: p):
    """The decoupling and pin networks a part's datasheet reference circuit lists, in its order: (decouple PIN F ...)
    capacitors from the pin's supply to ground, (pull_up PIN OHM) from the supply to the pin, (cap_to_ground PIN F).
    add(role, value, why) -> ref; supply(pin) -> net; pin_net(pin) -> net. Shared by design.py and the blocks."""
    from hwc_kicad import text_at
    why = f'{label} reference circuit: {text_at(reference, "provenance")}'
    for entry in (reference or [None])[1:]:
        kind = str(entry[0])
        if kind not in ('decouple', 'pull_up', 'cap_to_ground'):
            continue
        pin = str(entry[1])
        for value in (float(v) for v in entry[2:]):
            if kind == 'decouple':
                c = add('capacitor', value, f'decoupling on supply pin {pin}; {why}'); connect(c, '1', supply(pin)); connect(c, '2', ground)
            elif kind == 'pull_up':
                r = add('resistor', value, f'pull-up on pin {pin} ({pin_label(pin)}); {why}'); connect(r, '1', supply(pin)); connect(r, '2', pin_net(pin))
            else:
                c = add('capacitor', value, f'capacitor to ground on pin {pin} ({pin_label(pin)}); {why}'); connect(c, '1', pin_net(pin)); connect(c, '2', ground)


def ref_part(x, block, ref):
    import copper
    c = x.spec.component(ref)
    if c is None:
        raise copper.InvalidInput(f'block {block}: {ref} is not in the spec')
    if 'part' not in c.attrs:
        raise copper.Unverified(f'block {block}: {ref} has no catalogue part; bind one with (part ID)')
    return c, c.attrs['part'].value, x.part(block, c.attrs['part'].value)


# --------------------------------------------------------------------------- the blocks
def _reference_circuit(x, p):
    """The component's datasheet reference circuit (decoupling, pull-ups, RC) between its own supply and ground."""
    import copper
    from hwc_kicad import one
    ref = p['component']; c, part_id, part = ref_part(x, 'reference_circuit', ref)
    reference = reference_of(part)
    needed('reference_circuit', part_id, reference)
    mcu = one(part, 'mcu')
    supply_pins = [str(v) for v in (one(mcu, 'supply') or [None])[1:]] if mcu is not None else entries(reference, 'supply')
    ground_pins = [str(v) for v in (one(mcu, 'ground') or [None])[1:]] if mcu is not None else entries(reference, 'ground')
    if not supply_pins or not ground_pins:
        raise copper.Unverified(f'block reference_circuit: part {part_id} does not name its supply and ground pins')
    supply = x.pin_net(ref, supply_pins[0]); ground = x.pin_net(ref, ground_pins[0])
    reference_parts(reference, part_id, lambda role, v, why: x.add('reference_circuit', role, v, 'part', why), x.connect,
                    lambda pin: supply, ground, lambda pin: x.pin_net(ref, pin))


def _i2c_pullups(x, p):
    """SDA and SCL pull-ups: the weakest series value that still meets the mode's rise time on the stated bus
    capacitance, and no stronger than the mode's sink current allows."""
    import copper
    mode = p['mode']; tr, iol = I2C_MODES[mode]; cb = p['bus_capacitance']
    vdd = p.get('vdd_voltage') or x.supply_voltage(p['vdd'])
    if vdd is None:
        raise copper.Unverified(f'block i2c_pullups: no supply on {p["vdd"]} in the spec: state one or give vdd_voltage')
    if vdd <= I2C_VOL:
        raise copper.InvalidInput(f'block i2c_pullups: VDD {vdd:g} V is not above VOL {I2C_VOL} V')
    rmin, rmax = (vdd - I2C_VOL) / iol, tr / (0.8473 * cb)
    chosen = x.eseries_at_most(rmax)
    if chosen < rmin * (1 - 1e-9):
        raise copper.InvalidInput(f'block i2c_pullups: no {x.series} value between Rp(min) {rmin:.4g} ohm and Rp(max) {rmax:.4g} ohm '
                                  f'for {mode} mode on {cb * 1e12:g} pF: reduce the bus capacitance or choose a slower mode')
    why = (f'{I2C_SOURCE}; {mode} mode: tr {tr * 1e9:g} ns, IOL {iol * 1e3:g} mA, VOL {I2C_VOL} V; VDD {vdd:g} V, Cb {cb * 1e12:g} pF: '
           f'Rp from {rmin:.4g} to {rmax:.4g} ohm; the largest {x.series} value in range')
    for line in ('scl', 'sda'):
        r = x.add('i2c_pullups', 'resistor', chosen, 'standard', f'{line.upper()} pull-up; {why}')
        x.connect(r, '1', p['vdd']); x.connect(r, '2', p[line])


def _usb_c_sink(x, p):
    """A USB-C receptacle wired as a default-power sink: VBUS and GND pins, Rd on each CC pin."""
    part_id = p['connector']; part = x.part('usb_c_sink', part_id); reference = reference_of(part)
    needed('usb_c_sink', part_id, reference, 'vbus', 'gnd', 'cc1', 'cc2', 'provenance')
    from hwc_kicad import text_at
    j = x.add('usb_c_sink', 'connector', None, 'part', f'USB-C receptacle {part_id}: {text_at(reference, "provenance")}', part_id)
    for pin in entries(reference, 'vbus'):
        x.connect(j, pin, p['vbus'])
    for pin in entries(reference, 'gnd'):
        x.connect(j, pin, p['gnd'])
    for cc in ('cc1', 'cc2'):
        net = f'{j}_{cc.upper()}'; x.connect(j, entries(reference, cc)[0], net)
        r = x.add('usb_c_sink', 'resistor', USB_C_RD, 'standard', f'{cc.upper()} sink termination Rd; {USB_C_SOURCE}')
        x.connect(r, '1', net); x.connect(r, '2', p['gnd'])
    if entries(reference, 'nc'):
        x.spec.no_connect[j] = sorted(set(entries(reference, 'nc')))


def _ldo(x, p):
    """A linear regulator part (kind U, model ideal.ldo) with its datasheet input and output capacitors."""
    import copper
    from hwc_kicad import text_at
    part_id = p['regulator']; part = x.part('ldo', part_id); reference = reference_of(part)
    if text_at(part, 'kind') != 'U' or text_at(part, 'model') != 'ideal.ldo':
        raise copper.InvalidInput(f'block ldo: part {part_id} must be kind U with (model ideal.ldo) and its datasheet dropout_v and iout_max_a')
    needed('ldo', part_id, reference, 'in', 'out', 'gnd', 'output_v', 'input_cap', 'output_cap')
    why = f'{part_id} datasheet: {text_at(reference, "provenance")}'
    u = x.add('ldo', 'regulator', float(entries(reference, 'output_v')[0]), 'part', f'output voltage; {why}', part_id)
    x.connect(u, entries(reference, 'in')[0], p['vin']); x.connect(u, entries(reference, 'out')[0], p['vout'])
    for pin in entries(reference, 'gnd'):
        x.connect(u, pin, p['gnd'])
    for key, net in (('input_cap', p['vin']), ('output_cap', p['vout'])):
        for value in map(float, entries(reference, key)):
            c = x.add('ldo', 'capacitor', value, 'part', f'{key.replace("_", " ")} of {u}; {why}')
            x.connect(c, '1', net); x.connect(c, '2', p['gnd'])


def _i2c_sensor(x, p):
    """An I2C device part on the bus: its supply, ground, SCL and SDA pins and its datasheet decoupling."""
    from hwc_kicad import text_at
    part_id = p['device']; part = x.part('i2c_sensor', part_id); reference = reference_of(part)
    needed('i2c_sensor', part_id, reference, 'supply', 'ground', 'scl', 'sda', 'provenance')
    u = x.add('i2c_sensor', 'sensor', None, 'part', f'I2C device {part_id}: {text_at(reference, "provenance")}', part_id)
    for key, net in (('supply', p['vdd']), ('ground', p['gnd']), ('scl', p['scl']), ('sda', p['sda'])):
        for pin in entries(reference, key):
            x.connect(u, pin, net)
    if entries(reference, 'nc'):
        x.spec.no_connect[u] = sorted(set(entries(reference, 'nc')))
    reference_parts(reference, part_id, lambda role, v, why: x.add('i2c_sensor', role, v, 'part', why), x.connect,
                    lambda pin: p['vdd'], p['gnd'], lambda pin: x.pin_net(u, pin))


def _reset_boot(x, p):
    """Reset and boot buttons on an MCU: each pulls its pin to GND while pressed. The reset pin and the meaning of the
    boot pin come from the MCU's catalogue data (its reset pin and strapping notes)."""
    import copper
    from hwc_kicad import one, text_at
    mcu_ref = p['mcu']; c, mcu_id, part = ref_part(x, 'reset_boot', mcu_ref); mcu = one(part, 'mcu')
    if mcu is None or one(mcu, 'reset') is None:
        raise copper.Unverified(f'block reset_boot: part {mcu_id} has no (mcu (reset PIN ...)) data')
    straps = {str(s[0]): str(s[1]) for s in (one(mcu, 'straps') or [None])[1:]}
    boot = str(p['boot_pin'])
    if boot not in straps:
        raise copper.InvalidInput(f'block reset_boot: pin {boot} is not a strapping pin of {mcu_id} (strapping pins: {", ".join(straps)})')
    button = p['button']; reference = reference_of(x.part('reset_boot', button))
    needed('reset_boot', button, reference, 'a', 'b', 'provenance')
    reset_pin = text_at(mcu, 'reset')
    for pin, why in ((reset_pin, f'{mcu_id} catalogue: (reset {reset_pin} active_low); pressing holds the MCU in reset'),
                     (boot, f'{mcu_id} catalogue strapping note for pin {boot}: "{straps[boot]}"')):
        sw = x.add('reset_boot', 'switch', None, 'part', f'{why}; button {button}: {text_at(reference, "provenance")}', button)
        x.connect(sw, entries(reference, 'a')[0], x.pin_net(mcu_ref, pin)); x.connect(sw, entries(reference, 'b')[0], 'GND')


NET, REF, PART = 'net', 'ref', 'part'
BLOCKS = {b.name: b for b in [
    Block('reference_circuit', "a part's datasheet reference circuit: decoupling, pull-ups and RC networks",
          [Param('component', REF, True, 'the component whose catalogue part lists (reference ...) entries')], _reference_circuit),
    Block('i2c_pullups', 'SDA/SCL pull-ups sized by the I2C-bus specification for a mode and bus capacitance',
          [Param('vdd', NET, True, 'the bus supply net'), Param('scl', NET, True, 'SCL net'), Param('sda', NET, True, 'SDA net'),
           Param('mode', 'choice', True, 'bus speed mode', tuple(I2C_MODES)),
           Param('bus_capacitance', 'quantity', True, 'total bus capacitance (wiring, pins, devices)', unit='F'),
           Param('vdd_voltage', 'quantity', False, 'supply voltage when the spec has no supply on vdd', unit='V')], _i2c_pullups),
    Block('usb_c_sink', 'USB-C receptacle as a default-power sink: VBUS, GND and 5.1 kOhm Rd on each CC pin',
          [Param('connector', PART, True, 'catalogue USB-C receptacle with (reference (vbus ..) (gnd ..) (cc1 ..) (cc2 ..))'),
           Param('vbus', NET, True, 'VBUS net'), Param('gnd', NET, True, 'ground net')], _usb_c_sink),
    Block('ldo', 'linear regulator with its datasheet input and output capacitors',
          [Param('regulator', PART, True, 'catalogue part: kind U, (model ideal.ldo), (reference (in ..) (out ..) (gnd ..) (output_v ..) (input_cap ..) (output_cap ..))'),
           Param('vin', NET, True, 'input net'), Param('vout', NET, True, 'output net'), Param('gnd', NET, True, 'ground net')], _ldo),
    Block('i2c_sensor', 'an I2C device on the bus with its datasheet decoupling',
          [Param('device', PART, True, 'catalogue part with (reference (supply ..) (ground ..) (scl ..) (sda ..))'),
           Param('vdd', NET, True, 'supply net'), Param('gnd', NET, True, 'ground net'),
           Param('scl', NET, True, 'SCL net'), Param('sda', NET, True, 'SDA net')], _i2c_sensor),
    Block('reset_boot', 'reset and boot buttons on an MCU, from its catalogue reset pin and strapping notes',
          [Param('mcu', REF, True, 'the MCU component'), Param('button', PART, True, 'catalogue push button with (reference (a PIN) (b PIN))'),
           Param('boot_pin', 'pin', True, 'the strapping pin that selects the boot mode')], _reset_boot),
]}
IDENT = re.compile(r'^[A-Za-z0-9_][A-Za-z0-9_+./-]{0,63}$')


def check(uses):
    """(errors, questions) for the blocks of a spec, without a catalogue: known block, known parameters, every
    required parameter stated, values of the right kind. questions: (field, text, options)."""
    errors, questions = [], []
    for i, use in enumerate(uses):
        block = BLOCKS.get(use.name)
        where = f'block {use.name}'
        if block is None:
            errors.append(f'{where}: unknown block (known: {", ".join(sorted(BLOCKS))})'); continue
        known = {p.name: p for p in block.params}
        for name in use.params:
            if name not in known:
                errors.append(f'{where}: unknown parameter {name!r} (parameters: {", ".join(known)})')
        for prm in block.params:
            v = use.params.get(prm.name)
            if v is None:
                if prm.required:
                    questions.append((f'block.{use.name}.{prm.name}', f'Block {use.name}: what is {prm.name} ({prm.meaning})?', list(prm.choices)))
                continue
            if prm.kind == 'quantity':
                if not hasattr(v, 'unit') or v.unit != prm.unit or not (math.isfinite(v.value) and v.value > 0):
                    errors.append(f'{where}: {prm.name} must be a positive quantity in {prm.unit}')
            elif hasattr(v, 'unit') or not IDENT.match(str(v.value)):
                errors.append(f'{where}: {prm.name} must be a {prm.kind} name')
            elif prm.kind == 'choice' and v.value not in prm.choices:
                errors.append(f'{where}: {prm.name} must be one of {", ".join(prm.choices)}')
    return errors, questions


def expand(spec, catalogue, core=None, series='E24'):
    """Expand every block of `spec` in place (in declaration order). catalogue: {part id: part tree}. Returns the
    records of every value added, each with its source and provenance."""
    x = Expansion(spec, catalogue, core, series)
    for use in spec.blocks:
        BLOCKS[use.name].expand(x, {k: v.value for k, v in use.params.items()})
    spec.blocks = []
    return x.records


def describe():
    """Every block with its parameters, for `pcba blocks`."""
    return [dict(name=b.name, summary=b.summary, params=[dict(name=p.name, kind=p.kind, required=p.required, meaning=p.meaning,
                                                              **({'choices': list(p.choices)} if p.choices else {}),
                                                              **({'unit': p.unit} if p.unit else {})) for p in b.params])
            for b in BLOCKS.values()]
