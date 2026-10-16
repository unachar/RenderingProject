#pragma once

#include "pch.h"
#include "staticmodel.h"

class MeshShaderPipeline
{
public:
	struct Geometry
	{
		ID3D12Resource* Vertices = nullptr;
		ID3D12Resource* Indices = nullptr;
		D3D12_VERTEX_BUFFER_VIEW VertexView{};
		D3D12_INDEX_BUFFER_VIEW IndexView{};
		UINT ElementCount = 0;
		D3D12_RESOURCE_STATES VertexState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
		D3D12_RESOURCE_STATES IndexState = D3D12_RESOURCE_STATE_INDEX_BUFFER;
	};
	struct Statistics
	{
		UINT Draws = 0;
		UINT Pages = 0;
		UINT Meshlets = 0;
		UINT MaxGroupsPerPage = 0;
	};
	static bool Initialize(ID3D12Device* device, ID3D12RootSignature* rootSignature);
	static void Shutdown();
	static bool IsSupported();
	static bool IsAvailable();
	static void BeginFrame();
	static const Statistics& GetStatistics();
	static bool Draw(ID3D12GraphicsCommandList* commandList, const Geometry& geometry,
		const XMFLOAT3& localCenter, const XMFLOAT3& localExtents);
	static bool Draw(
		ID3D12GraphicsCommandList* commandList,
		const StaticMeshData& mesh,
		const XMFLOAT3& localCenter,
		const XMFLOAT3& localExtents);
};

