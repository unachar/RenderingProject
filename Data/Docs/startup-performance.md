# Startup performance

Runtime DXC compilation previously repeated on every application launch. The
in-memory shader blob cache only lasted for one process, so DXR variants,
reflection/GI helpers and mesh shaders blocked initialization again next time.

The DXC compatibility adapter now stores compiled DXIL and diagnostics in
`EditorCache/Shaders`. It preprocesses each requested shader and keys the cache
on the actual transitive include contents, original source, compiler arguments
(including target, entry point, macros and optimization flags), and a hash of
the loaded compiler DLL. Preprocessing still checks dependencies on a cache hit;
the expensive optimizer and code generator are skipped.

Cache files have a version marker, length bounds and a payload checksum. Writes
use a temporary file followed by replacement, so interrupted or simultaneous
application launches cannot expose a partial payload. Missing, invalid or
unwritable cache entries fall back to normal compilation. Failed compilations
are not cached. Shader optimization and rendering settings are preserved.

The first launch after a shader/compiler change still compiles the affected
variants. Removing `EditorCache/Shaders` is safe and causes regeneration.

## Validation and measurement

- `scripts/test-shader-cache.ps1`: verifies reuse in the same process and a new
  process, byte-identical DXIL, nested include changes, macro/entry/flag variants,
  corrupt files, an unavailable cache directory and missing dependencies.
- `scripts/measure-startup.ps1 -DisableShaderCache -Label uncached`: measures the
  original compilation path without deleting existing cache files.
- `scripts/measure-startup.ps1 -Label cached -Runs 3`: measures persisted reuse;
  the first run also fills any entries that do not yet exist.
- `DX12_STARTUP_TIMING=1`: adds elapsed milliseconds to graphics log messages.
- `DX12_DISABLE_SHADER_CACHE=1`: bypasses cache reads and writes for diagnosis.

The startup script restores the saved scene and editor layout, renders three
frames and checks normal shutdown. It preserves the user's settings files.
Logs are saved under `bin/validation/startup`. The metric runs from the
`window shown` milestone to `first frame complete`. Because automation launches
the window hidden, it is an initialization/first-frame proxy, not a direct
measurement of visible presentation or an injected input event. It excludes
process loading before WinMain and does not measure when all asynchronous scene
assets have finished loading.

## Measured result (2026-10-17)

Release x64, NVIDIA GeForce RTX 3050 Laptop GPU, saved
`asset/Scenes/MegaLights_NightCity_2000.scene`, restored editor layout:

| Shader cache | Window milestone to first frame, three runs | Median |
| --- | --- | --- |
| Disabled (original compilation path) | 45.737 s, 39.687 s, 38.986 s | 39.687 s |
| Persisted cache | 32.243 s, 34.065 s, 36.631 s | 34.065 s |

The median fell by 5.621 seconds (14.2%). A separate cache-populating launch
took 35.595 seconds and was excluded from the persisted-cache median. Runs were
sequential on the same machine; these are local observations, not a guarantee
for other hardware/scenes. The remaining initialization and scene/render-resource
creation costs are substantial; this change does not make the whole startup
asynchronous. Raw logs and `measurements.json` are in `bin/validation/startup`.

Release build succeeded using the Visual Studio developer environment with
`_CL_=/Z7 /MP4`. Direct MSBuild without that environment first failed at link
with an MSPDB140.DLL version mismatch; the developer-environment build resolved
it without changing project optimization settings.

The cache regression suite passes, including reuse by a second process.
Procedural editor checks (976) and simulation math checks (2801) pass. The
84-frame rendering feature sequence completes but reports one failure in
`NRD reduces reflection high-frequency variation in the test scene`. Repeating
the same sequence with `DX12_DISABLE_SHADER_CACHE=1` produces the same failure
(raw reflection variation is zero in both runs). Therefore the full rendering
suite is not claimed to pass. Both logs are preserved as
`features-cache-enabled.log` and `features-cache-disabled.log` in the startup
validation directory.
The DXR/DLSS-disabled fallback feature sequence passes all checks over 84 frames
(`fallback.log`).

## Initialization changes (2026-11-03)

The editor postprocess no longer initializes OCIO before drawing its first
frame or reloads its configuration whenever exposure changes. The GPU OCIO
display-transform integration is currently empty; the final shader already
handles exposure. Explicit `ColorManager::Initialize` and the color conversion
APIs remain available for consumers that actually use OCIO transforms.

Raytraced shadow/AO, radiance, mesh visibility/MRT, GPU-driven opaque/occluder
and indirect compute, and the meshlet/native virtual geometry pipelines use the
persistent driver PSO cache in `bin/cache/pso`. Mesh cache keys hash shader
contents and fixed pipeline state, not stream pointers. The driver validates
cached blobs; rejected or missing entries fall back to normal pipeline creation.
An accepted entry is no longer exported and rewritten on every launch.
Blend/depth state keys hash named fields rather than uninitialized structure
padding, so identical pipelines retain the same keys across launches. Graphics
keys are versioned again; those entries regenerate once. Initial cache
population and changed shaders still require driver compilation.

Additional startup milestones distinguish shadow/AO, individual radiance shader
entries, OptiX and graphics pipeline creation. The measurement script accepts
`-TimeoutSeconds` and an optional `-Executable` for comparison with a preserved
binary; timed-out runs retain their diagnostic log.

`scripts/test-startup-initialization.ps1 -Log <startup-log>` checks successful
first-frame completion and shutdown, DXR and mesh pipeline readiness, and the
absence of unused OCIO initialization. This check targets the DXR/mesh-capable
hardware used here; it is not a capability-independent fallback test.

### Measurement and verification

Release x64, NVIDIA GeForce RTX 3050 Laptop GPU, restored
`asset/Scenes/NewScene.scene` and editor layout, three frames per launch:

| Run | Window milestone to first completed frame |
| --- | ---: |
| Preserved original executable, no concurrent build | 173.293 s |
| Updated executable, populating the last added meshlet/native caches | 74.583 s |
| Updated executable, reusing caches | 25.967 s |
| Updated executable, one deliberately corrupt cache entry regenerated | 25.469 s |

The normal cached sample is about 85% shorter than the preserved executable.
These are individual local samples, not a multi-run statistical benchmark.
The population run already had raytracing and GPU-driven cache entries from
earlier validation; it does not represent an entirely empty cache. Initial
process loading, physical display presentation and completion of asynchronous
scene asset loading are outside this first-frame metric. A preliminary baseline
that overlapped compilation was excluded from this comparison.

Both updated normal launches and the corruption-recovery launch passed the
startup checks, including DXR, visibility/MRT and meshlet/native indirect
pipeline readiness, successful three-frame completion and normal shutdown.
No device-removal log growth was observed. Thirteen newly generated startup
cache files retained their exact timestamps across subsequent launches,
confirming that accepted entries were reused without rewriting them.
The corrupt entry was regenerated from 16 invalid bytes to a valid 1886-byte
driver blob, then its original contents were restored.

Release x64 build passed. Raw logs and JSON measurements are under
`bin/validation/startup`, with labels `baseline-clean`, `optimized-init` and
`pso-recovery`. The scope of these checks is startup; the full rendering
regression suite was not rerun.

## Asset readiness before showing the editor (2026-11-03)

Normal startup now keeps the main window hidden until the restored editor has
rendered its initial contents. All registered materials and their referenced
textures are loaded beforehand using the existing parallel texture decode and
batched GPU upload path. Saved material graph programs are prepared in the same
stage. Material sphere/plane/cube preview pipelines and the graph preview compute
pipeline are also prepared before interaction, and use the persistent driver PSO
cache. A changed shader or missing cache still requires compilation once.

The hidden frames publish the initial asset-browser directory listing and finish
thumbnail requests for the visible cells. GPU submission is completed before the
window is shown. This covers the restored scene, material dependencies and the
initial view; opening another folder still loads its thumbnails on demand.
Invalid thumbnails retain their existing failed/error state rather than blocking
startup forever. The hidden-frame wait has a 120-second failure limit; it does
not expose an unfinished editor on timeout. Automated rendering validation keeps
its own frame sequence and bypasses this preparation stage.

`test-startup-assets.ps1 -Log <startup-log>` checks the order: assets prepared,
assets ready, window shown, first visible-loop frame complete. The measurement
script now additionally reports `WinMainToAssetsReadyMs` and
`WinMainToFirstFrameMs`. Use these total startup metrics for comparisons:
`WindowToFirstFrameMs` now excludes the work deliberately performed before showing
the window, and cannot be compared with the older visible-window startup metric.
The timing still excludes process loading before WinMain and does not measure
physical display presentation.

The graph preview also had two compile failures that forced CPU evaluation:
the global bindless define selected scene-only `AssetParallax` constants even
though the editor uses an explicit texture table, and an HLSL conditional tried
to select between whole `PreviewPoint` structs. The standalone compute shader
now explicitly uses its texture-table binding and selects the preview point
with `if`/`else`. `scripts/test-startup-graph-shader.ps1` compiles this production
shader with both global bindless settings. Add `-RequireGraphGpu` to the startup
asset check when validating a GPU-capable target; it rejects CPU fallback.

On the local RTX 3050 Laptop GPU with the restored `NewScene.scene`, the final
Release build prepared 26 materials. After the graph-shader fix, total WinMain
to asset-ready time was 37.849 s while populating the graph preview cache and
29.765 s on the next cached launch. Both launches passed the asset-order checks
with `-RequireGraphGpu`, DXR/mesh startup checks, three rendered smoke frames,
normal shutdown and no device-removal log growth. These are individual local
samples; they are not a benchmark against the old asynchronous asset-ready time,
which was not measured. The visible-loop frame took a further 6.4/4.7 ms after
the window-show milestone, which excludes the hidden initialization work.
Raw results: `bin/validation/startup/assets-graph-fixed-measurements.json` and
the matching per-launch logs.

Release x64 build, material asset tests (including type-filtered startup
registration), asset-browser listing tests, production graph-shader compilation
for both binding settings, thumbnail material contracts and preview UV tests
passed. The material editor runtime sequence also reached
`Material validation failures = 0`, including transparency preview capture,
Ctrl+S, Undo and Redo. The Release runtime did not expose a D3D12 debug queue;
this is not a debug-layer validation claim. The full rendering suite was not
rerun for this change.
The first material-editor validation run reported zero failed assertions but
exceeded the harness's fixed 180-second process timeout after cold pipeline
creation. A rerun with the populated caches passed the complete runtime script
and exited normally (`Material runtime Release: PASS`).
