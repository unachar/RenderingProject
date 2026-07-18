#pragma once

#include "pch.h"
#include "staticmodel.h"

class MeshShaderPipeline
{
public:
	static bool Initialize(ID3D12Device* device, ID3D12RootSignature* rootSignature);
	static void Shutdown();
	static bool IsSupported();
	static bool Draw(
		ID3D12GraphicsCommandList* commandList,
		const StaticMeshData& mesh,
		const XMFLOAT3& localCenter,
		const XMFLOAT3& localExtents);
};
