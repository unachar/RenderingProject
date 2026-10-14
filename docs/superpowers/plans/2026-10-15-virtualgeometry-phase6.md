# VirtualGeometry Phase 6 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Render the existing cluster hierarchy using GPU-selected continuous LOD and native Mesh Shaders while retaining all fallback paths.

**Architecture:** Share immutable geometry through a global asset table. Resolve group refinement dependencies on the GPU before producing a complete, nonduplicated hierarchy cut. Draw native selected clusters with indirect Mesh Shader dispatch, alongside existing meshlets and traditional geometry.

**Tech Stack:** C++20, Direct3D 12, HLSL/DXC, meshoptimizer, RenderGraph, GpuSceneBuffer, ImGui, CMake/CTest, MSBuild.

**Spec:** `docs/virtualgeometry-phase6-design.md`; user extension specification sections 8–27 and Phase 6 acceptance criteria.

## Global Constraints

- Repository `unachar/RenderingProject`, branch `dev_main`.
- Preserve the existing hierarchy builder and cache format v1.
- Cluster limits: 64 vertices and 124 triangles; maximum hierarchy levels 32.
- Native vertices retain all 48 bytes; triangles retain three uint8 indices.
- Default target error: 1.0 pixel; ForceHierarchyLevel: -1 for automatic.
- Parent/child replacement operates on complete groups; geometry selection stays on GPU.
- CPU pass count may depend on the bounded hierarchy depth, never on cluster count.
- Phase 7 streaming, hysteresis, two-pass occlusion, material binning and compression are deferred.
- Record executed verification separately from unperformed visual acceptance.

## Review Focus

- Shared generating groups must not duplicate children or leave partial parent coverage (Task 1).
- Near-plane and nonuniform/mirrored transforms must preserve conservative selection/culling (Tasks 1 and 4).
- Queue exhaustion or GPU initialization failure must select a complete fallback (Tasks 3 and 5).
- Shared assets, reimport and material changes must not leave stale GPU references (Tasks 2 and 3).
- Camera cuts and invalid previous Hi-Z must not hide visible geometry (Tasks 4 and 7).

### Task 1: Define and prove group-consistent selection

**Files:** Create `source/virtualgeometry/virtualgeometryselection.h`, `.cpp`, `tests/virtualgeometry_selection_tests.cpp`; modify `tests/virtualgeometry/CMakeLists.txt`, `tests/virtualgeometry_tests.cpp` and `source/virtualgeometry/virtualgeometryvalidator.cpp`.

**Interfaces:** `VirtualGeometrySelection::View` holds local-space camera, conservative world scale and projection scale in pixels. `Settings` holds TargetErrorPixels and ForceHierarchyLevel. `Result` holds selected cluster indices and an error string. Produce `Result Select(const VirtualGeometryAsset&, const View&, const Settings&)` as a CPU test oracle only; runtime selection must not call it.

- [ ] Add failing tests: root-only asset terminates; error below/above 1.0 selects parent/children; near view refines; far view coarsens; an asymmetric graph produces multiple levels. A graph whose generating group has parents consumed by two different groups must replace both parents before selecting any child. Assert selected IDs are unique and symbolic source coverage is exactly once.
- [ ] Run `powershell -File scripts/validate-virtualgeometry.ps1 -SkipMillion`; confirm new assertions fail or missing symbols prevent compilation.
- [ ] Implement projected error with conservative distance-to-sphere, guard epsilon, and half viewport height times projection vertical scale. Implement group requests, ancestor closure and descending cut emission. For each requested group, require refinement of the consuming groups of all its parents; propagate upward in increasing child-level order. Starting from roots, process replacement in descending group-level order. Preserve terminal branches when Force Level cannot descend further.
- [ ] Add invalid-index, cyclic/nonascending dependency, invalid error, zero-distance, forced-level and nonuniform-scale tests. Validate each parent/child linkage and monotonic dependency before accepting assets.
- [ ] Run the same validation; all nonstress tests pass. Review coverage proof before translating to HLSL.
- [ ] Commit the selector and tests as `feat: define group-consistent virtual geometry selection`.

### Task 2: Pack the hierarchy for native GPU consumption

**Files:** Create `source/virtualgeometry/virtualgeometrygpu.h`, `.cpp`, `shader/hlsl/VirtualGeometryCommon.hlsli`, `tests/virtualgeometry_gpu_tests.cpp`; modify the test CMake file and test entry point, `DirectX12.vcxproj` and `.filters`.

**Interfaces:** `VirtualGeometryGPU::PackedAsset` owns GPU clusters/groups, hierarchy indices, roots, native vertices and padded triangle bytes. Produce `bool Pack(const VirtualGeometryAsset&, PackedAsset&, std::string& error)`. GPU records use the supplied specification's fields; add an explicit float4 cone apex record to retain conservative cone data. Hierarchy references remain asset-relative; table offsets resolve them.

- [ ] Add failing packing tests for two assets with nonzero offsets, all hierarchy levels, 48-byte vertex attributes, invalid index rejection and checked uint32 offset arithmetic.
- [ ] Implement records and packing with C++ size/offset assertions and matching HLSL layout. Include HierarchyIndexOffset and hierarchy count in the asset table, which the proposed table omitted. Pad each triangle allocation to four bytes without changing cluster byte offsets.
- [ ] Test byte decoding at offsets 0, 1, 2, 3 and the final triangle; assert reconstructed indices equal source bytes. Assert parent vertices remain distinct from source/leaf vertices where simplification changes them.
- [ ] Run `powershell -File scripts/validate-virtualgeometry.ps1 -SkipMillion`; expect all tests pass.
- [ ] Commit as `feat: pack native virtual geometry hierarchy`.

### Task 3: Register shared assets and upload resources

**Files:** Create `source/virtualgeometry/virtualgeometryresources.h`, `.cpp`; modify `source/rendering/gpuscene/gpuscenetypes.h`, `gpuscene.cpp`, `shader/hlsl/GpuSceneCommon.hlsli`, `source/models/staticmodel.h`, `source/rendering/meshlets/meshletmanager.cpp`, project file and filters.

**Interfaces:** `VirtualGeometryResources::Register(std::shared_ptr<const VirtualGeometryRuntime::RuntimeAsset>) -> uint32_t`, invalid ID UINT32_MAX; `Prepare(RenderGraph&)`, `Ready(uint32_t) -> bool`, `Shutdown()`. Expose named GpuSceneBuffer accessors for seven global buffers and the packed CPU asset table. Store VirtualGeometryAssetIndex in the existing instance padding slot where possible; keep matching HLSL load offsets explicit.

- [ ] Test registration deduplicates identical shared runtime assets; stale or failed registrations yield invalid readiness; repeated instances share geometry allocations.
- [ ] Implement frame-fence-safe upload through GpuSceneBuffer and RenderGraph copy/state declarations. Publish usable asset state only after resources can be consumed. Retain immutable ownership until no queued GPU work references it.
- [ ] Register only VirtualGeometryLODs[0] for native rendering. Preserve existing MeshletLODs and discrete fallback builds during Phase 6; do not combine import optimization with renderer migration.
- [ ] Add allocation-failure and replacement tests; a replaced asset gets a new stable registration, with old buffers retired through existing fences. Clear native eligibility for unsupported meshes/materials/deformation.
- [ ] Run CPU tests and build x64 Debug through `scripts/validate-rendering.ps1`; inspect failures before continuing.
- [ ] Commit as `feat: upload shared virtual geometry resources`.

### Task 4: GPU group decisions, traversal and culling

**Files:** Create `source/virtualgeometry/virtualgeometrytraversal.h`, `.cpp`, `shader/hlsl/VirtualGeometryTraversalCS.hlsl`; modify `Directory.Build.targets`, project and filters. Reuse conventions from `MeshletCullingCS.hlsl`, `ClusterCullingCS.hlsl` and `source/rendering/occlusion/` after locating the existing implementation.

**Interfaces:** `VirtualGeometryTraversal::Prepare(RenderGraph&, const FrameInputs&)` consumes instance/object visibility handles, frame capacity, camera/projection and previous Hi-Z validity. `FrameInputs` uses RenderGraph handles plus explicit raw resources only for root binding. Produce visible 16-byte records `{InstanceIndex, ClusterIndex, MaterialIndex, Flags}`, counters, indirect raster arguments, per-instance native success flags and debug records. Expose these through named buffer accessors.

- [ ] Add a validation mode that reads GPU selections back and compares them with Task 1's oracle for tiny synthetic graphs, including the shared-parent case.
- [ ] Implement reset, root seed, group error requests, upward dependency propagation and descending replacement/emission entry points. Use per-instance group state to deduplicate. Use QueueA/QueueB with explicit capacities and GPU work counts. Level passes require UAV ordering and indirect-dispatch argument transitions; no CPU counter readback drives selection.
- [ ] Make group culling conservative. Apply cone tests to actual selected geometry, unless a validated subtree cone exists. Disable cone rejection on transformations the chosen test cannot conservatively support. Disable previous Hi-Z rejection when history is invalid, including camera cuts.
- [ ] Bound scratch using validated asset sizes and visible-instance capacity. Reserve output per instance before emission, or stage it so failed instances cannot publish partial geometry. On failure set native success false and emit no native records for that instance.
- [ ] Compile all compute entry points as cs_6_5 and compare readback for near/far, forced levels, mixed depth, frustum edge, camera cut and deliberate capacity exhaustion. Expect oracle agreement with occlusion disabled and conservative visibility with culling enabled.
- [ ] Commit as `feat: select virtual geometry clusters on GPU`.

### Task 5: Native Mesh Shader and exclusive fallback routing

**Files:** Create `shader/hlsl/VirtualGeometryMS.hlsl`, `VirtualGeometryVelocityMS.hlsl`; modify `source/rendering/meshshader/meshshaderrenderer.h`, `.cpp`, `source/rendering/visibility/gpuvisibility.cpp`, `shader/hlsl/GPULODSelectionCS.hlsl`, legacy candidate generation shaders, `Directory.Build.targets`, `source/rendering/framepipeline.cpp` as required by actual pass ownership.

**Interfaces:** Add `VirtualGeometryRenderer::Prepare`, `Draw`, `DrawVelocity`, `EndFrame`, `Shutdown` in dedicated `source/virtualgeometry/virtualgeometryrenderer.h`, `.cpp`; mirror existing MeshShaderRenderer frame lifecycle. Consume Task 4 output and Task 3 resource accessors. Reuse compatible opaque/velocity pixel shader outputs and material semantics.

- [ ] Add routing tests: native-ready supported opaque instance uses only native output; native-disabled instance uses existing discrete LOD; unsupported material/deformation uses traditional output; native allocation or selection failure uses one complete fallback.
- [ ] Retain original mesh identity while native candidates bypass GPULODSelection. Keep discrete fallback identity available so per-instance traversal failure can route correctly. Never globally disable LOD for instances without a usable native asset.
- [ ] Implement 64-vertex/124-triangle mesh outputs, byte index expansion, native attributes, world/normal transforms, material lookup, current/previous clip positions and selected-level color output. Compile both Mesh Shaders as ms_6_5.
- [ ] Add native GBuffer and velocity PSOs/root bindings and indirect signatures. Declare all native raster reads in RenderGraph; use GPU native success flags to exclude successful native instances from legacy/traditional candidate paths.
- [ ] Run `scripts/test-shader-build-targets.ps1`, `scripts/validate-meshlets.ps1`, `scripts/validate-gpuvisibility.ps1` and relevant native validation. Build/run x64 Debug and Release via `scripts/validate-rendering.ps1 -Configuration Debug` and `-Configuration Release`. Inspect script parameters first; retain valid previous results when target/environment has not changed.
- [ ] Commit as `feat: rasterize native virtual geometry with fallback`.

### Task 6: Settings, actual selection visualization and profiling

**Files:** Modify `source/virtualgeometry/virtualgeometryconfig.h`, `virtualgeometrydebug.cpp`, `virtualgeometrydebug.h`, `source/rendering/renderprofiler.h`; Task 4/5 instrumentation sites.

**Interfaces:** `VirtualGeometryRenderer::GetSettings() -> VirtualGeometryRenderSettings&` with Enabled, TargetErrorPixels, FrustumCulling, ConeCulling, OcclusionCulling, ForceHierarchyLevel and debug selection options. Keep future-phase controls unavailable with an explicit phase label.

- [ ] Implement enabled, default 1.0 pixel error, force -1/0–31 and culling controls. Color actual selected output: 0 green, 1 cyan, 2 blue, 3 yellow, 4 orange, 5+ red/purple.
- [ ] Show selected clusters/triangles, selected level counts, asset memory, traversal capacity failures and native/legacy/traditional routing counts. Preserve existing CPU bounds preview with accurate labeling.
- [ ] Add profiler events for Nanite Instance Cull, Nanite Hierarchy Traversal, Nanite Cluster Cull and Nanite Mesh Shader; do not report streaming/binning work that does not exist.
- [ ] Build and inspect controls plus selection colors on supported hardware. Verify toggles alter the intended path and enabling/disabling native rendering does not invalidate resources.
- [ ] Commit as `feat: expose native hierarchy diagnostics`.

### Task 7: Acceptance scenes and evidence

**Files:** Create `scripts/validate-virtualgeometry-phase6.ps1`, `docs/virtualgeometry-phase6-validation.md`; extend existing GPU-driven validation hooks rather than introduce a separate renderer harness.

- [ ] Add a deterministic high-poly scene, elongated mesh spanning near/far regions, UV/material boundaries, shared asset instances, unsupported material/deformed mesh and low-capacity fallback scenarios.
- [ ] Run CPU validation including Release stress test through `scripts/validate-virtualgeometry.ps1`; run GPU readback/oracle cases and build/shader checks not already covered by valid evidence.
- [ ] On Mesh Shader hardware, capture near/far/forced-level selections, mixed levels within one mesh and transition closeups. Verify no cracks, holes, doubled geometry, incorrect materials or velocity artifacts. Repeat with camera cuts and native disabled.
- [ ] Record all ten Phase 6 acceptance criteria with evidence, GPU/device details, commands and any unverified condition. Lack of supported hardware leaves visual criteria unverified and Phase 6 incomplete.
- [ ] Review the complete diff for readable names, CPU/HLSL contracts, state transitions, fence lifetimes and fallback completeness. Update design/plan if actual contracts require adjustment.
- [ ] Commit as `test: validate native hierarchy rendering`.

## Execution handoff

Recommended: Native execution in this session, because registration, traversal and fallback share tightly coupled resource contracts. Follow tasks sequentially and preserve verification evidence. Independent review, if chosen, must use an available model without automatically changing account/session settings. Do not push, merge or deploy without a user request.
