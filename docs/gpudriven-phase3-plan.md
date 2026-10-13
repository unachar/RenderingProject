# GPU Driven Phase 3 Implementation Plan

**Goal:** GPU SceneからOpaque Static MeshのcommandをGPUで生成し、count buffer付きExecuteIndirectで描画する。
**Spec:** [設計](gpudriven-phase3-design.md)とユーザーのPhase 3仕様。
**Architecture:** 既存Scene同期とfence retirementを維持する。Static CPU Geometryから常駐共有VB/IBを作り、frame slotごとのVisible ID/Count/Command bufferをRenderGraphで接続する。
**Tech Stack:** C++20 / DirectX12 / HLSL SM5.1 / MSBuild / ImGui。

## 制約と判断

- 未コミットのPhase 2 / VirtualGeometry変更を維持。関連変更以外をcommitしない。
- 24-byte command、capacity >= Scene capacity、通常readbackなし、GPU counter reset。
- Deferred Standard PBRを最初の対象とする。Forward、Visibility/Compute GBuffer、特殊Shader、変形、LOD、outline対象は安全にCPUへfallbackする。
- 実装指示を受けたため、このsessionで直接実装と検証を続行する。
- Review focus: 空Scene、削除IDの再利用、frame slotの成長、Descriptor範囲外、CPU/GPU双方での重複描画。

## Task 1 — Geometry / layout

- [x] `tests/gpudriven_tests.cpp`で24-byte layout、offset、capacity成長、overflow、shared geometry offsetsを検証し、未実装の失敗を確認。
- [x] `source/rendering/gpudriven/indirectcommand.h`にcommandと安全なcapacity/geometry reservationを定義。
- [x] `gpudrivenrenderer.h/.cpp`に`RegisterGeometry(meshID, StaticMeshData)`、`Prepare(graph, slots, capacity)`、`Draw(commandList)`、`HandlesEntity(entity)`を追加。
- [x] `gpuscene.cpp`のStatic登録で共有Geometry offsetと対象flagを更新。既存Mesh record layoutは維持し、別Geometry tableを使用する。

## Task 2 — GPU commands / graph

- [x] reset、compact、generate CSを追加。Visible IDとcountはGPUで管理し、capacity dispatchでbounds check。
- [x] Root constant + DRAW_INDEXED signature。3 frame slot buffers、既存retirement、必要時だけ成長。
- [x] Scene graphにCompute passesを接続。Opaque graphの実resource宣言でScene SRV、Geometry VB/IB、Indirect stateを管理。
- [x] buildでC++/HLSL layoutとsignatureを検証。

## Task 3 — Opaque / fallback

- [x] 既存MRT shadingを共有するGPU VS/PSを追加。Instance ID、World、raw Material、Texture配列を接続。
- [x] `framepipeline.cpp`でOpaqueのGPU Drawを実行し、`modelsystem.cpp`と`instancingsystem.cpp`で対象EntityをCPU opaqueから除外。
- [x] toggleとDebug UI、Profilerを接続。CPU submissionを両modeで測定。

## Task 4 — validation

- [x] `gpudrivenvalidation.cpp`と`validate-gpudriven.ps1`で100/1000/10000 indexed Static Mesh、CPU/GPU GBuffer比較、command/count/ID validation、camera移動、create/delete、capacity拡張を検証。
- [x] Debug/Release buildとruntime、Debug Layerを完了。各84 frames、失敗0、Debug D3D12 errors 0。
- [x] GPU Based Validation合格。Phase 3の実経路に絞った84 frames、最大50000 commands、D3D12 errors 0。フレーム全体のGBVはtimeoutのため未確認。
- [x] 実測と未検証制約を`docs/gpudriven-phase3.md`に記録。50000はECS上限との整合性を確認して実行。

## 最終確認

- Shader／Name／ProceduralMotionの編集通知漏れを、追加テストの3失敗で再現して修正。最終テストは合格。
- 5 submeshes × 10000 Entityの50000 commandsをDebug／Releaseで検証。CPU／GPU GBuffer比較も差分0。
- CPU fallbackは変更時に一覧を更新し、GPU対象Static EntityをOpaque CPU loopへ列挙しない。
- 既存GPU Scene／描画機能のDebug回帰84 frames合格。変更後の最終reviewにmust-fix指摘なし。
- Releaseの既存中間ファイルによるLNK1103／PCH設定不一致C2855はクリーン再ビルドで解消。追加検証後の最終Debug／Release 84 framesも合格。
- 2026-10-13: GPU Scene／Culling／Command Generation／Opaque ExecuteIndirectを使うPhase 3 GBV試験が84 frames合格。既存補助Passを含むフレーム全体のGBVはtimeoutを別途記録する。
