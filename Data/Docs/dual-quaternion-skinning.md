# デュアルクォータニオンスキニング

`skinning_cs.hlsl` の通常スキニングに DQS を適用する。PMX・FBX・VRM は共通の Compute Shader を使うので、モデル側での設定や再インポートは不要。更新後に Visual Studio の x64 ビルドを行い、`shader/hlsl/build/skinning_cs.cso` を更新する。

- 有効なボーンウェイトを正規化し、逆バインド行列を含む最終スキニング行列から DQ を作る。
- クォータニオンの符号を揃えて加重合成し、実部と双対部を正規化する。位置には回転と移動、法線には回転を適用する。
- PMX の SDEF は元の専用処理を優先し、QDEF は DQS で処理する。
- 拡縮・せん断・反転など、剛体変換以外の行列が混ざる頂点は全体を従来の LBS で処理する。DQS によってスケールを失わないためのフォールバックである。
- ボーン行列や頂点のバッファ形式を変えず、輪郭線にも同じ Compute Shader を使う。描画・影・速度・DXR が参照するスキニング後の頂点バッファも既存のまま。

## 検証

Linux / macOS（C++17 コンパイラ）:

```sh
bash scripts/test-dual-quaternion-skinning.sh
```

本番の HLSL 計算ヘッダーをテスト用の C++ ベクトル型で実行し、回転、移動、180度回転、符号反転、ウェイト正規化、ねじり時の半径保持、非剛体行列の判定を検証する。

Windows（Visual Studio C++、NuGet 復元済み）:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/test-dual-quaternion-skinning.ps1
# 実 GPU で検証する場合
powershell -ExecutionPolicy Bypass -File scripts/test-dual-quaternion-skinning.ps1 -Hardware
```

数値テストに加え、DXC で本番 Compute Shader をコンパイルして D3D12 WARP / GPU 上で実行する。バッファの行列格納規則を含めて、DQS・SDEF・QDEF・スケール/反転時の LBS・無効なウェイト/ボーン番号・法線・UV・色を確認する。

今回の作業環境で実行したのはポータブル数値テスト、既存 CPU テスト、Linux DXC によるコンパイル。Windows のテスト、実モデルの画面表示と GPU 時間は未確認。DQS の行列変換と剛体判定は現在頂点ごとに行うので、モデル規模に応じた性能計測が必要。
