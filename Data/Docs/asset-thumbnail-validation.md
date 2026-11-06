# Asset Browserサムネイル — 実装と検証

## 実装範囲

`dev_main` の `0d72d8f2` を土台に、Phase 1〜4とThumbnail Size / Refreshを実装した。

- `AssetThumbnailManager`：既存AssetHandleをキーに要求、状態、優先度、変更検出、Cache、予算を管理。
- `ThumbnailRenderer`：CPUワーカーで画像を読む／Meshを解析し、通常のフレームCommandListでアップロードと描画を行う。
- `ThumbnailPreviewContext`：Worldと独立したCamera、Light、Sphere、Target、Depth、Material定数を保持。既存PBR経路を共有。
- Textureは読み込み済みのResource/SRVを借用する。未ロード時は縮小画像を生成。Materialの借用TextureにもGPU Resourceの複製を作らず、必要なSRVだけを作る。同じ要求内の重複Texture参照は共有する。
- Meshはノード変換、BoundsからのCamera Fit、複数Material、外部／埋め込みTextureに対応。
- Gridは128を初期値とする設定、BeginTable、行単位Clipper、選択／可視アセットの要求、既存アイコンと失敗マーカーを使用。
- ディレクトリ一覧は再利用し、切替・検索変更・定期確認で更新する。
- 既存Shader Visible Heap内の有限512スロットを使う。ResourceとDescriptorはGPU使用完了まで保持する。生成時の失敗経路も保持対象。
- LRUとGPU Memory上限、設定可能な生成件数、容量不足時の再試行、Upload／Readbackの解放を実装。
- DDS Cacheは `EditorCache/Thumbnails/<16桁GUID>.dds`。Source、依存、サイズ、Generator Versionを判定し、原子的に保存。Assetsへ生成画像を置かない。
- Material編集／保存、Texture変更、モデルが実際に読み込んだMTL・glTF外部Buffer・Texture、Shaderの更新で無効化する。RefreshはDisk Cacheを迂回して再生成する。

## 使用方法

Asset Browserのサイズ設定で32〜256へ変更できる。「サムネイル更新」は要求済みCacheを無効化する。生成中と未対応形式では従来アイコンを表示する。失敗は `!` を重ねる。

生成件数、Memory上限、Disk Cacheの有効化は `AssetThumbnailManager::Settings()` の設定値。初期値は2件／Frame、64 MiB、Disk Cache有効。

## 確認済み

| 検証 | 結果・証拠 |
|---|---|
| Thumbnail CPUテスト Debug / Release | 合格。重複要求、優先順、古い結果拒否、削除／再作成、失敗再試行、比率、Camera Fit、Descriptor Fence、Cacheファイル名、容量待ちの復帰 |
| Mesh I/O CPUテスト Debug / Release | 合格。実Assimp DLLへのC形式コールバック、任意名MTL、glTF外部 `.bin` の追跡 |
| 既存Material CPUテスト Debug / Release | 合格 |
| Debugビルド | 合格。PDBマネージャー不整合C1902を避ける既存検証手順の `/Z7` を使用 |
| Debug Thumbnail実GPUテスト | 合格。PNG/JPG/DDS/TGA、縦横比、既存Resource/SRV借用、Sphere、原点からずれた複数Material Mesh、Material変更、Texture依存変更、Rename、Disk Cache再利用、外部MTL変更、セル選択、1000件Scroll、サイズ変更 |
| Debug Thumbnail Debug Layer | `D3D12 errors = 0`、`Thumbnail validation failures = 0` |
| 既存Material Debug実GPU回帰 | 合格。Preview Sphere/Cube/Plane、Lighting、Orbit、透明、Texture、Viewport / Inspector / Material Editor D&D、Undo/Redo、保存／Revert、GPULOD Material比較 |
| 既存Material Debug Layer | `D3D12 errors = 0`、`Material validation failures = 0` |
| 既存Material Release実GPU回帰 | 合格。`Material validation failures = 0`。ReleaseのDebug Layerは無効 |
| Releaseビルド／Thumbnail実GPU | 合格。通常のRelease `/O2` を使用。Debug LayerはReleaseでは無効なので、その確認はDebugの結果を使う |

証拠は `build/thumbnail-tests/validation-Debug.log`、`validation-Release.log`、`build/material-tests/validation-Debug.log`、`validation-Release.log` とビルドログに保存した。画像は `build/thumbnail-tests/browser.png`、`material.png`、`mesh.png`。Browser画像は初回生成時の撮影なので、一部に生成待ちアイコンを含む。

1000件Scrollテストは1000件の未対応ファイルと8件の対象／検証ファイルを含むフォルダを使い、画面外のUIを作らないことを確認した。1000件すべてが大型Texture／Meshである負荷試験の代わりにはしない。

## 発見して修正した問題

- GUID表示の `guid:` をファイル名へ流用するとWindowsの特殊なストリーム扱いになる。16桁の識別子だけを使用する。
- AssimpのC++カスタムI/OはDebug / ReleaseのSTL ABI差でクラッシュした。ダンプの `Assimp::IOSystem::PushDirectory` から原因を特定し、C形式 `aiFileIO` に変更。Debug CRTの直接Importテストで再発防止。
- Shader Visible HeapはDescriptorコピーの読み元にできない。借用ResourceのSRVを作成し、GPU Resourceは共有する。
- Reviewer指摘の、任意名MTL／glTF外部Bufferの変更検出漏れと一時的容量不足からの復帰を修正した。
- MSVC 14.51.36231の `/GL` と `/Z7` の組み合わせで複数オブジェクトにLNK1103が再現した。全体再ビルドでも切り分け、該当バージョンのRelease x64だけ全体最適化を外す回避設定を追加した。通常の `/O2` とデバッグ情報は維持する。他のツールチェーンには適用しない。全体最適化を外した前後のゲーム性能比較は未実施。

## 未確認の範囲

- PIXでのPass確認。
- 1000件すべてが対応Assetである連続Scroll、実ユーザープロジェクトでの長時間Memory計測。
- 全形式／全Import設定の組み合わせ、全Meshの規模・細長さ・Submesh数。
- Browserから始めるD&D、Context Menu、作成／削除の全手動操作。既存D&Dの受け側とMaterial Editorは回帰テストで確認済み。

Frame BudgetとMemory上限による制御、Fence付き寿命管理は実装済みだが、この未確認範囲まで実測済みとは扱わない。

## 再実行

```powershell
./scripts/test-thumbnails.ps1 -Configuration Debug
./scripts/test-thumbnails.ps1 -Configuration Release
./scripts/test-thumbnail-mesh-io.ps1 -Configuration Debug
./scripts/test-thumbnail-mesh-io.ps1 -Configuration Release
./scripts/validate-thumbnails.ps1 -Configuration Debug
./scripts/validate-thumbnails.ps1 -Configuration Release
./scripts/validate-materials.ps1 -SkipBuild -Configuration Debug
```

実GPUスクリプトは事前ビルドが必要。検証用アプリはHiddenで起動し、所有する一時Fixtureを検証後に削除する。検証用画像の取得はGPU完了を待つが、製品のThumbnail Request／Generate／Cache SaveはGPU Waitを追加しない。
