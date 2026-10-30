# Stage 2 H — Offline reference path tracer: design

Implements expansion feature H (Phase 2 H in
`rendering-expansion-progress.md`): an independent progressive DXR
reference integrator used to validate the real-time GI/reflection
systems (ReSTIR GI, NRD denoising, MegaLights) against ground truth
on fixed scenes, with fixed seeds and reproducible output.

## Why independent

The reference must not share sampling, denoising or material
evaluation shortcuts with the systems it validates, or comparisons
become circular. It therefore gets its own integrator shader and
its own (deliberately simple, brute-force) sampling, reusing only
scene access: the same BLAS/TLAS, instance and material buffers the
real-time DXR passes consume, so both sides render *the same scene
data* by construction.

## Staging note

V1 (landed) renders fixture scenes the tool builds itself:
`ReferencePathTraceCS` consumes one combined position/index
buffer, per-instance transforms and per-instance analytic PBR
materials, plus an analytic light list — enough for the
Cornell/metal/glass/emissive fixtures. Evaluating the engine's
packed scene materials for arbitrary game scenes is the follow-on
step; the integrator, seeding and accumulation contracts stay the
same.

## Design

- **Integrator**: a standalone DXR compute/raytracing shader
  (`ReferencePathTraceCS` family) implementing unidirectional path
  tracing: next-event estimation to the scene's analytic lights,
  MIS between light sampling and BSDF sampling, Russian roulette
  after a few bounces, max depth configurable (default 8). Materials
  come from the packed scene material buffer with the engine's GGX
  PBR evaluation — the one deliberate share, since material
  *parameters* must match; the sampling of them is independent.
- **Progressive accumulation**: a per-job accumulation target
  (RGBA32F) plus a sample counter; each dispatch adds N samples per
  pixel with a per-pixel seed derived from (pixel, sample index,
  job seed) so runs are reproducible and resumable. Variance falls
  as 1/N; the job stops at a target sample count or on cancel.
- **Job model**: `ReferenceCapture` jobs are created from the
  validation/benchmark tooling (not the per-frame loop): set camera
  (from a named scene camera or explicit matrices), resolution,
  samples per pixel, seed; run over as many frames as needed on the
  existing command infrastructure; read back to EXR plus AOVs
  (albedo, normal, depth, direct/indirect split) for the
  capture-comparison tooling.
- **Outputs and metrics**: linear HDR RMSE/SSIM against stored
  references with the existing comparison tests; reference images
  for Cornell-box / metal / glass / emissive fixtures are stored
  under the validation assets once generated on a known-good build.
- **Non-goals**: no real-time use, no denoising, no spectral
  rendering, no participation in the frame graph of the game view.

## Acceptance (from the ledger)

Reference images for the fixture scenes, monotonically decreasing
variance with samples, identical output for identical seeds,
linear HDR RMSE/SSIM reporting wired into the comparison tooling.

## Progress and follow-ups (500-push run)

Landed: integrator v1 (analytic NEE + GGX/cosine BSDF), Cornell /
material-test / emissive-panel fixtures, quad area-light NEE with
the solid-angle PDF, PFM/PPM writers, validation-hook captures.

- **MIS (next)**: flagged light sources currently emit only for
  camera rays; BSDF hits on them are skipped to avoid
  double-counting with NEE. Balance-heuristic MIS between the
  light PDF (solid angle for quads, delta for analytic) and the
  BSDF PDF (GGX/cosine mixture, already computed for the sampling
  estimators) removes the bias for glossy chains and lets emission
  count at every depth. Needs the hit material's light-source flag
  plus the quad geometry lookup by material — store a
  material->light index in material entry 3 (.y) when building
  fixtures.
- **EXR writer**: PFM is the HDR deliverable today; EXR joins if
  the comparison tooling standardizes on it.
- **AOVs** (albedo/normal/depth, direct/indirect split): designed
  above, not yet emitted by the capture host.
- **Real-scene captures**: evaluating the engine's packed scene
  materials in the integrator remains the long-term step; fixture
  scenes are the contract until then.
