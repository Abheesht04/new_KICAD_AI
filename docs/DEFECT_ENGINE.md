# Defect analysis and DFM self-audit

## Design decision

`defect.c/.h` is not a second DFM report system. It emits typed `PcbDefectFinding` values through a visitor. `dfm_pcb.c` adapts these into the existing `PcbDfmReport`/`PcbDfmIssue` output so CLI and JSON consumers keep one issue schema.

## Current checks

- **Open connectivity:** convert existing tracks, circular pads and plated vias into copper shapes; union same-net shapes that touch/overlap on a common layer; report same-net non-zero-ID pads that belong to different physical connected components.
- **Direct short:** report different-net supported copper shapes that touch/overlap on a common copper layer.
- **Invalid copper geometry:** report objects that cannot be converted into supported copper geometry.
- **DFM report audit:** recheck rule evidence and key geometry references, including trace width, via size/drill/annular values, hole-to-hole clearance, board-edge clearance and pairwise copper clearance. Contradictory findings become `engine_inconsistency` issues.
- **Coverage registry:** publishes rules as `PARTIAL` or `UNSUPPORTED` where physical IR/checks do not yet exist.

## Important limits

1. Open detection considers represented pads with the same non-zero net ID. Net ID zero does not yet have a globally defined no-net meaning; the broader electrical/physical IR contract must settle this.
2. Geometric connectivity currently supports straight centreline tracks with width and circular pads/vias. It does not support arc tracks, custom polygon pads, filled zones, or imported footprint copper geometry.
3. The board model does not yet contain imported Edge.Cuts geometry, silk text/strokes, or soldermask openings; `invalid_outline`, `silk_over_copper`, and `silk_overlap` from the golden reports remain coverage gaps.
4. DFM self-audit checks whether emitted findings agree with the currently represented model; it cannot discover omitted rule families by itself. The coverage registry exists to make that limitation visible.
5. These checks are development diagnostics, not a claim of KiCad parity or fabrication signoff.

## Run

- `ctest --test-dir build --output-on-failure`
- `build/fabcheck_demo --coverage`

The DRC JSON fixtures are intentional fault-injection reports. A matching `.kicad_pcb` source is still required to reproduce every reported defect from raw geometry.
