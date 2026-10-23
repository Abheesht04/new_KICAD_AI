# Part registry, model variants and the MCU behavioral boundary

A part is **data**: a KiCad identity, an exact pin mapping, ratings with provenance, and a reference to a
registered device model ([DEVICE_MODEL_CONTRACT.md](DEVICE_MODEL_CONTRACT.md)). Adding a part never adds
code. There is no native plugin loading; custom model equations need a later, explicitly designed extension
contract. A symbol is never a simulation model, and a generic symbol never stands in for real pins.

## Part definition

```lisp
(part nmos_switch_test
  (kind M) (model level1.nmos)            ; kind + registered model id (copper-core models)
  (family mosfet) (role n_channel_switch) ; logical family and role (identifiers)
  (symbol "Transistor_FET:2N7002")        ; KiCad library:symbol
  (footprint "Package_TO_SOT_SMD:SOT-23") ; KiCad library:footprint
  (pinmap (drain 3) (gate 1) (source 2))  ; model terminal -> symbol pin = footprint pad
  (vto 1.6) (kp 0.05) (lambda 0.01)       ; model parameters: this makes it a model variant
  (manufacturer "...") (mpn "...")        ; optional procurement metadata
  (datasheet "https://...")
  (power_w 0.1) (voltage_v 60) (tolerance 0.01) (height_mm 1.1)   ; omit what you do not know
  (provenance "datasheet rev, table ...") ; required whenever ratings or model data are present
  (alternates other_part_id))             ; procurement substitutes in the same catalogue; never auto-swapped
```

* **Unknown is not zero.** An omitted rating is UNKNOWN; `0` is rejected; every check that needs an unknown
  value reports `rating_unknown`, so a build with unknown ratings is `UNVERIFIED` and fabrication refuses it.
* **Pin mapping.** KiCad pairs symbol pin *N* with footprint pad *N*, so one map serves both
  (`(terminal pin pad)` must repeat the same number). A model with more than two terminals must have an
  explicit `(pinmap ...)`: terminal order is never assumed. Pin names in the symbol may not contradict the map
  (pin `G` mapped to `drain` is rejected).
* **Project-local overrides** stay on the component: `(override (power_w ...) (provenance "..."))`.
* **Capabilities** come from the bound model (`dc`, `ac`, `transient`) and are listed by `copper-core catalogue FILE`
  and in every analysis report.

## Import and validation

`pcba parts import FILE [--into my.parts] [--symbols DIR] [--footprints DIR]`:

1. every part is checked alone by copper-core (`copper-core catalogue`): kind, model, parameters, pin map,
   ratings, MCU block, unique ids;
2. ratings, tolerances, heights, model parameters or an MCU block require `(provenance "...")`;
3. against the native KiCad 9.x libraries: the symbol and footprint exist; the set of symbol pin numbers equals
   the set of footprint pad numbers (unnumbered mechanical pads excepted); every mapped pin and MCU pin is a
   real symbol pin; MCU supply/ground pins are power inputs; GPIO names match the symbol's pin names;
4. alternates must name accepted or existing parts.

The report lists `accepted`, `rejected` (with reasons), `duplicates` (ids already in the bundled or target
catalogue) and `unverified` (library data outside KiCad 9.x support, e.g. KiCad 10 stacked pin numbers).
`--into` writes only when every part is accepted, after copper-core validates the merged file. Without usable
libraries the result is `UNVERIFIED` and nothing is written.

Libraries: the installed KiCad 9.x libraries are the default source (`KICAD_SYMBOL_DIR`,
`KICAD_FOOTPRINT_DIR`, or the KiCad install). Catalogues hold only library IDs; no library content is cloned
into them or into projects. A build snapshots exactly the symbol libraries and the selected footprints it uses
under `libraries/`, with the KiCad library licence (CC-BY-SA-4.0 with the KiCad libraries exception) and a
NOTICE; the manifest records their hashes and source directories.

## MCU behavioral boundary

A kind-`U` part may carry an `(mcu ...)` block: typed electrical constraints of the module, **not** a silicon
or firmware model. `profiles/mcu_modules.parts` defines the Espressif ESP32-WROOM-32E and -32UE from the KiCad
symbol/footprint pins and the datasheet named in its provenance.

```lisp
(mcu
  (behavior "DC electrical constraints only: ... not simulated")   ; required statement
  (supply 2) (ground 1 15 38 39) (operating_v 3.0 3.6)             ; required
  (vih_ratio 0.75) (vil_ratio 0.25)                                ; thresholds as fractions of the supply
  (max_source_a 0.04) (max_sink_a 0.028)                           ; per-pin limits, when sourced
  (output_ohm R) (pull_ohm R)                                      ; only when a source gives them
  (gpio (pin 26 IO4 in out) (pin 6 IO34 in) ...)                   ; directions per pin
  (straps (25 "IO0 selects the boot mode: ...") ...)               ; strapping pins and what they select
  (reset 3 active_low)                                             ; reset / enable pin
  (program 35 34 25 3)                                             ; in-circuit programming pins
  (reference (decouple 2 10e-6 100e-9) (pull_up 3 10000)            ; support circuit from the datasheet:
             (cap_to_ground 3 1e-6) (provenance "..."))             ; the design compiler adds exactly this
  (provenance "..."))
```

In a project the component states pin usage explicitly:

```lisp
(component U1 (part esp32_wroom_32e)
  (pins (1 GND) (2 3V3) (3 EN) (15 GND) (26 IO4) (34 RXD0) (35 TXD0) (38 GND) (39 GND))
  (nc 4 5 6 ...)                ; every other symbol pin, deliberately unconnected
  (drive (26 high)))            ; high | low | input | pull_up | pull_down
```

What DC analysis does with it:

| Situation | Treatment |
|---|---|
| Undriven pins, inputs | high impedance (no equation) |
| `high` / `low` output | ideal connection to the first supply / ground pin, or through `output_ohm` when sourced; ideal outputs report `mcu_output_ideal` |
| `pull_up` / `pull_down` | through `pull_ohm` when sourced, otherwise `mcu_pull_unknown` and high impedance |
| Supply current | never modelled: always `mcu_supply_current` (rail drop, source load and trace budgets exclude it) |
| A net only connector/MCU pins touch | DC voltage undefined (`null`, `boundary_net`); a target on it fails |

Checks: supply pins inside `operating_v` (`mcu_supply_voltage`, error); output current against
`max_source_a`/`max_sink_a` (`mcu_pin_current`, error; `rating_unknown` when not sourced); inputs between
VIL and VIH (`mcu_input_undefined`); a connected strapping pin (`mcu_strap_pin`: its reset level is not
verified); reset/enable pin without a defined run level (`mcu_reset`); unconnected programming pins
(`mcu_programming`). Contract errors: driving an input-only or unconnected pin, driving a pin twice, a supply
or ground pin left unconnected, ground pins on different nets, a pin both connected and `(nc ...)`.
Every MCU project is therefore `UNVERIFIED` at best: the boundary constrains, it does not prove. AC and
transient analyses refuse MCU boundaries (they cover DC constraints only).

The compiler never infers an exact module from "ESP32": the EngineeringSpec asks for the variant
([ENGINEERING_SPEC.md](ENGINEERING_SPEC.md)) and the user binds `(part esp32_wroom_32e)`.

## Footprint geometry from the selected footprint

* **Keepouts**: a footprint's own rule area (e.g. the ESP32 antenna keepout) becomes
  `(keepout REF X1 Y1 X2 Y2 LAYERS FLAGS)` in the resolved IR, taken from the footprint polygon — never
  guessed. copper-core rejects any track, via, other component's pad or other component's assembly envelope
  touching it (`keepout_violation`, exact geometry, zero clearance); the router keeps tracks and vias out. Only
  rectangular keepouts that keep something out on F.Cu/B.Cu are supported; anything else is `UNVERIFIED`.
* **No-connect pins** have pads with no net (`~` in the IR, KiCad's `unconnected-(REF-PIN-PadN)` net on the
  board), still copper for every clearance rule; the schematic gets no-connect flags.
* **Stacked pads** (copies of one pad number, such as a thermal pad with vias) are one copper feature: one
  route reaches it and no mask web is required between its copies. Different pins are always checked.
* **Pad drills** below the profile's minimum hole fail (`pad_drill`), as KiCad's hole rule does. The ESP32
  module's 0.2 mm thermal vias need `(rules (drill_mm 0.2))`, a 0.2 mm-capable process.
* **Polygon courtyards** are enclosed by their bounding box (a conservative envelope).
* **Power flags**: nets with power-input pins that the circuit's own source drives get a `power:PWR_FLAG`
  in the schematic; an undriven power input remains an ERC error.

## Tests

`tests/test_parts.py`: `parts_registry` (CTest) covers the module and MOSFET definitions, schema
rejections, the unknown-pin-map rule, every MCU check and contract error, keepouts, no-connect pads, pad
drills, stacked pads, and import parity on the committed library snapshot. `parts_native` builds the ESP32
example and a GPIO-driven MOSFET project with installed KiCad 9.x libraries and runs ERC, DRC, schematic
parity and the independent pin/net comparison; without KiCad 9.x it is skipped (`UNVERIFIED`), never passed.
