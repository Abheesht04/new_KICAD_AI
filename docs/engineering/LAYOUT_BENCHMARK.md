# Layout benchmark

`python tools/layout_bench.py --symbol-dir examples/generated/divider/libraries/symbols --out bench.json`
(defaults: seeds 1,2,3; 1500 annealing iterations; footprints from the committed KiCad 9 library snapshot).
Every number is from copper-core's exact check of the full candidate geometry. *Baseline* is the
pre-optimizer pipeline: the project's own placement, one routing pass in sorted net order at the profile
width. *Optimized* is `pcba place`'s chosen candidate. Two runs produce identical JSON apart from wall-time
fields.

Recorded 2026-10-23 (copper-core and tools at the Prompt 8 commit; Windows, MinGW GCC 12.1, Python 3.12):

| Fixture | Run | Feasible | Hard (by rule) | Unrouted | Length mm | Vias | Bends | Width mm | Explored | Blocked | Soft score |
|---|---|---|---|---|---|---|---|---|---|---|---|
| divider | baseline | yes | 0 | 0 | 52.24 | 0 | 11 | 0.20 | 3588 | 35 | 59.44 |
| divider | optimized | yes | 0 | 0 | 13.74 | 0 | 3 | 0.20 | 323 | 0 | 15.72 |
| multi_connector | baseline | NO | open_connection 2 | 1 | 140.99 | 4 | 24 | 0.20 | 54382 | 1784 | 167.97 |
| multi_connector | optimized | yes | 0 | 0 | 47.28 | 0 | 14 | 0.20 | 1361 | 20 | 55.73 |
| tight_clearance | baseline | NO | copper_clearance 4 | 0 | 55.24 | 0 | 9 | 0.20 | 3439 | 32 | 61.44 |
| tight_clearance | optimized | NO | copper_clearance 4 | 0 | 13.74 | 0 | 5 | 0.20 | 220 | 0 | 16.72 |
| edge_keepout | baseline | NO | open_connection 4, keepout_violation 4 | 3 | 6.65 | 0 | 0 | 0.20 | 106 | 0 | 8.57 |
| edge_keepout | optimized | yes | 0 | 0 | 13.74 | 0 | 3 | 0.20 | 323 | 0 | 15.72 |
| high_current | baseline | NO | current_density_budget 22 | 0 | 51.15 | 0 | 8 | 0.20 | 3490 | 75 | 56.48 |
| high_current | optimized | yes | 0 | 0 | 20.36 | 0 | 7 | 0.75 | 549 | 1 | 24.34 |
| unroutable | baseline | NO | open_connection 7 | 5 | 9.87 | 0 | 3 | 0.20 | 49371 | 122 | 15.57 |
| unroutable | optimized | NO | open_connection 7 | 5 | 9.87 | 0 | 3 | 0.20 | 49371 | 122 | 15.57 |
| rc_network | — | UNVERIFIED | Capacitor_SMD:C_0603_1608Metric is not in the committed snapshot | | | | | | | | |

`rc_network` with a library that has the capacitor footprint (here a local KiCad 10 footprint copy, so this
row is development evidence only): baseline feasible, 52.15 mm / 11 bends; optimized feasible, 13.60 mm /
3 bends.

## Reading the results

* **Feasible improvements**: divider (−74 % length), and boards the baseline could not finish:
  multi_connector (the baseline leaves a connection open; rip-up alone also rescues the given placement in two
  passes, at 157.6 mm), edge_keepout (the given placement sits in the keepout), high_current (copper widened
  from 0.20 to 0.75 mm until copper-core's current-density budget holds).
* **Honest failures**: tight_clearance is infeasible for every candidate — its 1.7 mm header pads are 0.84 mm
  apart under a 1.0 mm clearance profile, a property of the footprint and profile that no layout can fix.
  unroutable is infeasible: its fixed connectors sit on either side of a keepout band banning tracks and vias
  on both layers; five connections are reported unrouted with `no_path`.
* **Annealing**: on these fixtures no annealed candidate beat the constructive placement once the silkscreen
  envelope was enforced (seed 1 was worse on multi_connector: 65.8 mm). The gains above come from the
  constructive placement, rip-up/reroute and current-driven widening.
* **Native DRC**: the bench builds the chosen candidate and runs KiCad DRC only with a KiCad 9.x CLI; on the
  recording machine (KiCad 10.0.5) that column is UNVERIFIED. As development evidence only, the optimized
  divider, multi_connector, edge_keepout and high_current boards and the ESP32 example
  (`examples/v2/esp32_io.copper`, 302 → 185 mm, 6 → 2 vias) were built and checked with KiCad 10.0.5 DRC/ERC:
  0 violations, 0 unconnected items, 0 schematic-parity issues. An earlier optimizer revision produced
  silkscreen-overlap warnings there, which is why the silkscreen envelope exists.
