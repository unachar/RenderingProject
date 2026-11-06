# 幾何スペキュラAAの実レンダラー評価

評価結果: 対象材質だけを変更する統合動作は確認できたが、このシーンで画質改善を確認したとは判断しない。既定 `GEOMETRIC_SPECULAR_AA=0` を維持する。

## 条件と記録

RTX3050 Laptop GPU、Debug、960×640、固定1/60刻み。低粗さ0.04のPBR球と平面cube、Toon球、Unlit球を同じ画面に配置した。通常のbindless有効なCPU Deferred経路を使い、mesh shader・Compute GBuffer・DXR・DLSS・TAA/FXAA・雲・大気・自動露出・motion blurを無効にした。球は16 segments/8 rings。背景の空を無効にしても、既存のHDR環境反射は残っている。

staticな32〜95フレームで64件の異なる有効GPU timestampサンプルを取得し、この間は画像readbackを行わない。96フレームの静止画と、100〜115フレームの微小camera移動について、PNG、線形HDR、材質、深度を保存した。全17組。GPU時間の区間と画像取得区間を混ぜない。

- OFF: `bin/validation/technology/aa-engine-off-20261030-235039-33578777`
- ON: `bin/validation/technology/aa-engine-on-20261030-235359-e617b8c5`
- 比較: `bin/validation/technology/geometric-aa-engine-comparison.json`

両実行は終了コード0、D3D12 errors0、validation failures0。各run.jsonで元ソースと元CSOのハッシュへの復元を確認した。実際のGBuffer読み戻しでもON/OFFが反映されたことを検査した。

## 確認した統合動作

全17組で、深度・材質分類・粗さ以外の材質channelが一致した。平面PBR、Toon=4、Unlit=10の対照は、HDRのRGB数値、材質値、PNGのRGB画素が一致した。静止PBR球は29,702画素で粗さが変わり、平均は0.039978から0.104700になった。

これは `invariants_pass=true` の意味であり、画質採用を示すPASSではない。PBR球の変化と、対照へ影響しないことを分けて確認した。

## 時間と画像変化

|指標|OFF|ON|
|---|---:|---:|
|GPU frame中央値、64サンプル|2.324815 ms|2.339120 ms|
|GBuffer中央値|0.937472 ms|0.944128 ms|
|camera区間の共通球内平均輝度|0.430578|0.468527|
|同区間の最大輝度|2313.283|3245.512|
|画面上の隣接frame差のRMS|1.896161|3.432959|

時間は各モード1実行のDebug測定であり、反復したABBA試験やRelease性能検証ではない。差が小さいため一般的なGPU負荷率やFPS向上を主張しない。

camera区間の指標は、両実行・全frameで共通して見える球の内側28,579画素を使い、境界を2画素除外した線形RGB輝度から計算した。camera移動による正当な反射の移動も含み、reprojectionや画素積分のground truthとの比較は行っていない。したがってRMSをそのままちらつきの品質得点としない。この条件ではONの最大輝度と画面上の変化量が増え、時間方向の改善を支持する結果は得られなかった。

先行の[GGX数値実験](geometric-specular-aa-quality-probe.md)は標準NDFだけの積分参照であり、今回の環境反射を含む描画とは異なる。現行 `SurfaceLighting.hlsli` のdirect-light NDFには分母1e-5のfloorもあり、低粗さの応答が標準の非clamp式とは違う。これらを今回の差の原因と断定せず、既存PBR照明をこの検証の中で変更しない。

## 再現と復元

Debug rendererをビルドした後、他のrenderer/GPU試験を走らせず順に実行する。

```powershell
./scripts/validate-geometric-aa-scene.ps1 -Mode Off
./scripts/validate-geometric-aa-scene.ps1 -Mode On
# 各出力ディレクトリを指定する
python scripts/compare-geometric-aa-scene.py OFF_DIRECTORY ON_DIRECTORY --output bin/validation/technology/geometric-aa-engine-comparison.json
```

runnerは評価中だけDeferredPSの既定値とprebuilt CSOを切り替え、modern runtime compileとfallbackに同じmodeを与える。元のbytesとhashを結果フォルダーへ保存し、同じ場所の一時ファイルから原子的に置き換え、終了・失敗後に照合して復元する。検出した他者の変更は上書きしない。排他lockで同runnerの並列実行を防ぐ。途中でPowerShell自体を強制終了した場合は、次の作業前にrun.jsonと実ファイルを確認する。

`scripts/test-geometric-aa-evaluation-cleanup.ps1` はsource交換後、CSO交換後、metadata報告時の失敗を注入し、実ファイルのhash、未設定を含む環境変数、作業ディレクトリ、lockの解放を確認した。3ケースすべて合格（aa-evaluation-cleanup.log）。復元処理は独立したguardと外側finallyを使う。PowerShellからnull文字列を渡す箇所はNullString.Valueを使った。[File.Replaceのnull backup契約](https://learn.microsoft.com/en-us/dotnet/api/system.io.file.replace)、[PowerShellのnull文字列変換](https://github.com/PowerShell/PowerShell/issues/4616)を参照した。

最初の実シーンOFF試験は、試験側のToon/Unlit番号が1/0だったため対照検査が失敗した。実際のenum4/10へ修正し、失敗記録を残して再実行した。旧native raster fixtureにも同じラベル誤りがあったため4/10へ修正し、class channelのassertを追加。再実行でnative compute、raster、production visibility reconstructionは合格した。compute側は以前から全16分類を検査しており、製品のPBR=11だけを対象とする式は変更していない。

独立レビューでrunnerの異常時cleanup指摘を修正し、再レビューで解消した。比較解析も再レビューで指摘なし。新しいopt-in hookを追加したビルドで、通常features84フレーム・fallback84フレームがともに合格した（renderer-smoke-after-aa-evaluation.log）。5枚の通常キャプチャも以前の固定時計の基準と完全一致した（normal-after-aa-captures.json）。
