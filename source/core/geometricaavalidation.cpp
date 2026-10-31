#include "pch.h"
#include "geometricaavalidation.h"
#include "world.h"
#include "camera.h"
#include "sun.h"
#include "primitivegeometry.h"
#include "renderconfiguration.h"
#include "renderersettings.h"
#include "rendererstate.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "rendercapture.h"
#include "renderprofiler.h"
#include "rendertargets.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "raytracing.h"
#include "daynightcycle.h"
#include "atmosphere.h"
#include "cloud/cloudrenderer.h"
#include "advancedeffects.h"
#include "renderquality.h"
#include <filesystem>
#include <fstream>
#include <cmath>

namespace
{
    UINT failures = 0;
    uint64_t completedFrames = 0;
    uint64_t lastTimingSerial = 0;
    UINT timingSamples = 0, captureSamples = 0;
    bool expectedEnabled = false;
    std::filesystem::path outputDirectory;
    std::ofstream timings;

    void Check(bool passed, const char* message)
    {
        WriteGraphicsLog("Geometric AA: %s %s\n", passed ? "PASS" : "FAIL", message);
        if (!passed) ++failures;
    }

    void MakeObject(const char* name, const XMFLOAT3& position, ObjectType shape, ShaderClass shader)
    {
        Entity object = World::CreateEntity().Add<NameComponent>().Add<TransformComponent>()
                            .Add<MeshComponent>().Add<MaterialComponent>();
        object.SetName(name);
        auto& transform = object.Get<TransformComponent>();
        transform.Position = position;
        transform.IsDirty = true;
        auto& material = object.Get<MaterialComponent>();
        material.ShaderClassMode = MaterialMode::Manual;
        material.ShaderClass = shader;
        material.Metallic = 1.0f;
        material.Roughness = .04f;
        VertexResource vertices;
        vertices.entityid = object.GetID();
        vertices.objectType = shape;
        vertices.color = Color::WHITE;
        PrimitiveSettings geometry;
        geometry.Segments = 16;
        geometry.Rings = 8;
        PrimitiveGeometry::CreateObjectVertex(vertices, geometry);
    }

    void CreateScene()
    {
        char directory[2048]{};
        const DWORD length = GetEnvironmentVariableA("DX12_GEOMETRIC_AA_OUTPUT", directory, sizeof(directory));
        if (!length || length >= sizeof(directory)) throw std::runtime_error("AA output directory is required");
        outputDirectory = directory;
        std::filesystem::create_directories(outputDirectory);
        char enabled[8]{};
        const DWORD enabledLength = GetEnvironmentVariableA("DX12_GEOMETRIC_AA_EXPECTED", enabled, sizeof(enabled));
        if (enabledLength != 1 || (enabled[0] != '0' && enabled[0] != '1'))
            throw std::runtime_error("AA expected mode must be 0 or 1");
        expectedEnabled = enabled[0] == '1';
        timings.open(outputDirectory / "timings.csv");
        timings << "Serial,CpuRenderMs,GpuRenderMs,GBufferGpuMs\n";
        Check(bool(timings), "open timing output");

        vector<EntityID> removed;
        for (EntityID id : World::GetView<TransformComponent>())
        {
            if (!ComponentManager::HasComponent<CameraComponent>(id)) removed.push_back(id);
        }
        for (EntityID id : removed) World::DestroyEntity(Entity(id));
        RenderConfiguration::Resize(960, 640);
        RenderConfiguration::SetResolutionScale(1.0f);
        RenderConfiguration::SetRenderMode(RenderMode::DEFERRED);
        RenderConfiguration::SetHdr(true);
        RendererState::m_AntiAliasingMode = AntiAliasingMode::NONE;
        RendererSettings::SetUpscaleMode(UpscaleMode::Bilateral);
        RendererSettings::SetMeshShadersEnabled(false);
        RendererSettings::SetComputeGBufferEnabled(false);
        RendererSettings::SetTwoPhaseOcclusionEnabled(false);
        RendererSettings::SetSsaoEnabled(false);
        RendererSettings::SetSsgiEnabled(false);
        GPUDrivenRenderer::SetEnabled(false);
        Raytracing::GetSettings().Enabled = false;
        Atmosphere::GetMutableParameters().Enabled = false;
        Atmosphere::GetMutableParameters().PhysicalSky = false;
        CloudRenderer::GetSettings().Enabled = false;
        AdvancedEffects::GetSettings() = {};
        RenderQuality::Get() = {};
        RenderQuality::Get().BloomIntensity = 0;
        RenderQuality::Get().LensFlare = false;
        RenderQuality::Get().FilmGrain = 0;
        DayNightCycle::GetSettings().Running = false;
        DayNightCycle::GetSettings().TimeDriven = false;
        DayNightCycle::GetSettings().AutomaticLighting = false;
        const EntityID sun = Sun::Create({-2, 4, -8}, {0, 0, 0});
        auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(sun);
        light.CastShadow = false;
        light.AffectsVolumetrics = false;
        MakeObject("AA PBR sphere", {-.75f, -.6f, 0}, ObjectType::SPHERE, ShaderClass::PBR);
        MakeObject("AA PBR flat cube", {.75f, -.6f, 0}, ObjectType::CUBE, ShaderClass::PBR);
        MakeObject("AA Toon sphere", {-.75f, .6f, 0}, ObjectType::SPHERE, ShaderClass::Toon);
        MakeObject("AA Unlit sphere", {.75f, .6f, 0}, ObjectType::SPHERE, ShaderClass::Unlit);
        const EntityID cameraId = Camera::GetCameraEntity();
        auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(cameraId);
        camera.Target = {0, 0, 0};
        camera.AllowUserControl = false;
        camera.LockOnTarget = g_kINVALID_ENTITY;
        WriteGraphicsLog("Geometric AA fixture: expected=%u output=%s extent=960x640 authoredRoughness=0.04\n",
                         static_cast<unsigned>(expectedEnabled), outputDirectory.string().c_str());
    }

    bool WriteTexture(const char* kind, uint64_t frame, ID3D12Resource* resource)
    {
        RenderCapture::TextureData texture;
        if (!RenderCapture::ReadTexture(resource, texture)) return false;
        const auto path = outputDirectory / (string(kind) + "-" + std::to_string(frame) + ".bin");
        std::ofstream file(path, std::ios::binary);
        const uint32_t header[] = {texture.Width, texture.Height, texture.RowPitch, UINT(texture.Format)};
        file.write(reinterpret_cast<const char*>(header), sizeof(header));
        file.write(reinterpret_cast<const char*>(texture.Bytes.data()),
                   static_cast<std::streamsize>(texture.Bytes.size()));
        return bool(file);
    }

    void CheckRoughness()
    {
        RenderCapture::TextureData texture;
        RenderCapture::TextureData depth;
        if (!RenderCapture::ReadTexture(RenderTargets::GetGBufferResource(GBufferType::MATERIAL), texture) ||
            !RenderCapture::ReadTexture(RenderTargets::GetGBufferResource(GBufferType::DEPTH), depth) ||
            texture.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || depth.Format != DXGI_FORMAT_R32_FLOAT ||
            depth.Width != texture.Width || depth.Height != texture.Height)
        {
            Check(false, "read float16 material GBuffer");
            return;
        }
        double curved = 0, flat = 0;
        UINT curvedCount = 0, flatCount = 0, toonCount = 0, unlitCount = 0;
        bool finite = true;
        for (UINT y = 0; y < texture.Height; ++y)
        {
            const auto* row = reinterpret_cast<const uint16_t*>(texture.Bytes.data() + y * texture.RowPitch);
            const auto* depthRow = reinterpret_cast<const float*>(depth.Bytes.data() + y * depth.RowPitch);
            for (UINT x = 0; x < texture.Width; ++x)
            {
                if (!(depthRow[x] > 0)) continue;
                const float material = RenderCapture::HalfToFloat(row[x * 4 + 3]);
                const float roughness = RenderCapture::HalfToFloat(row[x * 4 + 1]);
                if (std::abs(material - 11) < .1f)
                {
                    finite = finite && std::isfinite(roughness);
                    if (x < texture.Width / 2) { curved += roughness; ++curvedCount; }
                    else { flat += roughness; ++flatCount; }
                }
                if (std::abs(material - static_cast<float>(ShaderClass::Toon)) < .1f) ++toonCount;
                if (std::abs(material - static_cast<float>(ShaderClass::Unlit)) < .1f) ++unlitCount;
            }
        }
        Check(finite, "finite PBR roughness");
        WriteGraphicsLog("Geometric AA control pixels: curved=%u flat=%u Toon=%u Unlit=%u\n",
                         curvedCount, flatCount, toonCount, unlitCount);
        Check(curvedCount > 1000 && flatCount > 1000 && toonCount > 1000 && unlitCount > 1000,
              "fixture includes visible curved/flat PBR and Toon/Unlit controls");
        if (curvedCount && flatCount)
        {
            curved /= curvedCount;
            flat /= flatCount;
            WriteGraphicsLog("Geometric AA roughness: curved=%.8f flat=%.8f pixels=%u/%u\n",
                             curved, flat, curvedCount, flatCount);
            Check(std::abs(flat - .04) < .0003, "flat PBR retains authored roughness");
            Check(expectedEnabled ? curved > .041 : std::abs(curved - .04) < .0003,
                  "curved PBR roughness confirms the requested shader mode");
        }
    }
}

bool GeometricAaValidation::Enabled()
{
    char value[8]{};
    return GetEnvironmentVariableA("DX12_GEOMETRIC_AA_VALIDATION", value, sizeof(value)) && value[0] == '1';
}

void GeometricAaValidation::BeforeFrame(uint64_t frame)
{
    if (frame == 0) CreateScene();
    const EntityID camera = Camera::GetCameraEntity();
    auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(camera);
    // Measurement frames are static. Captures later sample a small, deterministic camera translation.
    const float phase = frame >= 100 ? float(frame - 100) / 15.0f - .5f : 0;
    transform.Position = {phase * .01f, 0, -4};
    transform.IsDirty = true;
}

void GeometricAaValidation::AfterFrame(uint64_t frame)
{
    completedFrames = frame + 1;
    if (frame >= 32 && frame < 96)
    {
        const auto& sample = RenderProfiler::GetLatest();
        if (sample.Serial != lastTimingSerial && sample.GpuTimingValid && sample.DroppedGpuEvents == 0)
        {
            lastTimingSerial = sample.Serial;
            for (const auto& pass : sample.Passes)
            {
                if (pass.Name == "GBuffer / Opaque" && pass.GpuSamples)
                {
                    timings << sample.Serial << ',' << sample.CpuRenderMs << ',' << sample.GpuRenderMs
                            << ',' << pass.GpuMs << '\n';
                    ++timingSamples;
                    break;
                }
            }
        }
    }
    if (frame == 96) CheckRoughness();
    if (frame == 96 || (frame >= 100 && frame < 116))
    {
        const auto path = outputDirectory / ("scene-" + std::to_string(frame) + ".png");
        const bool saved = RenderCapture::SaveScenePng(path.c_str()) &&
            WriteTexture("scene", frame, RenderTargets::GetSceneResource()) &&
            WriteTexture("material", frame, RenderTargets::GetGBufferResource(GBufferType::MATERIAL)) &&
            WriteTexture("depth", frame, RenderTargets::GetGBufferResource(GBufferType::DEPTH));
        Check(saved, "save scene PNG, linear HDR and material mask");
        if (saved) ++captureSamples;
    }
}

bool GeometricAaValidation::Finish()
{
    GraphicsDevice::WaitForGpuIdle();
    timings.flush();
    Check(bool(timings), "write timing output");
    Check(completedFrames >= 116 && captureSamples == 17, "complete static and camera-phase captures");
    Check(timingSamples >= 32, "collect at least 32 distinct valid GPU timing samples");
    failures += RenderCapture::ReportGraphicsErrors();
    WriteGraphicsLog("Geometric AA validation failures = %u; timingSamples=%u captures=%u\n",
                     failures, timingSamples, captureSamples);
    return failures == 0;
}
