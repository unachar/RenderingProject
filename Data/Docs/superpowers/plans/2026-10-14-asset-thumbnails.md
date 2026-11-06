# Asset Browser Thumbnail Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Texture・Material・MeshをGUIDで管理する遅延サムネイルを追加し、既存Editor操作とGPU寿命の安全性を保つ。

**Architecture:** AssetThumbnailManagerが状態、優先Queue、Revision、依存、Cacheを管理する。ThumbnailRendererと独立ThumbnailPreviewContextが既存Material描画経路を利用する。Textureは既存Resourceを借用し、生成画像はFence付き所有Resourceとして管理する。

**Tech Stack:** C++20、DirectX 12、ImGui、DirectXTex、既存AssetHandle/AssetRegistry/MaterialRuntime。

**Spec:** ../specs/2026-10-14-asset-thumbnails.md、および同文書が参照するユーザー仕様全37節。

## Global Constraints

- dev_mainの0d72d8f2を土台とする。Phase 1〜4、Phase 5のThumbnail Size/Refreshを実装。
- 初期サイズ128x128。固定値ではなく設定値。生成件数とMemory上限も設定可能。
- 全件同期生成、UIからのGPU Wait、無制限Shader Visible Heapは禁止。
- AssetHandleをIdentityに使う。パスは解決・変更検出にのみ使う。
- 既存操作を維持し、生成失敗はFallback。生成仕様Version初期値1。
- Disk CacheはSource Assetディレクトリへ書かない。

## Review Focus

1. Queued中のDelete/Move：旧パスを読み直さず、消えたHandleは失敗・取消へ。
2. 生成中の再編集：古い結果で新RevisionをReadyにしない。
3. Cache排出と同フレームImGui参照：提出したFenceが完了するまでDescriptorを返さない。
4. 空/非有限Bounds・原点からずれたMesh：有限なCameraかFallbackで扱う。
5. 破損Disk Cache・Descriptor枯渇：Editorを止めず、再生成かFallbackへ。

## File Structure

- 新規source/assets/thumbnailcache.h/.cpp：GPU非依存のKey、状態、優先Queue、LRU方針。
- 新規source/editor/assetthumbnailmanager.h/.cpp：Registry解決、変更検出、Renderer/Cacheの調整。
- 新規source/rendering/thumbnailrenderer.h/.cpp：Texture解決、GPU生成、非同期アップロード。
- 新規source/rendering/thumbnailpreviewcontext.h/.cpp：独立Camera/Light/Geometry/Targetと共通Preview描画。
- 新規source/rendering/editordescriptors.h/.cpp：既存Heap内の有限スロット割り当てとFence付き返却。
- 修正source/rendering/materialpreview.h/.cpp：既存Material Previewの共通Context利用。操作仕様を維持。
- 修正source/rendering/rendererstate.h、graphicsdevice.h/.cpp、renderframe.cpp：Heap容量、Fence、生成Pass、Shutdownの接続。
- 修正source/assets/texturemanager.h/.cpp：借用Resource/SRVと待機しないロード接続。
- 修正source/assets/assetregistry.h/.cpp、materialmanager.h/.cpp：変更通知と依存更新。
- 修正source/editor/editorassets.cpp、imguimanager.h：Gridと既存操作。
- 修正DirectX12.vcxproj/.filters。新規tests/thumbnail_cache_tests.cpp、scripts/test-thumbnails.ps1、docs/asset-thumbnail-validation.md。

### Task 1: 状態・Queue・Cache Key

**Interfaces:** ThumbnailState、ThumbnailCacheKey(AssetHandle/assetRevision/dependencyRevision/generatorVersion)。Request(handle, priority)、MarkDirty(handle)、Remove(handle)、BeginFrame(frameIndex)、TakeNext()をGPU非依存Cacheに定義。Manager公開APIはRequest(AssetHandle)、MarkDirty(AssetHandle)、Remove(AssetHandle)、BeginFrame(uint64_t)、ProcessGenerationBudget()。

- [ ] CPUテストを追加：Request重複はQueue一件、SelectedがVisibleより優先、DirtyでRevision更新、MoveでHandle維持、DeleteでQueue除去、生成中再編集で旧結果を拒否。
- [ ] scripts/test-thumbnails.ps1を既存test-material-assets.ps1と同じVS検出方式で追加し、未実装のため失敗することを確認。
- [ ] thumbnailcache.h/.cppとManagerの状態契約を実装。Failedは変更/Refreshでのみ再試行し毎Frameの失敗ループを防ぐ。
- [ ] CPUテストを実行し全件成功を確認。変更単位をGit差分でレビュー。

### Task 2: DescriptorとGPU寿命

**Interfaces:** EditorDescriptorLeaseが所有スロットとCPU/GPU Handleを保持。Allocate()は枯渇時に失敗を返す。Retire(lease, resources, submissionFence)、Collect(completedFence)。Texture借用は所有Resource参照を保持しSRVを返却しない。

- [ ] CPU allocatorテスト追加：有限容量、重複返却拒否、completedFence < submissionFenceで再利用禁止、等値で再利用可能。
- [ ] RendererStateの既存Heap領域を調査し、衝突しない有限Editor領域を同じHeapへ追加。固定既存Descriptorを保持する。
- [ ] editordescriptors.h/.cppを実装し、renderframe.cppで提出Fenceを関連付け、BeginFrameで完了分のみ回収。Shutdownは既存GPU完了手順後に解放。
- [ ] CPUテストとDebugビルドで検証。Debug Layerで同フレーム参照・排出とDescriptor枯渇を確認。

### Task 3: TextureとGrid UI

**Interfaces:** ThumbnailHandleはstate、Key、dimensions、lastUsedFrame、所有/借用参照とGetImGuiTextureID()を持つ。RendererのTexture要求は既存SRVを優先し、未ロード時はCPU decodeと通常フレームCommandListへのuploadでGPU Waitを避ける。

- [ ] 寸法テスト追加：512x512→128x128、1920x1080→128x72、縦長、寸法0はFallback。
- [ ] texturemanagerとthumbnailrendererへ待機しないロード経路を追加。GPU完了までUpload保持。Cube/Array/Depth/不適合FormatはFallback。
- [ ] editorassets.cppをBeginTableと行単位ImGuiListClipperへ変更。可視セルのみRequestし、選択Assetを優先。セル全体を選択/D&D/Context Menu対象とし、既存各操作を維持。
- [ ] サイズ設定とRefresh操作を追加。初期128、サムネイルは中央配置してアスペクト比を保持。
- [ ] PNG/JPG/DDS、Rename/Move/Delete、100/1000件、高速Scroll/Folder切替を確認。既存ASSET_PATHと型付きPayloadの互換性を確認。

### Task 4: Material Preview

**Interfaces:** ThumbnailPreviewContextはWorldへEntityを登録しない。RenderMaterial(AssetHandle, target, commands)は既存Material Binding/Shader/PSOを利用。固定Sphere/Camera/Directional/Ambientと背景を持つ。Manager設定maxGenerationsPerFrameで描画数を制限する。

- [ ] Frame Budgetテスト追加：1なら一件、2なら二件、0なら生成停止。Dirtyと選択優先を確認。
- [ ] materialpreview.cppのGeometry/Pipeline/Bindingを共通Contextへ抽出し、既存Orbit/Shape/Lighting操作を保持。
- [ ] ThumbnailRendererに独立Target/Depth生成とShaderResource→RenderTarget→ShaderResourceのBarrierを追加。生成結果は当該Keyが有効な場合のみReadyへ。
- [ ] Material保存/変更通知をMarkDirtyへ接続。BaseColor、Texture、Roughness、Metallic、透明Material、失敗Fallbackを確認。
- [ ] scripts/test-material-assets.ps1と既存Material検証を実行。Debug Layerと画像比較で既存Preview回帰を確認。

### Task 5: Mesh Preview

**Interfaces:** RenderMesh(AssetHandle, target, commands)。既存ModelManager/StaticModel経由でGeometryを取得し、Bounds中心をCamera注視点に使用。Meshの既存Material、欠損時はEditor標準Materialを利用。

- [ ] Auto FitのCPUテスト追加：小型/巨大/細長い/原点からずれたBounds、ゼロ寸法、非有限値。有限なCameraと全Bounds包含を検証。
- [ ] Bounds半径とPerspective FOVから距離を求め、near/farにも余裕を付ける。空GeometryはFailed/Fallback。
- [ ] Mesh要求を共通Queue/Budgetに接続し、再ImportでDirtyにする。
- [ ] 代表Mesh画像とDebug Layerを確認し、Material情報あり/なしの両経路を検証。

### Task 6: 依存更新・LRU・Disk Cache

**Interfaces:** Texture/Shader→MaterialのReverse Dependency。Cache KeyにGenerator Version=1と内容/Import設定のRevisionを含む。Cache保存はAssets外のEditor CacheへDDSとKeyを原子的に保存する。

- [ ] CPUテスト追加：Texture変更で参照MaterialだけDirty、Shader変更、Import設定変更、Version変更、LRU排出、可視参照保護、破損/途中保存Cacheの拒否。
- [ ] RegistryとMaterialManagerの通知、外部変更検出を追加。大量全ファイルを毎Frame走査しない。必要な更新をFrame Budget内で処理。
- [ ] 所有GPU Memoryを計上し、上限超過でLRU排出。借用TextureのMemoryは二重計上せず、その所有者の寿命と協調する。Pending retireも計測する。
- [ ] DirectXTexでDDS保存/再利用。GPU読戻しはFenceポーリング後にCPU処理し、UIからWaitしない。破損Cacheは再生成。
- [ ] 連続Folder切替、Memory圧迫、再起動後Cache hit、依存更新を実機確認。

### Task 7: 統合検証と報告

- [ ] DirectX12.vcxproj/.filtersへ追加ファイルを登録し、Debug/Releaseビルドを実行。新CPUテストと既存Materialテストを両構成で実行。
- [ ] 原本34節の各ケースと既存Browser操作をチェックし、35節のResource State/Heap/解放済み参照/Fence/BarrierをDebug Layerで確認。利用可能ならPIX Passも確認。
- [ ] Descriptor数、所有/借用/解放待ちMemory、Queue長、生成数を計測し、1000件ScrollとFolder往復で増加が収束することを記録。
- [ ] 可読性、状態遷移、例外経路、所有権とShader/Material回帰を最終レビュー。
- [ ] docs/asset-thumbnail-validation.mdに実施結果・証拠・未実施を記録。未実施のGPU操作をCPUテストやビルド成功で代替したと報告しない。

## Execution

このセッションで本体が順に実装する方法を推奨。各タスクは同じHeap/Fence/Preview契約へ依存するため、変更と検証を一貫して進める。Subagent方式をユーザーが選んだ場合はタスク別実装とレビューへ切り替える。統合作業・Push・PRは別途ユーザー指示に従う。
