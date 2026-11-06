# 仮想太陽と雲の接続

空が使う最初の `SunComponent` をCPU側で特定し、Light Buffer内のindex+1を `SkySunDirection.w` へ渡す。0は太陽なし、無効、または描画予算から除外された状態。雲は同じ方向と `LightColors.rgb * LightColors.a` を使い、他の平行光源を太陽として代用しない。昼夜サイクル、手動移動、色、強度、IsActiveの変更が反映される。

`CloudMedium.hlsli` に雲層区間、密度、Wind UV、消散係数、Beer–Lambert透過率を集約した。地表からの積分は雲層の入口から出口まで行う。太陽円盤を角半径0.00465rad、4方向でサンプリングし、雲内部と地表で同じ透過率を使う。正のHG gは太陽を向いた視線で前方散乱が強くなる。

Deferred Lightingで仮想太陽の直接照明だけに雲の透過率を掛ける。Raster/DXR/Contact/Distance Fieldの遮蔽を取得した後に適用するため、DXRで雲影を上書きしない。直接光のdiffuse/specularが同じ減衰を受け、環境光や他の光源は雲影で暗くしない。背景の太陽円盤は既存の `Scattering + Scene * Transmittance` 合成で雲に隠れる。

Noise生成・Wind更新・定数UploadはDeferred Lightingの前に一度だけ実行し、雲描画でも同じ状態を使用する。フレーム別Uploadは112bytes/256bytes strideを維持。PostProcess root signatureへNoise SRV t31/t32、Cloud b9とWrap sampler s3を追加した。初期化失敗時には有効なnull volumeと無効定数をバインドする。

使用するにはDeferred Pipelineの **Procedural Cloud > Enable clouds** をONにする。**Cloud shadows** は既定ON。OFFにすると地表影だけ無効になり、雲内部の自己陰影と散乱は維持する。設定は従来どおり実行時のみ。

## 雲の隙間からのライトシャフト

フロクセルフォグと従来の大気レイマーチは、選択された太陽について雲の透過率を散乱光へ反映する。
雲が遮る領域では散乱が減り、隙間を通る光が筋として残る。雲が有効な場合は物理空の背景にも
フロクセルフォグを合成する。正の Mie g は太陽を向いた視線で前方散乱を強める。

`Cloud shadows` を有効にし、大気の「ライトシャフト強度」とフロクセルフォグの散乱強度で明るさを調整する。
高い雲まで光を見せる場合は大気の高度減衰を弱め、フォグの遠距離も対象の雲に合わせる。
極端に高い Mie g は光を太陽付近へ集中させるため、広がる光芒の確認には 0.65～0.8 程度から調整する。

フロクセルは中心方向への雲の透過率を使い、従来の大気パスは雲影を建物の影と同じ4ステップ間隔で更新する。
ノイズはピクセルとコンピュートの両方から読める状態に保つ。フォグの履歴を無効にした場合は
深度サンプルと合成のディザーを固定し、時間方向のちらつきを抑える。

`scripts/validate-clouds.ps1 -Configuration Debug` は364フレームを実行する。
末尾で太陽・カメラ・風・フォグのサンプルを固定し、雲影のON/OFFによって物理空のHDR散乱が変化することを確認する。
比較画像は `cloud-shafts-shadowed.png` と `cloud-shafts-unshadowed.png`。

## 近似と範囲

実時間向けのHG単一散乱・数値積分である。密度は手続き的な無次元分布、Extinctionはworld unitの逆数として扱う。既存Ambientとユーザーが有効化したPowderは芸術的な近似。大気と雲を通した分光輸送、多重散乱、物理単位での太陽放射較正、Forward描画の地表雲影は実装していない。光路はMax Trace Distanceで制限され、低いLightStepsでは細かい影の誤差が残る。4点の円盤サンプルは半影の近似であり、厳密な円盤積分ではない。

追加の地表積分コストはDeferred Lighting、自己陰影の4方向積分はCloud RaymarchのGPU時間へ入る。MVPの旧性能値を現在の速度として使わない。

物理式の参照: [PBRT Transmittance](https://www.pbr-book.org/4ed/Volume_Scattering/Transmittance)、[PBRT Phase Functions](https://www.pbr-book.org/4ed/Volume_Scattering/Phase_Functions)。

## 検証

`./scripts/validate-clouds.ps1 -Configuration Debug`。ReleaseとGpuValidationも指定可能。追加前に「clouds cast measurable shadows on foreground receivers」が失敗することを確認した。

308フレームの検証には、実際のHDR地表が雲で減衰すること、雲影切替で雲散乱が完全一致すること、優先度・強度の高い別平行光源が雲照明を置き換えないこと、太陽無効時に別光源があっても直射成分が減ることを含む。D3D12の既存WarningとGBV追跡制限はMVPと同じ基準で別途報告する。

DebugとReleaseの308フレーム検証は合格。DebugではD3D12 Error=0、追加Resource/Descriptor/Barrier Warning=0。ReleaseではD3D12 Debug Queueが利用できないため、その診断はDebug側の証拠による。独立コードレビューで修正必須の指摘はなかった。

検証SceneではDXRを無効にしている。DXRの遮蔽を取得した後に雲影を適用する順序はコードで確認したが、DXR併用の実描画は未確認。ユーザーの実Sceneでの見た目・長時間操作も別途確認が必要。

Release測定（1920×1080、雲960×540、各50サンプル）のCloud Total中央値/p95はLow=5.174/5.321ms、Medium=11.677/12.308ms、High=20.979/22.975ms。地表雲影の積分はDeferred Lightingに含まれるため、この数値は影を含むフレーム全体の負荷ではない。太陽円盤を4方向で積分するためMVPより重い。GPUクロックやSceneで変動する。

GPU検証の最初の実行は終盤で従来の240秒制限に達して中断した。この実行は合格扱いにしない。GpuValidation指定時のみ制限を600秒に延長した。

延長後のDebug GPU Based Validationは308フレーム完走・合格。D3D12 Error=0、Cloud失敗=0、追加Resource/Descriptor/Barrier Warning=0。既存のReadback/Scene Clear Warning=354件、ExecuteIndirectに由来するGBV追跡制限（ID 1044）=1件があり、無制約の検証成功ではない。結果は `bin/validation/Debug-clouds`、`Release-clouds`、`Debug-clouds-gbv`、Releaseビルドログは `build/cloud-sun-release.log`。
