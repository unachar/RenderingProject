# Stage 3 evaluation — GI radiance caches (SHaRC / RTXGI / NRC)

Question from the plan: compare SHaRC / RTXGI (and NRC as a
separate case) against the engine's existing hash radiance
cache.

## The incumbent (surveyed 2026-10-31)

`rayreflections.cpp` carries a spatial hash radiance cache —
Keys / Accumulation / Resolved / Ages buffers, a tunable cell
size (`RendererSettings::GetGiRadianceCacheCellSize`), a clear /
resolve compute pair, D3D12MA-backed since the 50-push batch —
feeding the ReSTIR GI and Lumen-style paths when
`GetGiRadianceCacheEnabled()` (or Lumen) is on. Alongside it sit
two other cache structures with different jobs: the Lumen
surface cache (per-object cards) and, since Stage 2J, local
reflection probes (specular only).

## The candidates

- **SHaRC** (NVIDIA RTX SDK): a spatially hashed radiance cache
  — the same algorithm family as the incumbent (hash grid,
  accumulate, resolve, age out). Adopting it would replace a
  working in-tree implementation of the same design with an
  SDK-licensed one (RTX SDK license, cf. the RTXDI note), with
  no published evidence of a quality/perf edge for this engine's
  cell sizes and update pattern. It is NOT present in the
  vendored RTXNS tree.
- **RTXGI / DDGI**: probe-volume irradiance — a different point
  in the space (regular grids, per-probe SH, visibility moments).
  It overlaps the Lumen surface cache's role more than the hash
  cache's, and would be a third GI structure to keep coherent;
  the engine's dynamic-scene, hash-based approach was chosen
  precisely to avoid probe-volume placement/baking concerns.
- **NRC** (neural radiance cache): replaces the cache with a
  small MLP trained online. Different risk class entirely: SDK
  and GPU requirements, training stability, history invalidation
  on scene edits, and a debugging surface (a neural net) the
  current tooling cannot inspect. The plan already flags it as a
  separate evaluation.

## Recommendation

**Keep the in-tree hash cache; adopt none of the three now.**
The branch rule is to prefer a library when it is more efficient
or higher quality — for SHaRC that case is unproven against an
incumbent implementing the same algorithm, and for DDGI/NRC the
trade is a different architecture, not an upgrade.

The comparison that would change this answer is empirical and
now has an oracle: once Stage 2H real-scene captures exist, the
cache's output can be RMSE/SSIM-scored against the reference
integrator on fixed scenes (the A/B runbook procedure). If a
candidate ever wins that comparison on quality per GPU ms, the
adoption conversation reopens with data.
