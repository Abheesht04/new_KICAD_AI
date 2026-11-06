/* Track H: the CPU reference (dfm_cpu_ref.c) and the CUDA kernel (cuda/dfm_gpu.cu) for track-to-track clearance.
 *
 *   dfm_gpu_test        the CPU reference against an independent implementation -- g2's exact capsule distance
 *                       (g2_compare_capsule_capsule_distance, the one dfm.c uses) -- on 1000 random boards, every
 *                       pair, plus hand-made boundary cases. Always runs.
 *   dfm_gpu_test --gpu  the GPU against the CPU reference on the same 1000 boards: per-track counts and pair totals
 *                       must be equal. Exit 77 (NOT RUN) when built without CUDA or without a device. */
#include "dfm_gpu.h"
#include "g2_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint64_t rng;
static uint64_t next(void)
{
    uint64_t z = (rng += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}
static int64_t range(int64_t lo, int64_t hi) { return lo + (int64_t)(next() % (uint64_t)(hi - lo + 1)); }

typedef struct Board {
    size_t n;
    int64_t ax[256], ay[256], bx[256], by[256], r[256];
    int32_t net[256];
    uint32_t layers[256];
} Board;

#define LIMIT 100000   /* 0.1 mm */

static void random_board(Board *b, int k)
{
    b->n = (size_t)range(2, 120);
    int64_t span = k % 3 == 0 ? 5000000 : 30000000;          /* some boards crowded, some sparse */
    for (size_t i = 0; i < b->n; i++) {
        int64_t x = range(-span, span), y = range(-span, span);
        int shape = (int)(next() % 4);
        int64_t len = range(0, 4000000);
        b->ax[i] = x; b->ay[i] = y;
        b->bx[i] = shape == 0 ? x + len : shape == 1 ? x : x + range(-len, len);   /* horizontal, vertical, any */
        b->by[i] = shape == 0 ? y : shape == 1 ? y + len : y + range(-len, len);
        if (shape == 3 && next() % 5 == 0) { b->bx[i] = x; b->by[i] = y; }        /* a dot */
        b->r[i] = range(50000, 300000);
        b->net[i] = (int32_t)range(-1, 4);
        b->layers[i] = (uint32_t)range(1, 3);
    }
    /* Exact boundary pairs: parallel horizontal tracks whose copper gap is LIMIT (passes) or LIMIT - 1 (fails). */
    if (b->n + 4 <= 256) {
        for (int e = 0; e < 2; e++) {
            size_t i = b->n++, j = b->n++;
            int64_t x = range(-span, span), y = range(-span, span), r1 = range(50000, 300000), r2 = range(50000, 300000);
            b->ax[i] = x; b->ay[i] = y; b->bx[i] = x + 2000000; b->by[i] = y; b->r[i] = r1; b->net[i] = 1; b->layers[i] = 1;
            int64_t gap = LIMIT - e;
            b->ax[j] = x + 500000; b->ay[j] = y + r1 + r2 + gap; b->bx[j] = x + 3000000; b->by[j] = b->ay[j]; b->r[j] = r2; b->net[j] = 2; b->layers[j] = 1;
        }
    }
}

static DfmTracks view(const Board *b)
{
    DfmTracks t = {b->n, b->ax, b->ay, b->bx, b->by, b->r, b->net, b->layers};
    return t;
}

/* The independent answer: g2's exact capsule distance, pair by pair. */
static void g2_answer(const Board *b, uint32_t *counts, uint64_t *pairs)
{
    memset(counts, 0, b->n * sizeof *counts);
    *pairs = 0;
    for (size_t i = 0; i < b->n; i++)
        for (size_t j = i + 1; j < b->n; j++) {
            if (!(b->layers[i] & b->layers[j]) || (b->net[i] >= 0 && b->net[i] == b->net[j])) continue;
            G2Capsule p = {{b->ax[i], b->ay[i]}, {b->bx[i], b->by[i]}, b->r[i]}, q = {{b->ax[j], b->ay[j]}, {b->bx[j], b->by[j]}, b->r[j]};
            G2Order o;
            if (g2_compare_capsule_capsule_distance(p, q, LIMIT, &o) == G2_OK && o == G2_ORDER_LESS) { counts[i]++; counts[j]++; (*pairs)++; }
        }
}

static void test_cases(void)
{
    /* crossing, touching ends, collinear overlap, a dot, parallel at exactly the limit, far apart */
    Board b;
    memset(&b, 0, sizeof b);
    const int64_t s[][4] = {{0, 0, 1000000, 1000000}, {0, 1000000, 1000000, 0},             /* cross */
                            {5000000, 0, 6000000, 0}, {6000000, 0, 7000000, 0},              /* touch at an end */
                            {10000000, 0, 12000000, 0}, {11000000, 0, 13000000, 0},          /* collinear overlap */
                            {20000000, 0, 20000000, 0}, {20000000, 300000, 21000000, 300000}, /* dot vs track: gap 300000 - 200000 */
                            {30000000, 0, 31000000, 0}, {30000000, 300000, 31000000, 300000}};/* parallel: gap 300000 - 200000 */
    b.n = 10;
    for (size_t i = 0; i < b.n; i++) {
        b.ax[i] = s[i][0]; b.ay[i] = s[i][1]; b.bx[i] = s[i][2]; b.by[i] = s[i][3];
        b.r[i] = 100000; b.net[i] = (int32_t)i; b.layers[i] = 1;
    }
    uint32_t c[16], g[16];
    uint64_t pairs, gp;
    DfmTracks t = view(&b);
    CHECK(dfm_cpu_ref_clearance(&t, LIMIT, c, &pairs) == DFM_GPU_OK);
    g2_answer(&b, g, &gp);
    CHECK(pairs == gp && !memcmp(c, g, b.n * sizeof *c));
    CHECK(c[0] == 1 && c[1] == 1 && c[2] == 1 && c[3] == 1 && c[4] == 1 && c[5] == 1);   /* cross, touch, overlap */
    CHECK(c[6] == 0 && c[7] == 0 && c[8] == 0 && c[9] == 0);                              /* exactly at the limit: pass */
    CHECK(pairs == 3);
    /* One nanometre closer fails. */
    b.ay[9] = b.by[9] = 299999;
    CHECK(dfm_cpu_ref_clearance(&t, LIMIT, c, &pairs) == DFM_GPU_OK && c[8] == 1 && pairs == 4);
    /* Same net: never a violation; no net (-1): always checked. */
    b.net[9] = b.net[8];
    CHECK(dfm_cpu_ref_clearance(&t, LIMIT, c, &pairs) == DFM_GPU_OK && c[8] == 0);
    b.net[8] = b.net[9] = -1;
    CHECK(dfm_cpu_ref_clearance(&t, LIMIT, c, &pairs) == DFM_GPU_OK && c[8] == 1);
    /* Inputs beyond the exact range are refused. */
    b.ax[0] = INT64_C(2000000000);
    CHECK(dfm_cpu_ref_clearance(&t, LIMIT, c, &pairs) == DFM_GPU_INVALID_INPUT);
    b.ax[0] = 0;
    CHECK(dfm_cpu_ref_clearance(&t, INT64_C(4000000000), c, &pairs) == DFM_GPU_INVALID_INPUT);
}

static int compare_boards(bool gpu)
{
    static Board b;
    uint32_t ref[256], other[256];
    size_t violations = 0, boundary_fails = 0;
    rng = 20261107;
    for (int k = 0; k < 1000; k++) {
        random_board(&b, k);
        DfmTracks t = view(&b);
        uint64_t rp, op;
        CHECK(dfm_cpu_ref_clearance(&t, LIMIT, ref, &rp) == DFM_GPU_OK);
        if (gpu) {
            DfmGpuInfo info;
            DfmGpuStatus s = dfm_gpu_clearance(&t, LIMIT, other, &op, &info);
            if (s == DFM_GPU_NOT_BUILT || s == DFM_GPU_NO_DEVICE) {
                printf("dfm_gpu --gpu: NOT RUN (%s: %s)\n", dfm_gpu_status_name(s), info.message ? info.message : "");
                return 77;
            }
            CHECK(s == DFM_GPU_OK);
        } else {
            g2_answer(&b, other, &op);
        }
        if (rp != op || memcmp(ref, other, b.n * sizeof *ref)) {
            fprintf(stderr, "board %d: CPU reference %llu pairs, %s %llu\n", k, (unsigned long long)rp, gpu ? "GPU" : "g2", (unsigned long long)op);
            failures++;
        }
        violations += (size_t)rp;
        boundary_fails += ref[b.n - 1] > 0;                 /* the last pair sits 1 nm inside the limit */
    }
    CHECK(boundary_fails == 1000);
    printf("dfm_gpu%s: 1000 random boards, %zu violating pairs, CPU reference == %s on every track\n", gpu ? " --gpu" : "", violations,
           gpu ? "GPU" : "g2 exact capsule distance");
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--gpu")) return compare_boards(true);
    test_cases();
    int rc = compare_boards(false);
    DfmGpuInfo info;
    uint32_t c[1];
    uint64_t p;
    DfmTracks none = {0, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
    DfmGpuStatus s = dfm_gpu_clearance(&none, LIMIT, c, &p, &info);
    printf("dfm_gpu: this build's GPU path: %s\n", dfm_gpu_status_name(s));
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    puts("dfm_gpu: all checks passed");
    return rc;
}
