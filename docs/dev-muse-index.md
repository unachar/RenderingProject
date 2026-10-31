# dev_muse documentation index

Everything the dev_muse work produced, by purpose. Start with
the adoption status for the current state of the branch.

## State and process

- `rendering-technology-adoption-status.md` - the ledger: what
  landed, what was evaluated, what is deferred and why.
- `dev-muse-fallback-matrix.md` - every feature's flag and its
  fallback when off or failed (Stage 9 acceptance).
- `dev-muse-500-verification-checklist.md` - the Windows-side
  build/run checklist for the 500-push run.
- `dev-muse-ab-runbook.md` - per-feature A/B decision
  procedures (metrics, thresholds, setup).
- `dev-muse-50-push-batch-summary.md` - the verified 50-push
  batch (historical).
- `rendering-technology-survey-2026-10-30.md` / `.json` - the
  original technology survey the plan was built from.

## Designs (implemented unless noted)

- `rendering-technology-ssr-design.md` + `...-ssr-temporal-design.md`
  - native Hi-Z SSR and its temporal accumulation.
- `rendering-technology-stage1c-multiview-design.md` +
  `...-stage1c-binning-design.md` - VG shadow views and shadow
  material binning.
- `rendering-technology-stage1d-design.md` - DBuffer decals.
- `rendering-technology-stage2h-design.md` - the reference
  path tracer (oracle).
- `rendering-technology-stage2j-design.md` +
  `...-stage2j-blending-design.md` +
  `...-stage2j-capture-design.md` - local reflection probes,
  two-probe blending, live capture (pass still outstanding).
- `rendering-technology-color-management-design.md` - the
  pipeline's color spec of record.
- `rendering-technology-integration-validation.md` /
  `rendering-technology-stage1c-validation.md` - validation
  records from the earlier batches.

## Evaluations (adopt / defer decisions)

- `rendering-technology-sssr-evaluation.md` - why SSSR was not
  vendored (native SSR instead).
- `rendering-technology-tracy-evaluation.md` - opt-in later.
- `rendering-technology-gi-cache-evaluation.md` - keep the
  incumbent hash cache.
- `rendering-technology-fastgltf-evaluation.md` +
  `rendering-technology-stage4-asset-evaluation.md` - asset
  pipeline candidates and their gates.
- `rendering-technology-xess-evaluation.md` +
  `rendering-technology-stage8-foundation-evaluation.md` -
  Stage 8 candidates and their profile triggers.

## Tooling (repo root)

- `scripts/pfm_compare.py` / `scripts/pfm_stat.py` - Stage 2H
  capture metrics (RMSE / SSIM / region stats).
- `scripts/parse_vram_log.py` - D3D12MA log summaries.
- `scripts/compile_check_linux.sh` - Linux DXC compile sweep.
- `tests/` - shader scan, PFM tooling tests, and the source
  contract tests for the run's cross-file agreements.
