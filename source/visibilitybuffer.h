#pragma once

#include "pch.h"

class VisibilityBuffer
{
public:
	static bool Initialize(
		ID3D12Device* device,
		ID3D12DescriptorHeap* descriptorHeap,
		UINT descriptorIncrement);
	static void Shutdown();
	static bool IsAvailable();
	static void GenerateGBuffer(
		ID3D12GraphicsCommandList* commandList,
		D3D12_GPU_DESCRIPTOR_HANDLE visibilitySrv,
		UINT width,
		UINT height);
};
