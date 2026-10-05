#pragma once

#include "main.h"

struct AtmosphereParameters
{
	bool Enabled = true;
	float RayleighStrength = 0.42f;
	float MieStrength = 0.075f;
	float Density = 0.36f;
	float HeightFalloff = 0.18f;
	float Extinction = 0.22f;
	float MieG = 0.76f;
	float DistanceScale = 0.030f;
	float LightShaftStrength = 0.55f;
	float LightShaftBlur = 0.25f;
	float AmbientStrength = 0.035f;
	XMFLOAT3 RayleighColor = { 0.46f, 0.62f, 1.0f };
	XMFLOAT3 MieColor = { 1.0f, 0.82f, 0.56f };
};

class Atmosphere
{
public:
	static const AtmosphereParameters& GetParameters();
	static AtmosphereParameters& GetMutableParameters();
	static void Reset();
};
