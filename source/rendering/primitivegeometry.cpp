#include "pch.h"
#include "primitivegeometry.h"
#include "graphicsdevice.h"
#include "componentmanager.h"
#include "staticmodel.h"
#include "meshlets/meshletmanager.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "cpuculling.h"
#include <numeric>
#include "../../External/meshoptimizer/src/meshoptimizer.h"

namespace
{
	void BuildPrimitiveLODs(StaticMeshData& mesh)
	{
		mesh.CpuLodIndices = {};
		mesh.LodIndexCounts = {};
		// Weld identical attributes for simplification, then map back to the original
		// triangle-list vertices so fallback drawing and vertex deformation stay intact.
		vector<uint32_t> remap(mesh.VertexCount);
		const size_t uniqueCount = meshopt_generateVertexRemap(remap.data(),
			mesh.CpuIndices.data(), mesh.CpuIndices.size(), mesh.CpuVertices.data(),
			mesh.CpuVertices.size(), sizeof(StaticModelVertex));
		vector<StaticModelVertex> vertices(uniqueCount);
		vector<uint32_t> indices(mesh.CpuIndices.size()), original(uniqueCount);
		meshopt_remapVertexBuffer(vertices.data(), mesh.CpuVertices.data(), mesh.VertexCount,
			sizeof(StaticModelVertex), remap.data());
		meshopt_remapIndexBuffer(indices.data(), mesh.CpuIndices.data(), indices.size(), remap.data());
		for (uint32_t i = 0; i < mesh.VertexCount; ++i) original[remap[i]] = i;
		constexpr float ratios[] = {0.50f, 0.20f, 0.08f, 0.03f};
		constexpr float errors[] = {0.01f, 0.035f, 0.08f, 0.15f};
		for (uint32_t lod = 0; lod < StaticMeshData::LodCount - 1; ++lod)
		{
			auto& result = mesh.CpuLodIndices[lod];
			result.resize(indices.size());
			const size_t target = (max)(size_t(3), size_t(indices.size() * ratios[lod]) / 3 * 3);
			const size_t count = meshopt_simplify(result.data(), indices.data(), indices.size(),
				reinterpret_cast<const float*>(vertices.data()), vertices.size(), sizeof(StaticModelVertex),
				target, errors[lod], meshopt_SimplifyRegularize);
			if (count < 3 || count >= indices.size()) { result.clear(); continue; }
			result.resize(count);
			for (auto& index : result) index = original[index];
			meshopt_optimizeVertexCache(result.data(), result.data(), result.size(), mesh.VertexCount);
			mesh.LodIndexCounts[lod] = static_cast<UINT>(count);
		}
	}
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
	vector<Vertex> CreateShapeVertices(ShapeType shapeType, float radius, Color color, ApplyUvFunc applyUv, int circleSegments = 32)
	{
		vector<Vertex> vertices;
		const XMFLOAT4 vertexColor = GetVertexColor(color);

		if (shapeType == ShapeType::QUAD)
		{
			float halfW = radius;
			float halfH = radius;

			vertices.push_back({ { -halfW,  halfH, 0.0f }, { 0.0f, 0.0f, -1.0f }, applyUv({ 0.0f, 0.0f }), vertexColor });
			vertices.push_back({ {  halfW,  halfH, 0.0f }, { 0.0f, 0.0f, -1.0f }, applyUv({ 1.0f, 0.0f }), vertexColor });
			vertices.push_back({ { -halfW, -halfH, 0.0f }, { 0.0f, 0.0f, -1.0f }, applyUv({ 0.0f, 1.0f }), vertexColor });

			vertices.push_back({ {  halfW,  halfH, 0.0f }, { 0.0f, 0.0f, -1.0f }, applyUv({ 1.0f, 0.0f }), vertexColor });
			vertices.push_back({ {  halfW, -halfH, 0.0f }, { 0.0f, 0.0f, -1.0f }, applyUv({ 1.0f, 1.0f }), vertexColor });
			vertices.push_back({ { -halfW, -halfH, 0.0f }, { 0.0f, 0.0f, -1.0f }, applyUv({ 0.0f, 1.0f }), vertexColor });
			return vertices;
		}

		const int actualSides = shapeType == ShapeType::CIRCLE ? circleSegments : GetShapeSideCount(shapeType);
		const float angleStep = (2.0f * XM_PI) / actualSides;
		const float rotationOffset = 0.0f;

		for (int i = 0; i < actualSides; ++i)
		{
			float angle1 = i * angleStep + rotationOffset;
			float angle2 = (i + 1) * angleStep + rotationOffset;

			vertices.push_back({ { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f }, applyUv({ 0.5f, 0.5f }), vertexColor });

			vertices.push_back({
				{ radius * sinf(angle1), radius * cosf(angle1), 0.0f },
				{ 0.0f, 0.0f, -1.0f },
				applyUv({ 0.5f + 0.5f * sinf(angle1), 0.5f - 0.5f * cosf(angle1) }),
				vertexColor
				});
			vertices.push_back({
				{ radius * sinf(angle2), radius * cosf(angle2), 0.0f },
				{ 0.0f, 0.0f, -1.0f },
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

	vector<Vertex> SubdivideFaces(const vector<Vertex>& source, int divisions)
	{
		vector<Vertex> result;
		result.reserve(source.size() * divisions * divisions);
		for (size_t triangle = 0; triangle < source.size(); triangle += 3)
		{
			const Vertex& a = source[triangle];
			const Vertex& b = source[triangle + 1];
			const Vertex& c = source[triangle + 2];
			const auto sample = [&](int row, int column)
			{
				const float u = float(row) / divisions;
				const float v = float(column) / divisions;
				const float w = 1.0f - u - v;
				Vertex vertex;
				vertex.Pos = { a.Pos.x*w + b.Pos.x*u + c.Pos.x*v,
					a.Pos.y*w + b.Pos.y*u + c.Pos.y*v, a.Pos.z*w + b.Pos.z*u + c.Pos.z*v };
				vertex.Normal = Normalize({ a.Normal.x*w + b.Normal.x*u + c.Normal.x*v,
					a.Normal.y*w + b.Normal.y*u + c.Normal.y*v, a.Normal.z*w + b.Normal.z*u + c.Normal.z*v });
				vertex.Tex = { a.Tex.x*w + b.Tex.x*u + c.Tex.x*v, a.Tex.y*w + b.Tex.y*u + c.Tex.y*v };
				vertex.Color = { a.Color.x*w + b.Color.x*u + c.Color.x*v,
					a.Color.y*w + b.Color.y*u + c.Color.y*v,
					a.Color.z*w + b.Color.z*u + c.Color.z*v,
					a.Color.w*w + b.Color.w*u + c.Color.w*v };
				return vertex;
			};
			for (int row = 0; row < divisions; ++row)
			{
				for (int column = 0; column < divisions - row; ++column)
				{
					result.push_back(sample(row, column));
					result.push_back(sample(row + 1, column));
					result.push_back(sample(row, column + 1));
					if (column < divisions - row - 1)
					{
						result.push_back(sample(row + 1, column));
						result.push_back(sample(row + 1, column + 1));
						result.push_back(sample(row, column + 1));
					}
				}
			}
		}
		return result;
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

	vector<Vertex> CreatePyramidVertices(Color color, int sides)
	{
		vector<Vertex> vertices;
		const auto vertexColor = GetVertexColor(color);
		const XMFLOAT3 apex = { 0.0f, 0.5f, 0.0f };
		const XMFLOAT3 center = { 0.0f, -0.5f, 0.0f };
		for (int side = 0; side < sides; ++side)
		{
			const float angle0 = XM_2PI * side / sides;
			const float angle1 = XM_2PI * (side + 1) / sides;
			const XMFLOAT3 a = { 0.5f * sinf(angle0), -0.5f, 0.5f * cosf(angle0) };
			const XMFLOAT3 b = { 0.5f * sinf(angle1), -0.5f, 0.5f * cosf(angle1) };
			const auto normal = Normalize(Cross(Subtract(a, apex), Subtract(b, apex)));
			AddTriangle(vertices, { apex, normal, { 0.5f, 0.0f }, vertexColor },
				{ b, normal, { 1.0f, 1.0f }, vertexColor },
				{ a, normal, { 0.0f, 1.0f }, vertexColor }, normal);
			const XMFLOAT3 down = { 0.0f, -1.0f, 0.0f };
			AddTriangle(vertices, { center, down, { 0.5f, 0.5f }, vertexColor },
				{ a, down, { a.x + 0.5f, a.z + 0.5f }, vertexColor },
				{ b, down, { b.x + 0.5f, b.z + 0.5f }, vertexColor }, down);
		}
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

	vector<Vertex> CreateSphereVertices(Color color, int slices, int stacks)
	{
		vector<Vertex> vertices;
		const XMFLOAT4 vertexColor = GetVertexColor(color);
		const float phiStep = XM_PI / stacks;

		for (int stack = 0; stack < stacks; ++stack)
		{
			AddSphereBand(vertices, vertexColor, 0.5f, 0.0f, stack * phiStep, (stack + 1) * phiStep, slices);
		}

		return vertices;
	}

	vector<Vertex> CreateCylinderVertices(Color color, int slices, bool includeCaps = true, bool flatSides = false)
	{
		vector<Vertex> vertices;
		const XMFLOAT4 vertexColor = GetVertexColor(color);
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

			const XMFLOAT3 normal0 = flatSides ? sideOut : n0;
			const XMFLOAT3 normal1 = flatSides ? sideOut : n1;
			AddTriangle(vertices, { top0, normal0, { 0.0f, 0.0f }, vertexColor }, { top1, normal1, { 1.0f, 0.0f }, vertexColor }, { bottom1, normal1, { 1.0f, 1.0f }, vertexColor }, sideOut);
			AddTriangle(vertices, { top0, normal0, { 0.0f, 0.0f }, vertexColor }, { bottom1, normal1, { 1.0f, 1.0f }, vertexColor }, { bottom0, normal0, { 0.0f, 1.0f }, vertexColor }, sideOut);
			if (includeCaps)
			{
				AddTriangle(vertices, { { 0.0f, halfH, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.5f, 0.5f }, vertexColor }, { top0, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f }, vertexColor }, { top1, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f }, vertexColor }, { 0.0f, 1.0f, 0.0f });
				AddTriangle(vertices, { { 0.0f, -halfH, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.5f, 0.5f }, vertexColor }, { bottom1, { 0.0f, -1.0f, 0.0f }, { 1.0f, 1.0f }, vertexColor }, { bottom0, { 0.0f, -1.0f, 0.0f }, { 0.0f, 1.0f }, vertexColor }, { 0.0f, -1.0f, 0.0f });
			}
		}

		return vertices;
	}

	vector<Vertex> CreateCapsuleVertices(Color color, int slices, int hemiStacks)
	{
		vector<Vertex> vertices = CreateCylinderVertices(color, slices, false);
		const XMFLOAT4 vertexColor = GetVertexColor(color);
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

	spriteComponent.SourceShape = static_cast<int>(vertexstruct.shapetype);
	spriteComponent.SourceRadius = vertexstruct.radius;
	spriteComponent.SourceColor = static_cast<int>(vertexstruct.color);
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

void PrimitiveGeometry::CreateObjectVertex(const VertexResource& vertexstruct, const PrimitiveSettings& settings)
{
	if (vertexstruct.entityid >= g_kMAX_ENTITIES) return;

	if (!ComponentManager::HasComponent(vertexstruct.entityid, ComponentType::MESH))
	{
		ComponentManager::ReportMissingComponentError(vertexstruct.entityid, "MeshComponent");
		return;
	}

	const int segments = std::clamp(settings.Segments, 3, 128);
	const int rings = std::clamp(settings.Rings, 2, 64);

	vector<Vertex> vertices;
	switch (vertexstruct.objectType)
	{
	case ObjectType::CUBE:
		vertices = CreateCubeVertices(vertexstruct.color);
		break;
	case ObjectType::SPHERE:
		vertices = CreateSphereVertices(vertexstruct.color, segments, rings);
		break;
	case ObjectType::CAPSULE:
		vertices = CreateCapsuleVertices(vertexstruct.color, segments, rings);
		break;
	case ObjectType::CYLINDER:
		vertices = CreateCylinderVertices(vertexstruct.color, segments);
		break;
	case ObjectType::PLANE:
		vertices = CreatePlaneVertices(vertexstruct.color);
		break;
	case ObjectType::QUAD:
		vertices = CreateQuadVertices(vertexstruct.color);
		break;
	case ObjectType::CIRCLE:
	case ObjectType::TRIANGLE:
		vertices = CreateShapeVertices(vertexstruct.objectType == ObjectType::CIRCLE
			? ShapeType::CIRCLE : ShapeType::TRIANGLE, 0.5f, vertexstruct.color,
			[](XMFLOAT2 uv) { return uv; }, segments);
		break;
	case ObjectType::CONE:
		vertices = CreatePyramidVertices(vertexstruct.color, segments);
		break;
	case ObjectType::TETRAHEDRON:
		vertices = CreatePyramidVertices(vertexstruct.color, 3);
		break;
	case ObjectType::TRIANGULAR_PRISM:
		vertices = CreateCylinderVertices(vertexstruct.color, 3, true, true);
		break;
	case ObjectType::NONE:
	default:
		Debug::Log("ERROR: Unsupported ObjectType in CreateObjectVertex\n");
		return;
	}

	const int divisions = std::clamp(settings.FaceDivisions, 1, 16);
	if (divisions > 1 && (vertexstruct.objectType == ObjectType::CUBE ||
		vertexstruct.objectType == ObjectType::QUAD || vertexstruct.objectType == ObjectType::PLANE ||
		vertexstruct.objectType == ObjectType::TRIANGLE || vertexstruct.objectType == ObjectType::TETRAHEDRON ||
		vertexstruct.objectType == ObjectType::TRIANGULAR_PRISM))
	{
		vertices = SubdivideFaces(vertices, divisions);
	}

	const float radius = std::clamp(settings.Radius, 0.001f, 1000.0f);
	const float height = std::clamp(settings.Height, 0.001f, 1000.0f);
	XMFLOAT3 scale = { radius * 2.0f, height, radius * 2.0f };
	switch (vertexstruct.objectType)
	{
	case ObjectType::CIRCLE:
	case ObjectType::TRIANGLE:
	case ObjectType::SPHERE:
		scale.y = radius * 2.0f;
		break;
	case ObjectType::CAPSULE:
		// Height is the straight section; keep both end caps spherical.
		for (auto& vertex : vertices)
		{
			const float center = vertex.Pos.y >= 0.0f ? 0.5f : -0.5f;
			vertex.Pos.y = (vertex.Pos.y - center) * radius * 2.0f + center * height;
		}
		scale.y = 1.0f;
		break;
	case ObjectType::CUBE:
	case ObjectType::QUAD:
	case ObjectType::PLANE:
		scale = { std::clamp(settings.Width, 0.001f, 1000.0f), height,
			std::clamp(settings.Depth, 0.001f, 1000.0f) };
		break;
	default:
		break;
	}
	for (auto& vertex : vertices)
	{
		vertex.Pos = { vertex.Pos.x * scale.x, vertex.Pos.y * scale.y, vertex.Pos.z * scale.z };
		if (vertexstruct.objectType != ObjectType::CAPSULE)
			vertex.Normal = Normalize({ vertex.Normal.x / scale.x, vertex.Normal.y / scale.y, vertex.Normal.z / scale.z });
	}
	if (UploadVertices(vertexstruct.entityid, vertices))
	{
		// シーン保存時に頂点ではなく作成パラメーターを書き出せるよう記録する
		auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(vertexstruct.entityid);
		mesh.PrimitiveType = static_cast<int>(vertexstruct.objectType);
		mesh.PrimitiveColor = static_cast<int>(vertexstruct.color);
		mesh.PrimitiveRadius = settings.Radius;
		mesh.PrimitiveHeight = settings.Height;
		mesh.PrimitiveWidth = settings.Width;
		mesh.PrimitiveDepth = settings.Depth;
		mesh.PrimitiveSegments = settings.Segments;
		mesh.PrimitiveRings = settings.Rings;
		mesh.PrimitiveFaceDivisions = settings.FaceDivisions;
	}
}

namespace
{
	// 負荷削減：同じ形（頂点が完全に一致）のプリミティブは頂点バッファと LOD / メッシュレットを共有する。
	//   ・GPU 駆動のメッシュ表・DXR の BLAS（頂点バッファのアドレスで共有）・メモリが形の種類数ぶんで済む
	//   ・共有中のものを編集すると use_count > 1 なので書き換えずに新しく作る（他のエンティティに影響しない）
	static_assert(sizeof(Vertex) == sizeof(StaticModelVertex), "primitive vertex layout must match the shared CPU copy");
	unordered_map<uint64_t, vector<weak_ptr<StaticMeshData>>>& SharedPrimitiveGeometry()
	{
		static unordered_map<uint64_t, vector<weak_ptr<StaticMeshData>>> cache;
		return cache;
	}

	shared_ptr<StaticMeshData> FindSharedPrimitive(uint64_t hash, const vector<Vertex>& vertices)
	{
		auto found = SharedPrimitiveGeometry().find(hash);
		if (found == SharedPrimitiveGeometry().end()) return nullptr;
		auto& entries = found->second;
		shared_ptr<StaticMeshData> match;
		for (auto it = entries.begin(); it != entries.end();)
		{
			auto entry = it->lock();
			if (!entry) { it = entries.erase(it); continue; }
			if (!match && entry->VertexBuffer && entry->CpuVertices.size() == vertices.size() &&
				memcmp(entry->CpuVertices.data(), vertices.data(), vertices.size() * sizeof(Vertex)) == 0)
			{
				match = entry;
			}
			++it;
		}
		return match;
	}
}

bool PrimitiveGeometry::UploadVertices(EntityID entity, const vector<Vertex>& vertices)
{
    ID3D12Device* device = GraphicsDevice::GetDevice();
    if (!device || vertices.empty() || !Registry::IsAlive(entity) ||
        !ComponentManager::HasComponent<MeshComponent>(entity) || vertices.size() > UINT_MAX / sizeof(Vertex))
    {
        return false;
    }
    const UINT byteSize = static_cast<UINT>(vertices.size() * sizeof(Vertex));
    auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
    mesh.PrimitiveType = 0; // 任意の頂点。プリミティブの場合は CreateObjectVertex が直後に設定し直す
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto description = CD3DX12_RESOURCE_DESC::Buffer(byteSize);
    // 同じ形がすでにあれば共有して終わり
    const uint64_t sharedHash = HashGeometry(vertices.data(), byteSize);
    const bool shareGeometry = RenderCulling::GetSettings().SharePrimitiveGeometry;
    if (shareGeometry && vertices.size() % 3 == 0)
    {
        if (auto shared = FindSharedPrimitive(sharedHash, vertices); shared && shared != mesh.NaniteMesh)
        {
            mesh.VertexBuffer = shared->VertexBuffer;
            mesh.VertexBufferView = shared->VertexBufferView;
            mesh.VertexCount = shared->VertexCount;
            mesh.GeometryHash = sharedHash;
            CalculateGeometryBounds(vertices, mesh.LocalBoundsCenter, mesh.LocalBoundsExtents);
            mesh.HasLocalBounds = true;
            mesh.NaniteMesh = move(shared);
            GpuSceneChanges::Notify(entity, GpuSceneDirtyFlags::Mesh | GpuSceneDirtyFlags::Bounds);
            return true;
        }
    }
    ComPtr<ID3D12Resource> buffer;
    const bool reuse = mesh.NaniteMesh && mesh.NaniteMesh.use_count()==1 &&
        mesh.VertexBuffer && mesh.VertexCount==vertices.size() && GraphicsDevice::WaitForGpuIdle();
    if (reuse) buffer=mesh.VertexBuffer;
    else if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer))))
    {
        return false;
    }
    void* mapped = nullptr;
    const CD3DX12_RANGE noRead(0, 0);
    if (FAILED(buffer->Map(0, &noRead, &mapped)))
    {
        return false;
    }
    memcpy(mapped, vertices.data(), byteSize);
    buffer->Unmap(0, nullptr);
    buffer->SetName(L"Primitive or Procedural Mesh");
    mesh.VertexBuffer = move(buffer);
    mesh.VertexBufferView = { mesh.VertexBuffer->GetGPUVirtualAddress(), byteSize, sizeof(Vertex) };
    mesh.VertexCount = static_cast<UINT>(vertices.size());
    mesh.GeometryHash = HashGeometry(vertices.data(), byteSize);
    CalculateGeometryBounds(vertices, mesh.LocalBoundsCenter, mesh.LocalBoundsExtents);
    mesh.HasLocalBounds = true;
    if (vertices.size()%3==0)
    {
        auto source = reuse ? mesh.NaniteMesh : make_shared<StaticMeshData>();
        source->VertexBuffer=mesh.VertexBuffer;
        source->VertexBufferView=mesh.VertexBufferView;
        source->VertexCount=mesh.VertexCount;
        source->IndexCount=mesh.VertexCount;
        source->IndexBufferView.Format=DXGI_FORMAT_R32_UINT;
        source->MeshName="Procedural/Primitive mesh";
        source->AppliedMaterialPartId=vertices.front().Color.w;
        source->CpuVertices.clear();
        source->CpuVertices.reserve(vertices.size());
        for (const auto& vertex : vertices)
            source->CpuVertices.push_back({vertex.Pos,vertex.Normal,vertex.Tex,vertex.Color});
        source->CpuIndices.resize(vertices.size());
        iota(source->CpuIndices.begin(),source->CpuIndices.end(),0u);
        BuildPrimitiveLODs(*source);
        MeshletManager::BuildImportedMesh(*source);
        if (shareGeometry) SharedPrimitiveGeometry()[sharedHash].push_back(source);
        mesh.NaniteMesh=move(source);
    }
    else mesh.NaniteMesh.reset();
    GpuSceneChanges::Notify(entity, GpuSceneDirtyFlags::Mesh | GpuSceneDirtyFlags::Bounds);
    return true;
}

