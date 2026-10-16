#include "pch.h"
#include "performancegovernor.h"
#include "materialmanager.h"
#include "imguimanager.h"
#include "editorwidgets.h"
#include "graphicsdevice.h"
#include "lightingresources.h"
#include "localheightfog.h"
#include "meshshaderpipeline.h"
#include "physicssystem.h"
#include "projectmanager.h"
#include "renderconfiguration.h"
#include "renderersettings.h"
#include "systemmanager.h"
#include "texturemanager.h"
#include "world.h"
#include "atmosphere.h"
#include "simulationconfiguration.h"
#include "raytracing.h"
#include "nrddenoiser.h"
#include "optixdenoiser.h"
#include "assetthumbnailmanager.h"
#include "camera.h"
#include "configvalue.h"
#include "cloud/cloudrenderer.h"
#include "virtualgeometry/virtualgeometrytraversal.h"
#include "virtualgeometry/virtualgeometrystreaming.h"
#include "virtualgeometry/virtualgeometrybinning.h"
#include "virtualgeometry/virtualgeometrydebug.h"
#include "meshshader/meshshaderrenderer.h"
#include "visibility/gpuvisibility.h"
#include "gpudriven/gpudrivenrenderer.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <DirectXCollision.h>
#include <ImGuizmo.h>



using namespace EditorWidgets;

void ImGuiManager::DrawPhysicsSettingsWindow()
{
	ImGui::SetNextWindowSize(ImVec2(430.0f, 420.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("物理設定", &m_ShowPhysicsSettingsWindow))
	{
		ImGui::End();
		return;
	}

	PhysicsSystem* physicsSystem = SystemManager::GetSystem<PhysicsSystem>();
	if (!physicsSystem)
	{
		ImGui::TextColored(
			ImVec4(0.95f, 0.35f, 0.30f, 1.0f),
			"PhysicsSystemが初期化されていません。");
		ImGui::End();
		return;
	}

	const char* statusText;
	if (ProjectManager::IsPaused())
	{
		statusText = "Paused";
	}
	else
	{
		const char* statusTextValue;
		if (ProjectManager::IsPlaying())
		{
			statusTextValue = "Playing";
		}
		else
		{
			statusTextValue = "Static / Edit";
		}
		statusText = (statusTextValue);
	}
	ImGui::Text("状態: %s",
		statusText);
	ImGui::Text(
		"剛体: %zu / PMXリグ: %zu (Bodies %zu / Joints %zu)",
		physicsSystem->GetEntityBodyCount(),
		physicsSystem->GetBoneRigCount(),
		physicsSystem->GetBoneBodyCount(),
		physicsSystem->GetBoneJointCount());
	ImGui::Text(
		"今フレームのFixed Step: %d",
		physicsSystem->GetLastSubStepCount());

	ImGui::SeparatorText("バックエンド");
	for (int i = 0; i < 3; ++i)
	{
		const PhysicsEngine engine = static_cast<PhysicsEngine>(i);
		const bool available = physicsSystem->IsBackendAvailable(engine);
		ImVec4 statusColor;
		if (available)
		{
			statusColor = ImVec4(0.30f, 0.85f, 0.45f, 1.0f);
		}
		else
		{
			statusColor = ImVec4(0.95f, 0.35f, 0.30f, 1.0f);
		}
		const char* statusText3;
		if (available)
		{
			statusText3 = "Ready";
		}
		else
		{
			statusText3 = "Unavailable";
		}
		ImGui::TextColored(
			statusColor,
			"%s: %s",
			physicsSystem->GetBackendName(engine),
			statusText3);
	}

	PhysicsSettings& settings = physicsSystem->GetSettings();
	ImGui::SeparatorText("Fixed Update");
	int fixedFrequency = clamp(
		static_cast<int>(roundf(1.0f / max(settings.FixedTimeStep, 0.000001f))),
		1,
		1000);
	if (ImGui::SliderInt("Fixed Update (Hz)", &fixedFrequency, 15, 240))
	{
		settings.FixedTimeStep = 1.0f / static_cast<float>(fixedFrequency);
		physicsSystem->ResetSimulation();
	}
	ImGui::TextDisabled("Fixed Delta Time: %.4f ms",
		settings.FixedTimeStep * 1000.0f);
	if (ImGui::SliderInt("最大Sub Step", &settings.MaxSubSteps, 1, 16))
	{
		settings.MaxSubSteps = clamp(settings.MaxSubSteps, 1, 32);
	}
	if (ImGui::DragFloat(
		"最大蓄積時間", &settings.MaxAccumulatedTime, 0.001f, 0.001f, 1.0f, "%.3f sec"))
	{
		settings.MaxAccumulatedTime =
			max(settings.MaxAccumulatedTime, settings.FixedTimeStep);
	}
	ImGui::DragFloat("Physics Time Scale", &settings.TimeScale, 0.01f, 0.0f, 10.0f);

	ImGui::SeparatorText("ワールド");
	if (ImGui::DragFloat3("重力", &settings.Gravity.x, 0.01f))
	{
		physicsSystem->ResetSimulation();
	}
	if (ImGui::Button("物理シミュレーションを再生成"))
	{
		physicsSystem->ResetSimulation();
	}
	ImGui::TextWrapped(
		"物理は描画FPSとは独立した固定ステップで更新されます。"
		"変更したバックエンドや剛体設定はランタイム中でも再生成されます。");

	ImGui::End();
}

void ImGuiManager::DrawProjectSettingsWindow()
{
	ImGui::SetNextWindowSize(ImVec2(520.0f, 560.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("環境設定", &m_ShowProjectSettingsWindow))
	{
		ImGui::End();
		return;
	}

	if (ImGui::CollapsingHeader("システム", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextUnformatted("レンダリング API: DirectX 12");
		ImGui::Text("バックバッファ: %u x %u", GraphicsDevice::GetWidth(), GraphicsDevice::GetHeight());
		ImGui::Text("シーン解像度: %u x %u", GraphicsDevice::GetSceneWidth(), GraphicsDevice::GetSceneHeight());
		ImGui::TextUnformatted("描画パス: Deferred");
	}
	if (ImGui::CollapsingHeader("レイトレーシング", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextWrapped("%s", Raytracing::GetStatus());
		ImGui::BeginDisabled(!Raytracing::IsAvailable());
		auto& rays = Raytracing::GetSettings();
		ImGui::Checkbox("レイトレーシングを使用", &rays.Enabled);
		int rayMode = static_cast<int>(rays.RenderMode);
		const char* rayModes[] = { "反射のみ", "フルレイトレーシング" };
		if (ImGui::Combo("描画モード", &rayMode, rayModes, IM_ARRAYSIZE(rayModes)))
			rays.RenderMode = static_cast<Raytracing::Mode>(rayMode);
		ImGui::BeginDisabled(rays.RenderMode == Raytracing::Mode::ReflectionsOnly);
		ImGui::Checkbox("全ライトのレイトレーシング影", &rays.Shadows);
		ImGui::Checkbox("レイトレーシング AO", &rays.AmbientOcclusion);
		ImGui::EndDisabled();
		ImGui::Checkbox("レイトレーシング反射", &rays.Reflections);
        ImGui::Checkbox("ノイズ除去を使用", &rays.Denoise);
        ImGui::BeginDisabled(!rays.Denoise);
        int denoiser = static_cast<int>(rays.RadianceDenoiser);
        const char* denoisers[] = {"NVIDIA NRD", "NVIDIA OptiX AI (HDR)"};
        if (ImGui::Combo("GI・反射デノイザー", &denoiser, denoisers, IM_ARRAYSIZE(denoisers)))
        {
            rays.RadianceDenoiser = static_cast<Raytracing::DenoiserType>(denoiser);
        }
        if (rays.RadianceDenoiser == Raytracing::DenoiserType::Optix)
        {
            ImGui::TextWrapped("%s", OptixDenoiser::GetStatus());
            if (!OptixDenoiser::IsAvailable())
            {
                ImGui::TextWrapped("OptiXを使用できない場合はNRDへフォールバックします。");
            }
        }
        ImGui::TextDisabled("影とAOはNRDを使用します");
        ImGui::EndDisabled();
		ImGui::EndDisabled();
	}
	if (ImGui::Button("プロジェクト設定を保存")) SaveProjectSettings();
	ImGui::SameLine();
	ImGui::TextDisabled("Save/project_environment.cfg");

	if (ImGui::CollapsingHeader("表示とフレーム", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextDisabled("適用後: %u x %u",
			max(static_cast<UINT>(roundf(GraphicsDevice::GetWidth() * RenderConfiguration::GetResolutionScale())), 1u),
			max(static_cast<UINT>(roundf(GraphicsDevice::GetHeight() * RenderConfiguration::GetResolutionScale())), 1u));
		bool vsync = World::IsVSyncEnabled();
		if (ImGui::Checkbox("垂直同期", &vsync)) World::SetVSyncEnabled(vsync);
		bool fixedRate = World::IsFixedFrameRateEnabled();
		if (ImGui::Checkbox("フレームレートを固定", &fixedRate)) World::SetFixedFrameRateEnabled(fixedRate);
		int targetFps = World::GetTargetFrameRate();
		ImGui::BeginDisabled(!fixedRate);
		if (ImGui::SliderInt("目標 FPS", &targetFps, 15, 360)) World::SetTargetFrameRate(targetFps);
		ImGui::EndDisabled();
		ImGui::SeparatorText("自動品質調整");
		PerformanceGovernor::DrawImGui();
	}

	if (ImGui::CollapsingHeader("グラフィックス", ImGuiTreeNodeFlags_DefaultOpen))
	{
		DrawUpscaleControls();
		bool computeGBuffer = RendererSettings::GetComputeGBufferEnabled();
		if (ImGui::Checkbox("Visibility Buffer + Compute GBuffer", &computeGBuffer))
		{
			RendererSettings::SetComputeGBufferEnabled(computeGBuffer);
			RenderConfiguration::InvalidateScenePipelineCache();
		}
		ImGui::SeparatorText("出力");
		int aa = static_cast<int>(RendererState::m_AntiAliasingMode);
		if (ImGui::Combo("アンチエイリアス", &aa, m_antiAliasingModeItems, IM_ARRAYSIZE(m_antiAliasingModeItems)))
		{
			RendererState::m_AntiAliasingMode = static_cast<AntiAliasingMode>(aa);
		}
		if (ImGui::Checkbox("HDR シーンカラー", &m_HdrEnabled)) RenderConfiguration::SetHdr(m_HdrEnabled);
		ImGui::Checkbox("トーンマッピング", &m_ToneMapEnabled);
		ImGui::SliderFloat("露光", &m_Exposure, 0.01f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
	}

	if (ImGui::CollapsingHeader("Advanced GPU", ImGuiTreeNodeFlags_DefaultOpen))
	{
		bool meshShaders = RendererSettings::GetMeshShadersEnabled();
		ImGui::BeginDisabled(!MeshShaderPipeline::IsAvailable());
		if (ImGui::Checkbox("Mesh Shader", &meshShaders))
			RendererSettings::SetMeshShadersEnabled(meshShaders);
		ImGui::EndDisabled();
		ImGui::SameLine();
		const char* statusText4;
		if (MeshShaderPipeline::IsAvailable())
		{
			statusText4 = "対応";
		}
		else
		{
			statusText4 = "GPU 非対応: indirect fallback";
		}
		ImGui::TextDisabled(statusText4);

		bool twoPhase = RendererSettings::GetTwoPhaseOcclusionEnabled();
		if (ImGui::Checkbox("2-phase Occlusion Culling (Hi-Z)", &twoPhase))
			RendererSettings::SetTwoPhaseOcclusionEnabled(twoPhase);

		bool textureStreaming = RendererSettings::GetTextureStreamingEnabled();
		if (ImGui::Checkbox("Texture Streaming", &textureStreaming))
			RendererSettings::SetTextureStreamingEnabled(textureStreaming);
		bool reservedResources = RendererSettings::GetReservedResourcesEnabled();
		const bool reservedHardwareSupported = TextureManager::IsReservedResourceStreamingSupported();
		const bool reservedStreamingAvailable = TextureManager::IsReservedResourceStreamingAvailable();
		if (!reservedStreamingAvailable && reservedResources)
		{
			reservedResources = false;
			RendererSettings::SetReservedResourcesEnabled(false);
		}
		if (ImGui::Checkbox("Reserved Resource (64 KiB tiles)", &reservedResources))
			RendererSettings::SetReservedResourcesEnabled(reservedResources);


		ImGui::SeparatorText("Screen Space");
		bool ssao = RendererSettings::GetSsaoEnabled();
		if (ImGui::Checkbox("SSAO Visibility Bitmask", &ssao)) RendererSettings::SetSsaoEnabled(ssao);
		ImGui::BeginDisabled(!ssao);
		float aoRadius = RendererSettings::GetSsaoRadius();
		if (ImGui::SliderFloat("AO 半径", &aoRadius, 0.1f, 4.0f, "%.2f m")) RendererSettings::SetSsaoRadius(aoRadius);
		float aoPower = RendererSettings::GetSsaoPower();
		if (ImGui::SliderFloat("AO 強度", &aoPower, 0.25f, 4.0f, "%.2f")) RendererSettings::SetSsaoPower(aoPower);
		ImGui::EndDisabled();

        int giMode = static_cast<int>(RendererSettings::GetGiMode());
        const char* giModes[] = { "Off", "SSGI", "Realtime GI (DXR)" };
        if (ImGui::Combo("Global Illumination", &giMode, giModes, 3))
        {
            RendererSettings::SetGiMode(static_cast<GlobalIlluminationMode>(giMode));
        }
        ImGui::BeginDisabled(giMode == 0);
        float giIntensity = RendererSettings::GetSsgiIntensity();
        if (ImGui::SliderFloat("GI Intensity", &giIntensity, 0.0f, 3.0f, "%.2f"))
        {
            RendererSettings::SetSsgiIntensity(giIntensity);
        }
        if (RendererSettings::GetRealtimeGiEnabled())
        {
            float giDistance = RendererSettings::GetGiDistance();
            if (ImGui::SliderFloat("GI Trace Distance", &giDistance, 1.0f, 200.0f, "%.1f"))
            {
                RendererSettings::SetGiDistance(giDistance);
            }
            if (!Raytracing::IsAvailable() || !Raytracing::GetSettings().Enabled)
            {
                ImGui::TextWrapped("Realtime GI requires DXR Tier 1.1 and enabled ray tracing.");
            }
        }
        ImGui::EndDisabled();
	}

	if (ImGui::CollapsingHeader("照明予算", ImGuiTreeNodeFlags_DefaultOpen))
	{
		int screenBudget = RendererSettings::GetScreenLightBudget();
		if (ImGui::SliderInt("画面内 Physical Light", &screenBudget, 1, 32))
			RendererSettings::SetScreenLightBudget(screenBudget);
		int tileBudget = RendererSettings::GetTileLightBudget();
		if (ImGui::SliderInt("16x16 タイル重複", &tileBudget, 1, 4))
			RendererSettings::SetTileLightBudget(tileBudget);
		int decalBudget = RendererSettings::GetDecalLightBudget();
		if (ImGui::SliderInt("画面内 Decal Light", &decalBudget, 0, 110))
			RendererSettings::SetDecalLightBudget(decalBudget);
		int decalTileBudget = RendererSettings::GetDecalTileLightBudget();
		if (ImGui::SliderInt("タイル内 Decal Light", &decalTileBudget, 0, 4))
			RendererSettings::SetDecalTileLightBudget(decalTileBudget);
		int volumetricBudget = RendererSettings::GetVolumetricLightBudget();
		if (ImGui::SliderInt("Volumetric Light", &volumetricBudget, 0, 5))
			RendererSettings::SetVolumetricLightBudget(volumetricBudget);
		int shadowBudget = RendererSettings::GetShadowLightBudget();
		if (ImGui::SliderInt("Shadow map layer", &shadowBudget, 1, 8))
			RendererSettings::SetShadowLightBudget(shadowBudget);
		const int monitorTextureIndex = RenderConfiguration::GetMonitorTextureIndex();
		const vector<TextureManager::TextureInfo> textureInfos = TextureManager::GetLoadedTextureInfos();
		string monitorLabel = "白 (未指定)";
		for (const auto& info : textureInfos)
		{
			if (info.SrvIndex == monitorTextureIndex)
			{
				monitorLabel = filesystem::path(info.Path).filename().string();
				break;
			}
		}
		if (ImGui::BeginCombo("Monitor Texture", monitorLabel.c_str()))
		{
			if (ImGui::Selectable("白 (未指定)", monitorTextureIndex < 0))
			{
				RenderConfiguration::SetMonitorTextureIndex(-1);
			}
			for (const auto& info : textureInfos)
			{
				const string label = filesystem::path(info.Path).filename().string() +
					"##monitor_" + to_string(info.SrvIndex);
				if (ImGui::Selectable(label.c_str(), info.SrvIndex == monitorTextureIndex))
				{
					RenderConfiguration::SetMonitorTextureIndex(info.SrvIndex);
				}
			}
			ImGui::EndCombo();
		}
		ImGui::TextWrapped("ライトは GPU 上でタイル別 instance list と volumetric shaft list に圧縮され、ピクセル当たりの評価数はタイル重複予算を超えません。");
	}

	if (ImGui::CollapsingHeader("シャドウ", ImGuiTreeNodeFlags_DefaultOpen))
	{
		const bool raytracedShadowsSupported = Raytracing::UsesRaytracedShadows();
		if (raytracedShadowsSupported)
		{
			ImGui::TextWrapped("現在はレイトレーシングで影を描画しています。反射のみのモードでは以下のシャドウマップ設定を使用します。");
		}
		ImGui::BeginDisabled(raytracedShadowsSupported);
		int method = static_cast<int>(RendererSettings::GetShadowMapMethod());
		const char* methods[] = { "ShadowMap", "Virtual ShadowMap" };
		if (ImGui::Combo("方式", &method, methods, IM_ARRAYSIZE(methods)))
		{
			RendererSettings::SetShadowMapMethod(static_cast<ShadowMapMethod>(method));
		}

		const bool virtualMode = method == static_cast<int>(ShadowMapMethod::VirtualShadowMap);
		ImGui::BeginDisabled(virtualMode);
		int cascadeCount = RendererSettings::GetShadowCascadeCount();
		if (ImGui::SliderInt("カスケード数", &cascadeCount, 1, 4)) RendererSettings::SetShadowCascadeCount(cascadeCount);
		float shadowDistance = RendererSettings::GetShadowDistance();
		if (ImGui::SliderFloat("動的シャドウ距離", &shadowDistance, 8.0f, 512.0f, "%.0f m", ImGuiSliderFlags_Logarithmic)) RendererSettings::SetShadowDistance(shadowDistance);
		ImGui::EndDisabled();
		ImGui::BeginDisabled(!virtualMode);
		int levels = RendererSettings::GetVirtualClipmapLevels();
		if (ImGui::SliderInt("クリップマップ レベル", &levels, 1, 4)) RendererSettings::SetVirtualClipmapLevels(levels);
		float firstRadius = RendererSettings::GetVirtualFirstLevelRadius();
		if (ImGui::SliderFloat("最内周の半径", &firstRadius, 4.0f, 64.0f, "%.1f m")) RendererSettings::SetVirtualFirstLevelRadius(firstRadius);
		bool stabilize = RendererSettings::GetStabilizeVirtualClipmaps();
		if (ImGui::Checkbox("カメラ移動時に安定化", &stabilize)) RendererSettings::SetStabilizeVirtualClipmaps(stabilize);
		bool cachePages = RendererSettings::GetCacheVirtualShadowPages();
		if (ImGui::Checkbox("静的ページをキャッシュ", &cachePages)) RendererSettings::SetCacheVirtualShadowPages(cachePages);
		ImGui::Text("物理ページ: %u x %u px / %u x %u ページ / level",
			RendererState::g_kVIRTUAL_SHADOW_PAGE_SIZE,
			RendererState::g_kVIRTUAL_SHADOW_PAGE_SIZE,
			RendererState::g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION,
			RendererState::g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION);
		ImGui::Text("resident: 中央 %u x %u ページ / level",
			RendererState::g_kVIRTUAL_SHADOW_RESIDENT_PAGES_PER_DIMENSION,
			RendererState::g_kVIRTUAL_SHADOW_RESIDENT_PAGES_PER_DIMENSION);
		const char* statusText5;
		if (LightingResources::IsVirtualShadowCacheHit())
		{
			statusText5 = "HIT (再利用)";
		}
		else
		{
			statusText5 = "MISS (更新)";
		}
		ImGui::Text("キャッシュ: %s", statusText5);
		ImGui::EndDisabled();
		int debugMode = RendererSettings::GetVirtualShadowDebugMode();
		const char* debugModes[] = { "なし", "Shadow Mask", "Clipmap Level", "Virtual Page" };
		int debugModeCount;
		if (virtualMode)
		{
			debugModeCount = IM_ARRAYSIZE(debugModes);
		}
		else
		{
			debugModeCount = 2;
		}
		if (debugMode >= debugModeCount)
		{
			debugMode = 0;
			RendererSettings::SetVirtualShadowDebugMode(debugMode);
		}
		if (ImGui::Combo("シャドウ可視化", &debugMode, debugModes, debugModeCount))
		{
			RendererSettings::SetVirtualShadowDebugMode(debugMode);
		}

		int filterRadius = RendererSettings::GetShadowFilterRadius();
		if (ImGui::SliderInt("PCF フィルター半径", &filterRadius, 0, 3)) RendererSettings::SetShadowFilterRadius(filterRadius);
		float resolutionTransition = RendererSettings::GetShadowResolutionTransition();
		if (ImGui::SliderFloat("解像度遷移スケール", &resolutionTransition, 0.05f, 0.40f, "%.2f"))
		{
			RendererSettings::SetShadowResolutionTransition(resolutionTransition);
		}
		ImGui::TextDisabled("ライト空間の重複領域で解像度を連続遷移（SM/VSM 共通）");
		float depthBias = RendererSettings::GetShadowDepthBias();
		if (virtualMode)
		{
			if (ImGui::SliderFloat("深度バイアス", &depthBias, 0.000001f, 0.005f, "%.7f", ImGuiSliderFlags_Logarithmic)) RendererSettings::SetShadowDepthBias(depthBias);
		}
		else if (ImGui::SliderFloat("深度バイアス", &depthBias, 0.0f, 0.001f, "%.7f"))
		{
			RendererSettings::SetShadowDepthBias(depthBias);
		}
		float normalBias = RendererSettings::GetShadowNormalBias();
		if (virtualMode)
		{
			if (ImGui::SliderFloat("法線バイアス", &normalBias, 0.000001f, 0.003f, "%.7f", ImGuiSliderFlags_Logarithmic)) RendererSettings::SetShadowNormalBias(normalBias);
		}
		else if (ImGui::SliderFloat("法線バイアス", &normalBias, 0.0f, 0.001f, "%.7f"))
		{
			RendererSettings::SetShadowNormalBias(normalBias);
		}
		if (virtualMode)
		{
			ImGui::TextDisabled("VSM バイアスはクリップマップの texel 幅に合わせてレベルごとに拡大");
		}
		bool contactShadows = RendererSettings::GetContactShadowsEnabled();
		if (ImGui::Checkbox("Contact Shadow (スクリーンスペース)", &contactShadows)) RendererSettings::SetContactShadowsEnabled(contactShadows);
		ImGui::BeginDisabled(!contactShadows);
		float contactLength = RendererSettings::GetContactShadowLength();
		if (ImGui::SliderFloat("Contact Shadow 長", &contactLength, 0.05f, 5.0f, "%.2f m")) RendererSettings::SetContactShadowLength(contactLength);
		int contactSteps = RendererSettings::GetContactShadowSteps();
		if (ImGui::SliderInt("Contact Shadow ステップ", &contactSteps, 4, 24)) RendererSettings::SetContactShadowSteps(contactSteps);
		ImGui::EndDisabled();
		bool distanceFieldShadows = RendererSettings::GetDistanceFieldShadowsEnabled();
		if (ImGui::Checkbox("Distance Field Shadow (AABB SDF)", &distanceFieldShadows)) RendererSettings::SetDistanceFieldShadowsEnabled(distanceFieldShadows);
		ImGui::BeginDisabled(!distanceFieldShadows);
		float distanceFieldDistance = RendererSettings::GetDistanceFieldShadowDistance();
		if (ImGui::SliderFloat("SDF レイ距離", &distanceFieldDistance, 2.0f, 100.0f, "%.1f m")) RendererSettings::SetDistanceFieldShadowDistance(distanceFieldDistance);
		int distanceFieldSteps = RendererSettings::GetDistanceFieldShadowSteps();
		if (ImGui::SliderInt("SDF ステップ", &distanceFieldSteps, 4, 24)) RendererSettings::SetDistanceFieldShadowSteps(distanceFieldSteps);
		ImGui::EndDisabled();
		int activeShadowLayers;
		if (virtualMode)
		{
			activeShadowLayers = RendererSettings::GetVirtualClipmapLevels();
		}
		else
		{
			activeShadowLayers = RendererSettings::GetShadowCascadeCount();
		}
		const float estimatedMemoryMb = static_cast<float>(activeShadowLayers * RendererState::g_kSHADOW_MAP_SIZE * RendererState::g_kSHADOW_MAP_SIZE * sizeof(float)) / (1024.0f * 1024.0f);
		const float allocatedMemoryMb = static_cast<float>(RendererState::g_kMAX_SHADOW_LIGHTS * RendererState::g_kSHADOW_MAP_SIZE * RendererState::g_kSHADOW_MAP_SIZE * sizeof(float)) / (1024.0f * 1024.0f);
		ImGui::Text("使用レイヤー: %d / 有効フットプリント: %.0f MB", activeShadowLayers, estimatedMemoryMb);
		ImGui::Text("確保済み深度プール: %.0f MB", allocatedMemoryMb);
		ImGui::TextWrapped("Virtual ShadowMap はカメラ周辺に複数のクリップマップを配置し、受光点に必要な最も細かいレベルを選択します。");
		if (ImGui::Button("シャドウ設定を初期値へ戻す")) RendererSettings::ResetShadowDefaults();
		ImGui::EndDisabled();
	}

	if (ImGui::CollapsingHeader("Local Height Fog", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (ImGui::Button("Height Fog を追加")) LocalHeightFog::Add(LocalFogShape::Height);
		ImGui::SameLine();
		if (ImGui::Button("Sphere Fog を追加")) LocalHeightFog::Add(LocalFogShape::Sphere);
		ImGui::SameLine();
		ImGui::Text("%zu / %zu", LocalHeightFog::GetVolumes().size(), LocalHeightFog::MaxVolumes);

		auto& fogVolumes = LocalHeightFog::GetMutableVolumes();
		int removeIndex = -1;
		for (size_t i = 0; i < fogVolumes.size(); ++i)
		{
			auto& fog = fogVolumes[i];
			ImGui::PushID(static_cast<int>(i));
			const char* shapeName;
			if (fog.Shape == LocalFogShape::Height)
			{
				shapeName = "Local Height Fog";
			}
			else
			{
				shapeName = "Local Sphere Fog";
			}
			if (ImGui::TreeNodeEx(shapeName, ImGuiTreeNodeFlags_DefaultOpen, "%s %zu", shapeName, i + 1))
			{
				ImGui::Checkbox("有効", &fog.Enabled);
				int shape = static_cast<int>(fog.Shape);
				const char* shapes[] = { "Height", "Sphere" };
				if (ImGui::Combo("形状", &shape, shapes, IM_ARRAYSIZE(shapes))) fog.Shape = static_cast<LocalFogShape>(shape);
				ImGui::DragFloat3("位置", &fog.Position.x, 0.1f);
				ImGui::SliderFloat("半径", &fog.Radius, 0.1f, 100.0f, "%.1f m");
				ImGui::SliderFloat("密度", &fog.Density, 0.0f, 4.0f, "%.3f");
				ImGui::BeginDisabled(fog.Shape == LocalFogShape::Sphere);
				ImGui::SliderFloat("高さフォールオフ", &fog.HeightFalloff, 0.01f, 8.0f, "%.2f");
				ImGui::EndDisabled();
				ImGui::ColorEdit3("フォグ色", &fog.Color.x);
				if (ImGui::Button("削除")) removeIndex = static_cast<int>(i);
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		if (removeIndex >= 0) LocalHeightFog::Remove(static_cast<size_t>(removeIndex));
		ImGui::TextWrapped("最大16個の局所フォグを1つの定数バッファにまとめて処理します。複数配置時も描画パスは増えません。");
	}

	ImGui::End();
}

bool ImGuiManager::PersistentSettings(ostream* output, const string& key, const string& value)
{
    bool matched = false;
    const auto field = [&](const char* name, auto& setting, long double low, long double high)
    {
        matched |= EditorConfig::Value(output, key, value, name, setting, low, high);
    };
    field("editor.ShowHierarchyWindow", m_ShowHierarchyWindow, 0, 1);
    field("editor.ShowInspectorWindow", m_ShowInspectorWindow, 0, 1);
    field("editor.ShowAdjustmentPanel", m_ShowAdjustmentPanel, 0, 1);
    field("editor.ShowAssetBrowser", m_ShowAssetBrowser, 0, 1);
    field("editor.ShowRenderDebugger", m_ShowRenderDebugger, 0, 1);
    field("editor.ShowGpuSceneWindow", m_ShowGpuSceneWindow, 0, 1);
    field("editor.ShowGpuDrivenWindow", m_ShowGpuDrivenWindow, 0, 1);
    field("editor.ShowVisibilityWindow", m_ShowVisibilityWindow, 0, 1);
    field("editor.ShowMeshShaderWindow", m_ShowMeshShaderWindow, 0, 1);
    field("editor.ShowVirtualGeometryWindow", m_ShowVirtualGeometryWindow, 0, 1);
    field("editor.ShowCloudWindow", m_ShowCloudWindow, 0, 1);
    field("editor.ShowTextureListWindow", m_ShowTextureListWindow, 0, 1);
    field("editor.ShowGBufferWindow", m_ShowGBufferWindow, 0, 1);
    field("editor.ShowLogWindow", m_ShowLogWindow, 0, 1);
    field("editor.ShowPerformanceWindow", m_ShowPerformanceWindow, 0, 1);
    field("editor.ShowMaterialEditorWindow", m_ShowMaterialEditorWindow, 0, 1);
    field("editor.ShowRimSettingsWindow", m_ShowRimSettingsWindow, 0, 1);
    field("editor.ShowMeshOutlineWindow", m_ShowMeshOutlineWindow, 0, 1);
    field("editor.ShowMeshShadingWindow", m_ShowMeshShadingWindow, 0, 1);
    field("editor.ShowAtmosphereWindow", m_ShowAtmosphereWindow, 0, 1);
    field("editor.ShowProceduralWindow", m_ShowProceduralWindow, 0, 1);
    field("editor.ShowRaytracingWindow", m_ShowRaytracingWindow, 0, 1);
    field("editor.ShowProjectSettingsWindow", m_ShowProjectSettingsWindow, 0, 1);
    field("editor.ShowPhysicsSettingsWindow", m_ShowPhysicsSettingsWindow, 0, 1);
    field("editor.ShowLightDebug", m_ShowLightDebug, 0, 1);
    field("editor.gizmo", m_GizmoOperation, 0, 3);
    field("editor.log_auto_scroll", m_LogAutoScroll, 0, 1);
    auto& thumbnails = AssetThumbnailManager::Settings();
    field("asset_thumbnail_size", thumbnails.Size, 32, 256);
    field("asset_thumbnail_generations", thumbnails.MaxGenerationsPerFrame, 1, 64);
    field("asset_thumbnail_memory", thumbnails.MemoryBudget, 1048576, 4294967296.0L);
    field("asset_thumbnail_disk_cache", thumbnails.DiskCache, 0, 1);
    auto& cloud = CloudRenderer::GetSettings();
    field("cloud.Enabled", cloud.Enabled, 0, 1);
    field("cloud.CastShadows", cloud.CastShadows, 0, 1);
    field("cloud.BottomHeight", cloud.BottomHeight, -1000000, 1000000);
    field("cloud.TopHeight", cloud.TopHeight, -1000000, 1000000);
    field("cloud.Coverage", cloud.Coverage, -1000000, 1000000);
    field("cloud.Density", cloud.Density, -1000000, 1000000);
    field("cloud.BaseScale", cloud.BaseScale, -1000000, 1000000);
    field("cloud.DetailScale", cloud.DetailScale, -1000000, 1000000);
    field("cloud.DetailStrength", cloud.DetailStrength, -1000000, 1000000);
    field("cloud.LayerCount", cloud.LayerCount, 1, CloudSettings::MaxLayers);
    field("cloud.LayerEnabled", cloud.LayerEnabled, 0, 1);
    for (int index = 1; index < CloudSettings::MaxLayers; ++index)
    {
        auto& layer = cloud.Layer(index);
        const string prefix = "cloud.Layer" + std::to_string(index + 1) + ".";
        field((prefix + "Enabled").c_str(), layer.LayerEnabled, 0, 1);
        field((prefix + "BottomHeight").c_str(), layer.BottomHeight, -1000000, 1000000);
        field((prefix + "TopHeight").c_str(), layer.TopHeight, -1000000, 1000000);
        field((prefix + "Coverage").c_str(), layer.Coverage, -1000000, 1000000);
        field((prefix + "Density").c_str(), layer.Density, -1000000, 1000000);
        field((prefix + "BaseScale").c_str(), layer.BaseScale, -1000000, 1000000);
        field((prefix + "DetailScale").c_str(), layer.DetailScale, -1000000, 1000000);
        field((prefix + "DetailStrength").c_str(), layer.DetailStrength, -1000000, 1000000);
    }
    field("cloud.Extinction", cloud.Extinction, -1000000, 1000000);
    field("cloud.PhaseG", cloud.PhaseG, -1000000, 1000000);
    field("cloud.AmbientStrength", cloud.AmbientStrength, -1000000, 1000000);
    field("cloud.PowderStrength", cloud.PowderStrength, -1000000, 1000000);
    field("cloud.JitterStrength", cloud.JitterStrength, -1000000, 1000000);
    field("cloud.WindX", cloud.WindX, -1000000, 1000000);
    field("cloud.WindZ", cloud.WindZ, -1000000, 1000000);
    field("cloud.WindSpeed", cloud.WindSpeed, -1000000, 1000000);
    field("cloud.MaxTraceDistance", cloud.MaxTraceDistance, -1000000, 1000000);
    field("cloud.ResolutionScale", cloud.ResolutionScale, -1000000, 1000000);
    field("cloud.NoiseSlice", cloud.NoiseSlice, -1000000, 1000000);
    field("cloud.ViewSteps", cloud.ViewSteps, -1, 10000000);
    field("cloud.LightSteps", cloud.LightSteps, -1, 10000000);
    field("cloud.Seed", cloud.Seed, 0, 4294967295);
    field("cloud.Debug", cloud.Debug, 0, 7);
    auto& nanite = VirtualGeometryTraversal::GetSettings();
    field("nanite.Enabled", nanite.Enabled, 0, 1);
    field("nanite.TargetErrorPixels", nanite.TargetErrorPixels, -1000000, 1000000);
    field("nanite.HysteresisEnabled", nanite.HysteresisEnabled, 0, 1);
    field("nanite.TwoPassOcclusion", nanite.TwoPassOcclusion, 0, 1);
    field("nanite.HysteresisFraction", nanite.HysteresisFraction, -1000000, 1000000);
    field("nanite.ForceHierarchyLevel", nanite.ForceHierarchyLevel, -1, 10000000);
    field("nanite.FrustumCulling", nanite.FrustumCulling, 0, 1);
    field("nanite.ConeCulling", nanite.ConeCulling, 0, 1);
    field("nanite.OcclusionCulling", nanite.OcclusionCulling, 0, 1);
    field("nanite.LevelColors", nanite.LevelColors, 0, 1);
    field("nanite.DebugReadback", nanite.DebugReadback, 0, 1);
    field("nanite.View", nanite.View, 0, 11);
    field("nanite.AutoGrowBuffers", nanite.AutoGrowBuffers, 0, 1);
    field("nanite.PredictiveStreaming", nanite.PredictiveStreaming, 0, 1);
    field("nanite.PrefetchSeconds", nanite.PrefetchSeconds, -1000000, 1000000);
    field("nanite.MaxPrefetchDistance", nanite.MaxPrefetchDistance, -1000000, 1000000);
    field("nanite.MaxSelectedClusters", nanite.MaxSelectedClusters, 1, 10000000);
    field("nanite.MaxGroupStates", nanite.MaxGroupStates, 1, 10000000);
    auto& nanite_streaming = VirtualGeometryStreaming::GetSettings();
    field("nanite_streaming.BudgetMiB", nanite_streaming.BudgetMiB, 1, 1024);
    field("nanite_streaming.UploadMiBPerFrame", nanite_streaming.UploadMiBPerFrame, 0, 64);
    field("nanite_streaming.MaxPageRequestsPerFrame", nanite_streaming.MaxPageRequestsPerFrame, 1, 65536);
    field("nanite_streaming.RecentlyUsedProtectionFrames", nanite_streaming.RecentlyUsedProtectionFrames, 0, 1000);
    auto& nanite_binning = VirtualGeometryBinning::GetSettings();
    field("nanite_binning.Enabled", nanite_binning.Enabled, 0, 1);
    field("nanite_binning.MaxMaterialBins", nanite_binning.MaxMaterialBins, 1, 65536);
    auto& mesh_shader = MeshShaderRenderer::GetSettings();
    field("mesh_shader.Enabled", mesh_shader.Enabled, 0, 1);
    field("mesh_shader.Frustum", mesh_shader.Frustum, 0, 1);
    field("mesh_shader.Cone", mesh_shader.Cone, 0, 1);
    field("mesh_shader.Occlusion", mesh_shader.Occlusion, 0, 1);
    field("mesh_shader.Clusters", mesh_shader.Clusters, 0, 1);
    field("mesh_shader.Colors", mesh_shader.Colors, 0, 1);
    field("mesh_shader.DebugReadback", mesh_shader.DebugReadback, 0, 1);
    field("mesh_shader.MeshletBounds", mesh_shader.MeshletBounds, 0, 1);
    field("mesh_shader.ClusterBounds", mesh_shader.ClusterBounds, 0, 1);
    field("mesh_shader.ConeAxes", mesh_shader.ConeAxes, 0, 1);
    field("mesh_shader.Bias", mesh_shader.Bias, -1000000, 1000000);
    field("mesh_shader.MaxMeshlets", mesh_shader.MaxMeshlets, 1, 10000000);
    auto& visibility = GPUVisibility::GetSettings();
    field("visibility.Frustum", visibility.Frustum, 0, 1);
    field("visibility.Occlusion", visibility.Occlusion, 0, 1);
    field("visibility.LOD", visibility.LOD, 0, 1);
    field("visibility.DebugReadback", visibility.DebugReadback, 0, 1);
    field("visibility.Bounds", visibility.Bounds, 0, 1);
    field("visibility.ScreenRectangles", visibility.ScreenRectangles, 0, 1);
    field("visibility.FreezeCamera", visibility.FreezeCamera, 0, 1);
    field("visibility.DepthBias", visibility.DepthBias, -1000000, 1000000);
    field("visibility.LODBias", visibility.LODBias, -1000000, 1000000);
    field("visibility.ForceLOD", visibility.ForceLOD, -1, 7);
    field("visibility.DebugMode", visibility.DebugMode, 0, 16);
    field("visibility.HiZMip", visibility.HiZMip, 0, 32);
    if (auto* physics = SystemManager::GetSystem<PhysicsSystem>())
    {
        auto& settings = physics->GetSettings();
        field("physics.FixedTimeStep", settings.FixedTimeStep, 0.001, 1);
        field("physics.MaxSubSteps", settings.MaxSubSteps, 1, 32);
        field("physics.MaxAccumulatedTime", settings.MaxAccumulatedTime, 0.001, 10);
        field("physics.TimeScale", settings.TimeScale, 0, 10);
        field("physics.Gravity.x", settings.Gravity.x, -100000, 100000);
        field("physics.Gravity.y", settings.Gravity.y, -100000, 100000);
        field("physics.Gravity.z", settings.Gravity.z, -100000, 100000);
    }
    int renderMode = static_cast<int>(RenderConfiguration::GetRequestedRenderMode());
    if (EditorConfig::Value(output, key, value, "render_mode", renderMode, 0, 1))
    {
        RenderConfiguration::SetRenderMode(static_cast<RenderMode>(renderMode));
        matched = true;
    }
    bool gpuDriven = GPUDrivenRenderer::IsEnabled();
    if (EditorConfig::Value(output, key, value, "gpu_driven", gpuDriven))
    {
        GPUDrivenRenderer::SetEnabled(gpuDriven);
        matched = true;
    }
    bool readback = GPUDrivenRenderer::GetDebugReadback();
    if (EditorConfig::Value(output, key, value, "gpu_driven_debug_readback", readback))
    {
        GPUDrivenRenderer::SetDebugReadback(readback);
        matched = true;
    }
    auto& overlay = VirtualGeometryDebug::GetPreferences();
    field("nanite_overlay.HierarchyBounds", overlay.HierarchyBounds, 0, 1);
    field("nanite_overlay.Wireframe", overlay.Wireframe, 0, 1);
    field("nanite_overlay.SelectedOnly", overlay.SelectedOnly, 0, 1);
    field("nanite_overlay.HierarchyLevel", overlay.HierarchyLevel, 0, 31);
    field("nanite_overlay.SourceLOD", overlay.SourceLOD, 0, 4);
    field("nanite_overlay.Limit", overlay.Limit, 16, 4096);
    int postProcess = static_cast<int>(Camera::GetCameraPostProcess());
    if (EditorConfig::Value(output, key, value, "camera_post_process", postProcess,
        0, static_cast<int>(PostProcessType::COUNT) - 1))
    {
        Camera::SetCameraPostProcess(static_cast<PostProcessType>(postProcess));
        matched = true;
    }
    // 現在のシーン（次回起動時に開く）。日本語のパスも扱えるよう UTF-8 で保存する
    if (output)
    {
        const auto scene = m_ScenePath.generic_u8string();
        *output << "scene=" << string(scene.begin(), scene.end()) << '\n';
    }
    else if (key == "scene")
    {
        m_StartupScenePath = filesystem::u8path(value);
        matched = true;
    }
    if (output) *output << "asset_directory=" << m_CurrentAssetDirectory.generic_string() << '\n';
    else if (key == "asset_directory")
    {
        const auto relative = filesystem::path(value).lexically_normal().lexically_relative(m_AssetRoot.lexically_normal());
        error_code error;
        if (!relative.empty() && *relative.begin() != ".." && filesystem::is_directory(value, error))
            m_CurrentAssetDirectory = filesystem::path(value);
        matched = true;
    }
    return matched;
}

void ImGuiManager::SaveProjectSettings()
{
	if (!m_ProjectSettingsLoaded || ProjectManager::IsPlaying()) return;
	char validationMode[8]{};
	if (GetEnvironmentVariableA("DX12_VALIDATION_MODE", validationMode, sizeof(validationMode)) > 0 && atoi(validationMode) != 0)
	{
		return;
	}
	error_code ec;
	filesystem::create_directories("Save", ec);
	ostringstream stream;
	stream << setprecision(numeric_limits<float>::max_digits10);
	stream << "version=1\n";
	SimulationConfiguration::Save(stream);
	PersistentSettings(&stream);
	stream << "vsync=" << (static_cast<int>(World::IsVSyncEnabled())) << '\n';
	stream << "fixed_fps=" << (static_cast<int>(World::IsFixedFrameRateEnabled())) << '\n';
	stream << "target_fps=" << World::GetTargetFrameRate() << '\n';
	stream << "resolution_scale=" << RenderConfiguration::GetResolutionScale() << '\n';
	stream << "upscale_mode=" << static_cast<int>(RendererSettings::GetUpscaleMode()) << '\n';
	stream << "upscale_quality=" << static_cast<int>(RendererSettings::GetUpscaleQuality()) << '\n';
	stream << "fsr_sharpness=" << RendererSettings::GetFsrSharpness() << '\n';
	stream << "nis_sharpness=" << RendererSettings::GetNisSharpness() << '\n';
	stream << "compute_gbuffer=" << (static_cast<int>(RendererSettings::GetAuthoredComputeGBufferEnabled())) << '\n';
	stream << "mesh_shaders=" << (static_cast<int>(RendererSettings::GetMeshShadersEnabled())) << '\n';
	stream << "two_phase_occlusion=" << (static_cast<int>(RendererSettings::GetTwoPhaseOcclusionEnabled())) << '\n';
	stream << "texture_streaming=" << (static_cast<int>(RendererSettings::GetTextureStreamingEnabled())) << '\n';
	stream << "reserved_resources=" << (static_cast<int>(RendererSettings::GetReservedResourcesEnabled())) << '\n';
	stream << "ssao=" << (static_cast<int>(RendererSettings::GetSsaoEnabled())) << '\n';
	stream << "ssao_radius=" << RendererSettings::GetSsaoRadius() << '\n';
	stream << "ssao_power=" << RendererSettings::GetSsaoPower() << '\n';
	stream << "ssgi=" << (static_cast<int>(RendererSettings::GetSsgiEnabled())) << '\n';
	stream << "gi_mode=" << static_cast<int>(RendererSettings::GetGiMode()) << '\n';
    stream << "gi_distance=" << RendererSettings::GetGiDistance() << '\n';
	stream << "ssgi_intensity=" << RendererSettings::GetSsgiIntensity() << '\n';
	stream << "ray_binning=" << (static_cast<int>(RendererSettings::GetRayBinningEnabled())) << '\n';
	stream << "anti_aliasing=" << static_cast<int>(RendererState::m_AntiAliasingMode) << '\n';
	stream << "hdr=" << (static_cast<int>(m_HdrEnabled)) << '\n';
	stream << "tone_map=" << (static_cast<int>(m_ToneMapEnabled)) << '\n';
	stream << "exposure=" << m_Exposure << '\n';
	stream << "shadow_method=" << static_cast<int>(RendererSettings::GetShadowMapMethod()) << '\n';
	stream << "shadow_cascades=" << RendererSettings::GetShadowCascadeCount() << '\n';
	stream << "shadow_distance=" << RendererSettings::GetShadowDistance() << '\n';
	stream << "vsm_levels=" << RendererSettings::GetVirtualClipmapLevels() << '\n';
	stream << "vsm_first_radius=" << RendererSettings::GetVirtualFirstLevelRadius() << '\n';
	stream << "vsm_stabilize=" << (static_cast<int>(RendererSettings::GetStabilizeVirtualClipmaps())) << '\n';
	stream << "vsm_cache_pages=" << (static_cast<int>(RendererSettings::GetCacheVirtualShadowPages())) << '\n';
	stream << "vsm_debug_mode=" << RendererSettings::GetVirtualShadowDebugMode() << '\n';
	stream << "shadow_filter_radius=" << RendererSettings::GetShadowFilterRadius() << '\n';
	stream << "shadow_depth_bias=" << RendererSettings::GetConventionalShadowDepthBias() << '\n';
	stream << "shadow_normal_bias=" << RendererSettings::GetConventionalShadowNormalBias() << '\n';
	stream << "vsm_depth_bias=" << RendererSettings::GetAuthoredVirtualShadowDepthBias() << '\n';
	stream << "vsm_normal_bias=" << RendererSettings::GetAuthoredVirtualShadowNormalBias() << '\n';
	stream << "shadow_resolution_transition=" << RendererSettings::GetShadowResolutionTransition() << '\n';
	stream << "contact_shadows=" << (static_cast<int>(RendererSettings::GetContactShadowsEnabled())) << '\n';
	stream << "contact_length=" << RendererSettings::GetContactShadowLength() << '\n';
	stream << "contact_steps=" << RendererSettings::GetContactShadowSteps() << '\n';
	stream << "distance_field_shadows=" << (static_cast<int>(RendererSettings::GetDistanceFieldShadowsEnabled())) << '\n';
	stream << "distance_field_distance=" << RendererSettings::GetDistanceFieldShadowDistance() << '\n';
	stream << "distance_field_steps=" << RendererSettings::GetDistanceFieldShadowSteps() << '\n';
	stream << "screen_light_budget=" << RendererSettings::GetScreenLightBudget() << '\n';
	stream << "tile_light_budget=" << RendererSettings::GetTileLightBudget() << '\n';
	stream << "decal_light_budget=" << RendererSettings::GetDecalLightBudget() << '\n';
	stream << "decal_tile_light_budget=" << RendererSettings::GetDecalTileLightBudget() << '\n';
	stream << "volumetric_light_budget=" << RendererSettings::GetVolumetricLightBudget() << '\n';
	stream << "shadow_light_budget=" << RendererSettings::GetShadowLightBudget() << '\n';
	for (const auto& info : TextureManager::GetLoadedTextureInfos())
	{
		if (info.SrvIndex == RenderConfiguration::GetMonitorTextureIndex())
		{
			stream << "monitor_texture=" << info.Path << '\n';
			break;
		}
	}
	for (const auto& fog : LocalHeightFog::GetVolumes())
	{
		stream << "fog=" << (static_cast<int>(fog.Enabled)) << ',' << static_cast<int>(fog.Shape) << ','
			<< fog.Position.x << ',' << fog.Position.y << ',' << fog.Position.z << ','
			<< fog.Radius << ',' << fog.HeightFalloff << ',' << fog.Density << ','
			<< fog.Color.x << ',' << fog.Color.y << ',' << fog.Color.z << '\n';
	}

    const string contents = stream.str();
    if (contents == m_LastSavedProjectSettings) return;
    const filesystem::path temporary = "Save/project_environment.cfg.tmp";
    ofstream file(temporary, ios::binary | ios::trunc);
    file << contents;
    file.close();
    if (!file || !MoveFileExW(temporary.c_str(), L"Save/project_environment.cfg",
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        AddLog("設定の保存に失敗しました: Save/project_environment.cfg");
        return;
    }
    m_LastSavedProjectSettings = contents;

}

void ImGuiManager::LoadProjectSettings()
{
    string materialError;
    if (!MaterialManager::LoadSceneReferences("Save/scene_materials.json", materialError))
    {
        AddLog("Scene material load failed: %s", materialError.c_str());
    }

	ifstream stream("Save/project_environment.cfg");
	if (!stream) return;
	LocalHeightFog::Clear();
	string line;
	while (getline(stream, line))
	{
		const size_t separator = line.find('=');
		if (separator == string::npos) continue;
		const string key = line.substr(0, separator);
		const string value = line.substr(separator + 1);
		try
		{
			if (PersistentSettings(nullptr, key, value) || SimulationConfiguration::TryLoad(key, value))
			{
				continue;
			}
			if (key == "vsync") World::SetVSyncEnabled(stoi(value) != 0);
			else if (key == "fixed_fps") World::SetFixedFrameRateEnabled(stoi(value) != 0);
			else if (key == "target_fps") World::SetTargetFrameRate(stoi(value));
			else if (key == "resolution_scale") RenderConfiguration::SetResolutionScale(stof(value));
			else if (key == "upscale_mode") RendererSettings::SetUpscaleMode(static_cast<UpscaleMode>(clamp(stoi(value), 0, 3)));
			else if (key == "upscale_quality") RendererSettings::SetUpscaleQuality(static_cast<UpscaleQuality>(clamp(stoi(value), 0, 4)));
			else if (key == "fsr_sharpness") RendererSettings::SetFsrSharpness(stof(value));
			else if (key == "nis_sharpness") RendererSettings::SetNisSharpness(stof(value));
			else if (key == "compute_gbuffer") RendererSettings::SetComputeGBufferEnabled(stoi(value) != 0);
			else if (key == "mesh_shaders") RendererSettings::SetMeshShadersEnabled(stoi(value) != 0);
			else if (key == "two_phase_occlusion") RendererSettings::SetTwoPhaseOcclusionEnabled(stoi(value) != 0);
			else if (key == "texture_streaming") RendererSettings::SetTextureStreamingEnabled(stoi(value) != 0);
			else if (key == "reserved_resources") RendererSettings::SetReservedResourcesEnabled(stoi(value) != 0);
			else if (key == "ssao") RendererSettings::SetSsaoEnabled(stoi(value) != 0);
			else if (key == "ssao_radius") RendererSettings::SetSsaoRadius(stof(value));
			else if (key == "ssao_power") RendererSettings::SetSsaoPower(stof(value));
			else if (key == "ssgi") RendererSettings::SetSsgiEnabled(stoi(value) != 0);
			else if (key == "gi_mode") RendererSettings::SetGiMode(static_cast<GlobalIlluminationMode>(clamp(stoi(value), 0, 2)));
            else if (key == "gi_distance") RendererSettings::SetGiDistance(stof(value));
            else if (key == "ssgi_intensity") RendererSettings::SetSsgiIntensity(stof(value));
			else if (key == "ray_binning") RendererSettings::SetRayBinningEnabled(stoi(value) != 0);
			else if (key == "anti_aliasing") RendererState::m_AntiAliasingMode = static_cast<AntiAliasingMode>(clamp(stoi(value), 0, static_cast<int>(AntiAliasingMode::COUNT) - 1));
			else if (key == "hdr") { m_HdrEnabled = stoi(value) != 0; RenderConfiguration::SetHdr(m_HdrEnabled); }
			else if (key == "tone_map") m_ToneMapEnabled = stoi(value) != 0;
			else if (key == "exposure") m_Exposure = clamp(stof(value), 0.01f, 10.0f);
			else if (key == "shadow_method") RendererSettings::SetShadowMapMethod(static_cast<ShadowMapMethod>(clamp(stoi(value), 0, 1)));
			else if (key == "shadow_cascades") RendererSettings::SetShadowCascadeCount(stoi(value));
			else if (key == "shadow_distance") RendererSettings::SetShadowDistance(stof(value));
			else if (key == "vsm_levels") RendererSettings::SetVirtualClipmapLevels(stoi(value));
			else if (key == "vsm_first_radius") RendererSettings::SetVirtualFirstLevelRadius(stof(value));
			else if (key == "vsm_stabilize") RendererSettings::SetStabilizeVirtualClipmaps(stoi(value) != 0);
			else if (key == "vsm_cache_pages") RendererSettings::SetCacheVirtualShadowPages(stoi(value) != 0);
			else if (key == "vsm_debug_mode") RendererSettings::SetVirtualShadowDebugMode(stoi(value));
			else if (key == "shadow_filter_radius") RendererSettings::SetShadowFilterRadius(stoi(value));


			else if (key == "shadow_depth_bias") RendererSettings::SetConventionalShadowDepthBias(stof(value));
			else if (key == "shadow_normal_bias") RendererSettings::SetConventionalShadowNormalBias(stof(value));
			else if (key == "vsm_depth_bias") RendererSettings::SetVirtualShadowDepthBias(stof(value));
			else if (key == "vsm_normal_bias") RendererSettings::SetVirtualShadowNormalBias(stof(value));
			else if (key == "shadow_resolution_transition")
			{
				float transition = stof(value);


				if (transition > 0.40f) transition /= 8.0f;
				RendererSettings::SetShadowResolutionTransition(transition);
			}
			else if (key == "contact_shadows") RendererSettings::SetContactShadowsEnabled(stoi(value) != 0);
			else if (key == "contact_length") RendererSettings::SetContactShadowLength(stof(value));
			else if (key == "contact_steps") RendererSettings::SetContactShadowSteps(stoi(value));
			else if (key == "distance_field_shadows") RendererSettings::SetDistanceFieldShadowsEnabled(stoi(value) != 0);
			else if (key == "distance_field_distance") RendererSettings::SetDistanceFieldShadowDistance(stof(value));
			else if (key == "distance_field_steps") RendererSettings::SetDistanceFieldShadowSteps(stoi(value));
			else if (key == "screen_light_budget") RendererSettings::SetScreenLightBudget(stoi(value));
			else if (key == "tile_light_budget") RendererSettings::SetTileLightBudget(stoi(value));
			else if (key == "decal_light_budget") RendererSettings::SetDecalLightBudget(stoi(value));
			else if (key == "decal_tile_light_budget") RendererSettings::SetDecalTileLightBudget(stoi(value));
			else if (key == "volumetric_light_budget") RendererSettings::SetVolumetricLightBudget(stoi(value));
			else if (key == "shadow_light_budget") RendererSettings::SetShadowLightBudget(stoi(value));
			else if (key == "monitor_texture") RenderConfiguration::SetMonitorTextureIndex(TextureManager::LoadTexture(value));
			else if (key == "fog" && LocalHeightFog::GetVolumes().size() < LocalHeightFog::MaxVolumes)
			{
				vector<float> fields;
				stringstream values(value);
				string field;
				while (getline(values, field, ',')) fields.push_back(stof(field));
				if (fields.size() == 11)
				{
					LocalHeightFogVolume fog{};
					fog.Enabled = fields[0] != 0.0f;
					fog.Shape = static_cast<LocalFogShape>(clamp(static_cast<int>(fields[1]), 0, 1));
					fog.Position = { fields[2], fields[3], fields[4] };
					fog.Radius = max(fields[5], 0.1f);
					fog.HeightFalloff = max(fields[6], 0.01f);
					fog.Density = max(fields[7], 0.0f);
					fog.Color = { fields[8], fields[9], fields[10] };
					LocalHeightFog::GetMutableVolumes().push_back(fog);
				}
			}
		}
		catch (...)
		{
			Debug::Log("環境設定の値を読み込めません: %s\n", line.c_str());
		}
	}
    CloudRenderer::GetSettings().Validate();
    if (auto* physics = SystemManager::GetSystem<PhysicsSystem>()) physics->ResetSimulation();

    // 前回開いていたシーンを開く（cfg の scene=）。見つからなければ既定シーンのまま
    if (!m_StartupScenePath.empty())
    {
        error_code sceneError;
        if (filesystem::exists(m_StartupScenePath, sceneError))
        {
            LoadScene(m_StartupScenePath);
        }
        else
        {
            AddLog("前回のシーンが見つかりません: %s", m_StartupScenePath.generic_string().c_str());
        }
        m_StartupScenePath.clear();
    }

}
