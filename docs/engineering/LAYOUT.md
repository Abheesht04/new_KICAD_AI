# Placement and routing

`tools/layout.py` owns placement, routing and the staged layout search. It never judges a board: every
candidate is re-checked by copper-core's exact copper kernel (`src/copper.*`, `src/u128.*`, unchanged), and
only that check decides whether a board is feasible. DFM rule ownership is unchanged (copper-core).

| Command | What it does | Status |
|---|---|---|
| `pcba place FILE --out PLACED [--seeds 1,2,3] [--iterations N] [--report F]` | staged placement search; writes the best feasible placement | PASS, or FAIL when no feasible board was found (nothing written) |
| `pcba route FILE --out DIR` | routes the project's own placement; writes `DIR/layout.json` and, if feasible, `DIR/routed.copper` | PASS / FAIL; UNVERIFIED if the geometry could not be judged |
| `pcba project build` | unchanged contract; its router now has rip-up/reroute and still fails rather than claim a partial route | — |
| `python tools/layout_bench.py` | baseline vs optimizer on the fixtures ([LAYOUT_BENCHMARK.md](LAYOUT_BENCHMARK.md)) | — |

## Router and instrumentation

The router is the original candidate router, moved unchanged: 0.25 mm grid, two layers, Manhattan A*,
via cost 20, nets in sorted order, each net grown from its first pad toward the nearest unconnected pad,
bounded at 250 000 states per connection. Conservative obstacle envelopes; the exact kernel judges.

It now reports, per net: connections, routed connections, explored states, length, bends, vias and failures
(`pad_escape`, `no_path`, `state_limit`), plus wall time. Per board: unrouted connections with their pads,
generated objects, total explored states, congestion (occupied cells, expansions blocked by other nets, and
the ten most-blocked cells), and copper-core's findings by rule. A failed connection is reported, never
raised past the caller and never drawn.

**Rip-up/reroute** (deterministic): when connections fail, the whole board is ripped up and the failed nets
are routed first in the next pass, up to four passes or until an order repeats; the best pass is kept and the
pass history reported.

## Hard constraints and soft objectives

Hard (a candidate with any of these is infeasible; no soft gain can buy one back):

* electrical connectivity: every connection routed; no `open_connection` or short;
* pad/net mapping as the electrical IR states (copper-core rejects any disagreement);
* board outline, copper-to-edge, courtyard gap, board keepouts and footprint keepouts;
* the selected profile's clearance, width, drill, annular, hole and mask rules;
* supported geometry only: geometry copper-core cannot judge (`dfm_engine`, `keepout_unverified`) makes the
  candidate *unjudged*, which ranks below every judged candidate and is never a pass;
* current-density and drop budgets from the circuit's explicit loads (copper-core's conservative
  whole-circuit supply current applied to every segment).

Soft (reported separately, compared only between candidates with identical hard standing), with weights:

| Term | Weight |
|---|---|
| routed length (mm) | 1.0 |
| vias = layer changes | 2.0 |
| bends | 0.5 |
| congestion: expansions blocked by other nets | 0.001 |
| placement area, bounding box of courtyards (mm²) | 0.01 |
| decoupling distance: capacitor on an MCU supply/ground pair to that supply pin (mm) | 1.0 |
| footprint keepout (e.g. antenna) distance from the board edge (mm) | 0.5 |

Selection is lexicographic: `(judged, hard violations, unrouted connections, soft score, candidate index)`.
Routing time is reported but never compared (it is not reproducible). Net classes for sensitive nets do not
exist yet, so "length of sensitive nets" is reported per net only.

## Staged search

1. **Constructive placement** (deterministic): `(fixed)` parts stay; parts with keepouts first, then by
   connectivity to what is placed; each at the legal 1 mm grid position and rotation (0/90/180/270) with the
   least half-perimeter wirelength to placed neighbours. Legal means: courtyard inside the outline, pads at
   least the profile's edge distance from it, courtyard gap kept, no body or pad in a board or footprint
   keepout (both directions), and a conservative silkscreen envelope (F.SilkS graphics plus an over-estimated
   reference-text box) kept off other parts' silkscreen and pads — KiCad DRC remains the judge of silkscreen.
   A part with no legal position is reported, and no constructive or annealed candidate is produced.
2. **Improvement**: seeded simulated annealing per seed (`random.Random(seed)`, fixed iterations, geometric
   cooling 0.997, moves shift / rotate / swap). Illegal states are rejected outright, never penalised. Cost:
   half-perimeter wirelength plus the weighted placement soft terms.
3. **Routing** of every candidate with rip-up/reroute.
4. **Exact post-check** with copper-core on the candidate's full geometry.
5. **Feedback**: a current-density or drop-budget violation widens all traces by the reported ratio (rounded
   up to 0.05 mm, at most 3 mm, at most three rounds) and the candidate is re-routed and re-checked. A widened
   width is written into the placed project as its (stricter) `trace_width_mm`.
6. **Limits and snapshot**: candidates are the given placement (when every part has one), the constructive
   placement and one annealed placement per seed; the best candidate's full resolved geometry is kept.
7. **Baseline comparison**: `layout_bench.py` measures the pre-optimizer pipeline (given placement, one
   routing pass, profile width) with the same exact check.

Determinism: identical inputs give byte-identical boards and reports apart from wall-time fields (tested).
No claim of optimality is made: the search is a small, bounded, deterministic heuristic. When no candidate is
feasible the result is FAIL with the best candidate's hard findings and unrouted connections, and nothing is
written as a board.

## Board keepouts and fixed parts

```lisp
(keepouts (area X1 Y1 X2 Y2 LAYERS FLAGS))   ; LAYERS: 1 F.Cu, 2 B.Cu, 3 both; FLAGS: 1 tracks, 2 vias, 4 pads, 8 footprints
(component J1 (part header3) (pins ...) (at 4 7 0) (fixed))   ; the optimizer never moves it
```

copper-core checks board keepouts exactly like footprint keepouts (`keepout_violation`).

## Known limits

* Annealing has not beaten the constructive placement on the current fixtures (see the benchmark); it is kept
  as a deterministic candidate source, not claimed as an improvement.
* Placement is grid-based (1 mm, four rotations); there is no analytic or global placer.
* Trace width is uniform per board; per-net widths need net classes and a per-net current model.
* The current budget is copper-core's whole-circuit bound, so widening is conservative for small branches.
* Silkscreen spacing is a conservative estimate; KiCad DRC is the authority for silkscreen.
