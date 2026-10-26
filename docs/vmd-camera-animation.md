# VMDカメラアニメーション

GameCameraのInspectorにある「VMDカメラアニメーション」で、カメラのキーを含む`.vmd`を指定する。「参照」でファイルを選ぶか、パスを入力してEnterで確定する。

「再生開始時にVMDを再生」を有効にすると、再生ボタンを押した時点のメインGameCameraが先頭から再生する。EditorCameraには適用しない。プロジェクトの一時停止で時刻が止まり、停止するとカメラの位置・注視方向・画角・設定が再生前に戻る。

- 「VMD座標の倍率」は、注視中心とカメラの距離に適用する。モデルを0.15倍で配置している場合は、まず0.15を指定する。
- 「VMD位置補正」は、倍率を適用したあとにワールド座標で加算する。モデルをX=1に配置している場合は、補正のXも1にすると合わせやすい。
- カメラとモデルは同じフレームの経過時間で進む。ループの長さはそれぞれのVMDに従うので、長さの異なるモーションはループ後に一致しない場合がある。
- VMDによる制御中は、マウス操作・カメラ回転入力・ロックオンによる上書きを抑止する。
- 位置、回転、距離、画角のベジェ補間とロールに対応する。隣接フレームのキーはカットとして扱い、描画の時間履歴もリセットする。
- カメラキーのないVMDや壊れたVMDはエラーを表示する。
- 透視投影と平行投影の両方に対応する。同じVMD内の投影方式は、補間せずキーの時点で切り替え、描画の時間履歴をリセットする。
- 平行投影の表示高さは、MMDの距離45に対して25の範囲を表示する方式で求める（`abs(距離 × 倍率) × 25 / 45`）。画角は平行投影の表示範囲に影響しない。距離が正なら、注視中心へ向くよう視線を反転する。
- GameCameraのInspectorで平行投影を手動設定する場合は「平行投影」と「表示範囲の高さ」を使う。「Far Clip」は平行投影の奥の描画限界になる。透視投影では従来どおり無限遠の投影を使う。

カメラのVMDパス、起動時再生、ループ、速度、倍率、位置補正、投影方式、表示高さはScene/Prefabへ保存する。読み込んだデータと再生位置は保存しない。

## C++ API

既存のモデル用オーバーロードに加え、`CameraComponent`用の操作を追加した。

```cpp
auto& camera = entity.Get<CameraComponent>();
auto& transform = entity.Get<TransformComponent>();

camera.Animation.Scale = 0.15f;
camera.Animation.Offset = {1.0f, 0.0f, 0.0f};
if (Animator::Play(camera, "asset/model/animation/camera.vmd"))
{
    Animator::Update(camera, transform, 0.0f);
}

Animator::Pause(camera);
Animator::Play(camera, false); // 現在位置から再開する。
Animator::SetSpeed(camera, 0.5f);
Animator::Stop(camera); // VMD制御を終了する。姿勢はその場に残る。
```

通常はAnimationSystemが毎フレーム更新するので、呼び出し側で`Update`を重ねて実行しない。プロジェクト全体の停止では、既存のスナップショット復元によって再生前の姿勢へ戻る。

## 検証

`scripts/test-vmd-camera.ps1`は、生成したVMDを実際のローダーへ渡し、カメラキーの読み込み、補間、カット、再生制御、座標変換、投影切り替え、Scene保存と旧Sceneの既定値、不正データの拒否を検証する。WARP上のGPUテストでは実際のカメラ用HLSLとMegaLightsの関数を実行し、両投影方式の視線・始点・深度・ピクセル座標の往復変換を確認する。

`scripts/validate-vmd-camera.ps1`は単体・GPUテストとDebugビルドのあと、アプリを35フレーム実行する。透視投影と平行投影が混在するVMDを使い、実際の投影行列、再生時点のメインGameCameraだけへの適用、AnimationSystemによる更新、プロジェクトの一時停止・再開・停止時の復元、EditorCameraの維持、別のGameCameraをメインにした次回再生を検証する。平行投影の画像は`bin/validation/vmd-camera-orthographic.png`へ保存する。生成物が最新なら`-SkipBuild`でビルドを省略できる。

カメラの座標変換と補間の照合資料:
[babylon-mmd camera](https://github.com/noname0310/babylon-mmd/blob/master/src/Runtime/mmdCamera.pure.ts)、
[babylon-mmd camera animation](https://github.com/noname0310/babylon-mmd/blob/master/src/Runtime/Animation/mmdRuntimeCameraAnimation.pure.ts)。

平行投影の距離・表示高さの変換と正の距離の扱い:
[Blender MMD Tools camera](https://github.com/MMD-Blender/blender_mmd_tools/blob/main/mmd_tools/core/camera.py)。
