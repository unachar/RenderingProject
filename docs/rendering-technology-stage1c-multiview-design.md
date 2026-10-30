# Stage 1C follow-up — VG shadows for multiple views: design

Status: IMPLEMENTED (S1–S3). The traversal state is a 4-slot
per-view pool, `PrepareShadowView` takes a viewId and prepares one
traversal per shadow pass, and the mesh-shader shadow draw
consumes the current pass's slot. Interim mapping: viewId = shadow
pass index; the lightIndex * 8 + faceOrCascade decomposition lands
when the pass enumeration exposes it. Remaining ledger item: "no
shadow material binning" (a separate optimization step).

Status of 1C at design time: the dedicated VG shadow path covered
the first shadow view (pass 0, the first directional light's first
cascade), including alpha-tested casters (v2). The remaining ledger
limits were "shadow pass 0 only" and "no shadow material binning".
This document designs the multi-view extension; implementation
followed the same small-step pattern as 1C itself.

## Why pass 0 only

The frame graph renders shadows before the camera traversal, so the
raster consumes a shadow visible list produced by a dedicated
shadow traversal. That traversal owns exactly one set of state
(view constants, visible/counter/argument buffers, group states)
keyed to a single view built from the first directional light.
Every additional shadow view needs its own traversal state set —
the camera traversal cannot be reused (it would omit off-camera
casters), and views cannot share buffers within a frame.

## Design

- **View ID scheme**: `viewId = lightIndex * 8 + faceOrCascade`
  (directional: cascade index; spot: 0; point: cube face 0..5), the
  scheme the expansion design already reserves. A per-frame list of
  requested shadow views is built where the shadow passes are set
  up, in the same order the legacy path draws them.
- **Per-view traversal state**: a small pool (e.g. 4 sets) of
  traversal state (constants upload, visible list, counters,
  indirect arguments, group states), each sized below the camera
  set — shadow views select far fewer clusters. Views beyond the
  pool fall back to the legacy submission for that pass, per view,
  so coverage never regresses (same rule as v1).
- **Traversal invocation**: the existing traversal entry point gains
  an explicit view-parameters block (view/projection, orthographic
  flag or perspective FOV, texel-error scale from the view's target
  resolution) instead of reading the camera globals. The camera
  invocation passes the current values unchanged.
- **Raster**: the mesh-shader shadow draw already takes the visible
  list as input; per view it binds that view's list and the pass's
  viewport/atlas region exactly as the legacy path sets them up.
  Alpha-tested shading (v2) is shared.
- **Latency model**: unchanged — a view's list is produced by the
  traversal of the previous frame for that same view slot, matching
  the current one-frame shadow latency; newly appearing views draw
  legacy for their first frame.
- **Material binning (separate step)**: shadow bins keyed by
  alpha-masked / two-sided flags can be layered on the per-view
  lists once multi-view works; it is an optimization, not a
  correctness requirement, because the v2 pixel shader resolves
  material data per cluster.

## Acceptance

Per-view A/B against the legacy path for spot and point lights
(shadow contour comparison with the capture tooling), off-camera
caster coverage, pool-overflow fallback correctness, GPU ms per
view, and no change to the camera path (existing traversal parity
tests green).
