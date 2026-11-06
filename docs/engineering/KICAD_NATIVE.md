# Native KiCad integration

What the `v2-native-kicad-integration` branch brought (one commit, `e04491b`, on the 2026-10-17 baseline) and how it
is wired into the product. The branch's files stay where it put them (owner rule 2026-11-06, `CLAUDE.md` §2: do not
rename or move existing files); the build compiles them in place.

| Brought (kept at this path) | Built and used as | Owner |
|---|---|---|
| `src/v2_src/` | the **V2 strict-C engine folder**: every new V2 module goes here as new files | `copper_v2` CMake library |
| `src/v2_src/g2_types.{c,h}` | `copper_v2`; tests `tests/g2_types_test.c` | exact nm geometry |
| `src/v2_src/excellon.{c,h}` | `copper_v2`; `copper-core drill FILE`; `pcba project cam-check`; re-read by `pcba project fabricate`; tests `tests/kicad_native_test.c`, `test_native_gates.Cam` | Excellon reader |
| `src/v2_src/kicad_library_table.{c,h}` | `copper_v2`; tests `tests/kicad_native_test.c` | KiCad library tables (C side) |
| `src/v2_src/filesystem.{c,h}` | `copper_v2`; tests `tests/kicad_native_test.c` | filesystem helpers |
| `src/v2_src/copper_project.{c,h}` | `copper_v2`; `copper-core kicad-new/kicad-open`; `pcba kicad new/open`; tests | native KiCad 9 projects |
| `src/v2_src/{IR,arena,cplx,nmath,sexpr,u128}.{c,h}` | byte-identical to the copies in `src/`, which `hwcore` compiles; test `v2_src_shared_in_sync` fails if they ever differ | base modules |
| `src/v2_src.zip` | not built: the folder as delivered | — |
| `export_cam_kicad906.ps1` | run by hand on a machine with KiCad 9.0.6 | KiCad 9 CAM export |
| `src/v2_src/copper_ai_model_probe.ps1` | run by hand against an Ollama server | Ollama probe (Qwen3 8B, Llama 3.2) for the smart-layer plan |
| `manufacturing_audit/{gerber_test,drill_test}` | **real KiCad 9.0.6 CAM output** of the divider board, the test oracle for `kicad_native_test` and `test_native_gates.Cam` | — |
| `manufacturing_audit/gerber_test.zip` | not built: the same Gerbers zipped | — |

`copper_v2` builds with `-std=c17 -Wall -Wextra -Wpedantic` and links `hwcore` for the base modules; `copper-core`
links `copper_v2`. `-DCOPPER_V2_SANITIZE=ON` adds `-fsanitize=address,undefined`; on MinGW (no `libasan`) it adds
UBSan in trap mode only, and ASan is reported as not run.

## Fixed while integrating

Each is a small edit to a protected file, noted here as `CLAUDE.md` §2 requires. Each was found by building the modules with this project's flags (C17, `-Wall -Wextra -Wpedantic`), or by their first
tests. On the branch none of the modules were part of any build target.

- **Compile error:** `g2_types.c` returned `G2_ERR_CAPACITY`, which the enum did not declare, so the module did not compile. It is now declared: more BVH results than the caller's buffer. The count stays exact (tested).
- **Include guard clash:** `copper_project.h` and `src/v2/project.h` used the same guard, `COPPER_PROJECT_H`, so whichever came second vanished. The new header uses `COPPER_NATIVE_KICAD_PROJECT_H`.
- **Wrong status names:** `kicad_lib_status_name` used a local table missing two statuses, so `UNRESOLVED_VARIABLE` and `RANGE_ERROR` printed as `UNKNOWN_STATUS`. The complete file-level table, which nothing used, is now the one used (tested).
- **Missing library on MinGW:** `copper_project.c` linked `ole32` (for `CoCreateGuid`) with an MSVC-only `#pragma`, so MinGW builds would not link. CMake now links `ole32` on Windows; the pragma remains for MSVC.
- **Unused variable:** `filesystem.c` declared a buffer it does not use on Windows; it is now scoped to the branch that uses it.

## What each module does

- **`g2_types`:** exact integer geometry in nanometres (±1 m). It covers orientation, distances compared against a threshold without rounding (point, segment, circle, capsule, rounded rectangle, quad, polygon, region, arc), containment, intersection and a BVH. Invalid input is refused, never guessed. Arc bounding boxes are conservative: the whole circle's box.
- **`excellon`:** reads KiCad Excellon drill files with integer arithmetic, no `strtod`.
  - **Supported:** metric or inch, decimal coordinates, absolute mode, X2 attributes (plating, drill function), and up to 99 tools.
  - **Refused as content errors, not guessed:** zero-suppressed coordinates, incremental mode, an undefined tool, a hit before any tool, and missing headers. Routed slots are counted, not compared.
- **`kicad_library_table`:** parses `sym-lib-table` / `fp-lib-table` and expands `${VAR}` and `${KIPRJMOD}`; an undefined variable is an error. A project table's nickname wins over the global one. The Python library index (`tools/kicad_library.py`) has its own reader with KiCad's variable precedence and per-entry statuses. The two readers are separate for now (C for native projects, Python for the index).
- **`copper_project`:** creates `NAME.kicad_pro`, `NAME.kicad_sch` (KiCad 9 format 20250114), `NAME.kicad_pcb` (20241229) and a `.copper/{cache,logs}` workspace.
  - The project is staged in a temporary folder and renamed into place, so an existing destination is never overwritten.
  - Names that would escape the parent folder are refused.
  - It finds `kicad-cli` from an explicit path, `KICAD_CLI_PATH`, `PATH` or the KiCad 9 install folders.

## Commands

| Command | Does |
|---|---|
| `pcba kicad new NAME [--dir DIR]` | an empty native KiCad 9 project (`copper-core kicad-new`) |
| `pcba kicad open DIR` | what a project folder holds (`copper-core kicad-open`) |
| `pcba project cam-check --out BUILD [--cam DIR]` | re-reads a build's CAM output without KiCad (below) |
| `pcba project fabricate --out BUILD` | exports the drill files as decimal mm with an absolute origin (the format the reader supports), then runs the same re-read; any disagreement is FAIL |
| `copper-core drill FILE` | an Excellon file as JSON (tools, hits in mm, issues) |

**What the CAM re-read checks** (`copper.cam_check`):
- **Drill holes:** every hole in every drill file, by position and diameter to 1 µm, must be exactly the design's pad and via holes (`resolved.copper`), with Excellon y = −board y under KiCad's absolute origin. Missing, extra and moved holes are named.
- **Gerber job file:** it must state two copper layers, the board's size (outline plus its 0.05 mm stroke), and only files that exist. Without a job file the result is UNVERIFIED.
- **Not checked yet:** the Gerber images themselves (copper, mask, silkscreen). The C Gerber reader now exists (`src/v2_src/gerber.c`, [GERBER.md](GERBER.md)); wiring it into this check is the next CAM step.

## Evidence

- **Real KiCad 9.0.6 output of a Copper board:** the files under `manufacturing_audit/{gerber_test,drill_test}` were exported by KiCad 9.0.6 on the owner's machine from the divider board (`examples/v2/divider.copper`, built with the committed library snapshot).
  - Re-read without KiCad, they agree with Copper's design: 3 plated 1.000 mm holes at x 8, y 12 / 14.54 / 17.08, and 14 Gerber files for a two-layer 40 × 30 mm board (`test_native_gates.Cam`).
  - Moving a hole by 0.1 mm, shrinking the drill, deleting a hole, changing the layer count or the board size, or deleting a listed Gerber each make the check FAIL.
- **`fabricate` end to end:** run against a fake `kicad-cli` that returns those real KiCad 9.0.6 files, it PASSes; with one hole moved it FAILs, naming the hole.
- **Generated projects** (development check, KiCad 10.0.5 CLI): a `pcba kicad new` project opens in `kicad-cli`, with DRC 0 violations and ERC 0 violations.

## Smart-layer probe

`src/v2_src/copper_ai_model_probe.ps1` sends one under-specified request (a USB-C sensor board) to a local Ollama server
(`qwen3:8b`, `llama3.2:latest`). It asks the model to separate requirements from assumptions and to ask at most three
questions, then reports latency, tokens per second, CPU/GPU placement and GPU memory. It is an evaluation script for
the plan's §6; nothing in the product calls a model. It has not been run on this machine (no Ollama here).
