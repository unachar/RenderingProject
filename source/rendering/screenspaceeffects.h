#pragma once

#include "pch.h"

class ScreenSpaceEffects
{
public:
	static bool Initialize(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT descriptorIncrement);
	static void Shutdown();
	static bool Resize(UINT width, UINT height, DXGI_FORMAT lightingFormat);
	static bool IsAvailable();
	static void Execute(
		ID3D12GraphicsCommandList* commandList,
		ID3D12Resource* depth,
		ID3D12Resource* normal,
		D3D12_GPU_DESCRIPTOR_HANDLE depthSrv,
		D3D12_GPU_DESCRIPTOR_HANDLE normalSrv,
		UINT frameIndex);
	static void CaptureHistory(ID3D12GraphicsCommandList* commandList, ID3D12Resource* lighting);
	static D3D12_GPU_DESCRIPTOR_HANDLE GetAoSrv();
	static D3D12_GPU_DESCRIPTOR_HANDLE GetGiSrv();
	static D3D12_GPU_DESCRIPTOR_HANDLE GetFallbackSrv();
};
