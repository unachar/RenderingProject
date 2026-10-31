# STBNの一次ソース追加調査

2026-10-31。100件調査の8番、NVIDIA-RTX/STBNをREADMEより深く確認した。結論は、既存R2/R4の一括置換ではなく、まず低サンプル数のAO入力で比較する候補。実装・データ取込み・画質/性能の採用判定はまだ行っていない。

## 確認した版と資料

commit `48b2839e4d8b7f0202ac72c6b0ae720d235a5b8b`（2025-02-05）を固定して、次を読んだ。

- [README](https://github.com/NVIDIA-RTX/STBN/blob/48b2839e4d8b7f0202ac72c6b0ae720d235a5b8b/README.md)、[License.txt全文](https://github.com/NVIDIA-RTX/STBN/blob/48b2839e4d8b7f0202ac72c6b0ae720d235a5b8b/License.txt)、[CMakeLists.txt](https://github.com/NVIDIA-RTX/STBN/blob/48b2839e4d8b7f0202ac72c6b0ae720d235a5b8b/CMakeLists.txt)。
- [生成CLIの型・寸法指定](https://github.com/NVIDIA-RTX/STBN/blob/48b2839e4d8b7f0202ac72c6b0ae720d235a5b8b/Tools/VectorApp/CMDOptionsParser.cpp)、[Float2 generator](https://github.com/NVIDIA-RTX/STBN/blob/48b2839e4d8b7f0202ac72c6b0ae720d235a5b8b/Libraries/Vector/RandomTypeGenerators/Float2RandomGenerators.cpp)。
- [RGBA export実装](https://github.com/NVIDIA-RTX/STBN/blob/48b2839e4d8b7f0202ac72c6b0ae720d235a5b8b/Libraries/Vector/Exporters/RGBAExporters.cpp)と[PDF変換](https://github.com/NVIDIA-RTX/STBN/blob/48b2839e4d8b7f0202ac72c6b0ae720d235a5b8b/Libraries/Vector/Exporters/RGBAExporters.h)、[scalar export/配列index](https://github.com/NVIDIA-RTX/STBN/blob/48b2839e4d8b7f0202ac72c6b0ae720d235a5b8b/Libraries/Shared/BlueNoiseTexturesND.cpp)、[乱数seed](https://github.com/NVIDIA-RTX/STBN/blob/48b2839e4d8b7f0202ac72c6b0ae720d235a5b8b/Libraries/Shared/STBNRandom.h)。
- [NVIDIAの技術記事Part1](https://developer.nvidia.com/blog/rendering-in-real-time-with-spatiotemporal-blue-noise-textures-part-1/)。Part2は取得を2回試みたがtimeoutとなったため、内容を確認済みとして使わない。

記事では、低次元・低サンプル数での利用、point sampling、時間方向のslice送りを説明している。動く画素ではTAA下の時間方向の利点を失う場合があり、高次元/多数サンプルには低食い違い系列が適する場合もある。この制約を、本エンジンでの改善実測と取り違えない。

## 本エンジンに既にあるもの

`source/rendering/stablesampling.h` は画素ごとのhash scrambleとR2/R4系の系列を持ち、追加textureを使わない。`index & 4095` の位相を使用し、domain1/2を4次元の組として扱う。checkerboardの追跡では `PixelSampleIndex(frame,true)=frame>>1` として系列を飛ばさない。

|既存経路|確認した入力契約|STBN比較時の扱い|
|---|---|---|
|RaytracingCSの安定shadow|domain3、`frame & 7` とsample×8。SIGMA入力の最初のrayをsample数から独立させている|まず維持。64sliceの周期へ変えるだけでSIGMA向けの挙動を同等とみなさない|
|同AO|domain4＋sample、2成分乱数をcosine hemisphereへwarp|最初の低SPP比較候補。既存PDF/積分を保つ|
|RayReflectionsCSのGI|domain0とcheckerboard-aware index|AO比較後に検討。frameを直接使って半分の系列を飛ばさない|
|反射のlobe＋pixel footprint|domain1/2を4Dとして使用|2成分texture一枚への単純置換をしない|
|発光体/三角形選択＋重心座標|domain1/2の条件付き分布|選択と面内座標の相関・PDFを検証せず置換しない|
|ReSTIRのreservoir更新|別のstateful hash乱数|同じSTBN値を全更新へ使い回す対象にしない|

`scripts/test-stablesampling.ps1` を実行してPASS。範囲、exact replay、checkerboardの両parity、domain分離、4D選択後の重心座標の条件付き平均が許容範囲に入ることを確認する既存試験である。全分布の独立性の証明や、STBNの品質・速度・分布の合格証拠にはしない。ログ: `bin/validation/technology/stable-sampling-baseline.log`。

## データの型とPDFは交換可能ではない

この版のCLI既定は128×128×64、Float2、Uniform。Uniform Float2は各成分を[-1,1]で生成する。一方、読んだ `GenerateFloat2Unit` 関数は各成分を±1に振っており、同じuniform squareの2成分乱数ではない。

exporterは値を8bit channelへ写してRGBA PNGに保存する。PDF alphaは `pdfMin/pdfMax` で正規化され、一定PDFなら255になる。したがって、alpha/255をそのままPDFとして使ってはいけない。型、generator、元のPDF範囲、decode方法をデータに付随させる必要がある。

初回AO候補はuniform Float2を[0,1]の2成分として復号し、既存のcosine warpを維持する方法。均一方向のunit vectorとcosine-weighted vectorを混ぜるとAOの推定対象が変わるため、名前だけで選ばない。8bit量子化、0/1端点、複数SPPでのslice/画素offset、同じ値のdomain間使い回しも別に検査する。PNGをRG16へ入れ直しても、失われた精度は増えない。

## 容量と実装境界

GitHub metadataで `Assets/STBN.zip` は29,681,961 bytesと確認した。archive全体は複数形式を含む配布物であり、これをruntime容量として扱わない。binaryは取得・展開しておらず、個別ファイル名・内容・hashや圧縮後容量は未確認。

128×128×64、mipなしという候補を仮定した数値データ本体は次のとおり。DX12の実allocation/heap alignment、upload staging、file headerは含まない。

|候補の格納形式|本体bytes|用途の候補|
|---|---:|---|
|R8_UNORM|1,048,576 (1MiB)|scalar|
|R8G8_UNORM|2,097,152 (2MiB)|uniform Float2の2成分だけをcookした場合|
|R8G8B8A8_UNORM|4,194,304 (4MiB)|3成分＋補助値など|

既存R2/R4のtexture容量は0なので、STBN自体は容量削減ではない。目的は同じray数での誤差改善、または同等品質までray数を減らせるかの評価になる。GPU時間の増減は未測定。

raw numerical dataとしてlinear/point/mipなしの2D arrayを想定し、sRGB変換・BC圧縮・通常のMIP生成を無条件に通さない。SRVは既存ray rootのt0〜t6とtelemetryを避けて追加が必要。現在の11 root parametersと、engine heapの予約域を確認した上で設計する。過去に修正したHi-Z/RT descriptor重複を再発させない。

generatorはoffline toolとして分離できる。SDKのCMakeはC++20とpcg/cxxopts/stb/gtest等を参照するが、それらをrendererへ丸ごと加える必要はない。なお現プロジェクトのx64 LanguageStandardはC++23であり、generatorのC++20指定を理由にプロジェクトの標準を変更しない。

## ライセンスの確認状況

License.txtには非商用のsource-code licenseと、商用向けのRTX SDK licenseの両ブロックがある。非商用側3.3は研究・評価に用途を限定している。商用側にはsample source、組込み配布、notice等の条件があり、supplementも含まれる。MIT/BSDの無条件な素材として扱わず、最初の非商用ブロックだけを見て「商用経路がない」とも判断しない。

この調査ではライセンス全文を読み、該当source fileのNVIDIA noticeも確認した。製品でどちらの条件を使い、どのasset/生成物を同梱するかは決定していない。SDK sourceやデータはworkspaceへコピーしておらず、ライセンス受諾・再配布手続を完了したという記録ではない。

## 次に比較するなら

AOの1SPPを最初の対象とし、R2を既定のまま残して個別に切替・比較する案が妥当。静止/移動/disocclusion、NRD有無、同じray budgetでの線形HDR誤差と履歴安定性、実allocation・load時間・GPU時間を測る。frame reset、checkerboard、1/複数SPP、データ欠落時のR2 fallback、PDF/平均の整合性も必要。shadowの8phase契約や4D発光体/反射系列を一括で変えない。

現段階はsource auditであり、STBNが既存R2/R4より良いとの採用判断は未成立。次の実装を承認済み扱いにせず、zstdの保留をSTBN実装の承認としても扱わない。
