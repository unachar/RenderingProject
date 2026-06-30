#pragma once
#include "renderercore.h"
#include "rendererresource.h"

class RendererShader : protected RendererState
{
public:
	static bool LoadShaderBlob(const rendererResource& resource);
	static bool CreateVertexShader(const rendererResource& resource);
	static bool CreatePixelShader(const rendererResource& resource);
	static bool CreateModelPipeline();
	static bool CreateSkinningPipeline();
	static bool CreatePostProcessPipeline();

	static ID3D12RootSignature* GetRootSignature() { return m_RootSignature.Get(); }
	static ID3D12PipelineState* GetPipelineState() { return m_PipelineState.Get(); }
	static ID3D12RootSignature* GetModelRootSignature() { return m_ModelRootSignature.Get(); }
	static ID3D12RootSignature* GetSkinningRootSignature() { return m_SkinningRootSignature.Get(); }
};

