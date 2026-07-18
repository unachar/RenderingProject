#pragma once
#include "ecs.h"
#include "rendererdraw.h"
#include "componentmanager.h"

class Camera
{
private:
	inline static EntityID m_EditorCameraEntity = g_kINVALID_ENTITY;
	inline static EntityID m_GameCameraEntity = g_kINVALID_ENTITY;
public:
	static void Create(
		XMFLOAT3 target = { 0.0f, 3.f, 0.0f },
		float fov = XM_PIDIV4);
	static EntityID CreateGameCamera(
		const string& name = "GameCamera",
		XMFLOAT3 position = { 5.0f, 4.0f, -10.0f },
		XMFLOAT3 target = { 0.0f, 3.0f, 0.0f },
		float fov = XM_PIDIV4,
		bool makeMain = true);

	static EntityID GetCameraEntity();
	static EntityID GetEditorCameraEntity();
	static EntityID GetGameCameraEntity();
	static void SetMainGameCamera(EntityID entity);
	static void GetCameraMatrices(EntityID camera, XMMATRIX& view, XMMATRIX& proj);

	static void SetCameraPostProcess(PostProcessType type);
	static PostProcessType GetCameraPostProcess();

	static void SetCameraPostProcessIntensity(float intensity);
	static float GetCameraPostProcessIntensity();
};

