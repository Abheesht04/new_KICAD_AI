# KiCad golden DRC fixtures (KiCad 9.0.6)

These reports are intentional defect-injection fixtures, not boards expected to be clean. They are supplied to validate that our kernel detects known problems rather than to claim parity before the relevant geometry/import code exists.

## Known findings represented by the reports

- Four `unconnected_items` findings: pads on the same logical nets are not physically connected in the test board.
- `golden_pcb_drc.json` and `golden_pcb_drc_full.json` also contain one `invalid_outline` finding.
- All three reports contain four `silk_over_copper` and four `silk_overlap` warnings.
- Reports use millimetres and identify KiCad 9.0.6. Reports with different timestamps intentionally differ in their outline finding.
- `golden_netlist.kicad_net` and `golden_netlist.xml` carry the logical connectivity reference.

## Current boundary

The JSON reports are expected findings, not complete physical board models. The matching `.kicad_pcb` / `.kicad_sch` source files and DRC configuration are needed to reproduce the reported geometry exactly. Do not infer absent copper or silk geometry from the JSON diagnostics alone.

The current defect module can detect open connectivity for same-net pads represented in `PcbBoard`, and touching/overlapping supported copper shapes on common layers. It cannot yet reproduce the fixture's outline or silkscreen checks because the physical IR does not yet represent imported Edge.Cuts, silkscreen text/strokes, or soldermask openings. `fabcheck_demo --coverage` lists these gaps explicitly.
