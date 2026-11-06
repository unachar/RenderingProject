# Render

DirectX 12 レンダラー。Visual Studio の `DirectX12.sln` から x64 の Debug / Release をビルドします。

| フォルダ | 内容 |
| --- | --- |
| `source` | アプリケーションの C++ ソース |
| `shader` | シェーダーのソースとコンパイル結果 |
| `asset` | モデル・テクスチャ・シーンなどの素材 |
| `External` | 外部ライブラリと SDK。Assimp ヘッダーは `include/assimp` |
| `packages` | NuGet パッケージ |
| `config/msbuild` | MSBuild の詳細設定。ルートの Directory.Build.* から読み込む |
| `tools` | ソース収集ツールと起動用 EXE のソース |
| `bin/x64/Release` | Release の起動用 `DirectX12.exe` |
| `bin/x64/Debug` | Debug の起動用 `DirectX12.exe` |
| `build/obj` | コンパイラの中間ファイル |
| `build/symbols` | デバッグ情報・リンク用ファイル |
| `build/logs` | 今回のビルド検証ログ |
| `build/archive` | 過去のログ・補助スクリプト・バックアップ・検証成果物 |
| `Data/Cache` | 実行時に生成するキャッシュ |
| `Data/Docs` | 設計・調査資料。ルートにあった調査資料は `archive` |
| `Data/Logs` | 初期化ログ・デバイス障害ログ |
| `Save` | エディター設定・保存データ |


起動用 EXE は、同じ場所の `runtime/DirectX12.exe` を実行し、終了コードを引き継ぎます。
DLL と Agility SDK は `runtime`、配布ライセンスは `licenses` に置きます。
Visual Studio のデバッグ実行では、デバッガーが本体 EXE を直接起動します。

`build` に残る機能別フォルダは、既存の検証機能が固定パスで使用する出力、または過去の調査資料です。
外部 SDK 内部と NuGet のフォルダ構成は各ツールの参照を保つため維持しています。
`.git` はバージョン履歴、`.vs` は Visual Studio の作業状態です。

ソースを一つのテキストへ集約する場合は `tools/bundle_render.ps1` を実行します。
出力は `build/reports/render_bundle.txt` です。
