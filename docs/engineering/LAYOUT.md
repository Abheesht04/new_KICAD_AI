# Placement and routing

`tools/layout.py` owns placement, routing and the staged layout search. It never judges a board: every
candidate is re-checked by copper-core's exact copper kernel (`src/copper.*`, `src/u128.*`, unchanged), and
only that check decides whether a board is feasible. DFM rule ownership is unchanged (copper-core).

| Command | What it does | Status |
|---|---|---|
| `pcba place FILE --out PLACED [--seeds 1,2,3] [--iterations N] [--report F]` | staged placement search; writes the best feasible placement | PASS, or FAIL when no feasible board was found (nothing written) |
| `pcba route FILE --out DIR` | routes the project's own placement; writes `DIR/layout.json` and, if feasible, `DIR/routed.copper` | PASS / FAIL; UNVERIFIED if the geometry could not be judged |
| `pcba project build` | routes with rip-up/reroute and fails rather than claim a partial route; a project with **no** placed part (e.g. lowered from a spec) is first placed by the same staged search, recorded in `manifest.json` → `placement`; a partly placed project is refused | — |
| `python tools/layout_bench.py` | baseline vs optimizer on the fixtures ([LAYOUT_BENCHMARK.md](LAYOUT_BENCHMARK.md)) | — |

## Router and instrumentation

0.25 mm grid, two layers, A*, via cost 20, nets in sorted order, each net grown from its first pad toward the
nearest unconnected pad, bounded at 250 000 states per connection. Conservative obstacle envelopes; the exact
kernel judges. Two styles, and every placement candidate is routed in both:

* **manhattan**: the original four-direction router. With default options it is bit-identical to the router
  before step 4 on every layout fixture: traces, vias and statistics.
* **octilinear**: 45-degree steps (cost √2) and a turn cost of 0.5 per 45° and 1.5 per 90° turn. No turn
  sharper than 90° is allowed, so the router never makes an acid trap. The turn is judged against the
  direction the best path to a cell arrived from, which keeps the search at one state per cell as in Manhattan
  routing. A diagonal step never cuts past a blocked corner. A goal-directed flood fill over the same moves
  proves "no path" before the search starts. It is exact: with no corner cutting, the octilinear search
  reaches exactly what the four-direction flood reaches.
  The style also prefers clearance: each step that passes within 0.1 mm beyond the clearance of foreign
  copper, the edge or a keepout costs 2 extra.

**Per-net width from current.** copper-core bounds each net's DC current. Every MNA device is evaluated alone
at the operating point; its residual on a node row is the current it carries out of that node. A net's bound
is half the sum of those magnitudes, which no segment of any copper tree joining its pins can exceed. A net
has no bound when it touches a connector, an MCU pin other than a driven output or pull, or a device with two
terminals on that net; such a net is charged the whole-circuit current, as before.

When a trace violates the current-density or drop budget, only its net is widened: by the reported ratio,
rounded up to 0.05 mm, at most 3 mm and at most three rounds. Near a pad narrower than the net's width
(within 0.5 mm), the trace necks down to the pad's smaller side. The neck never goes below the width the
net's current needs or the rule minimum.

A widened net is recorded as `(routing (width NET MM))`. copper-core then fails any trace of that net that is
narrower (`net_width`), and the KiCad export adds a matching `A.NetName` custom rule.

**Clearance margin** is measured for every candidate: the smallest distance between different-net copper
objects involving a trace or via, minus the clearance. Pads are taken as full rectangles, so the margin is
never overstated. Each pair within 0.05 mm of the limit is a soft cost (`near_limit_pairs`).

**Speed.** Routed copper marks the cells it blocks per probe kind (width, via, margin) when it is committed,
so each probe is one lookup instead of a neighbourhood scan. The test and its distances are the same, so
results are bit-identical.

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
* current-density and drop budgets: each trace is charged its net's DC current bound, or the whole-circuit
  supply current when the net has none;
* each net's declared `(routing (width ...))`; no acute angle (`acute_angle`: two segments of one net meeting
  at under 90° outside a pad of that net).

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
| copper pairs within 0.05 mm of the clearance limit | 1.0 |

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
3. **Routing** of every candidate in both styles with rip-up/reroute.
4. **Exact post-check** with copper-core on the candidate's full geometry: the first judge.
5. **Feedback**: a current-density or drop-budget violation widens the violating nets (above) and the
   candidate is re-routed and re-checked. The placed project records `(routing (style ...) (width NET MM) ...)`,
   so a build reproduces the proven copper exactly (tested).
5a. **Second judge** (`pcba design` with a KiCad 9.x CLI): the top three feasible candidates, in copper-core's
   order, are built through the shipping export and checked by KiCad DRC. The first that KiCad does not FAIL is
   chosen. Every verdict is in `design.json` → `layout.kicad_judge`, and without KiCad 9.x that entry says
   "not run".
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
* A net touching a connector or MCU supply has no DC current bound and is charged the whole-circuit current;
  declaring connector currents is not supported yet.
* The via budget is a cost (20 grid steps per layer change, weight 2 per via), not a hard limit.
* The octilinear router keeps one state per cell; it can miss a path that only a worse arrival direction would
  allow. It is still checked exactly, and the Manhattan candidate is always judged next to it.
* No teardrops, arcs or sliver checks yet.
* Silkscreen spacing is a conservative estimate; KiCad DRC is the authority for silkscreen.
