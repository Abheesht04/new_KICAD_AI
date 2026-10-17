# Copper engine (v2)

Owner: azure (design) - kernel. Files: `src/copper.[ch]`, `src/pcb_copper.[ch]`,
`src/geom.[ch]`, `src/u128.[ch]`, `src/nmath.[ch]`.

## One idea
Every copper object is a **core** (point, segment, axis-aligned rect) plus a
**radius**. Gap = distance(core_a, core_b) - r_a - r_b. The pass/fail test is
rewritten without a root:

    clear  <=>  distance(core_a, core_b) >= required + r_a + r_b

and decided by exact integer comparators `geom_cmp_*` (nm, int64/uint64 plus
128-bit via `u128`). They return sign(distance - T). Composite cores take the
`min` of the leaf comparisons because sign() is monotone.

## Layers of the code
| Layer | Files | Knows about |
|---|---|---|
| exact math | `u128`, `nmath`, `geom` | integers only |
| copper model | `copper` | CopperShape / CopperItem / pair check |
| adapter | `pcb_copper` | PcbBoard (double mm, KiCad layer IDs) -> CopperItem |
| rules | `dfm_pcb`, `defect` | profiles, findings, connectivity |

mm -> nm happens only in `pcb_copper.c` (positions: nearest; sizes and
requirements: up; copper radius = ceil(size / 2)).

## Result semantics
PASS / NOT_APPLICABLE (nothing measured) / TOO_CLOSE / SHORT / UNVERIFIED
(arc, polygon: never a pass) / INVALID_INPUT. Net 0 is "no net" and is never the
same net as anything, so touching unassigned copper is a SHORT.

## Bounds
Coordinates within +-500 mm (`pt_in_bounds`). Thresholds >= 2^31 nm are
"farther than any possible distance". Requirements <= 2^32 nm.

## Not yet
Arcs, polygons/zones (reserved enum values), rotated rects, grid broad-phase
(currently bbox reject + O(n^2)), per-net-class clearances.
