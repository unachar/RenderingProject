# リファクタリングの検証結果

実施日: 2026-10-05。基準: `69efcf7b2b82ea27f5588ae6dfd749f7d8c1979b`。変更ブランチ: `refactor/rendering-responsibilities`。

実装と実行可能なCPU検証は完了。WindowsのDebug／Releaseビルド・実GPU描画・画像比較は未検証であり、全テスト完了や動作同一は保証していない。ドラフトでレビューする。

環境はLinux、Python 3、g++ C++17。MSBuild／v145／Windows SDK／D3D12実GPUは利用できない。
テスト設計の51件を集計するとPASS 7件、BLOCKED 42件、N/A 2件。部分的なCPU／静的証跡があっても、期待結果を完全に確認できないケースはBLOCKEDにした。

## 実行した確認

- RenderGraph: 変更前・変更後で同じ10シナリオがPASS。実際の `rendergraph.cpp` をコンパイルし、CPUのコマンド記録用D3D型を使用。実D3D12バリアの検証ではない。
- Timeline: 両版の実際の `ApplyProperty`／`ReadProperty` 本体を抽出してコンパイル。位置・回転・スケール、書込み対象、無効Entity、Component欠落、nullable出力、カメラの範囲制限、後処理のクランプがPASS。ECSとDirectX依存はテスト用代替。
- project/filter登録、includeディレクトリ、旧クラス参照、主要6GPU構造体の宣言比較がPASS。
- シェーダーファイルは基準とバイト単位で同一。
- tree-sitter-cppによる追加の構文エラーノード0、自作三項演算子0、新規循環ヘッダーペア0。基準・変更版とも既存のマクロ等に起因する解析エラーノード138件。これはコンパイル／リンクの成功を意味しない。
- 実装分離直後の比較で509メソッド本体が名前と空白の正規化後に一致し、メソッド欠落0。`RenderFlow`とライト更新内の材質アップロードは同じ位置から新しい責務へ委譲する2件として別確認した。
- 基準の `InstancingSystem` に実装済みメソッド17宣言の欠落を発見。別コミットで前方宣言・メソッド宣言を補完し、本体は変更していない。Windowsでのコンパイル確認は引き続き必要。

## 再実行

リポジトリ直下から実行する。CPUテストはg++または `CXX` で指定したC++17コンパイラーを使用する。

```sh
python3 tests/check_project_structure.py
python3 tests/run_rendergraph_tests.py --baseline-ref 69efcf7b2b82ea27f5588ae6dfd749f7d8c1979b
python3 tests/run_timeline_tests.py --baseline-ref 69efcf7b2b82ea27f5588ae6dfd749f7d8c1979b
python3 -m pip install -r tests/requirements-structure.txt
python3 tests/check_cpp_structure.py
```

Windowsでは基準版と変更版を別チェックアウトへ置き、同じSDK・依存ライブラリ・モデル・設定を復元した後、v145が使えるDeveloper Command Promptで次を実行する。これは未実行の手順。

```bat
MSBuild DirectX12.sln /t:Rebuild /p:Configuration=Debug /p:Platform=x64 /bl:debug.binlog
MSBuild DirectX12.sln /t:Rebuild /p:Configuration=Release /p:Platform=x64 /bl:release.binlog
```

続いて[テスト設計](refactoring-test-cases.md)のS1～S5で起動・終了、リサイズ、影、透明物、AA、エディター、物理、保存読込、長時間動作を比較する。
A01／A02と描画・寿命の必須ケースが通るまではマージ完了扱いにしない。

## ケースごとの状態

| ID | 結果 | 証跡／未完了部分 |
|---|---|---|
| A01 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| A02 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| A03 | PASS | 全自作cpp／hの一度だけの登録、フィルター、include検索パス、旧クラス参照を検査。構文解析はコンパイルの代用ではない。 |
| A04 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| A05 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| B01 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| B02 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| B03 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| B04 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| B05 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| B06 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| B07 | BLOCKED | 移動段階の関数本体・呼出順を比較。Windowsでの呼出追跡は未実施。 |
| C01 | BLOCKED | Deferredの画像比較は未実施。基準のSetRenderModeはDeferredを強制し、Forward切替の実行確認は対象外。 |
| C02 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| C03 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| C04 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| C05 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| C06 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| C07 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| C08 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| C09 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| C10 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| C11 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| C12 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| C13 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| C14 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| C15 | N/A | 基準の自作コードにはNGXヘッダー／ライブラリ登録のみ。DLSS評価呼出・実行経路は見つからない。 |
| C16 | N/A | 基準の自作コード／シェーダーにAS構築、DispatchRays、TraceRay／RayQueryの実行経路は見つからない。 |
| D01 | BLOCKED | 登録・Init／Update／Draw／Uninitのループと順番を静的比較。実アプリ追跡は未実施。 |
| D02 | BLOCKED | Play専用システムの更新条件を維持。UIでの停止／実行確認は未実施。 |
| D03 | BLOCKED | タイムラインのプロパティ読書きと境界値は両版でCPUテストPASS。入力・描画・実ECSの統合確認は未実施。 |
| D04 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| D05 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| D06 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| D07 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| D08 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| D09 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| E01 | PASS | Graph構築をFramePipelineへ移動。SystemManagerはシステム管理と互換委譲を担当。 |
| E02 | PASS | 旧新対応表と各公開APIをレビュー。描画・設定・ライト・材質・図形・CBを分離。 |
| E03 | BLOCKED | 操作窓口は分離したが、RendererState共有状態と既存重複メンバーを保持。所有関係の全面整理は未完了。 |
| E04 | PASS | 自作ヘッダーの新規循環依存なし。分割cppの直接依存を確認し、不要なincludeを削減。 |
| E05 | PASS | 既存の括弧・インデントに合わせた移動と分岐変更をレビュー。全ファイルへの一括フォーマットはしていない。 |
| E06 | PASS | C++構文木上の自作コードのconditional_expressionは0。第三者ヘッダー2件は未変更。 |
| E07 | BLOCKED | 選択参照、nullptr、遅延分岐、真偽値、型をレビューし、タイムライン選択を両版でテスト。全GPU経路の境界値検証は未実施。 |
| E08 | BLOCKED | Vertex／ConstantBuffer3D／PostProcessConstants／MaterialPartShaderConstants／PBRConstants／ShadowConstantsの宣言が基準と一致。MSVCのsizeof／alignment実測は未実施。 |
| E09 | BLOCKED | 分離直後に509メソッド本体が名前・空白を除いて一致、欠落0。委譲に変わる残り2メソッドを確認。分岐整理後のGPU結果は未検証。 |
| E10 | PASS | 外部コード・シェーダー・アセット・SDK設定は変更なし。配置／責務／分岐／テスト／説明と、別コミットの既存宣言漏れ補完。仕様追加はない。 |
| F01 | BLOCKED | パス構築の移動を静的比較。RenderGraphの順序／依存／バリア等10シナリオは両版PASS。実GraphのGPUキャプチャは未実施。 |
| F02 | BLOCKED | RenderGraphの状態遷移・最終状態・UAV・手動バリアをCPUテスト。実GPUのリサイズ／設定切替は未実施。 |
| F03 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
| F04 | BLOCKED | Windows／実GPU／同一アセットによる基準・変更版比較をこの環境で実施できない。 |
