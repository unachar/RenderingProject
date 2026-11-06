# Material graph runtime work

The requested outcome is a material graph whose saved identity survives restart, whose node previews show the actual expression, and whose connected expressions drive scene rendering as time, object position and view change. The existing texture-bake path is insufficient for dynamic nodes and will not be treated as completing that requirement.

Work order:

1. Reproduce and fix false GUID collisions when the browser registers assets already discovered at startup. Preserve existing metadata. Check restart, path spelling, material opening and thumbnail generation with actual project assets.
2. Repair the existing engine validation against the current area-based editor. Restore early preview creation and exercise visible interactive controls rather than obsolete window identifiers or clipped rectangles. Verify each remaining failure and keep the assertions meaningful.
3. Trace material evaluation across deferred, forward, visibility/GPU-driven and ray paths. Integrate a GPU expression representation with actual per-pixel UV/world/view/time context, rather than substituting averaged constants or frozen baked images. Reuse node definitions/math and retain a working material while an edited graph is invalid.
4. Test numerical CPU/GPU agreement for the supported nodes, selected branch/type behavior, animated UVs, camera-dependent expressions, normal/mask/optics outputs, save/reload and scene restart. Verify the node editor and production rendering together in the running engine.

The design must preserve existing scene/material serialization, legacy materials and asset GUIDs. Do not change project files or metadata merely to conceal missing references. Only mark the goal complete after the dynamic runtime behavior and the previous failures are verified.

## Current verified state

Steps 1 and 2 pass: native desktop verification showed restored thumbnails, existing-GUID material opening and node images. Both `validate-materials.ps1 -Configuration Release -SkipBuild` and its Debug equivalent report zero failures. Debug startup had timed out compiling the shared material mesh shader without optimization; it now keeps debug symbols with optimized GPU code and passes startup plus the D3D12 error checks. Transitive shader dependencies and the color-management parallel job are also covered by the build-contract check.

Steps 3 and 4 remain open. The engine still uses the existing bake/average translation; this is not the requested final runtime behavior. Investigate GPU expression execution next. The existing HLSL generator already handles all value nodes and separates parameter values from shader text; its generated programs currently target isolated previews only. Production material inputs enter `shader/hlsl/MaterialSurface.hlsli`, `materialbindings.cpp`, `gpuscene/gpuscenetypes.h`, forward/deferred/visibility shaders and ray-hit shading. Preserve those paths rather than only making the editor preview dynamic.

Relevant constraints: GPU material shading parameters currently contain 192 float4 vectors; the PBR constant buffer has the same 3072-byte capacity. General programs use `GraphParameters` (b0) and `GraphTextures` (t0) in the isolated compiler output, so these bindings cannot be inserted unchanged into production shaders. Choose bindings/dispatch with actual UV, world position, normal, camera direction, tangent basis and time, and test CPU/GPU agreement. Do not implement animation by continuously baking PNGs or treat fixed context/averaged output as completion.
