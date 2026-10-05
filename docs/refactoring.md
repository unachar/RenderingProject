# コードの配置と責務

対象ブランチは `dev_dlss&raytracing`、基準コミットは `69efcf7b2b82ea27f5588ae6dfd749f7d8c1979b`。
動作維持を優先して、既存の処理を責務ごとに移動し、呼び出し元を更新した。

## 最初に読む場所

| 調べたいこと | ファイル／クラス |
|---|---|
| アプリケーション・フレームの管理 | `source/main.cpp`、`source/core/world.*` |
| システム登録、更新条件、各システムへの描画通知 | `source/systems/systemmanager.*` |
| 一フレームの描画パスと依存関係 | `source/rendering/framepipeline.*` |
| GPUの初期化・終了・待機・デバイス診断 | `source/rendering/graphicsdevice.*` |
| リサイズ・HDR・解像度スケールの保留設定 | `source/rendering/renderconfiguration.*` |
| 描画先の作成とSRV／RTVの取得 | `source/rendering/rendertargets.*` |
| フレーム開始・終了・バックバッファ・Present | `source/rendering/renderframe.*` |
| 影、シーン、透明物、Velocity、遮蔽のパス設定 | `source/rendering/scenepasses.*` |
| 後処理、AA、アップスケールの適用 | `source/rendering/postprocesspass.*` |
| ライト・シャドウ定数とシャドウキャッシュ | `source/rendering/lightingresources.*` |
| 材質定数とバッチ用ハッシュ | `source/rendering/materialbindings.*` |
| フレーム別CBVと一時定数の割当 | `source/rendering/frameconstants.*` |
| スプライト・基本図形の頂点生成 | `source/rendering/primitivegeometry.*` |
| シェーダー読込と共通パイプライン | `source/rendering/shaderpipelines.*`、`psomanager.*` |

## 旧ファイルとの対応

| 旧ファイル／クラス | 分離後 |
|---|---|
| `RendererCore` | `GraphicsDevice`、`RenderConfiguration`、`graphicslog.*` |
| `RendererDraw` | `RenderTargets`、`RenderFrame`、`ScenePasses`、`PostProcessPass` |
| `RendererResource` | `LightingResources`、`MaterialBindings`、`FrameConstants`、`PrimitiveGeometry` |
| `rendererResource` | `ShaderDescription`。フィールドと意味は従来どおり |
| `RendererShader` | `ShaderPipelines` |
| `rendererutils.*` | `shaderpaths.*` |
| `renderer.cpp` | `rendererstate.cpp` |
| `rendererstate.h` の列挙型・頂点・定数型 | `rendertypes.h` |
| `SystemManager::RenderFlow` | `FramePipeline::Execute` への委譲 |

`RenderGraph` は依存解決とバリアを扱い、`FramePipeline` はアプリケーションのパスを組み立てる。
既存の Shadow → Opaque／遮蔽第2フェーズ → Velocity → Lighting／PostProcess → Transparent／Overlay → AA の順と依存関係を保っている。
SystemManagerのシステム登録順、更新順、Play時だけ更新する条件はそのまま維持した。

## モデルとエディター

`AnimationModelResource` のクラス・インスタンス状態・公開APIは維持し、実装ファイルを分割した。

| ファイル | 内容 |
|---|---|
| `models/animationmodel.cpp` | モデル読込、PMX、材質・テクスチャ |
| `models/animationcache.cpp` | VMD／PMXとアニメーションキャッシュ |
| `models/animationpose.cpp` | サンプリング、ボーン姿勢、IK |
| `models/animationmorph.cpp` | モーフ、PMX順序付き変換 |
| `models/animationskinning.cpp` | GPUスキニングのバッファ・ディスパッチ |
| `models/animationmath.h` | 元から共通して使う行列検証等の小さな関数 |

`ImGuiManager` も状態とAPIを維持し、`editor/` 以下へ移動した。

| ファイル | 内容 |
|---|---|
| `imguimanager.cpp` | 初期化、統合、メニュー、Dock、スタイル |
| `editorscene.cpp` | 階層、シーン操作、ギズモ、選択 |
| `editorinspectors.cpp` | Component・材質・ライトの編集 |
| `editorassets.cpp` | アセットブラウザー、読込、ファイル操作 |
| `editorsettings.cpp` | プロジェクト・物理設定と保存読込 |
| `editorhistory.cpp` | Undo／Redo、スナップショット |
| `editordebugwindows.cpp` | 性能・描画バッファ・デバッグ表示 |
| `editorwidgets.*` | 元から共有されるUI・変換補助 |

その他は `core`、`ecs`、`systems`、`scene`、`entities`、`assets`、`physics` へ配置した。
`main`、PCH、リソースファイルと既存のD3D補助ヘッダーは `source/` 直下にある。
Visual Studioの登録、フィルター、include検索パスを配置に合わせて更新した。

## 状態と寿命

| 状態 | 保存場所 | 主な操作窓口 |
|---|---|---|
| デバイス、コマンド、Fence、SwapChain、フレーム別バッファ | `RendererState` の既存staticメンバー | `GraphicsDevice`、`RenderFrame` |
| リサイズと保留設定 | `RendererState` の既存staticメンバー | `RenderConfiguration` |
| シーン・後処理・AA・エディター描画先の配列 | `RenderTargets` のstatic配列 | `RenderTargets`、派生パスクラス |
| ライト・材質・一時CBのバッファ | `RendererState` の既存staticメンバー | 各リソースクラス |
| シャドウキャッシュ | `lightingresources.cpp` 内部 | `LightingResources` |
| アニメーションとエディターの状態 | 元のクラスのメンバー | 分割した実装ファイル |

共有GPU状態を各クラスの独立所有へ変更する作業は未実施。既存の状態・解放順と描画先の重複メンバーを保っている。
したがって今回の分離は操作責務と配置の整理であり、GPU所有関係の全面的な再設計は完了していない。
外部ライブラリ、シェーダー、アセット、既存の計算・ソート・キャッシュ方針は変更していない。

## 書式と分岐

自作コードの三項演算子は `if` へ変更した。真偽値を0／1にするだけの箇所は明示的な型変換を使う。
参照を選ぶ箇所は元のメンバー／既存の戻り値へポインターで接続し、コピーや一時オブジェクトへの参照を避ける。
両分岐の評価を先に行わず、選択された処理だけを実行する。
既存のインデント・括弧・命名規則に合わせ、第三者由来の `d3dx12.h` と `dxccompiler.h` は変更していない。

[テスト設計](refactoring-test-cases.md)と[検証結果](refactoring-validation.md)を参照。
