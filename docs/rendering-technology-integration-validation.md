# 技術追加の初回実装・検証記録

対象: 2026-10-30承認の初回設計A/B。100件の候補全体を実装完了した記録ではない。

## 現在の確認結果

- Debug全体buildと個別検証は合格。descriptor重複修正後、保持側・通常削減側の両方でfeatures84フレームとfallback84フレームが合格した。
- 画像比較のため検証時計を固定した最新buildでも、保持側・削減側のfeatures84フレームが合格。5枚（朝・正午・夕方・夜・DLSS）のRGB画素はすべて完全一致した。証拠: strip-retained-fixed-clock-captures.json。最新buildの削減側fallback84フレームも合格し、runner全体がexit0。
- AAは既定OFF。GPU材質/勾配試験とGGX数値実験に加え、実シーン17組のON/OFF比較と限定したDebug GPU時間を取得した。対照は一致したが、時間方向の改善を支持する結果は得られなかった。[実レンダラー評価](geometric-specular-aa-renderer-evaluation.md)に結果と制約を記録。
- 最新の評価用hook追加後も通常features/fallbackは84フレームずつ合格し、以前の基準5画像と完全一致。評価用ソース/CSOは元の既定OFFへ復元済み。

以下には、修正前の失敗と切り分けの経過も残している。過去の失敗記録を最新の合格記録へ置き換えたり、全技術の統合完了として扱ったりしない。

## A: runtimeシェーダー反射情報の削減

source/dxccompilerpolicy.hを追加し、dxccompiler.hのcompiler引数生成へ接続した。D3DCOMPILE_DEBUGなしのcompileに -Qstrip_reflect を追加。Debug指定と DX12_KEEP_SHADER_REFLECTION=1 では従来の情報を保持する。引数は既存cache keyに含まれ、旧cacheを破壊せず両variantを分離する。

既存compatibility compilerを通る経路が対象。shader16bit.hの独立compiler、MSBuild生成済みcso、外部SDKのshaderは初回の対象外。全shader cache容量が同じ割合で減るとは主張しない。

旧cacheを保持するため、移行直後のcache総容量が直ちに減るとは限らない。比較値は生成する各blobのサイズであり、既存cacheの自動削除は行わない。

scripts/test-dxccompilerpolicy.ps1 は実runtime DXC、永続cacheの再読込、override切替、Debug埋込情報、D3D12 WARPのgraphics/compute PSO作成を検証し合格。初回REDは「non-debug shader still contains reflection」で失敗し、実装後GREENを確認した。

|試験shader|反射情報保持|削減後|
|---|---:|---:|
|VS|3568 bytes|2076 bytes|
|PS|4216 bytes|2440 bytes|
|CS|3380 bytes|1872 bytes|
|合計|11164 bytes|6388 bytes|

実際のDeferredPSのDXC比較では425680→417588 bytes（8092 bytes減）。STATのみを除去し、DXIL/HASH/ISG1/OSG1/PSV0/SFI0のpayloadは完全一致。fixtureの削減率を実プロジェクト全体へ外挿しない。速度向上やVRAM削減の測定ではない。

証拠: bin/validation/technology/dxc-policy.log、production-strip-parity.json、DeferredPS-stripped-compiled.cso。

## B: PBRの幾何スペキュラAA

GeometricSpecularAA.hlsliを追加。幾何法線の勾配からGGX alphaの分散を補い、perceptual roughnessへ戻す。scale 0.15、threshold 0.2。法線map/POMの分散filterとは区別する。

DeferredPSの最終材質粗さへPBRクラス11だけ適用する。通常/GPU Driven/VGのPSではdiscard前に微分を取得し、Visibility Computeでは同一三角形の再構築法線差分を使う。Toon/Unlit、Forward透明、RT hit材質、packed impostorは対象外。

既定は GEOMETRIC_SPECULAR_AA=0。評価用にはDXCで -D GEOMETRIC_SPECULAR_AA=1 を指定する。runtime UIや保存設定は追加していない。ヘッダーの既定値をソース変更して評価する場合は関連shader/PSOを再生成し、固定条件で比較すること。engine全体をONにした品質・性能を保証する設定ではない。

既定値はDeferredPS先頭でも定義し、OFF時はAA専用includeとglobalsを除外する。ソースで既定値を変更する評価ではDeferredPS先頭の値を使う（shared mathヘッダーだけを変えてもengine全体は有効にならない）。

## Bの確認済み事項

- shared production式をC++で直接実行し、10201個の独立reference/単調性ケースと、ゼロ勾配・ゼロ設定・NaN/Inf・最大粗さ等の境界を確認。
- DeferredPS、GPUDrivenOpaquePS、GPUDrivenVisibilityResolveCS、VirtualGeometryOpaquePSをON/OFFの8通りでDXC compile。
- RTX 3050 Laptop GPU上でnative D3D12 computeをON/OFF各128ケース確認。finite失敗・Toon/Unlit不変を含む。
- 同GPUのraster試験でON/OFF各512画素を読戻し。平面PBR・Toon・Unlitは粗さ維持、変化する幾何法線のPBR領域128画素だけ補正された。rgba32fキャプチャを保存。
- 元HEADのDeferredPS対既定OFFは、実行DXILだけでなく全container payloadが完全一致。各425680 bytes。ヘッダーchecksum等の外枠だけの比較ではない。
- Visibilityはhelper分割でOFFのDXILが変化することを追加比較で検出した。OFFは元のShade本文を維持し、AA-only helper/globals/includeを除外するよう修正。修正後は元HEADと全container payloadが完全一致し、各457268 bytes（default-off-visibility-parity.json）。ハングの原因と断定した変更ではなく、OFF参照を正確に保つための修正。
- レビューで不足が見つかったproduction visibility試験を追加。fixtureは実際のGPUDrivenVisibilityResolveCS.hlslをincludeし、ReconstructとPrepareVisibilityMaterialGradientsを実行する。隣接三角形の法線・別instanceのtransformを変えても対象面の粗さと勾配は不変。別instanceを選べば変更を検出することも確認。
- 退化・非resident・範囲外primitive、非有限法線、中心は再構築成功して隣接X方向だけ失敗する透視投影を検証。失敗方向の勾配はゼロ。追加後のRTX 3050試験は合格。
- 独立レビューのP2指摘は修正後に再レビューし解消。新しいactionable defectなし。

証拠: bin/validation/technology/geometric-aa-gpu.log、default-off-parity.json、geometric-aa0.rgba32f、geometric-aa1.rgba32f。

追加の[GGX数値実験](geometric-specular-aa-quality-probe.md)では、製品の共有式と画素内積分参照を比較した。低粗さr0.04/0.12では誤差が減り、r0.30/0.80では小さな誤差増加もあった。NDFだけのCPU実験で、実シーン動画・GPU性能の結果とは区別する。既定OFFを維持する。

## buildと回帰検証

Per-shader dependency testsは合格。DeferredPSを明示的に追跡するMSBuild targetへ新includeを追加した。

最初のDebug /m:2 buildは診断を出さずexit 1。直接MSBuildを起動したserial再試行はC++ compile後、既存toolchainのMSPDB140.DLL不整合でLNK1101になった。対応するVS 18のVsDevCmd環境を明示し、/Z7・EnableParallelShaderBuild=false・/m:1・/nr:falseで全体Debug buildを再実行してexit 0。システム設定やprojectのtoolchain設定は変更していない。成功logは bin/validation/technology/build-debug-devenv.log。以前の失敗logを成功証拠として使わない。

native standalone runnerのcompileでは既存dxcshadercache.hのsigned/unsigned比較にC4389警告が出る。今回の変更による新警告として隠さず、既存箇所の変更は行っていない。

## 残る確認と次の候補

勾配helper抽出後の最終Visibility shaderもON/OFF両方で再compileし合格。既存stage lighting/Toon lighting/outline projection/skinning workの回帰試験も合格。全体Debug buildは成功し、976 editor checksと2801 simulation checksも合格。

全体renderer smokeは未合格。最初の試行はshader cache生成中に既定180秒を超えたため終了。検証runnerに StartupTimeoutSeconds（既定180を維持）を追加し600秒で再試行したところ、実描画へ進んだが、XBot startup entity count、frame 35のTSR history、最終smoke sequenceで失敗し、DXGI_ERROR_DEVICE_HUNG / D3D12 error 1 / feature smoke failures 4を報告した。失敗記録: bin/validation/technology/stripped-smoke-failed.log。これを成功や単なるcache待ちとして扱わない。

同一binaryで DX12_KEEP_SHADER_REFLECTION=1 として反射情報を保持した比較試験でも同じframe 35の履歴失敗とDXGI_ERROR_DEVICE_HUNGを再現した。記録: retained-smoke-failed.log。今回のstripが原因とする証拠は得られていない。既存のTSR direct/tiled GPU parityは336比較で合格。DREDは未完了Dispatchを示すが、pass名がなく原因は未確定。VRAMの観測値は1037/4096MiBで、メモリ超過を根拠なく原因扱いにしない。

XBotの失敗は別に切り分けた。startup codeは存在しないbal.fbxを参照し、model load失敗時にentityを作らない。smokeに限り付属XBot.fbxを選ぶように修正し、Debug rebuildは成功（build-debug-fixture.log）。通常の起動パスは維持。実行確認とdevice hangのGPU-based validation診断を進行中。DXR/DLSS無効側の全体smokeは、先行features試験が失敗したためまだ未実行。

XBot修正後の登録チェックは実行でも合格した。GPU-based validation試行は600秒制限で終了し、全体合格ではない。通常負荷の診断もframe34後の待機中に600秒制限で終了した。

既存プロファイラーのSetMarkerには文字列形式の誤りがあった。ANSI名をUTF-16 metadata0としてNULなしで渡していたため、metadata1 ANSI + NULへ修正。DRED表示は長いpassの直前contextも残すよう拡張。Debug再build合格（build-debug-markers.log）、独立レビューで問題なし。これらは診断改善であり、device hang修正済みとは主張しない。参照: [RenderDocのD3D12 annotation形式](https://github.com/baldurk/renderdoc/blob/v1.x/docs/how/how_annotate_capture.rst)。

OFF参照を修正したHLSLでも通常負荷・reflection保持で同じdevice hangを再現した（reference-off-smoke-failed.log）。XBot登録は合格したが、TSR historyと最終sequenceは失敗し、D3D12 errors1 / failures3。OFF guard変更後の8variant/nativeAA試験は再実行して全て合格（geometric-aa-final.log）。

診断用markerはMicrosoft公式PIXのGPU packet encoderへ更新した。External/PIXEventsにcommit b0caa735f8510f4ff60c29ef1c88101620defc5eの公開ヘッダー、MIT LICENSE/NOTICE、出典を保存。Debug x64/ARM64ではGPU packetをmetadata2で送信するpure encoderだけを利用し、CPU PIX APIや追加runtime DLLは使わない。ReleaseはANSI markerへ戻る。Debug/Releaseのnative D3D12 WARP送信はdebug layer有効でerrors0（gpumarkers-native.log）、全体Debug buildも合格（build-debug-pix.log）。実際のDRED context解読とARM64実行は未検証。

反射情報試験も最終再実行で合格（dxc-policy-final.log）。9種類の新規キャッシュが実際に保存され、production cache loaderでchecksum付き読戻しとbyte一致を確認した。単に再コンパイルして同じbyte列を得るだけの試験ではない。

PIX markerを使う全体診断はdevice hangで失敗（renderer-smoke-pix.log / pix-smoke-failed.log）。DREDは今回パス名を解読できた。未完了Dispatch1984/2402はHi-Z Build context1966の後、GBuffer Systems Phase2 context1987の前にある。停止したmipや根本原因は未確定で、TSR履歴失敗だけを根拠にTSR shaderが原因とは扱わない。

scripts/test-hiz-chain.ps1は、実際のprebuilt HiZReduceCS.csoをRTX3050で実行する。5×3、192×128、480×320、960×640の同一texture内mip chainを1×1まで生成し、一定値0.25の最終値とD3D12 debug queue errors0を確認（hiz-chain.log）。非一様depth、全中間画素、productionのmip0 copy、RenderGraph、descriptor寿命までは検証していない。次のDebug診断では各mipの寸法とtexture番号もPIX markerに残す。全体スモークは未合格。追加コードの独立レビューで指摘なし。

その後、固定シーンの静止画と16 camera位相、64件ずつのDebug GPU時間を取得した。PBR球以外の対照は一致したが、球の最大輝度と画面上のframe変化RMSはONで増えた。参照積分やreprojectionを持たないためちらつき得点と解釈せず、改善率やFPS向上を主張しない。AAは既定OFFを維持する。詳細は[実レンダラー評価](geometric-specular-aa-renderer-evaluation.md)。

Hi-Zの調査で、補助source/debug SRV/debug UAVの3スロットがray shadowの3フレーム分SRVと完全に重なる別の不具合を確認。非重複assertを追加すると変更前の配置でC2338になった（hiz-layout-red.log）。補助3スロットだけをquality予約域の後、material previewの前へ移した。ray shadowの番号と他機能の配置は維持し、独立レビューで指摘なし。修正後C++ compileは通ったが、診断中の旧EXEが開かれていてlinkはLNK1168となったため、全体build成功とはまだ扱わない。BuildCurrentは別のdepth SRVを使うので、この重複を今回のhang原因とは断定しない。

mip別markerの旧バイナリ診断は900秒でタイムアウト（hiz-marker-timeout-init.log）。frame34後で止まり、新しいDRED記録は得られなかった。成功やmip特定とは扱わない。終了を確認した後、descriptor修正後のbuildを再実行している（hiz-layout-green-retry.log）。

descriptor修正後のDebug全体buildは再実行でexit0、リンクまで合格した（hiz-layout-green-retry.log）。新バイナリでreflection保持・GBV無効の全体比較を開始した（renderer-smoke-hiz-layout-fixed.log）。featuresが通ればDXR/DLSS無効のfallback側も続けて検証する。実行結果はまだ未確定。

その新バイナリでfeatures、fallbackの両方が84フレームで合格し、runnerはexit0（renderer-smoke-hiz-layout-fixed.log）。各ログはD3D12 errors0 / feature smoke failures0と正常終了を記録した。保持側のログ・画像をretained-layout-features / retained-layout-fallbackへ保存。以前の失敗記録も残す。descriptor修正後に通ったことは確認できたが、単独の因果検証とは区別する。通常の反射情報削減policy（環境値0）での全体比較は現在実行中（renderer-smoke-strip-layout-fixed.log）。

通常policy側もfeatures84フレーム・fallback84フレームとも合格しexit0（renderer-smoke-strip-layout-fixed.log）。ログと画像をstripped-layout-features / stripped-layout-fallbackへ保存。保持側との8bit RGB比較は朝・DLSSが完全一致した一方、正午の平均絶対channel差は1.5488/255（最大148、変化画素26.06%）、夕方0.0268（最大9）、夜0.0581（最大36）。全画像の一致とは扱わない。既存の実行ごとの変動と切り分けるため、保持側featuresを1回再実行している（renderer-smoke-retained-layout-repeat.log）。実DXIL containerの非STAT payload一致と、全体スモークの合格は別の確認事項として保持する。

保持側再実行も84フレームで合格。保持同士の正午でも平均差0.415366/255・最大118があり、同じ設定だけでは画像条件が揃っていなかった（strip-retained-capture-repeat.json）。Worldの固定1/60刻みはRenderBenchmark限定で、スモーク時のProceduralMotionは実時間DeltaTimeで進むことを確認した。画像を見ても動くcubeの位置・遠近形状が異なる。検証モードも既存の固定刻み分岐へ含め、通常操作時の時計は維持する。独立レビューで指摘なし。修正後Debug buildはexit0（build-debug-validation-clock.log）。固定時計での保持側featuresを開始した（renderer-smoke-clock-retained.log）。これだけで全画像の決定性を保証したとは扱わない。

固定時計の保持側も84フレームで合格しexit0（renderer-smoke-clock-retained.log）、ログ・画像をclock-retained-featuresへ保存。固定時計の通常削減側はfeatures/fallbackの両方を実行中（renderer-smoke-clock-strip.log）。比較はこの同時刻の新しい組で行う。

固定時計の削減側featuresも84フレームで合格し、clock-stripped-featuresへ保存。保持側との5枚のキャプチャはRGB画素が完全一致し、平均差・最大差・変化画素率はいずれも0（strip-retained-fixed-clock-captures.json）。これは取得した5枚の一致であり、全将来シーンや時間方向の品質の保証ではない。最新のfallback終了を引き続き確認する。

固定時計のfallbackも84フレームで合格し、runnerはexit0（renderer-smoke-clock-strip.log）。ログ・画像はclock-stripped-fallbackへ保存。初回の実装・回帰検証を通過した記録とし、AA ONの実シーン採用評価や100件全体の統合完了とは区別する。

次の候補は100件の調査表に記録。ACL/zstd/KTX2/Basis/loader、STBN/RTXDI/SHaRC、材質拡張、streaming基盤を独立して評価する。初回A/Bだけで統合目標全体を完了扱いにしない。

## 追加：承認済みzstdキャッシュ統合（2026-10-31）

旧v1読込みを維持し、ヘッダー込みで縮む新規保存だけをzstd v2にする静的統合を実装した。ネイティブ524 checks、実DXC/PSO・cache・thumbnail・AA GPU、Debug全体ビルドが合格。製品features84/fallback84はerrors0/failures0、変更前との5枚×2経路のRGBは完全一致。固定3,207件の比較では125.46→84.97MB（32.27%減）、warm Loadは合計727→913msへ増えた。詳細は[検証と採用判断](rendering-technology-zstd-validation.md)。既存cacheの一括変換なし。100件全体の目標は継続中。

## 出典（初回A/B）

- [ShaderMakeのstripReflection](https://github.com/NVIDIA-RTX/ShaderMake/blob/main/README.md)。ツール本体は依存に追加していない。
- [Filamentのgeometric specular AA](https://github.com/google/filament/blob/main/shaders/src/surface_shading_lit.fs)。数式を参考に本プロジェクトの粗さ契約へ独立実装。外部LUT・素材はコピーしていない。
