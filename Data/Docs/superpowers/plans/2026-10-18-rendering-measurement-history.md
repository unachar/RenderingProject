# Rendering Measurement and History Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 固定条件の比較計測と履歴無効化契約を実装し、P1以降の採用判断に使える証拠を残す。

**Architecture:** 既存RenderProfilerとGPU検証経路を拡張する。純粋な集計・履歴判定はD3D12に依存しないヘッダーへ分離し、GPU結果はフレームSerialで関連付ける。通常描画の設定と既存経路を維持する。

**Tech Stack:** C++17、DirectX 12、HLSL、PowerShell、Visual Studio C++ tools。

**Spec:** `docs/rendering-optimization-design.md`（2026-10-18にユーザー承認）。

## Global Constraints

- 比較条件は同じシーン、カメラ経路、出力解像度、内部解像度、品質設定。
- Frame全体を採用基準とし、親子パスのinclusive時間を合算しない。
- NRDの任意疎密入力、露光/トーンマップ/シャープニングの無条件統合は禁止。
- 既存経路を残し、独立した機能設定で比較可能にする。
- 未取得値を0や成功として記録しない。管理割当量とアダプター使用量を区別する。
- 通常設定を保存・上書きしない。Debug安全性検証とRelease性能計測を区別する。
- RTKの既存出力圧縮を使い、無関係なログ・外部ライブラリーを再探索しない。

## Review Focus

- GPUリング結果の遅延: CPU/画像/レイ統計と異なるSerialを混ぜない（Task 2/4）。
- クエリ枯渇と計測開始途中: 開いていない親や無効イベントを参照しない（Task 1/2）。
- ウォームアップと1% Low境界: 少数サンプル・空集合・非有限時間を誤集計しない（Task 1）。
- 通常カメラ移動: カメラ切替と混同して毎フレーム全履歴を破棄しない（Task 3）。
- 照明/LOD編集と機能切替: 未更新のGIや旧デノイザーの履歴を有効と扱わない（Task 3/5）。

## Scope and Execution

この計画は最初の実装単位（P1-1、固定比較、P3-1の先行する共通履歴契約）を対象とする。P1–P4全体を完了扱いにしない。後続サブシステムは下記の依存順で具体化する。推奨実行方法はこのチャットで本体エージェントが順に実装するNative方式。サブエージェントの起動はユーザーが実行方式を選択した後に限る。

### Task 1: 独立した時間集計とイベント階層

**Files:** Create `source/rendering/rendermeasurement.h`, `tests/rendermeasurement_tests.cpp`, `scripts/test-rendermeasurement.ps1`。

**Interfaces:** `RenderMeasurement::Sample { uint64_t Serial; double FrameMs; double CpuMs; double GpuMs; bool GpuValid; }`、`Summary Summarize(const std::vector<Sample>&)`、`EventInterval { uint32_t Id; uint32_t ParentId; uint32_t QueueId; uint64_t Begin; uint64_t End; bool Valid; }`、`double ExclusiveTicks(const EventInterval&, const std::vector<EventInterval>&)`。無効親は`UINT32_MAX`。Summaryは各指標の有効件数、平均、1% Lowを持つ。

- [ ] 空入力、NaN、負時間、GPU欠落、重複Serial、親[0,100]/子[10,40]/孫[20,30]、兄弟区間重複、別キュー子、欠落親をテストする。親のexclusiveは70、子は20、別キューは差し引かない。1% Lowは遅い`max(1, ceil(N*0.01))`フレームの平均時間から算出する。
- [ ] `powershell -NoProfile -File scripts/test-rendermeasurement.ps1`で未実装による失敗を確認する。スクリプトは既存`test-shader-cache.ps1`のVS検出パターンを使い、純粋C++テストをコンパイルして実行する。
- [ ] 上記APIを実装する。exclusiveは直接の子区間の和集合を親範囲へクリップして差し引く。FrameのGPU時間を子パスから再構成しない。
- [ ] 同じコマンドで全ケース合格を確認する。無効サンプル除外数も検証する。

### Task 2: プロファイラー接続とTrace/Denoise/Compose分離

**Files:** Modify `source/rendering/renderprofiler.h`, `source/rendering/rayreflections.cpp`, `source/rendering/raytracing.cpp`, `source/rendering/postprocesspass.cpp`。Modify Task 1 tests。

**Interfaces:** PassTimingの既存`CpuMs/GpuMs`はinclusiveとして互換維持。追加する`GpuExclusiveMs`と有効フラグは同一キュー内の階層集計に限定。FrameSnapshotは既存Serialを維持し、イベント階層の記録を追加する。

- [ ] GPUクエリ枯渇、同名別親、途中開始、終了漏れをTask 1テストへ追加し、意図する集計契約を固定する。
- [ ] Begin/EndGpuEventに親ID・キュー・区間を接続する。失敗したBeginはスタックに積まず、Frame終了時に状態をクリアする。タイムスタンプ欠落は有効フラグで表す。
- [ ] 反射/GIを含む旧`DXR Reflections`親イベントの帰属を修正する。反射とGIのTrace、NRD/OptiX/空間Denoise、Composeを実際の実行区間へ配置する。共通ガイド準備は独立イベントとし、反射・GIへ重複帰属させない。
- [ ] F9の表でinclusive/exclusiveを区別し、既存カウンターの意味を維持する。
- [ ] Task 1テスト、`scripts/validate-rendering.ps1 -Configuration Debug`、`scripts/validate-gi.ps1 -SkipBuild`を実行する。UI表示有無による計測対象の変化とクエリ不足をログで確認する。

### Task 3: 共通履歴判定と既存履歴への接続

**Files:** Create `source/rendering/temporalhistory.h`, `tests/temporalhistory_tests.cpp`, `scripts/test-temporalhistory.ps1`。Modify `source/rendering/nrddenoiser.h`, `source/rendering/raytracing.cpp`, `source/rendering/rayreflections.cpp`, `source/rendering/postprocesspass.cpp`。必要な世代通知は`source/rendering/gpuscene/gpuscene.cpp`と既存シーン編集経路へ追加する。

**Interfaces:** `TemporalHistory::State`はSerial、CameraId、SceneGeneration、LightingGeneration、LodGeneration、内部/出力サイズ、露光、CameraCutを持つ。`Decision Evaluate(const State& previous, const State& current, bool valid)`は理由ビットと利用可否を返す。既存の局所リセット条件を削除せずORする。

- [ ] 初回、欠落Serial、同一カメラの通常移動、カメラ切替/カット、内部/出力resize、シーン/照明/LOD世代、無効露光、デノイザー切替をテストする。通常移動は有効、その他の安全性を証明できない条件はリセットする。
- [ ] `powershell -NoProfile -File scripts/test-temporalhistory.ps1`で失敗を確認してから純粋判定を実装し、再実行する。
- [ ] NRD、反射/GI、既存TAAへ判定結果を接続する。現存する照明・発光・距離変更判定を維持し、Stable ID/LOD世代の供給元を文書に記す。画素対応がないLOD変更は全体破棄とする。
- [ ] 切替・リサイズ・照明編集のGPU検証を既存`source/core/renderingvalidation.cpp`と`source/core/givalidation.cpp`へ追加する。通常移動で履歴が継続し、切替後の初回だけ破棄されることを確認する。
- [ ] 純粋判定テストとDebug描画/GI検証を実行する。露光補正を実装していない経路を補正済みと扱わない。

### Task 4: 固定再生と機械可読の測定成果物

**Files:** Create `source/core/renderbenchmark.h`, `source/core/renderbenchmark.cpp`, `scripts/measure-rendering.ps1`。Modify `source/core/world.cpp`, `source/core/renderingvalidation.cpp`, `DirectX12.vcxproj`, `DirectX12.vcxproj.filters`。

**Interfaces:** `RenderBenchmark::IsEnabled()`、`BeforeFrame(uint64_t frame)`、`Record(const RenderProfiler::FrameSnapshot&)`、`Finish()`。通常起動では無効。`DX12_RENDER_BENCHMARK`を専用有効化条件とする。結果は`bin/validation/render-benchmark/<run>/manifest.json`, `frames.csv`, `summary.json`。

- [ ] Serial照合・ウォームアップ除外・設定差・最終リング回収のテストをTask 1へ追加する。フレーム経過時間はCPU測定時点で保存し、GPU回収時のWorld値で代用しない。
- [ ] カメラ/アニメーション/物理を固定ステップで進める専用検証モードを追加する。乱数、露光、入力、解像度、品質、シーン内容、実行形式、アダプターをマニフェストに記録する。120 warmup + 600 measuredを初期規約とする。
- [ ] A/BとB/Aの複数回を実行するスクリプトを作る。比較対象以外のマニフェスト差は失敗とする。既存設定を保存しないモードとプロセス環境の復元を使う。
- [ ] CPU/GPU全体、パスinclusive/exclusive、有効件数、1% Low、VRAM使用/予算と管理割当量を出力する。実レイ数と履歴棄却率の未取得欄はnullとし、次Taskで埋める。
- [ ] Releaseで同一設定A/Aを反復し、マニフェストと同一Serialのシミュレーション状態が一致することを検査する。計測機能ON/OFFの費用も別記する。A/Aの性能ばらつきを採用判断の基準として保存する。

### Task 5: レイ・履歴統計と最初の基準データ

**Files:** Modify `shader/hlsl/RayReflectionsCS.hlsl`, `source/rendering/rayreflections.cpp`, `source/rendering/renderprofiler.h`, `source/core/renderbenchmark.cpp`。Create `docs/rendering-optimization-validation.md`。

**Interfaces:** FrameSnapshotに機能別`RayStatistics`と`HistoryStatistics`を追加する。値にはSerialと取得有効フラグを持たせる。`HistoryStatistics`は評価画素数/棄却画素数/リセット理由を区別する。

- [ ] カウンターが既知になる小さいGPU fixtureと、readback未完了/リング再利用/全リセットのケースを作る。一次・影・二次レイを区別し、実際のレイ命令を数える。
- [ ] 計測専用フラグ下でGPUカウンターと非同期readbackを追加する。常用パスへ無条件のatomic/readback待機を導入しない。NRD内部で取得できない棄却率はnullのまま残し、取得不能理由を記録する。
- [ ] 鏡面、夜景ネオン、移動物体、遮蔽解除、照明変化、細線/遠景の固定fixtureを登録する。欠落カテゴリを既存GI fixtureだけで充足したと扱わない。
- [ ] 固定比較の基準ログ/画像列、リセット理由、Debugエラー結果、Release性能を保存する。最初の段階は計測の正しさが判定対象であり、これだけでP1のGPU削減完了とはしない。

## 後続計画へ渡す成果物と順序

1. P1-2: 分類マスクとNRDに渡す密な信号の復元方式を確定し、旧経路比較。P1-3: 有効履歴/信頼度/経過時間を持つGI更新予算と優先領域を実装。
2. P1-4: 半解像度BloomとFroxelを再利用し、DOF/Motion Blurの未対応部分を実測して縮小。P1-5: 同一段階の合成を統合し、露光/トーンマップ/シャープニングの順序を固定。
3. P2-1/2/6: 変更分類、AS reuse/update/rebuild条件、差分転送を検証。P2-3/4: 共通DXR LOD/Proxyの欠落・影漏れ比較。P2-5: 平滑化したGPU予算を追加し、振動を検証。
4. P3-2: Velocityの符号/単位/ジッターを画像fixtureで検証。P3-3/4: 出力解像度履歴と深度棄却。P3-5/6: Reactive/細線/発光/LOD信頼度。P3-7: プリセットと30%入力のNIS+TAA比較、重複実行0を確認。
5. P4-1/2: GPU Drivenを保ったVisibility Bufferと面内評価共有。P4-3: 寿命に基づくAlias検証。P4-4: RenderGraph Queue/FenceとGPUタイムラインの重なり。P4-5: 永続キャッシュとフレームスパイク評価。

各後続計画はこの段階の実測で対象パス・入力契約・画像基準を確定した後に記述する。これは全体目標の削減ではなく、依存情報が必要な実装単位の分割である。

## 完了報告の制約

Task 1–5の合格は計測/履歴基盤の完了だけを証明する。P1–P4の達成には各フェーズの固定比較、画質/時間的安定性、GPU状態/Fence/Alias検証が必要。絶対目標フレーム時間と画質許容値は未指定のため、相対改善と未確定の採用条件を分けて報告する。
