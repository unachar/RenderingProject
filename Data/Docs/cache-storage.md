# レンダリングエンジンのキャッシュ

実行時のディスクキャッシュは、実行ルートの `Data/Cache` に保存する。
保存先は `source/cachepaths.h` で管理し、起動時に実行ルートを確定してから旧フォルダを移行する。

|フォルダ|内容|保存形式|
|---|---|---|
|Shaders|DXCのコンパイル結果|既存のチェックサム付きZstandard形式（v2）、旧v1も読込み可能|
|Pso|ドライバーのPSOバイナリ|チェックサム付きZstandard、旧バイナリも読込み可能|
|Textures|GPU用圧縮テクスチャ|DDSをZstandardで圧縮した `.dds.zst`、旧DDSも読込み可能|
|Thumbnails|サムネイルと依存情報|画像は `.dds.zst`、小さな依存情報は `.key`|
|ModelMaps|モデルのリソース名とインデックス|チェックサム付きZstandard、旧形式も読込み可能|
|VirtualGeometry|階層とストリーミング用ページ|階層はチェックサム付きZstandard、ページは独立したZstandardフレーム（v5）|
|VirtualGeometry/Models|元モデルと生成ジオメトリの対応|元モデルの絶対パスをハッシュした `.vgderived`、チェックサム付きZstandard|
|MaterialGraphs|復旧データと生成HLSL|復旧JSONはチェックサム付きZstandard。コンパイラーが読むHLSLはテキスト|
|Diagnostics|旧 `cache` フォルダの診断出力|診断ツールが読む元の形式|
|Validation|既存の検証用フィクスチャ|検証で指定された素材形式|

Zstandardは同梱の静的ライブラリを使用する（level 3）。ヘッダー込みで小さくなる場合に採用し、
縮まないデータは非圧縮で保存する。`DX12_COMPRESS_CACHES=0` は以降の保存で容量より速度を優先する。
読込み側はこの設定に関係なく両形式を扱う。GPU上のキャッシュにはディスク圧縮を適用しない。

汎用キャッシュの圧縮ヘッダーは、magic `CCHEZST1`、展開サイズ、保存サイズ、FNV-1aチェックサムの
4個のlittle-endian uint64。長さ・上限・単一フレーム・展開サイズ・チェックサムを確認してから使う。
一時ファイルを閉じてから置換し、失敗した読込みはキャッシュミスとして扱う。

Virtual Geometry v5は従来と同じ64 KiBの展開済みページを返す。ページディレクトリのSizeは
保存バイト数を表し、64 KiB未満のエントリーをZstandardで展開する。チェックサムは展開済みページに対する値。
ページごとのランダムアクセスと既存v2〜v4の読込みを維持し、ファイル全体の一括展開は行わない。

## 移行

- `EditorCache/*` → `Data/Cache/*`
- `bin/cache/pso` → `Data/Cache/Pso`
- `asset/virtualgeometry-cache` → `Data/Cache/VirtualGeometry`
- `cache` → `Data/Cache/Diagnostics`
- モデル横の `*.vgderived` → `Data/Cache/VirtualGeometry/Models`（旧ファイルはモデル読込み時にも移行）

移行先と名前が衝突した場合は、移行先を使用し旧ファイルを保持する。旧インデックス内の
`asset/virtualgeometry-cache` 参照も新しい保存先へ読み替える。キャッシュ以外の素材とユーザーの保存データは移動しない。
`shader/hlsl/build` やMSBuildの中間ファイルはビルド成果物であり、この実行時キャッシュには含めない。

## 検証記録

既存ファイルの一括圧縮の件数・容量・失敗一覧は `build/cache-compaction.json`、
Debugビルドのログは `build/cache-debug-build.log` に保存する。
一括圧縮では書換え前にZstandardの展開結果と元の全バイトを比較する。
新しいテストコードは追加しない。

2026-11-06の移行では18,747ファイルを処理し、圧縮した全ペイロードの展開後の一致を確認した。
別途31個のモデル対応インデックスも移行・圧縮した。旧フォルダは空になり、全体で約1.79 GBから
約1.19 GBへ削減した（約33%）。一括圧縮の失敗は0件。

DebugビルドはVisual StudioのPDBツール不整合を避けるため、検証用プロセスに
`_CL_=/Z7`、`_LINK_=/DEBUG:NONE /INCREMENTAL:NO` を設定した。プロジェクトのDebug設定は変更しない。
既存のVirtual Geometry実行検証ではストリーミング・描画とD3D12状態検証（エラー0件）を確認したが、
LOD選択・ラスタ被覆・材質比較の検証は失敗し、プロセスは終了コード3となった。
この実行検証は合格扱いにしない。記録は `build/cache-vg-validation.log`。

最終Debugビルドは成功。更新後のEXEによる3フレームの起動確認も終了コード0で完了した。
起動確認ログは `build/cache-startup-smoke.log`。Releaseビルドは未実行。
