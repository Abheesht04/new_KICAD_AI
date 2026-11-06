# CUDA DFM kernel (V2 track H)

Track-to-track clearance for many tracks at once. Every unordered pair is tested: two tracks violate the limit when they share a copper layer, are not on one net (no net counts as different), and their copper is closer than the limit. A distance exactly at the limit passes.

| File | Role |
|---|---|
| `src/v2_src/dfm_kernel.h` | **the predicate**, compiled by both paths. It works in integer nm: orientation tests for crossing segments, then point-to-segment distances compared without a square root (`cross² < T²·|d|²`, unsigned 128-bit). |
| `src/v2_src/dfm_cpu_ref.c` | the **CPU reference**, always built: the definition of the right answer |
| `cuda/dfm_gpu.cu`, `src/v2_src/dfm_gpu.h` | the **CUDA version**, built only with `-DCOPPER_CUDA=ON` (`COPPER_CUDA=1`; needs nvcc, CUDA 11.5+ for device `__int128`) |
| `src/v2_src/dfm_gpu_stub.c` | without CUDA, `dfm_gpu_clearance` returns `NOT_BUILT`; nothing needs a GPU to build or to pass tests |
| `apps/dfm_bench.c` | benchmark at N = 64, 1024 and 16384 |

**The kernel.** One thread per track i tests every j > i. Per-track counts and the pair total are integer atomics, so they are order-independent sums. Because both paths compile the same predicate from the same header, the GPU result must equal the CPU reference bit for bit.

**Accepted inputs.**
- Coordinates within ±1 m.
- `limit + 2·radius` at most 3 m.
- Anything else is `INVALID_INPUT`, never a wrapped integer.

## Evidence on this machine

**This machine has no CUDA toolkit and no NVIDIA GPU, so the GPU path was not built and not run.** The GPU-versus-CPU comparison (`dfm_gpu_compare`, `dfm_gpu_test --gpu`) reports **NOT RUN** (exit 77). The benchmark's GPU column says "not run".

What did run:
- **`dfm_gpu` test:**
  - the CPU reference against an **independent** implementation, g2's exact capsule distance (the one `dfm.c` uses), every pair on **1000 random boards**: equal per-track counts and totals on every board (29,244 violating pairs);
  - each board carries two exact boundary pairs: a copper gap of exactly the limit (passes) and 1 nm inside it (fails, on all 1000 boards);
  - hand-made cases: crossing, end-to-end touching, collinear overlap, a zero-length track, parallel tracks exactly at the limit and 1 nm inside, same net, no net, and out-of-range input.
- **Sanitizers:** clean under UBSan. ASan was not run; it is unavailable on MinGW.
- **`cuda/dfm_gpu.cu` syntax check:** with g++ `-std=c++17 -Wall -Wextra` against a minimal CUDA stub header (`build-tools/cuda_stub`, kernel launch removed). This catches C++ errors only; it is **not** an nvcc build.

**Benchmark,** CPU reference only (`dfm_bench`, `gcc -O2`, Intel Core i7-5600U, one thread):

| N | pairs tested | violating | CPU ms | GPU |
|---|---|---|---|---|
| 64 | 2,016 | 0 | 0 | not run |
| 1,024 | 523,776 | 117 | 11 | not run |
| 16,384 | 134,209,536 | 26,618 | 2,731 | not run |

## Running the GPU path

```
cmake -S . -B build-cuda -DCOPPER_CUDA=ON
cmake --build build-cuda
ctest --test-dir build-cuda -R dfm_gpu     # dfm_gpu_compare must PASS: GPU == CPU on 1000 boards
build-cuda/dfm_bench                       # the GPU column, with an equality check per N
```

## Not yet

- The kernel covers track-to-track clearance only.
- `dfm.c` does not offload to it yet; its clearance search stays on the CPU (BVH plus g2).
