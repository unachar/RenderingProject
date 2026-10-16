#pragma once
// ============================================================================
// EditorSceneNavigation
//  Unity のシーンビューに近いカメラ操作と表示をまとめたもの（ヘッダーのみ）。
//   ・右ドラッグ + WASD/QE : 既存の CameraSystem が処理（フライ移動）
//   ・ホイール             : 注視点までの距離に比例したズーム
//   ・中ボタンドラッグ      : パン
//   ・Alt + 左ドラッグ      : 注視点を中心にオービット
//   ・F                    : 選択中オブジェクトにフォーカス（滑らかに移動）
//   ・右上に FPS / GPU / 品質段階 のオーバーレイ
// ============================================================================
#include <imgui.h>
#include "componentmanager.h"
#include "world.h"
#include "Camera.h"
#include "performancegovernor.h"
#include "graphicsdevice.h"

namespace EditorSceneNavigation
{
	inline float& PivotDistance()
	{
		static float distance = 10.0f;
		return distance;
	}

	struct FocusAnimation
	{
		bool Active = false;
		float Time = 0.0f;
		XMFLOAT3 FromPosition{};
		XMFLOAT3 ToPosition{};
		XMFLOAT3 Forward{};
	};

	inline FocusAnimation& Focus()
	{
		static FocusAnimation animation;
		return animation;
	}

	inline XMVECTOR ForwardOf(const TransformComponent& transform)
	{
		return XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
			XMMatrixRotationRollPitchYaw(transform.Rotation.x, transform.Rotation.y, 0.0f)));
	}

	inline void Store(TransformComponent& transform, CameraComponent& camera, XMVECTOR eye, XMVECTOR forward)
	{
		XMStoreFloat3(&transform.Position, eye);
		XMStoreFloat3(&camera.Target, XMVectorAdd(eye, forward));
		camera.LockOnTarget = g_kINVALID_ENTITY;
		transform.IsDirty = true;
	}

	inline bool GetEditorCamera(EntityID& entity)
	{
		entity = Camera::GetCameraEntity();
		if (entity == g_kINVALID_ENTITY || !Registry::IsAlive(entity) ||
			!ComponentManager::HasComponent<CameraComponent>(entity) ||
			!ComponentManager::HasComponent<TransformComponent>(entity))
		{
			return false;
		}
		const auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(entity);
		return camera.AllowUserControl && !camera.IsGameCamera;
	}

	inline void FocusOn(EntityID selected)
	{
		EntityID cameraEntity;
		if (!GetEditorCamera(cameraEntity) || selected == g_kINVALID_ENTITY || !Registry::IsAlive(selected) ||
			!ComponentManager::HasComponent<TransformComponent>(selected))
		{
			return;
		}
		const auto& target = ComponentManager::GetComponentUnchecked<TransformComponent>(selected);
		XMFLOAT3 center = target.Position;
		float radius = (std::max)((std::max)(fabsf(target.Scale.x), fabsf(target.Scale.y)), fabsf(target.Scale.z));
		if (ComponentManager::HasComponent<AABBComponent>(selected))
		{
			const auto& bounds = ComponentManager::GetComponentUnchecked<AABBComponent>(selected);
			center.x += bounds.Center.x * target.Scale.x;
			center.y += bounds.Center.y * target.Scale.y;
			center.z += bounds.Center.z * target.Scale.z;
			const float ex = bounds.Extents.x * target.Scale.x;
			const float ey = bounds.Extents.y * target.Scale.y;
			const float ez = bounds.Extents.z * target.Scale.z;
			radius = sqrtf(ex * ex + ey * ey + ez * ez);
		}
		radius = (std::max)(radius, 0.25f);

		auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity);
		const auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(cameraEntity);
		const float halfFov = (std::max)(camera.Fov * 0.5f, 0.1f);
		const float distance = radius / sinf(halfFov) * 1.1f;
		const XMVECTOR forward = ForwardOf(transform);

		auto& focus = Focus();
		focus.Active = true;
		focus.Time = 0.0f;
		focus.FromPosition = transform.Position;
		XMStoreFloat3(&focus.ToPosition, XMVectorSubtract(XMLoadFloat3(&center), XMVectorScale(forward, distance)));
		XMStoreFloat3(&focus.Forward, forward);
		PivotDistance() = distance;
	}

	// シーンビュー描画の直後に呼ぶ。
	inline void Update(bool sceneHovered, EntityID selected, bool gizmoInUse)
	{
		EntityID cameraEntity;
		if (!GetEditorCamera(cameraEntity))
		{
			Focus().Active = false;
			return;
		}
		auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(cameraEntity);
		auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity);
		ImGuiIO& io = ImGui::GetIO();
		float& pivotDistance = PivotDistance();

		// F でフォーカス（Unity と同じく右ドラッグ中は無効）
		if (sceneHovered && !io.WantTextInput && !ImGui::IsAnyItemActive() &&
			!ImGui::IsMouseDown(ImGuiMouseButton_Right) && ImGui::IsKeyPressed(ImGuiKey_F, false))
		{
			FocusOn(selected);
		}

		auto& focus = Focus();
		if (focus.Active)
		{
			focus.Time += io.DeltaTime;
			const float t = (std::min)(focus.Time / 0.25f, 1.0f);
			const float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
			const XMVECTOR eye = XMVectorLerp(XMLoadFloat3(&focus.FromPosition), XMLoadFloat3(&focus.ToPosition), eased);
			Store(transform, camera, eye, XMLoadFloat3(&focus.Forward));
			if (t >= 1.0f) focus.Active = false;
		}

		if (!sceneHovered || gizmoInUse)
		{
			return;
		}

		XMVECTOR eye = XMLoadFloat3(&transform.Position);
		XMVECTOR forward = ForwardOf(transform);
		const XMMATRIX rotation = XMMatrixRotationRollPitchYaw(transform.Rotation.x, transform.Rotation.y, 0.0f);
		const XMVECTOR right = XMVector3TransformNormal(XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), rotation);
		const XMVECTOR up = XMVector3TransformNormal(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), rotation);
		const ImVec2 mouse = io.MouseDelta;

		// ホイール：注視点までの距離に比例
		if (fabsf(io.MouseWheel) > 0.001f && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
		{
			const float step = (std::max)(pivotDistance * 0.12f, 0.05f) * io.MouseWheel * (io.KeyShift ? 3.0f : 1.0f);
			eye = XMVectorAdd(eye, XMVectorScale(forward, step));
			pivotDistance = (std::max)(pivotDistance - step, 0.2f);
			focus.Active = false;
			Store(transform, camera, eye, forward);
			return;
		}

		// 中ボタン：パン
		if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) && (mouse.x != 0.0f || mouse.y != 0.0f))
		{
			const float scale = (std::max)(pivotDistance, 0.5f) * 0.0018f;
			eye = XMVectorAdd(eye, XMVectorScale(right, -mouse.x * scale));
			eye = XMVectorAdd(eye, XMVectorScale(up, mouse.y * scale));
			focus.Active = false;
			Store(transform, camera, eye, forward);
			return;
		}

		if (io.KeyAlt && (mouse.x != 0.0f || mouse.y != 0.0f))
		{
			// Alt + 左：オービット
			if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
			{
				const XMVECTOR pivot = XMVectorAdd(eye, XMVectorScale(forward, pivotDistance));
				transform.Rotation.y += mouse.x * 0.005f;
				transform.Rotation.x = (std::clamp)(transform.Rotation.x + mouse.y * 0.005f, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);
				forward = ForwardOf(transform);
				eye = XMVectorSubtract(pivot, XMVectorScale(forward, pivotDistance));
				focus.Active = false;
				Store(transform, camera, eye, forward);
				return;
			}
		}
	}

	// シーンビュー右上の統計表示
	inline void DrawOverlay(ImDrawList* drawList, ImVec2 position, ImVec2 size)
	{
		if (!drawList || size.x < 200.0f || size.y < 80.0f) return;
		char line0[96];
		char line1[96];
		char line2[96];
		const float fps = World::GetFrameRate();
		const int target = World::GetTargetFrameRate();
		snprintf(line0, sizeof(line0), "%.0f FPS  (目標 %d)", fps, target);
		snprintf(line1, sizeof(line1), "GPU %.2f ms / %.2f ms", PerformanceGovernor::GetSmoothedGpuMs(), PerformanceGovernor::GetBudgetMs());
		snprintf(line2, sizeof(line2), "品質 %s   %ux%u", PerformanceGovernor::GetTierName(PerformanceGovernor::GetTier()),
			GraphicsDevice::GetSceneWidth(), GraphicsDevice::GetSceneHeight());
		const float lineHeight = ImGui::GetTextLineHeight();
		float width = (std::max)((std::max)(ImGui::CalcTextSize(line0).x, ImGui::CalcTextSize(line1).x), ImGui::CalcTextSize(line2).x);
		const ImVec2 boxMin(position.x + size.x - width - 20.0f, position.y + 8.0f);
		const ImVec2 boxMax(position.x + size.x - 8.0f, boxMin.y + lineHeight * 3.0f + 10.0f);
		drawList->AddRectFilled(boxMin, boxMax, IM_COL32(20, 20, 20, 170), 4.0f);
		const ImU32 fpsColor = fps >= target * 0.97f ? IM_COL32(120, 220, 140, 255) :
			(fps >= target * 0.75f ? IM_COL32(240, 200, 90, 255) : IM_COL32(240, 100, 90, 255));
		drawList->AddText(ImVec2(boxMin.x + 6.0f, boxMin.y + 5.0f), fpsColor, line0);
		drawList->AddText(ImVec2(boxMin.x + 6.0f, boxMin.y + 5.0f + lineHeight), IM_COL32(220, 220, 220, 255), line1);
		drawList->AddText(ImVec2(boxMin.x + 6.0f, boxMin.y + 5.0f + lineHeight * 2.0f), IM_COL32(180, 180, 180, 255), line2);
	}
}
