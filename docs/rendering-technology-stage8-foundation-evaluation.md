# Evaluation: Stage 8 foundation candidates (OMM / RTXTS / DirectStorage / RTXCR)

Date: 2026-10-31. Scope: the plan's Stage 8 line - "基盤技術は
必要性が profile で確認できたものだけ: DirectStorage / RTXTS、
OMM (alpha-tested DXR)、RTXCR、XeSS". XeSS has its own evaluation.
The standing rule for this group: adopt only when a profile shows
the need. None of the four profiles hot today, so all four are
recorded as evaluated-and-deferred with their trigger.

## OMM (Opacity Micro-Maps, NVIDIA-RTX/Omm)

Bakes per-micro-triangle opacity states so DXR any-hit for
alpha-tested geometry (foliage) skips most shader invocations.
Engine fit: the DXR paths and the VG alpha-tested shadows both
pay alpha-test costs, but the VG pipeline is cluster-based with
its own material resolution - OMM applies to the BLAS/DXR side,
not to VG. Trigger: a profile showing any-hit alpha cost as a
top DXR expense in a foliage-heavy scene. The bake step would
live next to the existing asset pipeline's material processing.

## RTXTS (RTX Texture Streaming, NVIDIA-RTX/RTXTS)

Tiled texture streaming with feedback. Engine fit: poor today -
TextureManager already does residency-based streaming with the
VG residency system, and the D3D12MA sweep deliberately excluded
reserved/tiled resources because the streaming layer owns them.
Adopting RTXTS means replacing that layer, not augmenting it.
Trigger: streaming stalls or residency thrash in a profile at
target scene scale, after the incumbent's tuning knobs
(GiRadianceCache-style settings exist for streaming too) are
exhausted.

## DirectStorage (Microsoft)

GPU decompression / batched IO for asset loading. Engine fit:
load times are not a measured problem in the current workflow,
and the VG/asset pipeline reads through its own loaders.
Trigger: a load-time profile (cold start or scene switch) where
IO + decompression dominates; then evaluate DirectStorage with
GDeflate against the incumbent path on the largest scene.

## RTXCR (RTX Character Rendering)

Strand-based hair + skin path tracing from the RTX Kit.
Engine fit: Stage 6 territory (new materials: hair tangents,
skin SSS) and a character-content prerequisite the project does
not currently have; the MegaLights/ReSTIR stack already covers
the lighting side. Trigger: actual character assets entering
the project plus a Stage 6 material decision; evaluate then
against building on the existing PBR/SSS work.

## Note on STBN (cross-check during this evaluation)

The RTX Kit lists a SpatioTemporal Blue Noise utility, but the
vendored External/RTXNS tree is the Neural Shaders SDK only -
no STBN textures are in the tree, so the earlier STBN deferral
(no asset to adopt, unclear redistribution terms for the
reference data set) stands unchanged.
