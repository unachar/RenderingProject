#include "pch.h"
#include "primitivegeometry.h"
#include "graphicsdevice.h"
#include "componentmanager.h"

namespace
{
	uint64_t HashGeometry(const void* data, size_t size)
	{
		uint64_t hash = 1469598103934665603ull;
		const auto* bytes = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < size; ++i)
		{
			hash ^= bytes[i];
			hash *= 1099511628211ull;
		}
		return hash;
	}

	void CalculateGeometryBounds(const vector<Vertex>& vertices, XMFLOAT3& center, XMFLOAT3& extents)
	{
		if (vertices.empty())
		{
			center = {};
			extents = {};
			return;
		}
		XMFLOAT3 minimum = vertices.front().Pos;
		XMFLOAT3 maximum = vertices.front().Pos;
		for (const Vertex& vertex : vertices)
		{
			minimum.x = min(minimum.x, vertex.Pos.x);
			minimum.y = min(minimum.y, vertex.Pos.y);
			minimum.z = min(minimum.z, vertex.Pos.z);
			maximum.x = max(maximum.x, vertex.Pos.x);
			maximum.y = max(maximum.y, vertex.Pos.y);
			maximum.z = max(maximum.z, vertex.Pos.z);
		}
		center = { (minimum.x + maximum.x) * 0.5f, (minimum.y + maximum.y) * 0.5f, (minimum.z + maximum.z) * 0.5f };
		extents = { (maximum.x - minimum.x) * 0.5f, (maximum.y - minimum.y) * 0.5f, (maximum.z - minimum.z) * 0.5f };
	}

	int GetShapeSideCount(ShapeType shapeType)
	{
		switch (shapeType)
		{
		case ShapeType::TRIANGLE: return 3;
		case ShapeType::QUAD:     return 4;
		case ShapeType::PENTAGON: return 5;
		case ShapeType::HEXAGON:  return 6;
		case ShapeType::HEPTAGON: return 7;
		case ShapeType::OCTAGON:  return 8;
		case ShapeType::CIRCLE:   return 32;
		case ShapeType::NONE:
		default:
			return 3;
		}
	}

	XMFLOAT4 GetVertexColor(Color color)
	{
		switch (color)
		{
		case Color::RED:     return { 1.0f, 0.0f, 0.0f, 1.0f };
		case Color::GREEN:   return { 0.0f, 1.0f, 0.0f, 1.0f };
		case Color::BLUE:    return { 0.0f, 0.0f, 1.0f, 1.0f };
		case Color::YELLOW:  return { 1.0f, 1.0f, 0.0f, 1.0f };
		case Color::CYAN:    return { 0.0f, 1.0f, 1.0f, 1.0f };
		case Color::MAGENTA: return { 1.0f, 0.0f, 1.0f, 1.0f };
		case Color::WHITE:   return { 1.0f, 1.0f, 1.0f, 1.0f };
		case Color::NONE:
		default:
			return { 1.0f, 1.0f, 1.0f, 0.0f };
		}
	}

	template<typename ApplyUvFunc>
	vector<Vertex> CreateShapeVertices(ShapeType shapeType, float radius, Color color, ApplyUvFunc applyUv)
	{
		vector<Vertex> vertices;
		const XMFLOAT4 vertexColor = GetVertexColor(color);

		if (shapeType == ShapeType::QUAD)
		{
			float halfW = radius;
			float halfH = radius;

			vertices.push_back({ { -halfW,  halfH, 0.0f }, { 0.0f, 0.0f, 1.0f }, applyUv({ 0.0f, 0.0f }), vertexColor });
			vertices.push_back({ {  halfW,  halfH, 0.0f }, { 0.0f, 0.0f, 1.0f }, applyUv({ 1.0f, 0.0f }), vertexColor });
			vertices.push_back({ { -halfW, -halfH, 0.0f }, { 0.0f, 0.0f, 1.0f }, applyUv({ 0.0f, 1.0f }), vertexColor });

			vertices.push_back({ {  halfW,  halfH, 0.0f }, { 0.0f, 0.0f, 1.0f }, applyUv({ 1.0f, 0.0f }), vertexColor });
			vertices.push_back({ {  halfW, -halfH, 0.0f }, { 0.0f, 0.0f, 1.0f }, applyUv({ 1.0f, 1.0f }), vertexColor });
			vertices.push_back({ { -halfW, -halfH, 0.0f }, { 0.0f, 0.0f, 1.0f }, applyUv({ 0.0f, 1.0f }), vertexColor });
			return vertices;
		}

		const int actualSides = GetShapeSideCount(shapeType);
		const float angleStep = (2.0f * XM_PI) / actualSides;
		const float rotationOffset = 0.0f;

		for (int i = 0; i < actualSides; ++i)
		{
			float angle1 = i * angleStep + rotationOffset;
			float angle2 = (i + 1) * angleStep + rotationOffset;

			vertices.push_back({ { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, applyUv({ 0.5f, 0.5f }), vertexColor });

			vertices.push_back({
				{ radius * sinf(angle1), radius * cosf(angle1), 0.0f },
				{ 0.0f, 0.0f, 1.0f },
				applyUv({ 0.5f + 0.5f * sinf(angle1), 0.5f - 0.5f * cosf(angle1) }),
				vertexColor
				});
			vertices.push_back({
				{ radius * sinf(angle2), radius * cosf(angle2), 0.0f },
				{ 0.0f, 0.0f, 1.0f },
				applyUv({ 0.5f + 0.5f * sinf(angle2), 0.5f - 0.5f * cosf(angle2) }),
				vertexColor
				});
		}

		return vertices;
	}

	XMFLOAT3 Subtract(const XMFLOAT3& a, const XMFLOAT3& b)
	{
		return { a.x - b.x, a.y - b.y, a.z - b.z };
	}

	XMFLOAT3 Cross(const XMFLOAT3& a, const XMFLOAT3& b)
	{
		return {
			a.y * b.z - a.z * b.y,
			a.z * b.x - a.x * b.z,
			a.x * b.y - a.y * b.x
		};
	}

	float Dot(const XMFLOAT3& a, const XMFLOAT3& b)
	{
		return a.x * b.x + a.y * b.y + a.z * b.z;
	}

	XMFLOAT3 Normalize(const XMFLOAT3& v)
	{
		const float lengthSq = Dot(v, v);
		if (lengthSq <= 0.000001f)
		{
			return { 0.0f, 1.0f, 0.0f };
		}
		const float invLength = 1.0f / sqrtf(lengthSq);
		return { v.x * invLength, v.y * invLength, v.z * invLength };
	}

	void AddTriangle(vector<Vertex>& vertices, Vertex a, Vertex b, Vertex c, const XMFLOAT3& outward)
	{
		const XMFLOAT3 edge1 = Subtract(b.Pos, a.Pos);
		const XMFLOAT3 edge2 = Subtract(c.Pos, a.Pos);
		if (Dot(Cross(edge1, edge2), outward) < 0.0f)
		{
			swap(b, c);
		}

		vertices.push_back(a);
		vertices.push_back(b);
		vertices.push_back(c);
	}

	void AddFace(vector<Vertex>& vertices, const XMFLOAT4& color, XMFLOAT3 normal, XMFLOAT3 v0, XMFLOAT3 v1, XMFLOAT3 v2, XMFLOAT3 v3)
	{
		AddTriangle(vertices, { v0, normal, { 0.0f, 0.0f }, color }, { v1, normal, { 1.0f, 0.0f }, color }, { v2, normal, { 1.0f, 1.0f }, color }, normal);
		AddTriangle(vertices, { v0, normal, { 0.0f, 0.0f }, color }, { v2, normal, { 1.0f, 1.0f }, color }, { v3, normal, { 0.0f, 1.0f }, color }, normal);
	}

	XMFLOAT3 SpherePosition(float radius, float theta, float phi)
	{
		return {
			radius * sinf(phi) * sinf(theta),
			radius * cosf(phi),
			radius * sinf(phi) * cosf(theta)
		};
	}

	void AddSphereBand(vector<Vertex>& vertices, const XMFLOAT4& color, float radius, float centerY, float phi0, float phi1, int slices)
	{
		const float thetaStep = (2.0f * XM_PI) / slices;

		for (int slice = 0; slice < slices; ++slice)
		{
			const float theta0 = slice * thetaStep;
			const float theta1 = (slice + 1) * thetaStep;

			XMFLOAT3 n00 = Normalize(SpherePosition(1.0f, theta0, phi0));
			XMFLOAT3 n01 = Normalize(SpherePosition(1.0f, theta1, phi0));
			XMFLOAT3 n10 = Normalize(SpherePosition(1.0f, theta0, phi1));
			XMFLOAT3 n11 = Normalize(SpherePosition(1.0f, theta1, phi1));

			XMFLOAT3 p00 = { n00.x * radius, centerY + n00.y * radius, n00.z * radius };
			XMFLOAT3 p01 = { n01.x * radius, centerY + n01.y * radius, n01.z * radius };
			XMFLOAT3 p10 = { n10.x * radius, centerY + n10.y * radius, n10.z * radius };
			XMFLOAT3 p11 = { n11.x * radius, centerY + n11.y * radius, n11.z * radius };

			if (phi0 <= 0.0001f)
			{
				AddTriangle(vertices, { p00, n00, { 0.5f, 0.0f }, color }, { p11, n11, { 1.0f, 1.0f }, color }, { p10, n10, { 0.0f, 1.0f }, color }, Normalize(p00));
			}
			else if (phi1 >= XM_PI - 0.0001f)
			{
				AddTriangle(vertices, { p00, n00, { 0.0f, 0.0f }, color }, { p01, n01, { 1.0f, 0.0f }, color }, { p10, n10, { 0.5f, 1.0f }, color }, Normalize(p10));
			}
			else
			{
				XMFLOAT3 outward = Normalize({ p00.x + p01.x + p10.x + p11.x, p00.y + p01.y + p10.y + p11.y - centerY * 4.0f, p00.z + p01.z + p10.z + p11.z });
				AddTriangle(vertices, { p00, n00, { 0.0f, 0.0f }, color }, { p01, n01, { 1.0f, 0.0f }, color }, { p11, n11, { 1.0f, 1.0f }, color }, outward);
				AddTriangle(vertices, { p00, n00, { 0.0f, 0.0f }, color }, { p11, n11, { 1.0f, 1.0f }, color }, { p10, n10, { 0.0f, 1.0f }, color }, outward);
			}
		}
	}

	vector<Vertex> CreateQuadVertices(Color color)
	{
		vector<Vertex> vertices;
		const XMFLOAT4 vertexColor = GetVertexColor(color);
		const float h = 0.5f;
		AddFace(vertices, vertexColor, { 0.0f, 0.0f, 1.0f }, { -h,  h, 0.0f }, { h,  h, 0.0f }, { h, -h, 0.0f }, { -h, -h, 0.0f });
		return vertices;
	}

	vector<Vertex> CreatePlaneVertices(Color color)
	{
		vector<Vertex> vertices;
		const XMFLOAT4 vertexColor = GetVertexColor(color);
		const float h = 0.5f;
		AddFace(vertices, vertexColor, { 0.0f, 1.0f, 0.0f }, { -h, 0.0f, -h }, { h, 0.0f, -h }, { h, 0.0f, h }, { -h, 0.0f, h });
		return vertices;
	}

	vector<Vertex> CreateCubeVertices(Color color)
	{
		vector<Vertex> vertices;
		const XMFLOAT4 vertexColor = GetVertexColor(color);
		const float h = 0.5f;

		AddFace(vertices, vertexColor, { 0.0f, 0.0f, 1.0f }, { -h,  h,  h }, {  h,  h,  h }, {  h, -h,  h }, { -h, -h,  h });
		AddFace(vertices, vertexColor, { 0.0f, 0.0f,-1.0f }, {  h,  h, -h }, { -h,  h, -h }, { -h, -h, -h }, {  h, -h, -h });
		AddFace(vertices, vertexColor, { 1.0f, 0.0f, 0.0f }, {  h,  h,  h }, {  h,  h, -h }, {  h, -h, -h }, {  h, -h,  h });
		AddFace(vertices, vertexColor, {-1.0f, 0.0f, 0.0f }, { -h,  h, -h }, { -h,  h,  h }, { -h, -h,  h }, { -h, -h, -h });
		AddFace(vertices, vertexColor, { 0.0f, 1.0f, 0.0f }, { -h,  h, -h }, {  h,  h, -h }, {  h,  h,  h }, { -h,  h,  h });
		AddFace(vertices, vertexColor, { 0.0f,-1.0f, 0.0f }, { -h, -h,  h }, {  h, -h,  h }, {  h, -h, -h }, { -h, -h, -h });

		return vertices;
	}

	vector<Vertex> CreateSphereVertices(Color color)
	{
		vector<Vertex> vertices;
		const XMFLOAT4 vertexColor = GetVertexColor(color);
		const int stacks = 16;
		const int slices = 32;
		const float phiStep = XM_PI / stacks;

		for (int stack = 0; stack < stacks; ++stack)
		{
			AddSphereBand(vertices, vertexColor, 0.5f, 0.0f, stack * phiStep, (stack + 1) * phiStep, slices);
		}

		return vertices;
	}

	vector<Vertex> CreateCylinderVertices(Color color, bool includeCaps = true)
	{
		vector<Vertex> vertices;
		const XMFLOAT4 vertexColor = GetVertexColor(color);
		const int slices = 32;
		const float radius = 0.5f;
		const float halfH = 0.5f;
		const float thetaStep = (2.0f * XM_PI) / slices;

		for (int i = 0; i < slices; ++i)
		{
			const float t0 = i * thetaStep;
			const float t1 = (i + 1) * thetaStep;
			const XMFLOAT3 n0 = Normalize({ sinf(t0), 0.0f, cosf(t0) });
			const XMFLOAT3 n1 = Normalize({ sinf(t1), 0.0f, cosf(t1) });
			const XMFLOAT3 top0 = { n0.x * radius, halfH, n0.z * radius };
			const XMFLOAT3 top1 = { n1.x * radius, halfH, n1.z * radius };
			const XMFLOAT3 bottom0 = { n0.x * radius, -halfH, n0.z * radius };
			const XMFLOAT3 bottom1 = { n1.x * radius, -halfH, n1.z * radius };
			const XMFLOAT3 sideOut = Normalize({ n0.x + n1.x, 0.0f, n0.z + n1.z });

			AddTriangle(vertices, { top0, n0, { 0.0f, 0.0f }, vertexColor }, { top1, n1, { 1.0f, 0.0f }, vertexColor }, { bottom1, n1, { 1.0f, 1.0f }, vertexColor }, sideOut);
			AddTriangle(vertices, { top0, n0, { 0.0f, 0.0f }, vertexColor }, { bottom1, n1, { 1.0f, 1.0f }, vertexColor }, { bottom0, n0, { 0.0f, 1.0f }, vertexColor }, sideOut);
			if (includeCaps)
			{
				AddTriangle(vertices, { { 0.0f, halfH, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.5f, 0.5f }, vertexColor }, { top0, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f }, vertexColor }, { top1, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f }, vertexColor }, { 0.0f, 1.0f, 0.0f });
				AddTriangle(vertices, { { 0.0f, -halfH, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.5f, 0.5f }, vertexColor }, { bottom1, { 0.0f, -1.0f, 0.0f }, { 1.0f, 1.0f }, vertexColor }, { bottom0, { 0.0f, -1.0f, 0.0f }, { 0.0f, 1.0f }, vertexColor }, { 0.0f, -1.0f, 0.0f });
			}
		}

		return vertices;
	}

	vector<Vertex> CreateCapsuleVertices(Color color)
	{
		vector<Vertex> vertices = CreateCylinderVertices(color, false);
		const XMFLOAT4 vertexColor = GetVertexColor(color);
		const int hemiStacks = 8;
		const int slices = 32;
		const float radius = 0.5f;
		const float topCenterY = 0.5f;
		const float bottomCenterY = -0.5f;
		const float phiStep = (XM_PIDIV2) / hemiStacks;

		for (int stack = 0; stack < hemiStacks; ++stack)
		{
			AddSphereBand(vertices, vertexColor, radius, topCenterY, stack * phiStep, (stack + 1) * phiStep, slices);
		}
		for (int stack = 0; stack < hemiStacks; ++stack)
		{
			AddSphereBand(vertices, vertexColor, radius, bottomCenterY, XM_PIDIV2 + stack * phiStep, XM_PIDIV2 + (stack + 1) * phiStep, slices);
		}

		return vertices;
	}


}

void PrimitiveGeometry::CreateSpriteVertex(const VertexResource& vertexstruct)
{
	if (vertexstruct.entityid >= g_kMAX_ENTITIES) return;

	if (!ComponentManager::HasComponent(vertexstruct.entityid, ComponentType::SPRITE))
	{
		ComponentManager::ReportMissingComponentError(vertexstruct.entityid, "SpriteComponent");
		return;
	}

	auto& spriteComponent = ComponentManager::GetComponent<SpriteComponent>(vertexstruct.entityid);
	auto ApplyUv = [&](const XMFLOAT2& uv)
		{
			if (!spriteComponent.UseUvTransform)
			{
				return uv;
			}
			return XMFLOAT2(
				uv.x * spriteComponent.UvScale.x + spriteComponent.UvOffset.x,
				uv.y * spriteComponent.UvScale.y + spriteComponent.UvOffset.y
			);
		};

	vector<Vertex> vertices = CreateShapeVertices(vertexstruct.shapetype, vertexstruct.radius, vertexstruct.color, ApplyUv);

	const UINT vertexBufferSize = static_cast<UINT>(sizeof(Vertex) * vertices.size());

	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);

	ID3D12Device* device = GraphicsDevice::GetDevice();
	if (!device)
	{
		Debug::Log("ERROR: Renderer device is null in CreateSpriteVertex\n");
		return;
	}

	HRESULT hr = device->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&resourceDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&ComponentManager::GetComponent<SpriteComponent>(vertexstruct.entityid).VertexBuffer)
	);
	if (FAILED(hr))
	{
		Debug::Log("ERROR: CreateCommittedResource failed\n");
		return;
	}

	UINT8* pVertexDataBegin{};
	CD3DX12_RANGE readRange(0, 0);

	hr = spriteComponent.VertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin));
	if (FAILED(hr))
	{
		Debug::Log("ERROR: VertexBuffer Map failed in CreateSpriteVertex\n");
		return;
	}
	memcpy(pVertexDataBegin, vertices.data(), vertexBufferSize);
	spriteComponent.VertexBuffer->Unmap(0, nullptr);

	spriteComponent.VertexBufferView.BufferLocation = spriteComponent.VertexBuffer->GetGPUVirtualAddress();
	spriteComponent.VertexBufferView.StrideInBytes = sizeof(Vertex);
	spriteComponent.VertexBufferView.SizeInBytes = vertexBufferSize;
	spriteComponent.VertexCount = static_cast<UINT>(vertices.size());
	spriteComponent.GeometryHash = HashGeometry(vertices.data(), vertexBufferSize);
	CalculateGeometryBounds(vertices, spriteComponent.LocalBoundsCenter, spriteComponent.LocalBoundsExtents);
	spriteComponent.HasLocalBounds = !vertices.empty();
}

void PrimitiveGeometry::CreateObjectVertex(const VertexResource& vertexstruct)
{
	if (vertexstruct.entityid >= g_kMAX_ENTITIES) return;

	if (!ComponentManager::HasComponent(vertexstruct.entityid, ComponentType::MESH))
	{
		ComponentManager::ReportMissingComponentError(vertexstruct.entityid, "MeshComponent");
		return;
	}

	auto& meshComponent = ComponentManager::GetComponent<MeshComponent>(vertexstruct.entityid);

	vector<Vertex> vertices;
	switch (vertexstruct.objectType)
	{
	case ObjectType::CUBE:
		vertices = CreateCubeVertices(vertexstruct.color);
		break;
	case ObjectType::SPHERE:
		vertices = CreateSphereVertices(vertexstruct.color);
		break;
	case ObjectType::CAPSULE:
		vertices = CreateCapsuleVertices(vertexstruct.color);
		break;
	case ObjectType::CYLINDER:
		vertices = CreateCylinderVertices(vertexstruct.color);
		break;
	case ObjectType::PLANE:
		vertices = CreatePlaneVertices(vertexstruct.color);
		break;
	case ObjectType::QUAD:
		vertices = CreateQuadVertices(vertexstruct.color);
		break;
	case ObjectType::NONE:
	default:
		Debug::Log("ERROR: Unsupported ObjectType in CreateObjectVertex\n");
		return;
	}

	const UINT vertexBufferSize = static_cast<UINT>(sizeof(Vertex) * vertices.size());

	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);

	ID3D12Device* device = GraphicsDevice::GetDevice();
	if (!device)
	{
		Debug::Log("ERROR: Renderer device is null in CreateObjectVertex\n");
		return;
	}

	HRESULT hr = device->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&resourceDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&meshComponent.VertexBuffer)
	);
	if (FAILED(hr))
	{
		Debug::Log("ERROR: CreateCommittedResource failed\n");
		return;
	}

	UINT8* pVertexDataBegin{};
	CD3DX12_RANGE readRange(0, 0);

	hr = meshComponent.VertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin));
	if (FAILED(hr))
	{
		Debug::Log("ERROR: VertexBuffer Map failed in CreateObjectVertex\n");
		return;
	}
	memcpy(pVertexDataBegin, vertices.data(), vertexBufferSize);
	meshComponent.VertexBuffer->Unmap(0, nullptr);

	meshComponent.VertexBufferView.BufferLocation = meshComponent.VertexBuffer->GetGPUVirtualAddress();
	meshComponent.VertexBufferView.StrideInBytes = sizeof(Vertex);
	meshComponent.VertexBufferView.SizeInBytes = vertexBufferSize;
	meshComponent.VertexCount = static_cast<UINT>(vertices.size());
	meshComponent.GeometryHash = HashGeometry(vertices.data(), vertexBufferSize);
	CalculateGeometryBounds(vertices, meshComponent.LocalBoundsCenter, meshComponent.LocalBoundsExtents);
	meshComponent.HasLocalBounds = !vertices.empty();
}
