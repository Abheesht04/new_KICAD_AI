
#include "arena.h"
#include "u128.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int64_t G2Nm;

#define G2_NM_PER_MM INT64_C(1000000)

#define G2_COORD_MAX_NM INT64_C(1000000000)
#define G2_DELTA_MAX_NM (G2_COORD_MAX_NM * INT64_C(2))

typedef struct {
  G2Nm x;
  G2Nm y;
} G2Point;

typedef struct {
  G2Nm dx;
  G2Nm dy;
} G2Vector;

typedef struct {
  G2Point min;
  G2Point max;
} G2Aabb;

typedef enum {
  G2_OK = 0,

  G2_ERR_ARGUMENT = -1,
  G2_ERR_RANGE = -2,
  G2_ERR_OVERFLOW = -3,
  G2_ERR_INVALID_GEOMETRY = -4,
  G2_ERR_UNSUPPORTED = -5,
  G2_ERR_MEMORY = -6

} G2Status;

typedef enum {
  G2_ORDER_LESS = -1,
  G2_ORDER_EQUAL = 0,
  G2_ORDER_GREATER = 1

} G2Order;

typedef struct {
  const G2Point *vertices;
  size_t count;
} G2Polygon;

typedef enum {
  G2_CONTAINMENT_OUTSIDE = 0,
  G2_CONTAINMENT_BOUNDARY,
  G2_CONTAINMENT_INSIDE
} G2Containment;

typedef struct {
  G2Point center;
  G2Nm radius;
} G2Circle;

typedef struct {
  G2Point a;
  G2Point b;
  G2Nm radius;
} G2Capsule;

typedef struct {
  G2Aabb core;
  G2Nm radius;
} G2RoundedRect;

typedef struct {
  G2Point vertices[4];
} G2Quad;

typedef enum { G2_FILL_EVEN_ODD = 0, G2_FILL_NON_ZERO = 1 } G2FillRule;

typedef struct {
  const G2Polygon *rings;
  size_t ring_count;
  G2FillRule fill_rule;
} G2Region;

typedef enum { G2_ARC_CCW = 0, G2_ARC_CW = 1 } G2ArcDirection;

typedef struct {
  G2Point center;
  G2Point start;
  G2Point end;
  G2ArcDirection direction;
  bool full_circle;
} G2Arc;

typedef struct {
  G2Aabb bounds;
  size_t object_id;
  G2Nm center_x;
  G2Nm center_y;
} G2BvhItem;

typedef struct {
  G2Aabb bounds;

  size_t left;
  size_t right;

  size_t first;
  size_t count;

  bool is_leaf;
} G2BvhNode;

typedef struct {
  G2BvhItem *items;
  G2BvhNode *nodes;

  size_t item_count;
  size_t node_count;
  size_t root;
} G2Bvh;

typedef enum {
  G2_SHAPE_SEGMENT = 0,
  G2_SHAPE_CIRCLE,
  G2_SHAPE_CAPSULE,
  G2_SHAPE_QUAD,
  G2_SHAPE_POLYGON,
  G2_SHAPE_REGION,
  G2_SHAPE_ARC,
  G2_SHAPE_ROUNDED_RECT
} G2ShapeKind;

typedef struct {
  G2ShapeKind kind;

  union {
    struct {
      G2Point a;
      G2Point b;
    } segment;

    G2Circle circle;
    G2Capsule capsule;
    G2Quad quad;
    G2Polygon polygon;
    G2Region region;
    G2Arc arc;
    G2RoundedRect rounded_rect;

  } as;

} G2Shape;

G2Status g2_vector_between(G2Point from, G2Point to, G2Vector *out);

G2Status g2_vector_dot(G2Vector a, G2Vector b, int64_t *out_dot);

G2Status g2_vector_cross(G2Vector a, G2Vector b, int64_t *out_cross);

G2Status g2_orient2d(G2Point a, G2Point b, G2Point c, G2Order *out_order);

G2Status g2_distance_squared(G2Point a, G2Point b, uint64_t *out_squared);

G2Status g2_compare_distance(G2Point a, G2Point b, G2Nm threshold,
                             G2Order *out_order);

G2Status g2_compare_distance(G2Point a, G2Point b, G2Nm threshold,
                             G2Order *out_order);

G2Status g2_point_on_segment(G2Point p, G2Point a, G2Point b, bool *out_on);

G2Status g2_aabb_contains_point(G2Aabb box, G2Point p, bool *out_contains);

G2Status g2_aabb_intersects(G2Aabb a, G2Aabb b, bool *out_intersects);

G2Status g2_segment_intersects(G2Point a, G2Point b, G2Point c, G2Point d,
                               bool *out_intersects);

G2Status g2_compare_point_segment_distance(G2Point p, G2Point a, G2Point b,
                                           G2Nm threshold, G2Order *out_order);

G2Status g2_compare_segment_segment_distance(G2Point a, G2Point b, G2Point c,
                                             G2Point d, G2Nm threshold,
                                             G2Order *out_order);

G2Status g2_segment_intersects_aabb(G2Point a, G2Point b, G2Aabb box,
                                    bool *out_intersects);

G2Status g2_polygon_validate(G2Polygon polygon);

G2Status g2_polygon_locate_point(G2Polygon polygon, G2Point point,
                                 G2Containment *out_location);

G2Status g2_polygon_intersects(G2Polygon a, G2Polygon b, bool *out_intersects);

G2Status g2_compare_point_polygon_distance(G2Point point, G2Polygon polygon,
                                           G2Nm threshold, G2Order *out_order);

G2Status g2_compare_segment_aabb_distance(G2Point a, G2Point b, G2Aabb box,
                                          G2Nm threshold, G2Order *out_order);

G2Status g2_compare_polygon_polygon_distance(G2Polygon a, G2Polygon b,
                                             G2Nm threshold,
                                             G2Order *out_order);

G2Status g2_compare_point_circle_distance(G2Point point, G2Circle circle,
                                          G2Nm threshold, G2Order *out_order);

G2Status g2_compare_circle_circle_distance(G2Circle a, G2Circle b,
                                           G2Nm threshold, G2Order *out_order);

G2Status g2_compare_segment_circle_distance(G2Point a, G2Point b,
                                            G2Circle circle, G2Nm threshold,
                                            G2Order *out_order);

G2Status g2_compare_capsule_capsule_distance(G2Capsule a, G2Capsule b,
                                             G2Nm threshold,
                                             G2Order *out_order);

G2Status g2_compare_segment_polygon_distance(G2Point a, G2Point b,
                                             G2Polygon polygon, G2Nm threshold,
                                             G2Order *out_order);

G2Status g2_compare_circle_polygon_distance(G2Circle circle, G2Polygon polygon,
                                            G2Nm threshold, G2Order *out_order);

G2Status g2_compare_capsule_polygon_distance(G2Capsule capsule,
                                             G2Polygon polygon, G2Nm threshold,
                                             G2Order *out_order);

G2Status g2_rounded_rect_validate(G2RoundedRect rect);

G2Status g2_rounded_rect_aabb(G2RoundedRect rect, G2Aabb *out_box);

G2Status g2_compare_point_aabb_distance(G2Point point, G2Aabb box,
                                        G2Nm threshold, G2Order *out_order);

G2Status g2_compare_aabb_aabb_distance(G2Aabb a, G2Aabb b, G2Nm threshold,
                                       G2Order *out_order);

G2Status g2_compare_point_rounded_rect_distance(G2Point point,
                                                G2RoundedRect rect,
                                                G2Nm threshold,
                                                G2Order *out_order);

G2Status g2_compare_circle_rounded_rect_distance(G2Circle circle,
                                                 G2RoundedRect rect,
                                                 G2Nm threshold,
                                                 G2Order *out_order);

G2Status g2_compare_capsule_rounded_rect_distance(G2Capsule capsule,
                                                  G2RoundedRect rect,
                                                  G2Nm threshold,
                                                  G2Order *out_order);

G2Status g2_compare_rounded_rect_rounded_rect_distance(G2RoundedRect a,
                                                       G2RoundedRect b,
                                                       G2Nm threshold,
                                                       G2Order *out_order);

G2Status g2_quad_validate(G2Quad quad);

G2Status g2_quad_validate_rectangle(G2Quad quad);

G2Status g2_quad_aabb(G2Quad quad, G2Aabb *out_box);

G2Status g2_quad_intersects(G2Quad a, G2Quad b, bool *out_intersects);

G2Status g2_compare_quad_quad_distance(G2Quad a, G2Quad b, G2Nm threshold,
                                       G2Order *out_order);

G2Status g2_region_validate(G2Region region);

G2Status g2_region_locate_point(G2Region region, G2Point point,
                                G2Containment *out_location);

G2Status g2_region_aabb(G2Region region, G2Aabb *out_box);

G2Status g2_region_intersects(G2Region a, G2Region b, bool *out_intersects);

G2Status g2_compare_point_region_distance(G2Point point, G2Region region,
                                          G2Nm threshold, G2Order *out_order);

G2Status g2_compare_region_region_distance(G2Region a, G2Region b,
                                           G2Nm threshold, G2Order *out_order);

G2Status g2_arc_validate(G2Arc arc);

G2Status g2_arc_aabb(G2Arc arc, G2Aabb *out_box);

G2Status g2_arc_contains_direction(G2Arc arc, G2Vector direction,
                                   bool *out_contains);

G2Status g2_compare_point_arc_distance(G2Point point, G2Arc arc, G2Nm threshold,
                                       G2Order *out_order);

G2Status g2_shape_validate(G2Shape shape);

G2Status g2_shape_aabb(G2Shape shape, G2Aabb *out_box);

G2Status g2_compare_shape_distance(G2Shape a, G2Shape b, G2Nm threshold,
                                   G2Order *out_order);

G2Status g2_shapes_intersect(G2Shape a, G2Shape b, bool *out_intersects);

G2Status g2_bvh_build_shapes(Arena *arena, const G2Shape *shapes, size_t count,
                             size_t leaf_capacity, G2Bvh *out_tree);

G2Status g2_bvh_query_shape(const G2Bvh *tree, G2Shape query, size_t *out_ids,
                            size_t capacity, size_t *out_count);
