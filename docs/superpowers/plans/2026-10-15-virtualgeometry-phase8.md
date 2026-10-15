# Virtual Geometry Phase 8 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** LOD境界の往復切替を抑え、古い遮蔽履歴で除外されたnative geometryを現在depthから同一frame内に復帰させる。

**Architecture:** GPUの希望refine履歴を常駐cutから分離する。完全な常駐cutを先に選択し、前frame Hi-Zで第1パスと再判定候補へ分割する。第1パスdepthから現在Hi-Zを生成し、候補だけを再判定・追加描画する。opaque両パスの和集合をvelocityでも使う。

**Tech Stack:** C++20、D3D12、HLSL cs_6_5 / ms_6_5、既存RenderGraph、MSBuild/CMake/PowerShell。

**Spec:** `docs/virtualgeometry-phase8-design.md`、2026-10-15にユーザー承認済み。

**Execution:** フェーズ7で選択済みのNative方式を継承。このセッションで実装し、最後に独立レビューを行う。計画の確認前は製品コードを変更しない。

**最新指示による変更:** 2026-10-15、ユーザーが短時間・最小使用量での自走とテスト省略を指定。以降はNativeで実装し、テスト・ビルド・追加agent・独立レビューを実施しない。Task 1〜5の製品コードを追加したが、以下のテスト手順とTask 6の新規harness/runtime/stressは未実施。`docs/virtualgeometry-phase8-progress.md` と `docs/virtualgeometry-phase8-validation.md` に実際の範囲を記録する。

## Global Constraints

- ヒステリシス初期幅10%。Target Errorを中心に粗→細は上側、細→粗は下側閾値。幅0は従来動作。Force Hierarchy Levelは履歴を迂回する。
- 希望LODとPage常駐cutを分離する。64 KiB Page、既存Budgetと両queue Fence、generation契約を維持する。
- instance削除・ID再利用、asset変更、native切替、camera cut、容量変更で履歴を無効化する。
- 第1・第2パスで同じclusterを描かない。native成功は両パス分の容量確保後だけ公開する。
- feedbackは第2パス後、velocityは最終可視集合を参照。通常frameに同期readbackや新たなCPU fence待ちを追加しない。
- Material binning、圧縮、DirectStorage、Hierarchy pagingは対象外。10億triangleは既知のVRAM制約を引き継ぐ。
- 既存checkout、フェーズ7変更、ユーザー設定を保持する。自動commit/push/merge、追加checkoutは行わない。

## Review Focus

1. GPU instance ID再利用時に別objectのrefine履歴を引き継がない（Task 2）。
2. 初期object/group遮蔽で第2パスの復帰候補が消えない（Task 3/4）。
3. 再判定領域だけが満杯でもpartial nativeと従来fallbackが重複しない（Task 3）。
4. Page到着待ち・Evictで希望refineを上書きせず、同じframe内のPage snapshotを保つ（Task 1/3/5）。
5. opaque後に更新した可視集合とvelocity・feedbackの参照時点が食い違わない（Task 4/5）。

---

### Task 1: ヒステリシスのCPU参照と常駐cut

**Files:** 新規`source/virtualgeometry/virtualgeometrytemporal.h/.cpp`、`tests/virtualgeometry_temporal_tests.cpp`。変更`virtualgeometryselection.h/.cpp`、`tests/virtualgeometry_tests.cpp`、`tests/virtualgeometry/CMakeLists.txt`、`DirectX12.vcxproj/.filters`。

**Interfaces:** `VirtualGeometryTemporal::Refine(float errorPixels, float targetPixels, float fraction, bool previousRefine, bool historyValid) -> bool`。`VirtualGeometrySelection::SelectResidentTemporal(asset, view, settings, clusterPages, residentPages, previousRefine, historyValid, fraction) -> TemporalResult`。`TemporalResult : ResidentResult`に`std::vector<uint8_t> DesiredRefine`を追加。入力履歴spanはgroup数一致を要求する。

- [ ] `HysteresisThresholds`を追加。target=1、fraction=.1では未refineの1.05はfalse、1.11はtrue。refine済みの.95はtrue、.89はfalse。履歴無効・幅0では既存の`error > target`。負値・非有限値は既存設定と同じ安全な既定値に正規化する。
- [ ] `DesiredRefineSurvivesPageMiss`を追加。子欠落中も希望refineはtrue、描画は親。全到着で子へ、Evictで親へ戻り、共有親の選択cutとRequestは重複しない。Force Levelは履歴に依存しない。
- [ ] `--temporal-cpu`入口を追加し、未実装による失敗を確認してから参照処理を実装する。既存の履歴なしAPIの結果を変えない。
- [ ] Debug/Releaseで新規テストと既存resident selectionを実行し、結果をledgerへ記録する。

### Task 2: GPU希望LOD履歴と識別子

**Files:** 変更`source/rendering/gpuscene/gpuscene.h/.cpp`、`source/virtualgeometry/virtualgeometrytraversal.h/.cpp`、`shader/hlsl/VirtualGeometryTraversalCS.hlsl`。新規`shader/hlsl/VirtualGeometryTemporal.hlsli`。変更GPU harnessとshader依存target。

**Interfaces:** `GpuScene::InstanceHistoryGeneration(GPUInstanceID) -> uint64_t`（slot再割当・geometry identity変更で増加、transform更新では不変）。`VirtualGeometryTraversal::InvalidateHistory()`、Settingsに`HysteresisEnabled=true`、`HysteresisFraction=.1f`、`TwoPassOcclusion=true`を追加。履歴GPU headerは32 bytes `{uint64 Generation; uint Asset, GroupBase, GroupCount, Epoch, Padding[2];}`、group状態はuint。CPU/HLSLのstrideをassertする。

- [ ] GPU harnessに境界往復・明確な接近離反・履歴無効・幅0・Force Level・Page欠落を追加し、Task 1 oracleとの差で失敗を確認する。
- [ ] instance slot再利用、同一IDのasset交換、native無効→有効、camera cut、履歴容量変更のテストを追加する。別identityの履歴を使わない。
- [ ] group履歴の前/次bufferをGPU上で保持し、同じgraphics queueのsubmission順とRenderGraph barrierで連続frameを接続する。CPUが未完了bufferを書き換えない。layout変更時はepochを更新し、旧bufferはframe fenceでretireする。
- [ ] 希望refineを常駐条件適用前に更新する。既存shared-parent closureはその後に適用する。履歴領域不足はinstanceの履歴だけを無効にし、安全な既存選択を使う。
- [ ] shader依存に新includeを登録し、GPU/referenceと`test-shader-build-targets.ps1`を合格させる。

### Task 3: 完全なcutの予約と2パス候補分割

**Files:** 変更`VirtualGeometryTraversalCS.hlsl`、`virtualgeometrytraversal.h/.cpp`、`tests/virtualgeometry_traversal_gpu_tests.cpp`。新規`tests/virtualgeometry_temporal_gpu_tests.cpp`。

**Interfaces:** `enum class RasterPhase { First, Second, Combined };`。既存`VisibleBuffer/CounterBuffer/ArgumentBuffer()`はCombinedとして維持し、各APIに`RasterPhase`を受けるoverloadを追加する。`Prepare`は完全cutとphase別出力予約まで行う。新規`Retest(RenderGraph&, ResourceHandle currentHiZ)`が候補を再判定する。候補は16-byte VisibleVirtualClusterと同じlayout、上限`MaxSelectedClusters`。候補/phase別引数とcounterはframe単位で保持する。

- [ ] 小規模fixtureで全可視、全遮蔽、部分遮蔽、history無効、動くobject、現在Hi-Zで復帰、現在も遮蔽を検査する。FirstとSecondは交差なし、Combinedは両者の和集合。
- [ ] 以前group全体が遮蔽された場合でも復帰候補が残ること、出力/候補の容量1と境界、root欠落、Page部分到着で排他fallbackになることを検査する。
- [ ] 未実装の失敗を確認後、LOD/常駐cutの決定から時間的Occlusionを分離する。完全cutの全clusterについて第1パスまたは候補領域をinstance単位で予約してからSuccessを公開する。失敗時は出力もSuccessも公開しない。
- [ ] 第1パスはprevious VP/Hi-Zを保守的に使用し、移動objectとhistory無効では遮蔽しない。Retestは現在VP/Hi-Zを使う。near-plane交差・画面外project・不正depthは保守的に可視とする。
- [ ] 第2パスではLOD/cutとPage snapshotを変更せず、既存候補の可視性だけを判定する。CombinedはFirstの後ろへSecondを追加し、最終引数を再生成する。GPU harnessを合格させる。

### Task 4: 現在depthによる再判定と追加raster

**Files:** 変更`source/rendering/meshshader/meshshaderrenderer.h/.cpp`、`source/rendering/gpudriven/gpudrivenrenderer.cpp`、`source/rendering/scenepasses.cpp`、`source/rendering/occlusionculling.h/.cpp`、`source/rendering/visibility/gpuvisibility.cpp`。必要な呼出元変更は既存geometry pass内に限定する。

**Interfaces:** `OcclusionCulling::GetCurrentResource() -> ID3D12Resource*`を追加。`MeshShaderRenderer::DrawSecondPass(ID3D12GraphicsCommandList*, GpuSceneBuffer&) -> bool`を追加し、既存DrawはFirst、DrawVelocityはCombinedを使用する。

- [ ] 遮蔽壁の背後object、壁の移動/除去、camera teleportでOcclusion無効のdepth/coverage参照と比較するruntime fixtureを先に追加する。古いHi-Zで隠れていたobjectが同じframeに復帰しない失敗を確認する。
- [ ] GPUVisibilityの早期object遮蔽でnative候補を落とさず、native traversalへ引き渡す。非native経路の選択規則は維持する。
- [ ] opaque順序を「native Firstを含むopaque depth → BuildCurrent → Retest → native Second」とする。depthをclearし直さず、現在のGBuffer/DSVを再bindする。既存`BuildOcclusionHierarchyAndBeginPhaseTwo`のGPU-driven除外を調整し、従来2パスを二重実行しない。
- [ ] Hi-Zのwrite→readとdepthのCOPY_SOURCE→DEPTH_WRITEを明示し、別RenderGraph間も実resource stateを受け渡す。使用不可時はOcclusionなしのFirst描画へ戻す。
- [ ] First/Second専用drawではlegacy meshletとtraditional fallbackを重ねて再描画しない。現在Hi-Z生成前のdepthにunsupported-material等の描画が含まれるpass順序を確認する。
- [ ] Debug runtimeのcoverage一致・routing排他・D3D12 error 0を確認する。

### Task 5: Velocity・feedback・非同期統計

**Files:** 変更`virtualgeometrytraversal.cpp`、`virtualgeometrystreaming.cpp`、`meshshaderrenderer.cpp`、`virtualgeometrydebug.cpp`、`source/rendering/renderprofiler.h`。

**Interfaces:** `VirtualGeometryTraversal::RecordFeedback(RenderGraph&)`を追加し、準備直後のfeedback copyを最終再判定後へ移す。2パス無効時も1回だけ記録する。統計は`LodTransitions, FirstVisible, RetestCandidates, SecondVisible, CapacityFallbacks, HistoryBytes, CandidateBytes`のuint64値とpass別GPU profiler eventを使う。

- [ ] 移動nativeのFirst/Second/Combined velocityを参照と比較する。Second復帰clusterがvelocityから欠落しないこと、native disabledとunsupported materialの経路を確認する。
- [ ] 両passで同一Page requestを重複排除し、usageは実際に参照したPageを含める。feedback copyのfenceは最終commandsの実submissionを指し、copy完了だけをCPUがpollする。
- [ ] UIにヒステリシス幅と2パス設定を追加する。統計はFence完了後のreadback ringで取得し、通常frameにRenderCapture/WaitForGpuIdleを使わない。ProfilerのGPU時間は実timestamp eventを使い、取れなければN/Aとする。
- [ ] Page Evict、予算縮小、I/O遅延中の2パス・native切替でsnapshotと両queue寿命を検証する。

### Task 6: Runtime acceptanceと規模検証

**Files:** 新規`source/virtualgeometry/virtualgeometrytemporalvalidation.h/.cpp`、`scripts/validate-virtualgeometry-phase8.ps1`。変更`source/core/renderingvalidation.cpp`、project登録、既存stress harnessの再利用部分。

**Interfaces:** `VirtualGeometryTemporalValidation::Enabled/BeforeFrame/AfterFrame/Finish`はPhase 7 hookと同じ契約。script引数は`-Configuration Debug|Release -Gpu -Runtime -StressTriangles 30000000|100000000 -SkipRuntimeBuild`。CPUだけの実行も可能にする。

- [ ] 決定的な境界往復、接近離反、遮蔽物移動、camera cut、instance削除/再作成、asset変更、native切替、候補overflowを実行する。全stage実行数をassertし、空画面一致を合格にしない。
- [ ] ヒステリシス無効との切替数、2パス無効とのGPU時間、再判定・復帰件数を記録する。同期captureは比較sampleだけ、通常frameにgraphics未完了workが存在することを検査する。
- [ ] 小規模acceptance合格後に3000万Debug→1億Release source triangleで実行する。phase別可視数、RAM/VRAM、steady CPU/GPU時間、Page転送・Evict、穴・重複・error数を保存する。
- [ ] scriptは起動環境とユーザー設定をfinallyで復元し、processを非同期pollする。生成物cleanupは検証root内の対象fileだけとし、失敗ログは残す。

### Task 7: 回帰・レビュー・結果記録

**Files:** 新規`docs/virtualgeometry-phase8-progress.md`、`docs/virtualgeometry-phase8-validation.md`。変更本計画と設計書の状態。

- [ ] Taskごとに実装範囲、RED/GREEN、検証commandと結果、設計変更をledgerへ残す。未実行・SKIPをPASSにしない。
- [ ] full Debug/Release CPU（Release million含む）、shader依存、Phase 6 GPU/raster/velocity、Phase 6 runtime、Phase 7 streamingを回帰確認する。同一変更・環境で有効な成功証拠は再利用する。
- [ ] 最後に独立レビューを1回行い、history identity、group closure、候補予約、pass順、depth state、両queue寿命、fallback排他、frame統計の同期有無を確認する。指摘修正に必要な検証を追加する。
- [ ] `git diff --check`とコード可読性確認を行い、要件ごとの証拠・未検証条件・再開commandをvalidation文書へ記載する。ユーザーへ実測結果を報告する。自動commitはしない。

## 計画自己レビュー

設計のヒステリシス・identity resetはTask 1/2、2パスと候補消失防止はTask 3/4、velocity/feedback/UI/ProfilerはTask 5、受け入れと規模検証はTask 6/7に割り当てた。完全cutを先に作る方式はgroup遮蔽での候補消失を避ける一方、遮蔽領域のLOD traversal省略は行わず、metadata処理費用の削減は主張しない。既存APIをCombinedの互換入口として維持し、phase型と参照時点を固定した。最終実行方式はNativeを継承し、計画レビュー後に着手する。
