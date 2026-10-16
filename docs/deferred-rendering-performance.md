# レンダリング負荷の切り分けと最適化（2026-10-16）

## 計測条件と判断

RTX 3050 Laptop GPU、Release x64、現在の起動シーン。出力1200×600、
内部描画600×300、反射のみのDXR＋VSM、既存の画質設定を維持。
`scripts/validate-play-rendering.ps1 -Configuration Release -Benchmark` の
`optimized-play` 区間49フレームを比較した。これは旧VSM方式との比較ではなく、
VSM最適化済みの同じ経路における、今回の変更前後の比較。

変更前のGPU全体は7.5610ms。Shadowは2.3820ms、GBuffer / Opaqueは1.0987ms、
DXR加速構造は0.8671ms、DXR Reflectionsは0.9528ms、Atmosphereは0.4963ms、
Deferred Lightingは0.3382ms、Cloud Totalは0.6628msだった。

したがって、Deferred Lighting単体が最大の負荷ではない。
`Deferred Lighting / PostProcess` の3.6144msにはDXR、雲、大気、後処理が含まれる。
GBuffer / OpaqueにもカリングやHi-Zなどが含まれるため、GBuffer書き込み帯域だけの時間ではない。
親子パスの時間は重複するので合算しない。

## 実装

- DXRのBLAS入力リソースをまとめて読み取り状態へ遷移させ、同じリソースの遷移を重複させない。
- 独立したBLASビルドを連続して発行する。各BLAS固有のscratch領域を維持し、
  完了バリアと入力状態の復元をまとめてTLAS構築前に実行する。
- 同じフレーム枠の旧TLASが先行する照明処理で参照される場合に備え、
  既存BLASへの書き込み前にもUAVバリアを発行する。
- 割り当て失敗で途中終了しても、発行済み処理の完了バリアと入力状態の復元を行う。
- 同じ解像度で処理するDeferred LightingのGBuffer参照を整数座標のLoadに変更。
  半解像度になり得る大気テクスチャの補間は維持する。
- PBRでは使用しない旧照明用の環境テクスチャ参照と計算を、対象材質の分岐内へ移す。
  Emission/AOの取得は背景・非照明材質などの早期終了後へ移す。
- 既存ベンチマークに、各パスのGPU/CPU平均・direct draws・有効GPUサンプル数を追加する。

BLASビルド間の依存を省く条件と、利用前のUAVバリアは
[Microsoft DXR仕様](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html)
に従う。解像度、サンプル数、GBuffer形式、影品質は変更していない。

## 描画と安全性の検証

- Debugビルドと`validate-play-rendering.ps1 -Configuration Debug`が合格。
  D3D12エラー0、影4レイヤーの通常方式との比較と静止影キャッシュ再利用後の比較は
  許容誤差1e-5を超える画素0（最大深度誤差0.00000069）。
- `validate-materials.ps1 -Configuration Debug -SkipBuild -Transparency`を
  新旧のDeferred Lightingシェーダーでそれぞれ実行し、両方とも失敗0・D3D12エラー0。
  GBuffer 6ターゲットのCPU/GPU経路一致、法線、発光、AO、透過影、
  Deferred/ForwardのHDR照明比較を含む。
- 変更前のHLSLをGit HEADから取り出しFXC `/O3`で別途コンパイルして画像を比較した。
  10画像中8画像は完全一致。`cpu.png`と`gpulod.png`は各720,000画素中41画素に差があり、
  RGBの最大差は8bit値で3、平均絶対差は0.000030。完全なピクセル一致とは扱わない。
  テスト後に最適化版CSOを復元している。

ログと画像は`build/render-baseline.log`、`build/render-optimized.log`、
`build/render-debug-console.log`、`build/deferred-comparison/`に保存。
Release計測ではD3D12 debug queueは利用できず、安全性の確認はDebug実行で行った。

## 最終コードのRelease計測

| GPUパス | 変更前 | 変更後 |
| --- | ---: | ---: |
| 全体 | 7.5610 ms | 6.8317 ms |
| Shadow | 2.3820 ms | 2.3331 ms |
| GBuffer / Opaque | 1.0987 ms | 1.1134 ms |
| DXR加速構造 | 0.8671 ms | 0.1965 ms |
| DXR Reflections | 0.9528 ms | 0.9370 ms |
| Deferred Lighting | 0.3382 ms | 0.3358 ms |
| Deferred Lighting / PostProcess（親パス） | 3.6144 ms | 2.9191 ms |

GPU全体は約9.6%、加速構造更新は約77.3%短縮。GBufferとDeferred Lighting単体には
有意な短縮を確認していない。最終ログは`build/render-final.log`。

一方、CPU Renderは5.482→6.700ms、フレーム時間は9.391→11.025ms、
FPSは106.5→90.7だった。GPU時間の短縮をFPSの向上とは扱わない。
実時間で進むアニメーション、VSM更新ページ、CPU負荷などが異なる別プロセスの短期計測であり、
厳密に同一ポーズを再生する統計的な比較ではない。最初の最適化版計測ではShadowが
3.0984msに変動し、GPU全体も7.6348msだったが、加速構造更新は0.2000msだった。
再測定でも再現した加速構造の短縮が、この変更の最も明確な効果。

## 残る負荷

優先順位はVSM更新、GBuffer内のジオメトリ処理、DXR反射トレース／デノイズ。
GBufferの追加圧縮や深度ターゲットの共用は、精度・全シェーダー・SRV契約に影響するため、
今回の変更には含めていない。通常操作時や別シーンのFPS、全GPUでの改善は未検証。
