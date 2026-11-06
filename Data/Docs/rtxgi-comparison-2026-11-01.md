# DXR GI / RTXGI SHaRC 実機比較

## 採用判断

**標準は既存の Realtime GI (DXR) を維持する。RTXGI SHaRC は比較用の選択肢として追加する。**

最終版のSHaRCはキャッシュ本体を7 MiB減らせるが、GPU時間はLightTestで0.64%悪化し、室内で1.81%改善した。シーンによって優劣が逆転し、画質が明確に良くなった証拠も得られなかったため、全面置換の根拠にはしない。保存済みの `gi_mode=2` を維持し、検証用の設定は復元した。

この判断は今回の拡散放射照度キャッシュへのSHaRC統合と、この実機・シーンに限る。RTXGI全体の性能を否定するものではない。

## 統合した範囲

調査開始時点でプロジェクトにRTXGIのSDK本体・描画経路はなかった。[RTXGIの公式概要](https://github.com/NVIDIA-RTX/RTXGI)にあるSHaRCを選び、公式SDKの必要なヘッダーとライセンスを `External/RTXGI/SHaRC` に配置した。バージョンはSHaRC 1.8.3、リビジョン `4e21b585c33c83d723ca9a1e11bbb1090d145793`。

既存の拡散GIのキャッシュを、SDKのハッシュグリッド・蓄積・解決・検索へ接続した。材質に依存しない `E / pi` を格納し、検索後に放射照度へ戻す。既存の直接光評価、発光体サンプリング、ReSTIR GI、NRDは維持する。NRC、DDGI、RTXGIサンプル全体のパストレーサーは導入・比較していない。

最終版はSDKの推奨に沿って更新を約1/16に間引く。レイ数と検索解像度は減らさず、既存のGI追跡中に書き込み、UAVバリア後にSDKのResolveを実行する。参照する解決済み値は前フレームのもの。疎なセルが更新前に失効しないよう未更新寿命は64フレームとした。SHaRCにはヒット距離がないため、任意の主GIレイ省略機能には使わず、NRDへ実際のヒット距離を渡す。

## 最終版の測定

NVIDIA GeForce RTX 3050 Laptop GPU、VRAM 4 GiB、ドライバー617.14。製品のRelease実行ファイルを使用。1280×720の内部・出力解像度、固定カメラ移動、120フレームのウォームアップ後に360フレームを各方式・各シーン2回測定した。VSync、FPS固定、自動品質調整、詳細レイ計数は無効。NRD等の保存設定は共通。

既存の `RenderBenchmark` と `RenderProfiler` を使い、新しいテストコードは作成していない。最終比較に使った8試行すべてで360個の有効GPUサンプルと完了フラグを確認した。各シーンの全フレームでカメラ・シーンの署名が一致し、保存された設定の差は `gi_mode=2 / 4` だけだった。

以下は2試行の平均。GPU全体はdirect queueのフレーム計測範囲であり、表示待ちを含むフレーム間隔ではない。

| シーン | 既存 GPU全体 | SHaRC GPU全体 | SHaRCの変化 | 既存 GI全体 | SHaRC GI全体 |
|---|---:|---:|---:|---:|---:|
| 保存中のLightTest | 28.149 ms | 28.330 ms | +0.64% | 4.623 ms | 4.670 ms |
| 室内比較シーン | 21.352 ms | 20.964 ms | −1.81% | 5.063 ms | 4.962 ms |

| 試行 | 既存 GPU全体 | SHaRC GPU全体 |
|---|---:|---:|
| LightTest A | 27.921 ms | 28.263 ms |
| LightTest B | 28.378 ms | 28.397 ms |
| 室内 A | 21.602 ms | 20.825 ms |
| 室内 B | 21.101 ms | 21.103 ms |

キャッシュの確保量は262,144セルで既存18 MiB、SHaRC 11 MiB。DXGIのプロセスlocal usageの平均差は両シーンとも約6.69 MiB減だった。キャッシュ単独の確保量とプロセス全体の常駐量は別の指標である。

初期の全面更新版はLightTestで約37.84 ms、室内で約25.86 msとなり、採用しなかった。上の表は更新を間引き、寿命も調整した最終版である。詳細レイ計数付きの初回試行、中断試行、初期実装、寿命調整前の値は最終表から除外した。

## 画質と操作

LightTestは既定のエディターカメラから閉じた部屋の外側を見るため、画質の判断材料として不十分だった。そこで既存 `global_illuminationTest.scene` のコピーから手前の壁 `Cube (5)` だけを外し、室内を見える状態にした。GI強度は両方式とも1。コピーは `build/rtxgi/GiComparison.scene` に保存し、元シーンは変更していない。

同じカメラ位置の室内保存画像を目視比較した。両方式とも床・壁・立体への間接光と色の伝播があり、SHaRCの明確な画質優位は確認できなかった。GI OFF画像との差で間接光の動作も確認した。参照パストレーシング画像に対する誤差評価や、動く発光体・ライト変更への時間応答の網羅的な評価は実施していない。

- [既存GIの室内画像](../bin/validation/render-benchmark/rtxgi-room-existing-a/final.png)
- [最終SHaRCの室内画像](../bin/validation/render-benchmark/rtxgi-room-final-sharc-b/final.png)
- [GI OFFの参照画像](../bin/validation/render-benchmark/rtxgi-room-off/final.png)

実機UIでは「ウィンドウ → 環境設定 → Advanced GPU → Screen Space → Global Illumination」で `Realtime GI (DXR) → RTXGI SHaRC (DXR) → Realtime GI (DXR)` を操作した。旧キャッシュ設定がOFFの条件でもSHaRCの描画が継続し、戻した後のメモリ解放を確認した。UI確認時は内部360×180であり、その画面のFPSは上表の性能比較には使用していない。

- [RTXGI選択状態](../build/rtxgi/ui-rtxgi.png)
- [既存GIへ戻した状態](../build/rtxgi/ui-existing.png)

切り替え時はGPU完了を待って旧バッファを解放し、GI・リザーバーの履歴を破棄する。RTXGI選択中はそのキャッシュを必須として扱い、旧キャッシュ設定がOFFでも毎フレーム解放されないようにした。既存の設定番号0〜3は保持し、RTXGIを4として保存・読込する。

## 検証と証拠

Releaseビルド成功。既存中間ファイルにはコンパイラ不一致があったため、`build/rtxgi/obj-Release/` とプロセス内の `/Z7` を使用した。SHaRCのTrace / Clear / ResolveはDXCでコンパイルし、最終版は実機起動ログの `ready=1 native16=1` とGPUプロファイラーの `RTXGI SHaRC` パスで動作を確認した。変更C++範囲のclang-format確認と差分の空白チェックを実施。独立レビューのキャッシュ保持と疎なセルの寿命の指摘を反映した。

- [最終集計・条件・ハッシュ](../build/rtxgi/comparison.json)
- [Releaseビルドログ](../build/rtxgi-release-reviewed-build.log)
- 最終試行: `bin/validation/render-benchmark/rtxgi-light-final-existing-{a,b}`、`rtxgi-light-final2-sharc-{a,b}`、`rtxgi-room-existing-{a,b}`、`rtxgi-room-final-sharc-{a,b}`。
- 各試行に `frames.csv`、`passes.csv`、`settings.cfg`、`metadata.txt`、`summary.json`、`final.png` を保存。

実行ファイルSHA-256: `361c0228efcd2386f8cee52800d2ce4518c90268680ab3930ad1874e9f27719a`。生成物はローカル検証成果物であり、Git管理対象外。

未確認: 他GPU・他解像度での優位性、DXR/ネイティブ16bit非対応機での実機フォールバック、NRC/DDGIとの比較。今回の少数シーンの僅差を全般的な性能向上や高画質化として扱わない。
