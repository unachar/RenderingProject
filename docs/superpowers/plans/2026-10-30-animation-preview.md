# Animation Preview Implementation Plan

> Use superpowers:executing-plans to implement in the current session. The user has approved execution; preserve existing local edits and do not commit automatically.

**Goal:** ImGui でアニメーションを独立プレビューし、選択モデルへ適用する。
**Architecture:** UI が独立モデルを所有し、既存 GPU スキニングとプレビュー描画を利用する。適用時のみ新しい実行時モデルを ModelManager に渡し、Undo 用スナップショットを記録する。
**Tech Stack:** C++20, ImGui, DirectX 12, Assimp, existing VMD loader.
**Spec:** docs/superpowers/specs/2026-10-30-animation-preview-design.md

## Global Constraints

- 左に設定、右にプレビュー。Computer Use は使用しない。
- 既存ユーザー変更を維持する。適用失敗時はシーンを変更しない。
- プレビューのループ設定はプレビュー内のみ。適用する永続設定は既存形式のクリップ一覧・再生選択・速度とする。

## Review Focus

- 同じファイルのモデルが複数ある場合も、適用先以外のポーズ・クリップを変更しない。
- 無効なファイル・空の選択・全クリップ無効時は、元の状態を壊さない。
- 停止中のシーク、長さの異なる複数クリップ、末尾のループ境界。
- モデル切替・ウィンドウ終了時の GPU リソース寿命。
- 保存・再読込・Undo / Redo とタイムライン駆動の扱い。

## Task 1: Playback policy and independent rendering

- [x] Add tests/animation_preview_tests.cpp and scripts/test-animation-preview.ps1. Test enabled-layer duration, loop wrap, paused seek, invalid delta, nonloop endpoint, empty selection, duplicate clip names, and shorter-layer sampling.
- [x] Run the test before implementation; expect missing helper header.
- [x] Implement source/editor/animationpreviewstate.h with Clip, Playback, ValidateClips, Duration, SampleTime helpers.
- [x] Add source/rendering/animationpreviewcontext.h and implement its independent target, orbit camera, material constants, GPU skinning, and indexed mesh rendering alongside existing material preview internals.
- [x] Expose AnimationModelResource::GetAnimationDuration and ModelManager::RegisterAnimModelInstance for staged application.
- [x] Run policy tests; expect pass.

## Task 2: Editor window and scene application

- [x] Add source/editor/animationpreviewwindow.h/.cpp with Draw(bool*), Render(), Shutdown(). Use a fresh candidate for every model/clip load and keep the old model on failure.
- [x] Add model import, animation file picker, clip enable/add/replace/remove, playback/seek/speed/preview loop, orbit/reset and selected-target validation.
- [x] Integrate menu, window visibility settings, render-frame callback and GPU-idle shutdown. Add friend access for Undo snapshots and an Inspector entry point.
- [x] Apply a staged pose instance to the selected matching animation model only, preserve other components, notify GPU scene changes, reset physics, and push Undo.
- [x] Add project entries and formatting for changed files only.

## Task 3: Verification and review

- [x] Run playback tests and existing material-preview UV tests.
- [x] Build Release x64 and inspect actual result. Run affected existing scene/config checks where feasible.
- [x] Review resource transitions, descriptor ownership, failed-load paths, undo/serialization, and shared-model isolation. Fix material findings and verify again.
- [x] Document how to open/use the window and what requires user visual verification.


## Verification record

- Release x64 Build: PASS, 0 warnings / 0 errors. Final log: build/animation-preview-final-build.log. Disabled parallel shader build and parallel C++ compilation for validation after the environment exhausted compiler memory during the parallel build.
- scripts/test-animation-preview.ps1: PASS (playback/selection; resource ownership + fence retirement; actual pose loading without obsolete clips; actual scene Archive JSON round-trip + legacy compatibility). Log: build/animation-preview-tests.log.
- Resource lifetime test also passed after adding shutdown-before-Undo-release coverage.
- scripts/test-material-preview-uv.ps1: PASS. scripts/test-editor-config.ps1: PASS, 24 checks.
- Independent review found two P2 issues: model accumulation across repeated Apply and inherited obsolete clip dependencies. Both fixed; resource retirement and pose replacement tests cover the fixes. The pose replacement test failed against the old loading behavior before passing with copyAnimations=false.
- Model instances applied from this UI use shared scene/history ownership and GPU-fence retirement. Runtime ownership is not serialized; independentPose is serialized so restored scene models recreate separate resources.
- Ruling: preview loop and seek are preview-only, while applying stores clip registration, active selection and speed using the existing scene playback contract. VMD supports multiple layers; non-VMD clips are mutually exclusive to match the existing engine.
- No commits, GUI automation, engine UI launches, or edits to the user's existing scene/settings/entity changes were performed. Visual interaction and end-to-end Undo/Redo remain manual checks, documented in docs/animation-preview.md.
