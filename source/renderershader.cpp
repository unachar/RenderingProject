#include "pch.h"
#include "renderershader.h"
#include "psomanager.h"
#include "rendererdraw.h"
#include "rendererutils.h"
#include "world.h"
#include "ecs.h"
#include "texturemanager.h"
#include "componentmanager.h"
#include "imguimanager.h"

namespace
{
	unordered_map<string, ComPtr<ID3DBlob>> g_ShaderBlobCache;
}


bool RendererShader::CreateModelPipeline()
{
	m_ModelRootSignature = m_RootSignature;

	m_ModelPipelineState = nullptr;

	return true;
}

bool RendererShader::CreateSkinningPipeline()
{
	CD3DX12_DESCRIPTOR_RANGE ranges[4];
	ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
	ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
	ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
	ranges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 1);

	CD3DX12_ROOT_PARAMETER rootParams[4];
	rootParams[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_ALL);
	rootParams[1].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_ALL);
	rootParams[2].InitAsDescriptorTable(1, &ranges[2], D3D12_SHADER_VISIBILITY_ALL);
	rootParams[3].InitAsDescriptorTable(1, &ranges[3], D3D12_SHADER_VISIBILITY_ALL);

	CD3DX12_ROOT_SIGNATURE_DESC rsDesc;
	rsDesc.Init(_countof(rootParams), rootParams, 0, nullptr,
		D3D12_ROOT_SIGNATURE_FLAG_NONE);

	ComPtr<ID3DBlob> signature;
	ComPtr<ID3DBlob> error;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
	if (FAILED(hr))
	{
		Debug::Log("ERROR: D3D12SerializeRootSignature(Skinning) failed\n");
		if (error) Debug::Log("%s\n", (char*)error->GetBufferPointer());
		return false;
	}
	hr = m_Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
		IID_PPV_ARGS(&m_SkinningRootSignature));
	if (FAILED(hr))
	{
		Debug::Log("ERROR: CreateRootSignature(Skinning) failed\n");
		return false;
	}
	return PsoManager::CreateSkinningPso();
}

bool RendererShader::LoadShaderBlob(const rendererResource& resource)
{
	const char* csoPath = resource.csoPath;
	ID3DBlob** ppBlob = resource.ppBlob;

	if (!csoPath || !ppBlob)
	{
		return false;
	}
	*ppBlob = nullptr;

	auto cached = g_ShaderBlobCache.find(csoPath);
	if (cached != g_ShaderBlobCache.end())
	{
		*ppBlob = cached->second.Get();
		(*ppBlob)->AddRef();
		return true;
	}

	int len = MultiByteToWideChar(CP_UTF8, 0, csoPath, -1, NULL, 0);
	wstring wPath(len, 0);
	MultiByteToWideChar(CP_UTF8, 0, csoPath, -1, &wPath[0], len);

	HRESULT hr = D3DReadFileToBlob(wPath.c_str(), ppBlob);
	if (FAILED(hr))
	{
		char buf[256];
		sprintf_s(buf, "ERROR: Failed to load shader: %s (HRESULT: 0x%08X)\n", csoPath, hr);
		Debug::Log("%s", buf);
		return false;
	}

	g_ShaderBlobCache.emplace(csoPath, *ppBlob);
	return true;
}

bool RendererShader::CreateVertexShader(const rendererResource& resource)
{
	rendererResource shaderResource = resource;
	shaderResource.csoPath = resource.vsPath;
	return LoadShaderBlob(shaderResource);
}

bool RendererShader::CreatePixelShader(const rendererResource& resource)
{
	rendererResource shaderResource = resource;
	shaderResource.csoPath = resource.psPath;
	return LoadShaderBlob(shaderResource);
}

static void ShaderLogToFile(const char* msg)
{
	FILE* fp = nullptr;
	fopen_s(&fp, "init_log.txt", "a");
	if (fp) { fprintf(fp, "%s", msg); fclose(fp); }
}

bool RendererShader::CreatePostProcessPipeline()
{
	ShaderLogToFile("PP: setup ranges\n");
	CD3DX12_DESCRIPTOR_RANGE ranges[3];
	ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 0);
	ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6);
	ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 7);

	CD3DX12_ROOT_PARAMETER params[7];
	params[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_PIXEL);
	params[1].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	params[2].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	params[3].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_PIXEL);
	params[4].InitAsDescriptorTable(1, &ranges[2], D3D12_SHADER_VISIBILITY_PIXEL);
	params[5].InitAsConstantBufferView(3, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	params[6].InitAsConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_PIXEL);

	CD3DX12_STATIC_SAMPLER_DESC samplers[2] {};
	samplers[0] = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
	samplers[1] = CD3DX12_STATIC_SAMPLER_DESC(
		1,
		D3D12_FILTER_MIN_MAG_MIP_LINEAR,
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,
		0.0f,
		1,
		D3D12_COMPARISON_FUNC_NEVER,
		D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE);
	CD3DX12_ROOT_SIGNATURE_DESC rsDesc(_countof(params), params, _countof(samplers), samplers, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	ComPtr<ID3DBlob> rsBlob, errBlob;
	ShaderLogToFile("PP: serialize root sig\n");
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob);
	if (FAILED(hr))
	{
		ShaderLogToFile("PP: FAIL SerializeRootSig\n");
		if (errBlob) ShaderLogToFile((const char*)errBlob->GetBufferPointer());
		return false;
	}
	ShaderLogToFile("PP: create root sig\n");
	hr = m_Device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&m_PostProcessRootSignature));
	if (FAILED(hr)) { ShaderLogToFile("PP: FAIL CreateRootSig\n"); return false; }

	auto cbHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(g_kPP_CB_ALIGNED_SIZE * g_kFRAME_COUNT);
	ShaderLogToFile("PP: create PP CB\n");
	hr = m_Device->CreateCommittedResource(&cbHeapProps, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_PostProcessConstantBuffer));
	if (FAILED(hr)) { ShaderLogToFile("PP: FAIL PP CB\n"); return false; }
	m_PostProcessConstantBuffer->Map(0, nullptr, &m_pPostProcessCbvDataBegin);

	auto lightCbDesc = CD3DX12_RESOURCE_DESC::Buffer(g_kLIGHT_CB_ALIGNED_SIZE * g_kFRAME_COUNT);
	ShaderLogToFile("PP: create Light CB\n");
	hr = m_Device->CreateCommittedResource(&cbHeapProps, D3D12_HEAP_FLAG_NONE, &lightCbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_LightConstantBuffer));
	if (FAILED(hr)) { ShaderLogToFile("PP: FAIL Light CB\n"); return false; }
	m_LightConstantBuffer->Map(0, nullptr, &m_pLightCbvDataBegin);

	auto pbrCbDesc = CD3DX12_RESOURCE_DESC::Buffer(g_kPBR_CB_ALIGNED_SIZE * g_kPBR_CB_TOTAL_SLOT_COUNT);
	ShaderLogToFile("PP: create PBR CB\n");
	hr = m_Device->CreateCommittedResource(&cbHeapProps, D3D12_HEAP_FLAG_NONE, &pbrCbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_PBRConstantBuffer));
	if (FAILED(hr)) { ShaderLogToFile("PP: FAIL PBR CB\n"); return false; }
	m_PBRConstantBuffer->Map(0, nullptr, &m_pPBRCbvDataBegin);

	ShaderLogToFile("PP: create PSOs\n");
	if (!PsoManager::CreatePostProcessPipelines()) { ShaderLogToFile("PP: FAIL CreatePostProcessPipelines\n"); return false; }
	ShaderLogToFile("PP: done\n");
	return true;
}

