# Stage 1C follow-up — shadow material binning: design

Status: DESIGN. Multi-view VG shadows are implemented (S1–S3);
the remaining 1C ledger item is "no shadow material binning".
Today the dedicated shadow draw runs one ExecuteIndirect over
the view's visible list with the alpha-tested mesh/pixel shader
pair for every caster: correct for all materials (v2 resolves
material data per cluster in the PS), but opaque casters pay the
alpha path's texture fetch and discard logic.

## Design

- **Two bins**: bin 0 = opaque casters (depth-only: mesh shader +
  null/empty PS), bin 1 = alpha-tested / alpha-blended casters
  (current v2 PS). Two-sidedness stays a per-cluster PS/MS concern
  in bin 1; bin 0 casters are front-face culled as today.
- **Where binning happens**: in the traversal's output stage. A
  cluster's bin is a function of its material (already resolved
  per cluster by the traversal for LOD), so the traversal appends
  to per-bin visible lists with per-bin counters. Per view slot:
  Visible/Counters/Arguments are allocated with a bin dimension
  (2x the argument records, counters offset per bin); the existing
  single-list layout is the binCount = 1 degenerate case during
  migration.
- **Raster**: DrawShadow issues up to two ExecuteIndirect draws
  per view — bin 0 with the depth-only PSO, bin 1 with the alpha
  PSO — reading the same per-view slot buffers at bin offsets.
  Both draws are additive into the same shadow target, order
  irrelevant for depth.
- **Fallbacks**: if the depth-only PSO fails to build, bin 0 draws
  with the alpha PSO (today's behavior). Scenes without alpha
  casters skip the bin-1 draw entirely (counter = 0 fast-out on
  the CPU via the existing counter readback? No — ExecuteIndirect
  with a zero-count argument buffer is already a no-op; no
  readback needed).
- **Sizing**: bin lists share the view's selected-cluster capacity
  (a cluster lands in exactly one bin), so total memory grows only
  by the extra counter/argument records.

## Steps

1. Traversal output gains the bin dimension (shader + buffer
   layout), still emitting everything into bin 0.
2. Bin classification from the cluster's material flags; bin 1
   path enabled.
3. Depth-only shadow PSO + dual-draw raster; PSO fallback.

## Acceptance

- Identical shadow maps (capture comparison) before/after on a
  scene with mixed opaque and alpha-tested casters.
- GPU ms of the shadow draw on a foliage-heavy scene (the alpha
  PS cost should leave the opaque majority).
- No change when the dedicated path is off.
