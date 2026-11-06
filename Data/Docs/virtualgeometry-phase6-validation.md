# VirtualGeometry Phase 6 validation

Date: 2026-10-15. Checkout: `dev_main`. Hardware: NVIDIA GeForce RTX 3050 Laptop GPU, D3D12 Mesh Shader tier 1 (reported enum value 10).

## Executed evidence

- `scripts/validate-virtualgeometry-phase6.ps1 -SkipCpu -Runtime -Configuration Debug`: production GPU shader/reference comparison (28 cases), native Level 3 raster readback with nonzero vertex/triangle offsets, and frame-pipeline acceptance.
- The corresponding Release command also passed the extended 36-frame sequence. Both configurations include the material comparison and unsupported-material fallback depth check.
- `scripts/validate-virtualgeometry.ps1`: Debug CPU selection, packing, builder and serialization tests passed; Release also passed the million-triangle stress test.
- `scripts/test-shader-build-targets.ps1`: independently tracked shader output/dependency tests passed.
- Existing same-working-tree `build/meshlets/runtime-Debug.log` and `build/visibility/runtime-Debug.log`: zero failures and zero D3D12 errors. These prior results are reused; they were not rerun in this continuation.

Runtime results are stored in `bin/validation/virtualgeometry-phase6/runtime-*.log`; production shader results in `gpu-tests.log`. Captured actual selection output is in `build/virtualgeometry-phase6/{forced-leaf,forced-parent,near,far}.png`. The near capture was inspected: green, cyan and blue cluster regions are visible within one mesh, without interior background gaps.

## Phase 6 acceptance evidence

| Requirement | Evidence |
| --- | --- |
| Nonleaf clusters rasterize | Production Mesh Shader Level 3 readback; forced parent runtime capture. |
| Approaching camera refines | Near/far count comparison plus return-to-near count equality. |
| Receding camera coarsens | Far selection contains fewer clusters than near selection. |
| Different levels within one mesh | GPU level counters include multiple levels; near image confirms actual colors. |
| Parent/child transitions have no cracks | Parent and mixed-level raster coverage masks exactly match leaf coverage on the curved 18,432-triangle fixture; shared-group CPU/GPU tests also prove nonduplicated cuts. |
| Force Hierarchy Level works | Forced leaves and nonleaf runtime assertions; forced-level GPU reference cases. |
| LOD colors reflect selected clusters | Native raster reads cluster Level; images and parent color readback. |
| Existing Mesh Shader fallback preserved | Native-disabled runtime uses legacy path; existing Meshlet integration suite passes. |
| Unsupported geometry uses traditional rendering | GPU eligibility/routing tests and existing legacy suite; extended Debug/Release runtime Unlit case confirms traditional routing and visible center depth. |
| Existing discrete LOD path remains available | Native-disabled runtime assertion and existing GPU Visibility integration suite. |

## Review and limits

The extended Debug and Release material reference comparisons each checked 4,320,000 actual texel bytes across base color, normal, material and emission/AO; zero bytes differed beyond the one-byte tolerance. Unsupported Manual Unlit material correctly left GPU-driven ownership and retained visible center depth. Both 36-frame sequences completed with zero validation failures; Debug also reported zero D3D12 errors.

A fresh read-only reviewer found no critical or important defects in inspected resource offsets, group closure, bounded queue/output reservations, exclusive fallback routing and frame-fence ownership.

Coverage comparison proves the tested view/fixture, not arbitrary imported meshes or every camera angle. Native velocity is compiled and integrated, but a moving-native-versus-traditional velocity image comparison is still unperformed. The earlier Debug GPU-based validation run reported zero errors with D3D12 warning 1044 about tracking limitations around indirect dispatch; this does not prove complete GPU validation coverage. Release has no debug-layer error evidence.

The material equality case uses untextured PBR; textured PBR and Toon equality are not established by this comparison. Both reference/comparison frames additionally require visible center depth so matching blank images cannot pass.

The generic feature smoke sequence previously timed out with DXR/DLSS both with native enabled and disabled. Native acceptance isolates those features by setting `DX12_DISABLE_DXR=1` and `DX12_DISABLE_DLSS=1`; the generic timeout remains unresolved.

Traversal metadata closure scans all groups per hierarchy level. Geometry selection is GPU driven, but pixel-proportional metadata cost is not established. Phase 7 page streaming, Phase 8 hysteresis/two-pass occlusion, Phase 9 material binning and later compression are not implemented by Phase 6.
