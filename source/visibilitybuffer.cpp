#include "pch.h"
#include "visibilitybuffer.h"
#include "rendererstate.h"

namespace
{
	ComPtr<ID3D12Device> g_VisibilityDevice;
	ComPtr<ID3D12DescriptorHeap> g_VisibilityDescriptorHeap;
	ComPtr<ID3D12RootSignature> g_VisibilityRootSignature;
	ComPtr<ID3D12PipelineState> g_VisibilityPso;
	UINT g_VisibilityDescriptorIncrement = 0;
}

bool VisibilityBuffer::Initialize(
	ID3D12Device* device,
	ID3D12DescriptorHeap* descriptorHeap,
	UINT descriptorIncrement)
{
	Shutdown();
	if (!device || !descriptorHeap || descriptorIncrement == 0) return false;
	g_VisibilityDevice = device;
	g_VisibilityDescriptorHeap = descriptorHeap;
	g_VisibilityDescriptorIncrement = descriptorIncrement;

	CD3DX12_DESCRIPTOR_RANGE ranges[2];
	ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
	ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, RendererState::g_kGEOMETRY_GBUFFER_COUNT, 0);
	CD3DX12_ROOT_PARAMETER parameters[3];
	parameters[0].InitAsDescriptorTable(1, &ranges[0]);
	parameters[1].InitAsDescriptorTable(1, &ranges[1]);
	parameters[2].InitAsConstants(4, 0);
	CD3DX12_ROOT_SIGNATURE_DESC rootDesc(
		_countof(parameters), parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
	ComPtr<ID3DBlob> serialized;
	ComPtr<ID3DBlob> errors;
	HRESULT hr = D3D12SerializeRootSignature(
		&rootDesc,
		D3D_ROOT_SIGNATURE_VERSION_1,
		&serialized,
		&errors);
	if (FAILED(hr))
	{
		if (errors) Debug::Log("Visibility Buffer root signature: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
		return false;
	}
	hr = device->CreateRootSignature(
		0,
		serialized->GetBufferPointer(),
		serialized->GetBufferSize(),
		IID_PPV_ARGS(&g_VisibilityRootSignature));
	if (FAILED(hr)) return false;

	ComPtr<ID3DBlob> shader;
	if (FAILED(D3DReadFileToBlob(L"shader/hlsl/build/GBufferFromVisibilityCS.cso", &shader)))
	{
		Debug::Log("GBufferFromVisibilityCS.cso is missing.\n");
		return false;
	}
	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
	psoDesc.pRootSignature = g_VisibilityRootSignature.Get();
	psoDesc.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
	return SUCCEEDED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&g_VisibilityPso)));
}

void VisibilityBuffer::Shutdown()
{
	g_VisibilityPso.Reset();
	g_VisibilityRootSignature.Reset();
	g_VisibilityDescriptorHeap.Reset();
	g_VisibilityDevice.Reset();
	g_VisibilityDescriptorIncrement = 0;
}

bool VisibilityBuffer::IsAvailable()
{
	return g_VisibilityPso && g_VisibilityRootSignature && g_VisibilityDescriptorHeap;
}

void VisibilityBuffer::GenerateGBuffer(
	ID3D12GraphicsCommandList* commandList,
	D3D12_GPU_DESCRIPTOR_HANDLE visibilitySrv,
	UINT width,
	UINT height)
{
	if (!commandList || !IsAvailable() || width == 0 || height == 0) return;
	ID3D12DescriptorHeap* heaps[] = { g_VisibilityDescriptorHeap.Get() };
	commandList->SetDescriptorHeaps(_countof(heaps), heaps);
	commandList->SetComputeRootSignature(g_VisibilityRootSignature.Get());
	commandList->SetPipelineState(g_VisibilityPso.Get());
	commandList->SetComputeRootDescriptorTable(0, visibilitySrv);
	commandList->SetComputeRootDescriptorTable(
		1,
		CD3DX12_GPU_DESCRIPTOR_HANDLE(
			g_VisibilityDescriptorHeap->GetGPUDescriptorHandleForHeapStart(),
			RendererState::g_kGBUFFER_UAV_START_INDEX,
			g_VisibilityDescriptorIncrement));
	const UINT constants[4] = { width, height, 0, 0 };
	commandList->SetComputeRoot32BitConstants(2, 4, constants, 0);
	commandList->Dispatch((width + 7u) / 8u, (height + 3u) / 4u, 1);
}
