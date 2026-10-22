#include "pch.h"
#include "performancegovernor.h"
#include "advancedeffects.h"
#include "atmospherelut.h"
#include "renderquality.h"
#include "megalights.h"
#include "cpuculling.h"
#include "transparencyeffects.h"
#include "pbrluts.h"
#include "asynccompute.h"
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
#include "rayreflections.h"
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
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.30f, 1.0f), "PhysicsSystemが初期化されていません。");
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
    ImGui::Text("状態: %s", statusText);
    ImGui::Text("剛体: %zu / PMXリグ: %zu (Bodies %zu / Joints %zu)", physicsSystem->GetEntityBodyCount(),
                physicsSystem->GetBoneRigCount(), physicsSystem->GetBoneBodyCount(),
                physicsSystem->GetBoneJointCount());
    ImGui::Text("今フレームのFixed Step: %d", physicsSystem->GetLastSubStepCount());

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
        ImGui::TextColored(statusColor, "%s: %s", physicsSystem->GetBackendName(engine), statusText3);
    }

    PhysicsSettings& settings = physicsSystem->GetSettings();
    ImGui::SeparatorText("Fixed Update");
    int fixedFrequency = clamp(static_cast<int>(roundf(1.0f / max(settings.FixedTimeStep, 0.000001f))), 1, 1000);
    if (ImGui::SliderInt("Fixed Update (Hz)", &fixedFrequency, 15, 240))
    {
        settings.FixedTimeStep = 1.0f / static_cast<float>(fixedFrequency);
        physicsSystem->ResetSimulation();
    }
    ImGui::TextDisabled("Fixed Delta Time: %.4f ms", settings.FixedTimeStep * 1000.0f);
    if (ImGui::SliderInt("最大Sub Step", &settings.MaxSubSteps, 1, 16))
    {
        settings.MaxSubSteps = clamp(settings.MaxSubSteps, 1, 32);
    }
    if (ImGui::DragFloat("最大蓄積時間", &settings.MaxAccumulatedTime, 0.001f, 0.001f, 1.0f, "%.3f sec"))
    {
        settings.MaxAccumulatedTime = max(settings.MaxAccumulatedTime, settings.FixedTimeStep);
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
    ImGui::TextWrapped("物理は描画FPSとは独立した固定ステップで更新されます。"
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
        const char* rayModes[] = {"反射のみ", "フルレイトレーシング"};
        if (ImGui::Combo("描画モード", &rayMode, rayModes, IM_ARRAYSIZE(rayModes)))
        {
            rays.RenderMode = static_cast<Raytracing::Mode>(rayMode);
        }
        ImGui::BeginDisabled(rays.RenderMode == Raytracing::Mode::ReflectionsOnly);
        ImGui::Checkbox("全ライトのレイトレーシング影", &rays.Shadows);
        ImGui::Checkbox("レイトレーシング AO", &rays.AmbientOcclusion);
        ImGui::EndDisabled();
        ImGui::Checkbox("レイトレーシング反射", &rays.Reflections);
        ImGui::Checkbox("ノイズ除去を使用", &rays.Denoise);
        ImGui::Checkbox("NRDの時間・安定化設定を固定", &rays.StableNrdProfile);
        ImGui::Checkbox("GI・影・反射の低偏差サンプリング", &rays.StableSampling);
        if (ImGui::TreeNode("加速構造 (BLAS)"))
        {
            ImGui::Checkbox("静的 BLAS をコンパクション", &rays.CompactStaticBlas);
            ImGui::SliderInt("変形 BLAS の再構築間隔 (リフィット回数)", &rays.DeformingBlasRebuildInterval, 0, 240);
            ImGui::SliderInt("1 フレームの再構築上限", &rays.DeformingBlasRebuildsPerFrame, 1, 16);
            ImGui::Checkbox("BLAS 頂点を 16bit 化 (R16G16B16A16_SNORM)", &rays.QuantizeBlasVertices);
            ImGui::BeginDisabled(!rays.QuantizeBlasVertices);
            float errorMillimeters = rays.BlasQuantizationMaxError * 1000.0f;
            if (ImGui::SliderFloat("量子化誤差の上限", &errorMillimeters, 0.05f, 5.0f, "%.2f mm"))
            {
                rays.BlasQuantizationMaxError = errorMillimeters / 1000.0f;
            }
            ImGui::EndDisabled();
            const auto blas = Raytracing::GetBlasStatistics();
            ImGui::Text("BLAS: %u / 16bit: %u / コンパクション済み: %u", blas.BottomLevels, blas.Quantized,
                        blas.Compacted);
            ImGui::Text("BLAS メモリ: %.1f MB (削減 %.1f MB)", static_cast<double>(blas.ResultBytes) / (1024.0 * 1024.0),
                        static_cast<double>(blas.CompactionSavedBytes) / (1024.0 * 1024.0));
            ImGui::Text("このフレーム: リフィット %u / フルビルド %u", blas.RefitsThisFrame, blas.RebuildsThisFrame);
            ImGui::TextWrapped("統計は直近にレイトレーシングを実行したフレームスロットの値です。");
            ImGui::TreePop();
        }
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
    if (ImGui::Button("プロジェクト設定を保存"))
    {
        SaveProjectSettings();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Save/project_environment.cfg");

    if (ImGui::CollapsingHeader("表示とフレーム", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled(
            "適用後: %u x %u",
            max(static_cast<UINT>(roundf(GraphicsDevice::GetWidth() * RenderConfiguration::GetResolutionScale())), 1u),
            max(static_cast<UINT>(roundf(GraphicsDevice::GetHeight() * RenderConfiguration::GetResolutionScale())),
                1u));
        bool vsync = World::IsVSyncEnabled();
        if (ImGui::Checkbox("垂直同期", &vsync))
        {
            World::SetVSyncEnabled(vsync);
        }
        bool fixedRate = World::IsFixedFrameRateEnabled();
        if (ImGui::Checkbox("フレームレートを固定", &fixedRate))
        {
            World::SetFixedFrameRateEnabled(fixedRate);
        }
        int targetFps = World::GetTargetFrameRate();
        ImGui::BeginDisabled(!fixedRate);
        if (ImGui::SliderInt("目標 FPS", &targetFps, 15, 360))
        {
            World::SetTargetFrameRate(targetFps);
        }
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
        if (ImGui::Checkbox("HDR シーンカラー", &m_HdrEnabled))
        {
            RenderConfiguration::SetHdr(m_HdrEnabled);
        }
        ImGui::Checkbox("トーンマッピング", &m_ToneMapEnabled);
        ImGui::SliderFloat("露光", &m_Exposure, 0.01f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    }

    if (ImGui::CollapsingHeader("Advanced GPU", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool meshShaders = RendererSettings::GetMeshShadersEnabled();
        ImGui::BeginDisabled(!MeshShaderPipeline::IsAvailable());
        if (ImGui::Checkbox("Mesh Shader", &meshShaders))
        {
            RendererSettings::SetMeshShadersEnabled(meshShaders);
        }
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
        {
            RendererSettings::SetTwoPhaseOcclusionEnabled(twoPhase);
        }

        bool textureStreaming = RendererSettings::GetTextureStreamingEnabled();
        if (ImGui::Checkbox("Texture Streaming", &textureStreaming))
        {
            RendererSettings::SetTextureStreamingEnabled(textureStreaming);
        }
        bool reservedResources = RendererSettings::GetReservedResourcesEnabled();
        const bool reservedHardwareSupported = TextureManager::IsReservedResourceStreamingSupported();
        const bool reservedStreamingAvailable = TextureManager::IsReservedResourceStreamingAvailable();
        if (!reservedStreamingAvailable && reservedResources)
        {
            reservedResources = false;
            RendererSettings::SetReservedResourcesEnabled(false);
        }
        if (ImGui::Checkbox("Reserved Resource (64 KiB tiles)", &reservedResources))
        {
            RendererSettings::SetReservedResourcesEnabled(reservedResources);
        }
        auto& compression = TextureCompression::GetSettings();
        ImGui::Checkbox("取り込み時に BC7/BC5/BC4 へ圧縮 (キャッシュ)", &compression.Enabled);
        ImGui::BeginDisabled(!compression.Enabled);
        ImGui::Checkbox("未キャッシュ時はバックグラウンドで圧縮", &compression.BackgroundForSynchronousLoads);
        ImGui::EndDisabled();
        {
            const auto stats = TextureCompression::GetStatistics();
            ImGui::TextDisabled("キャッシュ使用 %u / 新規圧縮 %u / 対象外 %u / 待機中 %u", stats.CacheHits,
                                stats.Compressed, stats.Skipped, stats.BackgroundPending);
            if (stats.SourceBytes > 0)
            {
                ImGui::TextDisabled("新規圧縮分: %.1f MB → %.1f MB", static_cast<double>(stats.SourceBytes) / 1048576.0,
                                    static_cast<double>(stats.CompressedBytes) / 1048576.0);
            }
            ImGui::TextDisabled("保存先: Data/Cache/Textures（読み込み済みテクスチャには次回読み込みから反映）");
        }

        ImGui::SeparatorText("Screen Space");
        bool ssao = RendererSettings::GetSsaoEnabled();
        if (ImGui::Checkbox("SSAO Visibility Bitmask", &ssao))
        {
            RendererSettings::SetSsaoEnabled(ssao);
        }
        ImGui::BeginDisabled(!ssao);
        float aoRadius = RendererSettings::GetSsaoRadius();
        if (ImGui::SliderFloat("AO 半径", &aoRadius, 0.1f, 4.0f, "%.2f m"))
        {
            RendererSettings::SetSsaoRadius(aoRadius);
        }
        float aoPower = RendererSettings::GetSsaoPower();
        if (ImGui::SliderFloat("AO 強度", &aoPower, 0.25f, 4.0f, "%.2f"))
        {
            RendererSettings::SetSsaoPower(aoPower);
        }
        ImGui::EndDisabled();

        int giMode = static_cast<int>(RendererSettings::GetGiMode());
        const char* giModes[] = {"Off", "SSGI", "Realtime GI (DXR)", "Lumen-style GI (DXR)"};
        if (ImGui::Combo("Global Illumination", &giMode, giModes, 4))
        {
            RendererSettings::SetGiMode(static_cast<GlobalIlluminationMode>(giMode));
        }
        ImGui::BeginDisabled(giMode == 0);
        if (RendererSettings::GetLumenEnabled())
        {
            int captures = RendererSettings::GetLumenCaptureBudget();
            int lighting = RendererSettings::GetLumenLightingBudget();
            int spacing = RendererSettings::GetLumenProbeSpacing();
            if (ImGui::SliderInt("材質キャプチャ数 / frame", &captures, 1, 16))
            {
                RendererSettings::SetLumenCaptureBudget(captures);
            }
            if (ImGui::SliderInt("照明更新メッシュ数 / frame", &lighting, 1, 32))
            {
                RendererSettings::SetLumenLightingBudget(lighting);
            }
            if (ImGui::SliderInt("プローブ間隔 (px)", &spacing, 4, 16))
            {
                RendererSettings::SetLumenProbeSpacing(spacing);
            }
            int debug = RendererSettings::GetLumenDebugView();
            const char* views[] = {"Lighting", "Probe coverage", "Surface cache coverage"};
            if (ImGui::Combo("Lumen view", &debug, views, 3))
            {
                RendererSettings::SetLumenDebugView(debug);
            }
            ImGui::Text("Surface cache: %u / 256 meshes, %.1f MiB", RayReflections::GetLumenResidentMeshes(),
                        RayReflections::GetLumenMemoryBytes() / (1024.0 * 1024.0));
            ImGui::TextWrapped("DXR hardware required. Uncached surfaces use hit lighting.");
        }
        if (RendererSettings::GetRealtimeGiEnabled())
        {
            // 物理ベースの放射照度なので 1.0 がエネルギー保存（SSGI の強度とは別管理）
            float rtGiIntensity = RendererSettings::GetRealtimeGiIntensity();
            if (ImGui::SliderFloat("GI Intensity", &rtGiIntensity, 0.0f, 3.0f, "%.2f"))
            {
                RendererSettings::SetRealtimeGiIntensity(rtGiIntensity);
            }
        }
        else
        {
            float giIntensity = RendererSettings::GetSsgiIntensity();
            if (ImGui::SliderFloat("GI Intensity", &giIntensity, 0.0f, 3.0f, "%.2f"))
            {
                RendererSettings::SetSsgiIntensity(giIntensity);
            }
        }
        ImGui::EndDisabled();

        {
            auto& rq = RenderQuality::Get();
            ImGui::SeparatorText("PBR / Lights");
            ImGui::BeginDisabled(!PbrLuts::IsAvailable());
            ImGui::Checkbox("多重散乱エネルギー補償 (DFG LUT)", &rq.EnergyCompensation);
            ImGui::Checkbox("矩形エリアライト (LTC)", &rq.AreaLights);
            ImGui::EndDisabled();
            if (!PbrLuts::IsAvailable())
            {
                ImGui::TextDisabled("LUT 未初期化: 解析近似にフォールバック");
            }

            ImGui::SeparatorText("負荷削減（カリング / LOD）");
            {
                auto& culling = RenderCulling::GetSettings();
                const auto& cullStats = RenderCulling::GetStats();
                ImGui::Checkbox("オブジェクトの視錐台カリング", &culling.ObjectFrustumCulling);
                ImGui::Checkbox("小さいオブジェクトを描かない", &culling.ObjectSizeCulling);
                ImGui::BeginDisabled(!culling.ObjectSizeCulling);
                ImGui::SliderFloat("最小サイズ (px)", &culling.MinObjectPixels, 0.25f, 8.0f, "%.2f");
                ImGui::EndDisabled();
                ImGui::SliderFloat("描画距離 (m, 0 = 無制限)", &culling.MaxDrawDistance, 0.0f, 2000.0f, "%.0f");
                ImGui::Checkbox("静止物の Velocity を省く", &culling.StaticVelocitySkip);
                ImGui::Checkbox("自動 LOD（LOD 未設定のモデルにも適用）", &culling.AutoLod);
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "GPU 駆動の描画で、インポート時に生成した LOD を画面サイズで切り替えます。\n切り替えはモデルの再読み込み / シーンを開き直したときに反映されます。");
                }
                ImGui::Checkbox("同じ形のプリミティブを共有", &culling.SharePrimitiveGeometry);
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("シーンを開き直したときに反映されます。");
                }
                ImGui::Checkbox("ライトの視錐台カリング", &culling.LightFrustumCulling);
                ImGui::Checkbox("ライトの距離 / サイズ カリング", &culling.LightDistanceCulling);
                ImGui::BeginDisabled(!culling.LightDistanceCulling);
                ImGui::SliderFloat("ライトの最大距離 (m)", &culling.LightMaxDistance, 0.0f, 2000.0f, "%.0f");
                ImGui::SliderFloat("ライトの最小サイズ (px)", &culling.LightMinPixels, 0.0f, 32.0f, "%.1f");
                ImGui::EndDisabled();
                ImGui::Checkbox("ライトのオクルージョンカリング", &culling.LightOcclusionCulling);
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "MegaLights のグリッド（GPU）で「見えている不透明面に届かない」と判定されたライトを、\n"
                        "シャドウマップと定数バッファの割り当てから外します（MegaLights 有効時）。");
                }
                ImGui::Checkbox("DXR 影モードではローカルライトのシャドウマップを描かない",
                                &culling.SkipRayTracedShadowMaps);
                ImGui::SliderFloat("反射レイの粗さ上限", &culling.ReflectionRoughnessCutoff, 0.2f, 1.0f, "%.2f");
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("これより粗い面は反射レイを飛ばさず空で近似（1.0 で無効）");
                }
                ImGui::SliderInt("反射 / GI のヒット点のライト数", &culling.RayHitLights, 4, 160);
                ImGui::Checkbox("反射を半解像度（チェッカーボード）でトレース", &culling.ReflectionCheckerboard);
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("反射レイを半分のピクセルだけ飛ばし、NRD が残りを復元します（NRD 使用時のみ）。");
                }
                ImGui::TextDisabled("オブジェクト: 判定 %u / 視錐台外 %u / 小さい・遠い %u / Velocity 省略 %u",
                                    cullStats.ObjectsTested, cullStats.ObjectsFrustumCulled,
                                    cullStats.ObjectsSizeCulled, cullStats.VelocitySkipped);
                ImGui::TextDisabled("ライト: 判定 %u / 視錐台外 %u / 遠い・小さい %u / 隠れている %u（GPU 判定 %u）",
                                    cullStats.LightsTested, cullStats.LightsFrustumCulled,
                                    cullStats.LightsDistanceCulled, cullStats.LightsOccluded,
                                    MegaLights::GetOccludedLightCount());
                ImGui::TextDisabled("省いたシャドウマップ: %u", cullStats.ShadowMapsSkipped);
            }

            ImGui::SeparatorText("MegaLights");
            {
                auto& mega = MegaLights::GetSettings();
                const auto& stats = MegaLights::GetStats();
                ImGui::BeginDisabled(!MegaLights::IsAvailable());
                ImGui::Checkbox("MegaLights グリッドを使う", &mega.Enabled);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::SetTooltip(
                        "ディファード時、Point / Spot / Rect ライトを 16px タイル × 32 スライスのセルごとに評価します。\n"
                        "タイルあたり 8 灯・画面 160 灯の制限を受けません（最大 %u 灯）。\n"
                        "シャドウマップを持つライトは従来の経路のままです。",
                        MegaLights::kMaxLights);
                }
                ImGui::BeginDisabled(!mega.Enabled);

                const char* modes[] = {"M1: 全ライトを評価", "M2: 確率的サンプリング + 影レイ",
                                       "リファレンス（検証用・重い）"};
                ImGui::Combo("評価方式", &mega.EvaluationMode, modes, IM_ARRAYSIZE(modes));
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "M2 / リファレンスは PBR 面のみ（Toon / Lit などは M1 のまま）。レイトレーシングの TLAS が必要です。\n"
                        "M2 はデノイズ前なのでノイズが乗るのが正常です（M3 でデノイズ）。");
                }
                ImGui::BeginDisabled(mega.EvaluationMode != 1);
                int sppIndex = mega.SamplesPerPixel >= 4 ? 2 : mega.SamplesPerPixel >= 2 ? 1 : 0;
                const char* sppNames[] = {"1", "2", "4"};
                if (ImGui::Combo("サンプル数 / ピクセル", &sppIndex, sppNames, IM_ARRAYSIZE(sppNames)))
                {
                    mega.SamplesPerPixel = 1 << sppIndex;
                }
                ImGui::Checkbox("半解像度サンプリング", &mega.HalfResolution);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::SetTooltip(
                        "サンプル（選択・影レイ）を画面の 1/2 で作り、2x2 の近傍を深度・法線で重み付けして使います。\n"
                        "サンプルと影レイが 1/4 になる代わりに、細い物体や境界で影がにじみます（近似。RMSE の検証はオフで）。");
                }
                ImGui::SliderFloat("一様混合率 α", &mega.UniformMix, 0.01f, 1.0f, "%.2f");
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "見積もりに比例した選択に、一様な選択を混ぜる割合。\n見積もりが外れたライトでも選ばれる確率が 0 にならない（不偏性を保つ）。");
                }
                ImGui::EndDisabled();
                ImGui::BeginDisabled(mega.EvaluationMode != 2);
                ImGui::SliderInt("リファレンスの影レイ数", &mega.ReferenceRays, 1, 64);
                ImGui::EndDisabled();

                ImGui::BeginDisabled(mega.EvaluationMode != 1);
                ImGui::Checkbox("可視ライトハッシュでガイド (M4)", &mega.Guide);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::SetTooltip(
                        "8x8 タイルごとに前フレームで影レイが通ったライトを覚え、遮られているライトを選びにくくします。\n"
                        "確率に入れているので偏りは出ません（リファレンスとの RMSE で確認できます）。");
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("ハッシュをリセット"))
                {
                    MegaLights::RequestHashReset();
                }
                ImGui::BeginDisabled(!mega.Guide);
                ImGui::SliderFloat("載っていないライトの重み", &mega.GuideMinWeight, 0.01f, 1.0f, "%.2f");
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "小さいほど遮られたライトに無駄なレイを使わないが、遮りが外れたときの反応が遅くなる（0 にはできない）。");
                }
                ImGui::SliderFloat("ハッシュを残す確率", &mega.GuideKeep, 0.0f, 1.0f, "%.3f");
                ImGui::EndDisabled();
                ImGui::EndDisabled();

                ImGui::BeginDisabled(mega.EvaluationMode != 1);
                ImGui::Checkbox("スクリーントレース (M5)", &mega.ScreenTrace);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::SetTooltip(
                        "ワールドトレースの前に深度バッファを短くマーチし、遮蔽が見つかったサンプルはレイを飛ばしません。\n"
                        "TLAS にない細部の接地影も拾えます。厚みの仮定による近似なので、RMSE の検証はオフで行ってください。");
                }
                if (mega.ScreenTrace && ImGui::TreeNode("スクリーントレースの詳細"))
                {
                    ImGui::SliderInt("ステップ数", &mega.ScreenTraceSteps, 1, 32);
                    ImGui::SliderFloat("最大距離 (px)", &mega.ScreenTraceMaxPixels, 4.0f, 512.0f, "%.0f");
                    ImGui::SliderFloat("厚み (m)", &mega.ScreenTraceThickness, 0.01f, 2.0f, "%.2f");
                    ImGui::SliderFloat("自己遮蔽よけ (px)", &mega.ScreenTraceSelfPixels, 0.0f, 8.0f, "%.1f");
                    ImGui::TreePop();
                }
                ImGui::EndDisabled();

                ImGui::BeginDisabled(mega.EvaluationMode != 1 || !MegaLights::IsDenoiseAvailable());
                ImGui::Checkbox("デノイズ (M3)", &mega.Denoise);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::SetTooltip(
                        "時間方向の蓄積 + À-Trous（SVGF 方式）。照明を材質の反射率で割ってからぼかすので、テクスチャはぼけません。\n"
                        "検証（RMSE）はデノイズを切って行ってください（デノイズは偏りを持ちます）。");
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("履歴をリセット"))
                {
                    MegaLights::RequestHistoryReset();
                }
                {
                    const char* denoisers[] = {"自前 (SVGF)", "NRD (REBLUR)"};
                    ImGui::BeginDisabled(!mega.Denoise);
                    ImGui::Combo("デノイザー (M6)", &mega.Denoiser, denoisers, IM_ARRAYSIZE(denoisers));
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    {
                        ImGui::SetTooltip(
                            "NRD は NVIDIA NRD が組み込まれたビルドで、レイトレーシングが NRD のガイドを書いているときに使えます。\n"
                            "使えないフレームは自動で自前（SVGF）に戻ります。");
                    }
                }
                if (mega.Denoise && mega.Denoiser == 0 && ImGui::TreeNode("デノイズの詳細"))
                {
                    ImGui::SliderInt("À-Trous の回数", &mega.DenoiseIterations, 0, 5);
                    ImGui::SliderInt("履歴の上限 (フレーム)", &mega.MaxHistory, 1, 128);
                    ImGui::SliderInt("ライト変化時の履歴の上限", &mega.AntiLagHistory, 1, 32);
                    ImGui::Checkbox("ファイアフライ抑制", &mega.FireflySuppression);
                    ImGui::Checkbox("1 回目の結果を履歴へ戻す", &mega.HistoryFeedback);
                    ImGui::SliderFloat("色の最小ブレンド率", &mega.AlphaMin, 0.01f, 1.0f, "%.3f");
                    ImGui::SliderFloat("モーメントの最小ブレンド率", &mega.MomentsAlphaMin, 0.01f, 1.0f, "%.3f");
                    ImGui::SliderFloat("履歴の深度許容 (相対)", &mega.DepthTolerance, 0.005f, 0.5f, "%.3f");
                    ImGui::SliderFloat("履歴の法線許容 (cos)", &mega.NormalThreshold, 0.0f, 0.999f, "%.3f");
                    ImGui::SliderFloat("法線の鋭さ", &mega.PhiNormal, 1.0f, 256.0f, "%.0f");
                    ImGui::SliderFloat("輝度の許容 (σ)", &mega.PhiLuminance, 0.5f, 32.0f, "%.1f");
                    ImGui::SliderFloat("平面距離の許容 (px)", &mega.PhiPlane, 0.1f, 16.0f, "%.1f");
                    ImGui::TreePop();
                }
                ImGui::EndDisabled();

                const char* debugViews[] = {
                    "なし",     "セルのライト数", "奥行きスライス",     "選ばれたライト (M2)",     "可視率 (M2)",
                    "1/p (M2)", "累積平均",       "リファレンスとの差", "可視ライトハッシュ (M4)", "影レイの内訳 (M5)"};
                ImGui::Combo("デバッグ表示##MegaLights", &mega.DebugView, debugViews, IM_ARRAYSIZE(debugViews));
                ImGui::BeginDisabled(mega.DebugView != 1);
                ImGui::SliderInt("ヒートマップ上限", &mega.HeatmapMax, 4, 256);
                ImGui::EndDisabled();
                ImGui::BeginDisabled(mega.DebugView != 5);
                ImGui::SliderFloat("1/p の表示上限", &mega.InvPdfDisplayMax, 2.0f, 4096.0f, "%.0f",
                                   ImGuiSliderFlags_Logarithmic);
                ImGui::EndDisabled();
                ImGui::SliderFloat("スライス分割の奥行き (m)", &mega.SliceFar, 20.0f, 5000.0f, "%.0f",
                                   ImGuiSliderFlags_Logarithmic);
                ImGui::Checkbox("半透明 / Forward にも使う (M7)", &mega.Translucency);
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "半透明（OIT / Forward）の面も、セルのライトを全評価します（タイル 8 灯・画面 160 灯の制限なし）。\n"
                        "グリッドをジオメトリの手前の空間まで構築します。");
                }
                ImGui::Checkbox("フロクセルフォグにも使う (M6)", &mega.VolumetricFog);
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "フロクセルフォグのローカルライトをグリッドから確率的に選びます（フォグのライト上限 32 灯を撤廃）。\n"
                        "グリッドをジオメトリの手前の空間まで構築するので、グリッドの構築は少し重くなります。");
                }
                ImGui::BeginDisabled(!mega.VolumetricFog);
                ImGui::SliderInt("フロクセルあたりのサンプル数", &mega.FogSamples, 1, 4);
                ImGui::EndDisabled();

                if (ImGui::TreeNode("検証（累積・リファレンス比較）"))
                {
                    ImGui::BeginDisabled(!MegaLights::IsStochasticAvailable());
                    ImGui::Checkbox("静止時に累積する", &mega.Accumulate);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("リセット"))
                    {
                        MegaLights::RequestAccumulationReset();
                    }
                    ImGui::BeginDisabled(!mega.Accumulate || stats.AccumulatedFrames == 0);
                    if (ImGui::Button("現在の累積をリファレンスとして保存"))
                    {
                        MegaLights::RequestSaveReference();
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!stats.HasReference);
                    if (ImGui::Button("破棄"))
                    {
                        MegaLights::ClearReference();
                    }
                    ImGui::EndDisabled();
                    ImGui::EndDisabled();
                    ImGui::TextDisabled("累積 %u フレーム / リファレンス %s", stats.AccumulatedFrames,
                                        stats.HasReference ? "あり" : "なし");
                    if (stats.HasReference && stats.ErrorValid)
                    {
                        const bool pass = stats.RelativeRmse < 0.01;
                        ImGui::TextColored(pass ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f) : ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                                           "相対 RMSE（輝度）: %.3f%%（%u フレーム時点）", stats.RelativeRmse * 100.0,
                                           stats.ErrorFrames);
                    }
                    ImGui::TextWrapped(
                        "手順: 評価方式を「リファレンス」→ 累積 → 保存 → 「M2」に切り替え → 累積して RMSE を確認。"
                        "カメラ・ライトを動かすと累積は自動でリセットされます（オブジェクトの移動は「リセット」）。");
                    ImGui::TreePop();
                }
                ImGui::EndDisabled();
                ImGui::EndDisabled();
                if (!MegaLights::IsAvailable())
                {
                    ImGui::TextDisabled("初期化失敗（graphics ログを確認）");
                }
                else if (mega.Enabled)
                {
                    const char* activeNames[] = {"M1", "M2", "リファレンス"};
                    ImGui::TextDisabled("現在: %s%s%s", activeNames[static_cast<int>(stats.ActiveMode)],
                                        stats.FallbackReason ? " ← " : "",
                                        stats.FallbackReason ? stats.FallbackReason : "");
                    if (stats.ActiveMode == MegaLights::Mode::Stochastic)
                    {
                        ImGui::TextDisabled("サンプル %u / ピクセル・%ux%u（%s, バッファ %.1f MB）",
                                            stats.SamplesPerPixel, stats.SampleWidth, stats.SampleHeight,
                                            stats.SampleScale > 1 ? "半解像度" : "フル解像度", stats.SampleBufferMB);
                        if (stats.TraceStatsValid)
                        {
                            const double tileRatio =
                                stats.TotalTiles ? 100.0 * stats.ActiveTiles / stats.TotalTiles : 0.0;
                            const double screenRatio =
                                stats.TraceRequests ? 100.0 * stats.ScreenHits / stats.TraceRequests : 0.0;
                            ImGui::TextDisabled("対象タイル %u / %u (%.0f%%)", stats.ActiveTiles, stats.TotalTiles,
                                                tileRatio);
                            ImGui::TextDisabled("影レイ要求 %u → スクリーンで確定 %u (%.0f%%) / ワールド %u",
                                                stats.TraceRequests, stats.ScreenHits, screenRatio, stats.WorldTraces);
                        }
                        ImGui::TextDisabled("ガイド: %s（ハッシュ %ux%u タイル）",
                                            stats.Guided ? "ON" : (mega.Guide ? "準備中（次のフレームから）" : "OFF"),
                                            stats.HashTilesX, stats.HashTilesY);
                        if (stats.Denoised)
                        {
                            ImGui::TextDisabled("デノイズ: %s（バッファ %.1f MB）%s%s",
                                                stats.DenoiserUsed ? stats.DenoiserUsed : "ON", stats.DenoiseBufferMB,
                                                stats.AntiLag ? " / ライト変化" : "",
                                                stats.HistoryReset ? " / 履歴リセット" : "");
                            if (mega.Denoiser == 1 && stats.NrdFallback)
                            {
                                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "NRD: %s", stats.NrdFallback);
                            }
                        }
                        else if (stats.DenoiseFallback)
                        {
                            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "デノイズ: %s", stats.DenoiseFallback);
                        }
                    }
                    if (!MegaLights::IsStochasticAvailable())
                    {
                        ImGui::TextDisabled("M2 シェーダーの初期化に失敗（graphics ログを確認）");
                    }
                    ImGui::TextDisabled("半透明 / Forward: %s",
                                        stats.ForwardGrid ? "MegaLights" : "従来（タイル 8 灯）");
                    ImGui::TextDisabled("フォグ: %s",
                                        stats.VolumetricGrid
                                            ? "MegaLights（視錐台全体のグリッド）"
                                            : (mega.VolumetricFog ? "フロクセルフォグが無効" : "従来（上限 32 灯）"));
                    ImGui::TextDisabled("グリッドのライト %u 灯%s / グリッド %ux%ux%u", stats.SubmittedLights,
                                        stats.DroppedLights > 0 ? "（上限超過あり）" : "", stats.TilesX, stats.TilesY,
                                        stats.Slices);
                    ImGui::TextDisabled("リスト使用量 細 %u / %u・粗 %u / %u、セル最大 %u 灯", stats.FineIndices,
                                        MegaLights::kFineCapacity, stats.CoarseIndices, MegaLights::kCoarseCapacity,
                                        stats.MaxLightsPerCell);
                    if (stats.OverflowCells > 0)
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.3f, 1.0f), "リスト容量不足: %u セルで欠けあり",
                                           stats.OverflowCells);
                    }
                    if (stats.DroppedLights > 0)
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.3f, 1.0f), "%u 灯が上限 %u を超えたため従来の経路",
                                           stats.DroppedLights, MegaLights::kMaxLights);
                    }
                }
            }

            ImGui::SeparatorText("Tone Mapping / Color Grading");
            int toneMapper = static_cast<int>(rq.Tonemapper);
            if (ImGui::Combo("トーンマッパー", &toneMapper,
                             "ACES (従来の近似)\0AgX\0ACES Fitted (RRT+ODT)\0Khronos PBR Neutral\0"))
            {
                rq.Tonemapper = static_cast<ToneMapper>(toneMapper);
            }
            ImGui::BeginDisabled(rq.Tonemapper != ToneMapper::AgX);
            ImGui::Checkbox("AgX Punchy", &rq.AgXPunchy);
            ImGui::EndDisabled();
            ImGui::SliderFloat("コントラスト", &rq.Contrast, 0.5f, 2.0f, "%.2f");
            ImGui::SliderFloat("彩度", &rq.Saturation, 0.0f, 2.0f, "%.2f");
            ImGui::SliderFloat("自然な彩度", &rq.Vibrance, -1.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("色温度", &rq.Temperature, -1.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("色かぶり", &rq.Tint, -1.0f, 1.0f, "%.2f");
            ImGui::DragFloat3("Lift", &rq.Lift.x, 0.005f, -0.5f, 0.5f, "%.3f");
            ImGui::DragFloat3("Gamma", &rq.Gamma.x, 0.005f, 0.2f, 3.0f, "%.3f");
            ImGui::DragFloat3("Gain", &rq.Gain.x, 0.005f, 0.0f, 3.0f, "%.3f");
            if (ImGui::Button("グレーディングをリセット"))
            {
                rq.Contrast = rq.Saturation = 1.0f;
                rq.Vibrance = rq.Temperature = rq.Tint = 0.0f;
                rq.Lift = {0, 0, 0};
                rq.Gamma = {1, 1, 1};
                rq.Gain = {1, 1, 1};
            }

            ImGui::SeparatorText("Bloom / Lens");
            ImGui::BeginDisabled(!AdvancedEffects::IsAvailable());
            ImGui::Checkbox("デュアルフィルタ ブルーム", &rq.DualBloom);
            ImGui::BeginDisabled(!rq.DualBloom);
            ImGui::SliderFloat("ブルーム強度", &rq.BloomIntensity, 0.0f, 0.3f, "%.3f");
            ImGui::SliderFloat("しきい値 (0=物理ベース)", &rq.BloomThreshold, 0.0f, 8.0f, "%.2f");
            ImGui::SliderFloat("広がり", &rq.BloomRadius, 0.0f, 1.0f, "%.2f");
            ImGui::SliderInt("段数", &rq.BloomLevels, 2, 8);
            ImGui::EndDisabled();
            ImGui::Checkbox("レンズフレア", &rq.LensFlare);
            ImGui::BeginDisabled(!rq.LensFlare);
            ImGui::SliderFloat("フレア強度", &rq.LensFlareIntensity, 0.0f, 2.0f, "%.2f");
            ImGui::SliderFloat("ハロー", &rq.LensFlareHalo, 0.0f, 2.0f, "%.2f");
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::SliderFloat("ビネット", &rq.Vignette, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("ビネットの柔らかさ", &rq.VignetteSmoothness, 0.01f, 1.0f, "%.2f");
            ImGui::SliderFloat("色収差 (px)", &rq.ChromaticAberration, 0.0f, 8.0f, "%.1f");
            ImGui::SliderFloat("フィルムグレイン", &rq.FilmGrain, 0.0f, 1.0f, "%.2f");

            ImGui::SeparatorText("Weather");
            ImGui::Checkbox("天候を有効化", &rq.WeatherEnabled);
            ImGui::BeginDisabled(!rq.WeatherEnabled);
            ImGui::SliderFloat("雨", &rq.RainIntensity, 0.0f, 1.0f, "%.2f");
            ImGui::Checkbox("雨量に応じて濡れ・水たまりを変化", &rq.AutoWetness);
            ImGui::SliderFloat("濡れ", &rq.Wetness, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("水たまり", &rq.Puddles, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("積雪", &rq.Snow, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("降雪", &rq.SnowFall, 0.0f, 1.0f, "%.2f");
            ImGui::DragFloat("風 X", &rq.WindX, 0.01f, -5.0f, 5.0f, "%.2f");
            ImGui::DragFloat("風 Z", &rq.WindZ, 0.01f, -5.0f, 5.0f, "%.2f");
            ImGui::TextDisabled("現在: 濡れ %.2f / 水たまり %.2f", RenderQuality::GetEffectiveWetness(),
                                RenderQuality::GetEffectivePuddles());
            ImGui::EndDisabled();

            ImGui::SeparatorText("Transparency");
            ImGui::Checkbox("OIT (Weighted Blended)", &rq.OrderIndependentTransparency);
            if (rq.OrderIndependentTransparency && !TransparencyEffects::IsOitActive())
            {
                ImGui::TextDisabled("OIT は HDR（RGBA16F）時のみ有効です");
            }
            ImGui::Checkbox("RT 屈折 (Transmission > 0 の透明マテリアル)", &rq.RaytracedRefraction);

            ImGui::SeparatorText("ReSTIR GI");
            ImGui::Checkbox("ReSTIR GI (時間・空間リサンプリング)", &rq.ReSTIRGI);
            ImGui::BeginDisabled(!rq.ReSTIRGI);
            ImGui::SliderInt("時間方向の履歴上限 M", &rq.ReSTIRTemporalMaxM, 1, 64);
            ImGui::SliderInt("空間サンプル数", &rq.ReSTIRSpatialSamples, 0, 8);
            ImGui::SliderFloat("空間半径 (px)", &rq.ReSTIRSpatialRadius, 4.0f, 48.0f, "%.0f");
            ImGui::Checkbox("空間再利用の可視性チェック", &rq.ReSTIRSpatialVisibility);
            ImGui::EndDisabled();
        }

        ImGui::SeparatorText("Sky Atmosphere LUT / Async Compute");
        auto& skyLut = AtmosphereLut::GetSettings();
        ImGui::BeginDisabled(!AtmosphereLut::IsAvailable());
        ImGui::Checkbox("大気 LUT", &skyLut.Enabled);
        ImGui::BeginDisabled(!skyLut.Enabled);
        ImGui::SliderFloat("多重散乱", &skyLut.MultipleScattering, 0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("地表アルベド", &skyLut.GroundAlbedo, 0.0f, 1.0f, "%.2f");
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!AsyncCompute::IsAvailable());
        ImGui::Checkbox("非同期コンピュート", &AsyncCompute::Enabled());
        ImGui::EndDisabled();
        ImGui::TextDisabled("LUT 生成: %s", !AtmosphereLut::IsAvailable()    ? "無効（解析的な空）"
                                            : AtmosphereLut::WasBuiltAsync() ? "Compute キュー（非同期）"
                                                                             : "Graphics キュー");

        auto& advanced = AdvancedEffects::GetSettings();
        ImGui::SeparatorText("Volumetric Fog (Froxel)");
        ImGui::Checkbox("フロクセル ボリュームフォグ", &advanced.FroxelFog);
        ImGui::BeginDisabled(!advanced.FroxelFog);
        ImGui::SliderInt("奥行き分割数", &advanced.FroxelSliceCount, 16, 128);
        ImGui::SliderInt("タイルサイズ (px)", &advanced.FroxelTileSize, 4, 32);
        ImGui::SliderFloat("フォグ距離", &advanced.FogFar, 10.0f, 500.0f, "%.0f m");
        ImGui::SliderFloat("散乱の強さ", &advanced.FogScattering, 0.0f, 4.0f, "%.2f");
        ImGui::SliderFloat("環境光の散乱", &advanced.FogAmbient, 0.0f, 4.0f, "%.2f");
        ImGui::SliderFloat("時間方向の蓄積", &advanced.FogHistoryWeight, 0.0f, 0.97f, "%.2f");
        ImGui::TextDisabled("大気設定と局所フォグを使用。光芒の影はDXR有効時のみ");
        ImGui::EndDisabled();

        ImGui::SeparatorText("Auto Exposure");
        ImGui::Checkbox("自動露出 (ヒストグラム)", &advanced.AutoExposure);
        ImGui::BeginDisabled(!advanced.AutoExposure);
        ImGui::SliderFloat("露出補正 (EV)", &advanced.ExposureCompensation, -5.0f, 5.0f, "%.2f");
        ImGui::DragFloatRange2("EV 範囲", &advanced.ExposureMinEv, &advanced.ExposureMaxEv, 0.1f, -16.0f, 16.0f,
                               "%.1f");
        ImGui::SliderFloat("順応速度", &advanced.ExposureSpeed, 0.1f, 10.0f, "%.2f");
        ImGui::TextDisabled("手動の「露光」は自動露出に乗算されます");
        ImGui::EndDisabled();

        ImGui::SeparatorText("Motion Blur / Depth of Field");
        ImGui::Checkbox("MB / DOF 半解像度", &advanced.HalfResolutionLens);
        ImGui::Checkbox("モーションブラー", &advanced.MotionBlur);
        ImGui::BeginDisabled(!advanced.MotionBlur);
        ImGui::SliderFloat("シャッター", &advanced.MotionBlurShutter, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("最大長 (px)", &advanced.MotionBlurMaxPixels, 4.0f, 64.0f, "%.0f");
        ImGui::SliderInt("サンプル数", &advanced.MotionBlurSamples, 4, 16);
        ImGui::EndDisabled();
        ImGui::Checkbox("被写界深度", &advanced.DepthOfField);
        ImGui::BeginDisabled(!advanced.DepthOfField);
        ImGui::DragFloat("焦点距離", &advanced.FocusDistance, 0.05f, 0.05f, 1000.0f, "%.2f m");
        ImGui::DragFloat("ピント範囲", &advanced.FocusRange, 0.05f, 0.0f, 1000.0f, "%.2f m");
        ImGui::SliderFloat("最大ボケ (px)", &advanced.MaxBlurPixels, 1.0f, 32.0f, "%.1f");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(giMode == 0);
        if (RendererSettings::GetRealtimeGiEnabled())
        {
            float giDistance = RendererSettings::GetGiDistance();
            if (ImGui::SliderFloat("GI Trace Distance", &giDistance, 1.0f, 200.0f, "%.1f"))
            {
                RendererSettings::SetGiDistance(giDistance);
            }
            bool radianceCache = RendererSettings::GetGiRadianceCacheEnabled();
            if (ImGui::Checkbox("放射キャッシュ（多重バウンス）", &radianceCache))
            {
                RendererSettings::SetGiRadianceCacheEnabled(radianceCache);
            }
            ImGui::BeginDisabled(!radianceCache);
            float cacheCell = RendererSettings::GetGiRadianceCacheCellSize();
            if (ImGui::SliderFloat("キャッシュセルサイズ", &cacheCell, 0.05f, 1.0f, "%.2f m"))
            {
                RendererSettings::SetGiRadianceCacheCellSize(cacheCell);
            }
            int cacheHistory = RendererSettings::GetGiRadianceCacheHistory();
            if (ImGui::SliderInt("キャッシュ履歴フレーム", &cacheHistory, 4, 128))
            {
                RendererSettings::SetGiRadianceCacheHistory(cacheHistory);
            }
            ImGui::EndDisabled();
            if (!Raytracing::IsAvailable() || !Raytracing::GetSettings().Enabled)
            {
                ImGui::TextWrapped("Realtime GI requires DXR Tier 1.1 and enabled ray tracing.");
            }
            // 動作状態の診断（ON にしても効かない時に原因を表示する）
            const bool giRunning = Raytracing::IsGiActive();
            ImGui::TextColored(giRunning ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f) : ImVec4(1.0f, 0.55f, 0.3f, 1.0f), "状態: %s",
                               giRunning ? RayReflections::GetStatus()
                                         : (!Raytracing::IsAvailable()           ? Raytracing::GetStatus()
                                            : !Raytracing::GetSettings().Enabled ? "レイトレーシングが OFF です"
                                                                                 : RayReflections::GetStatus()));
            if (RayReflections::GetLastShaderError()[0] != '\0' && ImGui::TreeNode("シェーダーエラー"))
            {
                ImGui::TextWrapped("%s", RayReflections::GetLastShaderError());
                ImGui::TreePop();
            }
        }
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("照明予算", ImGuiTreeNodeFlags_DefaultOpen))
    {
        int screenBudget = RendererSettings::GetScreenLightBudget();
        if (ImGui::SliderInt("画面内 Physical Light", &screenBudget, 1, 32))
        {
            RendererSettings::SetScreenLightBudget(screenBudget);
        }
        int tileBudget = RendererSettings::GetTileLightBudget();
        if (ImGui::SliderInt("16x16 タイル重複", &tileBudget, 1, 4))
        {
            RendererSettings::SetTileLightBudget(tileBudget);
        }
        int decalBudget = RendererSettings::GetDecalLightBudget();
        if (ImGui::SliderInt("画面内 Decal Light", &decalBudget, 0, 110))
        {
            RendererSettings::SetDecalLightBudget(decalBudget);
        }
        int decalTileBudget = RendererSettings::GetDecalTileLightBudget();
        if (ImGui::SliderInt("タイル内 Decal Light", &decalTileBudget, 0, 4))
        {
            RendererSettings::SetDecalTileLightBudget(decalTileBudget);
        }
        int volumetricBudget = RendererSettings::GetVolumetricLightBudget();
        if (ImGui::SliderInt("Volumetric Light", &volumetricBudget, 0, 5))
        {
            RendererSettings::SetVolumetricLightBudget(volumetricBudget);
        }
        int shadowBudget = RendererSettings::GetShadowLightBudget();
        if (ImGui::SliderInt("Shadow map layer", &shadowBudget, 1, 8))
        {
            RendererSettings::SetShadowLightBudget(shadowBudget);
        }
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
                const string label =
                    filesystem::path(info.Path).filename().string() + "##monitor_" + to_string(info.SrvIndex);
                if (ImGui::Selectable(label.c_str(), info.SrvIndex == monitorTextureIndex))
                {
                    RenderConfiguration::SetMonitorTextureIndex(info.SrvIndex);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextWrapped(
            "ライトは GPU 上でタイル別 instance list と volumetric shaft list に圧縮され、ピクセル当たりの評価数はタイル重複予算を超えません。");
    }

    if (ImGui::CollapsingHeader("IES配光", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool iesEnabled = RendererSettings::GetIesProfilesEnabled();
        if (ImGui::Checkbox("IESプロファイルを有効化", &iesEnabled))
        {
            RendererSettings::SetIesProfilesEnabled(iesEnabled);
        }
        ImGui::TextWrapped("Point / Spot / Rect ライトに指定した IES ファイルの配光を適用します。"
                          "明るさはライト強度で調整できます。");
    }

    if (ImGui::CollapsingHeader("シャドウ", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const bool raytracedShadowsSupported = Raytracing::UsesRaytracedShadows();
        if (raytracedShadowsSupported)
        {
            ImGui::TextWrapped(
                "現在はレイトレーシングで影を描画しています。反射のみのモードでは以下のシャドウマップ設定を使用します。");
        }
        ImGui::BeginDisabled(raytracedShadowsSupported);
        int method = static_cast<int>(RendererSettings::GetShadowMapMethod());
        const char* methods[] = {"ShadowMap", "Virtual ShadowMap"};
        if (ImGui::Combo("方式", &method, methods, IM_ARRAYSIZE(methods)))
        {
            RendererSettings::SetShadowMapMethod(static_cast<ShadowMapMethod>(method));
        }

        const bool virtualMode = method == static_cast<int>(ShadowMapMethod::VirtualShadowMap);
        ImGui::BeginDisabled(virtualMode);
        int cascadeCount = RendererSettings::GetShadowCascadeCount();
        if (ImGui::SliderInt("カスケード数", &cascadeCount, 1, 4))
        {
            RendererSettings::SetShadowCascadeCount(cascadeCount);
        }
        float shadowDistance = RendererSettings::GetShadowDistance();
        if (ImGui::SliderFloat("動的シャドウ距離", &shadowDistance, 8.0f, 512.0f, "%.0f m",
                               ImGuiSliderFlags_Logarithmic))
        {
            RendererSettings::SetShadowDistance(shadowDistance);
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!virtualMode);
        int levels = RendererSettings::GetVirtualClipmapLevels();
        if (ImGui::SliderInt("クリップマップ レベル", &levels, 1, 4))
        {
            RendererSettings::SetVirtualClipmapLevels(levels);
        }
        float firstRadius = RendererSettings::GetVirtualFirstLevelRadius();
        if (ImGui::SliderFloat("最内周の半径", &firstRadius, 4.0f, 64.0f, "%.1f m"))
        {
            RendererSettings::SetVirtualFirstLevelRadius(firstRadius);
        }
        bool stabilize = RendererSettings::GetStabilizeVirtualClipmaps();
        if (ImGui::Checkbox("カメラ移動時に安定化", &stabilize))
        {
            RendererSettings::SetStabilizeVirtualClipmaps(stabilize);
        }
        bool cachePages = RendererSettings::GetCacheVirtualShadowPages();
        if (ImGui::Checkbox("静的ページをキャッシュ", &cachePages))
        {
            RendererSettings::SetCacheVirtualShadowPages(cachePages);
        }
        ImGui::Text("物理ページ: %u x %u px / %u x %u ページ / level", RendererState::g_kVIRTUAL_SHADOW_PAGE_SIZE,
                    RendererState::g_kVIRTUAL_SHADOW_PAGE_SIZE, RendererState::g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION,
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
        const char* debugModes[] = {"なし", "Shadow Mask", "Clipmap Level", "Virtual Page"};
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
        if (ImGui::SliderInt("PCF フィルター半径", &filterRadius, 0, 3))
        {
            RendererSettings::SetShadowFilterRadius(filterRadius);
        }
        float resolutionTransition = RendererSettings::GetShadowResolutionTransition();
        if (ImGui::SliderFloat("解像度遷移スケール", &resolutionTransition, 0.05f, 0.40f, "%.2f"))
        {
            RendererSettings::SetShadowResolutionTransition(resolutionTransition);
        }
        ImGui::TextDisabled("ライト空間の重複領域で解像度を連続遷移（SM/VSM 共通）");
        float depthBias = RendererSettings::GetShadowDepthBias();
        if (virtualMode)
        {
            if (ImGui::SliderFloat("深度バイアス", &depthBias, 0.000001f, 0.005f, "%.7f", ImGuiSliderFlags_Logarithmic))
            {
                RendererSettings::SetShadowDepthBias(depthBias);
            }
        }
        else if (ImGui::SliderFloat("深度バイアス", &depthBias, 0.0f, 0.001f, "%.7f"))
        {
            RendererSettings::SetShadowDepthBias(depthBias);
        }
        float normalBias = RendererSettings::GetShadowNormalBias();
        if (virtualMode)
        {
            if (ImGui::SliderFloat("法線バイアス", &normalBias, 0.000001f, 0.003f, "%.7f",
                                   ImGuiSliderFlags_Logarithmic))
            {
                RendererSettings::SetShadowNormalBias(normalBias);
            }
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
        if (ImGui::Checkbox("Contact Shadow (スクリーンスペース)", &contactShadows))
        {
            RendererSettings::SetContactShadowsEnabled(contactShadows);
        }
        ImGui::BeginDisabled(!contactShadows);
        float contactLength = RendererSettings::GetContactShadowLength();
        if (ImGui::SliderFloat("Contact Shadow 長", &contactLength, 0.05f, 5.0f, "%.2f m"))
        {
            RendererSettings::SetContactShadowLength(contactLength);
        }
        int contactSteps = RendererSettings::GetContactShadowSteps();
        if (ImGui::SliderInt("Contact Shadow ステップ", &contactSteps, 4, 24))
        {
            RendererSettings::SetContactShadowSteps(contactSteps);
        }
        ImGui::EndDisabled();
        bool distanceFieldShadows = RendererSettings::GetDistanceFieldShadowsEnabled();
        if (ImGui::Checkbox("Distance Field Shadow (AABB SDF)", &distanceFieldShadows))
        {
            RendererSettings::SetDistanceFieldShadowsEnabled(distanceFieldShadows);
        }
        ImGui::BeginDisabled(!distanceFieldShadows);
        float distanceFieldDistance = RendererSettings::GetDistanceFieldShadowDistance();
        if (ImGui::SliderFloat("SDF レイ距離", &distanceFieldDistance, 2.0f, 100.0f, "%.1f m"))
        {
            RendererSettings::SetDistanceFieldShadowDistance(distanceFieldDistance);
        }
        int distanceFieldSteps = RendererSettings::GetDistanceFieldShadowSteps();
        if (ImGui::SliderInt("SDF ステップ", &distanceFieldSteps, 4, 24))
        {
            RendererSettings::SetDistanceFieldShadowSteps(distanceFieldSteps);
        }
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
        const float estimatedMemoryMb = static_cast<float>(activeShadowLayers * RendererState::g_kSHADOW_MAP_SIZE *
                                                           RendererState::g_kSHADOW_MAP_SIZE * sizeof(float)) /
                                        (1024.0f * 1024.0f);
        const float allocatedMemoryMb =
            static_cast<float>(RendererState::g_kMAX_SHADOW_LIGHTS * RendererState::g_kSHADOW_MAP_SIZE *
                               RendererState::g_kSHADOW_MAP_SIZE * sizeof(float)) /
            (1024.0f * 1024.0f);
        ImGui::Text("使用レイヤー: %d / 有効フットプリント: %.0f MB", activeShadowLayers, estimatedMemoryMb);
        ImGui::Text("確保済み深度プール: %.0f MB", allocatedMemoryMb);
        ImGui::TextWrapped(
            "Virtual ShadowMap はカメラ周辺に複数のクリップマップを配置し、受光点に必要な最も細かいレベルを選択します。");
        if (ImGui::Button("シャドウ設定を初期値へ戻す"))
        {
            RendererSettings::ResetShadowDefaults();
        }
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Local Height Fog", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::Button("Height Fog を追加"))
        {
            LocalHeightFog::Add(LocalFogShape::Height);
        }
        ImGui::SameLine();
        if (ImGui::Button("Sphere Fog を追加"))
        {
            LocalHeightFog::Add(LocalFogShape::Sphere);
        }
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
                const char* shapes[] = {"Height", "Sphere"};
                if (ImGui::Combo("形状", &shape, shapes, IM_ARRAYSIZE(shapes)))
                {
                    fog.Shape = static_cast<LocalFogShape>(shape);
                }
                ImGui::DragFloat3("位置", &fog.Position.x, 0.1f);
                ImGui::SliderFloat("半径", &fog.Radius, 0.1f, 100.0f, "%.1f m");
                ImGui::SliderFloat("密度", &fog.Density, 0.0f, 4.0f, "%.3f");
                ImGui::BeginDisabled(fog.Shape == LocalFogShape::Sphere);
                ImGui::SliderFloat("高さフォールオフ", &fog.HeightFalloff, 0.01f, 8.0f, "%.2f");
                ImGui::EndDisabled();
                ImGui::ColorEdit3("フォグ色", &fog.Color.x);
                if (ImGui::Button("削除"))
                {
                    removeIndex = static_cast<int>(i);
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (removeIndex >= 0)
        {
            LocalHeightFog::Remove(static_cast<size_t>(removeIndex));
        }
        ImGui::TextWrapped(
            "最大16個の局所フォグを1つの定数バッファにまとめて処理します。複数配置時も描画パスは増えません。");
    }

    ImGui::End();
}

bool ImGuiManager::PersistentSettings(ostream* output, const string& key, const string& value)
{
    bool iesEnabled = RendererSettings::GetIesProfilesEnabled();
    if (EditorConfig::Value(output, key, value, "ies_profiles_enabled", iesEnabled))
    {
        RendererSettings::SetIesProfilesEnabled(iesEnabled);
        return true;
    }
    bool matched = false;
    auto& temporalRays = Raytracing::GetSettings();
    matched |= EditorConfig::Value(output, key, value, "ray.StableNrdProfile", temporalRays.StableNrdProfile, 0, 1);
    matched |= EditorConfig::Value(output, key, value, "ray.StableSampling", temporalRays.StableSampling, 0, 1);
    auto& textureCompression = TextureCompression::GetSettings();
    matched |= EditorConfig::Value(output, key, value, "texture.CompressOnImport", textureCompression.Enabled, 0, 1);
    matched |= EditorConfig::Value(output, key, value, "texture.BackgroundCompression",
                                   textureCompression.BackgroundForSynchronousLoads, 0, 1);
    matched |= EditorConfig::Value(output, key, value, "ray.CompactStaticBlas", temporalRays.CompactStaticBlas, 0, 1);
    matched |= EditorConfig::Value(output, key, value, "ray.DeformingBlasRebuildInterval",
                                   temporalRays.DeformingBlasRebuildInterval, 0, 240);
    matched |= EditorConfig::Value(output, key, value, "ray.DeformingBlasRebuildsPerFrame",
                                   temporalRays.DeformingBlasRebuildsPerFrame, 1, 16);
    matched |= EditorConfig::Value(output, key, value, "ray.QuantizeBlasVertices", temporalRays.QuantizeBlasVertices, 0, 1);
    matched |= EditorConfig::Value(output, key, value, "ray.BlasQuantizationMaxError",
                                   temporalRays.BlasQuantizationMaxError, 0.00001, 0.01);
    const auto field = [&](const char* name, auto& setting, long double low, long double high) {
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
    field("editor.ShowProceduralStudioWindow", m_ShowProceduralStudioWindow, 0, 1);
    field("editor.ShowRaytracingWindow", m_ShowRaytracingWindow, 0, 1);
    field("editor.ShowProjectSettingsWindow", m_ShowProjectSettingsWindow, 0, 1);
    field("editor.ShowPhysicsSettingsWindow", m_ShowPhysicsSettingsWindow, 0, 1);
    field("editor.ShowLightDebug", m_ShowLightDebug, 0, 1);
    field("editor.gizmo", m_GizmoOperation, 0, 3);
    field("editor.log_auto_scroll", m_LogAutoScroll, 0, 1);
    // 自動品質調整（可変レイトレーシング / 可変GI など）の設定
    auto& governor = PerformanceGovernor::GetOptions();
    field("governor.Enabled", governor.Enabled, 0, 1);
    field("governor.AllowResolutionScaling", governor.AllowResolutionScaling, 0, 1);
    field("governor.BudgetHeadroom", governor.BudgetHeadroom, 0.6, 1.0);
    field("governor.UpgradeMargin", governor.UpgradeMargin, 0.3, 0.95);
    field("governor.DowngradeDelay", governor.DowngradeDelay, 0.0, 10.0);
    field("governor.UpgradeDelay", governor.UpgradeDelay, 0.1, 30.0);
    field("governor.ResolutionDelay", governor.ResolutionDelay, 0.1, 30.0);
    field("governor.AllowFeatureToggles", governor.AllowFeatureToggles, 0, 1);
    // フロクセルフォグ / 自動露出 / モーションブラー / 被写界深度
    // 画質向上（renderquality.h）
    auto& rq = RenderQuality::Get();
    field("quality.EnergyCompensation", rq.EnergyCompensation, 0, 1);
    field("quality.AreaLights", rq.AreaLights, 0, 1);
    // 負荷削減（cpuculling.h）
    auto& culling = RenderCulling::GetSettings();
    field("culling.ObjectFrustum", culling.ObjectFrustumCulling, 0, 1);
    field("culling.ObjectSize", culling.ObjectSizeCulling, 0, 1);
    field("culling.MinObjectPixels", culling.MinObjectPixels, 0.0, 64.0);
    field("culling.MaxDrawDistance", culling.MaxDrawDistance, 0.0, 100000.0);
    field("culling.StaticVelocitySkip", culling.StaticVelocitySkip, 0, 1);
    field("culling.AutoLod", culling.AutoLod, 0, 1);
    field("culling.SharePrimitiveGeometry", culling.SharePrimitiveGeometry, 0, 1);
    field("culling.LightFrustum", culling.LightFrustumCulling, 0, 1);
    field("culling.LightDistance", culling.LightDistanceCulling, 0, 1);
    field("culling.LightMaxDistance", culling.LightMaxDistance, 0.0, 100000.0);
    field("culling.LightMinPixels", culling.LightMinPixels, 0.0, 64.0);
    field("culling.LightOcclusion", culling.LightOcclusionCulling, 0, 1);
    field("culling.SkipRayTracedShadowMaps", culling.SkipRayTracedShadowMaps, 0, 1);
    field("culling.ReflectionRoughnessCutoff", culling.ReflectionRoughnessCutoff, 0.0, 1.0);
    field("culling.RayHitLights", culling.RayHitLights, 1, 160);
    field("culling.ReflectionCheckerboard", culling.ReflectionCheckerboard, 0, 1);
    // MegaLights の設定（megalights.h）。
    auto& megaLights = MegaLights::GetSettings();
    field("megalights.Enabled", megaLights.Enabled, 0, 1);
    field("megalights.DebugView", megaLights.DebugView, 0, 9);
    field("megalights.SliceFar", megaLights.SliceFar, 20.0, 5000.0);
    field("megalights.HeatmapMax", megaLights.HeatmapMax, 1, 256);
    field("megalights.EvaluationMode", megaLights.EvaluationMode, 0, 2);
    field("megalights.SamplesPerPixel", megaLights.SamplesPerPixel, 1, 4);
    field("megalights.UniformMix", megaLights.UniformMix, 0.001, 1.0);
    field("megalights.ReferenceRays", megaLights.ReferenceRays, 1, 64);
    field("megalights.InvPdfDisplayMax", megaLights.InvPdfDisplayMax, 2.0, 4096.0);
    field("megalights.Guide", megaLights.Guide, 0, 1);
    field("megalights.GuideMinWeight", megaLights.GuideMinWeight, 0.001, 1.0);
    field("megalights.GuideKeep", megaLights.GuideKeep, 0.0, 1.0);
    field("megalights.HalfResolution", megaLights.HalfResolution, 0, 1);
    field("megalights.Translucency", megaLights.Translucency, 0, 1);
    field("megalights.Denoiser", megaLights.Denoiser, 0, 1);
    field("megalights.VolumetricFog", megaLights.VolumetricFog, 0, 1);
    field("megalights.FogSamples", megaLights.FogSamples, 1, 4);
    field("megalights.ScreenTrace", megaLights.ScreenTrace, 0, 1);
    field("megalights.ScreenTraceSteps", megaLights.ScreenTraceSteps, 1, 64);
    field("megalights.ScreenTraceMaxPixels", megaLights.ScreenTraceMaxPixels, 1.0, 1024.0);
    field("megalights.ScreenTraceThickness", megaLights.ScreenTraceThickness, 0.001, 10.0);
    field("megalights.ScreenTraceSelfPixels", megaLights.ScreenTraceSelfPixels, 0.0, 16.0);
    field("megalights.Denoise", megaLights.Denoise, 0, 1);
    field("megalights.DenoiseIterations", megaLights.DenoiseIterations, 0, 5);
    field("megalights.MaxHistory", megaLights.MaxHistory, 1, 256);
    field("megalights.AntiLagHistory", megaLights.AntiLagHistory, 1, 256);
    field("megalights.FireflySuppression", megaLights.FireflySuppression, 0, 1);
    field("megalights.HistoryFeedback", megaLights.HistoryFeedback, 0, 1);
    field("megalights.AlphaMin", megaLights.AlphaMin, 0.001, 1.0);
    field("megalights.MomentsAlphaMin", megaLights.MomentsAlphaMin, 0.001, 1.0);
    field("megalights.DepthTolerance", megaLights.DepthTolerance, 0.001, 1.0);
    field("megalights.NormalThreshold", megaLights.NormalThreshold, -1.0, 1.0);
    field("megalights.PhiNormal", megaLights.PhiNormal, 1.0, 256.0);
    field("megalights.PhiLuminance", megaLights.PhiLuminance, 0.1, 100.0);
    field("megalights.PhiPlane", megaLights.PhiPlane, 0.05, 100.0);
    int toneMapper = static_cast<int>(rq.Tonemapper);
    if (EditorConfig::Value(output, key, value, "quality.Tonemapper", toneMapper, 0, 3))
    {
        rq.Tonemapper = static_cast<ToneMapper>(toneMapper);
        matched = true;
    }
    field("quality.AgXPunchy", rq.AgXPunchy, 0, 1);
    field("quality.Contrast", rq.Contrast, 0, 4);
    field("quality.Saturation", rq.Saturation, 0, 4);
    field("quality.Vibrance", rq.Vibrance, -1, 1);
    field("quality.Temperature", rq.Temperature, -1, 1);
    field("quality.Tint", rq.Tint, -1, 1);
    field("quality.Lift.r", rq.Lift.x, -1, 1);
    field("quality.Lift.g", rq.Lift.y, -1, 1);
    field("quality.Lift.b", rq.Lift.z, -1, 1);
    field("quality.Gamma.r", rq.Gamma.x, 0.05, 4);
    field("quality.Gamma.g", rq.Gamma.y, 0.05, 4);
    field("quality.Gamma.b", rq.Gamma.z, 0.05, 4);
    field("quality.Gain.r", rq.Gain.x, 0, 4);
    field("quality.Gain.g", rq.Gain.y, 0, 4);
    field("quality.Gain.b", rq.Gain.z, 0, 4);
    field("quality.Vignette", rq.Vignette, 0, 1);
    field("quality.VignetteSmoothness", rq.VignetteSmoothness, 0.01, 1);
    field("quality.ChromaticAberration", rq.ChromaticAberration, 0, 16);
    field("quality.FilmGrain", rq.FilmGrain, 0, 1);
    field("quality.DualBloom", rq.DualBloom, 0, 1);
    field("quality.BloomIntensity", rq.BloomIntensity, 0, 1);
    field("quality.BloomThreshold", rq.BloomThreshold, 0, 100);
    field("quality.BloomRadius", rq.BloomRadius, 0, 1);
    field("quality.BloomLevels", rq.BloomLevels, 2, 8);
    field("quality.LensFlare", rq.LensFlare, 0, 1);
    field("quality.LensFlareIntensity", rq.LensFlareIntensity, 0, 10);
    field("quality.LensFlareHalo", rq.LensFlareHalo, 0, 10);
    field("quality.WeatherEnabled", rq.WeatherEnabled, 0, 1);
    field("quality.RainIntensity", rq.RainIntensity, 0, 1);
    field("quality.Wetness", rq.Wetness, 0, 1);
    field("quality.Puddles", rq.Puddles, 0, 1);
    field("quality.Snow", rq.Snow, 0, 1);
    field("quality.SnowFall", rq.SnowFall, 0, 1);
    field("quality.WindX", rq.WindX, -10, 10);
    field("quality.WindZ", rq.WindZ, -10, 10);
    field("quality.AutoWetness", rq.AutoWetness, 0, 1);
    field("quality.OIT", rq.OrderIndependentTransparency, 0, 1);
    field("quality.RaytracedRefraction", rq.RaytracedRefraction, 0, 1);
    field("quality.ReSTIRGI", rq.ReSTIRGI, 0, 1);
    field("quality.ReSTIRTemporalMaxM", rq.ReSTIRTemporalMaxM, 1, 64);
    field("quality.ReSTIRSpatialSamples", rq.ReSTIRSpatialSamples, 0, 16);
    field("quality.ReSTIRSpatialRadius", rq.ReSTIRSpatialRadius, 1, 64);
    field("quality.ReSTIRSpatialVisibility", rq.ReSTIRSpatialVisibility, 0, 1);
    auto& skyLut = AtmosphereLut::GetSettings();
    field("atmosphere_lut.Enabled", skyLut.Enabled, 0, 1);
    field("atmosphere_lut.MultipleScattering", skyLut.MultipleScattering, 0, 4);
    field("atmosphere_lut.GroundAlbedo", skyLut.GroundAlbedo, 0, 1);
    field("async_compute.Enabled", AsyncCompute::Enabled(), 0, 1);
    auto& advanced = AdvancedEffects::GetSettings();
    field("advanced.FroxelFog", advanced.FroxelFog, 0, 1);
    field("advanced.FroxelSliceCount", advanced.FroxelSliceCount, 16, 128);
    field("advanced.FroxelTileSize", advanced.FroxelTileSize, 4, 32);
    field("advanced.FogNear", advanced.FogNear, 0.05, 10);
    field("advanced.FogFar", advanced.FogFar, 5, 2000);
    field("advanced.FogHistoryWeight", advanced.FogHistoryWeight, 0, 0.97);
    field("advanced.FogScattering", advanced.FogScattering, 0, 10);
    field("advanced.FogAmbient", advanced.FogAmbient, 0, 10);
    field("advanced.AutoExposure", advanced.AutoExposure, 0, 1);
    field("advanced.ExposureCompensation", advanced.ExposureCompensation, -10, 10);
    field("advanced.ExposureMinEv", advanced.ExposureMinEv, -16, 16);
    field("advanced.ExposureMaxEv", advanced.ExposureMaxEv, -16, 16);
    field("advanced.ExposureSpeed", advanced.ExposureSpeed, 0.01, 20);
    field("advanced.ExposureLowPercent", advanced.ExposureLowPercent, 0, 0.99);
    field("advanced.ExposureHighPercent", advanced.ExposureHighPercent, 0.01, 1);
    field("advanced.HalfResolutionLens", advanced.HalfResolutionLens, 0, 1);
    field("advanced.MotionBlur", advanced.MotionBlur, 0, 1);
    field("advanced.MotionBlurShutter", advanced.MotionBlurShutter, 0, 2);
    field("advanced.MotionBlurMaxPixels", advanced.MotionBlurMaxPixels, 1, 128);
    field("advanced.MotionBlurSamples", advanced.MotionBlurSamples, 2, 32);
    field("advanced.DepthOfField", advanced.DepthOfField, 0, 1);
    field("advanced.FocusDistance", advanced.FocusDistance, 0.05, 10000);
    field("advanced.FocusRange", advanced.FocusRange, 0, 10000);
    field("advanced.MaxBlurPixels", advanced.MaxBlurPixels, 1, 48);
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
    if (EditorConfig::Value(output, key, value, "camera_post_process", postProcess, 0,
                            static_cast<int>(PostProcessType::COUNT) - 1))
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
    if (output)
    {
        *output << "asset_directory=" << m_CurrentAssetDirectory.generic_string() << '\n';
    }
    else if (key == "asset_directory")
    {
        const auto relative =
            filesystem::path(value).lexically_normal().lexically_relative(m_AssetRoot.lexically_normal());
        error_code error;
        if (!relative.empty() && *relative.begin() != ".." && filesystem::is_directory(value, error))
        {
            m_CurrentAssetDirectory = filesystem::path(value);
        }
        matched = true;
    }
    return matched;
}

string ImGuiManager::SerializeProjectSettings()
{
    ostringstream stream;
    stream << setprecision(numeric_limits<float>::max_digits10);
    stream << "version=1\n";
    // 自動品質調整で一時的に下げている値ではなく、ユーザーの設定値（基準値）を保存する
    PerformanceGovernor::ScopedPersistBaseline persistBaseline;
    SimulationConfiguration::Save(stream);
    PersistentSettings(&stream);
    stream << "vsync=" << (static_cast<int>(World::IsVSyncEnabled())) << '\n';
    stream << "fixed_fps=" << (static_cast<int>(World::IsFixedFrameRateEnabled())) << '\n';
    stream << "target_fps=" << World::GetTargetFrameRate() << '\n';
    stream << "resolution_scale=" << PerformanceGovernor::GetPersistentResolutionScale() << '\n';
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
    stream << "lumen_capture_budget=" << RendererSettings::GetLumenCaptureBudget() << '\n';
    stream << "lumen_lighting_budget=" << RendererSettings::GetLumenLightingBudget() << '\n';
    stream << "lumen_probe_spacing=" << RendererSettings::GetLumenProbeSpacing() << '\n';
    stream << "gi_distance=" << RendererSettings::GetGiDistance() << '\n';
    stream << "ssgi_intensity=" << RendererSettings::GetSsgiIntensity() << '\n';
    stream << "realtime_gi_intensity=" << RendererSettings::GetRealtimeGiIntensity() << '\n';
    stream << "gi_radiance_cache=" << static_cast<int>(RendererSettings::GetGiRadianceCacheEnabled()) << '\n';
    stream << "gi_radiance_cache_cell=" << RendererSettings::GetGiRadianceCacheCellSize() << '\n';
    stream << "gi_radiance_cache_history=" << RendererSettings::GetGiRadianceCacheHistory() << '\n';
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
    stream << "distance_field_shadows=" << (static_cast<int>(RendererSettings::GetDistanceFieldShadowsEnabled()))
           << '\n';
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
               << fog.Position.x << ',' << fog.Position.y << ',' << fog.Position.z << ',' << fog.Radius << ','
               << fog.HeightFalloff << ',' << fog.Density << ',' << fog.Color.x << ',' << fog.Color.y << ','
               << fog.Color.z << '\n';
    }

    return stream.str();
}

void ImGuiManager::SaveProjectSettings()
{
    if (!m_ProjectSettingsLoaded || ProjectManager::IsPlaying())
    {
        return;
    }
    char validationMode[8]{};
    if (GetEnvironmentVariableA("DX12_VALIDATION_MODE", validationMode, sizeof(validationMode)) > 0 &&
        atoi(validationMode) != 0)
    {
        return;
    }
    error_code ec;
    filesystem::create_directories("Save", ec);
    const string contents = SerializeProjectSettings();
    if (contents == m_LastSavedProjectSettings)
    {
        return;
    }
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
    if (!stream)
    {
        return;
    }
    LocalHeightFog::Clear();
    string line;
    while (getline(stream, line))
    {
        const size_t separator = line.find('=');
        if (separator == string::npos)
        {
            continue;
        }
        const string key = line.substr(0, separator);
        const string value = line.substr(separator + 1);
        try
        {
            if (PersistentSettings(nullptr, key, value) || SimulationConfiguration::TryLoad(key, value))
            {
                continue;
            }
            if (key == "vsync")
            {
                World::SetVSyncEnabled(stoi(value) != 0);
            }
            else if (key == "fixed_fps")
            {
                World::SetFixedFrameRateEnabled(stoi(value) != 0);
            }
            else if (key == "target_fps")
            {
                World::SetTargetFrameRate(stoi(value));
            }
            else if (key == "resolution_scale")
            {
                RenderConfiguration::SetResolutionScale(stof(value));
            }
            else if (key == "upscale_mode")
            {
                RendererSettings::SetUpscaleMode(static_cast<UpscaleMode>(clamp(stoi(value), 0, 4)));
            }
            else if (key == "upscale_quality")
            {
                RendererSettings::SetUpscaleQuality(static_cast<UpscaleQuality>(clamp(stoi(value), 0, 4)));
            }
            else if (key == "fsr_sharpness")
            {
                RendererSettings::SetFsrSharpness(stof(value));
            }
            else if (key == "nis_sharpness")
            {
                RendererSettings::SetNisSharpness(stof(value));
            }
            else if (key == "compute_gbuffer")
            {
                RendererSettings::SetComputeGBufferEnabled(stoi(value) != 0);
            }
            else if (key == "mesh_shaders")
            {
                RendererSettings::SetMeshShadersEnabled(stoi(value) != 0);
            }
            else if (key == "two_phase_occlusion")
            {
                RendererSettings::SetTwoPhaseOcclusionEnabled(stoi(value) != 0);
            }
            else if (key == "texture_streaming")
            {
                RendererSettings::SetTextureStreamingEnabled(stoi(value) != 0);
            }
            else if (key == "reserved_resources")
            {
                RendererSettings::SetReservedResourcesEnabled(stoi(value) != 0);
            }
            else if (key == "ssao")
            {
                RendererSettings::SetSsaoEnabled(stoi(value) != 0);
            }
            else if (key == "ssao_radius")
            {
                RendererSettings::SetSsaoRadius(stof(value));
            }
            else if (key == "ssao_power")
            {
                RendererSettings::SetSsaoPower(stof(value));
            }
            else if (key == "ssgi")
            {
                RendererSettings::SetSsgiEnabled(stoi(value) != 0);
            }
            else if (key == "gi_mode")
            {
                RendererSettings::SetGiMode(static_cast<GlobalIlluminationMode>(clamp(stoi(value), 0, 3)));
            }
            else if (key == "lumen_capture_budget")
            {
                RendererSettings::SetLumenCaptureBudget(stoi(value));
            }
            else if (key == "lumen_lighting_budget")
            {
                RendererSettings::SetLumenLightingBudget(stoi(value));
            }
            else if (key == "lumen_probe_spacing")
            {
                RendererSettings::SetLumenProbeSpacing(stoi(value));
            }
            else if (key == "gi_distance")
            {
                RendererSettings::SetGiDistance(stof(value));
            }
            else if (key == "ssgi_intensity")
            {
                RendererSettings::SetSsgiIntensity(stof(value));
            }
            else if (key == "realtime_gi_intensity")
            {
                RendererSettings::SetRealtimeGiIntensity(stof(value));
            }
            else if (key == "gi_radiance_cache")
            {
                RendererSettings::SetGiRadianceCacheEnabled(stoi(value) != 0);
            }
            else if (key == "gi_radiance_cache_cell")
            {
                RendererSettings::SetGiRadianceCacheCellSize(stof(value));
            }
            else if (key == "gi_radiance_cache_history")
            {
                RendererSettings::SetGiRadianceCacheHistory(stoi(value));
            }
            else if (key == "ray_binning")
            {
                RendererSettings::SetRayBinningEnabled(stoi(value) != 0);
            }
            else if (key == "anti_aliasing")
            {
                RendererState::m_AntiAliasingMode =
                    static_cast<AntiAliasingMode>(clamp(stoi(value), 0, static_cast<int>(AntiAliasingMode::COUNT) - 1));
            }
            else if (key == "hdr")
            {
                m_HdrEnabled = stoi(value) != 0;
                RenderConfiguration::SetHdr(m_HdrEnabled);
            }
            else if (key == "tone_map")
            {
                m_ToneMapEnabled = stoi(value) != 0;
            }
            else if (key == "exposure")
            {
                m_Exposure = clamp(stof(value), 0.01f, 10.0f);
            }
            else if (key == "shadow_method")
            {
                RendererSettings::SetShadowMapMethod(static_cast<ShadowMapMethod>(clamp(stoi(value), 0, 1)));
            }
            else if (key == "shadow_cascades")
            {
                RendererSettings::SetShadowCascadeCount(stoi(value));
            }
            else if (key == "shadow_distance")
            {
                RendererSettings::SetShadowDistance(stof(value));
            }
            else if (key == "vsm_levels")
            {
                RendererSettings::SetVirtualClipmapLevels(stoi(value));
            }
            else if (key == "vsm_first_radius")
            {
                RendererSettings::SetVirtualFirstLevelRadius(stof(value));
            }
            else if (key == "vsm_stabilize")
            {
                RendererSettings::SetStabilizeVirtualClipmaps(stoi(value) != 0);
            }
            else if (key == "vsm_cache_pages")
            {
                RendererSettings::SetCacheVirtualShadowPages(stoi(value) != 0);
            }
            else if (key == "vsm_debug_mode")
            {
                RendererSettings::SetVirtualShadowDebugMode(stoi(value));
            }
            else if (key == "shadow_filter_radius")
            {
                RendererSettings::SetShadowFilterRadius(stoi(value));
            }

            else if (key == "shadow_depth_bias")
            {
                RendererSettings::SetConventionalShadowDepthBias(stof(value));
            }
            else if (key == "shadow_normal_bias")
            {
                RendererSettings::SetConventionalShadowNormalBias(stof(value));
            }
            else if (key == "vsm_depth_bias")
            {
                RendererSettings::SetVirtualShadowDepthBias(stof(value));
            }
            else if (key == "vsm_normal_bias")
            {
                RendererSettings::SetVirtualShadowNormalBias(stof(value));
            }
            else if (key == "shadow_resolution_transition")
            {
                float transition = stof(value);

                if (transition > 0.40f)
                {
                    transition /= 8.0f;
                }
                RendererSettings::SetShadowResolutionTransition(transition);
            }
            else if (key == "contact_shadows")
            {
                RendererSettings::SetContactShadowsEnabled(stoi(value) != 0);
            }
            else if (key == "contact_length")
            {
                RendererSettings::SetContactShadowLength(stof(value));
            }
            else if (key == "contact_steps")
            {
                RendererSettings::SetContactShadowSteps(stoi(value));
            }
            else if (key == "distance_field_shadows")
            {
                RendererSettings::SetDistanceFieldShadowsEnabled(stoi(value) != 0);
            }
            else if (key == "distance_field_distance")
            {
                RendererSettings::SetDistanceFieldShadowDistance(stof(value));
            }
            else if (key == "distance_field_steps")
            {
                RendererSettings::SetDistanceFieldShadowSteps(stoi(value));
            }
            else if (key == "screen_light_budget")
            {
                RendererSettings::SetScreenLightBudget(stoi(value));
            }
            else if (key == "tile_light_budget")
            {
                RendererSettings::SetTileLightBudget(stoi(value));
            }
            else if (key == "decal_light_budget")
            {
                RendererSettings::SetDecalLightBudget(stoi(value));
            }
            else if (key == "decal_tile_light_budget")
            {
                RendererSettings::SetDecalTileLightBudget(stoi(value));
            }
            else if (key == "volumetric_light_budget")
            {
                RendererSettings::SetVolumetricLightBudget(stoi(value));
            }
            else if (key == "shadow_light_budget")
            {
                RendererSettings::SetShadowLightBudget(stoi(value));
            }
            else if (key == "monitor_texture")
            {
                RenderConfiguration::SetMonitorTextureIndex(TextureManager::LoadTexture(value));
            }
            else if (key == "fog" && LocalHeightFog::GetVolumes().size() < LocalHeightFog::MaxVolumes)
            {
                vector<float> fields;
                stringstream values(value);
                string field;
                while (getline(values, field, ','))
                {
                    fields.push_back(stof(field));
                }
                if (fields.size() == 11)
                {
                    LocalHeightFogVolume fog{};
                    fog.Enabled = fields[0] != 0.0f;
                    fog.Shape = static_cast<LocalFogShape>(clamp(static_cast<int>(fields[1]), 0, 1));
                    fog.Position = {fields[2], fields[3], fields[4]};
                    fog.Radius = max(fields[5], 0.1f);
                    fog.HeightFalloff = max(fields[6], 0.01f);
                    fog.Density = max(fields[7], 0.0f);
                    fog.Color = {fields[8], fields[9], fields[10]};
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
    if (auto* physics = SystemManager::GetSystem<PhysicsSystem>())
    {
        physics->ResetSimulation();
    }

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
