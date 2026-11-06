# Asset Browserサムネイル — 承認済み設計の実装要約

要求の原本は、このチャットでユーザーが提示した「Asset Browser サムネイル機能 仕様書」全37節。以下は実装計画用の要約であり、原本の要件を縮小しない。

## 対象と範囲

- Repository: unachar/RenderingProject、branch: dev_main。
- 基準: 0d72d8f27d0977c2817cd77b6e4c4e4b53749f35。ユーザーが2026-10-14に最新HEADでの作業を承認。
- 原本の900adb565e607863504ed4f7c6e9d5f41692c88aは2fb9d883のtree ID。
- Phase 1〜4の全項目。Disk Cacheも含む。Phase 5はThumbnail SizeとRefresh。
- Scene、Prefab、Audio、Animation、動画、リアルタイムアニメーション、起動時全件生成は対象外。

## 必須構造と動作

Asset BrowserはGUID相当の既存AssetHandleでRequestする。生成・Descriptor確保・Resource寿命・Queue・Cacheは専用システムが所有する。状態はUnloaded、Queued、Generating、Ready、Dirty、Failed。

Textureは既存2D Resource/SRVを再利用し、アスペクト比を維持する。未対応形式、生成中、失敗時は既存アイコンへ戻す。Materialは独立Preview ContextのSphere、固定Perspective Camera、固定Directional/Ambient Light、中立背景で描画する。MeshはBoundsからAuto Fitし、Material情報がなければ標準Materialを使う。

初期UIサイズ128x128を設定として持ち、BeginTableと行単位Clipperを使う。画面外全件Requestは禁止。選択、可視、直前直後、背景の順に優先し、Material/Mesh生成件数は設定可能なFrame Budgetへ制限する。Request/UI表示からGPU Waitを行わない。

既存ImGui DX12 BackendのShader Visible Heapを使う。GPU使用中のResourceとDescriptorはFence完了まで保持し、Raw Descriptorだけで所有権を管理しない。LRUとMemory上限を持つ。Cache KeyはHandle、Asset Revision、Dependency Revision、Generator Version。Disk CacheはAssets外に保存する。

Inspector保存、Texture/Material/Meshの更新・再Import・Import設定変更、Shader再Compileで無効化し、Texture/ShaderからMaterialへの依存も追跡する。Rename/MoveはIdentityを維持し、Deleteは安全に解放する。

既存選択、Double Click、D&D、Rename、Context Menu、Inspector、Folder移動、作成、削除を維持する。失敗はログとFallbackで扱い、Editorをクラッシュさせない。

## 検証

原本34節のTexture/Material/Mesh/UIテストと35節のDX12 Debug確認を対象とする。CPUテスト、Debug/Releaseビルド、既存Material回帰、実機操作、GPU寿命・Heap・Barrier検証を区別する。未実施は成功と報告しない。PIXは可能な場合に実施する。
