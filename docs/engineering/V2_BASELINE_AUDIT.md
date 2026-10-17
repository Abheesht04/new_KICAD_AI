# V2 baseline audit (Prompt 0)

Audit date: 2026-10-17. Commit audited: `bc2ea49` ("Save Copper Studio V2 baseline").
Scope: read-only. No production source, tests, CMake files or dependencies were changed.
The only file added by this stage is this document.

Status vocabulary used below: **PASS**, **FAIL**, **UNVERIFIED**, **NOT_IMPLEMENTED**,
**INVALID_INPUT**. "Confirmed" means reproduced by running the code in this audit;
"by inspection" means established by reading code only.

---

## 0. Headline results

| Item | Result |
|---|---|
| Clean configure + Release build (Windows, MinGW-w64 GCC 12.1, Ninja) | **PASS** (1 warning) |
| CTest, 30 registered targets | **FAIL: 28/30**. `kicad_pcb` and `fab_profile_io` fail on Windows (test portability, see I-04) |
| `tests/test_v2.py` (inside CTest `v2_integration`) | 21 pass, 1 **skipped** (`test_native_workflow`), reported as CTest "Passed" |
| `test_native_workflow` with KiCad 10.0.5 libraries | Internal build/determinism checks PASS; native gates inside it **silently skipped** (CLI is not 9.x) |
| Native ERC/DRC with the project's own `verify` gate | **UNVERIFIED**: only KiCad CLI 10.0.5 is available; `verify` refuses non-9.x |
| Native ERC/DRC/netlist on a fresh divider build, run directly with KiCad CLI 10.0.5 (informational) | 0 ERC violations, 0 DRC violations, 0 unconnected, 0 schematic-parity issues; independent pin/net parity matches the electrical IR (7/7 pins) |
| Windows/MSVC build | **UNVERIFIED**: no MSVC toolchain on this machine |
| Sanitizer build | **UNVERIFIED**: not run (MinGW lacks ASan/UBSan runtime) |
| `CHARTER.md` (referenced by the prompt pack) | **Does not exist** in the repository |

The prompt pack's "30/30" baseline is a Linux/GCC result. On this Windows host the
honest baseline is 28/30, and the two failures are test-harness portability defects,
not production defects.

### Environment

| Tool | Version | Notes |
|---|---|---|
| OS | Windows 10 Pro 10.0.19043 | |
| C compiler | GCC 12.1.0 (MSYS2 mingw64) | No MSVC/clang-cl found |
| CMake | 4.4.4 | Not installed system-wide; used from a throwaway virtualenv outside the repo |
| Ninja | 1.13.2 | Same virtualenv |
| Python | 3.12.0 | |
| KiCad CLI | 10.0.5 | Extracted copy at a local path, not installed; KiCad 9.x not present |

Build was out-of-tree (not the repository's `build/`), so the working tree stayed clean.

---

## 1. Build targets

| Target | Kind | Sources | Purpose |
|---|---|---|---|
| `hwcore` | static lib | 39 files in `src/`, `src/v2/`, plus `Extras/tokenizer.c` | All C engine code: IR, MNA, geometry/copper, DFM, legacy Design, V2 project, transformer |
| `hwc` | exe | `apps/hwc.c` | Legacy V1 CLI and REPL (`make`, `validate`, `DFM`, `physics-apply`, `catalogue`, `kicad-check`, `status`); `hwc project …` forwards to `tools/copper.py` |
| `copper-core` | exe | `apps/copper_core.c` | Machine-readable V2 backend: `analyze`, `transient`, `classify` |
| `fabcheck_demo` | exe | `apps/fabcheck_demo.c` | Developer demo of physical DFM and the legacy `kicad_pcb.c` writer |
| `<name>_test` ×26 | exe + CTest | `tests/<name>_test.c` | C unit tests (see §6) |
| `hwc_help` | CTest | `hwc --help` | Smoke |
| `hwc_cli_integration` | CTest | `tests/hwc_cli_integration.cmake` | Legacy CLI end-to-end |
| `v2_integration` | CTest (needs Python) | `tests/test_v2.py` | V2 end-to-end |
| `bridge_regression` | CTest (needs Python) | `tests/test_hwc_kicad.py` | `hwc_kicad.py` helpers |
| `copper-studio` | exe (opt-in `COPPER_BUILD_DESKTOP`) | `desktop/*.cpp`, vendored ImGui | GLFW/Vulkan desktop (not built here) |

Not wired into any target: `apps/prompt.c`, `Extras/NLP.c`, `tests/nlp_test.c`,
`tests/sexpr_test.c`, `tests/symlib_test.c` (see §4).

Options: `HWCOMPILER_BUILD_TESTS` (ON), `COPPER_SANITIZE` (OFF; non-MSVC only),
`COPPER_BUILD_DESKTOP` (OFF). C standard is C17, extensions off. Presets require Ninja.

---

## 2. Where each concern actually lives

There are **two pipelines**: the legacy V1 `Design` path (driven by `hwc`) and the
V2 `CpProject` path (driven by `tools/copper.py`). They share MNA, geometry, copper and
physical DFM, but not parsing, validation, schematic export or part resolution.

| Concern | V2 path (current product) | Legacy V1 path (`hwc`) |
|---|---|---|
| CLI / prompt | `copper-core classify` → `src/v2/transformer.c:ct_classify` (one category label, proposal only, no topology) | `apps/hwc.c:command_make` → `src/nlp_rules.c:hwc_intent_parse` (resistor and divider grammar only) |
| Project parse | `src/v2/project.c:cp_load` over `src/IR.c` + `arena.c` | `out/current_design.hwc` key=value state file in `apps/hwc.c` |
| Design validation | Inside `cp_load` (fields, IDs, pins, ranges) and `cp_analyze` (`open_node`) | `src/design_validate.c` |
| MNA stamping | `src/v2/project.c:assemble` | `src/mna_from_design.c:design_to_mna` + `src/node_map.c` |
| Solver | `src/mna.c:mna_solve` (Newton) → `src/matrix.c:matrix_solve` (dense Gaussian elimination, partial pivoting) | same |
| Transient | `src/v2/project.c:cp_transient` (Backward Euler companions in `assemble` when `dt>0`) | NOT_IMPLEMENTED |
| Tolerance corners | `src/v2/project.c:tolerance_corners` (≤8 resistors) | NOT_IMPLEMENTED |
| Catalogue | Part definitions embedded in each `.copper`; template catalogue `profiles/default.parts`; `copper.py new/catalogue/part-add` | `src/part_registry.c` + `src/kicad_generic_provider.c` |
| KiCad symbol resolution | `tools/copper.py:resolve_symbol`, `symbol_pins` (Python); dirs from `tools/hwc_kicad.py:discover_symbol_dir` | `src/kicad_schematic.c` embeds `Device:R` |
| Footprint resolution | `tools/copper.py:footprint_geometry` (Python) | none |
| Placement | Manual `(at x y rot)` per component; **no placer** | none |
| Routing | `tools/copper.py:route` (**Python**, 0.25 mm grid, 2-layer Manhattan A*, 250,000-state bound per connection) | none |
| Physical DFM | `src/dfm_pcb.c:pcb_dfm_validate` (+ `defect.c`, `copper.c`, `pcb_copper.c`, `geom.c`, `u128.c`) plus extra rules in `project.c:physical` (drill aspect, all-hole clearance, mask web, courtyard gap, height) | `hwc DFM -apply` runs Design-level `src/dfm.c` only |
| Electrical report | `project.c:cp_analyze` → JSON on stdout | `apps/hwc.c:command_physics_apply` (prose) |
| Verification | `tools/copper.py:verify` (output hashes, KiCad 9 ERC/DRC/parity, XML netlist, pin/net comparison) | `hwc kicad-check` (ERC + SVG) |
| Export | `tools/copper.py:export_project` (`.kicad_sch/.kicad_pcb/.kicad_pro/.kicad_dru`, lib tables, library snapshots, BOM, CALCULATIONS, SVG, manifest); `fabricate` → Gerber/drill via kicad-cli | `src/kicad_schematic.c` (1–2 resistors); `src/kicad_pcb.c` (used only by `fabcheck_demo`) |

Correction to the prompt pack: the A* router is Python (`tools/copper.py`), not C.
Prompt 8 work on routing therefore starts in Python unless it is ported deliberately.

---

## 3. Public structs and APIs

### MNA (`src/mna.h`)
- `MnaDeviceOps { flags, unknown_count, load, destroy, branch_current }`: the only
  device extension point. `load` adds residual F(x) and Jacobian J(x) through `MnaAssembly`.
- `MnaAssembly { x, jacobian, residual, node_count, unknown_count }`. Rows
  `0..node_count-1` are KCL rows for nodes 1..N (ground is node 0 and has no row);
  device-local unknowns follow at `node_count + unknown_offset`.
- Helpers: `mna_assembly_add_jacobian/add_residual/node_index/node_voltage`,
  `mna_device_unknown_index`.
- Built-ins: `mna_add_resistor`, `mna_add_voltage_source`, `mna_add_diode` (Shockley,
  exponent clamped to ±40). Return 1 on success, 0 on failure. V2 adds a private
  constant-current device in `project.c`.
- `MnaOptions { max_iterations, absolute/relative_residual_tolerance, pivot_tolerance,
  maximum_voltage_step_v, trace, trace_context }`. **`pivot_tolerance` is never set by
  `mna_options_default` and never read** (I-02).
- `MnaStatus`: `SUCCESS, INVALID_ARGUMENT, ALLOCATION_FAILURE, SINGULAR_MATRIX,
  NOT_CONVERGED, DEVICE_FAILURE`. `MnaResult` owns `x` (free with `mna_result_free`).
- Convergence: residual-only (`max|F| ≤ abs + rel·max(1,|F0|)`); no update-norm test;
  voltage-step limiting only when a nonlinear device is present. Residual rows mix
  amperes (KCL) and volts (source rows) in one max-norm.
- `mna_validate` re-evaluates residual/KCL independently of the solve loop.

### Component / model (legacy Design)
- `ComponentKind` (coarse enum: R, C, L, D, LED, BJT, MOSFET, crystal, IC, connector,
  source, custom); `ComponentModel { kind, union data }` with per-kind `Range` fields.
- `Component { reference[16], manufacturer_part_number[64], manufacturer, package,
  footprint, symbol, value, value_range, model, pins (vec), electrical, thermal,
  dimensions }`. Numeric 0 is used for "unknown" ratings (no distinct UNKNOWN).
- `ModelOps { name, kind, validate, derive_constraints }` in `ModelRegistry`. This is a
  validation registry keyed by `ComponentKind`, **separate from `MnaDeviceOps`**.

### Parts (legacy)
- `PartRequest { role, value Range, required_package, min_voltage/current/power, exact }`,
  `PartResult { manufacturer, mpn, package, footprint, symbol, model, limits, dims }`,
  `PartProvider { name, context, resolve }`, `PartRegistry` (first-match over providers).
- `PartIdentity { manufacturer, manufacturer_part_number }`.

### V2 project (`src/v2/project.h`)
- `CpComponent { ref, part, symbol, footprint, manufacturer, mpn, datasheet, kind (char
  'RCLDVIJU'), value, power_w, voltage_v, tolerance, is_a, ideality, height_mm, x, y,
  rotation, nodes[64], pins[64], pin_count }`. Strings borrowed from the project arena.
- `CpProject { arena, root, name, nets[256], net_count, component_count,
  components[256], board (PcbBoard), fab (FabProfile), max_aspect, mask_expansion,
  min_mask_web, courtyard_gap, max_drop, max_current_density, power/voltage_derating,
  has_geometry }`. About 300 KB, so callers heap-allocate it.
- `cp_init/cp_free/cp_load(path, err, cap)/cp_analyze(FILE*)/cp_transient(dt, steps, FILE*)`.
  Exit semantics: 0 = no errors, 2 = findings or failure, 3 = I/O error.

### Physical PCB and DFM
- `PcbBoard { spec, traces, pads, vias, placements }` (vec arrays). KiCad layer IDs
  (F.Cu=0, B.Cu=2, inner = even IDs). `PcbPad` has a circular legacy form plus V2
  rectangular/rounded fields.
- `FabProfile` (fixed numeric limits + name), `PcbDfmIssue { severity, rule, object_id,
  measured, limit, message, object_kind, related object }`, `PcbDfmReport`.
- `pcb_dfm_validate(board, profile, report)`; `defect.h`: `pcb_defect_check_board`
  (opens/shorts via visitor), `pcb_defect_audit_dfm_report`, `pcb_defect_coverage`
  (SUPPORTED/PARTIAL/UNSUPPORTED registry).
- Design-level `DfmProfile`/`DfmRegistry`/`dfm_run_all` (`src/dfm.h`) is a separate,
  logical-only engine.

### KiCad bridge
- C: `hwc_kicad_schematic_write` (1–2 `Device:R` only), `kicad_pcb_write[_file]`,
  `symlib_load_file/text` (full `.kicad_sym` reader with `extends`; **no production caller**).
- Python: `tools/hwc_kicad.py` (S-expression parser/dumper, library discovery,
  `discover_cli`, catalogue indexers, `custom-symbol`, legacy `generate`), and
  `tools/copper.py` (project build, symbol/footprint resolution, router, export, verify).

---

## 4. Connected, stubbed, duplicated, legacy-only, fixture-only

| Status | Items |
|---|---|
| **Connected (V2 product path)** | `IR.c`, `arena.c`, `v2/project.c`, `mna.c`, `matrix.c`, `pcb.c`, `pcb_copper.c`, `copper.c`, `geom.c`, `u128.c`, `nmath.c`, `dfm_pcb.c`, `defect.c`, `v2/transformer.c`, `tools/copper.py`, `tools/hwc_kicad.py` (discovery + parser), `desktop/` |
| **Connected (legacy V1 only)** | `nlp_rules.c`, `design.c`, `component.c`, `component_model.c`, `model_registry.c`, `design_validate.c`, `dfm.c`, `diagnostic.c`, `intern.c`, `net.c`, `node_map.c`, `mna_from_design.c`, `part_registry.c`, `part_provider.c`, `kicad_generic_provider.c`, `kicad_schematic.c`, `constraint.c`, `range.c`, `vec.c` |
| **Developer/demo only** | `kicad_pcb.c` (only `fabcheck_demo`), `fabcheck_demo` |
| **Library with tests but no production caller** | `symlib.c` (its test is not even registered), `fab_profile_io.c`, `e_series.c` |
| **Linked but unused** | `Extras/tokenizer.c` (compiled into `hwcore`; nothing calls it), `sexpr.c`, `xForm.c` (only orphaned `sexpr_test`) |
| **Unbuildable / stub** | `Extras/NLP.c` (ONNX runtime; not compiled), `apps/prompt.c` (includes `NLP.h`, which is not on the include path; no target), `tests/nlp_test.c` (link fails without `NLP.c`) |
| **Orphaned tests** | `sexpr_test.c` (not registered; **fails**: hard-codes `C:\Program Files\KiCad\9.0\...`, includes `xform.h` vs file `xForm.h`), `symlib_test.c` (not registered; passes), `nlp_test.c` (cannot link) |
| **Duplicated responsibility** | Schematic writers ×3: `kicad_schematic.c`, `copper.py:schematic`, `hwc_kicad.py:generate_divider`. Symbol resolvers ×3: `symlib.c`, `copper.py:resolve_symbol`, `hwc_kicad.py:symbol_definitions`. PCB writers ×2: `kicad_pcb.c`, `copper.py:export_project`. Part catalogues ×2: `PartRegistry` vs `.copper` `parts`. Model registries ×2: `ModelRegistry` (validation) vs `MnaDeviceOps` (equations). Hole clearance ×2: `dfm_pcb.c` `hole_clearance` and `project.c` `all_hole_clearance`. Language frontends ×4: `nlp_rules.c`, `v2/transformer.c`, `Extras/NLP.c`, `apps/prompt.c` |
| **Fixture-only data** | `tests/fixtures/kicad9/*` (symbol libs, uploaded DRC/netlist JSON), `examples/generated/divider/*` (checked-in generated output with library snapshots) |

---

## 5. Current limitations and false-success risks

### False-success risks (a run can report success that it hasn't established)

| ID | Risk | Evidence |
|---|---|---|
| F-01 | **An omitted source value becomes 0 V / 0 A silently.** `scalar(n,"value",0,…)` defaults to 0, and only R/C/L reject ≤0. A `V` part with no `(value …)` solves with `dc_solved:true`, 0 errors, exit 0. | Confirmed: probe project returned exit 0, all nodes 0 V |
| F-02 | **CTest `v2_integration` reports Passed while `test_native_workflow` is skipped.** A green CTest on a machine without KiCad libraries doesn't show the native path ran. | Confirmed on this host |
| F-03 | **Native gates in `test_native_workflow` are skipped silently** (plain `if … startswith('9.')`, not `skipTest`) when the CLI is not 9.x. The test passes with KiCad 10. | Confirmed with KiCad 10.0.5 |
| F-04 | **`verify` passes with unknown ratings.** Unknown power/voltage ratings and heights are warnings, so the manifest status is `INTERNAL_CHECKS_PASS_…` and `verify` can return `pass:true`. Only `fabricate` blocks on warnings. Under the pack's vocabulary these are UNVERIFIED, not PASS. | By inspection (`copper.py:export_project`, `verify`); divider example has 7 such warnings |
| F-05 | **`verify` doesn't detect stale source.** `manifest.source_sha256` is recorded but never compared with the current `.copper`; internal analysis isn't re-run. Editing the source after `build` leaves `verify` green. | By inspection (`copper.py:verify`) |
| F-06 | Report JSON replaces non-finite `measured`/`limit` with `0` (`project.c:issue`), which can disguise a NaN. | By inspection |
| F-07 | Diode exponent is clamped at ±40: above about 1.03·n V the current saturates while the conductance doesn't drop to zero, so residual and Jacobian disagree, without any diagnostic. | By inspection (`mna.c:diode_evaluate`) |
| F-08 | `copper-core transient` streams CSV rows before a later step fails, so a direct caller receives partial data plus exit 2. `copper.py` discards it correctly. | By inspection |
| F-09 | Legacy `hwc DFM -apply` "PASS" is Design-level only; documented, but the word PASS is shared with physical DFM. | Documented in `docs/CLI_QUICKSTART.md` |

### Fail-closed behaviour verified as correct
Unsupported `U` model blocks the solve. `--no-route` build produces `DRAFT_FAILED_CHECKS`,
5 errors, exit 2. Unknown/duplicate fields are rejected. The legacy `design_to_mna`
rejects unsupported kinds (`mna_unsupported_model`). Voltage targets and power derating
are gates. Footprint features outside supported geometry are rejected rather than
approximated.

### Functional limitations (documented, confirmed in code)
DC: R, V, I, Shockley D, C open, L short, J ideal boundary. Transient: Backward Euler only,
zero initial state, constant sources, fixed step. AC: NOT_IMPLEMENTED. BJT/MOSFET/IC
models: NOT_IMPLEMENTED. Placement optimizer: NOT_IMPLEMENTED (manual `at`). Routing:
Python A*, no rip-up/reroute. Two-layer rectangular boards only. Single-unit symbols
only (multi-unit rejected). 90° rotations only. No zones, arcs, slots, no-connect pins.
Ground must be the first net, named `GND`. C/L tolerances are not swept.

---

## 6. Tests and what they actually prove

| Test | Proves | Does not prove |
|---|---|---|
| `u128`, `v2_geometry` | Portable 64×64→128 multiply/compare; with GCC, 50,000 products vs native `__int128`; rounded-pad boundary contact | On MSVC the oracle falls back to commutativity, which uses the implementation as its own oracle |
| `geom` | Exact integer clearance predicates vs a long-double oracle, boundary cases, conversions | |
| `copper`, `defect`, `pcb_dfm` | Layer masks, trace/pad/via clearance, no-net shorts, open/short detection, invalid geometry reporting, report self-audit, coverage registry, one clean and one bad board | Real KiCad-derived boards (covered only by the native workflow) |
| `matrix` | Dense solve on a stamped system, singular detection | Scaling, near-singular or ill-conditioned behaviour |
| `mna` | 22 scenarios: chains, divider, diode, two sources, extra unknowns, external current/capacitor devices, floating node, option validation, non-finite residual/Jacobian, trace semantics, Wheatstone diode bridge | Series-diode node at the default pivot threshold (I-01); Jacobian vs finite-difference consistency |
| `mna_from_design`, `node_map`, `design`, `design_validate`, `model`, `component_model`, `intern`, `vec`, `e_series`, `provider`, `part_registry` | Legacy `Design` data structures, divider solve 3.3333 V, structural diagnostics, provider fallback/exact match, E-series ppm | Anything on the V2 path |
| `ir`, `ir_nm` | S-expression parse/serialize/errors, exact decimal-mm↔nm, KiCad 9 symbol fixtures parse | KiCad semantic validity |
| `kicad_pcb`, `fab_profile_io` | Writer emits/round-trips (on Linux) | **FAIL on Windows**: `tmpfile()` cannot create its file (I-04) |
| `schematic_export`, `nlp_rules`, `hwc_help`, `hwc_cli_integration` | Legacy resistor/divider grammar, schematic file creation, validate/DFM/physics commands, rejection of an unconnected resistor | KiCad acceptance of the schematic |
| `v2_integration` (`test_v2.py`, 22 cases) | Divider closed form (9 d.p.), KCL < 1e-10 A, current-source sign, RC/RL Backward Euler recurrence (8 d.p.) and analytic bound, diode polarity + KCL, tolerance corners, power/target gates, rejection of NaN/negative/duplicate/unknown inputs, floating reference, path traversal, symbol inheritance, rotation convention, transformer abstention | Native KiCad unless libraries **and** a 9.x CLI are present (F-02, F-03) |
| `bridge_regression` (`test_hwc_kicad.py`, 6 cases) | Python S-expression round trip, V1 prompt values, malformed input, divider generation from a symbol lib, footprint catalogue, reference roles | |

---

## 7. Do the CLI, Python adapter and desktop run the same pipeline?

**V2 path: yes.** `copper.cmd`, `hwc project …` and the desktop all launch
`tools/copper.py`, which calls `copper-core` for every electrical/DFM decision. The
desktop builds a correctly quoted `CreateProcess` command line (`desktop/studio.cpp:run_process`).

**But two things differ:**
1. `hwc project` uses `_spawnvp`, which does not quote, so **arguments containing spaces
   are split on Windows** (I-03). The desktop does not have this bug.
2. Interpreter selection is inconsistent: `hwc project` uses `HWC_PYTHON` (default
   `python`), the `hwc catalogue` bridge uses `HWC_PY_LAUNCHER` (default `py -3`), and the
   desktop uses `HWC_PYTHON`. `hwc` also resolves `tools/copper.py` relative to the
   **current directory**.

**Legacy `hwc make/validate/DFM/physics-apply` is a separate pipeline** (Design IR,
`nlp_rules`, `kicad_schematic.c`). It does not produce the same artifacts as V2.

---

## 8. Minimum clean build/test commands

Linux (from the Linux/GCC record in `docs/v2/VALIDATION.md`; not re-run here):
```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
# native path (needs KiCad 9 libraries + CLI):
KICAD_SYMBOL_DIR=... KICAD_FOOTPRINT_DIR=... python3 tests/test_v2.py build/copper-core -v
```

Windows, MinGW-w64 GCC + Ninja (run in this audit; 28/30):
```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Windows, MSVC (expected form; **UNVERIFIED**, no MSVC on this host):
```powershell
cmake -S . -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```
Expected MSVC differences: the same two `tmpfile()` failures, and a weaker u128 oracle.
Python-dependent tests need `python` on PATH at configure time.

---

## 9. Prioritized issue list

Severity: **P0** = false success or blocks the next stage; **P1** = wrong result or
false failure; **P2** = portability/test hygiene; **P3** = cleanup. Proposed fixes are
small and stage-scoped; none were applied.

| ID | Pri | Issue | Location | Proposed small fix |
|---|---|---|---|---|
| I-05 | P0 | Omitted V/I value silently becomes 0 (F-01) | `src/v2/project.c:cp_load` (`scalar(n,"value",0,…)`) | Require `value` for R/C/L/V/I (INVALID_INPUT when absent); keep it optional for D/J/U. Add negative test |
| I-06 | P0 | `verify` passes with UNKNOWN ratings (F-04) | `tools/copper.py:export_project`, `verify` | Give the manifest a distinct `UNVERIFIED_RATINGS` status and make `verify` report `pass:false`/UNVERIFIED while warnings with `rating_unknown`/`height_unknown` exist |
| I-07 | P0 | `verify` ignores source hash and doesn't re-run analysis (F-05) | `tools/copper.py:verify` | Require the source path (or snapshot), compare `source_sha256`, re-run `copper-core analyze` on `resolved.copper` and compare with `report.json` |
| I-08 | P0 | Silent skips look like PASS (F-02, F-03) | `tests/test_v2.py:test_native_workflow` | Use `skipTest` for a non-9.x CLI; emit a summary line; consider a separate CTest target (`v2_native`) that reports skipped via `SKIP_RETURN_CODE` |
| I-09 | P0 | KiCad version policy contradicts discovery | `tools/hwc_kicad.py:discover_cli` (prefers 10.0), `copper.py:verify` (requires 9.x) | Decide policy (accept 9.x and 10.x with recorded version, or prefer 9.0 in discovery). Owner decision required |
| I-01 | P1 | Absolute pivot threshold `1e-12` causes false SINGULAR (two series diodes + resistor fails) | `src/matrix.c:matrix_solve` | Make the threshold relative to the matrix/column scale, or pass `MnaOptions.pivot_tolerance` through; add series-diode regression test |
| I-02 | P1 | `pivot_tolerance` declared, uninitialized, unused | `src/mna.h`, `mna.c:mna_options_default` | Initialize it and use it (I-01), or remove it from the struct |
| I-03 | P1 | `_spawnvp` splits arguments with spaces on Windows | `apps/hwc.c:main` (project), `spawn_python_bridge` | Quote arguments on `_WIN32` using the same rule as `desktop/studio.cpp`, or use `CreateProcessW`; add a path-with-spaces test |
| I-10 | P1 | Diode clamp makes Jacobian inconsistent with residual (F-07) | `src/mna.c:diode_evaluate` | Linear continuation beyond the clamp (current = Is·e⁴⁰·(1+Δ), slope consistent), or report the limit; add finite-difference Jacobian test (Prompt 4A) |
| I-11 | P1 | Non-finite measurements masked as 0 (F-06) | `src/v2/project.c:issue` | Emit `null` and force severity error |
| I-04 | P2 | `tmpfile()` fails on Windows, so 2 CTest failures | `tests/kicad_pcb_test.c:69,92`, `tests/fab_profile_io_test.c:11` | Write to a named file in the CTest working directory instead |
| I-12 | P2 | Orphaned tests (`sexpr` fails, `symlib` passes, `nlp` unlinkable) | `CMakeLists.txt`, `tests/sexpr_test.c` | Register `symlib`; fix `sexpr_test` include case and fixture path, then register; leave `nlp_test` out until Prompt 6 decides the fate of `Extras/` |
| I-13 | P2 | `hwc` depends on CWD and creates `out/` on every call, including `--help` | `apps/hwc.c:main`, `spawn_python_bridge` | Resolve `tools/` from the executable/source dir (`HWC_SOURCE_DIR` is already defined for `hwcore`); create `out/` only for writing commands |
| I-14 | P2 | `copper.py` relies on script-dir import of `hwc_kicad` | `tools/copper.py:10` | Insert `Path(__file__).parent` explicitly into `sys.path`, or package `tools/` |
| I-15 | P3 | Misleading-indentation warning | `apps/hwc.c:689` | Reformat |
| I-16 | P3 | MNA status reported as a bare number in `measured` | `src/v2/project.c:cp_analyze` | Add a `status` string field |
| I-17 | P3 | Dead/duplicate code: `Extras/tokenizer.c` linked unused; `apps/prompt.c` unbuildable; 3 symbol resolvers, 3 schematic writers | §4 | No deletion now. Record ownership; retire after Prompt 1 (CLI) and Prompt 6 (NLP) reference analysis |
| I-18 | P3 | `CHARTER.md` missing | repo root | Owner to supply, or confirm this pack replaces it |

### Recommended order before Prompt 1
1. I-05, I-06, I-07, I-08: close the false-success paths, since Prompt 1 requires that
   "failed verification cannot return success".
2. I-09: the owner decides the KiCad version policy. Without it no native gate can run on
   this machine.
3. I-03, I-04, I-13: Windows correctness, which Prompt 1 tests require (paths with spaces).
4. I-01, I-02: solver false failure; small and independent.

---

## 10. Resolution log (Charter §14 correctness gate, 2026-10-18)

| ID | Status | Fix | Regression tests (`tests/test_v2.py`) |
|---|---|---|---|
| I-05 | Fixed | `cp_load` rejects an R/C/L/V/I component without `(value …)`; D/J/U stay optional; an explicit `0` is still accepted | `test_missing_required_value_rejected`, `test_optional_value_kinds_and_explicit_zero` |
| I-06 | Fixed | Build status `INTERNAL_CHECKS_UNVERIFIED_MISSING_DATA` plus `unverified_rules` when the report has any warning; `verify` reports `build_status`/`data_completeness` as UNVERIFIED and cannot PASS | `test_unknown_ratings_never_verify_as_pass`, `test_complete_data_leaves_only_native_unverified` |
| I-07 | Fixed | Manifest records `source_path` (relative to output); `verify` checks source hash (`--project` override), re-runs `copper-core` and requires an exact reproduction of `report.json`, deletes the old `verification.json` first, and writes per-check PASS/FAIL/UNVERIFIED. A missing or non-9.x KiCad CLI is an UNVERIFIED gate, not an exception | `test_verify_rejects_stale_source`, `test_verify_explicit_project_and_missing_source`, `test_verify_reruns_internal_analysis`, `test_verify_hash_mismatch_writes_fail_marker`, `test_draft_build_never_verifies` |
| I-08 | Fixed | Library/native cases moved to class `V2Native` and CTest target `v2_native`; the script exits 77 on any skip (`SKIP_RETURN_CODE 77`); the silent `if 9.x` became `skipTest` naming the version found; native gate test now expects UNVERIFIED for unrated parts and PASS for rated ones | `test_skipped_suite_is_not_reported_as_pass` |

New issues found during the fixes (not fixed; outside §14):

| ID | Pri | Issue | Proposed fix |
|---|---|---|---|
| I-19 | P2 | With `core.autocrlf=true` and no `.gitattributes`, Windows checkouts convert `examples/generated/divider/*` to CRLF, so its manifest hashes never match and `verify` reports `output_hashes` FAIL (before this change it raised an exception instead). Running `verify` on the example also overwrites the tracked `verification.json` | Add `.gitattributes` marking generated example artifacts `-text` (or `eol=lf`); regenerate the example with KiCad 9.x so its manifest carries `source_path` and the new status |
| I-20 | — | Charter §3 refers to a "C11/C23 decision"; `CMakeLists.txt` and the prompt pack use C17 | Owner to confirm. The standard was not changed |


---

## Appendix: commands run

```text
cmake -S . -B <scratch>/build-audit -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc   -> 0
cmake --build <scratch>/build-audit --parallel                                                  -> 0 (1 warning)
ctest -j4 --output-on-failure                                                                   -> 28/30
python tests/test_v2.py <core> -v                                                               -> 21 ok, 1 skipped
python tests/test_hwc_kicad.py -v                                                               -> 6 ok
KICAD_SYMBOL_DIR/KICAD_FOOTPRINT_DIR=<KiCad 10.0.5> test_v2.py V2.test_native_workflow          -> ok (native gates skipped)
python tools/copper.py build examples/v2/divider.copper --out <scratch>/native/divider          -> 0; 0 errors, 7 warnings
python tools/copper.py verify --out ... --kicad-cli <10.0.5>                                    -> refused: "targets KiCad 9.x"
kicad-cli 10.0.5 sch erc / pcb drc --schematic-parity / sch export netlist                      -> 0 / 0 / 0 violations; parity 7/7
copper-core analyze <two series diodes + 1k>                                                    -> mna_failure, status 3 (SINGULAR)
copper-core analyze <V source without value>                                                    -> dc_solved true, exit 0
hwc project check "D:\New folder (5)\examples\v2\divider.copper"                                -> argparse: unrecognized "folder (5)\..."
python tools/copper.py build ... --no-route                                                     -> DRAFT_FAILED_CHECKS, exit 2
```
