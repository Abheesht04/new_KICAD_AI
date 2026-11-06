#ifndef COPPER_V2_DFM_GPU_H
#define COPPER_V2_DFM_GPU_H

/*
 * dfm_gpu.h - track-to-track clearance on many tracks at once (Copper V2 track H).
 *
 * The problem is a structure of arrays of tracks (capsules: segment a-b, radius r, net, copper layer bits). Two tracks
 * violate the clearance when they share a layer, are not on one net (net < 0 is "no net" and always counts), and
 * their copper is closer than `limit_nm` (dfm_kernel.h; equal passes). Every unordered pair is tested.
 *
 *   dfm_cpu_ref_clearance  the CPU reference (dfm_cpu_ref.c), always built: the definition of the right answer.
 *   dfm_gpu_clearance      the CUDA version (cuda/dfm_gpu.cu), built only with -DCOPPER_CUDA=ON. It runs the same
 *                          predicate (dfm_kernel.h) and must equal the CPU reference bit for bit. Without CUDA it is a
 *                          stub that returns DFM_GPU_NOT_BUILT: nothing needs a GPU to build or to pass tests.
 * Results: per-track violation counts and the number of violating pairs, order-independent sums, so the GPU's
 * thread scheduling cannot change them.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DfmTracks {
    size_t count;
    const int64_t *ax, *ay, *bx, *by;   /* nm, |v| <= 1e9 */
    const int64_t *radius;              /* nm, >= 0 */
    const int32_t *net;                 /* < 0: no net */
    const uint32_t *layers;             /* copper layer bits */
} DfmTracks;

typedef enum DfmGpuStatus {
    DFM_GPU_OK = 0,
    DFM_GPU_NOT_BUILT,       /* compiled without COPPER_CUDA */
    DFM_GPU_NO_DEVICE,       /* built with CUDA, but no usable GPU */
    DFM_GPU_CUDA_ERROR,      /* a CUDA call failed (message in info) */
    DFM_GPU_INVALID_INPUT    /* NULL arrays, a coordinate beyond 1e9 or limit + 2 r beyond the predicate's range */
} DfmGpuStatus;

typedef struct DfmGpuInfo {
    const char *message;     /* static: the CUDA error or device, or NULL */
    double kernel_ms;        /* measured by CUDA events (benchmark only) */
} DfmGpuInfo;

const char *dfm_gpu_status_name(DfmGpuStatus status);

/* counts[i] = violations involving track i (caller-allocated, `count` entries); *pairs = violating pairs. */
DfmGpuStatus dfm_cpu_ref_clearance(const DfmTracks *tracks, int64_t limit_nm, uint32_t *counts, uint64_t *pairs);
DfmGpuStatus dfm_gpu_clearance(const DfmTracks *tracks, int64_t limit_nm, uint32_t *counts, uint64_t *pairs, DfmGpuInfo *info);

#ifdef __cplusplus
}
#endif

#endif
