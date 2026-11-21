# Gap register

*Created 2026-11-08 by P00 (baseline inventory) at revision `7199ef6b1e13a3df579c57b3d7cd874d38cb60ee` (branch `v2/complete`). The status words are defined in [IMPLEMENTATION_CONTRACT.md](IMPLEMENTATION_CONTRACT.md) §2.*

## How to read this register

**Defaults that apply to every row unless the row says otherwise:**

| Field | Default |
|---|---|
| Revision observed | `7199ef6` |
| Registered by | P00 (baseline inventory), unless the row names a later prompt |
| Owner / task | unassigned |
| Target prompt | TBD. The texts of P02–P52 are not in the repository or this session, so no row can be mapped to a later prompt yet. Each row gets its target prompt when that prompt arrives. |

**Severity:**
- **S1:** gives or could give a wrong verdict, or blocks the manufacturing-ready claim for the supported scope.
- **S2:** a major in-scope capability or an evidence gap.
- **S3:** limited impact, or the evidence is weak.
- **S4:** housekeeping.

**Sections:** each type of gap has its own section, and they are not mixed:
1. findings F1–F8 from the earlier assessment;
2. confirmed defects (reproducible at this revision);
3. documented missing features within the release scope;
4. implementations that are not verified, and integrations that are not tested;
5. capabilities documented as unsupported, which are outside the release scope;
6. proposed extensions;
7. historical findings that were already fixed (kept as evidence).

**Baseline measured for this register** (details in [PROMPT_RESULTS.md](PROMPT_RESULTS.md) P00):
- `ctest --test-dir build-v2 -j6`: **76/76 pass, 11 skipped, exit 0** with no KiCad or demo environment variables set;
- with `KICAD_CLI` (10.0.5) and `KICAD_DEMOS_DIR` (KiCad 9.0-saved demo boards) set, the 4 KiCad-data tests also pass, which leaves **7 skipped**.

---

## 1. Findings F1–F8 from the earlier assessment

These are findings F1–F8 from the owner's assessment of `v2/complete` at `7199ef6`. Their texts were not in the repository; each is recorded here as the owner supplies it with its prompt. A finding without a text is not guessed.

| ID | Sev | Status | Finding (as supplied by the owner) | Reproduction at `7199ef6` | Acceptance evidence | Prompt / branch |
|---|---|---|---|---|---|---|
| F1 | S1 (POSIX build) | **IMPLEMENTED_UNVERIFIED**: fixed and verified on Windows (MinGW64 GCC 12.1 and UCRT64 GCC 12.1); the Linux/glibc build has not run (BLOCKED_EXTERNAL: no POSIX toolchain here) | Strict C17/GCC POSIX build fails: the private `dup(Arena*,…)` collides with POSIX `dup(int)`; the `DIR` macro in `gerber_test.c` collides with `<dirent.h>`; `setenv`, `readlink` and `lstat` are undeclared. Also an unused `name_length` in `filesystem.c` and possible `snprintf` truncation in `schema_gbnf.c`. | `dup`: reproduced with the real MinGW `<io.h>` (`kicad_cli.c:78` conflicts with `io.h:332 int dup(int)`). `DIR`: reproduced with the real MinGW `<dirent.h>` in the POSIX include order. `setenv`/`readlink`/`lstat`: **not reproducible here** (MinGW has neither glibc's feature macros nor these functions); found by audit: the only 3 files that call guarded POSIX functions without `_POSIX_C_SOURCE`. `schema_gbnf.c`: **two runtime defects reproduced**: (a) an array rule over 8 KiB is cut off with status OK; (b) `rule_name` writes one byte past its 256-byte buffer. UCRT64 GCC warns at `schema_gbnf.c:97`. `name_length`: POSIX-only "set but not used"; behaviour unaffected. | **Done on Windows:** clean Release builds with strict flags on every translation unit give 0 warnings and 76/76 tests with both MinGW64 and UCRT64 GCC 12.1; the new `llm_test` regression fails 4 checks against `7199ef6`'s converter. **Still needed:** a clean Linux GCC `-std=c17 -Wall -Wextra -Wpedantic` build and `ctest`. | P01, branch `p01/f1-posix-portability` @ `7a29caa` (not pushed) |
| F5 | S2 | OPEN | On Linux, `KCLI_TEST_DIR` was `build/kicad_cli_test`, which is also the test executable; cleanup removed the executable and recreated the path as a directory, so `kicad_cli_real` could not launch | Not yet run (P02) | Both runner tests launch reliably on repeated and parallel runs; scratch directories are unique and owned; BLOCKED and SKIPPED are told apart | P02 |
| F2–F4, F6–F8 | — | BLOCKED_EXTERNAL | Texts not supplied yet | — | — | — |

## 2. Confirmed defects (reproducible at `7199ef6`)

| ID | Sev | Status | Defect | Source | Reproduction | Acceptance evidence | Deps |
|---|---|---|---|---|---|---|---|
| G-042 | S1 | OPEN (owner decision) | **The working tree does not build.** An uncommitted edit to `CMakeLists.txt`, made by someone other than P00 (file time 2026-11-08 11:15:38 +0530), removes `src/v2_src/rest_server.c`, `src/v2_src/placement.c` and their tests from the build. `apps/copper_core.c` still calls both, so the link fails. HEAD `7199ef6` builds and passes; only the uncommitted working tree is affected. Not reverted (contract rule 2); P01 worked in a separate worktree. | P00 | `cmake -S . -B <new dir> -G Ninja && cmake --build <new dir>` exits 1 with `undefined reference to 'rest_serve'` (`copper_core.c:387`) and `pl_problem_load`, `pl_anneal`, … (`copper_core.c:417-437`) | The owner either restores the two sources and tests, or removes their callers deliberately. In both cases a fresh configure, the build and `ctest` pass. | owner |
| G-043 | S3 | OPEN | **GBNF rule names collide.** `schema_gbnf.c` maps every non-alphanumeric character to `-`, so sibling properties `a_b` and `a.b` both become rule `root-a-b`. The result has status OK and two conflicting definitions, and the root rule refers to the same name for both. The agent's own schema has no such keys. Found during P01 and kept out of F1's scope. | P01 | `gbnf_collision.c` (P01 scratch, in `PROMPT_RESULTS.md` P01) prints two `root-a-b ::=` definitions with status 0 | Unique rule names (a numeric suffix, or an escape) or INVALID_ARGUMENT; a regression in `llm_test` | — |
| G-044 | S2 | OPEN | **Two parallel engines in `src/v2_src` since the `feature/update` port**: board models `board.c` (track B) and `design_board.c`; MNA in hwcore's `src/mna.c` and `spice_mna.c`; DFM `dfm.c` and `dfm_check.c`; placers `placement.c` and `placer.c`; JLCPCB limits in `fab_profiles/jlcpcb_2layer.json` and `src/v2_src/jlcpcb_rules.rules` (the shared values agree). `copper_design` defines symbols that `copper_v2` and hwcore also define (`board_rotate`, `mna_*`), so it is a separate library and the two lines cannot be linked into one executable. This breaks contract rule 5 (one pipeline) until it is unified. | port `e3b3d8b` (2026-11-18) | `CMakeLists.txt` `copper_design` comment; linking `copper_design` with `copper_v2` gives duplicate symbols | One board model, one MNA, one DFM and one profile format, each chosen by measured coverage; the other retired only when the chosen one passes both test sets | owner (choice) |
| G-045 | S3 | OPEN | **`malloc` in ported library modules** (CLAUDE.md §2 requires Arena): `filesystem.c` (20 calls, UTF-16 path conversion on Windows), `copper_design_json.c` (3), `engineering_loop_json.c` (3), `report.c` (3), `design_resolution_json.c` (2), `topology_json.c` (1), `optimization_loop_json.c` (1). `build-tools/v2_rules_check.py` does not cover the ported modules, so it still prints OK. | port `e3b3d8b` | `grep -cE '\b(malloc\|calloc\|realloc)\(' src/v2_src/<module>.c` | Arena-backed buffers, and the modules added to `v2_rules_check.py` | — |
| G-046 | S3 | OPEN | **The overlay DFM (`dfm_check.c`) covers fewer rules than `dfm.c`**: no pad-to-track or PTH-to-track clearance. On the generated divider it reports UNVERIFIED (8 PASS, 0 FAIL, 25 UNVERIFIED, including "Rule exists in dataset, but this DFM geometry check is not implemented yet"), while `dfm.c` reports the two real G-001 violations. It is fail-closed (no false PASS) but narrower. | port cross-check | `copper user dfm-check examples/generated/divider/design.kicad_pcb --rules src/v2_src/jlcpcb_rules.rules --copper-oz 1 --edge routed` exits 3 | Coverage at least equal to `dfm.c`, checked on the same oracle boards (`dfm_kicad_demos`) | G-044 |
| G-047 | S4 | OPEN | The `copper` CLI help still names `copperv2` in its legacy command examples (listed in the overlay's own scan report) | port | `copper --help` | Consistent names | — |
| G-001 | S1 | OPEN | **The generated divider board FAILs the cited JLCPCB plated-hole-to-track limit.** The router works to Copper's rule profile, not the fab profile. Two violations: track 6 and track 5 against pad 5 (J1), at (8.000, 15.750) mm and (9.250, 15.750) mm; the limit is 280000 nm ("0.35mm is recommended, minimum 0.28mm"). | `ARCHITECTURE_PLAN.md` §1.1; reproduced by P00 | `build-v2/copper-core.exe dfm examples/generated/divider/design.kicad_pcb fab_profiles/jlcpcb_2layer.json` gives `verdict FAIL`, `clearance` 2 violations, exit 2 | A freshly built divider and the ESP32 LED board give V2 DFM PASS or a listed UNVERIFIED against `jlcpcb_2layer.json`, plus a regression test | G-002, G-003 |
| G-041 | S2 | OPEN | **copper-core exit codes collapse verdicts.** `report` exits 0 for both PASS and UNVERIFIED, and 2 for both FAIL and INVALID_INPUT. `dfm` and `gerber` exit 2 on FAIL, while `analyze` and `eseries` exit 2 on invalid input. A caller that relies on the exit code alone can take UNVERIFIED as success. The JSON keeps the distinction. The `pcba` contract is 0/1/2/3/4/64/70. | P00 code reading: `apps/copper_core.c:51,153,220`, `CLI.md:128-157` | `copper-core report examples/generated/divider/design.kicad_pcb fab_profiles/jlcpcb_2layer.json` exits 2 (observed; the verdict is FAIL). `CLI.md:157` documents that INVALID_INPUT also exits 2 and UNVERIFIED exits 0. | Exit codes follow the `pcba` taxonomy, or a documented superset that keeps PASS, FAIL, UNVERIFIED and INVALID_INPUT distinct; tests in `kicad_native`, `dfm_v2`, `finding` and `pcba_cli` | — |
| G-029 | S2 | OPEN | **The REST server can bind a non-loopback address with no authentication.** `serve-http --host H` is accepted. The served tools read boards and run `kicad-cli` exports under `--root`. | `TOOL_SERVER.md:87-90`, `apps/copper_core.c:377` | `copper-core serve-http --host 0.0.0.0 --port 0` listens on every interface | Either refuse non-loopback hosts without an explicit authentication option, or implement authentication; tested in `rest_server` | — |
| G-006 | S3 | OPEN | **DFM clearance violations report no measured distance.** `measured_nm` is −1 ("known only to be below the limit"), because the exact predicate decides without computing the distance. The October audit's DFM acceptance rule asks for the measured value. | `src/v2_src/dfm.h:142`; Oct-23 audit §5 | the G-001 command: both violations have `measured_nm: -1` | An exact (or exactly bounded) integer distance in each violation, checked against the brute-force oracle in `dfm_kicad_demos` | — |
| G-040 | S3 | IMPLEMENTED_UNVERIFIED (docs corrected in P00) | **Status documents over-stated some evidence.** `V2_STATUS.md` (tracks A and B) cites demo-board Gerber and drill exports without saying they were written by kicad-cli **10.0.5**, a development preview. The board *files* are KiCad 9.0 output; the exports are not. `ARCHITECTURE_PLAN.md` and `V2_STATUS.md` give "7 skipped", which holds only when `KICAD_CLI` and `KICAD_DEMOS_DIR` are set; a default run skips 11. | P00 re-run | `ctest --test-dir build-v2 -R "kicad_cli_real\|gerber_kicad_demos\|board_kicad_demos\|dfm_kicad_demos"` skips with no environment and passes with it; `gerber_test --real` prints "(development check)" for KiCad 10 | Dated reconciliation notes added to `V2_STATUS.md`, `RELEASE_READINESS.md` and `ARCHITECTURE_PLAN.md` (P00). VERIFIED once the owner has reviewed them. | — |
| G-021 | S4 | OPEN | **Three C test sources are not registered.** `tests/nlp_test.c` (retired NLP; does not link), `tests/sexpr_test.c` (hard-codes a KiCad path; recorded as failing on 2026-10-23), `tests/symlib_test.c` (passed when last run; not registered). | `RELEASE_READINESS.md` (2026-10-23) | `grep -c <name> CMakeLists.txt` gives 0 for all three (P00). The build and run outcome was not re-measured in P00. | Each is either registered with a portable fixture or retired with a recorded reason. No assertion is weakened. | — |
| G-031 | S4 | OPEN | **Absolute local paths in committed KiCad 9 fixtures** (`D:\sample_resistor_lol\…`, `C:\Program Files\KiCad\9.0\…`): the KiCad export metadata in `golden_netlist.kicad_net` and `.xml`, and in `golden_pcb_drc*.json`. | `RELEASE_READINESS.md` (2026-10-23) | `grep -l sample_resistor_lol -r tests/fixtures` lists 5 files | Owner decision; the fixture's meaning is unchanged either way | owner |
| G-023 | S4 | OPEN | **Legacy `hwc` selects the interpreter itself** (`py -3`, or `HWC_PY_LAUNCHER` / `HWC_PYTHON`). On this machine, MinGW's Python 3.9 on `PATH` breaks `hwc` during ctest. | `RELEASE_READINESS.md`; `apps/hwc.c:152-163`; session memory | running ctest with `D:\msys64\mingw64\bin` first on `PATH` makes `pcba_cli` fail with a TypeError | One documented interpreter-selection rule shared with `pcba`, tested | — |

## 3. Documented missing features within the release scope

| ID | Sev | Status | Gap | Source | Reproduction / observation | Acceptance evidence | Deps |
|---|---|---|---|---|---|---|---|
| G-002 | S1 | OPEN | **The V2 DFM is not in the design pipeline.** `pcba design`, `project verify` and `dfm check` use the legacy `dfm_pcb.c` (double mm, `profiles/*.rules`), not `src/v2_src/dfm.c` with the cited `fab_profiles/*.json`. | `ARCHITECTURE_PLAN.md` §1.2 row 11 | Code reading in P00: `tools/design.py:301-308` picks a `(rules …)` profile (`profiles/conservative.rules` or `esp32_2layer.rules`), and nothing in `tools/copper.py` or `tools/design.py` refers to `fab_profiles/` or `copper-core dfm` | `design.json` and `verification.json` contain V2 DFM rule records with the profile name, source and date; the divider and ESP32 boards are checked | — |
| G-003 | S2 | OPEN | **Routing is legacy Python** (`tools/layout.py`): a grid, 2 layers, uniform width per net class, and clearance taken from Copper rules | `C_FIRST_AUDIT.md` §2 step 2 | — | `router.c` passes the `test_layout` routing tests and is used by `pcba route` and `design` | G-001 |
| G-004 | S2 | OPEN | **The C placer does not use fab-profile limits** and places on one side only | `ARCHITECTURE_PLAN.md` row 7 | — | Placement-problem limits come from the fab profile; tested | G-002 |
| G-011 | S2 | OPEN | **The C `kicad-cli` runner is not in the pipeline.** The native gates still run through `tools/copper.py native_gates`. | `ARCHITECTURE_PLAN.md` row 10 | — | `project verify` runs `kicad_cli.c` and keeps the `native_gates` classification tests green | G-010 for native evidence |
| G-008 | S2 | OPEN | **`fabricate` and `cam-check` do not re-read the Gerber images** with `gerber.c`; only the drill and job files are compared | `ARCHITECTURE_PLAN.md` row 12 | — | `cam-check` compares the Gerber outline and flashes with the board model; tested on `manufacturing_audit/gerber_test` | — |
| G-013 | S3 | OPEN | **The `design.json` gates do not produce `finding.c` Findings**, so there is no single sorted report for the pipeline | `ARCHITECTURE_PLAN.md` row 13 | — | Pipeline output includes a Findings report that validates against `FINDINGS.md` | G-002 |
| G-012 | S2 | OPEN | **Legacy deterministic Python still to port** (`C_FIRST_AUDIT.md` §2 steps 3–10): footprint geometry, KiCad export, the library index, rules import, blocks, spec validation and lowering, orchestration, `hwc_kicad.py` | `C_FIRST_AUDIT.md` | — | Per step: the C replacement passes the same Python tests, and only then is the Python retired | per step |
| G-022 | S3 | OPEN | **The `pcba` C front end mostly delegates to legacy Python** (`tools/copper.py`, `design.py`). The V2 C modules can be reached only through `copper-core`, not through `pcba`. | `ARCHITECTURE_PLAN.md` row 0 | — | The `pcba` commands call the C engines directly as the port steps land; `pcba_cli` stays green | G-012 |
| G-016 | S2 | OPEN | **No model in the design path.** The LLM agent can only call the checking tools (load, DFM, export); there is no intent schema, part ranking or repair loop. | `ARCHITECTURE_PLAN.md` §3 | — | Model proposals pass the same C validators as user input; mock-backend tests; a live-model evaluation recorded separately | G-015 for live evidence |
| G-017 | S3 | OPEN | **No learning models** beyond the tiny intent classifier: no translation model and no part-ranking model | `V2_STATUS.md` | — | Training code in Python that reads C-exported data, with a held-out evaluation | G-016 |
| G-018 | S1 | OPEN | **No rated parts.** Generic parts have unknown voltage, power and height, so every electrical and assembly result is UNVERIFIED at best. | `RELEASE_READINESS.md` issue 5; Oct-23 audit §4 | `examples/generated/divider/report.json`: 7 warnings for unknown ratings and heights | A rated catalogue with cited datasheets makes the divider's electrical gate PASS with no warning suppressed | owner data |
| G-019 | S2 | BLOCKED_EXTERNAL | **Transcribed datasheet data is awaiting owner confirmation** (the ESP32-WROOM-32E/32UE limits, strapping notes and reference circuit; the LED's single-point model; the 0.2 mm drill in `esp32_2layer.rules`). The BOM sourcing fields are blank. | `RELEASE_READINESS.md` issue 4 | `examples/generated/divider/bom.csv` has blank Manufacturer and MPN | Owner confirmation recorded with its sources | owner |
| G-007 | S3 | OPEN | **The Gerber reader records G02/G03 arcs as unsupported**, and macro primitives 5 and 7 are incomplete | `GERBER.md`, `gerber.h:12,54-56` | `tests/gerber_test.c:164-166` pins a G02 draw as `GERBER_ISSUE_ARC_UNSUPPORTED` (a warning, never drawn as a line) | Arc draws parsed with exact integer bounds; tested on KiCad 9 Gerbers that contain arcs | — |
| G-024 | S3 | OPEN | **The English front end is a constrained grammar**: plurals and counts are unsupported, and only the documented phrasings work | `ENGINEERING_SPEC.md`; `RELEASE_READINESS.md` stage 6 | `engineering_spec` corpus | A larger corpus that keeps its expected rejections; or model proposals through G-016 | G-016 |
| G-025 | S2 | OPEN | **Newton fails above about 24 V** (diode ladder; roadmap N1) | `NUMERICAL_ROADMAP.md`; `numeric_oracles` expected failure | `numeric_oracles` pins this as an expected failure | Converges to 48 V or higher against the analytic oracle; the expected failure is removed only because it now passes | — |
| G-026 | S3 | OPEN | **Transient uses a fixed step with no LTE control**; trapezoidal rings on stiff nodes | `NUMERICAL_ROADMAP.md` T1/T2 | `numeric_oracles` expected failure | Adaptive step with an error estimate, tested against closed forms | — |
| G-027 | S3 | OPEN | **AC has no device capacitances**: transistor and diode AC is conductance only | `RELEASE_READINESS.md` stage 3 | — | Junction and diffusion capacitances, tested against analytic poles | — |
| G-028 | S3 | BLOCKED_EXTERNAL | **The C placer has not been measured on larger boards**: the ESP32 design needs KiCad 9 libraries | `C_FIRST_AUDIT.md` §3 | — | `layout_bench` on the ESP32 board with KiCad 9 libraries | G-010 |
| G-035 | S4 | OPEN | **Process**: the tracks document's "15-line API summary per session" was never written down | `V2_STATUS.md` | — | Summaries in the track documents | — |
| G-034 | S4 | DEFERRED | **Process**: "one PR per track, new files only" was not met; the wiring touched `CMakeLists.txt`, `apps/copper_core.c` and shared docs. Deferred: changing the history would not change the product. | `V2_STATUS.md` | — | — | owner |

## 4. Unverified implementations and untested integrations

| ID | Sev | Status | What is unverified | Why | Reproducible test kept | Acceptance evidence |
|---|---|---|---|---|---|---|
| G-010 | S1 | BLOCKED_EXTERNAL | **KiCad 9.x native ERC, DRC and netlist parity** for generated projects | Only KiCad 10.0.5 is installed here | `v2_native`, `parts_native`, `layout_native`, `design_acceptance`, `slice1_divider` (exit 77) | All five pass on a machine with KiCad 9.x, version recorded (the commands are in `RELEASE_READINESS.md`) |
| G-009 | S2 | BLOCKED_EXTERNAL | **The cause of KiCad 9.0.6's `lib_footprint_mismatch` warning on the ESP32 module.** The gate classifies it as UNVERIFIED. | Needs KiCad 9.0.6 | owner's `out.zip` evidence; `test_native_gates.Classification` | The cause confirmed and either fixed or documented, on KiCad 9 (`M0_ACCEPTANCE_EVIDENCE.md`) |
| G-014 | S2 | BLOCKED_EXTERNAL | **CUDA clearance kernel on a GPU** | No `nvcc` or GPU here | `dfm_gpu_compare` (exit 77), `apps/dfm_bench.c` | 1000 boards equal between GPU and CPU on the owner's RTX 4050, plus benchmark numbers |
| G-015 | S2 | BLOCKED_EXTERNAL | **The live Ollama client** (`qwen3:8b`) | No Ollama here | `llm_ollama_live` (exit 77), `scripts/run_llm_eval.ps1` | The live test and the eval run against a recorded model and version; the scores are reported, not assumed |
| G-020 | S2 | BLOCKED_EXTERNAL | **Linux GCC, MSVC, CI and ASan** | No MSVC, CI or ASan runtime here | the CMake presets `core` and `sanitize` | Green CI on Linux and Windows (MSVC), and an ASan run on Linux |
| G-030 | S3 | OPEN | **Copper Studio desktop** (C++17, Vulkan) has not been built since before V2 | Not attempted; the Vulkan SDK on this machine is not checked | `COPPER_BUILD_DESKTOP=ON` | Builds and starts; no engineering logic in the shell |
| G-033 | S4 | DEFERRED | **KiCad 10 stacked pins**: outside the KiCad 9 target | KiCad 10 is not the validation target | — | — |
| G-005 | S3 | BLOCKED_EXTERNAL | **Hole-to-edge DFM rule**: JLCPCB publishes no value, so the rule is UNVERIFIED on every board | Fabricator data | `dfm_v2` | A cited value from the fabricator. No value is invented. |

## 5. Documented unsupported capabilities (outside the release scope)

These are reported as NOT_IMPLEMENTED or UNVERIFIED by the product. They are **DEFERRED, not closed**: each stays here until it is implemented or the release scope changes by owner decision.

| ID | Status | Capability | Source |
|---|---|---|---|
| G-050 | DEFERRED | Copper zones, planes and thermal reliefs (in the board model, the DFM and the export) | `BOARD_MODEL.md`, `DFM_V2.md` (`zone_clearance`: "no filled zones"), Oct-23 audit §5 |
| G-051 | DEFERRED | More than 2 copper layers | `ENGINEERING_SPEC.md:112` |
| G-052 | DEFERRED | Controlled impedance, differential pairs, length matching | Oct-23 audit §5 |
| G-053 | DEFERRED | RF, EMC, signal integrity, return paths | Oct-23 audit §5 |
| G-054 | DEFERRED | Creepage and clearance for safety isolation and high voltage | Oct-23 audit §5 |
| G-055 | DEFERRED | Thermal field simulation and validated ampacity (the current budgets are estimates) | Oct-23 audit §5 |
| G-056 | DEFERRED | Assembly: paste and stencil, fiducials, polarity, pick-and-place qualification, panelisation | Oct-23 audit §5 |
| G-057 | DEFERRED | Procurement: stock, price, lifecycle, alternates | Oct-23 audit §5 |
| G-058 | DEFERRED | Multi-unit and hierarchical schematics | `ARCHITECTURE_PLAN.md` row 9 |
| G-059 | DEFERRED | Sparse solver, Monte Carlo, ngspice differential checks | `RELEASE_READINESS.md` |
| G-060 | DEFERRED | Arbitrary MCUs and arbitrary conversational requests | `IMPLEMENTATION_CONTRACT.md` §3 |

## 6. Proposed extensions (not defects; nothing has been requested yet)

Each needs a decision under contract rule 10 before it can become a dependency.

| ID | Status | Proposal | Source |
|---|---|---|---|
| E-01 | DEFERRED | One C design session holding the spec, decisions, parts, board and findings, shared by the CLI, RPC, REST and the agent | `ARCHITECTURE_PLAN.md` §2 |
| E-02 | DEFERRED | llama.cpp (MIT) as an in-process model back end | `ARCHITECTURE_PLAN.md` §4 |
| E-03 | DEFERRED | SQLite (public domain) in C for the library index | `ARCHITECTURE_PLAN.md` §4 |
| E-04 | DEFERRED | ngspice (BSD) as an optional physics cross-check oracle | `ARCHITECTURE_PLAN.md` §4 |
| E-05 | DEFERRED | FreeRouting (GPL-3, external process only) and KiKit (MIT) as optional tools | `ARCHITECTURE_PLAN.md` §4 |
| E-06 | DEFERRED | A worker pool and GPU batch checks for scale | `ARCHITECTURE_PLAN.md` §3 |

## 7. Historical findings already fixed (kept as evidence)

These are from the owner's report `COPPER_EXPECTATIONS_VS_REALITY_DFM_AUDIT_2026-10-23.md`, fixed in M0 (`M0_ACCEPTANCE_EVIDENCE.md`, 2026-10-24). Their regression tests passed in the P00 baseline at `7199ef6`.

| ID | Status | Original finding (2026-10-23) | Root cause found in M0 | Regression test (passing at `7199ef6`) |
|---|---|---|---|---|
| H-01 | VERIFIED | Unicode µ and Ω prompts gave INVALID_INPUT | Harness: the prompt was double-encoded (PowerShell 5.1 ANSI read) | `engineering_spec` (`test_spec.test_double_encoded_text_is_named_and_bom_ignored`), `prompt_acceptance_harness` (`UnicodeRun`) |
| H-02 | VERIFIED | The `pcba` JSON envelope stripped escapes | Product: `extract_message` | `prompt_acceptance_harness` |
| H-03 | VERIFIED | "DC solve from English" was a harness false negative | Harness assertion | `prompt_acceptance_harness` (case `e2e:dc`) |
| H-04 | VERIFIED | English → board gave `NO_JSON/70` | Product: unplaced parts all sat at (0, 0) | `native_gates` (`Placement`), harness case `e2e:board` |
| H-05 | VERIFIED | An observational ESP32 FAIL was printed as `[PASS]` | Harness taxonomy | `prompt_acceptance_harness` (`test_observational_failure_is_not_a_pass`) |
| H-06 | VERIFIED (logic); its KiCad 9 cause is G-009 | DRC exit 5 on a warning was treated as FAIL | Gate classification | `native_gates` (`Classification`) |
| H-07 | VERIFIED for Copper's gate | stderr noise and exit 9009 in the raw DRC runner | That was the owner's script; Copper's gate reads the report | `native_gates` (`test_stderr_noise_with_a_clean_report_passes`, `test_crash_without_report_is_unverified`) |
| H-08 | VERIFIED | Empty boards passed DRC | A `board_content` check was added | `native_gates` (`Content`) |
| H-09 | VERIFIED | Duplicate "placed project" code crashed without `(rules …)` | Unified `layout.placed_project` | `layout_optimizer` (`test_placed_project_records_widened_copper_even_without_rules`) |

Historical successes recorded in older documents remain valid **at their own dates and revisions**:
- `RELEASE_READINESS.md` (2026-10-23): 46/46;
- `M0_ACCEPTANCE_EVIDENCE.md` (2026-10-24): prompt acceptance PASS 34, FAIL 0, SKIPPED 1;
- `V2_STATUS.md` (2026-11-07).

They are not re-asserted for the current revision except where P00 re-measured them.

## 8. Branch `lab/copper-lab-integrated` (2026-11-21; pipeline hard rules H1–H13)

These rows were registered on the lab branch. Earlier rows are not rewritten here; where the lab branch changed the
picture for an earlier row, the change is noted below and the row keeps its original text until that branch is
merged.

| ID | Sev | Status | Gap or change | Evidence |
|---|---|---|---|---|
| G-003 (note) | S2 | IN_PROGRESS | The Manus pipeline now routes in C (`src/v2_src/autoroute.c`, `copper design route`, commit `ac99bcf`). The `pcba` path still routes in `tools/layout.py`. KiCad 9 is not run. | `copper_autoroute_tests`; `manus/golden_boards.ps1`: 3 boards ROUTED, 0 unconnected (KiCad 10.0.5) |
| G-044 (note) | S2 | OPEN (owner decision) | `CONSOLIDATION_PLAN.md` names one engine per job for new work. Nothing is deleted or renamed (H10); merging the two C families stays an owner decision. | `CONSOLIDATION_PLAN.md` |
| G-061 | S3 | VERIFIED | `copper_design` had no sanitizer build. Step 0b applies `COPPER_V2_SANITIZE` (UBSan trap mode on MinGW) to it. | clean build `lab-0b-ubsan`: 0 warnings; ctest 78 passed, 6 skipped, 0 failed. ASan NOT RUN (MinGW) |
| G-062 | S4 | VERIFIED | The golden-board check (testing loop T5) was outside the repository. It is now `manus/golden_boards.ps1` with inputs and numbers in `manus/golden/`. | PASS on the Step 0b build. With a deliberately altered baseline it reported CHANGED and FAIL and exited 1 |
| G-063 | S4 | VERIFIED | `GAP_REGISTER.md`, `PROMPT_RESULTS.md`, `IMPLEMENTATION_CONTRACT.md` and `ARCHITECTURE_PLAN.md` existed only as untracked files in the main checkout. Copies are now on this branch. | this file |
| G-064 | S4 | DEFERRED (owner) | `router.c` and `placer.c` have no users outside their own tests. Kept (H10); new code uses `autoroute.c` and `placement.c`. | `CONSOLIDATION_PLAN.md` §2 |
| G-065 | S2 | OPEN | The ESP32 + LED golden board has 2 ERC errors (`power_pin_not_driven`: no PWR_FLAG symbols are placed) and 12 DRC errors (`drill_out_of_range`: 0.2 mm vias inside KiCad's ESP32-WROOM-32 footprint). Both are known and reported; neither is fixed. | `manus/golden/baseline.json` |
| G-066 | S2 | OPEN | The design step uses fixed templates (`template_gen.c`). A request outside the three templates is NOT_IMPLEMENTED. The roadmap replaces this with blocks and a constraint solver (Phase 2). | `PIPELINE_HARD_RULES.md`; scope check in `template_gen.c` |
| G-018 (note) | S1 | IN_PROGRESS | Step 1a adds `copper catalogue check` (`src/v2_src/parts_check.c`): every part's missing identity, incomplete citations, missing required ratings, unverified sources and KiCad mapping problems, all at once. No rated parts were added yet (Step 1b). | `copper_parts_check_tests` (11 test groups; a deliberately broken KiCad check is caught) |
| G-067 | S2 | OPEN (owner decision) | **Two part-catalogue formats.** copper-core's `(catalogue (part ID ...))` (`profiles/*.parts`; holds the transcribed ESP32-WROOM-32E data with MCU strapping and reference-circuit data, used by `pcba` and `tools/blocks.py`) and copper_design's `(copper_parts (schema 1) (part (id ...) ...))` (`part_catalogue.c`, per-rating sources with verified/unverified, used by `copper`). `copper catalogue check` reads only the second; it reports the first as INVALID_INPUT. A curated library (Step 1b) needs one format chosen. | `copper catalogue check profiles/mcu_modules.parts` → INVALID_INPUT, exit 2 |
| G-018 (note 2) | S1 | IN_PROGRESS | Step 1b: `parts_library/` holds 19 real parts in `copper_parts` format (resistors, capacitors, LED, Schottky diode, MOSFET, LDO, ESP32-WROOM-32E-N4, TMP102, switch, headers, USB-C). Every value is from the manufacturer's document with URL, revision and page, and **all are unverified** until a person checks them (H2). | `copper catalogue check` per file (KiCad 10.0.5 libraries): 18/19 parts KiCad PASS, 0 FAIL except G-069; ctest `parts_library_parse` |
| G-068 | S3 | OPEN (owner decision: base module) | **`part_catalogue.c` rejects every negative rating value**, although its message says "temperature may be signed" (`part_catalogue.c:388`: `result.value < 0` is tested before the temperature exception). So no part can record a minimum temperature below 0 °C. The library states these ranges in the descriptions instead. The fix is one condition in a base module, so it waits for the owner's approval (H10). | `rating temperature minimum -55000 mC` → INVALID_INPUT "ratings must be positive (temperature may be signed)" |
| G-069 | S3 | OPEN | **ESP32-WROOM-32E GND pin map fails against KiCad 10 libraries.** KiCad 10 stacks GND pins 1/15/38/39 into one symbol pin `[1,15,38,39]`, so `kicad_assets.c` finds no pin "1". KiCad 9 (the target) is expected to have separate pins; not verified here (G-010). Related: G-033. | `copper catalogue check parts_library/ics.parts` with KiCad 10.0.5 roots → 1 FAIL (KICAD_ASSET) |
| G-068 (update) | S3 | VERIFIED (2026-11-21, owner approved) | Fixed in `part_catalogue.c`: only non-temperature ratings must be positive; a temperature may be zero or negative. The 19 parts in `parts_library/` now carry their minimum temperatures, each citing the same page as the maximum. | `test_parts_check.c` `test_signed_temperature_only`: before the fix, `-55000 mC` gave RANGE_ERROR and the test failed; after, it passes. Negative or zero resistance, voltage or current is still rejected |
