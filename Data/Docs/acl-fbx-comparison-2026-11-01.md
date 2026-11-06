# ACLと既存FBX再生の比較

## 実装と採用判断

実際のACL SDKによるFBXキー圧縮・復元を実装し、`DX12_USE_ACL_ANIMATION=1`で既存のボーン更新へ接続した。標準は既存のキー参照を維持する。今回の単体測定では既存参照の方が速く、原データもfallback用に保持するため、この接続だけで実行時メモリが減るわけではない。

ACLの圧縮本体は大きく縮むが、その値をファイル全体や常駐メモリの削減率には使わない。容量目的で本採用するには、cook形式・所有権・旧資産読込を整えて原キーと二重保持しない構成の比較が残る。全ライブラリの統合目標は継続中。

## 同じ再生契約での測定

既存FBXは時刻から整数キーindexを作り、位置・回転それぞれのキー数で剰余を取る。mTimeによる補間へ変更すると、圧縮以外の差が混ざる。今回のACL経路はこの整数index方式を保持し、短い繰り返し配列を同じ周期に展開する。周期が合わない入力は旧経路へ戻す。scaleも既存どおり単位scaleとし、VMDのベジェ/IK/morph再生へ混用しない。

最適化ビルド `/O2 /MT /std:c++17`。同一プロセスで全channelのCPUサンプリングを各2,000回、順序を交互に5回測定した。表は中央値で、描画FPSやGPU時間ではない。実クリップの全整数サンプルで位置・回転誤差を検査し、追加512時刻で位置を照合した。

|入力|位置・回転キー配列|ACL本体|削減率|既存2,000回|ACL 2,000回|最大位置誤差（元の単位）|最大回転誤差|
|---|---:|---:|---:|---:|---:|---:|---:|
|XBot.fbx|1,728 bytes|727 bytes|57.93%|0.3398ms|0.9127ms|0|0.02793°|
|idle.fbx|1,250,496 bytes|118,947 bytes|90.49%|0.4058ms|2.5416ms|0.000009064|0.000752°|
|standup.fbx|735,840 bytes|77,911 bytes|89.41%|0.3438ms|2.0911ms|0.000008741|0.01402°|

圧縮と全サンプル誤差検査の合計は順に0.3785ms、51.3612ms、30.358ms。骨階層を適用した後の頂点誤差、実画像、フレーム全体の計測はまだ含まれない。ローカル位置0.0001以下、回転0.05°以下を受け入れ条件にしているが、任意の骨長・階層で同じスキニング誤差を保証する値ではない。

SDKは同梱commit `3ee568542eca4428e1041908b4a644b98e7885bd`、実ヘッダーのversionは`2.1.99`。manifestの`3.1.0`と一致していないため、manifestの値を実行版として報告しない。圧縮本体を永続化する形式は今回まだ定義していない。

## 動作・回帰検証

- 恒等/非恒等姿勢、欠けたsubtrack、非単位のmTime、異なるキー数、負時刻、端点・ループ・小数時刻、move後の所有権を検証した。
- 製品の`TrySampleAclClip`と`UpdateBoneMatrices`全体を抽出したCPU fixtureで、RAW/ACL切替、同一clipの異なる2時刻のブレンド、同時刻の再利用、欠けたchannel、VMD専用経路への分岐を検証した。GPUや後段の階層計算の代用品とは扱わない。
- scene/animationの置換・解放前に圧縮cacheを破棄し、Assimp所有の原キー配列を独自に解放しない。
- 重複channel pointerで出力配列が縮む不具合を再現して修正した。重複は圧縮前に拒否する。
- SDKのnative allocatorは失敗時にnullptrを返し、内部コードが参照するため、単純なbad_alloc捕捉だけでは不足していた。チェック付きallocator、SDK割当の256MiB上限、部分失敗時の全ブロック回収を実装した。割当0/1/8/32回目の失敗注入でfallbackとリークなしを確認した。
- `-Disabled`はACL/RTM include pathなしでコンパイルし、SDK無効時のfallbackを確認した。
- Debug全体ビルド成功。この環境のPDB管理不一致を回避するため、今回もビルドプロセス内だけ`_CL_=/Z7`を使用した。ACL/RTMのライセンス配置も確認した。
- 既存animation preview、resource lifetime、pose-instance loading、scene settingsの試験が合格した。レビューの2指摘は再現・修正・再検証済み。

再現: `scripts/test-acl-fbx.ps1`、`scripts/test-acl-fbx.ps1 -Disabled`、`scripts/test-acl-fbx.ps1 -Benchmark`。主な証拠は`bin/acl-fbx-verified-tests.log`、`bin/acl-fbx-disabled-tests.log`、`bin/acl-fbx-benchmark-final.log`、`bin/validation/acl/report.json`、`bin/acl-fbx-debug-build.log`、`bin/acl-animation-preview-tests.log`。元資産の上書きは行っていない。

`aclcompression.cpp`のVMD用仮実装は今回のcodecではなく、製品ビルドへ追加していない。VMD全体をACL化したという報告には使わない。
