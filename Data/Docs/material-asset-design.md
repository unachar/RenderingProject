# Material Asset / Editor 統合設計

## 目的と完成条件

ユーザー提供「RenderingProject マテリアルアセット・D&D・マテリアルエディタ実装仕様書」を要求仕様とする。対象は本リポジトリのC++ / DirectX 12 / ImGuiエディタ。全6フェーズ、MAT-01〜20、GPULOD回帰比較、およびDefinition of Doneを維持する。

共有Material Assetを編集すると、参照する全エンティティとプレビューに反映される。永続保存はSave時のみ。通常描画、GPULOD、Indirect、Mesh Shader、Previewが同一RuntimeとMaterial Bindingを利用する。

## 確認済みの既存構造

- `source/ecs/componentmanager.h`: MaterialComponentはエンティティ単位の値とテクスチャパスを保持する。既存のToon・部位別設定も存在する。
- `source/editor/editorassets.cpp`: Asset Browserとファイル作成UIがある。既存D&DはASSET_PATH。
- `source/editor/editorinspectors.cpp`: Material Editorは選択エンティティのMaterial Inspectorを表示する。
- `source/editor/editorhistory.cpp`: EntitySnapshotを使うUndoの基盤がある。
- `source/editor/projectmanager.cpp`: シーン永続化の統合対象。
- `source/rendering/materialbindings.h/.cpp`: 通常描画用Material設定、GPU用レコード生成、バッチハッシュの既存接続点。
- `source/rendering/gpuscene/gpuscenetypes.h`: GPUMaterialDataにはBaseColor、NormalTextureIndex、既存シェーダーパラメータがある。GPU構造変更はHLSLと同時に行う。

## 採用方針

### アセットと識別子

64bit AssetHandleと型を持つRegistryを追加する。MaterialとTextureの識別子をメタデータへ保存し、Rename / Moveではメタデータも移動する。Duplicateは新しい識別子を発行する。パスのハッシュを永続識別子にしない。Registry再構築、重複ID、不正形式、欠損参照を検証する。

MaterialSerializerはバージョン付き.materialを読み書きする。入力を検証し、保存失敗で既存ファイルや保存済み状態を失わないようにする。PBR値、5つのTexture Slot、Render Stateを保持する。

### Runtimeと互換性

MaterialManagerはHandleごとに共有Runtimeを保持する。保存済みAssetと編集状態は分離し、編集状態からRuntimeを更新する。パラメータ変更は定数更新、Texture変更はBinding更新、Render State変更は必要なPipeline更新に分ける。

既存MaterialComponentにはSubMeshごとのHandle参照を追加する。参照のない旧シーンは既存データで描画できる互換経路を残す。明示的に割り当てたHandleが欠損している場合はDefault Runtimeを使い、旧エンティティ値に戻して欠損を隠さない。既存Toon・部位別設定を不用意に削除しない。

MaterialBindingsをRuntimeの共通接続点として拡張し、GPU Scene・通常描画・Mesh Shader・Indirectも同じ値とTexture解決結果を利用する。GPULODで変化するのはGeometryだけとする。GPUリソースの交換・解放はフレームとFenceに従う。

### Editor操作

MaterialとTextureのD&DにはHandleとAssetTypeを含む型付きPayloadを用いる。Inspectorは対象Slotへ割り当てる。ViewportはRenderableだけを受け付け、SubMeshを特定できなければ複数Slotの選択Popupを出す。Rendererを自動追加せず、全Slotを一括変更しない。

割り当てと編集はUndo / Redoへ統合する。スライダー操作は連続反映しつつ一操作として履歴化する。Material EditorはAsset単位で開き、Dirty、Save、Revert、Ctrl+S、閉じる際のSave / Discard / Cancel、エラー表示を備える。

### Preview

Preview Scene、Camera、Lighting、RenderTargetを追加し、Sphere / Cube / Planeを表示する。Sceneと同じPBR ShaderとRuntime Bindingを利用し、簡易専用Shaderを作らない。非表示時は不要な描画を止める。カメラ操作とリセットを提供する。

## 実装順序と検証

1. Asset / Registry / Serializer: 作成・保存・再ロード・複製・Rename / Move・不正入力をCPUテスト。
2. Runtime / Renderer / Scene: 共有参照、Slot、旧シーン互換、欠損Fallback、GPUレコードを検証。
3. D&D / Undo: ViewportとInspector、拒否対象、複数Slot、Undo / Redoを検証。
4. Material Editor: PBRとTexture、保存状態、リアルタイム更新、Revert、閉じる確認を検証。
5. Preview: 同じRuntimeとShaderによる3種のMesh、カメラ操作、非表示時停止を検証。
6. 統合検証: Debug / Releaseビルド、必要なHLSLコンパイル、MAT-01〜20、同一カメラでGPULOD ON/OFFの画像比較。Constant変更でTexture Reload・Shader Compile・PSO再生成が起きないことを確認。

各フェーズは全機能完成に向けた実装単位であり、途中の状態を完成として扱わない。GUI実操作とGPU比較が未実施なら、ビルド成功だけで受け入れ条件合格としない。

## 実装前に追加調査する点

SubMesh識別とPicking精度、既存Undoの拡張形式、Sceneファイル形式、MRテクスチャのチャンネル規則、既存透明描画とRender State、PreviewのRenderTarget接続、各GPU描画経路のTexture Bindingを対象箇所に絞って確認する。

現在`shader/hlsl/DeferredPS.hlsl`にユーザーの未コミット変更がある。実装時は差分を確認して保持する。

## 状態

製品コードへ統合済み。最終検証結果と操作手順は `material-asset-validation.md` に記録する。

## 既存描画構造への適用

- Opaque / Masked / Transparentを保存・編集できる。Transparent、Front Cull、Depth Write無効などGPUバッチと両立しない状態は、同じRuntimeを使う通常描画へ戻す。
- EmissionとAO用のGBufferを追加した。VisibilityのCompute GBuffer経路はこの情報を格納できないため、Material Assetを割り当てたシーンでは通常のRaster GBufferを使う。ユーザー設定は保持し、参照を外すと元の経路へ戻る。
- 既存の48-byte VertexとLODキャッシュ形式を保ち、Normal Map用TBNは各描画面のPosition / UV微分から構成する。UVの反転方向も保持する。仕様のVertex Tangent属性を追加する方式とは異なるため、保存された接線との一致やMikkTSpaceとの一致は保証しない。
- SceneのMaterial参照は `Save/scene_materials.json` にGUIDで保存する。対象識別に安定IDを使い、旧オブジェクトとの照合は名前とモデル／形状情報を使う。数値Entity IDの再利用で別オブジェクトへ適用しない。
