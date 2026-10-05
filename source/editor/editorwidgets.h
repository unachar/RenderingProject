#pragma once
#include "imguimanager.h"
#include <ImGuizmo.h>

namespace EditorWidgets
{
	inline constexpr float kRadToDeg = 180.0f / XM_PI;
	inline constexpr float kDegToRad = XM_PI / 180.0f;
	void DrawUpscaleControls();
	XMMATRIX BuildWorldMatrix(const TransformComponent& transform);
	bool DrawAxisFloat3(const char* label, float* values, float speed, float minValue = 0.0f, float maxValue = 0.0f);
	bool DrawAxisFloat3(const char* label, XMFLOAT3& value, float speed, float minValue = 0.0f, float maxValue = 0.0f);
	bool DrawMaterialPartParams(const char* label, MaterialPartParams& params);
	XMFLOAT3 ClampScale(const float* scale, bool swapScaleYZ);
	bool ShouldConvertModelByPath(const filesystem::path& path);
	XMFLOAT3 GetDefaultModelRotationByPath(const filesystem::path& path, bool isConvert);
	bool DrawScaleAxisFloat3(const char* label, XMFLOAT3& value, float speed, float minValue, float maxValue);
	void DrawFpsMeter();
	ImGuizmo::OPERATION GetGizmoOperationFromIndex(int operation);
	const char* GetGizmoOperationLabel(int operation);
	bool GetLocalAabb(EntityID entity, XMFLOAT3& center, XMFLOAT3& extents);
	bool IsModelMaterialEntity(EntityID entity);
	void ApplyLightEntityToRuntime(EntityID entity);
	void RefreshLightEntityName(EntityID entity, LightType type);
}
