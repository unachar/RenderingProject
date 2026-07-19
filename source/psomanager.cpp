#include "pch.h"
#include "psomanager.h"
#include "renderershader.h"
#include "renderercore.h"
#include "rendererutils.h"
#include "texturemanager.h"
#include "renderersettings.h"
#include "visibilitybuffer.h"

bool PsoManager::CreateGraphicsPipelineState(
	const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
	const char* debugName,
	ComPtr<ID3D12PipelineState>& outPso)
{
	outPso.Reset();
	HRESULT hr = m_Device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&outPso));
	if (FAILED(hr))
	{
		const HRESULT removedReason = m_Device ? m_Device->GetDeviceRemovedReason() : E_POINTER;
		if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG || FAILED(removedReason))
		{
			RendererCore::CheckDeviceHealth(hr, "Create graphics pipeline state");
			return false;
		}
		Debug::Log("ERROR: Failed to create %s graphics PSO. HRESULT: 0x%08X\n", debugName ? debugName : "unnamed", hr);
		FILE* log = nullptr;
		fopen_s(&log, "init_log.txt", "a");
		if (log)
		{
			fprintf(log, "PSO ERROR: %s HRESULT=0x%08X\n", debugName ? debugName : "unnamed", hr);
			ComPtr<ID3D12InfoQueue> infoQueue;
			if (SUCCEEDED(m_Device.As(&infoQueue)))
			{
				const UINT64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
				const UINT64 firstMessage = messageCount > 8 ? messageCount - 8 : 0;
				for (UINT64 index = firstMessage; index < messageCount; ++index)
				{
					SIZE_T messageSize = 0;
					infoQueue->GetMessage(index, nullptr, &messageSize);
					vector<uint8_t> storage(messageSize);
					auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
					if (SUCCEEDED(infoQueue->GetMessage(index, message, &messageSize)))
						fprintf(log, "D3D12: %s\n", message->pDescription);
				}
			}
			fclose(log);
		}

		return false;
	}
	return true;
}

bool PsoManager::CreateComputePipelineState(
	const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc,
	const char* debugName,
	ComPtr<ID3D12PipelineState>& outPso)
{
	outPso.Reset();
	HRESULT hr = m_Device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&outPso));
	if (FAILED(hr))
	{
		const HRESULT removedReason = m_Device ? m_Device->GetDeviceRemovedReason() : E_POINTER;
		if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG || FAILED(removedReason))
		{
			RendererCore::CheckDeviceHealth(hr, "Create compute pipeline state");
			return false;
		}
		Debug::Log("ERROR: Failed to create %s compute PSO. HRESULT: 0x%08X\n", debugName ? debugName : "unnamed", hr);
		return false;
	}
	return true;
}

ID3D12PipelineState* PsoManager::GetOrCreateGraphicsPso(const rendererResource& resource)
{
	const bool requestDeferredScene = (m_RenderMode == RenderMode::DEFERRED && m_IsDeferredGeometryPass);
	const bool visibilityPass = requestDeferredScene &&
		RendererSettings::GetComputeGBufferEnabled() &&
		VisibilityBuffer::IsAvailable();
	const string resolvedPsPath = visibilityPass
		? (resource.isModel
			? "shader/hlsl/build/VisibilityPS.cso"
			: "shader/hlsl/build/Visibility2DPS.cso")
		: (requestDeferredScene
			? ResolvePixelShaderPathForRenderMode(resource.psPath, RenderMode::DEFERRED)
			: ResolvePixelShaderPathForRenderMode(resource.psPath, RenderMode::FORWARD));
	const bool useDeferredMrt = requestDeferredScene || RendererPathEndsWith(resolvedPsPath, "_MRT.cso") || RendererPathEndsWith(resolvedPsPath, "GeometryPS.cso");
	const bool enableAlphaBlend = resource.enableAlphaBlend && !useDeferredMrt;
	const DXGI_FORMAT forwardRtvFormat = useDeferredMrt ? DXGI_FORMAT_UNKNOWN : GetForwardRtvFormat();
	const UINT forwardRtvFormatVal = static_cast<UINT>(forwardRtvFormat);
	string key = resource.vsPath + resolvedPsPath + "|" + (resource.isModel ? "M" : "2D") + "|" + (visibilityPass ? "VISIBILITY" : (useDeferredMrt ? "DEFERRED" : "FORWARD")) + "|" + to_string(forwardRtvFormatVal) + "|" + (resource.frontCounterClockwise ? "FRONT_CCW" : "FRONT_CW") + "|" + (enableAlphaBlend ? "ALPHA" : "OPAQUE");

	auto it = m_PsoCache.find(key);
	if (it != m_PsoCache.end())
	{
		return it->second.Get();
	}

	Debug::Log("Creating PSO: VS=%s PS=%s Mode=%s RTV=%u\n",
		resource.vsPath,
		resolvedPsPath.c_str(),
		useDeferredMrt ? "DEFERRED" : "FORWARD",
		visibilityPass ? 1 : (useDeferredMrt ? g_kGEOMETRY_GBUFFER_COUNT : 1));

	ComPtr<ID3DBlob> vsBlob;
	ComPtr<ID3DBlob> psBlob;

	rendererResource vsResource = resource;
	vsResource.csoPath = resource.vsPath;
	vsResource.ppBlob = vsBlob.GetAddressOf();
	if (!RendererShader::LoadShaderBlob(vsResource))
	{
		return nullptr;
	}

	rendererResource psResource = resource;
	psResource.csoPath = resolvedPsPath.c_str();
	psResource.ppBlob = psBlob.GetAddressOf();
	if (!RendererShader::LoadShaderBlob(psResource))
	{
		return nullptr;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(psBlob.Get());
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.FrontCounterClockwise = resource.frontCounterClockwise;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = visibilityPass ? 1 : (useDeferredMrt ? g_kGEOMETRY_GBUFFER_COUNT : 1);
	if (visibilityPass)
	{
		psoDesc.RTVFormats[0] = GetGBufferFormat(GBufferType::VISIBILITY);
	}
	else if (useDeferredMrt)
	{
		for (UINT i = 0; i < g_kGEOMETRY_GBUFFER_COUNT; ++i)
		{
			psoDesc.RTVFormats[i] = m_kDeferredRtvFormats[i];
		}
	}
	else
	{
		psoDesc.RTVFormats[0] = GetForwardRtvFormat();
	}
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleDesc.Count = 1;

	if (resource.isModel)
	{
		static D3D12_INPUT_ELEMENT_DESC modelLayout[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
		};
		psoDesc.InputLayout = { modelLayout, _countof(modelLayout) };
		psoDesc.pRootSignature = m_ModelRootSignature.Get();
		psoDesc.DepthStencilState.DepthEnable = TRUE;
		psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		psoDesc.DepthStencilState.StencilEnable = FALSE;
	}
	else
	{
		static D3D12_INPUT_ELEMENT_DESC layout2D[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
		};
		psoDesc.InputLayout = { layout2D, _countof(layout2D) };
		psoDesc.pRootSignature = m_RootSignature.Get();
		psoDesc.DepthStencilState.DepthEnable = FALSE;
		psoDesc.DepthStencilState.StencilEnable = FALSE;
	}

	if (enableAlphaBlend)
	{
		auto& rtBlend = psoDesc.BlendState.RenderTarget[0];
		rtBlend.BlendEnable = TRUE;
		rtBlend.SrcBlend = D3D12_BLEND_ONE;
		rtBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
		rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
		rtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
		rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	}

	ComPtr<ID3D12PipelineState> newPso{};
	if (!CreateGraphicsPipelineState(psoDesc, "renderer", newPso))
	{
		return nullptr;
	}

	m_PsoCache[key] = newPso;
	return newPso.Get();
}

ID3D12PipelineState* PsoManager::GetOrCreateToonOutlinePso(bool enableAlphaBlend)
{
	const bool useDeferredMrt = (m_RenderMode == RenderMode::DEFERRED && m_IsDeferredGeometryPass);
	const bool visibilityPass = useDeferredMrt &&
		RendererSettings::GetComputeGBufferEnabled() &&
		VisibilityBuffer::IsAvailable();
	const UINT forwardRtvFmt = useDeferredMrt ? 0 : static_cast<UINT>(GetForwardRtvFormat());
	const bool useAlphaBlend = enableAlphaBlend && !useDeferredMrt;
	const string key = string("TOON_OUTLINE|") + (visibilityPass ? "VISIBILITY" : (useDeferredMrt ? "DEFERRED" : "FORWARD")) + "|" + to_string(forwardRtvFmt) + "|" + (useAlphaBlend ? "ALPHA" : "OPAQUE");
	auto it = m_PsoCache.find(key);
	if (it != m_PsoCache.end())
	{
		return it->second.Get();
	}

	ComPtr<ID3DBlob> vsBlob;
	ComPtr<ID3DBlob> psBlob;

	rendererResource vsResource{};
	vsResource.csoPath = "shader\\hlsl\\build\\toonOutlineVS.cso";
	vsResource.ppBlob = vsBlob.GetAddressOf();
	if (!RendererShader::LoadShaderBlob(vsResource))
	{
		return nullptr;
	}

	rendererResource psResource{};
	psResource.csoPath = visibilityPass
		? "shader\\hlsl\\build\\VisibilityOutlinePS.cso"
		: useDeferredMrt
		? "shader\\hlsl\\build\\toonOutlinePS_MRT.cso"
		: "shader\\hlsl\\build\\toonOutlinePS.cso";
	psResource.ppBlob = psBlob.GetAddressOf();
	if (!RendererShader::LoadShaderBlob(psResource))
	{
		return nullptr;
	}

	static D3D12_INPUT_ELEMENT_DESC modelLayout[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(psBlob.Get());
	psoDesc.InputLayout = { modelLayout, _countof(modelLayout) };
	psoDesc.pRootSignature = m_ModelRootSignature.Get();
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	if (useAlphaBlend)
	{
		auto& rtBlend = psoDesc.BlendState.RenderTarget[0];
		rtBlend.BlendEnable = TRUE;
		rtBlend.SrcBlend = D3D12_BLEND_ONE;
		rtBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
		rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
		rtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
		rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	}
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = visibilityPass ? 1 : (useDeferredMrt ? g_kGEOMETRY_GBUFFER_COUNT : 1);
	if (visibilityPass)
	{
		psoDesc.RTVFormats[0] = GetGBufferFormat(GBufferType::VISIBILITY);
	}
	else if (useDeferredMrt)
	{
		for (UINT i = 0; i < g_kGEOMETRY_GBUFFER_COUNT; ++i)
		{
			psoDesc.RTVFormats[i] = m_kDeferredRtvFormats[i];
		}
	}
	else
	{
		psoDesc.RTVFormats[0] = GetForwardRtvFormat();
	}
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleDesc.Count = 1;

	ComPtr<ID3D12PipelineState> pso;
	if (!CreateGraphicsPipelineState(psoDesc, "toon outline", pso))
	{
		return nullptr;
	}

	m_PsoCache[key] = pso;
	return pso.Get();
}

ID3D12PipelineState* PsoManager::GetOrCreateShadowMapPso()
{
	if (m_ShadowMapPso)
	{
		return m_ShadowMapPso.Get();
	}

	rendererResource resource{};
	resource.vsPath = "shader/hlsl/build/ShadowMapVS.cso";
	resource.csoPath = resource.vsPath;
	ComPtr<ID3DBlob> vsBlob;
	resource.ppBlob = vsBlob.GetAddressOf();
	if (!RendererShader::LoadShaderBlob(resource))
	{
		return nullptr;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
	psoDesc.PS = { nullptr, 0 };
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;




	psoDesc.RasterizerState.DepthBias = 0;
	psoDesc.RasterizerState.SlopeScaledDepthBias = 0.0f;
	psoDesc.RasterizerState.DepthBiasClamp = 0.0f;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = TRUE;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 0;
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleDesc.Count = 1;
	psoDesc.pRootSignature = m_ModelRootSignature.Get();

	static D3D12_INPUT_ELEMENT_DESC modelLayout[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
	psoDesc.InputLayout = { modelLayout, _countof(modelLayout) };

	if (!CreateGraphicsPipelineState(psoDesc, "shadow map", m_ShadowMapPso))
	{
		return nullptr;
	}

	return m_ShadowMapPso.Get();
}

ID3D12PipelineState* PsoManager::GetOrCreateShadowMapInstancedPso()
{
	constexpr const char* key = "shadow_map_instanced";
	auto cached = m_PsoCache.find(key);
	if (cached != m_PsoCache.end())
	{
		return cached->second.Get();
	}

	rendererResource resource{};
	resource.vsPath = "shader/hlsl/build/ShadowMapInstancedVS.cso";
	resource.csoPath = resource.vsPath;
	ComPtr<ID3DBlob> vsBlob;
	resource.ppBlob = vsBlob.GetAddressOf();
	if (!RendererShader::LoadShaderBlob(resource))
	{
		return nullptr;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	psoDesc.RasterizerState.DepthBias = 0;
	psoDesc.RasterizerState.SlopeScaledDepthBias = 0.0f;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = TRUE;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 0;
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleDesc.Count = 1;
	psoDesc.pRootSignature = m_ModelRootSignature.Get();

	static D3D12_INPUT_ELEMENT_DESC modelLayout[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
	psoDesc.InputLayout = { modelLayout, _countof(modelLayout) };

	ComPtr<ID3D12PipelineState> pso;
	if (!CreateGraphicsPipelineState(psoDesc, key, pso))
	{
		return nullptr;
	}
	m_PsoCache.emplace(key, pso);
	return pso.Get();
}

bool PsoManager::CreateSkinningPso()
{
	rendererResource resource{};
	resource.csoPath = "shader\\hlsl\\build\\skinning_cs.cso";
	resource.ppBlob = resource.csBlob.GetAddressOf();
	if (!RendererShader::LoadShaderBlob(resource))
	{
		Debug::Log("ERROR: Failed to load skinning_cs.cso. Skipping GPU skinning.\n");
		return true;
	}

	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
	psoDesc.pRootSignature = m_SkinningRootSignature.Get();
	psoDesc.CS = CD3DX12_SHADER_BYTECODE(resource.ppBlob ? *resource.ppBlob : nullptr);

	if (!CreateComputePipelineState(psoDesc, "skinning", m_SkinningPso))
	{
		return false;
	}

	return true;
}

bool PsoManager::CreatePostProcessPipelines()
{
	auto CreatePPPSO = [&](const char* psPath, PostProcessType type)
		{
			rendererResource resource{};
			resource.vsPath = "shader\\hlsl\\build\\postProcessVS.cso";
			resource.psPath = psPath;
			ComPtr<ID3DBlob> vsBlob;
			ComPtr<ID3DBlob> psBlob;
			rendererResource vsResource = resource;
			vsResource.csoPath = resource.vsPath;
			vsResource.ppBlob = vsBlob.GetAddressOf();
			if (!RendererShader::LoadShaderBlob(vsResource)) return false;
			rendererResource psResource = resource;
			psResource.csoPath = resource.psPath;
			psResource.ppBlob = psBlob.GetAddressOf();
			if (!RendererShader::LoadShaderBlob(psResource)) return false;

			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
			psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
			psoDesc.PS = CD3DX12_SHADER_BYTECODE(psBlob.Get());
			psoDesc.pRootSignature = m_PostProcessRootSignature.Get();
			psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
			psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
			psoDesc.DepthStencilState.DepthEnable = FALSE;
			psoDesc.SampleMask = UINT_MAX;
			psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			psoDesc.NumRenderTargets = 1;
			psoDesc.RTVFormats[0] = m_SceneColorFormat;
			psoDesc.SampleDesc.Count = 1;

			ComPtr<ID3D12PipelineState> pso;
			if (CreateGraphicsPipelineState(psoDesc, "post process", pso))
			{
				m_PostProcessPsoMap[type] = pso;
				return true;
			}
			return false;
		};

	CreatePPPSO("shader\\hlsl\\build\\postProcessNonePS.cso", PostProcessType::NONE);
	CreatePPPSO("shader\\hlsl\\build\\postProcessBlurPS.cso", PostProcessType::BLUR);
	CreatePPPSO("shader\\hlsl\\build\\postProcessSepiaPS.cso", PostProcessType::SEPIA);
	CreatePPPSO("shader\\hlsl\\build\\postProcessGrayPS.cso", PostProcessType::GRAYSCALE);
	CreatePPPSO("shader\\hlsl\\build\\postProcessInvertPS.cso", PostProcessType::INVERT);

	rendererResource resource{};
	resource.vsPath = "shader\\hlsl\\build\\postProcessVS.cso";
	resource.psPath = "shader\\hlsl\\build\\DeferredLightingPS.cso";

	rendererResource vsResource = resource;
	vsResource.csoPath = resource.vsPath;
	vsResource.ppBlob = resource.vsBlob.GetAddressOf();
	if (!RendererShader::LoadShaderBlob(vsResource)) return false;
	rendererResource psResource = resource;
	psResource.csoPath = resource.psPath;
	psResource.ppBlob = resource.psBlob.GetAddressOf();
	if (!RendererShader::LoadShaderBlob(psResource)) return false;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(resource.vsBlob.Get());
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(resource.psBlob.Get());
	psoDesc.pRootSignature = m_PostProcessRootSignature.Get();
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = m_SceneColorFormat;
	psoDesc.SampleDesc.Count = 1;

	return CreateGraphicsPipelineState(psoDesc, "deferred lighting", m_DeferredLightingPso);
}

bool PsoManager::CreateAtmospherePso()
{
	rendererResource resource{};
	resource.vsPath = "shader\\hlsl\\build\\postProcessVS.cso";
	resource.psPath = "shader\\hlsl\\build\\AtmosphereGBufferPS.cso";

	rendererResource vsResource = resource;
	vsResource.csoPath = resource.vsPath;
	vsResource.ppBlob = resource.vsBlob.GetAddressOf();
	if (!RendererShader::LoadShaderBlob(vsResource)) return false;
	rendererResource psResource = resource;
	psResource.csoPath = resource.psPath;
	psResource.ppBlob = resource.psBlob.GetAddressOf();
	if (!RendererShader::LoadShaderBlob(psResource)) return false;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(resource.vsBlob.Get());
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(resource.psBlob.Get());
	psoDesc.pRootSignature = m_PostProcessRootSignature.Get();
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	psoDesc.SampleDesc.Count = 1;

	return CreateGraphicsPipelineState(psoDesc, "atmosphere gbuffer", m_AtmospherePso);
}

bool PsoManager::CreateUpscalePso()
{
	const char* psPath = "shader\\hlsl\\build\\UpscaleBilateralPS.cso";

	rendererResource resource{};
	resource.vsPath = "shader\\hlsl\\build\\postProcessVS.cso";
	resource.psPath = psPath;

	rendererResource vsResource = resource;
	vsResource.csoPath = resource.vsPath;
	vsResource.ppBlob = resource.vsBlob.GetAddressOf();
	if (!RendererShader::LoadShaderBlob(vsResource)) return false;

	rendererResource psResource = resource;
	psResource.csoPath = resource.psPath;
	psResource.ppBlob = resource.psBlob.GetAddressOf();
	if (!RendererShader::LoadShaderBlob(psResource)) return false;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(resource.vsBlob.Get());
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(resource.psBlob.Get());
	psoDesc.pRootSignature = m_UpscaleRootSignature.Get();
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = m_SceneColorFormat;
	psoDesc.SampleDesc.Count = 1;

	return CreateGraphicsPipelineState(psoDesc, "UpscaleBilateralPso", m_UpscaleBilateralPso);
}

bool PsoManager::CreateUpscaleDepthPso()
{
	rendererResource resource{};
	resource.vsPath = "shader\\hlsl\\build\\postProcessVS.cso";
	resource.psPath = "shader\\hlsl\\build\\UpscaleDepthPS.cso";

	rendererResource vsResource = resource;
	vsResource.csoPath = resource.vsPath;
	vsResource.ppBlob = resource.vsBlob.GetAddressOf();
	if (!RendererShader::LoadShaderBlob(vsResource)) return false;

	rendererResource psResource = resource;
	psResource.csoPath = resource.psPath;
	psResource.ppBlob = resource.psBlob.GetAddressOf();
	if (!RendererShader::LoadShaderBlob(psResource)) return false;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(resource.vsBlob.Get());
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(resource.psBlob.Get());
	psoDesc.pRootSignature = m_UpscaleRootSignature.Get();
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 0;
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleDesc.Count = 1;

	return CreateGraphicsPipelineState(psoDesc, "UpscaleDepthPso", m_UpscaleDepthPso);
}

bool PsoManager::CreateVelocityPso()
{
	rendererResource resource{};
	resource.vsPath = "shader\\hlsl\\build\\postProcessVS.cso";
	resource.psPath = "shader\\hlsl\\build\\VelocityPS.cso";

	rendererResource vsResource = resource;
	vsResource.csoPath = resource.vsPath;
	vsResource.ppBlob = resource.vsBlob.GetAddressOf();
	if (!RendererShader::LoadShaderBlob(vsResource)) return false;

	rendererResource psResource = resource;
	psResource.csoPath = resource.psPath;
	psResource.ppBlob = resource.psBlob.GetAddressOf();
	if (!RendererShader::LoadShaderBlob(psResource)) return false;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(resource.vsBlob.Get());
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(resource.psBlob.Get());
	psoDesc.pRootSignature = m_AaRootSignature.Get();
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = GetGBufferFormat(GBufferType::VELOCITY);
	psoDesc.SampleDesc.Count = 1;

	return CreateGraphicsPipelineState(psoDesc, "VelocityPso", m_VelocityPso);
}

bool PsoManager::CreateVelocityGeometryPso()
{
	rendererResource resource{};
	resource.vsPath = "shader\\hlsl\\build\\VelocityGeometryVS.cso";
	resource.psPath = "shader\\hlsl\\build\\VelocityGeometryPS.cso";
	rendererResource vsResource = resource;
	vsResource.csoPath = resource.vsPath;
	vsResource.ppBlob = resource.vsBlob.GetAddressOf();
	if (!RendererShader::LoadShaderBlob(vsResource)) return false;
	rendererResource psResource = resource;
	psResource.csoPath = resource.psPath;
	psResource.ppBlob = resource.psBlob.GetAddressOf();
	if (!RendererShader::LoadShaderBlob(psResource)) return false;

	D3D12_INPUT_ELEMENT_DESC layout[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "PREVPOS", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.VS = CD3DX12_SHADER_BYTECODE(resource.vsBlob.Get());
	desc.PS = CD3DX12_SHADER_BYTECODE(resource.psBlob.Get());
	desc.InputLayout = { layout, _countof(layout) };
	desc.pRootSignature = m_ModelRootSignature.Get();
	desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	desc.DepthStencilState.DepthEnable = FALSE;
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = GetGBufferFormat(GBufferType::VELOCITY);
	desc.SampleDesc.Count = 1;
	return CreateGraphicsPipelineState(desc, "VelocityGeometryPso", m_VelocityGeometryPso);
}

bool PsoManager::CreateAaPsos()
{
	auto CreateAAPSO = [&](const char* psPath, ComPtr<ID3D12PipelineState>& outPso, const char* debugName)
		{
			rendererResource resource{};
			resource.vsPath = "shader\\hlsl\\build\\postProcessVS.cso";
			resource.psPath = psPath;

			rendererResource vsResource = resource;
			vsResource.csoPath = resource.vsPath;
			vsResource.ppBlob = resource.vsBlob.GetAddressOf();
			if (!RendererShader::LoadShaderBlob(vsResource)) return false;
			rendererResource psResource = resource;
			psResource.csoPath = resource.psPath;
			psResource.ppBlob = resource.psBlob.GetAddressOf();
			if (!RendererShader::LoadShaderBlob(psResource)) return false;

			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
			psoDesc.VS = CD3DX12_SHADER_BYTECODE(resource.vsBlob.Get());
			psoDesc.PS = CD3DX12_SHADER_BYTECODE(resource.psBlob.Get());
			psoDesc.pRootSignature = m_AaRootSignature.Get();
			psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
			psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
			psoDesc.DepthStencilState.DepthEnable = FALSE;
			psoDesc.SampleMask = UINT_MAX;
			psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			psoDesc.NumRenderTargets = 1;
			psoDesc.RTVFormats[0] = m_SceneColorFormat;
			psoDesc.SampleDesc.Count = 1;

			return CreateGraphicsPipelineState(psoDesc, debugName, outPso);
		};

	if (!CreateAAPSO("shader\\hlsl\\build\\FXAA_PS.cso", m_FxaaPso, "FxaaPso")) return false;
	if (!CreateAAPSO("shader\\hlsl\\build\\TAA_BlendPS.cso", m_TaaBlendPso, "TaaBlendPso")) return false;

	return true;
}

ID3D12PipelineState* PsoManager::GetPostProcessPso(PostProcessType type)
{
	auto psoIt = m_PostProcessPsoMap.find(type);
	if (psoIt == m_PostProcessPsoMap.end() || !psoIt->second)
	{
		psoIt = m_PostProcessPsoMap.find(PostProcessType::NONE);
	}
	if (psoIt == m_PostProcessPsoMap.end() || !psoIt->second)
	{
		return nullptr;
	}
	return psoIt->second.Get();
}
