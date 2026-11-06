# C-first audit (2026-11-06)

The owner's hard rule (`CHARTER.md` §3.1, `CLAUDE.md`):
- **C17, with CUDA opt-in, for everything deterministic:** physics, geometry and DFM, placement and routing, KiCad I/O, CAM checks, fab profiles, the tool server, the LLM bridge client, and REST and GPU serving.
- **Python only for:** NLP and transformers; learning (training, fine-tuning, translation models, part-picking models over C-parsed libraries and DFM rules); hosting the model server.

This audit records:
- how far the repository already follows that rule;
- the order in which the legacy deterministic Python is ported;
- the state of each "Copper V2 parallel tracks" item (A–H).

## 1. Where things stand

| Area | Language today | Rule says | Status |
|---|---|---|---|
| MNA DC, AC, transient, device models, condition estimate | C (`src/mna*.c`, `transient.c`, `device_models.c`) | C | **compliant** |
| Electrical checks, E-series, design validation, model registry | C (`copper.c`, `e_series.c`, `design_validate.c`, `model_registry.c`) | C | **compliant** |
| Intent classifier (inference) | C (`Extras/tokenizer.c`, `src/v2/transformer.c`) | C inference, Python training | **compliant** (training in `tools/train_intent.py`) |
| Exact nm geometry, Excellon reader, KiCad library tables, native KiCad projects | C (`src/v2_src/`) | C | **compliant**; V2 rules met (int64 nm, Arena, status enums, no `printf` or `exit`) |
| **Placement** | was Python (`tools/layout.py` `initial_placement`, `anneal`) | C | **ported** (§3): `src/v2_src/placement.c`; `layout.optimize` now calls it |
| Routing (grid A*, octilinear, rip-up), width rounds, clearance margins | Python (`tools/layout.py`) | C | **legacy**, port step 2 |
| `.copper` project build, KiCad schematic and PCB export, footprint geometry | Python (`tools/copper.py`, 1249 lines) plus C writers (`kicad_pcb.c`, `kicad_schematic.c`) | C | **legacy**, port steps 3–4 |
| DFM checks on built boards | C (`dfm.c`, `dfm_pcb.c`, `fab_profile_io.c`), in `double` mm | C, int64 nm (V2) | **C but not V2**: track C replaces it with nm checks against data-file profiles |
| KiCad library index (SQLite FTS5) | Python (`tools/kicad_library.py`) | C (parsing) + Python (learning over it) | **legacy**, port step 5; the C side has `kicad_library_table.c` |
| KiCad rules import (`.kicad_dru`, `.kicad_pro`) | Python (`tools/kicad_rules.py`) | C | **legacy**, port step 6 |
| Circuit blocks (LDO, I2C, USB-C sink…) | Python (`tools/blocks.py`) | C (deterministic expansion) | **legacy**, port step 7 |
| Spec IR validation and lowering | Python (`tools/spec_ir.py`) | C (validation, lowering); Python may emit it | **legacy**, port step 8 |
| English → spec (rules and patterns) | Python (`tools/spec_nlp.py`) | Python (NLP) | **allowed** |
| Design compiler orchestration (`design.py`), KiCad gates via `kicad-cli` | Python | C (track D runner, track F tool server) | **legacy**, port step 9 |
| `hwc_kicad.py` (S-expression bridge, catalogue, divider generator) | Python | C (IR already exists) | **legacy**, port step 10 |
| `train_intent.py` | Python (PyTorch) | Python (learning) | **allowed** |
| `prompt_acceptance.py`, `numeric_bench.py`, `layout_bench.py` | Python | test and benchmark harnesses | **allowed as harnesses**: they drive the C binaries and judge them; no product logic |
| CLI front ends | C (`apps/pcba`, `apps/hwc.c`, `apps/copper_core.c`) | C | **compliant** |

**In short:** the physics, the CLI, the V2 geometry and CAM readers, and placement are C. Routing, project build and KiCad export, the library index, rules import, blocks, spec lowering and orchestration are still deterministic Python. That Python is legacy:
- it is fixed but not extended;
- it is ported in the order below;
- each part is retired only when its C replacement passes the same tests.

## 2. Port order

Each step gets its own branch, with new files in `src/v2_src/`. Each is done when the Python tests for that part pass against the C code. Only then is the Python retired.

| Step | Python | C replacement | Depends on |
|---|---|---|---|
| 1 | `layout.py` placement (`initial_placement`, `anneal`) | `placement.c` | — **done** |
| 2 | `layout.py` routing (`route`, `route_with_ripup`, `clearance_margins`) | `router.c` on g2 geometry and the BVH | track B |
| 3 | `copper.py` footprint geometry, `resolved` copper | `board.c` (track B), `footprint.c` | IR |
| 4 | `copper.py` KiCad schematic and PCB export | `kicad_write.c`, the existing `kicad_pcb.c` and `kicad_schematic.c` | 3 |
| 5 | `kicad_library.py` (table reading, symbol and footprint parsing) | `kicad_library_table.c` and a C indexer; Python keeps only learning over the index | IR |
| 6 | `kicad_rules.py` | `kicad_rules.c` | IR |
| 7 | `blocks.py` | `blocks.c` with data-file block definitions | 3 |
| 8 | `spec_ir.py` validation and lowering | `spec_ir.c` | — |
| 9 | `design.py` orchestration, `copper.py` native gates | tool server (track F) and the `kicad-cli` runner (track D) | D, F |
| 10 | `hwc_kicad.py` | IR plus the above | all |

## 3. Placement, ported

`src/v2_src/placement.{c,h}` (copper_v2, C17, strict flags, Arena-only, status enums, no libm):

- **The same rules, exact:**
  - legality is integer nm on rectangles: outline, pad-to-edge, courtyard gap, silkscreen over silkscreen or pads, and board and footprint keepouts by flag;
  - a 2 nm margin absorbs the bridge's rounding of each float to the nearest nm, so C is never less strict than `Board.legal`;
  - an exact extent early-out skips pairs that cannot interact.
- **The same objective, integer:** HPWL in nm plus the weighted occupied area, decoupling distance (integer square root) and keepout-to-edge distance. Weights come from the problem file in ppm.
- **A stronger search:**
  - seeded splitmix64;
  - an integer cooling schedule;
  - a fixed-point `exp` table (Q32, tested monotone, e⁻¹ exact to the constant);
  - moves are shift (span shrinking from 8 steps to 1), rotate, swap, and **pull to the median of the part's nets' bounding boxes**;
  - nets touched by a move are re-measured incrementally;
  - one move per movable part per temperature step;
  - a final **greedy refinement** at half the grid step, over all rotations.
- **Interface:**
  - `copper-core place PROBLEM ITERATIONS SEEDS` takes a version-1 placement-problem S-expression and prints JSON (constructive start, failures, one annealed placement per seed);
  - `layout.optimize` writes the problem (`c_problem_text`) and uses the result;
  - the Python placer runs only when the C placer cannot take the problem (a rotation that is not a multiple of 90°, or an older copper-core);
  - `problem['placer']` records which ran.
- **Tests:**
  - `tests/placement_test.c`:
    - parse errors with line or form index;
    - an exact hand-computed cost;
    - gap and edge boundaries to the nanometre;
    - determinism;
    - fixed parts, keepouts, no legal position and an illegal start;
    - the exp table;
    - a golden problem written by the Python bridge for the divider board with KiCad 9 library footprints (Copper-generated: KiCad has no placement-problem format).
  - `tests/test_layout.py` `CPlacer`, run against all seven layout fixtures:
    - the same constructive cost as Python;
    - zero `Board.violations`;
    - annealed results no worse than the start;
    - deterministic results;
    - used by `optimize`;
    - the Python fallback path.
  - All 57 ctest tests pass with `optimize` on the C placer.
- **Measured against the Python placer** (3 seeds, 1500 iterations; Python's own objective; lower is better):

  | Fixture | Parts | Python start | Python best | Python time (s) | C start | C best | C time (s) |
  |---|---|---|---|---|---|---|---|
  | divider | 3 | 12.744 | 12.744 | 0.34 | 12.744 | **11.744** | 0.29 |
  | edge_keepout | 3 | 12.744 | 12.744 | 1.20 | 12.744 | **11.744** | 0.09 |
  | high_current | 3 | 14.649 | 14.649 | 0.38 | 14.649 | **13.649** | 0.04 |
  | mixed_current | 3 | 10.984 | 10.591 | 0.31 | 10.984 | **8.872** | 0.04 |
  | multi_connector | 5 | 41.484 | 41.079 | 2.00 | 41.484 | **41.051** | 0.07 |
  | tight_clearance | 3 | 12.744 | 12.744 | 1.75 | 12.744 | **11.744** | 0.04 |
  | unroutable | 4 | 142.977 | 142.977 | 0.04 | 142.977 | 142.977 | 0.18 |

  - **Times** are wall-clock and include starting the process and writing the problem file.
  - **Python's best** is the lowest-cost of its three annealed placements. Python's anneal never produces a placement costing more than its start.
  - **Board violations:** 0 for every placement from either placer.
  - **Not yet measured:** larger boards (the ESP32 design needs KiCad 9 libraries that are not on this machine).

## 4. V2 tracks A–H

What existed before this audit, and what exists now.

| Track | Before | Now |
|---|---|---|
| **A** Gerber reader | **not implemented**. The CAM re-read checks the Gerber job file, not the images. | not started; test oracle ready: `manufacturing_audit/gerber_test` (real KiCad 9.0.6) |
| **B** Board model (`board.c`) | **not implemented in C**. Boards are read by Python (`copper.py`), and `kicad_pcb.c` only writes. | not started; next (blocks placement-from-board and routing) |
| **C** DFM rules and fab profiles | **partly**. C DFM exists (`dfm.c`, `dfm_pcb.c`) in `double` mm with profiles in `profiles/*.rules`. No nm checks and no BVH. JLCPCB values are not sourced. | not started; JLCPCB limits must be cited from the fab's published capabilities, with a date, or the profile is marked as not a capability claim |
| **D** kicad-cli runner | **partly**. Python runs `kicad-cli` (`copper.py native_gates`); C finds it (`copper_project.c`); `apps/pcba/cli_os.c` launches without a shell. There is no C runner with a timeout or a never-overwrite rule. | not started |
| **E** Findings and report | **not implemented** as one struct; findings are Python dicts | not started |
| **F** Tool server (JSON-RPC over stdio) | **not implemented** | not started; needs a C JSON reader and writer |
| **G** LLM bridge | **not implemented**. Only `copper_ai_model_probe.ps1` (an Ollama probe; never run here). | not started; the mock backend will be the default |
| **H** CUDA DFM | **not implemented**. No `nvcc` or GPU on this machine. | not started; CPU reference first, CUDA only behind `COPPER_CUDA=1` |

**Folder structure** (owner rule: do not rename or move):
- The earlier integration had moved `src/v2_src/*` into `src/`, and the CAM files into `tests/fixtures`. That broke the rule, and this branch puts them back.
- `src/v2_src/` is the V2 engine folder, built in place as the `copper_v2` library.
- The base modules shared with `hwcore` are byte-identical, which test `v2_src_shared_in_sync` checks.
- `manufacturing_audit/` holds the real KiCad 9.0.6 CAM oracle.

## 5. Build and verification on this machine

- **Toolchain:** MinGW GCC 12.1, C17, `-Wall -Wextra -Wpedantic`. Zero warnings in `hwcore`, `copper_v2`, the apps and the tests.
- **Sanitizers:**
  - `-DCOPPER_V2_SANITIZE=ON` adds UBSan in trap mode. `placement_test`, `g2_types_test` and `kicad_native_test` pass under it, as does a 4-seed, 1500-iteration `copper-core place` run.
  - **ASan: not run.** MinGW has no `libasan`.
- **Not available here:** KiCad 9 (no native gates; the 5 KiCad-dependent tests skip as UNVERIFIED), CUDA, Ollama.
