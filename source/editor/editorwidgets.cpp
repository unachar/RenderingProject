#include "pch.h"
#include "imguimanager.h"
#include "editorwidgets.h"
#include "modelmanager.h"
#include "world.h"
#include "renderconfiguration.h"
#include "renderersettings.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <DirectXCollision.h>
#include <ImGuizmo.h>



namespace EditorWidgets
{
	void DrawSearchField(const char* id, const char* hint, ImGuiTextFilter& filter)
	{
		ImGui::PushID(id);
		const float clearWidth = ImGui::GetFrameHeight();
		ImGui::SetNextItemWidth(max(1.0f, ImGui::GetContentRegionAvail().x - clearWidth - ImGui::GetStyle().ItemSpacing.x));
		if (ImGui::InputTextWithHint("##Search", hint, filter.InputBuf, IM_ARRAYSIZE(filter.InputBuf))) filter.Build();
		// ImGui returns on the first positive match. Evaluate exclusions first,
		// so "Light,-Spot" and "-Spot,Light" have the same meaning.
		if (filter.Filters.Size > 1)
		{
			std::stable_partition(filter.Filters.begin(), filter.Filters.end(),
				[](const ImGuiTextFilter::ImGuiTextRange& range) { return *range.b == '-'; });
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("複数条件: カンマ区切り / 除外: -語句");
		ImGui::SameLine();
		ImGui::BeginDisabled(!filter.IsActive());
		if (ImGui::Button("X", ImVec2(clearWidth, 0.0f))) filter.Clear();
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("検索をクリア");
		ImGui::PopID();
	}

	void DrawUpscaleControls()
	{
		const char* modes[] = { "Bilateral", "AMD FidelityFX FSR 1", "NVIDIA Image Scaling" };
		int mode = static_cast<int>(RendererSettings::GetUpscaleMode());
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::Combo("アップスケーラー", &mode, modes, IM_ARRAYSIZE(modes)))
		{
			RendererSettings::SetUpscaleMode(static_cast<UpscaleMode>(mode));
			RendererState::m_TaaFrameIndex = 0;
			if (mode == static_cast<int>(UpscaleMode::Nis) && RenderConfiguration::GetResolutionScale() < 0.5f)
			{
				RenderConfiguration::SetResolutionScale(0.5f);
				RendererSettings::SetUpscaleQuality(UpscaleQuality::Custom);
			}
		}

		const bool vendorUpscaler = mode != static_cast<int>(UpscaleMode::Bilateral);
		UpscaleQuality quality = RendererSettings::GetUpscaleQuality();
		if (vendorUpscaler)
		{
			const char* qualities[] = { "Ultra Quality (1.3x)", "Quality (1.5x)", "Balanced (1.7x)", "Performance (2.0x)", "Custom" };
			int qualityIndex = static_cast<int>(quality);
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::Combo("品質", &qualityIndex, qualities, IM_ARRAYSIZE(qualities)))
			{
				quality = static_cast<UpscaleQuality>(qualityIndex);
				RendererSettings::SetUpscaleQuality(quality);
				if (quality != UpscaleQuality::Custom)
				{
					RenderConfiguration::SetResolutionScale(RendererSettings::GetUpscaleQualityScale(quality));
				}
			}
		}

		float resolutionScale = RenderConfiguration::GetResolutionScale();
		int resolutionPercent = static_cast<int>(roundf(resolutionScale * 100.0f));
		ImGui::BeginDisabled(vendorUpscaler && quality != UpscaleQuality::Custom);
		int minimumResolutionPercent;
		if (mode == static_cast<int>(UpscaleMode::Nis))
		{
			minimumResolutionPercent = 50;
		}
		else
		{
			minimumResolutionPercent = 25;
		}
		if (ImGui::SliderInt("内部解像度", &resolutionPercent, minimumResolutionPercent, 100, "%d%%"))
		{
			RenderConfiguration::SetResolutionScale(static_cast<float>(resolutionPercent) / 100.0f);
			if (vendorUpscaler) RendererSettings::SetUpscaleQuality(UpscaleQuality::Custom);
		}
		ImGui::EndDisabled();

		if (mode == static_cast<int>(UpscaleMode::Fsr1))
		{
			float sharpness = RendererSettings::GetFsrSharpness();
			if (ImGui::SliderFloat("RCAS シャープネス", &sharpness, 0.0f, 2.0f, "%.2f stops"))
			{
				RendererSettings::SetFsrSharpness(sharpness);
			}
			ImGui::TextDisabled("EASU + RCAS / 入力に FXAA を自動適用");
		}
		else if (mode == static_cast<int>(UpscaleMode::Nis))
		{
			float sharpness = RendererSettings::GetNisSharpness();
			if (ImGui::SliderFloat("NIS シャープネス", &sharpness, 0.0f, 1.0f, "%.2f"))
			{
				RendererSettings::SetNisSharpness(sharpness);
			}
			ImGui::TextDisabled("6-tap scaler + directional sharpening / 50-100%% / 入力に FXAA を自動適用");
		}
	}

	XMMATRIX BuildWorldMatrix(const TransformComponent& transform)
	{
		return XMMatrixScaling(transform.Scale.x, transform.Scale.y, transform.Scale.z) *
			XMMatrixRotationX(transform.Rotation.x) *
			XMMatrixRotationY(transform.Rotation.y) *
			XMMatrixRotationZ(transform.Rotation.z) *
			XMMatrixTranslation(transform.Position.x, transform.Position.y, transform.Position.z);
	}

	bool DrawAxisFloat3(const char* label, float* values, float speed, float minValue, float maxValue)
	{
		bool changed = false;
		ImGui::PushID(label);
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(3.0f, 2.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 2.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3.0f, 2.0f));
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.025f, 0.027f, 0.030f, 1.00f));
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.050f, 0.055f, 0.064f, 1.00f));
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.070f, 0.082f, 0.100f, 1.00f));
		if (ImGui::BeginTable("AxisFloat3", 4, ImGuiTableFlags_SizingStretchProp))
		{
			const char* axisIds[3] = { "##R", "##G", "##B" };
			const ImU32 axisColors[3] =
			{
				ImGui::GetColorU32(ImVec4(0.86f, 0.18f, 0.14f, 1.00f)),
				ImGui::GetColorU32(ImVec4(0.24f, 0.68f, 0.25f, 1.00f)),
				ImGui::GetColorU32(ImVec4(0.18f, 0.42f, 0.86f, 1.00f))
			};

			ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 82.0f);
			ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("G", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("B", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			{
				const float width = ImGui::GetContentRegionAvail().x;
				const float height = ImGui::GetFrameHeight();
				ImGui::InvisibleButton("##AxisLabel", ImVec2(width, height));

				ImDrawList* drawList = ImGui::GetWindowDrawList();
				const ImVec2 min = ImGui::GetItemRectMin();
				const ImVec2 max = ImGui::GetItemRectMax();
				drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImVec4(0.095f, 0.100f, 0.108f, 1.00f)), 2.0f);
				drawList->AddRect(min, max, ImGui::GetColorU32(ImVec4(0.18f, 0.19f, 0.20f, 1.00f)), 2.0f);

				const float textY = min.y + (height - ImGui::GetTextLineHeight()) * 0.5f;
				drawList->AddText(ImVec2(min.x + 8.0f, textY), ImGui::GetColorU32(ImGuiCol_Text), label);

				const float arrowX = max.x - 13.0f;
				const float arrowY = min.y + height * 0.5f - 1.0f;
				drawList->AddTriangleFilled(
					ImVec2(arrowX - 4.0f, arrowY - 2.0f),
					ImVec2(arrowX + 4.0f, arrowY - 2.0f),
					ImVec2(arrowX, arrowY + 3.0f),
					ImGui::GetColorU32(ImVec4(0.62f, 0.66f, 0.70f, 1.00f)));
			}

			for (int i = 0; i < 3; ++i)
			{
				ImGui::TableNextColumn();
				const float height = ImGui::GetFrameHeight();
				ImGui::Dummy(ImVec2(3.0f, height));
				ImGui::GetWindowDrawList()->AddRectFilled(
					ImGui::GetItemRectMin(),
					ImGui::GetItemRectMax(),
					axisColors[i],
					1.0f);
				ImGui::SameLine(0.0f, 2.0f);
				ImGui::SetNextItemWidth(-FLT_MIN);
				changed |= ImGui::DragFloat(axisIds[i], &values[i], speed, minValue, maxValue, "%.3f");
			}
			ImGui::EndTable();
		}
		ImGui::PopStyleColor(3);
		ImGui::PopStyleVar(3);
		ImGui::PopID();
		return changed;
	}

	bool DrawAxisFloat3(const char* label, XMFLOAT3& value, float speed, float minValue, float maxValue)
	{
		float values[3] = { value.x, value.y, value.z };
		if (!DrawAxisFloat3(label, values, speed, minValue, maxValue))
		{
			return false;
		}

		value = { values[0], values[1], values[2] };
		return true;
	}

	bool DrawMaterialPartParams(const char* label, MaterialPartParams& params)
	{
		bool changed = false;
		ImGui::PushID(label);
		if (ImGui::TreeNode(label))
		{
			changed |= ImGui::SliderFloat("メタリック", &params.Metallic, 0.0f, 1.0f);
			changed |= ImGui::SliderFloat("ラフネス", &params.Roughness, 0.0f, 1.0f);
			changed |= ImGui::SliderFloat("フレネル", &params.Fresnel, 0.0f, 1.0f);
			changed |= ImGui::SliderFloat("法線ブレンド", &params.NormalBlend, 0.0f, 1.0f);
			changed |= ImGui::SliderFloat("法線バイアス", &params.NormalBias, -1.0f, 1.0f);
			changed |= ImGui::SliderFloat("ベース彩度", &params.BaseSaturation, 0.0f, 3.0f);
			changed |= ImGui::SliderFloat("ベース明度", &params.BaseBrightness, 0.0f, 3.0f);
			changed |= ImGui::SliderFloat("かわいいブレンド", &params.KawaiiBlend, 0.0f, 1.0f);

			if (ImGui::TreeNode("影"))
			{
				changed |= ImGui::SliderFloat("影しきい値", &params.ShadowThreshold, 0.0f, 1.0f);
				changed |= ImGui::SliderFloat("影ぼかし", &params.ShadowSoftness, 0.0f, 0.5f);
				changed |= ImGui::SliderFloat("影の強さ", &params.ShadowStrength, 0.0f, 2.0f);
				changed |= ImGui::SliderFloat("中間色の強さ", &params.MidStrength, 0.0f, 2.0f);
				changed |= ImGui::SliderFloat("明部の強さ", &params.LitStrength, 0.0f, 2.0f);
				changed |= ImGui::SliderFloat("影しきい値", &params.CastShadowThreshold, 0.0f, 1.0f);
				changed |= ImGui::SliderFloat("影ぼかし", &params.CastShadowSoftness, 0.0f, 0.5f);
				ImGui::TreePop();
			}

			if (ImGui::TreeNode("ハイライト"))
			{
				changed |= ImGui::SliderFloat("リム強度", &params.RimStrength, 0.0f, 2.0f);
				changed |= ImGui::SliderFloat("リムしきい値", &params.RimThreshold, 0.0f, 1.0f);
				changed |= ImGui::SliderFloat("リムぼかし", &params.RimSoftness, 0.001f, 0.50f, "%.3f");
				changed |= ImGui::SliderFloat("リムカーブ", &params.RimPower, 0.05f, 6.0f, "%.2f");
				changed |= ImGui::ColorEdit3("リム色", &params.RimColor.x, ImGuiColorEditFlags_Float);
				changed |= ImGui::SliderFloat("ベース色混合", &params.RimAlbedoBlend, 0.0f, 1.0f);
				changed |= ImGui::SliderFloat("ライト色混合", &params.RimLightBlend, 0.0f, 1.0f);
				changed |= ImGui::SliderFloat("スペキュラ強度", &params.SpecularStrength, 0.0f, 2.0f);
				changed |= ImGui::SliderFloat("スペキュラしきい値", &params.SpecularThreshold, 0.0f, 1.0f);
				ImGui::TreePop();
			}

			if (ImGui::TreeNode("肌"))
			{
				changed |= ImGui::SliderFloat("肌散乱強度", &params.SkinScatterStrength, 0.0f, 3.0f);
				changed |= ImGui::SliderFloat("肌散乱ラップ", &params.SkinScatterWrap, 0.0f, 1.0f);
				changed |= ImGui::SliderFloat("肌逆光強度", &params.SkinBacklightStrength, 0.0f, 3.0f);
				changed |= ImGui::SliderFloat("肌リム散乱強度", &params.SkinRimScatterStrength, 0.0f, 3.0f);
				changed |= ImGui::SliderFloat("スペキュラ強度", &params.SkinOilSpecularStrength, 0.0f, 3.0f);
				changed |= ImGui::SliderFloat("肌影散乱", &params.SkinShadowScatter, 0.0f, 1.0f);
				ImGui::TreePop();
			}

			ImGui::TreePop();
		}
		ImGui::PopID();
		return changed;
	}

	XMFLOAT3 ClampScale(const float* scale, bool swapScaleYZ)
	{
		float scaleAxis;
		if (swapScaleYZ)
		{
			scaleAxis = scale[2];
		}
		else
		{
			scaleAxis = scale[1];
		}
		float scaleAxisValue;
		if (swapScaleYZ)
		{
			scaleAxisValue = scale[1];
		}
		else
		{
			scaleAxisValue = scale[2];
		}
		return
		{
			max(scale[0], 0.001f),
			max(scaleAxis, 0.001f),
			max(scaleAxisValue, 0.001f)
		};
	}

	bool ShouldConvertModelByPath(const filesystem::path& path)
	{
		string lower = path.generic_string();
		transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c)
			{
				return static_cast<char>(tolower(c));
			});


		if (lower.find("xbot") != string::npos)
		{
			return false;
		}

		if (lower.find("gusoku") != string::npos)
		{
			return false;
		}

		if (lower.find("tree") != string::npos)
		{
			return false;
		}

		if (lower.find("kacchatta_hone") != string::npos)
		{
			return false;
		}

		if (lower.find("alicia") != string::npos)
		{
			return true;
		}

		if (lower.find("moca") != string::npos)
		{
			return true;
		}


		return true;
	}

	XMFLOAT3 GetDefaultModelRotationByPath(const filesystem::path& path, bool isConvert)
	{
		string lower = path.generic_string();
		transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c)
			{
				return static_cast<char>(tolower(c));
			});

		if (lower.find("moca") != string::npos)
		{
			return { XM_PIDIV2, 0.0f, 0.0f };
		}

		if (lower.find("kacchatta_hone") != string::npos)
		{
			return { 0.0f, 0.0f, 0.0f };
		}

		if (filesystem::path(lower).extension() == ".vrm")
		{
			return { 0.0f, 0.0f, 0.0f };
		}

		if (lower.find("xbot") != string::npos)
		{
			return { 0.0f, XMConvertToRadians(160.0f), 0.0f };
		}

		if (isConvert)
		{
			return XMFLOAT3(XM_PIDIV2, 0.0f, 0.0f);
		}
		return XMFLOAT3(0.0f, 0.0f, 0.0f);
	}

	bool DrawScaleAxisFloat3(const char* label, XMFLOAT3& value, float speed, float minValue, float maxValue)
	{
		float values[3] = { value.x, value.y, value.z };
		if (!DrawAxisFloat3(label, values, speed, minValue, maxValue))
		{
			return false;
		}

		value = { values[0], values[1], values[2] };
		return true;
	}

	void DrawFpsMeter()
	{
		const float fps = World::GetFrameRate();
		const int targetFrameRate = World::GetTargetFrameRate();
		const float referenceFps = static_cast<float>(max(targetFrameRate, 60));
		float fraction;
		if (referenceFps > 0.0f)
		{
			fraction = min(fps / referenceFps, 1.0f);
		}
		else
		{
			fraction = 0.0f;
		}
		char overlay[32]{};
		snprintf(overlay, sizeof(overlay), "%.0f / %.0f FPS", fps, referenceFps);
		const float overlayWidth = ImGui::CalcTextSize(overlay).x;
		const float barWidth = max(80.0f, ImGui::GetContentRegionAvail().x - overlayWidth - 10.0f);

		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.18f, 0.68f, 0.82f, 1.00f));
		ImGui::ProgressBar(fraction, ImVec2(barWidth, 7.0f), "");
		ImGui::PopStyleColor();
		ImGui::SameLine();
		ImGui::TextDisabled("%s", overlay);
	}

	ImGuizmo::OPERATION GetGizmoOperationFromIndex(int operation)
	{
		switch (operation)
		{
		case 1:
			return ImGuizmo::ROTATE;
		case 2:
			return ImGuizmo::SCALE;
		case 0:
		default:
			return ImGuizmo::TRANSLATE;
		}
	}

	const char* GetGizmoOperationLabel(int operation)
	{
		switch (operation)
		{
		case 1:
			return "回転";
		case 2:
			return "スケール";
		case 0:
		default:
			return "移動";
		}
	}

	bool GetLocalAabb(EntityID entity, XMFLOAT3& center, XMFLOAT3& extents)
	{
		if (ComponentManager::HasComponent<AABBComponent>(entity))
		{
			const auto& aabb = ComponentManager::GetComponentUnchecked<AABBComponent>(entity);
			center = aabb.Center;
			extents = aabb.Extents;
			return true;
		}

		if (ComponentManager::HasComponent<StaticModelComponent>(entity))
		{
			const auto& model = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
			if (auto* resource = ModelManager::GetStaticModel(model.ModelId))
			{
				center = resource->GetAabbCenter();
				extents = resource->GetAabbExtents();
				return true;
			}
		}

		if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
		{
			const auto& model = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
			if (auto* resource = ModelManager::GetAnimModel(model.ModelId))
			{
				center = resource->GetAabbCenter();
				extents = resource->GetAabbExtents();
			}
			else
			{
				center = { 0.0f, 1.0f, 0.0f };
				extents = { 0.7f, 1.8f, 0.7f };
			}
			return true;
		}

		if (ComponentManager::HasComponent<SpriteComponent>(entity))
		{
			center = { 0.0f, 0.0f, 0.0f };
			extents = { 1.0f, 1.0f, 0.05f };
			return true;
		}

		return false;
	}

	bool IsModelMaterialEntity(EntityID entity)
	{
		return ComponentManager::HasComponent<StaticModelComponent>(entity) ||
			ComponentManager::HasComponent<AnimationModelComponent>(entity);
	}

	void ApplyLightEntityToRuntime(EntityID entity)
	{
		if (entity == g_kINVALID_ENTITY ||
			!Registry::IsAlive(entity) ||
			!ComponentManager::HasComponent<LightComponent>(entity) ||
			!ComponentManager::HasComponent<TransformComponent>(entity))
		{
			return;
		}

		auto& lightComponent = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
		if (!lightComponent.IsActive)
		{
			return;
		}

		const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
		lightComponent.Position = transform.Position;

	}

	void RefreshLightEntityName(EntityID entity, LightType type)
	{
		if (entity == g_kINVALID_ENTITY ||
			!Registry::IsAlive(entity) ||
			!ComponentManager::HasComponent<NameComponent>(entity))
		{
			return;
		}

		auto& name = ComponentManager::GetComponentUnchecked<NameComponent>(entity);
		const char* typeName = "Unknown";
		switch (type)
		{
		case LightType::Directional: typeName = "Directional"; break;
		case LightType::Point: typeName = "Point"; break;
		case LightType::Spot: typeName = "Spot"; break;
		case LightType::Volume: typeName = "Volume"; break;
		}
		name.Name = string(typeName) + " Light";
		World::RegisterName(entity, name.Name);
	}


}
