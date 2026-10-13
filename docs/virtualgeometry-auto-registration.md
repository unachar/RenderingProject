# Nanite automatic registration and editor debugging

## Behavior

Static model import, the procedural StaticModel factory, and PrimitiveGeometry vertex upload now acquire shared VirtualGeometry assets automatically. Existing startup objects and models loaded while rendering use the same path. No per-object component or manual registration is required. Generated terrain uses the primitive vertex-upload path, including subsequent edits.

Full position, normal, UV and color attributes are preserved when leaf clusters are mapped to the source vertex buffer. The cache identity includes geometry, material-part identity and integration version. Valid assets are shared in memory and stored under `asset/virtualgeometry-cache`. Invalid cache files are rebuilt. When protected boundaries prevent hierarchy simplification, exact leaf clusters remain drawable and the inspector explains the limitation.

Supported opaque PBR geometry and opaque Auto static models use the Mesh Shader renderer by default. This includes the default Auto material setting used by editor model drag-and-drop; its material-part parameters remain intact. Auto models retain their existing CPU toon-outline pass, with their base geometry drawn only by the GPU path. GPU-driven indirect drawing and traditional drawing remain fallbacks for missing clusters, unsupported hardware, transparency, animation, vertex deformation, special shaders and special environment objects. Auto transparent/shadow-only material parts and Auto models with normal maps retain traditional rendering until the GPU shader supports those paths. General Auto primitives retain their existing shader path because their legacy material-part interpretation differs. Existing shadow rendering remains in place.

This integration renders the finest VirtualGeometry clusters of the GPU-selected existing discrete LOD. Continuous cluster-hierarchy LOD selection, streaming and software rasterization are outside this implementation.

## Editor controls

Open **Nanite / LOD** in the Scene toolbar, or expand **Nanite / LOD View** in renderer debugging.

- **LOD View** colors the actual GPU-selected discrete LOD and shows its color legend. Force LOD `-1` restores automatic selection.
- **Meshlet colors** distinguishes individual clusters.
- **Freeze culling camera** retains the culling/LOD camera while the viewing camera moves. Hi-Z and cone rejection are conservatively disabled while frozen.
- **Viewport meshlet bounds**, **cluster bounds**, and **normal cones** show sampled culling data. Selected-object filtering and a cluster limit bound overlay work.
- **Hierarchy bounds preview** selects a source discrete LOD and a hierarchy level; **Leaf triangle wireframe** shows original leaf topology.
- Cache/build counters, CPU time, geometry arena/upload size, hierarchy statistics and fallback reasons help diagnose registration.
- **Return to normal view** resets visualizations, forced LOD, camera freeze and requested debug readbacks.

Viewport overlays are drawn over the Scene image without a depth test. They are diagnostic outlines and can show occluded geometry. Culling records are capped at 4096 candidates; synchronous readback can stall the GPU when enabled. Hierarchy/wireframe previews use CPU asset data. Debug-off clears culling samples and stops their readback.

## Ownership and update decisions

Leaf vertex indices remap to the existing geometry arena rather than allocating a second attribute arena. This preserves seams and the renderer's vertex/material contract. Bounds and normal cones are recalculated using the renderer's existing float arithmetic before validation.

Procedural meshes retain CPU source data, reuse uniquely owned vertex buffers after a GPU idle wait, and separate shared sources when edited. GPU Scene keeps stable mesh IDs for uniquely referenced edits and retires old vertex resources after the appropriate frame fence. Primitive arena reservations are reused, including resolution changes; unused meshlet registrations are compacted once per frame. Uploads stage each edited source once.

Procedural/primitive triangle meshes now generate four simplified discrete LODs during vertex upload. Identical complete vertex attributes are welded for simplification; the resulting indices map back to the original vertices, retaining UV/normal seams and the traditional triangle-list fallback. Reduction targets are 50%, 20%, 8%, and 3% of source triangles, subject to geometric error limits. Levels that cannot be simplified fall back to a finer available level.

Generated meshes automatically register their LOD group when no LOD component is attached. The global GPU LOD toggle and Force LOD select these levels. An explicit LOD component controls per-object enablement and custom screen thresholds; asset settings use the default generated thresholds. LODs share the base vertex arena and keep reusable index reservations and mesh IDs across edits. CPU fallback still draws the original mesh.

## Validation

Procedural GPU LOD integration (2026-10-13): Debug build and Release clean rebuild passed. The final serialized `scripts/validate-meshlets.ps1 -Configuration Debug -SkipBuild` run completed all 159 frames with zero validation failures and zero D3D12 errors. A 32x32 terrain drew 2048, 1024, 408, 163, and 61 triangles at forced LOD 0–4; editing and explicit per-object disable/re-enable also passed. Repeated edits retained stable mesh IDs, bounded arena size, and one shared vertex upload plus the current LOD indices. Traditional/Mesh Shader GBuffer comparison and existing clone/edit/delete checks passed. Evidence: `build/meshlets/runtime-Debug.log` and `comparison-Debug.csv`. Release runtime and GPU Based Validation were not rerun for this integration. An earlier run overlapping the Release rebuild had three GBuffer comparison mismatches; the serialized final run passed without relaxing the image comparison.

- Portable VirtualGeometry Debug and Release suites: passed, including source-triangle preservation, seams, shared ownership, cache identity changes, cache reload/corruption recovery and invalid source fallback.
- Read-only follow-up review: no further actionable issues found in primitive mesh ownership, arena reuse, mesh-ID reuse or meshlet compaction. The first review's repeated-edit resource growth findings were addressed. A final Auto-model review found missing GPU normal-map support; Auto normal-mapped models now preserve traditional rendering, with a regression check.
- Engine Debug build and clean Release rebuild: passed. Release required regeneration of stale/corrupt compiler/linker debug intermediates; no project build settings were changed for that repair.
- `scripts/validate-meshlets.ps1 -Configuration Debug`: passed, extended 144-frame suite, zero validation failures and zero D3D12 Debug Layer errors. Evidence: `build/meshlets/runtime-Debug.log` and `comparison-Debug.csv`.
- `scripts/validate-meshlets.ps1 -Configuration Release -SkipBuild`: passed, final 150-frame suite and zero validation failures. Release does not expose the D3D12 Debug Layer queue. Evidence: `build/meshlets/runtime-Release.log` and `comparison-Release.csv`.
- `scripts/validate-meshlets.ps1 -Configuration Debug -SkipBuild -GpuValidation`: passed, final 150 frames, zero validation failures and zero D3D12 errors with GPU Based Validation enabled. Evidence: `build/meshlets/runtime-Debug-gbv.log`.
- Forced unsupported-hardware fallback: passed, including the GPU-driven 84-frame regression (`scripts/validate-meshlets.ps1 -Configuration Debug -SkipBuild -ForceFallback`). Evidence: `build/meshlets/fallback-Debug.log`.
- The broader `scripts/validate-gpuscene.ps1 -Configuration Debug -SkipBuild` stress run did not complete: device hang during the 10,000-object workload, around frame 70. Raster-only, Mesh Shader-disabled and fully CPU geometry drawing runs reproduced the hang around frame 69. The CPU-only diagnostic is enabled by `DX12_GPU_SCENE_CPU_GEOMETRY=1` while GPU Scene validation is active. This isolates geometry drawing paths but does not establish the underlying cause. Earlier sharing, dirty upload, temporal settling and deletion checks passed; the full suite is not reported as passing. Evidence: `bin/validation/Debug-features/init_log.txt`, `build/gpuscene/runtime-Debug-raster.log`, `runtime-Debug-raster-legacy.log` and `runtime-Debug-raster-cpu.log`.
- Dedicated terrain sharing, editing and deletion checks passed in both 144-frame Debug/Release Nanite suites: clones share a mesh registration, editing separates their source/vertex buffer/mesh ID while preserving the original, and the edited clone remains drawable after original deletion.
- The final 150-frame Release and Debug GPU Based Validation suites also passed default Auto imported-model GPU routing, retained outlines, traditional/Nanite GBuffer comparison and fallback eligibility for normal maps and forward-only material parts.

GPU validation adds procedural terrain generation and edits, CPU/Mesh Shader GBuffer comparison, actual forced GPU LOD, frozen camera behavior, viewport outline generation, debug-off cleanup, material-part changes and repeated terrain resolution edits. Repeated edits check stable mesh IDs/table size, bounded arena size and upload volume.

Interactive external file drag-and-drop and visual inspection of the editor controls have not been performed. Import during rendering is exercised through the same model loader. Existing unrelated working-tree changes have been preserved; no commit has been created.
