# Phase 5 Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans for Native execution, or superpowers:subagent-driven-development if the user selects delegation. Track steps below.

**Goal:** Static OpaqueをGPU選択LODからMeshlet/Cluster単位でCullし、GPU生成ArgumentでMesh Shader描画する。

**Architecture:** GPUDrivenRendererのGeometry ArenaとGPUVisibilityのSelected Mesh IDを再利用する。MeshletManagerがGlobal Asset Data、MeshShaderRendererがFrame別Compute/Indirect描画を管理し、既存Opaque Pixel Shaderの契約を保つ。

**Tech Stack:** C++/DirectX12、HLSL/DXC、既存RenderGraph/GpuSceneBuffer/ImGui/RenderProfiler。

**Spec:** [承認済み設計](../../gpudriven-phase5-design.md)

## 実行結果（2026-10-13）

Nativeで実装。最終構成と検証証拠は [実装資料](../../gpudriven-phase5.md) と [検証台帳](../../gpudriven-phase5-progress.md) を参照する。以下は承認時の計画であり、ファイル分割やProbeの方式は実装時に調整した。

| Task | 実施内容 |
|---|---|
| 1 | 機能検出・PSO・Indirect Mesh Dispatch・Unsupported Fallbackを実機検証。描画Probeは実際のMeshlet描画で代替し、常設しない |
| 2 | Builder、Bounds/Cone、2階層Cluster、Import/Cache、CPU検証。Releaseは保存済みCacheを使用 |
| 3 | Global Buffer、GPU Scene/LOD、PBR/GBuffer/Velocity、Traditional混在Fallbackを統合 |
| 4 | GPU Cluster/Meshlet Cull、Compact、Dispatch Argument、RenderGraph/Frame寿命、Capacity Fallbackを実装 |
| 5 | Debug UI・色・Bounds/Cone Preview・統計・GPU Pass計測を実装。手動UI操作とPixel Shader単独の時間計測は未検証 |
| 6 | Stress Scene、画像/Velocity比較、性能比較、Phase 3/4回帰、独立レビューを実施。最終検証結果は台帳に記載 |

Cluster候補生成とCluster Cullingは1 Compute Passへ統合。Cluster Coneは安全側に無効化。Mesh Shader Opaque時間はRaster/Pixel/Fallbackを含む。DLSS/FSR有効時の画質を含め、未検証項目を完了扱いしない。

## Global Constraints

- Phase 1〜4の未コミット変更を維持する。無関係な変更を含めたCommitを行わない。
- 初期対象はStatic Opaque、Discrete LOD。Shadow/DXR Geometryを維持する。
- 初期Meshlet設定64 vertices/126 primitives。設定とShaderの出力上限を照合する。
- 通常描画でVisible Meshlet/Count/DispatchをCPUへReadbackしない。
- Global Asset Dataは容量不足時に拡張し、Frame別ScratchとFenceによるResource寿命を管理する。
- 全完了条件の証拠を記録する。未検証を成功扱いしない。

## Review Focus

- Cache破損・旧バージョン・設定変更: 不正なGPU Uploadを防ぎFallbackする。Task 2で検証。
- 容量不足とDispatch次元上限: Geometry欠落、Counter overflow、GPU hangを防ぐ。Task 4で境界検証。
- Non-uniform Scale/Shear/Mirror: Bounds過小評価とCone誤Cullを防ぐ。Task 2/4で検証。
- Meshlet Asset有無とMaterial対応の混在: 二重描画・欠落を防ぐ。Task 3/5で画像比較。
- Frame切替中のResize/Asset更新/Camera Cut: Resource寿命とHi-Z Historyを保つ。Task 4/6で実行検証。

## Task 1: Mesh Shader機能検出と描画Probe

**Files:** 追加 `source/rendering/meshshader/meshshadercapabilities.h/.cpp`、`shader/hlsl/MeshletProbeMS.hlsl`。変更 `graphicsdevice.cpp/.h`、`Directory.Build.targets`、`DirectX12.vcxproj`。検証スクリプト `scripts/validate-meshlets.ps1`を追加。

**Interface:** `MeshShaderCapabilities QueryMeshShaderCapabilities(ID3D12Device*)`。結果にTier、Shader Model、利用可否、理由を保持する。

- [ ] Feature Query、Device/Command List interface、Mesh Shader用PSOとIndirect Command Signatureの実機検証を追加する。
- [ ] DXCで `ms_6_5` Probeをビルドし、単一Triangleの出力とUnsupported設定のFallbackを確認する。
- [ ] Debug/ReleaseビルドとDebug Layerの結果を保存する。Probeは通常Sceneに混入させない。

## Task 2: Builder・Cluster・Asset Cache

**Files:** 追加 `source/rendering/meshlets/meshlet.h`、`meshletbuilder.h/.cpp`、`meshletasset.h/.cpp`、`clusterhierarchy.h/.cpp`、`tests/meshlet_tests.cpp`。変更 `source/models/staticmodel.h/.cpp`、`modelmanager.cpp`、プロジェクト設定。

**Interface:** `MeshletBuildConfig`、`MeshletAsset`。`MeshletAsset BuildMeshlets(std::span<const StaticModelVertex>, std::span<const uint32_t>, const MeshletBuildConfig&)`。`bool ValidateMeshletAsset(const MeshletAsset&, uint32_t vertexCount, std::string& error)`。`bool LoadMeshletAsset(const std::filesystem::path&, MeshletAsset&)`、`bool SaveMeshletAsset(const std::filesystem::path&, const MeshletAsset&)`。

- [ ] Triangle保存、Local/Global Index範囲、設定上限、空Mesh、Degenerate、有限値、Sphere包含をテストする。期待値: 元の有効Triangleと再構成結果が一致し、全Countが設定以内。
- [ ] ConeをTriangle Normal分布から生成し、安全でないConeを無効化する。Cluster Sphereが全子Sphereを包含することを検証する。
- [ ] Version/Source Geometry/LOD/Config識別をCacheに保存する。切断ファイル・範囲外・旧Version・設定不一致を拒否するテストを追加する。
- [ ] Importと各LOD構築後に保存・再ロードを接続する。Runtime BuildはDebug明示設定に限定する。
- [ ] CPUテストを `validate-meshlets.ps1 -CpuOnly` から実行し、全PASSを確認する。

## Task 3: Global Upload・Mesh Shader Opaque・Fallback

**Files:** 追加 `meshlets/meshletmanager.h/.cpp`、`meshshader/meshshaderrenderer.h/.cpp`、`shader/hlsl/MeshletCommon.hlsli`、`MeshletMS.hlsl`。変更 `gpudrivenrenderer.cpp/.h`、`framepipeline.cpp`、既存Opaque Shader共有部分とビルド設定。

**Interface:** `MeshletManager::RegisterGeometry(uint32_t meshID, const MeshletAsset&, uint32_t baseVertex)`、`MeshletManager::Prepare(RenderGraph&)`、`MeshShaderRenderer::IsActive()`、`MeshShaderRenderer::Draw(ID3D12GraphicsCommandList*)`。Mesh IDごとのMeshlet/Cluster RangeをGPU構造に保持する。

- [ ] GpuSceneBufferでGlobal Meshlet/Remap/Primitive/Bounds/Cluster/Rangeを確保・Uploadし、Geometry ArenaのVertex SRVを公開する。
- [ ] Mesh ShaderでVertex Fetch、World/PreviousWorld、既存Normal変換、Material/Instance伝達を実装する。Pixel ShaderとGBuffer形式を維持する。
- [ ] CPU Traditional/GPU Traditional/GPU Mesh Shaderの選択と混在Fallbackを実装する。対応判定をCompute側Command生成にも反映し、一度だけ描画する。
- [ ] Cube/SphereのGBuffer・Depth・Velocity比較、Multi-material、Meshlet欠落、未対応Material、LOD欠落を実機検証する。

## Task 4: GPU候補・Cluster/Meshlet Culling・Indirect Dispatch

**Files:** 追加 `meshlets/meshletculling.h/.cpp`、`meshshader/meshdispatchbuffer.h/.cpp`、`shader/hlsl/ClusterCullingCS.hlsl`、`BuildMeshletCandidatesCS.hlsl`、`MeshletCullingCS.hlsl`、`BuildMeshDispatchCS.hlsl`。変更 `meshshaderrenderer.cpp`、Phase 4可視結果の参照インターフェース。

**Interface:** `MeshletCulling::Prepare(RenderGraph&, RenderGraph::ResourceHandle selectedInstances, RenderGraph::ResourceHandle selectedCount)`。出力はFrame別Visible Meshlet List/Count/Dispatch Arguments。要素はInstance ID + Meshlet ID。

- [ ] Selected Mesh IDからCluster RangeをGPU取得し、Cluster Frustum/安全なCone/Hi-Zを判定する。CullされたClusterの子は列挙しない。
- [ ] 可視ClusterのMeshletを候補化し、World Sphere Frustum、Cone、Hi-Zの順でCompactする。Double-sided/Mirror/不確かな変換はCone無効。
- [ ] Phase 4のHistory有効条件・保守的投影を共有し、Near交差/内部CameraはVisible、Meshlet Biasは別設定とする。
- [ ] Counter/Capacity上限とOverflow時の保守的Fallbackを実装する。Debug Max Meshletsの制限を通常Capacity制御と区別する。
- [ ] CountからGPUでDispatch Argumentを生成する。複数次元Group ID線形化、末尾Group、ゼロ件を検証する。
- [ ] RenderGraphにUAV/SRV/INDIRECT_ARGUMENTとUAV同期を登録し、既存3 Frame Resourceへ統合する。
- [ ] Capacity境界、共有Instance mapping、Negative Scale/Shear、Camera Cut/Resize/高速移動をDebug Layer/GBVで検証する。

## Task 5: Debug Visualization・統計・Profiler

**Files:** 変更 `meshshaderrenderer.cpp`、`meshletculling.cpp`、`gpudrivenrenderer.cpp`、`MeshletMS.hlsl`と既存Debug描画接続。

- [ ] Support/Tier/Enabled/Path、各Cull Toggle、Bias、Max MeshletsをImGuiへ追加する。
- [ ] Meshlet色、Cluster/Meshlet Sphere、Cone AxisとCull理由を可視化する。表示ON/OFFとFallback混在を実機確認する。
- [ ] Total/Candidate/Visible/各Cull数、Cluster数、Processed Vertices/Primitives、VRAM/Assetサイズを追加する。Debug ReadbackはFence完了後のみ。
- [ ] Object/Cluster/Meshlet/Dispatch/Mesh Shader/OpaqueのProfiler計測を登録する。Pixel Shader評価には既存Pipeline統計とPS時間分離に使える実機計測手段を確認する。

## Task 6: Stress Test・回帰・完了判定

**Files:** 追加 `source/rendering/meshlets/meshletvalidation.h/.cpp`、`docs/gpudriven-phase5.md`。変更 `scripts/validate-meshlets.ps1`と既存Validation起動設定。

- [ ] Stress A〜Jを自動検証Sceneに実装する。100万Triangle、1000共有Instance、50000以上Instance、LOD0〜4、内部Camera/急速移動/Mirror/Double-sidedを含める。
- [ ] TraditionalとMesh Shaderの画像・Velocityを比較し、部分表示Sceneで可視Geometry削減を確認する。
- [ ] 同条件の複数FrameでCPU Submission/GPU Geometry/総GPU時間/Triangle数を比較する。
- [ ] `scripts/validate-meshlets.ps1`、`-GpuValidation`、`-Configuration Release`を実行する。GPU Hang/Page Fault/OOB/Descriptor/Stateエラーを検出した場合は原因修正後に影響検証を再実行する。
- [ ] `scripts/validate-gpudriven.ps1 -SkipBuild` と `scripts/validate-gpuvisibility.ps1`を単独実行し、共有ビルド出力の競合を避ける。
- [ ] 差分レビューと `git diff --check` を行い、全完了条件を証拠付きで記録する。未実行項目があればPhase 5未完了として明記する。

## 自己レビュー

Asset/Global Resource/LOD/Material/Fallback/Hierarchy/Culling/Indirect/Frame寿命/Debug/性能/Stressの各要件を上記Taskへ対応付けた。Optional AS、Small Meshlet Cull、Async Compute、Continuous LODは追加しない。実装は同じScene/Material契約を跨ぐためNativeで順番に進める方法を推奨する。
