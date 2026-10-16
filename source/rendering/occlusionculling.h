#pragma once

#include "pch.h"

class OcclusionCulling
{
public:
	static bool Initialize(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT descriptorIncrement);
	static void Shutdown();
	static bool Resize(UINT width, UINT height);
	static bool IsAvailable();
	static void BeginPhaseOne();
	static void BuildCurrent(
		ID3D12GraphicsCommandList* commandList,
		ID3D12Resource* sourceDepth);
	static void BeginPhaseTwo();
	static void BuildTemporal(ID3D12GraphicsCommandList* commands, ID3D12Resource* depth, D3D12_GPU_DESCRIPTOR_HANDLE depthSrv);
	static void InvalidateHistory();
	static ID3D12Resource* GetPreviousResource();
	static ID3D12Resource* GetCurrentResource();
	static D3D12_GPU_DESCRIPTOR_HANDLE GetDebugMipSrv(UINT mip);
	static void SetDebugPreview(UINT mip, bool enabled);
	static ID3D12Resource* GetOccluderDepth();
	static D3D12_CPU_DESCRIPTOR_HANDLE GetOccluderDsv();
	static D3D12_GPU_DESCRIPTOR_HANDLE GetOccluderSrv();
	static constexpr D3D12_RESOURCE_STATES ReadState() { return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; }
	static void EndFrame();
	static UINT GetPhase();
	static D3D12_GPU_DESCRIPTOR_HANDLE GetPreviousSrv();
	static D3D12_GPU_DESCRIPTOR_HANDLE GetCurrentSrv();
	static bool HasPrevious();
	static bool HasCurrent();
	static UINT GetWidth();
	static UINT GetHeight();
	static UINT GetMipCount();
private:
	static void BuildPyramid(ID3D12GraphicsCommandList* commands, ID3D12Resource* depth,
		D3D12_GPU_DESCRIPTOR_HANDLE srv, D3D12_RESOURCE_STATES depthState);
};

