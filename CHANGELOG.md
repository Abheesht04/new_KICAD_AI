# Copper Studio V2 integrated upgrade

- Preserved the original exact u128/copper kernels.
- Added typed project compilation, custom catalogues/rules, actual KiCad geometry,
  two-layer routing candidates, physics/DFM reports and native validation gates.
- Added C self-attention classification, GLFW/Vulkan desktop, CMake presets,
  Windows entry points, numerical/negative/integration tests and documentation.
- Fixed unsupported-model omission and stdout debug noise in the existing MNA path.
- Kept legacy commands; use `hwc project ...` or `tools/copper.py` for the wider V2 flow.
- See `docs/v2/VALIDATION.md` for exactly what was run. This is a bounded engineering
  workbench, not an unrestricted circuit synthesis or fabrication certification tool.

---

# Changelog

## V2 track H: CUDA DFM kernel and its CPU reference (2026-11-07)

- `src/v2_src/dfm_kernel.h` (exact integer clearance predicate shared by CPU and GPU), `dfm_cpu_ref.c`,
  `cuda/dfm_gpu.cu` + `dfm_gpu.h` (built only with -DCOPPER_CUDA=ON; a stub reports NOT_BUILT otherwise),
  `apps/dfm_bench.c` (N = 64, 1024, 16384).
- The CPU reference equals g2's exact capsule distance on 1000 random boards. The GPU path was not built or run
  here (no CUDA, no GPU): its 1000-board comparison test reports NOT RUN.

## V2 track G: LLM bridge (2026-11-07)

- `src/v2_src/llm.h` backend interface; `llm_mock.c` (default: scripted or rule planner), `llm_ollama.c`
  (POST /api/chat, stream false, think false, schema as format, plain HTTP sockets), `schema_gbnf.c` (JSON Schema
  subset to GBNF), `agent_loop.c` (tools through the track F server, 12-turn limit, JSONL turn log, scoring).
- `eval/prompts.jsonl` (10 prompts), `scripts/run_llm_eval.sh` / `.ps1`, `copper-core agent-eval` writing
  eval_out/<date>/results.jsonl and summary.txt. With the mock: 10/10 PASS. No model is needed for any test; the
  live Ollama test was not run here (no server): UNVERIFIED.

## V2 track F: JSON-RPC tool server (2026-11-07)

- `src/v2_src/tool_server.{c,h}`, `copper-core serve`: JSON-RPC 2.0 over line-delimited stdio; tools board.load,
  dfm.check, export.fab (kicad-cli export, read back, holes compared with the board), excellon.check (with a board:
  missing / extra / wrong-diameter holes); structured errors with kind and hint; JSONL session log.
- json.c: compact (one-line) writer mode and raw values; finding_report_write embeds a report in a response.
- Scripted transcripts: 25 messages through the library, and the real process over stdio, both compared line for
  line with golden responses.

## V2 track E: findings and one report (2026-11-07)

- `src/v2_src/finding.{c,h}`: one Finding for DFM violations and undecided rules, Excellon, Gerber and board-model
  issues; statuses FAIL / INVALID_INPUT / UNVERIFIED / NOT_IMPLEMENTED / INFO; a total sort order; verdict.
- `json.c` gains a deterministic JSON writer; `copper-core report BOARD PROFILE [FILES...]`.
- Golden `tests/golden/report.json` from test-authored and real KiCad 9.0.6 inputs, compared byte for byte.
- excellon.c: a non-coordinate line after an undefined tool no longer stops reading with EXC_ERR_ARGUMENT.

## V2 track A: Gerber reader in C (2026-11-07)

- `src/v2_src/gerber.{c,h}`, `copper-core gerber FILE`: RS-274X/X2 in exact nm: FS, MO, AD (C/R/O/P, macros), AM
  (primitives 1/4/20/21, variables, arithmetic), D01/D02/D03, G01, regions, polarity, TF/TA/TO/TD. G02/G03 reported
  as unsupported with its line; every other problem an issue with a line number.
- The 17 KiCad 9.0.6 Gerbers in manufacturing_audit read with no issue; Edge.Cuts closes into one 40 x 30 mm loop;
  copper flashes match the drill file and the C board model. 237 KiCad-written Gerbers of 10 demo boards read with
  no error.
- tests/test_hwc_kicad.py scans repository files only, not the gitignored build folders other tests write into.

## V2 track D: kicad-cli runner in C (2026-11-06)

- `src/v2_src/kicad_cli.{c,h}`, `copper-core kicad JOB BOARD OUT`: version, DRC (JSON), Gerbers, drill (metric,
  decimal, absolute) and position exports; CreateProcessW / posix_spawn without a shell; timeout kills the job
  object / process group; stdout and stderr to files; staged output moved into place without ever replacing;
  KiCad 9.x version gate.
- Tested against a fake kicad-cli (every failure path) and the real KiCad 10.0.5 CLI (development check).

## V2 track C: DFM against fab profiles (2026-11-06)

- `src/v2_src/dfm.{c,h}`, `copper-core dfm BOARD PROFILE`: track width, clearance (BVH, exact shape distances),
  annular rings, drills and slots, hole to hole, hole to track, copper to edge, hole to edge, board size; zone
  clearance NOT_IMPLEMENTED; inexact items never FAIL, they are UNVERIFIED.
- `fab_profiles/jlcpcb_2layer.json`: every limit quoted from jlcpcb.com/capabilities/pcb-capabilities (2026-11-06),
  unpublished ones null, page conflicts resolved to the stricter value and listed.
- `src/v2_src/json.{c,h}`: strict JSON reader; decimal mm to exact nm.
- Clearance equals a brute-force oracle on six KiCad 9.0-saved boards (199 violations, none missed or extra).

## V2 track B: the board model in C (2026-11-06)

- `src/v2_src/board.{c,h}` and `copper-core board`: KiCad boards (nets, footprints, absolute pads, tracks, vias, zones,
  Edge.Cuts loops with bounds and measured gaps) in exact integer nm. Checked against hand-computed pads, a KiCad 10
  re-save, the real KiCad 9.0.6 drill file of the divider, and 13 KiCad 9.0-saved demo boards against KiCad's drill
  export (4,294 holes). See docs/engineering/BOARD_MODEL.md.
- IR: quoted strings no longer reserve the rest of the input (memory was quadratic in file size).

## Connected C CLI / prompt-to-schematic milestone (2026-10-11)

- Added `apps/hwc.c` and root `hwc.cmd` for interactive and one-shot deterministic CLI use.
- Added `src/nlp_rules.c`: a dependency-free, explicit parser for resistor templates and a resistor-divider prompt; unsupported circuit intents are rejected, not guessed.
- Connected parsed intents to the existing `Design`, `PartRegistry` / KiCad generic provider, net topology, structural diagnostics, existing Design-level DFM registry, `NodeMap`, DC MNA, and the C `IR`-based `.kicad_sch` writer.
- Added `src/design_validate.c` checks for unconnected pins, duplicate references/pins/net names, invalid IDs, reciprocal pin/net asymmetry, orphan connections, empty nets, and invalid supported-model values.
- Corrected the Design-to-MNA adapter to honor constructor status semantics (device constructors return 1 for success and 0 for failure).
- Added a regression test for Design -> NodeMap -> MNA on a 5 V / 10 kOhm / 20 kOhm divider (Vout = 3.333333 V) and a CLI integration test that verifies schematic creation, structural validation, Design-level DFM, physics solve, and rejection of an unconnected resistor.
- Added `docs/CLI_QUICKSTART.md` documenting commands, output files and capability limits.
- Current verification in Linux/GCC CMake build: 27/27 CTest tests pass. KiCad CLI / ERC was not available in this build environment; validate the generated schematic with the locally installed KiCad 9.0.6 CLI before treating it as accepted by KiCad.
- Boundaries: prompt grammar and schematic export are currently resistor-only; `DFM -apply` is Design-level until physical PCB geometry is loaded; `physics-apply` is DC resistive MNA only. The ONNX inference path in `Extras/NLP.c` remains optional and is not represented as a complete language model.

## Unreleased - copper v2 (exact integer kernel)
- Added `u128` (64x64->128 multiply and compare) and exact `geom_cmp_*` comparators; no `double` in any pass/fail decision.
- Replaced `copper.[ch]` with the core+radius model (point, segment, rect); arcs/polygons return UNVERIFIED.
- New `pcb_copper.[ch]` adapter: the only place mm becomes nm and KiCad layer IDs become masks.
- Net 0 is now "no net": touching unassigned copper is a short.
- Removed `<math.h>` from `copper`, `defect`, `dfm_pcb`, `IR`; added `nm_isfinite`, `nm_from_mm_ceil`, declared `pt_in_bounds`.
- `IR`: exact decimal-mm <-> integer-nm (`ir_atom_to_nm`, `ir_node_atom_nm`).
- Tests: new `u128`, `geom` (long-double oracle), `ir_nm`; `copper` rewritten.

## DFM-first physical compiler upgrade

- Added first-class physical PCB IR for board geometry, pads, traces, vias and placements.
- Added fab-profile-driven geometry DFM with machine-readable rule IDs and measured limits.
- Added deterministic fab-profile serialization with a stable value-owned name field.
- Added deterministic KiCad PCB S-expression export with net table support.
- Updated the minimal KiCad writer to current KiCad 10 layer IDs/version used by the checked-in exporter.
- Hardened arena allocation and vector alignment.
- Fixed E-series ppm tolerance semantics.
- Added reproducible C23/CMake tests and demo tooling.

This remains an engineering-core prototype, not a claim of IPC/fabricator certification.

## Copper/DFM integration milestone (2026-10-09)

- Added `src/copper.h/.c`: layer-mask helpers, KiCad layer-ID mapping, checked mm-to-nm conversion, straight trace/circular pad/via adapters, signed clearance measurement, and a common-layer multi-object clearance scan.
- Wired the Copper scan into `pcb_dfm_validate()` for trace/trace, trace/pad, trace/via, pad/pad, pad/via and via/via pairs.
- Added structured primary/related object kind and index fields to `PcbDfmIssue` for copper clearance findings.
- Corrected KiCad 9 PCB format version/layer-table IDs and added inner-copper layer identifiers to the physical IR mapping.
- Fixed Linux case-sensitive header inclusion and added missing `geom.c`/`nmath.c` sources to CMake; moved the tokenizer source path to the actual `Extras/tokenizer.c` location.
- Added Copper unit tests and retained user-provided KiCad 9.0.6 netlist/DRC JSON files under `tests/fixtures/kicad9`.
- Validation: CMake build succeeded; all 16 CTest suites passed in normal and AddressSanitizer/UndefinedBehaviorSanitizer builds. The emitted PCB's S-expression parenthesis/string structure passed a local syntax-balance check. KiCad CLI validation was unavailable in this environment.
- Made CMake warning flags portable to MSVC vs GCC/Clang, kept legacy assert-based tests active in Release builds, and removed a truncation warning in fixed-size string copying. Release configuration also passes all tests.

## Defect analysis milestone (2026-10-09)

- Added `src/defect.h/.c` with physical open-connectivity checks, direct inter-net short detection, invalid copper geometry findings, and a DFM-report consistency audit.
- Routed defect findings through the existing `PcbDfmReport`; no separate competing diagnostics collection was introduced.
- Added a coverage registry for supported/partial/unsupported PCB/CAM/VLSI check families and `fabcheck_demo --coverage`.
- Added structured object references for width, edge-clearance, annular-ring, and hole-clearance findings so diagnostic consumers don't need to parse the message text.
- Added the explicit `DEFECT_ENGINE.md` architecture and limitations document.
- Refreshed the KiCad 9.0.6 DRC fixtures with the latest uploaded reports; they remain intentional defect-injection fixtures.
- Expanded automated tests for opens, connected pads, short circuits, invalid geometry, DFM report inconsistencies, and coverage gaps.
- Current verification: fresh CMake Release build and 17/17 CTest suites pass.
