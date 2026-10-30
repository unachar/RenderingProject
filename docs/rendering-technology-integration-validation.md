# 技術追加の初回実装・検証記録

対象: 2026-10-30承認の初回設計A/B。100件の候補全体を実装完了した記録ではない。

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

OFF参照を修正したHLSLで通常負荷・reflection保持・900秒制限の診断を進行中（renderer-smoke-reference-off.log）。OFF guard変更後の8variant/nativeAA試験は、この診断後に再実行する。

固定scene/cameraの動画・時間方向ノイズ・full-frame性能A/Bは未実施で、ちらつき改善率やFPS向上を主張しない。AAは既定OFFを維持する。

次の候補は100件の調査表に記録。ACL/zstd/KTX2/Basis/loader、STBN/RTXDI/SHaRC、材質拡張、streaming基盤を独立して評価する。初回A/Bだけで統合目標全体を完了扱いにしない。

## 出典

- [ShaderMakeのstripReflection](https://github.com/NVIDIA-RTX/ShaderMake/blob/main/README.md)。ツール本体は依存に追加していない。
- [Filamentのgeometric specular AA](https://github.com/google/filament/blob/main/shaders/src/surface_shading_lit.fs)。数式を参考に本プロジェクトの粗さ契約へ独立実装。外部LUT・素材はコピーしていない。
