# GPU / GPU-driven render optimization

This document records the optimization work on `dev_main_renderoptimize`, the
reasoning behind it, and the measurements required before expanding the path.

## Scope implemented in this branch

### 1. ExecuteIndirect mesh command streams

`GpuDrivenIndirectDrawCache` creates reusable indirect argument buffers for each
loaded model. The shadow pass uses this layout:

```text
VBV(slot 0) -> IBV -> DRAW_INDEXED
```

The velocity pass uses:

```text
VBV(slot 0/current) -> VBV(slot 1/previous) -> IBV -> DRAW_INDEXED
```

The CPU still binds per-entity constants once because the current model root
signature exposes them through a descriptor table. All mesh input-assembler and
draw commands inside that entity are then consumed by the GPU with one
`ExecuteIndirect` call. If command-signature or argument-buffer creation fails,
the renderer automatically uses the original direct-draw loop.

This is the first GPU-driven stage: command consumption is indirect and the
argument format is ready for a compute shader to generate or compact later.

### 2. Batched animated-vertex transitions

GPU-skinned vertex buffers must move from UAV state to vertex-buffer state for
rasterization and back to UAV state for the next skinning update. The original
path called `ResourceBarrier` separately for every mesh. The optimized path
builds one barrier array per model and submits it in one API call before and
after drawing.

The number of actual resource transitions is unchanged, but CPU command-list
recording overhead and driver entry overhead are reduced.

### 3. Velocity-pass frustum culling

The velocity pass now transforms each model's local AABB to an oriented world
box and tests it against a world-space camera frustum. Off-screen models do not
allocate transient velocity constants, transition skinning buffers, or submit
velocity draws.

Camera-frustum culling is deliberately not applied to shadow casters: an object
outside the camera view can still cast a visible shadow into the view.

### 4. ECS query-generation cache

`World::GetView<T...>()` results are cached by component mask and Registry
structure generation. Repeated render passes reuse a packed list of matching
entities. The list is rebuilt only when an entity is created, restored,
destroyed, or gains/loses a component.

This avoids repeatedly checking the same alive/component masks during shadow,
scene, velocity, overlay, animation, transform, and editor passes.

## Why the primary/transparent path is still a fallback

The current primary model shader and root signature combine:

- per-entity descriptor-table constant buffers;
- per-material texture and normal-map descriptors;
- opaque and transparent shading;
- automatic/manual material classification;
- toon extrusion and TEO outline modes;
- GPU-skinned and static geometry.

Replacing all of that in one change would make visual regressions difficult to
isolate. The existing `ModelSystem` therefore remains authoritative for primary
and overlay rendering while the indirect path is proven in shadow and velocity
passes.

## Recommended phase 2: compute-generated visibility and command compaction

1. Move per-instance world/material data to a `StructuredBuffer` indexed by an
   indirect root constant or `SV_InstanceID`.
2. Allocate default-heap command and count buffers with UAV and
   `INDIRECT_ARGUMENT` usage.
3. Dispatch a compute shader that performs frustum culling and appends visible
   commands.
4. Add a previous-frame hierarchical-Z depth pyramid for conservative occlusion
   culling.
5. Execute the compacted commands with a count buffer.
6. Separate opaque, alpha-tested, transparent, shadow, and outline command bins
   so each bin has a stable PSO/root-signature contract.

That stage removes the remaining CPU per-entity traversal from opaque rendering.
It should be introduced after this branch's baseline is measured and validated.

## Measurement checklist

Use a Release x64 build and a repeatable scene. Record at least 300 frames after
warm-up.

### CPU / PIX

- `SystemManager::RenderFlow` CPU duration.
- command-list recording duration for shadow and velocity passes.
- number of `DrawIndexedInstanced`, `ExecuteIndirect`, and `ResourceBarrier` API
  calls.
- time spent in `World::GetView` and entity mask filtering.

### GPU / PIX

- shadow-pass and velocity-pass GPU duration.
- compute-skinning duration.
- vertex/primitive counts before and after camera culling.
- bubbles around UAV-to-VB transitions.
- cache/memory pressure from velocity and G-buffer targets.

### Acceptance criteria

- no D3D12 debug-layer errors;
- identical shadow and velocity output for visible models;
- no missing first-frame velocity data;
- lower CPU render-thread time in mesh-heavy scenes;
- no statistically meaningful GPU regression in small scenes.

## References

- Microsoft, **Indirect Drawing**,
  <https://learn.microsoft.com/windows/win32/direct3d12/indirect-drawing>
- Microsoft, **ID3D12GraphicsCommandList::ExecuteIndirect**,
  <https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-executeindirect>
- Microsoft, **D3D12 ExecuteIndirect sample**, DirectX Graphics Samples,
  <https://github.com/microsoft/DirectX-Graphics-Samples/tree/master/Samples/Desktop/D3D12ExecuteIndirect>
- Tomas Akenine-Möller, Eric Haines, Naty Hoffman et al.,
  **Real-Time Rendering, Fourth Edition**, CRC Press, 2018.
- Ned Greene, Michael Kass, Gavin Miller,
  **Hierarchical Z-Buffer Visibility**, SIGGRAPH 1993.
- Ola Olsson, Markus Billeter, Ulf Assarsson,
  **Clustered Deferred and Forward Shading**, High Performance Graphics 2012.
- Graham Wihlidal,
  **Optimizing the Graphics Pipeline with Compute**, GDC 2016.
