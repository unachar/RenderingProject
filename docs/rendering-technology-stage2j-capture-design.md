# Stage 2J follow-up — live cubemap capture for probes: design

Status: DESIGN. Probes today sample authored equirect environment
textures (optionally GGX-prefiltered by EnvironmentPrefilterCS).
A probe that reflects the ACTUAL scene needs a live capture:
render the scene into a cubemap at the probe position, convert to
a prefiltered equirect (reusing the landed prefilter), and serve
it through the same SRV plumbing.

## Design

- **Capture**: on probe authoring demand (a `CaptureLive` flag on
  ReflectionProbeComponent) and then on a refresh interval or
  scene-dirty signal, render the scene's opaque pass 6 times into
  a 128x128-per-face cubemap render target at the probe position,
  reusing the engine's existing cubemap capture used by the sky /
  environment baking if one exists, otherwise a minimal
  depth+color deferred-lite pass (opaque GBuffer + direct
  lighting only; no post-processing).
- **Conversion**: cubemap -> equirect via a small compute shader
  (direction mapping mirrors SurfaceLighting.hlsli, same as the
  prefilter), then EnvironmentPrefilterCS produces the mip chain;
  the result is registered with TextureManager under
  `probe://capture/<entity>` and the probe's SRV index points at
  it — the selection, box projection and blending code does not
  change at all.
- **Budget**: captures are amortized: at most one probe capture
  per frame, round-robin over probes whose refresh is due; a
  probe keeps serving its previous capture meanwhile. Static
  scenes capture once.
- **Memory**: cubemap scratch is a single shared target reused by
  every capture; only the prefiltered equirect persists per probe
  (capped at 512x256, RGBA16F mip chain).
- **Invalidation**: probe transform edits, capture-flag toggles,
  and an explicit editor "recapture" action mark the probe dirty;
  entity deletion releases the registration.

## Steps

1. Cubemap scratch + 6-face opaque capture pass at a position.
2. Cube->equirect conversion shader + prefilter integration +
   registration; probes with CaptureLive sample the capture.
3. Amortized refresh scheduling + editor recapture action.

## Acceptance

- A probe in a test room reflects the room's colored walls (not
  the authored sky) in the capture comparison tooling.
- Frame cost with 4 live probes: captures add at most one
  6-face low-res pass per frame (round-robin), steady-state zero.
- Authored-texture probes are unaffected.
