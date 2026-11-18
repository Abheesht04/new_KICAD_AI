# Copper topology milestone

This overlay adds a generic, arena-backed electrical topology builder plus a
KiCad schematic/netlist adapter, deterministic JSON output, CLI commands and
regression tests.

## Integration

Copy these files into `v2_src`:

- `topology.h`, `topology.c`
- `topology_json.h`, `topology_json.c`
- `tests/test_topology.c`
- `tests/fixtures/topology_sample.kicad_net`
- `tests/run_kicad_sample_corpus.ps1`
- `schemas/copper.topology.v1.schema.json`

Replace the supplied `CMakeLists.txt` and `copper_cli.c` with the integration
versions in this overlay. They include the catalogue-resolution/enrichment
modules from the preceding milestones as well as topology.

## Build

```powershell
cmake -S v2_src -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

## CLI

Capture a natural-language request without pretending it has been interpreted:

```powershell
copper user design prompt "Design a 3.3V microcontroller board with a status LED" --out request.draft.json
```

Inspect and export a topology using a fresh KiCad netlist:

```powershell
kicad-cli sch export netlist --format kicadsexpr --output fresh.kicad_net design.kicad_sch
copper user topology show design.kicad_sch --netlist fresh.kicad_net --out topology.json
```

Developer build/validation commands:

```powershell
copper dev topology build design.kicad_sch --netlist fresh.kicad_net --out topology.json
copper dev topology validate design.kicad_sch --netlist fresh.kicad_net
```

Exit codes: 0 complete; 2 incomplete/validation issue; 1 execution or parse error;
64 invalid CLI arguments.

## Generated-design API

`copper_topology_init`, `copper_topology_add_component`,
`copper_topology_add_net`, and `copper_topology_connect` provide the
generation-side builder. The schematic/netlist adapter is only one source of
topology. Future request parsing and catalogue-resolved components can call
the same builder directly.

The builder validates duplicate references, duplicate pin numbers,
duplicate net names, unknown components/pins, invalid net indices, and
multiple-net pin assignments. Unconnected terminals remain explicitly
`unconnected_or_nc`; this adapter does not yet resolve no-connect markers
to symbol pins because the current schematic IR does not expose transformed
pin coordinates.

## KiCad sample corpus

Run against a local KiCad 9 demos folder (or any nested sample-project folder):

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\v2_src\tests\run_kicad_sample_corpus.ps1 `
  -SampleRoot "C:\Program Files\KiCad\9.0\share\kicad\demos" `
  -CopperExe .\build\Release\copper.exe `
  -RulesFile .\v2_src\jlcpcb_rules.rules
```

The script generates fresh netlists for schematic topology tests and delegates
PCB DFM/DRC testing to the existing `run_board_corpus.ps1`. It does not silently
treat KiCad DRC exit code 5 (violations found with `--exit-code-violations`)
as a tool execution failure.

## Scope boundary

This milestone does not infer topology from free-form human prompts. The
`user design prompt` command safely captures raw text as a draft request; it
does not invent parts, pin connections, power rails, or electrical values.
The generated-design topology API is ready for the later request parser and
catalogue resolver integration. It also does not yet create KiCad schematic,
PCB, Gerber or drill outputs.
