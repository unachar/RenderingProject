# 反射のみモードでPBRに太陽光が届かない問題

## 原因と修正

反射のみモードは、直接光の影にラスタライズしたShadowMapを使用する。
`ModelSystem::DrawModels` は影の対象判定をAnimation Modelだけに適用していたため、
非表示のStatic ModelのSkyも影マップへ描画され、内部のPBRオブジェクトへの太陽光を遮っていた。
フルレイトレーシングでは環境Meshが遮蔽物から除外されていたため、同じ問題は発生しなかった。

共通描画コンテキストへShadow Passの識別を追加し、Static / Animation Modelの両方へ
影の対象判定を適用する。Velocity Passにはこの判定を適用しない。
環境Meshは編集可能な名前に依存せず、`MeshComponent::IsEnvironment` で影描画と
影Boundsの対象から除外する。Primitive / Instanced Modelの影描画も同じ方針に揃える。

ユーザーの指定に従ってSkyの起動時自動生成を停止した。物理大気による空と太陽は維持する。
Skyのモデルファイルと明示的な生成APIは残し、既存シーンの環境Meshにも影の除外を適用する。

## 回帰試験

```powershell
.\scripts\validate-sun-lighting.ps1
.\scripts\validate-sun-lighting.ps1 -Configuration Release
.\scripts\validate-rendering.ps1 -SkipBuild
```

専用試験は、名前を変更した巨大な環境Static Model内の白いPBR Cubeを描画し、
HDR画像のPBR部分だけを読み取る。通常描画中のReadbackは追加しない。
Shadow無し / 反射のみ＋ShadowMap / 反射のみ＋Virtual ShadowMap / フルDXR / 太陽OFFを比較する。
太陽高度45度と8.9度を試し、低高度の暖色がPBRに届くことも検査する。
DXR比較時は実際にRaytraced Shadowsが有効であることを確認する。
ログは `bin/validation/<Configuration>-sun-lighting/init_log.txt` へ保存する。

修正前は、通常ShadowMap使用時の平均輝度が1.086880（Shadow無し）から0.111416へ低下し、
太陽OFFの0.111194とほぼ同じになった。修正後は1.086788となった。
朝の太陽色の直接光成分は、ShadowMapでRGB=(1.021624, 0.356878, 0.081489)、
フルDXRでRGB=(1.025325, 0.358194, 0.081817)となり、暖色が維持された。

この試験は直接光の不具合を分離するため、時刻による自動更新とデノイザーを停止する。
ユーザーのシーンを操作した目視比較は未実施。

## 検証記録（2026-10-13）

- Debugの専用試験：84フレーム合格、D3D12重大エラー0。
- Releaseの専用試験：全体再ビルド後、84フレーム合格。
  ReleaseではD3D12 Debug Queueを利用しないため、重大エラー0という判定はDebugの結果に基づく。
- CPU試験：Procedural Editor 976項目、Simulation Math 2801項目合格。
- 通常の全機能試験：解像度 / HDRの切替付近で `DEVICE_HUNG` が2回発生したため不合格。
  初回にはRenderTarget形式の不一致も記録された。2回目はGPU停止のみで、
  今回の影描画修正との因果関係は確認できていない。
  初回ログは `build/sun-general-validation-failed.log` に保存した。
- DXR / DLSS無効の通常試験も解像度切替付近で停止した。
  `bin/validation/sun-fallback.log` と `device_removed.log` に記録を残した。
  DREDはCompute Dispatchでの停止とPageFaultVA=0を記録している。
  DLSS固有の問題とは断定できず、全体検証は未合格である。
