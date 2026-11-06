# 眼球・頭・首の LookAt API

```cpp
#include "animator.h"
#include "camera.h"

LookAtSettings settings;
settings.Weight = 1.0f;
settings.Neck.Weight = 0.35f;
settings.Head.Weight = 0.8f;
settings.Eyes.Weight = 1.0f;

const bool registered = Animator::LookAt(characterEntity, Camera::GetCameraEntity(), settings);
// 毎フレーム呼ぶ必要はない。設定や対象を変更するときは再度 LookAt を呼ぶ。

// 追従解除。即座に姿勢を切り替えず、元のアニメーションへ滑らかに戻る。
Animator::ClearLookAt(characterEntity);
```

引数はモデルを持つ Entity と、注視先の Transform を持つ Entity。無効な Entity・自分自身・アニメーションモデルや Transform の不足・対象ボーンが全くないモデル・専用モデル生成失敗の場合は `false`。一部のボーンがない場合は、取得できる部位だけで動く。設定は Entity ごとに保持し、ゲーム再生中に更新する。シーン保存用の永続設定ではない。

## 自然な追従

小さな角度は目で追い、`EyeComfortDegrees` を超える方向は首、頭、左右の目の順に残りの角度を補正する。目は速く、首と頭はゆっくり追う初期設定。部位ごとに `Weight`、`MaxYawDegrees`、`MaxPitchDegrees`、`ResponseSpeed`、`MaxSpeedDegrees` を指定できる。速度制限は左右・上下それぞれに適用する。部位の Weight を 0 にすると、その部位の追加回転を解除する。

元のアニメーションを毎フレームの基準にし、ローカル位置・拡縮を保持したまま回転だけを追加する。前フレームの追加回転はアニメーション更新前に戻すため、停止したモーションでも回転が蓄積しない。アニメーション・IK・物理・カメラ・Transform 更新後に LookAt を適用し、その姿勢を既存のスキニングへ渡す。

対象が背後へ移ると `RearFadeStartDegrees` から `RearFadeEndDegrees` の範囲で追従を弱める。近すぎる対象は左右の眼球間隔を基準に距離を補正し、寄り目を制限する。削除時は Entity 番号の再利用前に登録を解除し、滑らかに元の姿勢へ戻る。親階層に非一様スケール・せん断・反転などがある場合、破綻を避けて追加回転を適用しない。

## ボーン名と正面方向

既存のモデルマップから `neck` / `head` / `leftEye` / `rightEye`、日本語の首・頭・左目・右目を解決する。モデルごとのボーン軸はバインド姿勢から補正する。認識されないモデルは名前を明示できる。

```cpp
settings.NeckBone = "Neck";
settings.HeadBone = "Head";
settings.LeftEyeBone = "Eye_L";
settings.RightEyeBone = "Eye_R";
settings.AutoDetectForward = false;
settings.ModelForward = {0, 0, -1}; // バインド姿勢でのモデル正面
settings.ModelUp = {0, 1, 0};
settings.TargetOffset = {0, 0.1f, 0}; // 注視先へのワールド座標オフセット
Animator::LookAt(characterEntity, targetEntity, settings);
```

通常は足首→つま先のバインド位置からモデルの正面を推定する。取得できない場合は `ModelForward`（既定 +Z）を使う。上方向が特殊なモデルは `ModelUp` も設定する。左右の目を合わせた1本のボーンだけで動くモデルは、左右両方に同じボーン名を指定できる。その場合は1回だけ回転する。

## 個体別の姿勢とコスト

モデルの共有姿勢を変更しないよう、初回の登録時にモデルとロード済みアニメーションを再読込し、専用の姿勢・GPUバッファを用意する。再読込元のファイルが必要。登録した個体の `AnimationModelComponent::ModelId` は専用 ID に変わり、描画インスタンシングは解除する。アニメーションの再生状態・モデルマップの明示設定を引き継ぐ。`ClearLookAt` 後も専用リソースを保持するため、再登録時の追加ロードは不要。同一モデルの人数分だけメモリが増えるので、キャラクター生成時に登録する使い方を推奨する。

## 検証

```sh
bash scripts/test-look-at.sh
bash scripts/test-model-resource-maps.sh
```

Windows では `powershell -ExecutionPolicy Bypass -File scripts/test-look-at.ps1`。C++20 と同梱 Assimp / jsoncpp のヘッダー・ソースを使用する。

正面・左右・上下・背後・近距離、回転したバインド軸、体の回転、可動範囲、時間応答、解除、欠損ボーン、補正済み非一様スケールの拒否、姿勢復元、対象削除時の登録解除を数値テストで検証。Moca VRM の実データ階層でも首・頭・眼球の視線方向と関節位置の保持を確認している。Windows のアプリケーションビルド、画面での自然さ、物理との見た目の調整、GPUメモリ量はこの作業環境では未確認。
