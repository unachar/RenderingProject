# Stage 1C: Virtual Geometry dedicated shadow — implementation and validation record

Branch: `dev_muse`. Commits: `52fc5fc0` (S1–S3), `08eb5bed` (S4/S5), `f1deb075` (S6).
Feature flag: `VirtualGeometryTraversal::Settings::ShadowTraversal` (default **off**;
no behavior change while off).

## What was implemented

- **S1 View separation**: `VirtualGeometryTraversal::ViewInput` + `FillViewConstants()`.
  Traversal constants no longer read the camera from `GraphicsDevice` globals;
  the camera path builds a camera `ViewInput` (moved code, predictive-streaming
  condition preserved).
- **S2 Shadow storage**: per-frame `ShadowStates/ShadowVisible/ShadowCounters/
  ShadowArguments/ShadowSuccess/ShadowQueue/ShadowTemporalCounters` buffers in
  the traversal frame (capacity 1 while disabled). `Bind()` gained override
  constants + shadow output targeting; existing call sites unchanged.
- **S3 Shadow traversal passes**: `LightingResources::GetShadowViewParts()` exposes
  the separate (non-transposed) view/projection built for each shadow view.
  `PrepareShadowView()` registers Reset/Traversal/Indirect-Dispatch passes over
  the shadow buffers with occlusion and LOD hysteresis off and `PageCount = 0`.
  Safety against the shared traversal shader was checked in source:
  `Traverse` writes `LodHistory` only under `HysteresisEnabled`
  (VirtualGeometryTraversalCS.hlsl), `Reset` clears page-request bits only for
  `id.x < (PageCount+31)/32`, and the page-request header clear is now gated on
  `PageCount != 0` (safe: zero pages means no requests can exist). Shadow binds
  point the temporal UAV slots at shadow-owned scratch buffers.
  Wired from `MeshShaderRenderer::Prepare` for shadow view 0 with
  `ViewportHeight = shadow map size`, so the orthographic traversal branch
  selects clusters by texel error.
- **S4/S5 Shadow raster (v1)**: `VIRTUAL_GEOMETRY_SHADOW` mode in
  `VirtualGeometryMS.hlsl` (position-only vertices) + `VirtualGeometryShadowMS.hlsl`;
  `NativeShadowPSO` in MeshShaderRenderer (no PS, 0 RTs, D32 depth,
  `LESS`, cull none — mirroring `PsoManager::GetOrCreateShadowMapPso`), created in
  try/catch with legacy fallback. `MeshShaderRenderer::DrawShadow()` is called at
  the end of the ShadowMap branch of `RenderSystem::Draw` (non-transmission).
  The frame graph renders shadow maps before the GPU-driven traversal runs, so
  the raster consumes the **previous frame's** shadow visible list (one frame of
  caster latency) via `ExecuteIndirect` over `ShadowArguments`; the legacy path
  still draws every caster, so shadow coverage cannot regress.
- **S6 Dirty linkage**: `DrawShadow` is skipped during the static shadow cache's
  Stationary bake (subset accessor `LightingResources::GetShadowCasterSubset()`),
  and `VirtualGeometryTraversal::ShadowViewStillValid()` rejects replay when the
  light direction changed (forward dot < 0.9999) or the view origin drifted more
  than 25% of the view width. Instance transforms are read fresh by the mesh
  shader at draw time and traversal re-runs every frame, so transform/material/
  residency changes propagate within the one-frame selection latency by
  construction.

## Verified on a Linux VM (2026-10-30, DXC v1.9.2609 linux build)

- HLSL compile, all `-O3`, exit 0: `VirtualGeometryTraversalCS` entries Reset /
  Traverse / BuildArguments (`cs_6_5`); `VirtualGeometryMS`, `VirtualGeometryVelocityMS`,
  `VirtualGeometryBinnedMS`, `VirtualGeometryShadowMS` (`ms_6_5`);
  `VirtualGeometryOpaquePS` (`ps_6_0`). The new shadow MS output is 13,156 bytes
  vs 19,924 for the full MS, consistent with position-only output.
- CPU tests: `virtualgeometry_shadow_selection_tests` + `virtualgeometry_selection_tests`
  compiled with g++ (`-std=c++20`) — "Orthographic shadow and perspective
  hierarchy tests passed", exit 0.
- `tests/shader_scan_tests.py`: PASS (96 production scan cases).
- `Directory.Build.targets`: XML well-formed; new DXC target
  `CompileVirtualGeometryShadersVirtualGeometryShadowMS` added (group DependsOn +
  FileWrites); `scripts/test-shader-build-targets.ps1` expected count updated 31 → 32.

## Not verified (needs the Windows + RTX machine)

- MSVC build of the C++ changes (traversal, lighting resources, mesh shader
  renderer, render system) and the full shader build through MSBuild.
- GPU execution: shadow traversal counters, `DrawShadow` depth parity vs the
  legacy path on a fixed scene (contour RMSE/SSIM A/B), shadow GPU ms, VRAM.
- The pre-existing full-renderer smoke failure (frame 35 TSR history /
  `DXGI_ERROR_DEVICE_HUNG`) is unrelated and still undiagnosed; it reproduces
  with Stage 1A disabled and predates this work.
- `tests/dxc_build_contract_tests.py` could not run here (invokes Windows
  `vswhere.exe`).

## Known v1 limitations

- Alpha-masked materials cast as opaque (no alpha test in the depth-only path).
- Only shadow view 0 / shadow pass 0 (directional first view); spot/point views
  are not traversed yet. Material binning is not used for shadow (direct
  dispatch); revisit if measurements justify it.
- One frame of caster-selection latency (see S4/S5 above).
