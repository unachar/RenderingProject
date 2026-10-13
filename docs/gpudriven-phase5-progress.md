# Phase 5 implementation / validation ledger

## Execution and rulings

- User approved design/implementation and confirmed local Mesh Shader support.
- Native execution preserving the Phase 4 baseline and concurrent user changes. This task performed no commits, resets, merges or pushes.
- Hardware: NVIDIA GeForce RTX 3050 Laptop GPU, Mesh Shader Tier 1 (enum 10), SM 6.5 accepted.
- Global Range table keyed by selected GPU Mesh ID preserves existing Mesh ABI.
- Cluster generation/culling share one Compute pass, 64 threads/object. Cluster-culled children receive reason sentinels without bounds/cone/Hi-Z testing. Cluster cones disabled conservatively.
- Original Phase 4 commands remain for Hi-Z. Separate fallback commands preserve whole objects on capacity reservation failure.
- Buffers reuse existing capacity/frame/fence lifetime. Allocation/PSO failure retains Traditional.
- Development import builds/caches LODs. Release loads prebuilt cache, falling back on absence/corruption. No per-frame build.
- Actual meshlet rendering covers support/PSO/probe checks; no permanent triangle probe retained. PBR source recompiled as DXIL because DXBC PS cannot share Mesh Shader pipeline.
- Instructions: [Phase 5](gpudriven-phase5.md).

## Verification

| Check | Evidence |
|---|---|
| CPU RED/GREEN | Missing builder failed first; reconstruction, limits, invalid indices/ranges/bounds/cones/clusters, cache key/truncation, dispatch boundaries now PASS |
| Cache corruption RED/GREEN | Same-size in-range triangle mutation accepted before fix; payload checksum v2 rejects it; suite PASS |
| Debug build/GPU | validate-meshlets.ps1 PASS: 112 frames, failures=0, D3D12 errors=0; build/meshlets/runtime-Debug.log |
| GPU Based Validation | Final comparator fix build +112 frames PASS: failures=0, D3D12 errors=0; build/meshlets/runtime-Debug-gbv.log |
| Forced unsupported feature | validate-meshlets.ps1 -SkipBuild -ForceFallback PASS. Full Phase 3 regression, 84 frames, failures=0, errors=0. Supported=0 and no Mesh Shader PSO initialization; build/meshlets/fallback-Debug.log |
| Phase 4 regression | validate-gpuvisibility.ps1 -SkipBuild PASS: 140 frames, failures=0, D3D12 errors=0; build/visibility/runtime-Debug.log |
| Release build/GPU | Full Rebuild previously PASS; final incremental build +112 frames PASS including legacy range/comparator fixes, failures=0; build/meshlets/runtime-Release.log. Debug queue unavailable in Release; error evidence is Debug/GBV |
| Independent review | Fixed double-sided fallback, payload checksum and occluder raster inheritance; final targeted review found no further issue. Reviewer ran no tests |
| Whitespace | git diff --check PASS |

Debug covers Cube/Sphere, 1,048,352 triangles, 1000 shared high-poly instances (visible subset), 50,000 submesh instances, LOD0–4, internal/fast camera, mirror/non-uniform transforms, double-sided cone disabling, capacity/asset fallback, newly imported legacy asset zero-range uploads, all culling stages and GBuffer/velocity comparison. Half occlusion rejected 22,544 meshlets without image changes. Capacity fallback compared from inside a double-sided sphere.

Moving velocity: expected +/-0.01005922 UV, observed +/-0.01005554 across 14,520 pixels. Reference pairs with changed camera/scale history compare geometry only; equal-history raw UV tolerance 1e-5. Encoded display channels amplify near-zero float errors.

## Performance (Release, illustrative short sample)

Same million-triangle asset/partial view, four settled samples per path:

- Traditional opaque 1.04115 ms; Mesh opaque 0.13235 ms + Compute 0.31667 ms.
- Total GPU: Traditional 3.14293 ms; Mesh 2.59166 ms.
- CPU submission: Traditional 0.01880 ms; Mesh 0.02810 ms. No CPU improvement claim.
- Processed primitives: 1,048,352 to 60,333; 2,873 visible meshlets.
- Serial candidate generation originally cost about 4.65 ms Compute; group parallelization/per-cluster statistics reduced it.

Readback/capture is test-only. These samples do not establish speed for every scene or sustained clock state. Opaque timestamp includes raster/pixel/fallback work. Pixel Shader stage-only timing, DLSS/FSR-enabled quality and manual Debug UI operation have not been validated.

GBV required a long first-frame preparation (high CPU and memory use), then completed successfully. Its instrumented frame timings are not used for performance claims.

Release incremental build hit C2855 because the existing PCH used a different debug-information option. Added an explicit -Rebuild switch, matching the existing validation scripts, and reran a full Release build with /Z7.

Final lifecycle review added explicit uploads when the Range table grows for legacy assets without metadata. Frames 82/83 now import such an asset during rendering, compare fallback output and verify its zero GPU Range. Debug, final GBV and Release revalidation PASS including frames 82/83.

An intermediate final GBV run completed 112 frames with zero D3D12 errors but failed the first image comparison (7,616 changed bytes). The comparator incorrectly included undefined CopyTextureRegion row padding. It now checks actual texel bytes, validates dimensions/format/pitch before comparison, and reports padding changes separately. The final GBV run reproduced exactly 7,616 changed padding bytes with zero changed texel bytes in that comparison, confirming the false failure. Debug texel-only, final GBV and Release revalidation PASS after the comparator fix. Investigation log: build/meshlets/runtime-Debug-gbv-padding-investigation.log.

Validation script rejects incompatible -SkipBuild/-Rebuild arguments before running: PASS. Final comparator dimension/format/pitch guard and row loops reviewed independently; no further findings.
