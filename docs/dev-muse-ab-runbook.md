# A/B measurement runbook — dev_muse integrations

The adoption rules for this branch: every flagged integration is
judged on the SAME scene, camera path and seed, reporting CPU ms,
GPU ms, 1% low frame time, VRAM, and image quality/stability.
Each feature below names its flag, the metric that decides it,
and the tooling that reads the result.

## Common setup

- One fixed test scene + recorded camera path per feature; run
  each configuration 3 times, report the median.
- GPU ms from the engine's pass profiler; 1% low from the frame
  time log over the same window.
- Quality: capture the same frame in both configurations and
  compare with `scripts/pfm_compare.py` where an HDR dump exists,
  otherwise side-by-side screenshots at the checklist viewpoints.

## FidelityFX SPD Hi-Z (flag: UseFidelityFxSpdHiZ, default OFF)

- Metric: Hi-Z / occlusion-culling pass GPU ms at 1440p and 4K;
  culling correctness = identical visible-instance counts vs the
  legacy chain in the occlusion debug view.
- Decide: adopt as default if GPU ms improves >= 5% with no
  culling differences; the hybrid tail covers > 12-mip chains.

## D3D12MA (flag: UseD3D12MemoryAllocator, default OFF)

- Metric: peak heap MiB and waste % from the graphics log,
  summarized by `scripts/parse_vram_log.py` (flag OFF logs
  nothing, so compare against the committed-resource baseline
  from the task manager / GPU profiler); allocation count over a
  scene-load + 5-minute soak.
- Decide: adopt as default if heap waste drops without new
  allocation stalls (frame-time spikes at load/teleport).

## SSR (flag: UseScreenSpaceReflections, default OFF)

- Metrics per sub-mode (base / checkerboard / roughness limit
  0.5): SSR pass GPU ms (trace + resolve + downsample chain),
  temporal stability (per-pixel variance of the reflection over
  60 static frames), and the checklist's visual items.
- Decide per sub-mode: checkerboard ships if trace ms roughly
  halves with no visible pattern; the roughness limit ships at
  the value where rough-floor quality stops changing.

## Stage 1C shadow material binning (always on for VG shadows)

- Metric: shadow-pass GPU ms in an alpha-heavy scene (foliage)
  vs the pre-binning build (git stash point 057c02ca^), and
  bit-identical shadow maps in the capture comparison.
- Decide: keep if foliage shadow ms improves and maps match;
  the legacy fallback covers any PSO/signature failure already.

## Stage 2H reference captures (correctness, not perf)

- Determinism: two runs, same seed -> `pfm_compare.py` RMSE 0.
- Convergence: rel-RMSE of cornell at 256 spp vs 4096 spp
  decreases monotonically; direct + indirect (beauty - direct)
  stays non-negative.
- Furnace: mirror-sphere region (`pfm_stat.py --region`, or
  `pfm_compare.py --region` between two seeds for the noise
  floor) approaches the wall emission 3.0; the rough-metal
  region sits clearly below it.
