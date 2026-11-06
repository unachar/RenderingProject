# Deterministic rendering work reduction

Date: 2026-10-19 (JST). Base: `dev_main` at `5cfccfbfb6e961199e47a1bd3e45c1c78642f4f3`.

## Scope and invariants

Reduce redundant work in VirtualGeometry (the project's Nanite-style path),
MegaLights and TSR. Preserve existing sample counts, light membership/order,
LOD policy, spatial tap coordinates, accumulation order, history rejection and
quality settings. These changes do not establish a global optimization limit.
They also do not change pre-existing atomic allocation/scatter ordering or
make overflow allocation deterministic across GPU workgroups.

## Changes

| Path | Change | Source-level reduction, not measured GPU speed |
| --- | --- | --- |
| VirtualGeometry material binning | Brent-Kung scan instead of Hillis-Steele | 2,036 shared additions instead of 10,240 per 1,024-element scan; 21 rather than 22 group barriers. Prefix executes two scans. |
| MegaLights coarse list | Replicate the running integer count in each lane | 3 rather than 4 group barriers per 64-light chunk. At 2,000 lights, 32 fewer barriers per occupied coarse workgroup. |
| TSR | Cooperatively load current color/depth into a bounded shared tile | Each required source texel is fetched once per workgroup, rather than separately for each output pixel's bicubic/3x3 footprint. 2,888 bytes of shared storage. |

The scan uses only integer addition and group barriers, without assumptions about
wave size or lane layout. Read/write sets within each tree stage are disjoint.
The final barrier protects shared-array reuse by the following scan.

MegaLights still ranks each hit by the same two 32-bit masks. All lanes see the
same completed masks and advance the same local count. The final barrier in
each chunk protects mask reads/list writes before the next chunk resets masks.
Capacity clamping and visibility semantics are unchanged.

TSR uses at most a 12x12 tile for an 8x8 output workgroup, including bicubic and
3x3 halos. Initialization and its barrier execute before any out-of-bounds output
lane returns. The cache is group-uniform; downscaling in either axis uses direct
loads. The arithmetic and tap accumulation order are unchanged. Define
`TSR_USE_SHARED_TILE=0` at shader compilation to compare with direct loads.
No new engine-side resources, descriptors or constant-buffer fields are needed.

## Verification performed here

Environment: Linux, Python 3.12, GCC 13. Windows SDK, DXC, PowerShell and a
DirectX-capable runtime are unavailable.

| Check | Result |
| --- | --- |
| `python tests/shader_scan_tests.py` | 96 cases pass, exact unsigned serial-prefix comparison, randomized lane arrivals, wraparound and a linear shared-add budget. The old scan fails the work-budget assertion. |
| `python tests/megalights_compaction_tests.py` | 52 cases pass, exact serial light-list comparison, 0–4,096 lights, word/chunk tails, sparse/dense hits and randomized lane arrivals. The old loop fails the barrier-budget assertion. |
| `python tests/tsr_tile_tests.py` | 408,247 tap addresses pass coverage checks, including 1px/odd/native sizes and jitter extrema. Executes the production scalar tile-bound functions. |
| Existing `TestVirtualGeometrySelection` / `TestVirtualGeometryResidentSelection` | Pass when linked with their production selection implementation and a small local test driver, GCC C++20 with warnings as errors. |
| Whitespace/diff review | `git diff --check` passes. |

The first two Python tests extract production HLSL algorithm bodies into C++20
coroutines and schedule lanes at group barriers. The compaction fixture injects
intersection outcomes; it does not test sphere/AABB math. These are CPU algorithm
checks, not HLSL compilation, GPU memory-model or full-renderer verification.
The existing selection test sources expose test functions rather than `main`;
standalone linking without a driver initially failed, then passed with the driver.

## Added Windows verification (not run here)

`scripts/test-temporalupscale.ps1` now runs `--cache-parity` before its existing
quality/regression fixtures. This compiles the production shader both with and
without the tile and checks color, depth and detail UAVs bitwise: seven extent
configurations, sixteen jitter phases and three outputs (336 comparisons).
It includes partial workgroups, 1px input, native size, anisotropic scaling and
direct-load downscale fallback. It also retains the existing DXC cs_6_5 compile
and D3D11 cs_5_0 fixtures. This new parity test is not a claimed passing result.

Before claiming a GPU performance gain, run the existing Windows validation,
compile the MegaLights `CoarseCull` and VirtualGeometry `Prefix` entry points,
and compare fixed-scene Release captures/timings on the target GPU. Use the
parent commit as baseline; use `TSR_USE_SHARED_TILE=0/1` for isolated TSR A/B.
Check full-frame time, each affected pass, 1% lows, VRAM and temporal quality.

## Remaining uncertainty

GPU compiler instruction scheduling, shared-memory occupancy and texture-cache
behavior can change the actual performance benefit. In particular, fewer source
fetches do not prove that the TSR tile is faster at every scale or on every GPU.
No FPS gain, full Windows build success, GPU parity success or exhaustive
optimization claim is made by these CPU checks. Further optimization needs
target-GPU profiles and image comparisons to select the next bottleneck safely.
