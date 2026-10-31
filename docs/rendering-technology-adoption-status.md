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

## Also landed in the 50-push batch (continued)

- Stage 1D completion work: DecalComponent + per-frame registry
  sync, scene save/load + undo snapshots, editor inspector section
  and hierarchy create-menu entry.
- Stage 2 J local reflection probes: design, flag, component +
  registry (priority/nearest selection, CPU-tested), active-probe
  environment override plumbing, runtime GGX prefilter
  (EnvironmentPrefilterCS + TextureManager::RegisterExternalTexture)
  for sources without mip chains, save/load, inspector, create menu.
- Stage 2 H reference path tracer: design + ReferenceCapture job
  bookkeeping (state machine, deterministic seeds, accumulation
  target) — integrator shader not yet landed.
- Native Hi-Z SSR (route chosen over vendoring FidelityFX SSSR):
  design, flag, reflection-source selection point, Hi-Z trace
  shader, trace-pass host. Frame wiring is the next step.
- Stage 1C multi-view shadow design (spot/point/cascades) recorded;
  implementation not yet started.

## Landed in the 500-push run (pending verification)

- Native Hi-Z SSR completed end to end: frame wiring, world-space
  trace, confidence blending through the raytraced-reflection
  override, per-pixel roughness from the material GBuffer.
  Temporal quality: T1 half-resolution trace + bilateral resolve,
  T2 hit-point reprojected accumulation with camera-cut reset,
  checkerboard trace mode (with the resolve-constants fix that
  lets its parity reach the resolve), a prefiltered result mip
  chain sampled at roughness * 4, and a per-pixel roughness
  trace-skip limit are all landed behind the default-OFF flag.
  Robustness fixes found by audit during the run: the mip chain
  is optional (a missing downsample CSO no longer fails SSR,
  and the result clamps to one mip); the previous-color source
  at native resolution is the AA history view (the pre-upscale
  history it used before is only written by the upscale AA
  branch; DLSS/Bilateral/TSR upscaling keeps the stale source -
  documented gap); and the tri-state w=2 requires a registered
  result texture, matching the binding guard.
- Stage 2H reference path tracer: integrator shader (RayQuery,
  NEE, GGX/diffuse sampling, RR, deterministic seeds), Cornell /
  material-test / emissive-panel fixtures, quad area-light NEE
  with balance-heuristic MIS, BLAS/TLAS host, synchronous capture
  run with readback, PFM/PPM writers, first-hit albedo/normal
  AOVs, a direct/indirect split output, a white-furnace fixture,
  and the validation-hook caller (env-gated; SIZE / SPP / SEED /
  FIXTURE overrides). Tooling: pfm_compare (RMSE / rel-RMSE /
  windowed SSIM / --region), pfm_stat, parse_vram_log, and the
  Linux DXC compile sweep (scripts/compile_check_linux.sh,
  17/17 entry points). Real-scene captures remain the long-term
  follow-up in the design doc.
- Stage 2J: per-probe GGX prefilter, box-projection parallax,
  probe intensity, and per-pixel two-probe blending (Route A)
  compiled into both lighting paths after the root-signature
  audit (postprocess root 39 binds the space-1 heap; the model
  root signature already bound it at root 18). Live cubemap
  capture: conversion, scratch, face matrices, the CaptureLive
  component flag + inspector + scene serialization landed; the
  6-face scene capture pass is scoped in the capture design as
  its own reviewed change.
- D3D12MA remaining exclusions, by design: UPLOAD/READBACK heaps
  (not allocator territory), the transient aliasing arena (own
  lifetime model), reserved/tiled streaming resources, mapped
  primitive geometry, shader-pipeline constant buffers (UPLOAD),
  OptiX interop (shared handles), and the swap chain. Statistics
  are surfaced live in the Advanced GPU settings section, and
  scripts/parse_vram_log.py summarizes them from logs.
- Stage 1C multi-view: per-view traversal state pool (4 slots),
  viewId-aware PrepareShadowView/validity/accessors, and
  prepare+draw wiring for every shadow pass (viewId = shadow
  light index; DrawShadow keys by the render pass's Layer so VSM
  page passes share their light's view). Shadow material binning is implemented end to end
  (two-sided visible-list allocation, dual indirect commands,
  depth-only opaque draw + alpha binned draw, legacy fallback).
- Settings UI: checkboxes for all integration flags above.
- Review fix: probe update moved after the frame command-list
  reset so prefilter dispatches are actually recorded.

## Also landed in the 50-push batch (pending verification)

- D3D12MA routing completed for the remaining persistent DEFAULT-heap
  resources: instancing, screen-space effects, DXR buffers/guides,
  clouds, transmittance shadows, thumbnails, occlusion culling
  (batch 1), VG streaming pages, Lumen surface cache, procedural
  texture/noise caches, light instance builder, animated model index
  buffers, FSR scratch, stationary VSM depth cache, main depth.
  Excluded by design: UPLOAD/READBACK heaps, the transient aliasing
  arena, reserved/tiled streaming, and OptiX interop buffers
  (shared-handle requirements).
- Stage 1D DBuffer surface decals: design, flag, registry, DBuffer
  targets, raster + composite shaders, frame wiring and a
  DecalComponent with per-frame sync (docs:
  rendering-technology-stage1d-design.md). Inspector/save support is
  still outstanding.

## Deferred / deprioritized

- CACAO A/B: deprioritized 2026-10-31. The FidelityFX SDK 2.x Kits
  tree contains no usable CACAO shader/host sources (only the effect
  ID), the standalone repo is stale (2021), and XeGTAO is archived.
  The engine's visibility-bitmask SSAO is already a GTAO-family
  implementation; revisit AO only with a fixed-scene comparison.
- RTXDI: license review required before any adoption (comparison
  only); MegaLights already covers the many-lights use case.
- Tracy: evaluated (docs/rendering-technology-tracy-evaluation.md)
  - adopt later as an opt-in TRACY_ON_DEMAND trace alongside the
  existing CSV/GPU measurement; not landed in this run.
- STBN sampling: deferred 2026-10-31. The engine has no shared
  R2/R4 sampling-sequence module or blue-noise asset for STBN to
  replace (each stochastic subsystem hashes independently), and the
  reference STBN data set's redistribution terms are unclear.
  Revisit if a shared sampling header is introduced.
- FidelityFX CAS: not adopted; the engine already ships FSR RCAS,
  the same sharpening family.
- FidelityFX SSSR: not vendored (evaluation:
  rendering-technology-sssr-evaluation.md). SDK 2.x does not ship it
  as shader sources and it drags in the FidelityFX Denoiser chain;
  a native Hi-Z screen-space specular pass against the engine's own
  Hi-Z/history is the preferred route if the fallback is needed
  beyond probes + IBL.
- Color management: the OpenColorIO runtime is not adopted; the
  pipeline spec (linear Rec.709 working space, single view
  transform) is recorded in
  docs/rendering-technology-color-management-design.md. Revisit
  with HDR display output or EXR interchange.
- fastgltf: deferred - assimp post-processing is load-bearing;
  adoption gates (parse-profile + parity harness) in
  docs/rendering-technology-fastgltf-evaluation.md.
- GI radiance caches (SHaRC / RTXGI / NRC): keep the incumbent
  spatial hash cache; the deciding comparison is empirical
  against the Stage 2H oracle
  (docs/rendering-technology-gi-cache-evaluation.md).
- XeSS: deferred - DLSS covers the NVIDIA target, TSR is the
  vendor-neutral fallback
  (docs/rendering-technology-xess-evaluation.md).
- Stage 8 foundation (OMM / RTXTS / DirectStorage / RTXCR):
  evaluated, all deferred behind profile triggers
  (docs/rendering-technology-stage8-foundation-evaluation.md).
- Stage 4 asset pipeline (ACL / zstd / KTX2-Basis / ufbx):
  evaluated; ACL is the strongest candidate on a bake-time
  track, the rest wait on a load/memory profile
  (docs/rendering-technology-stage4-asset-evaluation.md).
