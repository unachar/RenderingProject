#pragma once

#include "pch.h"

class LightInstanceBuilder
{
public:
	struct Input
	{
		XMUINT4 TileBounds{};
		XMUINT4 Metadata{};
	};

	static bool Initialize(ID3D12Device* device);
	static void Shutdown();
	static bool IsAvailable();
	static bool Build(
		ID3D12GraphicsCommandList* commandList,
		UINT frameIndex,
		const vector<Input>& inputs,
		UINT tileCountX,
		UINT tileCountY,
		UINT slotsPerTile);
	static D3D12_GPU_VIRTUAL_ADDRESS GetTileIndexAddress(UINT frameIndex);
	static D3D12_GPU_VIRTUAL_ADDRESS GetVolumetricIndexAddress(UINT frameIndex);
};
