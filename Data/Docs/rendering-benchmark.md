# 固定条件の描画ベンチマーク

## 実行

通常の性能基準取得:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/measure-rendering.ps1 -Configuration Release
```

既に同じソースでビルド済みなら`-SkipBuild`を付ける。既定は各プロセスで120フレームの準備後に600フレームを測り、baseline→candidate、candidate→baselineの計4プロセスを実行する。指定しなければ両者は同じバイナリで、性能改善の主張ではなくA/Aのばらつき測定になる。

短い診断だけを行う場合:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/measure-rendering.ps1 -Configuration Debug -WarmupFrames 8 -MeasuredFrames 16 -Pairs 1
```

この短いDebug実行をReleaseの採用証拠には使わない。PowerShellのExecutionPolicy変更はこの子プロセスだけに適用し、マシンの設定は変更しない。

## 固定する条件

- 同じ起動シーンと保存設定をプロセスごとに読み直す。
- 出力1280×720、内部384×216（縦横とも30%）。実際の測定フレームが異なるサイズなら失敗する。
- シミュレーションのdeltaは1/60秒。カメラは保存された初期位置を基点に、フレーム番号から計算する同じ経路を通る。
- ユーザー入力によるカメラ操作とロックオンを止め、太陽の時刻進行を停止する。
- VSync、FPS上限、自動品質調整を無効化し、保存設定の初回読込後も毎フレーム再適用する。
- 通常の検証モードと同時実行しない。設定ファイルの自動保存を無効にし、スクリプト終了時に元の設定ファイルとプロセス環境を復元する。

シーン全体の画質 fixture は今後追加する。現在の再生署名はエンティティIDとTransformの位置・回転・スケールを検査するもので、全マテリアル、全テクスチャの常駐状態や画像一致を証明するものではない。

## 記録と解釈

出力先は`bin/validation/render-benchmark/<run-id>/`。

| ファイル | 内容 |
| --- | --- |
| `manifest.json` | バイナリSHA256、アダプター、ドライバー、ビルド構成、条件、設定SHA256 |
| `settings.cfg` | ファイル保存を伴わず取得したプロジェクト設定 |
| `frames.csv` | Serial、再生フレーム、フレーム間隔、CPUフレーム/描画、GPU、有効性、再生署名、実サイズ、VRAM |
| `passes.csv` | パスごとのinclusive/self、有効イベント数、欠落イベント数 |
| `summary.json` | 有効件数、平均時間、1% Low、収集完了フラグ |
| `init_log.txt` | そのプロセスのログだけを切り出したもの |

`FrameMs`はGame::Run終了点間の実時間で、ゲーム用deltaの100msクランプを使用しない。`CpuFrameMs`は更新・描画・Present待ちを含むGame::Runの経過時間であり、CPUのアクティブ命令実行時間ではない。`CpuRenderMs`は描画記録区間。いずれもGPU回収時点の値ではなく、同じSerialへ保存された値を使う。

GPUはdirect queueのフレームコマンド区間を測る。テクスチャ転送から最終UI・Present用バリアまで含める。別キューの独立した総実行時間や、キューをまたぐ全体のクリティカルパスをこの値だけで証明しない。P4でGPUタイムラインと実際の重なりを検証する。

1% Lowは最も遅い`max(1, ceil(N/100))`フレームの平均時間を取り、その逆数をFPSへ換算する。GPU欠落は0msにしない。同名パスの一部だけが有効な場合、パス全体のGPU集計は`null`にする。

VRAMはDXGIが返すプロセスのlocalメモリ使用量と予算。エンジンの管理割当量、実レイ数、NRD内部の棄却率は現在未取得のため`null`。GPU時間・VRAM・CPU時間は違う指標として比較する。

比較スクリプトは設定、実サイズ、再生署名、件数を確認し、平均と1% LowをCSVから独立に再計算する。条件が違う場合やGPUサンプルが足りない場合は比較を失敗させる。比較JSONの`AdoptionProven`は現在常にfalseであり、画質・ちらつき・残像の検証を置き換えない。

別バイナリを指定する場合も、実行時のシェーダー・アセットは同じ作業ディレクトリから読む。過去のシェーダー版との比較には使わず、今後追加する同一バイナリ内の独立した機能切替、または完全な実行時アセット一式で比較する。

## 単体検証

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test-rendermeasurement.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test-renderbenchmark-comparison.ps1
```

遅延/順序違い/重複Serial、ウォームアップ境界、最終GPU結果、欠落値、同名部分集計、1% Low、設定/サイズ/再生状態の不一致、偽の集計、ゼロGPU時間の相対比を検証する。
