# Virtual Geometry Phase 7 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** GPUが要求したGeometry PageをSSDから非同期転送し、予算内のVRAMで欠けや重複のない階層描画を継続する。

**Architecture:** 常駐Hierarchyと64 KiBのGeometry Pageを分離する。CPUの状態機械、非同期I/O、Copy Queue、フレーム単位のResidency snapshotを通じてPageを公開し、GPUは完全な親集合・子集合だけを置換する。既存のnative成功フラグによる従来描画への排他的Fallbackを維持する。

**Tech Stack:** C++20、Direct3D 12、HLSL cs_6_5 / ms_6_5、既存MSBuild/CMake/PowerShell検証。

**Spec:** `docs/virtualgeometry-phase7-design.md`（2026-10-15承認）。

**Execution:** 既存のNative実行方針を継承。このチャットで順次実装する。計画確認前には製品コードを書かない。

## Global Constraints

- Pageサイズ64 KiB。Clusterの頂点・三角形を同じPageに収め、Clusterを分割しない。
- native上限64頂点・124三角形、頂点48 bytes、byte三角形indicesを維持する。
- geometry pool Budget 256 MiB、1フレーム転送上限8 MiB、I/O同時実行4件、CPU staging上限64 MiBを初期値にする。
- Root Pageを固定常駐させ、Loadingの予約スロットもpool予算に含める。予算不足のAssetはnative-readyにしない。
- GraphicsとCopy両fence完了前にスロット・descriptor・upload・allocatorを再利用しない。
- 通常フレームでディスク同期読み取り、CPU fence待ち、同期DebugReadbackを行わない。
- 旧キャッシュv1の読み取りを保持し、別バージョンのPageキャッシュを使用する。DirectStorage・圧縮は追加しない。
- 現在のdev_main checkoutと既存未コミット変更を保持。自動commit/push/mergeは行わない（既存記録および.gitのread-only権限に従う）。
- 成功判定にはCPU、GPU、実描画の証拠を要する。数千万〜1億triangleの安定後に10億triangleを試す。

## Review Focus

1. 共有親グラフで一部のPageだけ到着しても、完全な描画被覆を維持する（Task 2/6）。
2. cancel後の古いI/O・古いGPU feedbackが新しいスロット世代を破壊しない（Task 3/4/5）。
3. 全スロット使用中・予算縮小中でも、使用中Pageを破棄せず親LODで継続する（Task 3/5/8）。
4. 動的Asset追加、固定Root超過、キャッシュ破損で部分的なnative出力を公開しない（Task 1/5/6）。
5. 無効化・再有効化・終了時にもCopyとgraphics両方の寿命を守り、従来opaque/velocityを壊さない（Task 5/7/8）。

## ファイルと契約

新規CPUコードは`source/virtualgeometry/`に置き、D3D依存のないpacking・cache・selector・state machineは既存CPUテストtargetへ追加する。新規D3Dコードは同ディレクトリの`virtualgeometrypageio`、`virtualgeometrypagepool`、`virtualgeometrystreaming`へ分離する。`virtualgeometryresources`は常駐メタデータ、`virtualgeometrytraversal`はGPU選択、既存`meshshaderrenderer`はrasterを担当する。

各Taskで新規ファイルを`DirectX12.vcxproj`と`.filters`へ登録し、CPU用には既存portable translation unitと同じPCH設定を使う。CPUテストは`tests/virtualgeometry/CMakeLists.txt`へ追加する。

Task間で使う共通型は`virtualgeometrypages.h`と`virtualgeometryresidency.h`に定義する。`PageId`はuint32、`InvalidPage/InvalidSlot`はUINT32_MAX。ファイルオフセット・bytes・frame serial・fence・世代はuint64とし、GPUに渡す世代はuint32範囲を検査する。世代が上限に達したスロットは再利用せず、idle時のpool再初期化でのみ復帰する。

### Task 1: Page packingとランダムアクセスキャッシュ

**Files:** 新規`virtualgeometrypages.h/.cpp`、`virtualgeometrypagecache.h/.cpp`、`tests/virtualgeometry_page_tests.cpp`。変更`virtualgeometrygpu.h/.cpp`、`virtualgeometryruntime.h/.cpp`、CPU test main/CMake、project/filters。

**Interfaces:**

- `VirtualGeometryPages::Build(const VirtualGeometryAsset&, PagedAsset&, std::string&) -> bool`。PagedAssetは常駐メタデータ、ClusterごとのPage ID・byte offsets、64 KiB payload、固定Root Page一覧を持つ。既存Pack/Appendは旧経路の回帰用に維持する。
- `VirtualGeometryPageCache::WriteAtomic(const std::filesystem::path&, const PagedAsset&, std::string&) -> bool`。
- `VirtualGeometryPageCache::Open(const std::filesystem::path&, PageManifest&, std::string&) -> bool`。Manifestは常駐メタデータとPageEntry `{Offset:uint64, Size:uint32, Checksum:uint64}`、path、cache identityを持つ。全payloadを読まない。
- `VirtualGeometryPageCache::ReadPage(const PageManifest&, uint32_t, std::span<uint8_t>, std::string&) -> bool`。出力はちょうど65536 bytes。全範囲・チェックサム検証後に成功する。

- [ ] `PackingRoundTrip`を追加。全Clusterの頂点・三角形byteが原本と一致し、Page内範囲と4-byte load末尾が65536以下、同一入力の出力が一致することをassertする。
- [ ] `CacheRejectsCorruption`を追加。header/version、directory切断、uint64加算overflow、範囲外Page ID、payload切断、checksum不一致を拒否する。書き込み失敗で既存正常ファイルを残す。
- [ ] 未実装APIでテストが失敗することを確認する。`scripts/validate-virtualgeometry.ps1 -SkipMillion`を使用する。
- [ ] Page header magic `VGP2`、version 2を定義し、little-endianで明示serializeする。Root用Pageを先に配置して通常Pageと分離し、パディングをゼロにする。Manifest自身とmetadataも検証する。
- [ ] runtimeにManifestからnativeを登録する経路を追加する。従来のRuntimeAssetを強制破壊せず、Page専用stress入力ではv1の全payload/葉Meshletsを保持しない。旧描画Fallback用source geometryのRAMは別計上する。
- [ ] 同コマンドでDebug/Release CPUテストがPASSすることを確認する。旧v1 roundtripの回帰を含める。

### Task 2: Resident cutのCPU参照実装

**Files:** 変更`virtualgeometryselection.h/.cpp`、`tests/virtualgeometry_selection_tests.cpp`、既存shared-parent fixture。必要なPage mapping型はTask 1を使う。

**Interface:** `VirtualGeometrySelection::SelectResident(const VirtualGeometryAsset&, const View&, const Settings&, std::span<const uint32_t> clusterPages, std::span<const uint8_t> residentPages) -> ResidentResult`。ResidentResultはselected cluster一覧、deduplicated requested Page一覧、Errorを持つ。既存Selectは変更しない。

- [ ] `ResidentSharedParents`を追加。小グラフの全resident組み合わせを列挙し、固定Rootがある場合は非Resident Clusterゼロ、duplicateゼロ、source coverageがちょうど1回であることをassertする。
- [ ] `ResidentPromotionAndEviction`を追加。Rootのみ→子の一部→全子→再退避の順で、部分到着は完全な親集合を維持し、全到着後のcutは既存Selectと一致することをassertする。Root欠落はErrorとしてinstance全体Fallbackにする。
- [ ] テストの失敗を確認してから実装する。希望refinementの上向きclosureを計算し、Level降順でactive親集合と全子Residencyを検査して置換する。不可視判定がないCPU参照では可視性を模擬せず、GPU比較時にもcullingを無効にする。
- [ ] `scripts/validate-virtualgeometry.ps1 -SkipMillion`で全CPUテストを確認する。

### Task 3: 状態機械・予算・安全なEviction

**Files:** 新規`virtualgeometryresidency.h/.cpp`、`tests/virtualgeometry_residency_tests.cpp`、CMake/project/filters。

**Interfaces:** `ResidencyManager`はD3D非依存。`Request(PageId, uint64_t frame)`、`BeginLoad(PageId) -> optional<LoadTicket>`、`CompleteCopy(const LoadTicket&, uint64_t copyFence)`、`Publish(uint64_t completedCopy)`、`Retire(PageId, uint64_t lastGraphicsFence)`、`Collect(uint64_t completedGraphics, uint64_t completedCopy)`、`SetBudget(uint64_t bytes)`を持つ。LoadTicketはPage ID、Slot、Generation。Snapshot entryはSlot/Generation/Resident、使用記録はsnapshot serialとPage IDを持つ。

- [ ] `DeduplicatedRequestsAndLimits`を追加。同一Pageの多重要求が1件、I/O上限4、8 MiB/frame、64 MiB staging上限、Loading込みの予算をassertする。
- [ ] `DelayedCompletionCannotOverwrite`を追加。cancel→新世代→旧ticket完了、古いfeedback、両fence片方のみ完了を試し、新割当の状態とpayloadが変わらないことをassertする。
- [ ] `BudgetShrinkKeepsPinnedAndInFlight`を追加。固定Pageとframe参照Pageは退避不可、目標超過中は新ロードを延期、完全退役後だけ古い順に再利用、Rootが予算超過した新Assetは拒否する。
- [ ] 失敗を確認して実装する。退避開始時点で将来snapshotから除外し、旧snapshotの最終submission fenceを保存する。参照済みPageを単純なLRU timestampだけで再利用しない。
- [ ] Debug/Release CPUテストを実行する。モデル上のfence完了をGPU完了の実証とは記録しない。

### Task 4: bounded非同期SSD読み取り

**Files:** 新規`virtualgeometrypageio.h/.cpp`、`tests/virtualgeometry_page_io_tests.cpp`、CMake/project/filters。

**Interface:** `PageIo::Submit(const PageManifest&, LoadTicket) -> bool`、`Poll() -> vector<PageIoCompletion>`、`Cancel(LoadTicket)`、`Shutdown()`。Completionはticket、65536-byte payloadまたはerror、read bytes、I/O durationを持つ。Manifestはworker完了までshared ownershipで保持する。

- [ ] `IoReadsOnlyRequestedPages`を追加。実ファイルの必要Pageだけを読み、処理件数・byte数が一致、Submit/Pollが完了待ちをしないことをworker用barrierで確認する。
- [ ] `IoFailureCancellationAndShutdown`を追加。ファイル削除・破損・遅延・cancel・終了を決定的に注入し、古いticketの結果を採用しないこと、4 worker/64 MiB上限をassertする。時間依存のsleepで合否を決めない。
- [ ] 失敗を確認後、bounded worker queueを実装する。rendererからI/Oを呼ばず、workerでTask 1のReadPageを使う。失敗は1、2、4、8、16、30秒上限のbackoffを適用し、待機中も親LODで継続する。
- [ ] CPU Debug/Releaseを実行する。

### Task 5: Copy Queue・Page pool・Residency snapshot

**Files:** 新規`virtualgeometrypagepool.h/.cpp`、`virtualgeometrystreaming.h/.cpp`、`tests/virtualgeometry_streaming_gpu_tests.cpp`、`scripts/validate-virtualgeometry-phase7.ps1`。変更`virtualgeometryresources.h/.cpp`、`graphicsdevice.h`（必要なfence APIのみ）、project/filters。

**Interfaces:**

- `PagePool::Submit(LoadTicket, std::span<const uint8_t>) -> uint64_t`はCopy fenceを返す。`CompletedCopyFence()`、`Collect(completedGraphics)`、`Shutdown()`を持つ。独立64 KiB default buffer、copy queue、allocator/upload ring、専用fenceを所有する。
- `VirtualGeometryStreaming::Register(std::shared_ptr<const PageManifest>) -> uint32_t`、`Prepare(RenderGraph&)`、`RecordFeedbackCopy(RenderGraph&)`、`EndFrame()`、`Shutdown()`。既存ResourcesのAsset ID契約を維持し、asset-relative Page IDはAssetのPageBaseでglobal IDにする。
- Streamingの`ResidencyBuffer()`、`RequestBuffer()`、`UsageBuffer()`、`PageDescriptors()`、`Statistics()`をTask 6/7が使用する。snapshotはframeごと、SRV entryは16 bytes `{Slot, Generation, Flags, Padding}`。

- [ ] GPU harnessに`CopyPublishAndRetire`を追加。Copy完了前は非公開、完了後にPage checksum一致、graphics fence未完了のスロット/descriptorは再利用不可を確認する。Debug layerで状態エラーゼロを要求する。
- [ ] `PinnedAdmissionAndAssetGrowth`を追加。Root過多はinstanceを非ready、既存Asset ID不変、途中追加と予算縮小でも古いframeが参照するmetadata/snapshotを保持する。
- [ ] harnessの失敗を確認してから独立buffer方式を実装する。poolのSRV数は最大16384（1 GiB）とし、初期4096（256 MiB）。設定が上限を超えたら明示clampする。物理capacity変更は非同期retirementを伴い、使用中descriptorを上書きしない。
- [ ] Page descriptorを既存material SRVと同じshader-visible heapの予約範囲に確保する。heap全体を切り替えて既存tableを無効化しない。空き範囲不足・必要なresource binding tier非対応はnative Streaming無効として理由を表示する。
- [ ] COMMON/COPY_DEST/読み取りの引き渡しを実装する。公開前のgraphics passがNON_PIXEL_SHADER_RESOURCEへ遷移し、退避passはCOMMONへ戻してsubmission fence後にCopyへ渡す。Copy未完了をgraphics queueでWaitしない。
- [ ] 既存`GraphicsDevice::GetPendingSubmissionFence/GetCompletedFence`を再利用する。EndFrameが実際のSignalより前かを`renderconfiguration.cpp`と`gpudrivenrenderer.cpp`で確認し、feedbackのfenceが実際のsubmissionを指すよう接続する。
- [ ] readback ringはgpu request/usageをCopyResourceで記録し、完了fenceだけPollする。GPU停止時はdevice-health errorを返し、通常フレームでReadBuffer/WaitForGpuIdleを使わない。
- [ ] `scripts/validate-virtualgeometry-phase7.ps1 -Gpu -Configuration Debug`を作成・実行し、production pool利用、checksum、fence順序、Debug layerの結果を保存する。機能非対応はSKIPとしPASSと区別する。

### Task 6: GPUの常駐cutと不足Page Request

**Files:** 変更`VirtualGeometryCommon.hlsli`、`VirtualGeometryTraversalCS.hlsl`、`virtualgeometrytraversal.h/.cpp`、`tests/virtualgeometry_traversal_gpu_tests.cpp`、`Directory.Build.targets`、shader依存テスト。

**Contract:** 常駐metadataにPageBase/PageCountとClusterのPage内byte offsetsを追加する。CPU/HLSLのstrideとoffsetofをassertする。新規SRV/UAVは既存registerの後に追加し、root signatureは64 DWORD以下に保つ。GPU feedbackはPage単位のrequest bitset、usage bitset、固定65536件のrequest一覧、count/overflowを持つ。

- [ ] GPU harnessをTask 2 oracleと比較する。Rootのみ、子の部分到着、全到着、共有親、再Evict、非ゼロPageBase、request容量1、出力容量不足を追加する。選択cut一致と不足Page要求、範囲外アクセスなしをassertする。
- [ ] 失敗を確認してからgroup stateを希望refine/active/展開済みに分ける。レベル降順のgroup置換で全親activeと全子Residentを確かめ、親を抑制するのは置換成立後だけにする。部分到着で共有親の一部だけを抑制しない。
- [ ] requestは可視かつactiveな親集合から到達する希望の子Pageに限定し、InterlockedOrでframe内重複排除する。overflow時は書き込みせず、親LODを維持し、次frameで再要求する。
- [ ] Successフラグは完全なResident cutの出力予約成功後だけpublishする。Root欠落やscratch overflowはinstance全体従来Fallbackにし、partial nativeを公開しない。
- [ ] 追加reset/selection entry pointを独立shader outputとしてbuild targetsへ登録する。`scripts/test-shader-build-targets.ps1`、CPU suite、`scripts/validate-virtualgeometry-phase6.ps1 -SkipCpu`、Phase 7 GPU suiteを実行する。

### Task 7: Page raster・UI・Profiler

**Files:** 変更`VirtualGeometryMS.hlsl`（velocityはincludeで共有）、`meshshader/meshshaderrenderer.cpp`、`virtualgeometrydebug.cpp`、`renderprofiler.h`、`Directory.Build.targets`、GPU raster harness。

**Contract:** descriptor配列は`ByteAddressBuffer GeometryPages[16384] : register(t0, space2)`、Residencyは`t39,space0`とする。既存opaque rootにPage tableとResidency root SRVを追加し、material texture space1を保持する。Page選択には`NonUniformResourceIndex(slot)`を使用する。Page entryはcluster.PageIndex+asset.PageBaseから取得し、頂点・三角形を同じbufferからbyte offsetで読む。

- [ ] `PagedRasterMatchesContiguousReference`を追加。非ゼロPage・非ゼロoffsetの親/葉/混在を描画し、center depthとGBuffer byte比較で空画像の偽PASSを防ぐ。
- [ ] `PagedVelocityAndFallback`を追加。移動instanceのvelocityを従来経路と比較し、unsupported material、native無効→有効、Root未到着時は片方の経路だけが描画することを確認する。
- [ ] 失敗を確認後opaque/velocityのPage参照とRenderGraphのread依存を接続する。従来meshlet/traditional PSOとindirect root constant indexを維持する。新root引数が未使用の従来shaderにも安全なbindingを与える。
- [ ] UIに設定と4状態Page数、固定常駐数、累積Eviction、overflow/I/O error、目標/予約/実使用bytesを表示する。`Streaming: later phases`表記を実装済みの範囲に更新する。
- [ ] Profiler snapshotにStreaming値を追加する。request GPU/CPU件数、SSD bytes、Copy submitted/completed bytes、pool/metadata/staging bytes、CPU処理/I/O/Copy完了時間を区別する。Copy GPU timestamp非対応ではN/Aを表示する。
- [ ] Phase 7 GPU tests、HLSL増分テスト、Debug/Release C++ build、Phase 6の36-frame acceptanceを実行する。変更前からあるDXR/DLSS汎用timeoutは別記録とする。

### Task 8: 高速カメラ・大規模stress・証拠とレビュー

**Files:** 新規`source/virtualgeometry/virtualgeometrystreamingvalidation.h/.cpp`、`docs/virtualgeometry-phase7-validation.md`、`docs/virtualgeometry-phase7-progress.md`。変更`source/core/renderingvalidation.cpp`への最小hook、Phase 7 script、必要なproject登録。

- [ ] deterministicな移動経路で近遠、高速旋回、teleport、I/O遅延・失敗、低予算・予算縮小、asset追加、無効化/再有効化を実行する。selected source coverage、duplicate、depth穴、native/従来排他、error数を記録する。
- [ ] `validate-virtualgeometry-phase7.ps1`に`-Runtime -Configuration Debug|Release`、`-StressTriangles 30000000|100000000|1000000000`を追加する。runtimeはprocessの非同期pollで扱い、60秒以内ごとに進捗取得できるようにする。既存設定ファイルを変更した場合はfinallyで復元する。
- [ ] stress入力をディスクに分割生成し、Page単位で読む。source数・LOD追加数・instance換算数・unique Page数を分ける。最低でもunique geometryのworking setが設定pool予算を超える入力でEvictionと再要求を確認する。
- [ ] 3000万→1億triangleで検証する。各実行のhardware、RAM/VRAM、frame time、resident/request/loading/evicted、転送bytes、Streaming時間、穴/重複/Debug layer errorを保存する。どちらかで問題が出たら原因を修正してその条件を再検証する。
- [ ] 前段安定後に10億triangleを実行する。時間・SSD容量・hardwareの制約を実測し、未完了なら何が未確認かと再開コマンドを記録する。instance換算だけでunique 10億triangle合格とはしない。
- [ ] 最終Debug/Release CPU suite（Release millionを含む）、必要なshader/build/GPU/runtime回帰を実行する。有効な同一変更の成功証拠は再利用し、変更した箇所だけ追加検証する。
- [ ] 全変更を読み返し、名前・責務・CPU/HLSL layout・generation・両queueの寿命・部分cut公開・予算計上をレビューする。独立reviewを利用する場合は、利用可能なレビューtoolを使い、許可なく別チャットやsubagentを作らない。
- [ ] 要件ごとに実装箇所と検証証拠をvalidation文書へ記録する。未検証条件と既知制約を明示してユーザーへ結果を報告する。

## 計画の自己レビュー

設計書の全要件をTask 1〜8へ割り当てた。packing→Manifest→LoadTicket→fence→snapshot→cut→rasterの依存順を固定し、CPUモデルと実GPUの検証を区別した。5つのReview Focusに対応するテストを各Taskへ記載した。新API名と型の依存は上記Interfacesに統一した。

まだ実行していないコマンドを合格とは扱わない。Page descriptor予約の実装、device resource binding tier確認、巨大入力の実測は実装・検証Taskであり、この計画作成時点の達成ではない。
