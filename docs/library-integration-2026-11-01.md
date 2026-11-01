# Externalライブラリの実装・比較状況

## 採用基準

ユーザーの最新指示: 圧縮は速度とサイズの両方、描画は品質と負荷の両方で既存実装と比較する。未実装ライブラリは実装対象とする。以下は作業途中の記録であり、全ライブラリの統合完了報告ではない。

## 今回の変更: zstdキャッシュの用途別選択

既存のbeneficial-only zstd保存を標準として維持する。`DX12_COMPRESS_CACHES=0`をプロセス起動前に指定すると、新規シェーダー・テクスチャキャッシュを非圧縮で保存する。未設定または`1`は圧縮を選ぶが、縮まない入力や圧縮失敗では従来形式へ戻る。両形式の読込を維持し、既存ファイルをロード時に変換しない。したがって設定を変えただけで既存の圧縮キャッシュが高速化するわけではない。

|固定入力|非圧縮容量|zstd容量|削減率|非圧縮warm読込|zstd warm読込|
|---|---:|---:|---:|---:|---:|
|シェーダー3,214件|126,837,266 bytes|85,991,600 bytes|32.20%|695.723ms|904.964ms|
|DDS 65件|122,108,212 bytes|43,983,070 bytes|63.98%|62.962ms|389.655ms|

シェーダーの非圧縮列は同じ製品Loadのv1経路。DDSの非圧縮列は従来DirectXTex直接読込。いずれも同一実行内の交互順3回平均で、出力の全バイト／BC画素とメタデータを比較した。シェーダーは全3,214件、DDSは64件が圧縮、1件は非圧縮に戻った。圧縮でVRAMやFPSが改善するという結果ではない。cold I/Oや起動全体も未測定。

標準の圧縮は容量効果を重視した実装者判断。速度だけならこのwarm条件では非圧縮を選ぶ。描画結果は同じだが、全用途で一方が優れているとは扱わない。

速度設定の追加実測も同じ入力の隔離コピーで完了。DDSは65件すべて非圧縮、122,108,212 bytes、画素・メタデータ完全一致。シェーダーは3,214件すべてv1、126,837,266 bytes、診断文字列を含め完全一致。別実行の絶対時間は環境負荷が異なるため、上表との速度比には使わない。既存比較器の`newCompressedLoadWarmMsMean`等のフィールド名は歴史的名称で、速度設定の出力では非圧縮を測っている。`compressedFiles`と実ファイルのheaderが判別根拠。

### 回帰修正と確認

- 非圧縮DDSで保存し直したとき、古い圧縮sidecarが新しい内容を隠さないよう、保存成功後にsidecarを削除する。
- zstd内部のメモリ不足は破損ではなく資源不足として扱い、有効なsidecarを削除しない。従来DDSへfallbackする既存契約を復旧した。
- allocation注入用ビルドの`/FI:`がコロンをファイル名へ含めていたため、MSVCの`/FI`構文へ修正した。この不備により従来の注入試験がこの環境では実行できなかった。
- shader codec 526チェック、texture storage 661チェック、DXCキャッシュ統合試験に合格。破損、旧形式、メモリ不足、非圧縮保存、圧縮保存、置換を含む。
- 変更した製品ヘッダーのclang-format確認とgit diff --checkに合格。別レビューでも具体的な追加不具合なし。

ログ・レポートは`bin/validation/technology/library-comparison-{shader,texture}-{before,speed}-20261101/`と`bin/library-balanced-*.log`。入力キャッシュは上書きしていない。

## 追加実装: Tracy CPUトレース

`/p:EnableTracy=true`でTracyClientを1回だけコンパイルし、`Game::Run/Update/Draw`のCPU区間とフレーム境界を記録する。既定は無効で、既存のCSV/GPU計測を維持する。有効時は`TRACY_ON_DEMAND`を指定し、収集クライアント接続時に記録する。SDKは同梱BSDライセンスを保持して使用する。GPU timestamp/contextのTracyへの接続はまだ行っていない。

`scripts/test-tracy-cpu.ps1`で無効版はTracyClientをリンクせず、有効版はSDK本体とリンクして実行。両方ともmain/workerの区間とフレーム境界のsmokeに合格。同じ100,000区間ループの単発計測は無効0.0745ms、有効・未接続0.1508ms。接続中の収集負荷、描画性能、GPUトレースの結果には使わない。収集サーバーでのcapture閲覧も未実施。

全体Debugリンクでは、既存のstandalone Release版`ShaderMakeBlob.lib`の検索パスがNRD用Debug版を隠し、LNK2038を起こしていた。NRD有効時は同SDKの構成一致ライブラリを使い、standalone版の検索パスとリンク追加を抑止するよう修正した。

Tracy有効のDebug全体ビルドは成功（`bin/library-tracy-build-enabled-direct.log`）。この環境では通常DebugのPDB生成がC1902を起こすため、ビルドプロセス内だけ`_CL_=/Z7`を指定してデバッグ情報を埋め込んだ。恒久的なDebug設定変更ではない。最初の並列ビルドと開発者環境からの再試行は中断し、以降は単一MSBuildノードで検証した。

OpenColorIOについても、リンク済みの`OpenColorIO_2_3.dll`が出力先になく、配布工程が不足していた。`CopyOcioRuntime`ターゲットを追加し、インストール済みSDKのDLLとLICENSEを出力先へ配置する。DLL不在は明示的なビルドエラーにする。DLLの依存先もdumpbinで確認した。

Tracy有効版の実レンダラーは3フレーム描画して正常終了した（`bin/validation/startup/library-tracy-enabled-desktop-Debug-1.log`、runner exit0）。最初の制限環境からの起動は中断し、デスクトッププロセスへのアクセスを許可した環境で検証した。これは起動・描画・終了のsmokeであり、比較画質やFPSの検証ではない。

既定のTracy無効構成もDebug全体ビルド成功（`bin/library-tracy-build-disabled.log`、`/Z7`使用）と3フレーム正常終了を確認した（`bin/validation/startup/library-tracy-disabled-desktop-Debug-1.log`、runner exit0）。最終実行ファイルはこの無効構成。ON/OFFの起動時間はキャッシュ状態が異なるので性能比較には用いない。Tracy有効ビルドでのライセンス配置ターゲットも追加・実行確認した。

## 残る実装単位

旧`library-adoption-decisions.md`の「採用」は方針を含んでおり、実装完了の意味には使わない。今回のsource/build検索で確認した接続状態:

|候補|確認した状態|次の実装・採用条件|
|---|---|---|
|ACL|実FBX codecと任意再生経路を実装、3実資産で容量・CPU・ローカル誤差比較。既存方式が速いため標準を維持|[比較記録](acl-fbx-comparison-2026-11-01.md)。原キー二重保持を解くcook形式、階層適用後の誤差、VMD用codecは未完了|
|KTX/Basis|実SDKへ接続しKTX2入力・native encode・カラーBasis transcodeを実装。65件の速度・容量・全画素比較で既存キャッシュを維持|[比較記録](ktx2-comparison-2026-11-01.md)。Basis法線channel、単体.basis、実資産の非可逆画質・GPU表示検証は未完了|
|fastgltf|専用DLLのモデル変換backendへ接続。階層・基本材質・skin・静的morph・線形animationを比較。既存で誤読・クラッシュする有効な入力のため、対応glTFは新経路を先行させ、未対応機能はAssimpへ戻す|[比較記録](fastgltf-comparison-2026-11-01.md)。実アニメーション資産で約15%遅い結果があり、コピー削減、非線形animation、extensions、メモリ・画面検証を継続|
|Draco|圧縮glTF meshの属性ID・mapping・型・normalized・skinを実装。容量・速度・誤差をzstd/meshoptimizerと比較し、入力対応として採用|[比較記録](draco-comparison-2026-11-01.md)。単体.drc、cook/export、一般実資産の量子化品質は未完了。既存cacheの自動量子化はしない|
|OpenColorIO|ビルド設定・ColorManager・postprocessからの呼出あり|既存経路との二重変換を確認し、実出力の色・GPU負荷を検証|
|ShaderMake|props設定あり、既存DXCのreflection strip比較済み|既存MSBuildに対する追加実装の役割を整理し、ビルド時間・出力を比較|
|tracy|今回CPU frame/update/drawを任意ビルドで接続、SDK・実レンダラー有効/無効ビルドとsmoke合格|収集クライアントでのcapture確認、接続中の負荷とGPUトレースを検証|
|CACAO / FSR2 / XeSS|製品source/shaderでSDK利用を検出せず|既存AO・TSR/DLSSと切替可能に実装し、同一scene・解像度・入力契約で画質とGPU時間比較|
|RTXDI / STBN / RTXTF|既存照明・samplingがあり、新SDKの採用比較は未完了|DI再利用・sampling・filterの役割ごとに接続。ノイズと時間方向の品質を含め比較|
|D3D12MA / DirectStorage|製品sourceで直接利用を検出せず|既存placed allocator・streamingと予算・所有権・Fenceを揃えて実装・比較|
|OMM / RTXCR / Streamline|SDK導入と製品機能接続を分けて確認が必要|対応GPU・bake・材質・既存DLSS経路の契約を確認して実装|

これらの未実装を「既存の方が速い」と推測して除外しない。反対に、実装しただけで標準へ切り替えない。既存のDLSS/NRD/OptiX、meshoptimizer、物理backend等の統合済み機能も、対応する比較条件のもとで判断する。
