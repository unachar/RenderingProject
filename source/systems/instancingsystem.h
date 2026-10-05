#pragma once

#include "systembase.h"
#include "ecs.h"
#include <d3d12.h>
#include <wrl.h>
#include <array>
#include <vector>
#include <functional>

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
    static bool IsAvailable() { return s_Available; }

    void Init() override;
    void Uninit() override;
    void Draw(RenderPass renderPass, bool receivingPostProcessOnly) override;

private:

    static uint64_t HashMaterial(const MaterialComponent* material);
    static const char* ResolvePixelShader(EntityID entity, InstanceKind kind);
    static bool IsBoundsVisible(EntityID entity, const XMMATRIX& viewProjection, const XMFLOAT3& fallbackCenter,
        const XMFLOAT3& fallbackExtents, bool hasFallbackBounds);
    static BatchKey MakeKey(
        EntityID entity,
        InstanceKind kind,
        ID3D12PipelineState* pso,
        D3D12_GPU_VIRTUAL_ADDRESS vertexBuffer,
        uint64_t geometryHash,
        UINT vertexCount,
        D3D12_GPU_VIRTUAL_ADDRESS indexBuffer,
        UINT indexCount,
        int textureIndex,
        int normalIndex,
        const MaterialComponent* material,
        UINT meshIndex = 0,
        const AnimationModelComponent* animation = nullptr);
    bool SetupDrawContext(RenderPass renderPass, DrawContext& ctx);
    bool AcceptsPass(EntityID entity, const MaterialComponent* material, const DrawContext& ctx);
    bool IsCameraVisible(EntityID entity, const XMFLOAT3& center,
        const XMFLOAT3& extents, bool hasBounds, const DrawContext& ctx);
    void BuildShadowBatches(DrawContext& ctx, BatchBuilder& builder);
    InstanceBatch CreateAnimShadowBatch(const MeshData& mesh, AnimationModelResource* model, UINT meshIndex, ID3D12PipelineState* pso);
    InstanceBatch CreateStaticShadowBatch(const StaticMeshData& mesh, StaticModelResource* model, UINT meshIndex, ID3D12PipelineState* pso);
    InstanceBatch CreateNonIndexedShadowBatch(const SpriteComponent& sprite, ID3D12PipelineState* pso, const BatchKey&);
    InstanceBatch CreateNonIndexedShadowBatch(const MeshComponent& mesh, ID3D12PipelineState* pso, const BatchKey&);
    void ExecuteShadowBatches(const DrawContext& ctx, vector<InstanceBatch>& batches);
    void BuildMainBatches(DrawContext& ctx, BatchBuilder& builder);
    void ExecuteMainBatches(const DrawContext& ctx, vector<InstanceBatch>& batches);
    void ExecuteGpuCullLod(InstanceBatch& batch, const function<void()>& bindGraphics, const DrawContext& ctx);
    bool ExecuteCpuCulledDraw(InstanceBatch& batch, UINT minimumLod, const function<void()>& bindGraphics, const DrawContext& ctx);


    static constexpr UINT kMaxInstancesPerFrame = g_kMAX_ENTITIES * 16;

    struct GpuInstanceInput
    {
        XMFLOAT4X4 World{};
        XMFLOAT4 LocalCenter{};
        XMFLOAT4 LocalExtents{};
        XMFLOAT4 LodDistances{};
    };
	static_assert(sizeof(GpuInstanceInput) == 112);

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
	Microsoft::WRL::ComPtr<ID3D12PipelineState> m_CullLodPso;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> m_ArgsRootSignature;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> m_ArgsPso;
	Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_DrawIndexedSignature;
	Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_DrawSignature;
    UINT m_FrameIndex = UINT_MAX;
    UINT m_FrameCursor = 0;
	UINT m_DirectFrameCursor = 0;
	bool m_GpuCullingInitialized = false;
    static inline bool s_Available = false;

	bool CreateGpuCullingResources(ID3D12Device* device);
};
