#include "pch.h"
#include "meshletvalidation.h"
#include "meshletmanager.h"
#include "meshshader/meshshaderrenderer.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "gpuscene/gpuscene.h"
#include "gpuscene/gpuscenechanges.h"
#include "visibility/gpuvisibility.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "rendercapture.h"
#include "renderconfiguration.h"
#include "renderersettings.h"
#include "rendertargets.h"
#include "raytracing.h"
#include "camera.h"
#include "daynightcycle.h"
#include "modelmanager.h"
#include "renderprofiler.h"
#include "world.h"
#include <fstream>
#include <filesystem>

namespace MeshletValidation
{
namespace
{
uint32_t failures = 0;
uint32_t disabledMesh = UINT32_MAX;
uint64_t frames = 0;
std::vector<EntityID> fixtures;
int model = -1, high = -1, sections = -1, cube = -1, sphere = -1;
std::vector<RenderCapture::TextureData> reference;
std::ofstream measurements;
double traditionalOpaque = 0, meshOpaque = 0, meshCompute = 0;
double traditionalFrame = 0, meshFrame = 0, traditionalSubmission = 0, meshSubmission = 0;
uint32_t traditionalSamples = 0, meshSamples = 0;
void Check(bool success, const char *name)
{
    WriteGraphicsLog("Meshlet validation: %s %s\n", success ? "PASS" : "FAIL", name);
    if (!success)
        ++failures;
}
void Grid(const std::string &path, uint32_t size, uint32_t parts = 1)
{
    std::ofstream file(path);
    if (parts > 1)
    {
        auto mtl = std::filesystem::path(path).replace_extension(".mtl");
        std::ofstream material(mtl);
        file << "mtllib " << mtl.filename().string() << '\n';
        for (uint32_t part = 0; part < parts; ++part)
            material << "newmtl Section" << part << "\nKd " << 0.2f + 0.1f * part << " 1 1\n";
    }
    file << "vn 0 0 -1\n";
    for (uint32_t y = 0; y <= size; ++y)
        for (uint32_t x = 0; x <= size; ++x)
            file << "v " << float(x) * 4 / size - 2 << ' ' << float(y) * 4 / size - 2 << " 0\n";
    for (uint32_t part = 0; part < parts; ++part)
    {
        file << "g Section" << part << "\nusemtl Section" << part << '\n';
        for (uint32_t y = 0; y < size; ++y)
            for (uint32_t x = 0; x < size; ++x)
            {
                if (y * parts / size != part)
                    continue;
                uint32_t a = y * (size + 1) + x + 1, b = a + 1, c = a + size + 1, d = c + 1;
                file << "f " << a << "//1 " << c << "//1 " << b << "//1\nf " << b << "//1 " << c << "//1 "
                     << d << "//1\n";
            }
    }
}
void Clear()
{
    for (auto id : fixtures)
        World::DestroyEntity(Entity(id));
    fixtures.clear();
}
void WritePrimitives()
{
    std::ofstream box("build/meshlets/cube.obj");
    box << "v -1 -1 -1\nv 1 -1 -1\nv 1 1 -1\nv -1 1 -1\nv -1 -1 1\nv 1 -1 1\nv 1 1 1\nv -1 1 1\n";
    box << "f 1 3 2\nf 1 4 3\nf 5 6 7\nf 5 7 8\nf 1 5 8\nf 1 8 4\nf 2 3 7\nf 2 7 6\nf 1 2 6\nf 1 6 5\nf 4 8 "
           "7\nf 4 7 3\n";
    std::ofstream ball("build/meshlets/sphere.obj");
    constexpr uint32_t latitude = 24, longitude = 48;
    for (uint32_t y = 0; y <= latitude; ++y)
        for (uint32_t x = 0; x <= longitude; ++x)
        {
            float phi = XM_PI * float(y) / latitude, theta = XM_2PI * float(x) / longitude;
            ball << "v " << sinf(phi) * cosf(theta) << ' ' << cosf(phi) << ' ' << sinf(phi) * sinf(theta)
                 << '\n';
        }
    for (uint32_t y = 0; y < latitude; ++y)
        for (uint32_t x = 0; x < longitude; ++x)
        {
            uint32_t a = y * (longitude + 1) + x + 1, b = a + 1, c = a + longitude + 1, d = c + 1;
            ball << "f " << a << ' ' << b << ' ' << c << "\nf " << b << ' ' << d << ' ' << c << '\n';
        }
}
void Grow(size_t count, int mesh)
{
    while (fixtures.size() < count)
    {
        Entity e = World::CreateEntity();
        if (!e.IsValid())
        {
            Check(false, "ECS capacity");
            break;
        }
        e.Add<TransformComponent>()
            .Add<StaticModelComponent>()
            .Add<MaterialComponent>()
            .Add<ShaderComponent>()
            .Add<NameComponent>();
        e.Get<NameComponent>().Name = "Meshlet fixture";
        e.Get<StaticModelComponent>().ModelId = mesh;
        auto &mat = e.Get<MaterialComponent>();
        mat.ShaderClassMode = MaterialMode::Manual;
        mat.ShaderClass = ShaderClass::PBR;
        mat.Roughness = 0.6f;
        mat.Metallic = 0.2f;
        auto &t = e.Get<TransformComponent>();
        if (count > 1)
        {
            size_t i = fixtures.size();
            t.Position = {float(i % 100) * 0.09f - 4.5f, float((i / 100) % 100) * 0.09f - 4.5f, 0};
            t.Scale = {0.018f, 0.018f, 0.018f};
        }
        t.IsDirty = true;
        fixtures.push_back(e.GetID());
    }
}
void Capture(bool compare, bool compareVelocity = true)
{
    std::vector<RenderCapture::TextureData> output(6);
    bool read = true;
    for (uint32_t i = 0; i < 6; ++i)
        read &= RenderCapture::ReadTexture(
            RenderTargets::GetGBufferResource(i == 5 ? GBufferType::VELOCITY : static_cast<GBufferType>(i)),
            output[i]);
    Check(read, "read GBuffer");
    if (!read)
        return;
    if (!compare)
    {
        reference = std::move(output);
        return;
    }
    uint64_t changed = 0, total = 0;
    for (size_t i = 0; i < output.size(); ++i)
    {
        if (i == 5)
        {
            if (!compareVelocity)
                continue;
            float maximumMotionError = 0;
            for (uint32_t y = 0; y < output[i].Height; ++y)
                for (uint32_t x = 0; x < output[i].Width; ++x)
                {
                    auto *current =
                        reinterpret_cast<const uint16_t *>(output[i].Bytes.data() + y * output[i].RowPitch) +
                        x * 4;
                    auto *previous = reinterpret_cast<const uint16_t *>(reference[i].Bytes.data() +
                                                                        y * reference[i].RowPitch) +
                                     x * 4;
                    for (uint32_t lane = 2; lane < 4; ++lane)
                        maximumMotionError = (std::max)(maximumMotionError,
                                                        std::abs(RenderCapture::HalfToFloat(current[lane]) -
                                                                 RenderCapture::HalfToFloat(previous[lane])));
                }
            WriteGraphicsLog("Meshlet velocity comparison maximum UV error=%.8f\n", maximumMotionError);
            Check(maximumMotionError < 0.00001f, "Traditional/Mesh Shader velocity UV agreement");
            continue; // Encoded display channels magnify tiny near-zero floating-point errors.
        }
        if (i >= reference.size() || reference[i].Bytes.size() != output[i].Bytes.size())
        {
            Check(false, "matching GBuffer dimensions");
            return;
        }
        for (size_t b = 0; b < output[i].Bytes.size(); ++b)
        {
            changed += std::abs(int(output[i].Bytes[b]) - int(reference[i].Bytes[b])) > 1;
            ++total;
        }
    }
    WriteGraphicsLog("Meshlet image comparison changed=%llu total=%llu\n", changed, total);
    Check(changed <= total / 10000, "Traditional/Mesh Shader GBuffer agreement");
}
std::array<uint32_t, 16> Stats()
{
    std::array<uint32_t, 16> stats{};
    std::vector<uint8_t> bytes;
    Check(MeshShaderRenderer::IsActive(), "GPU Mesh Shader path active");
    if (MeshShaderRenderer::IsActive() &&
        RenderCapture::ReadBuffer(MeshShaderRenderer::StatisticsBuffer(),
                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 64, bytes))
        std::memcpy(stats.data(), bytes.data(), 64);
    else
        Check(false, "read Meshlet counters");
    return stats;
}
} // namespace
bool Enabled()
{
    char env[8]{};
    return GetEnvironmentVariableA("DX12_MESHLET_STRESS_TEST", env, sizeof(env)) && atoi(env) != 0;
}
void BeforeFrame(uint64_t frame)
{
    frames = frame + 1;
    if (frame == 0)
    {
        std::vector<EntityID> remove;
        for (uint32_t i = 0; i < Registry::GetEntities().size(); ++i)
            if (Registry::IsAlive(i) && !ComponentManager::HasComponent<CameraComponent>(i))
                remove.push_back(i);
        for (auto id : remove)
            World::DestroyEntity(Entity(id));
        std::filesystem::create_directories("build/meshlets");
        Grid("build/meshlets/grid.obj", 32);
        Grid("build/meshlets/high.obj", 724);
        Grid("build/meshlets/sections.obj", 10, 5);
        WritePrimitives();
        model = ModelManager::LoadStaticModel("build/meshlets/grid.obj", false);
        high = ModelManager::LoadStaticModel("build/meshlets/high.obj", false);
        sections = ModelManager::LoadStaticModel("build/meshlets/sections.obj", false);
        auto *sectionAsset = ModelManager::GetStaticModel(sections);
        Check(sectionAsset &&
                  sectionAsset->LoadAssimpModel("build/meshlets/sections.obj", GraphicsDevice::GetDevice(),
                                                false) &&
                  sectionAsset->GetMeshCount() == 5,
              "import five independent material sections");
        cube = ModelManager::LoadStaticModel("build/meshlets/cube.obj", false);
        sphere = ModelManager::LoadStaticModel("build/meshlets/sphere.obj", false);
        Check(model >= 0 && high >= 0 && sections >= 0, "import fixtures including million triangles");
        Check(MeshShaderRenderer::Supported(), "hardware Mesh Shader support");
        Grow(1, model);
        measurements.open("build/meshlets/comparison.csv");
        measurements << "frame,active,instances,candidates,visible,clusters,visibleClusters,frustum,cone,"
                        "occlusion,vertices,primitives,overflow,cpuSubmissionMs,gpuFrameMs\n";
        World::SetVSyncEnabled(false);
    }
    RenderConfiguration::SetRenderMode(RenderMode::DEFERRED);
    RendererSettings::SetComputeGBufferEnabled(false);
    RendererSettings::SetMeshShadersEnabled(false);
    RendererSettings::SetTwoPhaseOcclusionEnabled(false);
    RendererSettings::SetUpscaleMode(UpscaleMode::Bilateral);
    Raytracing::GetSettings().Enabled = false;
    DayNightCycle::GetSettings().Running = false;
    GPUDrivenRenderer::SetEnabled(true);
    GPUVisibility::GetSettings().Occlusion = false;
    GPUVisibility::GetSettings().LOD = false;
    auto &options = MeshShaderRenderer::GetSettings();
    options.Enabled = frame >= 3;
    options.Frustum = frame >= 6;
    options.Cone = frame >= 6;
    options.Occlusion = false;
    options.Clusters = true;
    options.MaxMeshlets = 2000000;
    if (frame >= 15 && frame <= 17)
        options.MaxMeshlets = 1;
    auto &camera = ComponentManager::GetComponentUnchecked<CameraComponent>(Camera::GetCameraEntity());
    auto &transform = ComponentManager::GetComponentUnchecked<TransformComponent>(Camera::GetCameraEntity());
    transform.Position = {0, 0, -12};
    transform.IsDirty = true;
    camera.Target = {0, 0, 0};
    camera.AllowUserControl = false;
    camera.LockOnTarget = g_kINVALID_ENTITY;
    if (frame == 9 || frame == 12)
    {
        auto &material = Entity(fixtures[0]).Get<MaterialComponent>();
        material.DoubleSided = frame == 9;
        GpuSceneChanges::Notify(fixtures[0], GpuSceneDirtyFlags::Material);
    }
    if (frame == 12)
    {
        auto &t = Entity(fixtures[0]).Get<TransformComponent>();
        t.Scale.x = -1;
        t.IsDirty = true;
    }
    if (frame == 14)
    {
        auto &t = Entity(fixtures[0]).Get<TransformComponent>();
        t.Scale.x = 1;
        t.IsDirty = true;
    }
    if (frame == 21)
    {
        Clear();
        Grow(1000, model);
    }
    if (frame == 27)
    {
        Clear();
        Grow(1, high);
    }
    if (frame == 22 || frame == 28 || frame == 32)
        options.Enabled = false;
    if (frame >= 32 && frame < 36)
    {
        transform.Position = {1.8f, 0, -0.8f};
        camera.Target = {1.8f, 0, 0};
    }
    if (frame == 36)
    {
        Clear();
        Grow(10000, sections);
    }
    if (frame >= 40 && frame < 44)
    {
        transform.Position = {frame % 2 ? 30.f : -30.f, 0, -12};
        camera.Target = {transform.Position.x, 0, 0};
    }
    if (frame == 44)
    {
        Clear();
        Grow(1, model);
    }
    if (frame >= 45 && frame < 48)
    {
        transform.Position = {0, 0, 0.001f};
        camera.Target = {0, 0, 1};
    }
    if (frame == 48)
    {
        auto &material = Entity(fixtures[0]).Get<MaterialComponent>();
        material.DoubleSided = true;
        GpuSceneChanges::Notify(fixtures[0], GpuSceneDirtyFlags::Material);
    }
    if (frame == 51)
        options.Clusters = false;
    if (frame == 54)
    {
        Clear();
        Grow(1, high);
        Grow(2, model);
        auto &wall = Entity(fixtures[1]).Get<TransformComponent>();
        wall.Position = {-1, 0, -2};
        wall.Scale = {0.5f, 1, 1};
        wall.IsDirty = true;
    }
    if (frame >= 54 && frame < 62)
    {
        GPUVisibility::GetSettings().Occlusion = true;
        options.Occlusion = frame >= 58;
        options.Cone = false;
        options.Clusters = true;
    }
    if (frame == 62)
    {
        Clear();
        Grow(1, high);
        auto e = Entity(fixtures[0]);
        e.Add<LODComponent>();
        auto &lod = e.Get<LODComponent>();
        lod.UseLOD = true;
        lod.UseAssetSettings = false;
        GpuSceneChanges::Notify(fixtures[0], GpuSceneDirtyFlags::All);
    }
    if (frame >= 62 && frame < 72)
    {
        GPUVisibility::GetSettings().LOD = true;
        GPUVisibility::GetSettings().ForceLOD = int((frame - 62) / 2);
        options.Enabled = (frame % 2) != 0;
        options.Frustum = false;
        options.Cone = false;
    }
    else
        GPUVisibility::GetSettings().ForceLOD = -1;
    if (frame == 72)
    {
        auto &t = Entity(fixtures[0]).Get<TransformComponent>();
        t.Scale = {2, 0.5f, 1};
        t.IsDirty = true;
    }
    if (frame == 72 || frame == 78)
        options.Enabled = false;
    if (frame == 74)
    {
        auto &t = Entity(fixtures[0]).Get<TransformComponent>();
        t.Position.x += 0.2f;
        t.IsDirty = true;
    }
    if (frame == 75)
    {
        auto &t = Entity(fixtures[0]).Get<TransformComponent>();
        t.Position.x -= 0.2f;
        t.IsDirty = true;
    }
    if (frame == 76)
    {
        auto &t = Entity(fixtures[0]).Get<TransformComponent>();
        t.Scale = {1, 1, 1};
        t.IsDirty = true;
    }
    if (frame == 80)
    {
        const auto *instance = GpuScene::GetInstance(GpuScene::GetEntityInstance(fixtures[0]));
        if (instance)
        {
            disabledMesh = instance->MeshIndex;
            MeshletManager::Invalidate(disabledMesh);
        }
    }
    if (frame == 84)
    {
        Clear();
        Grow(1, cube);
    }
    if (frame == 87)
    {
        Clear();
        Grow(1, sphere);
    }
    if (frame == 85 || frame == 88)
        options.Enabled = false;
    if (frame >= 84 && frame < 90)
    {
        options.Frustum = false;
        options.Cone = false;
    }
    if (frame == 91)
    {
        Entity(fixtures[0]).Get<MaterialComponent>().DoubleSided = true;
        GpuSceneChanges::Notify(fixtures[0], GpuSceneDirtyFlags::Material);
    }
    if (frame == 91 || frame == 92)
    {
        transform.Position = {0, 0, 0};
        camera.Target = {0, 0, 1};
    }
    if (frame == 92)
        options.MaxMeshlets = 1;
    if (frame == 93)
    {
        Clear();
        Grow(1000, high);
        for (size_t i = 0; i < fixtures.size(); ++i)
        {
            auto &t = Entity(fixtures[i]).Get<TransformComponent>();
            t.Position = {float(i % 100) * 5 - 250, float(i / 100) * 5 - 25, 0};
            t.Scale = {1, 1, 1};
            t.IsDirty = true;
        }
        MeshletManager::RestoreForValidation(disabledMesh);
    }
    if (frame == 94 || frame == 95)
    {
        transform.Position = {50, 0, -12};
        camera.Target = {50, 0, 0};
    }
    if (frame == 96)
    {
        Clear();
        Grow(1, high);
    }
    if (frame >= 96)
    {
        transform.Position = {1.8f, 0, -0.8f};
        camera.Target = {1.8f, 0, 0};
        options.Enabled = frame >= 104;
        options.Cone = false;
        options.Clusters = true;
    }
}
void AfterFrame(uint64_t frame)
{
    if (frame == 74 || frame == 75)
    {
        RenderCapture::TextureData velocity;
        bool read =
            RenderCapture::ReadTexture(RenderTargets::GetGBufferResource(GBufferType::VELOCITY), velocity);
        double sum = 0;
        uint32_t samples = 0;
        if (read)
            for (uint32_t y = 0; y < velocity.Height; ++y)
                for (uint32_t x = 0; x < velocity.Width; ++x)
                {
                    const auto *pixel =
                        reinterpret_cast<const uint16_t *>(velocity.Bytes.data() + y * velocity.RowPitch) +
                        x * 4;
                    float motion = RenderCapture::HalfToFloat(pixel[2]);
                    if (std::abs(motion) > 1e-5f)
                    {
                        sum += motion;
                        ++samples;
                    }
                }
        XMMATRIX view, projection;
        Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
        const float current = Entity(fixtures[0]).Get<TransformComponent>().Position.x;
        XMFLOAT4 now, previous;
        XMStoreFloat4(&now, XMVector4Transform(XMVectorSet(current, 0, 0, 1), view * projection));
        XMStoreFloat4(&previous,
                      XMVector4Transform(XMVectorSet(current + (frame == 74 ? -0.2f : 0.2f), 0, 0, 1),
                                         view * projection));
        float expected = (now.x / now.w - previous.x / previous.w) * 0.5f;
        double observed = samples ? sum / samples : 0;
        WriteGraphicsLog("Meshlet moving velocity expected=%.8f observed=%.8f pixels=%u\n", expected,
                         observed, samples);
        Check(read && samples > 100 && std::abs(observed - expected) < 0.00002,
              "moving mesh Current/Previous GPU Scene velocity");
    }
    if (frame == 1 || frame == 22 || frame == 28 || frame == 32)
        Capture(false);
    if (frame == 56 || frame == 72 || frame == 78 || (frame >= 62 && frame < 72 && frame % 2 == 0))
        Capture(false);
    if (frame == 59 || frame == 81 || (frame >= 62 && frame < 72 && frame % 2 != 0))
        Capture(true);
    if (frame == 73)
        Capture(true, false); // Reference frame applies scale change; motion histories differ.
    if (frame == 85 || frame == 88 || frame == 91 || frame == 100)
        Capture(false);
    if (frame == 86 || frame == 89 || frame == 90 || frame == 92 || frame == 108)
        Capture(true);
    if (frame == 4 || frame == 7 || frame == 10 || frame == 16 || frame == 24 || frame == 30 || frame == 34)
    {
        Capture(true, frame != 34);
        Check(MeshShaderRenderer::IsActive(), "required Mesh Shader execution");
    }
    if (MeshShaderRenderer::IsActive())
    {
        auto stats = Stats();
        WriteGraphicsLog("Meshlet frame=%llu objects=%zu candidate=%u visible=%u cluster=%u "
                         "visibleCluster=%u frustum=%u cone=%u occlusion=%u primitives=%u overflow=%u\n",
                         frame, fixtures.size(), stats[11], stats[1], stats[2], stats[3], stats[4], stats[5],
                         stats[6], stats[8], stats[9]);
        if (frame == 4)
            Check(stats[1] > 0 && stats[8] == 2048, "meshlet vertex/primitive execution");
        if (frame == 10 || frame == 49)
            Check(stats[5] == 0, "double-sided disables cone");
        if (frame == 13)
            Check(stats[5] == 0, "mirrored transform disables cone");
        if (frame == 16)
            Check(stats[9] > 0 && stats[1] == 0, "whole-object capacity fallback");
        if (frame == 24)
            Check(stats[1] > 0 && stats[9] == 0, "shared geometry 1000 instances");
        if (frame == 30)
            Check(stats[8] >= 1000000, "million triangle mesh output");
        if (frame == 34)
            Check(stats[4] > 0 || stats[10] > 0, "partial mesh cluster/meshlet culling");
        if (frame == 38)
            Check(GpuScene::Statistics().ActiveInstances >= 50000, "50000 multi-material instances");
        if (frame == 59)
            Check(stats[6] > 0 || stats[10] > 0, "meshlet or cluster Hi-Z culls partial occlusion");
        if (frame >= 62 && frame < 72 && frame % 2 != 0)
        {
            uint32_t slot = GpuScene::GetEntityInstance(fixtures[0]);
            std::vector<uint8_t> bytes;
            bool read = RenderCapture::ReadBuffer(GPUVisibility::ResultBuffer(),
                                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                  uint64_t(slot + 1) * 16, bytes);
            Visibility::VisibleInstanceData selected{};
            if (read)
                std::memcpy(&selected, bytes.data() + size_t(slot) * 16, 16);
            Check(read && selected.SelectedLOD == (frame - 62) / 2, "correct GPU selected LOD");
            if (read && selected.SelectedMeshID < MeshletManager::Ranges().size())
                Check(stats[1] == MeshletManager::Ranges()[selected.SelectedMeshID].MeshletCount,
                      "correct selected meshlet range");
        }
        if (frame == 81)
            Check(stats[1] == 0, "missing meshlet asset draws with traditional fallback");
        if (frame == 90)
            Check(stats[5] > 0, "sphere cone culling removes back-facing meshlets");
        if (frame == 92)
            Check(stats[5] == 0 && stats[1] == 0 && stats[9] > 0,
                  "inside double-sided sphere survives capacity fallback");
        if (frame == 94)
            Check(GpuScene::Statistics().ActiveInstances >= 1000,
                  "1000 shared million-triangle mesh instances");
        const auto &profile = RenderProfiler::GetLatest();
        measurements << frame << ",1," << GpuScene::Statistics().ActiveInstances << ',' << stats[11] << ','
                     << stats[1] << ',' << stats[2] << ',' << stats[3] << ',' << stats[4] << ',' << stats[5]
                     << ',' << stats[6] << ',' << stats[7] << ',' << stats[8] << ',' << stats[9] << ','
                     << GPUDrivenRenderer::SubmissionMilliseconds() << ',' << profile.GpuRenderMs << '\n';
    }
    if ((frame >= 100 && frame <= 103) || (frame >= 108 && frame <= 111))
    {
        if (frame <= 103) {
            traditionalFrame += RenderProfiler::GetLatest().GpuRenderMs;
            traditionalSubmission += GPUDrivenRenderer::SubmissionMilliseconds();
        } else {
            meshFrame += RenderProfiler::GetLatest().GpuRenderMs;
            meshSubmission += GPUDrivenRenderer::SubmissionMilliseconds();
        }
        for (const auto &pass : RenderProfiler::GetLatest().Passes)
        {
            if (frame <= 103 && pass.Name == "Opaque ExecuteIndirect")
            {
                traditionalOpaque += pass.GpuMs;
                ++traditionalSamples;
            }
            if (frame >= 108 && pass.Name == "Mesh Shader Opaque")
            {
                meshOpaque += pass.GpuMs;
                ++meshSamples;
            }
            if (frame >= 108 && (pass.Name == "Cluster Culling" || pass.Name == "Meshlet Culling" ||
                                 pass.Name == "Mesh Dispatch Generation"))
                meshCompute += pass.GpuMs;
        }
    }
    Check(RenderCapture::ReportGraphicsErrors() == 0, "no D3D12 errors");
}
bool Finish()
{
    GraphicsDevice::WaitForGpuIdle();
    Check(frames >= 112, "complete meshlet sequence");
    WriteGraphicsLog("Meshlet performance samples traditional=%u mesh=%u opaqueTraditionalMs=%.5f "
                     "opaqueMeshMs=%.5f meshComputeMs=%.5f\n",
                     traditionalSamples, meshSamples,
                     traditionalSamples ? traditionalOpaque / traditionalSamples : 0,
                     meshSamples ? meshOpaque / meshSamples : 0, meshSamples ? meshCompute / meshSamples : 0);
    WriteGraphicsLog("Meshlet performance totalGPU traditionalMs=%.5f meshMs=%.5f CPU submission traditionalMs=%.5f meshMs=%.5f\n",
        traditionalSamples ? traditionalFrame / traditionalSamples : 0,
        meshSamples ? meshFrame / meshSamples : 0,
        traditionalSamples ? traditionalSubmission / traditionalSamples : 0,
        meshSamples ? meshSubmission / meshSamples : 0);
    Check(RenderCapture::ReportGraphicsErrors() == 0, "final D3D12 errors");
    measurements.close();
    WriteGraphicsLog("Meshlet validation failures = %u\n", failures);
    return failures == 0;
}
} // namespace MeshletValidation
