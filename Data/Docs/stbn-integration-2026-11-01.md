# STBNの描画経路への統合と実機確認

## 最終構成

既存のSTBN配布アーカイブからuniform Float2をRG8へ変換し、約2 MiBのGPUバッファとして一度だけ転送する。生成器のライブラリをランタイムへ追加する方式ではない。データの出典・SHA256・再生成方法・NVIDIAのライセンスは `asset/Noise/README.md` と同ディレクトリのnoticeに記録した。

- AO: 128×128×64のSTBNを画素位置とフレーム番号で読み、既存のcosine hemisphere変換へ渡す。複数レイは画素オフセットとスライスを分離する。
- 通常反射: STBNを既存のGGXサンプリングへ渡す。チェッカーボードでは `PixelSampleIndex` によって追跡したフレームだけ時間を進める。
- GI: STBNの空間分布をR2系列の画素ごとのオフセットに使い、既存の低偏差な時間系列を保つ。
- SIGMA向けの太陽影: 実測でTSRの揺れが増えたため、既存の8位相R2系列を維持する。
- 発光体選択と三角形座標、反射の4サンプル時のローブと画素内位置: 既存の4次元系列を維持する。ReSTIRのreservoir更新乱数も変更しない。

NRD・OptiX・TSRには、このサンプリングで得たレイ結果が既存の入力経路から渡る。デノイザーのPDF・信号パッキング、TSRの履歴判定やカメラのHaltonジッターは変更しない。

`ray.StableSampling` が有効な既存経路で適用する。STBNは既定で有効。起動前にプロセス環境変数 `DX12_STBN=0` を指定すると既存系列へ戻る。ファイル欠落・サイズ不正でも同じフォールバックを使い、`init_log.txt` に記録する。

## 実装境界

`source/rendering/stbn.h` がデータ読込・GPU転送・寿命を管理し、`shader/hlsl/Stbn.hlsli` が復号と系列を共有する。数値データは線形・補間なしで読み、量子化ビン中央を復号する。sRGB変換・MIP・BC圧縮を使わない。

共有raw SRVは `t0, space16`。shadow/AO rootの引数11と、radiance rootの引数24へ追加し、既存descriptor heapの予約域は消費しない。GPUバッファの論理サイズは2,097,156 bytes（RG8本体と4-byte有効フラグ）。実heap allocationのアラインメントは含まない。HLSLのビルド依存にも共有includeを追加した。

## 実機検証

NVIDIA GeForce RTX 3050 Laptop GPU、Release x64。新規テストコードは追加せず、既存の実行検証シーンを使用した。

|確認|結果・証拠|
|---|---|
|Releaseビルド|MSBuild終了コード0。`build/stbn/build-gi-only.log`|
|AO・反射・影・TSR等の既存84フレーム検証|`feature smoke failures = 0`。`build/stbn/features.log`|
|NRDの実ディスパッチ|REBLUR_SPECULAR、REBLUR_DIFFUSE_OCCLUSION、SIGMAのPASSを同ログで確認|
|OptiXとNRDの実機切替、GIと反射の同時描画|218フレーム、`GI validation failures = 0`。`build/gi-tests/validation-Release-optix.log`|
|最終方式のSTBN無効／有効|各512フレーム、両方 `GI validation failures = 0`。`build/stbn/final-off.log` と `final-on.log`|
|最終方式の再現性|`stbn-gi-only/hashes.csv` と `stbn-final-on/hashes.csv` が完全一致。4ケース×32フレームのGI・影・出力・raw GIハッシュを比較|
|画像|比較シーンの正面画像と既存featureシーンのキャプチャを目視確認|

Release実行ではD3D12 debug queueは利用できない。Debug layer/GBVの検証に合格したという結果ではない。

## 最終方式の比較値

`build/temporal-lighting/stbn-final-off/summary.csv` と `stbn-final-on/summary.csv` を比較。出力1280×720、入力371×209、各ケース128フレーム中の最後32フレームを測定。RMSは固定した受光面の時間変動であり、正解画像に対する誤差ではない。負値は揺れの減少。

|経路・視点|NRD後GIのRMS|最終出力のRMS|GPU時間|
|---|---:|---:|---:|
|TSR・正面|-2.56%|-0.48%|1.704→1.721 ms|
|TSR・浅い角度|+0.77%|-0.56%|1.680→1.696 ms|
|NIS＋TAA・正面|+1.49%|+0.98%|1.417→1.421 ms|
|NIS＋TAA・浅い角度|-1.15%|-0.12%|1.413→1.423 ms|

影のハッシュは最終無効／有効で一致する。最終方式の比較データは `build/stbn/comparison.json` にも保存した。

改善は小さく、視点によってGIやNIS＋TAAのRMSは増加する。全シーンでの画質改善や処理時間短縮を証明した結果ではない。AO・反射は実ディスパッチと出力の正常性を検証したが、この表はAO・反射のノイズ低減率を測ったものではない。

影を含めて時間系列を一括置換する方式は、浅い角度のTSR出力のRMSが増えたため採用しなかった。既存の時間系列を保つ対象とSTBNの時間スライスを使う対象を分けることが、このエンジンでの実測に基づく選択となる。
