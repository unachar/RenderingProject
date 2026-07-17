#include "pch.h"
#include "debugsystem.h"
#include "componentmanager.h"
#include "rendererdraw.h"
#include "renderercore.h"
#include "renderershader.h"
#include "psomanager.h"
#include "texturemanager.h"
#include "systemmanager.h"
#include "camera.h"
#include "light.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "world.h"

Microsoft::WRL::ComPtr<ID3D12PipelineState> DebugSystem::m_DebugLinePso;
Microsoft::WRL::ComPtr<ID3D12PipelineState> DebugSystem::m_DebugSolidPso;
Microsoft::WRL::ComPtr<ID3D12PipelineState> DebugSystem::m_DebugLineDeferredPso;
Microsoft::WRL::ComPtr<ID3D12PipelineState> DebugSystem::m_DebugSolidDeferredPso;
bool DebugSystem::m_ShowLightDebug = true;

struct DebugVertex { XMFLOAT3 Position; XMFLOAT3 Normal; XMFLOAT2 TexCoord; XMFLOAT4 Color; };

void DebugSystem::Init()
{
	InitDebugLinePso();
}

void DebugSystem::Uninit()
{
	m_DebugLinePso.Reset();
	m_DebugSolidPso.Reset();
	m_DebugLineDeferredPso.Reset();
	m_DebugSolidDeferredPso.Reset();
}

template <typename TVertex>
static void AppendDebugBoxLines(vector<TVertex>& outVertices, const XMFLOAT3& center, const XMFLOAT3& extents, const XMFLOAT4& color)
{
	XMFLOAT3 v[8] = {
		{ center.x - extents.x, center.y - extents.y, center.z - extents.z }, { center.x + extents.x, center.y - extents.y, center.z - extents.z },
		{ center.x + extents.x, center.y + extents.y, center.z - extents.z }, { center.x - extents.x, center.y + extents.y, center.z - extents.z },
		{ center.x - extents.x, center.y - extents.y, center.z + extents.z }, { center.x + extents.x, center.y - extents.y, center.z + extents.z },
		{ center.x + extents.x, center.y + extents.y, center.z + extents.z }, { center.x - extents.x, center.y + extents.y, center.z + extents.z }
	};

	int indices[24] = {
		0,1, 1,2, 2,3, 3,0,  4,5, 5,6, 6,7, 7,4,  0,4, 1,5, 2,6, 3,7
	};

	outVertices.reserve(outVertices.size() + 24);
	for (int idx = 0; idx < 24; ++idx)
	{
		outVertices.push_back({ v[indices[idx]], {0,1,0}, {0,0}, color });
	}
}

static XMFLOAT3 Add3(const XMFLOAT3& a, const XMFLOAT3& b)
{
	return { a.x + b.x, a.y + b.y, a.z + b.z };
}

static XMFLOAT3 Scale3(const XMFLOAT3& v, float s)
{
	return { v.x * s, v.y * s, v.z * s };
}

static XMFLOAT3 Normalize3(const XMFLOAT3& v, const XMFLOAT3& fallback)
{
	XMVECTOR vec = XMLoadFloat3(&v);
	if (XMVectorGetX(XMVector3LengthSq(vec)) <= 0.000001f)
	{
		return fallback;
	}

	XMFLOAT3 out{};
	XMStoreFloat3(&out, XMVector3Normalize(vec));
	return out;
}

static void AppendLine(vector<DebugVertex>& vertices, const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT4& color)
{
	vertices.push_back({ a, {0,1,0}, {0,0}, color });
	vertices.push_back({ b, {0,1,0}, {0,0}, color });
}

static void AppendCircle(vector<DebugVertex>& vertices, const XMFLOAT3& center, float radius, int axis, const XMFLOAT4& color)
{
	constexpr int kSegments = 48;
	for (int i = 0; i < kSegments; ++i)
	{
		const float a0 = XM_2PI * static_cast<float>(i) / static_cast<float>(kSegments);
		const float a1 = XM_2PI * static_cast<float>(i + 1) / static_cast<float>(kSegments);
		XMFLOAT3 p0 = center;
		XMFLOAT3 p1 = center;

		if (axis == 0)
		{
			p0.y += cosf(a0) * radius; p0.z += sinf(a0) * radius;
			p1.y += cosf(a1) * radius; p1.z += sinf(a1) * radius;
		}
		else if (axis == 1)
		{
			p0.x += cosf(a0) * radius; p0.z += sinf(a0) * radius;
			p1.x += cosf(a1) * radius; p1.z += sinf(a1) * radius;
		}
		else
		{
			p0.x += cosf(a0) * radius; p0.y += sinf(a0) * radius;
			p1.x += cosf(a1) * radius; p1.y += sinf(a1) * radius;
		}

		AppendLine(vertices, p0, p1, color);
	}
}

static void BuildDirectionBasis(const XMFLOAT3& direction, XMFLOAT3& right, XMFLOAT3& up)
{
	XMVECTOR forward = XMLoadFloat3(&direction);
	if (XMVectorGetX(XMVector3LengthSq(forward)) <= 0.000001f)
	{
		forward = XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f);
	}
	forward = XMVector3Normalize(forward);

	XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	XMVECTOR rightVec = XMVector3Cross(worldUp, forward);
	if (XMVectorGetX(XMVector3LengthSq(rightVec)) <= 0.000001f)
	{
		rightVec = XMVector3Cross(XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), forward);
	}
	rightVec = XMVector3Normalize(rightVec);
	XMVECTOR upVec = XMVector3Normalize(XMVector3Cross(forward, rightVec));
	XMStoreFloat3(&right, rightVec);
	XMStoreFloat3(&up, upVec);
}

static XMFLOAT3 ConePoint(const XMFLOAT3& center, const XMFLOAT3& right, const XMFLOAT3& up, float radius, float angle)
{
	return Add3(center, Add3(Scale3(right, cosf(angle) * radius), Scale3(up, sinf(angle) * radius)));
}

static void AppendOrientedCircle(vector<DebugVertex>& vertices, const XMFLOAT3& center, const XMFLOAT3& direction, float radius, const XMFLOAT4& color)
{
	XMFLOAT3 right{}, up{};
	BuildDirectionBasis(direction, right, up);
	constexpr int kSegments = 48;
	for (int i = 0; i < kSegments; ++i)
	{
		const float a0 = XM_2PI * static_cast<float>(i) / static_cast<float>(kSegments);
		const float a1 = XM_2PI * static_cast<float>(i + 1) / static_cast<float>(kSegments);
		AppendLine(vertices, ConePoint(center, right, up, radius, a0), ConePoint(center, right, up, radius, a1), color);
	}
}

static void AppendConeLines(vector<DebugVertex>& vertices, const XMFLOAT3& position, const XMFLOAT3& direction, float length, float radius, const XMFLOAT4& color)
{
	XMFLOAT3 right{}, up{};
	BuildDirectionBasis(direction, right, up);
	XMFLOAT3 coneCenter = Add3(position, Scale3(direction, length));
	AppendOrientedCircle(vertices, coneCenter, direction, radius, color);

	for (int i = 0; i < 4; ++i)
	{
		const float angle = XM_PIDIV2 * static_cast<float>(i);
		AppendLine(vertices, position, ConePoint(coneCenter, right, up, radius, angle), color);
	}
}

static void AppendConeSolid(vector<DebugVertex>& vertices, const XMFLOAT3& position, const XMFLOAT3& direction, float length, float radius, const XMFLOAT4& color)
{
	XMFLOAT3 right{}, up{};
	BuildDirectionBasis(direction, right, up);
	XMFLOAT3 coneCenter = Add3(position, Scale3(direction, length));
	constexpr int kSegments = 36;
	for (int i = 0; i < kSegments; ++i)
	{
		const float a0 = XM_2PI * static_cast<float>(i) / static_cast<float>(kSegments);
		const float a1 = XM_2PI * static_cast<float>(i + 1) / static_cast<float>(kSegments);
		XMFLOAT3 p0 = ConePoint(coneCenter, right, up, radius, a0);
		XMFLOAT3 p1 = ConePoint(coneCenter, right, up, radius, a1);
		vertices.push_back({ position, {0,1,0}, {0,0}, color });
		vertices.push_back({ p0, {0,1,0}, {0,0}, color });
		vertices.push_back({ p1, {0,1,0}, {0,0}, color });
	}
}

static void AppendCylinderLines(vector<DebugVertex>& vertices, const XMFLOAT3& position, const XMFLOAT3& direction, float length, float radius, const XMFLOAT4& color)
{
	XMFLOAT3 end = Add3(position, Scale3(direction, length));
	XMFLOAT3 right{}, up{};
	BuildDirectionBasis(direction, right, up);
	AppendOrientedCircle(vertices, position, direction, radius, color);
	AppendOrientedCircle(vertices, end, direction, radius, color);
	for (int i = 0; i < 4; ++i)
	{
		const float angle = XM_PIDIV2 * static_cast<float>(i);
		AppendLine(vertices, ConePoint(position, right, up, radius, angle), ConePoint(end, right, up, radius, angle), color);
	}
}

static void AppendCylinderSolid(vector<DebugVertex>& vertices, const XMFLOAT3& position, const XMFLOAT3& direction, float length, float radius, const XMFLOAT4& color)
{
	XMFLOAT3 right{}, up{};
	BuildDirectionBasis(direction, right, up);
	XMFLOAT3 end = Add3(position, Scale3(direction, length));
	constexpr int kSegments = 36;
	for (int i = 0; i < kSegments; ++i)
	{
		const float a0 = XM_2PI * static_cast<float>(i) / static_cast<float>(kSegments);
		const float a1 = XM_2PI * static_cast<float>(i + 1) / static_cast<float>(kSegments);
		XMFLOAT3 p0 = ConePoint(position, right, up, radius, a0);
		XMFLOAT3 p1 = ConePoint(position, right, up, radius, a1);
		XMFLOAT3 e0 = ConePoint(end, right, up, radius, a0);
		XMFLOAT3 e1 = ConePoint(end, right, up, radius, a1);

		vertices.push_back({ p0, {0,1,0}, {0,0}, color });
		vertices.push_back({ e0, {0,1,0}, {0,0}, color });
		vertices.push_back({ e1, {0,1,0}, {0,0}, color });

		vertices.push_back({ p0, {0,1,0}, {0,0}, color });
		vertices.push_back({ e1, {0,1,0}, {0,0}, color });
		vertices.push_back({ p1, {0,1,0}, {0,0}, color });
	}
}

static void AppendLightLines(vector<DebugVertex>& vertices, const TransformComponent& transform, const LightComponent& light)
{
	const XMFLOAT3 position = transform.Position;
	const XMFLOAT3 direction = Normalize3(light.Direction, { 0.0f, -1.0f, 0.0f });
	const XMFLOAT4 color = { light.Color.x, light.Color.y, light.Color.z, 1.0f };

	AppendLine(vertices, Add3(position, { -0.25f, 0.0f, 0.0f }), Add3(position, { 0.25f, 0.0f, 0.0f }), color);
	AppendLine(vertices, Add3(position, { 0.0f, -0.25f, 0.0f }), Add3(position, { 0.0f, 0.25f, 0.0f }), color);
	AppendLine(vertices, Add3(position, { 0.0f, 0.0f, -0.25f }), Add3(position, { 0.0f, 0.0f, 0.25f }), color);

	if (light.Type == LightType::Directional)
	{
		XMFLOAT3 end = Add3(position, Scale3(direction, 2.0f));
		AppendLine(vertices, position, end, color);
		AppendLine(vertices, end, Add3(end, { -0.18f, 0.12f, 0.0f }), color);
		AppendLine(vertices, end, Add3(end, { 0.18f, 0.12f, 0.0f }), color);
		return;
	}

	const float range = max(light.Range, 0.1f);
	if (light.Type == LightType::Point)
	{
		AppendCircle(vertices, position, range, 0, color);
		AppendCircle(vertices, position, range, 1, color);
		AppendCircle(vertices, position, range, 2, color);
	}
	else if (light.Type == LightType::Spot || light.Type == LightType::Volume)
	{
		const float outer = light.OuterAngle * XM_PI / 180.0f;
		if (light.Type == LightType::Volume)
		{
			if (light.VolumeShape == 1)
			{
				AppendCylinderLines(vertices, position, direction, range, max(0.15f, tanf(outer) * range * 0.35f), color);
			}
			else
			{
				AppendConeLines(vertices, position, direction, range, tanf(outer) * range, color);
			}
		}
		else
		{
			AppendConeLines(vertices, position, direction, range, tanf(outer) * range, color);
		}
	}
}

static void SubmitDebugLines(ID3D12GraphicsCommandList* commandList, const vector<DebugVertex>& vertices)
{
	if (vertices.empty())
	{
		return;
	}

	const UINT vertexBufferSize = static_cast<UINT>(sizeof(DebugVertex) * vertices.size());
	const UINT offsetIncrement = (vertexBufferSize + sizeof(Vertex) - 1) / sizeof(Vertex);
	if (RendererResource::m_DynamicVertexOffset + offsetIncrement > RendererResource::g_kMAX_DYNAMIC_VERTICES)
	{
		Debug::Log("Warning: Dynamic vertex buffer overflow in debug light draw!\n");
		return;
	}

	memcpy(&RendererResource::m_pDynamicVertexDataBegin[RendererResource::m_DynamicVertexOffset], vertices.data(), vertexBufferSize);

	D3D12_VERTEX_BUFFER_VIEW vbView{};
	vbView.BufferLocation = RendererResource::m_DynamicVertexBuffer->GetGPUVirtualAddress() + (RendererResource::m_DynamicVertexOffset * sizeof(Vertex));
	vbView.StrideInBytes = sizeof(DebugVertex);
	vbView.SizeInBytes = vertexBufferSize;

	RendererResource::m_DynamicVertexOffset += offsetIncrement;
	commandList->IASetVertexBuffers(0, 1, &vbView);
	commandList->DrawInstanced(static_cast<UINT>(vertices.size()), 1, 0, 0);
}

void DebugSystem::InitDebugLinePso()
{
	Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
	Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
	Microsoft::WRL::ComPtr<ID3DBlob> psMrtBlob;

	rendererResource lineResource {};
	lineResource.vsPath = "shader\\hlsl\\build\\debugLineVS.cso";
	lineResource.psPath = "shader\\hlsl\\build\\debugLinePS.cso";
	lineResource.psMrtPath = "shader\\hlsl\\build\\debugLinePS_MRT.cso";
	lineResource.ppBlob = vsBlob.GetAddressOf();
	if (!RendererShader::CreateVertexShader(lineResource))
	{
		return;
	}
	lineResource.ppBlob = psBlob.GetAddressOf();
	if (!RendererShader::CreatePixelShader(lineResource))
	{
		return;
	}
	rendererResource lineMrtResource = lineResource;
	lineMrtResource.csoPath = lineResource.psMrtPath;
	lineMrtResource.ppBlob = psMrtBlob.GetAddressOf();
	RendererShader::LoadShaderBlob(lineMrtResource);

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc {};
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(psBlob.Get());
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleDesc.Count = 1;

	D3D12_INPUT_ELEMENT_DESC modelLayout[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
	psoDesc.InputLayout = { modelLayout, _countof(modelLayout) };
	psoDesc.pRootSignature = RendererShader::GetModelRootSignature();
	psoDesc.DepthStencilState.DepthEnable = TRUE;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
	psoDesc.DepthStencilState.StencilEnable = FALSE;

	if (!PsoManager::CreateGraphicsPipelineState(psoDesc, "AABB debug line", m_DebugLinePso))
	{
		Debug::Log("ERROR: Failed to create AABB debug line PSO.\n");
	}

	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
	psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
	psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	if (!PsoManager::CreateGraphicsPipelineState(psoDesc, "AABB debug solid", m_DebugSolidPso))
	{
		Debug::Log("ERROR: Failed to create AABB debug solid PSO.\n");
	}

	if (psMrtBlob)
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC deferredDesc = psoDesc;
		deferredDesc.PS = CD3DX12_SHADER_BYTECODE(psMrtBlob.Get());
		deferredDesc.NumRenderTargets = RendererState::g_kGEOMETRY_GBUFFER_COUNT;
		for (UINT i = 0; i < RendererState::g_kGEOMETRY_GBUFFER_COUNT; ++i)
		{
			deferredDesc.RTVFormats[i] = RendererDraw::GetGBufferFormat(static_cast<GBufferType>(i));
		}

		deferredDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
		deferredDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		if (!PsoManager::CreateGraphicsPipelineState(deferredDesc, "deferred debug line", m_DebugLineDeferredPso))
		{
			Debug::Log("ERROR: Failed to create deferred debug line PSO.\n");
		}

		deferredDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		deferredDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
		deferredDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
		deferredDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		deferredDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
		deferredDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
		deferredDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
		deferredDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
		deferredDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		if (!PsoManager::CreateGraphicsPipelineState(deferredDesc, "deferred debug solid", m_DebugSolidDeferredPso))
		{
			Debug::Log("ERROR: Failed to create deferred debug solid PSO.\n");
		}
	}
}

void DebugSystem::Draw(RenderPass renderPass, bool receivingPostProcessOnly)
{
#ifdef _DEBUG
	if (renderPass == RenderPass::OverlayScene || renderPass == RenderPass::ShadowMap ||
		renderPass == RenderPass::Velocity || renderPass == RenderPass::OcclusionPhase2)
	{
		return;
	}

	const bool drawInPostProcessPass = RendererCore::GetRenderMode() == RenderMode::DEFERRED;
	if (!drawInPostProcessPass && receivingPostProcessOnly)
	{
		return;
	}

	if (!m_DebugLinePso)
	{
		return;
	}

	ID3D12GraphicsCommandList* pCommandList = RendererCore::GetCommandList();
	RendererDraw::BeginModelPass();
	const bool useDeferredPso = RendererCore::GetRenderMode() == RenderMode::DEFERRED;
	ID3D12PipelineState* linePso = (useDeferredPso && m_DebugLineDeferredPso) ? m_DebugLineDeferredPso.Get() : m_DebugLinePso.Get();
	ID3D12PipelineState* solidPso = (useDeferredPso && m_DebugSolidDeferredPso) ? m_DebugSolidDeferredPso.Get() : m_DebugSolidPso.Get();

	XMMATRIX viewMat;
	XMMATRIX projMat;
	Camera::GetCameraMatrices(Camera::GetCameraEntity(), viewMat, projMat);

	auto aabbEntities = World::GetView<AABBComponent>();
	auto obbEntities = World::GetView<OBBComponent>();
	unordered_map<EntityID, TransformComponent*> transformMap;
	unordered_set<EntityID> meshEntitiesSet;

	auto transformEntities = World::GetView<TransformComponent>();
	for (EntityID i : transformEntities)
	{
		transformMap.emplace(i, &ComponentManager::GetComponent<TransformComponent>(i));
	}

	auto meshEntities = World::GetView<MeshComponent>();
	for (EntityID i : meshEntities)
	{
		meshEntitiesSet.insert(i);
	}

	auto lightEntities = World::GetView<LightComponent, TransformComponent>();
	if (aabbEntities.empty() && obbEntities.empty() && lightEntities.empty())
	{
		return;
	}

	UINT cbvIncrement = RendererResource::GetCbvIncrementSize();
	int srvIndex = TextureManager::GetDefaultTextureIndex();
	CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(RendererResource::GetCbvHeap()->GetGPUDescriptorHandleForHeapStart(), srvIndex, cbvIncrement);
	pCommandList->SetGraphicsRootDescriptorTable(1, srvHandle);

	for (EntityID i : aabbEntities)
	{
		XMFLOAT4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
		const auto& aabb = ComponentManager::GetComponent<AABBComponent>(i);
		if (!aabb.DrawDebug)
		{
			continue;
		}

		XMMATRIX world = XMMatrixIdentity();
		auto transformIt = transformMap.find(i);
		if (transformIt != transformMap.end())
		{
			world = XMLoadFloat4x4(&transformIt->second->WorldMatrix);
		}

		ConstantBuffer3D cb{};
		cb.World = XMMatrixTranspose(world);
		cb.View = XMMatrixTranspose(viewMat);
		cb.Projection = XMMatrixTranspose(projMat);
		cb.UseTexture = 0;

		pCommandList->SetGraphicsRootDescriptorTable(0, RendererResource::AllocateTransientConstantBuffer(cb));

		XMFLOAT3 c = aabb.Center;
		XMFLOAT3 e = aabb.Extents;

		XMFLOAT3 v[8] = {
			{ c.x - e.x, c.y - e.y, c.z - e.z }, { c.x + e.x, c.y - e.y, c.z - e.z },
			{ c.x + e.x, c.y + e.y, c.z - e.z }, { c.x - e.x, c.y + e.y, c.z - e.z },
			{ c.x - e.x, c.y - e.y, c.z + e.z }, { c.x + e.x, c.y - e.y, c.z + e.z },
			{ c.x + e.x, c.y + e.y, c.z + e.z }, { c.x - e.x, c.y + e.y, c.z + e.z }
		};

		if (meshEntitiesSet.count(i) > 0)
		{
			pCommandList->SetPipelineState(solidPso);
			RendererDraw::BeginModelPass();
			color = { 0.5f, 0.0f, 0.0f, 0.5f };

			int solidIndices[36] = {
				0,2,1, 0,3,2,  4,5,6, 4,6,7,  4,1,5, 4,0,1,
				3,6,2, 3,7,6,  4,3,0, 4,7,3,  1,2,6, 1,6,5
			};

			vector<DebugVertex> solidVertices;
			solidVertices.reserve(36);
			for (int idx = 0; idx < 36; ++idx)
			{
				solidVertices.push_back({ v[solidIndices[idx]], {0,1,0}, {0,0}, color });
			}

			const UINT vertexBufferSize = sizeof(DebugVertex) * 36;
			const UINT offsetIncrement = (vertexBufferSize + sizeof(Vertex) - 1) / sizeof(Vertex);
			if (RendererResource::m_DynamicVertexOffset + offsetIncrement > RendererResource::g_kMAX_DYNAMIC_VERTICES)
			{
				Debug::Log("Warning: Dynamic vertex buffer overflow in debug draw (solid)!\n");
				break;
			}

			memcpy(&RendererResource::m_pDynamicVertexDataBegin[RendererResource::m_DynamicVertexOffset], solidVertices.data(), vertexBufferSize);

			D3D12_VERTEX_BUFFER_VIEW vbView{};
			vbView.BufferLocation = RendererResource::m_DynamicVertexBuffer->GetGPUVirtualAddress() + (RendererResource::m_DynamicVertexOffset * sizeof(Vertex));
			vbView.StrideInBytes = sizeof(DebugVertex);
			vbView.SizeInBytes = vertexBufferSize;

			RendererResource::m_DynamicVertexOffset += offsetIncrement;
			pCommandList->IASetVertexBuffers(0, 1, &vbView);
			pCommandList->DrawInstanced(36, 1, 0, 0);
		}
		else
		{
			pCommandList->SetPipelineState(linePso);
			RendererDraw::BeginLinePass();

			vector<DebugVertex> lineVertices;
			AppendDebugBoxLines(lineVertices, c, e, color);

			const UINT vertexBufferSize = sizeof(DebugVertex) * 24;
			const UINT offsetIncrement = (vertexBufferSize + sizeof(Vertex) - 1) / sizeof(Vertex);
			if (RendererResource::m_DynamicVertexOffset + offsetIncrement > RendererResource::g_kMAX_DYNAMIC_VERTICES)
			{
				Debug::Log("Warning: Dynamic vertex buffer overflow in debug draw (lines)!\n");
				break;
			}

			memcpy(&RendererResource::m_pDynamicVertexDataBegin[RendererResource::m_DynamicVertexOffset], lineVertices.data(), vertexBufferSize);

			D3D12_VERTEX_BUFFER_VIEW vbView{};
			vbView.BufferLocation = RendererResource::m_DynamicVertexBuffer->GetGPUVirtualAddress() + (RendererResource::m_DynamicVertexOffset * sizeof(Vertex));
			vbView.StrideInBytes = sizeof(DebugVertex);
			vbView.SizeInBytes = vertexBufferSize;

			RendererResource::m_DynamicVertexOffset += offsetIncrement;
			pCommandList->IASetVertexBuffers(0, 1, &vbView);
			pCommandList->DrawInstanced(24, 1, 0, 0);
		}
	}

	for (EntityID i : obbEntities)
	{
		XMFLOAT4 color = { 0.0f, 1.0f, 0.0f, 1.0f };
		const auto& obb = ComponentManager::GetComponent<OBBComponent>(i);

		XMMATRIX world = XMMatrixIdentity();
		auto transformIt = transformMap.find(i);
		if (transformIt != transformMap.end())
		{
			world = XMLoadFloat4x4(&transformIt->second->WorldMatrix);
		}

		ConstantBuffer3D cb{};
		cb.World = XMMatrixTranspose(world);
		cb.View = XMMatrixTranspose(viewMat);
		cb.Projection = XMMatrixTranspose(projMat);
		cb.UseTexture = 0;

		pCommandList->SetGraphicsRootDescriptorTable(0, RendererResource::AllocateTransientConstantBuffer(cb));

		XMFLOAT3 c = obb.Center;
		XMFLOAT3 e = obb.Extents;

		XMFLOAT3 v[8] = {
			{ c.x - e.x, c.y - e.y, c.z - e.z }, { c.x + e.x, c.y - e.y, c.z - e.z },
			{ c.x + e.x, c.y + e.y, c.z - e.z }, { c.x - e.x, c.y + e.y, c.z - e.z },
			{ c.x - e.x, c.y - e.y, c.z + e.z }, { c.x + e.x, c.y - e.y, c.z + e.z },
			{ c.x + e.x, c.y + e.y, c.z + e.z }, { c.x - e.x, c.y + e.y, c.z + e.z }
		};

		if (meshEntitiesSet.count(i) > 0)
		{
			pCommandList->SetPipelineState(solidPso);
			RendererDraw::BeginModelPass();
			color = { 0.0f, 0.5f, 0.0f, 0.5f };

			int solidIndices[36] = {
				0,2,1, 0,3,2,  4,5,6, 4,6,7,  4,1,5, 4,0,1,
				3,6,2, 3,7,6,  4,3,0, 4,7,3,  1,2,6, 1,6,5
			};

			vector<DebugVertex> solidVertices;
			solidVertices.reserve(36);
			for (int idx = 0; idx < 36; ++idx)
			{
				solidVertices.push_back({ v[solidIndices[idx]], {0,1,0}, {0,0}, color });
			}

			const UINT vertexBufferSize = sizeof(DebugVertex) * 36;
			const UINT offsetIncrement = (vertexBufferSize + sizeof(Vertex) - 1) / sizeof(Vertex);
			if (RendererResource::m_DynamicVertexOffset + offsetIncrement > RendererResource::g_kMAX_DYNAMIC_VERTICES)
			{
				Debug::Log("Warning: Dynamic vertex buffer overflow in debug draw (solid obb)!\n");
				break;
			}

			memcpy(&RendererResource::m_pDynamicVertexDataBegin[RendererResource::m_DynamicVertexOffset], solidVertices.data(), vertexBufferSize);

			D3D12_VERTEX_BUFFER_VIEW vbView{};
			vbView.BufferLocation = RendererResource::m_DynamicVertexBuffer->GetGPUVirtualAddress() + (RendererResource::m_DynamicVertexOffset * sizeof(Vertex));
			vbView.StrideInBytes = sizeof(DebugVertex);
			vbView.SizeInBytes = vertexBufferSize;

			RendererResource::m_DynamicVertexOffset += offsetIncrement;
			pCommandList->IASetVertexBuffers(0, 1, &vbView);
			pCommandList->DrawInstanced(36, 1, 0, 0);
		}
		else
		{
			pCommandList->SetPipelineState(linePso);
			RendererDraw::BeginLinePass();

			vector<DebugVertex> lineVertices;
			AppendDebugBoxLines(lineVertices, c, e, color);

			const UINT vertexBufferSize = sizeof(DebugVertex) * 24;
			const UINT offsetIncrement = (vertexBufferSize + sizeof(Vertex) - 1) / sizeof(Vertex);
			if (RendererResource::m_DynamicVertexOffset + offsetIncrement > RendererResource::g_kMAX_DYNAMIC_VERTICES)
			{
				Debug::Log("Warning: Dynamic vertex buffer overflow in debug draw (lines obb)!\n");
				break;
			}

			memcpy(&RendererResource::m_pDynamicVertexDataBegin[RendererResource::m_DynamicVertexOffset], lineVertices.data(), vertexBufferSize);

			D3D12_VERTEX_BUFFER_VIEW vbView{};
			vbView.BufferLocation = RendererResource::m_DynamicVertexBuffer->GetGPUVirtualAddress() + (RendererResource::m_DynamicVertexOffset * sizeof(Vertex));
			vbView.StrideInBytes = sizeof(DebugVertex);
			vbView.SizeInBytes = vertexBufferSize;

			RendererResource::m_DynamicVertexOffset += offsetIncrement;
			pCommandList->IASetVertexBuffers(0, 1, &vbView);
			pCommandList->DrawInstanced(24, 1, 0, 0);
		}
	}

	if (m_ShowLightDebug)
	{
		ConstantBuffer3D cb{};
		cb.World = XMMatrixTranspose(XMMatrixIdentity());
		cb.View = XMMatrixTranspose(viewMat);
		cb.Projection = XMMatrixTranspose(projMat);
		cb.UseTexture = 0;

		D3D12_GPU_DESCRIPTOR_HANDLE cbvHandle = RendererResource::AllocateTransientConstantBuffer(cb);
		pCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);

		vector<DebugVertex> lightLines;
		for (EntityID entity : lightEntities)
		{
			const auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
			if (!light.DrawDebug)
			{
				continue;
			}
			const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
			AppendLightLines(lightLines, transform, light);
		}

		pCommandList->SetPipelineState(linePso);
		RendererDraw::BeginLinePass();
		pCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);
		SubmitDebugLines(pCommandList, lightLines);
	}
#endif
}

