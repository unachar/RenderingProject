# Material graph fixes and validation (2026-11-03)

## Fixed

- Node previews passed `ImTextureData*` to the legacy `ImTextureRef(void*)` constructor. This interpreted a CPU pointer as a GPU descriptor. Use `GetTexRef()` so the backend resolves the uploaded texture.
- Released preview textures were recreated by `SetStatus(Destroyed)` because destruction intent was not set. Set `WantDestroyNextFrame` and let registration own its reference count.
- Generated HLSL used scalar vector constructors that fail for runtime parameters. Broadcast scalar components explicitly and pad smaller vectors explicitly.
- DDX/DDY of an unconnected input generated an unsuffixed literal and selected an unsupported double derivative overload. Emit float literals.
- Static Switch output types and dependency flags included the inactive branch. Follow the selected branch, matching evaluation and HLSL generation.
- CPU RGB-to-RGBA promotion used alpha zero while HLSL used alpha one. Match the HLSL behavior.
- RGB Base Color inputs retained stale material alpha. Reset RGB alpha to one, and multiply explicit Opacity with the resulting base alpha.
- Constant Opacity Mask inputs only affected the preview. Bake them to the engine's alpha texture, including entirely clipped masks.

## Repeatable checks

Run `scripts/test-material-graph.ps1` for CPU node evaluation, preview draw-command texture references, texture retirement, Static Switch semantics, material translation, mask/MR baking, deterministic baking with one/two workers, and DXC compilation of all value nodes with compatible scalar/vector input variants (453 shaders).

The test injects texture samples and aborts on file IO; it does not test actual texture loading or GPU rendering. Run `scripts/validate-materials.ps1 -Configuration Release -SkipBuild` against a freshly built engine for the existing asset/UI/rendering checks. The validation opens Properties explicitly because it operates those controls. The POM boundary test uses the existing serializer/shader limit of 1024 steps.

Current results: Release build PASS; graph tests PASS; 453 DXC shader variants PASS; material asset tests PASS. The Release engine validation now passes with `Material validation failures = 0`, including all nine previously failing checks. Debug build and runtime validation also PASS with zero failures and zero reported D3D12 errors (`build/material-graph-runtime-debug.log`). Debug mesh shaders retain debug symbols with GPU optimization, avoiding the startup timeout caused by unoptimized shared material code.

The startup/browser GUID regression is also fixed. Re-registering a file with another Windows path spelling must refer to the same filesystem identity rather than mark its GUID ambiguous. Root containment uses Windows component comparison, and real duplicate metadata on different files is still rejected. Existing GUID metadata is preserved. A regression covers restart, generic separators, case variations and true duplicates.

Desktop visual verification confirmed that material thumbnails return, existing materials open, and node images render. Initial workspace registration now precedes material-open requests. Focus requests survive initial docking until the material tab is selected, and floating areas are kept in the visible work area. UI validation uses actual root-window IDs, scrolls to the tested controls and clicks slider tracks rather than their labels. Opaque studio previews also receive the controllable direct light.

The shader build contract check exposed missing transitive includes and a missing color-management parallel job. Both dependency lists are corrected; `scripts/test-material-shader-build.ps1` passes both dependency and parallel-coverage checks.

## Existing engine limitations

The production material path consumes constants and textures. General graph expressions are baked, rather than executed as a per-pixel graph shader in the scene. Time/Panner/Rotator use the configured bake time; camera/world-dependent expressions use the bake's fixed plane context. Changing the camera or advancing scene time does not reevaluate these expressions in the scene.

Normal intensity, parallax controls, IOR, transmission, thickness and shading strength are scalar engine parameters: spatial expressions connected to these inputs are averaged. Absorption uses the center sample. The editor reports these approximations. Shader compilation coverage alone does not establish complete Unreal Engine equivalence or correctness for every possible graph.
