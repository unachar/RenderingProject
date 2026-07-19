#include "pch.h"
#include "grid.h"
#include "rendererdraw.h"
#include "psomanager.h"

#include <d3dcompiler.h>
#include <vector>

using namespace DirectX;


	struct GridVertex
	{
		XMFLOAT3 Pos;
		XMFLOAT3 Normal;
		XMFLOAT2 Tex;
		XMFLOAT4 Color;
	};


ComPtr<ID3D12Resource> Grid::VertexBuffer;
D3D12_VERTEX_BUFFER_VIEW Grid::VertexBufferView {};
UINT Grid::m_VertexCount = 0;
ComPtr<ID3D12PipelineState> Grid::m_LinePso;
ComPtr<ID3D12PipelineState> Grid::m_DeferredLinePso;
bool Grid::m_IsInitialized = false;

void Grid::Init(ID3D12Device* device, ID3D12RootSignature* rootSignature, int gridSize, float spacing)
{
	if (m_IsInitialized || !device || !rootSignature || gridSize <= 0 || spacing <= 0.0f)
	{
		return;
	}

	vector<GridVertex> vertices;
	vertices.reserve(static_cast<size_t>(gridSize + 1) * 4);

	const float halfExtent = gridSize * spacing * 0.5f;
	const XMFLOAT3 normal = { 0.0f, 1.0f, 0.0f };
	const XMFLOAT2 uv = { 0.0f, 0.0f };
	const XMFLOAT4 color = { 0.45f, 0.45f, 0.45f, 1.0f };

	for (int i = 0; i <= gridSize; ++i)
	{
		const float offset = -halfExtent + i * spacing;

		vertices.push_back({ { -halfExtent, 0.0f, offset }, normal, uv, color });
		vertices.push_back({ {  halfExtent, 0.0f, offset }, normal, uv, color });

		vertices.push_back({ { offset, 0.0f, -halfExtent }, normal, uv, color });
		vertices.push_back({ { offset, 0.0f,  halfExtent }, normal, uv, color });
	}

	m_VertexCount = static_cast<UINT>(vertices.size());
	const UINT bufferSize = static_cast<UINT>(vertices.size() * sizeof(GridVertex));

	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);
	HRESULT hr = device->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&resourceDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&VertexBuffer));
	if (FAILED(hr))
	{
		Debug::Log("ERROR: Failed to create grid vertex buffer.\n");
		return;
	}

	void* mapped = nullptr;
	hr = VertexBuffer->Map(0, nullptr, &mapped);
	if (FAILED(hr) || !mapped)
	{
		Debug::Log("ERROR: Failed to map grid vertex buffer.\n");
		VertexBuffer.Reset();
		m_VertexCount = 0;
		return;
	}

	memcpy(mapped, vertices.data(), bufferSize);
	VertexBuffer->Unmap(0, nullptr);

	VertexBufferView.BufferLocation = VertexBuffer->GetGPUVirtualAddress();
	VertexBufferView.SizeInBytes = bufferSize;
	VertexBufferView.StrideInBytes = sizeof(GridVertex);

	ComPtr<ID3DBlob> vsBlob;
	ComPtr<ID3DBlob> psBlob;
	ComPtr<ID3DBlob> deferredPsBlob;
	hr = D3DReadFileToBlob(L"shader/hlsl/build/debugLineVS.cso", &vsBlob);
	if (FAILED(hr))
	{
		Debug::Log("ERROR: Failed to load debugLineVS.cso for grid.\n");
		Uninit();
		return;
	}

	hr = D3DReadFileToBlob(L"shader/hlsl/build/debugLinePS.cso", &psBlob);
	if (FAILED(hr))
	{
		Debug::Log("ERROR: Failed to load debugLinePS.cso for grid.\n");
		Uninit();
		return;
	}

	hr = D3DReadFileToBlob(L"shader/hlsl/build/debugLinePS_MRT.cso", &deferredPsBlob);
	if (FAILED(hr))
	{
		Debug::Log("ERROR: Failed to load debugLinePS_MRT.cso for deferred grid.\n");
		Uninit();
		return;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc {};
	psoDesc.pRootSignature = rootSignature;
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(psBlob.Get());
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleDesc.Count = 1;

	static D3D12_INPUT_ELEMENT_DESC inputLayout[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };

	if (!PsoManager::CreateGraphicsPipelineState(psoDesc, "grid line", m_LinePso))
	{
		Debug::Log("ERROR: Failed to create grid line PSO.\n");
		Uninit();
		return;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC deferredPsoDesc = psoDesc;
	deferredPsoDesc.PS = CD3DX12_SHADER_BYTECODE(deferredPsBlob.Get());
	deferredPsoDesc.NumRenderTargets = 2;
	deferredPsoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	deferredPsoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;

	if (!PsoManager::CreateGraphicsPipelineState(deferredPsoDesc, "deferred grid line", m_DeferredLinePso))
	{
		Debug::Log("ERROR: Failed to create deferred grid line PSO.\n");
		Uninit();
		return;
	}

	m_IsInitialized = true;
}

void Grid::Uninit()
{
	m_DeferredLinePso.Reset();
	m_LinePso.Reset();
	VertexBuffer.Reset();
	VertexBufferView = {};
	m_VertexCount = 0;
	m_IsInitialized = false;
}
