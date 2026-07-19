#include "pch.h"
#include "occlusionculling.h"
#include "rendererstate.h"
#include "renderersettings.h"
#include "renderprofiler.h"


	static ComPtr<ID3D12Device> g_Device;
	static ComPtr<ID3D12DescriptorHeap> g_Heap;
	static UINT g_Increment = 0;
	static ComPtr<ID3D12RootSignature> g_RootSignature;
	static ComPtr<ID3D12PipelineState> g_ReducePso;
	static ComPtr<ID3D12Resource> g_HiZ[2];
	static UINT g_Width = 0;
	static UINT g_Height = 0;
	static UINT g_MipCount = 0;
	static UINT g_PreviousIndex = 0;
	static UINT g_CurrentIndex = 1;
	static UINT g_Phase = 0;
	static bool g_PreviousValid = false;
	static bool g_CurrentValid = false;

	static D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT index)
	{
		return CD3DX12_CPU_DESCRIPTOR_HANDLE(g_Heap->GetCPUDescriptorHandleForHeapStart(), index, g_Increment);
	}
	static D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT index)
	{
		return CD3DX12_GPU_DESCRIPTOR_HANDLE(g_Heap->GetGPUDescriptorHandleForHeapStart(), index, g_Increment);
	}
	static bool LoadPipeline(const wchar_t* path, ComPtr<ID3D12PipelineState>& output)
	{
		ComPtr<ID3DBlob> shader;
		if (FAILED(D3DReadFileToBlob(path, &shader))) return false;
		D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
		desc.pRootSignature = g_RootSignature.Get();
		desc.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
		return SUCCEEDED(g_Device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&output)));
	}


bool OcclusionCulling::Initialize(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT descriptorIncrement)
{
	Shutdown();
	if (!device || !heap || descriptorIncrement == 0) return false;
	g_Device = device;
	g_Heap = heap;
	g_Increment = descriptorIncrement;
	CD3DX12_DESCRIPTOR_RANGE ranges[2];
	ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
	ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
	CD3DX12_ROOT_PARAMETER parameters[3];
	parameters[0].InitAsDescriptorTable(1, &ranges[0]);
	parameters[1].InitAsDescriptorTable(1, &ranges[1]);
	parameters[2].InitAsConstants(4, 0);
	CD3DX12_ROOT_SIGNATURE_DESC desc(_countof(parameters), parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
	ComPtr<ID3DBlob> serialized;
	ComPtr<ID3DBlob> errors;
	if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
	{
		if (errors) Debug::Log("Hi-Z root signature: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
		return false;
	}
	if (FAILED(device->CreateRootSignature(
		0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&g_RootSignature)))) return false;
	return LoadPipeline(L"shader/hlsl/build/HiZReduceCS.cso", g_ReducePso);
}

void OcclusionCulling::Shutdown()
{
	g_HiZ[0].Reset();
	g_HiZ[1].Reset();
	g_ReducePso.Reset();
	g_RootSignature.Reset();
	g_Heap.Reset();
	g_Device.Reset();
	g_Increment = g_Width = g_Height = g_MipCount = g_Phase = 0;
	g_PreviousIndex = 0;
	g_CurrentIndex = 1;
	g_PreviousValid = g_CurrentValid = false;
}

bool OcclusionCulling::Resize(UINT width, UINT height)
{
	if (!g_Device || !g_Heap || width == 0 || height == 0) return false;
	if (width == g_Width && height == g_Height && g_HiZ[0] && g_HiZ[1]) return true;
	g_Width = width;
	g_Height = height;
	g_MipCount = min<UINT>(
		1u + static_cast<UINT>(floor(log2(static_cast<double>(max(width, height))))),
		RendererState::g_kHIZ_MAX_MIPS);
	g_PreviousValid = g_CurrentValid = false;
	g_PreviousIndex = 0;
	g_CurrentIndex = 1;
	auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_R32_FLOAT, width, height, 1, static_cast<UINT16>(g_MipCount), 1, 0,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	for (UINT texture = 0; texture < 2; ++texture)
	{
		g_HiZ[texture].Reset();
		if (FAILED(g_Device->CreateCommittedResource(
			&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
			IID_PPV_ARGS(&g_HiZ[texture])))) return false;
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Format = DXGI_FORMAT_R32_FLOAT;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MipLevels = g_MipCount;
		g_Device->CreateShaderResourceView(
			g_HiZ[texture].Get(), &srv,
			Cpu(RendererState::g_kHIZ_SRV_START_INDEX + texture));
		for (UINT mip = 0; mip < g_MipCount; ++mip)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC mipSrv = srv;
			mipSrv.Texture2D.MostDetailedMip = mip;
			mipSrv.Texture2D.MipLevels = 1;
			g_Device->CreateShaderResourceView(
				g_HiZ[texture].Get(), &mipSrv,
				Cpu(RendererState::g_kHIZ_MIP_SRV_START_INDEX +
					texture * RendererState::g_kHIZ_MAX_MIPS + mip));
			D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = DXGI_FORMAT_R32_FLOAT;
			uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			uav.Texture2D.MipSlice = mip;
			g_Device->CreateUnorderedAccessView(
				g_HiZ[texture].Get(), nullptr, &uav,
				Cpu(RendererState::g_kHIZ_UAV_START_INDEX + texture * RendererState::g_kHIZ_MAX_MIPS + mip));
		}
	}
	return true;
}

bool OcclusionCulling::IsAvailable()
{
	return g_RootSignature && g_ReducePso && g_HiZ[0] && g_HiZ[1];
}

void OcclusionCulling::BeginPhaseOne()
{
	g_Phase = RendererSettings::GetTwoPhaseOcclusionEnabled() && IsAvailable() ? 1u : 0u;
	g_CurrentValid = false;
}

void OcclusionCulling::BuildCurrent(
	ID3D12GraphicsCommandList* commandList,
	ID3D12Resource* sourceDepth)
{
	if (!commandList || !sourceDepth || g_Phase != 1u || !IsAvailable()) return;
	RenderProfiler::ScopedEvent profile("Hi-Z Build", commandList);
	ID3D12DescriptorHeap* heaps[] = { g_Heap.Get() };
	commandList->SetDescriptorHeaps(1, heaps);
	commandList->SetComputeRootSignature(g_RootSignature.Get());



	auto mipZeroToCopy = CD3DX12_RESOURCE_BARRIER::Transition(
		g_HiZ[g_CurrentIndex].Get(),
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_COPY_DEST,
		0);
	commandList->ResourceBarrier(1, &mipZeroToCopy);
	CD3DX12_TEXTURE_COPY_LOCATION sourceLocation(sourceDepth, 0);
	CD3DX12_TEXTURE_COPY_LOCATION destinationLocation(g_HiZ[g_CurrentIndex].Get(), 0);
	commandList->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);
	auto mipZeroToRead = CD3DX12_RESOURCE_BARRIER::Transition(
		g_HiZ[g_CurrentIndex].Get(),
		D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		0);
	commandList->ResourceBarrier(1, &mipZeroToRead);

	UINT width = max(g_Width / 2u, 1u);
	UINT height = max(g_Height / 2u, 1u);
	for (UINT mip = 1; mip < g_MipCount; ++mip)
	{
		auto toUav = CD3DX12_RESOURCE_BARRIER::Transition(
			g_HiZ[g_CurrentIndex].Get(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			mip);
		commandList->ResourceBarrier(1, &toUav);
		commandList->SetPipelineState(g_ReducePso.Get());
		commandList->SetComputeRootDescriptorTable(
			0, Gpu(RendererState::g_kHIZ_MIP_SRV_START_INDEX +
				g_CurrentIndex * RendererState::g_kHIZ_MAX_MIPS + mip - 1));
		commandList->SetComputeRootDescriptorTable(
			1, Gpu(RendererState::g_kHIZ_UAV_START_INDEX +
				g_CurrentIndex * RendererState::g_kHIZ_MAX_MIPS + mip));


		const UINT constants[4] = { width, height, 0u, 1u };
		commandList->SetComputeRoot32BitConstants(2, 4, constants, 0);
		commandList->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1);
		auto toRead = CD3DX12_RESOURCE_BARRIER::Transition(
			g_HiZ[g_CurrentIndex].Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			mip);
		commandList->ResourceBarrier(1, &toRead);
		width = max(width / 2u, 1u);
		height = max(height / 2u, 1u);
	}
	g_CurrentValid = true;
}

void OcclusionCulling::BeginPhaseTwo()
{
	if (g_Phase == 1u && g_CurrentValid) g_Phase = 2u;
}

void OcclusionCulling::EndFrame()
{
	if (g_CurrentValid)
	{
		g_PreviousIndex = g_CurrentIndex;
		g_CurrentIndex = 1u - g_CurrentIndex;
		g_PreviousValid = true;
	}
	g_CurrentValid = false;
	g_Phase = 0;
}

UINT OcclusionCulling::GetPhase() { return g_Phase; }
bool OcclusionCulling::HasHierarchyForCurrentPhase()
{
	return g_Phase == 1u ? g_PreviousValid : (g_Phase == 2u && g_CurrentValid);
}
D3D12_GPU_DESCRIPTOR_HANDLE OcclusionCulling::GetHierarchySrv()
{
	const UINT index = g_Phase == 2u ? g_CurrentIndex : g_PreviousIndex;
	return Gpu(RendererState::g_kHIZ_SRV_START_INDEX + index);
}
D3D12_GPU_DESCRIPTOR_HANDLE OcclusionCulling::GetPreviousSrv()
{
	return Gpu(RendererState::g_kHIZ_SRV_START_INDEX + g_PreviousIndex);
}
D3D12_GPU_DESCRIPTOR_HANDLE OcclusionCulling::GetCurrentSrv()
{
	return Gpu(RendererState::g_kHIZ_SRV_START_INDEX + g_CurrentIndex);
}
bool OcclusionCulling::HasPrevious() { return g_PreviousValid; }
bool OcclusionCulling::HasCurrent() { return g_CurrentValid; }
UINT OcclusionCulling::GetWidth() { return g_Width; }
UINT OcclusionCulling::GetHeight() { return g_Height; }
UINT OcclusionCulling::GetMipCount() { return g_MipCount; }
