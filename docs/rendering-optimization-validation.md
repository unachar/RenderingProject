# 描画最適化の検証状況

2026-10-18。P1–P4全体は実装・検証中。性能改善や採用を証明した段階ではない。

## 実装した範囲

- CPU/GPU集計は独立した有効件数を持ち、GPU欠落を0msとして平均しない。1% Lowは遅い`ceil(N/100)`フレームの平均時間の逆数。
- プロファイラーはGPUイベントの親IDと深さを記録する。同一キューの直接の子区間を和集合として除外し、inclusiveとselfを分ける。Frame全体は独立したタイムスタンプ区間を使用する。
- 反射/GIの共通準備、Trace、NRD Denoise、Composeを分離。OptiXはPack、外部依存のdirect queue待機、Composeを分け、CUDAカーネル時間そのものとは区別する。空間GIの既存1 dispatchはDenoise/Compose一体と明記する。
- 共通履歴判定、フレーム/世代コンテキスト、シーンリセット通知、明示的な`Camera::NotifyCut`を追加。NRDと既存TAAの前段/最終段へ接続した。
- 通常のカメラ移動は履歴を破棄しない。解像度・カメラ・シーン・機能設定変更時の安全な全体リセットを先行する。

## 確認済みの証拠

| 検証 | 結果 | 証拠 |
| --- | --- | --- |
| 集計単体テスト | RED→GREEN、`/W4 /WX` | `scripts/test-rendermeasurement.ps1` |
| 共通履歴単体テスト | RED→GREEN、`/W4 /WX` | `scripts/test-temporalhistory.ps1` |
| Debugビルド・DXR/DLSS有効描画 | 84フレーム合格、D3D12エラー0 | `build/rendering-p1-history-debug.log`, `bin/validation/Debug-features/init_log.txt` |
| DXR/DLSS無効フォールバック | 84フレーム合格、D3D12エラー0 | `bin/validation/Debug-fallback/init_log.txt` |
| 実GPUイベント階層 | 親ID・深さ・self時間の範囲を検証して合格 | 上記Debug-featuresログ |
| 動的GI回帰 | 218フレーム合格、失敗0、D3D12エラー0 | `build/rendering-p1-gi-debug.log` |
| GPU計測の異常系 | クエリ枯渇、終了漏れ、順序違反、フレーム外開始が通常/フォールバックで合格 | `build/rendering-p1-profiler-faults-debug.log` |
| 最新コードのDebug描画/GI | 通常84・フォールバック84・GI218フレーム合格、D3D12エラー0 | `build/rendering-benchmark-final-debug.log`, `build/rendering-benchmark-final-gi.log` |
| 既存NRD反射ノイズ検証 | 最新実行は正しいpacked座標で128ペア、raw 0.148093 / filtered 0.035409 | 上記Debug-featuresログ |
| 固定条件のRelease A/A | 120準備+600測定を4回、全回600件の有効GPU結果、条件/再生一致 | `bin/validation/render-benchmark/e8e54897eadd-comparisons.json` |
| 比較結果の検証 | 設定・サイズ・再生不一致、重複、偽/非有限集計、ゼロ分母を検証して合格 | `scripts/test-renderbenchmark-comparison.ps1` |

NRD検証は同一実行内のノイズ除去比較であり、今回の実装による画質改善を意味しない。従来の検証はpacked checkerboardの入力を全解像度座標で読み、未トレース領域を比較していた。シェーダーと一致する格納座標と2画素間隔の縦横ペアへ修正し、サンプル数>100とノイズ減少の条件を維持した。

## 未完了の必須項目

- P1-1: 異常系と部分集計の扱いは検証済み。全統計の接続後、計測機能自身のON/OFF費用を比較する。
- 共通履歴: 明示的な編集/Undoの照明・LOD通知とTAAの手動露光入力は接続済み。自動GPU LODと連続する実行時照明、GPU露光、理由別GPU回帰は未完了。遅延CPU統計をそのまま当該フレームのLOD無効化として使わない。
- 固定比較: 基本経路は実装・Release反復検証済み。現在は起動シーンのTransform再生までで、全画質fixture、マテリアル/テクスチャ常駐の一致確認、独立機能切替による実最適化比較、計測オーバーヘッド評価が残る。手順は`docs/rendering-benchmark.md`。
- 統計: VRAM、実行レイ数、機能別履歴棄却率を追加する。NRD内部の未取得棄却率をアプリ側リセット率で代用しない。
- P1-2〜5: 適応反射、GI予算、エフェクト縮小、合成統合は未着手。
- P2-1〜6: 変更分類、AS再利用/更新、DXR LOD、Proxy、レイ予算、差分転送は未着手。既存のBLAS UPDATEだけで完了とは扱わない。
- P3-2〜7: Velocity契約、TSR再構成/棄却/Reactive/細線/LOD/プリセットとNIS+TAA比較は未着手。
- P4-1〜5: Visibility Buffer、Shading Rate、Alias、実効Async、永続キャッシュの最適化は未着手。既存機能の存在だけで採用条件達成とは扱わない。
- 鏡面/夜景ネオン/移動/遮蔽解除/照明変更/細線/遠景の固定画質比較と、全体のフレーム改善・ちらつき・残像評価を行う。

実装経過と判断は`docs/superpowers/ledger/rendering-optimization/progress.md`に記録する。

## 現在の性能基準（改善結果ではない）

同じReleaseバイナリ、1280×720出力・384×216入力。120フレーム準備後の600フレームを各行に示す。

| 実行順 | GPU ms | フレーム間隔 ms | CPUフレーム ms | 1% Low FPS |
| --- | ---: | ---: | ---: | ---: |
| A0 | 3.3283 | 3.3919 | 3.1002 | 216.4 |
| B0 | 3.3353 | 3.3896 | 3.0860 | 223.2 |
| B1 | 3.3360 | 3.3956 | 3.0931 | 209.0 |
| A1 | 3.3292 | 3.3945 | 3.0938 | 228.9 |

A/Bの平均差はGPU約0.2%、フレーム間隔0.1%未満だった。一方、1% Lowには209.0–228.9 FPSのばらつきがある。小さな差で採用を決めず、必要時に追加反復する。異なる最適化や画質を比較した結果ではない。

GPU値はdirect queueのフレームコマンド区間であり、独立したasync compute全体の実行時間ではない。別キューのタイムライン検証はP4で行う。VRAMはプロセスlocal使用量/予算を記録し、未取得の管理割当量・実レイ数・内部NRD棄却率を0で代用しない。
