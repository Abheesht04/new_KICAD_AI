"""Placement and routing for Copper V2 (docs/engineering/LAYOUT.md).

Owns the deterministic 0.25 mm grid, two-layer A* router (Manhattan, or octilinear with 45-degree steps),
its instrumentation, rip-up/reroute, placement and the staged optimizer. It never judges a board: every
candidate is re-checked by copper-core's exact copper kernel, and only that check decides feasibility. Hard violations are counted, never weighed against soft objectives.

Determinism: no wall clock, hash order or unseeded randomness influences a result. Effort is bounded by
explored search states and iteration counts; elapsed time is reported but never used to choose.
"""
from __future__ import annotations

import copy, heapq, math, random, time

GRID = .25
PAD_BUCKET = 2.0  # mm: spatial bucket for pad obstacle lookup (a performance detail; results are unchanged)
MAX_STATES = 250000
# Soft objective weights (per unit). Applied only between candidates with equal hard feasibility.
WEIGHTS = {'length_mm': 1.0, 'vias': 2.0, 'bends': .5, 'blocked_expansions': .001, 'placement_area_mm2': .01,
           'decoupling_mm': 1.0, 'keepout_edge_mm': .5, 'near_limit_pairs': 1.0}
SOFT_TERMS = tuple(WEIGHTS)
# copper-core findings that mean the geometry could not be judged: never a pass.
UNJUDGED = {'dfm_engine', 'keepout_unverified'}


# --------------------------------------------------------------------------- router
DIRS = [(1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1)]  # index k = k * 45 degrees
MANHATTAN_MOVES = [0, 4, 2, 6]       # the original +x, -x, +y, -y expansion order
VIA_COST = 20
BEND_COST = {1: .5, 2: 1.5}          # octilinear: a 45-degree and a 90-degree turn, in grid steps
MARGIN_COST = 2.0                    # per step whose clearance to foreign copper is within the margin band
NECK_ZONE = .5                       # mm around a pad in which a wide trace may neck down to fit it
STYLES = ('manhattan', 'octilinear')


def route(pads, width, height, trace_width, clearance, edge, via_diameter=.6, keepouts=(), order=None,
          max_states=MAX_STATES, style='manhattan', widths=None, min_widths=None, margin=0.0):
    """Route every net of `pads`. Returns (traces, vias, stats); never raises for an unroutable net.
    Nets in `order` (default: sorted names), each grown from its first pad by nearest-pad A* on a GRID mm lattice,
    layer change cost VIA_COST, bounded states. style 'manhattan' is the original four-direction router;
    'octilinear' adds 45-degree steps, BEND_COST per turn and no turn sharper than 90 degrees (no acid traps),
    judged against the direction the best path to a cell arrived from (the state stays one per cell, as in
    Manhattan routing), and never cuts a corner diagonally. widths: per-net trace width (default
    trace_width); near a pad narrower than its net's width the trace necks down to the pad, never below the net's
    min_widths entry (e.g. what its current needs). margin > 0 adds MARGIN_COST to each step that passes within
    `margin` mm beyond the clearance of foreign copper, the edge or a keepout.
    stats: per-net effort and quality, unrouted connections with reasons, congestion."""
    if style not in STYLES:
        raise ValueError(f'routing style must be one of {STYLES}')
    octo = style == 'octilinear'
    widths = dict(widths or {}); min_widths = dict(min_widths or {})
    w_of = lambda n: widths.get(n, trace_width)
    w_max = max([trace_width] + list(widths.values()))
    nx = round(width / GRID); ny = round(height / GRID); traces = []; vias = []
    occupied = [{}, {}]; mids = [{}, {}]      # grid points of routed paths; midpoints of their diagonal steps
    blocked_at = {}
    # Pads, keepouts and the board edge do not change while routing: their verdict per (cell, layer, net, via,
    # width, inflation) is computed once. Pads are bucketed by their inflated extent so a probe only examines nearby pads.
    r_max = max(w_max / 2 + clearance, via_diameter / 2 + clearance) + margin
    buckets = {}
    for p in pads:
        x0, x1 = (p['x'] - p['w'] / 2 - r_max) / PAD_BUCKET, (p['x'] + p['w'] / 2 + r_max) / PAD_BUCKET
        y0, y1 = (p['y'] - p['h'] / 2 - r_max) / PAD_BUCKET, (p['y'] + p['h'] / 2 + r_max) / PAD_BUCKET
        for bx in range(math.floor(x0 - 1e-9), math.floor(x1 + 1e-9) + 1):
            for by in range(math.floor(y0 - 1e-9), math.floor(y1 + 1e-9) + 1):
                buckets.setdefault((bx, by), []).append(p)
    static = {}
    # Routed copper blocks probes near it. Rather than scanning a probe's neighbourhood, each committed path point
    # (grid points, and on the half grid the midpoints of diagonal steps) marks the cells it blocks for every probe
    # kind in use (via or not, probe width, margin): a probe is then one lookup. The test is the exact one,
    # distance < probe radius + the occupant's half width (+ margin), with distances from integer offsets.
    committed = []          # (hx, hy, z, net) in half-grid units
    influence = {}          # (via, w, extra) -> [layer 0, layer 1]: cell -> nets whose copper blocks the probe there

    def mark(cells, hx, hy, z, net, key):
        via, w, extra = key
        lim = ((via_diameter / 2 + clearance) if via else (w / 2 + clearance)) + extra + w_of(net) / 2
        n = math.ceil(lim / GRID) + 1; layer = cells[z]
        for gx in range(hx // 2 - n, hx // 2 + n + 2):
            for gy in range(hy // 2 - n, hy // 2 + n + 2):
                d = math.hypot(gx - hx // 2, gy - hy // 2) * GRID if hx % 2 == 0 and hy % 2 == 0 else math.hypot(2 * gx - hx, 2 * gy - hy) * GRID / 2
                if d < lim:
                    layer.setdefault((gx, gy), set()).add(net)

    def zone(via, w, extra):
        key = (via, w, extra)
        cells = influence.get(key)
        if cells is None:
            cells = influence[key] = [{}, {}]
            for hx, hy, z, net in committed:
                mark(cells, hx, hy, z, net, key)
        return cells

    def commit(hx, hy, z, net):
        committed.append((hx, hy, z, net))
        for key, cells in influence.items():
            mark(cells, hx, hy, z, net, key)

    def node(p):
        return (round(p['x'] / GRID), round(p['y'] / GRID), 0)

    def static_blocked(x, y, z, net, via, r):
        xx, yy = x * GRID, y * GRID
        if min(xx, yy, width - xx, height - yy) < edge + r:
            return True
        for k in keepouts:  # footprint and board rule areas: tracks (and vias) stay out, with clearance
            if k['flags'] & (2 if via else 1) and (k['layers'] & (3 if via else 1 << z)) and \
                    k['x1'] - r < xx < k['x2'] + r and k['y1'] - r < yy < k['y2'] + r:
                return True
        for p in buckets.get((math.floor(xx / PAD_BUCKET), math.floor(yy / PAD_BUCKET)), ()):
            # A via never lands on a drilled pad, even of its own net: the holes would overlap.
            if via and p['through'] and abs(xx - p['x']) <= p['w'] / 2 + r and abs(yy - p['y']) <= p['h'] / 2 + r:
                return True
            if p['net'] == net or (z == 1 and not p['through']):
                continue
            if abs(xx - p['x']) <= p['w'] / 2 + r and abs(yy - p['y']) <= p['h'] / 2 + r:
                return True
        return False

    def clash(x, y, z, net, via, w, extra):
        """Foreign copper within the probe's clearance plus `extra` mm: 'trace' (routed paths), 'via', or None."""
        nets = zone(via, w, extra)[z].get((x, y))
        if nets and (len(nets) > 1 or net not in nets):
            return 'trace'
        xx, yy = x * GRID, y * GRID; r = ((via_diameter / 2 + clearance) if via else (w / 2 + clearance)) + extra
        for vx, vy, vnet in vias:
            if vnet != net and math.hypot(xx - vx, yy - vy) < r + via_diameter / 2:
                return 'via'
        return None

    def blocked(x, y, z, net, via=False, count=False, w=None):
        w = w_of(net) if w is None else w
        key = (x, y, z, net, via, w, 0)
        hit = static.get(key)
        if hit is None:
            hit = static[key] = static_blocked(x, y, z, net, via, (via_diameter / 2 + clearance) if via else (w / 2 + clearance))
        if hit:
            return True
        hit = clash(x, y, z, net, via, w, 0.0)
        if hit == 'trace' and count:   # congestion: routed copper in the way (not vias)
            blocked_at[x, y, z] = blocked_at.get((x, y, z), 0) + 1
        return hit is not None

    def near(x, y, z, net, w):
        """Within the margin band (and not blocked): clearance is met, but by less than `margin`."""
        key = (x, y, z, net, False, w, 1)
        hit = static.get(key)
        if hit is None:
            hit = static[key] = static_blocked(x, y, z, net, False, w / 2 + clearance + margin)
        return hit or clash(x, y, z, net, False, w, margin) is not None

    def reachable(start, goal, net, w_at):
        """Breadth-first flood over the same moves without direction: octilinear steps never cut a blocked corner and a
        path never needs a turn sharper than 90 degrees, so this is exactly the octilinear search's reachability. It
        proves 'no path' cheaply where the direction-aware search would exhaust the board."""
        seen = {start}; todo = [(0, start)]     # goal-directed: finds a reachable goal quickly, floods only when none
        while todo:
            _, u = heapq.heappop(todo)
            if u == goal:
                return True
            x, y, z = u
            for xx, yy, zz in ((x + 1, y, z), (x - 1, y, z), (x, y + 1, z), (x, y - 1, z), (x, y, 1 - z)):
                v = (xx, yy, zz)
                if v in seen or not (0 <= xx <= nx and 0 <= yy <= ny):
                    continue
                if zz != z:
                    if blocked(xx, yy, zz, net, w=w_at(xx, yy)) or blocked(x, y, z, net, True) or blocked(x, y, zz, net, True):
                        continue
                elif blocked(xx, yy, zz, net, w=w_at(xx, yy)):
                    continue
                seen.add(v); heapq.heappush(todo, (abs(xx - goal[0]) + abs(yy - goal[1]) + (VIA_COST if zz != goal[2] else 0), v))
        return False

    nets = list(order) if order is not None else sorted({p['net'] for p in pads} - {'~'})  # '~': no-connect pads
    per_net = []; unrouted = []; started = time.perf_counter()
    for net in nets:
        t0 = time.perf_counter(); wn = w_of(net)
        # Stacked pads of one pin (e.g. a thermal pad's copies) are joined by the footprint; route one of them.
        targets = list({(p['ref'], p['pin']): p for p in reversed([p for p in pads if p['net'] == net])}.values())[::-1]
        stat = dict(net=net, connections=max(len(targets) - 1, 0), routed=0, explored=0, length_mm=0.0, bends=0,
                    vias=0, failures=[], width_mm=wn)
        if len(targets) < 2:
            per_net.append(dict(stat, time_s=0.0)); continue
        joined = [targets.pop(0)]
        while targets:
            _, i, j = min((abs(a['x'] - b['x']) + abs(a['y'] - b['y']), i, j) for i, a in enumerate(joined) for j, b in enumerate(targets))
            a, b = joined[i], targets.pop(j); start, goal = node(a), node(b)
            where = f"{a['ref']}.{a['pin']} -> {b['ref']}.{b['pin']}"
            # Neck-down: within NECK_ZONE of an end pad narrower than the net's width, the width that fits the pad,
            # never below what the net needs (min_widths) or the rule minimum.
            necks = []
            for p in (a, b):
                fit = min(p['w'], p['h'])
                if wn > fit:
                    necks.append((p, min(wn, max(trace_width, min_widths.get(net, trace_width), fit))))

            def w_at(x, y):
                xx, yy = x * GRID, y * GRID
                for p, w in necks:
                    if abs(xx - p['x']) <= p['w'] / 2 + NECK_ZONE and abs(yy - p['y']) <= p['h'] / 2 + NECK_ZONE:
                        return w
                return wn
            if blocked(*start, net, w=w_at(*start[:2])) or blocked(*goal, net, w=w_at(*goal[:2])):
                stat['failures'].append('pad_escape'); unrouted.append(dict(net=net, connection=where, reason='pad_escape'))
                continue
            if octo and not reachable(start, goal, net, w_at):
                stat['failures'].append('no_path'); unrouted.append(dict(net=net, connection=where, reason='no_path'))
                continue
            s0 = start + (8,)
            heap = [(0, 0, s0)]; cost = {s0: 0}; prev = {}; came = {}; found = None; reason = 'no_path'
            while heap:
                _, g, u = heapq.heappop(heap)
                if g != cost[u]:
                    continue
                if u[:3] == goal:
                    found = u; break
                if len(cost) > max_states:
                    reason = 'state_limit'; break
                x, y, z, _ = u; d = came.get(u, 8)
                moves = [(k, (x + DIRS[k][0], y + DIRS[k][1], z)) for k in (range(8) if octo else MANHATTAN_MOVES)] + [(None, (x, y, 1 - z))]
                for k, (xx, yy, zz) in moves:
                    if not (0 <= xx <= nx and 0 <= yy <= ny):
                        continue
                    if k is None:
                        wv = w_at(xx, yy)
                        if blocked(xx, yy, zz, net, count=True, w=wv) or blocked(x, y, z, net, True) or blocked(x, y, zz, net, True):
                            continue
                        step, nd = VIA_COST, 8
                    else:
                        turn = 0 if d == 8 or not octo else min(abs(k - d), 8 - abs(k - d))
                        if turn > 2:          # sharper than 90 degrees: an acute corner (or a reversal)
                            continue
                        wv = w_at(xx, yy)
                        if blocked(xx, yy, zz, net, count=True, w=wv):
                            continue
                        if k % 2 and (blocked(x + DIRS[k][0], y, z, net, w=wv) or blocked(x, y + DIRS[k][1], z, net, w=wv)):
                            continue          # a diagonal step never cuts past a blocked corner
                        step = (math.sqrt(2) if k % 2 else 1) + (BEND_COST.get(turn, 0) if octo else 0)
                        nd = k if octo else 8
                    if margin > 0 and near(xx, yy, zz, net, wv):
                        step += MARGIN_COST
                    v = (xx, yy, zz, 8); ng = g + step
                    if ng < cost.get(v, 1e30):
                        cost[v] = ng; prev[v] = u; came[v] = nd
                        ddx, ddy = abs(xx - goal[0]), abs(yy - goal[1])
                        h = (max(ddx, ddy) - min(ddx, ddy) + math.sqrt(2) * min(ddx, ddy) if octo else ddx + ddy) + (VIA_COST if zz != goal[2] else 0)
                        heapq.heappush(heap, (ng + h, ng, v))
            stat['explored'] += len(cost)
            if found is None:
                stat['failures'].append(reason); unrouted.append(dict(net=net, connection=where, reason=reason))
                continue
            path = [found]
            while path[-1] != s0:
                path.append(prev[path[-1]])
            path = [s[:3] for s in reversed(path)]
            ws = [w_at(x, y) for x, y, _ in path]
            for (x, y, z), (x2, y2, z2) in zip(path, path[1:] + path[-1:]):
                if (x, y) not in occupied[z]:
                    occupied[z][x, y] = net; commit(2 * x, 2 * y, z, net)
                if z == z2 and x != x2 and y != y2 and (x + x2, y + y2) not in mids[z]:
                    mids[z][x + x2, y + y2] = net; commit(x + x2, y + y2, z, net)
            # Compress collinear grid runs of one width to avoid thousands of segments.
            keep = [0]
            for k in range(1, len(path) - 1):
                d1 = tuple(path[k][q] - path[k - 1][q] for q in range(3)); d2 = tuple(path[k + 1][q] - path[k][q] for q in range(3))
                if d1 != d2 or ws[k] != ws[k - 1] or ws[k] != ws[k + 1]:
                    keep.append(k)
            if len(path) > 1:
                keep.append(len(path) - 1)
            points = [path[k] for k in keep]
            new = [(net, a['x'], a['y'], start[0] * GRID, start[1] * GRID, ws[0], 0)]
            for k1, k2 in zip(keep, keep[1:]):
                u, v = path[k1], path[k2]
                if u[2] != v[2]:
                    vias.append((u[0] * GRID, u[1] * GRID, net)); stat['vias'] += 1
                else:
                    new.append((net, u[0] * GRID, u[1] * GRID, v[0] * GRID, v[1] * GRID, min(ws[k1], ws[k2]), 0 if u[2] == 0 else 2))
            new.append((net, goal[0] * GRID, goal[1] * GRID, b['x'], b['y'], ws[-1], 0))
            stat['bends'] += sum(1 for k in range(1, len(points) - 1) if points[k - 1][2] == points[k][2] == points[k + 1][2]
                                 and tuple(points[k][q] - points[k - 1][q] for q in (0, 1)) != tuple(points[k + 1][q] - points[k][q] for q in (0, 1)))
            stat['length_mm'] += sum(math.hypot(t[1] - t[3], t[2] - t[4]) for t in new)
            traces.extend(new); stat['routed'] += 1; joined.append(b)
        per_net.append(dict(stat, time_s=time.perf_counter() - t0))
    traces = [t for t in traces if math.hypot(t[1] - t[3], t[2] - t[4]) > 1e-9]; vias = sorted(set(vias))
    hot = sorted(((c, x, y, z) for (x, y, z), c in blocked_at.items()), key=lambda h: (-h[0], h[1], h[2], h[3]))[:10]
    stats = dict(nets=per_net, unrouted=unrouted, connections=sum(n['connections'] for n in per_net),
                 routed_connections=sum(n['routed'] for n in per_net), explored=sum(n['explored'] for n in per_net),
                 length_mm=sum(n['length_mm'] for n in per_net), vias=len(vias), layer_changes=len(vias),
                 bends=sum(n['bends'] for n in per_net), objects=dict(traces=len(traces), vias=len(vias)),
                 congestion=dict(occupied_cells=sum(len(o) for o in occupied), blocked_expansions=sum(blocked_at.values()),
                                 hotspots=[dict(x_mm=x * GRID, y_mm=y * GRID, layer='F.Cu' if z == 0 else 'B.Cu', blocked=c) for c, x, y, z in hot]),
                 order=nets, trace_width_mm=trace_width, style=style, widths=dict(sorted(widths.items())), margin_mm=margin,
                 time_s=time.perf_counter() - started)
    return traces, vias, stats


def route_with_ripup(pads, width, height, trace_width, clearance, edge, via_diameter=.6, keepouts=(), passes=4, **options):
    """Deterministic rip-up and reroute: when connections fail, everything is ripped up and the failed nets
    are routed first in the next pass. Returns the best pass (fewest unrouted, then shortest) with the pass
    history; never claims more than that pass achieved. options: route()'s style, widths, min_widths, margin."""
    order = sorted({p['net'] for p in pads} - {'~'}); best = None; history = []; seen = set()
    for attempt in range(passes):
        traces, vias, stats = route(pads, width, height, trace_width, clearance, edge, via_diameter, keepouts, order, **options)
        history.append(dict(order=list(order), unrouted=len(stats['unrouted']), length_mm=stats['length_mm']))
        key = (len(stats['unrouted']), stats['length_mm'], attempt)
        if best is None or key < best[0]:
            best = (key, traces, vias, stats)
        if not stats['unrouted']:
            break
        failed = []
        for u in stats['unrouted']:
            if u['net'] not in failed:
                failed.append(u['net'])
        seen.add(tuple(order)); order = failed + [n for n in order if n not in failed]
        if tuple(order) in seen:
            break
    _, traces, vias, stats = best
    stats = dict(stats, ripup_passes=history)
    return traces, vias, stats


# --------------------------------------------------------------------------- footprints and legality
class Footprints:
    """Local (component-frame) pads, body and keepouts per component and rotation, from the real footprint."""
    def __init__(self, components, fpdir):
        self.components = {c['ref']: c for c in components}; self.fpdir = fpdir; self.cache = {}

    def local(self, ref, rotation):
        import copper
        key = (ref, rotation % 360)
        if key not in self.cache:
            c = dict(self.components[ref], x=0.0, y=0.0, rotation=float(rotation % 360))
            netids = {n: 1 for n in c['pins'].values()}
            _, pads, geo, keepouts = copper.footprint_geometry(c, self.fpdir, netids, 'layout')
            body = next(g for g in geo if str(g[0]) == 'body')
            cx, cy, w, h = (float(v) for v in body[2:6])
            rects = [pad_rect(p) for p in pads]
            env = (min(r[0] for r in rects), min(r[1] for r in rects), max(r[2] for r in rects), max(r[3] for r in rects)) if rects else None
            self.cache[key] = dict(pads=pads, body=(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2), keepouts=keepouts,
                                   silk=self.silk(c), pad_rects=rects, pad_env=env)
        return self.cache[key]

    def silk(self, c):
        """Conservative envelope of the footprint's F.SilkS: its graphics plus the visible reference text, whose
        box is over-estimated (each character as wide as the text height, on both axes). Placement keeps it off
        other parts' silkscreen and pads; KiCad DRC remains the judge of silkscreen."""
        import copper
        from hwc_kicad import direct, one, text_at
        lib, name = copper.safe_lib_id(c['footprint']); fp = copper.load(self.fpdir / f'{lib}.pretty' / f'{name}.kicad_mod')
        pts = []
        for obj in fp:
            if not isinstance(obj, list) or text_at(obj, 'layer') != 'F.SilkS':
                continue
            if str(obj[0]) in ('property', 'fp_text'):
                hidden = one(obj, 'hide') or one(one(obj, 'effects') or [None], 'hide')
                if hidden or str(obj[1]) not in ('Reference', 'reference'):
                    continue
                at = one(obj, 'at'); size = one(one(one(obj, 'effects') or [None], 'font') or [None], 'size')
                height = float(size[1]) if size else 1.0; half = (len(c['ref']) * height + .3) / 2
                for dx in (-half, half):
                    for dy in (-half, half):
                        pts.append(copper.rotate(float(at[1]) + dx, float(at[2]) + dy, c['rotation']))
                continue
            width = float(text_at(one(obj, 'stroke') or [None], 'width') or .12) / 2
            coords = [one(obj, k) for k in ('start', 'end', 'center', 'mid')] + list(direct(one(obj, 'pts') or [None], 'xy'))
            for xy in coords:
                if xy:
                    for dx in (-width, width):
                        for dy in (-width, width):
                            pts.append(copper.rotate(float(xy[1]) + dx, float(xy[2]) + dy, c['rotation']))
        if not pts:
            return None
        return (min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts), max(p[1] for p in pts))

    def placed(self, ref, at):
        x, y, rot = at; loc = self.local(ref, rot)
        b = loc['body']
        silk = loc['silk']
        return dict(body=(b[0] + x, b[1] + y, b[2] + x, b[3] + y),
                    silk=None if silk is None else (silk[0] + x, silk[1] + y, silk[2] + x, silk[3] + y),
                    pads=[dict(p, x=p['x'] + x, y=p['y'] + y) for p in loc['pads']],
                    pad_rects=[(r[0] + x, r[1] + y, r[2] + x, r[3] + y) for r in loc['pad_rects']], pad_env=None if loc['pad_env'] is None else
                    (loc['pad_env'][0] + x, loc['pad_env'][1] + y, loc['pad_env'][2] + x, loc['pad_env'][3] + y),
                    keepouts=[dict(k, x1=k['x1'] + x, y1=k['y1'] + y, x2=k['x2'] + x, y2=k['y2'] + y) for k in loc['keepouts']])


def overlap(a, b, gap=0.0):
    """True when rectangles a and b (x1, y1, x2, y2) are closer than gap (touching counts)."""
    return a[0] < b[2] + gap and b[0] < a[2] + gap and a[1] < b[3] + gap and b[1] < a[3] + gap


def pad_rect(p):
    return (p['x'] - p['w'] / 2, p['y'] - p['h'] / 2, p['x'] + p['w'] / 2, p['y'] + p['h'] / 2)


class Board:
    """Hard placement constraints: outline, copper-to-edge, courtyard gap, board and footprint keepouts."""
    def __init__(self, width, height, edge, gap, keepouts):
        self.width, self.height, self.edge, self.gap, self.keepouts = width, height, edge, gap + 1e-6, list(keepouts)

    def legal(self, ref, shape, others):
        """Exactly `not violations(...)`, stopping at the first violation; pad tests are skipped when the pads'
        bounding envelope cannot touch the other object (no pad can touch what its envelope does not)."""
        b = shape['body']; eps = 1e-6; rects = shape['pad_rects']; env = shape['pad_env']
        if b[0] < -eps or b[1] < -eps or b[2] > self.width + eps or b[3] > self.height + eps:
            return False
        if env is not None and min(env[0], env[1], self.width - env[2], self.height - env[3]) < self.edge:
            return False

        def pads_touch(rs, e, area, gap):
            return e is not None and overlap(e, area, gap) and any(overlap(r, area, gap) for r in rs)

        for k in self.keepouts:
            area = (k['x1'], k['y1'], k['x2'], k['y2'])
            if (k['flags'] & 8 and overlap(b, area, 1e-6)) or (k['flags'] & 4 and pads_touch(rects, env, area, 1e-6)):
                return False
        for other, s in others.items():
            if other == ref:
                continue
            if overlap(b, s['body'], self.gap):
                return False
            if shape['silk'] and ((s['silk'] and overlap(shape['silk'], s['silk'])) or pads_touch(s['pad_rects'], s['pad_env'], shape['silk'], 0.0)):
                return False
            if s['silk'] and pads_touch(rects, env, s['silk'], 0.0):
                return False
            for k in s['keepouts']:
                area = (k['x1'], k['y1'], k['x2'], k['y2'])
                if (k['flags'] & 8 and overlap(b, area, 1e-6)) or (k['flags'] & 4 and pads_touch(rects, env, area, 1e-6)):
                    return False
            for k in shape['keepouts']:
                area = (k['x1'], k['y1'], k['x2'], k['y2'])
                if (k['flags'] & 8 and overlap(s['body'], area, 1e-6)) or (k['flags'] & 4 and pads_touch(s['pad_rects'], s['pad_env'], area, 1e-6)):
                    return False
        return True

    def violations(self, ref, shape, others):
        """Reasons `shape` (Footprints.placed) cannot be where it is, given `others` {ref: shape}."""
        out = []; b = shape['body']; eps = 1e-6
        if b[0] < -eps or b[1] < -eps or b[2] > self.width + eps or b[3] > self.height + eps:
            out.append('outside the board outline')
        for p in shape['pads']:
            r = pad_rect(p)
            if min(r[0], r[1], self.width - r[2], self.height - r[3]) < self.edge:
                out.append('pad closer to the board edge than the profile allows'); break
        own = shape['keepouts']
        for k in self.keepouts:
            if (k['flags'] & 8 and overlap(b, (k['x1'], k['y1'], k['x2'], k['y2']), 1e-6)) or \
                    (k['flags'] & 4 and any(overlap(pad_rect(p), (k['x1'], k['y1'], k['x2'], k['y2']), 1e-6) for p in shape['pads'])):
                out.append('inside a board keepout'); break
        for other, s in others.items():
            if other == ref:
                continue
            if overlap(b, s['body'], self.gap):
                out.append(f'courtyard within the courtyard gap of {other}')
            if shape['silk'] and ((s['silk'] and overlap(shape['silk'], s['silk'])) or any(overlap(shape['silk'], pad_rect(p)) for p in s['pads'])):
                out.append(f'silkscreen over the silkscreen or pads of {other}')
            if s['silk'] and any(overlap(s['silk'], pad_rect(p)) for p in shape['pads']):
                out.append(f'pads under the silkscreen of {other}')
            for k in s['keepouts']:
                if (k['flags'] & 8 and overlap(b, (k['x1'], k['y1'], k['x2'], k['y2']), 1e-6)) or \
                        (k['flags'] & 4 and any(overlap(pad_rect(p), (k['x1'], k['y1'], k['x2'], k['y2']), 1e-6) for p in shape['pads'])):
                    out.append(f'inside the keepout of {other}')
            for k in own:
                if (k['flags'] & 8 and overlap(s['body'], (k['x1'], k['y1'], k['x2'], k['y2']), 1e-6)) or \
                        (k['flags'] & 4 and any(overlap(pad_rect(p), (k['x1'], k['y1'], k['x2'], k['y2']), 1e-6) for p in s['pads'])):
                    out.append(f'{other} inside this footprint\'s keepout')
        return out


def hpwl(nets, shapes):
    """Half-perimeter wirelength over nets, from pad centres (a routing-length proxy, not a length)."""
    total = 0.0
    for net in nets:
        xs, ys = [], []
        for s in shapes.values():
            for p in s['pads']:
                if p['net'] == net:
                    xs.append(p['x']); ys.append(p['y'])
        if len(xs) > 1:
            total += max(xs) - min(xs) + max(ys) - min(ys)
    return total


# --------------------------------------------------------------------------- objectives
def soft_placement_terms(problem, shapes):
    """Placement soft terms: occupied area, decoupling distance, keepout-to-edge distance."""
    bodies = [s['body'] for s in shapes.values()]
    area = (max(b[2] for b in bodies) - min(b[0] for b in bodies)) * (max(b[3] for b in bodies) - min(b[1] for b in bodies)) if bodies else 0.0
    decoupling = 0.0
    for cap, supply, ground in problem['decoupling']:
        if cap in shapes:
            cp = [p for p in shapes[cap]['pads'] if p['net'] == supply]; mp = [p for s in shapes.values() for p in s['pads'] if p['net'] == supply and p['ref'] == problem['mcu_of'][(cap, supply)]]
            if cp and mp:
                decoupling += min(math.hypot(a['x'] - b['x'], a['y'] - b['y']) for a in cp for b in mp)
    edge = 0.0
    for s in shapes.values():
        for k in s['keepouts']:  # a footprint keepout (an antenna) is preferred against the board edge
            edge += max(0.0, min(k['x1'], k['y1'], problem['width'] - k['x2'], problem['height'] - k['y2']))
    return dict(placement_area_mm2=area, decoupling_mm=decoupling, keepout_edge_mm=edge)


def soft_score(terms):
    return sum(WEIGHTS[k] * terms.get(k, 0.0) for k in SOFT_TERMS)


def candidate_key(c):
    """Lexicographic: no soft improvement can trade away a hard violation or an unrouted connection."""
    return (0 if c['judged'] else 1, c['hard_violations'], c['unrouted'], round(c['soft_score'], 9), c['index'])


# --------------------------------------------------------------------------- placement
def problem_from(tree, fpdir):
    """Everything the optimizer needs from a .copper project tree."""
    import copper
    from hwc_kicad import one, direct
    components = [c for c in copper.component_data(tree) if c['kind'] not in ('V', 'I')]
    dims = list(map(float, (one(tree, 'board') or [None, 40, 30, 1.6, .035, 2])[1:]))
    rule = lambda key, default: copper.rule(tree, key, default)
    fixed = {str(c[1]) for c in direct(one(tree, 'components'), 'component') if one(c, 'fixed')}
    areas = [dict(x1=float(a[1]), y1=float(a[2]), x2=float(a[3]), y2=float(a[4]), layers=int(float(a[5])), flags=int(float(a[6])))
             for a in direct(one(tree, 'keepouts') or [None], 'area')]
    decoupling, mcu_of = [], {}
    for u in components:
        mcu = one(u['part'], 'mcu')
        if not mcu:
            continue
        rails = [u['pins'].get(str(p)) for p in (one(mcu, 'supply') or [None])[1:]]
        grounds = {u['pins'].get(str(p)) for p in (one(mcu, 'ground') or [None])[1:]}
        for c in components:
            if c['kind'] == 'C':
                nets = set(c['pins'].values())
                for rail in rails:
                    if rail in nets and nets & grounds:
                        decoupling.append((c['ref'], rail, next(iter(nets & grounds)))); mcu_of[(c['ref'], rail)] = u['ref']
    nets = sorted({n for c in components for n in c['pins'].values()})
    placed = {str(c[1]) for c in direct(one(tree, 'components'), 'component') if one(c, 'at')}
    return dict(components=components, width=dims[0], height=dims[1], fixed=fixed, keepouts=areas, nets=nets,
                given={c['ref']: (c['x'], c['y'], c['rotation']) for c in components if c['ref'] in placed},
                edge=rule('edge_mm', .3), gap=rule('courtyard_gap_mm', .25), trace_width=rule('trace_width_mm', .2),
                density=rule('max_current_density_a_mm2', 20), copper=dims[3],
                clearance=rule('clearance_mm', .2), via=rule('via_diameter_mm', .6), decoupling=decoupling, mcu_of=mcu_of,
                footprints=Footprints(components, fpdir))


def initial_placement(problem, step=1.0):
    """Deterministic constructive placement. Fixed parts stay; parts with keepouts go first (preferring their
    keepout against an edge); the rest follow by connectivity to what is placed, each at the legal grid
    position and rotation with the least half-perimeter wirelength. Returns (placement, failures)."""
    fp = problem['footprints']; board = Board(problem['width'], problem['height'], problem['edge'], problem['gap'], problem['keepouts'])
    placement = {r: problem['given'][r] for r in sorted(problem['fixed'])}
    shapes = {r: fp.placed(r, a) for r, a in placement.items()}
    pending = sorted(c['ref'] for c in problem['components'] if c['ref'] not in placement)
    nets_of = {c['ref']: set(c['pins'].values()) for c in problem['components']}
    failures = []
    while pending:
        def priority(r):
            has_keepout = bool(fp.local(r, 0)['keepouts'])
            shared = sum(1 for o in placement for n in nets_of[r] if n in nets_of[o])
            return (not has_keepout, -shared, -len(nets_of[r]), r)
        ref = min(pending, key=priority); pending.remove(ref)
        best = None
        for rot in (0, 90, 180, 270):
            loc = fp.local(ref, rot); b = loc['body']
            xs = [i * step for i in range(int(problem['width'] / step) + 1)]; ys = [j * step for j in range(int(problem['height'] / step) + 1)]
            for y in ys:
                for x in xs:
                    if b[0] + x < 0 or b[1] + y < 0 or b[2] + x > problem['width'] or b[3] + y > problem['height']:
                        continue
                    shape = fp.placed(ref, (x, y, rot))
                    if not board.legal(ref, shape, shapes):
                        continue
                    trial = dict(shapes, **{ref: shape})
                    cost = hpwl(nets_of[ref], trial) + WEIGHTS['keepout_edge_mm'] * soft_placement_terms(problem, {ref: shape})['keepout_edge_mm']
                    if not placement:  # the first part sits near the board centre
                        cost += abs(x - problem['width'] / 2) + abs(y - problem['height'] / 2)
                    key = (round(cost, 9), rot, y, x)
                    if best is None or key < best[0]:
                        best = (key, (x, y, rot), shape)
        if best is None:
            failures.append(f'{ref}: no legal position on the board'); continue
        placement[ref] = best[1]; shapes[ref] = best[2]
    return placement, failures


def anneal(problem, placement, seed, iterations=1500, step=1.0):
    """Deterministic simulated annealing over legal placements (seeded; fixed iteration count). Moves: shift,
    rotate, swap. Illegal states are rejected, never penalised. Cost: half-perimeter wirelength plus the
    weighted placement soft terms. Returns the best legal placement seen."""
    fp = problem['footprints']; board = Board(problem['width'], problem['height'], problem['edge'], problem['gap'], problem['keepouts'])
    rng = random.Random(seed); movable = sorted(r for r in placement if r not in problem['fixed'])
    shapes = {r: fp.placed(r, a) for r, a in placement.items()}; current = dict(placement)

    def cost(sh):
        terms = soft_placement_terms(problem, sh)
        return hpwl(problem['nets'], sh) + sum(WEIGHTS[k] * terms[k] for k in ('placement_area_mm2', 'decoupling_mm', 'keepout_edge_mm'))

    now = cost(shapes); best = (now, dict(current)); temperature = max(now * .05, 1.0)
    for i in range(iterations if movable else 0):
        kind = rng.random(); trial = dict(current)
        a = rng.choice(movable)
        if kind < .6:
            span = max(1, int(round((1 - i / iterations) * 8)))
            x, y, r = current[a]; trial[a] = (x + rng.randint(-span, span) * step, y + rng.randint(-span, span) * step, r)
        elif kind < .8:
            x, y, r = current[a]; trial[a] = (x, y, (r + 90) % 360)
        else:
            b = rng.choice(movable)
            if a == b:
                continue
            trial[a] = (current[b][0], current[b][1], current[a][2]); trial[b] = (current[a][0], current[a][1], current[b][2])
        changed = [r for r in trial if trial[r] != current[r]]
        new_shapes = dict(shapes)
        for r in changed:
            new_shapes[r] = fp.placed(r, trial[r])
        if not all(board.legal(r, new_shapes[r], new_shapes) for r in changed):
            continue
        new = cost(new_shapes); delta = new - now
        if delta <= 0 or rng.random() < math.exp(-delta / temperature):
            current, shapes, now = trial, new_shapes, new
            if now < best[0] - 1e-12:
                best = (now, dict(current))
        temperature *= .997
    return best[1]


# --------------------------------------------------------------------------- evaluation
OCTILINEAR_MARGIN = .1   # mm beyond the clearance that the octilinear router prefers to keep from foreign copper


def current_widths(report, trace_width, density, copper_mm):
    """{net: the narrowest trace (0.05 mm steps, never below trace_width) whose copper area keeps the net's DC current
    bound within the current-density budget}. Nets without a bound are charged the whole-circuit current, as
    copper-core does."""
    circuit = report.get('circuit_current_a') or 0.0; out = {}
    for n in report.get('net_current_bounds', []):
        current = n['bound_a'] if n['bound_a'] is not None else circuit
        need = math.ceil(current / (density * copper_mm) / .05 - 1e-9) * .05
        if need > trace_width + 1e-9:
            out[n['name']] = round(need, 6)
    return out


def routing_options(tree, report=None):
    """The router options a project records in (routing ...): style, per-net widths, and the neck-down floor per
    net from copper-core's current bounds (report: copper-core's analysis of this project). Build and optimizer
    both use this, so a build reproduces the geometry its candidate was proven with."""
    import copper
    from hwc_kicad import one, direct
    node = one(tree, 'routing')
    style = copper.text_at(node, 'style', default='manhattan') if node else 'manhattan'
    widths = {str(w[1]): float(w[2]) for w in direct(node, 'width')} if node else {}
    dims = list(map(float, (one(tree, 'board') or [None, 40, 30, 1.6, .035, 2])[1:]))
    floor = current_widths(report, copper.rule(tree, 'trace_width_mm', .2), copper.rule(tree, 'max_current_density_a_mm2', 20), dims[3]) if report else {}
    return dict(style=style, widths=widths, min_widths={n: min(floor[n], widths[n]) for n in widths if n in floor},
                margin=OCTILINEAR_MARGIN if style == 'octilinear' else 0.0)


def routing_node(style, widths):
    import copper
    from hwc_kicad import Atom as A
    return copper.sx('routing', copper.sx('style', A(style)), *[copper.sx('width', A(n), copper.number(w)) for n, w in sorted(widths.items())])


def evaluate(tree, problem, placement, fpdir, core, index, label, max_width=3.0, width_rounds=3, baseline=False, style='manhattan'):
    """Route a placement and re-check it with copper-core. Raises nothing for a bad board: a candidate that
    cannot be judged (unsupported geometry) is marked judged=False. When copper-core reports a current-density or
    drop budget violation, widens only the nets whose traces violate it (each trace is charged its net's DC current
    bound, or the whole-circuit current when the net has none); a widened net necks down near smaller pads, never
    below the width its current needs. style: 'manhattan' or 'octilinear' (45-degree steps, no acute corners, a
    clearance margin preference).
    baseline=True reproduces the pre-optimizer pipeline: one routing pass in sorted net order at the profile
    width, no rip-up, no widening."""
    import copper
    shapes = {r: problem['footprints'].placed(r, a) for r, a in placement.items()}
    pads = [p for s in shapes.values() for p in s['pads']]; keepouts = problem['keepouts'] + [k for s in shapes.values() for k in s['keepouts']]
    widths, min_widths = {}, {}; rounds = []
    while True:
        router = route if baseline else route_with_ripup
        options = dict(style=style, widths=widths, min_widths=min_widths, margin=OCTILINEAR_MARGIN if style == 'octilinear' else 0.0)
        traces, vias, stats = router(pads, problem['width'], problem['height'], problem['trace_width'], problem['clearance'],
                                     problem['edge'], problem['via'], keepouts, **options)
        resolved = resolved_tree(tree, problem, placement, fpdir, traces, vias, routing_node(style, widths))
        report = copper.core_check(resolved, core)
        hard = [i for i in report['issues'] if i['severity'] == 'error' and i['rule'] not in copper.ELECTRICAL_RULES]
        budget = [i for i in hard if i['rule'] in ('current_density_budget', 'trace_drop_budget') and i['limit'] > 0]
        rounds.append(dict(trace_width_mm=max([problem['trace_width']] + list(widths.values())), widths=dict(sorted(widths.items())),
                           hard_violations=len(hard), unrouted=len(stats['unrouted'])))
        if baseline or not budget or len(rounds) > width_rounds:
            break
        floor = current_widths(report, problem['trace_width'], problem['density'], problem['copper'])
        factor = {}
        for i in budget:
            net = traces[int(i['object'].split(':')[1])][0]
            factor[net] = max(factor.get(net, 0), i['measured'] / i['limit'])
        wider = {}
        for net, f in sorted(factor.items()):
            w = widths.get(net, problem['trace_width'])
            wider[net] = max(round(math.ceil(w * f * 1.02 / .05 - 1e-9) * .05, 6), floor.get(net, 0))
        if any(w > max_width for w in wider.values()) or all(w <= widths.get(n, problem['trace_width']) for n, w in wider.items()):
            break
        widths = dict(widths, **wider); min_widths = {n: min(floor[n], widths[n]) for n in widths if n in floor}
    unjudged = [i for i in report['issues'] if i['rule'] in UNJUDGED]
    by_rule = {}
    for i in hard:
        by_rule[i['rule']] = by_rule.get(i['rule'], 0) + 1
    margin = dict(zip(('clearance_margin_mm', 'near_limit_pairs'), clearance_margins(resolved, problem['clearance'])))
    terms = dict(length_mm=stats['length_mm'], vias=stats['vias'], bends=stats['bends'], near_limit_pairs=margin['near_limit_pairs'],
                 blocked_expansions=stats['congestion']['blocked_expansions'], **soft_placement_terms(problem, shapes))
    return dict(index=index, label=label, style=style, placement={r: list(a) for r, a in sorted(placement.items())}, judged=not unjudged,
                hard_violations=len(hard), hard_by_rule=by_rule, unrouted=len(stats['unrouted']),
                feasible=not unjudged and not hard and not stats['unrouted'], soft=terms, soft_score=soft_score(terms),
                hpwl_mm=hpwl(problem['nets'], shapes), routing=stats, width_rounds=rounds, widths=dict(sorted(widths.items())),
                margin=margin,
                electrical={'errors': sum(1 for i in report['issues'] if i['severity'] == 'error' and i['rule'] in copper.ELECTRICAL_RULES)},
                findings_by_rule=_count(report['issues']), resolved=resolved)


def _seg_dist(a, b):
    """Distance between segments a and b, each ((x1, y1), (x2, y2))."""
    def point_seg(p, s):
        (x1, y1), (x2, y2) = s; dx, dy = x2 - x1, y2 - y1; n = dx * dx + dy * dy
        t = 0.0 if n == 0 else max(0.0, min(1.0, ((p[0] - x1) * dx + (p[1] - y1) * dy) / n))
        return math.hypot(p[0] - x1 - t * dx, p[1] - y1 - t * dy)

    def cross(o, p, q):
        return (p[0] - o[0]) * (q[1] - o[1]) - (p[1] - o[1]) * (q[0] - o[0])
    (p1, p2), (q1, q2) = a, b
    if cross(p1, p2, q1) * cross(p1, p2, q2) < 0 and cross(q1, q2, p1) * cross(q1, q2, p2) < 0:
        return 0.0
    return min(point_seg(p1, b), point_seg(p2, b), point_seg(q1, a), point_seg(q2, a))


def clearance_margins(resolved, clearance):
    """Measured margin to the clearance rule of the router's copper: for every pair of different-net objects on a
    shared layer where at least one is a trace or via, edge distance minus `clearance`. Pads are taken as their
    full rectangles (corner rounding ignored), so a margin is never overstated. Returns the minimum (None without
    such pairs) and how many pairs are within 0.05 mm of the limit."""
    from hwc_kicad import one, direct
    objs = []                                   # (net, layers, kind, shape)
    geo = one(resolved, 'geometry') or []
    for t in direct(geo, 'trace'):
        x1, y1, x2, y2, w, layer = map(float, t[2:8])
        objs.append((str(t[1]), {int(layer)}, 'seg', (((x1, y1), (x2, y2)), w / 2)))
    for v in direct(geo, 'via'):
        x, y, d = map(float, v[2:5])
        objs.append((str(v[1]), {0, 2}, 'seg', (((x, y), (x, y)), d / 2)))
    pads = []
    for p in direct(geo, 'pad'):
        x, y, dia, drill, layer, w, h = map(float, p[4:11])
        w, h = (w or dia), (h or dia)
        pads.append((str(p[3]), {0, 2} if drill > 0 else {int(layer)}, (x - w / 2, y - h / 2, x + w / 2, y + h / 2)))
    worst, near = None, 0

    def count(gap):
        nonlocal worst, near
        m = gap - clearance
        worst = m if worst is None else min(worst, m)
        near += m < .05
    for i, (na, la, _, (sa, ra)) in enumerate(objs):
        for nb, lb, _, (sb, rb) in objs[i + 1:]:
            if nb != na and la & lb:
                count(_seg_dist(sa, sb) - ra - rb)
        for nb, lb, (x0, y0, x1, y1) in pads:
            if nb == na or not la & lb:       # no-connect pads ('~') are copper of no net: always foreign
                continue
            if any(x0 <= px <= x1 and y0 <= py <= y1 for px, py in sa):
                count(-ra)
                continue
            edges = (((x0, y0), (x1, y0)), ((x1, y0), (x1, y1)), ((x1, y1), (x0, y1)), ((x0, y1), (x0, y0)))
            count(min(_seg_dist(sa, e) for e in edges) - ra)
    return (None if worst is None else round(worst, 6)), near


def _count(issues):
    out = {}
    for i in issues:
        k = f"{i['severity']}:{i['rule']}"; out[k] = out.get(k, 0) + 1
    return dict(sorted(out.items()))


def resolved_tree(tree, problem, placement, fpdir, traces, vias, routing=None):
    """The project with candidate placements, its (routing ...) record and geometry (pads, bodies, keepouts,
    traces, vias)."""
    import copper
    from hwc_kicad import one, direct, Atom as A
    t = copy.deepcopy(tree); comps = {str(c[1]): c for c in direct(one(t, 'components'), 'component')}
    for ref, (x, y, rot) in placement.items():
        c = comps[ref]; c[:] = [v for v in c if not (isinstance(v, list) and str(v[0]) == 'at')]
        c.append(copper.sx('at', copper.number(x), copper.number(y), copper.number(rot)))
    geometry = copper.sx('geometry')
    data = {c['ref']: c for c in copper.component_data(t)}
    for ref in sorted(placement):
        _, _, geo, _ = copper.footprint_geometry(data[ref], fpdir, {n: 1 for n in data[ref]['pins'].values()}, 'layout')
        geometry.extend(geo)
    for net, x1, y1, x2, y2, w, layer in traces:
        geometry.append(copper.sx('trace', A(net), *map(copper.number, [x1, y1, x2, y2, w, layer])))
    for x, y, net in vias:
        geometry.append(copper.sx('via', A(net), *map(copper.number, [x, y, problem['via'], copper.rule(t, 'drill_mm', .3)])))
    t[:] = [v for v in t if not (isinstance(v, list) and str(v[0]) in ('geometry', 'routing'))]
    if routing is not None:
        t.append(routing)
    t.append(geometry)
    return t


# --------------------------------------------------------------------------- staged optimizer
def optimize(tree, fpdir, core=None, seeds=(1, 2, 3), iterations=1500, styles=STYLES):
    """Staged, deterministic layout search. Candidates: the given placement (when every part has one), the
    constructive placement, and one annealed placement per seed; each is routed in every style (Manhattan and
    octilinear) with rip-up/reroute, its nets widened for their current budgets, and re-checked exactly. The best candidate is chosen lexicographically:
    judged, hard violations, unrouted connections, then soft score. Returns (best, candidates, problem)."""
    problem = problem_from(tree, fpdir); candidates = []
    starts = []
    if problem['components'] and all(c['ref'] in problem['given'] for c in problem['components']):
        starts.append(('given placement', dict(problem['given'])))
    initial, failures = initial_placement(problem)
    if not failures:
        starts.append(('constructive placement', initial))
        for seed in seeds:
            starts.append((f'annealed (seed {seed}, {iterations} iterations)', anneal(problem, initial, seed, iterations)))
    for label, placement in starts:
        for style in styles:
            candidates.append(evaluate(tree, problem, placement, fpdir, core, len(candidates),
                                       label if style == 'manhattan' else f'{label}, octilinear', style=style))
    best = min(candidates, key=candidate_key) if candidates else None
    return best, candidates, dict(problem, placement_failures=failures)


def second_judge(candidates, judge, limit=3):
    """KiCad DRC as the second judge. copper-core's ranking stands; the first `limit` feasible candidates in that
    order are built through the shipping export and checked by `judge(candidate)` -> {'status', 'message', ...}
    (KiCad 9.x DRC). The first that KiCad does not FAIL is chosen; every verdict is kept on its candidate. When all
    of them FAIL, the top candidate is returned with its FAIL: verification then reports it, nothing is hidden."""
    ranked = sorted((c for c in candidates if c['feasible']), key=candidate_key); verdicts = []
    for c in ranked[:limit]:
        c['kicad_drc'] = judge(c)
        verdicts.append(dict(label=c['label'], status=c['kicad_drc']['status'], message=c['kicad_drc'].get('message', '')))
        if c['kicad_drc']['status'] != 'FAIL':
            return c, verdicts
    return (ranked[0] if ranked else None), verdicts


def brief(c):
    """A candidate's report: feasibility, hard findings, every soft term and per-net routing effort."""
    if c is None:
        return None
    r = c['routing']
    return dict(label=c['label'], feasible=c['feasible'], judged=c['judged'], hard_violations=c['hard_violations'],
                hard_by_rule=c['hard_by_rule'], unrouted=c['unrouted'], soft_score=round(c['soft_score'], 3),
                length_mm=round(r['length_mm'], 3), vias=r['vias'], bends=r['bends'], explored=r['explored'],
                blocked_expansions=r['congestion']['blocked_expansions'], trace_width_mm=c['width_rounds'][-1]['trace_width_mm'],
                hpwl_mm=round(c['hpwl_mm'], 3), routing_time_s=round(r['time_s'], 3), objects=r['objects'],
                ripup_passes=len(r.get('ripup_passes', [])) or 1, unrouted_detail=r['unrouted'],
                findings=c['findings_by_rule'], soft={k: round(v, 3) for k, v in c['soft'].items()}, placement=c['placement'],
                width_rounds=c['width_rounds'], style=c['style'], widths=c['widths'], **c['margin'], congestion_hotspots=r['congestion']['hotspots'], electrical=c['electrical'], per_net=[dict(net=n['net'], connections=n['connections'], routed=n['routed'],
                explored=n['explored'], length_mm=round(n['length_mm'], 3), bends=n['bends'], vias=n['vias'],
                time_s=round(n['time_s'], 4), failures=n['failures']) for n in r['nets']])


def placed_project(best, problem):
    """The project with the chosen candidate's placements, its (routing (style ...) (width NET MM) ...) record and no
    geometry: a later build routes with the style and per-net copper the candidate was proven with, and copper-core
    checks every trace of a widened net against its width."""
    placed = copy.deepcopy(best['resolved'])
    placed[:] = [v for v in placed if not (isinstance(v, list) and str(v[0]) == 'geometry')]
    return placed


def baseline(tree, fpdir, core=None):
    """The pre-optimizer pipeline on the project's own placement, measured with the same exact check."""
    problem = problem_from(tree, fpdir)
    missing = [c['ref'] for c in problem['components'] if c['ref'] not in problem['given']]
    if missing:
        raise ValueError(f'baseline needs a placement for every part; missing: {missing}')
    return evaluate(tree, problem, dict(problem['given']), fpdir, core, 0, 'baseline (given placement, single pass)', baseline=True)
