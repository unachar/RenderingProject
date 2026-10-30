# Atmosphere / Clouds / Height Fog — Draft Integration

Target: `dev_main` (reviewed at 238285341602412f8f3ee689c106cbb5676ff40e)

**Status: architecture draft only. This PR does NOT add a working renderer or claim GPU validation.** Keep this separate from production until the implementation/checklist below is complete.

## Existing systems — do not reimport
- `source/rendering/cloud/cloudrenderer.*`, `cloudsettings.h`, `CloudRaymarchPS.hlsl`, `CloudCompositePS.hlsl`: four layers, procedural base/detail noise, depth-aware composition, sun/moon lighting, cloud shadows.
- `shader/hlsl/AtmosphereLutCS.hlsl`, `AtmosphereLutCommon.hlsli`, `SkyAtmosphere.hlsli`, `source/scene/atmosphere.*`: atmosphere LUT, physical sky and scattering configuration.
- `source/rendering/localheightfog.h`: configurable local height/sphere fog volumes (16 max).
- `docs/procedural-cloud.md` and `scripts/validate-clouds.ps1` define the current cloud regression/performance baseline.

## Reference-only sources (do not vendor whole engines)
| Source | Adopt only | Existing overlapping function |
| --- | --- | --- |
| https://github.com/rubenaryo/Cumulus | cloud light caching, SDF empty-space skipping, dual-lobe phase, art-directed cloud shapes | volumetric cloud render, Bruneton atmosphere |
| https://github.com/ebruneton/precomputed_atmospheric_scattering | unit-tested spectral/ozone density references and LUT validation approach | atmosphere scattering and LUT generation |
| https://github.com/konai123/AtmosphereRender | DirectX12 reference for cloud/atmosphere compositing and LUT lifetime | cloud/sky rendering |
| https://github.com/supercoderrrrr/VolumetricClouds-URP | weather-map parametrization, temporal history clamp, camera cut handling | ray march, noise baking |
| https://github.com/Depersonalizc/volumetric-clouds | adaptive marching and backtracking reference | Perlin/Worley procedural clouds |

Check each source's current license, shader asset ownership, and attribution before any code copying. Prefer implementing independently from documented algorithms.

## Implementation order

### A. Cloud integration improvements
1. Add optional `CloudWeatherMap` (coverage, type, precipitation) per layer. Default: no weather texture, use existing scalar Coverage exactly.
2. Add optional `CloudLightCache` compute pass with 3D lighting volume, invalidated by light direction/weather/seed changes; retain existing per-pixel light-march fallback.
3. Add optional empty-space skipping using a conservative density/occupancy mip or SDF; never skip nonempty small/thin layers. Verify 4-layer overlap.
4. Add `CloudTemporalResolve` with camera motion reprojection and neighborhood clamp using existing velocity/depth. Reset on camera teleport, FOV, resize, quality, layer count, sun/weather changes and disable on debug output.
5. Use separate feature switches, initially OFF, to preserve reference rendering.

### B. Atmosphere physical accuracy
1. Keep current LUT owner. Compare existing transmittance/multiple-scattering outputs against Bruneton reference; don't replace LUT with an independent implementation.
2. Introduce optional ozone absorption and physically meaningful Rayleigh/Mie scale heights with controlled UI migration and validation against reference sunrise/sunset.
3. Keep sun/moon lighting and cloud illumination on the same world-unit and pre-exposure convention.

### C. Height fog enhancements
1. Preserve `LocalHeightFog` data, evolve its shading into an analytic extinction baseline for constant/exponential height density.
2. Add optional froxel 3D scattering (sun, local lights, volumetric shadows) with camera frustum aligned grid and temporal reprojection. This is a new feature, not a second independent global fog system.
3. Compose lighting and sky in **linear HDR** before exposure/tone mapping; use consistent transmittance to avoid double-fogging clouds.
4. Introduce dedicated render passes with explicit depth and UAV/SRV state transitions; expose view diagnostics (density, scattering, transmittance, history validity).

## Composition invariant
For each participating layer, composite front to back using premultiplied scattering:
`L_out = L_scatter + T * L_background`.
Depth ordering and shared transmittance must be checked for cloud in front of/behind world fog volumes. Avoid applying full-screen height fog twice after the cloud composite.

## Performance and validation (RTX 3050 Laptop 4GB)
- Baseline (recorded before this draft, 1080p clouds half resolution): Low 2.028ms, Medium 3.607ms, High 6.555ms median; check new code against these *scene-dependent* values.
- Benchmark OFF vs each independent feature ON, then combined. Log median, p95, VRAM, ray steps, quality and resolution.
- Smoke: zero-cloud, cloud inside/outside, thin/high/overlapping layers, sun horizon, moon, weather extremes, fog inside/outside spheres, camera cut, resizing, HDR/post-processing.
- Run `scripts/validate-clouds.ps1` in Debug, Release and GPU validation where possible; inspect unrelated baseline D3D12 warnings separately.
- Completion criterion: code merged into RenderGraph/editor, successful DXC compilation, visual comparison, tested GPU resource barriers and no unexplained regressions.

## First implementation unit
Use a separately reviewed PR for **analytic height-fog extinction and aerial-perspective compositing** before froxel ray march; this limits VRAM exposure and reduces overlap with existing volumetric-cloud architecture.
