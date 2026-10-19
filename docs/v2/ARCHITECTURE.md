# V2 architecture and extension guide

## Preserved design philosophy

Your `u128` implementation builds a portable full 64x64 product from four 32-bit
products. It carries high/low words explicitly rather than assuming a compiler's
128-bit extension. `geom` rewrites clearance predicates as integer comparisons;
`copper` expresses point/segment/rectangle cores plus a radius. The new workflow
keeps those engineering decisions deterministic. Floating-point gap measurements
are for reports. MNA remains numerical floating-point linear/nonlinear analysis,
which is a different problem from exact geometric clearance.

The original `u128.c/.h` and `copper.c/.h` are unchanged byte-for-byte. The adapter
`pcb_copper.c` now maps axis-aligned rectangular, rounded and oval pads into the
existing core-plus-radius representation. Corners are real empty space for
connectivity: the checker does not turn a rounded pad into a solid rectangle.
The router uses conservative boxes to propose candidates; exact checks determine
whether those candidates pass.

## Ownership and authority

1. `src/v2/transformer.c` proposes one category from a short English phrase.
2. A `.copper` file declares component values, symbol/footprint IDs and every pin/net.
3. `src/v2/project.c` parses this file using your existing `IR.c` and arena.
4. It validates references/limits and stamps your existing extensible MNA engine.
5. `tools/copper.py` reads local KiCad libraries, resolves symbols, loads actual
   footprints, and generates a deterministic layout candidate.
6. It writes `resolved.copper` with the exact supported geometry and asks the C
   core for physics/DFM findings before producing a manifest.
7. KiCad 9 parses the outputs and performs ERC, DRC, schematic parity, and an
   independently extracted netlist comparison with the original electrical IR.
8. The desktop uses this same pipeline. It does not own another physics engine.

The original `Design`/part-provider APIs and legacy CLI are preserved. The V2
`CpProject` is an explicitly bounded typed projection of the general S-expression
IR, introduced to support arbitrary declared pin names and stable snapshots of
catalogue/rule data. Both paths share MNA, geometry, copper and PCB checks. The
legacy MNA bridge now rejects unsupported devices rather than dropping them.

The C core has no Python dependency. The KiCad adapter and candidate router use
Python's standard library. CMake does not fetch libraries or execute training.
The optional UI uses C++17, GLFW, Vulkan and vendored Dear ImGui.

## Source map

| File | Responsibility |
|---|---|
| `src/v2/project.h/.c` | Typed project parser, model stamps, reports, transient/corners, advanced physical rules |
| `apps/copper_core.c` | Machine-readable C command line |
| `src/v2/transformer.c` | Bounded, local self-attention inference |
| `src/v2/intent_weights.h` | Trained weights and vocabulary |
| `tools/train_intent.py` | Inspectable synthetic training set and weight export |
| `tools/copper.py` | Project creation, catalogue edits, library loading, routing, export and native gates |
| `tools/hwc_kicad.py` | Existing full-library indexing, searching, doctor and custom-symbol tools |
| `desktop/main.cpp` | Vulkan instance/device/swapchain/render lifecycle, adapted from Dear ImGui's example |
| `desktop/studio.cpp` | Project editor, explorer, actual board canvas, worker jobs, output panel |
| `tests/test_v2.py` | End-to-end behavior, numerical oracles, negative cases, determinism and native gate tests |
| `tests/v2_geometry_test.c` | 50,000 u128 oracle products and exact rounded-pad boundary cases |

## `.copper` schema

Values are SI for component models; physical dimensions are millimetres.
An example with explicitly supplied illustrative ratings:

```scheme
(copper_project
 (version 2)
 (name "divider")
 (nets GND VIN OUT)
 (board 40 30 1.6 0.035 2)
 (parts
  (part r (kind R) (symbol "Device:R")
   (footprint "Resistor_SMD:R_0603_1608Metric")
   (power_w 0.1) (voltage_v 50) (tolerance 0.01))
  (part supply (kind V)))
 (components
  (component V1 (part supply) (value 5) (pins (1 VIN) (2 GND)))
  (component R1 (part r) (value 10000)
   (pins (1 VIN) (2 OUT)) (at 16 12 0))
  (component R2 (part r) (value 20000)
   (pins (1 OUT) (2 GND)) (at 24 12 0)))
 (rules (clearance_mm 0.2) (power_derating 0.5))
 (targets (voltage OUT 3.28 3.38)))
```

An actual board should expose input/output boundaries through physical connectors;
the supplied divider adds J1. V/I are simulation boundary sources and never become
BOM parts or physical footprints. J is an explicit ideal connector boundary; U
means an active device whose electrical model has not been implemented.

Supported model kinds:

| Kind | `value` | DC | Transient |
|---|---|---|---|
| R | Ohms > 0 | Ohm's law | Ohm's law |
| C | Farads > 0 | Open circuit | Backward Euler companion |
| L | Henries > 0 | Ideal short with current unknown | Backward Euler companion |
| D | Unused | Shockley, pin 1 cathode / 2 anode | Memoryless Shockley |
| V | Volts | Ideal source, pin 1 positive | Constant source applied at start |
| I | Amperes | Current from pin 1 to pin 2 | Constant source applied at start |
| J | Unused | Explicit connector boundary | Explicit connector boundary |
| U | Unused | Unsupported; block solve | Unsupported; block solve |

Each modelled kind binds a versioned device model (`copper-core models` lists them; see
[DEVICE_MODEL_CONTRACT.md](../engineering/DEVICE_MODEL_CONTRACT.md)). R/C/L/V/I components
must state `(value ...)` explicitly; an omitted value is rejected, never defaulted (an
explicit `0` V/A source is accepted). Model terminals are matched to pins by name through
the part's `(pinmap ...)`, which defaults to terminal *i* → pin *i+1* (diode: cathode 1,
anode 2). A component must list exactly the mapped pins, in any order, and a two-terminal
part cannot short its own terminals. Other pin labels may be alphanumeric and must
match the symbol and footprint exactly. All pins must be assigned; intentional
no-connect pins are not yet represented. Ground is the first declared net `GND`.
MNA uses node 0 for ground, while physical copper uses `net_index + 1`, reserving
physical net 0 for genuinely unassigned copper.

Part attributes: `kind`, `model`, `pinmap`, `symbol`, `footprint`, `power_w`, `voltage_v`,
`tolerance` (fraction), `height_mm`, `manufacturer`, `mpn`, `datasheet`, `provenance`,
`catalogue`, and the bound model's other parameters (diode: `is_a`, `ideality`). Ratings and
height are UNKNOWN when absent or written `unknown`; an explicit 0 is rejected. Diode
parameters default to a generic idealized model, not a manufacturer datasheet. A component may
add `(override (power_w ...) (voltage_v ...) (height_mm ...) (tolerance ...) (provenance "..."))`,
which takes precedence over the part's data for that component only.
`project new --catalogue FILE` overlays your catalogue on the bundled default (same id
replaces). No provider infers procurement data from KiCad geometry.

Project fields: `version`, `name`, `nets`, `parts`, `components`, `board`, `rules`,
`geometry`, `targets`. Unknown or duplicate structured fields are rejected.
Maximum: 256 components, 256 nets, 64 pins per component, rectangular two-layer
boards up to 500 mm. Integer copper predicates retain the original ±500 mm bound.

## Process rules

`profiles/conservative.rules` is an editable example, not a fab's certification.
Rules not provided use the explicit defaults in `load_rules()`.

| Rule | Meaning |
|---|---|
| `trace_width_mm`, `clearance_mm`, `edge_mm` | Minimum route width, copper gap and edge setback |
| `via_diameter_mm`, `drill_mm`, `annular_mm` | Via size, finished drill and annular limits |
| `hole_gap_mm`, `max_aspect` | Drill edge spacing, board thickness / drill diameter |
| `mask_expansion_mm`, `mask_web_mm` | Mask-opening expansion and minimum pad-to-pad web |
| `courtyard_gap_mm` | Separation of loaded courtyard bounding envelopes |
| `max_height_mm` | Maximum catalogue-specified component height |
| `power_derating`, `voltage_derating` | Fractions of supplied component ratings allowed |
| `max_drop_v` | Per-segment conservative nominal DC drop budget |
| `max_current_density_a_mm2` | User-defined nominal DC current-density heuristic |

Trace calculations use copper resistivity at 20 °C, `R = 1.724e-5 * length_mm /
(width_mm * copper_thickness_mm)`. The current budget is the sum of absolute ideal
source currents (including inductor branch current) and specified current sources.
It is applied pessimistically to every segment; it is not a distributed copper
network solve, an IPC temperature-rise equation, or a thermal field solution.

Soldermask web checks currently cover pad openings; via tenting and separate
front/back mask policies are not modelled. Courtyard checks use the loaded
rectilinear envelope, not arbitrary courtyard polygons. Height must be supplied in the part catalogue as `height_mm`; zero is unknown
and produces a finding. It is never inferred from a package name.

## Calculations and limits

The transient solver starts with zero capacitor voltage and zero inductor current.
It applies constant sources at the first positive time step. There is no arbitrary
waveform source. Each step solves MNA and checks residual/KCL before
emitting CSV. Backward Euler is first-order and introduces numerical damping; the
user chooses a suitable step. The tests compare both its exact discrete solution
and an analytical RC/RL response.

Small-signal AC (`pcba simulate ac`, `copper-core ac`) solves the complex system
A(jw) x = b(jw) at each frequency of a log or linear sweep; independent sources need
`(ac MAGNITUDE PHASE_DEG)` on the component, and the diode is linearised at the DC
operating point. See [AC_ANALYSIS.md](../engineering/AC_ANALYSIS.md).

At most eight resistors with nonzero tolerance are exhaustively corner-tested.
For each corner the actual MNA equations are re-solved, node ranges accumulated,
and targets/resistor ratings checked. This tests all corners, not all possible
interior extrema of nonlinear designs. C/L tolerances are not swept in transient
analysis. Source variation, temperature dependence and nonlinear parameter
uncertainty are not part of this analysis.

The A* router uses a 0.25 mm grid, two layers, deterministic ordering and a
250,000-state search bound per connection. Pad escapes and all completed geometry
are checked afterward. Failure is reported; there is no hidden success threshold
or fake completion rate. There is no global placement optimizer or routing repair
loop yet. Edit `(at ...)` to improve a candidate placement.

Only single-unit symbols are exported. Symbol inheritance is flattened. Physical
pins must match; unsupported geometry is rejected. Pads may be circular,
axis-aligned rectangular, rounded or oval. Arbitrary rotation, slots, custom pad
polygons, footprint zones, planes and complex outlines are not silently reduced.
The schematic uses electrically connected net labels and a grid layout, not an
interactive schematic wiring editor.

## Build and native verification

`build` invalidates old success markers before working. On success it writes the
source snapshot, supported geometry, reports and deterministic UUIDs. A failed
physical check can still produce a clearly labelled draft for inspection. `--draft`
explicitly allows a validly parsed project with an unsupported/failing electrical
model to reach export. Its engineering status remains failed.

`build` writes one of three manifest statuses: `INTERNAL_CHECKS_PASS_NATIVE_KICAD_REQUIRED`,
`INTERNAL_CHECKS_UNVERIFIED_MISSING_DATA` (any analysis warning, i.e. an unknown rating,
height or geometry, listed in `unverified_rules`) or `DRAFT_FAILED_CHECKS`. The manifest
also records the source hash and its path relative to the output directory.

`verify` first deletes any previous `verification.json`, then records each check as
`PASS`, `FAIL` or `UNVERIFIED`: output hashes; source freshness (recorded path or
`--project`; a missing source is `UNVERIFIED`, a changed one `FAIL`); build status;
re-analysis of `resolved.copper` by `copper-core`, which must reproduce `report.json`
exactly; and data completeness. The overall status is `FAIL` if any check failed, else
`UNVERIFIED` if any check could not be established, else `PASS`; only `PASS` exits 0.
Native gates require KiCad 9.x; a missing CLI or another version is recorded as
`UNVERIFIED` together with the detected version. With KiCad 9.x it runs ERC and DRC
with violation exit codes, checks schematic parity, and exports KiCad XML netlist.
Root-sheet net names are normalized by removing one leading `/` before comparing
every physical `(reference, pin)` against the electrical IR. Reports are retained.
Selected library snapshots and local library tables prevent global settings from
silently changing the result. `fabricate` reruns the gates and blocks on any internal
finding before exporting Gerbers and drill files. Supplier review is still outside
the program's scope.

## Extension boundaries

Add a model through `MnaDeviceOps` and tests before allowing a new kind in a solve.
Add a geometric shape to the exact kernel before declaring its checks supported.
Introduce explicit pad layers/slots/no-connects/units into the IR rather than
silently guessing them. Preserve the fail-closed semantics, stable rule IDs,
repeatable generated artifacts and the separation between language and authority.

Primary format references:
- https://docs.kicad.org/9.0/en/cli/cli.html
- https://dev-docs.kicad.org/en/file-formats/sexpr-schematic/
- https://dev-docs.kicad.org/en/file-formats/sexpr-pcb/
- https://dev-docs.kicad.org/en/file-formats/sexpr-footprint/
