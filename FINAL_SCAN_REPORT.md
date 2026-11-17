# Copper combined source — final scan report

Source reviewed: user-uploaded `v2_src(1).zip` (extracted to a clean working directory).

## Build and test verification

| Configuration | Build | CTest |
|---|---:|---:|
| GCC 14.2 Debug | PASS | 6/6 |
| GCC 14.2 Release | PASS after CMake test-assertion fix | 6/6 |
| Clang 17 Release | PASS after CMake test-assertion fix | 6/6 |
| GCC Debug + ASan + UBSan | PASS | 6/6 |

The unmodified uploaded CMake file failed the GCC Release build because CMake defines `NDEBUG`; the `assert()`-based optimization test then compiled away assertion expressions and triggered `-Werror=unused-*`. The supplied replacement CMake file adds `copper_enable_test_assertions()` and applies `/UNDEBUG` (MSVC) or `-UNDEBUG` (GCC/Clang) to the six test executables only. Production targets retain the existing Release settings and strict warnings.

## Additional smoke tests

- `copper --help`: PASS (exit 0).
- Sample `sample_resistor_lol.kicad_pcb` inspection: PASS; 5 footprints, 10 pads, 7 nets, 0 tracks, 0 vias, 1 Edge.Cuts primitive.
- Sample board structural validation: command PASS, but import coverage reports 79 skipped records and 75 unmodelled geometry records. This is not proof of complete board coverage and must not be interpreted as manufacturing sign-off.
- Catalogue resolution fixture: resolved `Device:R` and `Resistor_SMD:R_0603_1608Metric`; explicit pin map valid; generated resolution JSON parsed and validated against `copper.design-resolution.v2.schema.json`.
- Optimization evaluation on sample board: 44 candidates; evaluation JSON passed schema validation.
- Developer three-iteration optimization run: completed with 3 accepted actions; output JSON passed schema validation.
- All six JSON schema files parse as valid JSON.
- All 43 C sources listed by CMake exist.
- CMake patch applies cleanly to the original uploaded CMake file.

## Findings and limitations to keep visible

1. MSVC 2026 was not available in this environment. The Windows build still needs the user's verification.
2. KiCad CLI was not installed in this environment. Native KiCad ERC/DRC and sample-demo corpus tests were not run here.
3. The uploaded source tree contains no `pwsh` invocation. If Windows MSBuild still prints `'pwsh.exe' is not recognized`, use a verbose MSBuild/CMake log to identify the external build customization emitting it; it is not referenced in this source tree.
4. The sample PCB importer can report structural PASS while also reporting skipped/unmodelled records. Consumers must inspect coverage and treat relevant DFM as UNVERIFIED when required geometry is unsupported.
5. Optimization snapshots are partial JSON representations, not native `.kicad_pcb` files. The current source explicitly reports native export as NOT_IMPLEMENTED. No Gerber/Excellon export from optimized board state is present in this build.
6. The optimization CLI does not attach a candidate-specific MNA/parasitics/thermal evaluator. The optional physics callback is an API integration point; absent physics evidence must remain visible.
7. The CLI's help text still uses `copperv2` in several legacy command examples although the generated executable target is named `copper`. This is cosmetic, not a build failure.
8. Root `manifest.json` is packaging metadata for the earlier optimization overlay, not the runtime project manifest created under a user's `.copper/` directory. It is not consumed by the build. Its CMake hash was updated for the replacement CMake file.
9. `README.md` describes the SPICE subset and explicitly lists limitations such as dense MNA, no complete ngspice replacement, and no adaptive timestep. These remain product limitations, not build failures.

## Recommended Windows build commands

For the Visual Studio multi-configuration generator, omit `CMAKE_BUILD_TYPE` and select Release at build time:

```powershell
Remove-Item .\build -Recurse -Force -ErrorAction SilentlyContinue
cmake -S v2_src -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Do not disable `/WX`, `/W4`, or `/fp:strict` to get a successful build.
