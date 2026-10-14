# Material Assets Implementation Plan

> Use superpowers:executing-plans to implement this plan in the current session. The user authorized implementation on 2026-10-14; proceed without another approval handoff.

**Goal:** Implement the complete material asset, assignment, editing and preview workflow.

**Architecture:** Stable metadata handles resolve to shared MaterialRuntime objects. Extend MaterialBindings and the existing GPU record instead of creating separate material logic for GPU LOD or preview.

**Tech Stack:** C++20, DirectX 12, ImGui, existing bundled JsonCpp.

**Spec:** `docs/material-asset-design.md` and the user supplied full specification.

## Global Constraints

- Preserve legacy entity materials and the existing DeferredPS user change.
- Save only on explicit Save; update shared runtime during editing.
- Keep GPU descriptor lifetime and per-frame constants safe.
- No Shader Graph, Material Instances or per-entity overrides.

## Review Focus

- Malformed files and failed writes preserve saved content and report errors.
- Duplicate metadata IDs must not silently replace registry entries.
- Missing textures/materials must fall back without crashing.
- Multi-slot assignment must affect only the chosen slot.
- Undo across Save/Revert must keep runtime and dirty state consistent.

## Task 1: Asset foundation

Files: new `source/assets/materialasset.h`, `materialasset.cpp`, `assetregistry.h`, `assetregistry.cpp`, `tests/material_asset_tests.cpp`, `scripts/test-material-assets.ps1`, project build registration.

Interfaces: `AssetHandle`, `AssetDragPayload`, `MaterialAssetData`; `MaterialSerializer::Load/Save`; `AssetRegistry::Register/Resolve/Refresh/Move/Duplicate`.

- [x] Test defaults, JSON roundtrip, malformed/range/version rejection, failed writes, handle roundtrip, stable rename/move, duplicate identity and collision rejection.
- [x] Run tests and confirm missing implementation fails.
- [x] Implement asset serialization and persistent metadata using bundled JsonCpp.
- [x] Run CPU tests until passing.

## Task 2: Runtime and rendering

Files: new `source/assets/materialmanager.h/.cpp`; modify componentmanager, materialbindings, gpuscene, modelsystem, rendersystem, materialsystem, instancingsystem, graphicsdevice and production HLSL.

Interfaces: `MaterialManager::GetRuntime/Resolve/SetEditing/Save/Revert`; `MaterialBindings` consumes resolved runtime components; entity MaterialSlots hold handles.

- [x] Add runtime tests for shared updates, revision tracking, missing fallback and no reloading on constant edits.
- [x] Share one runtime per handle, resolve texture descriptors once per texture change, update existing GPU material entries in place.
- [x] Support all five texture slots and common PBR sampling across CPU and GPU geometry shaders.
- [x] Make multi-submesh draws use the chosen slot; prevent incompatible instancing/indirect batching.
- [x] Build and compile affected HLSL.

## Task 3: Assignment and scene references

Files: editorassets, editorscene, editorhistory, editorinspectors, imguimanager; new scene reference serialization.

- [x] Add typed material/texture payloads, creation, duplicate, rename and move UI.
- [x] Use existing picking bounds for drop target highlight; multiple slots require popup selection.
- [x] Store slot handles in existing Undo snapshots and persistent scene reference file; reject non-renderable entities.
- [x] Verify assignment, isolated slots, Undo/Redo and persistence.

## Task 4: Asset editor

Files: new `source/editor/editormaterialassets.cpp`; modify imguimanager and history.

- [x] Add asset-focused properties, texture D&D, dirty title, Save, Revert, Ctrl+S, grouped edit history and close confirmation.
- [x] Verify edits update shared scene materials and failed saves retain dirty state.

## Task 5: Production preview

Files: new `source/rendering/materialpreview.h/.cpp`; integrate renderframe and editor.

- [x] Render sphere/cube/plane with shared runtime, production shader and binding into dedicated RT.
- [x] Provide orbit, zoom, reset and lighting controls; render only when visible.
- [x] Verify GPU resource states, frame constants and scene/preview agreement.

## Task 6: Validation and review

- [ ] Run CPU suite, Debug and Release builds and rendering smoke validation.
- [x] Add runtime validation for shared material records, assignment, defaults and GPULOD normal-map equivalence where supported.
- [x] Review modified code and record exact passed / unavailable acceptance checks.

## Execution record

Ruling: The user's explicit request to implement authorizes native execution of this plan; do not ask them to approve the same scope again.

実装・レビュー済み。検証の対象と仕様との差分は `docs/material-asset-validation.md` を参照。UI開始側のネイティブ手操作とMesh Shader専用の画素比較は未実施であり、上記チェックはそれらの合格を意味しない。
