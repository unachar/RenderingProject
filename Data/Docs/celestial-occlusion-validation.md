# Closed-room celestial-light leak

## Reproduction and cause

The reported bright spot changes when the Sun moves even in a closed GI room. The shared PBR environment function added a virtual moon's direct light after the regular shadowed-light evaluation. This virtual moon follows the opposite solar direction. Its contribution had cloud extinction, but no scene visibility, so walls and roofs could not block it. This is a lighting integration bug; changing Field to a diffuse shader only avoided that PBR contribution for Field.

A second integration issue prevented the first shader-only correction from taking effect in ordinary deferred lighting: its PSO was built before Raytracing initialization. Shader selection consequently loaded the old shader-model path without inline ray visibility. MegaLights and later-created forward pipelines could select the DXR variant, making the behavior dependent on the render path.

## Changes

- Virtual moon surface lighting now requires a valid scene-visibility provider and traces toward the moon through the existing shadow TLAS. Cloud extinction still applies. Without a visibility provider, the virtual moon remains visible in the sky but does not inject unshadowed direct surface illumination. This means outdoor fallback rendering without DXR no longer receives the former unshadowed virtual moon contribution; authored lights remain available.
- Deferred lighting PSO creation now occurs immediately after Raytracing initialization, before any frame. Both its vertex and pixel stages select the same supported shader model. HDR format changes recreate this PSO as well. The existing raster fallback remains available when DXR is unavailable.
- The already-denoised primary sun uses its visibility texture directly, skipping an otherwise redundant inline shadow query whose result would have been overwritten. Its shadow eligibility uses the DXR cast-shadow flag even when no raster shadow-map layer was allocated. This preserves the NRD sun-shadow path after selecting the DXR deferred shader. The new moon query executes only when moon radiance is nonzero. There is no new texture or pass, but moonlit PBR pixels incur a visibility query when DXR is available.

## Repeatable fixture

Run `scripts/validate-gi.ps1 -Configuration Release -Celestial` (or Debug). This routes GI validation mode5 through a closed six-sided PBR room. It moves the Sun above/below the horizon, opens the roof by moving it sideways, then closes it again. A sixth case disables only the direct Sun while keeping its daytime sky direction, providing an environment-only control. The ordinary deferred path is used with GI intensity0, no reflections, no clouds, no AO or temporal AA, isolating celestial direct illumination from emission, denoising and TSR. Six24-frame cases measure324 floor samples each from the scene target. The CSV field named `hdr_luminance` measures the configured scene target; it is not absolute physical radiance and the target may be UNORM. The currently saved scene and materials are not edited by this fixture; the wrapper restores editor/project settings.

Before production changes, the corrected fixture fails only the two moon-occlusion assertions:

| Case | Scene-target luminance before |
| --- | --- |
| Closed, Sun above | .0456258 |
| Closed, Sun below / moon above | .166118 |
| Open, moon above | .166118 |
| Open, Sun above | .949020 |
| Closed again, moon above | .166118 |

The nonzero baseline is constant environment illumination. The test compares the moon-specific increase with that baseline rather than requiring the room's entire lighting result to be black. Opening the roof provides positive controls for both Sun and moon, preventing a false pass from disabling all celestial light.

The first corrected Release run passes all five cases: closed daytime0, closed moonlit0, open moonlit.165876, open daytime.949020, and reclosed moonlit0. Its screenshots were captured, and the closed-night image was inspected. Selecting the appropriate deferred shader also removes the old unoccluded environment-specular fallback in this fixture when raytraced visibility is active. Outdoor moonlight and sunlight remain present; the fix does not disable the Moon globally.

After preserving the NRD primary-sun path, both Debug and Release repeat the same first five values (0 /0 /.165876 /.949020 /0), passing all assertions. Debug reports D3D12 errors=0. Both configurations' normal/features and fallback rendering also pass84frames each. The source was compiled for both configurations; HDR format changes also recreate the deferred PSO with the appropriate shader variant.

The explicit DXR-disabled test exposed an invalid assumption in the original Sun assertion: fallback environment illumination remains .315436 in the closed daytime room, versus .949020 with the roof open. The environment-only sixth case is also exactly .315436, confirming no extra direct Sun reaches the closed room. The test now checks this difference, rather than mistaking environment light for direct-light leakage. Nighttime closed/open/reclosed are all .00784663, with no virtual-moon increase. The DXR-enabled sixth case is0. Final session74447 completed with exit0: Debug and Release six-case fixtures, explicit DXR-disabled six-case fixture, and the original dynamic GI218-frame regression all pass. Final Debug celestial validation reports D3D12 errors=0; Release has no debug queue. The already-passing normal/fallback rendering checks are unchanged by the final fixture-only control addition. Both application builds are current and no validation job remains active.

Baseline evidence: `build/celestial-baseline.log`, `build/celestial-baseline.csv`. Fixed evidence: `build/celestial-fixed-{Debug,Release}.log`, `build/celestial-fixed-{Debug,Release}.csv`, `build/celestial-fallback-Release.{log,csv}`, `build/celestial-rendering-{Debug,Release}.log`, `build/celestial-gi-Release.log`. This fixture does not establish alpha-tested/transmitted lunar shadows or a full-frame performance improvement.

## Remaining sky reflection leak (2026-10-18)

The user's follow-up reported that the lobe remained with the Sun disabled and followed atmosphere time. The earlier six cases deliberately disabled reflections and GI, so their success did not establish secondary sky occlusion. Extending the fixture reproduced two independent unoccluded environment lookups in `RayReflectionsCS.hlsl`: the roughness-cutoff shortcut sampled the sky without any ray, and reflected surface hits added diffuse/specular sky without visibility. Sky simulation can remain active with the authored Sun light disabled, so disabling that light did not remove either lookup.

Cases6–11 enable reflections and GI intensity1, disable the authored Sun, and sample the separate denoised reflection/GI resources as well as the composed scene. Cases6/7 close/open the roof with cutoff1 (full hit shading); cases8/9 close/open it with cutoff.5 (rough approximation); cases10/11 close the roof at atmosphere hours8/16. Each case retains24frames and324receiver samples. The command now runs288frames in total.

Before the reflection fix, the closed-room reflection averages were .0132011 (full hit shading), .1273531 (rough approximation), .1108102 (08:00) and .1109229 (16:00). GI was zero in all four closed cases. All four new reflection-occlusion assertions failed, while the original direct-light assertions and open-roof controls passed. Debug reported no D3D12 errors. Evidence: `build/celestial-secondary-baseline.{csv,log}`.

The reflection fix requires a scene visibility query before either shortcut uses sky. It checks all reflection geometry, including objects whose authored cast-shadow flag is disabled. Diffuse secondary ambient excludes the Sun/Moon discs, matching diffuse GI. The rough shortcut still skips hit material/light evaluation; it no longer skips visibility. Blocked rays skip the atmosphere evaluation. This adds one visibility ray per freshly evaluated rough sample and up to two per reflected surface hit; it is a correctness change, with no claim of improved full-frame performance. It preserves the existing opaque-ray and maximum ray-distance limitations.

Both rebuilt Debug and Release pass all12cases. All four closed-room reflection/GI averages are exactly0; opening the roof yields reflection.0522776 (full shading) /.0356934 (rough approximation) and GI.274034 /.273206. The original Sun/Moon controls retain their previous results. Debug reports D3D12 errors0. Evidence: `build/celestial-secondary-fixed-{Debug,Release}.{csv,log}`. Closed-room before/after scene captures were inspected; this is a synthetic regression, not a replay of the user's exact attached scene/camera. The wrapper now copies celestial captures and the summary from the correct directory.

Release also passes the existing218-frame dynamic GI regression: emissive color edits/removal, moving blockers, SSGI switching, GI/reflection coexistence, emission textures and distance handling. Its log is `build/gi-tests/validation-Release-nrd.log`. This change does not alter TSR or NRD parameters.
