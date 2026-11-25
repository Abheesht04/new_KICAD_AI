# Circuit block format `copper.block/1` (Phase 2)

A **block** is a small circuit with a fixed topology: an LED with its resistor, an LDO with its capacitors, an ESP32
module with its reset circuit. Blocks are **data** (JSON, one file per block). The C module `src/v2_src/blocks.c` loads
and checks a block and designs nothing. The composer (Phase 2c) will join checked blocks into a design that the existing
pipeline compiles, routes and checks.

This replaces the fixed templates of `template_gen.c` step by step. The templates stay working (PIPELINE_HARD_RULES.md
H10). `tools/blocks.py` (legacy Python, `pcba`) is the reference for porting existing blocks, not their format.

## Format

The full key list is in the header comment of `src/v2_src/blocks.h`. In short:

| Section | Contents | Rules |
|---|---|---|
| `format`, `name`, `summary` | `"copper.block/1"`, an identifier, a text | required |
| `sources` | `{id, document, url or standard, revision, page}` | at least one; every default, power-port range, formula and note names one (H2) |
| `parameters` | `{name, unit, required, default, source, min, max}` | integers in the named unit; a required parameter has no default (it comes from the request); an optional one has a default **and its source** |
| `ports` | `{name, kind, voltage_min, voltage_max, voltage_parameter, source}` | kinds: power_in, power_out, ground, gpio, i2c_scl, i2c_sda, uart_tx, uart_rx, analog, signal; power_in needs its range and source; power_out needs a uV parameter or a range |
| `nets` | internal net names | each joins at least two pins |
| `parts` | `{role, prefix, select, pins}` | `select` is a catalogue `part_id`, or `kind` + `package` (+ `value` from a formula or parameter for R, C, L); `pins` maps every terminal of the part to a port, a net or `"nc"` |
| `formulas` | `{name, function, source, inputs}` | `function` comes from the fixed C table below; the inputs are parameters with the right unit |
| `notes` | `{text, source}` | design notes that carry a source |

**Units:** uV, uA, uW, uohm, pF, nH, mC (milli-degrees C), ppm, ns. All numbers are integers: no decimals, no
exponents.

**Reference prefixes per part kind:**

| Kind | Prefix |
|---|---|
| R | R |
| C | C |
| L | L |
| D | D |
| Q, M | Q |
| U | U |
| J | J |
| S | SW |

## Keys for composition (added in Step 2c)

| Key | On | Meaning |
|---|---|---|
| `"from": {"port", "quantity"}` | a required parameter | the composer derives it from what the port is connected to: `voltage_min` / `voltage_max` of the net, or the `current` drawn from a power_out's net |
| `current` + `current_source` | power_in port | the current the block draws (µA), with its source |
| `current_follows` | power_in port | a regulator's input also carries whatever its output (a power_out port of the same block) delivers |
| `current_max` + `current_source` | power_out port | what the source can deliver (µA), with its source |
| `level_port` | signal ports (gpio, uart, i2c, analog, signal) | the power port whose voltage the signal swings to (an ESP32 GPIO swings to its 3V3 rail) |
| `pullup: true` | i2c_scl / i2c_sda ports | the block puts pull-ups on this line; one pair per bus |
| `external_supply: true` | power_out / ground ports | power enters the board here from outside (a connector or USB). The composer lists the port's net under `power_flags` in `layout_hints.json`, and `copper design schematic-rules` puts one KiCad PWR_FLAG on it (Step 3d). Only nets fed from outside get a flag: a power net no output drives stays an ERC error. |

## Formula functions (`block_functions()`)

| Function | Output | Inputs | Formula and source |
|---|---|---|---|
| `led_series_resistor` | uohm | v_drive uV, v_forward uV, current uA | R = (V_drive − V_F) / I_F, rounded up to the next E24 value (Ohm's law) |
| `i2c_pullup` | uohm | vdd uV, vol uV, iol uA, rise_time ns, bus_capacitance pF | NXP UM10204: Rp(min) = (VDD − VOL) / IOL, Rp(max) = tr / (0.8473 Cb); the largest E24 value in range |
| `ldo_dissipation` | uW | vin uV, vout uV, current uA | P = (V_in − V_out) · I_out |
| `rc_time_constant` | ns | resistance uohm, capacitance pF | τ = R · C |

Step 2a only checks that formulas are declared correctly. The composer computes them (Phase 2c).

## Checking a block

```
copper block check blocks_library/<name>.block.json --parts parts_library/<file>.parts [--parts ...] [--out report.json]
```

Exit codes:
- **0 VALID:** every check ran and passed.
- **3 UNVERIFIED:** no `--parts` catalogue was given, so the part references were not checked.
- **2 INVALID:** the findings are listed with their JSON path.
- **64:** usage error.
- **70:** internal error.

**Findings:**

| Group | Codes |
|---|---|
| Structure | JSON, TYPE, FORMAT, UNKNOWN_KEY, MISSING_KEY, BAD_NAME, DUPLICATE, CONFLICT |
| Values | BAD_NUMBER, BAD_UNIT, BAD_RANGE, BAD_KIND |
| Sources | NO_SOURCE, UNKNOWN_SOURCE |
| Names | UNKNOWN_PARAMETER, UNKNOWN_FORMULA, UNKNOWN_FUNCTION, BAD_INPUT, UNIT_MISMATCH |
| Connections | UNKNOWN_NET, UNUSED_PORT, SINGLE_ENDED_NET |
| Catalogue (with `--parts`) | UNKNOWN_PART, KIND_MISMATCH, TERMINAL_MISMATCH, NO_CATALOGUE_MATCH |
| File | IO |

## Tests

`src/v2_src/tests/test_blocks.c` (ctest `copper_blocks_tests`):
- **Inputs:** a valid fixture block (`tests/fixtures/blocks/led_indicator.block.json`, Copper-written) checked against
  the real `parts_library` catalogues.
- **Negative cases:** 29 of them, each changing one thing and expecting the matching finding. One more case shows that
  an unknown part is UNVERIFIED, not VALID, when no catalogue is given.
- **Mutation check:** with the terminal check disabled, the test fails.
