# Copper solver source notes

## 1. Purpose and scope

Copper parses a deliberately bounded SPICE subset into a typed C circuit IR and solves DC operating point, small-signal AC, transient and single-source DC sweep analyses.
The SPICE front end supports nested subcircuits, include expansion, arithmetic parameters and selected diode/BJT/MOS/JFET model features.
The solver is a C17 dense-MNA implementation with explicit failure for unsupported syntax; it is not a full ngspice replacement.

**Not implemented:** full ngspice compatibility, behavioral sources, full vendor-model language, sparse MNA, adaptive timestep control, transient nonlinear charge integration, full Gummel–Poon/BSIM models, KiCad refdes/pin/pad mapping, part-rating checks, and an executed ngspice differential suite.

## 2. Module map

| File | One-line role | Public functions |
|---|---|---|
| `arena.h/.c` | Chunked scratch/result allocator used by parser, circuit and solver. | `arena_init`, `arena_reset`, `arena_destroy`, `arena_alloc`, `arena_alloc_array` |
| `nmath.h/.c` | Copper numeric primitives and finite checks. | Functions declared in `nmath.h`, including `nm_sqrt`, `nm_rsqrt`, integer magnitude helpers and `nm_isfinite` |
| `cplx.h/.c` | Scalar double-precision complex arithmetic and phasor helpers. | `cplx`, `cplx_add`, `cplx_sub`, `cplx_mul`, `cplx_scale`, `cplx_div`, `cplx_abs`, `cplx_arg_deg`, `cplx_polar_deg`, `cplx_isfinite` |
| `circuit.h/.c` | Typed circuit IR, element validation, source waveforms and trace callback dispatch. | `circuit_init`, `circuit_add`, `circuit_validate`, `circuit_status_name`, `circuit_element_kind_name`, `circuit_source_value`, `circuit_source_dc_value`, `copper_trace_set_callback`, `copper_trace_emit` |
| `copper_linalg.h/.c` | Dense real/complex scaled-pivot LU with iterative refinement and reusable scratch workspaces. | `cla_default_options`, `cla_workspace_init_real`, `cla_workspace_init_complex`, `cla_solve_real_with_workspace`, `cla_solve_complex_with_workspace`, `cla_solve_real`, `cla_solve_complex`, `cla_status_name` |
| `mna.h/.c` | MNA stamping, nonlinear device evaluation/limiting, operating point, AC and transient solves. | `mna_default_options`, `mna_default_transient_options`, `mna_solve_dc`, `mna_solve_ac`, `mna_solve_transient`, `mna_status_name`; `mna_test_*` functions only with `COPPER_TESTING` |
| `copper_spice.h/.c` | SPICE tokenizer/parser, model validation, Circuit conversion and analysis runner. | `csp_parse`, `csp_parse_with_includes`, `csp_build_circuit`, `csp_run_analysis`, `csp_status_name` |
| `copper_spice_expand.h/.c` | Include expansion, bounded parameter evaluation and hierarchical subcircuit flattening. | `csp_expand_deck` |
| `copper_spice_fs.h/.c` | Adapter that reads a top-level deck and includes through Copper's filesystem layer, with allowed-root checking. | `csp_parse_file`, `csp_parse_file_in_root` |
| `filesystem.h/.c` and `sha256.h/.c` | UTF-8 filesystem boundary, lexical/physical path handling, atomic writes, deterministic directory listing, Arena reads and SHA-256 file/data hashing. | `fs_path_is_absolute`, `fs_path_join`, `fs_path_absolute`, `fs_path_normalize`, `fs_path_resolve_existing`, `fs_path_is_within`, `fs_path_basename`, `fs_file_exists`, `fs_directory_exists`, `fs_is_executable`, `fs_get_current_directory`, `fs_create_directory`, `fs_create_directories`, `fs_write_file_exclusive`, `fs_write_file_atomic_exclusive`, `fs_write_file_atomic_replace`, `fs_rename_path`, `fs_rename_path_replace`, `fs_list_directory`, `fs_free_directory_listing`, `fs_read_file`, `fs_read_file_arena`, `fs_copy_file_exclusive`, `fs_remove_tree`, `fs_sha256_bytes`, `fs_sha256_hex`, `fs_sha256_file`, `fs_sha256_file_hex`, `fs_status_name` |
| `cmake/CopperFpContract.cmake` | Adds the supported strict floating-point compiler flags to an existing target. | CMake function `copper_apply_strict_fp(target)` |

`CspIncludeReadFn` receives the expansion scratch `Arena`. On success, the callback **must copy** `resolved_path` and file bytes into that arena; both remain valid until expansion ends. The expander does not individually free callback memory. The filesystem adapter now reads top-level and included files directly into the supplied Arena with `fs_read_file_arena`; it does not allocate a temporary malloc-owned file buffer.

`CspExpandOptions.max_include_depth` defaults to 16 and is capped at 31; `max_subcircuit_depth` defaults to 16 and is capped at 64. Include cycles are detected by resolved path strings. File-root checks use `fs_path_resolve_existing` and `fs_path_is_within`, resolving existing symlinks/junctions before checking containment. On POSIX this uses `realpath`; on Windows it uses a file handle plus `GetFinalPathNameByHandleW`. The check and subsequent read are separate operations, so a hostile process able to replace links concurrently can still create a time-of-check/time-of-use race. Do not treat this as a complete isolation boundary for untrusted multi-tenant uploads; a hosted service should use OS-level sandboxing or handle-relative no-follow access.

The trace callback is process-global. Register it before simulation and do not change it concurrently with solver calls. Event strings and token pointers are borrowed only for the duration of the callback.

## 3. Data flow

```text
SPICE deck / KiCad-exported netlist
    -> csp_parse_with_includes(Arena, source_name, text, size, options)
       or csp_parse_file(Arena, path)
         -> .INCLUDE expansion through callback / filesystem adapter
         -> .SUBCKT/.ENDS collection and nested X-instance flattening
         -> .PARAM evaluation and {expr} substitution
         -> csp_parse
    -> CspNetlist (elements, models, node names, ICs, nodesets, temperature, analysis)
    -> csp_build_circuit(Arena, CspNetlist)
    -> Circuit (integer node IDs, typed devices, SI-valued parameters)
    -> MNA: OP / AC / transient / DC sweep
    -> CspAnalysisResult (node voltages, element currents and numerical diagnostics)
```

`csp_parse` handles a single already-expanded deck. Use `csp_parse_with_includes` or `csp_parse_file` for `.INCLUDE`, `.SUBCKT`, `.ENDS`, `X` and `.PARAM`. `csp_run_analysis` builds a temporary `Circuit` internally; the caller owns the result arena and must keep it alive while reading result arrays. The expander reports a physical source line for its own errors, but a source map is not retained through flattening; later parser diagnostics refer to the expanded deck line. The current IR has no refdes/pin/pad/net join to a KiCad `Board`.

## 4. Conventions

- All values passed into `Circuit` are SI: V, A, ohm, F, H, s, Hz and S. Parser scale suffixes are converted to SI. `mil` means 0.001 inch (25.4 micrometres); `meg` means 1e6 and `m` means 1e-3. Alphabetic unit labels such as `V`, `A`, `Ohm` and `F` are ignored after a recognised suffix.
- Node `0` is ground. The literal name `gnd` is an ordinary node name; it is not an alias for `0`. Node lookup is case-insensitive. Internal solver unknown vectors use compact non-ground indices `0..N-1`; public result voltage arrays reserve slot 0 for ground and place node IDs 1..N at matching array indices.
- Two-terminal passive branch current is positive from `n1` to `n2`: R uses `(V(n1)-V(n2))/R`, C current is reported as zero at DC, and L branch current follows its MNA branch unknown. Independent current-source value is positive from `n1` to `n2`. Independent voltage-source branch current is positive from `n1` to `n2`.
- Dependent-source output current is positive from output `n1` to output `n2`. E and H branch-current unknowns follow that same orientation. G and F use their controlling voltage/branch current as specified by the element; F/H control references name a separate element and are not additional device terminals.
- D terminal order is `anode, cathode`; Q is `collector, base, emitter`; M is `drain, gate, source, body`; J is `drain, gate, source`. For D/Q/M/J, nonlinear terminal currents are defined positive leaving each device terminal. `element_currents_a` reports the first terminal's current. For Q, terminal currents are collector/base/emitter; for M/J, the current reported is drain current.
- E/G terminal order is `output+, output-, control+, control-`. F/H have `output+, output-` plus a controlling branch element reference.
- `.TEMP` and `.OPTIONS TNOM` are in degrees Celsius. The parser defaults both to 27.0 °C, i.e. 300.15 K. Diode/BJT thermal voltage is recalculated from the operating temperature. The implemented temperature scaling applies to saturation current through `XTI`/`EG`; other temperature coefficients are not implemented.
- Transient time, timestep and source delay parameters are seconds. AC frequency is hertz; the solver uses angular frequency `omega = 2*pi*f` radians/second. DEC/OCT sweep point counts are points per decade/octave; LIN uses its point count across the interval. SIN source phase is parsed in degrees and stored in radians.
- Transient runs are initialised from the DC operating point by default in the SPICE runner; `.IC` node values override transient starting node voltages and `.NODESET` values are initial DC guesses. Trapezoidal and Gear-2 start with a backward-Euler step to establish integration history.

## 5. SPICE subset

Statuses apply only to this Copper implementation. `UNSUPPORTED (fails closed)` means the syntax is rejected with an error instead of being silently ignored. `DIFFERS FROM NGSPICE` means a constrained implementation exists but does not implement the full ngspice model/semantic surface.

### Elements

| Element | Status | Scope / difference |
|---|---|---|
| `R`, `C`, `L` | SUPPORTED | Ideal two-terminal devices; no distributed/parasitic vendor models. |
| `V`, `I` | SUPPORTED | DC, AC magnitude/phase, PULSE, SIN and EXP forms. PWL and arbitrary behavioural waveforms are unsupported. |
| `D` | DIFFERS FROM NGSPICE | Shockley diode, series resistance, BV/IBV, CJO/VJ/M/TT and IS temperature scaling; not a complete SPICE diode charge model. |
| `Q` | DIFFERS FROM NGSPICE | NPN/PNP Ebers–Moll-like model with BF/BR/VAF and CJE/CJC/TF AC stamps; not full Gummel–Poon. |
| `M` | DIFFERS FROM NGSPICE | Level-1 long-channel model with KP/VTO/LAMBDA/GAMMA/PHI and approximate small-signal capacitances; no BSIM. Only LEVEL=1 is accepted. |
| `J` | DIFFERS FROM NGSPICE | Simplified N/P JFET model; no full vendor JFET model surface. |
| `E`, `G`, `F`, `H` | SUPPORTED | Linear VCVS, VCCS, CCCS and CCVS forms using node/branch references. |
| `X` | SUPPORTED | Nested subcircuit instances are flattened with scoped instance/element/node names and positional node mapping. Parameter expression subset only. |
| `S`, `W`, `T`, `K`, `B` | UNSUPPORTED (fails closed) | Switches, transmission lines, coupled inductors and behavioural sources are not implemented. |
| Unknown element prefix | UNSUPPORTED (fails closed) | Parser returns a line-associated error. |

### Directives

| Directive | Status | Scope / difference |
|---|---|---|
| `.OP` | SUPPORTED | DC operating point. |
| `.AC DEC/OCT/LIN` | SUPPORTED | Positive start frequency and stop >= start; DEC/OCT point counts are per decade/octave. |
| `.TRAN` | DIFFERS FROM NGSPICE | Fixed-step transient; start time and maximum step are parsed, but adaptive local-truncation control is absent. |
| `.DC` | DIFFERS FROM NGSPICE | One independent source sweep only; nested/multi-source sweeps are unsupported. |
| `.SUBCKT`, `.ENDS`, `X` | DIFFERS FROM NGSPICE | Flattened subset with positional ports, nested scopes and bounded recursion. No subcircuit-local functions/conditional syntax. |
| `.INCLUDE` | DIFFERS FROM NGSPICE | Callback/filesystem include with relative paths, cycle detection and depth bound. No symlink-resolved root sandbox. |
| `.PARAM` and `{expr}` | DIFFERS FROM NGSPICE | Arithmetic operators, parentheses, unary signs and suffixes. No functions, conditionals, string parameters or general forward-reference engine. |
| `.IC` | DIFFERS FROM NGSPICE | Node-voltage initial conditions mapped to transient start voltages; not all device/state initial-condition syntax. |
| `.NODESET` | DIFFERS FROM NGSPICE | Node-voltage initial guesses for DC solve; not a complete ngspice nodeset implementation. |
| `.TEMP` | SUPPORTED | One circuit-wide temperature in °C. |
| `.OPTIONS` | DIFFERS FROM NGSPICE | Supported keys are individually listed below. Other options fail closed. |
| `.END` | SUPPORTED | Ends parsing; trailing content is not processed. |

### Options

| Option | Status | Scope / difference |
|---|---|---|
| `METHOD=BE/TRAP/GEAR/GEAR2` | SUPPORTED | Selects backward Euler, trapezoidal or Gear-2 transient integration. |
| `TNOM` | SUPPORTED | Nominal model temperature in °C. |
| `RELTOL` | SUPPORTED | Relative solver tolerance. |
| `VNTOL` | SUPPORTED | Absolute node-voltage tolerance in V. |
| `ABSTOL` | SUPPORTED | Absolute current tolerance in A. |
| `GMIN` | DIFFERS FROM NGSPICE | Sets the configured start and end conductance to the same supplied value; full ngspice option semantics are not claimed. |
| `ITL1` | SUPPORTED | Maps to the DC/Newton iteration limit. |
| `SRCSTEPS` | SUPPORTED | Maps to independent-source stepping count. |
| `GMINSTEPS` | SUPPORTED | Maps to the geometric GMIN continuation stage count. |
| Any other option key | UNSUPPORTED (fails closed) | Rejected with a line-associated syntax/unsupported status. |
| `.FUNC`, `.STEP`, `.MEASURE`, `.SAVE`, `.PROBE`, `.GLOBAL`, `.ALTER`, `.CONTROL` | UNSUPPORTED (fails closed) | Not implemented by this parser. |

### Model parameters and suffixes

| Model | Status | Recognised parameters |
|---|---|---|
| Diode `D` | DIFFERS FROM NGSPICE | `IS`, `N`, `RS`, `BV`, `IBV`, `CJO`, `VJ`, `M`, `TT`, `XTI`, `EG`. |
| BJT `NPN/PNP` | DIFFERS FROM NGSPICE | `IS`, `BF`, `BR`, `VAF`, `CJE`, `VJE`, `MJE`, `CJC`, `VJC`, `MJC`, `TF`, `XTI`, `EG`. |
| MOS `NMOS/PMOS` | DIFFERS FROM NGSPICE | `KP`, `VTO`, `LAMBDA`, `LEVEL=1`, `GAMMA`, `PHI`, `TOX`, `CGSO`, `CGDO`, `CGBO`. Approximate AC capacitance only. |
| JFET `NJF/PJF` | DIFFERS FROM NGSPICE | `BETA`, `IDSS`, `VTO`, `LAMBDA`. |
| Unknown model parameter | UNSUPPORTED (fails closed) | `csp_build_circuit` rejects unrecognised parameters for the model family. |
| Scale suffixes | SUPPORTED | `T`, `G`, `MEG`, `K`, `M`, `U`, `N`, `P`, `F`, `MIL`; trailing alphabetic unit labels are ignored. |
| Numeric expressions | DIFFERS FROM NGSPICE | Arithmetic `+ - * /`, unary sign and parentheses are supported. Division by zero, non-finite values, recursive definitions and unsupported tokens fail closed with a line number. |

## 6. Build and test on Windows

The source patch is C17. From a **Developer PowerShell for Visual Studio** prompt, the following compiles each checked-in test twice, once with trace disabled and once enabled. These commands are provided for your Windows integration; they were **NOT RUN** in this environment.

```powershell
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force build | Out-Null
$src = @(
  'v2_src\arena.c',
  'v2_src\nmath.c',
  'v2_src\cplx.c',
  'v2_src\circuit.c',
  'v2_src\copper_linalg.c',
  'v2_src\mna.c',
  'v2_src\copper_spice.c',
  'v2_src\copper_spice_expand.c',
  'v2_src\copper_spice_fs.c',
  'v2_src\filesystem.c',
  'v2_src\sha256.c'
)
$tests = @(
  @{ Name = 'copper_spice_contract_test'; Define = @() },
  @{ Name = 'copper_spice_step23_test'; Define = @() },
  @{ Name = 'mna_contract_test'; Define = @('/DCOPPER_TESTING=1') },
  @{ Name = 'mna_bug_regressions_test'; Define = @('/DCOPPER_TESTING=1') },
  @{ Name = 'ngspice_fixture_execution_test'; Define = @() }
)
foreach ($trace in 0, 1) {
  New-Item -ItemType Directory -Force "build\trace$trace" | Out-Null
  foreach ($test in $tests) {
    $exe = "build\trace$trace\$($test.Name).exe"
    $args = @(
      '/nologo', '/std:c17', '/W4', '/WX', '/fp:strict',
      "/DCOPPER_TRACE=$trace", '/I', 'v2_src',
      "/Fobuild\trace$trace\", "/Fe$exe"
    ) + $test.Define + @("tests\$($test.Name).c") + $src
    & cl.exe @args
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $($test.Name), TRACE=$trace" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "test failed: $($test.Name), TRACE=$trace" }
  }
}
```

For GCC/Clang reference runs, use strict C17 plus `-ffp-contract=off -fno-fast-math`:

```bash
SRC='v2_src/arena.c v2_src/nmath.c v2_src/cplx.c v2_src/circuit.c v2_src/copper_linalg.c v2_src/mna.c v2_src/copper_spice.c v2_src/copper_spice_expand.c v2_src/copper_spice_fs.c v2_src/filesystem.c v2_src/sha256.c'
gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math \
  -DCOPPER_TRACE=0 -Iv2_src tests/copper_spice_step23_test.c $SRC -lm \
  -o /tmp/copper_spice_step23_test && /tmp/copper_spice_step23_test
```

Replace the test filename for the other test programs. Add `-DCOPPER_TESTING=1` for `mna_contract_test.c` and `mna_bug_regressions_test.c`. Repeat with `-DCOPPER_TRACE=1` for the trace-enabled build. To inspect tracing, call `copper_trace_set_callback(callback, context)` before parsing/simulation; the callback receives synchronous `CopperTraceEvent` objects. Parse events carry line/tokens/status; Newton events carry iteration/residual/alpha/voltage-delta/GMIN/source-scale; linear events carry pivot growth/residual/refinement; transient events carry time/dt/method/iterations. Strings/token pointers are borrowed only for the callback duration. `COPPER_TRACE=0` removes dispatch from the build.

To apply strict floating-point options to an existing CMake target, include `cmake/CopperFpContract.cmake` after creating the target and call `copper_apply_strict_fp(your_solver_target)`. GCC/Clang use `-fno-fast-math -ffp-contract=off`; MSVC uses `/fp:strict`. The helper does not add sources or alter the project's target graph.

## 7. Bug registry

The B-number rows are retained as requested. B01–B03 and B09/B11/B14 were not described in the supplied source bundles or the current defect note, so no new failure description or regression was invented for those entries. Their mapping is required to make a meaningful regression for each.

| ID | One-line description | Status | Regression test name | Patch date |
|---|---|---|---|---|
| B01 | Original issue description/reproducer was not present in supplied files. | USER-REPORTED FIXED; NOT INDEPENDENTLY REVALIDATED | No mapped test; source issue unavailable | 2026-11-15 |
| B02 | Original issue description/reproducer was not present in supplied files. | USER-REPORTED FIXED; NOT INDEPENDENTLY REVALIDATED | No mapped test; source issue unavailable | 2026-11-15 |
| B03 | Original issue description/reproducer was not present in supplied files. | USER-REPORTED FIXED; NOT INDEPENDENTLY REVALIDATED | No mapped test; source issue unavailable | 2026-11-15 |
| B04 | `fabs(VTO)` discarded PMOS/depletion-mode threshold polarity. | PATCHED; focused test PASS | `test_signed_mos_thresholds` | 2026-11-15 |
| B05 | Device thermal voltage/default temperature could differ from 300.15 K. | DEFAULT VERIFIED AT 300.15 K; test PASS | `test_default_temperature_is_300_15k`, `test_temperature_scaling` | 2026-11-15 |
| B06 | Diode series-resistance Newton solve lacked robust limiting/failure reporting. | PATCHED WITH BRACKETED NEWTON/BISECTION; fail-closed regression PASS; ngspice comparison NOT RUN | `test_diode_series_resistance_bias`, `test_diode_series_solver_fails_closed`, `test_newton_iteration_limit_is_reported` | 2026-11-15 |
| B07 | Logarithmic AC sweep did not maintain constant decade spacing. | PATCHED; ratio/end-point regression PASS | `test_ac_decade_spacing` | 2026-11-15 |
| B08 | Generic `limit_pair_step` did not apply device-specific PN/FET limiting. | PATCHED; diode/BJT/FET limiter regression PASS | `test_pnjlim_and_fetlim`, `test_signed_mos_thresholds` | 2026-11-15 |
| B09 | Original issue description/reproducer was not present in supplied files. | BLOCKED — needs B09 definition | No mapped test; source issue unavailable | 2026-11-15 |
| B10 | Scale parser needed `mil`, `meg` versus `m`, and trailing unit coverage. | PATCHED/VERIFIED; suffix regression PASS | `test_scale_suffixes` | 2026-11-15 |
| B11 | Original issue description/reproducer was not present in supplied files. | BLOCKED — needs B11 definition | No mapped test; source issue unavailable | 2026-11-15 |
| B12 | AVX2/FMA-specific solver code caused build/result divergence. | SIMD INTRINSICS REMOVED; scalar source scan PASS | Static source scan: no `__m256`, `_mm256`, `immintrin`, `__AVX2__` | 2026-11-15 |
| B13 | Direct MNA `malloc`/`calloc` allocations occurred in solve/refinement paths. | PATCHED TO ARENA SCRATCH + REUSABLE LU WORKSPACE; workspace test PASS | `test_reusable_linear_workspaces`; MNA allocation source scan | 2026-11-15 |
| B14 | Original issue description/reproducer was not present in supplied files. | BLOCKED — needs B14 definition | No mapped test; source issue unavailable | 2026-11-15 |
| FP-01 | Floating-point contraction/fast-math policy was not enforced for solver builds. | CMake helper and strict GCC command added; CMake helper smoke test PASS | `CopperFpContract.cmake` smoke configure/build | 2026-11-15 |
| SPICE-ROOT-01 | Include paths could escape the deck's permitted directory through path traversal or symlink resolution. | PHYSICAL PATH CONTAINMENT IMPLEMENTED; POSIX SYMLINK REGRESSION PASS; CONCURRENT-MUTATION TOCTOU LIMIT DOCUMENTED | `csp_include_root_test` | 2026-11-15 |
| FS-001 | Windows filesystem calls previously used narrow ANSI APIs rather than a defined UTF-8 boundary. | IMPLEMENTED WITH UTF-8/UTF-16 WRAPPERS; Windows runtime NOT RUN | `filesystem_v2_test` (UTF-8 path case; POSIX PASS) | 2026-11-15 |
| FS-002 | Generated files could become visible partially written or have inconsistent replace semantics. | ATOMIC CREATE/REPLACE APIS ADDED; POSIX TEST PASS; Windows NOT RUN | `filesystem_v2_test` | 2026-11-15 |
| FS-003 | Lexical include-root checks did not resolve symlink targets. | PHYSICAL RESOLUTION AND CONTAINMENT ADDED; POSIX SYMLINK REGRESSION PASS; TOCTOU LIMIT DOCUMENTED | `csp_include_root_test` | 2026-11-15 |
| FS-004 | Directory enumeration order was platform-native and nondeterministic. | SORTED BY UTF-8 BYTE ORDER | `filesystem_v2_test` | 2026-11-15 |
| FS-005 | Windows executable lookup checked `.exe` only and ignored `PATHEXT`. | PATHEXT SEARCH IMPLEMENTED; Windows runtime NOT RUN | Windows `filesystem_v2_test` executable lookup branch | 2026-11-15 |
| FS-006 | Filesystem reads required malloc-owned buffers even for Arena consumers. | `fs_read_file_arena` ADDED AND USED BY SPICE FILE ADAPTER | `filesystem_v2_test`, `csp_include_root_test` | 2026-11-15 |
| FS-007 | No in-project SHA-256 primitive/file hashing API existed. | SHA-256 ADDED; STANDARD KNOWN-ANSWER TESTS PASS | `sha256_test`, `filesystem_v2_test` | 2026-11-15 |
| FS-008 | Filesystem operations did not classify permission, missing-path and non-file failures consistently. | POSIX CLASSIFICATION TESTED; WINDOWS ERROR MAPPING NOT RUN | `filesystem_v2_test` (run as `nobody`) | 2026-11-15 |

## 8. Latest test results

These results were freshly generated for the Filesystem V2 patch on 2026-11-15. Earlier solver tests were not rerun as part of this filesystem-only step and are therefore marked NOT RUN here.

| Test | Result | Command | Date |
|---|---|---|---|
| Filesystem API contracts (UTF-8 test path, path normalisation, physical containment, Arena read, atomic writes, sorted listing, PATH lookup, status mapping) | PASS | `cmake -S tests/filesystem_v2 -B build/filesystem-v2 -DCMAKE_BUILD_TYPE=Release && cmake --build build/filesystem-v2 --parallel && ctest --test-dir build/filesystem-v2 --output-on-failure` | 2026-11-15 |
| SHA-256 known-answer tests (empty, `abc`, one million `a`, binary file hash) | PASS | Included in the same CMake/CTest command above (`sha256_test`) | 2026-11-15 |
| SPICE include-root integration and symlink-escape rejection | PASS | Included in the same CMake/CTest command above (`csp_include_root_test`) | 2026-11-15 |
| Filesystem, SHA-256 and SPICE include-root ASan+UBSan | PASS | `cmake -S tests/filesystem_v2 -B build/filesystem-v2-sanitize -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g' -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' && cmake --build build/filesystem-v2-sanitize --parallel && ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build/filesystem-v2-sanitize --output-on-failure` | 2026-11-15 |
| Permission-denied classification as unprivileged user | PASS | Run `filesystem_v2_test` as user `nobody`; it verifies that writing beneath a mode-0555 directory returns `FS_ERR_PERMISSION` | 2026-11-15 |
| All `v2_src/*.c` translation units strict compile | PASS | `for f in v2_src/*.c; do gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -Iv2_src -c "$f" -o "build/all-objects/$(basename "$f" .c).o" || exit 1; done` | 2026-11-15 |
| Windows UTF-8 wrappers, atomic replace and `PATHEXT` lookup | NOT RUN | Requires Windows/MSVC runtime; this environment is Linux | 2026-11-15 |
| Windows ACL permission mapping | NOT RUN | Requires Windows ACL test account and filesystem | 2026-11-15 |
| Full assembled-project CMake/CTest | NOT RUN | The uploaded archive contains `v2_src/` only; no root project CMake file/test graph was supplied | 2026-11-15 |
| Existing solver regression suite | NOT RUN | This patch changes filesystem code; solver tests were not rerun in this step | 2026-11-15 |
| ngspice differential suite | NOT RUN | Not part of this filesystem step; previous ngspice availability limitation remains | 2026-11-15 |

The POSIX symlink test resolves a link inside the allowed root that targets a file outside it and confirms that the path is rejected. The unprivileged permission test was executed as `nobody`; under a privileged UID that individual permission assertion prints `NOT RUN` rather than claiming a result. The Windows-specific implementation is present but has not been compiled or executed here.

## 9. Determinism notes

- Numeric operations use `double` IEEE-754 values and the C math library. `nmath.h` is the project's numeric utility header; `cplx.c` supplies scalar complex arithmetic. The solver is not fixed-point.
- The thermal conversion constant currently used by the circuit/model code is `k/q = 8.617333262145e-5 V/K`. The MOS oxide capacitance conversion uses a local permittivity constant. A central physical-constants header is not part of this patch.
- Solver reference builds should use `-fno-fast-math -ffp-contract=off` on GCC/Clang or `/fp:strict` on MSVC. Do not use `-ffast-math` or `/fp:fast` for numerical regression runs. The added CMake helper applies the policy only when explicitly called on a target.
- Active AVX2/FMA source paths were removed from the solver in this patch. The solver now uses scalar C loops in both configurations; no SIMD runtime-dispatch path is present.
- Disabling contraction makes compiler-level FMA contraction predictable, but it does not guarantee bit-identical results across compilers, CPUs or different C math libraries. Tests should compare with stated numeric tolerances.
- Parameter parsing uses `strtod`; `exp`, `pow`, `log`, `sqrt` and other math functions remain libm operations. Cross-platform libm output may vary in the final low-order bits.
- SHA-256 uses the standard 32-bit word schedule and byte order. File hashing streams in fixed-size blocks; hashes are deterministic for the exact byte sequence read. The file is not locked or snapshot-isolated while hashing, so callers must prevent concurrent mutation when producing a release manifest. No manifest JSON schema/writer is introduced in this patch.
- Directory listings are sorted by UTF-8 byte order, not locale collation. This is deterministic across runs and locales for the same filenames.
- `fs_path_absolute` is lexical: it normalises separators and `.`/`..` but does not resolve symlinks. `fs_path_resolve_existing` resolves physical targets and requires them to exist. `fs_path_is_within` compares physically resolved paths; its check/read sequence is not race-free against concurrent filesystem mutation.
- Atomic create uses a sibling temporary file and a no-replace commit. Atomic replace uses a sibling temporary file and a replace commit. Both flush file contents before commit; POSIX also attempts to fsync the parent directory. Durability semantics ultimately depend on the filesystem and storage hardware.
- Windows path inputs/results are UTF-8; the Windows implementation converts at the API boundary to UTF-16. POSIX paths are passed as UTF-8 byte strings without Unicode normalisation.
- Parser node/model/element name matching is case-insensitive. First spelling may be retained in `CspNetlist.node_names`; circuit node IDs are resolved case-insensitively. The literal `gnd` is not an alias for node `0`.
- Subcircuit names are deterministic scoped paths. Include paths are resolved physically before root containment checks. Path case follows platform filesystem behaviour; concurrent path replacement can still race the check/read sequence.
- `COPPER_TRACE=0` compiles trace sites to no-ops. `COPPER_TRACE=1` calls a synchronous process-global callback; callback registration is not safe to mutate concurrently with active simulations.

## 10. Checklists

### Add a device model

1. Add model fields and explicit units to `Circuit*Model` in `circuit.h`.
2. Validate model values and device terminals in `circuit.c` (`circuit_validate`).
3. Add model-family parameter allow-list and Csp-to-Circuit mapping in `copper_spice.c` (`model_params_supported`, `csp_build_circuit`). Unknown parameters must fail closed.
4. Implement terminal currents and analytic Jacobian in `mna.c` `device_eval`; keep terminal order and current signs explicit.
5. Add any AC capacitance stamps in `device_ac_capacitances` and its caller. For transient charge, add state/history terms consistently to the integration-method stamping; an AC-only stamp is not a transient charge model.
6. Add PN/FET limiting to `limit_nonlinear_steps` if required, preserving signed polarity-normalised thresholds.
7. Add finite-difference Jacobian tests in `tests/mna_contract_test.c` and parser/model/OP/AC/TRAN regression decks in `tests/copper_spice_step23_test.c` or `tests/ngspice/decks/`.
8. Add documented parameter scope, tests, bug ID and changelog entry in `README.md`/`CHANGELOG.md` in the same patch. Compare to ngspice if available; otherwise mark differential validation NOT RUN.

### Add a SPICE directive

1. Parse directive tokens in `copper_spice.c` and define the exact accepted grammar and errors.
2. If it affects hierarchy, expressions or external files, implement the expansion-stage handling in `copper_spice_expand.c/.h` or filesystem access in `copper_spice_fs.c/.h`.
3. Map directive state into `CspNetlist`, `Circuit` or analysis execution without silently discarding it.
4. Reject unsupported forms and malformed values with a physical line number where available.
5. Add accepted, rejected, case-insensitive, CRLF, continuation, overflow and scope tests in `tests/copper_spice_step23_test.c` and/or `tests/copper_spice_contract_test.c`.
6. Add a reference deck and tolerance if the directive affects simulation output; run ngspice differential when installed, else mark it NOT RUN.
7. Update the SPICE subset table, bug registry, test-results table and `CHANGELOG.md` in the same patch.


## 11. Filesystem V2

### Path model

- Public path strings are UTF-8. Windows converts UTF-8 to UTF-16 before calling Win32 file APIs and converts returned names/paths back to UTF-8. POSIX treats the UTF-8 bytes as native path bytes; it does not perform NFC/NFD normalisation.
- `fs_path_join` joins path fragments and does not collapse dot segments. `fs_path_absolute` / `fs_path_normalize` return an absolute lexically normalised path, collapsing repeated separators, `.` and `..`. They do not resolve symbolic links.
- `fs_path_resolve_existing` requires an existing path and resolves symlinks/junctions to the physical target. `fs_path_is_within` physically resolves both arguments and performs a separator-boundary containment check. Both the candidate and root must exist.
- The SPICE include adapter uses physical containment. The containment check and subsequent file read are separate operations; a concurrent attacker with permission to replace path components may race them. This is not a substitute for OS-level sandboxing.

### Reads, writes and status handling

- `fs_read_file` returns malloc-owned bytes. `fs_read_file_arena` returns Arena-owned bytes with a trailing NUL; `out_size` excludes that terminator and embedded NUL bytes are preserved. The Arena must remain alive for the data's full use period.
- `fs_write_file_exclusive` retains its legacy direct-write behaviour; the final path is visible while it is being written. Use `fs_write_file_atomic_exclusive` for create-only generated outputs and `fs_write_file_atomic_replace` only when replacement is intended. Both stage in a sibling temporary file, flush file contents and then commit.
- `fs_rename_path` retains platform behaviour: POSIX `rename()` may replace an existing file, while Windows `MoveFileW` does not. `fs_rename_path_replace` explicitly requests replacement.
- `FsStatus` distinguishes `FS_ERR_NOT_FOUND`, `FS_ERR_NOT_DIRECTORY`, `FS_ERR_NOT_FILE`, `FS_ERR_PERMISSION`, `FS_ERR_ALREADY_EXISTS`, `FS_ERR_RANGE`, `FS_ERR_OUT_OF_MEMORY` and `FS_ERR_IO`. On Windows, permission/not-found classification maps `GetLastError`; on POSIX it maps `errno`.
- `fs_list_directory` returns entries sorted by raw UTF-8 byte order. Symlink/reparse-point entries are not reported as regular files.
- `fs_find_executable_on_path` searches `PATH`; Windows uses `PATHEXT` (or `.COM;.EXE;.BAT;.CMD` defaults). `.BAT` and `.CMD` are located as executable candidates, but launching them may require a command interpreter.

### SHA-256

- `fs_sha256_bytes` / `fs_sha256_hex` hash an in-memory byte sequence. `fs_sha256_file` / `fs_sha256_file_hex` stream a file in 64 KiB chunks without loading the entire file into memory.
- Digest bytes and lowercase hex formatting follow standard SHA-256. The tests include the empty string, `abc`, one million `a` bytes and a binary file containing embedded NUL.
- The hashing API does not generate a manifest or lock source files against concurrent modification. A later manifest layer should record canonical input/output paths, byte sizes, SHA-256, tool version and relevant fab-profile identity, and should hash files only after writers have closed them.

### Build and test this layer

From the Copper project root with CMake and a C17 compiler:

```bash
cmake -S tests/filesystem_v2 -B build/filesystem-v2 -DCMAKE_BUILD_TYPE=Release
cmake --build build/filesystem-v2 --parallel
ctest --test-dir build/filesystem-v2 --output-on-failure
```

Sanitizer reference command on GCC/Clang platforms:

```bash
cmake -S tests/filesystem_v2 -B build/filesystem-v2-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build/filesystem-v2-sanitize --parallel
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build/filesystem-v2-sanitize --output-on-failure
```

From a Visual Studio Developer PowerShell prompt, the Windows build is:

```powershell
cmake -S tests/filesystem_v2 -B build/filesystem-v2 -A x64
cmake --build build/filesystem-v2 --config Release --parallel
ctest --test-dir build/filesystem-v2 -C Release --output-on-failure
```

The Windows commands are supplied for your machine and were **NOT RUN** in this environment. The test target includes the Windows `PATHEXT` executable lookup branch and the UTF-8 path case.

## 6.1. Run a Windows board corpus

Place read-only copies of `.kicad_pcb` files beneath a corpus directory, then run this script from Windows PowerShell 5.1:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\v2_src\tests\run_board_corpus.ps1 `
  -BoardRoot .\validation\boards `
  -CopperExe .\build\Release\copper.exe `
  -RulesFile .\v2_src\jlcpcb_rules.rules `
  -OutputRoot .\validation\runs\local
```

The script runs board inspection, structural validation, Copper DFM, and (when available) KiCad CLI DRC. Each board gets an isolated output directory and the summary records the SHA-256 of the input board. The current KiCad DRC invocation uses each board's project settings; it is a reference run, not a rule-equivalent differential test, until equivalent `.kicad_dru` rules are generated.
