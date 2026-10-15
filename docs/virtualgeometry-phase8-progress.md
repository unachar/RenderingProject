# Execution ledger — docs/superpowers/plans/2026-10-15-virtualgeometry-phase8.md

2026-10-15。設計承認後、ユーザーが短時間・最小使用量の自走と「テストはせず」を指示。Native実行を継承。

Ruling: 最新指示を優先し、テスト追加・実行、ビルド、shader compile、runtime、大規模stress、subagent reviewを省略する。製品コードと既存harnessのinterface追従、手動のコード読取りのみ行う。動作・コンパイル成功は未確認であり、以前のPhase 7合格結果をPhase 8の合格として使わない。

Ruling: 既存checkoutを継続し、commit/pushや設定変更をしない。別worktreeやSDD shell bookkeepingを追加せず本ledgerを使用する。

Pre-flight: Task 1の希望refineをTask 2のGPU履歴へ、Task 3のphase別list/count/argumentsをTask 4/5のopaque/velocity/feedbackへ接続する。既存framepipelineには現在depthのHi-Z構築とphase 2呼出点があり、GPU-driven時の除外をnative 2パスに限定して解除する。

Task 1: CPU Refine helperとSelectResidentTemporalを追加。希望refineはPage条件を適用する前の独立vectorで返す。テスト未実施。

Task 2: instance slot generation、32-byte CPU/GPU history header、GPU group希望refine履歴を追加。削除/再利用とmesh/asset変更を識別する。camera cut、native/mesh/GPU経路切替、layout/容量変更で履歴を無効化する。履歴なし・幅0は既存閾値、Force Levelは履歴を迂回。テスト未実施。

Ruling: groupごとの履歴は同一graphics queueで順序付けした単一UAVに保持する。frameごとのCPU headerとlayout epochを使い、identity/layout変更時は保守的に全履歴をresetする。二重bufferより少ないメモリで、同じgroupへのframe間同時アクセスをqueue順とUAV barrierで防ぐ。代償はasset追加等でも他instanceのヒステリシスが一度resetされること。

Task 3/4: 完全な常駐cutを選択してからFirst/Deferredへ分割し、現在Hi-ZでDeferredだけを再判定してSecond/Combinedへ追加する。各listを完全cutと同じcapacityにして、instance全体の既存予約成功後の追加overflowを避ける。current Hi-Z未生成時は候補をすべて復帰させる。early GPU object occlusionでnative候補を捨てない。Secondはlegacy meshlet/fallbackを再描画しない。

Task 5: VelocityはCombinedを参照。Page feedbackは再判定後へ移動。4状態Page snapshotと両queue寿命は維持する。LOD切替、第1パス、候補、復帰、容量Fallback、history/phase buffer bytesをUI/Profilerへ追加。GPU pass timingには既存ScopedEventを使用。統計は1024-byte readback ringを完了Fenceだけpollし、通常frameで待たない。

Task 6: ユーザー指示により新規acceptance harness/script、runtime、3000万/1億stressは未実施。

Task 7: 変更箇所の自己読取りのみ。独立レビュー・回帰テスト・ビルド・実機検証なし。既存GPU harnessは256-byte constantsと29 root parametersに追従させたが、実行していない。結果と未確認事項は `docs/virtualgeometry-phase8-validation.md`。
