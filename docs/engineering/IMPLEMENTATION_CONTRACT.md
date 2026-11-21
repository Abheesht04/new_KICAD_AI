# Implementation contract for prompts P01–P52

*Established 2026-11-08 by P00 (baseline inventory, logged before P01), at revision `7199ef6b1e13a3df579c57b3d7cd874d38cb60ee` on branch `v2/complete`.*

This contract sets the working rules, the status vocabulary, the supported release scope and the capability baseline for the prompt series P01–P52. It does not replace `CHARTER.md` or `CLAUDE.md`. Where they differ, the dated rule in `CHARTER.md` §3.1 and `CLAUDE.md` (owner decision 2026-11-06) wins.

Related files:
- [GAP_REGISTER.md](GAP_REGISTER.md): every known defect, missing feature, unverified implementation and proposed extension, with a status.
- [PROMPT_RESULTS.md](PROMPT_RESULTS.md): one entry per prompt, giving what changed, the exact commands, the counts and the remaining limits.

---

## 1. Working rules (apply to every prompt P01–P52)

1. **Implement, don't describe.**
   - Each prompt delivers its milestone as working, tested code.
   - A plan, a stub, a mock demonstration or a TODO is not a delivery.
   - If the milestone already exists, demonstrate it with the commands and evidence; don't duplicate it.
2. **Preserve the repository.**
   - Keep user edits, module ownership and file locations; don't rename or move files.
   - Use an isolated milestone branch or worktree when that helps.
   - Don't push, publish, tag or purchase anything unless that prompt asks.
   - Changes to protected core files (`g2_types`, `IR`, `arena`, `excellon`, `filesystem`) carry a note in the commit.
   - Attribution must be truthful (`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`).
3. **Language split** (`CHARTER.md` §3.1, `CLAUDE.md` §1).
   - Deterministic product work is C17; CUDA is optional.
   - Python is for NLP, learning, model hosting and independent test harnesses.
   - Legacy deterministic Python in `tools/` may be fixed but not extended; new deterministic features go in C.
   - The C++ desktop shell (`desktop/`) stays. Engineering decisions belong in the shared C engine, never in the shell.
4. **V2 engine rules** (`src/v2_src/`):
   - int64 nanometres with overflow-safe arithmetic;
   - Arena ownership and explicit status enums;
   - no float or libm in geometry or DFM;
   - no hidden global state, and no `exit` or `printf` in library modules;
   - no invented component or fabricator limits: every limit is in a data file with a cited source and date.
5. **One pipeline:** Spec IR → topology, model and catalogue → analysis, layout and DFM → native KiCad.
   - No second competing path is added.
   - Model output is untrusted input: it is validated like user input and can never assign an engineering verdict.
6. **Verdicts.**
   - PASS requires the relevant check to have run.
   - FAIL, UNVERIFIED, NOT_IMPLEMENTED, INVALID_INPUT and ERROR stay distinct.
   - Missing data is not unlimited capacity.
   - A numerical solution is not a verified circuit, and an exported board is not a manufactured board.
7. **Native evidence.**
   - Native acceptance uses actual KiCad 9.x, with the exact version recorded.
   - KiCad 10 results are development previews and are never counted as native-9 evidence.
   - Synthetic fixtures, Copper-generated golden files, mock models and optional skips are labelled as such.
8. **Tests.**
   - Behavioural changes get independent engineering oracles and meaningful regressions.
   - Never weaken an assertion, remove a finding, fabricate a source, replace a failing test with a skip, or treat output-file existence as correctness.
9. **Loop.**
   - Observe the failure, make a bounded fix, run the targeted checks, then the milestone suite.
   - Don't rerun unrelated expensive suites without a reason.
10. **Dependencies.**
    - No new required external dependency without an explicit decision consistent with the charter.
    - Optional research or reference tools stay outside the default build.
    - The supplied milestone is the work scope. Report a real conflicting constraint instead of inventing approval steps.
11. **Record keeping.** After each prompt, update `GAP_REGISTER.md` and `PROMPT_RESULTS.md` with:
    - the changed behaviour and the files touched;
    - the exact commands, counts, exit codes and environment;
    - the remaining limits and the next dependencies.

    Then stop at that prompt's stage boundary for review.
12. **External blockers.**
    - If hardware, credentials, a platform or a live model is unavailable, finish the independent work and keep a reproducible test.
    - Mark that specific evidence BLOCKED_EXTERNAL.
    - Never mark it passed by substituting a fake.

## 2. Status vocabularies

There are two vocabularies. They are never mixed.

**Gap status** (in `GAP_REGISTER.md`; it describes the work item):

| Status | Meaning |
|---|---|
| OPEN | Known, reproducible or documented, and no work has started |
| IN_PROGRESS | Work has started on a named branch |
| IMPLEMENTED_UNVERIFIED | Code exists, but its acceptance evidence has not been produced (for example, it needs KiCad 9, a GPU or a live model) |
| VERIFIED | The acceptance evidence named in the row was produced at the recorded revision |
| BLOCKED_EXTERNAL | Needs hardware, a platform, credentials, a live model or an owner decision that is not available here |
| DEFERRED | Deliberately postponed, with a reason. **DEFERRED is not closed**: the gap remains and is still reported. |

**Result verdicts** (produced by the product and the tests; they describe a check):
- PASS, FAIL, UNVERIFIED, NOT_IMPLEMENTED, INVALID_INPUT, ERROR;
- the harness adds BLOCKED, SKIPPED and OBSERVATIONAL (`tools/prompt_acceptance.py`);
- the exit codes are in `pcba help`: 0 PASS, 1 FAIL, 2 INVALID_INPUT, 3 UNVERIFIED, 4 NOT_IMPLEMENTED, 64 USAGE, 70 ERROR;
- in ctest, exit 77 means an UNVERIFIED skip.

## 3. Initial supported release scope

This scope is fixed for the series. It is not to be broadened to "arbitrary electronics". Nor is it to be narrowed later so that a failing case falls outside it: a case in scope that fails stays a FAIL.

**In scope:**

| Dimension | Supported |
|---|---|
| Requests | Constrained English as documented in `ENGINEERING_SPEC.md`, and `.espec` files. Model-proposed intent once it passes the same validators. |
| Circuit families | Resistor divider; RC and RL filters; diode circuit; one ESP32-WROOM-32E/32UE module with GPIO indicator LEDs (`profiles/esp32_led.design`); the blocks listed by `pcba blocks` (reference circuit, I2C pull-ups, USB-C sink, LDO, I2C sensor, reset/boot) using catalogue parts |
| Analysis | DC (MNA), small-signal AC, fixed-step transient; diode, BJT and MOSFET models within the limits recorded in `NUMERICAL_ROADMAP.md` |
| Board | 2-layer rigid board, rectangular or closed polygonal outline, SMD and through-hole pads, tracks and vias; no zones |
| Native toolchain | KiCad 9.x (`kicad-cli` and the 9.x symbol and footprint libraries) |
| Fabricator | `fab_profiles/jlcpcb_2layer.json` (JLCPCB standard 2-layer, values retrieved 2026-11-06) |
| Outputs | KiCad 9 project (`.kicad_sch`, `.kicad_pcb`, `.kicad_pro`, `.kicad_dru`), engineering BOM draft, reports, Gerber and Excellon via `kicad-cli`, a Findings report |
| Platforms | Windows 10/11 x64 with MinGW-w64 GCC 12+ (tested); Linux with GCC (supported by the CMake files, not run here); MSVC (supported by CMake, never run) |
| Model | Optional. Mock backend by default; Ollama (`qwen3:8b`) as an opt-in client |
| GPU | Optional. CPU reference by default; CUDA clearance kernel behind `COPPER_CUDA` |

**Out of scope, to be reported as NOT_IMPLEMENTED or UNVERIFIED rather than attempted:**
- boards with more than 2 layers, copper zones, planes and thermal reliefs;
- controlled impedance, differential pairs and length matching;
- RF, EMC, signal integrity, creepage and clearance for safety isolation, thermal field simulation;
- panelisation, stencil and paste, fiducials, pick-and-place qualification;
- procurement (stock, price, lifecycle), and any MPN or rating not supplied by a cited catalogue;
- arbitrary MCUs and arbitrary conversational requests.

## 4. Baseline inventory (revision `7199ef6`, 2026-11-08)

### 4.1 Environment

| Item | Value |
|---|---|
| OS | Windows 10 Pro 10.0.19043 |
| C compiler | MinGW-w64 GCC 12.1.0 (MSYS2 Rev2), C17 |
| Build | CMake 4.4.4, Ninja 1.13.2 (`build-tools/venv`) |
| Python | 3.12.0 (MinGW's Python 3.9 must not be first on `PATH` for ctest) |
| KiCad | **10.0.5 only** (`D:\Practice\tools\kicad-extract`, a development preview). KiCad 9.x is **not installed**. |
| CUDA / GPU | `nvcc` and `nvidia-smi` absent |
| Ollama | absent |
| MSVC | absent |
| CI | none: the repository has no CI configuration |
| Build trees | `build-v2` (Debug, all `COPPER_*` options OFF), `build-v2san` (UBSan in trap mode) |

### 4.2 Public commands

**`pcba`** (primary CLI, `apps/pcba`). Most commands are C front ends over `tools/copper.py`, `tools/design.py` and related legacy Python.
- General: `help`, `version`, `doctor`, `test`.
- Project: `project new|check|build|verify|fabricate|cam-check`.
- KiCad: `kicad new|open`.
- Simulation: `simulate dc|ac|transient`.
- Design and spec: `design`, `spec parse|check|lower`.
- Parts and library: `parts search|add|remove|import`, `library env|scan|search|pair`.
- Layout: `place`, `route`, `blocks`.
- Rules and DFM: `rules import`, `dfm check`.

**`copper-core`** (machine back end, C):
- physics: `analyze`, `transient`, `ac`, `classify`, `models`, `catalogue`, `eseries`;
- V2 board and CAM: `drill`, `board`, `gerber`, `report`, `dfm`;
- KiCad: `kicad drc|gerbers|drill|pos`, `kicad-new`, `kicad-open`;
- servers and agent: `serve`, `serve-http`, `agent-eval`;
- layout: `place`.

**Other apps:**
- `hwc`: the legacy V1 CLI; its `project` subcommands launch Python through `py -3`, or `HWC_PY_LAUNCHER` / `HWC_PYTHON` when set;
- `fabcheck_demo`, `numeric_bench`, `dfm_bench`, `prompt`: developer tools.

**Desktop:** `copper-studio` (C++17, GLFW and Vulkan, `COPPER_BUILD_DESKTOP=ON`). Not built in this baseline.

### 4.3 CMake build options

| Option | Default |
|---|---|
| `HWCOMPILER_BUILD_TESTS` | ON |
| `COPPER_CUDA` | OFF |
| `COPPER_V2_SANITIZE` | OFF |
| `COPPER_SANITIZE` | OFF |
| `COPPER_BUILD_DESKTOP` | OFF |

Presets: `core`, `desktop`, `sanitize`.

### 4.4 Registered CMake tests (76)

| Group | Tests |
|---|---|
| Core C | `intern`, `ir`, `ir_nm`, `matrix`, `mna`, `model`, `node_map`, `part_registry`, `provider`, `pcb_dfm`, `kicad_pcb`, `e_series`, `fab_profile_io`, `nlp_rules`, `design_validate`, `schematic_export`, `mna_from_design`, `device_model`, `cplx`, `mna_ac`, `nonlinear_models`, `transient` |
| V2 C | `g2_types`, `kicad_native`, `board`, `json`, `dfm_v2`, `kicad_cli`, `gerber`, `finding`, `tool_server`, `llm`, `dfm_gpu`, `rest_server`, `placement`, `v2_src_shared_in_sync` |
| V2 external or real data | `kicad_cli_real`, `gerber_kicad_demos`, `tool_server_stdio`, `llm_eval_mock`, `board_kicad_demos`, `dfm_kicad_demos`, `rest_server_http` |
| Optional (skip with 77 unless the hardware or service is present) | `llm_ollama_live` (needs Ollama), `dfm_gpu_compare` (needs CUDA) |
| CLI and integration | `cli_quote`, `hwc_help`, `hwc_cli_integration`, `v2_integration`, `pcba_cli`, `bridge_regression` |
| Pipeline (Python harnesses over C and legacy Python) | `engineering_spec`, `parts_registry`, `layout_optimizer`, `design_compiler`, `dfm_rules`, `circuit_blocks`, `kicad_rules`, `kicad_library`, `native_gates`, `prompt_acceptance_harness`, `slice1_divider`, `numeric_oracles` |
| KiCad 9 native (skip with 77 without KiCad 9.x) | `v2_native`, `parts_native`, `layout_native`, `design_acceptance`, plus the KiCad 9 part of `slice1_divider` |

C test sources that exist but are not registered: `tests/nlp_test.c`, `tests/sexpr_test.c`, `tests/symlib_test.c` (gap G-021).

### 4.5 Project-format limits

| Format | Read | Write | Limits |
|---|---|---|---|
| `.copper` (S-expression project) | Python and C (`IR.c`) | Python | the project contract in `DESIGN.md` |
| `.espec` (EngineeringSpec) | Python (`spec_ir.py`) | Python | constrained grammar; plurals and counts unsupported |
| `.parts` catalogues, `.rules` profiles, `.design` profiles | C and Python | C and Python | provenance is required on import |
| `.kicad_pcb` | C (`board.c`; KiCad 9 and 10 files) | Python export plus C writers | no zones in the model; rotated rectangle pads, round-rectangle, oval and custom pads are approximated as documented in `BOARD_MODEL.md` |
| `.kicad_sch` | Python (bridge) | Python, `kicad_schematic.c` | single-unit symbols; no hierarchy |
| `.kicad_sym` / `.kicad_mod` | Python (`kicad_library.py`), C (`symlib.c`, partial) | — | KiCad 10 stacked pins are UNVERIFIED |
| `.kicad_dru` / `.kicad_pro` rules | Python (`kicad_rules.py`) | Python | the subset in `KICAD_RULES.md` |
| Excellon | C (`excellon.c`) | via `kicad-cli` | metric and inch, routed slots as documented |
| Gerber X2 | C (`gerber.c`) | via `kicad-cli` | **G02/G03 arcs are recorded as unsupported**; macro primitives 5 and 7 are incomplete |
| Fab profile JSON | C (`json.c`, `dfm.c`) | — | one profile: `jlcpcb_2layer.json` |

## 5. Capability matrix (baseline)

**Legend.**
- **Tested:** passing tests against independent oracles here.
- **Internal only:** tested internally; the KiCad 9 native gate has not run.
- **Unverified:** exists, with no passing evidence here.
- **Not integrated:** works standalone but is not used by `pcba design`.
- **Not implemented.**

| Area | Capability | Status at `7199ef6` | Evidence | Gap IDs |
|---|---|---|---|---|
| CLI | `pcba` command tree, JSON envelope, exit codes, safe process launching | Tested | `pcba_cli`, `cli_quote` | G-022 |
| CLI | `copper-core` V2 commands | Tested | V2 C tests, `*_kicad_demos` | — |
| CLI | Legacy `hwc` | Tested; uses a separate path | `hwc_help`, `hwc_cli_integration` | G-022, G-023 |
| Desktop | Copper Studio (Vulkan) | Unverified (not built) | — | G-030 |
| Import | Constrained English → EngineeringSpec | Tested (corpus) | `engineering_spec`, `prompt_acceptance_harness` | G-024 |
| Import | KiCad rules import | Tested (Python, legacy) | `kicad_rules` | G-012 |
| Import | KiCad library index (SQLite FTS5) | Tested (Python, legacy) | `kicad_library` | G-012 |
| Import | `.kicad_pcb` → board model (C) | Tested on 13 KiCad 9.0 demo boards | `board_kicad_demos` | — |
| Model | Mock LLM, agent loop, JSON-schema to GBNF | Tested (mock) | `llm`, `llm_eval_mock` | — |
| Model | Ollama client | Implemented; live check not run | `llm_ollama_live` skips | G-015 |
| Model | LLM in the design path (intent, parts, repair) | Not implemented | — | G-016 |
| Model | Learning (translation, part ranking) | Not implemented (only the tiny intent classifier) | `tools/train_intent.py` | G-017 |
| Physics | DC, AC and transient MNA | Tested | `mna*`, `transient`, `numeric_oracles` | G-025, G-026, G-027 |
| Geometry | int64 nm primitives, exact clearance | Tested | `g2_types`, `dfm_v2`, `dfm_gpu` | — |
| Layout | C placer | Tested (7 synthetic fixtures) | `placement`, `layout_optimizer` | G-004, G-028 |
| Layout | Router | Tested (Python, legacy); uses Copper rules, not fab rules | `layout_optimizer` | G-001, G-003 |
| DFM | V2 DFM against cited JLCPCB limits | Tested; **not integrated** | `dfm_v2`, `dfm_kicad_demos` | G-002, G-005, G-006 |
| DFM | Legacy DFM (`dfm_pcb.c`, double mm) | Tested; it is the one in the pipeline | `pcb_dfm`, `dfm_rules` | G-002 |
| CAM | Excellon reader | Tested on real KiCad 9.0.6 output | `kicad_native`, `board_kicad_demos` | — |
| CAM | Gerber reader | Tested on KiCad 9.0.6 and 237 demo files; arcs unsupported | `gerber`, `gerber_kicad_demos` | G-007, G-008 |
| CAM | `fabricate` re-read of the Gerber images | Not integrated (drill files only) | — | G-008 |
| Native | KiCad 9 ERC, DRC and netlist parity | Internal only: **blocked here** | 5 tests skip | G-010 |
| Native | `kicad-cli` runner (C) | Tested with a fake CLI; real KiCad 10 only as a preview | `kicad_cli`, `kicad_cli_real` | G-010, G-011 |
| Report | One sorted Findings report (C) | Tested; `design.json` gates don't use it yet | `finding` | G-013 |
| Serving | JSON-RPC tool server, REST | Tested; loopback only, no authentication | `tool_server*`, `rest_server*` | G-029 |
| GPU | CUDA clearance kernel | Implemented; GPU not run | `dfm_gpu_compare` skips | G-014 |
| Parts | Catalogue parts with provenance | Tested; no rated parts, BOM sourcing blank | `parts_registry` | G-018, G-019 |
| Platform | Windows MinGW | Tested | this baseline | — |
| Platform | Linux GCC, MSVC, CI | Unverified | none | G-020 |
| Sanitizers | UBSan (trap mode) | Tested on the V2 modules | `build-v2san` | — |
| Sanitizers | ASan | Not run (no runtime on MinGW) | — | G-020 |
