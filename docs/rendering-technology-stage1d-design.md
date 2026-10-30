# Stage 1D — DBuffer surface decals: design

Implements expansion feature G (Phase 1 G in
`rendering-expansion-progress.md`). Surface decals are a separate
feature from the existing `LightRenderMode::Decal` (a light-side
mode); nothing about light decals changes.

## Design (per the expansion design doc)

- A decal is a **box projector** (oriented box: world matrix + size)
  with a **decal material**: albedo/normal/roughness textures from the
  bindless heap, blend opacity, angle fade, priority and layer mask.
- Before deferred lighting, decals rasterize into a **DBuffer**:
  albedo (RGBA8), normal (RGBA16F or packed), roughness/mask
  (RGBA8). The raster pass reconstructs the receiver's world position
  from the scene depth, transforms it into decal-local space, clips
  outside the unit box, samples the material with decal UVs, and
  applies an angle fade from the angle between the receiver normal
  and the projector's projection axis.
- A **composite pass** merges the DBuffer into the GBuffer (albedo
  blend, normal blend, roughness blend, driven by the DBuffer mask)
  before lighting runs, so VG and legacy receivers behave identically
  (both read the same GBuffer afterwards).
- Ordering: decals sort by priority (then submission order); the
  DBuffer raster blends in that order.
- Scope limits for v1: opaque receivers only (transparent materials
  are excluded, matching the design's "define the transparent scope"
  note by exclusion), no GPU tile culling (noted as a future option
  in the design).

## Gating

`RendererSettings::UseDBufferDecals` (default OFF). With the flag
off, no DBuffer resources are created and no passes are added; the
frame is identical to before.

## Implementation steps

1. Flag + this design note.
2. Decal registry: CPU list of projectors (`SurfaceDecal` records),
   packed into a structured buffer each frame (like the light
   instance builder); a scene component feeds the registry.
3. DBuffer targets in the render-target set (flag-gated), routed
   through D3D12MA like the other persistent targets.
4. Decal raster pass (box volume VS/PS, depth-reconstruction PS) +
   DXC build targets.
5. GBuffer composite pass + graph wiring before deferred lighting.
6. Editor inspector for the decal component (later; the component
   data round-trips through scene serialization first).

## Acceptance (from the ledger)

Normal/roughness/shadow agreement with the receiver surface,
ordering/layers/angle fade behavior, projection distortion on walls
and floors, VG and legacy receivers, TSR stability, GPU timings —
all measured on the owner's machine with the flag on vs off.
