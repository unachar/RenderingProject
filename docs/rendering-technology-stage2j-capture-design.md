# Stage 2J follow-up — live cubemap capture for probes: design

Status: IMPLEMENTED - the 6-face capture pass, conversion,
prefilter, registration and round-robin scheduling have all
landed (see "As built" at the end of this doc for the route and
its v1 limits). Probes today sample authored equirect environment
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

## Progress (500-push run)

- Step 2's conversion is landed: EnvironmentCubeToEquirectCS +
  ReflectionProbes::ConvertCubeToEquirect (registered equirect out).
- Step 1's groundwork is landed: the shared capture scratch
  (EnsureCaptureScratch, 6-slice RGBA16F RT target), the cube-face
  view/projection helper (GetCaptureFaceMatrices), and the
  CaptureLive flag on ReflectionProbeComponent.
- Remaining: the 6-face scene capture pass itself (render the
  opaque scene into the scratch from a probe position), wiring
  CaptureLive probes through conversion + prefilter in
  SyncFromScene, and the refresh scheduling of step 3.
- Scoping of the capture pass (surveyed 2026-10-31): the engine
  has no offscreen scene-render API — ScenePasses is bound to
  the main frame's targets and the scene camera entity, and no
  other cubemap scene capture exists in the tree.

## As built (2026-10-31)

The capture landed on a different, lower-risk route than the
target swap scoped above — the MaterialPreview precedent:
`MaterialPreview::Render` already renders production-shaded
geometry into an offscreen target from `RenderFrame::EndDraw`
(after the main passes) with its own targets and full state
restoration, proving the integration point. The probe capture
(`ReflectionProbes::ProcessPendingCaptures`, called from EndDraw
next to the preview) does the same:

- One dirty probe per frame, round-robin by entity id
  (`CaptureState` per entity in the probes module; SyncFromScene
  refreshes centers and marks a state dirty on first sight and
  on moves > 5 cm; states of deleted or un-flagged probes are
  pruned).
- Per face: `ScenePasses::BeginPass` with the model root
  signature (real scene lights, shadow maps, environment and
  post-process constants), root 17 bound to the scene light
  constant buffer like the production mesh draws, then every
  opaque static model is drawn forward-lit with the production
  model shaders — per-entity `ConstantBuffer3D` with the face
  matrices from GetCaptureFaceMatrices, world from the
  transform, material fields following the instancing path's
  constants rules (mesh texture fallback included). Faces render
  into the shared scratch slices (128x128) over a private depth
  target, reverse-Z like the rest of the engine.
- The cube is then converted (256x128 equirect) and prefiltered
  through the landed steps, and published via the new
  `TextureManager::ReplaceExternalTexture`, which re-points the
  probe's SRV slot at the fresh texture and fence-retires the
  previous one — so a refresh replaces the capture in place
  under `probe://capture/<entity>[#prefiltered]`.
- Selection needs no changes: SyncFromScene resolves a
  CaptureLive probe to its capture SRV once one exists (the
  authored texture is served until then).

V1 limits, deliberate: animated/skinned models and
GPU-driven-only geometry (no StaticModelComponent) are not
captured; transparent materials are skipped; captures reuse the
main frame's forward-lighting bindings, so a MegaLights grid
built for the main camera may imprint on captured lighting;
refresh triggers are first-sight and probe movement (no
scene-dirty signal yet).

## Acceptance

- A probe in a test room reflects the room's colored walls (not
  the authored sky) in the capture comparison tooling.
- Frame cost with 4 live probes: captures add at most one
  6-face low-res pass per frame (round-robin), steady-state zero.
- Authored-texture probes are unaffected.
