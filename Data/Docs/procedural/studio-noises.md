# Procedural Studio のノイズ

基底ノイズは `source/procedural/noise.h` の `ProcStudio::Basis`、フラクタル合成は `FractalType` で指定する。GPU 実装は `shader/hlsl/noise.hlsl` の `TextureNoise`。スタジオの「基底ノイズ」「フラクタル」から選択できる。

既存の enum 値は維持した。今回追加したのは基底18種類と合成2種類で、基底は計22種類、合成は「なし」を含め計9種類。テクスチャ、立体テクスチャ、ボリューム、ノイズを利用するモデル生成が同じ CPU 評価器を利用し、2D GPU 生成にも同じ方式を実装している。

| 指定された方式 | 選択項目・実装 |
| --- | --- |
| White Noise | 基底 White。セル内を各軸256区画に分け、整数ハッシュで独立した値を返す離散白色ノイズ |
| Value Noise | 既存基底 Value |
| Perlin Noise | 既存基底 Perlin |
| Simplex Noise | 基底 Simplex。3D単体格子の4頂点で評価。タイル時は平行移動した場を滑らかにブレンド |
| Voronoi Noise | 基底 Voronoi。最寄り特徴点のIDを値に変換 |
| Worley Noise | 既存基底 Worley。最寄り特徴点までの距離F1。既存Cell EdgeはF2−F1 |
| Cell Noise | 基底 Cell。直交格子セルごとに一定のランダム値 |
| Gabor Noise | 基底 Gabor。ランダムなXY方向のガボール核を疎に加算 |
| Wavelet Noise | 基底 Wavelet (Compact)。コンパクトな3Dウェーブレット核の重ね合わせ |
| Curl Noise | 基底 Curl。3つの独立したポテンシャルのcurl。スカラー出力はZ成分を有界化 |
| fBm | 既存合成 fBm |
| Ridged Noise | 基底 Ridged (単一)。Perlinの絶対値を反転した単一オクターブの稜線 |
| Value-Gradient Noise | 基底 Value-Gradient。値と勾配の補間結果を同じ比率で合成 |
| Sparse Convolution Noise | 基底 Sparse Convolution。符号付きインパルスとコンパクト核の畳み込み |
| Spot Noise | 基底 Spot。シードごとに位置・半径が異なる滑らかなスポットの集合 |
| Local Random Noise | 基底 Local Random Phase。局所窓内の複数周波数・ランダム位相の合成として定義 |
| Chevron Noise | 基底 Chevron。シード付き位相を持つシェブロン模様。XY方向のパターンをZ方向へ延長 |
| Lattice-Free Noise | 基底 Lattice-Free。空間格子の補間を使わない16本のランダムフーリエ波 |
| Phasor Noise | 基底 Phasor。局所複素ガボール場を合成し、その位相のcosを返す |
| Anisotropic Gabor Noise | 基底 Anisotropic Gabor。方向を揃え、横方向の包絡を圧縮したガボール核 |
| Harmonic Noise | 基底 Harmonic。シード付き基本波と16の整数次倍音を合成 |
| Flow Noise | 基底 Flow。格子勾配を位相で回転させる時間変化可能な勾配ノイズ |
| Domain Warping Noise | 既存合成 Domain Warp |
| Multifractal Noise | 合成 Multifractal。各オクターブのオフセット信号を乗算 |
| Heterogeneous Multifractal Noise | 合成 Heterogeneous Multifractal。累積標高に応じて次の詳細量を変化 |
| Ridged Multifractal Noise | 既存の合成 RidgedをUIでRidged Multifractalと明記。稜線の二乗と前段フィードバックを利用 |

## パラメーター

従来の Frequency、Seed、Octaves、Lacunarity、Gain は引き続き共通。「フラクタル：なし」なら基底を単独で評価する。

- カーネル周波数：Gabor、Anisotropic Gabor、Phasor、Local Random Phase の振動数（0.25〜8）。
- 方向：Anisotropic Gabor、Phasor のXY方向。内部はラジアン。
- 異方性：上記2方式の横方向の包絡圧縮率（1〜8）。
- 位相：Gabor系、Phasor、Local Random Phase、Flow、Chevron。内部はラジアン。Flowは平行移動ではなく勾配を回転する。
- Ridge オフセット：既存Ridged Multifractal、Hybrid Multifractalに加え、Heterogeneous Multifractalにも適用。

新しい4つの値はGPU定数にも転送する。定数は208バイト、ルート署名は合計58 DWORDで64 DWORDの制限内。

## 実装方式の範囲

Wavelet (Compact) の核は3Dの放射状バンプ `(1-r²)^5` のラプラシアンを定数倍したもの。核は積分ゼロで、サポート境界で値と勾配がゼロになる。Cook–DeRoseの事前計算タイルや投影フィルタリングを再現したものではなく、厳密な帯域制限は行っていない。

カーネル系は1セルに2つのジッター付きインパルスを置く、計算量を固定した方式。包絡のサポート半径は1セルで、周囲27セルを調べる。Poisson点過程そのものを生成する実装ではない。Gabor系はXY方向に振動し、包絡はZも含む3D空間で評価する。

`ProcStudio::NoiseDetail::CurlVector(x,y,z,seed,period)` は3成分を返す。ベクトルを正規化すると発散ゼロの性質を失うため、ベクトルAPIでは正規化しない。スカラー基底では `curl.z/(1+abs(curl.z))` に写像する。

White、Cell、Voronoi IDには本来の不連続がある。Phasor場の零点にも位相の特異点がある。これらを高さ・法線に使用すると鋭い変化が現れる。

## 検証

Windows / Visual Studio:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/test-procedural-noise.ps1
powershell -ExecutionPolicy Bypass -File scripts/test-procedural-texture-patterns.ps1
powershell -ExecutionPolicy Bypass -File scripts/test-procedural-gpu.ps1
```

ヘッダー単独テストはLinuxでも実行できる:

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Werror tests/procedural_noise_tests.cpp -o /tmp/noise-tests
/tmp/noise-tests
```

今回の実行環境では、ヘッダーテスト、全22基底の色・高さ・法線画像／ボリューム／地形生成、既存の幾何学模様・樹木法線・シミュレーションテストを実行。CPUの既存方式は変更前と10,752サンプルを比較し完全一致。Windows用PCHはテスト用コピーから除外し、シミュレーションの既存ヘッダーにある非修飾`isfinite`はテスト専用のインクルードで補った。

DXC 1.9.2609でProceduralTextureCSのGenerate / Normals / PreviewをSM6.0、警告をエラーとしてコンパイル。GPU比較テストは全22基底・全9合成・タイル有無を回すよう拡張した。Windowsアプリ全体のビルドとD3D12上の数値比較・実機パフォーマンス測定は、このLinux環境では未実行。

## 理論の参照先

コードの転載ではなく、それぞれの考え方を参考に独立実装した。

- Gabor: https://graphics.cs.kuleuven.be/publications/LLDD09PNSGC/
- Phasor: https://mfx.loria.fr/software/2023/04/28/phasornoise.html
- Cook–DeRoseのWavelet Noise（今回のCompact方式との比較用）: https://graphics.pixar.com/people/derose/publications/WaveletNoise/paper.pdf
