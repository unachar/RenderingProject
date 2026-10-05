#include "pch.h"
#include "rendertargets.h"
#include "texturemanager.h"
#include "spatialupscaler.h"
#include "screenspaceeffects.h"


void RenderTargets::GetGBufferClearColor(UINT index, float outColor[4])
{
	outColor[0] = 0.0f;
	outColor[1] = 0.0f;
	outColor[2] = 0.0f;
	outColor[3] = 0.0f;

	if (index == static_cast<UINT>(GBufferType::BASE_COLOR))
	{
		outColor[0] = RendererState::m_kSceneClearColor[0];
		outColor[1] = RendererState::m_kSceneClearColor[1];
		outColor[2] = RendererState::m_kSceneClearColor[2];
		outColor[3] = RendererState::m_kSceneClearColor[3];
	}
	else if (index == static_cast<UINT>(GBufferType::NORMAL))
	{
		outColor[3] = 1.0f;
	}
	else if (index == static_cast<UINT>(GBufferType::DEPTH))
	{
		outColor[0] = 1.0f;
		outColor[1] = 1.0f;
		outColor[2] = 1.0f;
		outColor[3] = 1.0f;
	}
	else if (index == static_cast<UINT>(GBufferType::MATERIAL))
	{
		outColor[3] = -1.0f;
	}
	else if (index == static_cast<UINT>(GBufferType::ATMOSPHERE))
	{
		outColor[3] = 1.0f;
	}
	else if (index == static_cast<UINT>(GBufferType::VELOCITY))
	{
		outColor[0] = 0.5f;
		outColor[1] = 0.5f;
	}
}

void RenderTargets::ReleaseGBufferResources()
{
	for (UINT i = 0; i < g_kGBUFFER_COUNT; ++i)
	{
		m_GBufferTargets[i].Reset();
		m_GBufferRtvHandles[i] = {};
		m_GBufferSrvHandles[i] = {};
	}
	m_LowResDepthBuffer.Reset();
}

bool RenderTargets::CreateDepthBuffer()
{
	UINT dsvHeapSize = 1 + RendererState::g_kMAX_SHADOW_LIGHTS + 1;
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
	dsvHeapDesc.NumDescriptors = dsvHeapSize;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	HRESULT hr = m_Device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_DsvHeap));
	if (FAILED(hr))
	{
		Debug::Log("ERROR: CreateDescriptorHeap(DSV) failed\n");
		return false;
	}

	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

	UINT dsvIncrement = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

	D3D12_RESOURCE_DESC depthDesc{};
	depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthDesc.Width = m_Width;
	depthDesc.Height = m_Height;
	depthDesc.DepthOrArraySize = 1;
	depthDesc.MipLevels = 1;
	depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	depthDesc.SampleDesc.Count = 1;
	depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE clearValue{};
	clearValue.Format = DXGI_FORMAT_D32_FLOAT;
	clearValue.DepthStencil.Depth = 1.0f;
	clearValue.DepthStencil.Stencil = 0;

	hr = m_Device->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&depthDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		&clearValue,
		IID_PPV_ARGS(&m_DepthStencilBuffer));
	if (FAILED(hr))
	{
		Debug::Log("ERROR: CreateCommittedResource(Depth) failed\n");
		return false;
	}

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
	dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	m_Device->CreateDepthStencilView(m_DepthStencilBuffer.Get(), &dsvDesc,
		m_DsvHeap->GetCPUDescriptorHandleForHeapStart());
	m_DepthStencilState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	UINT cbvIncrementDepth = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	CD3DX12_CPU_DESCRIPTOR_HANDLE depthSrvCpuHandle(m_CbvHeap->GetCPUDescriptorHandleForHeapStart(), RendererState::g_kDEPTH_SRV_INDEX, cbvIncrementDepth);
	D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc{};
	depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
	depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	depthSrvDesc.Texture2D.MipLevels = 1;
	m_Device->CreateShaderResourceView(m_DepthStencilBuffer.Get(), &depthSrvDesc, depthSrvCpuHandle);

	UINT lowResDsvIndex = 1 + RendererState::g_kMAX_SHADOW_LIGHTS;
	CD3DX12_CPU_DESCRIPTOR_HANDLE lowResDsvHandle(m_DsvHeap->GetCPUDescriptorHandleForHeapStart(), lowResDsvIndex, dsvIncrement);
	m_LowResDsvHandle = lowResDsvHandle;

	return true;
}

bool RenderTargets::CreateShadowDepthBuffer()
{
	D3D12_CLEAR_VALUE clearValue{};
	clearValue.Format = DXGI_FORMAT_D32_FLOAT;
	clearValue.DepthStencil.Depth = 1.0f;
	clearValue.DepthStencil.Stencil = 0;
	auto depthDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_R32_TYPELESS,
		RendererState::g_kSHADOW_MAP_SIZE,
		RendererState::g_kSHADOW_MAP_SIZE,
		RendererState::g_kMAX_SHADOW_LIGHTS,
		1,
		1,
		0,
		D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	HRESULT hr = m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue, IID_PPV_ARGS(&m_ShadowDepthBuffer));
	if (FAILED(hr)) { Debug::Log("ERROR: CreateCommittedResource(ShadowDepth) failed\n"); return false; }
	if (!m_DsvHeap)
	{
		D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
		dsvHeapDesc.NumDescriptors = 1 + RendererState::g_kMAX_SHADOW_LIGHTS + 1;
		dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		hr = m_Device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_DsvHeap));
		if (FAILED(hr)) { Debug::Log("ERROR: CreateDescriptorHeap(DSV) failed\n"); return false; }
	}
	const UINT dsvIncrement = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	for (UINT i = 0; i < RendererState::g_kMAX_SHADOW_LIGHTS; ++i)
	{
		D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
		dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		dsvDesc.Texture2DArray.MipSlice = 0;
		dsvDesc.Texture2DArray.FirstArraySlice = i;
		dsvDesc.Texture2DArray.ArraySize = 1;
		CD3DX12_CPU_DESCRIPTOR_HANDLE shadowDsvHandle(m_DsvHeap->GetCPUDescriptorHandleForHeapStart(), 1 + i, dsvIncrement);
		m_Device->CreateDepthStencilView(m_ShadowDepthBuffer.Get(), &dsvDesc, shadowDsvHandle);
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
	srvDesc.Texture2DArray.MostDetailedMip = 0;
	srvDesc.Texture2DArray.MipLevels = 1;
	srvDesc.Texture2DArray.FirstArraySlice = 0;
	srvDesc.Texture2DArray.ArraySize = RendererState::g_kMAX_SHADOW_LIGHTS;
	CD3DX12_CPU_DESCRIPTOR_HANDLE srvCpuHandle(m_CbvHeap->GetCPUDescriptorHandleForHeapStart(), RendererState::g_kSHADOW_SRV_INDEX, m_CbvIncrementSize);
	m_Device->CreateShaderResourceView(m_ShadowDepthBuffer.Get(), &srvDesc, srvCpuHandle);
	auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(RendererState::g_kSHADOW_CB_ALIGNED_SIZE * RendererState::g_kSHADOW_CB_SLOT_COUNT);
	auto cbHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	hr = m_Device->CreateCommittedResource(&cbHeapProps, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_ShadowConstantBuffer));
	if (FAILED(hr)) { Debug::Log("ERROR: CreateCommittedResource(Shadow CB) failed\n"); return false; }
	CD3DX12_RANGE readRange(0, 0);
	m_ShadowConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_pShadowCbvDataBegin));
	m_ShadowViewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(RendererState::g_kSHADOW_MAP_SIZE), static_cast<float>(RendererState::g_kSHADOW_MAP_SIZE));
	m_ShadowScissorRect = CD3DX12_RECT(0, 0, RendererState::g_kSHADOW_MAP_SIZE, RendererState::g_kSHADOW_MAP_SIZE);
	return true;
}

bool RenderTargets::CreateSceneRenderTarget()
{
	if (m_SceneWidth == 0 || m_SceneHeight == 0) return true;

	ReleaseGBufferResources();
	for (auto& resource : m_RenderTargetResources)
	{
		resource.Reset();
	}
	m_RenderTargetRtvHandles = {};
	m_RenderTargetSrvHandles = {};
	m_TransparentSceneCopy.Reset();
	m_SceneRtvHeap.Reset();

	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto resDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		m_SceneColorFormat, m_SceneWidth, m_SceneHeight, 1, 1, 1, 0,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
	auto fullResDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		m_SceneColorFormat, m_Width, m_Height, 1, 1, 1, 0,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

	D3D12_CLEAR_VALUE clearValue{};
	clearValue.Format = m_SceneColorFormat;
	clearValue.Color[0] = m_kSceneClearColor[0];
	clearValue.Color[1] = m_kSceneClearColor[1];
	clearValue.Color[2] = m_kSceneClearColor[2];
	clearValue.Color[3] = m_kSceneClearColor[3];

	constexpr array<const wchar_t*, kRenderTargetCount> kRenderTargetNames =
	{
		L"Scene Lighting (Internal Resolution)",
		L"Post Process (Internal Resolution)",
		L"Pre-Upscale Antialiasing",
		L"Pre-Upscale TAA History",
		L"Editor Scene (Display Resolution)"
	};


	for (size_t i = 0; i < kRenderTargetCount; ++i)
	{
		const auto type = static_cast<RenderTargetType>(i);
		const D3D12_RESOURCE_DESC* desc;
		if (type == RenderTargetType::EditorScene)
		{
			desc = &fullResDesc;
		}
		else
		{
			desc = &resDesc;
		}

		HRESULT hr = m_Device->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			desc,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			&clearValue,
			IID_PPV_ARGS(&GetRenderTarget(type)));

		if (FAILED(hr))
			return false;

		GetRenderTarget(type)->SetName(kRenderTargetNames[i]);
	}

	auto transparentCopyDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		m_SceneColorFormat, m_Width, m_Height, 1, 1, 1, 0,
		D3D12_RESOURCE_FLAG_NONE);
	HRESULT hr = m_Device->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &transparentCopyDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_TransparentSceneCopy));
	if (FAILED(hr)) return false;
	m_TransparentSceneCopy->SetName(L"TransparentSceneCopy");

	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
	UINT rtvCount;
	if ((m_RenderMode == RenderMode::DEFERRED))
	{
		rtvCount = (4 + g_kGBUFFER_COUNT);
	}
	else
	{
		rtvCount = 4;
	}
	rtvHeapDesc.NumDescriptors = rtvCount + 1;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	hr = m_Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_SceneRtvHeap));
	if (FAILED(hr)) return false;

	UINT cbvIncrement = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	UINT rtvIncrement = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	const D3D12_CPU_DESCRIPTOR_HANDLE rtvHeapStart = m_SceneRtvHeap->GetCPUDescriptorHandleForHeapStart();

	constexpr array<RenderTargetType, 4> kRtvRenderTargets =
	{
		RenderTargetType::Scene,
		RenderTargetType::EditorScene,
		RenderTargetType::PostProcess,
		RenderTargetType::PreUpscaleAa
	};

	for (UINT i = 0; i < static_cast<UINT>(kRtvRenderTargets.size()); ++i)
	{
		const RenderTargetType type = kRtvRenderTargets[i];
		GetRenderTargetRtvHandle(type) = CD3DX12_CPU_DESCRIPTOR_HANDLE(
			rtvHeapStart, i, rtvIncrement);
		m_Device->CreateRenderTargetView(
			GetRenderTarget(type).Get(),
			nullptr,
			GetRenderTargetRtvHandle(type));
	}

	constexpr array<UINT, kRenderTargetCount> kRenderTargetSrvIndices =
	{
		RendererState::g_kSCENE_SRV_INDEX,
		RendererState::g_kPOST_PROCESS_SRV_INDEX,
		RendererState::g_kPRE_UPSCALE_AA_SRV_INDEX,
		RendererState::g_kPRE_UPSCALE_AA_HISTORY_SRV_INDEX,
		RendererState::g_kEDITOR_SCENE_SRV_INDEX
	};

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = m_SceneColorFormat;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;

	const D3D12_GPU_DESCRIPTOR_HANDLE cbvHeapGpuStart = m_CbvHeap->GetGPUDescriptorHandleForHeapStart();
	const D3D12_CPU_DESCRIPTOR_HANDLE cbvHeapCpuStart = m_CbvHeap->GetCPUDescriptorHandleForHeapStart();

	for (size_t i = 0; i < kRenderTargetCount; ++i)
	{
		const auto type = static_cast<RenderTargetType>(i);
		const UINT srvIndex = kRenderTargetSrvIndices[i];

		GetRenderTargetSrvHandle(type) = CD3DX12_GPU_DESCRIPTOR_HANDLE(
			cbvHeapGpuStart, srvIndex, cbvIncrement);

		const CD3DX12_CPU_DESCRIPTOR_HANDLE srvCpuHandle(
			cbvHeapCpuStart, srvIndex, cbvIncrement);

		m_Device->CreateShaderResourceView(
			GetRenderTarget(type).Get(),
			&srvDesc,
			srvCpuHandle);
	}

	m_EditorSceneUavHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(
		cbvHeapGpuStart,
		RendererState::g_kEDITOR_SCENE_UAV_INDEX,
		cbvIncrement);

	D3D12_UNORDERED_ACCESS_VIEW_DESC editorUavDesc{};
	editorUavDesc.Format = m_SceneColorFormat;
	editorUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	m_Device->CreateUnorderedAccessView(
		GetRenderTarget(RenderTargetType::EditorScene).Get(),
		nullptr,
		&editorUavDesc,
		CD3DX12_CPU_DESCRIPTOR_HANDLE(
			cbvHeapCpuStart,
			RendererState::g_kEDITOR_SCENE_UAV_INDEX,
			cbvIncrement));

	m_TransparentSceneSrvHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(
		cbvHeapGpuStart,
		RendererState::g_kTRANSPARENT_SCENE_SRV_INDEX,
		cbvIncrement);
	const CD3DX12_CPU_DESCRIPTOR_HANDLE transparentSceneSrvCpuHandle(
		cbvHeapCpuStart,
		RendererState::g_kTRANSPARENT_SCENE_SRV_INDEX,
		cbvIncrement);
	m_Device->CreateShaderResourceView(
		m_TransparentSceneCopy.Get(),
		&srvDesc,
		transparentSceneSrvCpuHandle);

	{
		auto aaDesc = CD3DX12_RESOURCE_DESC::Tex2D(
			m_SceneColorFormat, m_Width, m_Height, 1, 1, 1, 0,
			D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
		D3D12_CLEAR_VALUE aaClear{};
		aaClear.Format = m_SceneColorFormat;
		aaClear.Color[0] = m_kSceneClearColor[0];
		aaClear.Color[1] = m_kSceneClearColor[1];
		aaClear.Color[2] = m_kSceneClearColor[2];
		aaClear.Color[3] = m_kSceneClearColor[3];
		m_AaRenderTarget.Reset();
		hr = m_Device->CreateCommittedResource(
			&heapProps, D3D12_HEAP_FLAG_NONE, &aaDesc,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &aaClear,
			IID_PPV_ARGS(&m_AaRenderTarget));
		if (FAILED(hr)) return false;
		m_AaRenderTarget->SetName(L"AaRenderTarget");

		UINT rtvHeapSize = rtvIncrement;
		UINT aaRtvOffset = rtvCount;
		m_AaRtvHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
			rtvHeapStart, aaRtvOffset, rtvHeapSize);
		m_Device->CreateRenderTargetView(m_AaRenderTarget.Get(), nullptr, m_AaRtvHandle);

		m_AaSrvHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(
			cbvHeapGpuStart, RendererState::g_kAA_SRV_INDEX, cbvIncrement);
		const CD3DX12_CPU_DESCRIPTOR_HANDLE aaSrvCpuHandle(
			cbvHeapCpuStart, RendererState::g_kAA_SRV_INDEX, cbvIncrement);

		D3D12_SHADER_RESOURCE_VIEW_DESC aaSrvDesc{};
		aaSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		aaSrvDesc.Format = m_SceneColorFormat;
		aaSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		aaSrvDesc.Texture2D.MipLevels = 1;
		m_Device->CreateShaderResourceView(m_AaRenderTarget.Get(), &aaSrvDesc, aaSrvCpuHandle);

		const CD3DX12_CPU_DESCRIPTOR_HANDLE historySrvCpuHandle(
			cbvHeapCpuStart, RendererState::g_kAA_HISTORY_SRV_INDEX, cbvIncrement);
		m_Device->CreateShaderResourceView(m_AaRenderTarget.Get(), &aaSrvDesc, historySrvCpuHandle);
		m_TaaFrameIndex = 0;
	}

	if (m_EnvironmentTextureSrvIndex < 0)
	{
		m_EnvironmentTextureSrvIndex = TextureManager::LoadTexture("asset\\model\\sky\\charolettenbrunn_park_2k.DDS");
	}

	if (m_RenderMode == RenderMode::DEFERRED)
	{
		UINT gbufferWidth = m_SceneWidth;
		UINT gbufferHeight = m_SceneHeight;
		CD3DX12_CPU_DESCRIPTOR_HANDLE gbufferRtvHandle(rtvHeapStart, 4, rtvIncrement);
		for (UINT i = 0; i < g_kGBUFFER_COUNT; ++i)
		{
			const bool halfResolutionAtmosphere =
				i == static_cast<UINT>(GBufferType::ATMOSPHERE) &&
				m_ResolutionScale >= 0.75f;
			const bool halfResolutionBloom = i == static_cast<UINT>(GBufferType::BLOOM);
			UINT targetWidth;
			if ((halfResolutionAtmosphere || halfResolutionBloom))
			{
				targetWidth = max((gbufferWidth + 1u) / 2u, 1u);
			}
			else
			{
				targetWidth = gbufferWidth;
			}
			UINT targetHeight;
			if ((halfResolutionAtmosphere || halfResolutionBloom))
			{
				targetHeight = max((gbufferHeight + 1u) / 2u, 1u);
			}
			else
			{
				targetHeight = gbufferHeight;
			}
			D3D12_RESOURCE_FLAGS gbufferFlags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			if (i < g_kGEOMETRY_GBUFFER_COUNT)
			{
				gbufferFlags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			}
			auto gbufferDesc = CD3DX12_RESOURCE_DESC::Tex2D(
				m_kDeferredRtvFormats[i], targetWidth, targetHeight, 1, 1, 1, 0,
				gbufferFlags);

			D3D12_CLEAR_VALUE gbufferClear{};
			gbufferClear.Format = m_kDeferredRtvFormats[i];
			GetGBufferClearColor(i, gbufferClear.Color);

			hr = m_Device->CreateCommittedResource(
				&heapProps,
				D3D12_HEAP_FLAG_NONE,
				&gbufferDesc,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				&gbufferClear,
				IID_PPV_ARGS(&m_GBufferTargets[i]));
			if (FAILED(hr)) return false;

			m_GBufferTargets[i]->SetName(g_GBufferTargetNames[i]);

			D3D12_RENDER_TARGET_VIEW_DESC gbufferRtvDesc{};
			gbufferRtvDesc.Format = m_kDeferredRtvFormats[i];
			gbufferRtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			m_Device->CreateRenderTargetView(m_GBufferTargets[i].Get(), &gbufferRtvDesc, gbufferRtvHandle);
			m_GBufferRtvHandles[i] = gbufferRtvHandle;

			const UINT gbufferSrvIndex = RendererState::g_kGBUFFER_SRV_START_INDEX + i;
			CD3DX12_CPU_DESCRIPTOR_HANDLE gbufferSrvCpuHandle(
				cbvHeapCpuStart, gbufferSrvIndex, cbvIncrement);
			m_GBufferSrvHandles[i] = CD3DX12_GPU_DESCRIPTOR_HANDLE(
				cbvHeapGpuStart, gbufferSrvIndex, cbvIncrement);

			D3D12_SHADER_RESOURCE_VIEW_DESC gbufferSrvDesc{};
			gbufferSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			gbufferSrvDesc.Format = m_kDeferredRtvFormats[i];
			gbufferSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			gbufferSrvDesc.Texture2D.MipLevels = 1;
			m_Device->CreateShaderResourceView(m_GBufferTargets[i].Get(), &gbufferSrvDesc, gbufferSrvCpuHandle);

			gbufferRtvHandle.Offset(1, rtvIncrement);
		}
	}

	if (!SpatialUpscaler::Resize(m_Width, m_Height, m_SceneColorFormat))
	{
		Debug::Log("WARNING: SpatialUpscaler::Resize(%u, %u) failed; FSR1 falls back to bilateral.\n", m_Width, m_Height);
	}
	if (!ScreenSpaceEffects::Resize(m_SceneWidth, m_SceneHeight, m_SceneColorFormat))
	{
		Debug::Log("WARNING: ScreenSpaceEffects::Resize(%u, %u) failed; SSAO/SSGI disabled.\n", m_SceneWidth, m_SceneHeight);
	}

	return true;
}

void RenderTargets::ResizeScene(UINT width, UINT height)
{
	m_SceneWidth = width;
	m_SceneHeight = height;
	CreateSceneRenderTarget();
}

D3D12_CPU_DESCRIPTOR_HANDLE RenderTargets::GetImGuiCpuHandle()
{
	UINT cbvIncrement = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetCPUDescriptorHandleForHeapStart(), RendererState::g_kENGINE_DESCRIPTOR_END + 1, cbvIncrement);
}

D3D12_GPU_DESCRIPTOR_HANDLE RenderTargets::GetImGuiGpuHandle()
{
	UINT cbvIncrement = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	return CD3DX12_GPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(), RendererState::g_kENGINE_DESCRIPTOR_END + 1, cbvIncrement);
}
