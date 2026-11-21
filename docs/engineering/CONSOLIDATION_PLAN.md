# Consolidation plan: one engine per job, nothing deleted

- **Date:** 2026-11-21
- **Status:** read-only audit (Step 0a). No code was changed to write it.
- **Rules:** `PIPELINE_HARD_RULES.md` (H10: keep the base architecture and code; add beside it); gap G-044

## 1. The situation

Copper has two C engine families, plus legacy deterministic Python:

| Family | Library | Binaries | Origin |
|---|---|---|---|
| **copper-core** | `copper_v2` (+ `hwcore`) | `copper-core`, `pcba`, `hwc` | V2 tracks A–H |
| **copper** | `copper_design` | `copper` | the `feature/update` port, the Manus pipeline, template_gen, autoroute |
| Python (legacy) | `tools/*.py` | `pcba` → `copper.py` | V1, the `pcba design` path |

The two C families **cannot be linked into one binary**: they define the same type and function names
(`BoardStatus`, `BoardPad`, `board_load_kicad`, `mna_*`…), which is why they are separate libraries.
Merging them would mean renaming and rewriting base modules, which H10 rules out without the owner's explicit
decision.

**The Manus pipeline already joins the families without changing either.** `copper.ps1` calls
`copper design generate|route|import` (copper_design) and `copper-core dfm` (copper_v2), and passes files and
JSON between them. This plan keeps that pattern.

## 2. The engine for each job

"Canonical" means: **new pipeline code uses this engine.** The others stay unchanged, keep their current users
and tests, and are not extended. Where useful, they serve as an **independent cross-check** (H6).

| Job | Canonical for new work | How it is reached | Kept unchanged, and their role | Why this one |
|---|---|---|---|---|
| Board model | `design_board.c` + `board_kicad.c` | in-process (copper_design) | `board.c`: the copper-core board model (DFM, Gerber, kicad-cli, tool server) | already used by the router, import and inspect; per-layer table, arc tracks, board rules, the KiCad tree kept for round-tripping |
| DFM | `dfm.c` | `copper-core dfm BOARD PROFILE` (JSON) | `dfm_check.c` (narrower, G-046); `src/dfm_pcb.c` (legacy pcba) | cited and dated `fab_profiles/*.json`; the most rules (pad-to-track, PTH-to-track); exact integer geometry |
| Placement | `placement.c` | `copper-core place PROBLEM ITER SEEDS` (JSON) | `placer.c` (only its own test uses it); `tools/layout.py` (legacy) | the tested port of the layout.py placer: constructive placement plus seeded annealing, deterministic. Known gaps: no fab-profile limits, one side only (G-004) |
| Routing | `autoroute.c` | `copper design route` | `router.c` (single net, **no users**); `tools/layout.py` (legacy) | multi-net, rip-up, widths and keep-outs; exact re-check of every net; in the pipeline with 0-unconnected results |
| Circuit physics (MNA) | `spice_mna.c` + `circuit.c` | in-process (copper_design) | `src/mna.c`, `mna_ac.c`, `transient.c` (hwcore; `copper-core analyze`, numeric oracles): **independent cross-check** for tests (H6) | same library as the pipeline; R/L/C, sources, controlled sources, diode/BJT/MOSFET/JFET; DC, AC and transient through one API; `engineering_loop.c` already binds catalogue parts to it |
| Verified parts | `part_catalogue.c` | in-process | `src/part_registry.c` (V1) | provenance per part; rating bounds; `part_record_sources_verified()` matches H2 |
| Candidate parts (distributor data) | `jlc_catalogue.c` | in-process | — | indexes by part id and LCSC id; refuses stale availability data (`max_availability_age_days`) |
| Topology, netlist, parity, grounding gate | `topology.c`, `netlist_kicad.c`, `design_parity.c`, `intent_check.c` | in-process | — | no duplicates |
| Design generation | `template_gen.c` (today); `blocks.c` (new, Phase 2) | `copper design generate` | `tools/blocks.py`, `tools/design.py` (legacy, reference for the port) | templates stay as a working path and become the **regression oracle**: the ESP32 board built from blocks must match the template one |
| Correction loop | `engineering_loop.c`, `optimization_loop.c` | in-process | — | already have typed actions (move footprint, route a two-pad net) and physics feedback; Phase 7 builds on them |
| KiCad schematic and PCB writing | **gap**: today `tools/copper_design_compile.py` (legacy Python) | — | `src/kicad_schematic.c`, `src/kicad_pcb.c` (V1 `hwc` writers; reference only) | a new C writer in `copper_design` (Phase 4) replaces the Python compiler once it passes the same tests |
| CAM and kicad-cli | `kicad_cli.c`, `gerber.c`, `excellon.c` | `copper-core kicad …`, `copper-core gerber`, `copper-core report` | — | no duplicates |
| Findings format | the format `copper.findings/1` (`FINDINGS.md`) | JSON | `finding.c` (copper-core), `report.c` (copper_design) both stay | new code writes the documented format; no third report module |

## 3. Consequences for the roadmap

- **No module is deleted, moved or renamed** (H10). Retiring a module ever (for example `router.c`, `placer.c` or
  `dfm_check.c`) is a separate owner decision. G-044 stays OPEN as an owner decision.
- **Cross-family calls go through the command line and JSON**, as they do today. Each call is a process run with
  the inputs passed as files and the result as JSON. This keeps the families independent and testable.
- **Phase 2 (blocks)**: `blocks.c` lives in `copper_design`, uses `part_catalogue.c` and `topology.c`, and
  is checked against `template_gen.c` output.
- **Phase 3 (pre-layout physics)**: uses `spice_mna.c` through `engineering_loop.c`. The tests compare chosen
  results with the hwcore solver.
- **Phase 4 (writers)**: a new C writer. Placement comes from `copper-core place`.
- **Phase 5/6/7** build on `autoroute.c`, `spice_mna.c` and `optimization_loop.c`. DFM always comes from
  `copper-core dfm`.

## 4. Gaps found by this audit

| Gap | Where | Handling |
|---|---|---|
| `copper_design` has no sanitizer option; `COPPER_V2_SANITIZE` covers only `copper_v2` | `CMakeLists.txt` | **Done in Step 0b:** the same flags are applied to `copper_design` under that option (G-061) |
| The golden-board check (T5) is a script outside the repository | `D:\copper-build-tmp\baseline_t5.ps1` | **Done in Step 0b:** `manus/golden_boards.ps1`, `manus/golden/` (G-062) |
| `GAP_REGISTER.md`, `PROMPT_RESULTS.md` and `IMPLEMENTATION_CONTRACT.md` exist only as untracked files in the main checkout (`v2/complete`), not on this branch | main checkout | **Done in Step 0b:** copies are on this branch (G-063) |
| `router.c` and `placer.c` have no users outside their tests | `copper_design` | kept (H10); recorded here so new code does not start using them (G-064) |
