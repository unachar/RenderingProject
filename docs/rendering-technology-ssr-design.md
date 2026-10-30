# Native Hi-Z screen-space reflections (SSR): design

Follow-up to the FidelityFX SSSR evaluation
(`rendering-technology-sssr-evaluation.md`): if a screen-space
specular fallback is needed beyond probes + IBL, build it natively
on the engine's own Hi-Z and history instead of vendoring SSSR.

## Integration contract

`SurfaceLighting.hlsli` already has exactly one reflection override
point: when raytracing reflections are active, `envSpecular` is
replaced by the `RaytracedReflections` texture (the denoised
RayReflections result, bound per frame). SSR slots into the same
contract:

- A `ScreenSpaceReflections` module produces a result texture with
  the same meaning (linear HDR reflected radiance, screen space,
  roughness-aware) and the lighting pass binds it through the same
  selection: DXR result > SSR result > probe/environment IBL.
- Selection lives in one place on the CPU side (which texture and
  flag values the frame binds), keeping the shader change to the
  existing branch.

## Pipeline

1. **Trace**: one compute pass at half resolution (checkerboard
   option, mirroring RayReflections' SamplingLayout idea). Per
   pixel: reconstruct world position from depth, reflect the view
   vector about the GBuffer normal, jitter the ray by roughness
   (GGX-cone spread approximated by mip selection during resolve),
   and march the ray against the existing Hi-Z chain (reverse-Z,
   per-mip descriptors already maintained for occlusion) with the
   standard cell-crossing traversal, refining with a short linear
   search + one binary refinement step on hit.
2. **Resolve**: sample the previous frame's lit scene color at the
   hit UV (the standard SSR approximation: reflect the *previous*
   frame's final color), validate the hit (behind-camera, off
   screen, depth discontinuity, facing ratio), fade by screen
   edge distance and by the angle between the reflected ray and
   the view, and write radiance + confidence.
3. **Temporal filter**: reproject with the existing velocity and
   history infrastructure (same as TSR/SSGI resolve): accumulate
   with confidence-weighted blending, reject on disocclusion. No
   external denoiser dependency.
4. **Composite**: no separate composite — the result texture is
   consumed by the lighting override point above, multiplied by
   the same specular weight the DXR result receives.

## Settings and fallback

`RendererSettings::UseScreenSpaceReflections` (default OFF).
Effective only when DXR reflections are unavailable or disabled;
per-pixel fallback to probes/IBL wherever confidence is zero.

## Acceptance

Fixed-scene A/B against the DXR reflection result (RMSE/SSIM via
the capture tooling), glossy floor / corridor fixtures for edge
fade and grazing behavior, GPU ms at half/full resolution, and no
change when the flag is off or DXR reflections are on.
