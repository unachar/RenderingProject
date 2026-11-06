# ModelTest の起動・シーン切り替え修正

確認日: 2026-10-19 (JST)

## 原因と修正

透明描画の `Raytracing::BeginGlassRead` が、スキニング済み頂点バッファの
`UNORDERED_ACCESS` に `PIXEL_SHADER_RESOURCE` を加えていた。
DirectX 12 では書き込み状態と読み取り状態を同時に指定できず、診断ログに
`ResourceBarrier` の invalid combination of state bits (ID 526) が記録された。
そのフレームの command list の Close が `0x80070057` で失敗し、エディターが終了した。
保存された `scene=asset/Scenes/ModelTest.scene` により、次回起動でも同じ問題が発生していた。

スキニング済み頂点を透明描画中だけ `VERTEX_AND_CONSTANT_BUFFER | PIXEL_SHADER_RESOURCE`
へ切り替え、終了時に元の UAV 状態へ戻すように修正した。
ModelSystem も、透明描画が管理する頂点バッファに対して状態遷移を重複発行しない。
読み取り専用の静的頂点・インデックス・アップロードバッファは既存の扱いを保つ。

## 可読性

- 自作 C++ と既存テストの 411 ファイルに共通の書式を適用した。
  外部コード、DirectX の同梱ヘルパー、生成済み LTC 数値テーブル、シェーダーは変更しない。
- シーン復元を `RestoreLoadedEntity` に分け、JSON を `void*` で渡していた箇所を型付き参照へ変更した。
- モデルパス・キャッシュキーの変数名と条件分岐を整理した。
- `.clang-format` と `scripts/format-source.ps1` で同じ書式を再適用・確認できる。
- 作業開始時に存在したシーン入力検証と GPU マテリアル参照の修正は保った。

## 検証

- Debug / Release x64 ビルド: 成功。
- ModelTest: Debug / Release とも実際に 120 フレーム描画し、正常終了を記録。
  `device_removed.log` に新たな GPU 命令エラーは追加されなかった。
- Release の通常起動: ModelTest の描画とヒエラルキーを画面で確認。
- アセットブラウザーを実操作して GlassTest → ModelTest の切り替えを確認。
- Debug の再起動検証: 30 フレーム描画・正常終了・設定復元まで成功。
- 既存テスト: Editor configuration 14 checks と Material asset tests が成功。
- 411 ファイルの書式確認と `git diff --check`: 成功。

実行ログ: `bin/validation/startup/modeltest-fixed-{Debug,Release}-1.log`

再起動ログ: `bin/validation/startup/modeltest-restart-Debug-1.log`

画面: `build/modeltest-recovery/modeltest-after.png`

起動検証スクリプトには `-Scene` と `-Frames` を追加した。検証で変更する設定は一時ファイルから
置き換えて復元し、IDE がファイルをマップしている場合の上書き失敗を避ける。
新しいテスト用 C++ ファイルは追加していない。
