# 視差オクルージョンマッピング（POM）

不透明なPBRマテリアルに高さマップを設定すると、視線と高さ場の交差を探索して表面のUVをずらす。
通常のラスター描画、GPU駆動描画、Visibility Resolve、マテリアルプレビュー、サムネイルで共通処理を使う。

## 設定

マテリアルエディターのParallax Occlusion Mapping欄で設定する。

- **Height (POM)**: 線形の高さテクスチャ。Rチャンネルを使い、白が元の表面、黒が最も深い位置。
- **Parallax Depth**: UV単位の深さ。範囲0～0.2。まず0.02～0.05程度で調整する。0で無効。
- **Parallax Steps**: 斜めから見る際の最大探索回数。8～128、初期値32。正面では8回まで減らす。

高さマップにはアルベドと同じUV配置を使う。カラー向けのsRGB変換を含まないデータを用意する。
テクスチャ未設定時と深さ0ではUVを変更しない。旧形式のマテリアルもPOM無効で読み込む。
色、法線、Metallic/Roughness、AO、発光へ同じUV変位を適用し、法線用の接空間は元のUVから求める。

保存形式（version 1へ任意項目を追加）:

```json
{
  "parameters": { "parallaxDepth": 0.04, "parallaxSteps": 32 },
  "textures": { "height": "guid:0123456789abcdef" }
}
```

## 対象と制約

POMは表面内の奥行き表現で、メッシュ形状、輪郭、深度バッファ、モーションベクトル、レイトレース用形状、外部へ落とす影は変更しない。
追加の自己シャドウは行わない。半透明とアルファ切り抜きでは、影・Visibilityの被覆率と整合させるため無効。
縮退UVと裏側では無効にし、地平線付近では変位をフェードする。反転UVの向きも保持する。
UV境界は既存サンプラーのアドレスモードに従うため、繰り返し可能な素材を推奨する。

GPUレコードの既存strideを維持し、予約済み領域のvector 150に深さ・探索回数・高さテクスチャ番号を格納する。

## 検証

```powershell
./scripts/test-material-assets.ps1 -Configuration Release
./scripts/validate-materials.ps1 -Configuration Release -Parallax
```

専用実描画テストでは、斜め視点での有効/無効差、CPU/GPU駆動描画のGBuffer一致、
高さ未設定・深さ0の復帰を確認する。比較画像は `build/material-tests/pom-disabled.png` と `pom-enabled.png`。
ライティング前の比較には `pom-cpu-gbuffer.png` と `pom-gpu-gbuffer.png` を使う。

2026-10-19のRelease実描画検証ではCPU/GPUのGBufferが完全一致し、高さ未設定・深さ0への復帰も合格した。
GPU駆動の通常描画・Mesh Shader描画へカメラ位置を渡す処理も追加した。
