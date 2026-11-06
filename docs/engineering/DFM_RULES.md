# DFM rules

Generated from `tests/test_dfm_rules.py`, where every row below is executed against copper-core on the routed
divider board (`examples/v2/divider.copper`, committed KiCad 9 library snapshot). A board row has one passing
and one failing board; the test also fails if the engine can report a rule that has no row. Native KiCad DRC is
a separate, independent gate (`project verify`) and is not counted here.

| Rule | Family | Status | Exercised by |
|---|---|---|---|
| `trace_width` | copper | tested: passing and failing board | routed traces are 0.20 mm |
| `copper_clearance` | copper | tested: passing and failing board | exact copper kernel, all copper pairs |
| `trace_clearance` | copper | tested: passing and failing board | trace-to-trace subset of copper clearance |
| `short_circuit` | connectivity | tested: passing and failing board | a GND trace laid over VIN |
| `open_connection` | connectivity | tested: passing and failing board | the long VIN segment removed |
| `pad_missing` | connectivity | tested: passing and failing board | J1 pad 1 removed from geometry |
| `edge_clearance` | outline | tested: passing and failing board | J1 pads sit 7.15 mm from the left edge |
| `via_diameter` | drills | tested: passing and failing board | 0.6 mm via |
| `via_drill` | drills | tested: passing and failing board | 0.3 mm via drill |
| `via_annular_ring` | drills | tested: passing and failing board | 0.15 mm annular ring |
| `pad_annular_ring` | drills | tested: passing and failing board | header pads: 1.7 mm pad, 1.0 mm drill, 0.35 mm ring |
| `pad_drill` | drills | tested: passing and failing board | 1.0 mm header drills against the minimum hole |
| `hole_clearance` | drills | tested: passing and failing board | header holes 1.54 mm apart |
| `all_hole_clearance` | drills | tested: passing and failing board | pad and via holes together |
| `drill_aspect` | drills | tested: passing and failing board | 1.6 mm board / 1.0 mm drill = 1.6 |
| `mask_web` | solder mask | tested: passing and failing board | between neighbouring pad openings |
| `courtyard_gap` | assembly | tested: passing and failing board | rectangular courtyard envelopes |
| `component_height` | assembly | tested: passing and failing board | synthetic 0.55 mm heights |
| `keepout_violation` | keepouts | tested: passing and failing board | a rule area over the VIN trace |
| `current_density_budget` | current | tested: passing and failing board | 0.17 mA over 0.007 mm2 = 0.024 A/mm2; an estimate, not IPC-2152 |
| `trace_drop_budget` | current | tested: passing and failing board | I x rho x L / A per segment |
| `net_width` | current | tested: passing and failing board | VIN declared 0.25 mm wide, routed at 0.20 mm |
| `acute_angle` | copper | tested: passing and failing board | VIN segments meeting at 90 degrees (and one overlapping another: the same copper), then at 17 degrees, away from pads |
| `board_size` | outline | enforced at load (INVALID_INPUT) | boards over 500 mm are refused at load (INVALID_INPUT) |
| `layer_count` | stack-up | enforced at load (INVALID_INPUT) | only two-layer boards are supported; others are refused at load |
| `invalid_geometry` | geometry | enforced at load (INVALID_INPUT) | a zero-width trace is refused at load |
| `height_unknown` | assembly | reports UNVERIFIED when data is missing | a part without a sourced height keeps assembly checks UNVERIFIED |
| `geometry_missing` | geometry | reports UNVERIFIED when data is missing | a project without board geometry: physical DFM did not run |
| `engine_inconsistency` | self-check | internal self-check (no board can trigger it) | the DFM engine's audit of its own findings; covered by tests/dfm_test.c and tests/defect_test.c |
| `dfm_engine` | self-check | internal self-check (no board can trigger it) | the physical DFM engine returned an error; never reachable with a valid board |
| `keepout_unverified` | keepouts | internal self-check (no board can trigger it) | a copper object could not be compared with a keepout; unsupported shapes are refused earlier |

## Not implemented (never reported as PASS)

- zones, copper pours, planes and thermal relief
- non-rectangular board outlines, cutouts, slots, castellations
- arcs
- more than two copper layers
- silkscreen checks (only placement keeps silk apart)
- sliver checks, teardrops, via-in-pad, tenting
- solder-mask slivers and mask-to-trace clearance
- paste apertures, stencil ratio, fiducials, tooling holes, panel rails
- creepage and voltage-dependent clearance
- IPC-2152 ampacity and temperature rise (current checks are estimates)
- impedance, differential pairs, length matching, return paths, EMC, RF, thermal
- Gerber/drill structural re-reading
