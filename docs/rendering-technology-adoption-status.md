# Rendering technology adoption status (dev_muse)

Consolidated record for the long-term engine plan
(`ULTIMATE_ENGINE_PLAN.md`). All integrations follow the same rules:
settings flag with the existing path as fallback, A/B on the same
scene/camera, license checked before vendoring.

## Verified on the user's Windows machine (2026-10-31)

The repo owner built and ran the following on their RTX machine and
reported no problems:

- Stage 1C VG dedicated shadow v1 (52fc5fc0, 08eb5bed, f1deb075,
  validation record 51f1da7a)
- D3D12MemoryAllocator foundation + TextureManager connection
  (13f06fc0, a09437ce)
- FidelityFX SPD single-pass Hi-Z (bdc10da6)
- The pre-existing full-renderer smoke device hang (frame 35) did not
  surface as a problem in this verification run.

## Verified on the user's Windows machine (2026-10-31, second batch)

The batch below was also built and run by the repo owner on
2026-10-31 and reported problem-free.

| Commit | Change |
| --- | --- |
| 7e410f86 | D3D12MA: persistent render targets (depth, shadow depth, scene RTs, transparent copy, AA target, GBuffers) |
| 6a5efd87 | D3D12MA: material preview target/depth (the file's other creations are UPLOAD-heap and stay committed) |
| e3d60ad2 | D3D12MA: ray reflection / GI signals, ReSTIR reservoirs, radiance cache |
| ea9985f9 | D3D12MA: GPU skinning output buffers |
| 96c952eb | D3D12MA: static model geometry buffers |
| f45ae02e | Stage 1C v2: alpha-tested VG shadows (shadow PS, UV/alpha decode in the shadow MS) |
| 2b531b01 | D3D12MA: allocator statistics in the graphics log (periodic + at shutdown) |
| af43ce93 | SPD Hi-Z: hybrid Reduce tail for mip chains longer than 12 |
| d1eec2f9 | This status document (initial version) |
| 8ead54ea | D3D12MA: occlusion culling resources (Hi-Z pyramids, occluder depth, debug texture, SPD counter) |

VM-side checks for this batch: HiZSpdCS, VirtualGeometryShadowMS and
VirtualGeometryShadowPS compile clean with Linux DXC v1.9.2609
(cs_6_5 / ms_6_5 / ps_6_0, -O3); the other VirtualGeometry MS variants
still compile; `tests/shader_scan_tests.py` passes; Directory.Build
targets XML is well-formed with 34 tracked DXC outputs.

## A/B procedures

- D3D12MA (`RendererSettings::UseD3D12MemoryAllocator`): run the same
  scene with the flag off and on; compare the `VRAM: D3D12MA ...` log
  lines (heap count / heap MiB / waste %) against the committed-heap
  baseline. Flag off must be byte-identical in behavior.
- SPD (`RendererSettings::UseFidelityFxSpdHiZ`): compare the Hi-Z
  build profiler event GPU time and the occlusion results at 1440p/4K;
  chains above 12 mips exercise the hybrid tail.
- Stage 1C v2: compare shadow appearance of alpha-masked casters
  (foliage/cutout materials) between the VG shadow path and the legacy
  shadow path; the legacy path still draws every caster, so coverage
  differences show up as shadow detail, never as missing shadows.

## Deferred / deprioritized

- CACAO A/B: deprioritized 2026-10-31. The FidelityFX SDK 2.x Kits
  tree contains no usable CACAO shader/host sources (only the effect
  ID), the standalone repo is stale (2021), and XeGTAO is archived.
  The engine's visibility-bitmask SSAO is already a GTAO-family
  implementation; revisit AO only with a fixed-scene comparison.
- RTXDI: license review required before any adoption (comparison
  only); MegaLights already covers the many-lights use case.
- Stage 1C remaining v1 limits: shadow pass 0 only (first directional
  view), no shadow material binning.
