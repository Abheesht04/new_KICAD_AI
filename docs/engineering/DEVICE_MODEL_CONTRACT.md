# Device model and part contracts (Prompts 2 and 3)

Contract version: **3** (`DEVICE_MODEL_CONTRACT_VERSION`). v1 (Prompt 2) defined DC and
transient; v2 (Prompt 3) adds the AC small-signal hook; v3 (Prompt 5) adds declared
integration methods and state meaning (see [TRANSIENT.md](TRANSIENT.md)). This document covers the device
model registry, the `.copper` part/component schema additions, ownership and lifetime, and
what is deliberately not implemented yet.

## What is kept separate

| Concept | Where it lives | Identified by |
|---|---|---|
| Device model: equations, terminals, parameters, capabilities | `DeviceModelSpec` in `src/model_registry.h`; built-ins in `src/device_models.c` | model id + version, e.g. `shockley.diode` v1 |
| Catalogue part: what you place and buy | `(part ...)` in a catalogue or a project's `(parts ...)` | part id, optional `manufacturer`/`mpn`/`datasheet` |
| KiCad symbol | `symbol` field, resolved by `tools/copper.py` | `Library:Name` |
| KiCad footprint | `footprint` field, resolved by `tools/copper.py` | `Library:Name` |
| Logical terminal → symbol pin → pad | `(pinmap ...)` on the part (default for two-terminal models only: terminal *i* → pin *i+1*; models with more terminals must map explicitly) | terminal names from the model |
| Project-specific data | `(override ...)` on a component | component reference |

A part binds to a model **by id** and maps the model's terminals to symbol pins
**by name**. A symbol or footprint never selects a model, a package never implies a rating,
and the MPN is procurement metadata only.

## Registry (C API, `src/model_registry.h`)

The existing `ModelRegistry` owner is extended. It keeps the legacy `ModelOps` validation
hooks (used only by the V1 `Design` path) and now also holds `DeviceModelSpec` pointers.
There is no second registry and no second solver: every spec stamps its equations through
the existing `MnaDeviceOps`/`MnaAssembly` engine.

| Function | Purpose |
|---|---|
| `device_model_validate_spec` | Contract-v1 checks (below) |
| `model_registry_register_device` | Validate, then register; rejects duplicate ids |
| `model_registry_find_device`, `_device_count`, `_device_at` | Lookup and iteration (registration order) |
| `model_registry_register_builtin_devices` | The six built-in V2 models |
| `device_model_resolve_params` | Apply defaults, reject missing / non-finite / out-of-range values |
| `device_model_supports` | Capability query per analysis (`dc`, `transient`, `ac`) |
| `device_model_terminal_index`, `device_terminal_accepts_pin_name` | Terminal lookup; KiCad pin-name hints |
| `device_unit_parse`, `device_unit_symbol` | The SI units a parameter may declare |

`device_model_validate_spec` rejects:
- an id outside `[A-Za-z0-9_.-]{1,63}`, version 0, or a missing family, summary or assumptions;
- zero or more than 8 terminals, or duplicate terminal names;
- more than 8 parameters, duplicate parameter names, an unknown unit, a non-finite range or
  `minimum > maximum`, or a default outside its range (including on an exclusive minimum);
- an out-of-range `primary_param`;
- the temperature capability, which has no hook yet, so no model can claim it;
- a capability declared without its stamp function, or the reverse; transient or AC without DC;
- `ac_branch_count`, `ac_excitation` or `needs_operating_point` without the AC capability;
- transient state without a commit function or the transient capability, or the reverse.

**Ownership and lifetime.** Specs, and their terminal and parameter arrays, are static and
owned by the caller; they must outlive the registry. The registry stores pointers only, and
`model_registry_free` frees only its own arrays. Every `CpProject` owns a registry, filled
with the built-ins in `cp_init` and released in `cp_free`.

**Failure semantics.** Validation and lookup functions return 0/NULL and write a reason into
the caller's buffer; they never abort. Stamp functions return 0 only on allocation failure.
`assemble()` in `src/v2/project.c` reports one of:
- `unsupported_model`: kind U, which has no model;
- `unsupported_analysis`: the model lacks the capability for the requested analysis;
- `model_stamp`: allocation failure.

Each blocks the analysis. The first two map to UNVERIFIED in pcba; nothing is silently
omitted.

**Transient state.** At most 2 values per instance. A model reads the previous accepted state
through `DeviceStampContext.state`. `commit_transient` computes the next state only after the
step has solved and validated. A failed step leaves the accepted state untouched, because the
run stops.

## AC hook (contract v2)

`stamp_ac(MnaAcSystem*, nodes, params, DeviceAcContext*)` stamps a model's small-signal
equations at angular frequency `omega` into the complex system from `src/mna_ac.h`. The
context carries:
- the DC operating point, which is passed only when the spec sets `needs_operating_point`;
- the AC excitation phasor, which is zero unless the component declares `(ac MAG PHASE)`;
- the first of the instance's `ac_branch_count` branch unknowns.

A spec that sets `ac_excitation` is an independent source and may receive `(ac ...)`.
Linearisation must come from the model's own equations: the diode uses
`mna_diode_evaluate`, which is the same Shockley evaluation DC uses. Capacitive
behaviour must come from the model's own charge or capacitance, never from a DC Jacobian.

## Built-in models (all v2; DC, transient and AC)

| Id | Kind | Terminals (default pins) | Parameters (unit, range, default) |
|---|---|---|---|
| `ideal.resistor` | R | p (1), n (2) | resistance Ω, (0, 1e12], from `value` |
| `ideal.capacitor` (v3) | C | p (1), n (2) | capacitance F, (0, 1e12], from `value`; state [v, i]; Backward Euler or trapezoidal |
| `ideal.inductor` (v3) | L | p (1), n (2) | inductance H, (0, 1e12], from `value`; state [i, v]; Backward Euler or trapezoidal |
| `shockley.diode` | D | cathode (1), anode (2) | is_a A [1e-30, 1] default 1e-14; ideality [0.1, 10] default 1 |
| `ideal.voltage_source` | V | positive, negative | voltage V, [-1e12, 1e12], from `value` |
| `ideal.current_source` | I | p, n | current A, [-1e12, 1e12], from `value` |
| `ebers_moll.npn` / `.pnp` (v1) | Q | collector, base, emitter (explicit pin map) | is_a, beta_f, beta_r, nf, nr (SPICE defaults) |
| `level1.nmos` / `.pmos` (v1) | M | drain, gate, source (explicit pin map) | vto (required), kp, w, l, lambda |
| `ideal.ldo` (v1) | U | in, out, gnd (explicit pin map) | vout V from `value`; dropout_v V (required), iout_max_a A (required), iq_a A default 0. DC and transient; limits checked after the solve ([BLOCKS.md](BLOCKS.md)) |

Q and M parts must name their model; see [NONLINEAR_MODELS.md](NONLINEAR_MODELS.md).

DC and transient equations are exactly those previously inlined in `project.c`; v2 added
the AC stamps, so every built-in is now version 2:
- R: G = 1/R.
- C: jωC.
- L: branch row `V(p) − V(n) − jωL·I = 0`.
- D: dI/dV at the operating point. There is no capacitance, so this is valid only where junction
  capacitance is negligible.
- V: branch row whose right-hand side is the excitation.
- I: injected excitation.

Each spec's
`assumptions` text lists what it leaves out (ESR, junction capacitance, breakdown and so on).
`copper-core models` prints all specs as JSON.

The diode defaults are generic, not datasheet values. Diode terminals carry KiCad pin-name
hints (`K|C|CATHODE|KATHODE`, `A|ANODE`). Passives and sources accept any name.

## `.copper` schema additions (still `(version 2)`)

Part fields, in addition to `kind symbol footprint power_w voltage_v tolerance height_mm
manufacturer mpn datasheet`:

| Field | Meaning |
|---|---|
| `(model <id>)` | Bind a registered model. Its family must match the kind; J and U cannot bind one. Default: the kind's built-in model |
| `(pinmap (<terminal> <pin> [<pad>]) ...)` | Map every model terminal exactly once to a distinct symbol pin. A pad, if given, must equal the pin: KiCad pairs symbol pin N with footprint pad N, so a remap needs a dedicated symbol or footprint |
| `(<param> <value>)` | Non-primary model parameters, e.g. `(is_a 1e-12)` on a diode. A parameter the bound model doesn't have is rejected |
| `(provenance "...")` | Where the part's data comes from |
| `(reference (KEY VALUE ...) ... (provenance "..."))` | Datasheet application data that circuit blocks use (pin roles, reference-circuit values); its provenance is mandatory. On an unmodelled U, `(supply ...)` makes it a declared DC boundary |
| `(catalogue "...")` | Which catalogue the definition came from. `project new` writes it |

Ratings and height (`power_w`, `voltage_v`, `height_mm`): an absent field, or `unknown`, is
**UNKNOWN**. A value must be finite and positive. **An explicit `0` is rejected** ("zero is
not a rating"). Unknown values keep producing `rating_unknown`/`height_unknown` warnings, so
verification stays UNVERIFIED. They are never filled in from a package or footprint.

Components:
- Pins are matched to terminals **by name** through the pin map, so the order inside
  `(pins ...)` no longer matters.
- A component of a modelled kind must list exactly the mapped pins.
- A two-terminal model cannot have both terminals on one net.
- `(value ...)` is required whenever the model has a primary parameter.
- `(override (power_w ...) (voltage_v ...) (height_mm ...) (tolerance ...) (provenance "..."))`
  replaces part data for that component only. `provenance` is mandatory. Identity fields
  (symbol, footprint, model, MPN) cannot be overridden; use a different part.

**Precedence** is deterministic: component override > part definition > UNKNOWN. Each known
value records its source (`part:<id>` or `override:<ref>`). The analysis report's
`components` array lists, per component:
- model id, version and source (`explicit` or `kind_default`);
- capabilities;
- the terminal → pin map;
- resolved parameters;
- ratings, as known/value/source;
- identity, provenance and catalogue.

## Catalogues (`tools/copper.py`)

- `project new --catalogue FILE` overlays FILE on the bundled `profiles/default.parts`. A user
  part replaces the bundled part with the same id, in place; new ids follow in file order.
- Both files are validated by `copper-core catalogue FILE`, using the same rules as a
  project's parts, before anything is written.
- `part-add` writes only the fields you supply; unknown ratings are omitted, never written
  as 0. It validates the whole resulting catalogue before replacing the file.
- `part-remove` deletes one id.
- The bundled default catalogue is read-only to both commands.
- A project embeds a snapshot of its parts. Editing a catalogue never changes an existing
  project.

## Symbol pin-name check

During `build`, each mapped symbol pin's KiCad name is compared with the model's terminal
hints. A name that belongs to a *different* terminal (for example `A` on the pin mapped to
`cathode`) is rejected as a swapped pin map. Unnamed pins, and names no terminal claims, are
accepted, because they carry no evidence either way.

## Not implemented (deliberately)

- **Temperature hook.** It is a reserved capability bit, and registration rejects it.
- **Diode junction/diffusion capacitance, BJT, MOSFET.** Prompt 4.
- **User-defined equations.** There is no dynamic C loading and no declarative equation
  language. Users bind data-only parts to the registered models. A compiled-extension ABI is
  a later, separate decision.
- **Multi-unit symbols, one pin with several pads, symbol pin ≠ pad number.** These are
  rejected rather than guessed.
- **External catalogue import** (`pcba parts import`, CSV/JLCPCB formats). It remains
  NOT_IMPLEMENTED (Prompt 7).
- **Tolerance as a known/unknown quantity.** Tolerance still uses 0 = no corner sweep, and
  its absence is not reported. **Ratings for C, L and D** other than voltage are not checked.
- **The legacy V1 `Design` path** (`mna_from_design.c`, `ModelOps`) is unchanged and does
  not use device specs yet. It still has its own per-kind switch.
- **`value` on D/J/U.** It is accepted and unused, as before.
