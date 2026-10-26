# Audit checklist — results

The owner's plan (§4, "nothing new is built until these are done"), item by item, with evidence. Measured on
2026-10-26: Windows 10, MinGW GCC 12.1 Release, Python 3.12, committed KiCad 9 library snapshot plus a local
KiCad 10.0.5 library copy for the ESP32 module (development only; not native validation).

| Item | Status | Evidence |
|---|---|---|
| Capture the failing English-to-board command | **Done** (M0) | root cause `Cannot escape pad`: unplaced spec-lowered project; `project build` now places it — [M0_ACCEPTANCE_EVIDENCE.md](M0_ACCEPTANCE_EVIDENCE.md) #4 |
| Fix the Unicode unit cases | **Done** (M0) | harness double-encoded prompts; parser names double encoding; `test_prompt_acceptance.UnicodeRun` |
| Harness statuses PASS/FAIL/BLOCKED/SKIPPED/UNVERIFIED/OBSERVATIONAL | **Done** (M0) | `tools/prompt_acceptance.py`, `test_prompt_acceptance.Classification` |
| Content gate: empty boards are never a DRC pass | **Done** (M0) | `project verify` → `board_content`; `test_native_gates.Content` |
| DRC runner: separate stdout/stderr, use the exit code and the report | **Done for Copper's gate** (M0) | `test_native_gates.Classification`; the owner's raw-board runner is outside this repository (triage below) |
| Install KiCad 9.x natively and record the version | **Owner** | not possible from this machine; every native gate already records `kicad_version` and refuses non-9.x |
| Per-rule DFM table, one passing and one failing board per rule | **Done** | [DFM_RULES.md](DFM_RULES.md), generated from `tests/test_dfm_rules.py` (CTest `dfm_rules`): 21 rules with passing and failing boards, 3 load-time, 2 UNVERIFIED-reporting, 3 internal self-checks; the test fails if the engine gains a rule without a row |
| Profile one full `pcba design` run | **Done** | below; `design.json` now carries `profile` (time per stage, child processes per executable) on every run |
| Triage table of every failing prompt or board | **Done** | below |

## Profile and what changed

`cProfile` of `pcba design "Build me an ESP32 LED PCB. Generate the KiCad PCB."` before any change (271 s under
the profiler, **72 s** wall clock):

| Cost | Share | Cause |
|---|---|---|
| S-expression parsing | ~37 % | KiCad symbol libraries parsed again for every symbol lookup (31 times; `Connector_Generic` is 6.8 MB) |
| Router `blocked()` | ~40 % | every grid probe re-tested every pad, keepout and the board edge, which never change while routing |
| Placement legality | ~12 % | pad rectangles recomputed and every pad scanned for every candidate position |
| copper-core and kicad-cli processes | ~3 % | 42 copper-core runs, 1.3 s; kicad-cli 0.8 s (KiCad 10 here: version checks only) |

Changes — each kept outputs **byte-identical** (all 20 ESP32 build artifacts, gates and spec fingerprint; every
layout-benchmark candidate including explored-state and congestion counts):

1. `copper.library_tree`: each library file parsed once per process, keyed by path, size and mtime; footprints are
   deep-copied before modification.
2. Router: the static verdict (edge, keepouts, pads) cached per cell, layer, net and probe type within a route;
   pads bucketed by inflated extent; occupancy neighbourhood offsets precomputed with the same float test.
3. Placement: `Board.legal` (early exit, cached pad rectangles, envelope pre-filter) for the search; `violations`
   kept for reporting. A randomised test asserts `legal == not violations` on 1 200 placements.
4. S-expression tokenizer: a regex tokenizer with an explicit stack replaces the character loop. The previous
   parser is kept in `tests/test_hwc_kicad.py` as the oracle; trees are identical (with Atom/Quoted types) for every
   S-expression file in the repository, 400 KiCad library files and malformed inputs. `Device.kicad_sym` 2.7 → 0.9 s,
   `RF_Module` 0.65 → 0.15 s, `Connector_Generic` 6.0 → 4.0 s (pure-Python per-token floor).

| Run | Before | After |
|---|---|---|
| `pcba design` ESP32 LED (wall clock, includes the determinism re-run) | 72 s | **19 s** |
| `layout_bench.py --seeds 1,2 --iterations 300` | 32 s | **17 s** |

After the changes the run is spread over the stages (`design.json` → `profile`): compile 3.7 s (first parse of the
large symbol libraries), placement and routing 3.6 s, KiCad export 4.5 s, verify 0.6 s, tamper checks 1.4 s,
determinism re-run 5.1 s. The solver remains negligible. CUDA or a C port of the router is not justified by these
numbers yet; the next target would be the remaining first parse of `Connector_Generic`.

## Triage

| Case | Stage | Cause | Status now |
|---|---|---|---|
| English prompts "rc filter with unicode units", "micro sign and ohm sign" | harness → parser | prompts double-encoded by the harness (UTF-8 read as Windows-1252) | PASS with UTF-8; double encoding named precisely |
| "DC solve from English" | harness | wrong assertion on a correct result | PASS against the analytical V(OUT) |
| "KiCad board generated from English" (`NO_JSON/70`) | `project build` | spec-lowered project had no placements | PASS: populated board, content checked |
| ESP32 LED `design.json` gate 8 FAIL (KiCad 9.0.6) | native DRC | one `lib_footprint_mismatch` **warning** on U1; gate treated exit 5 as FAIL | UNVERIFIED with the warning listed; cause of the warning open (see M0 document) |
| `examples/v2/esp32_io.copper` verify exit 1 (KiCad 9.0.6 benchmark) | native DRC | the same single `lib_footprint_mismatch` warning | UNVERIFIED with the warning listed |
| Other Copper examples (diode, divider, rc, rc_ac, rl) on KiCad 9.0.6 | all | unknown ratings and heights | UNVERIFIED (correct; needs rated parts) |
| 114 of 133 open-hardware boards: no DRC report, exit 9009 | owner's raw-board runner → kicad-cli | KiCad 9.0.6 could not load the board ("Failed to load board"): **104** are KiCad 5 files (`20171130`, legacy zone fill), **7** are KiCad 10 files (`20260206`), **3** are KiCad 4 legacy files. `9009` is the runner's fallback, not a KiCad exit code | input-format incompatibility, not a DRC result: classify as `INPUT_UNSUPPORTED` in that runner (outside this repository) |
| 15 open-hardware boards `ERRORS_PRESENT`, 2 `WARNINGS_ONLY`, 2 `PASS` | KiCad 9 DRC | real findings in third-party boards (e.g. `hackrf-one`: 22 errors, 448 warnings) | third-party evidence only; not Copper output |
| Open-hardware "passes" on empty boards | owner's runner | boards with 0 pads/tracks/nets | Copper's own gate refuses empty boards (`board_content`) |

## Build plan, Slice 1: English to a divider board that passes KiCad 9 DRC

| Item | Status | Evidence |
|---|---|---|
| Automated test | **Written; UNVERIFIED here** (no KiCad 9.x on this machine) | CTest `slice1_divider` → `tests/test_prompt_acceptance.py Slice1`: the corpus case `divider board` through `spec parse` → `spec lower` → `project build` → `project verify`; requires KiCad 9.x and asserts ERC, DRC, netlist and pin/net parity all `PASS`, the content gate (3 footprints, 7 pads, 3 nets) and unchanged output hashes. Exit 77 (skipped) without a KiCad 9.x CLI or libraries |
| Open it in KiCad | after the run | the project is kept in `<build>/slice1/build/design.kicad_pro` |
| Connector from English | **Done** | the first divider board had no connector, so VIN and GND each reached one pin (ERC `isolated_pin_label`). English can now bind a catalogue part by id (`J1 uses part header3`); nothing else selects a part. `test_spec.Lowering.test_english_binds_a_part_by_id_only` |

The overall verdict of `project verify` stays `UNVERIFIED` for this board: generic parts have no ratings or heights.
That is correct and separate from the board gates this slice asserts.

Development preview only (not native validation): KiCad 10.0.5's engine, run through a wrapper that reports 9.0.6,
gave ERC, DRC, netlist and parity `PASS` for this board. The slice is done when `slice1_divider` passes on a real
KiCad 9.x install.

## Build plan, step 2: KiCad environment and library index

| Item | Status | Evidence |
|---|---|---|
| Detect installed KiCad versions, select by policy, JSON snapshot | **Done** | `pcba library env`; highest 9.x with CLI and libraries, never another major; `test_kicad_library.Installs` |
| Global and project lib-tables, `${VAR}` expansion, user libraries | **Done** | environment, then `kicad_common.json`, then install defaults; per-entry status; `test_kicad_library.Tables` |
| SQLite index with full-text search, regenerable | **Done** | `pcba library scan` / `search`; only changed libraries reparsed; byte-identical for any `--jobs`; `test_kicad_library.Index` |
| Symbol-to-footprint pairing by pin/pad number sets | **Done** | `pcba library pair`; filters rank, never add; `test_kicad_library.Index.test_search_and_pairing` |
| One owner for finding KiCad | **Done** | `hwc_kicad.discover_*` use `kicad_library` (9.x first) |

Cold index of a full KiCad library set: 292 s for the first version, **33.8 s** after profiling (selective reader,
bulk inserts, largest libraries first); warm re-scan 0.9 s. Details: [KICAD_LIBRARY.md](KICAD_LIBRARY.md).

## Build plan, step 3: rules importer

| Item | Status | Evidence |
|---|---|---|
| `.kicad_pro` board setup, `.kicad_dru` and net classes into the profile | **Done** | `pcba rules import`; the strictest global limit per field; `test_kicad_rules.Import` |
| Source and date for each limit | **Done** | `(provenance (FIELD "source" "date"))`, checked by copper-core; file SHA-256 in the report; `test_kicad_rules.RoundTrip.test_copper_core_checks_provenance` |
| What a global profile cannot express | **Reported** | conditional/layer rules, maximums, other constraint types, hole-to-copper clearance: listed, result UNVERIFIED |
| Builds hand KiCad the same limits | **Done (found while testing)** | builds wrote no board constraints and exported `hole_gap_mm` as `hole_clearance` (hole-to-copper); now `.kicad_pro` rules, `hole_to_hole`, `via_diameter`, `courtyard_clearance` and mask web; `test_kicad_rules.RoundTrip` |

Details: [KICAD_RULES.md](KICAD_RULES.md).

## Build plan, step 4: DFM-aware router

| Item | Status | Evidence |
|---|---|---|
| Cost includes margin to rule limits | **Done** | octilinear routing costs steps within 0.1 mm beyond clearance; candidates pay per copper pair within 0.05 mm of the limit; measured margin per candidate; `test_layout.Dfm.test_margin_metric` |
| 45-degree routing, no acute angles | **Done** | octilinear style, no turn sharper than 90°, no corner cutting; kernel rule `acute_angle` judges every board; `Dfm.test_octilinear_is_shorter...` |
| Slivers | **Not done** | no sliver check yet (DFM_RULES.md, not implemented) |
| Via budget | **Cost only** | 20 grid steps per layer change, weight 2 per via; no hard per-net limit |
| Neck-downs at pads | **Done** | within 0.5 mm of a narrower pad, never below the net's current floor; `Dfm.test_neck_down_never_below_the_current_floor` |
| Per-net width from current | **Done** | copper-core per-net DC current bound (device residuals, half the absolute sum); only violating nets widened; `(routing (width NET MM))` checked by the kernel (`net_width`) and exported to KiCad; `Dfm.test_per_net_width_from_the_current_bound` |
| Two judges: exact kernel and KiCad DRC | **Done** | copper-core judges every candidate; with KiCad 9.x the top three are built and KiCad-DRC-checked (`layout.second_judge`); `test_native_gates.SecondJudge`, `Dfm.test_kicad_is_the_second_judge` |
| C port or threads only after profiling | **Profiled; neither needed yet** | octilinear routing first spent its time in a per-probe neighbourhood scan (7.3 M dict lookups on one board); routed copper now marks the cells it blocks, so a probe is one lookup (multi_connector given placement 3.0 → 0.9 s, identical results); a hopeless connection is refuted by a flood fill (unroutable 69 s → about 1 s). `layout_optimizer` runs both styles in 38 s (35 s before, Manhattan only) |

Results: [LAYOUT_BENCHMARK.md](LAYOUT_BENCHMARK.md).

ESP32 LED design: the chosen octilinear candidate has 222.95 mm and 12 vias, against 252.56 mm and 14 vias.
Placement and routing take 8.6 s (both styles), and the KiCad judge adds 4 s when KiCad is present. The Manhattan
candidates of that design had six 0-degree "acute angles": overlapping copies of one segment, the same copper.
The rule now ignores exact overlaps, and the DFM table tests that case.

## Build plan, step 5: parallelism

| Item | Status | Evidence |
|---|---|---|
| Placement seeds in parallel | **Done** | `layout.optimize` runs `anneal` per seed on a reused process pool (`COPPER_JOBS`, default the CPU count) |
| Candidate routing in parallel | **Done** | every (placement, style) evaluation is a pool job |
| Deterministic winner, independent of finish time | **Done** | results in submission order; ties broken by candidate index; 1 vs 3 processes identical on multi_connector, mixed_current, unroutable (`test_layout.Parallel`); the ESP32 board is byte-identical with `COPPER_JOBS=1` and `4` |
| KiCad second judge in parallel | **Tried, not kept** | judging three candidates at once took 9.6 s against 4.8 s sequentially: the first candidate usually passes, so the extra builds are wasted |

Measured on this 4-logical-core machine (1500 iterations, 3 seeds, wall clock):

| Run | 1 process | 4 processes |
|---|---|---|
| high_current | 3.54 s | 1.18 s |
| tight_clearance | 2.09 s | 0.88 s |
| multi_connector | 5.92 s | 3.43 s |
| unroutable | 10.60 s | 5.00 s |
| ESP32 LED design, placement and routing stage | 9.6 s | 6.3 s |

Each search's wall time is now bounded by its slowest single candidate. That is typically a congested given
placement that needs rip-up passes, and those passes are sequential by nature.

## Build plan, step 6: circuit blocks

| Block in the plan | Status | Source of its values |
|---|---|---|
| LDO | **Done** (`ldo`) | the regulator part's datasheet data; copper-core's new `ideal.ldo` model and `regulator_dropout` / `regulator_current` / `regulator_reverse` / dissipation checks |
| USB-C power | **Done** (`usb_c_sink`) | USB Type-C specification Rd = 5.1 kΩ; the receptacle part's pin roles |
| I2C sensor | **Done** (`i2c_sensor`, `i2c_pullups`) | NXP UM10204 pull-up sizing (Rp min/max formulas, mode limits); the sensor part's pin roles and decoupling |
| Decoupling | **Done** (`reference_circuit`) | the part's datasheet reference circuit (one implementation, also used by `pcba design`) |
| Reset/boot | **Done** (`reset_boot`) | the MCU catalogue's reset pin and strapping notes |
| Provenance for every value | **Done** | every added value is listed with `standard` / `part` / `param` and its text; `(reference ...)` without provenance is refused by copper-core |

No regulator, receptacle, sensor or button is bundled: inventing one is not allowed, so a real board needs real
parts with their datasheet data. Tests use TEST ONLY stand-ins. Details: [BLOCKS.md](BLOCKS.md).

