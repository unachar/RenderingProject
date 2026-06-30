#include "pch.h"
#include "rendererdraw.h"
#include "renderercore.h"
#include "rendererutils.h"
#include "world.h"
#include "ecs.h"
#include "texturemanager.h"
#include "componentmanager.h"
#include "imguimanager.h"
#include "light.h"
#include "camera.h"

namespace
{
	struct RuntimeLightState
	{
		LightComponent Component{};
		XMFLOAT3 Position = { 0.0f, 0.0f, 0.0f };
		bool HasLight = false;
	};

	RuntimeLightState g_CachedDirectionalLight{};
	RuntimeLightState g_CachedAnyLight{};
	bool g_LightCacheValid = false;

	struct LightConstants
	{
		XMFLOAT4 LightDirection = { 0.0f, 1.0f, 0.0f, 0.0f };
		XMFLOAT4 LightColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		XMFLOAT4 LightPositionType = { 0.0f, 0.0f, 0.0f, 0.0f };
		XMFLOAT4 LightExtra = { 0.95f, 0.85f, 0.35f, 0.0f };
		XMFLOAT4 LightCount = { 0.0f, 0.0f, 0.0f, 0.0f };
		XMFLOAT4 LightDirections[RendererState::g_kMAX_SHADER_LIGHTS]{};
		XMFLOAT4 LightColors[RendererState::g_kMAX_SHADER_LIGHTS]{};
		XMFLOAT4 LightPositionTypes[RendererState::g_kMAX_SHADER_LIGHTS]{};
		XMFLOAT4 LightExtras[RendererState::g_kMAX_SHADER_LIGHTS]{};
	};

	struct MaterialPartShaderConstants
	{
		XMFLOAT4 Basic{};
		XMFLOAT4 Base{};
		XMFLOAT4 Shadow0{};
		XMFLOAT4 Shadow1{};
		XMFLOAT4 Highlight{};
		XMFLOAT4 Skin0{};
		XMFLOAT4 Skin1{};
	};

	struct PBRConstants
	{
		float Metallic = 0.0f;
		float Roughness = 0.5f;
		float Fresnel = 0.04f;
		float NormalBlend = 0.0f;
		float NormalBias = 0.0f;
		float BaseSaturation = 1.2f;
		float BaseBrightness = 1.0f;
		float ShadowThreshold = 0.44f;
		float ShadowSoftness = 0.045f;
		float ShadowStrength = 1.0f;
		float MidStrength = 1.0f;
		float LitStrength = 1.0f;
		float RimStrength = 0.45f;
		float RimThreshold = 0.70f;
		float SpecularStrength = 0.35f;
		float SpecularThreshold = 0.35f;
		float KawaiiBlend = 1.0f;
		float SkinScatterStrength = 1.0f;
		float SkinScatterWrap = 0.42f;
		float SkinBacklightStrength = 1.0f;
		float SkinRimScatterStrength = 1.0f;
		float SkinOilSpecularStrength = 1.0f;
		float SkinShadowScatter = 0.54f;
		float CastShadowThreshold = 0.28f;
		float CastShadowSoftness = 0.10f;
		float Padding[3]{};
		MaterialPartShaderConstants PartParams[kMaterialPartParamCount]{};
	};

	void WriteMaterialPartConstants(PBRConstants& constants, int index, const MaterialPartParams& params)
	{
		if (index < 0 || index >= kMaterialPartParamCount)
		{
			return;
		}

		constants.PartParams[index].Basic = XMFLOAT4(params.Metallic, params.Roughness, params.Fresnel, params.NormalBlend);
		constants.PartParams[index].Base = XMFLOAT4(params.NormalBias, params.BaseSaturation, params.BaseBrightness, params.KawaiiBlend);
		constants.PartParams[index].Shadow0 = XMFLOAT4(params.ShadowThreshold, params.ShadowSoftness, params.ShadowStrength, params.MidStrength);
		constants.PartParams[index].Shadow1 = XMFLOAT4(params.LitStrength, params.CastShadowThreshold, params.CastShadowSoftness, 0.0f);
		constants.PartParams[index].Highlight = XMFLOAT4(params.RimStrength, params.RimThreshold, params.SpecularStrength, params.SpecularThreshold);
		constants.PartParams[index].Skin0 = XMFLOAT4(params.SkinScatterStrength, params.SkinScatterWrap, params.SkinBacklightStrength, params.SkinRimScatterStrength);
		constants.PartParams[index].Skin1 = XMFLOAT4(params.SkinOilSpecularStrength, params.SkinShadowScatter, 0.0f, 0.0f);
	}

	PBRConstants BuildPBRConstantsFromMaterial(const MaterialComponent& material)
	{
		PBRConstants constants{};
		constants.Metallic = material.Metallic;
		constants.Roughness = material.Roughness;
		constants.Fresnel = material.Fresnel;
		constants.NormalBlend = material.NormalBlend;
		constants.NormalBias = material.NormalBias;
		constants.BaseSaturation = material.BaseSaturation;
		constants.BaseBrightness = material.BaseBrightness;
		constants.ShadowThreshold = material.ShadowThreshold;
		constants.ShadowSoftness = material.ShadowSoftness;
		constants.ShadowStrength = material.ShadowStrength;
		constants.MidStrength = material.MidStrength;
		constants.LitStrength = material.LitStrength;
		constants.RimStrength = material.RimStrength;
		constants.RimThreshold = material.RimThreshold;
		constants.SpecularStrength = material.SpecularStrength;
		constants.SpecularThreshold = material.SpecularThreshold;
		constants.KawaiiBlend = material.KawaiiBlend;
		constants.SkinScatterStrength = material.SkinScatterStrength;
		constants.SkinScatterWrap = material.SkinScatterWrap;
		constants.SkinBacklightStrength = material.SkinBacklightStrength;
		constants.SkinRimScatterStrength = material.SkinRimScatterStrength;
		constants.SkinOilSpecularStrength = material.SkinOilSpecularStrength;
		constants.SkinShadowScatter = material.SkinShadowScatter;
		constants.CastShadowThreshold = material.CastShadowThreshold;
		constants.CastShadowSoftness = material.CastShadowSoftness;
		for (int i = 0; i < kMaterialPartParamCount; ++i)
		{
			WriteMaterialPartConstants(constants, i, material.PartParams[i]);
		}
		return constants;
	}

	const MaterialComponent& GetDeferredLightingMaterial()
	{
		static MaterialComponent defaultMaterial{};
		EntityID fallbackEntity = g_kINVALID_ENTITY;
		EntityID litEntity = g_kINVALID_ENTITY;
		EntityID selfShadowEntity = g_kINVALID_ENTITY;
		EntityID toonEntity = g_kINVALID_ENTITY;

		for (EntityID entity : World::GetView<MaterialComponent>())
		{
			const auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
			if (material.ShaderClassMode != MaterialMode::Manual)
			{
				continue;
			}

			if (fallbackEntity == g_kINVALID_ENTITY)
			{
				fallbackEntity = entity;
			}
			if (material.ShaderClass == ShaderClass::Lit && litEntity == g_kINVALID_ENTITY)
			{
				litEntity = entity;
			}
			if (material.ShaderClass == ShaderClass::SelfShadow && selfShadowEntity == g_kINVALID_ENTITY)
			{
				selfShadowEntity = entity;
			}
			if (material.ShaderClass == ShaderClass::Toon && toonEntity == g_kINVALID_ENTITY)
			{
				toonEntity = entity;
			}
		}

		if (litEntity != g_kINVALID_ENTITY)
		{
			return ComponentManager::GetComponentUnchecked<MaterialComponent>(litEntity);
		}

		if (selfShadowEntity != g_kINVALID_ENTITY)
		{
			return ComponentManager::GetComponentUnchecked<MaterialComponent>(selfShadowEntity);
		}

		if (toonEntity != g_kINVALID_ENTITY)
		{
			return ComponentManager::GetComponentUnchecked<MaterialComponent>(toonEntity);
		}

		if (fallbackEntity != g_kINVALID_ENTITY)
		{
			return ComponentManager::GetComponentUnchecked<MaterialComponent>(fallbackEntity);
		}

		return defaultMaterial;
	}

	struct ShadowConstants
	{
		XMMATRIX LightViewProjection{};
		XMFLOAT4 ShadowMapParams = { 1.0f / RendererState::g_kSHADOW_MAP_SIZE, 0.0015f, 0.0025f, 1.0f };
	};

	XMFLOAT3 NormalizeFloat3(const XMFLOAT3& value, const XMFLOAT3& fallback)
	{
		XMVECTOR v = XMLoadFloat3(&value);
		if (XMVectorGetX(XMVector3LengthSq(v)) <= 0.000001f) return fallback;
		XMFLOAT3 result{};
		XMStoreFloat3(&result, XMVector3Normalize(v));
		return result;
	}

	void RebuildLightCache()
	{
		g_CachedDirectionalLight = {};
		g_CachedAnyLight = {};
		for (EntityID entity : World::GetView<LightComponent, TransformComponent>())
		{
			const auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
			const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
			if (!light.IsActive)
			{
				continue;
			}

			if (!g_CachedAnyLight.HasLight)
			{
				g_CachedAnyLight.Component = light;
				g_CachedAnyLight.Position = transform.Position;
				g_CachedAnyLight.HasLight = true;
			}
			if (light.Type == LightType::Directional && !g_CachedDirectionalLight.HasLight)
			{
				g_CachedDirectionalLight.Component = light;
				g_CachedDirectionalLight.Position = transform.Position;
				g_CachedDirectionalLight.HasLight = true;
			}
		}
		g_LightCacheValid = true;
	}

	const RuntimeLightState& GetCachedLightState(bool preferDirectional)
	{
		if (!g_LightCacheValid)
		{
			RebuildLightCache();
		}
		return preferDirectional ? g_CachedDirectionalLight : g_CachedAnyLight;
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

void RendererResource::BeginFrame()
{
	g_LightCacheValid = false;
}

void RendererResource::UpdateLightConstantBuffer(float deferredLightStrength)
{
	if (!m_pLightCbvDataBegin)
	{
		return;
	}

	const RuntimeLightState& runtimeLight = GetCachedLightState(false);
	LightConstants constants{};
	if (runtimeLight.HasLight)
	{
		const LightComponent& lightComponent = runtimeLight.Component;
		XMFLOAT3 lightDirection = NormalizeFloat3(lightComponent.Direction, { 0.0f, 1.0f, -1.0f });
		XMFLOAT4 lightColor = lightComponent.Color;
		lightColor.w = deferredLightStrength * lightComponent.Intensity;

		constants.LightDirection = XMFLOAT4(lightDirection.x, lightDirection.y, lightDirection.z, lightComponent.Range);
		constants.LightColor = lightColor;
		constants.LightPositionType = XMFLOAT4(runtimeLight.Position.x, runtimeLight.Position.y, runtimeLight.Position.z, static_cast<float>(lightComponent.Type));
		const float innerRad = lightComponent.InnerAngle * XM_PI / 180.0f;
		const float outerRad = lightComponent.OuterAngle * XM_PI / 180.0f;
		constants.LightExtra = XMFLOAT4(cosf(innerRad), cosf(outerRad), lightComponent.VolumeDensity, static_cast<float>(lightComponent.VolumeShape));
	}
	else
	{
		constants.LightDirection = XMFLOAT4(0.0f, 1.0f, -1.0f, 12.0f);
		constants.LightColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 0.0f);
		constants.LightPositionType = XMFLOAT4(0.0f, 0.0f, 0.0f, static_cast<float>(LightType::Directional));
		constants.LightExtra = XMFLOAT4(cosf(18.0f * XM_PI / 180.0f), cosf(32.0f * XM_PI / 180.0f), 0.35f, 0.0f);
	}

	UINT lightCount = 0;
	for (EntityID entity : World::GetView<LightComponent, TransformComponent>())
	{
		if (lightCount >= g_kMAX_SHADER_LIGHTS)
		{
			break;
		}

		const auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
		if (!light.IsActive)
		{
			continue;
		}

		const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
		const XMFLOAT3 direction = NormalizeFloat3(light.Direction, { 0.0f, 1.0f, -1.0f });
		XMFLOAT4 color = light.Color;
		color.w = deferredLightStrength * light.Intensity;
		const float innerRad = light.InnerAngle * XM_PI / 180.0f;
		const float outerRad = light.OuterAngle * XM_PI / 180.0f;

		constants.LightDirections[lightCount] = XMFLOAT4(direction.x, direction.y, direction.z, light.Range);
		constants.LightColors[lightCount] = color;
		constants.LightPositionTypes[lightCount] = XMFLOAT4(transform.Position.x, transform.Position.y, transform.Position.z, static_cast<float>(light.Type));
		constants.LightExtras[lightCount] = XMFLOAT4(cosf(innerRad), cosf(outerRad), light.VolumeDensity, static_cast<float>(light.VolumeShape));
		++lightCount;
	}
	constants.LightCount = XMFLOAT4(static_cast<float>(lightCount), 0.0f, 0.0f, 0.0f);
	memcpy(m_pLightCbvDataBegin, &constants, sizeof(constants));

	// Update PBR constants
	if (m_pPBRCbvDataBegin)
	{
		PBRConstants pbrConstants = BuildPBRConstantsFromMaterial(GetDeferredLightingMaterial());
		memcpy(m_pPBRCbvDataBegin, &pbrConstants, sizeof(pbrConstants));
	}
}

void RendererResource::UpdateShadowConstantBuffer()
{
	if (!m_pShadowCbvDataBegin)
	{
		return;
	}

	const RuntimeLightState& runtimeLight = GetCachedLightState(false);
	XMFLOAT3 lightDirection = runtimeLight.HasLight ? runtimeLight.Component.Direction : XMFLOAT3(0.25f, 1.0f, -0.25f);
	XMVECTOR dir = XMVectorSet(lightDirection.x, lightDirection.y, lightDirection.z, 0.0f);
	if (XMVectorGetX(XMVector3LengthSq(dir)) < 0.000001f)
	{
		dir = XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f);
	}
	dir = XMVector3Normalize(dir);

	XMVECTOR target = XMVectorSet(0.0f, 1.2f, 0.0f, 1.0f);
	XMVECTOR characterFoot = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
	Entity alicia = World::GetEntityByName("Alicia");
	if (alicia.IsValid() &&
		Registry::IsAlive(alicia.GetID()) &&
		ComponentManager::HasComponent<TransformComponent>(alicia.GetID()))
	{
		const auto& aliciaTransform = ComponentManager::GetComponentUnchecked<TransformComponent>(alicia.GetID());
		characterFoot = XMVectorSet(aliciaTransform.Position.x, aliciaTransform.Position.y, aliciaTransform.Position.z, 1.0f);
		target = XMVectorSet(aliciaTransform.Position.x, aliciaTransform.Position.y + 0.6f, aliciaTransform.Position.z, 1.0f);
	}

	Entity field = World::GetEntityByName("field");
	if (field.IsValid() &&
		Registry::IsAlive(field.GetID()) &&
		ComponentManager::HasComponent<TransformComponent>(field.GetID()) &&
		ComponentManager::HasComponent<AABBComponent>(field.GetID()))
	{
		const auto& fieldTransform = ComponentManager::GetComponentUnchecked<TransformComponent>(field.GetID());
		const XMVECTOR fieldCenter = XMVectorSet(fieldTransform.Position.x, fieldTransform.Position.y, fieldTransform.Position.z, 1.0f);
		target = XMVectorLerp(fieldCenter, target, 0.55f);
	}

	const LightType lightType = runtimeLight.HasLight ? runtimeLight.Component.Type : LightType::Directional;
	XMVECTOR lightPos = XMVectorAdd(target, XMVectorScale(dir, 45.0f));
	float nearClip = 0.1f;
	float farClip = 1000.0f;
	float orthoSize = 20.0f;
	float fovY = XM_PIDIV2;

	if (runtimeLight.HasLight && lightType != LightType::Directional)
	{
		lightPos = XMLoadFloat3(&runtimeLight.Position);
		const XMVECTOR lowerBodyTarget = XMVectorLerp(characterFoot, target, 0.35f);
		XMVECTOR toTarget = XMVectorSubtract(lowerBodyTarget, lightPos);
		if (XMVectorGetX(XMVector3LengthSq(toTarget)) > 0.000001f)
		{
			dir = XMVector3Normalize(toTarget);
			target = lowerBodyTarget;
		}
		else
		{
			target = XMVectorAdd(lightPos, XMVectorScale(dir, 1.0f));
		}

		nearClip = 0.05f;
		const float distanceToTarget = XMVectorGetX(XMVector3Length(XMVectorSubtract(target, lightPos)));
		farClip = max(runtimeLight.Component.Range, distanceToTarget + 8.0f);
		float outerAngle = runtimeLight.Component.OuterAngle * XM_PI / 180.0f;
		if (lightType == LightType::Point)
		{
			fovY = XM_PIDIV2;
		}
		else
		{
			fovY = outerAngle * 2.0f;
			if (fovY < 0.70f) fovY = 0.70f;
			if (fovY > XM_PIDIV2) fovY = XM_PIDIV2;
		}
	}

	XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	if (fabsf(XMVectorGetX(XMVector3Dot(dir, up))) > 0.96f)
	{
		up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
	}

	const XMMATRIX lightView = XMMatrixLookAtLH(lightPos, target, up);
	const XMMATRIX lightProjection = (lightType == LightType::Directional)
		? XMMatrixOrthographicLH(orthoSize, orthoSize, nearClip, farClip)
		: XMMatrixPerspectiveFovLH(fovY, 1.0f, nearClip, farClip);

	ShadowConstants constants{};
	constants.LightViewProjection = XMMatrixTranspose(lightView * lightProjection);
	constants.ShadowMapParams = XMFLOAT4(1.0f / static_cast<float>(g_kSHADOW_MAP_SIZE), 0.000008f, 0.00001f, 1.0f);
	memcpy(m_pShadowCbvDataBegin, &constants, sizeof(constants));
}

void RendererResource::CreateSpriteVertex(const VertexResource& vertexstruct)
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

	ID3D12Device* device = RendererCore::GetDevice();
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
}

void RendererResource::CreateObjectVertex(const VertexResource& vertexstruct)
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

	ID3D12Device* device = RendererCore::GetDevice();
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
}

void RendererResource::SetMaterial(const EntityID entityID, const MaterialComponent& material)
{
	if (!m_pPBRCbvDataBegin || !m_PBRConstantBuffer || !m_CommandList)
	{
		return;
	}

	const UINT slot = entityID < g_kMAX_ENTITIES ? entityID + 1 : 0;
	PBRConstants constants = BuildPBRConstantsFromMaterial(material);

	auto* dst = static_cast<UINT8*>(m_pPBRCbvDataBegin) + slot * g_kPBR_CB_ALIGNED_SIZE;
	memcpy(dst, &constants, sizeof(constants));
	m_CommandList->SetGraphicsRootConstantBufferView(3, m_PBRConstantBuffer->GetGPUVirtualAddress() + slot * g_kPBR_CB_ALIGNED_SIZE);
}

