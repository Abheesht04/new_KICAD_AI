# Circuit blocks

`tools/blocks.py` holds reusable circuits. A spec declares a block, and lowering (`pcba spec lower`, `project
build`) expands it into ordinary components, connections and no-connects, which copper-core then checks like any
other part of the design. `pcba blocks` lists the blocks and their parameters.

```text
use block i2c_pullups: vdd=VDD scl=SCL sda=SDA mode=fast bus_capacitance=100pF
```

```lisp
(blocks (block i2c_pullups (vdd "VDD") (scl "SCL") (sda "SDA") (mode "fast") (bus_capacitance 1e-10 F)))
```

## Every value has a source

| Source | Meaning |
|---|---|
| `standard` | a cited specification, with the formula applied to the block's parameters |
| `part` | the bound catalogue part's datasheet data: its `(reference ...)` entry, whose `(provenance ...)` is mandatory (copper-core refuses one without it) |
| `param` | a parameter the user stated |

Each added value is listed in the lowering notes with its source and provenance text.

**Fail-closed rules:**
- A missing required parameter is a question (UNVERIFIED), never a default. For example, no bus capacitance is assumed.
- A part without the datasheet data a block needs is refused.
- No part is chosen: parts are catalogue ids the user names, and no regulator, connector, sensor or button is bundled, since bundling one would mean inventing it.
- The checks a block cannot make itself belong to copper-core, which runs them on the lowered project.

## Blocks

| Block | Parameters | What it adds, and from where |
|---|---|---|
| `reference_circuit` | `component` | The component's datasheet reference circuit (`decouple`, `pull_up`, `cap_to_ground` entries) between its own supply and ground pins. `pcba design` uses the same code for the MCU (`blocks.reference_parts`); the ESP32 design is byte-identical. |
| `i2c_pullups` | `vdd scl sda mode bus_capacitance [vdd_voltage]` | Two pull-ups. Rp(min) = (VDD − VOL)/IOL and Rp(max) = tr/(0.8473·Cb), per the NXP UM10204 I2C-bus specification, pull-up resistor sizing: VOL 0.4 V at IOL 3 mA (standard, fast) or 20 mA (fast-mode plus); tr 1000/300/120 ns. The block takes the largest E24 value in range (via copper-core's `eseries ... at_most`). With no value in range the result is INVALID_INPUT, naming the range. VDD comes from the spec's supply on `vdd` or from `vdd_voltage`. |
| `usb_c_sink` | `connector vbus gnd` | The receptacle's VBUS/GND pins, from the part's `(reference (vbus ..) (gnd ..) (cc1 ..) (cc2 ..) [(nc ..)])`. A 5.1 kΩ Rd from each CC pin to ground, per the USB Type-C Cable and Connector Specification (sink, default USB power, ±20 %). VBUS is a boundary: state the supply (e.g. "a 5 V supply on VBUS") for DC analysis. |
| `ldo` | `regulator vin vout gnd` | A kind U part with `(model ideal.ldo)`, its datasheet `dropout_v` and `iout_max_a`, and `(reference (in ..) (out ..) (gnd ..) (output_v ..) (input_cap ..) (output_cap ..))`. The output voltage and both capacitors come from the part. |
| `i2c_sensor` | `device vdd gnd scl sda` | An I2C device: its `(reference (supply ..) (ground ..) (scl ..) (sda ..) [(nc ..)])` pins on the bus, and its datasheet decoupling. |
| `reset_boot` | `mcu button boot_pin` | Two buttons (`(reference (a PIN) (b PIN))`), each pulling a pin to ground. The reset pin comes from the MCU's catalogue `(reset PIN active_low)`. The boot pin must be one of its catalogue strapping pins, and that strap's note is the provenance (for the ESP32 module: "IO0 selects the boot mode: low at reset enters serial download"). |

## Regulator model and IC boundaries (copper-core)

**`ideal.ldo` (kind U, family `regulator`, DC and transient):**
- **Model:** V(out) − V(gnd) = vout. The input supplies the output current plus `iq_a` (0 when not given).
- **Checks after the DC solve,** against the part's datasheet values:
  - `regulator_dropout`: Vin < vout + `dropout_v`;
  - `regulator_current`: the load exceeds `iout_max_a`;
  - `regulator_reverse`: the circuit drives current into the output, which a linear regulator does not sink;
  - dissipation (Vin − Vout)·Iout + Vin·Iq against the part's `power_w` (derated), or `rating_unknown` when it is not given.
- **Not modelled:** line/load regulation, PSRR, current limit, thermal shutdown, start-up, and any AC model.
- **Per-net current bounds** (layout widths) see the regulator's exact input and output currents.
- **Binding:** a kind U part binds only a regulator model; any other U stays an unmodelled IC.

**IC boundary:** an unmodelled kind U part whose `(reference (supply ...))` declares it, such as a sensor, is a DC
boundary, like a connector. It gets an `ic_supply_current` warning, so the result stays UNVERIFIED. Any other
unmodelled U still blocks analysis (`unsupported_model`).

## Tests

`tests/test_blocks.py` (CTest `circuit_blocks`) covers:
- the pull-up values for each mode;
- the impossible-bus refusal and the missing-capacitance question;
- spec round trip and fingerprint;
- USB-C sink, regulator (values, bounds, dropout, over-current, reverse current), and USB-C → LDO → sensor with pull-ups;
- ESP32 reference circuit and reset/boot, including a non-strapping boot pin;
- parts without datasheet data;
- the kernel's checks on `(reference ...)` and regulator binding.

The non-generic parts are TEST ONLY stand-ins with synthetic numbers (`tests/fixtures/blocks/test_parts.parts`).
For a real board, add the real parts with their datasheet values and provenance (`pcba parts add` / `parts import`).

## Not done

- No bundled regulator, USB-C receptacle, sensor or button: real parts with datasheet data are needed.
- No VBUS bulk capacitance, ESD protection, I2C address conflict check, regulator AC model or thermal model.
