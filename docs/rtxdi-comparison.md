# RTXDI 実機比較

テストコードを生成せず、Release の実レンダラーを同一バイナリ内で切り替えて計測する。
保存設定は実行前の状態に復元する。結果は `bin/validation/render-benchmark/` に保存する。

## 切り替え

全条件を一括で測る場合は `scripts/measure-rtxdi.ps1 -Configuration Release`。
決定論的な全ライト評価、静的・動的直接光、レイトレーシング併用、既存GIを固定したDI比較、
DIを固定したGI比較、DI/GI同時切り替えの7条件を順番に実行する。
`-Cases direct-static,restir-gi` などで対象を絞れる。ビルドは最初の1回だけ実行する。
既定の内部解像度は384×216、出力1280×720。`-NativeResolution` を付けると内部も1280×720にする。

| 対象 | 選択肢 |
| --- | --- |
| 直接光 `-BaselineLighting` / `-CandidateLighting` | `saved`, `deterministic`, `megalights`, `rtxdi` |
| GI `-BaselineGi` / `-CandidateGi` | `saved`, `raw`, `native`, `rtxdi` |
| 共通の描画条件 `-LightingWorkload` | `saved`, `direct`, `raytracing`, `restir` |
| 動的光源 | `-DynamicLights`：既存の有効な Point/Spot/Rect を移動し、強度を変化 |
| 起動シーン | `-Scene asset/Scenes/MegaLights_NightCity_2000.scene` など |

`direct` は DXR 直接影を維持し、GI・反射・AOを停止する。
`raytracing` は DXR の影・反射・AO と再利用なしの追跡 GI、`restir` はその GI を ReSTIR にする。
GI 選択を明示した場合は `raw` / `native` / `rtxdi` が再利用方式を上書きする。
GI 比較では放射輝度キャッシュを停止し、別方式の影響を減らす。

```powershell
# 直接光。既定は120フレーム準備 + 600フレーム計測、AB/BAの2組。
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/measure-rendering.ps1 -Configuration Release -BaselineLighting megalights -CandidateLighting rtxdi -LightingWorkload direct

# 動的な多数光源と、反射・AO・既存 ReSTIR GI を併用した直接光比較。
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/measure-rendering.ps1 -Configuration Release -SkipBuild -BaselineLighting megalights -CandidateLighting rtxdi -BaselineGi native -CandidateGi native -LightingWorkload restir -DynamicLights -Scene asset/Scenes/MegaLights_NightCity_2000.scene

# RTXDI直接光を固定し、既存ReSTIR GIとSDKリザーバー統合を比較。
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/measure-rendering.ps1 -Configuration Release -SkipBuild -BaselineLighting rtxdi -CandidateLighting rtxdi -BaselineGi native -CandidateGi rtxdi -LightingWorkload restir -DynamicLights
```

エディターの評価方式 `RTXDI` と `RTXDI SDK で GI リザーバーを再サンプリング` から通常実行でも選べる。
プロセス単位の環境変数は `DX12_DIRECT_LIGHTING=deterministic|megalights|rtxdi` と
`DX12_GI_RESAMPLING=raw|native|rtxdi`。保存設定の比較用ハッシュは変えない。

## 実装範囲と制約

- DI：同梱SDKの `RTXDI_StreamSample`、`RTXDI_CombineDIReservoirs`、`RTXDI_FinalizeResampling` を使用。
  グリッド内の一様選択と全ローカルライトの一様選択を混ぜた初期候補（既定8個）を作り、
  時間再利用と近傍4点の空間再利用を行う。一様混合率を全体から選ぶ割合として使い、混合PDFで重み付けする。
  既存の材質評価、影レイ、スクリーントレース、SVGF/NRDを共用する。
- DI履歴：解像度・サンプル数・カメラEntityが変わると破棄する。
  Entity IDから現在のライト番号へ引き直すので、カメラ移動に伴う並び替えや候補集合の変更にも対応する。
  選択したライトの位置・強度などが変わった履歴は保守的に拒否するため、全光源が動く条件は
  主に初期候補と空間再利用の比較になる。消えたライトや別のEntityになった履歴も拒否する。
- GI：SDKの `RTXDI_CombineGIReservoirs` と `RTXDI_FinalizeGIResampling` による統合・正規化の切り替え。
  追跡、ターゲット関数、再投影、近傍選択、ヤコビアン、可視性、保存形式は既存方式を共用する。
  SDK高水準の時間・空間再利用パイプライン全体、ReGIR、ReSTIR PTの移植ではない。
- DXR、TLAS、既存のライトグリッドは共通基盤として必要。Forward、半透明、フォグは既存経路を維持する。
- DIは基本MIS正規化で、可視性を含めた不偏補正を実装していない。候補数や履歴により画質が異なる。
  速度差だけで採用を判断せず、同じフレームの画像・ノイズ・光漏れ・残像も確認する。
- DI追加バッファは3枚、サンプルあたり合計288 byte、上限384 MiB。予算に合わせてサンプル数を下げ、
  1サンプルでも収まらなければ半解像度にする。実際のサンプル数・縮小率はCSVへ記録する。
  MegaLightsとの比較でこれらが一致しなければ比較を拒否する。確保失敗は明示的なフォールバック。
  比較中に要求経路が動かなければ計測を失敗させる。

`frames.csv` は実際の直接光方式、ローカルライト数、DI履歴有効性、追加バッファ量、GI有効性も記録する。
GPUパスは `RTXDI Initial + Temporal` / `RTXDI Spatial` / `RTXDI GI Temporal` / `RTXDI GI Spatial`。
比較では明示した直接光・GI方式以外の条件と、カメラ・光源の再生署名が一致することを確認する。
同じバイナリでも機能が異なれば機能A/B比較であり、`BinaryComparison=A/A` はバイナリ同一性だけを表す。

参考：[NVIDIA RTXDI Integration Guide](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/Integration.md)。
