#include "pch.h"
#include "lightinstancebuilder.h"
#include "rendererstate.h"
#include "renderprofiler.h"


	static ComPtr<ID3D12Device> g_Device;
	static ComPtr<ID3D12RootSignature> g_RootSignature;
	static ComPtr<ID3D12PipelineState> g_PipelineState;
	static ComPtr<ID3D12Resource> g_Input[RendererState::g_kFRAME_COUNT];
	static ComPtr<ID3D12Resource> g_TileIndices[RendererState::g_kFRAME_COUNT];
	static ComPtr<ID3D12Resource> g_VolumetricIndices[RendererState::g_kFRAME_COUNT];
	static ComPtr<ID3D12Resource> g_Counters[RendererState::g_kFRAME_COUNT];
	LightInstanceBuilder::Input* g_MappedInput[RendererState::g_kFRAME_COUNT]{};
	static bool g_OutputInShaderRead[RendererState::g_kFRAME_COUNT]{};

	bool CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
		D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& output)
	{
		auto heap = CD3DX12_HEAP_PROPERTIES(heapType);
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(size, flags);
		return SUCCEEDED(g_Device->CreateCommittedResource(
			&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&output)));
	}


bool LightInstanceBuilder::Initialize(ID3D12Device* device)
{
	Shutdown();
	if (!device) return false;
	g_Device = device;
	CD3DX12_ROOT_PARAMETER parameters[5];
	parameters[0].InitAsShaderResourceView(0);
	parameters[1].InitAsUnorderedAccessView(0);
	parameters[2].InitAsUnorderedAccessView(1);
	parameters[3].InitAsUnorderedAccessView(2);
	parameters[4].InitAsConstants(8, 0);
	CD3DX12_ROOT_SIGNATURE_DESC rootDesc(_countof(parameters), parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
	ComPtr<ID3DBlob> serialized;
	ComPtr<ID3DBlob> errors;
	if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
		FAILED(device->CreateRootSignature(
			0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&g_RootSignature))))
	{
		return false;
	}
	ComPtr<ID3DBlob> shader;
	if (FAILED(D3DReadFileToBlob(L"shader/hlsl/build/LightInstanceBuilderCS.cso", &shader))) return false;
	D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
	pso.pRootSignature = g_RootSignature.Get();
	pso.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
	if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_PipelineState)))) return false;

	const UINT64 inputSize = sizeof(Input) * RendererState::g_kMAX_SHADER_LIGHTS;
	const UINT64 tileSize = static_cast<UINT64>(RendererState::g_kMAX_LIGHT_TILE_COUNT) *
		RendererState::g_kMAX_LIGHTS_PER_TILE * sizeof(uint32_t);
	const UINT64 volumetricSize = RendererState::g_kMAX_SHADER_LIGHTS * sizeof(uint32_t);
	for (UINT frame = 0; frame < RendererState::g_kFRAME_COUNT; ++frame)
	{
		if (!CreateBuffer(inputSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
			D3D12_RESOURCE_STATE_GENERIC_READ, g_Input[frame]) ||
			!CreateBuffer(tileSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_COMMON, g_TileIndices[frame]) ||
			!CreateBuffer(volumetricSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_COMMON, g_VolumetricIndices[frame]) ||
			!CreateBuffer(sizeof(uint32_t), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_COMMON, g_Counters[frame])) return false;
		CD3DX12_RANGE noRead(0, 0);
		if (FAILED(g_Input[frame]->Map(0, &noRead, reinterpret_cast<void**>(&g_MappedInput[frame])))) return false;
	}
	return true;
}

void LightInstanceBuilder::Shutdown()
{
	for (UINT frame = 0; frame < RendererState::g_kFRAME_COUNT; ++frame)
	{
		if (g_Input[frame] && g_MappedInput[frame]) g_Input[frame]->Unmap(0, nullptr);
		g_MappedInput[frame] = nullptr;
		g_Input[frame].Reset();
		g_TileIndices[frame].Reset();
		g_VolumetricIndices[frame].Reset();
		g_Counters[frame].Reset();
		g_OutputInShaderRead[frame] = false;
	}
	g_PipelineState.Reset();
	g_RootSignature.Reset();
	g_Device.Reset();
}

bool LightInstanceBuilder::IsAvailable()
{
	return g_Device && g_RootSignature && g_PipelineState;
}

bool LightInstanceBuilder::Build(
	ID3D12GraphicsCommandList* commandList,
	UINT frameIndex,
	const vector<Input>& inputs,
	UINT tileCountX,
	UINT tileCountY,
	UINT slotsPerTile)
{
	if (!commandList || !IsAvailable() || tileCountX == 0 || tileCountY == 0 ||
		tileCountX * tileCountY > RendererState::g_kMAX_LIGHT_TILE_COUNT) return false;
	frameIndex %= RendererState::g_kFRAME_COUNT;
	const UINT lightCount = min(static_cast<UINT>(inputs.size()), RendererState::g_kMAX_SHADER_LIGHTS);
	if (lightCount > 0) memcpy(g_MappedInput[frameIndex], inputs.data(), lightCount * sizeof(Input));
	if (g_OutputInShaderRead[frameIndex])
	{
		D3D12_RESOURCE_BARRIER barriers[2] =
		{
			CD3DX12_RESOURCE_BARRIER::Transition(g_TileIndices[frameIndex].Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			CD3DX12_RESOURCE_BARRIER::Transition(g_VolumetricIndices[frameIndex].Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
		};
		commandList->ResourceBarrier(2, barriers);
	}
	commandList->SetComputeRootSignature(g_RootSignature.Get());
	commandList->SetPipelineState(g_PipelineState.Get());
	commandList->SetComputeRootShaderResourceView(0, g_Input[frameIndex]->GetGPUVirtualAddress());
	commandList->SetComputeRootUnorderedAccessView(1, g_TileIndices[frameIndex]->GetGPUVirtualAddress());
	commandList->SetComputeRootUnorderedAccessView(2, g_VolumetricIndices[frameIndex]->GetGPUVirtualAddress());
	commandList->SetComputeRootUnorderedAccessView(3, g_Counters[frameIndex]->GetGPUVirtualAddress());
	const UINT clearConstants[8] = { lightCount, tileCountX, tileCountY, slotsPerTile, 0u,
		RendererState::g_kMAX_SHADER_LIGHTS, RendererState::g_kMAX_LIGHTS_PER_TILE, 0u };
	commandList->SetComputeRoot32BitConstants(4, 8, clearConstants, 0);
	const UINT clearCount = max(
		tileCountX * tileCountY * RendererState::g_kMAX_LIGHTS_PER_TILE,
		RendererState::g_kMAX_SHADER_LIGHTS);
	commandList->Dispatch((clearCount + 63u) / 64u, 1, 1);
	D3D12_RESOURCE_BARRIER uavBarriers[3] =
	{
		CD3DX12_RESOURCE_BARRIER::UAV(g_TileIndices[frameIndex].Get()),
		CD3DX12_RESOURCE_BARRIER::UAV(g_VolumetricIndices[frameIndex].Get()),
		CD3DX12_RESOURCE_BARRIER::UAV(g_Counters[frameIndex].Get())
	};
	commandList->ResourceBarrier(3, uavBarriers);
	UINT buildConstants[8];
	memcpy(buildConstants, clearConstants, sizeof(buildConstants));
	buildConstants[4] = 1u;
	commandList->SetComputeRoot32BitConstants(4, 8, buildConstants, 0);
	if (lightCount > 0) commandList->Dispatch((lightCount + 63u) / 64u, 1, 1);
	D3D12_RESOURCE_BARRIER toRead[2] =
	{
		CD3DX12_RESOURCE_BARRIER::Transition(g_TileIndices[frameIndex].Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
		CD3DX12_RESOURCE_BARRIER::Transition(g_VolumetricIndices[frameIndex].Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
	};
	commandList->ResourceBarrier(2, toRead);
	g_OutputInShaderRead[frameIndex] = true;
	return true;
}

D3D12_GPU_VIRTUAL_ADDRESS LightInstanceBuilder::GetTileIndexAddress(UINT frameIndex)
{
	frameIndex %= RendererState::g_kFRAME_COUNT;
	return g_TileIndices[frameIndex] ? g_TileIndices[frameIndex]->GetGPUVirtualAddress() : 0;
}

D3D12_GPU_VIRTUAL_ADDRESS LightInstanceBuilder::GetVolumetricIndexAddress(UINT frameIndex)
{
	frameIndex %= RendererState::g_kFRAME_COUNT;
	return g_VolumetricIndices[frameIndex] ? g_VolumetricIndices[frameIndex]->GetGPUVirtualAddress() : 0;
}
