# Changelog

## 2026-11-15 — Solver bug regressions, Arena workspace reuse and expander readability

- **Bug IDs:** `B04`, `B05`, `B06`, `B07`, `B08`, `B10`, `B12`, `B13`, `FP-01`, `SPICE-ROOT-01`; `B01-B03`, `B09`, `B11`, `B14` retained as unresolved/mapping-required registry entries.
- **Files changed:** `v2_src/mna.c`, `v2_src/mna.h`, `v2_src/copper_linalg.c`, `v2_src/copper_linalg.h`, `v2_src/copper_spice.c`, `v2_src/copper_spice_expand.c`, `v2_src/copper_spice_expand.h`, `v2_src/copper_spice_fs.c`, `v2_src/copper_spice_fs.h`, `v2_src/README.md`, `v2_src/CHANGELOG.md`, `cmake/CopperFpContract.cmake`, `tests/copper_spice_step23_test.c`, `tests/mna_bug_regressions_test.c`, `tests/cmake_fp_smoke/CMakeLists.txt`, `tests/cmake_fp_smoke/fp_smoke.c`.
- **B04 — `mna.c`:** Preserve signed polarity-normalised MOS `VTO` instead of applying `fabs`; add depletion NMOS and PMOS regressions in `mna_bug_regressions_test.c`.
- **B05 — `copper_spice.c`:** Keep the parser/model default at 27.0 °C and verify the resulting 300.15 K thermal voltage and device operating temperature in `test_default_temperature_is_300_15k`; no model-temperature formula was changed in this hunk.
- **B06 — `mna.c`:** Replace the unbounded-result diode series-resistance inner Newton update with a bracketed Newton method, bisection fallback, finite checks and explicit failure. Add normal series-resistance bias, fail-closed overflow and outer Newton-iteration-exhaustion regressions.
- **B07 — `copper_spice.c`:** Add a regression for the geometric AC sweep ratio and exact final frequency; the already-correct constant-ratio expression is retained, not rewritten by this patch.
- **B08 — `mna.c`:** Replace generic pair-step limiting with PN junction limiting for diode/BJT junctions and polarity-normalised FET limiting for MOS/JFET gate-source updates. Add direct limiter and signed-threshold tests.
- **B10 — `copper_spice.c`, `copper_spice_expand.c`:** Guard suffix lookahead at the end of numeric strings and test `5V`, `1megOhm`, `1mOhm`, `1mil` and `10kOhm`; preserve `meg` versus milli semantics.
- **B12 — `copper_linalg.c`:** Remove active AVX2/FMA intrinsics and the stale disabled SIMD fallback block; use one scalar LU path. This makes the source implementation uniform but does not promise cross-compiler bit identity.
- **B13 — `mna.c`, `copper_linalg.c/.h`:** Replace direct MNA heap allocations with Arena-backed scratch arrays; initialise reusable real/complex LU workspaces outside repeated Newton solve loops, and reset transient scratch between steps. Add a test that solves multiple right-hand sides through one workspace and a source scan for direct MNA heap calls.
- **SPICE-ROOT-01 — `copper_spice_fs.c/.h`:** Add `csp_parse_file_in_root` and make `csp_parse_file` default to the top-level deck directory; lexically normalise `.`/`..` and reject a resolved include outside the allowed root. Symlink escape protection is not implemented and is explicitly documented.
- **FP-01 — `cmake/CopperFpContract.cmake`:** Add opt-in strict floating-point flags (`-fno-fast-math -ffp-contract=off`, `/fp:strict`) for an existing target. The root target graph is intentionally not edited.
- **Regression tests — `tests/copper_spice_step23_test.c`:** Add AC decade ratio, default 300.15 K, suffix, case-insensitive node identity, `gnd` versus `0`, late subcircuit definition, dangerous expression and allowed-root traversal tests.
- **Regression tests — `tests/mna_bug_regressions_test.c`:** Add polarity, PN/FET limiting, reusable real/complex workspace, Newton iteration exhaustion, diode series-resistance and fail-closed overflow tests.
- **Build-policy smoke test — `tests/cmake_fp_smoke/*`:** Add a tiny CMake target that includes the FP helper; its verbose GCC compile command was checked for both required flags.
- **Documentation hunk — `README.md`:** Reformat and update module map, callback ownership, exact syntax scope, conventions, Windows/GCC command examples, B01–B14 bug registry, latest actual test results and extension checklists. This hunk documents the code changes; it is not a solver behaviour change.
- **Not associated with a B-number:** no original descriptions/reproducers were supplied for `B01-B03`, `B09`, `B11` or `B14`; these IDs are retained and marked mapping-required rather than silently dropped or assigned invented defects.
- **Not associated with a B-number:** `CHANGELOG.md` records this patch. No other code hunks are intentionally included.

## 2026-11-15 — Step 2/3 SPICE hierarchy and device-model expansion

- **Bug IDs:** `SPICE-001`, `SPICE-002`, `SPICE-003`, `SPICE-004`, `DEV-001`, `DEV-002`, `DEV-003`, `DEV-004`, `SOL-004`, `SPICE-005`, `SPICE-006`.
- **Files changed:** `v2_src/copper_spice.h`, `v2_src/copper_spice.c`, `v2_src/copper_spice_expand.h`, `v2_src/copper_spice_expand.c`, `v2_src/copper_spice_fs.h`, `v2_src/copper_spice_fs.c`, `v2_src/circuit.h`, `v2_src/circuit.c`, `v2_src/mna.h`, `v2_src/mna.c`, `tests/copper_spice_contract_test.c`, `tests/copper_spice_step23_test.c`, `tests/ngspice_fixture_execution_test.c`, `tests/fixtures/include_main.cir`, `tests/fixtures/step23_models.lib`, `tests/ngspice/README.md`, `tests/ngspice/decks/*.cir`, `tests/ngspice/decks/*.tolerance.json`, `v2_src/README.md`, `v2_src/CHANGELOG.md`.
- Added bounded `.INCLUDE`, `.SUBCKT/.ENDS/X` and parameter-expression expansion; include callback and nesting limits are documented in the README.
- Added diode breakdown/capacitance/temperature model fields, BJT junction capacitances and MOS small-signal capacitance approximations; full transient charge and full vendor model families remain out of scope.
- Added `.IC`, `.NODESET`, `.TEMP`, `TNOM`, AC/DC/transient analysis parsing and filesystem include adapter.
- Added a Copper-side execution test and candidate ngspice decks; external ngspice differential remains NOT RUN where ngspice is not installed.
