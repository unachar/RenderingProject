# RTXDI 実機比較結果（2026-11-02）

既存方式を標準として維持する。今回のRTXDI実装では明確な品質改善を確認できず、直接光のGPU時間は増えた。GIコアの差は小さく、採用を裏付ける結果ではない。RTXDIは明示的に選んだ場合の検証用として残す。

## 条件

- NVIDIA GeForce RTX 3050 Laptop GPU（4 GB）、ドライバー617.14、Release。
- `asset/Scenes/MegaLights_NightCity_2000.scene`（ローカル2000灯＋太陽）。画面内の実送信灯数は結果JSONに記録。
- 出力・内部とも1280×720。確率的直接光は両方式とも半解像度640×360、1サンプル/ピクセル。RTXDI初期候補8個。
- 各条件で準備60フレーム＋計測150フレーム、既存→候補のA/Bを1組。14プロセス、全2100 GPUサンプルが有効。
- カメラと動的光源のフレーム単位の再生署名、実解像度、保存設定、シェーダーハッシュを照合済み。
- 同一バイナリ内の機能A/B。比較JSONの `BinaryComparison=A/A` はバイナリ同一性だけを意味する。
- 短時間測定のため、特に1%未満の差を高速化の証拠に使わない。順序反転を含む長時間の採用検証ではない。

バイナリ SHA256: `6F247A91404E1B345A3BE36BD6367A501B8B302F6457BE899E8FF9F645752203`

## 平均GPU時間

正の増減率はRTXDI側が遅いことを示す。行をまたぐ時間差ではなく、各行内の同一条件のA/Bを比較する。

| 条件 | 既存側 ms | RTXDI側 ms | GPU時間増減 |
| --- | ---: | ---: | ---: |
| 既存の全ライト評価 → RTXDI DI | 18.798 | 28.115 | +49.57% |
| 確率的MegaLights → RTXDI DI（静的） | 26.778 | 28.144 | +5.10% |
| 確率的MegaLights → RTXDI DI（動的） | 26.857 | 27.487 | +2.35% |
| 反射・AO・追跡GI併用でDI交換（動的） | 40.409 | 41.904 | +3.70% |
| 既存ReSTIR GIを固定してDI交換（動的） | 41.254 | 43.452 | +5.33% |
| RTXDI DIを固定してGIコア交換（動的） | 44.950 | 44.710 | -0.53% |
| DI・GIコアを同時交換（動的） | 43.716 | 44.598 | +2.02% |

## 品質と実装範囲

最終キャプチャを目視で確認した範囲では、RTXDI側の明確な品質優位は見つからなかった。画像は完全一致せず、同等品質を証明したものではない。基準画像に対する誤差、長時間のちらつき・残像、さまざまな材質・シーンでの品質は未検証。

DIはSDKの初期RIS・時間/空間リザーバー統合・基本MIS正規化を使う実験経路。GIはSDKのリザーバー統合・正規化コアを交換し、既存の追跡・再投影・近傍選択・ヤコビアン・可視性判定を共用する。SDK高水準パイプライン全体、ReGIR、ReSTIR PTの移植ではない。Forward・半透明・フォグは既存経路を使う。

動くライトは変更されたライトの履歴を拒否する保守的な方式。時間方向の画質優位を示した結果ではない。DI追加リザーバー割当はこの条件で63.28 MiB。GI SDKコアの交換ではバッファ形式を維持している。

## 成果物と再実行

[切り替えと再実行手順](rtxdi-comparison.md)。テストコードの生成は行わず、実際のReleaseレンダラーを実行した。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/measure-rtxdi.ps1 -Configuration Release -SkipBuild -NativeResolution -WarmupFrames 60 -MeasuredFrames 150 -Pairs 1
```

`-Pairs 2` ならA/BとB/Aの順序反転、既定は準備120＋計測600フレーム。

各ディレクトリに `manifest.json`、`frames.csv`、`passes.csv`、`summary.json`、`final.png`、プロセスごとのログを保存。CPU/フレーム時間、パス別GPU時間、灯数、バッファ量を集計したJSON:

[全結果JSON](../bin/validation/render-benchmark/rtxdi-reviewed-results-20261102.json)

| 条件 | 実行ID |
| --- | --- |
| deterministic | `d7dc01812296-0-baseline` / `d7dc01812296-0-candidate` |
| direct-static | `9318045d1c6f-0-baseline` / `9318045d1c6f-0-candidate` |
| direct-dynamic | `eada19b9ab69-0-baseline` / `eada19b9ab69-0-candidate` |
| raytracing | `64922f7b2d3e-0-baseline` / `64922f7b2d3e-0-candidate` |
| restir-di | `9398e7d7acc9-0-baseline` / `9398e7d7acc9-0-candidate` |
| restir-gi | `486044bcc912-0-baseline` / `486044bcc912-0-candidate` |
| combined | `4d407a0e7dff-0-baseline` / `4d407a0e7dff-0-candidate` |

今回の結果は `build/rtxdi-reviewed-matrix.log` の7条件のみを採用。修正前の診断・中断した計測は除外した。

## 検証と併せて修正した項目

- Releaseビルド成功（`build/rtxdi-release-build.log`）。実行したバイナリSHAは上記と一致。
- DI初期/空間、GI時間/空間、既存SHaRC用16bitシェーダーのDXCコンパイルを確認。
- デノイズ用PSOがSM5頂点シェーダーとSM6ピクセルシェーダーの混在で拒否される既存問題を修正し、実機でデノイズ用パス作成成功を確認。
- Entity IDによるライト番号の引き直し、履歴バッファの予算・アラインメント・同期、選択元に応じたMIS正規化を確認。
- GPUベース検証の全完走は未実施。重い初期化を伴う診断は中断し、D3D12デバッグレイヤーでPSO失敗原因を特定した。
- 起動シーンと保存設定は終了時に元へ復元。`megalights.EvaluationMode=0` を維持し、`quality.RtxdiGI` の既定値はfalse。
