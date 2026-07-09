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
	static ID3D12PipelineState* GetOrCreateToonOutlinePso();
	static ID3D12PipelineState* GetOrCreateShadowMapPso();

	static bool CreateSkinningPso();
	static bool CreatePostProcessPipelines();
	static bool CreateUpscalePso();
	static bool CreateAaPsos();

	static ID3D12PipelineState* GetPostProcessPso(PostProcessType type);
	static ID3D12PipelineState* GetDeferredLightingPso() { return m_DeferredLightingPso.Get(); }
	static ID3D12PipelineState* GetUpscaleBilateralPso() { return m_UpscaleBilateralPso.Get(); }
	static ID3D12PipelineState* GetFxaaPso() { return m_FxaaPso.Get(); }
	static ID3D12PipelineState* GetTaaBlendPso() { return m_TaaBlendPso.Get(); }
	static ID3D12PipelineState* GetSkinningPso() { return m_SkinningPso.Get(); }
};
