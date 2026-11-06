# V2 validation record

Validated on 2026-10-17. These results describe the delivered source and example;
they do not establish correctness for every circuit, footprint or manufacturing process.

## Environment and results

| Check | Environment | Result | Evidence |
| --- | --- | --- | --- |
| Release build and CTest | Ubuntu 24.04, GCC 13.3.0, CMake 4.4.4, C17 | 30/30 targets passed | `evidence/configure.txt`, `build.txt`, `ctest.txt` |
| V2 integration suite | Python, compiled C core, selected KiCad libraries | 22/22 cases passed, including native workflow | `evidence/v2-integration.txt` |
| Address/undefined behavior sanitizers | GCC debug build, ASan + UBSan | 30/30 targets passed | `evidence/sanitizer-tests.txt` |
| Native schematic/board validation | KiCad CLI 9.0.9 | Zero ERC/DRC violations, unconnected items or schematic parity issues; netlist matches electrical IR | `evidence/kicad-native.txt` |
| Desktop | GLFW, Vulkan, Dear ImGui, Mesa software Vulkan, Xvfb | Built; 20 frames rendered; exit 0; screenshot visually inspected | `evidence/desktop-build.txt`, `desktop-smoke.txt`, `studio.png` |
| CAM export integration | Separate divider fixture with synthetic explicit ratings/heights | Native gates passed; 19 CAM output files produced | `evidence/cam-test.txt` |

The desktop screenshot is [studio.png](studio.png). Windows/MSVC and macOS were
not executed here. Windows launch helpers and process support are supplied as source.
The desktop smoke run verifies startup/rendering/shutdown, not exhaustive interactive
UI behavior. Hardware GPU drivers were not exercised.

LeakSanitizer cannot operate under this environment's tracing restrictions. The
sanitizer run used `ASAN_OPTIONS=detect_leaks=0`; it is **not a leak-check result**.
The 22 Python integration cases are inside one of the 30 CTest targets, not 22
additional CTest targets.

## What the tests establish

- Divider voltage agrees with its closed form to nine decimal places; KCL residual
  is below 1e-10 A. Current-source polarity and summed resistor dissipation are checked.
- RC and RL transient outputs agree with the backward-Euler recurrence to eight
  decimal places. The selected RC endpoint is also within 0.01 V of the analytic
  exponential. Diode polarity and nonlinear KCL residual are checked independently.
- Resistor tolerance corners agree with closed-form divider bounds; targets and
  underspecified power ratings produce expected failures.
- Non-finite/negative input, duplicate identifiers/pins/rules, unknown rules/nets,
  floating nodes, unsupported active models and invalid transient parameters fail.
- The native workflow checks deterministic regeneration, installed geometry,
  symbol inheritance, pin mapping, 90-degree footprint rotation, opens/shorts,
  and mutated DFM rules. Both ordinary and rotated connector
  examples pass actual KiCad 9 ERC/DRC/parity checks.
- The geometry test compares 50,000 u128 products against GCC's native 128-bit
  oracle and checks rounded-pad contact/clearance boundaries. The production
  u128 implementation remains portable 64-bit arithmetic.
- Transformer tests cover representative categories and rejection of unknown or
  mixed inputs. These are regression tests, not an independent language benchmark.

Original `src/u128.c`, `src/u128.h`, `src/copper.c` and `src/copper.h` were compared
byte-for-byte with the input archive and are unchanged.

## Included example status

`examples/generated/divider` contains the native project, selected local library
snapshots, reports, calculation trace, BOM and content hashes. Its physical report
contains **0 errors and 7 warnings**: four unknown resistor power/voltage ratings
and three unknown component heights. Those properties cannot be inferred from
generic KiCad geometry. Native ERC/DRC/parity are clean, but `fabricate` intentionally
blocks this generic example until actual part ratings and heights are supplied.

The separate CAM integration fixture used synthetic values only to exercise that
export path. Its generated manufacturing files are not distributed as a verified
real-world design. The included `rc_transient.csv` is a numerical example, not a
measurement from hardware.

## Reproduce

From the extracted project directory on Linux:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
export KICAD_SYMBOL_DIR="$PWD/examples/generated/divider/libraries/symbols"
export KICAD_FOOTPRINT_DIR="$PWD/examples/generated/divider/libraries/footprints"
ctest --test-dir build --output-on-failure
python3 tests/test_v2.py build/copper-core -v
python3 tools/copper.py verify --out examples/generated/divider
```

Use installed full KiCad library roots instead when testing additional parts.
Library and native cases live in the `V2Native` class and the separate CTest target
`v2_native`. `test_library_build_workflow` is skipped without the two library
environment variables; `test_kicad9_native_gates` is additionally skipped unless a
KiCad 9.x CLI is discoverable, and the skip message names the version found.
`test_v2.py` exits 77 when anything was skipped, which CTest reports as "Skipped"
(not "Passed"). The supplied logs above predate this split.

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DCOPPER_SANITIZE=ON
cmake --build build-sanitize --parallel
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-sanitize --output-on-failure
```

Enable leak detection on a host that supports it. The optional CMake presets
require CMake 3.21+ and Ninja; explicit configure commands require CMake 3.20+.
See `START_HERE.txt` for Windows commands and `V2_IMPLEMENTATION_NOTES.txt` for
the implemented modeling/geometry limits.
