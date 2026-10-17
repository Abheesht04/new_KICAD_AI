# HWC deterministic CLI quickstart

This quickstart covers the connected C CLI (`apps/hwc.c`), the existing `Design` / part provider / net structures, the C S-expression `IR`, the DC MNA adapter and the existing Design-level and PCB DFM modules.

## Windows: build once

Run from the project root (the folder containing `CMakeLists.txt`). A clean build directory is recommended when upgrading from an older ZIP because Visual Studio can retain old generated targets in a stale build tree.

```powershell
cmake -S . -B build -DHWCOMPILER_BUILD_TESTS=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

There is also `hwc.cmd`, which finds `build\Debug\hwc.exe` or builds the `hwc` target if it has not been built yet.

## Commands

```powershell
.\hwc.cmd help
.\hwc.cmd make resistor
.\hwc.cmd make resistor 4.7k ohm
.\hwc.cmd make resistor divider 5 V to 3.3 V using 10k ohm and 20k ohm
.\hwc.cmd validate
.\hwc.cmd "DFM -help"
.\hwc.cmd "DFM -apply"
.\hwc.cmd physics-apply
.\hwc.cmd kicad-check
.\hwc.cmd catalogue -help
.\hwc.cmd catalogue rebuild
.\hwc.cmd catalogue list
.\hwc.cmd catalogue search resistor
.\hwc.cmd catalogue footprints
.\hwc.cmd catalogue footprint-search resistor 0603
.\hwc.cmd status
```

`hwc.cmd` forwards arguments to the C executable. From PowerShell, quoting the full multiword prompt is also supported:

```powershell
.\hwc.cmd "make resistor divider 5 V to 3.3 V using 10k ohm and 20k ohm"
```

Running `hwc.cmd` without arguments opens an interactive REPL. At the prompt, enter commands without outer quotes, and enter `quit` to leave.

## Output files

- `out/current_design.hwc`: small deterministic V1 intent/state file used by subsequent CLI commands to reconstruct the design.
- `out/current_design.kicad_sch`: schematic written by the C `IR` backend.
- `build/kicad_catalog.json` / `.csv`: inventory of symbols discovered in the locally installed KiCad symbol libraries.
- `build/kicad_footprints.json` / `.csv`: inventory of footprints discovered in installed `.pretty` libraries.

The catalogue uses the Python standard-library helper at `tools/hwc_kicad.py` to read installed KiCad file formats. Circuit intent parsing, `Design` creation, topology checks, DFM orchestration, and the supported DC MNA solve are C-side CLI operations. The old Python `generate` helper is a prototype and is not the C CLI's schematic-generation path.

If Python 3.12 is installed as the Windows Python Launcher target, the default catalogue integration runs `py -3.12`. To use another launcher, set `HWC_PY_LAUNCHER` to a compatible Python Launcher command. On non-Windows systems the CLI defaults to `python3`.

## Implemented and checked in V1

- `make resistor [value]`: makes one 0603 resistor Design and exports an intentionally unconnected schematic template. `validate` and `DFM -apply` should flag its unconnected pins; that is expected.
- Resistor-divider grammar: creates an explicit source boundary (`VSIM`) plus two resistors, named `VIN`, `VOUT`, `GND` nets, and a matching resistor schematic. The source is an analysis excitation, not a BOM part in this V1 exporter.
- Structural validation: empty/duplicate references (case-insensitive), duplicate pin numbers, unconnected pins, invalid net IDs, net ID/index mismatch, missing or duplicate reciprocal pin/net connections, orphan net links, empty/duplicate named nets, and single-terminal net warnings.
- Design-level DFM: runs the currently registered `DfmRegistry` checks, currently including floating pins, missing footprints (not required for ideal simulation sources), and component height.
- DC MNA: translates the supported divider through `NodeMap` to MNA, solves it, and reports node voltage, current, resistor power, residual and independent validation. Resistor models and ideal DC sources are supported in this V1 route.
- C schematic export: constructs a `kicad_sch` S-expression using `src/IR.c` and embeds `Device:R` from the local KiCad symbol library when found (the KiCad 9 `Device:R` fixture is only a portable regression fallback).
- KiCad symbol/footprint inventory and search through the installed-library helper.

## Important limits

This is a narrow, connected V1—not arbitrary circuit synthesis. The deterministic frontend currently recognises a resistor and a resistor-divider prompt grammar, and the schematic exporter intentionally supports one or two `Device:R` symbols. Unsupported prompts are rejected instead of being converted into guessed circuit topology.

`Extras/NLP.c` is an optional ONNX Runtime-oriented model backend and its inference path is not a completed general language model. It is not required for the CLI and was deliberately not linked into `hwcore` because the project ZIP does not ship a model or ONNX Runtime. `src/nlp_rules.c` is the deterministic parser used by this CLI.

`DFM -apply` runs Design-level checks against the abstract circuit, not physical PCB geometry. Physical checks implemented in `pcb_dfm.c`, `copper.c`, and `defect.c` still require a populated `PcbBoard` and manufacturing profile. The current command does not import `.kicad_pcb` geometry; it reports that the physical DFM pass has not run. `DFM -coverage` displays implemented/partial/unsupported coverage. A logical DFM PASS is not fab sign-off.

`physics-apply` is DC resistive operating-point analysis only. It is not AC/transient analysis, semiconductor-model coverage, thermal analysis, SI/PI, reliability or formal hardware verification. The supplied divider prompt `5 V -> 3.3 V` with `10k` top and `20k` bottom calculates approximately `3.3333 V`; the CLI correctly warns that this is not within the requested 3.3 V target tolerance.

Run `.\hwc.cmd kicad-check` on Windows before demoing the output. This invokes `kicad-cli sch erc` and SVG export using your installed KiCad version. A parser round-trip test confirms the file is well-formed according to the C IR parser, but the test environment may not have KiCad CLI installed. Always inspect the ERC report and exported SVG on the target KiCad version; a zero-returning CLI invocation does not mean the ERC report contains no violations.
