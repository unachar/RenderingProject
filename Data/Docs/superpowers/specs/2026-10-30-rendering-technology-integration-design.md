# 既存レンダラーへの技術追加: 導入設計

日付: 2026-10-30。状態: ユーザー承認済み。初回A/Bの実装と検証は docs/rendering-technology-integration-validation.md に記録する。

## 目的と選択済み方針

指定100件の調査をもとに、品質・速度・VRAM・配布容量・起動/読込時間の改善が見込める部分を既存C++/DirectX 12/HLSLへ追加する。ユーザーは「既存実装へ段階的に追加」を選択済み。候補を一括有効化せず、個別比較できる状態で接続する。未測定の向上率は約束しない。

調査表: docs/rendering-technology-survey-2026-10-30.md。100件のREADME一次調査は完了。全ソースの監査と統合全体は未完了。

## アプローチ

採用: 既存経路を残し、1候補ずつfeature/compile切替、比較、採用判定を行う。小さな改善を早く検証でき、重複SDKと資産再変換を抑えられる。ユーザーが選択した方式。

比較案: RenderGraph・材質・IO・animationを新基盤へまとめて置換する。API統一の利点はあるが、既存VMD/Toon/NRD/VGとeditorの再統合、広い回帰検証、容量増の費用が大きい。現時点では採らない。

## 既存契約

- LTC/DFG、多重散乱補償、ReSTIR GI、放射照度cache、NRD、DLSS、BLAS圧縮、placed transient資源、BC/MIP cache、GPU skinning、DQS、Toon輪郭がある。これらを未実装として再導入しない。
- engineのsource検索ではDXIL reflection API利用を発見していない。採用前にruntime compile利用先とPSO生成を確認する。
- runtime compilerは source/dxccompiler.h。debug指定で -Zi/-Qembed_debug を追加する。cacheは引数をhashするため、追加strip引数をcache keyへ含められる。
- 通常/GPU Driven GBufferは DeferredPS.hlsl を共有する。Visibility Computeも同ファイルをincludeし、GPUDrivenVisibilityResolveCS.hlsl の Reconstruct で同一三角形の隣接位置を再構築している。
- GBuffer Material.g はperceptual roughness。照明側で roughness^2 をGGXのalphaとして使う。Toonクラスは既存の IsLiveToonClass 契約に従う。

## 初回実装A: runtimeシェーダーcacheの容量削減

ShaderMakeのstripReflection機能を参考に、engine内runtime DXCへ -Qstrip_reflect を追加できるようにする。ShaderMake本体の依存は追加しない。対象は runtime compile/cache のobject blobのみ。MSBuildの事前生成csoや全SDKのshaderをこの初回で一括変更しない。

既定はD3DCOMPILE_DEBUGが指定されないcompileのみstrip。debug compileは現在の埋込情報を保持する。DX12_KEEP_SHADER_REFLECTION=1でstripを抑止し、元のobjectを取得できる。compiler引数を作成してからcache pathを計算し、両者のcacheを必ず分離する。既存cacheの削除・上書き変換は行わない。root signature、入力署名、実行DXIL、PSOに必要な検証情報は維持する。

検証: 実際のDXCでPS/VS/CSを旧/新引数でcompileし、objectの反射partを確認。実行DXILと署名/PSV/root-signature等の必要partが一致すること、PSO作成とDebug描画smokeに回帰がないことを検証する。debug保持とoverrideの引数・cache分離をproduction helperを使う試験で確認する。実byte数を報告し、FPS/VRAM削減とは呼ばない。strip compile失敗時は壊れたblobをcacheへ保存しない。既存error/fallback処理を維持する。

## 初回実装B: PBR幾何スペキュラAA

参照は Filament の shaders/src/surface_shading_lit.fs の computeKernelRoughness / normalFiltering。法線変化の大きい小三角形や遠景のPBR鏡面ハイライトを安定させる。post AAの二重実行を増やさず、GBuffer粗さに有限範囲のfilterを掛ける。

初回の対象はDeferred通常/GPU Driven/再構築可能なVisibility GBufferのPBR。Forward透明・RTのhit材質・packed impostor・Toon/unlitへ無理に適用しない。一次表面の変更がGBufferを読む照明/反射へ伝わることを確認し、これらの経路差を記録する。対象外の経路を対応済みと報告しない。

計算は幾何の補間法線のみを使う。normal/height mapを適用する前の正規化法線Nを保存し、variance = scale * (dot(dNdx,dNdx) + dot(dNdy,dNdy))、kernel = min(2*variance, threshold)、rFiltered = (saturate(r^4 + kernel))^(1/4)。微分ゼロ・scaleゼロ・thresholdゼロなら入力rを厳密に返す。粗さは有限の0..1、varianceは非負に制限する。normal-mapやPOMの高周波まで解決したとは扱わない。

PSでは法線のddx/ddyをdiscard前のuniformな位置で求める。Computeではddx/ddyを追加せず、Reconstructで得た同一三角形の法線を正規化し、VisibilityNormalDx/Dyとして差分を渡す。再構築失敗・非有限・退化三角形ではその方向の差分をゼロとする。他三角形や他instanceの法線を参照しない。粗さfilterはpart/assetの最終粗さ選択の後に一度だけ適用し、POMの材質alpha/shadow encodingを維持する。

初回はcompile切替による評価版とし、既定OFF。評価用のvariance scale/thresholdは共有定数として0.15/0.2を初期値にする（実装者提案であり画質承認値ではない）。出力/内部解像度を固定した比較を先に行う。dynamic resolutionやDLSS比率に対応した補正とruntime UI/保存は次の変更で設計し、初回で全設定へ既定ONにはしない。OFFなら旧経路の計算と出力を保持する。

検証: production計算のC++/HLSL共用試験で、平面・最大粗さ・ゼロ設定・大勾配・0付近の粗さ・有限性・単調性を確認。通常/GPU Driven/Visibility ComputeでON/OFFをDXC compile。PBRの平面と細三角形を使ったGPU fixtureでOFFとの一致/相違条件、Toon不変、三角形境界での異常blurを確認。固定camera/seedのA/AとA/B画像を保存し、静止/移動cameraで時間方向変動、平均画質差、pass/full frame GPU msを記録。compileだけでちらつき改善を主張しない。

## 初回の完了条件

AとBを別変更単位でreview・検証し、差分/確認コマンド/結果/未検証を文書化する。Aの容量減とBの画質改善は独立に判定する。AまたはBが有効な比較で改善しなければ、失敗結果を残し旧経路を既定とする。新規依存ライブラリを大量に追加する必要はない。自動commit/push/PRはこの設計の必須工程にしない。

## 後続候補

1. ACL/zstd/KTX2/Basis/fastgltf/ufbx: 既存資産のcook・容量・decode・精度比較。ロード形式の変更はversionと旧読込を定義してから。
2. STBN/RTXDI/SHaRC/SSSR/CACAO: 既存sampling/NRD/TSRの入力契約を保って1種類ずつ評価。
3. 顔SDF、髪の接線ハイライト、PBR肌SSS、iridescence/OpenPBR/MaterialX: 既存材質分類へ独立追加する設計を作る。
4. DirectStorage/RTXTS/OMM/RTXCR/NRC/D3D12MA: 対応API/GPU/利用条件とprofileで必要性を確認してから、cook・所有権・予算・fallbackを接続する。

各候補の個別採用判断は調査表に追記し、未採用は理由を残す。新しいという理由だけで既存の処理と二重に実行しない。画質・負荷・容量のいずれかを悪化させる場合は用途を限定し、比較できる設定を持つ。

## 実装者が決めてよいこと

新しいhelper/test/scriptのファイル名、既存runnerの利用、fixtureの小さな配置、計測ログ名は既存規約に合わせて決める。テストの境界値・失敗系と可読性を維持する。ライセンス本文を確認せず外部コードやLUTをコピーしない。Filament式を参照して実装する際は必要な出典・NOTICEを付す。

## 進捗と制約

この文書作成時点で製品実装・build・実行・性能測定は未着手。READMEの一次調査を全ソース探索完了とは扱わない。GPUやtoolが利用できなければ、実行済みの範囲と保留を明記する。統合目標は初回A/Bだけで完了扱いにしない。

独立builder checkは関連するruntime compiler、Deferred GBuffer、Visibility Computeを確認し、重要な未決事項なしと判定。PSの微分を早期discard前に置くことと、Computeの再構築法線の有限性確認を実装時に再確認する。

ローカルの設定済みDXC実行ファイルは存在し、helpで -Qstrip_reflect / -Qembed_debug を確認。実際のAPI compileとpart比較は実装工程で行う。設計承認を依頼済みで、製品変更は承認後に開始する。

