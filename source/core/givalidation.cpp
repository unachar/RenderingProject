#include "pch.h"
#include "givalidation.h"
#include "envflags.h"
#include "temporallightingvalidation.h"
#include "celestialocclusionvalidation.h"
#include "world.h"
#include "camera.h"
#include "primitivegeometry.h"
#include "materialmanager.h"
#include "renderersettings.h"
#include "raytracing.h"
#include "rayreflections.h"
#include "optixdenoiser.h"
#include "rendercapture.h"
#include "renderprofiler.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "imguimanager.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "cloud/cloudrenderer.h"
#include <fstream>

namespace
{
    UINT failures = 0;
    AssetHandle emitterMaterial = 0;
    AssetHandle blueTexture = 0;
    EntityID emitter = g_kINVALID_ENTITY;
    EntityID blocker = g_kINVALID_ENTITY;
    filesystem::path fixtureRoot;
    XMFLOAT3 redSignal{}, greenSignal{};
    XMFLOAT3 blueSignal{};
    uint64_t lastFrame = 0;
    bool optixValidation = false;
    bool lumenValidation = false;
    array<double, 81> noiseSum{}, noiseSquareSum{};
    UINT noiseFrames = 0;

    void Check(bool passed, const char* message)
    {
        if (!passed)
        {
            ++failures;
        }
        WriteGraphicsLog("GI validation: %s %s\n", passed ? "PASS" : "FAIL", message);
    }

    EntityID Cube(XMFLOAT3 position, XMFLOAT3 scale, AssetHandle material)
    {
        auto entity = World::CreateEntity()
                          .Add<TransformComponent>()
                          .Add<MeshComponent>()
                          .Add<MaterialComponent>()
                          .Add<NameComponent>();
        entity.SetName("GI validation fixture");
        entity.Get<TransformComponent>().Position = position;
        entity.Get<TransformComponent>().Scale = scale;
        entity.Get<TransformComponent>().IsDirty = true;
        VertexResource mesh;
        mesh.entityid = entity.GetID();
        mesh.objectType = ObjectType::CUBE;
        mesh.color = Color::WHITE;
        PrimitiveGeometry::CreateObjectVertex(mesh);
        Check(MaterialManager::Assign(entity.GetID(), 0, material), "assign fixture material");
        return entity.GetID();
    }

    AssetHandle Material(const char* name, MaterialAssetData data)
    {
        string error;
        const auto path = fixtureRoot / name;
        Check(MaterialSerializer::Save(path, data, error), "save fixture material");
        return MaterialManager::Registry().Register(path, AssetType::Material, error);
    }

    XMFLOAT3 SampleReceiver(bool measureNoise = false)
    {
        RenderCapture::TextureData image;
        if (!RenderCapture::ReadTexture(RayReflections::GetGiResource(), image))
        {
            Check(false, "read GI irradiance");
            return {};
        }
        XMMATRIX view, projection;
        Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
        const XMFLOAT3 receiver{0, -0.89f, 0};
        XMFLOAT3 pixel;
        XMStoreFloat3(&pixel,
                      XMVector3Project(XMLoadFloat3(&receiver), 0, 0, static_cast<float>(image.Width),
                                       static_cast<float>(image.Height), 0, 1, projection, view, XMMatrixIdentity()));
        XMFLOAT3 sum{};
        UINT samples = 0;
        for (int y = -4; y <= 4; ++y)
        {
            for (int x = -4; x <= 4; ++x)
            {
                const int px = static_cast<int>(pixel.x) + x;
                const int py = static_cast<int>(pixel.y) + y;
                if (px < 0 || py < 0 || px >= static_cast<int>(image.Width) || py >= static_cast<int>(image.Height))
                {
                    continue;
                }
                const auto* rgb = reinterpret_cast<const uint16_t*>(image.Bytes.data() + py * image.RowPitch + px * 8);
                sum.x += RenderCapture::HalfToFloat(rgb[0]);
                sum.y += RenderCapture::HalfToFloat(rgb[1]);
                sum.z += RenderCapture::HalfToFloat(rgb[2]);
                if (measureNoise)
                {
                    const double red = RenderCapture::HalfToFloat(rgb[0]);
                    noiseSum[samples] += red;
                    noiseSquareSum[samples] += red * red;
                }
                ++samples;
            }
        }
        const float divisor = static_cast<float>(max(samples, 1u));
        if (measureNoise)
        {
            ++noiseFrames;
        }
        return {sum.x / divisor, sum.y / divisor, sum.z / divisor};
    }
}

bool GiValidation::Enabled()
{
    char value[8]{};
    return GetEnvironmentVariableA("DX12_GI_TEST", value, sizeof(value)) > 0 && atoi(value) != 0;
}

void GiValidation::BeforeFrame(uint64_t frame)
{
    if (CelestialOcclusionValidation::Enabled())
    {
        CelestialOcclusionValidation::BeforeFrame(frame);
        return;
    }
    if (TemporalLightingValidation::Enabled())
    {
        TemporalLightingValidation::BeforeFrame(frame);
        return;
    }
    lastFrame = frame;
    if (frame == 0)
    {
        vector<EntityID> remove;
        for (uint32_t entity = 0; entity < Registry::GetEntities().size(); ++entity)
        {
            if (Registry::IsAlive(entity) && !ComponentManager::HasComponent<CameraComponent>(entity))
            {
                remove.push_back(entity);
            }
        }
        for (EntityID entity : remove)
        {
            World::DestroyEntity(Entity(entity));
        }
        fixtureRoot = AssetRegistry::ValidationCacheRoot() / ("__gi_validation_" + to_string(GetCurrentProcessId()));
        filesystem::create_directories(fixtureRoot);
        filesystem::create_directories("build/gi-tests");
        MaterialAssetData white;
        white.BaseColor = {0.8f, 0.8f, 0.8f, 1};
        white.Roughness = 1;
        const auto receiverMaterial = Material("Receiver.material", white);
        MaterialAssetData emission = white;
        emission.BaseColor = {0, 0, 0, 1};
        emission.EmissiveColor = {1, 0, 0};
        emission.EmissiveIntensity = 80;
        emitterMaterial = Material("Emitter.material", emission);
        const auto texturePath = fixtureRoot / "Blue.tga";
        array<uint8_t, 18> textureHeader{};
        textureHeader[2] = 2;
        textureHeader[12] = textureHeader[14] = 1;
        textureHeader[16] = 24;
        const array<uint8_t, 3> bluePixel{255, 0, 0};
        {
            ofstream texture(texturePath, ios::binary);
            texture.write(reinterpret_cast<const char*>(textureHeader.data()), textureHeader.size());
            texture.write(reinterpret_cast<const char*>(bluePixel.data()), bluePixel.size());
        }
        string textureError;
        blueTexture = MaterialManager::Registry().Register(texturePath, AssetType::Texture, textureError);
        Check(blueTexture != 0, "register emission texture");
        Cube({0, -1, 0}, {8, 0.1f, 8}, receiverMaterial);
        // カメラの視錐台の上方外側だが、床からは見える位置。
        emitter = Cube({0, 4, 0}, {1.5f, 0.1f, 1.5f}, emitterMaterial);
        blocker = Cube({0, 20, 0}, {4, 0.1f, 4}, receiverMaterial);
        auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(Camera::GetCameraEntity());
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(Camera::GetCameraEntity());
        transform.Position = {0, 1, -7};
        transform.IsDirty = true;
        camera.Target = {0, -1, 0};
        camera.AllowUserControl = false;
        camera.LockOnTarget = g_kINVALID_ENTITY;
        lumenValidation = ReadEnvironmentFlagOnce("DX12_LUMEN_GI_TEST");
        RendererSettings::SetGiMode(lumenValidation ? GlobalIlluminationMode::Lumen : GlobalIlluminationMode::Realtime);
        if (lumenValidation)
        {
            char setting[16]{};
            if (GetEnvironmentVariableA("DX12_LUMEN_PROBE_SPACING", setting, sizeof(setting)))
            {
                RendererSettings::SetLumenProbeSpacing(atoi(setting));
            }
            if (GetEnvironmentVariableA("DX12_LUMEN_LIGHTING_BUDGET", setting, sizeof(setting)))
            {
                RendererSettings::SetLumenLightingBudget(atoi(setting));
            }
        }
        RendererSettings::SetSsgiIntensity(1);
        RendererSettings::SetRealtimeGiIntensity(1);
        RendererSettings::SetSsaoEnabled(false);
        RendererSettings::SetMeshShadersEnabled(false);
        GPUDrivenRenderer::SetEnabled(false);
        auto& ray = Raytracing::GetSettings();
        ray.Enabled = true;
        ray.Reflections = false;
        ray.AmbientOcclusion = false;
        ray.Shadows = false;
        char mode[8]{};
        GetEnvironmentVariableA("DX12_GI_TEST", mode, sizeof(mode));
        ray.Denoise = atoi(mode) != 2;
        ray.RadianceDenoiser = Raytracing::DenoiserType::Nrd;
        if (atoi(mode) == 3)
        {
            optixValidation = true;
            ray.RadianceDenoiser = Raytracing::DenoiserType::Optix;
            Check(OptixDenoiser::IsAvailable(), "OptiX driver and CUDA interop available");
        }
    }
    if (frame == 24)
    {
        auto material = MaterialManager::GetRuntime(emitterMaterial).Editing;
        material.EmissiveColor = {0, 1, 0};
        MaterialManager::SetEditing(emitterMaterial, material);
    }
    if (frame == 48)
    {
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(blocker);
        transform.Position = {0, 1.5f, 0};
        transform.IsDirty = true;
    }
    if (frame == 72)
    {
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(blocker);
        transform.Position = {0, 20, 0};
        transform.IsDirty = true;
        auto material = MaterialManager::GetRuntime(emitterMaterial).Editing;
        material.EmissiveIntensity = 0;
        MaterialManager::SetEditing(emitterMaterial, material);
    }
    if (frame == 96)
    {
        RendererSettings::SetGiMode(GlobalIlluminationMode::ScreenSpace);
    }
    if (frame == 100)
    {
        RendererSettings::SetGiMode(lumenValidation ? GlobalIlluminationMode::Lumen : GlobalIlluminationMode::Realtime);
        Raytracing::GetSettings().Reflections = true;
    }
    if (frame == 112)
    {
        auto material = MaterialManager::GetRuntime(emitterMaterial).Editing;
        material.EmissiveColor = {1, 1, 1};
        material.EmissiveIntensity = 80;
        material.Textures[4] = blueTexture;
        MaterialManager::SetEditing(emitterMaterial, material);
    }
    if (frame == 144)
    {
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(emitter);
        transform.Position = {0, 200, 0};
        transform.IsDirty = true;
    }
    if (frame == 170)
    {
        auto& source = ComponentManager::GetComponentUnchecked<TransformComponent>(emitter);
        source.Position = {0, 4, 0};
        source.IsDirty = true;
        auto& camera = ComponentManager::GetComponentUnchecked<TransformComponent>(Camera::GetCameraEntity());
        camera.Position = {0, 1, -22};
        camera.IsDirty = true;
    }
    if (lumenValidation && frame >= 195)
    {
        auto& moving = ComponentManager::GetComponentUnchecked<TransformComponent>(blocker);
        moving.Position.x = float(frame - 195) * 0.03f;
        moving.IsDirty = true;
    }
    if (frame == 202 && optixValidation)
    {
        Raytracing::GetSettings().RadianceDenoiser = Raytracing::DenoiserType::Nrd;
    }
    if (frame == 210 && optixValidation)
    {
        Raytracing::GetSettings().RadianceDenoiser = Raytracing::DenoiserType::Optix;
    }
}

void GiValidation::AfterFrame(uint64_t frame)
{
    if (CelestialOcclusionValidation::Enabled())
    {
        CelestialOcclusionValidation::AfterFrame(frame);
        return;
    }
    if (TemporalLightingValidation::Enabled())
    {
        TemporalLightingValidation::AfterFrame(frame);
        return;
    }
    if (frame >= 12 && frame <= 23)
    {
        SampleReceiver(true);
    }
    if (frame == 23)
    {
        if (lumenValidation)
        {
            Check(RayReflections::GetLumenResidentMeshes() == 3, "Lumen captures the three fixture meshes");
            Check(RayReflections::GetLumenMemoryBytes() > 0, "Lumen allocates the surface cache and screen probes");
            WriteGraphicsLog("Lumen cache: resident=%u bytes=%llu\n", RayReflections::GetLumenResidentMeshes(),
                             static_cast<unsigned long long>(RayReflections::GetLumenMemoryBytes()));
            RenderCapture::TextureData atlas;
            const bool read = RenderCapture::ReadTextureMip(RayReflections::GetLumenLightingResource(), atlas, 0,
                                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Check(read, "read Lumen surface lighting atlas");
            UINT illuminated = 0;
            UINT populated = 0;
            bool finite = true;
            if (read)
            {
                for (UINT y = 0; y < 3 * 16; ++y)
                {
                    for (UINT x = 0; x < atlas.Width; ++x)
                    {
                        const auto* value =
                            reinterpret_cast<const uint16_t*>(atlas.Bytes.data() + y * atlas.RowPitch + x * 8);
                        const float r = RenderCapture::HalfToFloat(value[0]);
                        const float g = RenderCapture::HalfToFloat(value[1]);
                        const float b = RenderCapture::HalfToFloat(value[2]);
                        const float samples = RenderCapture::HalfToFloat(value[3]);
                        finite = finite && isfinite(r) && isfinite(g) && isfinite(b) && isfinite(samples);
                        if (samples > 0 && r + g + b > 0)
                        {
                            ++illuminated;
                        }
                        if (samples > 0)
                        {
                            ++populated;
                        }
                    }
                }
            }
            const bool singleBounce = ReadEnvironmentFlagOnce("DX12_LUMEN_SINGLE_BOUNCE");
            Check(finite && (singleBounce ? populated : illuminated) > 256,
                  "Lumen updates finite surface texels on GPU");
        }
        double variance = 0;
        double mean = 0;
        for (size_t index = 0; index < noiseSum.size(); ++index)
        {
            const double pixelMean = noiseSum[index] / max(noiseFrames, 1u);
            variance += max(0.0, noiseSquareSum[index] / max(noiseFrames, 1u) - pixelMean * pixelMean);
            mean += pixelMean;
        }
        mean /= noiseSum.size();
        const double relativeNoise = sqrt(variance / noiseSum.size()) / max(mean, 0.00001);
        WriteGraphicsLog("GI temporal noise: relative_rms=%.6f frames=%u pixels=%zu\n", relativeNoise, noiseFrames,
                         noiseSum.size());
    }
    if (frame == 23 || frame == 47 || frame == 71 || frame == 95)
    {
        Check(Raytracing::IsGiActive(), "GI runs with reflections and RT shadows disabled");
        const auto signal = SampleReceiver();
        WriteGraphicsLog("GI receiver frame=%llu rgb=(%.6f,%.6f,%.6f)\n", frame, signal.x, signal.y, signal.z);
        for (const auto& pass : RenderProfiler::GetLatest().Passes)
        {
            if (pass.Name.rfind("Realtime GI", 0) == 0 || pass.Name.rfind("Lumen ", 0) == 0)
            {
                WriteGraphicsLog("GI timing frame=%llu pass=%s gpu=%.3fms cpu=%.3fms\n", frame, pass.Name.c_str(),
                                 pass.GpuMs, pass.CpuMs);
            }
        }
        if (frame == 23)
        {
            if (RayTelemetry::Requested())
            {
                const bool idle = GraphicsDevice::WaitForGpuIdle();
                Check(idle, "GI ray telemetry fence completes");
                const auto counts = idle ? RayTelemetry::Data().ConsumeCurrent(RenderProfiler::GetFrameSerial())
                                         : RayTelemetry::Counts{};
                Check(counts.ProviderKnown(RayTelemetry::Radiance) && counts.Rays[RAY_TELEMETRY_GI] > 0,
                      "GI primary rays are measured in the actual diffuse shader");
                WriteGraphicsLog("GI ray telemetry serial=%llu primary=%llu visibility=%llu restir=%llu\n",
                                 counts.Serial, counts.Rays[RAY_TELEMETRY_GI],
                                 counts.Rays[RAY_TELEMETRY_RADIANCE_SHADOW],
                                 counts.Rays[RAY_TELEMETRY_RESTIR_VISIBILITY]);
            }
            if (Raytracing::GetSettings().RadianceDenoiser == Raytracing::DenoiserType::Optix)
            {
                Check(RayReflections::IsOptixGiActive(), "OptiX AI denoises GI on GPU");
            }
            XMMATRIX view, projection;
            Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
            const auto sourcePosition = ComponentManager::GetComponentUnchecked<TransformComponent>(emitter).Position;
            XMFLOAT4 clip;
            XMStoreFloat4(&clip,
                          XMVector4Transform(XMVectorSet(sourcePosition.x, sourcePosition.y, sourcePosition.z, 1),
                                             view * projection));
            Check(clip.y > clip.w, "emitter is outside camera frustum");
            redSignal = signal;
            Check(signal.x > signal.y + 0.05f, "offscreen red emission illuminates receiver");
            RenderCapture::SaveScenePng(L"build/gi-tests/red.png");
        }
        if (frame == 47)
        {
            greenSignal = signal;
            Check(signal.y > redSignal.y + 0.1f && signal.x < redSignal.x * 0.3f,
                  "emission color edits update GI without stale red history");
            RenderCapture::SaveScenePng(L"build/gi-tests/green.png");
        }
        if (frame == 71)
        {
            Check(signal.y < greenSignal.y * 0.6f, "moving occluder blocks emissive lighting");
            RenderCapture::SaveScenePng(L"build/gi-tests/occluded.png");
        }
        if (frame == 95)
        {
            Check(signal.y < greenSignal.y * 0.6f, "zero emission removes irradiance");
        }
    }
    if (frame == 98)
    {
        Check(!Raytracing::IsGiActive() && RendererSettings::GetSsgiEnabled(), "switch back to existing SSGI");
    }
    if (frame == 105)
    {
        Check(Raytracing::IsGiActive() && Raytracing::AreReflectionsActive(),
              "GI and reflections coexist after mode switch");
        if (Raytracing::GetSettings().RadianceDenoiser == Raytracing::DenoiserType::Optix)
        {
            Check(RayReflections::IsOptixGiActive() && RayReflections::IsOptixReflectionActive(),
                  "OptiX AI denoises GI and reflections in one frame");
        }
    }
    if (frame == 135)
    {
        const auto signal = SampleReceiver();
        blueSignal = signal;
        Check(signal.z > signal.y + 0.05f && signal.z > signal.x + 0.05f, "emission texture colors GI");
        RenderCapture::SaveScenePng(L"build/gi-tests/textured.png");
    }
    if (frame == 167)
    {
        const auto signal = SampleReceiver();
        Check(signal.z < blueSignal.z * 0.6f, "emitter movement updates GI and respects trace distance");
    }
    if (frame == 194)
    {
        const auto signal = SampleReceiver();
        Check(isfinite(signal.z) && signal.z > blueSignal.z * 0.4f, "distant camera preserves visible GI irradiance");
        RenderCapture::SaveScenePng(L"build/gi-tests/distant.png");
    }
    if (frame == 207 && optixValidation)
    {
        Check(Raytracing::IsGiActive() && !RayReflections::IsOptixGiActive(), "switch OptiX to NRD during rendering");
    }
    if (frame == 215 && lumenValidation)
    {
        RenderCapture::TextureData atlas;
        bool accumulated = false;
        if (RenderCapture::ReadTextureMip(RayReflections::GetLumenLightingResource(), atlas, 0,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            for (UINT y = 0; y < 3 * 16 && !accumulated; ++y)
            {
                for (UINT x = 0; x < atlas.Width; ++x)
                {
                    const auto* value =
                        reinterpret_cast<const uint16_t*>(atlas.Bytes.data() + y * atlas.RowPitch + x * 8);
                    accumulated = accumulated || RenderCapture::HalfToFloat(value[3]) >= 2;
                }
            }
        }
        Check(accumulated, "moving geometry preserves bounded surface lighting accumulation");
    }
    if (frame == 215 && optixValidation)
    {
        Check(Raytracing::IsGiActive() && RayReflections::IsOptixGiActive(),
              "switch NRD back to OptiX during rendering");
    }
}

bool GiValidation::Finish()
{
    if (CelestialOcclusionValidation::Enabled())
    {
        return CelestialOcclusionValidation::Finish();
    }
    if (TemporalLightingValidation::Enabled())
    {
        return TemporalLightingValidation::Finish();
    }
    Check(lastFrame >= 215, "complete dynamic GI sequence");
    failures += RenderCapture::ReportGraphicsErrors();
    // このテストが作成したファイルだけを削除し、想定外の内容は保持する。
    for (const char* name : {"Receiver.material", "Receiver.material.meta", "Emitter.material", "Emitter.material.meta",
                             "Blue.tga", "Blue.tga.meta"})
    {
        error_code error;
        filesystem::remove(fixtureRoot / name, error);
    }
    error_code error;
    if (!fixtureRoot.empty())
    {
        filesystem::remove_all(fixtureRoot, error);
    }
    WriteGraphicsLog("GI validation failures = %u\n", failures);
    return failures == 0;
}

