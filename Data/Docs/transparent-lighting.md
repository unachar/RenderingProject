# Transparent Lighting

Phase 1 keeps transparent geometry in Forward and shares the existing Deferred
PBR, environment, per-light attenuation, cascade/virtual shadow filtering,
contact shadow and screen-space composition through SurfaceLighting.hlsli.
Material textures and normal mapping continue to use MaterialSurface.hlsli
and the existing common TBN helper.

Transparent 3D geometry now renders into the HDR Scene target after Deferred
lighting and clouds, before bloom, post processing, tone mapping and upscaling.
Its depth test uses the scene's read-only depth buffer. Premultiplied RGB and
alpha use ONE / INV_SRC_ALPHA. Coverage below 1/255 is discarded.
The 2D overlay stays after post processing.

Material assets persist renderState.shadowMode (None, Opaque, AlphaTest,
Transmittance, Auto) and renderState.shadowCutoff. Missing modes default to Auto;
Auto selects RGB transmittance for transparent surfaces and Opaque otherwise. Explicit
Opaque overrides remain unchanged. Old Masked assets
without shadow settings migrate to AlphaTest with their alpha cutoff.
Both material editors expose these settings. Zero material coverage suppresses
shadow casting even in Opaque mode. Material-aware model shadows bind each mesh's
resolved material slot. Virtual shadow page keys include shadow material state.

Inline DXR shadow queries use a separate instance mask from AO/reflections.
AlphaTest casters select the shared raster shadow path because the existing
opaque ray query cannot evaluate texture alpha. Material-aware raster shadows
use ordinary draws rather than the depth-only instanced path.

Validation command: ./scripts/validate-materials.ps1 -Configuration Debug -Transparency

This extends the material regression suite with HDR Forward/Deferred comparison,
zero coverage versus no draw, shadow-map depth for all three modes and cutoffs,
and transparent shadow receivers. Captures and logs are under build/material-tests.
Repeat with Release for the optimized executable.

## Optical Shadows

Transmittance shadows use four depth-peeled entry layers per shadow light at
512 x 512. Directional, point and spot lights reuse their existing projections,
including cascaded and virtual directional shadow maps. Maps allocate on demand.
Each lighting sample multiplies only layers between the light and receiver, then
combines that RGB visibility with opaque occlusion before the existing PCF average.
Forward and Deferred share this implementation. Approximate box distance-field
shadows exclude purely transmissive casters. Such casters select raster shadows
instead of the current binary DXR query; ray AO and reflections remain available.

Opacity is surface coverage, not volume absorption. Each covered fraction uses an
air/dielectric/air parallel slab: exact unpolarized Fresnel reflection, Snell-adjusted
path length, Beer-Lambert RGB extinction, and incoherent internal reflections.
The transmitted fraction is (1 - opacity) + opacity * transmission * slabTransmission.
Metallic material cannot transmit. Absorption coefficients are nonnegative and
measured per world unit. Thickness uses world units. Base color provides the
transmission tint at the authored normal-incidence thickness. Roughness is not used
as an arbitrary extra extinction term. Defaults use coverage-only shadows; clear
glass uses Transmission = 1, IOR = 1.5, white Base Color and zero absorption.

Closed meshes should use Back culling so one entry represents both slab interfaces.
Flat engine sprites and meshes are detected as two-sided sheets for shadows;
other open sheets can use None to receive light on either face. Authored thickness, not
mesh exit distance, controls absorption. This is an energy-conserving thin-slab
approximation, not a full optical path tracer: no lateral refractive displacement,
caustics, volumetric multiple scattering or rough transmission spreading. More than
four entries along a ray are truncated. Coplanar entries within the depth-peeling
epsilon merge. Finite-resolution filtering retains the existing shadow-map limits.

References: https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF and
https://pbr-book.org/4ed/Volume_Scattering/Transmittance.

The extended GPU validation checks half coverage, analytic clear-glass Fresnel
transmission, RGB absorption, increasing thickness, two peeled entries without
exit double-counting, and zero-opacity bypass. Receiver samples check exclusion of
objects behind the receiver, RGB attenuation in Deferred, virtual-map rendering
and optical-parameter cache invalidation, in addition to the prior regressions.

Phase 3 refraction/OIT is not implemented.
The existing transparent draw lists sort back to front within each backend;
intersecting geometry and ordering across separate backends remain limitations.

## Build Context

The subsequent build-performance repair restored required standard-namespace
qualifications in standalone sources and headers, including JSON. Release Build,
Release Rebuild and fresh material unit tests now pass. See build-performance.md
for timings and preserved optimization settings. Current regression logs are under
build/build-time; earlier optical validation logs remain under build/material-tests.
