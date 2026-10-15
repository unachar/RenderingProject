# フェーズ7: Virtual Geometry Page Streaming

状態: 2026-10-15に設計承認済み。実装計画は `docs/superpowers/plans/2026-10-15-virtualgeometry-phase7.md`。実装・実行検証は未着手。

## 目的と完了条件

Hierarchyのメタデータを常駐させ、実ジオメトリを固定サイズのPageとしてSSDから必要に応じて転送する。GPUのHierarchy Traversalが不足Pageを要求し、転送中も常駐する親LODで描画を継続する。Pageの到着後は通常の選択処理で高LODへ戻る。

ユーザー合意済みの範囲は、Page分割、GPU Residency Table、GPU Request、SSD→RAM→VRAM、Copy Queue、VRAM Budget、Eviction、親LODへのFallback、自動高LOD復帰、状態別Page数、Profiler計測、カメラ高速移動への耐性、数千万〜1億triangleの検証、安定後の10億triangle検証。

成功はビルド成功だけでは判定しない。常駐・非同期転送・退避を繰り返しても、穴・重複描画・不正参照・クラッシュが発生しないことをCPU参照選択、GPU readback、実描画で確認する。10億triangle級は前段の安定確認後に実施し、未実施を合格扱いしない。

## 既存構成と変更境界

- `virtualgeometrygpu.cpp`: 現在は全LODのVertices/Trianglesをグローバル配列に連結する。Cluster/Groupには未使用のPageIndexがある。
- `virtualgeometryresources.cpp`: 現在はHierarchyと実ジオメトリを同じRenderGraph経由で全量アップロードする。Page用リソース管理を独立させる。
- `VirtualGeometryTraversalCS.hlsl`: グループ単位の要求伝播と展開重複防止を実装済み。親集合を共有するグラフであるため、常駐条件もこの単位で扱う。
- `VirtualGeometryMS.hlsl` / `VirtualGeometryVelocityMS.hlsl`: 現在の連続配列参照をPage内オフセットと物理スロットによる参照に置き換える。
- `virtualgeometryruntime.h`: 現在はHierarchy、葉Meshlets、全ジオメトリをCPUに保持する。SSDからの実ストリーミングを検証する経路では、全PageのRAM常駐を前提にしない。
- 既存キャッシュv1の読み取りを維持する。Page用キャッシュは別バージョン・別ファイルとして作成し、旧キャッシュを上書きして互換性を失わせない。

既存の未コミット変更を保持し、現在のdev_main作業場所を使用する。自動コミット・push・mergeは行わない。

## Pageとディスク形式

初期の物理Pageサイズは64 KiB。Clusterの頂点と三角形を同じPageに収め、ClusterをPage境界で分断しない。頂点、三角形、Page末尾のパディングを明示し、ByteAddressBufferの4-byte loadがPage外へ出ない配置にする。現在のnative上限64頂点・124三角形なら1 Clusterは十分収まる。

Hierarchy、Cluster/GroupのBounds・誤差・リンク・Root一覧、Page directoryを常駐メタデータにする。ClusterにはPage IDとPage内の頂点・三角形バイトオフセットを保持する。グループが複数Pageにまたがるため、Group.PageIndexだけで可否を判断しない。

Page directoryは64-bitのファイルオフセット、保存サイズ、検証用チェックサムを持つ。Page ID・物理スロット・GPUアドレスの範囲と乗算を検証し、巨大な入力を切り詰めて受理しない。ファイルの作成は一時ファイルからの置換で完了し、破損・途中ファイルは無効として扱う。初期実装は非圧縮とし、圧縮やDirectStorageを必須依存にしない。

Root Clusterを含むPageは固定常駐にする。固定常駐Page数が予算を超えるAssetはnative-readyにせず、既存の完全な描画Fallbackへ送る。

## Residencyとフレームの契約

Pageは `Evicted → Requested → Loading → Resident` を基本状態とする。ここでEvictedは未ロードを含む非常駐状態、LoadingはディスクI/OからCopy完了待ちまでを含む。失敗時は非公開状態へ戻して再試行間隔と失敗回数を記録し、親LODで継続する。

GPU Residency Tableは物理スロット、世代、Residentフラグを持つ。フレーム内で一貫したsnapshotを読み、描画途中に参照先を変更しない。CPUはCopy fence完了を確認したPageだけを次のsnapshotに公開する。公開を待つために未完了Copyを描画queueで待たせることはしない。

物理スロットの再利用には、旧snapshotを参照し得るすべての描画frame fence完了と、そのスロットへのCopy完了の両方が必要。取消後の遅延I/O完了はPage IDと世代で検査し、新しい割り当てに書き込まない。旧Residency Table、upload buffer、command allocatorも対応するfenceより先に破棄・再利用しない。

## GPU Requestとグループ単位のFallback

Traversalで誤差から希望する詳細度を求めた後、希望する切り替えに必要なPageを確認する。不足PageはGPUの重複排除bitsetと固定容量request bufferへ記録する。Request overflowはカウンタに記録し、範囲外書き込みを防ぎ、次フレーム以降に再要求する。描画の可否をRequest書き込み成功に依存させない。

実際の選択は、Rootの完全な常駐cutから開始する。グループを展開できる条件は、希望された展開であること、親集合全体がactiveであること、直接の子集合全体のPageがResidentであること。条件が揃ったときだけ親集合を取り除いて子集合全体を採用する。

共有親グラフでは、あるグループの親集合を供給する複数の上位グループの展開が必要になる。希望の細分化に対する上向きの要求伝播は維持し、実際に到達できたactiveな親集合からのみ下向きに置換する。上位の一部が不足している場合は、下位グループの一部だけを展開しない。Phase 6の単純なrefine bitに個別ClusterのResident条件を加えるだけでは実装しない。

GPUが実際に選択したPageは同時に使用記録へ出す。全グループのメタデータ走査だけを根拠に不要Pageを無制限にRequestせず、可視性と到達可能な詳細化を考慮して段階的に要求する。到着後は次フレームの選択により自動で高LODへ切り替わる。

native選択全体が容量不足や初期化失敗になった場合は、既存のSuccessフラグ契約でinstance全体を従来経路へ送る。native出力と従来出力の二重描画を禁止する。

## CPU I/O・Copy Queue・予算

request/readbackはフレームごとのリングで管理し、完了済みの描画fenceだけを非ブロッキングで回収する。I/O workerがSSDから必要Pageだけ読み、bounded RAM stagingへ載せる。レンダースレッドでディスク読み取り・fence待ちを行わない。

専用Copy Queue、copy allocator/list、copy fenceを用意する。Page poolは64 KiBごとの独立したbuffer resourceまたは同等に所有権を分離できる構成にし、他Pageが描画中でも新規PageへのCopyが競合しないようにする。初期実装では独立bufferとSRV descriptor配列を採用し、Mesh Shaderは物理スロットからdescriptorを選ぶ。単一bufferの一部だけをCOPY_DESTに遷移できるとは仮定しない。

Copy側はCOMMONからCOPY_DESTへの許可された状態処理を行い、Copy完了後にgraphics側で読み取り状態へ遷移する。Eviction後にCopyへ戻す際もgraphics側でCOMMONへの引き渡しとfence完了を確認する。Descriptorの上書きは旧描画完了後に限定する。

初期設定はgeometry pool Budget 256 MiB、1フレームあたり転送上限8 MiB、I/O同時実行4件、CPU staging上限64 MiB。これらは調整可能にし、メタデータ・upload heap・readbackはpoolとは別に実測表示する。固定常駐Pageもpool予算に含め、Loadingが確保済みのスロットも予約使用量として含める。

Evictionは固定常駐でなく、現在公開中・実行中のフレームから安全に退避できるPageを対象に、最終使用frameの古い順に行う。空き不足時は転送を延期して親LODを維持する。予算縮小時は即座に使用中スロットを破棄せず、実使用と目標の差を表示して段階的に縮小する。

## デバッグとProfiler

表示: Resident / Requested / Loading / Evictedの現在Page数、固定常駐Page数、累積Eviction数、request overflow、I/Oエラー、利用スロット数。

Profiler: GPU Page Request数、CPUが受理した重複排除後の件数、SSD read bytes、Copy submitted/completed bytes、geometry poolの使用・予約・予算bytes、メタデータ/CPU staging bytes、CPU request処理時間、I/O待ち時間、Copy完了までの時間。CPU経過時間とGPU timestampを混同せず、Copy QueueのGPU時間は当該queueでtimestampを取得できる場合にのみ表示する。

既存のHierarchy TraversalとMesh Shaderイベントを維持する。同期readbackを使う既存DebugReadbackをStreamingの通常処理には使用しない。

## 検証と実装順

1. Page packing / disk directory / byte decode。deterministic配置、境界、切断ファイル、checksum、64-bit範囲、旧v1読み込みをCPUテストする。
2. CPU参照の常駐cut選択。共有親グラフ、全子到着、部分到着、Rootのみ、再退避を試し、元の被覆の完全性と重複ゼロを検証する。
3. request dedup / bounded queue / 状態機械 / budget / generation。遅延I/O・取消・再要求・古いreadback・frame fence未完了時の再利用禁止をテストする。
4. Copy QueueとPage poolを統合する。GPU readbackで転送byte一致と完了前の非公開を確認し、Debug layerでqueue間状態・寿命を検証する。
5. GPU traversalとopaque/velocityの両Mesh Shaderを接続する。CPU oracleとのcut比較、Request overflow、低予算、native無効、従来Fallbackを検証する。
6. デバッグUI / Profilerと実シーン。I/O遅延・失敗を注入し、近遠移動・瞬間移動・高速旋回・予算縮小・共有Asset・Asset追加を実行する。親/子の描画被覆をreadbackで比較する。
7. 数千万〜1億triangle、次いで10億triangleを試す。source triangle数とinstanceで増やした換算数を区別し、異なるPageが予算を超えるworking setも用意する。RAM/VRAM、page fault、転送量、frame time、エラー数を実測する。入力生成・処理時間やハードウェア不足で未完了の場合は、その条件を記録する。

各段階で対象CPUテスト、変更したHLSL、C++ Debug/Releaseを検証する。Phase 6の独立GPU harnessと36-frame acceptanceも、新しいbindingとPage経路に対応させて回帰確認する。既知の汎用DXR/DLSS smoke timeoutは別問題として記録し、Streamingの合格で解決済みとしない。

## 設計判断

全量アップロードにPage番号だけを追加する方式では、SSD→RAM→VRAMや予算制御を満たさない。単一巨大bufferに対する無条件Copy Queue転送は、graphics側とのbuffer単位の状態管理が競合する。初期実装は独立Page bufferとdescriptor配列により所有権を明示する。reserved/tiled resourceによる物理pool最適化は、この段階の成立条件に含めない。

仕様を確認後、具体的なAPI・変更ファイル・テストコマンドを実装計画に確定してから着手する。
