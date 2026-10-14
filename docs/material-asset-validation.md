# Material Asset 実装・検証記録

## 操作

1. Content BrowserのCreateまたは背景メニューからMaterialを作成する。
2. `.material` をダブルクリックしてMaterial Editorを開く。
3. BaseColor、Metallic、Roughness、Normal Scale、AO、Emission、Surface、Cull、Depth Writeを編集する。Textureは対応する欄へD&Dする。
4. PreviewのSphere / Cube / Plane、回転、ズーム、リセット、照明で表示を確認する。
5. MaterialをViewportのRenderableまたはInspectorのMaterial SlotへD&Dする。複数SubMeshのViewport DropではSlotを選ぶ。
6. Material EditorのSave / Ctrl+SでAssetを保存する。Revertは未保存編集を破棄する。閉じる／別Assetへ切り替える際はSave / Discard / Cancelを選ぶ。
7. Sceneを保存するとGUID参照を保存する。Content BrowserのRename / Moveは `.meta` も移動し、Duplicateは新しいGUIDを発行する。

## 検証方法

`scripts/test-material-assets.ps1` はSerializer、Registry、異常入力、保存失敗、Move、Duplicate、ID衝突を検証する。

`scripts/validate-materials.ps1 -Configuration Debug` または `Release` はビルド後、専用の一時Assetで共有Runtime、GPU共有Index、Undo / Redo、Scene保存・復元、欠損Fallback、SubMesh、実ImGuiのD&D受信、Ctrl+S、Revertを検証する。全6枚のGBufferをGPULOD ON/OFFで画素比較し、MRのR/G、AO強度、Emission、NormalMapの描画効果も確認する。変更時のPSO作成数とGPU Instance転送量を検査する。

PreviewとSceneのPNGは `build/material-tests/`、ビルド・実行ログも同ディレクトリと `build/material-build-*.log` に出力する。生成したFixtureだけを終了時に削除する。

## 適用上の制約

この環境のMSVCでは既存設定のPDBが不整合になるため、検証Scriptは `/Z7` を適用する。Release増分リンクでLNK1103が発生した場合は `-Rebuild` を指定する。全再ビルドの成功を確認済み。

Normal Mapの接線基底は既存Vertex形式を維持するためUV微分から計算する。仕様のVertex Tangent属性追加は実施していない。MikkTSpaceで焼いたNormal Mapとの完全一致は未検証。

Compute GBufferのVisibility圧縮形式はEmission / AOに対応しないため、Material Asset使用時はRaster GBufferへ切り替える。透過等のGPUバッチ非対応Stateも通常Rendererで同じRuntimeを使う。

自動UIテストは実際のImGui受信側へ入力・Payloadを注入する。ネイティブのContent Browserからマウスを持ってドラッグ開始する操作、閉じる確認の各選択肢、全モデル形式での外観、Mesh Shader経路の画素比較は別途手動確認が必要。

## 最終実行結果

2026-10-14の実行結果：

| 検証 | 結果 |
|---|---|
| Material CPUテスト（Debug / Release） | PASS |
| C++ / HLSL Debugビルド | PASS |
| C++ / HLSL Release全再ビルド | PASS |
| Material統合テスト Debug | 69 PASS / 0 FAIL、D3D12 errors = 0 |
| Material統合テスト Release | 69 PASS / 0 FAIL |
| GPULOD ON/OFF | 全6 GBufferの画素が一致 |
| パラメータ編集 | graphics PSO作成なし、GPU Entity Instance転送なし |
| 実ImGuiの受信操作 | Viewport / Inspector / Normal Texture D&D、Ctrl+S、RevertがPASS |

最終追加レビューで確実な新規不具合なし。レビューで発見した欠損Assetの破棄、混在SubMeshの描画Pass、旧モデルのDefault Material、Sceneの再利用ID誤適用、Masked Texture復旧時のPipeline更新を修正した。

最初のUIテストでDrop先を覆っていたスモークWindowを分離した。Viewport検出にはMeshComponentの生成済み境界も使用する。テストがProject設定をロードしないよう隔離した。

Release増分リンクのLNK1103は最終全再ビルドで解消。再ビルド途中のShaderを使った既存スモークテストは初期化失敗したため、完成した成果物で再実行する。
