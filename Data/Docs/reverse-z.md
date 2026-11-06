# 無限遠 Reverse-Z

シーンカメラとマテリアルプレビューは左手系の無限遠透視投影を使う。
`source/rendering/scenedepth.h` の行ベクトル用行列では、ビュー空間の距離 `z` に対して
`depth = NearClip / z` となる。近面は 1、背景・無限遠は 0。
射影行列の `_33 = 0`, `_34 = 1`, `_43 = NearClip`, `_44 = 0`。
ジッターは従来どおり `_31`, `_32` に加える。

- シーン深度は D32_FLOAT、クリア値は 0。通常描画は GREATER、深度再利用は GREATER_EQUAL。
- Hi-Z は最小値を縮約し、背景の穴を保持する。物体の最近深度は最大値。
- 背景判定は深度 0 を使う。正の微小な深度を背景と判定しない。
- POM・インポスターは SV_DepthLessEqual を出力し、補間深度以下に制限する。
- Visibility Buffer の GBuffer 復元はハードウェアの32ビット深度を直接読む。
  ペイロード内の16ビット深度は遠景の復元には使わない。
- 無限遠の視錐台平面はゼロ法線を持つため、CPUで正規化する際は無効化する。
- 背景の視線・TSR履歴は同次座標の方向として扱い、`w = 0` で透視除算しない。
- DLSS は DepthInverted フラグを指定する。

シャドウマップ、透過シャドウ、インポスターのアトラスベイクは従来の通常 Z を使う。
カメラの既存 `FarClip` 保存項目は互換性のため保持し、エフェクトの距離設定として扱う。
Inspector では Effect Range と表示する。投影の遠平面を変更する項目ではない。

WorkGraphを除去した構成でも同じ深度規約を使う。可視性バッファの復元は通常の
Compute Shaderで行い、ハードウェア深度をfallbackのt1、GPUDriven経路のt45に渡す。

## 検証

ビルドはVisual Studioのx64開発環境で実行する。

```powershell
MSBuild.exe DirectX12.sln /t:Build /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /m
./scripts/test-reversez.ps1
./scripts/test-temporalupscale.ps1
./scripts/validate-gpuvisibility.ps1 -Configuration Release -SkipBuild
```

Reverse-Zテストは、近面・近面より手前・遠距離の投影と逆投影、無限遠平面の正規化、
実際のHi-Z縮約シェーダーのGPU結果を確認する。Hi-Zは奇数サイズ、背景の穴、微小深度を含む。
TSRのGPUテストも本番の無限遠Reverse-Z行列と背景深度0を使う。
離散LODの可視性テストは元メッシュを直接読み込み、派生ジオメトリの境界拡張や
以前の実行で生成したキャッシュの影響を除いて検証する。

参考: [NVIDIA: Visualizing Depth Precision](https://developer.nvidia.com/blog/visualizing-depth-precision/)
