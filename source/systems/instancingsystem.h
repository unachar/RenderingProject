#pragma once

#include "systembase.h"
#include "ecs.h"
#include <d3d12.h>
#include <wrl.h>
#include <array>
#include <vector>
#include <functional>
#include "workgraphruntime.h"

enum class InstanceKind : UINT8;
struct InstanceBatch;
struct BatchKey;
struct DrawContext;
struct BatchBuilder;
struct MaterialComponent;
struct AnimationModelComponent;
struct SpriteComponent;
struct MeshComponent;
struct MeshData;
struct StaticMeshData;
class AnimationModelResource;
class StaticModelResource;

class InstancingSystem final : public SystemBase
{
  public:
    static bool CanInstance(EntityID entity);
    static bool IsEntityVisible(EntityID entity);
    static bool RegisterMeshPipeline(ID3D12Device* device, ID3D12PipelineState* pipeline,
                                      const D3D12_GRAPHICS_PIPELINE_STATE_DESC& description, bool shadowPass);

    static bool IsAvailable()
    {
        return s_Available;
    }

    void Init() override;
    void Uninit() override;
    void Draw(RenderPass renderPass, bool receivingPostProcessOnly) override;

  private:
    static uint64_t HashMaterial(const MaterialComponent* material);
    static const char* ResolvePixelShader(EntityID entity, InstanceKind kind);
    static bool IsBoundsVisible(EntityID entity, const XMMATRIX& viewProjection, const XMFLOAT3& fallbackCenter,
                                const XMFLOAT3& fallbackExtents, bool hasFallbackBounds);
    static BatchKey MakeKey(EntityID entity, InstanceKind kind, ID3D12PipelineState* pso,
                            D3D12_GPU_VIRTUAL_ADDRESS vertexBuffer, uint64_t geometryHash, UINT vertexCount,
                            D3D12_GPU_VIRTUAL_ADDRESS indexBuffer, UINT indexCount, int textureIndex, int normalIndex,
                            const MaterialComponent* material, UINT meshIndex = 0,
                            const AnimationModelComponent* animation = nullptr);
    bool SetupDrawContext(RenderPass renderPass, DrawContext& ctx);
    bool AcceptsPass(EntityID entity, const MaterialComponent* material, const DrawContext& ctx);
    bool IsCameraVisible(EntityID entity, const XMFLOAT3& center, const XMFLOAT3& extents, bool hasBounds,
                         const DrawContext& ctx);
    void BuildShadowBatches(DrawContext& ctx, BatchBuilder& builder);
    InstanceBatch CreateAnimShadowBatch(const MeshData& mesh, AnimationModelResource* model, UINT meshIndex,
                                        ID3D12PipelineState* pso);
    InstanceBatch CreateStaticShadowBatch(const StaticMeshData& mesh, StaticModelResource* model, UINT meshIndex,
                                          ID3D12PipelineState* pso);
    InstanceBatch CreateNonIndexedShadowBatch(const SpriteComponent& sprite, ID3D12PipelineState* pso, const BatchKey&);
    InstanceBatch CreateNonIndexedShadowBatch(const MeshComponent& mesh, ID3D12PipelineState* pso, const BatchKey&);
    void ExecuteShadowBatches(const DrawContext& ctx, vector<InstanceBatch>& batches);
    void BuildMainBatches(DrawContext& ctx, BatchBuilder& builder);
    void ExecuteMainBatches(const DrawContext& ctx, vector<InstanceBatch>& batches);

    // GPU カリング（視錐台・LOD・インポスター置き換えはすべて GpuInstanceCullLodCS）。
    struct GpuBatchRecord
    {
        UINT FirstInput = 0;
        UINT Count = 0;
        UINT BatchIndex = 0;
        bool Valid = false;
    };
    struct GpuCullView
    {
        XMFLOAT4 Planes[6]{};
        bool EnableFrustum = false;
        UINT MinimumLod = 0;
        bool Impostor = false;
    };
    bool CullBatchesOnGpu(const DrawContext& ctx, vector<InstanceBatch>& batches, const GpuCullView& view,
                          vector<GpuBatchRecord>& records);
    void DrawCulledBatch(const DrawContext& ctx, const InstanceBatch& batch, const GpuBatchRecord& record,
                         const function<void()>& bindGraphics);
    void FinishGpuCulling(const DrawContext& ctx);


    static constexpr UINT kMaxInstancesPerFrame = 32768;
    static constexpr UINT kMaxBatchesPerFrame = 4096;

    struct GpuInstanceInput
    {
        XMFLOAT4X4 World{};
        XMFLOAT4 LocalCenter{};
        XMFLOAT4 LocalExtents{};
        XMFLOAT4 LodDistances{};
        XMFLOAT4 Impostor{};     // xyz: モデルのローカル中心, w: 切替距離（0 = なし）
        XMUINT4 ImpostorInfo{};  // x: グループ, y: 出力先頭, z: 容量, w: 有効
    };

    static_assert(sizeof(GpuInstanceInput) == 144);

    Microsoft::WRL::ComPtr<ID3D12Resource> m_InstanceUpload;
    GpuInstanceInput* m_MappedInstances = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_DirectInstanceUpload;
    XMFLOAT4X4* m_MappedDirectInstances = nullptr;
    array<vector<XMFLOAT4X4>, 3> m_DirectLodScratch;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_LodInstances[3];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_LodCounts;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_IndirectArguments;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_ZeroCountsUpload;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_CullLodRootSignature;
    WorkGraph::ComputeGraph m_CullLodGraph;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_ArgsRootSignature;
    WorkGraph::ComputeGraph m_ArgsGraph;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_RasterGraphInputs;
    D3D12_NODE_GPU_INPUT* m_MappedRasterGraphInputs = nullptr;
    UINT m_FrameIndex = UINT_MAX;
    UINT m_FrameCursor = 0;
    UINT m_DirectFrameCursor = 0;
    UINT m_BatchCursor = 0;
    bool m_GpuCullingInitialized = false;
    static inline bool s_Available = false;

    bool CreateGpuCullingResources(ID3D12Device* device);
};
