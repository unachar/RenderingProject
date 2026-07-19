#pragma once

#include "systembase.h"
#include "ecs.h"
#include <d3d12.h>
#include <wrl.h>
#include <array>
#include <vector>

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
	std::array<std::vector<XMFLOAT4X4>, 3> m_DirectLodScratch;
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
