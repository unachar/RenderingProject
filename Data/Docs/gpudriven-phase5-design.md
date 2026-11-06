# Phase 5 実装設計案

状態: ユーザー承認済み。実装とDebug/Release実機検証・GPU Based Validation・Phase 3/4回帰を実施。最新の結果・制約は [実装記録](gpudriven-phase5.md) と [検証台帳](gpudriven-phase5-progress.md) を参照。

## 目的と範囲

ユーザーのPhase 5仕様を実装する。Phase 1〜4のGPU Scene、Object Frustum/Hi-Z、Discrete LOD、ExecuteIndirect、Material、GBuffer、DXRを維持し、Static OpaqueのGeometry StageへMeshlet/Cluster/Mesh Shader経路を追加する。Continuous LOD、Virtual Geometry、Streaming、Software Rasterizationは対象外。

## 確認した既存接続点

- `source/rendering/gpudriven/gpudrivenrenderer.cpp`: Global Vertex/Index/Geometryバッファ、Frame別Visible/Count/Commands、Opaque/Depth PSOを管理。
- `source/rendering/gpuscene/gpuscenetypes.h`: InstanceにWorld/PreviousWorldとLODGroupIndexがある。既存GPUMeshDataのCPU/HLSLレイアウトを同時に管理する必要がある。
- `source/models/staticmodel.h`: SubMeshにCPU Vertex、LOD0と追加LODのIndexを保持している。
- Phase 4はSelected Mesh IDをVisible Instanceへ出力する。Meshlet/Cluster RangeはこのIDから引く。
- `DirectX12.vcxproj`の既存ShaderはSM 5.1。`Directory.Build.targets`には導入済みDXCによるCompute Shaderビルドがある。新しいMesh Shaderはこの仕組みに専用ビルドを追加する。
- 作業ツリーにはPhase 4の未コミット変更がある。これを基準とし、既存変更を取り消さない。

## 選択する構成

独立したMeshlet管理・描画モジュールを追加し、既存GPUDrivenRendererから呼ぶ。既存Renderer全体の再設計は行わない。既存Rendererへ全機能を直接追加する案は責務が集中するため採らない。Amplification Shader内に可視判定を移す案は、初期検証と統計を複雑にするため後続とする。

### AssetとGlobal Data

MeshletBuilderはSubMesh/LODごとにVertex Remap、Local Triangle、Sphere、Normal Coneを作る。設定は初期64 vertices/126 primitives。GPU上限に適合しない設定はImport時に拒否し、Shader出力配列とビルド設定を一致させる。

Clusterは連続したMeshlet Rangeと保守的Sphereを持つ2階層とする。子Sphereを包含するBoundsを作り、Coneが安全に求まらないClusterはCone判定を無効にする。

Import結果はバージョン付きMeshlet Assetとして保存する。Source Geometry、LOD、設定の変更を識別し、古いCacheを使用しない。保存前にCount、全Index/Range、有限値、Sphere包含、Coneを検証する。Cacheの欠落・破損はTraditionalへFallbackし、通常描画中に再生成しない。Debug用Runtime Buildは明示設定とする。

GPU上ではGlobal Meshlet/Remap/Primitive/Bounds/Clusterバッファを容量方式で管理する。GPU Mesh IDごとにLODに対応するRangeを持つ。既存Geometry ArenaのVertexをSRVで共有し、BLAS用Triangle Indexは維持する。Asset削除・容量拡張後も使用中FrameのResourceをFence完了まで保持する。

### GPU可視判定

Object Frustum → Object Occlusion → LOD → Cluster Candidates → Cluster Culling → Meshlet Candidates → Meshlet Culling → Compact Visible Meshlets → Dispatch Argumentsの順。

候補・可視要素はInstance IDとMeshlet IDを保持する。Cluster Cull時には子を列挙しない。Meshlet SphereはWorldへ変換し、Non-uniform Scale/Shearを含む変換で半径を過小評価しない。

ConeはDouble-sided、Two-sided、特殊Materialで無効化する。初期はMirrored Transformおよび安全なCone変換を保証できないTransformでも無効化し、描画欠落を避ける。Phase 4のHi-Z履歴有効条件と保守的投影を再利用し、Meshlet用Biasを分ける。Near Plane交差やMesh内部Camera、不確かな投影はVisibleとする。

Candidate/Visible容量とAtomic Counterに上限を設ける。DebugのMax Meshletsは意図した制限として表示する。通常動作の容量不足では、Geometryを黙って欠落させず、安全なGPU経路へ戻すか保守的な描画を行う。Dispatchの各次元上限を超える件数は複数次元に分解し、Mesh ShaderでGroup IDを線形化する。末尾GroupはCountを確認する。

### Mesh ShaderとFallback

起動時にMesh Shader Tier、必要なShader Model、Device/Command List機能を検査する。対応時のみMesh PSO/Command Signatureを作る。非対応や初期化失敗はEngine起動を継続し、理由をDebug UIへ表示する。

1 Groupで1 Meshletを処理し、既存Vertex形式、World/PreviousWorld、Normal変換、Material IDを使用する。既存Opaque Pixel Shaderの入力契約へ合わせ、PBR/GBuffer/Velocityの形式を維持する。GPU生成ArgumentをExecuteIndirectへ渡し、CPU Readbackを挟まない。

描画経路はCPU Traditional / GPU Driven Traditional / GPU Driven Mesh Shaderから選択する。Mesh Shader選択時も、未対応Material・Geometry・Meshlet欠落Assetは従来経路へ振り分ける。各Instance/SubMeshを一度だけ描画し、Fallbackでも選択LODを維持する。ShadowとDXRは既存経路を保持する。

### RenderGraph・Frame Resource

Global AssetバッファはSRV。候補/Visible/Counterは書き込み時UAV、読み取り時SRV。DispatchはUAVからINDIRECT_ARGUMENTへ遷移する。Pass間の依存とUAV同期をRenderGraphへ登録する。可視・候補・Argument・統計バッファは既存Frame in Flightへ統合し、毎Frame生成しない。

## Debug・計測

Support/Tier/選択Path、各Culling Toggle、Meshlet Bias、Max Meshlets、Meshlet色、Cluster/Meshlet Bounds、Cone Axisを追加する。Object/Cluster/各Meshlet Cull理由を区別する。

Total/Candidate/Visible Meshlets、Clusters、各Cull数、Processed Vertices/Primitives、Global Buffer容量/VRAM、Asset追加サイズを表示する。Object/Cluster/Meshlet Culling、Dispatch Generation、Mesh Shader OpaqueをGPU Profilerへ登録する。Pixel ShaderコストとGeometryコストを分離して評価できる計測方法は実装時に確認する。統計ReadbackはDebug/Validation時のみ、Fence完了後に行う。

## 実装と検証の区切り

1. Feature検出、DXC、Mesh PSO、Triangle Probe、非対応Fallback。
2. Builder、Bounds/Cone/Cluster、Asset保存・再ロード・不正Asset拒否のCPUテスト。
3. Global Upload、GPU Scene/LOD/Material/Velocityを参照するMeshlet描画、従来経路との画像比較。
4. Cluster/候補/各Meshlet Culling、GPU Compact/Indirect Dispatch、Frame Resource/容量制御。
5. Debug表示、統計、Profiler、Stress TestとPhase 3/4回帰。

CPUテストはIndex/Range、Triangle保存、Bounds包含、Coneの安全性、LOD Range、Cache破損、容量/Dispatch境界を確認する。GPU検証ではCube/Sphere、高Polygon100万Triangle、部分表示、1000共有Instance、可能な範囲で50000 Instance、急速Camera移動、内部Camera、Negative Scale、Double-sided、LOD0〜4、Meshlet欠落混在を実行する。

Debug/Releaseビルド、Debug Layer、GPU Based Validation、GBuffer/Velocity比較と既存Phase 3/4回帰を実行し、ログを保存する。性能比較は同じScene・Camera・解像度で複数Frameを測定する。実機未対応・未実行項目は未検証と記録し、全完了条件の証拠が揃うまでPhase 5完了としない。
