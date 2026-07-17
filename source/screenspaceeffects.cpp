#include "pch.h"
#include "screenspaceeffects.h"
#include "rendererstate.h"
#include "renderersettings.h"
#include "camera.h"
#include "renderprofiler.h"

namespace
{
	constexpr D3D12_RESOURCE_STATES kShaderReadState =
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

	ComPtr<ID3D12Device> g_Device;
	ComPtr<ID3D12DescriptorHeap> g_Heap;
	UINT g_DescriptorIncrement = 0;
	ComPtr<ID3D12RootSignature> g_RootSignature;
	ComPtr<ID3D12PipelineState> g_DeinterleavePso;
	ComPtr<ID3D12PipelineState> g_RayBinningPso;
	ComPtr<ID3D12PipelineState> g_SsgiPso;
	ComPtr<ID3D12PipelineState> g_ResolvePso;
	ComPtr<ID3D12PipelineState> g_SsaoPso;
	ComPtr<ID3D12Resource> g_Ao;
	ComPtr<ID3D12Resource> g_Gi;
	ComPtr<ID3D12Resource> g_History;
	ComPtr<ID3D12Resource> g_DeDepth;
	ComPtr<ID3D12Resource> g_DeNormal;
	ComPtr<ID3D12Resource> g_DeLight;
	ComPtr<ID3D12Resource> g_DeGi;
	ComPtr<ID3D12Resource> g_RayOrder;
	ComPtr<ID3D12Resource> g_Constants;
	UINT8* g_MappedConstants = nullptr;
	UINT g_Width = 0;
	UINT g_Height = 0;
	UINT g_DeWidth = 0;
	UINT g_DeHeight = 0;
	UINT g_RayCount = 0;
	DXGI_FORMAT g_LightingFormat = DXGI_FORMAT_UNKNOWN;
	bool g_HistoryValid = false;
	bool g_RayOrderInitialized = false;

	struct alignas(16) Constants
	{
		XMFLOAT4X4 InvViewProjection{};
		XMUINT2 FullExtent{};
		XMUINT2 DeinterleavedExtent{};
		XMFLOAT4 EffectParams{};
		XMUINT4 FeatureFlags{};
	};
	constexpr UINT kConstantStride = (sizeof(Constants) + 255u) & ~255u;

	D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT index)
	{
		return CD3DX12_CPU_DESCRIPTOR_HANDLE(
			g_Heap->GetCPUDescriptorHandleForHeapStart(), index, g_DescriptorIncrement);
	}

	D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT index)
	{
		return CD3DX12_GPU_DESCRIPTOR_HANDLE(
			g_Heap->GetGPUDescriptorHandleForHeapStart(), index, g_DescriptorIncrement);
	}

	bool LoadPipeline(const wchar_t* path, ComPtr<ID3D12PipelineState>& output)
	{
		ComPtr<ID3DBlob> shader;
		if (FAILED(D3DReadFileToBlob(path, &shader)))
		{
			Debug::Log("Screen-space shader is missing: %ls\n", path);
			return false;
		}
		D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
		desc.pRootSignature = g_RootSignature.Get();
		desc.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
		return SUCCEEDED(g_Device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&output)));
	}

	bool CreateTexture(
		UINT width,
		UINT height,
		UINT16 arraySize,
		DXGI_FORMAT format,
		ComPtr<ID3D12Resource>& resource,
		UINT srvIndex,
		UINT uavIndex)
	{
		auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
			format, width, height, arraySize, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		if (FAILED(g_Device->CreateCommittedResource(
			&heap, D3D12_HEAP_FLAG_NONE, &desc, kShaderReadState, nullptr, IID_PPV_ARGS(&resource))))
		{
			return false;
		}

		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Format = format;
		srv.ViewDimension = arraySize > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY : D3D12_SRV_DIMENSION_TEXTURE2D;
		if (arraySize > 1)
		{
			srv.Texture2DArray.MipLevels = 1;
			srv.Texture2DArray.ArraySize = arraySize;
		}
		else
		{
			srv.Texture2D.MipLevels = 1;
		}
		g_Device->CreateShaderResourceView(resource.Get(), &srv, Cpu(srvIndex));

		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = format;
		uav.ViewDimension = arraySize > 1 ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY : D3D12_UAV_DIMENSION_TEXTURE2D;
		if (arraySize > 1) uav.Texture2DArray.ArraySize = arraySize;
		g_Device->CreateUnorderedAccessView(resource.Get(), nullptr, &uav, Cpu(uavIndex));
		return true;
	}

	void BindCommon(
		ID3D12GraphicsCommandList* commandList,
		D3D12_GPU_DESCRIPTOR_HANDLE t0,
		D3D12_GPU_DESCRIPTOR_HANDLE t1,
		D3D12_GPU_DESCRIPTOR_HANDLE t2,
		D3D12_GPU_DESCRIPTOR_HANDLE t3,
		D3D12_GPU_DESCRIPTOR_HANDLE u0,
		D3D12_GPU_DESCRIPTOR_HANDLE u1,
		D3D12_GPU_DESCRIPTOR_HANDLE u2,
		D3D12_GPU_DESCRIPTOR_HANDLE u3,
		UINT frameIndex)
	{
		commandList->SetComputeRootDescriptorTable(0, t0);
		commandList->SetComputeRootDescriptorTable(1, t1);
		commandList->SetComputeRootDescriptorTable(2, t2);
		commandList->SetComputeRootDescriptorTable(3, t3);
		commandList->SetComputeRootDescriptorTable(4, u0);
		commandList->SetComputeRootDescriptorTable(5, u1);
		commandList->SetComputeRootDescriptorTable(6, u2);
		commandList->SetComputeRootDescriptorTable(7, u3);
		commandList->SetComputeRootConstantBufferView(
			8, g_Constants->GetGPUVirtualAddress() + frameIndex * kConstantStride);
	}

	void Transition(
		ID3D12GraphicsCommandList* commandList,
		ID3D12Resource* resource,
		D3D12_RESOURCE_STATES before,
		D3D12_RESOURCE_STATES after)
	{
		if (!resource || before == after) return;
		auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
		commandList->ResourceBarrier(1, &barrier);
	}

	void UavBarrier(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource)
	{
		auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(resource);
		commandList->ResourceBarrier(1, &barrier);
	}
}

bool ScreenSpaceEffects::Initialize(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT descriptorIncrement)
{
	Shutdown();
	if (!device || !heap || descriptorIncrement == 0) return false;
	g_Device = device;
	g_Heap = heap;
	g_DescriptorIncrement = descriptorIncrement;

	CD3DX12_DESCRIPTOR_RANGE ranges[8];
	for (UINT i = 0; i < 4; ++i) ranges[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, i);
	for (UINT i = 0; i < 4; ++i) ranges[4 + i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, i);
	CD3DX12_ROOT_PARAMETER parameters[9];
	for (UINT i = 0; i < 8; ++i) parameters[i].InitAsDescriptorTable(1, &ranges[i]);
	parameters[8].InitAsConstantBufferView(0);
	CD3DX12_STATIC_SAMPLER_DESC sampler(
		0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
	CD3DX12_ROOT_SIGNATURE_DESC rootDesc(
		_countof(parameters), parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);
	ComPtr<ID3DBlob> serialized;
	ComPtr<ID3DBlob> errors;
	if (FAILED(D3D12SerializeRootSignature(
		&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
	{
		if (errors) Debug::Log("Screen-space root signature: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
		return false;
	}
	if (FAILED(device->CreateRootSignature(
		0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&g_RootSignature))))
	{
		return false;
	}

	const bool pipelinesReady =
		LoadPipeline(L"shader/hlsl/build/ScreenSpaceDeinterleaveCS.cso", g_DeinterleavePso) &&
		LoadPipeline(L"shader/hlsl/build/RayBinningCS.cso", g_RayBinningPso) &&
		LoadPipeline(L"shader/hlsl/build/SsgiDeinterleavedCS.cso", g_SsgiPso) &&
		LoadPipeline(L"shader/hlsl/build/SsgiResolveCS.cso", g_ResolvePso) &&
		LoadPipeline(L"shader/hlsl/build/SsaoVisibilityBitmaskCS.cso", g_SsaoPso);
	if (!pipelinesReady) return false;

	auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto constantDesc = CD3DX12_RESOURCE_DESC::Buffer(kConstantStride * RendererState::g_kFRAME_COUNT);
	if (FAILED(device->CreateCommittedResource(
		&uploadHeap, D3D12_HEAP_FLAG_NONE, &constantDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_Constants))))
	{
		return false;
	}
	CD3DX12_RANGE readRange(0, 0);
	return SUCCEEDED(g_Constants->Map(0, &readRange, reinterpret_cast<void**>(&g_MappedConstants)));
}

void ScreenSpaceEffects::Shutdown()
{
	if (g_Constants && g_MappedConstants) g_Constants->Unmap(0, nullptr);
	g_MappedConstants = nullptr;
	g_Constants.Reset();
	g_RayOrder.Reset();
	g_DeGi.Reset();
	g_DeLight.Reset();
	g_DeNormal.Reset();
	g_DeDepth.Reset();
	g_History.Reset();
	g_Gi.Reset();
	g_Ao.Reset();
	g_SsaoPso.Reset();
	g_ResolvePso.Reset();
	g_SsgiPso.Reset();
	g_RayBinningPso.Reset();
	g_DeinterleavePso.Reset();
	g_RootSignature.Reset();
	g_Heap.Reset();
	g_Device.Reset();
	g_Width = g_Height = g_DeWidth = g_DeHeight = g_RayCount = 0;
	g_LightingFormat = DXGI_FORMAT_UNKNOWN;
	g_HistoryValid = false;
	g_RayOrderInitialized = false;
	g_DescriptorIncrement = 0;
}

bool ScreenSpaceEffects::Resize(UINT width, UINT height, DXGI_FORMAT lightingFormat)
{
	if (!g_Device || !g_Heap || width == 0 || height == 0) return false;
	if (g_Width == width && g_Height == height && g_LightingFormat == lightingFormat && g_Ao) return true;
	g_Width = width;
	g_Height = height;
	g_DeWidth = (width + 3u) / 4u;
	g_DeHeight = (height + 3u) / 4u;
	g_LightingFormat = lightingFormat;
	g_HistoryValid = false;
	g_RayOrderInitialized = false;
	g_RayCount = ((width + 7u) / 8u) * ((height + 7u) / 8u) * 64u;

	if (!CreateTexture(width, height, 1, DXGI_FORMAT_R16_FLOAT, g_Ao,
		RendererState::g_kSSAO_SRV_INDEX, RendererState::g_kSSAO_UAV_INDEX) ||
		!CreateTexture(width, height, 1, lightingFormat, g_Gi,
			RendererState::g_kSSGI_SRV_INDEX, RendererState::g_kSSGI_UAV_INDEX) ||
		!CreateTexture(g_DeWidth, g_DeHeight, 16, DXGI_FORMAT_R32_FLOAT, g_DeDepth,
			RendererState::g_kSS_DEINTERLEAVED_DEPTH_SRV_INDEX, RendererState::g_kSS_DEINTERLEAVED_DEPTH_UAV_INDEX) ||
		!CreateTexture(g_DeWidth, g_DeHeight, 16, DXGI_FORMAT_R16G16B16A16_FLOAT, g_DeNormal,
			RendererState::g_kSS_DEINTERLEAVED_NORMAL_SRV_INDEX, RendererState::g_kSS_DEINTERLEAVED_NORMAL_UAV_INDEX) ||
		!CreateTexture(g_DeWidth, g_DeHeight, 16, lightingFormat, g_DeLight,
			RendererState::g_kSS_DEINTERLEAVED_LIGHT_SRV_INDEX, RendererState::g_kSS_DEINTERLEAVED_LIGHT_UAV_INDEX) ||
		!CreateTexture(g_DeWidth, g_DeHeight, 16, lightingFormat, g_DeGi,
			RendererState::g_kSS_DEINTERLEAVED_GI_SRV_INDEX, RendererState::g_kSS_DEINTERLEAVED_GI_UAV_INDEX))
	{
		return false;
	}

	auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	// CopyResource requires identical subresource layouts.  Tex2D's default
	// MipLevels=0 creates a complete chain, while the lighting source has one mip.
	auto historyDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		lightingFormat, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_NONE);
	if (FAILED(g_Device->CreateCommittedResource(
		&defaultHeap, D3D12_HEAP_FLAG_NONE, &historyDesc,
		kShaderReadState, nullptr, IID_PPV_ARGS(&g_History))))
	{
		return false;
	}
	g_History->SetName(L"SSGI Previous Lighting History");
	D3D12_SHADER_RESOURCE_VIEW_DESC historySrv{};
	historySrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	historySrv.Format = lightingFormat;
	historySrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	historySrv.Texture2D.MipLevels = 1;
	g_Device->CreateShaderResourceView(g_History.Get(), &historySrv, Cpu(RendererState::g_kSS_HISTORY_SRV_INDEX));

	auto rayDesc = CD3DX12_RESOURCE_DESC::Buffer(
		static_cast<UINT64>(g_RayCount) * sizeof(uint32_t),
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	if (FAILED(g_Device->CreateCommittedResource(
		&defaultHeap, D3D12_HEAP_FLAG_NONE, &rayDesc,
		D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_RayOrder))))
	{
		return false;
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC raySrv{};
	raySrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	raySrv.Format = DXGI_FORMAT_UNKNOWN;
	raySrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	raySrv.Buffer.NumElements = g_RayCount;
	raySrv.Buffer.StructureByteStride = sizeof(uint32_t);
	g_Device->CreateShaderResourceView(g_RayOrder.Get(), &raySrv, Cpu(RendererState::g_kSS_RAY_ORDER_SRV_INDEX));
	D3D12_UNORDERED_ACCESS_VIEW_DESC rayUav{};
	rayUav.Format = DXGI_FORMAT_UNKNOWN;
	rayUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	rayUav.Buffer.NumElements = g_RayCount;
	rayUav.Buffer.StructureByteStride = sizeof(uint32_t);
	g_Device->CreateUnorderedAccessView(g_RayOrder.Get(), nullptr, &rayUav, Cpu(RendererState::g_kSS_RAY_ORDER_UAV_INDEX));
	return true;
}

bool ScreenSpaceEffects::IsAvailable()
{
	return g_RootSignature && g_DeinterleavePso && g_RayBinningPso &&
		g_SsgiPso && g_ResolvePso && g_SsaoPso && g_Ao && g_Gi && g_History;
}

void ScreenSpaceEffects::Execute(
	ID3D12GraphicsCommandList* commandList,
	ID3D12Resource* depth,
	ID3D12Resource* normal,
	D3D12_GPU_DESCRIPTOR_HANDLE depthSrv,
	D3D12_GPU_DESCRIPTOR_HANDLE normalSrv,
	UINT frameIndex)
{
	if (!commandList || !depth || !normal || !IsAvailable()) return;
	frameIndex %= RendererState::g_kFRAME_COUNT;
	Constants constants{};
	XMMATRIX view = XMMatrixIdentity();
	XMMATRIX projection = XMMatrixIdentity();
	Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
	XMStoreFloat4x4(&constants.InvViewProjection, XMMatrixTranspose(XMMatrixInverse(nullptr, view * projection)));
	constants.FullExtent = XMUINT2(g_Width, g_Height);
	constants.DeinterleavedExtent = XMUINT2(g_DeWidth, g_DeHeight);
	constants.EffectParams = XMFLOAT4(
		RendererSettings::GetSsaoRadius(),
		RendererSettings::GetSsaoPower(),
		RendererSettings::GetSsgiIntensity(),
		g_HistoryValid ? 1.0f : 0.0f);
	constants.FeatureFlags = XMUINT4(
		RendererSettings::GetRayBinningEnabled() ? 1u : 0u, 0u, 0u, 0u);
	memcpy(g_MappedConstants + frameIndex * kConstantStride, &constants, sizeof(constants));

	ID3D12DescriptorHeap* heaps[] = { g_Heap.Get() };
	commandList->SetDescriptorHeaps(1, heaps);
	commandList->SetComputeRootSignature(g_RootSignature.Get());
	Transition(commandList, depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, kShaderReadState);
	Transition(commandList, normal, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, kShaderReadState);

	const auto deDepthSrv = Gpu(RendererState::g_kSS_DEINTERLEAVED_DEPTH_SRV_INDEX);
	const auto deNormalSrv = Gpu(RendererState::g_kSS_DEINTERLEAVED_NORMAL_SRV_INDEX);
	const auto deLightSrv = Gpu(RendererState::g_kSS_DEINTERLEAVED_LIGHT_SRV_INDEX);
	const auto deGiSrv = Gpu(RendererState::g_kSS_DEINTERLEAVED_GI_SRV_INDEX);
	const auto raySrv = Gpu(RendererState::g_kSS_RAY_ORDER_SRV_INDEX);
	const auto deDepthUav = Gpu(RendererState::g_kSS_DEINTERLEAVED_DEPTH_UAV_INDEX);
	const auto deNormalUav = Gpu(RendererState::g_kSS_DEINTERLEAVED_NORMAL_UAV_INDEX);
	const auto deLightUav = Gpu(RendererState::g_kSS_DEINTERLEAVED_LIGHT_UAV_INDEX);
	const auto deGiUav = Gpu(RendererState::g_kSS_DEINTERLEAVED_GI_UAV_INDEX);

	if (RendererSettings::GetSsaoEnabled())
	{
		RenderProfiler::ScopedEvent profile("SSAO Visibility Bitmask", commandList);
		Transition(commandList, g_Ao.Get(), kShaderReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList->SetPipelineState(g_SsaoPso.Get());
		BindCommon(commandList, depthSrv, normalSrv, GetFallbackSrv(), GetFallbackSrv(),
			Gpu(RendererState::g_kSSAO_UAV_INDEX), deNormalUav, deLightUav, deGiUav, frameIndex);
		commandList->Dispatch((g_Width + 7u) / 8u, (g_Height + 7u) / 8u, 1);
		Transition(commandList, g_Ao.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderReadState);
	}

	if (RendererSettings::GetSsgiEnabled())
	{
		RenderProfiler::ScopedEvent profile("SSGI Deinterleaved + Ray Binning", commandList);
		Transition(commandList, g_DeDepth.Get(), kShaderReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		Transition(commandList, g_DeNormal.Get(), kShaderReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		Transition(commandList, g_DeLight.Get(), kShaderReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList->SetPipelineState(g_DeinterleavePso.Get());
		BindCommon(commandList, depthSrv, normalSrv, Gpu(RendererState::g_kSS_HISTORY_SRV_INDEX), GetFallbackSrv(),
			deDepthUav, deNormalUav, deLightUav, deGiUav, frameIndex);
		commandList->Dispatch((g_DeWidth + 7u) / 8u, (g_DeHeight + 7u) / 8u, 16);
		Transition(commandList, g_DeDepth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderReadState);
		Transition(commandList, g_DeNormal.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderReadState);
		Transition(commandList, g_DeLight.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderReadState);

		Transition(commandList, g_RayOrder.Get(), g_RayOrderInitialized
			? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		g_RayOrderInitialized = true;
		commandList->SetPipelineState(g_RayBinningPso.Get());
		BindCommon(commandList, deDepthSrv, deNormalSrv, deLightSrv, deGiSrv,
			Gpu(RendererState::g_kSS_RAY_ORDER_UAV_INDEX), deNormalUav, deLightUav, deGiUav, frameIndex);
		const UINT tileCount = ((g_Width + 7u) / 8u) * ((g_Height + 7u) / 8u);
		commandList->Dispatch(tileCount, 1, 1);
		Transition(commandList, g_RayOrder.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		Transition(commandList, g_DeGi.Get(), kShaderReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList->SetPipelineState(g_SsgiPso.Get());
		BindCommon(commandList, deDepthSrv, deNormalSrv, deLightSrv, raySrv,
			deGiUav, deNormalUav, deLightUav, deDepthUav, frameIndex);
		commandList->Dispatch((g_RayCount + 63u) / 64u, 1, 1);
		Transition(commandList, g_DeGi.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderReadState);

		Transition(commandList, g_Gi.Get(), kShaderReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList->SetPipelineState(g_ResolvePso.Get());
		BindCommon(commandList, deGiSrv, deNormalSrv, deLightSrv, raySrv,
			Gpu(RendererState::g_kSSGI_UAV_INDEX), deNormalUav, deLightUav, deDepthUav, frameIndex);
		commandList->Dispatch((g_Width + 7u) / 8u, (g_Height + 7u) / 8u, 1);
		Transition(commandList, g_Gi.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderReadState);
	}

	Transition(commandList, depth, kShaderReadState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	Transition(commandList, normal, kShaderReadState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void ScreenSpaceEffects::CaptureHistory(ID3D12GraphicsCommandList* commandList, ID3D12Resource* lighting)
{
	if (!commandList || !lighting || !g_History) return;
	Transition(commandList, lighting, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Transition(commandList, g_History.Get(), kShaderReadState, D3D12_RESOURCE_STATE_COPY_DEST);
	commandList->CopyResource(g_History.Get(), lighting);
	Transition(commandList, g_History.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kShaderReadState);
	Transition(commandList, lighting, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	g_HistoryValid = true;
}

D3D12_GPU_DESCRIPTOR_HANDLE ScreenSpaceEffects::GetAoSrv()
{
	return Gpu(RendererState::g_kSSAO_SRV_INDEX);
}

D3D12_GPU_DESCRIPTOR_HANDLE ScreenSpaceEffects::GetGiSrv()
{
	return Gpu(RendererState::g_kSSGI_SRV_INDEX);
}

D3D12_GPU_DESCRIPTOR_HANDLE ScreenSpaceEffects::GetFallbackSrv()
{
	return Gpu(RendererState::g_kSS_HISTORY_SRV_INDEX);
}
