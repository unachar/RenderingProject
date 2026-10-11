#include "pch.h"
#include "renderingvalidation.h"
#include "rendercapture.h"
#include "rendertargets.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "renderconfiguration.h"
#include "raytracing.h"
#include "dlssupscaler.h"
#include "renderersettings.h"
#include "imguimanager.h"
#include "renderprofiler.h"
#include "proceduralscene.h"
#include "primitivegeometry.h"
#include "simulationconfiguration.h"
#include "daynightcycle.h"
#include "sun.h"
#include "projectmanager.h"
#include "world.h"
#include "vertexdeformation.h"
#include "vertexdeformationmath.h"
#include "nrddenoiser.h"
#include "meshshaderpipeline.h"
#include "rayreflections.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cmath>

namespace
{
EntityID g_Terrain = g_kINVALID_ENTITY;
EntityID g_Mover = g_kINVALID_ENTITY;
UINT g_Failures = 0;
float g_NoonSkyLuminance = 0.0f;
uint64_t g_FrameCount = 0;
double g_PausedTime = 0;
uint64_t g_OriginalTerrainHash = 0;
double g_PreviousVertexTime = 0;
void Check(bool passed, const char* name);

void ValidateReflectionDenoising(const RenderCapture::TextureData& filtered)
{
    if (!NrdDenoiser::IsCompiled()) return;
    RenderCapture::TextureData noisy;
    const bool read = RenderCapture::ReadTexture(RayReflections::GetNoisyResource(), noisy);
    Check(read && noisy.Width == filtered.Width && noisy.Height == filtered.Height,
        "read matching raw NRD reflection input");
    if (!read || noisy.Width != filtered.Width || noisy.Height != filtered.Height) return;

    // NRD's input Y channel is 0.25 R + 0.5 G + 0.25 B. Compare the same luminance
    // in the composed RGB output. Neighbor differences measure high-frequency variation;
    // skip background and hit/miss boundaries so silhouettes do not dominate this fixture.
    double rawVariation = 0, filteredVariation = 0;
    UINT pairs = 0;
    for (UINT y = 0; y < filtered.Height; ++y)
    {
        const auto* raw = reinterpret_cast<const uint16_t*>(noisy.Bytes.data() + y * noisy.RowPitch);
        const auto* output = reinterpret_cast<const uint16_t*>(filtered.Bytes.data() + y * filtered.RowPitch);
        for (UINT x = 1; x < filtered.Width; ++x)
        {
            const UINT current = x * 4, previous = current - 4;
            const float distance = RenderCapture::HalfToFloat(output[current + 3]);
            const float previousDistance = RenderCapture::HalfToFloat(output[previous + 3]);
            if (std::abs(distance) < 0.01f || std::abs(previousDistance) < 0.01f) continue;
            const auto luminance = [&](UINT index)
            {
                return 0.25f * RenderCapture::HalfToFloat(output[index]) +
                    0.5f * RenderCapture::HalfToFloat(output[index + 1]) +
                    0.25f * RenderCapture::HalfToFloat(output[index + 2]);
            };
            rawVariation += std::abs(RenderCapture::HalfToFloat(raw[current]) - RenderCapture::HalfToFloat(raw[previous]));
            filteredVariation += std::abs(luminance(current) - luminance(previous));
            ++pairs;
        }
    }
    WriteGraphicsLog("Validation NRD reflection: neighbor pairs=%u rawVariation=%.6f filteredVariation=%.6f\n",
        pairs, rawVariation / std::max(pairs, 1u), filteredVariation / std::max(pairs, 1u));
    Check(pairs > 100 && rawVariation > 0 && filteredVariation < rawVariation,
        "NRD reduces reflection high-frequency variation in the test scene");
}

void ValidateVertexDeformation()
{
    VertexDeformationView view;
    const bool resolved = VertexDeformation::TryResolveMesh(g_Mover, view);
    Check(resolved && VertexDeformation::GetDispatchCount() > 0, "GPU procedural vertex dispatch");
    if (!resolved) return;
    const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(g_Mover);
    const auto& motion = ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(g_Mover);
    std::vector<uint8_t> current, previous;
    const bool read = RenderCapture::ReadBuffer(view.CurrentResource,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, view.CurrentView.SizeInBytes, current) &&
        RenderCapture::ReadBuffer(view.PreviousResource,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, view.PreviousView.SizeInBytes, previous);
    Check(read, "read current and previous GPU vertices");
    if (!read) return;
    void* mapped = nullptr;
    const CD3DX12_RANGE noRead(0, 0);
    if (FAILED(mesh.VertexBuffer->Map(0, &noRead, &mapped)))
    {
        Check(false, "map immutable procedural base vertices");
        return;
    }
    Procedural::VertexMotionSettings settings;
    settings.Method = motion.Method;
    settings.Amplitude = { motion.Amplitude.x, motion.Amplitude.y, motion.Amplitude.z };
    settings.Speed = motion.Speed;
    settings.SpatialFrequency = motion.SpatialFrequency;
    settings.Seed = motion.Seed;
    bool positions = true, normals = true, attributes = true, moved = false;
    const auto close = [](float a, float b, float tolerance) { return std::isfinite(a) && std::abs(a - b) < tolerance; };
    for (UINT index = 0; index < mesh.VertexCount; ++index)
    {
        const auto& base = static_cast<const Vertex*>(mapped)[index];
        const auto& now = reinterpret_cast<const Vertex*>(current.data())[index];
        const auto& old = reinterpret_cast<const Vertex*>(previous.data())[index];
        const Procedural::Vector3 basePosition{ base.Pos.x, base.Pos.y, base.Pos.z };
        const auto expected = Procedural::DeformPosition(basePosition, settings, motion.Time);
        const auto expectedPrevious = Procedural::DeformPosition(basePosition, settings, g_PreviousVertexTime);
        const auto normal = Procedural::DeformNormal(basePosition,
            { base.Normal.x, base.Normal.y, base.Normal.z }, settings, motion.Time);
        positions &= close(now.Pos.x, expected.X, 0.002f) && close(now.Pos.y, expected.Y, 0.002f) &&
            close(now.Pos.z, expected.Z, 0.002f) && close(old.Pos.x, expectedPrevious.X, 0.002f) &&
            close(old.Pos.y, expectedPrevious.Y, 0.002f) && close(old.Pos.z, expectedPrevious.Z, 0.002f);
        normals &= close(now.Normal.x, normal.X, 0.02f) && close(now.Normal.y, normal.Y, 0.02f) && close(now.Normal.z, normal.Z, 0.02f);
        attributes &= memcmp(&now.Tex, &base.Tex, sizeof(base.Tex)) == 0 &&
            memcmp(&now.Color, &base.Color, sizeof(base.Color)) == 0;
        moved |= std::abs(now.Pos.y - base.Pos.y) > 0.001f;
    }
    const CD3DX12_RANGE noWrite(0, 0);
    mesh.VertexBuffer->Unmap(0, &noWrite);
    Check(positions && moved, "GPU positions and motion history agree with CPU reference");
    Check(normals && attributes, "GPU deformed normals and unchanged UV/color");
}

void Check(bool passed, const char* name)
{
    if (!passed)
    {
        ++g_Failures;
    }
    const char* result = "FAIL";
    if (passed)
    {
        result = "PASS";
    }
    WriteGraphicsLog("Validation: %s %s\n", result, name);
}

void CreateTestScene()
{
    // Check the real startup scene, including registration from separate .cpp files.
    const char* startupNames[] = {
        "Cube", "polygon3d", "XBot", "field", "Moca", "Karen",
        "Kacchatta_Hone", "Alicia", "Sky"
    };
    for (const char* name : startupNames)
    {
        unsigned int count = 0;
        for (EntityID id : World::GetView<NameComponent>())
        {
            if (ComponentManager::GetComponent<NameComponent>(id).Name == name)
            {
                ++count;
            }
        }
        const std::string checkName = std::string("auto-registered startup entity created once: ") + name;
        Check(count == 1, checkName.c_str());
    }

    RendererSettings::SetMeshShadersEnabled(true);
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 meshOptions{};
    if (SUCCEEDED(GraphicsDevice::GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &meshOptions, sizeof(meshOptions))) &&
        meshOptions.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED)
        Check(MeshShaderPipeline::IsAvailable(), "mesh shader pipelines initialize on supported hardware");
    const auto suns = World::GetView<SunComponent>();
    if (suns.begin() == suns.end())
    {
        Check(Sun::CreateDefault() != g_kINVALID_ENTITY, "create simulation Sun");
    }
    for (EntityID sun : World::GetView<SunComponent>())
    {
        Check(!ComponentManager::HasComponent<MeshComponent>(sun) &&
            !ComponentManager::HasComponent<MaterialComponent>(sun), "virtual Sun has no rendered mesh or material");
    }
    Procedural::TerrainSettings terrain;
    terrain.Resolution = 24;
    g_Terrain = ProceduralScene::GenerateTerrain(terrain);
    Check(g_Terrain != g_kINVALID_ENTITY, "generate terrain");
    Entity mover = World::CreateEntity().Add<NameComponent>().Add<MeshComponent>()
        .Add<TransformComponent>().Add<MaterialComponent>().Add<ProceduralMotionComponent>();
    mover.SetName("Procedural Validation Cube");
    g_Mover = mover.GetID();
    auto& transform = mover.Get<TransformComponent>();
    transform.Position = { 0.0f, 4.0f, 0.0f };
    mover.Get<MaterialComponent>().ShaderClassMode = MaterialMode::Manual;
    mover.Get<MaterialComponent>().ShaderClass = ShaderClass::PBR;
    mover.Get<ProceduralMotionComponent>().Origin = transform.Position;
    mover.Get<ProceduralMotionComponent>().Amplitude = { 1.0f, 0.75f, 1.0f };
    VertexResource resource;
    resource.entityid = mover.GetID();
    resource.objectType = ObjectType::CUBE;
    resource.color = Color::WHITE;
    PrimitiveGeometry::CreateObjectVertex(resource);
    Check(ProceduralScene::SavePreset("bin/validation/terrain.proc", terrain), "save terrain preset");
    Procedural::TerrainSettings restored;
    Check(ProceduralScene::LoadPreset("bin/validation/terrain.proc", restored) &&
        Procedural::BuildHeightMap(restored) == Procedural::BuildHeightMap(terrain), "load reproducible terrain preset");
    const auto before = restored;
    std::ofstream invalid("bin/validation/invalid.proc");
    invalid << "PROCEDURAL_TERRAIN 1\n99 0 nan 100 0 0 0\n-1 0 0 0\n";
    invalid.close();
    Check(!ProceduralScene::LoadPreset("bin/validation/invalid.proc", restored) &&
        restored.Noise.Seed == before.Noise.Seed, "reject invalid preset without mutation");
    std::ostringstream configuration;
    SimulationConfiguration::Save(configuration);
    std::istringstream records(configuration.str());
    std::string line;
    while (std::getline(records, line))
    {
        const size_t delimiter = line.find('=');
        Check(SimulationConfiguration::TryLoad(line.substr(0, delimiter), line.substr(delimiter + 1)), "simulation configuration roundtrip");
    }
}

void ValidateRayTexture()
{
    RenderCapture::TextureData data;
    if (!Raytracing::IsActive())
    {
        return;
    }
    Check(RenderCapture::ReadTexture(Raytracing::GetResultResource(), data), "read raytracing texture");
    UINT shadowed = 0;
    UINT occluded = 0;
    bool finite = true;
    for (UINT row = 0; row < data.Height; ++row)
    {
        const auto* values = reinterpret_cast<const uint16_t*>(data.Bytes.data() + static_cast<size_t>(row) * data.RowPitch);
        for (UINT column = 0; column < data.Width; ++column)
        {
            const float shadow = RenderCapture::HalfToFloat(values[column * 2]);
            const float ambient = RenderCapture::HalfToFloat(values[column * 2 + 1]);
            finite &= std::isfinite(shadow) && std::isfinite(ambient) && shadow >= 0.0f && shadow <= 1.0f && ambient >= 0.0f && ambient <= 1.0f;
            shadowed += shadow < 0.99f;
            occluded += ambient < 0.99f;
        }
    }
    Check(finite, "finite bounded ray visibility");
    Check(shadowed > 0 && occluded > 0, "rays intersect scene geometry");
    WriteGraphicsLog("Validation: ray hits shadow=%u ao=%u instances=%u\n", shadowed, occluded, Raytracing::GetInstanceCount());
}

void ValidateSkyBrightness(bool night)
{
    RenderCapture::TextureData texture;
    if (!RenderCapture::ReadTexture(RenderTargets::GetEditorSceneResource(), texture))
    {
        Check(false, "read atmospheric sky");
        return;
    }
    double total = 0;
    UINT samples = 0;
    for (UINT row = texture.Height / 32; row < texture.Height / 6; row += 4)
    {
        for (UINT column = 0; column < texture.Width; column += 4)
        {
            const BYTE* pixel = texture.Bytes.data() + static_cast<size_t>(row) * texture.RowPitch;
            float red, green, blue;
            if (texture.Format == DXGI_FORMAT_R8G8B8A8_UNORM)
            {
                pixel += column * 4;
                red = pixel[0] / 255.0f;
                green = pixel[1] / 255.0f;
                blue = pixel[2] / 255.0f;
            }
            else
            {
                const auto* color = reinterpret_cast<const uint16_t*>(pixel) + column * 4;
                red = RenderCapture::HalfToFloat(color[0]);
                green = RenderCapture::HalfToFloat(color[1]);
                blue = RenderCapture::HalfToFloat(color[2]);
            }
            total += red * 0.2126f + green * 0.7152f + blue * 0.0722f;
            ++samples;
        }
    }
    const float luminance = samples ? static_cast<float>(total / samples) : 0.0f;
    Check(std::isfinite(luminance) && luminance > 0, "finite visible atmospheric sky");
    if (night)
    {
        Check(g_NoonSkyLuminance > luminance * 1.5f, "night sky is darker than noon");
    }
    else
    {
        g_NoonSkyLuminance = luminance;
    }
    WriteGraphicsLog("Validation: %s sky luminance %.4f\n", night ? "night" : "noon", luminance);
}
}

bool RenderingValidation::IsEnabled()
{
    char value[8]{};
    return GetEnvironmentVariableA("DX12_FEATURE_SMOKE_TEST", value, sizeof(value)) > 0 && atoi(value) != 0;
}

void RenderingValidation::BeforeFrame(uint64_t frame)
{
    if (!IsEnabled())
    {
        return;
    }
    if (frame == 0)
    {
        char startup[8]{};
        if (GetEnvironmentVariableA("DX12_VALIDATION_DLSS_STARTUP", startup, sizeof(startup)) > 0 && atoi(startup) != 0)
        {
            RendererSettings::SetUpscaleMode(UpscaleMode::Dlss);
        }
        std::filesystem::create_directories("bin/validation");
        CreateTestScene();
    }
    g_FrameCount = frame + 1;
    const float hours[] = { 6.0f, 12.0f, 18.0f, 0.0f };
    DayNightCycle::GetSettings().Running = false;
    DayNightCycle::GetSettings().DayOfYear = 81;
    DayNightCycle::SetTime(hours[(frame / 8) % 4]);
    Raytracing::GetSettings().Enabled = frame < 32 || frame >= 36;
    Raytracing::GetSettings().Reflections = (frame < 24 || frame >= 28) && frame != 18;
    Raytracing::GetSettings().Shadows = frame != 12 && frame != 13;
    if (frame == 8)
    {
        ProjectManager::Play();
    }
    if (frame == 16)
    {
        g_PausedTime = ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(g_Mover).Time;
        ProjectManager::TogglePause();
    }
    if (frame == 18)
    {
        ProjectManager::TogglePause();
    }
    if (frame == 20)
    {
        ProjectManager::Stop();
    }
    if (frame == 24)
    {
        const auto before = ImGuiManager::CaptureEntity(g_Terrain);
        g_OriginalTerrainHash = ComponentManager::GetComponentUnchecked<MeshComponent>(g_Terrain).GeometryHash;
        auto settings = ComponentManager::GetComponentUnchecked<ProceduralTerrainComponent>(g_Terrain).Settings;
        settings.Noise.Method = Procedural::NoiseMethod::DomainWarp;
        settings.Noise.Seed = 123;
        Check(ProceduralScene::GenerateTerrain(settings, g_Terrain) == g_Terrain, "regenerate existing terrain");
        ImGuiManager::PushUndoSnapshot(before);
    }
    if (frame == 21)
    {
        RendererSettings::SetComputeGBufferEnabled(false);
        auto& motion = ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(g_Mover);
        motion.Target = Procedural::MotionTarget::MeshVertices;
        motion.PreviewInEditor = true;
        motion.Speed = 0.4f;
        motion.Time = 0.6;
        VertexDeformation::ResetHistory(g_Mover);
    }
    if (frame >= 22 && frame <= 38)
        g_PreviousVertexTime = ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(g_Mover).Time;
    if (frame == 39)
        ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(g_Mover).Enabled = false;
    if (frame == 30)
    {
        auto terrain = ComponentManager::GetComponentUnchecked<ProceduralTerrainComponent>(g_Terrain).Settings;
        terrain.Resolution = 256;
        Check(ProceduralScene::GenerateTerrain(terrain, g_Terrain) == g_Terrain, "generate large terrain for mesh shader validation");
        RendererSettings::SetComputeGBufferEnabled(true);
    }
    if (frame == 25)
    {
        ImGuiManager::Undo();
        Check(ComponentManager::GetComponentUnchecked<ProceduralTerrainComponent>(g_Terrain).Settings.Noise.Method ==
            Procedural::NoiseMethod::Fractal &&
            ComponentManager::GetComponentUnchecked<MeshComponent>(g_Terrain).GeometryHash == g_OriginalTerrainHash,
            "Undo restores terrain geometry and settings");
        ImGuiManager::Redo();
        Check(ComponentManager::GetComponentUnchecked<ProceduralTerrainComponent>(g_Terrain).Settings.Noise.Method ==
            Procedural::NoiseMethod::DomainWarp, "Redo restores regenerated terrain");
    }
    if (frame == 28)
    {
        RenderConfiguration::Resize(960, 640);
    }
    if (frame == 40)
    {
        GraphicsDevice::WaitForGpuIdle();
        std::vector<EntityID> removed;
        for (EntityID entity : World::GetView<MeshComponent>())
        {
            if (!ComponentManager::HasComponent<SunComponent>(entity) && !ComponentManager::HasComponent<CameraComponent>(entity))
            {
                removed.push_back(entity);
            }
        }
        for (EntityID entity : removed)
        {
            World::DestroyEntity(Entity(entity));
        }
    }
    if (frame == 44)
    {
        Procedural::TerrainSettings terrain;
        terrain.Resolution = 16;
        g_Terrain = ProceduralScene::GenerateTerrain(terrain);
        Check(g_Terrain != g_kINVALID_ENTITY, "restore geometry after empty scene");
    }
    if (frame == 48 || frame == 68)
    {
        RendererSettings::SetUpscaleMode(UpscaleMode::Dlss);
        RenderConfiguration::SetResolutionScale(0.5f);
    }
    if (frame == 56)
    {
        RenderConfiguration::Resize(1280, 720);
    }
    if (frame == 64)
    {
        RendererSettings::SetUpscaleMode(UpscaleMode::Bilateral);
    }
    if (frame == 68) RenderConfiguration::SetResolutionScale(1.0f / 1.5f);
    if (frame == 70) RenderConfiguration::SetResolutionScale(1.0f / 1.7f);
    if (frame == 72) RenderConfiguration::SetResolutionScale(1.0f / 3.0f);
    if (frame == 74) RenderConfiguration::SetResolutionScale(1.0f / 1.3f);
    if (frame == 82) RenderConfiguration::SetResolutionScale(0.25f);
    if (frame == 76)
    {
        ImGuiManager::m_HdrEnabled = true;
        ImGuiManager::m_ToneMapEnabled = false;
        RenderConfiguration::SetHdr(true);
    }
    if (frame == 79)
    {
        ImGuiManager::m_ToneMapEnabled = true;
    }
}

void RenderingValidation::AfterFrame(uint64_t frame)
{
    if (!IsEnabled())
    {
        return;
    }
    if (frame == 15)
    {
        Check(ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(g_Mover).Time > 0.0, "Play advances motion");
        ValidateRayTexture();
        if (Raytracing::IsAvailable())
        {
            Check(Raytracing::AreReflectionsActive(), "scene raytracing reflection dispatch");
            if (NrdDenoiser::IsCompiled())
            {
                Check(RayReflections::GetDenoiserDispatchCount() > 0, "actual NVIDIA NRD REBLUR_SPECULAR dispatch sequence");
                Check(Raytracing::GetAmbientDenoiserDispatchCount() > 0, "actual NVIDIA NRD REBLUR_DIFFUSE_OCCLUSION dispatch sequence");
            }
            RenderCapture::TextureData reflection;
            const bool read = RenderCapture::ReadTexture(RayReflections::GetResultResource(), reflection);
            Check(read, "read scene reflection radiance");
            if (read)
            {
                UINT hits = 0, offscreenHits = 0, bright = 0;
                bool finite = true;
                for (UINT y = 0; y < reflection.Height; ++y)
                {
                    const auto* pixels = reinterpret_cast<const uint16_t*>(reflection.Bytes.data() + y * reflection.RowPitch);
                    for (UINT x = 0; x < reflection.Width; ++x)
                    {
                        const float distance = RenderCapture::HalfToFloat(pixels[x * 4 + 3]);
                        hits += std::abs(distance) > 0.01f;
                        offscreenHits += distance < -0.01f;
                        for (UINT c = 0; c < 3; ++c)
                        {
                            const float value = RenderCapture::HalfToFloat(pixels[x * 4 + c]);
                            finite &= std::isfinite(value) && value >= 0;
                            bright += value > 0.005f;
                        }
                    }
                }
                Check(finite && hits > 0 && bright > 0, "finite reflected radiance with actual scene intersections");
                Check(offscreenHits > 0, "reflection rays intersect geometry outside the camera image");
                WriteGraphicsLog("Validation reflection: hits=%u offscreenHits=%u brightChannels=%u extent=%ux%u\n", hits, offscreenHits, bright, reflection.Width, reflection.Height);
                ValidateReflectionDenoising(reflection);
            }
        }
        if (Raytracing::IsAvailable() && NrdDenoiser::IsCompiled())
            Check(Raytracing::GetDenoiserDispatchCount() > 0, "actual NVIDIA NRD SIGMA dispatch sequence");
        ValidateSkyBrightness(false);
    }
    if (frame == 31) ValidateSkyBrightness(true);
    if (frame == 13 && Raytracing::IsAvailable())
    {
        Check(Raytracing::AreReflectionsActive(), "reflection-only mode remains active");
        if (NrdDenoiser::IsCompiled()) Check(!RayReflections::DidRestartHistory(), "reflection-only mode accumulates temporal history");
        RenderCapture::TextureData visibility;
        bool unshadowed = RenderCapture::ReadTexture(Raytracing::GetResultResource(), visibility);
        if (unshadowed)
        {
            for (UINT y = 0; y < visibility.Height; ++y)
            {
                const auto* pixels = reinterpret_cast<const uint16_t*>(visibility.Bytes.data() + y * visibility.RowPitch);
                for (UINT x = 0; x < visibility.Width; ++x)
                    unshadowed &= RenderCapture::HalfToFloat(pixels[x * 2]) >= 0.999f;
            }
        }
        Check(unshadowed, "disabled ray shadows produce unshadowed visibility");
    }
    if (frame == 19 && Raytracing::IsAvailable() && NrdDenoiser::IsCompiled())
        Check(RayReflections::DidRestartHistory(), "reflection re-enable restarts stale temporal history");
    if (frame == 25 && Raytracing::IsAvailable())
        Check(!Raytracing::AreReflectionsActive() && Raytracing::IsActive(), "reflection switch leaves raytracing shadows active");
    if (frame == 23 || frame == 29 || frame == 37) ValidateVertexDeformation();
    if ((frame == 23 || frame == 31) && MeshShaderPipeline::IsSupported())
    {
        const auto& stats = MeshShaderPipeline::GetStatistics();
        Check(stats.Draws > 0 && stats.Meshlets > 0 && stats.MaxGroupsPerPage <= 65535,
            frame == 23 ? "mesh shader renders procedural vertices into MRT GBuffer" :
                "mesh shader renders large procedural terrain into visibility GBuffer");
        WriteGraphicsLog("Validation mesh shaders: draws=%u meshlets=%u pages=%u maximumGroups=%u\n",
            stats.Draws, stats.Meshlets, stats.Pages, stats.MaxGroupsPerPage);
        if (frame == 31) Check(stats.Pages > stats.Draws, "GPU executes multiple mesh pages with a nonzero meshlet base");
    }
    if (frame == 39)
    {
        VertexDeformationView view;
        Check(!VertexDeformation::TryResolveMesh(g_Mover, view), "disabled vertex motion restores base geometry");
    }
    if (frame == 17)
    {
        Check(ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(g_Mover).Time == g_PausedTime, "Pause freezes motion");
    }
    if (frame == 19)
    {
        Check(ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(g_Mover).Time > g_PausedTime, "resume advances motion");
    }
    if (frame == 20)
    {
        const auto& motion = ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(g_Mover);
        const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(g_Mover);
        Check(motion.Time == 0.0 && transform.Position.y == motion.Origin.y, "Stop restores motion origin and time");
    }
    if (frame == 29)
    {
        Check(GraphicsDevice::GetWidth() == 960 && GraphicsDevice::GetHeight() == 640, "resize render targets");
    }
    if (frame == 33)
    {
        Check(!Raytracing::IsActive(), "DXR off uses raster fallback");
    }
    if (frame == 37 && Raytracing::IsAvailable())
    {
        Check(Raytracing::IsActive(), "DXR on rebuilds active frame");
    }
    if (frame == 41)
    {
        Check(!Raytracing::IsActive() && Raytracing::GetInstanceCount() == 0, "empty acceleration structure fallback");
    }
    if (frame == 47 && Raytracing::IsAvailable())
    {
        Check(Raytracing::IsActive(), "DXR resumes after geometry recreation");
    }
    if (frame == 5 || frame == 53 || frame == 61 || frame == 69 || frame == 71 || frame == 73 ||
        frame == 75 || frame == 78 || frame == 81 || frame == 83)
    {
        if (frame == 5 && RendererSettings::GetUpscaleMode() != UpscaleMode::Dlss) return;
        if (DlssUpscaler::IsAvailable())
        {
            Check(DlssUpscaler::IsActive(), "DLSS evaluates after resize/toggle/format change");
        }
        else
        {
            char disabled[8]{};
            const bool forcedDisabled = GetEnvironmentVariableA("DX12_DISABLE_DLSS", disabled, sizeof(disabled)) > 0 && atoi(disabled) != 0;
            Check(!DlssUpscaler::IsActive(), "unsupported DLSS uses raster upscale fallback");
            // A capability-positive device must not silently lose DLSS during evaluation.
            Check(forcedDisabled || std::string(DlssUpscaler::GetStatus()).find("failed") == std::string::npos, "DLSS availability does not hide evaluation failure");
        }
        const auto& timing = RenderProfiler::GetLatest();
        for (const auto& pass : timing.Passes)
        {
            if (pass.Name == "DXR Shadows / AO" || pass.Name == "DLSS Super Resolution")
            {
                WriteGraphicsLog("Validation timing: %s GPU %.3f ms CPU %.3f ms\n", pass.Name.c_str(), pass.GpuMs, pass.CpuMs);
            }
        }
    }
    if (frame == 65)
    {
        Check(!DlssUpscaler::IsActive(), "DLSS off uses Bilateral");
    }
    if (frame == 55)
    {
        Check(RenderCapture::SaveScenePng(L"bin/validation/dlss.png"), "capture DLSS output");
    }
    if (frame < 32 && (frame % 8) == 7)
    {
        const wchar_t* paths[] = { L"bin/validation/morning.png", L"bin/validation/noon.png", L"bin/validation/evening.png", L"bin/validation/night.png" };
        Check(RenderCapture::SaveScenePng(paths[frame / 8]), "capture day period");
    }
}

bool RenderingValidation::Finish()
{
    if (!IsEnabled())
    {
        return true;
    }
    GraphicsDevice::WaitForGpuIdle();
    Check(g_FrameCount >= 84, "complete feature smoke sequence");
    g_Failures += RenderCapture::ReportGraphicsErrors();
    WriteGraphicsLog("Validation: feature smoke failures = %u\n", g_Failures);
    return g_Failures == 0;
}
