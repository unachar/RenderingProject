# マテリアル・Niagara拡張（2026-11-06）

既存のノード／モジュールに加え、汎用制作に使う以下を追加。UEの数学演算・Niagaraモジュールの役割を参考に、このエンジンの型・固定刻み実行・保存形式へ合わせて独立実装した。UEアセットやモジュールスクリプトの読み込み互換ではない。

## マテリアルノード

| 分類 | ノード | 動作 |
| --- | --- | --- |
| 基本演算 | Logarithm10 | 底10の対数。入力は1e-8以上として評価 |
| 基本演算 | Exponential2 | 2の累乗。指数は-126〜126に制限 |
| 基本演算 | Inverse Lerp | (Value-Min)/(Max-Min)。範囲がゼロなら0、結果はクランプしない |
| ベクトル | Reflect Vector | 入射ベクトルを法線で反射。法線を内部で正規化し、ゼロ法線なら入力を返す |
| Render / Original | Render Soft Band | 中心値・半幅・ぼかしで帯状マスクを生成 |
| Render / Original | Render Radial Pulse | 距離・位相・周波数から同心円／球殻の繰り返しマスクを生成 |
| Render / Original | Render Flow UV | 方向スクロールに交差する正弦波の歪みを加えるUV |

CPUプレビュー、HLSL生成、実描画用VM、GPUノードプレビューへ接続。複合ノードはコンパイル時に既存VM命令へ展開し、共通シェーダーの命令種別を増やさない。Radial Pulseの未接続PositionはWorld Position、PhaseはTime。Flow UVの未接続UVはメッシュUV、Timeは経過時間。スカラー入力は既存ピンと同様に直接編集し、ベクトルは定数／パラメーターノードから接続できる。

Logarithm10・Exponential2・Inverse Lerp・Reflect VectorはNiagaraカスタムグラフのCPU／GPUでも使える。独自の3ノードはマテリアル向けで、Niagaraの追加候補には表示しない。

## Niagaraモジュール

| 分類 | モジュール | 実行段階・動作 |
| --- | --- | --- |
| 放出 | Add Velocity in Cone | Particle Spawn。円錐内の立体角に均一に方向を選び、初速度へ加算 |
| 力 | Point Attraction Force | Particle Update。中心へ吸引し、半径と減衰指数で調整。負の強度で反発 |
| 力 | Vortex Force | Particle Update。指定軸・中心の周りに接線方向の力を加える |
| 制限 | Limit Velocity | Particle Update。このモジュールまでに適用した速度を上限以内に制限 |
| Render独自 | Render Radial Wave | Particle Update。中心から進む周期波の位相で吸引／反発を交互に加える |
| Render独自 | Render Speed Color | Particle Update。速度範囲に応じて低速／高速カラーを初期RGBへ乗算。アルファは維持 |

中心・軸・加速度は既存Gravityと同様にシミュレーション空間で指定する。ローカル空間はエンティティに追従し、ワールド空間ではワールド座標を使う。力→Drag→Limit Velocity→Speed Colorの順を出発点に調整すると分かりやすい。後ろのモジュールは前の結果へ作用する。

円錐放出は両バックエンド共通のCPU放出処理、更新モジュールはCPU／GPU双方へ実装。数値検証・パラメーター参照・Undo／Redo・保存／再読込を既存モデルへ接続。新しい列挙値は末尾へ追加し、既存アセットの番号を変えていない。範囲グリッドにも追加した放出速度と加速度を含める。GPU実行は既存の同期型バックエンドを使用する。

## サンプル

- `asset/Material/Render_NodeToolkit_Demo.material` と付随する `.graph.json`：新しい7ノードを組み合わせた、外部テクスチャ不要のマテリアル。
- `asset/VFX/Render_Toolkit_CPU.naiagara`：追加した6モジュールと、追加演算を使うカスタムグラフを組み合わせたCPU版。
- `asset/VFX/Render_Toolkit_GPU.naiagara`：同じ構成のGPU版。

## 検証

- Release x64の全体コンパイル・リンク成功。
- 全シェーダー並列再生成成功（66.625秒）。記録：`build/build-performance/Release-Build-20261106-094606.json`。
- 最終通常ビルド成功（7.318秒）。記録：`build/build-performance/Release-Build-20261106-094742.json`。
- 既存GPUグラフのparameter/time/view/world検証成功：`build/node-module-lowered-gpu-check.log`。
- 既存シェーダービルドの依存関係・出力チェック成功。VM includeの依存漏れを修正した。
- 既存CPUグラフ単独検証はスクリプトのinclude・ライブラリ参照不足で未完了。参照先を補って再リンクすると、スクリプトに含まれないAssetRegistry、ComponentTypeRegistry、MaterialGraphGpu等の未解決シンボルが残る（`build/node-module-cpu-relink.log`）。新規テストコードは追加していない。

## 参照

- Epic Games: https://dev.epicgames.com/documentation/en-us/unreal-engine/math-material-expressions-in-unreal-engine
- Epic Games: https://dev.epicgames.com/documentation/en-us/unreal-engine/particle-update-group-reference-for-niagara-effects-in-unreal-engine

実画面でCPU版・GPU版のサンプル読込と継続再生、追加した全6モジュールの有効状態、カスタムグラフの実行を確認。サンプルマテリアルの読込、GPU描画時評価（93命令・12レジスター・Time利用）、青い帯の描画を確認した。CPU／GPUの数値誤差比較、大量粒子の負荷、追加機能すべての境界値とUndo／保存再読込の網羅検証は未実施。
