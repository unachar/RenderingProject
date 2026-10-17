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
