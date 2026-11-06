# Timelineの身体・表情アニメーション

LiveStageのkarenは、`kasou.vmd`（身体）と`kasou_kao.vmd`（表情）を同時再生します。
LiveStage.scene、LED設定を保持したシーン、LiveStage.prefabに反映しています。

1. LiveDirectorを選択し、InspectorのTimelineで対象のトラックを開きます。
2. 対象Entityにアニメーションモデル、パラメーターに「Animation Time」を指定します。
3. 「アニメーション (.vmd) をここにドロップ」欄に、身体、表情の順でVMDを追加します。
   アセットブラウザとエクスプローラーの両方から追加でき、欄のクリックでも選択できます。
4. 既存のClip／Keyで再生時刻を指定し、シーンを保存します。

登録したVMDは同じTimeline時刻で評価されます。同じボーン・表情を制御する場合は後のVMDが優先されます。
「上へ」で順序を変更し、「削除」でトラックから外せます。すべて削除すると、割り当て前の対象モデルの再生設定に戻ります。
この欄はモデル用VMDに対応します。カメラ用VMDは既存のCamera設定を使用してください。
読み込みに失敗した場合は欄の下にエラーが表示されます。ファイルを修正した後、「読み込みを再試行」を押してください。

回帰テスト: `scripts/test-timeline-animation.ps1`
