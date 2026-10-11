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
	static void EndFrame();
	static UINT GetPhase();
	static bool HasHierarchyForCurrentPhase();
	static D3D12_GPU_DESCRIPTOR_HANDLE GetHierarchySrv();
	static D3D12_GPU_DESCRIPTOR_HANDLE GetPreviousSrv();
	static D3D12_GPU_DESCRIPTOR_HANDLE GetCurrentSrv();
	static bool HasPrevious();
	static bool HasCurrent();
	static UINT GetWidth();
	static UINT GetHeight();
	static UINT GetMipCount();
};
