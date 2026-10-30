# Native Hi-Z SSR — temporal resolve: design

Status: DESIGN. The SSR v1 landed in the 500-push run traces at
full resolution every frame and blends by per-pixel confidence.
Two costs remain: full-res Hi-Z marching, and single-frame noise
on rough surfaces (the previous-color mip sample is one tap).

## Goals

- Halve trace cost with a half-resolution trace + bilateral upsample.
- Stabilize rough reflections temporally without ghosting: the
  reflection hit point is a 3D anchor, so history can be reprojected
  through the hit position rather than the reflecting pixel.

## Design

### Half-resolution trace (step T1)

- Trace at half resolution (checkerboard option deferred): the
  result target becomes half res; a small resolve compute pass
  upsamples bilaterally using full-res depth/normal (weights:
  depth similarity + normal similarity), writing the full-res
  result the lighting pass already consumes. Confidence from the
  trace is upsampled with the same weights; disoccluded half-res
  holes fall back to the nearest valid tap's confidence scaled by
  0.5 so the env blend stays honest.
- The selection/binding contract does not change: the module still
  registers one full-res result texture with TextureManager.

### Hit-point temporal accumulation (step T2)

- The trace additionally writes the hit world position (RGB32F)
  and the hit UV. The resolve pass reprojects the PREVIOUS frame's
  accumulated reflection through the current hit position:
  project hitPos with the previous view-projection (already
  available to the frame), sample the previous accumulation, and
  blend with a per-pixel factor driven by (a) hit-distance delta
  between frames, (b) roughness (rougher = more accumulation),
  (c) confidence of both frames.
- Accumulation buffers: two full-res RGBA16F (radiance+confidence)
  ping-pong, owned by the SSR module like the result target.
- Reset conditions: camera cuts (view-matrix jump, reuse the
  engine's existing history-validity signal from the TAA context),
  resolution change, flag toggles. On reset, accumulation = current
  frame (factor 0).

### What does not change

- The tri-state RaytracingFlags.w contract (0 env / 1 RT / 2 SSR)
  and the confidence-blend in SurfaceLighting.hlsli.
- The one-frame lag before SSR becomes selectable.

## Acceptance

- GPU ms of trace+resolve vs full-res trace at 1440p on the test
  scene (target: >= 35% cheaper at equal settings).
- Temporal stability: variance of a static rough-metal region over
  60 frames (target: visibly lower, no trailing on a moving glossy
  object in the capture tooling).
- Fallback: with history invalid, output equals v1.
