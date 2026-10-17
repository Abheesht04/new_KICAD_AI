#include "xForm.h"

static const Mat2 ROT_TABLE[4] = {
    {1, 0, 0, 1},   //   0
    {0, -1, 1, 0},  //  90 CCW
    {-1, 0, 0, -1}, // 180
    {0, 1, -1, 0},  // 270 CCW
};
static const Mat2 FLIP_Y = {1, 0, 0, -1};
static const Mat2 MIRROR_X = {-1, 0, 0, 1};
static const Mat2 IDENTITY = {1, 0, 0, 1};

Mat2 mat2_mul(Mat2 x, Mat2 y) {
  return (Mat2){x.a * y.a + x.b * y.c, x.a * y.b + x.b * y.d,
                x.c * y.a + x.d * y.c, x.c * y.b + x.d * y.d};
}

Point2 mat2_apply(Mat2 m, Point2 p) {
  return (Point2){m.a * p.x + m.b * p.y, m.c * p.x + m.d * p.y};
}

Placement placement_from_symbol(double at_x, double at_y, int rotation_deg,
                                int mirrored_x) {
  int idx = (rotation_deg / 90) % 4;
  Mat2 mirror = mirrored_x ? MIRROR_X : IDENTITY;
  // Composition order, innermost first: FlipY, then Mirror, then Rotate.
  Mat2 m = mat2_mul(ROT_TABLE[idx], mat2_mul(mirror, FLIP_Y));
  return (Placement){m, at_x, at_y};
}

Point2 placement_apply(Placement p, Point2 lib_point) {
  Point2 r = mat2_apply(p.m, lib_point);
  return (Point2){r.x + p.tx, r.y + p.ty};
}
