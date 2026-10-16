# Phase 11: Production Integration

更新: 2026-10-16。Phase 11の統合コードを追加した。その後のコンパイル指示により、Debug / x64のC++ビルド・HLSLコンパイル・リンク成功を確認した。テスト、アプリ起動、Asset生成の実行、GPU検証、Benchmark採取は行っていない。**Acceptanceの合格やProduction Readyの実証を示す報告ではない。** Phase 9・10の既存ローカル変更は保持した。

## Import・Asset・Entity

- OBJ / FBX / glTF / GLB / Engine Meshの通常ImportからVG生成へ接続する。専用Componentは追加しない。既存StaticModel / MeshのTransform、Material、Entity IDを使う。
- `VGImportSettings`をSource横の`.vgsettings`へ保存する。Enabled、Low / Medium / High / Production / Custom、Position Error / LOD ErrorのAuto、Streaming Priority、Background Buildを持つ。Asset BrowserのMesh欄にInspectorを追加した。
- ProductionはMesh Shaderの64頂点／124三角形制限と、相対LOD誤差0.01を使う。Position ErrorのAutoはMesh extentから決める。
- CPU入力が空、不完全、triangle list以外、不正Index、不正Material partならVGを拒否する。Shader、材質、変形、Device、描画モードの互換性は既存Runtime判定を継承する。
- 1 workerのBackground Buildは入力Snapshotを所有する。実行中を含む入力Queue予算は512 MiB。結果を変更中のMeshへ直接書かず、main/render threadが完了Futureを反映する。再Importは古いFutureを切り離す。待機・拒否・失敗中も元のLegacy Bufferを保持する。
- Scene配置の有無にかかわらず、CPU hierarchyの生成後に圧縮／Page packing／Encoded `.vgmesh`まで作成する。Page preparationだけ失敗した場合も、検証済みCPU GeometryによるFallbackを残す。
- Cache keyはGeometry内容、全Build設定、Builder revision、Material partに基づく。圧縮設定とEncoder revisionはEncoded Cache keyへ含める。CPU cache、Page container、設定、Source indexは一時ファイルから公開する。
- Source横の`.vgderived`に各sectionのEncoded Asset参照、材質名、Material part、外部Texture参照、設定Hash、Source依存ファイルHashを記録する。元のSourceを通常どおり読み込むと、有効なIndexからRoot Geometryだけを読み込み、細部はStreamingへ渡す。`.vgmesh`の直接Importにも対応する。
- Sourceと設定を監視し、変更時は再Import／再Buildする。glTF / GLBの外部buffer、OBJのMTLも依存として扱う。古いGPU MeshはFence完了まで保持する。非対応描画やLegacy比較が必要な場合、元のSourceを持つAssetは通常Importへ戻す。
- Asset DatabaseにMeshと生成済み`.vgmesh`を登録する。同じ通常Mesh AssetのInstanceはModel / Runtime Asset参照を共有する。パス表記違いはModel cacheで正規化する。

## 描画・Streaming

- Auto / VirtualGeometry / Legacyの切替を提供する。初期値はAuto。VirtualGeometry指定も互換性・準備失敗時のFallbackを維持する。
- 既存GPU Instance Culling → hierarchy / cluster traversal → material binning → encoded page decode → Mesh Shader → GBuffer / Velocityへ接続する。透明、非対応masked、独自Shader、変形、Forward等の非対応条件は既存Rendererへ戻す。
- Depthは既存Scene Depthを共有する。既存Camera Cut、Projection、Resize、Hi-Z履歴無効化と、Current / Previous WorldのMotion Vector経路を利用する。
- Directional / Spot / Point Shadowは既存Shadow Pipelineへ参加する。通常SourceのLegacy Geometry、またはStreaming AssetのRoot fallbackを使う。専用のVG Shadow traversalを追加したものではない。
- Editor Pickingは既存Entity Boundsの選択を維持する。GPU Instance IDからは`GpuScene::ResolveEntity(id, generation)`で元Entityへ解決し、古いInstance generationを拒否する。Cluster / PageをEntity IDとして返さない。
- `VGStreamingBudget` API、Page要求数上限、Upload帯域、Resident容量、Priority、Recently Used保護を提供する。現在のPage Pool上限は1 GiB。APIはResident 1..1024 MiB、Upload 0..64 MiBをMiB単位で扱う。
- Root pin、親GeometryへのFallback、checksum / 構造検査、不正PageのUpload拒否、Retry、Copy / Graphics FenceによるRetireを継承する。Evictionは最近使ったPageを保護し、設定Priorityの低いものを先に選ぶ。
- Visible Instance appendをCASで上限内に保ち、異常を記録する。不正Indirect参照はゼロcommandへ置き換える。Fence完了後の診断で異常があれば通常描画へ戻す。Material scatterにも出力範囲保護を追加した。
- Visible Cluster / traversal scratch、Material Binは完了済みfeedbackから次のPrepare時に拡張する。既存Frame Fenceに従って旧Resourceを保持する。選択Cluster / scratchの上限は各4Mで、上限時の既存Fallbackを維持する。
- Page containerはFormat 4、Builder 4、Compression 1。旧2 / 3の読み取りを保つ。新規生成は4。Sourceが存在する場合、非互換データや無効なSource indexは通常Importによる再生成へ戻す。

## 診断・比較・Acceptance用コード

- `VGStatistics`とDashboardにInstance、可視数、登録／Instanced Source triangles、Clusters、Streaming、容量、圧縮、Overflowを公開する。遅延したGPUサンプルには有効性とSerialを持たせる。
- LOD、Cluster、Hierarchy、Material Bin、Compression、Page Residency、Streaming Priority、Occlusion、Overdraw、Geometry Error、Object IDの表示を追加した。Residency / Priorityには実Page状態の表、OcclusionにはHi-Z mip表示とTemporal統計を用意する。Overdrawは選択済みfront-facing Clusterの加算coverageであり、Cullingで捨てたGeometryを含まない。
- GPU時間は既存timestampの実測Passを表示する。Hierarchy / Cluster処理はTraversal内、DecodeはRaster内に含まれる。別測定していない時間を個別の実測値として表示しない。
- opt-inのFinal Validationは通常OBJ Import → VG生成／Load → Render → Readback → Legacy参照比較を行うコードを持つ。Color、Normal、Material、Depth、Velocity、Shadow、Emission / AO、Light shadow slicesを比較する。Moving Instance、Object ID解決、永続Source indexの再利用も検査する。PNGと結果Reportを保存する。
- D3D12 debug queueがない状態をerror 0の合格と扱わない。GPU Based Validationが実際に有効化されたかも確認する。
- DashboardのBenchmark記録、または`DX12_VG_BENCHMARK=1`でCSVを出す。GPU / CPU / Driver / RAMを含むReference hardware keyとsession別ファイルにし、Frame / CPU / GPU時間、FPS、VRAM / RAM、Geometry、Page、Upload bandwidth、Draw / Indirect回数、GPU Passを記録する。未取得のGPU時間は空欄にする。描画モードはProfilerが当該Frameへ記録した値を使う。
- 既存のStreaming Dataset generatorとScene生成を100M / 1B Acceptanceへ接続した。Stress Sceneは`.vgp2`を通常Static MeshとしてImportし、専用ComponentやRuntime pointerの直接注入を使わない。Source trianglesをInstance multiplierだけで水増ししない。
- `scripts/validate-virtualgeometry-phase11.ps1`は既存Engine / Geometry tool binaryを使う実行入口。ビルドを行う処理は含めない。Reference比較、100M / 1B Dataset生成・Streaming stressを選択でき、Timeoutを失敗扱いにする。**このスクリプトは今回実行していない。**

## 実行入口（未実行）

Reference比較は、Debug版Engineの既存binaryを`-EngineExecutable`へ渡して上記scriptを実行する。Stressは`-StressTriangles 100000000`または`1000000000`と既存Datasetを指定する。生成する場合だけ`-GenerateDataset -GeometryTool <既存CPU tool>`を指定する。1B Datasetは数十GiBのディスクを使う。Generatorの空き容量検査を維持する。

Benchmarkは通常操作時の採取と、Readbackを含むValidation実行を分けて評価する。Reference比較中の同期Captureによるstallを、通常描画性能と混同しない。

## 制約と未確認事項

- 全Acceptanceの合否、Release構成のコンパイル可否、画面・Shadow・Depth・Motionの一致、100M / 1Bの安定性、D3D12 / GBV error 0、GPU Hang 0、実測性能は未確認。
- 巨大Datasetそのものの生成と、実測Benchmark Reportの採取は行っていない。追加したのは生成／実行／記録のコードと入口である。
- Legacy / Shadow fallback用Root proxyはSourceの最高詳細Geometryと同一ではない。Streaming開始時の表示維持と全量Upload回避のための粗いGeometryであり、特にShadow品質は実機で評価する必要がある。
- 埋め込みTextureや解決できない依存URIでは、永続Indexの高速Importを使わず元のImporterへ戻す。Scene MetadataとLegacy fallback、CPU cacheを含む全体メモリが圧縮payloadと同率で減るとは主張しない。
- Camera Velocity / Directionから予測位置を作り、現在可視なMesh内の近い将来の詳細PageをGPUでPrefetch要求する。予測は最大128要求／frameで制限し、実要求を先に処理する。実要求が予測要求と重なった場合は優先度を引き上げる。Camera Cutと強制LOD比較では予測を無効化する。画面外Instance全体を先読みするものではない。
- Debug構成のコンパイル成功を、実行検証の成功やPhase 11完了の宣言へ置き換えない。

最終静的確認: `git diff --check` は空白エラーなし。Project / filter XMLとStress定義JSONを読めること、新規C++ 2ファイルのCompile登録が各1件で実体があること、実行scriptの構文診断がないことを確認した。C++ / HLSLのコンパイルと実行を確認したものではない。

追加コンパイル確認: Visual Studio 2026の開発環境で `MSBuild.exe DirectX12.sln /t:Build /p:Configuration=Debug /p:Platform=x64` が終了コード0。HLSL予約語 `triangle` との引数名衝突とMeshletManagerのGPUDrivenRendererヘッダー不足を修正した。PDBマネージャー不一致を回避するため、このビルドの環境変数 `_CL_=/Z7` を使用した。プロジェクト設定は変更していない。警告は残る。ログ: `phase11-compile-debug.log`。アプリは起動していない。

### 後続のウィンドウ拡大不具合修正

上記の未実行記録の後、最大化時の不具合調査のため限定的な実行確認を行った。`RenderConfiguration::Resize` がDepthリソースを再作成してもDepth SRVを更新せず、VirtualGeometryの2段階遮蔽判定で解放済みDepthを読んでGPUが停止していた。Depth再作成時にSRVも更新するよう修正した。

`DX12_STARTUP_SMOKE_FRAMES=140` と `DX12_RESIZE_SMOKE_TEST=1` で起動すると、8 / 40 / 72 / 104フレーム目に1919×677 / 1200×600 / 1920×1080 / 1200×600へ変更する。修正前は1920×1080への変更後に `DXGI_ERROR_DEVICE_HUNG` で中断し、修正後はVirtualGeometry・2段階遮蔽判定・Ray Tracingを有効にした状態で終了コード0、140フレーム完走、Device Removedログ追加なしを確認した。Debug / x64ビルドも成功。ログ: `resize-debug-build.log`、`resize-after-fix.log`。これは共通Resize処理の確認であり、OSの最大化ボタンの直接操作やPhase 11全Acceptanceの検証ではない。
