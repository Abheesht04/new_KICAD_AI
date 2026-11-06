/* Exact nanometre geometry (src/g2_types.c): predicates and distance comparisons against hand-computed answers.
 * Every case is chosen so the exact answer is known: 3-4-5 triangles, axis-aligned boxes, unit squares. */
#include <assert.h>
#include <stdio.h>

#include "g2_types.h"

#define MM(v) ((G2Nm)((v) * 1000000.0))
#define P(x, y) ((G2Point){MM(x), MM(y)})

static G2Order dist(G2Shape a, G2Shape b, G2Nm threshold)
{
    G2Order o = G2_ORDER_EQUAL;
    assert(g2_compare_shape_distance(a, b, threshold, &o) == G2_OK);
    return o;
}

static G2Shape circle(G2Point c, G2Nm r)
{
    G2Shape s = {.kind = G2_SHAPE_CIRCLE};
    s.as.circle = (G2Circle){c, r};
    return s;
}

static G2Shape capsule(G2Point a, G2Point b, G2Nm r)
{
    G2Shape s = {.kind = G2_SHAPE_CAPSULE};
    s.as.capsule = (G2Capsule){a, b, r};
    return s;
}

static G2Shape segment(G2Point a, G2Point b)
{
    G2Shape s = {.kind = G2_SHAPE_SEGMENT};
    s.as.segment.a = a;
    s.as.segment.b = b;
    return s;
}

int main(void)
{
    G2Order o;
    uint64_t d2;
    bool flag;

    /* Points: a 3-4-5 triangle in mm; the exact distance equals the threshold. */
    assert(g2_distance_squared(P(0, 0), P(3, 4), &d2) == G2_OK && d2 == (uint64_t)MM(5) * (uint64_t)MM(5));
    assert(g2_compare_distance(P(0, 0), P(3, 4), MM(5), &o) == G2_OK && o == G2_ORDER_EQUAL);
    assert(g2_compare_distance(P(0, 0), P(3, 4), MM(5) + 1, &o) == G2_OK && o == G2_ORDER_LESS);
    assert(g2_compare_distance(P(0, 0), P(3, 4), MM(5) - 1, &o) == G2_OK && o == G2_ORDER_GREATER);

    /* Orientation and incidence. */
    assert(g2_orient2d(P(0, 0), P(1, 0), P(0, 1), &o) == G2_OK && o == G2_ORDER_GREATER); /* counter-clockwise */
    assert(g2_orient2d(P(0, 0), P(1, 0), P(2, 0), &o) == G2_OK && o == G2_ORDER_EQUAL);
    assert(g2_point_on_segment(P(1, 1), P(0, 0), P(2, 2), &flag) == G2_OK && flag);
    assert(g2_segment_intersects(P(0, 0), P(2, 2), P(0, 2), P(2, 0), &flag) == G2_OK && flag);
    assert(g2_segment_intersects(P(0, 0), P(1, 0), P(0, 1), P(1, 1), &flag) == G2_OK && !flag);

    /* Clearance between copper shapes: two 0.1 mm-radius capsules 0.5 mm apart (centre lines) have a 0.3 mm gap. */
    G2Shape a = capsule(P(0, 0), P(10, 0), MM(0.1)), b = capsule(P(0, 0.5), P(10, 0.5), MM(0.1));
    assert(dist(a, b, MM(0.3)) == G2_ORDER_EQUAL && dist(a, b, MM(0.2)) == G2_ORDER_GREATER && dist(a, b, MM(0.31)) == G2_ORDER_LESS);
    /* Circles 5 mm apart (3-4-5), radii 1 and 1.5: gap 2.5 mm. */
    assert(dist(circle(P(0, 0), MM(1)), circle(P(3, 4), MM(1.5)), MM(2.5)) == G2_ORDER_EQUAL);
    /* A segment crossing a circle: zero distance, and they intersect. */
    assert(dist(segment(P(-2, 0), P(2, 0)), circle(P(0, 0), MM(1)), 0) == G2_ORDER_EQUAL);
    assert(g2_shapes_intersect(segment(P(-2, 0), P(2, 0)), circle(P(0, 0), MM(1)), &flag) == G2_OK && flag);

    /* Polygon: the unit square contains its centre, has (1, 0.5) on its boundary and (2, 2) outside. */
    G2Point square[] = {P(0, 0), P(1, 0), P(1, 1), P(0, 1)};
    G2Polygon poly = {square, 4};
    G2Containment where;
    assert(g2_polygon_validate(poly) == G2_OK);
    assert(g2_polygon_locate_point(poly, P(0.5, 0.5), &where) == G2_OK && where == G2_CONTAINMENT_INSIDE);
    assert(g2_polygon_locate_point(poly, P(1, 0.5), &where) == G2_OK && where == G2_CONTAINMENT_BOUNDARY);
    assert(g2_polygon_locate_point(poly, P(2, 2), &where) == G2_OK && where == G2_CONTAINMENT_OUTSIDE);
    /* (4, 5) is 3-4-5 from the corner (1, 1): exactly 5 mm away. */
    assert(g2_compare_point_polygon_distance(P(4, 5), poly, MM(5), &o) == G2_OK && o == G2_ORDER_EQUAL);

    /* Rounded rectangle (a KiCad roundrect pad): core 2 x 1 mm, radius 0.25 mm; a point 0.75 mm above the core's
     * top edge is 0.5 mm from the pad. */
    G2RoundedRect pad = {{P(-1, -0.5), P(1, 0.5)}, MM(0.25)};
    assert(g2_rounded_rect_validate(pad) == G2_OK);
    assert(g2_compare_point_rounded_rect_distance(P(0, 1.25), pad, MM(0.5), &o) == G2_OK && o == G2_ORDER_EQUAL);

    /* Arc: a quarter circle of radius 1 mm from (1, 0) to (0, 1). Its box is conservative by design (the whole
     * circle's box, radius rounded up): it must contain the arc, never cut it. Direction is honoured exactly. */
    G2Arc arc = {P(0, 0), P(1, 0), P(0, 1), G2_ARC_CCW, false};
    G2Aabb box;
    assert(g2_arc_validate(arc) == G2_OK && g2_arc_aabb(arc, &box) == G2_OK);
    assert(box.min.x <= 0 && box.min.y <= 0 && box.max.x >= MM(1) && box.max.y >= MM(1));
    assert(box.min.x == -MM(1) && box.max.y == MM(1));
    assert(g2_arc_contains_direction(arc, (G2Vector){MM(1), MM(1)}, &flag) == G2_OK && flag);     /* 45 degrees */
    assert(g2_arc_contains_direction(arc, (G2Vector){-MM(1), -MM(1)}, &flag) == G2_OK && !flag);  /* 225 degrees */
    arc.direction = G2_ARC_CW;                                                                    /* the other 270 */
    assert(g2_arc_contains_direction(arc, (G2Vector){-MM(1), -MM(1)}, &flag) == G2_OK && flag);
    assert(g2_compare_point_arc_distance(P(0, 0), (G2Arc){P(0, 0), P(1, 0), P(0, 1), G2_ARC_CCW, false}, MM(1), &o) == G2_OK &&
           o == G2_ORDER_EQUAL); /* the centre is exactly one radius from the arc */

    /* Invalid input is refused, never guessed: a coordinate past +-1 m, a degenerate polygon. */
    assert(g2_compare_distance(P(0, 0), (G2Point){G2_COORD_MAX_NM + 1, 0}, 0, &o) != G2_OK);
    G2Point line[] = {P(0, 0), P(1, 0), P(2, 0)};
    assert(g2_polygon_validate((G2Polygon){line, 3}) != G2_OK);

    /* BVH: shapes that can touch a query; a too-small buffer reports G2_ERR_CAPACITY with the exact count. */
    G2Shape shapes[] = {circle(P(0, 0), MM(1)), circle(P(10, 0), MM(1)), circle(P(0.5, 0.5), MM(1))};
    Arena arena;
    arena_init(&arena, 1 << 16);
    G2Bvh tree;
    size_t ids[4], count = 0;
    assert(g2_bvh_build_shapes(&arena, shapes, 3, 1, &tree) == G2_OK);
    assert(g2_bvh_query_shape(&tree, circle(P(0, 0), MM(0.1)), ids, 4, &count) == G2_OK && count == 2);
    assert((ids[0] == 0 && ids[1] == 2) || (ids[0] == 2 && ids[1] == 0));
    assert(g2_bvh_query_shape(&tree, circle(P(0, 0), MM(0.1)), ids, 1, &count) == G2_ERR_CAPACITY && count == 2);
    arena_destroy(&arena);

    printf("g2_types: exact geometry PASSED\n");
    return 0;
}
