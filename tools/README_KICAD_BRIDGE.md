# KiCad library inventory helper

The integrated C CLI is the schematic-generation front end: use `hwc.cmd make ...`, not the old Python generator subcommand, for the connected C `Design` -> C `IR` -> `.kicad_sch` path. This Python standard-library helper is used for installed KiCad library inventory/search and optional custom-symbol cloning.

From a PowerShell prompt in the repository root:

```powershell
py -3.12 tools\hwc_kicad.py doctor
py -3.12 tools\hwc_kicad.py catalog
py -3.12 tools\hwc_kicad.py search resistor
py -3.12 tools\hwc_kicad.py footprints
py -3.12 tools\hwc_kicad.py search-footprint resistor 0603
py -3.12 tools\hwc_kicad.py custom-symbol --source Device:R --name HWC_R_10k --value 10k --footprint "Resistor_SMD:R_0603_1608Metric" --out custom\CustomParts.kicad_sym
```

The default catalogue locations are `build\kicad_catalog.json`, `build\kicad_catalog.csv`, `build\kicad_footprints.json`, and `build\kicad_footprints.csv`. They reflect the installed KiCad libraries only; manufacturer ordering, stock, and electrical accuracy are not inferred.

`hwc_kicad.py generate` and `validate` are retained as development utilities for backwards compatibility but are not the integrated C CLI's schematic generation path. The C CLI exports the `.kicad_sch`; run your installed KiCad 9 `kicad-cli` against the generated file and inspect the ERC report/SVG.
