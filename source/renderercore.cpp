#include "pch.h"
#include "rendererdraw.h"
#include "renderershader.h"
#include "rendererutils.h"
#include "world.h"
#include "ecs.h"
#include "texturemanager.h"
#include "componentmanager.h"
#include "imguimanager.h"
#include "psomanager.h"


static void LogToFile(const char* msg)
{
	FILE* fp = nullptr;
	fopen_s(&fp, "init_log.txt", "a");
	if (fp) { fprintf(fp, "%s", msg); fclose(fp); }
}


bool RendererCore::Init(HWND hwnd)
{
	m_FrameIndex = 0;
	m_FenceEvent = nullptr;
	for (UINT i = 0; i < g_kFRAME_COUNT; ++i)
	{
		m_FenceValues[i] = 0;
	}
	m_Hwnd = hwnd;

	RECT rc;
	GetClientRect(hwnd, &rc);
	m_Width = rc.right - rc.left;
	m_Height = rc.bottom - rc.top;

	if (m_Width == 0) m_Width = g_kSCREEN_WIDTH;
	if (m_Height == 0) m_Height = g_kSCREEN_HEIGHT;

	m_SceneWidth = m_Width;
	m_SceneHeight = m_Height;

#ifdef _DEBUG
	ComPtr<ID3D12Debug> debugController;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
	{
		debugController->EnableDebugLayer();
	}
#endif

	LogToFile("Step: CreateDXGIFactory2\n");
	HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_Factory));
	if (FAILED(hr))
	{
		LogToFile("ERROR: CreateDXGIFactory2 failed\n");
		return false;
	}

	m_AllowTearing = false;
	ComPtr<IDXGIFactory5> factory5;
	if (SUCCEEDED(m_Factory.As(&factory5)))
	{
		BOOL allowTearing = FALSE;
		if (SUCCEEDED(factory5->CheckFeatureSupport(
			DXGI_FEATURE_PRESENT_ALLOW_TEARING,
			&allowTearing,
			sizeof(allowTearing))))
		{
			m_AllowTearing = allowTearing == TRUE;
		}
	}

	LogToFile("Step: D3D12CreateDevice\n");
	hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_Device));
	if (FAILED(hr))
	{
		LogToFile("ERROR: D3D12CreateDevice failed\n");
		return false;
	}

	D3D12_COMMAND_QUEUE_DESC queueDesc {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	hr = m_Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_CommandQueue));
	if (FAILED(hr))
	{
		LogToFile("ERROR: CreateCommandQueue failed\n");
		return false;
	}

	DXGI_SWAP_CHAIN_DESC1 swapChainDesc {};
	swapChainDesc.BufferCount = g_kFRAME_COUNT;
	swapChainDesc.Width = m_Width;
	swapChainDesc.Height = m_Height;
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.Flags = m_AllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

	ComPtr<IDXGISwapChain1> sc1;
	hr = m_Factory->CreateSwapChainForHwnd(m_CommandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &sc1);
	if (FAILED(hr))
	{
		LogToFile("ERROR: CreateSwapChainForHwnd failed\n");
		return false;
	}
	sc1.As(&m_SwapChain);
	m_FrameIndex = m_SwapChain->GetCurrentBackBufferIndex();

	D3D12_DESCRIPTOR_HEAP_DESC rtvDesc {};
	rtvDesc.NumDescriptors = g_kFRAME_COUNT;
	rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	hr = m_Device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_RtvHeap));
	if (FAILED(hr))
	{
		LogToFile("ERROR: CreateDescriptorHeap(RTV) failed\n");
		return false;
	}

	UINT rtvSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_RtvHeap->GetCPUDescriptorHandleForHeapStart());
	for (UINT n = 0; n < g_kFRAME_COUNT; n++)
	{
		m_SwapChain->GetBuffer(n, IID_PPV_ARGS(&m_RenderTargets[n]));
		m_Device->CreateRenderTargetView(m_RenderTargets[n].Get(), nullptr, rtvHandle);
		rtvHandle.Offset(1, rtvSize);
	}

	for (UINT n = 0; n < g_kFRAME_COUNT; n++)
	{
		hr = m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_CommandAllocator[n]));
		if (FAILED(hr))
		{
			LogToFile("ERROR: CreateCommandAllocator failed\n");
			return false;
		}
	}

	LogToFile("Step: CreateCommandList\n");
	hr = m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_CommandAllocator[0].Get(), nullptr, IID_PPV_ARGS(&m_CommandList));
	if (FAILED(hr))
	{
		LogToFile("ERROR: CreateCommandList failed\n");
		return false;
	}
	m_CommandList->Close();

	LogToFile("Step: CreateFence\n");
	hr = m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence));
	if (FAILED(hr))
	{
		LogToFile("ERROR: CreateFence failed\n");
		return false;
	}
	m_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	m_CurrentFenceValue = 0;
	for (UINT i = 0; i < g_kFRAME_COUNT; i++)
	{
		m_FenceValues[i] = 0;
	}

	CD3DX12_DESCRIPTOR_RANGE ranges[1];
	ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);

	CD3DX12_DESCRIPTOR_RANGE rangesTex[1];
	rangesTex[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);
	CD3DX12_DESCRIPTOR_RANGE rangesShadowTex[1];
	rangesShadowTex[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);
	CD3DX12_DESCRIPTOR_RANGE rangesNormalTex[1];
	rangesNormalTex[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);
	CD3DX12_DESCRIPTOR_RANGE rangesEnvironmentTex[1];
	rangesEnvironmentTex[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);

	CD3DX12_ROOT_PARAMETER rootParametersAll[8];
	rootParametersAll[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_ALL);
	rootParametersAll[1].InitAsDescriptorTable(1, &rangesTex[0], D3D12_SHADER_VISIBILITY_ALL);
	rootParametersAll[2].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	rootParametersAll[3].InitAsConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	rootParametersAll[4].InitAsDescriptorTable(1, &rangesShadowTex[0], D3D12_SHADER_VISIBILITY_PIXEL);
	rootParametersAll[5].InitAsConstantBufferView(3, 0, D3D12_SHADER_VISIBILITY_ALL);
	rootParametersAll[6].InitAsDescriptorTable(1, &rangesNormalTex[0], D3D12_SHADER_VISIBILITY_PIXEL);
	rootParametersAll[7].InitAsDescriptorTable(1, &rangesEnvironmentTex[0], D3D12_SHADER_VISIBILITY_PIXEL);

	CD3DX12_STATIC_SAMPLER_DESC sampler {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.MipLODBias = 0.0f;
	sampler.MaxAnisotropy = 16;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
	sampler.MinLOD = 0.0f;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderRegister = 0;
	sampler.RegisterSpace = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	CD3DX12_STATIC_SAMPLER_DESC staticSamplers[2] {};
	staticSamplers[0] = sampler;
	staticSamplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	staticSamplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	staticSamplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	staticSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	staticSamplers[1].MipLODBias = 0.0f;
	staticSamplers[1].MaxAnisotropy = 1;
	staticSamplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	staticSamplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	staticSamplers[1].MinLOD = 0.0f;
	staticSamplers[1].MaxLOD = D3D12_FLOAT32_MAX;
	staticSamplers[1].ShaderRegister = 1;
	staticSamplers[1].RegisterSpace = 0;
	staticSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
	rootSignatureDesc.Init(_countof(rootParametersAll), rootParametersAll, _countof(staticSamplers), staticSamplers, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
	ComPtr<ID3DBlob> signature;
	ComPtr<ID3DBlob> error;
	LogToFile("Step: D3D12SerializeRootSignature\n");
	hr = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
	if (FAILED(hr))
	{
		LogToFile("ERROR: D3D12SerializeRootSignature failed\n");
		if (error)
		{
			Debug::Log("%s\\n", (char*)error->GetBufferPointer());
			FILE* fp = nullptr;
			fopen_s(&fp, "error_log.txt", "w");
			if (fp) {
				fprintf(fp, "%s\n", (char*)error->GetBufferPointer());
				fclose(fp);
			}
		}
		return false;
	}
	LogToFile("Step: CreateRootSignature\n");
	hr = m_Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_RootSignature));
	if (FAILED(hr))
	{
		LogToFile("ERROR: CreateRootSignature failed\n");
		return false;
	}

	D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc {};
	cbvHeapDesc.NumDescriptors = g_kSHADOW_SRV_INDEX + 1;
	cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	LogToFile("Step: CreateDescriptorHeap(CBV)\n");
	hr = m_Device->CreateDescriptorHeap(&cbvHeapDesc, IID_PPV_ARGS(&m_CbvHeap));
	if (FAILED(hr))
	{
		LogToFile("ERROR: CreateDescriptorHeap(CBV) failed\n");
		return false;
	}

	m_PipelineState = nullptr;

	m_Viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, (float)m_SceneWidth, (float)m_SceneHeight);
	m_ScissorRect = CD3DX12_RECT(0, 0, m_SceneWidth, m_SceneHeight);

	const UINT totalCbSize = g_kCB_ALIGNED_SIZE * g_kCBV_COUNT;
	auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(totalCbSize);
	auto cbHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	hr = m_Device->CreateCommittedResource(
		&cbHeapProps,
		D3D12_HEAP_FLAG_NONE,
		&cbDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_ConstantBuffer));
	if (FAILED(hr))
	{
		LogToFile("ERROR: CreateCommittedResource(CB) failed\n");
		return false;
	}

	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc {};
	cbvDesc.SizeInBytes = g_kCB_ALIGNED_SIZE;
	CD3DX12_CPU_DESCRIPTOR_HANDLE cbvHandle(m_CbvHeap->GetCPUDescriptorHandleForHeapStart());
	m_CbvIncrementSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	UINT cbvIncrement = m_CbvIncrementSize;
	for (uint32_t i = 0; i < g_kCBV_COUNT; ++i)
	{
		cbvDesc.BufferLocation = m_ConstantBuffer->GetGPUVirtualAddress() + (i * g_kCB_ALIGNED_SIZE);
		m_Device->CreateConstantBufferView(&cbvDesc, cbvHandle);
		cbvHandle.Offset(1, cbvIncrement);
	}

	CD3DX12_RANGE readRange(0, 0);
	m_ConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_pCbvDataBegin));

	LogToFile("Step: CreateDepthBuffer\n");
if (!RendererDraw::CreateDepthBuffer())
	{
		return false;
	}

	LogToFile("Step: CreateModelPipeline\n");
if (!RendererShader::CreateModelPipeline())
	{
		return false;
	}

	const UINT dynamicVertexBufferSize = g_kMAX_DYNAMIC_VERTICES * sizeof(Vertex);
	auto dvbDesc = CD3DX12_RESOURCE_DESC::Buffer(dynamicVertexBufferSize);
	auto dvbHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	hr = m_Device->CreateCommittedResource(
		&dvbHeapProps,
		D3D12_HEAP_FLAG_NONE,
		&dvbDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_DynamicVertexBuffer));
	if (FAILED(hr))
	{
		LogToFile("ERROR: CreateCommittedResource(Dynamic VB) failed\n");
		return false;
	}

	m_DynamicVertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_pDynamicVertexDataBegin));
	m_DynamicVertexBufferView.BufferLocation = m_DynamicVertexBuffer->GetGPUVirtualAddress();
	m_DynamicVertexBufferView.SizeInBytes = dynamicVertexBufferSize;
	m_DynamicVertexBufferView.StrideInBytes = sizeof(Vertex);
	m_DynamicVertexOffset = 0;

	LogToFile("Step: TextureManager::Init\n");
TextureManager::Init();

	LogToFile("Step: CreatePostProcessPipeline\n");
if (!RendererShader::CreatePostProcessPipeline()) return false;

	LogToFile("Step: CreateSceneRenderTarget\n");
if (!RendererDraw::CreateSceneRenderTarget()) return false;

	LogToFile("Step: CreateSkinningPipeline\n");
if (!RendererShader::CreateSkinningPipeline()) return false;

	LogToFile("Step: CreateShadowDepthBuffer\n");
if (!RendererDraw::CreateShadowDepthBuffer()) return false;

	LogToFile("Step: ImGuiManager::Init\n");
if (!ImGuiManager::Init(
		m_Hwnd,
		m_Device.Get(),
		m_CommandQueue.Get(),
		g_kFRAME_COUNT,
		DXGI_FORMAT_R8G8B8A8_UNORM,
		m_CbvHeap.Get(),
		RendererDraw::GetImGuiCpuHandle(),
		RendererDraw::GetImGuiGpuHandle()))
	{
		LogToFile("ERROR: ImGuiManager::Init failed\n");
		return false;
	}

	return true;
}

void RendererCore::Uninit()
{
	if (m_CommandQueue && m_Fence && m_FenceEvent)
	{
		m_CurrentFenceValue++;
		m_CommandQueue->Signal(m_Fence.Get(), m_CurrentFenceValue);
		if (m_Fence->GetCompletedValue() < m_CurrentFenceValue)
		{
			m_Fence->SetEventOnCompletion(m_CurrentFenceValue, m_FenceEvent);
			WaitForSingleObject(m_FenceEvent, INFINITE);
		}
	}

	ImGuiManager::Uninit();

	TextureManager::Uninit();

	if (m_ConstantBuffer)
	{
		m_ConstantBuffer->Unmap(0, nullptr);
		m_pCbvDataBegin = nullptr;
	}
	if (m_PostProcessConstantBuffer)
	{
		m_PostProcessConstantBuffer->Unmap(0, nullptr);
		m_pPostProcessCbvDataBegin = nullptr;
	}

	m_PsoCache.clear();
	m_PostProcessPsoMap.clear();

	if (m_DynamicVertexBuffer)
	{
		m_DynamicVertexBuffer->Unmap(0, nullptr);
		m_DynamicVertexBuffer.Reset();
	}

	m_ModelPipelineState.Reset();
	m_ModelRootSignature.Reset();
	m_DepthStencilBuffer.Reset();
	m_DsvHeap.Reset();
	m_PipelineState.Reset();
	m_ConstantBuffer.Reset();
	m_PostProcessConstantBuffer.Reset();
	m_PostProcessRootSignature.Reset();
    RendererDraw::ReleaseGBufferResources();
	m_SceneRenderTarget.Reset();
	m_SceneRtvHeap.Reset();
	m_CbvHeap.Reset();
	m_RootSignature.Reset();
	m_CommandList.Reset();
	for (UINT n = 0; n < g_kFRAME_COUNT; n++)
	{
		m_CommandAllocator[n].Reset();
	}
	m_Fence.Reset();
	for (UINT n = 0; n < g_kFRAME_COUNT; n++)
	{
		m_RenderTargets[n].Reset();
	}
	m_RtvHeap.Reset();
	m_SwapChain.Reset();
	m_CommandQueue.Reset();
	m_Factory.Reset();
	m_Device.Reset();

	if (m_FenceEvent)
	{
		CloseHandle(m_FenceEvent);
		m_FenceEvent = nullptr;
	}
}

void RendererCore::Resize(UINT width, UINT height)
{
	if (!m_SwapChain || !m_Device)
	{
		return;
	}
	if (width == 0 || height == 0)
	{
		return;
	}
	if (width == m_Width && height == m_Height)
	{
		return;
	}

	if (m_CommandQueue && m_Fence && m_FenceEvent)
	{
		m_CurrentFenceValue++;
		m_CommandQueue->Signal(m_Fence.Get(), m_CurrentFenceValue);
		if (m_Fence->GetCompletedValue() < m_CurrentFenceValue)
		{
			m_Fence->SetEventOnCompletion(m_CurrentFenceValue, m_FenceEvent);
			WaitForSingleObject(m_FenceEvent, INFINITE);
		}

		for (UINT i = 0; i < g_kFRAME_COUNT; ++i)
		{
			m_FenceValues[i] = m_CurrentFenceValue;
		}
	}

	for (UINT n = 0; n < g_kFRAME_COUNT; n++)
	{
		m_RenderTargets[n].Reset();
	}

	m_DepthStencilBuffer.Reset();

	HRESULT hr = m_SwapChain->ResizeBuffers(
		g_kFRAME_COUNT, width, height,
		DXGI_FORMAT_R8G8B8A8_UNORM,
		m_AllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
	if (FAILED(hr))
	{
		LogToFile("ERROR: ResizeBuffers failed\n");
		return;
	}

	m_FrameIndex = m_SwapChain->GetCurrentBackBufferIndex();

	UINT rtvSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_RtvHeap->GetCPUDescriptorHandleForHeapStart());
	for (UINT n = 0; n < g_kFRAME_COUNT; n++)
	{
		m_SwapChain->GetBuffer(n, IID_PPV_ARGS(&m_RenderTargets[n]));
		m_Device->CreateRenderTargetView(m_RenderTargets[n].Get(), nullptr, rtvHandle);
		rtvHandle.Offset(1, rtvSize);
	}

	D3D12_RESOURCE_DESC depthDesc {};
	depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthDesc.Width = width;
	depthDesc.Height = height;
	depthDesc.DepthOrArraySize = 1;
	depthDesc.MipLevels = 1;
	depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
	depthDesc.SampleDesc.Count = 1;
	depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE clearValue {};
	clearValue.Format = DXGI_FORMAT_D32_FLOAT;
	clearValue.DepthStencil.Depth = 1.0f;
	clearValue.DepthStencil.Stencil = 0;

	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	hr = m_Device->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&depthDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		&clearValue,
		IID_PPV_ARGS(&m_DepthStencilBuffer));
	if (FAILED(hr))
	{
		LogToFile("ERROR: Resize CreateCommittedResource(Depth) failed\n");
		return;
	}

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc {};
	dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	m_Device->CreateDepthStencilView(m_DepthStencilBuffer.Get(), &dsvDesc,
		m_DsvHeap->GetCPUDescriptorHandleForHeapStart());
	m_DepthStencilState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	m_Viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, (float)width, (float)height);
	m_ScissorRect = CD3DX12_RECT(0, 0, width, height);

	m_Width = width;
	m_Height = height;
	m_SceneWidth = width;
	m_SceneHeight = height;

	RendererDraw::CreateSceneRenderTarget();
}

void RendererCore::ApplyPendingRenderMode()
{
	m_PendingRenderMode = RenderMode::DEFERRED;
	if (!m_HasPendingRenderMode || m_RenderMode == m_PendingRenderMode)
	{
		m_HasPendingRenderMode = false;
		m_RenderMode = RenderMode::DEFERRED;
		return;
	}

	if (m_CommandQueue && m_Fence && m_FenceEvent)
	{
		m_CurrentFenceValue++;
		m_CommandQueue->Signal(m_Fence.Get(), m_CurrentFenceValue);
		if (m_Fence->GetCompletedValue() < m_CurrentFenceValue)
		{
			m_Fence->SetEventOnCompletion(m_CurrentFenceValue, m_FenceEvent);
			WaitForSingleObject(m_FenceEvent, INFINITE);
		}

		for (UINT i = 0; i < g_kFRAME_COUNT; ++i)
		{
			m_FenceValues[i] = m_CurrentFenceValue;
		}
	}

	m_RenderMode = m_PendingRenderMode;
	m_HasPendingRenderMode = false;

	for (EntityID entity : World::GetView<ShaderComponent>())
	{
		ComponentManager::GetComponent<ShaderComponent>(entity).Pso.Reset();
	}
	m_PsoCache.clear();

	if (m_Device && m_SceneWidth > 0 && m_SceneHeight > 0)
	{
		RendererDraw::CreateSceneRenderTarget();
	}
}

void RendererCore::SetRenderMode(RenderMode mode)
{
	(void)mode;
	if (GetRequestedRenderMode() == RenderMode::DEFERRED)
	{
		return;
	}

	m_PendingRenderMode = RenderMode::DEFERRED;
	m_HasPendingRenderMode = true;
}

void RendererCore::SetHdr(bool enabled)
{
	DXGI_FORMAT targetFormat = enabled ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
	if (m_SceneColorFormat == targetFormat)
	{
		return;
	}
	m_PendingHdr = enabled;
	m_HasPendingHdr = true;
}

void RendererCore::ApplyPendingHdr()
{
	if (!m_HasPendingHdr || m_SceneColorFormat == (m_PendingHdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM))
	{
		m_HasPendingHdr = false;
		return;
	}

	if (m_CommandQueue && m_Fence && m_FenceEvent)
	{
		m_CurrentFenceValue++;
		m_CommandQueue->Signal(m_Fence.Get(), m_CurrentFenceValue);
		if (m_Fence->GetCompletedValue() < m_CurrentFenceValue)
		{
			m_Fence->SetEventOnCompletion(m_CurrentFenceValue, m_FenceEvent);
			WaitForSingleObject(m_FenceEvent, INFINITE);
		}

		for (UINT i = 0; i < g_kFRAME_COUNT; ++i)
		{
			m_FenceValues[i] = m_CurrentFenceValue;
		}
	}

	m_SceneColorFormat = m_PendingHdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
	m_HasPendingHdr = false;

	m_PsoCache.clear();
	m_PostProcessPsoMap.clear();
	m_DeferredLightingPso.Reset();

	for (EntityID entity : World::GetView<ShaderComponent>())
	{
		ComponentManager::GetComponent<ShaderComponent>(entity).Pso.Reset();
	}

	if (m_Device && m_SceneWidth > 0 && m_SceneHeight > 0)
	{
		RendererDraw::CreateSceneRenderTarget();
	}

	PsoManager::CreatePostProcessPipelines();
}





