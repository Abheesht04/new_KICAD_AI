# Release readiness review

Review of branch `v2/correctness-and-cli` after Prompts 0–10, performed 2026-10-23. No features were added
in this review. **Verdict: not ready for a release tag or for any "verified" claim; ready for owner review
as an engineering preview.** The internal pipeline is tested end to end, but native KiCad 9.x validation,
Windows/MSVC CI and real part ratings are absent, so every design result is UNVERIFIED at best.

Nothing here claims state of the art, full verification or fabrication readiness. Fabrication export is not
fabricator acceptance. A clean KiCad DRC does not prove the electrical simulation; a passing electrical solve
does not prove native KiCad ERC/DRC. Each is reported separately throughout the tools.

## Environment

| Item | Value |
|---|---|
| OS | Windows 10 Pro 10.0.19043 |
| Compiler | MinGW-w64 GCC 12.1.0 (MSYS2), C17; CMake + Ninja |
| Python | 3.12.0 (numpy 2.2.3 present, optional) |
| KiCad | **10.0.5 only** (`D:\Practice\tools\kicad-extract`); KiCad 9.x not installed |
| MSVC | not installed (Visual Studio Installer only) |
| ngspice | not installed |
| Source path | `D:\New folder (5)` — contains spaces and parentheses; every build and test ran from it |

## Build and test results

| Run | Result | Evidence |
|---|---|---|
| Clean Release build | 38 s, **0 warnings** (`-Wall -Wextra -Wpedantic` on hwcore, pcba and cli_support; default warnings elsewhere), 0 errors | fresh out-of-tree build directory |
| CTest, Release | **46/46**: 42 passed, 4 skipped (below), 0 failed; 217 s | `ctest --output-on-failure` |
| UBSan (trap mode: `-fsanitize=undefined -fsanitize-undefined-trap-on-error -fno-sanitize-recover=all`, Debug) | **46/46**: 42 passed, 4 skipped, 0 traps | separate build tree, all 127 compile commands instrumented |
| AddressSanitizer | **not run**: MinGW-w64 GCC ships no ASan runtime | — |
| Windows/MSVC CI | **not run**: the repository has no CI configuration and no MSVC is installed | — |
| KiCad 9.x native ERC/DRC/parity | **not run** (UNVERIFIED): only KiCad 10.0.5 is installed; the gates refuse non-9.x by design | `v2_native`, `parts_native`, `layout_native`, `design_acceptance` skip |
| Paths with spaces/metacharacters | pass | repository path itself; `test_pcba_cli.Paths` (spaces, quotes, `%VAR%`, `&`, `;`, `$(...)`) |

**Skips (all four are native suites, exit 77 = UNVERIFIED, never reported as passed):**

| Test | Reason |
|---|---|
| `v2_native` | needs `KICAD_SYMBOL_DIR`/`KICAD_FOOTPRINT_DIR` (KiCad 9.x libraries) and KiCad 9.x CLI |
| `parts_native` | same; ESP32 module + MOSFET variant through KiCad 9.x ERC/DRC/parity |
| `layout_native` | RC fixture needs `Capacitor_SMD` from installed libraries |
| `design_acceptance` | ESP32 LED acceptance run with KiCad 9.x libraries and CLI |

Also within the passing suites: the two `numeric_oracles` expected failures (Newton above ~24 V; trapezoidal
ringing) pin known limitations rather than hide them.

## Repository hygiene

| Check | Result |
|---|---|
| Credentials / keys / tokens | none found (matches were Dear ImGui password-field code and a path-traversal test string) |
| Build outputs, caches, `out/`, binaries, `__pycache__`, temporary files tracked | none (`build/`, `build-*/`, `out/`, `.tmpbuild/`, `__pycache__/`, `*.exe`… are ignored) |
| Absolute local paths in tracked files | **one finding**: `tests/fixtures/kicad9/golden_netlist.kicad_net` / `.xml` embed the original machine's KiCad export metadata (`D:\sample_resistor_lol\...`, `C:\Program Files\KiCad\9.0\...`). Harmless fixture metadata; left unchanged pending owner decision. Other matches are documented KiCad install defaults and path-quoting test strings |
| Large files | two committed KiCad library snapshots for offline tests (`Connector_Generic.kicad_sym` 6.8 MB, `Device.kicad_sym` 2.3 MB), with licence notice |
| Untracked, not committed | `CLAUDE_CODE_V2_EVOLUTION_PROMPTS.pdf`, `noble-tourism-redesign/` (unrelated) |
| Unregistered C tests | `tests/nlp_test.c` (retired NLP; does not link), `tests/sexpr_test.c` (hard-codes a KiCad path; fails), `tests/symlib_test.c` (passes, not registered) — from the Prompt 0 audit, still open |

## Capability matrix

Legend: **Tested** — implemented and covered by passing tests against independent oracles;
**Not native-validated** — implemented and internally tested, but the KiCad 9.x gate has not run here;
**Unverified** — exists but has no passing evidence in this review; **Not implemented**.

| Stage | Capability | Status | Evidence | Limitations |
|---|---|---|---|---|
| 0 | Baseline audit | Done | `docs/engineering/V2_BASELINE_AUDIT.md` | — |
| Charter | I-04 (Windows `tmpfile`), I-05…I-08 correctness fixes | Tested | regression tests in `test_v2.py`, C tests | — |
| 1 | `pcba` single CLI: command tree, help, JSON envelope, exit codes, no-shell launching | Tested | `pcba_cli` (26 tests) | legacy `hwc` launches bare `python` (old interpreter can win on PATH) |
| 2 | Versioned device-model and part contracts; unknown ≠ zero ratings; catalogue overlay | Tested | `device_model`, `test_v2 ModelContract`, `parts_registry` | — |
| 3 | AC small-signal: complex MNA, phasors, DC-point linearisation | Tested | `mna_ac`, `cplx`, `test_v2 AcAnalysis`, `numeric_oracles` (6e-13 vs closed form) | no device capacitances: transistor/diode AC is conductance only |
| 4 | Shockley diode, Ebers–Moll BJT, Level-1 MOSFET; consistent Jacobians | Tested | `nonlinear_models` (finite-difference Jacobians), `test_v2 Transistors` | **Newton fails above ~24 V supply** (roadmap N1); no temperature, breakdown or charge |
| 5 | Transient: transactional steps, Backward Euler, trapezoidal | Tested | `transient`, `test_v2 TransientEngine`, `numeric_oracles` (orders 1 and ~2) | fixed step, no LTE control; trapezoidal rings on stiff nodes; BE damps LC |
| 6 | EngineeringSpec IR, `.espec`, fingerprint, constrained-English frontend, lowering | Tested | `engineering_spec` (25 tests + corpus) | constrained grammar only; plurals/counts unsupported |
| 7 | Part registry, `parts import` parity, MCU behavioral boundary, footprint keepouts | Tested internally; **not native-validated** | `parts_registry` (21 tests); `parts_native` skipped | ESP32 datasheet values transcribed, need owner confirmation; KiCad 10 stacked pins UNVERIFIED; rectangular keepouts only |
| 8 | Instrumented router, rip-up/reroute, staged placement search, exact re-check | Tested internally; **not native-validated** | `layout_optimizer` (15 tests), `docs/engineering/LAYOUT_BENCHMARK.md` | 1 mm grid placement; uniform trace width; annealing has not beaten the constructive placement |
| 9 | `pcba design` request → verified KiCad project, 11 gates | Tested internally; **not native-validated** | `design_compiler` (10 tests); `design_acceptance` skipped | one MCU + GPIO indicator LEDs only; generic parts → UNVERIFIED |
| 10 | Numerical baseline and roadmap | Measured | `numeric_oracles`, `numeric_bench`, `docs/engineering/NUMERICAL_ROADMAP.md` | proposals N1/S1/S2/T1/T2 awaiting approval |
| — | KiCad export (`.kicad_sch/.kicad_pcb/.kicad_pro`, BOM, manifest, library snapshots) | Tested internally; **not native-validated** | `v2_integration`, `pcba_cli`; `v2_native` skipped | KiCad 9.x ERC/DRC/parity not run in this review |
| — | Verification: hashes, source freshness, re-analysis; rejection of modified/stale artifacts | Tested | `pcba_cli`, `design_compiler` gate 10 | — |
| — | Gerber/drill fabrication export | **Unverified** | requires a PASSing KiCad 9.x verification, which cannot run here | not fabricator acceptance |
| — | Copper Studio desktop (Vulkan) | **Unverified** | not built in this review (`COPPER_BUILD_DESKTOP=OFF`) | — |
| — | Sparse solver, adaptive timestep, BDF2, device capacitances, Monte Carlo, ngspice differential checks | **Not implemented** | roadmap only | — |

Development-only evidence (not counted above): with a local copy of the KiCad 10.0.5 libraries whose one
stacked pin was split, KiCad 10.0.5 ERC/DRC/schematic parity and the independent netlist comparison were clean
for the ESP32 example, the optimized layout fixtures and the ESP32 LED acceptance board. KiCad 10 is not the
validation target, so these remain UNVERIFIED.

## Performance and statistics (Release build)

**Electrical analysis of every committed example** (`copper-core analyze`, includes process start):

| Project | Time | DC | Max residual | Errors | Warnings | Electrical |
|---|---|---|---|---|---|---|
| `divider.copper` | 16 ms | solved | 5.4e-20 | 0 | 5 | UNVERIFIED (unknown ratings) |
| `diode.copper` | 17 ms | solved | 8.7e-19 | 0 | 4 | UNVERIFIED |
| `rc.copper` / `rc_ac.copper` / `rl.copper` | 17–21 ms | solved | 0 | 0 | 4 | UNVERIFIED |
| `esp32_io.copper` | 20 ms | solved | 0 | 0 | 11 | UNVERIFIED (MCU boundary, unknown ratings) |

**Build and repeatability** (`divider.copper`, committed KiCad 9 snapshot): 5.3 s, 7 pads, 22 tracks, 0 vias,
0 internal errors, 7 warnings, status `INTERNAL_CHECKS_UNVERIFIED_MISSING_DATA`; a second build produced the
same SHA-256 for all 15 manifest files.

**Layout optimizer** (one seed, 300 iterations, exact re-check):

| Fixture | Time | Feasible | Hard findings | Unrouted | Length | Vias | Width | Explored states |
|---|---|---|---|---|---|---|---|---|
| divider | 0.75 s | yes | — | 0 | 13.7 mm | 0 | 0.20 mm | 323 |
| multi_connector | 6.1 s | yes | — | 0 | 47.3 mm | 0 | 0.20 mm | 1 361 |
| edge_keepout | 0.47 s | yes | — | 0 | 13.7 mm | 0 | 0.20 mm | 323 |
| high_current | 0.78 s | yes | — | 0 | 20.4 mm | 0 | 0.75 mm | 549 |
| tight_clearance | 0.78 s | **no** | copper_clearance 4 | 0 | 13.7 mm | 0 | 0.20 mm | 220 |
| unroutable | 12.5 s | **no** | open_connection 7 | 5 | 9.9 mm | 0 | 0.20 mm | 49 371 |

**ESP32 LED acceptance** (`pcba design`, development libraries): 64 s end to end including a second full run
for the determinism gate; gates spec, topology, parts, models, routing, manifest, determinism PASS;
electrical, internal DFM, KiCad ERC/DRC and connectivity UNVERIFIED (unknown ratings and heights; no KiCad
9.x). Chosen layout: 252.6 mm, 14 vias, 0.25 mm traces; LED current 4.59 mA against a 5 mA target.

**Numerics** (`numeric_bench`): dense ladder solve 5 µs (10 nodes) to 3 ms (200 nodes), errors ≤ 2.6e-14 V;
diode Newton converges to round-off up to 24 V supply (98 iterations) and fails at 25 V (roadmap N1).

## Owner acceptance checklist

| Item | Status | Evidence / gap |
|---|---|---|
| One primary CLI lists its commands, consistent exit codes and logging | **Met** | `pcba help`, `pcba_cli` |
| Every frontend emits the same normalized, versioned Spec IR | **Met** for both frontends (`.espec`, constrained English) | `engineering_spec` fingerprint equality |
| Design records every component, terminal, net, source and required constraint | **Met** | `design_compiler`, `design.espec` with explicit no-connects |
| No unsupported model is omitted from analysis | **Met** | `unsupported_model` findings; MCU boundary named as constraint-only |
| AC uses a genuine complex system and reports magnitude/phase | **Met** | `mna_ac`, `AcAnalysis` |
| Nonlinear models have tested equations and Jacobians | **Met, with a limitation** | finite-difference tests; convergence fails above ~24 V |
| Transient commits state only after successful steps | **Met** | `TransientEngine` rollback tests |
| Real KiCad symbol/footprint IDs and pads mapped correctly | **Met internally; not native-validated** | `parts import` parity; KiCad 9.x gates not run |
| Catalogues and DFM profiles override defaults only within scope, with provenance | **Met** | catalogue overlay tests, provenance-required import |
| Placement/routing never trades away hard DFM constraints | **Met** | lexicographic selection tests, exact re-check |
| Internal geometry checks and native ERC/DRC reported separately | **Met** | separate scopes and gates everywhere |
| "ESP32 LED PCB" resolves ambiguity, real module, correct pins, checked board or precise blocked report | **Met internally**; fully checked board **not yet shown** (needs KiCad 9.x and rated parts) | `design_compiler`; `design_acceptance` skipped |
| Reference suite, sanitizer build and supported Windows CI green | **Partly**: suite and UBSan green; no ASan; **no CI and no MSVC run** | this report |
| Documentation lists verified, unverified and not-implemented honestly | **Met** | this report, stage docs |

## Open issues before a release tag

1. **KiCad 9.x native validation** has not run in this review: run the four native suites on a machine with
   KiCad 9.x (commands below).
2. **Windows/MSVC CI**: none exists; MSVC builds are untested here. Adding CI is outside this review.
3. **Newton convergence above ~24 V** (roadmap N1) — awaiting approval.
4. **Transcribed datasheet data** to confirm: ESP32-WROOM-32E/32UE limits, strapping notes and reference
   circuit; the indicator LED's stated single-point model; the 0.2 mm drill in `esp32_2layer.rules`.
5. **No rated parts**: generic parts keep every electrical result UNVERIFIED; a rated user catalogue is needed
   to reach PASS.
6. Housekeeping for owner decision: unregistered `nlp/sexpr/symlib` C tests; the absolute paths in the KiCad
   golden-netlist fixture; legacy `hwc` interpreter selection.

## Reproducing on a KiCad 9.x machine

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
$env:KICAD_SYMBOL_DIR="C:\Program Files\KiCad\9.0\share\kicad\symbols"
$env:KICAD_FOOTPRINT_DIR="C:\Program Files\KiCad\9.0\share\kicad\footprints"
$env:KICAD_CLI="C:\Program Files\KiCad\9.0\bin\kicad-cli.exe"
ctest --test-dir build --output-on-failure
.\build\pcba design "Build me an ESP32 LED PCB. Generate the KiCad PCB." --out out\esp32_led --profile profiles\esp32_led.design
python tools\layout_bench.py --symbol-dir $env:KICAD_SYMBOL_DIR --footprint-dir $env:KICAD_FOOTPRINT_DIR --kicad-cli $env:KICAD_CLI
python tools\numeric_bench.py --core build\copper-core.exe --out out\numeric.json
```

This review stops here for owner review. No release tag has been created and no broad cleanup was done.
