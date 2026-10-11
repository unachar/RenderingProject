# Raytracing and procedural editor expansion

Work on the user's current `dev_dlss&raytracing` branch. Preserve all existing work and `Save/project_environment.cfg`. This is a native C++/D3D12 renderer. Unity-GameDev supplies the development and validation workflow, not Unity APIs.

## Acceptance criteria

1. Every existing shadow light (directional, point, spot, area) uses scene ray queries when DXR is supported, without the raster shadow-map budget of eight lights. Opaque lighting, forward/transparent receivers and volumetric shadow consumers must agree. Environment controls disable shadows, rather than silently restoring legacy shadows on a capable GPU. Unsupported hardware keeps an explicit compatible raster fallback.
2. Integrate genuine NVIDIA NRD, pinned official SDK and license, with actual GPU dispatches and correct normal/roughness, linear view depth, motion vectors, hit distance and temporal history. SIGMA denoises light shadows; REBLUR denoises reflections/AO if applicable. No stub or renamed blur can count as NRD. Compare raw and denoised GPU output and test motion/history invalidation.
3. Procedural motion has object and vertex modes. Vertex mode changes geometry and normals, shares the result across rasterization, velocity and DXR, has previous positions for temporal consumers, and restores base geometry when removed/disabled. Keep GPU resources alive until their frame fence is complete.
4. Replace deferred SSR with actual scene raytraced reflections that can see geometry outside the screen, with material shading and sky misses. Environment controls enable/disable reflections independently of shadows.
5. Sun entities are virtual directional lights without yellow mesh/material. Existing Sun entities migrate safely. Their transform, target, editor selection and day/night controls remain usable.
6. A stable procedural star field and night sky exist throughout the day. Atmospheric/sun brightness overwhelms stars in daylight. Night captures contain clearly visible stars; no new asset is required.
7. Mesh shaders draw large static and procedural geometry, support both visibility/compute-GBuffer and regular deferred GBuffer routes, bound DispatchMesh group dimensions and shader output limits, and retain indexed/non-indexed fallback on unsupported hardware. Include frustum/meshlet culling and diagnostics demonstrating actual dispatches.
8. Selecting generated terrain loads its parameters; editing any parameter regenerates the selected terrain live when enabled. A persistent setting disables live updates; manual update still works. Preserve undo, play snapshots and motion state.

## Global constraints

- No branch/worktree/commit/PR changes. SDK sources/build products go to ignored directories; installers and license provenance are reviewable.
- Readable names and explicit resource ownership/state transitions. No full-code mechanical rewrite.
- Parent implements every change, including project registration, renderer integration, serialization and validation. Subagents may perform read-only tests/review only. Do not run whole-project builds concurrently.
- Debug and Release builds; CPU tests; real GPU output captures with D3D12 debug diagnostics in Debug; feature toggles, resolution changes and SDK/hardware fallback. Preserve valid earlier DLSS evidence but recheck any affected motion/bindings paths.

## Tasks

1. Parent: extend DXR scene metadata, multi-light shadow rays, reflections, material/sky shading and all lighting consumers. Bind actual resources through frame-owned descriptors and retain geometry until its fence completes.
2. Parent: pinned official NRD SDK installer/build and native D3D12 denoiser. SIGMA filters the stochastic soft Sun shadow, REBLUR_SPECULAR filters reflected radiance, and REBLUR_DIFFUSE_OCCLUSION filters AO. Other lights and volume samples use deterministic binary scene visibility and introduce no Monte Carlo shadow noise. Respect official signal packing, resource formats, resizing and temporal reset contracts.
3. Parent: vertex deformation and live editor update, ECS/history/projectmanager/render consumer edits and focused tests.
4. Parent: virtual Sun and procedural celestial sky, editor integration and validation.
5. Parent: large-mesh dispatch, culling, indexed/non-indexed and both GBuffer modes and ModelSystem/RenderSystem integration.
6. Parent: environment setting migration and UI, meaningful automated fixtures/assertions, compile and GPU validation, targeted refactoring and final review.

## Validation evidence

Record each criterion's implementation and checks in `docs/superpowers/ledger/raytracing-expansion/ledger.md`. Completion requires all eight, with honest hardware fallback/optional SDK status and material limitations recorded. Reuse prior tests only where unchanged inputs/dependencies make them valid.

The latest user instruction restricts subagents to the test phase. All implementation is performed by the parent; earlier implementer agents stopped. No new implementation delegation is permitted. Independent test/review agents can be used once an implementation batch is ready.
