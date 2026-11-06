# Layout benchmark

`python tools/layout_bench.py --symbol-dir examples/generated/divider/libraries/symbols --out bench.json`
(defaults: seeds 1,2,3; 1500 annealing iterations; footprints from the committed KiCad 9 library snapshot).
Every number comes from copper-core's exact check of the full candidate geometry. The three runs:
- *baseline*: the pre-optimizer pipeline, which is the project's own placement and one routing pass in sorted
  net order at the profile width;
- *manhattan*: the optimizer before step 4, with Manhattan candidates only;
- *optimized*: `pcba place`'s choice among Manhattan and octilinear candidates, with per-net widths and the
  clearance margin.

*Margin* is the measured smallest clearance margin of the router's copper. *Near limit* counts copper pairs
within 0.05 mm of the clearance. Two runs produce identical JSON apart from wall-time fields.

Recorded 2026-10-27 (build plan step 4; Windows, MinGW GCC 12.1, Python 3.12):

| Fixture | Run | Style | Feasible | Hard (by rule) | Unrouted | Length mm | Vias | Bends | Widths mm | Margin mm | Near limit | Explored | Soft score |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| divider | baseline | manhattan | yes | 0 | 0 | 52.24 | 0 | 11 | 0.20 | 0.060 | 0 | 3588 | 59.44 |
| divider | manhattan | manhattan | yes | 0 | 0 | 13.74 | 0 | 3 | 0.20 | 0.600 | 0 | 323 | 15.72 |
| divider | optimized | octilinear | yes | 0 | 0 | 12.72 | 0 | 3 | 0.20 | 0.206 | 0 | 302 | 14.69 |
| rc_network | — | | UNVERIFIED | footprint C_0603_1608Metric.kicad_mod is not in D:\New folder (5)\examples\generated\divider\libraries\footprints | | | | | | | | | |
| multi_connector | baseline | manhattan | NO | open_connection 2 | 1 | 140.99 | 4 | 24 | 0.20 | 0.020 | 3 | 54382 | 170.97 |
| multi_connector | manhattan | manhattan | yes | 0 | 0 | 47.28 | 0 | 14 | 0.20 | 0.100 | 0 | 1361 | 55.73 |
| multi_connector | optimized | octilinear | yes | 0 | 0 | 41.19 | 0 | 12 | 0.20 | 0.100 | 0 | 1704 | 48.73 |
| tight_clearance | baseline | manhattan | NO | copper_clearance 4 | 0 | 55.24 | 0 | 9 | 0.20 | 0.010 | 2 | 3439 | 63.44 |
| tight_clearance | manhattan | manhattan | NO | copper_clearance 4 | 0 | 13.74 | 0 | 5 | 0.20 | 0.050 | 0 | 220 | 16.72 |
| tight_clearance | optimized | octilinear | NO | copper_clearance 4 | 0 | 12.93 | 0 | 5 | 0.20 | 0.075 | 0 | 420 | 15.90 |
| edge_keepout | baseline | manhattan | NO | open_connection 4, keepout_violation 4 | 3 | 6.65 | 0 | 0 | 0.20 | 0.875 | 0 | 106 | 8.57 |
| edge_keepout | manhattan | manhattan | yes | 0 | 0 | 13.74 | 0 | 3 | 0.20 | 0.600 | 0 | 323 | 15.72 |
| edge_keepout | optimized | octilinear | yes | 0 | 0 | 12.72 | 0 | 3 | 0.20 | 0.206 | 0 | 302 | 14.69 |
| high_current | baseline | manhattan | NO | current_density_budget 22 | 0 | 51.15 | 0 | 8 | 0.20 | 0.125 | 0 | 3490 | 56.48 |
| high_current | manhattan | manhattan | yes | 0 | 0 | 20.36 | 0 | 7 | GND 0.75, VIN 0.75 | 0.050 | 4 | 549 | 28.34 |
| high_current | optimized | octilinear | yes | 0 | 0 | 18.32 | 0 | 8 | GND 0.75, VIN 0.75 | 0.200 | 0 | 489 | 22.79 |
| mixed_current | baseline | | — | the project has no placement of its own | | | | | | | | | |
| mixed_current | manhattan | manhattan | yes | 0 | 0 | 10.95 | 0 | 3 | GND 0.75, VIN 0.75 | 0.050 | 4 | 273 | 16.69 |
| mixed_current | optimized | octilinear | yes | 0 | 0 | 9.99 | 0 | 3 | GND 0.75, VIN 0.75 | 0.550 | 0 | 291 | 11.77 |
| unroutable | baseline | manhattan | NO | open_connection 7 | 5 | 9.87 | 0 | 3 | 0.20 | 0.100 | 0 | 49371 | 15.57 |
| unroutable | manhattan | manhattan | NO | open_connection 7 | 5 | 9.87 | 0 | 3 | 0.20 | 0.100 | 0 | 49371 | 15.57 |
| unroutable | optimized | octilinear | NO | open_connection 7 | 5 | 7.96 | 0 | 1 | 0.20 | 0.322 | 0 | 321 | 12.55 |

## Reading the results

* **Octilinear routing wins every feasible fixture.**
  - Shorter: divider −7 %, multi_connector −13 % (47.28 → 41.19 mm, 14 → 12 bends), high_current −10 %,
    mixed_current −9 %.
  - Never at the cost of a hard finding, and no acute corner: the kernel's new `acute_angle` rule judges every
    candidate.
* **The margin cost moves copper off the limit.**
  - high_current: Manhattan leaves 4 pairs at 0.05 mm of margin; octilinear leaves none, with a 0.20 mm
    minimum.
  - mixed_current: 4 pairs → 0, 0.05 → 0.55 mm.
  - divider and edge_keepout: the shorter octilinear board sits closer (0.21 mm against 0.60 mm) while staying
    above the 0.05 mm band; length outweighs margin in the soft score there.
* **Per-net widths from current** (`mixed_current`): VIN and GND carry a 0.500025 A bound and are widened to
  0.75 mm; SIG carries 25 µA and stays at 0.20 mm. Under the old whole-circuit budget every net was charged the
  full supply current. high_current's power nets touch a connector, so they still take the whole-circuit bound.
* **Unroutable** stays unroutable in both styles, with the same unrouted connections. The flood fill proves
  "no path" after 321 explored states instead of exhausting 424 k direction-aware states (69 s → about 1 s).
* **tight_clearance** is still infeasible for every candidate: its header pads are 0.84 mm apart under a
  1.0 mm clearance profile, which no layout can fix.
* **Development preview, not native validation:** each feasible optimized board, built and checked by KiCad
  10.0.5 DRC through a wrapper that reports 9.0.6, had 0 violations and 0 unconnected items. The ESP32 LED
  design (`pcba design`) moved from 252.56 mm / 14 vias to 222.95 mm / 12 vias, with ERC/DRC PASS and the
  determinism gate PASS: the build reproduced the octilinear candidate from its `(routing ...)` record.
* **rc_network** needs Capacitor_SMD, which is not in the committed snapshot.
