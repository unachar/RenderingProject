# dev_muse 50-push batch summary (8ead54ea → batch end)

The second long batch on `dev_muse`, run under the "notify after
50 pushes" cadence. **None of it is build/GPU verified yet** — every
feature is flag-gated default OFF, with CPU tests or DXC compiles
run on Linux where possible. Verification on the Windows/RTX
machine is requested as one batch, like the previous two.

## What landed

1. **D3D12MA sweep completed** (14 pushes): every remaining
   persistent DEFAULT-heap allocation site routes through the
   allocator wrapper with committed fallback — instancing,
   screen-space effects, DXR buffers/guides, clouds, transmittance
   shadows, thumbnails, VG streaming pages, Lumen surface cache,
   procedural texture/noise, light instance builder, animated model
   index buffers, FSR scratch, stationary VSM depth cache, main
   depth-stencil. Excluded by design: UPLOAD/READBACK heaps, the
   transient aliasing arena, reserved/tiled streaming, OptiX
   interop (shared handles), swap chain.
2. **Stage 1D DBuffer decals** (9 pushes): design + flag, registry
   with GPU packing (CPU-tested), DBuffer targets, raster/composite
   shaders (DXC-verified), frame hook in both deferred paths,
   DecalComponent + per-frame sync, scene save/load + undo,
   inspector + create menu. Flag: `UseDBufferDecals`.
3. **Stage 2 J local reflection probes** (7 pushes): design + flag,
   component/registry/selection (CPU-tested), active-probe
   environment override, GGX prefilter shader + runtime generation
   (via new `TextureManager::RegisterExternalTexture`), save/load,
   inspector + create menu. Flag: `UseLocalReflectionProbes`.
4. **Stage 2 H reference path tracer** (3 pushes): design +
   ReferenceCapture job bookkeeping (state machine, deterministic
   seeds — CPU-tested) + accumulation target. Integrator shader is
   the next step.
5. **Native Hi-Z SSR** (5 pushes): SSSR evaluation concluded
   "don't vendor" (SDK 2.x ships no SSSR shader sources; it would
   drag in FidelityFX Denoiser), native design recorded instead:
   flag, reflection-source selection point (CPU-tested), Hi-Z trace
   shader (DXC-verified), trace-pass host. Frame wiring is the next
   step. Flag: `UseScreenSpaceReflections`.
6. **Designs/evaluations** (4 pushes): Stage 1C multi-view shadow
   design, SSSR evaluation, STBN/CAS deferrals recorded in the
   adoption status doc.
7. **Settings UI** (2 pushes): checkboxes for all integration
   flags, so the batch can actually be switched on for testing.
8. **Fixes found by self-review** (3 pushes): GetComponent
   reference semantics in the probe update; probe update moved to
   after the frame command-list reset (prefilter dispatches would
   have been discarded); one split commit completed.

## Suggested verification order

1. Build (MSVC) — new files are registered in the vcxproj; tracked
   DXC outputs now 39 (`scripts/test-shader-build-targets.ps1`).
2. Default-off smoke run: nothing should change with all flags off.
3. Toggle on one at a time from the Advanced GPU settings section:
   D3D12MA (watch the "VRAM: D3D12MA..." log lines), SPD Hi-Z,
   DBuffer decals (create via hierarchy menu, assign textures),
   local probes (create, assign an equirect HDR, walk in/out of the
   influence box), SSR (trace host exists but frame wiring is not
   in yet — expect no visual change from this toggle alone).
