#include "pch.h"
#include "imguimanager.h"
#include "editorwidgets.h"
#include "atmosphere.h"
#include "graphicsdevice.h"
#include "lightingresources.h"
#include "renderconfiguration.h"
#include "rendertargets.h"
#include "renderersettings.h"
#include "sun.h"
#include "texturemanager.h"
#include "world.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <DirectXCollision.h>
#include <ImGuizmo.h>



using namespace EditorWidgets;

void ImGuiManager::DrawPerformanceWindow()
{
	ImGui::SetNextWindowSize(ImVec2(260.0f, 150.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("パフォーマンス", &m_ShowPerformanceWindow, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::End();
		return;
	}

	ImGui::Text("FPS: %.1f", World::GetFrameRate());
	DrawFpsMeter();
	ImGui::Text("Frame: %.2f ms", World::GetFrameTimeMs());
	ImGui::Separator();
	const LightingResources::LightGridStats& lightStats =
		LightingResources::GetLightGridStats();
	ImGui::Text("Lights: authored %u / screen %u / GPU %u",
		lightStats.AuthoredLights,
		lightStats.OnScreenLights,
		lightStats.GpuVisibleLights);
	ImGui::Text("Physical %u / Decal %u",
		lightStats.GpuPhysicalLights,
		lightStats.GpuDecalLights);
	ImGui::Text("Tiles: %ux%u / overlap %u of %u",
		lightStats.TileCountX,
		lightStats.TileCountY,
		lightStats.MaxLightsPerTile,
		RendererSettings::GetTileLightBudget() +
		RendererSettings::GetDecalTileLightBudget());
	ImGui::Text("Geometry GBuffer: %u bit/pixel",
		RendererState::g_kGEOMETRY_GBUFFER_BITS_PER_PIXEL);
	ImGui::Text("Volumetric %u / shadowed %u / overflow %u",
		lightStats.VolumetricLights,
		lightStats.ShadowedLights,
		lightStats.OverflowedTileAssignments);
	if (lightStats.OverflowedTileAssignments > 0)
	{
		ImGui::TextColored(
			ImVec4(1.0f, 0.45f, 0.20f, 1.0f),
			"ライト重複予算を超過しています");
	}
	ImGui::Separator();

	bool vsyncEnabled = World::IsVSyncEnabled();
	if (ImGui::Checkbox("垂直同期", &vsyncEnabled))
	{
		World::SetVSyncEnabled(vsyncEnabled);
	}

	bool fixedFrameRateEnabled = World::IsFixedFrameRateEnabled();
	if (ImGui::Checkbox("FPS固定", &fixedFrameRateEnabled))
	{
		World::SetFixedFrameRateEnabled(fixedFrameRateEnabled);
	}

	int targetFrameRate = World::GetTargetFrameRate();
	ImGui::BeginDisabled(!fixedFrameRateEnabled);
	ImGui::SetNextItemWidth(120.0f);
	if (ImGui::SliderInt("目標FPS", &targetFrameRate, 15, 360))
	{
		World::SetTargetFrameRate(targetFrameRate);
	}
	ImGui::EndDisabled();

	ImGui::End();
}

void ImGuiManager::DrawRenderDebuggerWindow()
{
	ImGui::SetNextWindowSize(ImVec2(420.0f, 360.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("描画デバッガー", &m_ShowRenderDebugger))
	{
		ImGui::End();
		return;
	}

	const char* statusText;
	if (RenderConfiguration::GetRenderMode() == RenderMode::DEFERRED)
	{
		statusText = "ディファード";
	}
	else
	{
		statusText = "フォワード";
	}
	ImGui::Text("描画モード: %s", statusText);
	ImGui::Text("画面: %u x %u", GraphicsDevice::GetSceneWidth(), GraphicsDevice::GetSceneHeight());

	ImGui::SeparatorText("セクション");
	vector<TextureManager::TextureInfo> textures = TextureManager::GetLoadedTextureInfos();
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(textures.size()));
	while (clipper.Step())
	{
		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
		{
			const auto& tex = textures[i];
			ImGui::Text("#%d %ux%u %s", tex.SrvIndex, tex.Width, tex.Height, tex.Path.c_str());
		}
	}
	ImGui::End();
}

void ImGuiManager::DrawGBufferWindow()
{
	ImGui::SetNextWindowSize(ImVec2(700.0f, 560.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Gバッファ", &m_ShowGBufferWindow))
	{
		ImGui::End();
		return;
	}

	if (RenderMode::DEFERRED != RenderConfiguration::GetRenderMode())
	{
		ImGui::TextUnformatted("ディファードモードのみ");
		ImGui::End();
		return;
	}

	ImGui::BeginChild("GBufferPreview", ImVec2(0, 0), true);

	const ImVec2 previewSize(320.0f, 180.0f);
	const float cellSpacing = 12.0f;
	const int columns = 2;
	const struct
	{
		const char* Label;
		GBufferType Type;
	}
	cells[] =
	{
		{ "ベースカラー",     GBufferType::BASE_COLOR },
		{ "法線",       GBufferType::NORMAL },
		{ "深度",       GBufferType::DEPTH },
		{ "マテリアル", GBufferType::MATERIAL },
		{ "影", GBufferType::SHADOW },
		{ "大気", GBufferType::ATMOSPHERE },
		{ "ベロシティ", GBufferType::VELOCITY },
		{ "ブルーム", GBufferType::BLOOM },
	};

	const int cellCount = int(size(cells));
	const int rowCount = (cellCount + columns - 1) / columns;
	for (int row = 0; row < rowCount; ++row)
	{
		for (int col = 0; col < columns; ++col)
		{
			const int index = row * columns + col;
			if (index >= cellCount)
			{
				break;
			}

			ImGui::BeginGroup();
			ImGui::TextUnformatted(cells[index].Label);
			ImGui::Image(
				ImTextureID(RenderTargets::GetGBufferSrvHandle(cells[index].Type).ptr),
				previewSize);
			ImGui::EndGroup();

			if (col < columns - 1 && index + 1 < cellCount)
			{
				ImGui::SameLine(0.0f, cellSpacing);
			}
		}

		if (row < rowCount - 1)
		{
			ImGui::Dummy(ImVec2(0.0f, cellSpacing));
		}
	}

	ImGui::EndChild();
	ImGui::End();
}

void ImGuiManager::DrawAtmosphereWindow()
{
	ImGui::SetNextWindowSize(ImVec2(360.0f, 430.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("大気シミュレーション", &m_ShowAtmosphereWindow))
	{
		ImGui::End();
		return;
	}

	AtmosphereParameters& atmosphere = Atmosphere::GetMutableParameters();
	ImGui::Checkbox("有効", &atmosphere.Enabled);
	ImGui::SeparatorText("散乱");
	ImGui::SliderFloat("Rayleigh強度", &atmosphere.RayleighStrength, 0.0f, 4.0f, "%.3f");
	ImGui::SliderFloat("Mie強度", &atmosphere.MieStrength, 0.0f, 2.0f, "%.3f");
	ImGui::SliderFloat("大気密度", &atmosphere.Density, 0.0f, 3.0f, "%.3f");
	ImGui::SliderFloat("高度減衰", &atmosphere.HeightFalloff, 0.0f, 1.0f, "%.3f");
	ImGui::SliderFloat("消散", &atmosphere.Extinction, 0.0f, 2.0f, "%.3f");
	ImGui::SliderFloat("Mie g", &atmosphere.MieG, -0.95f, 0.95f, "%.3f");
	ImGui::SliderFloat("距離スケール", &atmosphere.DistanceScale, 0.001f, 0.25f, "%.3f");
	ImGui::SliderFloat("ライトシャフト強度", &atmosphere.LightShaftStrength, 0.0f, 4.0f, "%.3f");
	ImGui::SliderFloat("ライトシャフトぼかし", &atmosphere.LightShaftBlur, 0.0f, 1.0f, "%.3f");
	ImGui::SliderFloat("環境散乱", &atmosphere.AmbientStrength, 0.0f, 1.0f, "%.3f");

	ImGui::SeparatorText("色");
	ImGui::ColorEdit3("Rayleigh色", &atmosphere.RayleighColor.x);
	ImGui::ColorEdit3("Mie色", &atmosphere.MieColor.x);

	ImGui::SeparatorText("Sun");
	EntityID firstSun = g_kINVALID_ENTITY;
	for (EntityID entity : World::GetView<SunComponent>())
	{
		firstSun = entity;
		break;
	}

	if (firstSun == g_kINVALID_ENTITY)
	{
		if (ImGui::Button("Sunを作成"))
		{
			m_SelectedEntity = Sun::CreateDefault();
		}
	}
	else
	{
		auto& sun = ComponentManager::GetComponentUnchecked<SunComponent>(firstSun);
		auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(firstSun);
		EntitySnapshot before = CaptureEntity(firstSun);
		bool sunChanged = false;
		ImGui::Text("Sun Entity: %s", GetEntityDisplayName(firstSun));
		if (DrawAxisFloat3("位置", transform.Position, 0.1f))
		{
			transform.IsDirty = true;
			sunChanged = true;
		}
		sunChanged |= DrawAxisFloat3("注視点", sun.Target, 0.1f);
		sunChanged |= ImGui::Checkbox("Directional同期", &sun.SyncDirectionalLight);
		if (sunChanged)
		{
			BeginUndoCapture(firstSun, before);
			Sun::Sync(firstSun);
		}
		if (ImGui::Button("Sunを選択"))
		{
			m_SelectedEntity = firstSun;
		}
	}

	if (ImGui::Button("大気をリセット"))
	{
		Atmosphere::Reset();
	}

	ImGui::End();
}

void ImGuiManager::DrawLogWindow()
{
	ImGui::SetNextWindowSize(ImVec2(620.0f, 220.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("ログ", &m_ShowLogWindow))
	{
		ImGui::End();
		return;
	}

	if (ImGui::Button("クリア"))
	{
		m_Logs.clear();
	}
	ImGui::SameLine();
	ImGui::Text("件数: %d", static_cast<int>(m_Logs.size()));
	ImGui::Separator();

	ImGui::BeginChild("LogScroll", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(m_Logs.size()));
	while (clipper.Step())
	{
		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
		{
			ImGui::TextUnformatted(m_Logs[i].c_str());
		}
	}
	if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
	{
		ImGui::SetScrollHereY(1.0f);
	}
	ImGui::EndChild();
	ImGui::End();
}

void ImGuiManager::DrawMeshOutlineWindow()
{
	ImGui::SetNextWindowSize(ImVec2(760.0f, 420.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("メッシュ単位のアウトライン", &m_ShowMeshOutlineWindow))
	{
		ImGui::End();
		return;
	}

	if (m_SelectedEntity == g_kINVALID_ENTITY || !Registry::IsAlive(m_SelectedEntity))
	{
		ImGui::TextUnformatted("オブジェクト未選択");
		ImGui::End();
		return;
	}

	ImGui::Text("選択中: %s", GetEntityDisplayName(m_SelectedEntity));
	ImGui::Separator();
	if (!IsModelMaterialEntity(m_SelectedEntity))
	{
		ImGui::TextUnformatted("メッシュ付きモデルを選択してください");
		ImGui::End();
		return;
	}

	DrawToonMeshOutlineInspector(m_SelectedEntity, false);
	ImGui::End();
}

void ImGuiManager::DrawMeshShadingWindow()
{
	ImGui::SetNextWindowSize(ImVec2(760.0f, 420.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("メッシュ単位のシェーディング", &m_ShowMeshShadingWindow))
	{
		ImGui::End();
		return;
	}

	if (m_SelectedEntity == g_kINVALID_ENTITY || !Registry::IsAlive(m_SelectedEntity))
	{
		ImGui::TextUnformatted("オブジェクト未選択");
		ImGui::End();
		return;
	}

	ImGui::Text("選択中: %s", GetEntityDisplayName(m_SelectedEntity));
	ImGui::Separator();
	if (!IsModelMaterialEntity(m_SelectedEntity))
	{
		ImGui::TextUnformatted("メッシュ付きモデルを選択してください");
		ImGui::End();
		return;
	}

	DrawMeshShadingInspector(m_SelectedEntity, false);
	ImGui::End();
}
