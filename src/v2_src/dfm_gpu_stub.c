/* dfm_gpu_stub.c - dfm_gpu_clearance when Copper is built without CUDA (the default): it reports NOT_BUILT and never
 * pretends to have run. cuda/dfm_gpu.cu replaces this file under -DCOPPER_CUDA=ON. */
#include "dfm_gpu.h"

DfmGpuStatus dfm_gpu_clearance(const DfmTracks *tracks, int64_t limit_nm, uint32_t *counts, uint64_t *pairs, DfmGpuInfo *info)
{
    (void)tracks; (void)limit_nm; (void)counts; (void)pairs;
    if (info) { info->message = "built without CUDA (configure with -DCOPPER_CUDA=ON and an nvcc toolchain)"; info->kernel_ms = 0; }
    return DFM_GPU_NOT_BUILT;
}
