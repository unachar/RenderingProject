#pragma once
#include "ecs.h"
#include "rendererdraw.h"
#include "componentmanager.h"

class Camera
{
private:
	inline static EntityID m_PrimaryCameraEntity = g_kINVALID_ENTITY;
public:
	static void Create(
		XMFLOAT3 target = { 0.0f, 3.f, 0.0f },
		float fov = XM_PIDIV4);

	static EntityID GetCameraEntity();
	static void GetCameraMatrices(EntityID camera, XMMATRIX& view, XMMATRIX& proj);

	static void SetCameraPostProcess(PostProcessType type);
	static PostProcessType GetCameraPostProcess();

	static void SetCameraPostProcessIntensity(float intensity);
	static float GetCameraPostProcessIntensity();
};

