# Stage 3 design — color management (OpenColorIO question)

The plan asks for input/output color-space management designed
*separately* from the tonemapping approximation, with no double
application. This note records the engine's current (implicit)
color pipeline and decides the OpenColorIO question.

## The pipeline as it stands (surveyed 2026-10-31)

- **Working space: linear Rec.709 everywhere.** All lighting,
  GI caches, probes and the reference integrator compute in
  linear Rec.709; ToneMapping.hlsli states the contract in its
  header (functions take and return linear Rec.709).
- **Inputs:** color textures decode sRGB -> linear on sample
  (SRV format), data textures stay linear; IES/LTC/HDR assets are
  linear by construction. There is no per-asset color-space tag
  beyond the sRGB/data distinction.
- **View transform:** `ApplyToneMapper` in common.hlsl — ACES
  fitted or AgX (with the punchy variant), preceded by the
  grading stage (lift/gamma/gain, color temperature/tint via
  GradeParams), applied once at the end of the post chain under
  HdrFlags.y. This is the single place display-referred values
  are produced; nothing upstream tonemaps.
- **Output:** display-encoded Rec.709 to the swap chain. No HDR
  output path (no scRGB/PQ color-space negotiation exists in the
  tree today).

So the engine already has the separation OCIO formalizes —
scene-referred working space, one view transform, one display
encoding — it is just implicit and Rec.709-only.

## What OCIO would actually buy

- Interchange with film/VFX pipelines (ACEScg working space,
  IDTs for camera EXRs, show LUTs). Nothing in this project
  consumes or produces such material today.
- A config-driven display transform for multiple displays
  (sRGB + PQ HDR). Real, but only once HDR output exists.

## What it would cost

- Moving the working space to ACEScg touches every implicit
  Rec.709 assumption (LUTs, probe prefilter, GI caches, the
  reference integrator's comparisons against the realtime path —
  which must share the space to stay a valid oracle).
- A runtime dependency + config assets for a benefit no current
  workflow exercises, plus a permanent double-application hazard
  at every boundary the plan explicitly warns about.

## Decision

**Do not adopt the OpenColorIO runtime.** Instead:

1. This document is the color-management spec of record: working
   space linear Rec.709; sRGB-decode for color inputs only; one
   view transform (ACES/AgX + grade) at the end of the post
   chain; display encode Rec.709/sRGB.
2. New features must state which side of the view transform they
   live on (the SSR result and probe textures are scene-referred;
   the reference PFM outputs are scene-referred linear).
3. Revisit triggers: (a) an HDR display output path (then design
   the display transform as a selectable PQ/scRGB variant first,
   OCIO only if configs must be shared), or (b) EXR/ACES
   interchange arriving with the Stage 2H EXR follow-up.
