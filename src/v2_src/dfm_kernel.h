#ifndef COPPER_V2_DFM_KERNEL_H
#define COPPER_V2_DFM_KERNEL_H

/*
 * dfm_kernel.h - the exact clearance predicate shared by the CPU reference (dfm_cpu_ref.c) and the CUDA kernel
 * (cuda/dfm_gpu.cu), Copper V2 track H. Both compile this same code, so they decide every pair identically: the GPU
 * result must equal the CPU reference bit for bit.
 *
 * A track is a capsule: segment A-B in integer nm (|coordinate| <= 1e9) swept by radius r. Two tracks are too close
 * when the distance between their segments is below limit + r1 + r2 (equal passes, as in dfm.c). The test is exact:
 * integer orientation tests for crossing segments, then point-to-segment distances compared without a square root,
 *   perpendicular distance^2 < T^2  <=>  cross(d, v)^2 < T^2 * |d|^2,
 * in unsigned 128-bit (the products reach ~1e38; T is capped at 3e9 nm). 128-bit integers are a GCC/Clang extension
 * on the host and supported in CUDA device code since CUDA 11.5.
 */

#include <stdbool.h>
#include <stdint.h>

#if defined(__CUDACC__)
#define COPPER_HD __host__ __device__ __forceinline__
#else
#define COPPER_HD static inline
#endif

#define DFM_KERNEL_MAX_T INT64_C(3000000000)   /* the largest limit + r1 + r2 the predicate accepts */

#if defined(__CUDACC__) || defined(__SIZEOF_INT128__)
#define DFM_KERNEL_HAVE_I128 1
#ifdef __cplusplus
typedef unsigned __int128 DfmU128;
#else
__extension__ typedef unsigned __int128 DfmU128;
#endif
#else
#define DFM_KERNEL_HAVE_I128 0
#endif

COPPER_HD int64_t dfm_k_cross(int64_t ax, int64_t ay, int64_t bx, int64_t by, int64_t cx, int64_t cy)
{
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);   /* |deltas| <= 2e9: each product <= 4e18 */
}

COPPER_HD int dfm_k_sign(int64_t v) { return (v > 0) - (v < 0); }

COPPER_HD bool dfm_k_on_segment(int64_t ax, int64_t ay, int64_t bx, int64_t by, int64_t px, int64_t py)
{
    return (px >= (ax < bx ? ax : bx)) && (px <= (ax < bx ? bx : ax)) && (py >= (ay < by ? ay : by)) && (py <= (ay < by ? by : ay));
}

/* Do segments a-b and c-d touch or cross? */
COPPER_HD bool dfm_k_segments_meet(int64_t ax, int64_t ay, int64_t bx, int64_t by, int64_t cx, int64_t cy, int64_t dx, int64_t dy)
{
    int o1 = dfm_k_sign(dfm_k_cross(ax, ay, bx, by, cx, cy)), o2 = dfm_k_sign(dfm_k_cross(ax, ay, bx, by, dx, dy));
    int o3 = dfm_k_sign(dfm_k_cross(cx, cy, dx, dy, ax, ay)), o4 = dfm_k_sign(dfm_k_cross(cx, cy, dx, dy, bx, by));
    if (o1 != o2 && o3 != o4) return true;
    if (!o1 && dfm_k_on_segment(ax, ay, bx, by, cx, cy)) return true;
    if (!o2 && dfm_k_on_segment(ax, ay, bx, by, dx, dy)) return true;
    if (!o3 && dfm_k_on_segment(cx, cy, dx, dy, ax, ay)) return true;
    if (!o4 && dfm_k_on_segment(cx, cy, dx, dy, bx, by)) return true;
    return false;
}

#if DFM_KERNEL_HAVE_I128
/* Is the distance from P to segment A-B below T (T > 0)? */
COPPER_HD bool dfm_k_point_closer(int64_t px, int64_t py, int64_t ax, int64_t ay, int64_t bx, int64_t by, int64_t t)
{
    int64_t dx = bx - ax, dy = by - ay, vx = px - ax, vy = py - ay;
    uint64_t t2 = (uint64_t)t * (uint64_t)t;                               /* T <= 3e9: T^2 <= 9e18 */
    uint64_t l2 = (uint64_t)(dx * dx) + (uint64_t)(dy * dy);              /* <= 8e18 */
    int64_t dot = vx * dx + vy * dy;                                       /* |.| <= 8e18 */
    if (l2 == 0 || dot <= 0) return (uint64_t)(vx * vx) + (uint64_t)(vy * vy) < t2;
    if ((uint64_t)dot >= l2) {
        int64_t wx = px - bx, wy = py - by;
        return (uint64_t)(wx * wx) + (uint64_t)(wy * wy) < t2;
    }
    int64_t c = dx * vy - dy * vx;                                         /* |.| <= 8e18 */
    uint64_t ac = (uint64_t)(c < 0 ? -c : c);
    return (DfmU128)ac * ac < (DfmU128)t2 * l2;
}

/* Are segments a-b and c-d closer than T (T > 0)? */
COPPER_HD bool dfm_k_segments_closer(int64_t ax, int64_t ay, int64_t bx, int64_t by, int64_t cx, int64_t cy, int64_t dx, int64_t dy, int64_t t)
{
    if (t <= 0) return false;
    if (dfm_k_segments_meet(ax, ay, bx, by, cx, cy, dx, dy)) return true;
    return dfm_k_point_closer(ax, ay, cx, cy, dx, dy, t) || dfm_k_point_closer(bx, by, cx, cy, dx, dy, t) ||
           dfm_k_point_closer(cx, cy, ax, ay, bx, by, t) || dfm_k_point_closer(dx, dy, ax, ay, bx, by, t);
}
#endif

#endif
