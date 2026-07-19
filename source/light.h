#pragma once
#include "main.h"
#include "ecs.h"
#include <string>

enum class LightType
{
	Directional = 0,
	Point,
	Spot,
	Volume
};

enum class LightRenderMode
{
	Physical = 0,
	Decal,
	EmissionOnly
};

class Light
{
public:
	struct CreateDesc
	{
		std::string Name;
		LightType Type = LightType::Directional;
		XMFLOAT3 Position = { 0.0f, 2.5f, -2.0f };
		XMFLOAT3 Rotation = { 0.0f, 0.0f, 0.0f };
		XMFLOAT3 Scale = { 1.0f, 1.0f, 1.0f };
		XMFLOAT3 Direction = { 0.0f, -0.75f, 0.65f };
		XMFLOAT4 Color = { 1.0f, 0.96f, 0.88f, 1.0f };
		float Intensity = 1.0f;
		float Range = 8.0f;
		float InnerAngle = 18.0f;
		float OuterAngle = 32.0f;
		float VolumeDensity = 0.0f;
		int VolumeShape = 0;
		bool IsActive = true;
		bool DrawDebug = true;
		bool AffectsOpaque = true;
		bool AffectsForward = true;
		bool AffectsVolumetrics = false;


		bool CastShadow = false;
		float Priority = 0.0f;
		LightRenderMode RenderMode = LightRenderMode::Physical;
	};

	static CreateDesc MakeDefaultDesc(LightType type);
	static EntityID Create(const CreateDesc& desc);
	static EntityID Create(LightType type);
	static EntityID CreateDefaultDirectional();
	static const char* GetTypeName(LightType type);
	static void AttachLightTimeLine(EntityID entity);
};
