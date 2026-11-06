# フェーズ8 実装状況

2026-10-15。ユーザーの「短時間・最小使用量で自走、テストはせず」の指示により、ビルド、shader compile、テスト、runtime、stress、独立レビューは実施していない。本記録は成功証拠ではなく変更範囲と未確認事項を示す。

## 追加した処理

- LOD希望refineに10%幅のヒステリシス。設定で無効化・幅変更可能。Force Levelは履歴を迂回。
- GPU instanceのgenerationとasset/group layoutで履歴を識別。camera cut、identity/layout/容量変更、native/GPU/mesh経路切替でreset。
- Pageに基づく完全な常駐cutを先に決定し、前frame Hi-ZによるFirstとDeferredへ分割。
- 第1パスdepthから現在Hi-Zを生成し、Deferredだけを再判定してSecondを追加描画。Hi-Z利用不可時は候補を保守的に復帰。
- Secondでlegacy/fallbackを再描画せず、VelocityはFirstとSecondのCombinedを参照。
- Page request/usageのfeedbackを再判定後に回収。
- UI/Profilerにphase別件数、LOD切替、容量Fallback、history/list memoryを追加。統計はFence完了済みreadbackのみ読み、GPU passは既存timestamp eventで計測する構成。
- 3つの追加compute entry pointとMSBuild出力を登録。既存GPU harnessのconstants/root bindingsを追従。

## 実装上の選択

履歴は単一GPU UAVを同一graphics queueの順序で更新し、frame別CPU headerとepochで識別する。layout変更時は全履歴を保守的に無効化する。候補list各々を完全cutのcapacityまで確保するため、候補overflowはcut予約で防ぐ構成。代償として追加GPU memoryと完全cutの選択処理が必要で、遮蔽subtreeのmetadata走査削減は実装していない。

## 未確認

C++/HLSLのコンパイル、D3D12 resource state、root binding、render pass順、Hi-Zによる復帰、境界付近のLOD安定性、native/legacy排他、Velocity、Page Evict中の動作、メモリ使用量・速度・画面の正しさは実行確認していない。新規Phase 8受け入れharness/scriptは追加していない。既存Phase 6/7の以前の合格は今回変更の検証にはならない。

次に検証を許可された場合は、小規模のCPU/GPU参照比較、Debug buildと遮蔽解除fixture、既存Phase 6/7回帰、その後3000万/1億stressの順に確認する。10億triangleはPhase 7で記録した常駐metadataのVRAM容量制約が残る。
