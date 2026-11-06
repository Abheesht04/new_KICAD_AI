/* dfm_cpu_ref.c - the CPU reference for track-to-track clearance (see dfm_gpu.h). Every unordered pair, with the
 * predicate in dfm_kernel.h that the CUDA kernel also runs. */
#include "dfm_gpu.h"

#include "dfm_kernel.h"

#include <string.h>

static const char *const NAMES[] = {"OK", "NOT_BUILT", "NO_DEVICE", "CUDA_ERROR", "INVALID_INPUT"};

const char *dfm_gpu_status_name(DfmGpuStatus s) { return (unsigned)s < sizeof NAMES / sizeof *NAMES ? NAMES[s] : "UNKNOWN"; }

/* Shared by both paths: the inputs the predicate's integer ranges allow. */
DfmGpuStatus dfm_tracks_check(const DfmTracks *t, int64_t limit_nm, int64_t *max_radius);

DfmGpuStatus dfm_tracks_check(const DfmTracks *t, int64_t limit_nm, int64_t *max_radius)
{
    if (!t || (t->count && (!t->ax || !t->ay || !t->bx || !t->by || !t->radius || !t->net || !t->layers)) || limit_nm < 0)
        return DFM_GPU_INVALID_INPUT;
    int64_t mr = 0;
    for (size_t i = 0; i < t->count; i++) {
        const int64_t v[4] = {t->ax[i], t->ay[i], t->bx[i], t->by[i]};
        for (int k = 0; k < 4; k++) if (v[k] > INT64_C(1000000000) || v[k] < -INT64_C(1000000000)) return DFM_GPU_INVALID_INPUT;
        if (t->radius[i] < 0) return DFM_GPU_INVALID_INPUT;
        if (t->radius[i] > mr) mr = t->radius[i];
    }
    if (limit_nm + 2 * mr > DFM_KERNEL_MAX_T) return DFM_GPU_INVALID_INPUT;
    if (max_radius) *max_radius = mr;
    return DFM_GPU_OK;
}

DfmGpuStatus dfm_cpu_ref_clearance(const DfmTracks *t, int64_t limit_nm, uint32_t *counts, uint64_t *pairs)
{
#if !DFM_KERNEL_HAVE_I128
    (void)t; (void)limit_nm; (void)counts; (void)pairs;
    return DFM_GPU_INVALID_INPUT;   /* the exact predicate needs 128-bit integers */
#else
    DfmGpuStatus s = dfm_tracks_check(t, limit_nm, NULL);
    if (s || !pairs || (t->count && !counts)) return s ? s : DFM_GPU_INVALID_INPUT;
    if (t->count) memset(counts, 0, t->count * sizeof *counts);
    uint64_t total = 0;
    for (size_t i = 0; i < t->count; i++) {
        for (size_t j = i + 1; j < t->count; j++) {
            if (!(t->layers[i] & t->layers[j])) continue;
            if (t->net[i] >= 0 && t->net[i] == t->net[j]) continue;
            int64_t T = limit_nm + t->radius[i] + t->radius[j];
            if (dfm_k_segments_closer(t->ax[i], t->ay[i], t->bx[i], t->by[i], t->ax[j], t->ay[j], t->bx[j], t->by[j], T)) {
                counts[i]++;
                counts[j]++;
                total++;
            }
        }
    }
    *pairs = total;
    return DFM_GPU_OK;
#endif
}
