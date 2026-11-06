# Phase 4 Implementation Plan

**Goal:** Extend Phase 3 with conservative temporal Hi-Z visibility and screen-space GPU LOD through ExecuteIndirect.
**Architecture:** Retain GPU Scene and geometry arena. Add a focused GPUVisibility module for per-slot occlusion/LOD records, history, metadata, counters and debug. Reuse Hi-Z textures with RenderGraph subresource barriers.
**Tech Stack:** C++20, DirectX12, HLSL SM5.1, ImGui, existing MSBuild and GPU validation hooks.
**Spec:** [gpudriven-phase4-design.md](gpudriven-phase4-design.md)
**Execution:** Native implementation in this session, as requested by the user. Preserve existing changes; no automatic commit or separate worktree.

## Constraints and review focus

- Normal Z, opaque-only occluders, no visibility readback outside debug/validation.
- Camera/occluder changes invalidate temporal depth; first frame and resize never sample invalid history.
- Odd resolutions retain edge texels; projected bounds cover every sampled texel.
- LOD history resets on scene edits/slot reuse and missing meshes fall back to finer geometry.
- All new GPU resource barriers belong to RenderGraph; lifetime follows the existing queue/fence design.

## Tasks

- [x] Add CPU tests for pyramid footprints, full rectangle coverage, LOD thresholds/hysteresis/fallback and metadata round trip. Run failing tests before implementing the associated helpers.
- [x] Add `source/rendering/visibility/visibilitytypes.h` and `gpuvisibility.h/.cpp`: GPU layouts, settings, immutable LOD registration, per-frame output/statistics buffers, persistent history, camera/scene invalidation, debug counters and overlays.
- [x] Extend `StaticMeshData` to retain generated LOD indices; register low-LOD Scene Mesh IDs and arena offsets without rebuilding Phase 3. Add instance LOD group linkage and editor metadata save/load.
- [x] Correct `HiZReduceCS.hlsl` odd-size footprints and move Hi-Z copy/reduction barriers to RenderGraph. Generate Phase 4 Hi-Z from dedicated GPU eligible opaque draws so alpha-tested CPU fallback geometry cannot occlude objects.
- [x] Add `GPUOcclusionCS.hlsl` and `GPULODSelectionCS.hlsl`; integrate between Frustum and Compact, add selected mesh/LOD to Visible records and update Command Generation.
- [x] Add feature toggles, Hi-Z mip preview, state/LOD/bounds debug, per-pass timings and debug-only statistics readback. Preserve Phase 3 checks by explicitly disabling Phase 4 in its validation run.
- [x] Add `gpuvisibilityvalidation.h/.cpp`, `tests/gpuvisibility_tests.cpp` and `scripts/validate-gpuvisibility.ps1` for image equivalence, wall occlusion, camera motion/cuts, five-level LOD, hysteresis, 50k/100k stress and three-mode comparisons.
- [x] Build Debug and Release, execute CPU tests, Phase 3 regression and Phase 4 debug-layer/GBV validation. Review changed code and record actual measurements and any unverified requirements in `docs/gpudriven-phase4.md`.
