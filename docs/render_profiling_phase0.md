# Phase 0: Rendering measurement foundation

This phase establishes a repeatable baseline before further GPU-driven rendering work.
Optimizations in later phases should be accepted only when these measurements show a
benefit in the target scene.

## Implemented measurements

`RenderProfiler` collects the following data without a GPU stall in the current frame:

- CPU render-command recording time using `std::chrono::steady_clock`
- Whole scene GPU time using D3D12 timestamp queries
- Per-pass CPU and GPU time
  - Shadow
  - GBuffer / Opaque
  - Velocity
  - Deferred Lighting / PostProcess
  - Transparent / Overlay
  - AntiAliasing
- D3D12 submission counts
  - Draw and indexed draw calls
  - Dispatch calls and dispatched thread groups
  - ExecuteIndirect calls and maximum command capacity
  - Pipeline-state binds
  - Graphics and compute descriptor-table binds
  - ResourceBarrier calls and barrier count
- Pipeline statistics
  - Input-assembler vertices and primitives
  - Vertex, pixel, geometry and compute shader invocations
  - Clipping input and output primitives
- GPU-driven culling candidate capacity

The profiler window is shown by default and can be toggled with **F9**.

## Query-ring and synchronization model

Timestamp and pipeline-statistics results are stored per swap-chain frame slot. Results
are read only when that slot is reused. `RendererDraw::EndDraw` already waits for the
fence associated with the next back-buffer slot, so reading that slot on the next
`BeginFrame` does not introduce an additional CPU/GPU synchronization point.

GPU timestamps are converted to milliseconds with the direct command queue timestamp
frequency. Readback resources remain in `D3D12_RESOURCE_STATE_COPY_DEST`, as required by
`ResolveQueryData`.

## Meaning of the displayed frame times

- **CPU Render** measures render command preparation and recording between
  `RenderProfiler::BeginFrame` and `RenderProfiler::EndFrame`.
- **GPU Render** measures the matching scene-command region.
- Swap-chain `Present`, fence waiting, VSync waiting, and the optional fixed-frame-rate
  sleep are intentionally excluded.
- `World::GetFrameTimeMs()` remains the wall-clock frame measurement that includes the
  application loop behavior.

This separation is intentional: renderer optimization should be judged using render
work rather than a frame limiter or presentation wait.

## Indirect-command visibility note

Phase 0 records the maximum indirect command count submitted to each `ExecuteIndirect`
call. The exact value produced in the GPU count buffer is not copied back in the
baseline path because doing so would add resource transitions and copies to every
indirect submission and would perturb the timing being measured.

Actual rendered primitive and shader-invocation counts are available through the
pipeline-statistics query. A batched, optional GPU-culling statistics readback is part
of Phase 1, where the per-entity dispatches and count buffers are consolidated.

## Recommended benchmark procedure

1. Build `Release | x64` and enable the D3D12 debug layer only for correctness testing,
   not for final performance numbers.
2. Disable VSync and the fixed frame-rate limiter.
3. Use a fixed scene, camera transform, resolution, render mode, AA mode and light set.
4. Let shader compilation, resource uploads and caches settle before recording data.
5. Capture at least 300 frames.
6. Compare median and 95th-percentile CPU/GPU times rather than one instantaneous FPS.
7. Record the API counts and pipeline statistics together with every timing result.

## Phase 0 completion criteria

Phase 0 is complete when:

- CPU render time and GPU render time are visible without a current-frame readback stall.
- Every major render pass has an independent CPU/GPU timing.
- D3D12 draw, dispatch, indirect, state-bind and barrier activity can be compared before
  and after a change.
- Actual pipeline work can be inspected through D3D12 pipeline statistics.
- The same benchmark procedure can be repeated for later phases.

## Next phase

Phase 1 consolidates the current model/entity-level GPU-culling submissions into shared
GPU Scene buffers and fewer compute dispatches. It will also add optional batched
readback of culling candidates and visible counts for validation.
