# FidelityFX SSSR evaluation (Stage 3)

Question: adopt FidelityFX SSSR as the specular reflection fallback
when DXR reflections are off?

## What the engine already has

- DXR reflections (`RayReflectionsCS`) when raytracing is enabled.
- Image-based lighting from the global environment texture, plus
  local reflection probes (Stage 2 J) for local specular.
- SSGI (deinterleaved screen-space GI) for indirect diffuse.
- A full Hi-Z chain over the scene depth (with the FidelityFX SPD
  fast path behind a flag), GBuffer normals, and temporal history
  infrastructure (TSR) — i.e. every input a screen-space specular
  tracer needs. There is no screen-space *specular* pass today.

## What SSSR would require

- The FidelityFX SDK 2.x Kits tree does not ship SSSR as vendable
  shader sources (same distribution shape as CACAO: effect IDs and
  host headers only). The full sources live in the per-effect tree
  and are built around the FidelityFX frame graph: classify tiles,
  ray march against a Hi-Z, resolve, and a dedicated denoiser pass
  chain (SSSR is designed to pair with FidelityFX Denoiser). Pulling
  it in means either the whole SDK backend or a substantial port.
- Its interfaces assume the SDK's resource/heap model; this engine
  has its own RenderGraph, descriptor layout and history system, so
  the integration surface is large relative to the algorithm itself.

## Verdict

Do not vendor SSSR. If a screen-space specular fallback is wanted
beyond probes + IBL, implement it natively against the existing
Hi-Z chain and the reflection resolve contract the DXR path already
uses, reusing engine temporal reprojection for denoising instead of
FidelityFX Denoiser. That keeps one reflection integration point
(probes/IBL < SSR < DXR by roughness and settings) and avoids a
second framework dependency. Revisit only if the SDK ships SSSR in
a shader-only form like SPD.
