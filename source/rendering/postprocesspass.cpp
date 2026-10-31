#include "pch.h"
#include "temporalupscaler.h"
#include "megalights.h"
#include "transmittanceshadows.h"
#include "cloud/cloudrenderer.h"
#include "raytracing.h"
#include "dlssupscaler.h"
#include "postprocesspass.h"
#include "scenepasses.h"
#include "renderframe.h"
#include "frameconstants.h"
#include "lightingresources.h"
#include "graphicsdevice.h"
#include "shaderpaths.h"
#include "world.h"
#include "ecs.h"
#include "texturemanager.h"
#include "componentmanager.h"
#include "imguimanager.h"
#include "psomanager.h"
#include "camera.h"
#include "renderprofiler.h"
#include "renderersettings.h"
#include "spatialupscaler.h"
#include "visibilitybuffer.h"
#include "screenspaceeffects.h"
#include "occlusionculling.h"
#include "materialbindings.h"
#include "systemmanager.h"
#include "advancedeffects.h"
#include "atmospherelut.h"
#include "renderquality.h"
#include "pbrluts.h"
#include "transparencyeffects.h"
#include "temporalhistorycontext.h"
#include "colormanagement.h"

namespace
{
    TemporalHistory::State g_PreviousTaaState;
    TemporalHistory::State g_CurrentTaaState;
    bool g_TaaHistoryValid = false;
    bool g_CanReuseTaaHistory = false;

    struct PostProcessConstants
    {
        XMFLOAT4 Flags{};
        XMFLOAT4 PPCameraPos{};
        XMFLOAT4 HdrFlags{};
        XMFLOAT4 BloomParams{};
        XMFLOAT4X4 PPInvViewProjection{};
        XMFLOAT4X4 PPViewProjection{};
        XMFLOAT4 RaytracingFlags{};
        XMFLOAT4 RaytracingSunDirection{};
        XMFLOAT4 Quality[11]{}; // 描画品質設定のビット詰め（RenderQuality::Pack）
    };

    static_assert(sizeof(PostProcessConstants) <= RendererState::g_kPP_CB_ALIGNED_SIZE);

    UpscaleMode ResolveUpscaleMode(UINT inputWidth, UINT inputHeight, UINT outputWidth, UINT outputHeight)
    {
        UpscaleMode mode = RendererSettings::GetUpscaleMode();
        if (mode == UpscaleMode::Tsr)
        {
            return mode;
        }
        if (mode == UpscaleMode::Dlss)
        {
            return DlssUpscaler::IsRequested() ? UpscaleMode::Dlss : UpscaleMode::Bilateral;
        }
        if (!SpatialUpscaler::IsAvailable(mode) ||
            !SpatialUpscaler::IsScaleSupported(mode, inputWidth, inputHeight, outputWidth, outputHeight))
        {
            return UpscaleMode::Bilateral;
        }
        return mode;
    }

} // 名前空間

void PostProcessPass::UpdateCameraConstants()
{
    if (!m_pPostProcessCbvDataBegin)
    {
        return;
    }
    const EntityID camera = Camera::GetCameraEntity();
    XMFLOAT3 position = {0.0f, 0.0f, 5.0f};
    if (camera != g_kINVALID_ENTITY && ComponentManager::HasComponent<TransformComponent>(camera))
    {
        position = ComponentManager::GetComponentUnchecked<TransformComponent>(camera).Position;
    }
    XMMATRIX view = XMMatrixIdentity();
    XMMATRIX projection = XMMatrixIdentity();
    Camera::GetCameraMatrices(camera, view, projection);
    PostProcessConstants constants{};
    constants.PPCameraPos = {position.x, position.y, position.z, 1.0f};
    XMStoreFloat4x4(&constants.PPInvViewProjection, XMMatrixTranspose(XMMatrixInverse(nullptr, view * projection)));
    XMStoreFloat4x4(&constants.PPViewProjection, XMMatrixTranspose(view * projection));
    // 品質、カラー調整、天候のブロックを全画面パスの各スロットへコピーする。
    // フォワードシェーディングも同じスロット0のバインド（b6）を通して読む。
    static float s_QualityTime = 0.0f;
    static unsigned s_QualityFrame = 0;
    const float deltaTime = World::GetRawDeltaTime();
    s_QualityTime = fmodf(s_QualityTime + deltaTime, 3600.0f);
    RenderQuality::Tick(deltaTime);
    RenderQuality::Pack(constants.Quality, s_QualityTime, ++s_QualityFrame, TransparencyEffects::IsOitActive(),
                        TransparencyEffects::IsRefractionActive());
    if (!PbrLuts::IsAvailable())
    {
        // エネルギー補償と LTC 鏡面反射にはテーブルが必要。
        constants.Quality[9].x = 0.0f;
        constants.Quality[9].y = 0.0f;
    }
    memcpy(m_pPostProcessCbvDataBegin + GetPostProcessFrameOffset(), &constants, sizeof(constants));
}

namespace
{
    bool s_ColorManagementInitialized = false;
    ColorManagement::ColorSpaceConfig s_ColorConfig;
}

void InitializeColorManagement()
{
    if (!s_ColorManagementInitialized)
    {
        s_ColorConfig.configPath = "";
        s_ColorConfig.workingSpace = "ACES - ACEScg";
        s_ColorConfig.displayDevice = "sRGB";
        s_ColorConfig.viewTransform = "sRGB";
        s_ColorConfig.lookTransform = "";
        s_ColorConfig.exposure = ImGuiManager::GetExposure();
        s_ColorConfig.gamma = 2.2f;
        s_ColorConfig.enableOCIO = true;

        ColorManagement::GetColorManager().Initialize(s_ColorConfig);
        s_ColorManagementInitialized = true;
    }
    else
    {
        // Update exposure from ImGui
        ColorManagement::ColorSpaceConfig currentConfig = s_ColorConfig;
        currentConfig.exposure = ImGuiManager::GetExposure();
        ColorManagement::GetColorManager().Initialize(currentConfig);
    }
}

void PostProcessPass::ApplyPostProcess(const PostProcessComponent& config)
{
    InitializeColorManagement();
    const uint64_t historyConfiguration =
        static_cast<uint64_t>(m_AntiAliasingMode) |
        (static_cast<uint64_t>(ResolveUpscaleMode(m_SceneWidth, m_SceneHeight, m_Width, m_Height)) << 8) |
        (uint64_t(TemporalUpscaler::IsInputTaaEnabled()) << 16);
    g_CurrentTaaState = TemporalHistory::Context::Capture(Camera::GetCameraEntity(), m_SceneWidth, m_SceneHeight,
                                                          historyConfiguration, ImGuiManager::GetExposure());
    // カメラはすでにこのフレームのジッターを使っている。履歴の有効性判定で系列を巻き戻さない。
    // 特に TSR が独自の色履歴を持つ場合は、この順序を維持する。
    g_CanReuseTaaHistory = TemporalHistory::Evaluate(g_PreviousTaaState, g_CurrentTaaState, g_TaaHistoryValid).CanReuse;
    UpdateCameraConstants();
    UINT nextConstantSlot = 1; // スロット0は速度と AA が使うフレームのカメラデータを保持する。
    auto DrawFullscreenPass = [&](ID3D12PipelineState* pso, D3D12_CPU_DESCRIPTOR_HANDLE targetRtv,
                                  D3D12_GPU_DESCRIPTOR_HANDLE sourceHandle, float intensity, float renderModeFlag,
                                  float deferredLightStrength,
                                  D3D12_GPU_DESCRIPTOR_HANDLE atmosphereHandle = D3D12_GPU_DESCRIPTOR_HANDLE{},
                                  D3D12_GPU_DESCRIPTOR_HANDLE rimStyleHandle = D3D12_GPU_DESCRIPTOR_HANDLE{},
                                  D3D12_GPU_DESCRIPTOR_HANDLE rimLightHandle = D3D12_GPU_DESCRIPTOR_HANDLE{},
                                  D3D12_GPU_DESCRIPTOR_HANDLE bloomHandle = D3D12_GPU_DESCRIPTOR_HANDLE{},
                                  float bloomPass = 0.0f) {
        if (!pso || !m_PostProcessRootSignature || nextConstantSlot >= g_kPP_CB_SLOTS_PER_FRAME)
        {
            return;
        }

        const bool needsLightingConstants = deferredLightStrength > 0.0f;
        if (needsLightingConstants)
        {
            LightingResources::UpdateLightConstantBuffer(deferredLightStrength);
            LightingResources::UpdateShadowConstantBuffer();
        }

        m_CommandList->OMSetRenderTargets(1, &targetRtv, FALSE, nullptr);
        m_CommandList->SetPipelineState(pso);
        m_CommandList->SetGraphicsRootSignature(m_PostProcessRootSignature.Get());
        m_CommandList->SetGraphicsRootDescriptorTable(23,
                                                      m_GBufferSrvHandles[static_cast<UINT>(GBufferType::EMISSION_AO)]);
        TransmittanceShadows::BindLighting(m_CommandList.Get(), 24, 25);
        Raytracing::BindLighting(m_CommandList.Get(), 17, 18);
        m_CommandList->SetGraphicsRootDescriptorTable(19, Raytracing::GetReflectionSrv());
        CloudRenderer::BindLighting(m_CommandList.Get(), m_FrameIndex);
        m_CommandList->SetGraphicsRootDescriptorTable(26, AtmosphereLut::GetGraphicsSrv());
        m_CommandList->SetGraphicsRootDescriptorTable(27, PbrLuts::GetSrv());
        MegaLights::BindGraphics(m_CommandList.Get(), 28, 29, 30, 31, 32, 33, 34, 35, 36, 37);
        m_CommandList->SetGraphicsRootDescriptorTable(
            38, CD3DX12_GPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(),
                                              RendererState::g_kTEXTURE_SRV_START_INDEX, m_CbvIncrementSize));

        RenderFrame::SetDescriptorHeap();

        D3D12_GPU_VIRTUAL_ADDRESS constantAddress = 0;
        if (m_pPostProcessCbvDataBegin)
        {
            PostProcessConstants params{};
            memcpy(&params, m_pPostProcessCbvDataBegin + GetPostProcessFrameOffset(), sizeof(params));
            params.Flags = XMFLOAT4(ImGuiManager::GetExposure(), intensity, renderModeFlag, m_ResolutionScale);
            params.HdrFlags = XMFLOAT4(
                static_cast<float>(ImGuiManager::IsHdrEnabled()), static_cast<float>(ImGuiManager::IsToneMapEnabled()),
                static_cast<float>(RendererSettings::GetSsaoEnabled()),
                (Raytracing::IsGiActive() ? 2.0f : static_cast<float>(RendererSettings::GetSsgiEnabled())));
            params.BloomParams = XMFLOAT4(bloomPass, config.BloomThreshold, config.BloomSoftKnee, config.BloomRadius);
            params.RaytracingFlags = XMFLOAT4(static_cast<float>(Raytracing::IsActive()),
                                              static_cast<float>(Raytracing::UsesRaytracedAmbientOcclusion()),
                                              static_cast<float>(Raytracing::UsesRaytracedShadows()),
                                              static_cast<float>(Raytracing::AreReflectionsActive()));
            params.RaytracingSunDirection = Raytracing::GetSunDirection();
            const UINT offset = GetPostProcessFrameOffset() + nextConstantSlot++ * g_kPP_CB_ALIGNED_SIZE;
            auto* ppDst = m_pPostProcessCbvDataBegin + offset;
            memcpy(ppDst, &params, sizeof(params));
            memcpy(m_pPostProcessCbvDataBegin + GetPostProcessFrameOffset(), &params, sizeof(params));
            constantAddress = m_PostProcessConstantBuffer->GetGPUVirtualAddress() + offset;
        }
        m_CommandList->SetGraphicsRootDescriptorTable(0, sourceHandle);
        if (m_PostProcessConstantBuffer)
        {
            m_CommandList->SetGraphicsRootConstantBufferView(1, constantAddress);
        }
        if (m_LightConstantBuffer)
        {
            m_CommandList->SetGraphicsRootConstantBufferView(2,
                                                             LightingResources::GetCurrentLightConstantBufferAddress());
        }
        int environmentSrvIndex = m_EnvironmentTextureSrvIndex;
        if (environmentSrvIndex < 0)
        {
            environmentSrvIndex = TextureManager::GetDefaultTextureIndex();
        }
        CD3DX12_GPU_DESCRIPTOR_HANDLE environmentSrvHandle(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(),
                                                           environmentSrvIndex, m_CbvIncrementSize);
        m_CommandList->SetGraphicsRootDescriptorTable(3, environmentSrvHandle);
        if (m_ShadowDepthBuffer)
        {
            CD3DX12_GPU_DESCRIPTOR_HANDLE shadowSrvHandle(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(),
                                                          RendererState::g_kSHADOW_SRV_INDEX, m_CbvIncrementSize);
            m_CommandList->SetGraphicsRootDescriptorTable(4, shadowSrvHandle);
        }
        if (m_ShadowConstantBuffer)
        {
            m_CommandList->SetGraphicsRootConstantBufferView(
                5, LightingResources::GetCurrentShadowConstantBufferAddress());
        }
        if (m_PBRConstantBuffer)
        {
            m_CommandList->SetGraphicsRootConstantBufferView(6, MaterialBindings::GetPBRConstantBufferAddress());
        }
        if (atmosphereHandle.ptr != 0)
        {
            m_CommandList->SetGraphicsRootDescriptorTable(7, atmosphereHandle);
        }
        if (rimStyleHandle.ptr != 0)
        {
            m_CommandList->SetGraphicsRootDescriptorTable(8, rimStyleHandle);
        }
        if (rimLightHandle.ptr != 0)
        {
            m_CommandList->SetGraphicsRootDescriptorTable(9, rimLightHandle);
        }
        const D3D12_GPU_VIRTUAL_ADDRESS lightTileAddress = LightingResources::GetCurrentLightTileIndexBufferAddress();
        if (lightTileAddress != 0)
        {
            m_CommandList->SetGraphicsRootShaderResourceView(10, lightTileAddress);
        }
        int monitorSrvIndex = m_MonitorTextureSrvIndex;
        if (monitorSrvIndex < 0)
        {
            monitorSrvIndex = TextureManager::GetDefaultTextureIndex();
        }
        CD3DX12_GPU_DESCRIPTOR_HANDLE monitorSrvHandle(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(), monitorSrvIndex,
                                                       m_CbvIncrementSize);
        m_CommandList->SetGraphicsRootDescriptorTable(11, monitorSrvHandle);
        m_CommandList->SetGraphicsRootDescriptorTable(12, ScreenSpaceEffects::GetAoSrv());
        m_CommandList->SetGraphicsRootDescriptorTable(
            13, (Raytracing::IsGiActive() ? Raytracing::GetGiSrv() : ScreenSpaceEffects::GetGiSrv()));
        m_CommandList->SetGraphicsRootDescriptorTable(16, Raytracing::GetResultSrv());
        const D3D12_GPU_VIRTUAL_ADDRESS volumetricLightAddress =
            LightingResources::GetCurrentVolumetricLightIndexBufferAddress();
        if (volumetricLightAddress != 0)
        {
            m_CommandList->SetGraphicsRootShaderResourceView(14, volumetricLightAddress);
        }
        D3D12_GPU_DESCRIPTOR_HANDLE bloomSourceHandle;
        if (bloomHandle.ptr != 0)
        {
            bloomSourceHandle = bloomHandle;
        }
        else
        {
            bloomSourceHandle = sourceHandle;
        }
        m_CommandList->SetGraphicsRootDescriptorTable(15, bloomSourceHandle);
        m_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_CommandList->DrawInstanced(3, 1, 0, 0);
    };

    if (!GetRenderTarget(RenderTargetType::EditorScene))
    {
        return;
    }

    // AdvancedEffects のデュアルフィルターブルームが HDR に加算済みなので、二重適用を避ける。
    const bool dualBloomReplacesLegacy = config.Type == PostProcessType::BLOOM && AdvancedEffects::IsDualBloomActive();
    ID3D12PipelineState* postProcessPso =
        PsoManager::GetPostProcessPso(dualBloomReplacesLegacy ? PostProcessType::NONE : config.Type);

    if (m_RenderMode == RenderMode::DEFERRED)
    {
        ID3D12PipelineState* deferredLightingPso = PsoManager::GetDeferredLightingPso();
        ID3D12PipelineState* atmospherePso = PsoManager::GetAtmospherePso();
        const UINT atmosphereIndex = static_cast<UINT>(GBufferType::ATMOSPHERE);
        if (!deferredLightingPso || !atmospherePso || !GetRenderTarget(RenderTargetType::Scene) ||
            !m_GBufferTargets[atmosphereIndex])
        {
            return;
        }

        CloudRenderer::PrepareLighting(m_CommandList.Get(), m_FrameIndex, m_SceneWidth, m_SceneHeight,
                                       World::GetRawDeltaTime());
        Raytracing::Execute(m_CommandList.Get(), m_GBufferTargets[static_cast<UINT>(GBufferType::DEPTH)].Get(),
                            m_GBufferTargets[static_cast<UINT>(GBufferType::NORMAL)].Get(),
                            m_GBufferSrvHandles[static_cast<UINT>(GBufferType::DEPTH)],
                            m_GBufferSrvHandles[static_cast<UINT>(GBufferType::NORMAL)], m_FrameIndex);
        D3D12_RESOURCE_BARRIER atmosphereToRt = CD3DX12_RESOURCE_BARRIER::Transition(
            m_GBufferTargets[atmosphereIndex].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_CommandList->ResourceBarrier(1, &atmosphereToRt);
        const float atmosphereClear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        m_CommandList->ClearRenderTargetView(m_GBufferRtvHandles[atmosphereIndex], atmosphereClear, 0, nullptr);
        const D3D12_RESOURCE_DESC atmosphereDesc = m_GBufferTargets[atmosphereIndex]->GetDesc();
        const CD3DX12_VIEWPORT atmosphereViewport(0.0f, 0.0f, static_cast<float>(atmosphereDesc.Width),
                                                  static_cast<float>(atmosphereDesc.Height));
        const CD3DX12_RECT atmosphereScissor(0, 0, static_cast<LONG>(atmosphereDesc.Width),
                                             static_cast<LONG>(atmosphereDesc.Height));
        m_CommandList->RSSetViewports(1, &atmosphereViewport);
        m_CommandList->RSSetScissorRects(1, &atmosphereScissor);
        // フロクセルボリュームはピクセル単位の大気レイマーチを置き換える。クリア済みターゲットは
        // 入射散乱なし、透過率1を表す。実際の適用はライティング後に行う。
        if (!AdvancedEffects::IsFroxelFogActive())
        {
            RenderProfiler::ScopedEvent profile("Atmosphere", m_CommandList.Get());
            DrawFullscreenPass(atmospherePso, m_GBufferRtvHandles[atmosphereIndex],
                               m_GBufferSrvHandles[static_cast<UINT>(GBufferType::BASE_COLOR)], 1.0f, 1.0f, 1.35f);
        }
        m_CommandList->RSSetViewports(1, &m_Viewport);
        m_CommandList->RSSetScissorRects(1, &m_ScissorRect);
        D3D12_RESOURCE_BARRIER atmosphereToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            m_GBufferTargets[atmosphereIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_CommandList->ResourceBarrier(1, &atmosphereToSrv);

        D3D12_RESOURCE_BARRIER toSceneRt = CD3DX12_RESOURCE_BARRIER::Transition(
            GetRenderTarget(RenderTargetType::Scene).Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        ScreenSpaceEffects::Execute(m_CommandList.Get(), m_GBufferTargets[static_cast<UINT>(GBufferType::DEPTH)].Get(),
                                    m_GBufferTargets[static_cast<UINT>(GBufferType::NORMAL)].Get(),
                                    m_GBufferSrvHandles[static_cast<UINT>(GBufferType::DEPTH)],
                                    m_GBufferSrvHandles[static_cast<UINT>(GBufferType::NORMAL)], m_FrameIndex);
        // MegaLights：GBuffer の深度からクラスターライトグリッドを構築し、M2 ならライトのサンプリングと影レイまで行う
        // （ライトの収集はライト定数の更新時。TLAS は上の Raytracing::Execute で構築済み）
        {
            LightingResources::UpdateLightConstantBuffer(1.35f);
            ID3D12Resource* const megaLightGBuffer[MegaLights::kBuildGBufferCount] = {
                m_GBufferTargets[static_cast<UINT>(GBufferType::BASE_COLOR)].Get(),
                m_GBufferTargets[static_cast<UINT>(GBufferType::NORMAL)].Get(),
                m_GBufferTargets[static_cast<UINT>(GBufferType::DEPTH)].Get(),
                m_GBufferTargets[static_cast<UINT>(GBufferType::MATERIAL)].Get(),
                m_GBufferTargets[static_cast<UINT>(GBufferType::SHADOW)].Get(),
                m_GBufferTargets[static_cast<UINT>(GBufferType::VELOCITY)].Get()};
            MegaLights::Build(m_CommandList.Get(), m_CbvHeap.Get(), megaLightGBuffer,
                              m_GBufferSrvHandles[static_cast<UINT>(GBufferType::BASE_COLOR)],
                              m_GBufferSrvHandles[static_cast<UINT>(GBufferType::DEPTH)],
                              AdvancedEffects::IsFroxelFogActive());
        }
        m_CommandList->ResourceBarrier(1, &toSceneRt);
        m_CommandList->ClearRenderTargetView(GetRenderTargetRtvHandle(RenderTargetType::Scene), m_kSceneClearColor, 0,
                                             nullptr);

        // MegaLights M3：ML6 の結果をデモジュレートして書き出し（カラーは書かない）→ デノイズ。
        // ディファードライティングはデノイズ済みの照明に材質の反射率を掛け戻して使う
        if (MegaLights::WantsShadePass())
        {
            RenderProfiler::ScopedEvent megaLightsProfile("MegaLights Shade", m_CommandList.Get());
            ID3D12PipelineState* megaLightsShadePso =
                MegaLights::GetShadePso(m_PostProcessRootSignature.Get(), m_SceneColorFormat);
            if (megaLightsShadePso)
            {
                MegaLights::BeginShade(m_CommandList.Get());
                DrawFullscreenPass(megaLightsShadePso, GetRenderTargetRtvHandle(RenderTargetType::Scene),
                                   m_GBufferSrvHandles[static_cast<UINT>(GBufferType::BASE_COLOR)], 1.0f, 1.0f, 1.35f,
                                   m_GBufferSrvHandles[atmosphereIndex]);
                ID3D12Resource* const megaLightsDenoiseGBuffer[3] = {
                    m_GBufferTargets[static_cast<UINT>(GBufferType::NORMAL)].Get(),
                    m_GBufferTargets[static_cast<UINT>(GBufferType::DEPTH)].Get(),
                    m_GBufferTargets[static_cast<UINT>(GBufferType::VELOCITY)].Get()};
                MegaLights::Denoise(m_CommandList.Get(), m_CbvHeap.Get(), megaLightsDenoiseGBuffer,
                                    m_GBufferSrvHandles[static_cast<UINT>(GBufferType::BASE_COLOR)]);
            }
            else
            {
                MegaLights::CancelDenoise();
            }
        }

        {
            RenderProfiler::ScopedEvent profile("Deferred Lighting", m_CommandList.Get());
            DrawFullscreenPass(deferredLightingPso, GetRenderTargetRtvHandle(RenderTargetType::Scene),
                               m_GBufferSrvHandles[static_cast<UINT>(GBufferType::BASE_COLOR)], 1.0f, 1.0f, 1.35f,
                               m_GBufferSrvHandles[atmosphereIndex]);
        }

        CloudRenderer::Render(m_CommandList.Get(), m_FrameIndex, m_SceneWidth, m_SceneHeight,
                              m_GBufferSrvHandles[static_cast<UINT>(GBufferType::DEPTH)],
                              m_GBufferSrvHandles[static_cast<UINT>(GBufferType::MATERIAL)],
                              m_PostProcessConstantBuffer->GetGPUVirtualAddress() + GetPostProcessFrameOffset(),
                              LightingResources::GetCurrentLightConstantBufferAddress(),
                              GetRenderTargetRtvHandle(RenderTargetType::Scene), m_SceneColorFormat);
        m_CommandList->RSSetViewports(1, &m_Viewport);
        m_CommandList->RSSetScissorRects(1, &m_ScissorRect);

        {
            RenderProfiler::ScopedEvent profile("HDR Transparent", m_CommandList.Get());
            ScenePasses::BeginHdrTransparentPass();
            SystemManager::DrawSystem(RenderPass::TransparentScene, false);
            ScenePasses::EndHdrTransparentPass();
        }
        D3D12_RESOURCE_BARRIER toSceneSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            GetRenderTarget(RenderTargetType::Scene).Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_CommandList->ResourceBarrier(1, &toSceneSrv);
        ScreenSpaceEffects::CaptureHistory(m_CommandList.Get(), GetRenderTarget(RenderTargetType::Scene).Get());
        // 線形 HDR シーンにフロクセルフォグ、自動露出、モーションブラー、DOF、ブルーム、
        // レンズフレア、雨雪をトーンマッピング前に適用する。
        AdvancedEffects::SetBloomRequested(config.Type == PostProcessType::BLOOM);
        AdvancedEffects::Execute(m_CommandList.Get(), GetRenderTarget(RenderTargetType::Scene).Get(),
                                 m_GBufferTargets[static_cast<UINT>(GBufferType::DEPTH)].Get(),
                                 m_GBufferTargets[static_cast<UINT>(GBufferType::VELOCITY)].Get(), m_FrameIndex);
        RenderFrame::SetDescriptorHeap();
    }

    if (m_RenderMode != RenderMode::DEFERRED)
    {
        auto* scene = GetRenderTarget(RenderTargetType::Scene).Get();
        auto toTarget = CD3DX12_RESOURCE_BARRIER::Transition(scene, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                             D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_CommandList->ResourceBarrier(1, &toTarget);
        ScenePasses::BeginHdrTransparentPass();
        SystemManager::DrawSystem(RenderPass::TransparentScene, false);
        ScenePasses::EndHdrTransparentPass();
        auto toShader = CD3DX12_RESOURCE_BARRIER::Transition(scene, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_CommandList->ResourceBarrier(1, &toShader);
    }
    if (!postProcessPso || !GetRenderTarget(RenderTargetType::PostProcess))
    {
        return;
    }

    const UINT bloomIndex = static_cast<UINT>(GBufferType::BLOOM);
    const bool useBloomBuffer = config.Type == PostProcessType::BLOOM && !dualBloomReplacesLegacy &&
                                m_RenderMode == RenderMode::DEFERRED && m_GBufferTargets[bloomIndex];
    if (useBloomBuffer)
    {
        D3D12_RESOURCE_BARRIER bloomToRt = CD3DX12_RESOURCE_BARRIER::Transition(
            m_GBufferTargets[bloomIndex].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_CommandList->ResourceBarrier(1, &bloomToRt);
        const float bloomClear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        m_CommandList->ClearRenderTargetView(m_GBufferRtvHandles[bloomIndex], bloomClear, 0, nullptr);
        {
            const D3D12_RESOURCE_DESC bloomDesc = m_GBufferTargets[bloomIndex]->GetDesc();
            const CD3DX12_VIEWPORT bloomViewport(0.0f, 0.0f, static_cast<float>(bloomDesc.Width),
                                                 static_cast<float>(bloomDesc.Height));
            const CD3DX12_RECT bloomScissor(0, 0, static_cast<LONG>(bloomDesc.Width),
                                            static_cast<LONG>(bloomDesc.Height));
            m_CommandList->RSSetViewports(1, &bloomViewport);
            m_CommandList->RSSetScissorRects(1, &bloomScissor);

            RenderProfiler::ScopedEvent profile("Bloom Extract", m_CommandList.Get());
            DrawFullscreenPass(m_BloomExtractPso.Get(), m_GBufferRtvHandles[bloomIndex],
                               GetRenderTargetSrvHandle(RenderTargetType::Scene), 1.0f, 0.0f, 0.0f,
                               D3D12_GPU_DESCRIPTOR_HANDLE{}, D3D12_GPU_DESCRIPTOR_HANDLE{},
                               D3D12_GPU_DESCRIPTOR_HANDLE{}, GetRenderTargetSrvHandle(RenderTargetType::Scene), 1.0f);

            m_CommandList->RSSetViewports(1, &m_Viewport);
            m_CommandList->RSSetScissorRects(1, &m_ScissorRect);
        }
        D3D12_RESOURCE_BARRIER bloomToSrv =
            CD3DX12_RESOURCE_BARRIER::Transition(m_GBufferTargets[bloomIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_CommandList->ResourceBarrier(1, &bloomToSrv);
    }

    D3D12_RESOURCE_BARRIER postToRt = CD3DX12_RESOURCE_BARRIER::Transition(
        GetRenderTarget(RenderTargetType::PostProcess).Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_CommandList->ResourceBarrier(1, &postToRt);
    m_CommandList->ClearRenderTargetView(GetRenderTargetRtvHandle(RenderTargetType::PostProcess), m_kSceneClearColor, 0,
                                         nullptr);
    m_CommandList->RSSetViewports(1, &m_Viewport);
    m_CommandList->RSSetScissorRects(1, &m_ScissorRect);
    {
        RenderProfiler::ScopedEvent profile("Final PostProcess", m_CommandList.Get());
        D3D12_GPU_DESCRIPTOR_HANDLE bloomSourceHandleValue;
        if (useBloomBuffer)
        {
            bloomSourceHandleValue = m_GBufferSrvHandles[bloomIndex];
        }
        else
        {
            bloomSourceHandleValue = D3D12_GPU_DESCRIPTOR_HANDLE{};
        }
        float shaderValue;
        if (useBloomBuffer)
        {
            shaderValue = 2.0f;
        }
        else
        {
            shaderValue = 0.0f;
        }
        DrawFullscreenPass(postProcessPso, GetRenderTargetRtvHandle(RenderTargetType::PostProcess),
                           GetRenderTargetSrvHandle(RenderTargetType::Scene), config.Intensity, 0.0f, 0.0f,
                           D3D12_GPU_DESCRIPTOR_HANDLE{}, D3D12_GPU_DESCRIPTOR_HANDLE{}, D3D12_GPU_DESCRIPTOR_HANDLE{},
                           bloomSourceHandleValue, shaderValue);
    }
    D3D12_RESOURCE_BARRIER postToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
        GetRenderTarget(RenderTargetType::PostProcess).Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_CommandList->ResourceBarrier(1, &postToSrv);

    const bool needsUpscale = m_SceneWidth != m_Width || m_SceneHeight != m_Height;
    const UpscaleMode upscaleMode = ResolveUpscaleMode(m_SceneWidth, m_SceneHeight, m_Width, m_Height);

    D3D12_GPU_DESCRIPTOR_HANDLE upscaleSourceSrv = GetRenderTargetSrvHandle(RenderTargetType::PostProcess);
    ID3D12Resource* upscaleSource = GetRenderTarget(RenderTargetType::PostProcess).Get();

    const bool tsrInputTaa = upscaleMode == UpscaleMode::Tsr && TemporalUpscaler::IsInputTaaEnabled();
    if (needsUpscale && (upscaleMode != UpscaleMode::Tsr || tsrInputTaa) && upscaleMode != UpscaleMode::Bilateral &&
        upscaleMode != UpscaleMode::Dlss && GetRenderTarget(RenderTargetType::PreUpscaleAa))
    {
        const bool useTaa = (m_AntiAliasingMode == AntiAliasingMode::TAA || tsrInputTaa) &&
                            GetRenderTarget(RenderTargetType::PreUpscaleAaHistory);
        ID3D12PipelineState* aaPso;
        if (useTaa)
        {
            aaPso = PsoManager::GetTaaBlendPso();
        }
        else
        {
            aaPso = PsoManager::GetFxaaPso();
        }
        if (aaPso && m_AaRootSignature)
        {
            const char* antiAliasingLabel;
            if (useTaa)
            {
                antiAliasingLabel = "Pre-upscale TAA";
            }
            else
            {
                antiAliasingLabel = "Pre-upscale FXAA";
            }
            RenderProfiler::ScopedEvent profile(antiAliasingLabel, m_CommandList.Get());
            RenderTargets::ActivatePreUpscaleAa(m_CommandList.Get());
            D3D12_RESOURCE_BARRIER preAaToRt = CD3DX12_RESOURCE_BARRIER::Transition(
                GetRenderTarget(RenderTargetType::PreUpscaleAa).Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            m_CommandList->ResourceBarrier(1, &preAaToRt);
            m_CommandList->ClearRenderTargetView(GetRenderTargetRtvHandle(RenderTargetType::PreUpscaleAa),
                                                 m_kSceneClearColor, 0, nullptr);
            m_CommandList->OMSetRenderTargets(1, &GetRenderTargetRtvHandle(RenderTargetType::PreUpscaleAa), FALSE,
                                              nullptr);
            m_CommandList->SetPipelineState(aaPso);
            m_CommandList->SetGraphicsRootSignature(m_AaRootSignature.Get());
            RenderFrame::SetDescriptorHeap();
            m_CommandList->SetGraphicsRootConstantBufferView(0, m_PostProcessConstantBuffer->GetGPUVirtualAddress() +
                                                                    GetPostProcessFrameOffset());
            m_CommandList->SetGraphicsRootDescriptorTable(1, GetRenderTargetSrvHandle(RenderTargetType::PostProcess));
            if (useTaa)
            {
                m_CommandList->SetGraphicsRootDescriptorTable(
                    2, GetRenderTargetSrvHandle(RenderTargetType::PreUpscaleAaHistory));
                m_CommandList->SetGraphicsRootDescriptorTable(
                    3, m_GBufferSrvHandles[static_cast<UINT>(GBufferType::DEPTH)]);
                m_CommandList->SetGraphicsRootDescriptorTable(
                    5, CD3DX12_GPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(),
                                                     RendererState::g_kVELOCITY_CALCULATION_SRV_INDEX,
                                                     m_CbvIncrementSize));

                XMMATRIX prevView = XMLoadFloat4x4(&m_PrevViewMatrix);
                XMMATRIX prevProj = XMLoadFloat4x4(&m_PrevProjMatrix);
                XMMATRIX prevViewProj = XMMatrixTranspose(prevView * prevProj);
                float constants[20] = {};
                memcpy(constants, &prevViewProj, sizeof(XMFLOAT4X4));
                constants[16] = 0.90f;
                constants[17] = 1.0f / max(static_cast<float>(m_SceneWidth), 1.0f);
                constants[18] = 1.0f / max(static_cast<float>(m_SceneHeight), 1.0f);
                constants[19] = static_cast<float>(g_CanReuseTaaHistory);
                m_CommandList->SetGraphicsRoot32BitConstants(4, 20, constants, 0);
            }
            else
            {
                m_CommandList->SetGraphicsRootDescriptorTable(2,
                                                              GetRenderTargetSrvHandle(RenderTargetType::PostProcess));
                m_CommandList->SetGraphicsRootDescriptorTable(3,
                                                              GetRenderTargetSrvHandle(RenderTargetType::PostProcess));
                m_CommandList->SetGraphicsRootDescriptorTable(5,
                                                              GetRenderTargetSrvHandle(RenderTargetType::PostProcess));
                const float reciprocalExtent[2] = {1.0f / max(static_cast<float>(m_SceneWidth), 1.0f),
                                                   1.0f / max(static_cast<float>(m_SceneHeight), 1.0f)};
                m_CommandList->SetGraphicsRoot32BitConstants(4, 2, reciprocalExtent, 0);
            }
            m_CommandList->RSSetViewports(1, &m_Viewport);
            m_CommandList->RSSetScissorRects(1, &m_ScissorRect);
            m_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_CommandList->DrawInstanced(3, 1, 0, 0);

            if (useTaa)
            {
                D3D12_RESOURCE_BARRIER toCopy[] = {
                    CD3DX12_RESOURCE_BARRIER::Transition(GetRenderTarget(RenderTargetType::PreUpscaleAa).Get(),
                                                         D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                         D3D12_RESOURCE_STATE_COPY_SOURCE),
                    CD3DX12_RESOURCE_BARRIER::Transition(GetRenderTarget(RenderTargetType::PreUpscaleAaHistory).Get(),
                                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                         D3D12_RESOURCE_STATE_COPY_DEST)};
                m_CommandList->ResourceBarrier(_countof(toCopy), toCopy);
                m_CommandList->CopyResource(GetRenderTarget(RenderTargetType::PreUpscaleAaHistory).Get(),
                                            GetRenderTarget(RenderTargetType::PreUpscaleAa).Get());
                D3D12_RESOURCE_BARRIER toShaderRead[] = {
                    CD3DX12_RESOURCE_BARRIER::Transition(GetRenderTarget(RenderTargetType::PreUpscaleAa).Get(),
                                                         D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                    CD3DX12_RESOURCE_BARRIER::Transition(GetRenderTarget(RenderTargetType::PreUpscaleAaHistory).Get(),
                                                         D3D12_RESOURCE_STATE_COPY_DEST,
                                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
                m_CommandList->ResourceBarrier(_countof(toShaderRead), toShaderRead);
                // TSR は再構成後に共通カメラのジッターを1回進める。
                if (!tsrInputTaa)
                {
                    ++m_TaaFrameIndex;
                }
                g_PreviousTaaState = g_CurrentTaaState;
                g_TaaHistoryValid = true;
            }
            else
            {
                D3D12_RESOURCE_BARRIER preAaToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                    GetRenderTarget(RenderTargetType::PreUpscaleAa).Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                m_CommandList->ResourceBarrier(1, &preAaToSrv);
            }
            upscaleSourceSrv = GetRenderTargetSrvHandle(RenderTargetType::PreUpscaleAa);
            upscaleSource = GetRenderTarget(RenderTargetType::PreUpscaleAa).Get();
        }
    }

    bool tsrRendered = false;
    if (upscaleMode == UpscaleMode::Tsr)
    {
        tsrRendered = TemporalUpscaler::Execute(m_CommandList.Get(), m_FrameIndex, upscaleSource,
                                                m_GBufferTargets[static_cast<UINT>(GBufferType::DEPTH)].Get(),
                                                m_GBufferTargets[static_cast<UINT>(GBufferType::VELOCITY)].Get(),
                                                TransparencyEffects::GetReactiveRevealage(),
                                                GetRenderTarget(RenderTargetType::EditorScene).Get(),
                                                ImGuiManager::GetExposure());
        ++m_TaaFrameIndex;
        RenderFrame::SetDescriptorHeap();
    }
    else
    {
        TemporalUpscaler::Invalidate();
    }
    bool dlssRendered = false;
    if (needsUpscale && upscaleMode == UpscaleMode::Dlss)
    {
        ID3D12Resource* depth = m_GBufferTargets[static_cast<UINT>(GBufferType::DEPTH)].Get();
        ID3D12Resource* velocity = m_GBufferTargets[static_cast<UINT>(GBufferType::VELOCITY)].Get();
        ID3D12Resource* output = GetRenderTarget(RenderTargetType::EditorScene).Get();
        if (depth && velocity && output)
        {
            D3D12_RESOURCE_BARRIER barriers[] = {
                CD3DX12_RESOURCE_BARRIER::Transition(upscaleSource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(velocity, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(output, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
            m_CommandList->ResourceBarrier(_countof(barriers), barriers);
            {
                RenderProfiler::ScopedEvent profile("DLSS Super Resolution", m_CommandList.Get());
                dlssRendered = DlssUpscaler::Execute(
                    m_CommandList.Get(), m_FrameIndex, upscaleSource, upscaleSourceSrv, depth, velocity,
                    m_GBufferSrvHandles[static_cast<UINT>(GBufferType::VELOCITY)], output, m_EditorSceneUavHandle);
            }
            for (auto& barrier : barriers)
            {
                swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            }
            m_CommandList->ResourceBarrier(_countof(barriers), barriers);
            RenderFrame::SetDescriptorHeap();
        }
    }

    if (dlssRendered || tsrRendered)
    {
        // DLSS は表示解像度の画像生成とアンチエイリアスを完了している。
    }
    else if (!needsUpscale)
    {
        D3D12_RESOURCE_BARRIER barriers[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(upscaleSource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(GetRenderTarget(RenderTargetType::EditorScene).Get(),
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_COPY_DEST)};
        m_CommandList->ResourceBarrier(_countof(barriers), barriers);
        m_CommandList->CopyResource(GetRenderTarget(RenderTargetType::EditorScene).Get(), upscaleSource);
        barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(upscaleSource, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(GetRenderTarget(RenderTargetType::EditorScene).Get(),
                                                           D3D12_RESOURCE_STATE_COPY_DEST,
                                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_CommandList->ResourceBarrier(_countof(barriers), barriers);
    }
    else if (upscaleMode == UpscaleMode::Bilateral || upscaleMode == UpscaleMode::Dlss ||
             upscaleMode == UpscaleMode::Tsr)
    {
        D3D12_RESOURCE_BARRIER editorToRt = CD3DX12_RESOURCE_BARRIER::Transition(
            GetRenderTarget(RenderTargetType::EditorScene).Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_CommandList->ResourceBarrier(1, &editorToRt);
        m_CommandList->ClearRenderTargetView(GetRenderTargetRtvHandle(RenderTargetType::EditorScene),
                                             m_kSceneClearColor, 0, nullptr);
        m_CommandList->RSSetViewports(1, &m_FullViewport);
        m_CommandList->RSSetScissorRects(1, &m_FullScissorRect);

        ID3D12PipelineState* upscalePso = PsoManager::GetUpscaleBilateralPso();
        if (upscalePso && m_UpscaleRootSignature)
        {
            RenderProfiler::ScopedEvent profile("Bilateral Upscale", m_CommandList.Get());
            m_CommandList->OMSetRenderTargets(1, &GetRenderTargetRtvHandle(RenderTargetType::EditorScene), FALSE,
                                              nullptr);
            m_CommandList->SetPipelineState(upscalePso);
            m_CommandList->SetGraphicsRootSignature(m_UpscaleRootSignature.Get());
            RenderFrame::SetDescriptorHeap();
            m_CommandList->SetGraphicsRootDescriptorTable(0, upscaleSourceSrv);
            m_CommandList->SetGraphicsRootDescriptorTable(1,
                                                          m_GBufferSrvHandles[static_cast<UINT>(GBufferType::DEPTH)]);
            m_CommandList->SetGraphicsRootConstantBufferView(2, m_PostProcessConstantBuffer->GetGPUVirtualAddress() +
                                                                    GetPostProcessFrameOffset());
            m_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_CommandList->DrawInstanced(3, 1, 0, 0);
        }
        D3D12_RESOURCE_BARRIER editorToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            GetRenderTarget(RenderTargetType::EditorScene).Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_CommandList->ResourceBarrier(1, &editorToSrv);
    }
    else
    {
        D3D12_RESOURCE_BARRIER barriers[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(upscaleSource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(GetRenderTarget(RenderTargetType::EditorScene).Get(),
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        m_CommandList->ResourceBarrier(_countof(barriers), barriers);
        {
            const char* upscalerLabel;
            if (upscaleMode == UpscaleMode::Fsr1)
            {
                upscalerLabel = "FSR 1 EASU + RCAS";
            }
            else if (SpatialUpscaler::NisPassCount(m_SceneWidth, m_SceneHeight, m_Width, m_Height) > 1)
            {
                upscalerLabel = "NVIDIA Image Scaling (multi-pass)";
            }
            else
            {
                upscalerLabel = "NVIDIA Image Scaling";
            }
            RenderProfiler::ScopedEvent profile(upscalerLabel, m_CommandList.Get());
            SpatialUpscaler::Execute(m_CommandList.Get(), upscaleSourceSrv,
                                     GetRenderTarget(RenderTargetType::EditorScene).Get(), m_EditorSceneUavHandle,
                                     m_SceneWidth, m_SceneHeight, m_Width, m_Height, upscaleMode, upscaleSource,
                                     GetRenderTargetSrvHandle(RenderTargetType::EditorScene));
        }
        barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
            upscaleSource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(GetRenderTarget(RenderTargetType::EditorScene).Get(),
                                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_CommandList->ResourceBarrier(_countof(barriers), barriers);
    }

    m_CommandList->RSSetViewports(1, &m_FullViewport);
    m_CommandList->RSSetScissorRects(1, &m_FullScissorRect);

    // Apply color management display transform to the final output
    ID3D12Resource* finalOutput = GetRenderTarget(RenderTargetType::EditorScene).Get();
    if (finalOutput && ColorManagement::GetColorManager().IsEnabled())
    {
        // The display transform is applied in the final output shader.
        // For now, we rely on the CPU-side implementation for readback/validation.
        // A full GPU implementation would dispatch a compute shader here with
        // the OCIO-generated 3D LUT or 1D LUTs.
    }
}

namespace
{
    void ApplyColorManagementDisplayTransform(ID3D12GraphicsCommandList* commandList,
                                               ID3D12Resource* sourceTexture,
                                               ID3D12Resource* destTexture,
                                               int width, int height)
    {
        if (!ColorManagement::GetColorManager().IsEnabled())
        {
            return;
        }

        // For now, we'll do a simple copy since implementing a full compute shader
        // for the display transform would require additional pipeline states.
        // The CPU-side ApplyDisplayTransform can be used for readback scenarios.
        // In a full implementation, this would dispatch a compute shader with the
        // OCIO-generated 3D LUT or 1D LUTs.
    }
}

void PostProcessPass::ApplyAntiAliasing()
{
    if (TemporalUpscaler::IsRequested())
    {
        return;
    }
    if (!m_AaRenderTarget || !m_AaRootSignature)
    {
        return;
    }

    AntiAliasingMode mode = m_AntiAliasingMode;
    if (mode == AntiAliasingMode::NONE)
    {
        return;
    }
    const bool usesPreUpscaleAa =
        (m_SceneWidth != m_Width || m_SceneHeight != m_Height) &&
        ResolveUpscaleMode(m_SceneWidth, m_SceneHeight, m_Width, m_Height) != UpscaleMode::Bilateral;
    if (usesPreUpscaleAa)
    {

        return;
    }

    ID3D12PipelineState* pso = nullptr;
    if (mode == AntiAliasingMode::FXAA)
    {
        pso = PsoManager::GetFxaaPso();
    }
    else if (mode == AntiAliasingMode::TAA)
    {
        pso = PsoManager::GetTaaBlendPso();
    }
    else
    {
        return;
    }

    if (!pso)
    {
        return;
    }

    RenderFrame::SetDescriptorHeap();

    D3D12_RESOURCE_BARRIER toAaCopy = CD3DX12_RESOURCE_BARRIER::Transition(
        m_AaRenderTarget.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_CommandList->ResourceBarrier(1, &toAaCopy);

    m_CommandList->OMSetRenderTargets(1, &m_AaRtvHandle, FALSE, nullptr);
    m_CommandList->SetPipelineState(pso);
    m_CommandList->SetGraphicsRootSignature(m_AaRootSignature.Get());
    m_CommandList->RSSetViewports(1, &m_FullViewport);
    m_CommandList->RSSetScissorRects(1, &m_FullScissorRect);

    m_CommandList->SetGraphicsRootConstantBufferView(0, m_PostProcessConstantBuffer->GetGPUVirtualAddress() +
                                                            GetPostProcessFrameOffset());

    m_CommandList->SetGraphicsRootDescriptorTable(1, GetRenderTargetSrvHandle(RenderTargetType::EditorScene));

    if (mode == AntiAliasingMode::FXAA)
    {
        m_CommandList->SetGraphicsRootDescriptorTable(2, GetRenderTargetSrvHandle(RenderTargetType::EditorScene));
        m_CommandList->SetGraphicsRootDescriptorTable(3, GetRenderTargetSrvHandle(RenderTargetType::EditorScene));
        m_CommandList->SetGraphicsRootDescriptorTable(5, GetRenderTargetSrvHandle(RenderTargetType::EditorScene));

        struct FxaaCb
        {
            float rcpWidth;
            float rcpHeight;
        } cb;

        cb.rcpWidth = 1.0f / (float)m_Width;
        cb.rcpHeight = 1.0f / (float)m_Height;
        m_CommandList->SetGraphicsRoot32BitConstants(4, 2, &cb, 0);
    }
    else if (mode == AntiAliasingMode::TAA)
    {
        CD3DX12_GPU_DESCRIPTOR_HANDLE historySrv(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(),
                                                 RendererState::g_kAA_HISTORY_SRV_INDEX, m_CbvIncrementSize);
        m_CommandList->SetGraphicsRootDescriptorTable(2, historySrv);

        CD3DX12_GPU_DESCRIPTOR_HANDLE depthSrv(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(),
                                               RendererState::g_kDEPTH_SRV_INDEX, m_CbvIncrementSize);
        m_CommandList->SetGraphicsRootDescriptorTable(3, depthSrv);
        CD3DX12_GPU_DESCRIPTOR_HANDLE velocityCalculationSrv(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(),
                                                             RendererState::g_kVELOCITY_CALCULATION_SRV_INDEX,
                                                             m_CbvIncrementSize);
        m_CommandList->SetGraphicsRootDescriptorTable(5, velocityCalculationSrv);

        XMMATRIX prevView = XMLoadFloat4x4(&m_PrevViewMatrix);
        XMMATRIX prevProj = XMLoadFloat4x4(&m_PrevProjMatrix);
        XMMATRIX prevViewProj = XMMatrixTranspose(prevView * prevProj);

        float cbData[20] = {};
        memcpy(cbData, &prevViewProj, 64);
        cbData[16] = 0.90f;
        cbData[17] = 1.0f / (float)m_Width;
        cbData[18] = 1.0f / (float)m_Height;
        cbData[19] = static_cast<float>(g_CanReuseTaaHistory);
        m_CommandList->SetGraphicsRoot32BitConstants(4, 20, cbData, 0);
    }

    m_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_CommandList->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER toAaSrv = CD3DX12_RESOURCE_BARRIER::Transition(
        m_AaRenderTarget.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_CommandList->ResourceBarrier(1, &toAaSrv);

    D3D12_RESOURCE_BARRIER toEditorCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(
        GetRenderTarget(RenderTargetType::EditorScene).Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST);
    m_CommandList->ResourceBarrier(1, &toEditorCopyDest);

    m_CommandList->CopyResource(GetRenderTarget(RenderTargetType::EditorScene).Get(), m_AaRenderTarget.Get());

    if (mode == AntiAliasingMode::TAA)
    {
        m_TaaFrameIndex++;
        g_PreviousTaaState = g_CurrentTaaState;
        g_TaaHistoryValid = true;

        D3D12_RESOURCE_BARRIER toHistoryCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(
            m_AaRenderTarget.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        m_CommandList->ResourceBarrier(1, &toHistoryCopyDest);

        D3D12_RESOURCE_BARRIER toEditorCopySource =
            CD3DX12_RESOURCE_BARRIER::Transition(GetRenderTarget(RenderTargetType::EditorScene).Get(),
                                                 D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_CommandList->ResourceBarrier(1, &toEditorCopySource);

        m_CommandList->CopyResource(m_AaRenderTarget.Get(), GetRenderTarget(RenderTargetType::EditorScene).Get());

        D3D12_RESOURCE_BARRIER toHistorySrv = CD3DX12_RESOURCE_BARRIER::Transition(
            m_AaRenderTarget.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_CommandList->ResourceBarrier(1, &toHistorySrv);

        D3D12_RESOURCE_BARRIER toEditorSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            GetRenderTarget(RenderTargetType::EditorScene).Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_CommandList->ResourceBarrier(1, &toEditorSrv);
    }
    else
    {
        D3D12_RESOURCE_BARRIER toAaFinalSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            m_AaRenderTarget.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_CommandList->ResourceBarrier(1, &toAaFinalSrv);

        D3D12_RESOURCE_BARRIER toEditorFinalSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            GetRenderTarget(RenderTargetType::EditorScene).Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_CommandList->ResourceBarrier(1, &toEditorFinalSrv);
    }
}
