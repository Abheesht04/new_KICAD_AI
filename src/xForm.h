#pragma once

#ifndef XFORM_H
#define XFORM_H

typedef struct {
  double a, b, c, d;
} Mat2;
typedef struct {
  double x, y;
} Point2;
typedef struct {
  Mat2 m;
  double tx, ty;
} Placement;

Mat2 mat2_mul(Mat2 x, Mat2 y);
Point2 mat2_apply(Mat2 m, Point2 p);

Placement placement_from_symbol(double at_x, double at_y, int rotation_deg,
                                int mirrored_x);
Point2 placement_apply(Placement p, Point2 lib_point);

#endif
