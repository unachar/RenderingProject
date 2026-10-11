#pragma once
#include "rendererstate.h"
#include "shaderdescription.h"

class ShaderPipelines : protected RendererState
{
public:
	static bool LoadShaderBlob(const ShaderDescription& resource);
	static bool CreateVertexShader(const ShaderDescription& resource);
	static bool CreatePixelShader(const ShaderDescription& resource);
	static bool CreateModelPipeline();
	static bool CreateSkinningPipeline();
	static bool CreatePostProcessPipeline();

	static ID3D12RootSignature* GetRootSignature() { return m_RootSignature.Get(); }
	static ID3D12PipelineState* GetPipelineState() { return m_PipelineState.Get(); }
	static ID3D12RootSignature* GetModelRootSignature() { return m_ModelRootSignature.Get(); }
	static ID3D12RootSignature* GetSkinningRootSignature() { return m_SkinningRootSignature.Get(); }
};

