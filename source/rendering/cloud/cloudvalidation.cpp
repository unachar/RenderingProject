#include "pch.h"
#include "cloudvalidation.h"
#include "cloudrenderer.h"
#include "rendercapture.h"
#include "rendertargets.h"
#include "renderprofiler.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "renderconfiguration.h"
#include "renderersettings.h"
#include "imguimanager.h"
#include "raytracing.h"
#include "camera.h"
#include "world.h"
#include "sun.h"
#include "light.h"
#include "daynightcycle.h"
#include <cmath>

namespace
{
UINT g_Failures = 0;
UINT g_LastFrame = 0;
uint64_t g_NoiseHash = 0;
double g_InitialOpacity = 0;
double g_InitialRadiance = 0;
double g_ConnectedSunRadiance = 0;
RenderCapture::TextureData g_ConnectedSunCloud;
EntityID g_UnrelatedDirectional = g_kINVALID_ENTITY;
bool g_SawGpuTiming = false;
UINT g_RecordedFrameIndex = 0;
RenderCapture::TextureData g_CloudHdr;
std::vector<uint8_t> g_ForegroundMask;
struct TimingConfiguration { UINT Width; UINT Height; int ViewSteps; int LightSteps; };
std::vector<TimingConfiguration> g_Configurations;
std::array<std::vector<double>, 3> g_BenchmarkMs;
uint64_t g_LastTimingSerial = 0;
void Check(bool condition, const char* message)
{
    WriteGraphicsLog("Cloud validation: %s %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++g_Failures;
}
uint64_t NoiseHash()
{
    RenderCapture::TextureData noise;
    if (!RenderCapture::ReadTexture(CloudRenderer::GetBaseNoiseResource(), noise)) return 0;
    uint64_t hash = 1469598103934665603ull;
    const UINT depth = CloudRenderer::GetBaseNoiseResource()->GetDesc().DepthOrArraySize;
    const UINT bytesPerVoxel = noise.Format == DXGI_FORMAT_R8_UNORM ? 1 : 2;
    for (UINT z = 0; z < depth; ++z)
        for (UINT y = 0; y < noise.Height; ++y)
            for (UINT x = 0; x < noise.Width * bytesPerVoxel; ++x)
            {
                hash ^= noise.Bytes[(z * noise.Height + y) * noise.RowPitch + x];
                hash *= 1099511628211ull;
            }
    return hash;
}
void ValidateSceneOcclusion(const RenderCapture::TextureData& cloud)
{
    RenderCapture::TextureData depth, material;
    const bool read = RenderCapture::ReadTexture(RenderTargets::GetGBufferResource(GBufferType::DEPTH), depth) &&
        RenderCapture::ReadTexture(RenderTargets::GetGBufferResource(GBufferType::MATERIAL), material);
    Check(read && depth.Format == DXGI_FORMAT_R32_FLOAT, "read scene depth for occlusion oracle");
    if (!read) return;
    XMMATRIX view, projection;
    const auto camera = Camera::GetCameraEntity();
    Camera::GetCameraMatrices(camera, view, projection);
    const XMMATRIX inverse = XMMatrixInverse(nullptr, view * projection);
    const auto position = ComponentManager::GetComponentUnchecked<TransformComponent>(camera).Position;
    const auto& settings = CloudRenderer::GetSettings();
    UINT tested = 0;
    bool occluded = true;
    for (UINT y = 0; y < cloud.Height; ++y)
    {
        const UINT fullY = min(depth.Height - 1, static_cast<UINT>((y + 0.5f) / cloud.Height * depth.Height));
        const auto* depthRow = reinterpret_cast<const float*>(depth.Bytes.data() + fullY * depth.RowPitch);
        const auto* materialRow = reinterpret_cast<const uint16_t*>(material.Bytes.data() + fullY * material.RowPitch);
        const auto* cloudRow = reinterpret_cast<const uint16_t*>(cloud.Bytes.data() + y * cloud.RowPitch);
        for (UINT x = 0; x < cloud.Width; ++x)
        {
            const UINT fullX = min(depth.Width - 1, static_cast<UINT>((x + 0.5f) / cloud.Width * depth.Width));
            if (RenderCapture::HalfToFloat(materialRow[fullX * 4 + 3]) < -0.5f || depthRow[fullX] >= 0.999999f) continue;
            const float uvX = (x + 0.5f) / cloud.Width;
            const float uvY = (y + 0.5f) / cloud.Height;
            XMFLOAT3 world;
            XMStoreFloat3(&world, XMVector3TransformCoord(XMVectorSet(uvX * 2 - 1, 1 - uvY * 2, depthRow[fullX], 1), inverse));
            // The camera is below the cloud slab: geometry below its bottom cannot contain cloud.
            if (position.y < settings.BottomHeight && world.y < settings.BottomHeight - 0.01f)
            {
                // Raster interpolation and CPU UV division can choose adjacent full-res
                // texels at exact integer boundaries. Test interior footprints here;
                // the final HDR comparison below includes every silhouette pixel.
                bool interior = fullX > 0 && fullY > 0 && fullX + 1 < depth.Width && fullY + 1 < depth.Height;
                for (int row = -1; row <= 1 && interior; ++row)
                    for (int column = -1; column <= 1 && interior; ++column)
                    {
                        const UINT nearX = fullX + column;
                        const UINT nearY = fullY + row;
                        const auto* nearDepth = reinterpret_cast<const float*>(depth.Bytes.data() + nearY * depth.RowPitch);
                        const auto* nearMaterial = reinterpret_cast<const uint16_t*>(material.Bytes.data() + nearY * material.RowPitch);
                        const float ndcX = (nearX + 0.5f) / depth.Width * 2 - 1;
                        const float ndcY = 1 - (nearY + 0.5f) / depth.Height * 2;
                        const float nearWorldY = XMVectorGetY(XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, nearDepth[nearX], 1), inverse));
                        interior &= RenderCapture::HalfToFloat(nearMaterial[nearX * 4 + 3]) >= -0.5f && nearWorldY < settings.BottomHeight - 0.1f;
                    }
                if (!interior) continue;
                ++tested;
                occluded &= RenderCapture::HalfToFloat(cloudRow[x * 4 + 3]) == 1;
                for (UINT channel = 0; channel < 3; ++channel)
                    occluded &= RenderCapture::HalfToFloat(cloudRow[x * 4 + channel]) == 0;
            }
        }
    }
    WriteGraphicsLog("Cloud occlusion: checked foreground pixels=%u\n", tested);
    Check(tested > 100 && occluded, "foreground geometry clips cloud ray before layer entry");
    Check(RenderCapture::ReadTexture(RenderTargets::GetSceneResource(), g_CloudHdr), "read final HDR composite");
    g_ForegroundMask.assign(static_cast<size_t>(depth.Width) * depth.Height, 0);
    for (UINT y = 0; y < depth.Height; ++y)
    {
        const auto* depths = reinterpret_cast<const float*>(depth.Bytes.data() + y * depth.RowPitch);
        const auto* materials = reinterpret_cast<const uint16_t*>(material.Bytes.data() + y * material.RowPitch);
        for (UINT x = 0; x < depth.Width; ++x)
        {
            if (RenderCapture::HalfToFloat(materials[x * 4 + 3]) < -0.5f || depths[x] >= 0.999999f) continue;
            const float uvX = (x + 0.5f) / depth.Width;
            const float uvY = (y + 0.5f) / depth.Height;
            const float worldY = XMVectorGetY(XMVector3TransformCoord(XMVectorSet(uvX * 2 - 1, 1 - uvY * 2, depths[x], 1), inverse));
            g_ForegroundMask[y * depth.Width + x] = worldY < settings.BottomHeight - 0.01f;
        }
    }
}
void ValidateCompositeForeground()
{
    RenderCapture::TextureData clearHdr;
    const bool read = RenderCapture::ReadTexture(RenderTargets::GetSceneResource(), clearHdr);
    const bool compatible = read && clearHdr.Width == g_CloudHdr.Width && clearHdr.Height == g_CloudHdr.Height &&
        clearHdr.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && g_CloudHdr.Format == clearHdr.Format &&
        g_ForegroundMask.size() == static_cast<size_t>(clearHdr.Width) * clearHdr.Height &&
        !clearHdr.Bytes.empty() && !g_CloudHdr.Bytes.empty();
    Check(compatible, "read HDR reference without cloud shadows");
    if (!compatible) return;
    UINT tested = 0;
    bool attenuated = true;
    UINT shadowed = 0;
    for (UINT y = 0; y < clearHdr.Height; ++y)
    {
        const auto* actual = reinterpret_cast<const uint16_t*>(g_CloudHdr.Bytes.data() + y * g_CloudHdr.RowPitch);
        const auto* reference = reinterpret_cast<const uint16_t*>(clearHdr.Bytes.data() + y * clearHdr.RowPitch);
        for (UINT x = 0; x < clearHdr.Width; ++x)
            if (g_ForegroundMask[y * clearHdr.Width + x])
            {
                ++tested;
                for (UINT channel = 0; channel < 3; ++channel)
                {
                    const float lit = RenderCapture::HalfToFloat(reference[x * 4 + channel]);
                    const float shaded = RenderCapture::HalfToFloat(actual[x * 4 + channel]);
                    attenuated &= shaded <= lit + 0.002f;
                    if (lit - shaded > 0.005f) ++shadowed;
                }
            }
    }
    Check(tested > 100 && attenuated, "cloud shadow only attenuates foreground illumination");
    Check(shadowed > 100, "clouds cast measurable shadows on foreground receivers");
}
}
bool CloudValidation::Enabled()
{
    char value[8]{};
    return GetEnvironmentVariableA("DX12_CLOUD_TEST", value, sizeof(value)) && atoi(value) != 0;
}
void CloudValidation::BeforeFrame(uint64_t frame)
{
    g_LastFrame = static_cast<UINT>(frame);
    g_RecordedFrameIndex = GraphicsDevice::GetFrameIndex();
    if (frame == 52) RenderConfiguration::Resize(1920, 1080);
    if (frame == 64) RenderConfiguration::Resize(1200, 600);
    if (frame == 84) RenderConfiguration::Resize(1920, 1080);
    // Resize may select a new swap-chain index.
    g_RecordedFrameIndex = GraphicsDevice::GetFrameIndex();
    RenderConfiguration::SetHdr(true);
    RenderConfiguration::SetRenderMode(RenderMode::DEFERRED);
    RenderConfiguration::SetResolutionScale(frame >= 76 && frame < 80 ? 0.67f : 1.0f);
    Raytracing::GetSettings().Enabled = false;
    RendererState::m_AntiAliasingMode = AntiAliasingMode::NONE;
    auto& settings = CloudRenderer::GetSettings();
    settings = CloudSettings{};
    settings.Enabled = true;
    settings.WindSpeed = 0;
    settings.Coverage = 0.55f;
    if (frame == 0)
    {
        for (auto id : World::GetView<MeshComponent>())
        {
            if (ComponentManager::GetComponentUnchecked<MeshComponent>(id).IsEnvironment) continue;
            auto& tower = ComponentManager::GetComponentUnchecked<TransformComponent>(id);
            tower.Position = { 0, 18, 30 };
            tower.Scale = { 6, 35, 6 };
            tower.IsDirty = true;
        }
    }
    const auto camera = Camera::GetCameraEntity();
    auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(camera);
    auto& view = ComponentManager::GetComponentUnchecked<CameraComponent>(camera);
    transform.Position = { 0, 3, -8 };
    view.Target = { 0, 12, 40 };
    view.AllowUserControl = false;
    transform.IsDirty = true;
    // The selected solar light drives daylight; a shared virtual moon lights night.
    for (auto id : World::GetView<LightComponent>())
    {
        auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(id);
        light.IsActive = true;
        light.Intensity = frame >= 44 && frame < 48 ? 0.0f : frame >= 48 && frame < 52 ? 100.0f : 3.0f;
        light.Color = { 1, 0.9f, 0.75f, 1 };
        auto& sun = ComponentManager::GetComponentUnchecked<TransformComponent>(id);
        sun.Position = frame >= 48 && frame < 52 ? XMFLOAT3{ 0, 1, -8 } : XMFLOAT3{ 0, 8, -8 };
        sun.IsDirty = true;
    }
    const UINT scenario = static_cast<UINT>(frame / 4);
    switch (scenario)
    {
    case 1: settings.Coverage = 0; break;
    case 2: settings.Density = 0; break;
    case 3: transform.Position.y = 30; view.Target.y = 31; break;
    case 4: transform.Position.y = 60; view.Target.y = 0; break;
    case 5: view.Target.y = transform.Position.y; break;
    case 6: transform.Position.y = 30; view.Target.y = 30; break;
    case 7: view.Target = { 0.001f, 50, -8 }; break;
    case 8: view.Target = { 0.001f, -50, -8 }; break;
    case 9: settings.TopHeight = settings.BottomHeight + 0.01f; break;
    case 10: settings.TopHeight = 400; settings.Density = 10; settings.Coverage = 1; break;
    case 13: settings.ApplyQuality(CloudQuality::Low); break;
    case 14: settings.ApplyQuality(CloudQuality::Medium); break;
    case 15: settings.ApplyQuality(CloudQuality::High); break;
    case 16: settings.ResolutionScale = 1; break;
    case 17: settings.Seed = 1338; break;
    case 20: settings.WindSpeed = 1000; break;
    default: break;
    }
    if (frame == 72) CloudRenderer::GenerateNoise(1337);
    if (frame >= 84 && frame < 144) settings.ApplyQuality(CloudQuality::Low);
    if (frame >= 204 && frame < 264) settings.ApplyQuality(CloudQuality::High);
    if (frame >= 264 && frame < 292) settings.Debug = static_cast<CloudDebug>(1 + (frame - 264) / 4);
    if (frame >= 292)
    {
        settings.Debug = CloudDebug::Normal;
        settings.CastShadows = frame < 296;
        if (frame == 300)
        {
            Light::CreateDesc desc;
            desc.Name = "Cloud unrelated directional regression";
            desc.Direction = { 0, 1, 0 };
            desc.Color = { 1, 0, 1, 1 };
            desc.Intensity = 100;
            desc.Priority = 100;
            g_UnrelatedDirectional = Light::Create(desc);
        }
        if (Registry::IsAlive(g_UnrelatedDirectional))
        {
            auto& unrelated = ComponentManager::GetComponentUnchecked<LightComponent>(g_UnrelatedDirectional);
            unrelated.Intensity = 100;
            unrelated.Color = { 1, 0, 1, 1 };
        }
        if (frame >= 304)
            for (auto id : World::GetView<SunComponent, LightComponent>())
                ComponentManager::GetComponentUnchecked<LightComponent>(id).IsActive = false;
    }
    if (frame == 0) Check(CloudRenderer::IsAvailable(), "cloud pipelines initialize");
    if (frame >= 308)
    {
        auto& cycle = DayNightCycle::GetSettings();
        cycle.Running = false;
        cycle.AutomaticLighting = true;
        for (auto id : World::GetView<SunComponent, LightComponent>())
            ComponentManager::GetComponentUnchecked<LightComponent>(id).IsActive = true;
        if (Registry::IsAlive(g_UnrelatedDirectional))
            ComponentManager::GetComponentUnchecked<LightComponent>(g_UnrelatedDirectional).IsActive = false;
        DayNightCycle::SetTime(frame < 312 ? 17.15f : 0.0f);
        const auto direction = DayNightCycle::GetSunDirection();
        const float sign = frame < 312 ? 1.0f : -1.0f;
        view.Target = { transform.Position.x + direction.x * sign * 100,
            transform.Position.y + (std::max)(direction.y * sign, 0.15f) * 100,
            transform.Position.z + direction.z * sign * 100 };
    }
    g_Configurations.push_back({ static_cast<UINT>(ceilf(GraphicsDevice::GetWidth() * settings.ResolutionScale)),
        static_cast<UINT>(ceilf(GraphicsDevice::GetHeight() * settings.ResolutionScale)), settings.ViewSteps, settings.LightSteps });
}
void CloudValidation::AfterFrame(uint64_t frame)
{
    if (auto* rendered = CloudRenderer::GetCloudResource(g_RecordedFrameIndex))
    {
        const auto desc = rendered->GetDesc();
        g_Configurations.back().Width = static_cast<UINT>(desc.Width);
        g_Configurations.back().Height = desc.Height;
    }
    const auto& timing = RenderProfiler::GetLatest();
    if (timing.Serial && timing.Serial != g_LastTimingSerial && timing.Serial <= g_Configurations.size())
    {
        g_LastTimingSerial = timing.Serial;
        const auto& config = g_Configurations[timing.Serial - 1];
        for (const auto& pass : timing.Passes)
            if (pass.Name == "Cloud Total" || pass.Name == "Cloud Raymarch" || pass.Name == "Cloud Composite")
            {
                g_SawGpuTiming |= pass.GpuMs > 0;
                WriteGraphicsLog("Cloud profiling: serial=%llu %s %.3f ms resolution=%ux%u steps=%d/%d\n",
                    timing.Serial, pass.Name.c_str(), pass.GpuMs, config.Width, config.Height, config.ViewSteps, config.LightSteps);
                if (timing.Serial >= 85 && timing.Serial <= 264 && config.Width == 960 && config.Height == 540 && pass.Name == "Cloud Total" && pass.GpuMs > 0)
                {
                    const uint64_t benchmarkFrame = timing.Serial - 85;
                    const UINT quality = static_cast<UINT>(benchmarkFrame / 60);
                    if (benchmarkFrame % 60 >= 10) g_BenchmarkMs[quality].push_back(pass.GpuMs);
                }
            }
    }
    if (frame >= 84 && frame < 264) return;
    if (frame % 4 != 3) return;
    RenderCapture::TextureData cloud;
    const bool read = RenderCapture::ReadTexture(CloudRenderer::GetCloudResource(g_RecordedFrameIndex), cloud);
    Check(read && cloud.Format == DXGI_FORMAT_R16G16B16A16_FLOAT, "read scattering/transmittance buffer");
    if (!read) return;
    const auto& settings = CloudRenderer::GetSettings();
    Check(cloud.Width == max(1u, static_cast<UINT>(ceilf(GraphicsDevice::GetSceneWidth() * settings.ResolutionScale))) &&
        cloud.Height == max(1u, static_cast<UINT>(ceilf(GraphicsDevice::GetSceneHeight() * settings.ResolutionScale))),
        "cloud extent follows scene resize and resolution scale");
    bool valid = true;
    double opacity = 0;
    double radiance = 0;
    for (UINT y = 0; y < cloud.Height; ++y)
    {
        const auto* pixels = reinterpret_cast<const uint16_t*>(cloud.Bytes.data() + y * cloud.RowPitch);
        for (UINT x = 0; x < cloud.Width; ++x)
        {
            for (UINT channel = 0; channel < 4; ++channel)
            {
                const float value = RenderCapture::HalfToFloat(pixels[x * 4 + channel]);
                valid &= std::isfinite(value) && value >= 0;
                if (channel == 3) { valid &= value <= 1; opacity += 1 - value; }
                else radiance += value;
            }
        }
    }
    Check(valid, "finite nonnegative scattering and transmittance in [0,1]");
    if (frame >= 308)
    {
        Check(opacity > 1 && radiance > 0, "celestial lighting produces visible cloud scattering");
        if (frame == 311 || frame == 315)
        {
            const wchar_t* path = frame == 311 ? L"bin/validation/cloud-sunset.png" : L"bin/validation/cloud-moon.png";
            Check(RenderCapture::SaveScenePng(path), "capture sunset or moonlit clouds");
        }
        if (frame == 315)
            Check(DayNightCycle::GetSunDirection().y < 0, "moonlit clouds render with sun below horizon");
        return;
    }
    if (frame >= 292)
    {
        if (frame == 295)
        {
            g_ConnectedSunCloud = cloud;
            g_ConnectedSunRadiance = radiance;
            ValidateSceneOcclusion(cloud);
        }
        if (frame == 299 || frame == 303)
        {
            bool identical = cloud.Width == g_ConnectedSunCloud.Width && cloud.Height == g_ConnectedSunCloud.Height;
            if (identical)
                for (UINT y = 0; y < cloud.Height; ++y)
                    identical &= memcmp(cloud.Bytes.data() + y * cloud.RowPitch,
                        g_ConnectedSunCloud.Bytes.data() + y * g_ConnectedSunCloud.RowPitch, cloud.Width * 8) == 0;
            Check(identical, frame == 299 ? "shadow toggle preserves cloud scattering" :
                "bright unrelated directional light does not replace the virtual sun");
            if (frame == 299) ValidateCompositeForeground();
        }
        if (frame == 307)
            Check(radiance < g_ConnectedSunRadiance, "inactive sun removes direct cloud scattering despite unrelated light");
        return;
    }
    if (frame >= 264)
    {
        Check(settings.Debug != CloudDebug::Normal, "runtime diagnostic view renders without shader recompile");
        return;
    }
    const UINT scenario = static_cast<UINT>(frame / 4);
    if (scenario == 0)
    {
        Check(opacity > 1 && radiance > 0, "volume is visible and lit");
        g_InitialOpacity = opacity;
        g_InitialRadiance = radiance;
        ValidateSceneOcclusion(cloud);
        g_NoiseHash = NoiseHash();
        Check(g_NoiseHash != 0, "GPU base noise readback");
    }
    if (scenario == 1 || scenario == 2 || scenario == 8)
        Check(opacity == 0 && radiance == 0, "clear/zero-density/nonintersecting rays transmit scene exactly");
    if (scenario == 1) ValidateCompositeForeground();
    if (scenario == 11) Check(radiance < g_InitialRadiance, "zero sun intensity reduces cloud scattering");
    if (scenario == 5)
    {
        bool horizontalClear = true;
        for (UINT y = cloud.Height / 2 - 1; y <= cloud.Height / 2; ++y)
        {
            const auto* pixels = reinterpret_cast<const uint16_t*>(cloud.Bytes.data() + y * cloud.RowPitch);
            for (UINT x = 0; x < cloud.Width; ++x)
                horizontalClear &= RenderCapture::HalfToFloat(pixels[x * 4 + 3]) == 1;
        }
        Check(horizontalClear, "horizontal rays below layer remain clear");
    }
    if (scenario == 17) Check(NoiseHash() != g_NoiseHash, "different seed changes GPU noise");
    if (scenario == 18)
    {
        Check(NoiseHash() == g_NoiseHash, "same seed regenerates identical GPU noise");
        Check(std::abs(opacity - g_InitialOpacity) < 0.01, "returning camera and seed restores world-space cloud");
        Check(CloudRenderer::GetNoiseGenerationCount() == 3, "noise generated only at startup and seed changes");
    }
    if (scenario == 0 || scenario == 3 || scenario == 4 || scenario == 6 || scenario == 20)
    {
        wchar_t path[128]{};
        swprintf_s(path, L"bin/validation/cloud-%02u.png", scenario);
        Check(RenderCapture::SaveScenePng(path), "capture cloud scenario");
    }
}
bool CloudValidation::Finish()
{
    Check(g_LastFrame >= 315, "complete cloud validation including sunset and moonlight");
    Check(g_SawGpuTiming, "cloud GPU timestamps available");
    const char* qualityNames[] = { "Low", "Medium", "High" };
    std::array<double, 3> medians{};
    for (UINT quality = 0; quality < 3; ++quality)
    {
        auto& samples = g_BenchmarkMs[quality];
        Check(samples.size() >= 30, "warmed 1920x1080 GPU samples for quality preset");
        if (samples.empty()) continue;
        std::sort(samples.begin(), samples.end());
        medians[quality] = samples[samples.size() / 2];
        WriteGraphicsLog("Cloud benchmark: 1920x1080 %s samples=%zu median=%.3f ms p95=%.3f ms\n",
            qualityNames[quality], samples.size(), medians[quality],
            samples[static_cast<size_t>((samples.size() - 1) * 0.95)]);
    }
    Check(medians[0] < medians[1] && medians[1] < medians[2], "quality presets change measured GPU load");
    g_Failures += RenderCapture::ReportGraphicsErrors();
    ComPtr<ID3D12InfoQueue> info;
    if (SUCCEEDED(GraphicsDevice::GetDevice()->QueryInterface(IID_PPV_ARGS(&info))))
    {
        UINT warnings = 0;
        UINT legacyWarnings = 0;
        UINT trackingLimitations = 0;
        for (UINT64 index = 0; index < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index)
        {
            SIZE_T bytes = 0;
            info->GetMessage(index, nullptr, &bytes);
            std::vector<BYTE> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(info->GetMessage(index, message, &bytes)) && message->Severity == D3D12_MESSAGE_SEVERITY_WARNING)
            {
                // Reproduced with clouds disabled in the existing Sun Lighting test.
                // Report separately: existing indirect VB/IB/root updates limit GBV.
                if (message->ID == D3D12_MESSAGE_ID_GPU_BASED_VALIDATION_RESOURCE_STATE_IMPRECISE)
                {
                    ++trackingLimitations;
                    continue;
                }
                // Existing readback helpers use COPY_DEST on readback buffers, and
                // legacy scene passes clear with values different from optimized clears.
                // Cloud passes neither create those buffers nor issue clears.
                if (message->ID == D3D12_MESSAGE_ID_CREATERESOURCE_STATE_IGNORED ||
                    message->ID == D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE)
                {
                    ++legacyWarnings;
                    continue;
                }
                ++warnings;
                WriteGraphicsLog("Cloud validation D3D12 warning id=%u: %s\n", static_cast<UINT>(message->ID), message->pDescription);
            }
        }
        WriteGraphicsLog("Cloud validation: existing readback/scene-clear warnings = %u\n", legacyWarnings);
        WriteGraphicsLog("Cloud validation: existing GBV tracking limitations = %u\n", trackingLimitations);
        Check(warnings == 0, "no additional D3D12 resource/descriptor/barrier warnings");
    }
    WriteGraphicsLog("Validation: cloud failures = %u\n", g_Failures);
    return g_Failures == 0;
}
