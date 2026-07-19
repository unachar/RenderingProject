# RenderGraph

The frame renderer is scheduled by `RenderGraph`. The graph makes pass
dependencies explicit and provides D3D12 resource-state tracking for resources
that opt into automatic barriers.

## Frame graph

```text
Shadow
  |
GBuffer / Opaque
  |
Velocity
  |
Deferred Lighting / PostProcess
  |
Transparent / Overlay
  |
AntiAliasing
```

Dependencies are inferred from declared reads and writes instead of hard-coded
pass indices. Independent passes retain insertion order, so the compiled
schedule is deterministic.

## Resource modes

`CreateLogicalResource` represents a dependency without a backing
`ID3D12Resource`. Existing compound `RendererDraw` passes use this mode while
their internal transitions remain locally owned.

`ImportResource` accepts an initial state, optional final state, and subresource.
When `AutomaticBarriers` is enabled, the graph:

- batches transitions before each consuming pass;
- tracks state through the compiled schedule;
- emits UAV barriers for consecutive UAV write hazards;
- restores the requested final state after the last pass.

A callback for an automatically tracked resource must not independently
transition that resource. Set `AutomaticBarriers` to `false` while migrating
legacy pass code that still owns its barriers.

## Validation

Compilation rejects invalid handles, a resource used in multiple states by one
pass, missing execute callbacks, invalid dependencies, and dependency cycles.
Execution is rejected until a successful compile. The execution order is exposed
for diagnostics and tests.

New render targets and buffers should use automatic tracking. Existing compound
renderer functions can be split into smaller graph passes and migrated from
logical resources one resource at a time.

The shadow depth array is the first migrated renderer resource: the graph moves
it from shader-read to depth-write for the shadow pass, then back to shader-read
before GBuffer lighting consumes it. `RendererDraw` no longer emits those two
manual barriers.
