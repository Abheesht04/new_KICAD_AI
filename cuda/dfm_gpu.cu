// dfm_gpu.cu - track-to-track clearance on the GPU (Copper V2 track H). Built only with -DCOPPER_CUDA=ON.
//
// One thread per track i tests every j > i with the same predicate as the CPU reference (src/v2_src/dfm_kernel.h),
// so every pair is decided identically. Counts are integer atomics (order-independent sums): the result must equal
// dfm_cpu_ref_clearance bit for bit, which tests/dfm_gpu_test.c checks on 1000 random boards.
// Requires CUDA 11.5 or later (unsigned __int128 in device code).

#include "dfm_gpu.h"
#include "dfm_kernel.h"

#include <cuda_runtime.h>

#include <string.h>

extern "C" DfmGpuStatus dfm_tracks_check(const DfmTracks *t, int64_t limit_nm, int64_t *max_radius);

__global__ void clearance_kernel(size_t n, const int64_t *ax, const int64_t *ay, const int64_t *bx, const int64_t *by,
                                 const int64_t *r, const int32_t *net, const uint32_t *layers, int64_t limit,
                                 unsigned int *counts, unsigned long long *pairs)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    unsigned int mine = 0;
    unsigned long long found = 0;
    for (size_t j = i + 1; j < n; j++) {
        if (!(layers[i] & layers[j])) continue;
        if (net[i] >= 0 && net[i] == net[j]) continue;
        int64_t T = limit + r[i] + r[j];
        if (dfm_k_segments_closer(ax[i], ay[i], bx[i], by[i], ax[j], ay[j], bx[j], by[j], T)) {
            mine++;
            found++;
            atomicAdd(&counts[j], 1u);
        }
    }
    if (mine) atomicAdd(&counts[i], mine);
    if (found) atomicAdd(pairs, found);
}

static DfmGpuStatus fail(DfmGpuInfo *info, cudaError_t e)
{
    if (info) info->message = cudaGetErrorString(e);
    return e == cudaErrorNoDevice || e == cudaErrorInsufficientDriver ? DFM_GPU_NO_DEVICE : DFM_GPU_CUDA_ERROR;
}

#define TRY(call) do { cudaError_t e_ = (call); if (e_ != cudaSuccess) { status = fail(info, e_); goto out; } } while (0)

extern "C" DfmGpuStatus dfm_gpu_clearance(const DfmTracks *t, int64_t limit_nm, uint32_t *counts, uint64_t *pairs, DfmGpuInfo *info)
{
    DfmGpuStatus status = dfm_tracks_check(t, limit_nm, NULL);
    if (info) { info->message = NULL; info->kernel_ms = 0; }
    if (status != DFM_GPU_OK || !pairs || (t->count && !counts)) return status != DFM_GPU_OK ? status : DFM_GPU_INVALID_INPUT;
    *pairs = 0;
    if (!t->count) return DFM_GPU_OK;
    size_t n = t->count;
    int64_t *d64 = NULL;
    int32_t *dnet = NULL;
    uint32_t *dlay = NULL;
    unsigned int *dcount = NULL;
    unsigned long long *dpairs = NULL;
    cudaEvent_t start = NULL, stop = NULL;
    int devices = 0;
    TRY(cudaGetDeviceCount(&devices));
    if (!devices) { if (info) info->message = "no CUDA device"; return DFM_GPU_NO_DEVICE; }
    TRY(cudaMalloc((void **)&d64, 5 * n * sizeof *d64));
    TRY(cudaMalloc((void **)&dnet, n * sizeof *dnet));
    TRY(cudaMalloc((void **)&dlay, n * sizeof *dlay));
    TRY(cudaMalloc((void **)&dcount, n * sizeof *dcount));
    TRY(cudaMalloc((void **)&dpairs, sizeof *dpairs));
    TRY(cudaMemcpy(d64 + 0 * n, t->ax, n * sizeof *d64, cudaMemcpyHostToDevice));
    TRY(cudaMemcpy(d64 + 1 * n, t->ay, n * sizeof *d64, cudaMemcpyHostToDevice));
    TRY(cudaMemcpy(d64 + 2 * n, t->bx, n * sizeof *d64, cudaMemcpyHostToDevice));
    TRY(cudaMemcpy(d64 + 3 * n, t->by, n * sizeof *d64, cudaMemcpyHostToDevice));
    TRY(cudaMemcpy(d64 + 4 * n, t->radius, n * sizeof *d64, cudaMemcpyHostToDevice));
    TRY(cudaMemcpy(dnet, t->net, n * sizeof *dnet, cudaMemcpyHostToDevice));
    TRY(cudaMemcpy(dlay, t->layers, n * sizeof *dlay, cudaMemcpyHostToDevice));
    TRY(cudaMemset(dcount, 0, n * sizeof *dcount));
    TRY(cudaMemset(dpairs, 0, sizeof *dpairs));
    TRY(cudaEventCreate(&start));
    TRY(cudaEventCreate(&stop));
    {
        unsigned int threads = 256, blocks = (unsigned int)((n + threads - 1) / threads);
        TRY(cudaEventRecord(start));
        clearance_kernel<<<blocks, threads>>>(n, d64, d64 + n, d64 + 2 * n, d64 + 3 * n, d64 + 4 * n, dnet, dlay, limit_nm, dcount, dpairs);
        TRY(cudaGetLastError());
        TRY(cudaEventRecord(stop));
        TRY(cudaEventSynchronize(stop));
        float ms = 0;
        TRY(cudaEventElapsedTime(&ms, start, stop));
        if (info) info->kernel_ms = ms;
    }
    {
        unsigned long long p = 0;
        TRY(cudaMemcpy(counts, dcount, n * sizeof *dcount, cudaMemcpyDeviceToHost));
        TRY(cudaMemcpy(&p, dpairs, sizeof p, cudaMemcpyDeviceToHost));
        *pairs = (uint64_t)p;
    }
    status = DFM_GPU_OK;
out:
    if (start) cudaEventDestroy(start);
    if (stop) cudaEventDestroy(stop);
    cudaFree(d64); cudaFree(dnet); cudaFree(dlay); cudaFree(dcount); cudaFree(dpairs);
    return status;
}
