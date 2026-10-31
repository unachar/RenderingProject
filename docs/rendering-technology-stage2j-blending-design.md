# Stage 2J follow-up — per-pixel two-probe blending: design

## Root-signature audit (2026-10-31, resolves the Route A gate)

Route A samples through `LoadMaterialTexture`
(MaterialTextureHeap.hlsli), which has two modes:

- **RP_BINDLESS=1** (runtime DXC path, `g_BindlessShaderSettings`
  in dxccompiler.h): uses `ResourceDescriptorHeap` (SM 6.6) with
  a heap-base constant — no root table required. Route A works
  as-is in this mode, in both deferred and forward shaders.
- **RP_BINDLESS=0** (precompiled CSOs, the default): declares
  `AssetTextureHeap[512] : register(t0, space1)`, so the root
  signature must bind a space-1 SRV table. The postprocess root
  signature (shaderpipelines.cpp) binds its 512-texture table at
  root 38 in **space 5**, not space 1 — so the deferred lighting
  path would sample an unbound table. The forward / GPU-driven
  opaque path already exercises the same include successfully.

**Decision:** `PROBE_BLENDING_BINDLESS` may be enabled only when
the engine's bindless shader mode is active. Enabling it for the
default precompiled deferred path additionally requires adding a
space-1 range to the postprocess root signature (and binding the
engine texture heap to it in DrawFullscreenPass). The frame-global
top-2 selection and remainder weighting stay live either way;
only the per-pixel Route A fetch is gated.

**Update (same day):** the deferred path now has the space-1
range (postprocess root 39, bound in DrawFullscreenPass to the
engine texture heap), and DeferredLightingPS defines
PROBE_BLENDING_BINDLESS itself — per-pixel two-probe blending is
live in the deferred path. The forward path audit also passes:
the model root signature has bound the space-1 asset texture
heap at root 18 all along (MaterialSurface.hlsli samples through
it in production), so modelshaderPS defines the flag too.
Per-pixel two-probe blending is live in both paths.

Pre-implementation state (for context): Stage 2J v1 selects ONE
probe per frame (the
camera-containing, highest-priority one) and overrides the
environment texture for the whole frame; box projection (landed in
the 500-push run) corrects parallax for that probe. Limits: a hard
pop when the camera crosses a probe boundary, and no influence
from a second nearby probe.

## Design

- **Top-2 selection on the CPU**: the existing SelectProbe logic
  generalizes to the two best probes whose boxes contain the
  CAMERA (per-pixel containment is evaluated in the shader from
  the world position; the CPU only picks the frame's candidates —
  scenes with probes scattered far apart are served by the camera
  pre-selection plus the shader-side containment test below).
- **Shader-side blending**: PostProcessParams gains a second probe
  slot (center/extents/intensity + SRV index packed). The lighting
  shader cannot switch textures per pixel — the environment SRV is
  a single bound texture — so per-pixel blending requires the two
  probe environments to be reachable another way:
  - Route A (chosen): bind the two probe environments through the
    existing bindless texture heap (AssetTextureHeap indices are
    already known to the CPU) and sample them in
    EvaluateSurfaceEnvironment with the same equirect mapping,
    per-pixel box-projected per probe, blended by box-distance
    weights (weight = smooth falloff of the position's normalized
    distance to each box face; standard probe weight). The global
    EnvironmentTexture path remains for pixels outside both boxes.
  - Route B (rejected): render a blended equirect per frame —
    an extra full-screen pass per frame and still frame-global.
- **Weights**: for probe i, d_i = min over axes of
  (extent_i - |p - center_i|) / extent_i (1 at center, 0 at the
  faces); w_i = clamp(d_i, 0, 1)^2; normalize w_0 + w_1, remainder
  goes to the global environment. Intensity multiplies each probe's
  weight before normalization.
- **Migration**: the frame-global SRV override stays as the
  fallback when the bindless route is unavailable (e.g. the
  forward path without the heap bound); the two routes must not
  both apply (the override index is only set when Route A is off).

## Steps

1. CPU top-2 selection + PostProcessParams probe slot 2 + indices.
2. HLSL: bindless probe sampling + weights + box projection per
   probe (reuse the landed projection math).
3. Route gating + A/B capture comparison at a probe boundary.

## Acceptance

- No visible pop crossing a probe boundary in the capture tooling
  (frame sequence across the boundary).
- Interior of the dominant probe matches v1 within noise.
- Cost: two extra equirect taps only for pixels inside any probe
  box (early-out by the active flags).
