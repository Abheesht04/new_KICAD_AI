/* dfm_bench: track-to-track clearance, CPU reference vs GPU (track H), at N = 64, 1024 and 16384 tracks.
 * Every pair is tested (N^2/2). The GPU column is "not built" unless configured with -DCOPPER_CUDA=ON; when it runs,
 * its result must equal the CPU reference or the benchmark fails. Output: one line per N. */
#include "dfm_gpu.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t rng = 7;
static uint64_t next(void)
{
    uint64_t z = (rng += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

/* Processor time of this (single-threaded) CPU reference; GPU time comes from CUDA events. */
static double now_ms(void) { return (double)clock() * 1000.0 / CLOCKS_PER_SEC; }

int main(void)
{
    static const size_t SIZES[] = {64, 1024, 16384};
    printf("%8s %14s %10s %12s %12s %s\n", "N", "pairs tested", "violating", "CPU ms", "GPU ms", "GPU == CPU");
    for (size_t s = 0; s < sizeof SIZES / sizeof *SIZES; s++) {
        size_t n = SIZES[s];
        int64_t *v = malloc(5 * n * sizeof *v);
        int32_t *net = malloc(n * sizeof *net);
        uint32_t *lay = malloc(n * sizeof *lay), *c1 = malloc(n * sizeof *c1), *c2 = malloc(n * sizeof *c2);
        if (!v || !net || !lay || !c1 || !c2) return 1;
        int64_t span = 100000000;   /* tracks spread over 100 x 100 mm */
        for (size_t i = 0; i < n; i++) {
            int64_t x = (int64_t)(next() % (uint64_t)span), y = (int64_t)(next() % (uint64_t)span);
            v[i] = x; v[n + i] = y;
            v[2 * n + i] = x + (int64_t)(next() % 3000000); v[3 * n + i] = y + (int64_t)(next() % 3000000) - 1500000;
            v[4 * n + i] = 100000;
            net[i] = (int32_t)(next() % 64);
            lay[i] = 1u + (uint32_t)(next() % 2);
        }
        DfmTracks t = {n, v, v + n, v + 2 * n, v + 3 * n, v + 4 * n, net, lay};
        uint64_t p1 = 0, p2 = 0;
        double t0 = now_ms();
        if (dfm_cpu_ref_clearance(&t, 100000, c1, &p1) != DFM_GPU_OK) return 1;
        double cpu = now_ms() - t0;
        DfmGpuInfo info;
        DfmGpuStatus g = dfm_gpu_clearance(&t, 100000, c2, &p2, &info);
        char gpu[64], same[64];
        if (g == DFM_GPU_OK) {
            snprintf(gpu, sizeof gpu, "%.3f", info.kernel_ms);
            bool eq = p1 == p2 && !memcmp(c1, c2, n * sizeof *c1);
            snprintf(same, sizeof same, "%s", eq ? "yes" : "NO");
            if (!eq) { printf("GPU result differs from the CPU reference at N = %zu\n", n); return 2; }
        } else {
            snprintf(gpu, sizeof gpu, "-");
            snprintf(same, sizeof same, "not run (%s)", dfm_gpu_status_name(g));
        }
        printf("%8zu %14llu %10llu %12.1f %12s %s\n", n, (unsigned long long)(n * (n - 1) / 2), (unsigned long long)p1, cpu, gpu, same);
        free(v); free(net); free(lay); free(c1); free(c2);
    }
    return 0;
}
