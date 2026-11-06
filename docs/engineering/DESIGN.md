# From a request to a verified KiCad project

```text
pcba design "Build me an ESP32 LED PCB. Generate the KiCad PCB." --out out/esp32_led --profile profiles/esp32_led.design
```

`tools/design.py` is a compiler, not a macro: the sentence is read by the constrained-English grammar
([ENGINEERING_SPEC.md](ENGINEERING_SPEC.md)), and everything after that goes through the shared pipeline
(EngineeringSpec → `.copper` → copper-core → `tools/layout.py` → KiCad export → verification). "Make an ESP32
LED board" or "design a board with an ESP32 and a red LED" produce the same intent.

## Stages

| Stage | Output (in `--out`) |
|---|---|
| intent: the parts the request names, and requested outputs | `intent.espec` |
| decisions: each one from the request, `--answers`, the design profile, the catalogue or a named rule | `design.json` → `decisions` |
| expansion: the complete design from catalogue data and named rules | `design.espec` (all nets explicit) |
| lowering + DC analysis (copper-core) | `design.copper` |
| placement and routing (`layout.optimize`, exact re-check) | `placed.copper` |
| KiCad export | `build/design.kicad_sch`, `.kicad_pcb`, `.kicad_pro`, `bom.csv`, `report.json`, `manifest.json`, library snapshots |
| verification and quality gates | `build/verification.json`, `design.json` → `gates` |

A decision that nothing settles is a **question** (`design.json` → `questions`, each with `--answers KEY=VALUE`);
the run stops UNVERIFIED and builds nothing. Unsupported designs stop NOT_IMPLEMENTED; contradictions
(a supply outside the module's range, an input-only GPIO, a current above the pin limit, a colour the
chosen part does not have, an unknown answer key) stop INVALID_INPUT.

## Decisions for an MCU + indicator LED request

| Decision | Settled by (first that applies) |
|---|---|
| exact module (`mcu_part`) | `(part ID)` in the request; a catalogue part whose MPN equals the stated variant; answer; profile. Never the family name alone |
| what the LED is (`led_function`) | answer; profile. Only `gpio_indicator` has a design rule |
| LED part (`led_part`), colour | request (`(part ID)`); answer; profile. A requested colour must match the part |
| LED current (`led_current`, A) | "the current through the LED must be at most …"; answer; profile. At most the module's per-pin source limit |
| power (`supply_kind`, `supply_voltage`) | a supply in the request; answer; profile. `header`: power enters on the header; voltage inside the module's operating range |
| programming (`programming`, `programming_part`) | answer; profile. `uart_header`: a header with power, ground and the module's programming pins |
| board size and layers, DFM profile | request; answer; profile |
| resistor series | answer; profile; rule (E24) |
| catalogues | answer (`catalogues=a.parts;b.parts`); profile; rule (the bundled module and indicator catalogues) |

The profile `profiles/esp32_led.design` holds documented project defaults for this target; every value
taken from it is reported as a `profile` decision.

## Design rules (expansion)

* **Module power**: every supply pin to the supply net, every ground pin to GND, from the part's `(mcu ...)`.
* **Reference circuit**: the module's `(mcu (reference ...))` data only — decoupling capacitors on supply pins,
  pull-ups and capacitors to ground as listed, with the datasheet provenance in each decision. No capacitor
  is added that the catalogue does not list.
* **Programming header**: power, ground, then the module's `(program ...)` pins in catalogue order; the header
  part must have exactly that many pins.
* **GPIO indicator**: GPIO → series resistor → LED anode; LED cathode → GND (polarity from the LED part's pin
  map). The GPIO is the request's, or else the lowest-numbered output-capable pin that is not a strapping,
  programming or reset pin. The resistor is the smallest value of the series not below
  `(Vsupply − Vf(I)) / I`, with Vf from the LED part's own model (`copper-core eseries`), so the current stays at
  or below the target; copper-core's DC solve then checks the actual current.
* **No-connects**: every module pin the design does not use is an explicit `(nc ...)`.
* **DC scenario**: the GPIO drives its LED high (LED on), through the MCU behavioral boundary
  ([PART_REGISTRY.md](PART_REGISTRY.md)); supply current of the module is not modelled and is reported.

## Quality gates (`design.json` → `gates`)

| # | Gate | PASS means |
|---|---|---|
| 1 | spec | the complete spec validates; every decision not stated in the request is listed |
| 2 | topology | every spec component and connection is in the lowered design |
| 3 | parts | every physical part's symbol and footprint exist and pins equal pads (`parts import` parity); UNVERIFIED for unsupported library data |
| 4 | models | every device has a DC model; MCU boundaries are named as constraint-only |
| 5 | electrical | copper-core's electrical scope (DC solved, residuals, ratings) and each LED current at or below its target; unknown ratings keep it UNVERIFIED |
| 6 | routing | the chosen candidate is fully connected (or FAIL with the unrouted connections) |
| 7 | internal_dfm | copper-core's physical scope on the built board; unknown heights and unsupported rule families keep it UNVERIFIED |
| 8 | kicad_erc_drc | KiCad 9.x ERC and DRC ran and passed, exit codes captured; UNVERIFIED without KiCad 9.x |
| 9 | connectivity | KiCad's netlist equals the design's pin/net map (independent comparison) |
| 10 | manifest | output hashes and source freshness pass, and verification rejects a modified output and a stale source (checked on copies) |
| 11 | determinism | a second run from the same request, profile, catalogue and tools reproduces every artifact byte for byte |

The overall status is the worst gate. With generic parts (no MPN, so no ratings or heights) the honest best
result is UNVERIFIED: give the catalogue rated parts to go further.

## Output format

The board is `build/design.kicad_pcb`, KiCad's native board format (KiCad 6 and later). A request for a
".pcb" file is answered with this note; no legacy `.pcb`/`.brd` file is produced.

## Scope

One microcontroller with GPIO indicator LEDs, powered through its programming header. Not covered: external
loads, addressable LEDs, LED drivers, regulators or USB power entry, multiple MCUs, firmware, RF.
