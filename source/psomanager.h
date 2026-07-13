#pragma once
#include "rendererresource.h"

class PsoManager : protected RendererState
{
public:
	static bool CreateGraphicsPipelineState(
		const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
		const char* debugName,
		ComPtr<ID3D12PipelineState>& outPso);
	static bool CreateComputePipelineState(
		const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc,
		const char* debugName,
		ComPtr<ID3D12PipelineState>& outPso);

	static ID3D12PipelineState* GetOrCreateGraphicsPso(const rendererResource& resource);
	static ID3D12PipelineState* GetOrCreateToonOutlinePso(bool enableAlphaBlend = false);
	static ID3D12PipelineState* GetOrCreateShadowMapPso();
	static ID3D12PipelineState* GetOrCreateShadowMapInstancedPso();

	static bool CreateSkinningPso();
	static bool CreatePostProcessPipelines();
	static bool CreateAtmospherePso();
	static bool CreateUpscalePso();
	static bool CreateUpscaleDepthPso();
	static bool CreateVelocityPso();
	static bool CreateVelocityGeometryPso();
	static bool CreateAaPsos();

	static ID3D12PipelineState* GetPostProcessPso(PostProcessType type);
	static ID3D12PipelineState* GetDeferredLightingPso() { return m_DeferredLightingPso.Get(); }
	static ID3D12PipelineState* GetAtmospherePso() { return m_AtmospherePso.Get(); }
	static ID3D12PipelineState* GetUpscaleBilateralPso() { return m_UpscaleBilateralPso.Get(); }
	static ID3D12PipelineState* GetUpscaleDepthPso() { return m_UpscaleDepthPso.Get(); }
	static ID3D12PipelineState* GetVelocityPso() { return m_VelocityPso.Get(); }
	static ID3D12PipelineState* GetVelocityGeometryPso() { return m_VelocityGeometryPso.Get(); }
	static ID3D12PipelineState* GetFxaaPso() { return m_FxaaPso.Get(); }
	static ID3D12PipelineState* GetTaaBlendPso() { return m_TaaBlendPso.Get(); }
	static ID3D12PipelineState* GetSkinningPso() { return m_SkinningPso.Get(); }
};
