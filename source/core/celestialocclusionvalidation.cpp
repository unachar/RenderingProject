#include "pch.h"
#include "celestialocclusionvalidation.h"
#include "world.h"
#include "camera.h"
#include "sun.h"
#include "daynightcycle.h"
#include "atmosphere.h"
#include "primitivegeometry.h"
#include "renderconfiguration.h"
#include "renderersettings.h"
#include "performancegovernor.h"
#include "renderquality.h"
#include "advancedeffects.h"
#include "raytracing.h"
#include "rayreflections.h"
#include "cpuculling.h"
#include "rendertargets.h"
#include "rendercapture.h"
#include "graphicslog.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "cloud/cloudrenderer.h"
#include <fstream>

namespace
{
constexpr UINT FramesPerCase = 24, Cases = 12;
UINT failures = 0;
uint64_t lastFrame = 0;
EntityID roof = g_kINVALID_ENTITY, sun = g_kINVALID_ENTITY;
array<double, Cases> brightness{};
array<UINT, Cases> samples{};
array<double, Cases> reflectionBrightness{}, giBrightness{};

void Check(bool valid, const char* message)
{
    if (!valid)
    {
        ++failures;
    }
    WriteGraphicsLog("Celestial occlusion: %s %s\n", valid ? "PASS" : "FAIL", message);
}

EntityID Cube(XMFLOAT3 position, XMFLOAT3 scale)
{
    auto entity = World::CreateEntity()
                      .Add<TransformComponent>()
                      .Add<MeshComponent>()
                      .Add<MaterialComponent>()
                      .Add<NameComponent>();
    entity.SetName("Closed PBR room fixture");
    auto& transform = entity.Get<TransformComponent>();
    transform.Position = position;
    transform.Scale = scale;
    transform.IsDirty = true;
    auto& material = entity.Get<MaterialComponent>();
    material.UseTexture = false;
    material.Roughness = .65f;
    material.ShaderClassMode = MaterialMode::Manual;
    material.ShaderClass = ShaderClass::PBR;
    VertexResource geometry{};
    geometry.entityid = entity.GetID();
    geometry.objectType = ObjectType::CUBE;
    geometry.color = Color::WHITE;
    PrimitiveGeometry::CreateObjectVertex(geometry);
    return entity.GetID();
}

float Luminance(const RenderCapture::TextureData& image, UINT x, UINT y)
{
    const auto* row = image.Bytes.data() + y * image.RowPitch;
    if (image.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        const auto* pixel = reinterpret_cast<const uint16_t*>(row) + 4 * x;
        return .2126f * RenderCapture::HalfToFloat(pixel[0]) + .7152f * RenderCapture::HalfToFloat(pixel[1]) +
               .0722f * RenderCapture::HalfToFloat(pixel[2]);
    }
    if (image.Format == DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        const auto* pixel = row + 4 * x;
        return (.2126f * pixel[0] + .7152f * pixel[1] + .0722f * pixel[2]) / 255;
    }
    Check(false, "supported HDR scene format");
    return 0;
}
} // namespace

bool CelestialOcclusionValidation::Enabled()
{
    char mode[8]{};
    return GetEnvironmentVariableA("DX12_GI_TEST", mode, sizeof(mode)) && atoi(mode) == 5;
}

void CelestialOcclusionValidation::BeforeFrame(uint64_t frame)
{
    lastFrame = frame;
    if (frame >= FramesPerCase * Cases)
    {
        return;
    }
    if (frame == 0)
    {
        vector<EntityID> remove;
        for (EntityID entity : World::GetView<TransformComponent>())
        {
            if (!ComponentManager::HasComponent<CameraComponent>(entity))
            {
                remove.push_back(entity);
            }
        }
        for (EntityID entity : remove)
        {
            World::DestroyEntity(Entity(entity));
        }
        Cube({0, -1, 0}, {8, .1f, 8});
        roof = Cube({0, 3, 0}, {8.4f, .2f, 8.4f});
        Cube({-4, 1, 0}, {.2f, 4, 8.4f});
        Cube({4, 1, 0}, {.2f, 4, 8.4f});
        Cube({0, 1, -4}, {8.4f, 4, .2f});
        Cube({0, 1, 4}, {8.4f, 4, .2f});
        sun = Sun::CreateDefault();
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(Camera::GetCameraEntity());
        transform.Position = {0, 1, -2.5f};
        transform.IsDirty = true;
        auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(Camera::GetCameraEntity());
        camera.Target = {0, -.95f, .5f};
        camera.AllowUserControl = false;
        camera.LockOnTarget = g_kINVALID_ENTITY;
        RenderConfiguration::Resize(640, 360);
        filesystem::create_directories("build/celestial-occlusion");
    }
    PerformanceGovernor::GetOptions().Enabled = false;
    World::SetVSyncEnabled(false);
    RenderConfiguration::SetResolutionScale(.5f);
    RendererSettings::SetUpscaleMode(UpscaleMode::Bilateral);
    RendererState::m_AntiAliasingMode = AntiAliasingMode::NONE;
    RendererSettings::SetGiMode(GlobalIlluminationMode::Realtime);
    RendererSettings::SetRealtimeGiIntensity(0);
    RendererSettings::SetGiRadianceCacheEnabled(false);
    RendererSettings::SetSsaoEnabled(false);
    RendererSettings::SetMeshShadersEnabled(false);
    GPUDrivenRenderer::SetEnabled(false);
    RenderQuality::Get() = {};
    RenderQuality::Get().ReSTIRGI = false;
    AdvancedEffects::GetSettings() = {};
    CloudRenderer::GetSettings().Enabled = false;
    DayNightCycle::GetSettings().TimeDriven = false;
    DayNightCycle::GetSettings().Running = false;
    DayNightCycle::GetSettings().AutomaticLighting = false;
    auto& rays = Raytracing::GetSettings();
    rays.Enabled = rays.Shadows = true;
    rays.Reflections = rays.AmbientOcclusion = false;
    rays.RenderMode = Raytracing::Mode::Full;
    const UINT phase = UINT(frame / FramesPerCase);
    if (phase >= 6)
    {
        // Exercise the actual secondary lighting paths, with the authored Sun disabled.
        rays.Reflections = true;
        Atmosphere::GetMutableParameters().Enabled = true;
        Atmosphere::GetMutableParameters().PhysicalSky = true;
        RendererSettings::SetRealtimeGiIntensity(1);
        RenderCulling::GetSettings().ReflectionRoughnessCutoff = phase < 8 ? 1.f : .5f;
        DayNightCycle::SetTime(phase == 10 ? 8.f : phase == 11 ? 16.f : 12.f);
    }
    if (frame % FramesPerCase == 0)
    {
        auto& sunTransform = ComponentManager::GetComponentUnchecked<TransformComponent>(sun);
        sunTransform.Position = {0, phase == 0 || phase == 3 || phase == 5 ? 20.f : -20.f, 0};
        sunTransform.IsDirty = true;
        auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(sun);
        light.Intensity = 5;
        light.Color = {1, 1, 1, 1};
        light.IsActive = phase < 5;
        light.CastShadow = true;
        auto& roofTransform = ComponentManager::GetComponentUnchecked<TransformComponent>(roof);
        roofTransform.Position = {phase == 2 || phase == 3 || phase == 7 || phase == 9 ? 30.f : 0.f, 3, 0};
        roofTransform.IsDirty = true;
        if (phase >= 6)
        {
            DayNightCycle::Update(0);
        }
        Camera::NotifyCut();
    }
}

void CelestialOcclusionValidation::AfterFrame(uint64_t frame)
{
    if (frame >= FramesPerCase * Cases || frame % FramesPerCase < 20)
    {
        return;
    }
    const UINT phase = UINT(frame / FramesPerCase);
    RenderCapture::TextureData image;
    if (!RenderCapture::ReadTexture(RenderTargets::GetSceneResource(), image))
    {
        Check(false, "capture HDR lighting");
        return;
    }
    XMMATRIX view, projection;
    RenderCapture::TextureData reflections, gi;
    if (phase >= 6 && Raytracing::IsAvailable())
    {
        Check(RenderCapture::ReadTexture(RayReflections::GetResultResource(), reflections),
              "capture secondary reflections");
        Check(RenderCapture::ReadTexture(RayReflections::GetGiResource(), gi), "capture diffuse GI");
    }
    Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
    for (int z = -4; z <= 4; ++z)
    {
        for (int x = -4; x <= 4; ++x)
        {
            XMFLOAT3 pixel;
            XMStoreFloat3(&pixel, XMVector3Project(XMVectorSet(x * .2f, -.95f, z * .2f, 1), 0, 0, float(image.Width),
                                                   float(image.Height), 0, 1, projection, view, XMMatrixIdentity()));
            if (pixel.x < 0 || pixel.x >= image.Width || pixel.y < 0 || pixel.y >= image.Height)
            {
                continue;
            }
            brightness[phase] += Luminance(image, UINT(pixel.x), UINT(pixel.y));
            if (reflections.Width && gi.Width)
            {
                reflectionBrightness[phase] += Luminance(reflections, UINT(pixel.x * reflections.Width / image.Width),
                                                         UINT(pixel.y * reflections.Height / image.Height));
                giBrightness[phase] +=
                    Luminance(gi, UINT(pixel.x * gi.Width / image.Width), UINT(pixel.y * gi.Height / image.Height));
            }
            ++samples[phase];
        }
    }
    if (frame % FramesPerCase == FramesPerCase - 1)
    {
        RenderCapture::SaveScenePng((L"build/celestial-occlusion/" + to_wstring(phase) + L".png").c_str());
    }
}

bool CelestialOcclusionValidation::Finish()
{
    ofstream report("build/celestial-occlusion/summary.csv");
    report << "case,hdr_luminance,samples,reflection_luminance,gi_luminance\n";
    for (UINT phase = 0; phase < Cases; ++phase)
    {
        Check(samples[phase] >= 200, "sufficient receiver samples");
        brightness[phase] /= (std::max)(samples[phase], 1u);
        reflectionBrightness[phase] /= (std::max)(samples[phase], 1u);
        giBrightness[phase] /= (std::max)(samples[phase], 1u);
        report << phase << ',' << brightness[phase] << ',' << samples[phase] << ',' << reflectionBrightness[phase]
               << ',' << giBrightness[phase] << '\n';
        WriteGraphicsLog("Celestial receiver: case=%u luminance=%.7f samples=%u\n", phase, brightness[phase],
                         samples[phase]);
        if (phase >= 6)
        {
            WriteGraphicsLog("Celestial secondary: case=%u reflection=%.7f GI=%.7f\n", phase,
                             reflectionBrightness[phase], giBrightness[phase]);
        }
    }
    const double moonContrast = brightness[2] - brightness[0];
    if (Raytracing::IsAvailable())
    {
        Check(moonContrast > .005, "open roof admits moonlight above constant environment");
    }
    else
    {
        Check(moonContrast < .002, "no unshadowed virtual moon without a visibility provider");
    }
    const double sunContrast = brightness[3] - brightness[5];
    Check(sunContrast > .05, "open roof admits sunlight above environment-only control");
    const double allowedMoonLeak = (std::max)(.002, moonContrast * .1);
    Check(brightness[1] <= brightness[0] + allowedMoonLeak, "closed roof blocks moonlight");
    Check(brightness[4] <= brightness[0] + allowedMoonLeak, "closing roof blocks moonlight again");
    Check(abs(brightness[0] - brightness[5]) < (std::max)(.002, sunContrast * .1), "closed roof blocks sunlight");
    if (Raytracing::IsAvailable())
    {
        for (UINT phase : {6u, 8u, 10u, 11u})
        {
            Check(reflectionBrightness[phase] < .002, "closed room blocks secondary sky reflection with Sun inactive");
            Check(giBrightness[phase] < .002, "closed room blocks sky GI with Sun inactive");
        }
        Check(reflectionBrightness[7] > .005, "open roof admits traced sky reflection with Sun inactive");
        Check(reflectionBrightness[9] > .005, "open roof admits rough sky reflection with Sun inactive");
        Check(giBrightness[9] > .005, "open roof admits sky GI with Sun inactive");
    }
    Check(lastFrame >= FramesPerCase * Cases - 1, "complete sun movement and roof sequence");
    failures += RenderCapture::ReportGraphicsErrors();
    WriteGraphicsLog("GI validation failures = %u\n", failures);
    return failures == 0;
}
