#include "g2_types.h"
#include "nmath.h"

#define G2_MAX_POINT_DISTANCE_CEIL_NM INT64_C(2828427125)

static bool g2_coord_valid(G2Nm value) {
  return value >= -G2_COORD_MAX_NM && value <= G2_COORD_MAX_NM;
}

static bool g2_point_valid(G2Point p) {
  return g2_coord_valid(p.x) && g2_coord_valid(p.y);
}

static bool g2_vector_valid(G2Vector v) {
  return v.dx >= -G2_DELTA_MAX_NM && v.dx <= G2_DELTA_MAX_NM &&
         v.dy >= -G2_DELTA_MAX_NM && v.dy <= G2_DELTA_MAX_NM;
}

static G2Status g2_validate_aabb(G2Aabb box) {
  if (!g2_point_valid(box.min) || !g2_point_valid(box.max))
    return G2_ERR_RANGE;

  if (box.min.x > box.max.x || box.min.y > box.max.y)
    return G2_ERR_INVALID_GEOMETRY;

  return G2_OK;
}

static bool g2_between_closed(G2Nm value, G2Nm a, G2Nm b) {
  G2Nm lo = (a < b) ? a : b;
  G2Nm hi = (a > b) ? a : b;

  return value >= lo && value <= hi;
}

static bool g2_opposite_sides(G2Order a, G2Order b) {
  return (a == G2_ORDER_LESS && b == G2_ORDER_GREATER) ||
         (a == G2_ORDER_GREATER && b == G2_ORDER_LESS);
}

static bool g2_radius_valid_at_point(G2Point p, G2Nm radius) {
  if (!g2_point_valid(p) || radius < 0 || radius > G2_COORD_MAX_NM)
    return false;

  return p.x - radius >= -G2_COORD_MAX_NM && p.x + radius <= G2_COORD_MAX_NM &&
         p.y - radius >= -G2_COORD_MAX_NM && p.y + radius <= G2_COORD_MAX_NM;
}

static void g2_set_zero_distance_order(G2Nm threshold, G2Order *out_order) {
  *out_order = (threshold == 0) ? G2_ORDER_EQUAL : G2_ORDER_LESS;
}

static G2Polygon g2_quad_as_polygon(const G2Quad *quad) {
  G2Polygon polygon = {.vertices = quad->vertices, .count = 4};

  return polygon;
}

static int g2_vector_half(G2Vector v) {
  return (v.dy > 0 || (v.dy == 0 && v.dx >= 0)) ? 0 : 1;
}

/*
    Returns:
      -1 when a is before b in CCW angular order
       0 when both have the same direction
      +1 when a is after b
*/
static G2Status g2_angle_compare(G2Vector a, G2Vector b, int *out_comparison) {
  if (out_comparison == NULL)
    return G2_ERR_ARGUMENT;

  if (!g2_vector_valid(a) || !g2_vector_valid(b))
    return G2_ERR_RANGE;

  if ((a.dx == 0 && a.dy == 0) || (b.dx == 0 && b.dy == 0))
    return G2_ERR_INVALID_GEOMETRY;

  int ha = g2_vector_half(a);
  int hb = g2_vector_half(b);

  if (ha != hb) {
    *out_comparison = ha < hb ? -1 : 1;
    return G2_OK;
  }

  int64_t cross;

  G2Status status = g2_vector_cross(a, b, &cross);
  if (status != G2_OK)
    return status;

  if (cross > 0)
    *out_comparison = -1;
  else if (cross < 0)
    *out_comparison = 1;
  else
    *out_comparison = 0;

  return G2_OK;
}

static G2Status g2_arc_contains_direction_validated(G2Arc arc,
                                                    G2Vector direction,
                                                    bool *out_contains) {
  if (out_contains == NULL)
    return G2_ERR_ARGUMENT;

  if (!g2_vector_valid(direction) || (direction.dx == 0 && direction.dy == 0))
    return G2_ERR_ARGUMENT;

  if (arc.full_circle) {
    *out_contains = true;
    return G2_OK;
  }

  G2Vector start, end;

  G2Status status = g2_vector_between(arc.center, arc.start, &start);

  if (status != G2_OK)
    return status;

  status = g2_vector_between(arc.center, arc.end, &end);

  if (status != G2_OK)
    return status;

  /* CW start -> end is equivalent to CCW end -> start. */
  if (arc.direction == G2_ARC_CW) {
    G2Vector temp = start;
    start = end;
    end = temp;
  }

  int start_end;
  int start_direction;
  int direction_end;

  status = g2_angle_compare(start, end, &start_end);
  if (status != G2_OK)
    return status;

  status = g2_angle_compare(start, direction, &start_direction);
  if (status != G2_OK)
    return status;

  status = g2_angle_compare(direction, end, &direction_end);
  if (status != G2_OK)
    return status;

  bool result;

  if (start_end <= 0) {
    result = start_direction <= 0 && direction_end <= 0;
  } else {
    result = start_direction <= 0 || direction_end <= 0;
  }

  *out_contains = result;
  return G2_OK;
}

static bool g2_u128_mul4(U128 *value) {
  if (value == NULL || (value->hi >> 62) != 0)
    return false;

  value->hi = (value->hi << 2) | (value->lo >> 62);
  value->lo <<= 2;

  return true;
}

static G2Status g2_compare_sqrt_difference(uint64_t a2, uint64_t b2,
                                           uint64_t threshold,
                                           G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  uint64_t x = (a2 > b2) ? a2 : b2;
  uint64_t y = (a2 > b2) ? b2 : a2;

  if (x == y) {
    *out_order = threshold == 0 ? G2_ORDER_EQUAL : G2_ORDER_LESS;
    return G2_OK;
  }

  if (threshold == 0) {
    *out_order = G2_ORDER_GREATER;
    return G2_OK;
  }

  uint64_t t2 = threshold * threshold;
  int64_t k = (int64_t)x - (int64_t)y - (int64_t)t2;

  if (k < 0) {
    *out_order = G2_ORDER_LESS;
    return G2_OK;
  }

  if (k == 0) {
    *out_order = y == 0 ? G2_ORDER_EQUAL : G2_ORDER_LESS;
    return G2_OK;
  }

  if (y == 0) {
    *out_order = G2_ORDER_GREATER;
    return G2_OK;
  }

  uint64_t uk = (uint64_t)k;

  U128 lhs = u128_mul(uk, uk);
  U128 rhs = u128_mul(t2, y);

  if (!g2_u128_mul4(&rhs))
    return G2_ERR_OVERFLOW;

  int comparison = u128_cmp(lhs, rhs);

  if (comparison < 0)
    *out_order = G2_ORDER_LESS;
  else if (comparison > 0)
    *out_order = G2_ORDER_GREATER;
  else
    *out_order = G2_ORDER_EQUAL;

  return G2_OK;
}

static G2Aabb g2_aabb_union(G2Aabb a, G2Aabb b) {
  G2Aabb result = {.min = {a.min.x < b.min.x ? a.min.x : b.min.x,
                           a.min.y < b.min.y ? a.min.y : b.min.y},
                   .max = {a.max.x > b.max.x ? a.max.x : b.max.x,
                           a.max.y > b.max.y ? a.max.y : b.max.y}};

  return result;
}

static G2Nm g2_bvh_center_axis(const G2BvhItem *item, int axis) {
  if (axis == 0) {
    return item->bounds.min.x + (item->bounds.max.x - item->bounds.min.x) / 2;
  }

  return item->bounds.min.y + (item->bounds.max.y - item->bounds.min.y) / 2;
}

static bool g2_bvh_item_less(const G2BvhItem *a, const G2BvhItem *b, int axis) {
  G2Nm ca = g2_bvh_center_axis(a, axis);
  G2Nm cb = g2_bvh_center_axis(b, axis);

  if (ca != cb)
    return ca < cb;

  return a->object_id < b->object_id;
}

static void g2_bvh_swap(G2BvhItem *a, G2BvhItem *b) {
  G2BvhItem temp = *a;
  *a = *b;
  *b = temp;
}

static void g2_bvh_sift_down(G2BvhItem *items, size_t first, size_t root,
                             size_t count, int axis) {
  while (root < count / 2) {
    size_t child = root * 2 + 1;

    if (child + 1 < count &&
        g2_bvh_item_less(&items[first + child], &items[first + child + 1],
                         axis)) {
      ++child;
    }

    if (!g2_bvh_item_less(&items[first + root], &items[first + child], axis))
      return;

    g2_bvh_swap(&items[first + root], &items[first + child]);
    root = child;
  }
}

static void g2_bvh_sort(G2BvhItem *items, size_t first, size_t count,
                        int axis) {
  for (size_t start = count / 2; start > 0;) {
    --start;
    g2_bvh_sift_down(items, first, start, count, axis);
  }

  for (size_t end = count; end > 1;) {
    g2_bvh_swap(&items[first], &items[first + end - 1]);
    --end;
    g2_bvh_sift_down(items, first, 0, end, axis);
  }
}

static G2Status g2_bvh_build_node(G2Bvh *tree, size_t first, size_t count,
                                  size_t leaf_capacity, size_t *out_node) {
  if (out_node == NULL || count == 0)
    return G2_ERR_ARGUMENT;

  if (tree->node_count >= 2 * tree->item_count - 1)
    return G2_ERR_OVERFLOW;

  size_t node_id = tree->node_count++;
  G2Aabb bounds = tree->items[first].bounds;

  for (size_t i = 1; i < count; ++i) {
    bounds = g2_aabb_union(bounds, tree->items[first + i].bounds);
  }

  G2BvhNode *node = &tree->nodes[node_id];

  node->bounds = bounds;
  node->left = SIZE_MAX;
  node->right = SIZE_MAX;
  node->first = first;
  node->count = count;

  if (count <= leaf_capacity) {
    node->is_leaf = true;
    *out_node = node_id;
    return G2_OK;
  }

  G2Nm width = bounds.max.x - bounds.min.x;
  G2Nm height = bounds.max.y - bounds.min.y;
  int axis = width >= height ? 0 : 1;

  g2_bvh_sort(tree->items, first, count, axis);

  size_t left_count = count / 2;
  size_t right_count = count - left_count;
  size_t left_node, right_node;

  G2Status status =
      g2_bvh_build_node(tree, first, left_count, leaf_capacity, &left_node);

  if (status != G2_OK)
    return status;

  status = g2_bvh_build_node(tree, first + left_count, right_count,
                             leaf_capacity, &right_node);

  if (status != G2_OK)
    return status;

  tree->nodes[node_id].left = left_node;
  tree->nodes[node_id].right = right_node;
  tree->nodes[node_id].is_leaf = false;
  tree->nodes[node_id].first = 0;
  tree->nodes[node_id].count = 0;

  *out_node = node_id;
  return G2_OK;
}

G2Status g2_circle_validate(G2Circle circle) {
  if (!g2_radius_valid_at_point(circle.center, circle.radius))
    return G2_ERR_RANGE;

  return G2_OK;
}

G2Status g2_capsule_validate(G2Capsule capsule) {
  if (!g2_point_valid(capsule.a) || !g2_point_valid(capsule.b) ||
      capsule.radius < 0 || capsule.radius > G2_COORD_MAX_NM)
    return G2_ERR_RANGE;

  G2Nm min_x = (capsule.a.x < capsule.b.x) ? capsule.a.x : capsule.b.x;
  G2Nm max_x = (capsule.a.x > capsule.b.x) ? capsule.a.x : capsule.b.x;
  G2Nm min_y = (capsule.a.y < capsule.b.y) ? capsule.a.y : capsule.b.y;
  G2Nm max_y = (capsule.a.y > capsule.b.y) ? capsule.a.y : capsule.b.y;

  if (min_x - capsule.radius < -G2_COORD_MAX_NM ||
      max_x + capsule.radius > G2_COORD_MAX_NM ||
      min_y - capsule.radius < -G2_COORD_MAX_NM ||
      max_y + capsule.radius > G2_COORD_MAX_NM)
    return G2_ERR_RANGE;

  return G2_OK;
}

G2Status g2_circle_aabb(G2Circle circle, G2Aabb *out_box) {
  if (out_box == NULL)
    return G2_ERR_ARGUMENT;

  G2Status status = g2_circle_validate(circle);
  if (status != G2_OK)
    return status;

  G2Nm r = circle.radius;
  G2Aabb result = {.min = {circle.center.x - r, circle.center.y - r},
                   .max = {circle.center.x + r, circle.center.y + r}};

  *out_box = result;
  return G2_OK;
}

G2Status g2_capsule_aabb(G2Capsule capsule, G2Aabb *out_box) {
  if (out_box == NULL)
    return G2_ERR_ARGUMENT;

  G2Status status = g2_capsule_validate(capsule);
  if (status != G2_OK)
    return status;

  G2Nm r = capsule.radius;
  G2Aabb result = {
      .min = {((capsule.a.x < capsule.b.x) ? capsule.a.x : capsule.b.x) - r,
              ((capsule.a.y < capsule.b.y) ? capsule.a.y : capsule.b.y) - r},
      .max = {((capsule.a.x > capsule.b.x) ? capsule.a.x : capsule.b.x) + r,
              ((capsule.a.y > capsule.b.y) ? capsule.a.y : capsule.b.y) + r}};

  *out_box = result;
  return G2_OK;
}

G2Status g2_vector_between(G2Point from, G2Point to, G2Vector *out) {
  if (out == NULL)
    return G2_ERR_ARGUMENT;

  if (!g2_point_valid(from) || !g2_point_valid(to))
    return G2_ERR_RANGE;

  G2Vector result = {.dx = to.x - from.x, .dy = to.y - from.y};
  *out = result;
  return G2_OK;
}

G2Status g2_vector_dot(G2Vector a, G2Vector b, int64_t *out_dot) {
  if (out_dot == NULL)
    return G2_ERR_ARGUMENT;

  if (!g2_vector_valid(a) || !g2_vector_valid(b))
    return G2_ERR_RANGE;

  int64_t result = a.dx * b.dx + a.dy * b.dy;
  *out_dot = result;
  return G2_OK;
}

G2Status g2_vector_cross(G2Vector a, G2Vector b, int64_t *out_cross) {
  if (out_cross == NULL)
    return G2_ERR_ARGUMENT;

  if (!g2_vector_valid(a) || !g2_vector_valid(b))
    return G2_ERR_RANGE;

  int64_t result = a.dx * b.dy - a.dy * b.dx;
  *out_cross = result;
  return G2_OK;
}

G2Status g2_orient2d(G2Point a, G2Point b, G2Point c, G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  G2Vector ab, ac;
  G2Status status = g2_vector_between(a, b, &ab);
  if (status != G2_OK)
    return status;

  status = g2_vector_between(a, c, &ac);
  if (status != G2_OK)
    return status;

  int64_t cross;
  status = g2_vector_cross(ab, ac, &cross);
  if (status != G2_OK)
    return status;

  G2Order result = G2_ORDER_EQUAL;
  if (cross > 0)
    result = G2_ORDER_GREATER;
  else if (cross < 0)
    result = G2_ORDER_LESS;

  *out_order = result;
  return G2_OK;
}

G2Status g2_distance_squared(G2Point a, G2Point b, uint64_t *out_squared) {
  if (out_squared == NULL)
    return G2_ERR_ARGUMENT;

  if (!g2_point_valid(a) || !g2_point_valid(b))
    return G2_ERR_RANGE;

  int64_t dx = a.x - b.x;
  int64_t dy = a.y - b.y;

  uint64_t adx = (dx < 0) ? (uint64_t)(-dx) : (uint64_t)dx;
  uint64_t ady = (dy < 0) ? (uint64_t)(-dy) : (uint64_t)dy;

  uint64_t result = adx * adx + ady * ady;
  *out_squared = result;
  return G2_OK;
}

G2Status g2_compare_distance(G2Point a, G2Point b, G2Nm threshold,
                             G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  uint64_t distance_squared;
  G2Status status = g2_distance_squared(a, b, &distance_squared);
  if (status != G2_OK)
    return status;

  uint64_t t = (uint64_t)threshold;
  uint64_t upper_bound = (uint64_t)G2_DELTA_MAX_NM * UINT64_C(2);

  if (t >= upper_bound) {
    *out_order = G2_ORDER_LESS;
    return G2_OK;
  }

  uint64_t threshold_squared = t * t;

  if (distance_squared < threshold_squared)
    *out_order = G2_ORDER_LESS;
  else if (distance_squared > threshold_squared)
    *out_order = G2_ORDER_GREATER;
  else
    *out_order = G2_ORDER_EQUAL;

  return G2_OK;
}

G2Status g2_point_on_segment(G2Point p, G2Point a, G2Point b, bool *out_on) {
  if (out_on == NULL)
    return G2_ERR_ARGUMENT;

  G2Order orientation;
  G2Status status = g2_orient2d(a, b, p, &orientation);
  if (status != G2_OK)
    return status;

  bool result = orientation == G2_ORDER_EQUAL &&
                g2_between_closed(p.x, a.x, b.x) &&
                g2_between_closed(p.y, a.y, b.y);

  *out_on = result;
  return G2_OK;
}

G2Status g2_aabb_contains_point(G2Aabb box, G2Point p, bool *out_contains) {
  if (out_contains == NULL)
    return G2_ERR_ARGUMENT;

  G2Status status = g2_validate_aabb(box);
  if (status != G2_OK)
    return status;

  if (!g2_point_valid(p))
    return G2_ERR_RANGE;

  bool result = p.x >= box.min.x && p.x <= box.max.x && p.y >= box.min.y &&
                p.y <= box.max.y;

  *out_contains = result;
  return G2_OK;
}

G2Status g2_aabb_intersects(G2Aabb a, G2Aabb b, bool *out_intersects) {
  if (out_intersects == NULL)
    return G2_ERR_ARGUMENT;

  G2Status status = g2_validate_aabb(a);
  if (status != G2_OK)
    return status;

  status = g2_validate_aabb(b);
  if (status != G2_OK)
    return status;

  bool result = a.min.x <= b.max.x && a.max.x >= b.min.x &&
                a.min.y <= b.max.y && a.max.y >= b.min.y;

  *out_intersects = result;
  return G2_OK;
}

G2Status g2_segment_intersects(G2Point a, G2Point b, G2Point c, G2Point d,
                               bool *out_intersects) {
  if (out_intersects == NULL)
    return G2_ERR_ARGUMENT;

  G2Order o1, o2, o3, o4;
  G2Status status;

  status = g2_orient2d(a, b, c, &o1);
  if (status != G2_OK)
    return status;

  status = g2_orient2d(a, b, d, &o2);
  if (status != G2_OK)
    return status;

  status = g2_orient2d(c, d, a, &o3);
  if (status != G2_OK)
    return status;

  status = g2_orient2d(c, d, b, &o4);
  if (status != G2_OK)
    return status;

  bool intersects = g2_opposite_sides(o1, o2) && g2_opposite_sides(o3, o4);

  if (!intersects && o1 == G2_ORDER_EQUAL)
    intersects =
        g2_between_closed(c.x, a.x, b.x) && g2_between_closed(c.y, a.y, b.y);

  if (!intersects && o2 == G2_ORDER_EQUAL)
    intersects =
        g2_between_closed(d.x, a.x, b.x) && g2_between_closed(d.y, a.y, b.y);

  if (!intersects && o3 == G2_ORDER_EQUAL)
    intersects =
        g2_between_closed(a.x, c.x, d.x) && g2_between_closed(a.y, c.y, d.y);

  if (!intersects && o4 == G2_ORDER_EQUAL)
    intersects =
        g2_between_closed(b.x, c.x, d.x) && g2_between_closed(b.y, c.y, d.y);

  *out_intersects = intersects;
  return G2_OK;
}

G2Status g2_compare_point_segment_distance(G2Point p, G2Point a, G2Point b,
                                           G2Nm threshold, G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  G2Vector ab, ap;
  G2Status status = g2_vector_between(a, b, &ab);
  if (status != G2_OK)
    return status;

  status = g2_vector_between(a, p, &ap);
  if (status != G2_OK)
    return status;

  const uint64_t trivial_threshold = (uint64_t)G2_DELTA_MAX_NM * UINT64_C(2);
  if ((uint64_t)threshold >= trivial_threshold) {
    *out_order = G2_ORDER_LESS;
    return G2_OK;
  }

  uint64_t length_squared;
  status = g2_distance_squared(a, b, &length_squared);
  if (status != G2_OK)
    return status;

  if (length_squared == 0)
    return g2_compare_distance(p, a, threshold, out_order);

  int64_t projection;
  status = g2_vector_dot(ap, ab, &projection);
  if (status != G2_OK)
    return status;

  if (projection <= 0)
    return g2_compare_distance(p, a, threshold, out_order);

  if ((uint64_t)projection >= length_squared)
    return g2_compare_distance(p, b, threshold, out_order);

  int64_t cross;
  status = g2_vector_cross(ab, ap, &cross);
  if (status != G2_OK)
    return status;

  uint64_t abs_cross = (cross < 0) ? (uint64_t)(-cross) : (uint64_t)cross;
  uint64_t t = (uint64_t)threshold;
  uint64_t threshold_squared = t * t;

  U128 lhs = u128_mul(abs_cross, abs_cross);
  U128 rhs = u128_mul(threshold_squared, length_squared);

  int comparison = u128_cmp(lhs, rhs);

  if (comparison < 0)
    *out_order = G2_ORDER_LESS;
  else if (comparison > 0)
    *out_order = G2_ORDER_GREATER;
  else
    *out_order = G2_ORDER_EQUAL;

  return G2_OK;
}

G2Status g2_compare_segment_segment_distance(G2Point a, G2Point b, G2Point c,
                                             G2Point d, G2Nm threshold,
                                             G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  bool intersects;
  G2Status status = g2_segment_intersects(a, b, c, d, &intersects);
  if (status != G2_OK)
    return status;

  if (intersects) {
    *out_order = (threshold == 0) ? G2_ORDER_EQUAL : G2_ORDER_LESS;
    return G2_OK;
  }

  G2Order best = G2_ORDER_GREATER;
  G2Order current;

  status = g2_compare_point_segment_distance(a, c, d, threshold, &current);
  if (status != G2_OK)
    return status;
  if (current == G2_ORDER_LESS) {
    *out_order = current;
    return G2_OK;
  }
  if (current < best)
    best = current;

  status = g2_compare_point_segment_distance(b, c, d, threshold, &current);
  if (status != G2_OK)
    return status;
  if (current == G2_ORDER_LESS) {
    *out_order = current;
    return G2_OK;
  }
  if (current < best)
    best = current;

  status = g2_compare_point_segment_distance(c, a, b, threshold, &current);
  if (status != G2_OK)
    return status;
  if (current == G2_ORDER_LESS) {
    *out_order = current;
    return G2_OK;
  }
  if (current < best)
    best = current;

  status = g2_compare_point_segment_distance(d, a, b, threshold, &current);
  if (status != G2_OK)
    return status;
  if (current < best)
    best = current;

  *out_order = best;
  return G2_OK;
}

G2Status g2_segment_intersects_aabb(G2Point a, G2Point b, G2Aabb box,
                                    bool *out_intersects) {
  if (out_intersects == NULL)
    return G2_ERR_ARGUMENT;

  G2Status status = g2_validate_aabb(box);
  if (status != G2_OK)
    return status;

  if (!g2_point_valid(a) || !g2_point_valid(b))
    return G2_ERR_RANGE;

  G2Aabb segment_bounds = {
      .min = {.x = (a.x < b.x) ? a.x : b.x, .y = (a.y < b.y) ? a.y : b.y},
      .max = {.x = (a.x > b.x) ? a.x : b.x, .y = (a.y > b.y) ? a.y : b.y}};

  bool bounds_overlap;
  status = g2_aabb_intersects(segment_bounds, box, &bounds_overlap);
  if (status != G2_OK)
    return status;

  if (!bounds_overlap) {
    *out_intersects = false;
    return G2_OK;
  }

  bool contained;
  status = g2_aabb_contains_point(box, a, &contained);
  if (status != G2_OK)
    return status;

  if (contained) {
    *out_intersects = true;
    return G2_OK;
  }

  status = g2_aabb_contains_point(box, b, &contained);
  if (status != G2_OK)
    return status;

  if (contained) {
    *out_intersects = true;
    return G2_OK;
  }

  G2Point corners[4] = {{box.min.x, box.min.y},
                        {box.max.x, box.min.y},
                        {box.max.x, box.max.y},
                        {box.min.x, box.max.y}};

  for (size_t i = 0; i < 4; ++i) {
    size_t next = (i + 1 == 4) ? 0 : i + 1;
    bool edge_intersects;

    status = g2_segment_intersects(a, b, corners[i], corners[next],
                                   &edge_intersects);
    if (status != G2_OK)
      return status;
    if (edge_intersects) {
      *out_intersects = true;
      return G2_OK;
    }
  }

  *out_intersects = false;
  return G2_OK;
}

G2Status g2_polygon_validate(G2Polygon polygon) {
  if (polygon.vertices == NULL || polygon.count < 3)
    return G2_ERR_ARGUMENT;

  const size_t count = polygon.count;

  for (size_t i = 0; i < count; ++i) {
    if (!g2_point_valid(polygon.vertices[i]))
      return G2_ERR_RANGE;

    size_t next = (i + 1 == count) ? 0 : i + 1;
    G2Point a = polygon.vertices[i];
    G2Point b = polygon.vertices[next];

    if (a.x == b.x && a.y == b.y)
      return G2_ERR_INVALID_GEOMETRY;
  }

  bool has_area = false;
  for (size_t i = 2; i < count; ++i) {
    G2Order orientation;
    G2Status status = g2_orient2d(polygon.vertices[0], polygon.vertices[1],
                                  polygon.vertices[i], &orientation);
    if (status != G2_OK)
      return status;

    if (orientation != G2_ORDER_EQUAL) {
      has_area = true;
      break;
    }
  }

  if (!has_area)
    return G2_ERR_INVALID_GEOMETRY;

  for (size_t i = 0; i < count; ++i) {
    size_t prev = (i == 0) ? count - 1 : i - 1;
    size_t next = (i + 1 == count) ? 0 : i + 1;

    G2Point a = polygon.vertices[prev];
    G2Point b = polygon.vertices[i];
    G2Point c = polygon.vertices[next];

    G2Order orientation;
    G2Status status = g2_orient2d(a, b, c, &orientation);
    if (status != G2_OK)
      return status;

    if (orientation == G2_ORDER_EQUAL) {
      bool a_on_bc, c_on_ab;

      status = g2_point_on_segment(a, b, c, &a_on_bc);
      if (status != G2_OK)
        return status;

      status = g2_point_on_segment(c, a, b, &c_on_ab);
      if (status != G2_OK)
        return status;

      if (a_on_bc || c_on_ab)
        return G2_ERR_INVALID_GEOMETRY;
    }
  }

  for (size_t i = 0; i < count; ++i) {
    size_t i_next = (i + 1 == count) ? 0 : i + 1;

    for (size_t j = i + 1; j < count; ++j) {
      if (j == i + 1 || (i == 0 && j == count - 1))
        continue;

      size_t j_next = (j + 1 == count) ? 0 : j + 1;
      bool intersects;

      G2Status status = g2_segment_intersects(
          polygon.vertices[i], polygon.vertices[i_next], polygon.vertices[j],
          polygon.vertices[j_next], &intersects);
      if (status != G2_OK)
        return status;

      if (intersects)
        return G2_ERR_INVALID_GEOMETRY;
    }
  }

  return G2_OK;
}

static G2Status g2_polygon_locate_point_validated(G2Polygon polygon,
                                                  G2Point point,
                                                  G2Containment *out_location) {
  bool inside = false;

  for (size_t i = 0; i < polygon.count; ++i) {
    size_t next = (i + 1 == polygon.count) ? 0 : i + 1;
    G2Point a = polygon.vertices[i];
    G2Point b = polygon.vertices[next];

    bool on_edge;
    G2Status status = g2_point_on_segment(point, a, b, &on_edge);
    if (status != G2_OK)
      return status;

    if (on_edge) {
      *out_location = G2_CONTAINMENT_BOUNDARY;
      return G2_OK;
    }

    bool crosses_ray = (a.y > point.y) != (b.y > point.y);
    if (!crosses_ray)
      continue;

    G2Order orientation;
    status = g2_orient2d(a, b, point, &orientation);
    if (status != G2_OK)
      return status;

    bool crosses_to_right = (b.y > a.y && orientation == G2_ORDER_GREATER) ||
                            (b.y < a.y && orientation == G2_ORDER_LESS);

    if (crosses_to_right)
      inside = !inside;
  }

  *out_location = inside ? G2_CONTAINMENT_INSIDE : G2_CONTAINMENT_OUTSIDE;
  return G2_OK;
}

G2Status g2_polygon_locate_point(G2Polygon polygon, G2Point point,
                                 G2Containment *out_location) {
  if (out_location == NULL)
    return G2_ERR_ARGUMENT;

  if (!g2_point_valid(point))
    return G2_ERR_RANGE;

  G2Status status = g2_polygon_validate(polygon);
  if (status != G2_OK)
    return status;

  return g2_polygon_locate_point_validated(polygon, point, out_location);
}

G2Status g2_polygon_intersects(G2Polygon a, G2Polygon b, bool *out_intersects) {
  if (out_intersects == NULL)
    return G2_ERR_ARGUMENT;

  G2Status status = g2_polygon_validate(a);
  if (status != G2_OK)
    return status;

  status = g2_polygon_validate(b);
  if (status != G2_OK)
    return status;

  G2Aabb bounds_a = {.min = a.vertices[0], .max = a.vertices[0]};
  G2Aabb bounds_b = {.min = b.vertices[0], .max = b.vertices[0]};

  for (size_t i = 1; i < a.count; ++i) {
    G2Point p = a.vertices[i];
    if (p.x < bounds_a.min.x)
      bounds_a.min.x = p.x;
    if (p.y < bounds_a.min.y)
      bounds_a.min.y = p.y;
    if (p.x > bounds_a.max.x)
      bounds_a.max.x = p.x;
    if (p.y > bounds_a.max.y)
      bounds_a.max.y = p.y;
  }

  for (size_t i = 1; i < b.count; ++i) {
    G2Point p = b.vertices[i];
    if (p.x < bounds_b.min.x)
      bounds_b.min.x = p.x;
    if (p.y < bounds_b.min.y)
      bounds_b.min.y = p.y;
    if (p.x > bounds_b.max.x)
      bounds_b.max.x = p.x;
    if (p.y > bounds_b.max.y)
      bounds_b.max.y = p.y;
  }

  bool bounds_overlap;
  status = g2_aabb_intersects(bounds_a, bounds_b, &bounds_overlap);
  if (status != G2_OK)
    return status;

  if (!bounds_overlap) {
    *out_intersects = false;
    return G2_OK;
  }

  for (size_t i = 0; i < a.count; ++i) {
    size_t ai_next = (i + 1 == a.count) ? 0 : i + 1;

    for (size_t j = 0; j < b.count; ++j) {
      size_t bj_next = (j + 1 == b.count) ? 0 : j + 1;
      bool intersects;

      status = g2_segment_intersects(a.vertices[i], a.vertices[ai_next],
                                     b.vertices[j], b.vertices[bj_next],
                                     &intersects);
      if (status != G2_OK)
        return status;

      if (intersects) {
        *out_intersects = true;
        return G2_OK;
      }
    }
  }

  G2Containment location;
  status = g2_polygon_locate_point_validated(b, a.vertices[0], &location);
  if (status != G2_OK)
    return status;

  if (location != G2_CONTAINMENT_OUTSIDE) {
    *out_intersects = true;
    return G2_OK;
  }

  status = g2_polygon_locate_point_validated(a, b.vertices[0], &location);
  if (status != G2_OK)
    return status;

  *out_intersects = location != G2_CONTAINMENT_OUTSIDE;
  return G2_OK;
}

G2Status g2_compare_point_polygon_distance(G2Point point, G2Polygon polygon,
                                           G2Nm threshold, G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  G2Containment location;
  G2Status status = g2_polygon_locate_point(polygon, point, &location);
  if (status != G2_OK)
    return status;

  if (location != G2_CONTAINMENT_OUTSIDE) {
    *out_order = (threshold == 0) ? G2_ORDER_EQUAL : G2_ORDER_LESS;
    return G2_OK;
  }

  G2Order best = G2_ORDER_GREATER;

  for (size_t i = 0; i < polygon.count; ++i) {
    size_t next = (i + 1 == polygon.count) ? 0 : i + 1;
    G2Order current;

    status = g2_compare_point_segment_distance(point, polygon.vertices[i],
                                               polygon.vertices[next],
                                               threshold, &current);
    if (status != G2_OK)
      return status;

    if (current == G2_ORDER_LESS) {
      *out_order = G2_ORDER_LESS;
      return G2_OK;
    }

    if (current == G2_ORDER_EQUAL)
      best = G2_ORDER_EQUAL;
  }

  *out_order = best;
  return G2_OK;
}

G2Status g2_compare_segment_aabb_distance(G2Point a, G2Point b, G2Aabb box,
                                          G2Nm threshold, G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  bool intersects;
  G2Status status = g2_segment_intersects_aabb(a, b, box, &intersects);
  if (status != G2_OK)
    return status;

  if (intersects) {
    *out_order = (threshold == 0) ? G2_ORDER_EQUAL : G2_ORDER_LESS;
    return G2_OK;
  }

  G2Point corners[4] = {{box.min.x, box.min.y},
                        {box.max.x, box.min.y},
                        {box.max.x, box.max.y},
                        {box.min.x, box.max.y}};

  G2Order best = G2_ORDER_GREATER;

  for (size_t i = 0; i < 4; ++i) {
    size_t next = (i + 1 == 4) ? 0 : i + 1;
    G2Order current;

    status = g2_compare_segment_segment_distance(
        a, b, corners[i], corners[next], threshold, &current);
    if (status != G2_OK)
      return status;

    if (current == G2_ORDER_LESS) {
      *out_order = G2_ORDER_LESS;
      return G2_OK;
    }

    if (current == G2_ORDER_EQUAL)
      best = G2_ORDER_EQUAL;
  }

  *out_order = best;
  return G2_OK;
}

G2Status g2_compare_polygon_polygon_distance(G2Polygon a, G2Polygon b,
                                             G2Nm threshold,
                                             G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  bool intersects;
  G2Status status = g2_polygon_intersects(a, b, &intersects);
  if (status != G2_OK)
    return status;

  if (intersects) {
    *out_order = (threshold == 0) ? G2_ORDER_EQUAL : G2_ORDER_LESS;
    return G2_OK;
  }

  G2Order best = G2_ORDER_GREATER;

  for (size_t i = 0; i < a.count; ++i) {
    size_t ai_next = (i + 1 == a.count) ? 0 : i + 1;

    for (size_t j = 0; j < b.count; ++j) {
      size_t bj_next = (j + 1 == b.count) ? 0 : j + 1;
      G2Order current;

      status = g2_compare_segment_segment_distance(
          a.vertices[i], a.vertices[ai_next], b.vertices[j],
          b.vertices[bj_next], threshold, &current);
      if (status != G2_OK)
        return status;

      if (current == G2_ORDER_LESS) {
        *out_order = G2_ORDER_LESS;
        return G2_OK;
      }

      if (current == G2_ORDER_EQUAL)
        best = G2_ORDER_EQUAL;
    }
  }

  *out_order = best;
  return G2_OK;
}

G2Status g2_compare_point_circle_distance(G2Point point, G2Circle circle,
                                          G2Nm threshold, G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  G2Status status = g2_circle_validate(circle);
  if (status != G2_OK)
    return status;

  if (!g2_point_valid(point))
    return G2_ERR_RANGE;

  if (threshold >= G2_MAX_POINT_DISTANCE_CEIL_NM) {
    *out_order = G2_ORDER_LESS;
    return G2_OK;
  }

  G2Order current;
  status = g2_compare_distance(point, circle.center, circle.radius, &current);
  if (status != G2_OK)
    return status;

  if (current != G2_ORDER_GREATER) {
    g2_set_zero_distance_order(threshold, out_order);
    return G2_OK;
  }

  G2Nm combined = circle.radius + threshold;
  return g2_compare_distance(point, circle.center, combined, out_order);
}

G2Status g2_compare_circle_circle_distance(G2Circle a, G2Circle b,
                                           G2Nm threshold, G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  G2Status status = g2_circle_validate(a);
  if (status != G2_OK)
    return status;

  status = g2_circle_validate(b);
  if (status != G2_OK)
    return status;

  if (threshold >= G2_MAX_POINT_DISTANCE_CEIL_NM) {
    *out_order = G2_ORDER_LESS;
    return G2_OK;
  }

  G2Nm radii = a.radius + b.radius;
  G2Order current;

  status = g2_compare_distance(a.center, b.center, radii, &current);
  if (status != G2_OK)
    return status;

  if (current != G2_ORDER_GREATER) {
    g2_set_zero_distance_order(threshold, out_order);
    return G2_OK;
  }

  return g2_compare_distance(a.center, b.center, radii + threshold, out_order);
}

G2Status g2_compare_segment_circle_distance(G2Point a, G2Point b,
                                            G2Circle circle, G2Nm threshold,
                                            G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  G2Status status = g2_circle_validate(circle);
  if (status != G2_OK)
    return status;

  if (threshold >= G2_MAX_POINT_DISTANCE_CEIL_NM) {
    *out_order = G2_ORDER_LESS;
    return G2_OK;
  }

  G2Order current;
  status = g2_compare_point_segment_distance(circle.center, a, b, circle.radius,
                                             &current);
  if (status != G2_OK)
    return status;

  if (current != G2_ORDER_GREATER) {
    g2_set_zero_distance_order(threshold, out_order);
    return G2_OK;
  }

  return g2_compare_point_segment_distance(
      circle.center, a, b, circle.radius + threshold, out_order);
}

G2Status g2_compare_capsule_capsule_distance(G2Capsule a, G2Capsule b,
                                             G2Nm threshold,
                                             G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  G2Status status = g2_capsule_validate(a);
  if (status != G2_OK)
    return status;

  status = g2_capsule_validate(b);
  if (status != G2_OK)
    return status;

  if (threshold >= G2_MAX_POINT_DISTANCE_CEIL_NM) {
    *out_order = G2_ORDER_LESS;
    return G2_OK;
  }

  G2Nm radii = a.radius + b.radius;
  G2Order current;

  status =
      g2_compare_segment_segment_distance(a.a, a.b, b.a, b.b, radii, &current);
  if (status != G2_OK)
    return status;

  if (current != G2_ORDER_GREATER) {
    g2_set_zero_distance_order(threshold, out_order);
    return G2_OK;
  }

  return g2_compare_segment_segment_distance(a.a, a.b, b.a, b.b,
                                             radii + threshold, out_order);
}

G2Status g2_compare_segment_polygon_distance(G2Point a, G2Point b,
                                             G2Polygon polygon, G2Nm threshold,
                                             G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  if (!g2_point_valid(a) || !g2_point_valid(b))
    return G2_ERR_RANGE;

  G2Status status = g2_polygon_validate(polygon);
  if (status != G2_OK)
    return status;

  if (threshold >= G2_MAX_POINT_DISTANCE_CEIL_NM) {
    *out_order = G2_ORDER_LESS;
    return G2_OK;
  }

  G2Containment location;
  status = g2_polygon_locate_point_validated(polygon, a, &location);
  if (status != G2_OK)
    return status;

  if (location != G2_CONTAINMENT_OUTSIDE) {
    g2_set_zero_distance_order(threshold, out_order);
    return G2_OK;
  }

  status = g2_polygon_locate_point_validated(polygon, b, &location);
  if (status != G2_OK)
    return status;

  if (location != G2_CONTAINMENT_OUTSIDE) {
    g2_set_zero_distance_order(threshold, out_order);
    return G2_OK;
  }

  G2Order best = G2_ORDER_GREATER;

  for (size_t i = 0; i < polygon.count; ++i) {
    size_t next = (i + 1 == polygon.count) ? 0 : i + 1;
    bool intersects;

    status = g2_segment_intersects(a, b, polygon.vertices[i],
                                   polygon.vertices[next], &intersects);
    if (status != G2_OK)
      return status;

    if (intersects) {
      g2_set_zero_distance_order(threshold, out_order);
      return G2_OK;
    }

    G2Order current;
    status = g2_compare_segment_segment_distance(
        a, b, polygon.vertices[i], polygon.vertices[next], threshold, &current);
    if (status != G2_OK)
      return status;

    if (current == G2_ORDER_LESS) {
      *out_order = G2_ORDER_LESS;
      return G2_OK;
    }

    if (current == G2_ORDER_EQUAL)
      best = G2_ORDER_EQUAL;
  }

  *out_order = best;
  return G2_OK;
}

G2Status g2_compare_circle_polygon_distance(G2Circle circle, G2Polygon polygon,
                                            G2Nm threshold,
                                            G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  G2Status status = g2_circle_validate(circle);
  if (status != G2_OK)
    return status;

  if (threshold >= G2_MAX_POINT_DISTANCE_CEIL_NM) {
    *out_order = G2_ORDER_LESS;
    return G2_OK;
  }

  G2Order current;
  status = g2_compare_point_polygon_distance(circle.center, polygon,
                                             circle.radius, &current);
  if (status != G2_OK)
    return status;

  if (current != G2_ORDER_GREATER) {
    g2_set_zero_distance_order(threshold, out_order);
    return G2_OK;
  }

  return g2_compare_point_polygon_distance(
      circle.center, polygon, circle.radius + threshold, out_order);
}

G2Status g2_compare_capsule_polygon_distance(G2Capsule capsule,
                                             G2Polygon polygon, G2Nm threshold,
                                             G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  G2Status status = g2_capsule_validate(capsule);
  if (status != G2_OK)
    return status;

  if (threshold >= G2_MAX_POINT_DISTANCE_CEIL_NM) {
    *out_order = G2_ORDER_LESS;
    return G2_OK;
  }

  G2Order current;
  status = g2_compare_segment_polygon_distance(capsule.a, capsule.b, polygon,
                                               capsule.radius, &current);
  if (status != G2_OK)
    return status;

  if (current != G2_ORDER_GREATER) {
    g2_set_zero_distance_order(threshold, out_order);
    return G2_OK;
  }

  return g2_compare_segment_polygon_distance(
      capsule.a, capsule.b, polygon, capsule.radius + threshold, out_order);
}

G2Status g2_compare_point_aabb_distance(G2Point point, G2Aabb box,
                                        G2Nm threshold, G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  G2Status status = g2_validate_aabb(box);
  if (status != G2_OK)
    return status;

  if (!g2_point_valid(point))
    return G2_ERR_RANGE;

  if (threshold >= G2_MAX_POINT_DISTANCE_CEIL_NM) {
    *out_order = G2_ORDER_LESS;
    return G2_OK;
  }

  uint64_t dx = 0;
  uint64_t dy = 0;

  if (point.x < box.min.x)
    dx = (uint64_t)(box.min.x - point.x);
  else if (point.x > box.max.x)
    dx = (uint64_t)(point.x - box.max.x);

  if (point.y < box.min.y)
    dy = (uint64_t)(box.min.y - point.y);
  else if (point.y > box.max.y)
    dy = (uint64_t)(point.y - box.max.y);

  uint64_t distance_squared = dx * dx + dy * dy;
  uint64_t t = (uint64_t)threshold;
  uint64_t threshold_squared = t * t;

  if (distance_squared < threshold_squared)
    *out_order = G2_ORDER_LESS;
  else if (distance_squared > threshold_squared)
    *out_order = G2_ORDER_GREATER;
  else
    *out_order = G2_ORDER_EQUAL;

  return G2_OK;
}

G2Status g2_compare_aabb_aabb_distance(G2Aabb a, G2Aabb b, G2Nm threshold,
                                       G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  G2Status status = g2_validate_aabb(a);
  if (status != G2_OK)
    return status;

  status = g2_validate_aabb(b);
  if (status != G2_OK)
    return status;

  if (threshold >= G2_MAX_POINT_DISTANCE_CEIL_NM) {
    *out_order = G2_ORDER_LESS;
    return G2_OK;
  }

  uint64_t dx = 0;
  uint64_t dy = 0;

  if (a.max.x < b.min.x)
    dx = (uint64_t)(b.min.x - a.max.x);
  else if (b.max.x < a.min.x)
    dx = (uint64_t)(a.min.x - b.max.x);

  if (a.max.y < b.min.y)
    dy = (uint64_t)(b.min.y - a.max.y);
  else if (b.max.y < a.min.y)
    dy = (uint64_t)(a.min.y - b.max.y);

  uint64_t distance_squared = dx * dx + dy * dy;
  uint64_t t = (uint64_t)threshold;
  uint64_t threshold_squared = t * t;

  if (distance_squared < threshold_squared)
    *out_order = G2_ORDER_LESS;
  else if (distance_squared > threshold_squared)
    *out_order = G2_ORDER_GREATER;
  else
    *out_order = G2_ORDER_EQUAL;

  return G2_OK;
}

G2Status g2_rounded_rect_validate(G2RoundedRect rect) {
  G2Status status = g2_validate_aabb(rect.core);
  if (status != G2_OK)
    return status;

  if (rect.radius < 0 || rect.radius > G2_COORD_MAX_NM)
    return G2_ERR_RANGE;

  G2Nm r = rect.radius;
  if (rect.core.min.x - r < -G2_COORD_MAX_NM ||
      rect.core.max.x + r > G2_COORD_MAX_NM ||
      rect.core.min.y - r < -G2_COORD_MAX_NM ||
      rect.core.max.y + r > G2_COORD_MAX_NM)
    return G2_ERR_RANGE;

  return G2_OK;
}

G2Status g2_rounded_rect_aabb(G2RoundedRect rect, G2Aabb *out_box) {
  if (out_box == NULL)
    return G2_ERR_ARGUMENT;

  G2Status status = g2_rounded_rect_validate(rect);
  if (status != G2_OK)
    return status;

  G2Nm r = rect.radius;
  G2Aabb result = {.min = {rect.core.min.x - r, rect.core.min.y - r},
                   .max = {rect.core.max.x + r, rect.core.max.y + r}};

  *out_box = result;
  return G2_OK;
}

G2Status g2_compare_point_rounded_rect_distance(G2Point point,
                                                G2RoundedRect rect,
                                                G2Nm threshold,
                                                G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  G2Status status = g2_rounded_rect_validate(rect);
  if (status != G2_OK)
    return status;

  if (threshold >= G2_MAX_POINT_DISTANCE_CEIL_NM) {
    *out_order = G2_ORDER_LESS;
    return G2_OK;
  }

  G2Order current;
  status =
      g2_compare_point_aabb_distance(point, rect.core, rect.radius, &current);
  if (status != G2_OK)
    return status;

  if (current != G2_ORDER_GREATER) {
    g2_set_zero_distance_order(threshold, out_order);
    return G2_OK;
  }

  return g2_compare_point_aabb_distance(point, rect.core,
                                        rect.radius + threshold, out_order);
}

G2Status g2_compare_circle_rounded_rect_distance(G2Circle circle,
                                                 G2RoundedRect rect,
                                                 G2Nm threshold,
                                                 G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  G2Status status = g2_circle_validate(circle);
  if (status != G2_OK)
    return status;

  status = g2_rounded_rect_validate(rect);
  if (status != G2_OK)
    return status;

  if (threshold >= G2_MAX_POINT_DISTANCE_CEIL_NM) {
    *out_order = G2_ORDER_LESS;
    return G2_OK;
  }

  G2Nm radii = circle.radius + rect.radius;
  G2Order current;

  status =
      g2_compare_point_aabb_distance(circle.center, rect.core, radii, &current);
  if (status != G2_OK)
    return status;

  if (current != G2_ORDER_GREATER) {
    g2_set_zero_distance_order(threshold, out_order);
    return G2_OK;
  }

  return g2_compare_point_aabb_distance(circle.center, rect.core,
                                        radii + threshold, out_order);
}

G2Status g2_compare_capsule_rounded_rect_distance(G2Capsule capsule,
                                                  G2RoundedRect rect,
                                                  G2Nm threshold,
                                                  G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  G2Status status = g2_capsule_validate(capsule);
  if (status != G2_OK)
    return status;

  status = g2_rounded_rect_validate(rect);
  if (status != G2_OK)
    return status;

  if (threshold >= G2_MAX_POINT_DISTANCE_CEIL_NM) {
    *out_order = G2_ORDER_LESS;
    return G2_OK;
  }

  G2Nm radii = capsule.radius + rect.radius;
  G2Order current;

  status = g2_compare_segment_aabb_distance(capsule.a, capsule.b, rect.core,
                                            radii, &current);
  if (status != G2_OK)
    return status;

  if (current != G2_ORDER_GREATER) {
    g2_set_zero_distance_order(threshold, out_order);
    return G2_OK;
  }

  return g2_compare_segment_aabb_distance(capsule.a, capsule.b, rect.core,
                                          radii + threshold, out_order);
}

G2Status g2_compare_rounded_rect_rounded_rect_distance(G2RoundedRect a,
                                                       G2RoundedRect b,
                                                       G2Nm threshold,
                                                       G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  G2Status status = g2_rounded_rect_validate(a);
  if (status != G2_OK)
    return status;

  status = g2_rounded_rect_validate(b);
  if (status != G2_OK)
    return status;

  if (threshold >= G2_MAX_POINT_DISTANCE_CEIL_NM) {
    *out_order = G2_ORDER_LESS;
    return G2_OK;
  }

  G2Nm radii = a.radius + b.radius;
  G2Order current;

  status = g2_compare_aabb_aabb_distance(a.core, b.core, radii, &current);
  if (status != G2_OK)
    return status;

  if (current != G2_ORDER_GREATER) {
    g2_set_zero_distance_order(threshold, out_order);
    return G2_OK;
  }

  return g2_compare_aabb_aabb_distance(a.core, b.core, radii + threshold,
                                       out_order);
}

G2Status g2_quad_validate(G2Quad quad) {
  G2Polygon polygon = g2_quad_as_polygon(&quad);
  G2Status status = g2_polygon_validate(polygon);
  if (status != G2_OK)
    return status;

  G2Order reference = G2_ORDER_EQUAL;

  for (size_t i = 0; i < 4; ++i) {
    size_t j = (i + 1) % 4;
    size_t k = (i + 2) % 4;
    G2Order orientation;

    status = g2_orient2d(quad.vertices[i], quad.vertices[j], quad.vertices[k],
                         &orientation);
    if (status != G2_OK)
      return status;

    if (orientation == G2_ORDER_EQUAL)
      return G2_ERR_INVALID_GEOMETRY;

    if (i == 0)
      reference = orientation;
    else if (orientation != reference)
      return G2_ERR_INVALID_GEOMETRY;
  }

  return G2_OK;
}

G2Status g2_quad_validate_rectangle(G2Quad quad) {
  G2Status status = g2_quad_validate(quad);
  if (status != G2_OK)
    return status;

  G2Point p0 = quad.vertices[0];
  G2Point p1 = quad.vertices[1];
  G2Point p2 = quad.vertices[2];
  G2Point p3 = quad.vertices[3];

  if (p0.x + p2.x != p1.x + p3.x || p0.y + p2.y != p1.y + p3.y)
    return G2_ERR_INVALID_GEOMETRY;

  G2Vector v01, v12;
  status = g2_vector_between(p0, p1, &v01);
  if (status != G2_OK)
    return status;

  status = g2_vector_between(p1, p2, &v12);
  if (status != G2_OK)
    return status;

  int64_t dot;
  status = g2_vector_dot(v01, v12, &dot);
  if (status != G2_OK)
    return status;

  return dot == 0 ? G2_OK : G2_ERR_INVALID_GEOMETRY;
}

G2Status g2_quad_aabb(G2Quad quad, G2Aabb *out_box) {
  if (out_box == NULL)
    return G2_ERR_ARGUMENT;

  G2Status status = g2_quad_validate(quad);
  if (status != G2_OK)
    return status;

  G2Aabb result = {.min = quad.vertices[0], .max = quad.vertices[0]};

  for (size_t i = 1; i < 4; ++i) {
    G2Point p = quad.vertices[i];
    if (p.x < result.min.x)
      result.min.x = p.x;
    if (p.y < result.min.y)
      result.min.y = p.y;
    if (p.x > result.max.x)
      result.max.x = p.x;
    if (p.y > result.max.y)
      result.max.y = p.y;
  }

  *out_box = result;
  return G2_OK;
}

G2Status g2_quad_intersects(G2Quad a, G2Quad b, bool *out_intersects) {
  if (out_intersects == NULL)
    return G2_ERR_ARGUMENT;

  G2Status status = g2_quad_validate(a);
  if (status != G2_OK)
    return status;

  status = g2_quad_validate(b);
  if (status != G2_OK)
    return status;

  G2Polygon pa = g2_quad_as_polygon(&a);
  G2Polygon pb = g2_quad_as_polygon(&b);

  return g2_polygon_intersects(pa, pb, out_intersects);
}

G2Status g2_compare_quad_quad_distance(G2Quad a, G2Quad b, G2Nm threshold,
                                       G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  G2Status status = g2_quad_validate(a);
  if (status != G2_OK)
    return status;

  status = g2_quad_validate(b);
  if (status != G2_OK)
    return status;

  G2Polygon pa = g2_quad_as_polygon(&a);
  G2Polygon pb = g2_quad_as_polygon(&b);

  return g2_compare_polygon_polygon_distance(pa, pb, threshold, out_order);
}

G2Status g2_region_validate(G2Region region) {
  if (region.rings == NULL || region.ring_count == 0)
    return G2_ERR_ARGUMENT;

  if (region.fill_rule != G2_FILL_EVEN_ODD &&
      region.fill_rule != G2_FILL_NON_ZERO)
    return G2_ERR_ARGUMENT;

  for (size_t i = 0; i < region.ring_count; ++i) {
    G2Status status = g2_polygon_validate(region.rings[i]);
    if (status != G2_OK)
      return status;
  }

  for (size_t i = 0; i < region.ring_count; ++i) {
    G2Polygon a = region.rings[i];

    for (size_t j = i + 1; j < region.ring_count; ++j) {
      G2Polygon b = region.rings[j];

      for (size_t ei = 0; ei < a.count; ++ei) {
        size_t ai_next = (ei + 1 == a.count) ? 0 : ei + 1;

        for (size_t ej = 0; ej < b.count; ++ej) {
          size_t bj_next = (ej + 1 == b.count) ? 0 : ej + 1;
          bool intersects;

          G2Status status = g2_segment_intersects(
              a.vertices[ei], a.vertices[ai_next], b.vertices[ej],
              b.vertices[bj_next], &intersects);
          if (status != G2_OK)
            return status;

          if (intersects)
            return G2_ERR_INVALID_GEOMETRY;
        }
      }
    }
  }

  return G2_OK;
}

static G2Status g2_region_locate_point_validated(G2Region region, G2Point point,
                                                 G2Containment *out_location) {
  bool inside_even_odd = false;
  int64_t winding = 0;

  for (size_t r = 0; r < region.ring_count; ++r) {
    G2Polygon ring = region.rings[r];

    for (size_t i = 0; i < ring.count; ++i) {
      size_t next = (i + 1 == ring.count) ? 0 : i + 1;
      G2Point a = ring.vertices[i];
      G2Point b = ring.vertices[next];

      bool on_edge;
      G2Status status = g2_point_on_segment(point, a, b, &on_edge);
      if (status != G2_OK)
        return status;

      if (on_edge) {
        *out_location = G2_CONTAINMENT_BOUNDARY;
        return G2_OK;
      }

      G2Order orientation = G2_ORDER_EQUAL;
      bool crosses_ray = (a.y > point.y) != (b.y > point.y);

      if (crosses_ray) {
        status = g2_orient2d(a, b, point, &orientation);
        if (status != G2_OK)
          return status;

        bool crosses_to_right =
            (b.y > a.y && orientation == G2_ORDER_GREATER) ||
            (b.y < a.y && orientation == G2_ORDER_LESS);

        if (region.fill_rule == G2_FILL_EVEN_ODD) {
          if (crosses_to_right)
            inside_even_odd = !inside_even_odd;
        }
      }

      if (region.fill_rule == G2_FILL_NON_ZERO) {
        if (a.y <= point.y && b.y > point.y &&
            orientation == G2_ORDER_GREATER) {
          ++winding;
        } else if (a.y > point.y && b.y <= point.y &&
                   orientation == G2_ORDER_LESS) {
          --winding;
        }
      }
    }
  }

  bool inside =
      region.fill_rule == G2_FILL_EVEN_ODD ? inside_even_odd : winding != 0;

  *out_location = inside ? G2_CONTAINMENT_INSIDE : G2_CONTAINMENT_OUTSIDE;
  return G2_OK;
}

G2Status g2_region_locate_point(G2Region region, G2Point point,
                                G2Containment *out_location) {
  if (out_location == NULL)
    return G2_ERR_ARGUMENT;

  if (!g2_point_valid(point))
    return G2_ERR_RANGE;

  G2Status status = g2_region_validate(region);
  if (status != G2_OK)
    return status;

  return g2_region_locate_point_validated(region, point, out_location);
}

G2Status g2_region_aabb(G2Region region, G2Aabb *out_box) {
  if (out_box == NULL)
    return G2_ERR_ARGUMENT;

  G2Status status = g2_region_validate(region);
  if (status != G2_OK)
    return status;

  G2Aabb result = {.min = region.rings[0].vertices[0],
                   .max = region.rings[0].vertices[0]};

  for (size_t r = 0; r < region.ring_count; ++r) {
    G2Polygon ring = region.rings[r];

    for (size_t i = 0; i < ring.count; ++i) {
      G2Point p = ring.vertices[i];
      if (p.x < result.min.x)
        result.min.x = p.x;
      if (p.y < result.min.y)
        result.min.y = p.y;
      if (p.x > result.max.x)
        result.max.x = p.x;
      if (p.y > result.max.y)
        result.max.y = p.y;
    }
  }

  *out_box = result;
  return G2_OK;
}

G2Status g2_region_intersects(G2Region a, G2Region b, bool *out_intersects) {
  if (out_intersects == NULL)
    return G2_ERR_ARGUMENT;

  G2Status status = g2_region_validate(a);
  if (status != G2_OK)
    return status;

  status = g2_region_validate(b);
  if (status != G2_OK)
    return status;

  G2Aabb bounds_a, bounds_b;
  bool bounds_overlap;

  status = g2_region_aabb(a, &bounds_a);
  if (status != G2_OK)
    return status;

  status = g2_region_aabb(b, &bounds_b);
  if (status != G2_OK)
    return status;

  status = g2_aabb_intersects(bounds_a, bounds_b, &bounds_overlap);
  if (status != G2_OK)
    return status;

  if (!bounds_overlap) {
    *out_intersects = false;
    return G2_OK;
  }

  for (size_t ra = 0; ra < a.ring_count; ++ra) {
    G2Polygon pa = a.rings[ra];

    for (size_t rb = 0; rb < b.ring_count; ++rb) {
      G2Polygon pb = b.rings[rb];

      for (size_t i = 0; i < pa.count; ++i) {
        size_t inext = (i + 1 == pa.count) ? 0 : i + 1;

        for (size_t j = 0; j < pb.count; ++j) {
          size_t jnext = (j + 1 == pb.count) ? 0 : j + 1;
          bool intersects;

          status = g2_segment_intersects(pa.vertices[i], pa.vertices[inext],
                                         pb.vertices[j], pb.vertices[jnext],
                                         &intersects);
          if (status != G2_OK)
            return status;

          if (intersects) {
            *out_intersects = true;
            return G2_OK;
          }
        }
      }
    }
  }

  G2Containment location;
  for (size_t r = 0; r < a.ring_count; ++r) {
    for (size_t i = 0; i < a.rings[r].count; ++i) {
      status = g2_region_locate_point_validated(b, a.rings[r].vertices[i],
                                                &location);
      if (status != G2_OK)
        return status;

      if (location != G2_CONTAINMENT_OUTSIDE) {
        *out_intersects = true;
        return G2_OK;
      }
    }
  }

  for (size_t r = 0; r < b.ring_count; ++r) {
    for (size_t i = 0; i < b.rings[r].count; ++i) {
      status = g2_region_locate_point_validated(a, b.rings[r].vertices[i],
                                                &location);
      if (status != G2_OK)
        return status;

      if (location != G2_CONTAINMENT_OUTSIDE) {
        *out_intersects = true;
        return G2_OK;
      }
    }
  }

  *out_intersects = false;
  return G2_OK;
}

G2Status g2_compare_point_region_distance(G2Point point, G2Region region,
                                          G2Nm threshold, G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  if (!g2_point_valid(point))
    return G2_ERR_RANGE;

  G2Status status = g2_region_validate(region);
  if (status != G2_OK)
    return status;

  G2Containment location;
  status = g2_region_locate_point_validated(region, point, &location);
  if (status != G2_OK)
    return status;

  if (location != G2_CONTAINMENT_OUTSIDE) {
    g2_set_zero_distance_order(threshold, out_order);
    return G2_OK;
  }

  G2Order best = G2_ORDER_GREATER;

  for (size_t r = 0; r < region.ring_count; ++r) {
    G2Polygon ring = region.rings[r];

    for (size_t i = 0; i < ring.count; ++i) {
      size_t next = (i + 1 == ring.count) ? 0 : i + 1;
      G2Order current;

      status = g2_compare_point_segment_distance(
          point, ring.vertices[i], ring.vertices[next], threshold, &current);
      if (status != G2_OK)
        return status;

      if (current == G2_ORDER_LESS) {
        *out_order = G2_ORDER_LESS;
        return G2_OK;
      }

      if (current == G2_ORDER_EQUAL)
        best = G2_ORDER_EQUAL;
    }
  }

  *out_order = best;
  return G2_OK;
}

G2Status g2_compare_region_region_distance(G2Region a, G2Region b,
                                           G2Nm threshold, G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  bool intersects;
  G2Status status = g2_region_intersects(a, b, &intersects);
  if (status != G2_OK)
    return status;

  if (intersects) {
    g2_set_zero_distance_order(threshold, out_order);
    return G2_OK;
  }

  G2Order best = G2_ORDER_GREATER;

  for (size_t ra = 0; ra < a.ring_count; ++ra) {
    G2Polygon pa = a.rings[ra];

    for (size_t rb = 0; rb < b.ring_count; ++rb) {
      G2Polygon pb = b.rings[rb];

      for (size_t i = 0; i < pa.count; ++i) {
        size_t inext = (i + 1 == pa.count) ? 0 : i + 1;

        for (size_t j = 0; j < pb.count; ++j) {
          size_t jnext = (j + 1 == pb.count) ? 0 : j + 1;
          G2Order current;

          status = g2_compare_segment_segment_distance(
              pa.vertices[i], pa.vertices[inext], pb.vertices[j],
              pb.vertices[jnext], threshold, &current);
          if (status != G2_OK)
            return status;

          if (current == G2_ORDER_LESS) {
            *out_order = G2_ORDER_LESS;
            return G2_OK;
          }

          if (current == G2_ORDER_EQUAL)
            best = G2_ORDER_EQUAL;
        }
      }
    }
  }

  *out_order = best;
  return G2_OK;
}

G2Status g2_arc_contains_direction(G2Arc arc, G2Vector direction,
                                   bool *out_contains) {
  if (out_contains == NULL)
    return G2_ERR_ARGUMENT;

  G2Status status = g2_arc_validate(arc);
  if (status != G2_OK)
    return status;

  return g2_arc_contains_direction_validated(arc, direction, out_contains);
}

G2Status g2_arc_validate(G2Arc arc) {
  if (!g2_point_valid(arc.center) || !g2_point_valid(arc.start) ||
      !g2_point_valid(arc.end))
    return G2_ERR_RANGE;

  if (arc.direction != G2_ARC_CCW && arc.direction != G2_ARC_CW)
    return G2_ERR_ARGUMENT;

  if (arc.center.x == arc.start.x && arc.center.y == arc.start.y)
    return G2_ERR_INVALID_GEOMETRY;

  bool same_endpoint = arc.start.x == arc.end.x && arc.start.y == arc.end.y;

  if (arc.full_circle) {
    if (!same_endpoint)
      return G2_ERR_INVALID_GEOMETRY;
  } else {
    if (same_endpoint)
      return G2_ERR_INVALID_GEOMETRY;

    if (arc.center.x == arc.end.x && arc.center.y == arc.end.y)
      return G2_ERR_INVALID_GEOMETRY;
  }

  G2Vector start, end;
  G2Status status = g2_vector_between(arc.center, arc.start, &start);
  if (status != G2_OK)
    return status;

  status = g2_vector_between(arc.center, arc.end, &end);
  if (status != G2_OK)
    return status;

  uint64_t r2_start, r2_end;
  status = g2_distance_squared(arc.center, arc.start, &r2_start);
  if (status != G2_OK)
    return status;

  status = g2_distance_squared(arc.center, arc.end, &r2_end);
  if (status != G2_OK)
    return status;

  if (r2_start != r2_end)
    return G2_ERR_INVALID_GEOMETRY;

  uint64_t radius_floor = nm_isqrt_u64(r2_start);
  uint64_t radius_ceil = radius_floor;

  if (radius_floor * radius_floor != r2_start)
    ++radius_ceil;

  G2Nm r = (G2Nm)radius_ceil;
  if (arc.center.x - r < -G2_COORD_MAX_NM ||
      arc.center.x + r > G2_COORD_MAX_NM ||
      arc.center.y - r < -G2_COORD_MAX_NM || arc.center.y + r > G2_COORD_MAX_NM)
    return G2_ERR_RANGE;

  return G2_OK;
}

G2Status g2_arc_aabb(G2Arc arc, G2Aabb *out_box) {
  if (out_box == NULL)
    return G2_ERR_ARGUMENT;

  G2Status status = g2_arc_validate(arc);
  if (status != G2_OK)
    return status;

  uint64_t r2;
  status = g2_distance_squared(arc.center, arc.start, &r2);
  if (status != G2_OK)
    return status;

  uint64_t radius = nm_isqrt_u64(r2);
  if (radius * radius != r2)
    ++radius;

  G2Nm r = (G2Nm)radius;
  G2Aabb result = {.min = {arc.center.x - r, arc.center.y - r},
                   .max = {arc.center.x + r, arc.center.y + r}};

  *out_box = result;
  return G2_OK;
}

G2Status g2_compare_point_arc_distance(G2Point point, G2Arc arc, G2Nm threshold,
                                       G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  if (!g2_point_valid(point))
    return G2_ERR_RANGE;

  G2Status status = g2_arc_validate(arc);
  if (status != G2_OK)
    return status;

  if (threshold >= G2_MAX_POINT_DISTANCE_CEIL_NM) {
    *out_order = G2_ORDER_LESS;
    return G2_OK;
  }

  uint64_t radius_squared;
  uint64_t point_squared;

  status = g2_distance_squared(arc.center, arc.start, &radius_squared);
  if (status != G2_OK)
    return status;

  status = g2_distance_squared(arc.center, point, &point_squared);
  if (status != G2_OK)
    return status;

  G2Vector radial;
  status = g2_vector_between(arc.center, point, &radial);
  if (status != G2_OK)
    return status;

  if (radial.dx == 0 && radial.dy == 0) {
    return g2_compare_sqrt_difference(radius_squared, 0, (uint64_t)threshold,
                                      out_order);
  }

  bool radial_in_arc = false;
  status = g2_arc_contains_direction_validated(arc, radial, &radial_in_arc);
  if (status != G2_OK)
    return status;

  if (radial_in_arc) {
    return g2_compare_sqrt_difference(point_squared, radius_squared,
                                      (uint64_t)threshold, out_order);
  }

  G2Order first, second;
  status = g2_compare_distance(point, arc.start, threshold, &first);
  if (status != G2_OK)
    return status;

  status = g2_compare_distance(point, arc.end, threshold, &second);
  if (status != G2_OK)
    return status;

  if (first == G2_ORDER_LESS || second == G2_ORDER_LESS)
    *out_order = G2_ORDER_LESS;
  else if (first == G2_ORDER_EQUAL || second == G2_ORDER_EQUAL)
    *out_order = G2_ORDER_EQUAL;
  else
    *out_order = G2_ORDER_GREATER;

  return G2_OK;
}

G2Status g2_bvh_build(Arena *arena, const G2Aabb *bounds, size_t count,
                      size_t leaf_capacity, G2Bvh *out_tree) {
  if (arena == NULL || bounds == NULL || out_tree == NULL || count == 0)
    return G2_ERR_ARGUMENT;

  if (count > SIZE_MAX / 2 + 1)
    return G2_ERR_OVERFLOW;

  if (leaf_capacity == 0)
    leaf_capacity = 4;

  G2Bvh tree = {0};
  tree.items = arena_alloc_array(arena, count, sizeof(G2BvhItem));
  tree.nodes = arena_alloc_array(arena, 2 * count - 1, sizeof(G2BvhNode));

  if (tree.items == NULL || tree.nodes == NULL)
    return G2_ERR_MEMORY;

  tree.item_count = count;
  tree.node_count = 0;

  for (size_t i = 0; i < count; ++i) {
    G2Status status = g2_validate_aabb(bounds[i]);
    if (status != G2_OK)
      return status;

    G2BvhItem item = {
        .bounds = bounds[i],
        .object_id = i,
        .center_x = bounds[i].min.x + (bounds[i].max.x - bounds[i].min.x) / 2,
        .center_y = bounds[i].min.y + (bounds[i].max.y - bounds[i].min.y) / 2};

    tree.items[i] = item;
  }

  G2Status status =
      g2_bvh_build_node(&tree, 0, count, leaf_capacity, &tree.root);
  if (status != G2_OK)
    return status;

  *out_tree = tree;
  return G2_OK;
}

G2Status g2_bvh_query(const G2Bvh *tree, G2Aabb query, size_t *out_ids,
                      size_t capacity, size_t *out_count) {
  if (tree == NULL || out_count == NULL || tree->nodes == NULL ||
      tree->items == NULL || tree->node_count == 0 ||
      tree->root >= tree->node_count)
    return G2_ERR_ARGUMENT;

  if (capacity != 0 && out_ids == NULL)
    return G2_ERR_ARGUMENT;

  G2Status status = g2_validate_aabb(query);
  if (status != G2_OK)
    return status;

  size_t stack[sizeof(size_t) * 8 + 1];
  size_t top = 0;

  stack[top++] = tree->root;

  size_t found = 0;
  bool capacity_exceeded = false;

  while (top != 0) {
    size_t node_id = stack[--top];

    if (node_id >= tree->node_count)
      return G2_ERR_INVALID_GEOMETRY;

    const G2BvhNode *node = &tree->nodes[node_id];
    bool overlaps;

    status = g2_aabb_intersects(node->bounds, query, &overlaps);
    if (status != G2_OK)
      return status;

    if (!overlaps)
      continue;

    if (node->is_leaf) {
      if (node->first > tree->item_count ||
          node->count > tree->item_count - node->first)
        return G2_ERR_INVALID_GEOMETRY;

      for (size_t i = 0; i < node->count; ++i) {
        const G2BvhItem *item = &tree->items[node->first + i];

        status = g2_aabb_intersects(item->bounds, query, &overlaps);
        if (status != G2_OK)
          return status;

        if (!overlaps)
          continue;

        if (found < capacity)
          out_ids[found] = item->object_id;
        else
          capacity_exceeded = true;

        if (found == SIZE_MAX)
          return G2_ERR_OVERFLOW;

        ++found;
      }
    } else {
      if (node->left >= tree->node_count || node->right >= tree->node_count)
        return G2_ERR_INVALID_GEOMETRY;

      if (top + 2 > sizeof(stack) / sizeof(stack[0]))
        return G2_ERR_OVERFLOW;

      stack[top++] = node->left;
      stack[top++] = node->right;
    }
  }

  *out_count = found;
  return capacity_exceeded ? G2_ERR_CAPACITY : G2_OK;
}

G2Status g2_shape_validate(G2Shape shape) {
  switch (shape.kind) {
  case G2_SHAPE_SEGMENT:
    if (!g2_point_valid(shape.as.segment.a) ||
        !g2_point_valid(shape.as.segment.b))
      return G2_ERR_RANGE;
    return G2_OK;

  case G2_SHAPE_CIRCLE:
    return g2_circle_validate(shape.as.circle);

  case G2_SHAPE_CAPSULE:
    return g2_capsule_validate(shape.as.capsule);

  case G2_SHAPE_QUAD:
    return g2_quad_validate(shape.as.quad);

  case G2_SHAPE_POLYGON:
    return g2_polygon_validate(shape.as.polygon);

  case G2_SHAPE_REGION:
    return g2_region_validate(shape.as.region);

  case G2_SHAPE_ARC:
    return g2_arc_validate(shape.as.arc);

  case G2_SHAPE_ROUNDED_RECT:
    return g2_rounded_rect_validate(shape.as.rounded_rect);

  default:
    return G2_ERR_ARGUMENT;
  }
}

static G2Status g2_polygon_aabb(G2Polygon polygon, G2Aabb *out_box) {
  if (out_box == NULL)
    return G2_ERR_ARGUMENT;

  G2Status status = g2_polygon_validate(polygon);
  if (status != G2_OK)
    return status;

  G2Aabb result = {.min = polygon.vertices[0], .max = polygon.vertices[0]};

  for (size_t i = 1; i < polygon.count; ++i) {
    G2Point p = polygon.vertices[i];
    if (p.x < result.min.x)
      result.min.x = p.x;
    if (p.y < result.min.y)
      result.min.y = p.y;
    if (p.x > result.max.x)
      result.max.x = p.x;
    if (p.y > result.max.y)
      result.max.y = p.y;
  }

  *out_box = result;
  return G2_OK;
}

G2Status g2_shape_aabb(G2Shape shape, G2Aabb *out_box) {
  if (out_box == NULL)
    return G2_ERR_ARGUMENT;

  G2Status status = g2_shape_validate(shape);
  if (status != G2_OK)
    return status;

  G2Aabb result;

  switch (shape.kind) {
  case G2_SHAPE_SEGMENT: {
    G2Point a = shape.as.segment.a;
    G2Point b = shape.as.segment.b;

    result.min.x = a.x < b.x ? a.x : b.x;
    result.min.y = a.y < b.y ? a.y : b.y;
    result.max.x = a.x > b.x ? a.x : b.x;
    result.max.y = a.y > b.y ? a.y : b.y;
    break;
  }

  case G2_SHAPE_CIRCLE:
    status = g2_circle_aabb(shape.as.circle, &result);
    if (status != G2_OK)
      return status;
    break;

  case G2_SHAPE_CAPSULE:
    status = g2_capsule_aabb(shape.as.capsule, &result);
    if (status != G2_OK)
      return status;
    break;

  case G2_SHAPE_QUAD:
    status = g2_quad_aabb(shape.as.quad, &result);
    if (status != G2_OK)
      return status;
    break;

  case G2_SHAPE_POLYGON:
    status = g2_polygon_aabb(shape.as.polygon, &result);
    if (status != G2_OK)
      return status;
    break;

  case G2_SHAPE_REGION:
    status = g2_region_aabb(shape.as.region, &result);
    if (status != G2_OK)
      return status;
    break;

  case G2_SHAPE_ARC:
    status = g2_arc_aabb(shape.as.arc, &result);
    if (status != G2_OK)
      return status;
    break;

  case G2_SHAPE_ROUNDED_RECT:
    status = g2_rounded_rect_aabb(shape.as.rounded_rect, &result);
    if (status != G2_OK)
      return status;
    break;

  default:
    return G2_ERR_ARGUMENT;
  }

  *out_box = result;
  return G2_OK;
}

static G2Status g2_compare_segment_region_distance(G2Point a, G2Point b,
                                                   G2Region region,
                                                   G2Nm threshold,
                                                   G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  if (!g2_point_valid(a) || !g2_point_valid(b))
    return G2_ERR_RANGE;

  G2Status status = g2_region_validate(region);
  if (status != G2_OK)
    return status;

  G2Containment location;
  status = g2_region_locate_point_validated(region, a, &location);
  if (status != G2_OK)
    return status;

  if (location != G2_CONTAINMENT_OUTSIDE) {
    g2_set_zero_distance_order(threshold, out_order);
    return G2_OK;
  }

  status = g2_region_locate_point_validated(region, b, &location);
  if (status != G2_OK)
    return status;

  if (location != G2_CONTAINMENT_OUTSIDE) {
    g2_set_zero_distance_order(threshold, out_order);
    return G2_OK;
  }

  G2Order best = G2_ORDER_GREATER;

  for (size_t r = 0; r < region.ring_count; ++r) {
    G2Polygon ring = region.rings[r];

    for (size_t i = 0; i < ring.count; ++i) {
      size_t next = (i + 1 == ring.count) ? 0 : i + 1;
      bool intersects;

      status = g2_segment_intersects(a, b, ring.vertices[i],
                                     ring.vertices[next], &intersects);
      if (status != G2_OK)
        return status;

      if (intersects) {
        g2_set_zero_distance_order(threshold, out_order);
        return G2_OK;
      }

      G2Order current;
      status = g2_compare_segment_segment_distance(
          a, b, ring.vertices[i], ring.vertices[next], threshold, &current);
      if (status != G2_OK)
        return status;

      if (current == G2_ORDER_LESS) {
        *out_order = G2_ORDER_LESS;
        return G2_OK;
      }

      if (current == G2_ORDER_EQUAL)
        best = G2_ORDER_EQUAL;
    }
  }

  *out_order = best;
  return G2_OK;
}

static G2Status g2_compare_circle_region_distance(G2Circle circle,
                                                  G2Region region,
                                                  G2Nm threshold,
                                                  G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  G2Status status = g2_circle_validate(circle);
  if (status != G2_OK)
    return status;

  status = g2_region_validate(region);
  if (status != G2_OK)
    return status;

  if (threshold >= G2_MAX_POINT_DISTANCE_CEIL_NM) {
    *out_order = G2_ORDER_LESS;
    return G2_OK;
  }

  G2Order current;
  status = g2_compare_point_region_distance(circle.center, region,
                                            circle.radius, &current);
  if (status != G2_OK)
    return status;

  if (current != G2_ORDER_GREATER) {
    g2_set_zero_distance_order(threshold, out_order);
    return G2_OK;
  }

  return g2_compare_point_region_distance(circle.center, region,
                                          circle.radius + threshold, out_order);
}

static G2Status g2_compare_capsule_region_distance(G2Capsule capsule,
                                                   G2Region region,
                                                   G2Nm threshold,
                                                   G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  G2Status status = g2_capsule_validate(capsule);
  if (status != G2_OK)
    return status;

  status = g2_region_validate(region);
  if (status != G2_OK)
    return status;

  if (threshold >= G2_MAX_POINT_DISTANCE_CEIL_NM) {
    *out_order = G2_ORDER_LESS;
    return G2_OK;
  }

  G2Order current;
  status = g2_compare_segment_region_distance(capsule.a, capsule.b, region,
                                              capsule.radius, &current);
  if (status != G2_OK)
    return status;

  if (current != G2_ORDER_GREATER) {
    g2_set_zero_distance_order(threshold, out_order);
    return G2_OK;
  }

  return g2_compare_segment_region_distance(
      capsule.a, capsule.b, region, capsule.radius + threshold, out_order);
}

static G2Status g2_compare_circle_capsule_distance(G2Circle circle,
                                                   G2Capsule capsule,
                                                   G2Nm threshold,
                                                   G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  G2Status status = g2_circle_validate(circle);
  if (status != G2_OK)
    return status;

  status = g2_capsule_validate(capsule);
  if (status != G2_OK)
    return status;

  if (threshold >= G2_MAX_POINT_DISTANCE_CEIL_NM) {
    *out_order = G2_ORDER_LESS;
    return G2_OK;
  }

  G2Nm combined_radius = circle.radius + capsule.radius;
  G2Order current;

  status = g2_compare_point_segment_distance(
      circle.center, capsule.a, capsule.b, combined_radius, &current);
  if (status != G2_OK)
    return status;

  if (current != G2_ORDER_GREATER) {
    g2_set_zero_distance_order(threshold, out_order);
    return G2_OK;
  }

  return g2_compare_point_segment_distance(circle.center, capsule.a, capsule.b,
                                           combined_radius + threshold,
                                           out_order);
}

G2Status g2_compare_shape_distance(G2Shape a, G2Shape b, G2Nm threshold,
                                   G2Order *out_order) {
  if (out_order == NULL)
    return G2_ERR_ARGUMENT;

  if (threshold < 0)
    return G2_ERR_RANGE;

  G2Status status = g2_shape_validate(a);
  if (status != G2_OK)
    return status;

  status = g2_shape_validate(b);
  if (status != G2_OK)
    return status;

  if (a.kind > b.kind) {
    G2Shape temp = a;
    a = b;
    b = temp;
  }

  G2Polygon pa, pb;

  switch (a.kind) {
  case G2_SHAPE_SEGMENT: {
    G2Point sa = a.as.segment.a;
    G2Point sb = a.as.segment.b;

    switch (b.kind) {
    case G2_SHAPE_SEGMENT:
      return g2_compare_segment_segment_distance(
          sa, sb, b.as.segment.a, b.as.segment.b, threshold, out_order);

    case G2_SHAPE_CIRCLE:
      return g2_compare_segment_circle_distance(sa, sb, b.as.circle, threshold,
                                                out_order);

    case G2_SHAPE_CAPSULE: {
      G2Capsule segment_as_capsule = {.a = sa, .b = sb, .radius = 0};
      return g2_compare_capsule_capsule_distance(
          segment_as_capsule, b.as.capsule, threshold, out_order);
    }

    case G2_SHAPE_QUAD:
      pa = g2_quad_as_polygon(&b.as.quad);
      return g2_compare_segment_polygon_distance(sa, sb, pa, threshold,
                                                 out_order);

    case G2_SHAPE_POLYGON:
      return g2_compare_segment_polygon_distance(sa, sb, b.as.polygon,
                                                 threshold, out_order);

    case G2_SHAPE_REGION:
      return g2_compare_segment_region_distance(sa, sb, b.as.region, threshold,
                                                out_order);

    case G2_SHAPE_ROUNDED_RECT: {
      G2Capsule segment_as_capsule = {.a = sa, .b = sb, .radius = 0};
      return g2_compare_capsule_rounded_rect_distance(
          segment_as_capsule, b.as.rounded_rect, threshold, out_order);
    }

    default:
      return G2_ERR_UNSUPPORTED;
    }
  }

  case G2_SHAPE_CIRCLE: {
    G2Circle circle = a.as.circle;

    switch (b.kind) {
    case G2_SHAPE_CIRCLE:
      return g2_compare_circle_circle_distance(circle, b.as.circle, threshold,
                                               out_order);

    case G2_SHAPE_CAPSULE:
      return g2_compare_circle_capsule_distance(circle, b.as.capsule, threshold,
                                                out_order);

    case G2_SHAPE_QUAD:
      pb = g2_quad_as_polygon(&b.as.quad);
      return g2_compare_circle_polygon_distance(circle, pb, threshold,
                                                out_order);

    case G2_SHAPE_POLYGON:
      return g2_compare_circle_polygon_distance(circle, b.as.polygon, threshold,
                                                out_order);

    case G2_SHAPE_REGION:
      return g2_compare_circle_region_distance(circle, b.as.region, threshold,
                                               out_order);

    case G2_SHAPE_ROUNDED_RECT:
      return g2_compare_circle_rounded_rect_distance(circle, b.as.rounded_rect,
                                                     threshold, out_order);

    default:
      return G2_ERR_UNSUPPORTED;
    }
  }

  case G2_SHAPE_CAPSULE: {
    G2Capsule capsule = a.as.capsule;

    switch (b.kind) {
    case G2_SHAPE_CAPSULE:
      return g2_compare_capsule_capsule_distance(capsule, b.as.capsule,
                                                 threshold, out_order);

    case G2_SHAPE_QUAD:
      pb = g2_quad_as_polygon(&b.as.quad);
      return g2_compare_capsule_polygon_distance(capsule, pb, threshold,
                                                 out_order);

    case G2_SHAPE_POLYGON:
      return g2_compare_capsule_polygon_distance(capsule, b.as.polygon,
                                                 threshold, out_order);

    case G2_SHAPE_REGION:
      return g2_compare_capsule_region_distance(capsule, b.as.region, threshold,
                                                out_order);

    case G2_SHAPE_ROUNDED_RECT:
      return g2_compare_capsule_rounded_rect_distance(
          capsule, b.as.rounded_rect, threshold, out_order);

    default:
      return G2_ERR_UNSUPPORTED;
    }
  }

  case G2_SHAPE_QUAD: {
    G2Quad quad = a.as.quad;
    pa = g2_quad_as_polygon(&quad);

    switch (b.kind) {
    case G2_SHAPE_QUAD:
      return g2_compare_quad_quad_distance(quad, b.as.quad, threshold,
                                           out_order);

    case G2_SHAPE_POLYGON:
      return g2_compare_polygon_polygon_distance(pa, b.as.polygon, threshold,
                                                 out_order);

    default:
      return G2_ERR_UNSUPPORTED;
    }
  }

  case G2_SHAPE_POLYGON: {
    G2Polygon polygon = a.as.polygon;

    switch (b.kind) {
    case G2_SHAPE_POLYGON:
      return g2_compare_polygon_polygon_distance(polygon, b.as.polygon,
                                                 threshold, out_order);

    default:
      return G2_ERR_UNSUPPORTED;
    }
  }

  case G2_SHAPE_REGION: {
    switch (b.kind) {
    case G2_SHAPE_REGION:
      return g2_compare_region_region_distance(a.as.region, b.as.region,
                                               threshold, out_order);

    default:
      return G2_ERR_UNSUPPORTED;
    }
  }

  case G2_SHAPE_ARC:
    return G2_ERR_UNSUPPORTED;

  case G2_SHAPE_ROUNDED_RECT:
    if (b.kind == G2_SHAPE_ROUNDED_RECT) {
      return g2_compare_rounded_rect_rounded_rect_distance(
          a.as.rounded_rect, b.as.rounded_rect, threshold, out_order);
    }
    return G2_ERR_UNSUPPORTED;

  default:
    return G2_ERR_ARGUMENT;
  }
}

G2Status g2_shapes_intersect(G2Shape a, G2Shape b, bool *out_intersects) {
  if (out_intersects == NULL)
    return G2_ERR_ARGUMENT;

  G2Order order;
  G2Status status = g2_compare_shape_distance(a, b, 0, &order);
  if (status != G2_OK)
    return status;

  *out_intersects = order != G2_ORDER_GREATER;
  return G2_OK;
}

G2Status g2_bvh_build_shapes(Arena *arena, const G2Shape *shapes, size_t count,
                             size_t leaf_capacity, G2Bvh *out_tree) {
  if (arena == NULL || shapes == NULL || out_tree == NULL || count == 0)
    return G2_ERR_ARGUMENT;

  G2Aabb *bounds = arena_alloc_array(arena, count, sizeof(G2Aabb));
  if (bounds == NULL)
    return G2_ERR_MEMORY;

  for (size_t i = 0; i < count; ++i) {
    G2Status status = g2_shape_aabb(shapes[i], &bounds[i]);
    if (status != G2_OK)
      return status;
  }

  return g2_bvh_build(arena, bounds, count, leaf_capacity, out_tree);
}

G2Status g2_bvh_query_shape(const G2Bvh *tree, G2Shape query, size_t *out_ids,
                            size_t capacity, size_t *out_count) {
  G2Aabb bounds;
  G2Status status = g2_shape_aabb(query, &bounds);
  if (status != G2_OK)
    return status;

  return g2_bvh_query(tree, bounds, out_ids, capacity, out_count);
}

/* Example Usage Function (Encapsulated properly) */
void g2_bvh_usage_example(const G2Shape *shapes, size_t shape_count,
                          G2Shape query_shape, G2Nm required_clearance_nm) {
  Arena geometry_arena;
  arena_init(&geometry_arena, 64 * 1024);

  G2Bvh tree;
  G2Status status =
      g2_bvh_build_shapes(&geometry_arena, shapes, shape_count, 4, &tree);

  if (status == G2_OK) {
    size_t candidates[256];
    size_t candidate_count = 0;

    status = g2_bvh_query_shape(&tree, query_shape, candidates, 256,
                                &candidate_count);

    if (status == G2_OK) {
      for (size_t i = 0; i < candidate_count; ++i) {
        G2Order clearance;
        G2Status result =
            g2_compare_shape_distance(query_shape, shapes[candidates[i]],
                                      required_clearance_nm, &clearance);

        if (result == G2_ERR_UNSUPPORTED) {
          continue;
        }

        if (result != G2_OK) {
          continue;
        }

        /* Interpret the exact comparison result here */
      }
    }
  }

  arena_destroy(&geometry_arena);
}
