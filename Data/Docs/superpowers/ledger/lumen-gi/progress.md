# SDD ledger — plan: docs/superpowers/plans/2026-10-19-lumen-gi.md

Spec: docs/lumen-integration-design.md

Ruling: ユーザーの「最適化も含めて実装して」を設計案への承認と実装継続指示として扱う。追加の手続き承認は求めず、このチャットで作業する。
Ruling: 既存checkoutはdev_main。ビルド依存とアセットが大きいため現workspaceを使用し、依頼外のGit操作・コミットは行わない。
Pre-flight: カード管理→GPUキャッシュ→ギャザーは同じスロット/インスタンス対応を共有する。GPU対応表は毎フレーム再作成し、永続IDとTLAS順序を分離する。

Task 1: complete — CPU RED→GREEN: stable ownership under reordering/removal/reuse, capture/update budgets, fair lighting updates, unsupported IDs, depth/normal rejection, quiet-period invalidation and stale lighting after motion.
Task 2: complete — 6 directional cards x16x16, 128 resident meshes, GPU bounds/material capture, separate read/write total/indirect irradiance. Debug GPU atlas readback confirms populated finite illuminated texels.
Task 3: complete — 8 cosine samples per screen probe, shared averaged signal, geometric rejection plus DXR connection visibility, direct-hit fallback, reflected indirect light without emission duplication. NRD shader variants compiled and Debug dynamic fixture passed.
Task 4: complete — mode3 preserves prior numeric values, budget/spacing persistence, diagnostics and governor fallback integrated. Interactive editor controls have not been manually exercised.
Task 5: in progress — final code build, baseline/single-bounce/spatial/performance comparisons and Release without NRD pending.

Ruling: GPU capture computes world bounds from current vertices instead of adding CPU asset-bound dependencies. Deforming/transparent geometry uses the hit-lighting fallback.
Ruling: Probe integrals are reduced once in the trace group, retaining eight samples plus one average; reconstruction reads4 averages instead of32 directional values per pixel.
Review: coplanar reconstruction now traces short connections through all opaque geometry. Albedo/emission captures are consumed by cached hit shading. Diffuse-only hits still exclude self-emission because emissive NEE estimates that path.
Review: continuously changing geometry does not clear the entire cache each frame. Dynamic history is2 updates, age is bounded to8 frames, and slots predating the last change remain rejected after the quiet period until refreshed. Static history is16 updates.
Review: per-frame upload failure does not publish a scheduler allocation that was never captured on GPU.
Evidence: build/lumen-tests/unit-and-shaders.log records CPU tests and20 DXC variants. The218-frame Debug DXR+NRD fixture passed, including continuous-motion accumulation and zero D3D12 errors. Final rerun after stale-slot fix is active.
