# Deterministic Hardware Compiler — DFM-first kernel

> **Windows CLI entry point:** see [`docs/CLI_QUICKSTART.md`](docs/CLI_QUICKSTART.md). Build with CMake, then run `hwc.cmd` or launch `build\Debug\hwc.exe` directly.

The current connected V1 command path is:

```text
prompt -> deterministic C intent parser -> Design / components / nets
       -> structural validation -> C IR -> KiCad .kicad_sch
       -> Design-level DFM / supported DC MNA solve
```

Quick start from PowerShell in the project root:

```powershell
cmake -S . -B build -DHWCOMPILER_BUILD_TESTS=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
.\hwc.cmd make resistor
.\hwc.cmd "make resistor divider 5 V to 3.3 V using 10k ohm and 20k ohm"
.\hwc.cmd validate
.\hwc.cmd "DFM -apply"
.\hwc.cmd physics-apply
.\hwc.cmd kicad-check
.\hwc.cmd catalogue -help
.\hwc.cmd catalogue list
```

The V1 prompt grammar and schematic exporter are intentionally narrow: one unconnected resistor template or a resistor divider. The full installed KiCad symbol/footprint inventories are accessed through `tools/hwc_kicad.py`; they do not mean every KiCad symbol has a corresponding validated C electrical model. See the quickstart for exact capability boundaries, expected outputs and missing features.

This repository is a deterministic electronics design core with a natural-language frontend at the boundary.

The intended authority chain is:

    Natural language
          |
          v
    semantic extraction
          |
          v
    engineering IR  <---- deterministic
          |
          +--> topology / component models
          +--> electrical simulation (MNA)
          +--> physical PCB IR
          +--> manufacturing constraints
          |
          v
    DFM / DFA / SI / thermal validation
          |
          v
    KiCad / fabrication outputs

## What changed in this upgrade

The prototype previously had a strong logical design core but no first-class physical PCB representation.

This upgrade adds:

- `pcb.h/.c`: board dimensions, copper traces, pads, vias and placements.
- `dfm_pcb.h/.c`: fab-profile-driven DFM validation with machine-readable rule IDs, measurements and limits.
- `kicad_pcb.h/.c`: deterministic KiCad PCB export so KiCad remains the visualization/inspection surface rather than the design authority.
- `CMakeLists.txt`: reproducible C23 build and test definition.
- Cross-platform `xform.h` canonical casing with a compatibility wrapper.
- Safer arena allocation and ppm-correct E-series tolerance checks.

## Product principle

The LLM is not allowed to decide whether geometry or a circuit is correct.

It may propose intent:

    "5 V input, 3.3 V rail, USB-C, two mounting holes."

The compiler resolves that into structured requirements, selects components, synthesizes topology, solves the resulting equations, chooses physical geometry, and proves the board against a selected manufacturing process.

A future manufacturing profile should be a versioned data object supplied by a fab, not a universal hard-coded number.

## DFM roadmap

Current physical rules:

- board size
- layer count
- minimum trace width
- trace-to-trace clearance
- via diameter / drill / annular ring
- plated pad annular ring
- hole clearance
- copper-to-edge clearance
- component height

Next rules should include:

- stackup and dielectric constraints
- finished copper vs current / temperature rise
- impedance-controlled geometry
- soldermask dam / sliver
- silkscreen-to-copper and silkscreen-to-edge
- drill aspect ratio
- PTH / NPTH rules
- slot rules
- BGA escape geometry
- via-in-pad / filled-via process constraints
- copper balance / plating risk
- acid traps, acute angles and etch compensation
- assembly keepouts and component courtyard collisions
- pick-and-place reachability
- stencil aperture constraints
- panelization
- test-point coverage
- fab-specific CAM rules
- DFA / AOI / ICT / flying-probe constraints
- final manufacturing package generation and consistency checks

The long-term target is not "passes KiCad DRC."

The target is:

    requirement -> synthesized board -> validated manufacturing process
    -> deterministic fab package -> less CAM feedback -> predictable first-pass build

## Competitive thesis

AI schematic generation is increasingly crowded.

The differentiated layer is the manufacturing compiler:

    component capability
        x
    electrical requirements
        x
    physical geometry
        x
    fab process window
        x
    assembly process
        x
    reliability constraints
        =
    manufacturable design space

A fab should eventually be able to publish its process profile directly to this compiler.

That turns the fab from a final checker into a design-space provider.

## Near-term architecture

1. Freeze the logical Design IR.
2. Make PCB physical geometry a first-class IR.
3. Make manufacturing profiles versioned and sourceable.
4. Make DFM rules constraints that influence synthesis, not merely post-run warnings.
5. Build deterministic solvers/optimizers around that IR.
6. Keep NLP/LLMs at the intent boundary.
7. Export KiCad for human inspection and fabrication tooling for production.

## Important limitation

The sample `conservative_2layer` profile is intentionally generic and is NOT a certification of any specific fabricator.

Vendor profiles should be created from current, documented capability data and versioned with their source date.


See `docs/MARKET_RESEARCH.md` for the 2026 competitive landscape, fab-process thesis, India market signal, standards map, and product sequence.

## Copper geometry milestone

`src/copper.h/.c` now maps the current PCB IR's straight tracks, circular pads, and plated vias into a layer-mask-aware copper model. `pcb_dfm_validate()` uses it for trace/trace, trace/pad, trace/via, pad/pad, pad/via, and via/via inter-net clearance checks on shared copper layers. Pair findings include both object kinds and indexes.

The module is intentionally not yet a comprehensive DFM engine: arcs, zones, polygon pads, arbitrary outline geometry, connectivity/open checks, mask/silkscreen rules, and all hole interactions remain future work. See `docs/COPPER_ENGINE.md` and the supplied KiCad 9 regression fixtures under `tests/fixtures/kicad9`.


## Defect analysis and honest coverage

`src/defect.c/.h` detects disconnected same-net pads and direct shorts between supported copper shapes. It also audits reported DFM findings for measurement/reference inconsistencies and exposes a coverage registry. Run `fabcheck_demo --coverage` to see partial and unsupported checks. A PASS from currently implemented checks is not a claim of complete PCB or VLSI signoff.

The KiCad DRC JSON files under `tests/fixtures/kicad9/` intentionally contain design defects and are regression targets; they are not expected-clean-board examples.
