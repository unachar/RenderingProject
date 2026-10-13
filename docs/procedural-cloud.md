# Procedural Volumetric Cloud MVP

`dev_main` のDeferred HDRパイプラインへ、一層のWorld Spaceボリュームクラウドを追加した。外部Cloud Shape Textureは使用しない。既存の変更との干渉を避けるため、初期状態はOFF。

## 使用方法

Editorの描画設定にある **Procedural Cloud** を開き、**Enable clouds** をONにする。Deferred RenderingとHDR Sceneを使用する。

- Shape: Coverage、Density、高度、Base/Detail Scale、Detail Strength。
- Wind: X/Z方向と速度。方向は正規化し、ゼロベクトルは静止扱い。
- Lighting: Extinction、HGのG、Ambient、Powder。Powderの初期値0で無効。
- Quality: Low=40/4、Medium=64/6、High=96/8。全PresetはHalf Resolution。Resolution ScaleでFullへ変更可能。
- Noise: Seedを変更すると次のCloud Passで再生成。同一SeedのRegenerateも可能。Noise Sliceは診断表示用。
- Debug View: Normal、Density、Transmittance、Lighting、Cloud Depth、Step Count、Base/Detail Noise Slice。
- GPU時間: Cloud Raymarch、Cloud Composite、Cloud Totalを既存RenderProfilerから表示。

初期LayerはWorld Y=20〜40、Max Trace Distance=2000。大きなスケールのシーンでは高度、Noise Scale、Extinctionを合わせて調整する。UI値は実行時設定であり、現時点ではプロジェクト保存対象に追加していない。

## 実装と接続

- `source/rendering/cloud/cloudrenderer.*`: GPUリソース、PSO、Noise生成、Raymarch、Composite、UI。Camera/Scene/Lightの管理は持たない。
- `cloudsettings.h`: 安全な範囲への補正、品質/天候Preset。非有限値、薄いLayer、ゼロWindを処理する。
- `shader/hlsl/CloudNoise.hlsli`: 周期的なPerlin/WorleyとFBM。整数格子を各octaveの周期でWrapする。
- Base=128³、Detail=32³。R8_UNORMのTyped UAV Store対応を問い合わせ、未対応時のみR16_FLOATを選ぶ。
- 初回の有効なCloud Pass、Seed変更、明示的Regenerate時だけCompute Dispatchする。
- NoiseはUAV生成後にPixel Shader Resourceへ遷移する。
- Half Resolution ColorはR16G16B16A16_FLOAT。RGB=散乱光、A=透過率。
- 深度はR32G32_FLOAT。X=Scene Ray Distance、Y=Opacity Weightで重み付けしたCloud Distance。
- フライト中の各フレームがColor/Depth/定数とDescriptorを持つ。品質変更ではFence待機済みのフレームだけ再作成する。Scene Resize時は既存のGPU待機後に全フレームをリセットする。
- SRV/UAVは既存Engine Heapの予約範囲、RTVは既存Scene RTV Heapの追加範囲を使用する。
- 既存`postProcessVS`のFullscreen Triangle、PostProcess Camera CB、Light CBを直接再利用する。CPU/HLSL Cloud CBは16-byte単位の7グループ、112 bytes、配置をstatic_assertし、Uploadのフレーム間隔は256 bytes。

接続位置はDeferred Lightingの直後、SceneをSRVへ戻す前。Scene DepthでRay Endを制限する。Materialの背景判定は既存DeferredLightingと一致させ、Sky Meshを遮蔽物として扱わない。

CompositeはSceneを読みながら同じRTへ書く構成を避け、`ONE / SRC_ALPHA`のRGBブレンドで `Scattering + Scene * Transmittance` を計算する。Scene Alphaは保持する。近傍Cloud SampleのScene DistanceでBilateral Upsampleし、対応する近傍がない輪郭ピクセルではFull Resolution Raymarchへフォールバックする。合成後にPostProcess、Exposure、Tone Mappingへ進む。

密度はHeight Gradient、Coverage Threshold、Detail Erosionから計算。空のSampleでは照明処理を省略し、透過率0.01未満で終了する。光学モデルはBeer-Lambert、HG、太陽方向への短いRaymarch。太陽は共有Light CB内のDirectional Lightを使用し、Ambient色は既存Atmosphere定数から取得する。

Windの蓄積には共有Delta Timeとdoubleの移動量を使用する。GPUへ渡すBase/Detail UV Offsetはそれぞれの周期内へ折り返し、長時間移動のfloat精度低下を抑える。

## 検証

最終Cloud検証はDebug / Release / GBVで各292フレーム成功。DebugとGBVのD3D12 Errorは0、Cloudに追加されたResource / Descriptor / Barrier Warningは0。GBVには後述の既存追跡制限がある。Settings単体テスト、既存Editor 976項目、Simulation Math 2801項目も成功した。

再実行:

```powershell
./scripts/validate-clouds.ps1 -Configuration Debug
./scripts/validate-clouds.ps1 -Configuration Debug -GpuValidation
./scripts/validate-clouds.ps1 -Configuration Release
```

検証用Sceneに前景の塔を配置し、Cameraの下/内部/上、水平/真上/真下、Teleport、Coverage=0/1、Density=0/10、極薄/厚いLayer、静止/高速Wind、光量0/100、Resize、Render Scale、Full Cloud Resolution、Seed変更と復帰、全診断表示を292フレームで実行する。

GPU readbackで有限値・非負の散乱光・透過率[0,1]・Bufferサイズを検査。同じSeedの生成結果はpaddingを除いた全VoxelのHashで比較し、生成回数も検査する。Camera/Seed復帰時には積算Opacityが復帰することを確認する。

低解像度の遮蔽テストは前景内部を使用する。整数UV境界でCPUとRasterizerが隣接Texelを選ぶ場合があるため、輪郭の検証はFull ResolutionのHDR結果で行う。雲あり/なしの前景RGBを比較し、輪郭を含めて完全一致することを確認する。

GPU TimingはSnapshot Serialを記録済みの設定・実際のBufferサイズと対応付ける。1920×1080の各品質を60フレーム実行し、最初の10フレームを除いた50フレームから中央値とp95を算出する。

Release最終確認値（RTX 3050 Laptop GPU、Cloud Half Resolution=960×540、前景の塔を含む検証Scene）:

| 品質 | View / Light | Median | p95 |
|---|---:|---:|---:|
| Low | 40 / 4 | 2.028 ms | 2.059 ms |
| Medium | 64 / 6 | 3.607 ms | 3.694 ms |
| High | 96 / 8 | 6.555 ms | 7.014 ms |

Mediumは暫定3.5 ms目標を少し超えている。Noise生成はこの定常値に含めない。GPU Based Validationの測定値は診断オーバーヘッドを含むため、この性能評価には使用しない。Sceneの雲量、視点、輪郭フォールバックの面積、GPUクロックにより負荷は変わる。

結果と画像は`bin/validation/Debug-clouds`、`Debug-clouds-gbv`、`Release-clouds`へ保存する。既存Readback BufferのCOPY_DEST初期状態とScene PassのOptimized Clear Value不一致には既存のWarningがある。Cloud検証は該当する2つのMessage IDを件数付きで報告し、その他のWarningと全Error/Corruptionを検査する。全Warningが0という意味ではない。

GBVでは既存Shadow/Velocity等のExecuteIndirectがVB/IB/Root Bindingを変更するため、状態追跡の精度制限（ID 1044）が出る。雲OFFの既存Sun Lightingテストでも同一警告を再現した。Cloud検証では件数を別途表示し、スクリプトも制約を通知する。無制約のGBV検証が完了したという意味ではない。基準ログは`bin/validation/Debug-gbv-baseline.txt`。この基準テストでは既存Raytraced Sun Shadow/AOのCompute SRV Resource Stateエラーも検出された。雲検証はDXRを無効にして実行する。

GBVの切り分け中、通常Opaque描画への一時切替ではResize付近でDevice Hungが発生したため切替を取り消した。別の既存DXRテストもGBVではタイムアウトした。これらの診断実行を成功扱いにせず、通常のDebug/Release検証と区別する。

この環境ではDebugのPDB Manager不整合を`/Z7`で回避している。Releaseの増分LTCGと埋め込みデバッグ情報の不整合には、検証スクリプト内で専用IntDirとWholeProgramOptimization=falseを使用する。通常のRelease `/O2`は維持し、製品のプロジェクト設定は変更していない。

## 範囲と残る確認

既存Pipeline全体の回帰テストは未合格。雲OFFのDebug features実行はHDR/DLSS切替付近でDevice HungとRTV形式不一致を検出した。同じエラーは作業開始前の`build/sun-general-validation-failed.log`（2026-10-13 14:26）にも記録されている。DXR/DLSSを無効にしたFallback実行も異常終了した。これらをCloud検証の成功と混同せず、既存描画系の残る問題として扱う。ログは`bin/validation/Debug-features/init_log.txt`、`Debug-fallback/init_log.txt`。

追加前のHEAD `eaaa3be3`を別フォルダへ展開・ビルドし、同じFallback設定（Mesh Page Limit=1024）で再実行したところ、Cloud変更あり/なし双方が終了コード2173で中断した。比較ログは`bin/validation/head-baseline/baseline-fallback-log.txt`。依存フォルダは既存のものを参照し、作業ツリーを戻さず比較した。全Pipelineの成功証明は依然として未完了。

クラウドの統合対象はDeferred Pipeline。Forward専用の深度入力経路は追加していない。Temporal Reprojection、Weather Map、地表Cloud Shadow、Multi Layer、Atmosphereとの完全な物理統合は仕様どおりMVP外。

ネイティブFullscreenの手動切替、Editor UIの操作一式、広い実Sceneでの長時間プレイは別途確認が必要。ResizeとRender ScaleのGPUリソース経路は自動検証する。ジッターはTemporal Historyを持たない安定したScreen Space IGNなので、極端に少ないSample数では粒状感が残る。
