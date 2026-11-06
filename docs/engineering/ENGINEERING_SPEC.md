# EngineeringSpec IR and the constrained-English frontend

The EngineeringSpec is the first thing every frontend produces: **what the user asked for**,
with SI units and provenance. It is not a schematic, a part choice, a PCB or a solver result.
Two frontends produce it, and both produce the *same* IR:

```text
constrained English ──tools/spec_nlp.py──┐
                                         ├─> EngineeringSpec (tools/spec_ir.py) ──lower──> .copper ──> copper-core
hand-written .espec ──spec_ir.loads──────┘        validate · fingerprint · questions
```

| Command | Backend | Status |
|---|---|---|
| `pcba spec parse "TEXT" [--out F.espec]` / `--file request.txt` | `copper.py spec-parse` | INVALID_INPUT on any error, UNVERIFIED with open questions, PASS when complete |
| `pcba spec check F.espec` | `copper.py spec-check` | same rule |
| `pcba spec lower F.espec --out F.copper [--catalogue C]` | `copper.py spec-lower` | PASS only when the written project loads in copper-core |

`--json` output carries `fingerprint`, `errors`, `questions` (`id`, `field`, `text`, `options`),
`provenance` (every value that was not stated explicitly) and the IR itself (`spec`).
`spec parse --out` writes the file only when there are no errors; open questions are recorded
in it under `(unresolved ...)` so the file can be completed by hand.

The frontend is a new, deterministic implementation (normalizer, quantity scanner, lexicon,
clause grammar). It shares no code, data or weights with the retired language components
(`src/nlp_rules.*`, `src/v2/transformer.*`, `Extras/`); `tests/test_spec.py` checks this.
No model, network or randomness is involved: the same text always gives the same spec.

## The IR (schema version 1)

```lisp
(engineering_spec
  (version 1)
  (name "divider")
  (source handwritten)                    ; or (source nlp "original text")
  (components
    (component V1 (role supply) (value 5 V))
    (component R1 (role resistor) (value 10000 ohm) (tolerance 0.01) (package "0603"))
    (component U1 (role mcu) (family "ESP32") (variant "ESP32-WROOM-32E"))
    (component D1 (role led) (color "red") (part "red_led")))
  (connections
    (connect V1 + VIN) (connect V1 - GND)
    (connect R1 1 VIN) (connect R1 2 OUT inferred))
  (requirements (voltage OUT (min 3.2 V) (max 3.4 V)) (current R1 (max 0.01 A)))
  (gpio (drive U1 GPIO2 D1))
  (board (width 40e-3 m) (height 30e-3 m) (layers 2))
  (analyses (dc) (ac (start 10 Hz) (stop 100e3 Hz) (points 50)) (transient (step 1e-6 s) (stop 1e-3 s) (method trap)))
  (profiles (catalogue "my.parts") (dfm "../profiles/conservative.rules"))
  (unresolved (question q1 "U1.variant" "Which exact ESP32 module ...?" ("ESP32-WROOM-32E module" ...))))
```

* **Units** are SI: `ohm F H V A W Hz s m`. A dimensionless quantity (tolerance as a fraction,
  layers, points) has no unit atom. Values are numbers; `nan`/`inf` parse but are rejected.
* **Provenance** is the last atom of a value, attribute or connection: `explicit` (default when
  omitted), `inferred` (read from context, e.g. a unit implied by the role, the supply's `-`
  terminal on GND, LED polarity from "from A to B"), `defaulted` (chosen because nothing was
  said, e.g. an automatic reference), `imported`, `catalogue-derived`. `(provenance P)` on a
  component describes the component itself.
* **Roles** and their contract:

  | Role | Ref | Value | Terminals | Bundled part used by `lower` |
  |---|---|---|---|---|
  | resistor | R | ohm, required, > 0 | 1 2 | `resistor` |
  | capacitor | C | F, required, > 0 | 1 2 | `capacitor` |
  | inductor | L | H, required, > 0 | 1 2 | `inductor` |
  | diode | D | none | 1/cathode/k, 2/anode/a | `diode` |
  | led | D | none | as diode | none: bind `(part ID)` |
  | supply | V | V, required | 1/+/positive, 2/-/negative | `voltage` |
  | current_source | I | A, required | as supply | `current` |
  | connector, mcu, sensor | J, U, U | none | any pin name | none: bind `(part ID)` |
  | mosfet, bjt | Q | none | any pin name | none: bind `(part ID)` |

  Terminal names are canonicalized on validation (`anode` → `2`, `+` → `1`).
* **Attributes**: `package`, `color`, `family`, `variant`, `part`. `part` names a catalogue
  part id (in English: `J1 uses part header3`); nothing else selects a part.

### Validation

Errors (INVALID_INPUT) — the spec says something wrong: unknown role/field/unit, duplicate
reference, wrong reference prefix, unit mismatch (`10 V resistor`), non-finite value, zero or
negative value where it must be positive, terminal the role does not have, one terminal on two
nets, connection or requirement to an unknown component/net, min > max, unknown analysis
parameter, unparseable `.espec` text, and in English any clause the grammar does not accept.

Questions (UNVERIFIED) — the spec leaves a decision open: a required value omitted, an
unconnected terminal, a component with no connections, an MCU named only by family (`ESP32`,
`ESP32 module`), a power input without a voltage and net (`powered by USB`), a diode placed
"between" two nets (polarity unknown), GPIO intent with several MCUs, no `GND` net.
**An omitted value is a question; an explicit invalid value is an error.** Nothing is filled in.

A question carried in `(unresolved ...)` closes when what it asks is stated (the value, the
terminal connections, the attribute, the GND net, the GPIO owner). `power.input` closes only
when you delete it after adding the supply.

### Fingerprint

`fingerprint` is the SHA-256 of the canonical semantic content: components (ref, role, value,
tolerance, attributes), canonical connections, requirements, GPIO, board, analyses and profiles,
sorted. Provenance, name, source text and questions are excluded, so a hand-written spec and the
English description of the same circuit have the same fingerprint, and reordering a file does
not change it. `tests/fixtures/spec/divider.espec` and the first corpus entry demonstrate this.

### Handoff to the compiler (`lower`)

Only a spec with no errors and no questions is lowered. Deterministically:

* each component binds `(part ID)` if given, else the bundled generic part for its role; the
  part must exist in the catalogue overlay (`--catalogue`, else the spec's catalogue profile,
  over `profiles/default.parts`). A role with no bundled part and no binding is UNVERIFIED;
* nets are written with `GND` first; pins come from the canonical connections;
* the board is the spec's width/height (default 40 × 30 mm, reported), 1.6 mm, 35 µm copper;
  only two layers are supported (anything else is UNVERIFIED);
* voltage requirements become `(targets (voltage NET MIN MAX))`; a current requirement cannot be
  checked yet and makes the lowering UNVERIFIED rather than being dropped;
* the DFM profile is embedded as `(rules ...)`;
* GPIO intent, tolerance and requested analyses are reported as notes; no `(at ...)` placement
  is invented.

The written project is loaded by `copper-core analyze` before it replaces `--out`; profile
paths are relative to the `.espec` file.

## Constrained English

Text is split into clauses at `.` `;` newlines, commas and "and" before `a/an/the/connect/...`
or a reference. Pass 1 reads declarations, board, analyses and profiles; pass 2 reads
connections, GPIO and requirements, so clauses may come in any order. Every clause must match
one of these forms (words in brackets are optional):

| Clause | Examples |
|---|---|
| component | `a 10k resistor R1 from VIN to OUT`, `R2 is a 20 kohm 0603 resistor between OUT and ground`, `a 100 nF capacitor C1 between OUT and GND`, `a red LED from SIG to GND`, `a 1% 0603 resistor`, `a 4k7 resistor in 0805 package from A1 to GND` |
| supply | `a 5 V supply on VIN` (− on GND, inferred), `V1 is a 3V3 supply between VDD and GND`, `powered from 5 V on VIN` |
| MCU | `an ESP32-WROOM-32E module U1`, `use an ESP32` (asks for the variant), `an RP2040 chip` |
| connection | `R1 pin 1 to VIN`, `R1.2 to GND`, `connect the LED anode to OUT`, `D1 cathode to GND`, `R1 between VIN and OUT` |
| part | `J1 uses part header3`, `use part header3 for J1`: binds a catalogue part id (the only way English selects a part) |
| GPIO intent | `GPIO2 drives the LED`, `U1 GPIO4 drives D1`, `drive the red LED from GPIO5` |
| requirement | `OUT must be between 3.2 V and 3.4 V`, `the voltage at OUT should stay within 3.2 V and 3.4 V`, `OUT must be 3.3 V ± 3%`, `VIN must be at least 4.5 V`, `the current through R1 must be below 10 mA` |
| board | `the board is 40 x 30 mm with 2 layers`, `board: 40 mm x 30 mm`, `two layers` |
| analysis | `run a DC analysis`, `simulate dc`, `run an AC sweep from 10 Hz to 100 kHz with 50 points`, `transient with step 1 us for 1 ms using trapezoidal` |
| block | `use block i2c_pullups: vdd=VDD scl=SCL sda=SDA mode=fast bus_capacitance=100pF`: one clause, `key=value` pairs; see [BLOCKS.md](BLOCKS.md) |
| profile, name | `use catalogue my.parts`, `use dfm profile ../profiles/conservative.rules`, `name: divider` |

* **Quantities**: SI prefixes `p n u µ m k K M meg G`, units `ohm Ω F H V A W Hz s mm cm mil %`,
  RKM codes `4k7`, `4r7`, `3V3`, `4n7`. `m` is milli (`10m` resistor = 10 mΩ); lengths need
  `mm`/`cm`. A number with no unit takes the role's unit with provenance `inferred`.
  `nan`, `inf` and negative numbers are read so that validation can reject them explicitly.
* **References** are `R C L D V I J U Q` + digits. Omitted references are assigned in order,
  skipping every reference written anywhere in the text (`defaulted`).
* **Nets** are upper-case names of two or more characters (`VIN`, `OUT`, `IO2`, `LED_A`); `ground`
  and `gnd` mean `GND`. A name that starts with a digit (`3V3`) is a quantity, not a net.
* **Coreference**: `the LED`, `the resistor`, `the ESP32`, `the red LED` name the single matching
  component; zero or several matches is an error asking for a reference.
* **Polarity**: for a diode or LED, `from A to B` puts the anode on A (`inferred`); `between`
  asks. For a source, `on N` / `between N and M` puts `+` on the first net.

## Tests

`tests/test_spec.py` (CTest `engineering_spec`) covers the corpus in
`tests/fixtures/spec/corpus.json`, quantity aliases (`1uF`, `1 µF`, `100n`, `3V3`, `3.3 V`,
`4k7`, `kΩ`), explicit invalid values versus omitted ones, ESP32 and power-input ambiguity,
malformed and contradictory connections, the handwritten/English fingerprint equivalence,
`.espec` round-trips and structural errors, lowering accepted by copper-core, refusal to
lower incomplete or unbindable specs, and the independence from the retired language code.
`tests/test_pcba_cli.py` covers the `pcba spec` commands, their exit codes and JSON envelope.
