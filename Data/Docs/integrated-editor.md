# 統合エディター実装記録

## 対象と検証方針

2026-11-02。対象は RenderingProject の `dev_main` と同一コミット
`6f2fcb21` から始まる既存 `dev_ui` 作業ツリー。ユーザーの指定に従い、
テストコードの追加・利用は行わない。本体の Debug / Release ビルドと
実機の画面・操作・再起動で確認する。未確認の項目を完了扱いにしない。

## 確認した接続先

- Dear ImGui: `External/ImGUI/imgui.h` の 1.92.8 WIP / 19272、Docking 対応。
  Win32 と DX12 バックエンド、共有 SRV heap、既存の日本語フォント設定を使う。
- 現在のエディター: `source/editor/imguimanager.cpp` と機能別 cpp。
  既存 DockSpace は一つで、表示フラグは `editorsettings.cpp` に保存される。
- マテリアル: `MaterialAssetData` / `MaterialManager` / `editormaterialassets.cpp`。
  編集・保存・破棄・モデルへのスロット割り当てを既存経路に接続する。
- プレビュー: `MaterialPreview` の既存 Sphere / Cube / Plane と
  `EditorDescriptors` の割り当て・遅延解放を再利用する。
- コンパイラー: `source/dxccompiler.h` と `source/dxcshadercache.h`。
  新しい DXC インスタンス管理・DXIL キャッシュを重複実装しない。
- Procedural Studio、Animation Preview、Sequencer は既存の独立ウィンドウ。
  RenderGraph、GBuffer、描画概要、パフォーマンス、GPU 関連デバッグも存在する。
- ビルド: `DirectX12.sln` / `DirectX12.vcxproj`、v145、x64。
  外部ライブラリは主に `External` 以下、NuGet は `packages` 以下。
- ImGuizmo は既存で使用中。`External/imgui-node-editor` は存在するが、
  調査時点でルートのライブラリソースとライセンスが不足し、vcxproj 登録もない。
  フォルダーの存在を統合済みの証拠としない。

## 設計

主操作は作業目的に応じた Workspace 選択、領域内 Editor Type の変更、
Material Graph 編集と結果のモデル適用。参考画像の高密度な領域分割を使い、
上部を小さな Workspace 列、中央を主作業、左右を一覧・プロパティ、下部を
タイムライン・コンテンツ・ログとする。ダークグレー、薄い境界、控えめな青、
日本語を読める一貫した文字サイズをテーマに集約する。

Workspace、ImGui Docking 配置、Editor Area、Editor Type、各ツールの編集データを
分離する。Docking の置き換えや既存レンダラーの全面改修は行わない。
ツールの見出しと永続 ID は分離し、名称変更でも配置を維持する。
未実装機能のボタンや架空の GPU 計測値は置かない。

### 採用する構成

既存 ImGui Docking + Workspace/Area 管理層 + imgui-node-editor を使う。
Docking だけでは領域の機能切り替えや Workspace ごとの状態を表せず、
独自の分割 UI 全体の実装では既存操作と入力経路の変更が大きくなるため。
imgui-node-editor と imnodes の両方を製品に追加しない。

ノードライブラリの公式資料:

- https://github.com/thedmd/imgui-node-editor
- https://github.com/Nelarius/imnodes

API・同梱版互換性、グループ・画像描画・選択・パン/ズーム、ビルド容易性、
保守履歴を比較し、バージョン固定とライセンス保持を伴って導入する。
描画負荷の優劣は実測前に断定しない。

### 保存と寿命

`Save/editor_workspaces.json` にバージョン、Workspace の安定 ID・名前・順番・
プリセット・各表示状態・ImGui ini スナップショット・アクティブ ID を保存する。
既存 `WriteAssetFile` で同一ディレクトリの一時ファイルから置換し、保存失敗は
画面に表示する。切り替えは UI 提出前に旧状態を取得し、旧 Docking 設定を
クリアして新しいスナップショットを復元する。描画中に配置を破棄しない。
従来の `Save/editor_layout.cfg` は初回だけ取り込む。

グラフの保存は既存マテリアルの ID・スロット契約を維持する。型付きモデルと
コマンド履歴を UI から分離し、接続時の型・循環検証、入力依存の式生成、
ソース位置とノード ID の対応、構造とパラメーターのリビジョンを管理する。
生成したシェーダーは成功時だけ描画へ反映し、失敗時は最後の有効な PSO を保つ。
GPU リソースとディスクリプターは既存 fence の完了後に解放する。

## 実装・受け入れ範囲

| Phase | 必要な成果 | 現在の状態 |
| --- | --- | --- |
| 1 | 調査、ライブラリ比較・固定、Registry、Workspace、Layout | 基盤とライブラリ固定・製品ビルドへの登録を実装。推奨ライブラリの追加判断は継続 |
| 2 | 上部タブ、自由 Docking、開閉・再表示、領域 Type、保存復元 | 既存機能のType切替、タブ、表示、保存復元、最大化を接続。要求14種の専用エディターは未完了 |
| 3 | ノードライブラリ統合、Graph Model、編集・検索・グループ・履歴・保存 | 固定ライブラリのコンパイルとGraph Modelを追加。ノードUIと履歴の接続は未実装 |
| 4 | 型・循環検証、最適化、HLSL、DXC、Material Output と描画への反映 | 未実装 |
| 5 | ノード画像、offscreen 描画、表示ノードだけの更新、非同期・キャッシュ | 未実装 |
| 6 | Debug / Release、全操作、再起動復元、描画回帰、GPU 解放・同期 | 未完了 |

初期 Workspace は Layout / Shading / Animation / Sequencer / Rendering / Procedural。
追加・複製・名前変更・順番変更・削除・初期配置への復元を備える。
各プリセットは現時点で既存の実機能を配置し、新規ノード編集・Dope Sheet・
専用 GPU Profiler 等が完成したと偽って表示しない。

受け入れでは上記に加え、14 種の Editor Type、20 種の基本ノード、既存の
Base Color / Metallic / Roughness / Normal / Emissive / Opacity 等の接続、
64/128/256 のプレビューと更新モード、Undo/Redo・コピー・貼り付け、
日本語・フォーカス・リサイズ・代表 DPI を確認する。既存 Deferred / DXR /
TSR / ReSTIR GI / MegaLights / RenderGraph / Virtual Geometry / Animation /
IK / DQS / Bullet / Procedural / Import-Export の回帰確認も残る。

## ビルド記録

最初の並列 MSBuild は shader 子プロジェクトの直後に進まなかったため中断。
並列 shader ジョブを無効化した実ビルドへ切り替えた。既存の開発記録と同様に
`/ZI` の PCH 作成で進まなかったため、ビルドプロセスだけ `_CL_=/Z7` を指定。
プロジェクトのデバッグ情報設定は変更していない。結果は後続の検証で追記する。

### 確認済みの結果

- 最新の Debug x64 / Release x64 本体ビルド成功。
  `build/workspace-debug-z7.log`、`build/workspace-release.log`。
- リンクで `LNK1101: MSPDB140.DLL バージョンが正しくありません` が発生したため、
  `build/workspace-local.targets` を `ForceImportAfterCppTargets` で明示指定し、
  確認用ビルドだけ PDB 出力・増分リンクを無効にした。通常プロジェクト設定は維持。
  Release には既存と外部ソース由来の警告が残る。警告ゼロとは報告しない。
- `git diff --check` 成功。テストコードの追加・実行はしていない。
- 通常のデスクトップ環境で起動して実画面を確認。初回は既存 Mesh Shader の
  初期化に数分かかり、その後は既存シーンと GBuffer を描画できた。
- Shading / Rendering の異なるレイアウト、Material領域→Scene Outliner切替、
  Workspace往復での種類・配置保持、領域を閉じてWindowメニューから再表示、
  分割幅の変更を実機操作で確認。
- 通常終了は exit 0。再起動でアクティブShading、変更した種類、左右の分割幅を復元。
  複数同種領域の修正後操作、完全なDock解除・結合、DPI、すべての既存描画機能の
  回帰はまだ確認途中。最大化とCRUDの後続確認結果は下記。

- 続く実機操作でProcedural領域の最大化、旧Texture Viewerの同時非表示、
  `Ctrl+Space`による元配置への復帰を確認。
- Workspace追加、日本語名`UI確認`への名称変更、複製、左への並び替え、削除を
  実機で確認。確認用に追加した2件を削除し、元の6種類へ戻した。
- 狭いInspectorで最大化ボタンが領域外へ出ることを実画面で検出し、
  Type Selectorの幅をボタン分だけ減らし、さらに狭い場合は折り返すよう修正。

### 基盤レビューで修正した事項

- 各領域の非表示とマテリアル文書の閉鎖を分離。領域を閉じても文書は保持し、
  文書自体の終了には `Close Material` を使う。
- 複数同種領域でもAnimation / Sequencer / Proceduralの時刻更新は1フレーム1回。
- ギズモの領域別IDと全体Undo確定の判定を分離し、全体ガードに `IsUsingAny()` を使う。
- 複数Scene領域のマウス座標とホバーを集約し、ナビゲーション更新も1フレーム1回。
- 非表示のAnimationプレビューを描画し続けないよう、フレーム開始時に可視性をリセット。
- 従来のGPU Featureウィンドウも他領域の最大化中は表示を抑止。

これらは完了条件全体の達成を意味しない。Graph UI、HLSL/DXC接続、Material反映、
ノードプレビューとそのキャッシュ・同期を含む後続Phaseを引き続き実装する。
