# Pipeline baseline (H9: no step may make these numbers worse)

- **Recorded:** 2026-11-21
- **Branch:** `lab/copper-lab-integrated`, on `2f01f87` plus the uncommitted work committed in Step B
  (Ollama removal, key file, scope check, ESP32 template, C router)
- **Testing loop:** `PIPELINE_HARD_RULES.md`

## Environment

- **Compiler:** MinGW-w64 GCC 12.1 (`D:\msys64\mingw64\bin`), cmake and ninja from the build-tools venv
- **Build type:** Release; `CMAKE_C_FLAGS=-Wall -Wextra -Wpedantic`, with the V2 and `copper_design` targets also using `-Werror`
- **KiCad:** **10.0.5** CLI and libraries (`D:\Practice\tools\kicad-extract`). Every ERC and DRC number below is a
  **development check, not KiCad 9 evidence**. No KiCad 9.x is installed here (G-010).
- **Hardware:** no GPU, no `nvcc`

## T1: build

| Build | Result |
|---|---|
| Clean configure in a new directory, `-DCOPPER_V2_SANITIZE=ON` | exit 0, **247/247 targets, 0 warnings** |
| UBSan | trap mode on `copper_v2` only. **`copper_design`** (template_gen, autoroute and the feature/update modules) **has no sanitizer option**: recorded as a gap, not changed (H10) |
| ASan | **NOT RUN**: unavailable on MinGW |

## T2: ctest

| Environment | Result |
|---|---|
| Default | 84 tests: **74 passed, 10 skipped, 0 failed** (exit 0) |
| `KICAD_CLI` and `KICAD_DEMOS_DIR` set to KiCad 10.0.5 | 84 tests: **78 passed, 6 skipped, 0 failed** (exit 0, 214 s) |

The 6 tests still skipped (exit 77, never reported as passed) are:
- **need KiCad 9.x:** `v2_native`, `parts_native`, `layout_native`, `design_acceptance`, `slice1_divider`;
- **needs a GPU:** `dfm_gpu_compare`.

## T4: Manus self-test (mock; no credits)

| Command | Result |
|---|---|
| `manus/selftest.ps1 -KiCadCli <10.0.5> -KiCadShare <10.0.5 share>` | **23 passed, 0 failed, 0 blocked** |
| Without `-KiCadCli` | 2 passed, 1 BLOCKED (doctor: no KiCad); this is expected |

## T5: golden boards

Each board was generated from the same requirements twice; `D:\copper-build-tmp\baseline_t5.ps1` is the harness.

| Board | Compile | Routing | DRC unconnected | ERC error / warning | DRC error / warning | DFM | Two runs identical |
|---|---|---|---|---|---|---|---|
| LED indicator | GENERATED_ROUTED | ROUTED | 0 | 0 / 6 | 0 / 3 | UNVERIFIED (hole_to_edge only) | yes |
| Low-side MOSFET switch | GENERATED_ROUTED | ROUTED | 0 | 0 / 12 | 0 / 6 | UNVERIFIED (hole_to_edge only) | yes |
| ESP32 + LED | GENERATED_ROUTED | ROUTED | 0 | **2** / 26 | **12** / 13 | UNVERIFIED (hole_to_edge only) | yes |

The known errors on the ESP32 board are:
- **ERC 2 × `power_pin_not_driven`:** no PWR_FLAG symbols are placed;
- **DRC 12 × `drill_out_of_range`:** the 0.2 mm vias in KiCad's ESP32-WROOM-32 footprint.

### SHA-256 of the files

| Board | `.kicad_pcb` | `.kicad_sch` |
|---|---|---|
| LED | `8E63A3D50202531FDB8096A35E4905F2FD7A8C39C10D264B99DF326CC6871E96` | `353B1183C12499CAAF7E0CF8CCA5DB4747788E443DCDFC423B5375C9B71BBF30` |
| MOSFET | `DD0E519DF49C4B116B5F035B2273F2EB0EBC6D30FB03166C3EF9C47F5FE72BDD` | `292D101151E44FE75F23CCBE09CF87BD7C06DFFE63246662FA2B4515974CC2EF` |
| ESP32 | `9BA034E620D7E33BE270B9672BC3E39B7CC8FD30F38CD9285B6478C84CCC1291` | `D3271B511420461AF54F75864B0E29547756C449BB899F8FE9EFA28B40ABD44B` |

A step that changes these files on purpose must say why (H9). Any other change is a regression.

## T6: rule lint in `src/v2_src/*.c`

| Pattern | Baseline |
|---|---|
| `malloc` / `calloc` / `realloc` | 40 lines in 9 files. `IR.c` (5) and `arena.c` (2) are allowed; the others are G-045: `filesystem.c` 20, `copper_design_json.c` 3, `report.c` 3, `engineering_loop_json.c` 3, `design_resolution_json.c` 2, `optimization_loop_json.c` 1, `topology_json.c` 1 |
| `printf` / `fprintf` / `puts` | 0 |
| `exit(` | 0 |
| Float in geometry, DFM or routing | enforced by the CMake no-float lint (passes in T2); `autoroute.c` is on the list |
