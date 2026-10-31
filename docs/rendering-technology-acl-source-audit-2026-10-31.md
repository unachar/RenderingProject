# ACLアニメーション圧縮の統合前調査

候補42のACLを、既存のFBX/VMD再生に追加できるか調べた。ACL圧縮・展開器はまだビルドしておらず、製品の再生処理も変更していない。今回の成果は固定ソースの契約確認と、既存FBX3クリップのネイティブ調査。

## ソースと依存

調査したdevelopの固定commitは`3ee568542eca4428e1041908b4a644b98e7885bd`。`version.h`の値は2.1.99であり、安定版2.1.0として扱わない。公式latest release APIはv2.1.0（2023-12-07）を返した。実際の圧縮比較を行う段階では安定版のcommitも固定し、APIと結果をその版に対応させる。

ACLと、当該commitが参照するRTM `113ca0d0c1824cd66662ff2edab4a45c56772759`のMIT LICENSEを読んだ。`.gitmodules`にはRTM、sjson-cpp、Catch2、benchmarkがある。ACLの公開APIはRTM型を使用する。sjsonは`ACL_USE_SJSON`を有効にする統計・テキストclip I/Oで使う依存であり、テスト・ベンチ一式を製品依存に自動追加しない。

raw track arrayはtrack数、各trackのサンプル数とsample rateを揃える。transform trackには出力index、parent index、precision、shell distanceがある。元の非等間隔キーやVMDベジェをそのまま圧縮する仕組みではなく、raw sampleへの変換誤差とACL圧縮誤差を分ける必要がある。

復元contextは`seek`と`decompress_track(s)`を持ち、rounding policyに補間なし/floor/ceil/nearest等がある。loopのclamp/wrapはdurationや末端補間に影響し、非loopにwrapを使わないよう明記されている。assertは既定で除去され、設定は同じライブラリ内の各TUで統一する必要がある。浮動小数点例外の状態も復元APIの前提なので、製品全体のFPU設定を推測で変えない。

誤差指標は親子transformとshell distanceに依存する。既定のqvv metricは非一様scaleの結合によるshear/skewを完全には扱わず、matrix3x4 metric等の選択がある。本プロジェクトの座標単位やskinningとの一致を確認する前に、文書例の0.01cmをそのまま許容誤差として採用しない。

## 既存実装との接点

`source/models/animationpose.cpp`の汎用FBX経路は、時刻をticksへ変換し、`floor(timeInTicks) % keyCount`に相当するキー番号で位置・回転を選ぶ。`mTime`による探索やキー間補間は行わず、scaleは単位scaleで行列化する。アニメーション間のblendと、同一clip内の時刻補間は別である。

一方VMDは別経路でベジェ補間・サンプルcursor・retarget・root motion・IK・morph・camera等を保持している。まずFBXのclip samplingを独立した候補にし、ACLだけでVMD全体を置き換えられるとは扱わない。

現在の汎用骨階層更新は、`m_Bone`に含まれないnodeではそのnodeのtransformを加えず子へ進む。ACLへ全Assimp nodeの親子関係をそのまま渡すと、無圧縮でも現在のruntimeと異なる可能性がある。親index・bind pose・helper node・scaleの契約を先に揃え、補正が必要なら圧縮とは別の変更として検証する。

## 実FBXの比較

既存Assimp DLLとヘッダーを使ったread-only probeを作り、外部clip読込みと同じ`aiProcess_ConvertToLeftHanded`で3ファイルを読み込んだ。キー時刻の有限・昇順を検証し、60Hzの半フレーム時刻で現行の整数キー選択と、位置linear/回転slerpのキー時刻補間を比較した。元モデル読込みの全postprocessやruntimeのボーン選別・合成までを再現したものではない。

|入力|秒数|channels|各position/rotation/scaleキー数|キー配列bytes|最大位置差（ソース単位）|最大回転差|
|---|---:|---:|---:|---:|---:|---:|
|XBot.fbx|0.03333|35|36 / 36 / 36|2,592|0|約0°|
|idle.fbx|8.33333|52|26,052 / 26,052 / 26,052|1,875,744|0.0367569|5.29906°|
|standup.fbx|6.06667|42|15,330 / 15,330 / 15,330|1,103,760|0.851067|3.44376°|

XBotはこの読込みではほぼbind poseで、圧縮の主な動作評価には不足する。idle/standupの合計キー配列は2,979,504bytes。これは構造体の`sizeof`による配列部分だけで、FBXファイル容量・scene全体のメモリ・ACL圧縮後容量ではない。

idleの最大角度差はLeftHandThumb3、standupはRightHand、最大位置差は両方Hips。standupのRightHandでは少なくとも一部のキー時刻が整数キー番号と一致しなかった。半フレームの補間差を含み、現在の再生が見た目でどれだけ破綻するかや、どちらをユーザーが望むかをこの数値だけでは断定しない。

差分は全imported channelのローカル値であり、runtimeのweighted boneだけの値、階層後の末端位置、skinned vertex、画面上の誤差ではない。ACLがこれらの差を生んだ結果でもない。今後のACL比較は、raw resampleの再生方式を先に定義し、その同じraw sampleとの圧縮誤差を測る。既存のstepped再生を維持する実験なら、ACL側のfloor roundingも揃える。

## 次の検証

idle/standupを最初の実データとして、sample rate・loop/end・bind/default transform・親indexを明示した無圧縮sample列を作る。raw列の容量と既存キー配列の容量を別記し、ACL出力サイズ、encode/seek/decode時間、ローカル位置/角度と階層後・skinned vertex誤差を比較する。ACL型からAssimp/DirectX行列への変換も往復検証する。VMDと製品再生処理はこの段階では維持する。

## 証拠と再現

```
./scripts/probe-animation-sampling.ps1 -Report <new-report.json>
```

native probeは`tests/animation_sampling_probe.cpp`。既存Assimp DLLを検証出力へコピーするだけで、製品依存・アセット・再生設定は変更しない。最終ログ`bin/validation/technology/animation-sampling-details.log`はexit0、3clip合格。最終JSONは同階層`animation-sampling-details-20261031.json`。比較したchannel/timeはXBot70、idle26,000、standup15,288、すべて有限だった。初回の一覧reportも残す。

出典：

- [ACL raw track契約](https://github.com/nfrechette/acl/blob/3ee568542eca4428e1041908b4a644b98e7885bd/docs/creating_a_raw_track_list.md)
- [ACL復元とrounding](https://github.com/nfrechette/acl/blob/3ee568542eca4428e1041908b4a644b98e7885bd/docs/decompressing_a_track_list.md)
- [ACL誤差指標](https://github.com/nfrechette/acl/blob/3ee568542eca4428e1041908b4a644b98e7885bd/docs/error_metrics.md)
- [ACL loop](https://github.com/nfrechette/acl/blob/3ee568542eca4428e1041908b4a644b98e7885bd/docs/handling_looping_playback.md)
- [ACL assert契約](https://github.com/nfrechette/acl/blob/3ee568542eca4428e1041908b4a644b98e7885bd/docs/handling_asserts.md)
- [ACL LICENSE](https://github.com/nfrechette/acl/blob/3ee568542eca4428e1041908b4a644b98e7885bd/LICENSE)、[RTM LICENSE](https://github.com/nfrechette/rtm/blob/113ca0d0c1824cd66662ff2edab4a45c56772759/LICENSE)
