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
#include "proceduralscene.h"
#include "virtualgeometry/virtualgeometrydebug.h"
#include "virtualgeometry/virtualgeometryruntime.h"
#include <fstream>
#include <filesystem>

namespace MeshletValidation
{
namespace
{
uint32_t failures = 0;
uint32_t disabledMesh = UINT32_MAX;
uint64_t frames = 0;
vector<EntityID> fixtures;
int model = -1, high = -1, sections = -1, cube = -1, sphere = -1;
vector<RenderCapture::TextureData> reference;
ofstream measurements;
double traditionalOpaque = 0, meshOpaque = 0, meshCompute = 0;
double traditionalFrame = 0, meshFrame = 0, traditionalSubmission = 0, meshSubmission = 0;
uint32_t traditionalSamples = 0, meshSamples = 0;
EntityID terrain = g_kINVALID_ENTITY;
EntityID terrainClone = g_kINVALID_ENTITY;
uint64_t sharedTerrainHash = 0, sharedTerrainAddress = 0;
uint32_t sharedTerrainMesh = UINT32_MAX;
Procedural::TerrainSettings terrainSettings;
uint64_t terrainVertexAddress = 0;
XMFLOAT4X4 frozenCamera{};
uint64_t repeatedEditArena = 0;
uint32_t repeatedEditMeshID = UINT32_MAX, repeatedEditMeshSlots = 0;
void Check(bool success, const char *name)
{
    WriteGraphicsLog("Meshlet validation: %s %s\n", success ? "PASS" : "FAIL", name);
    if (!success)
        ++failures;
}
void Grid(const string &path, uint32_t size, uint32_t parts = 1)
{
    ofstream file(path);
    if (parts > 1)
    {
        auto mtl = filesystem::path(path).replace_extension(".mtl");
        ofstream material(mtl);
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
    ofstream box("build/meshlets/cube.obj");
    box << "v -1 -1 -1\nv 1 -1 -1\nv 1 1 -1\nv -1 1 -1\nv -1 -1 1\nv 1 -1 1\nv 1 1 1\nv -1 1 1\n";
    box << "f 1 3 2\nf 1 4 3\nf 5 6 7\nf 5 7 8\nf 1 5 8\nf 1 8 4\nf 2 3 7\nf 2 7 6\nf 1 2 6\nf 1 6 5\nf 4 8 "
           "7\nf 4 7 3\n";
    ofstream ball("build/meshlets/sphere.obj");
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
    vector<RenderCapture::TextureData> output(6);
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
        reference = move(output);
        return;
    }
    uint64_t changed = 0, total = 0, paddingChanged = 0;
    for (size_t i = 0; i < output.size(); ++i)
    {
        if (i >= reference.size() || output[i].Width != reference[i].Width ||
            output[i].Height != reference[i].Height || output[i].Format != reference[i].Format ||
            output[i].RowPitch != reference[i].RowPitch ||
            output[i].Bytes.size() != reference[i].Bytes.size())
        {
            Check(false, "matching GBuffer dimensions and format");
            return;
        }
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
                        maximumMotionError = (max)(maximumMotionError,
                                                        abs(RenderCapture::HalfToFloat(current[lane]) -
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
        const auto description = RenderTargets::GetGBufferResource(static_cast<GBufferType>(i))->GetDesc();
        UINT64 rowBytes = 0;
        GraphicsDevice::GetDevice()->GetCopyableFootprints(&description, 0, 1, 0, nullptr, nullptr,
                                                          &rowBytes, nullptr);
        uint64_t targetChanged = 0;
        for (uint32_t y = 0; y < output[i].Height; ++y)
        {
            const size_t row = size_t(y) * output[i].RowPitch;
            // CopyTextureRegion does not define row padding. Compare only actual texel bytes.
            for (size_t b = 0; b < rowBytes; ++b)
            {
                targetChanged += abs(int(output[i].Bytes[row + b]) -
                                          int(reference[i].Bytes[row + b])) > 1;
                ++total;
            }
            const size_t end = (min)(row + output[i].RowPitch, output[i].Bytes.size());
            for (size_t b = row + size_t(rowBytes); b < end; ++b)
                paddingChanged += abs(int(output[i].Bytes[b]) - int(reference[i].Bytes[b])) > 1;
        }
        changed += targetChanged;
        WriteGraphicsLog("Meshlet image target=%zu changed texel bytes=%llu\n", i, targetChanged);
    }
    WriteGraphicsLog("Meshlet image comparison changed=%llu total=%llu ignoredPaddingChanges=%llu\n",
                     changed, total, paddingChanged);
    Check(changed <= total / 10000, "Traditional/Mesh Shader GBuffer agreement");
}
array<uint32_t, 16> Stats()
{
    array<uint32_t, 16> stats{};
    vector<uint8_t> bytes;
    Check(MeshShaderRenderer::IsActive(), "GPU Mesh Shader path active");
    if (MeshShaderRenderer::IsActive() &&
        RenderCapture::ReadBuffer(MeshShaderRenderer::StatisticsBuffer(),
                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 64, bytes))
        memcpy(stats.data(), bytes.data(), 64);
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
        vector<EntityID> remove;
        for (uint32_t i = 0; i < Registry::GetEntities().size(); ++i)
            if (Registry::IsAlive(i) && !ComponentManager::HasComponent<CameraComponent>(i))
                remove.push_back(i);
        for (auto id : remove)
            World::DestroyEntity(Entity(id));
        filesystem::create_directories("build/meshlets");
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
    if (frame == 82)
    {
        Grid("build/meshlets/legacy.obj", 8);
        int legacy = ModelManager::LoadStaticModel("build/meshlets/legacy.obj", false);
        auto *asset = ModelManager::GetStaticModel(legacy);
        Check(asset && asset->GetMeshCount() == 1, "import legacy asset during rendering");
        if (asset)
            for (uint32_t i = 0; i < asset->GetMeshCount(); ++i)
                // Simulate an imported asset predating meshlet metadata before its first registration.
                const_cast<StaticMeshData &>(asset->GetMeshData(i)).MeshletLODs = {};
        Clear();
        Grow(1, legacy);
        options.Enabled = false;
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
    if (frame >= 112)
    {
        options.Enabled = true;
        options.Frustum = false;
        options.Cone = false;
        options.Occlusion = false;
        options.Clusters = false;
        GPUVisibility::GetSettings().Occlusion = false;
        if (frame == 112)
        {
            Clear();
            terrainSettings.Resolution=16;
            terrainSettings.Size=8;
            terrainSettings.Height=1;
            terrainSettings.BaseHeight=0;
            terrain=ProceduralScene::GenerateTerrain(terrainSettings);
            Check(Registry::IsAlive(terrain),"generate procedural Nanite terrain");
            if (Registry::IsAlive(terrain))
            {
                fixtures.push_back(terrain);
                const auto& mesh=Entity(terrain).Get<MeshComponent>();
                terrainVertexAddress=mesh.VertexBufferView.BufferLocation;
                Check(mesh.NaniteMesh && mesh.NaniteMesh->VirtualGeometryLODs[0] &&
                    mesh.NaniteMesh->VirtualGeometryLODs[0]->Error.empty(),"terrain automatically builds VirtualGeometry");
            }
        }
        if (frame < 120)
        {
            transform.Position={0,8,-12}; camera.Target={0,0,0};
            GPUDrivenRenderer::SetEnabled(frame!=112 && frame!=113 && frame!=116 && frame!=117);
        }
        if (frame == 116 && Registry::IsAlive(terrain))
        {
            terrainSettings.Height=2;
            terrainSettings.Noise.Seed+=17;
            Check(ProceduralScene::GenerateTerrain(terrainSettings,terrain)==terrain,"edit terrain and rebuild Nanite");
            Check(Entity(terrain).Get<MeshComponent>().VertexBufferView.BufferLocation==terrainVertexAddress,
                "same-resolution terrain edit reuses vertex buffer");
        }
        if (frame == 120)
        {
            Clear(); Grow(1,model);
            auto e=Entity(fixtures[0]); e.Add<LODComponent>();
            auto& lod=e.Get<LODComponent>(); lod.UseLOD=true; lod.UseAssetSettings=false;
            GpuSceneChanges::Notify(fixtures[0],GpuSceneDirtyFlags::All);
        }
        if (frame>=120 && frame<124)
        {
            GPUVisibility::GetSettings().LOD=true;
            GPUVisibility::GetSettings().ForceLOD=2;
            GPUVisibility::GetSettings().DebugMode=2;
            options.Enabled=frame>=122;
        }
        if (frame>=124) GPUVisibility::GetSettings().DebugMode=0;
        if (frame == 124) GPUVisibility::GetSettings().FreezeCamera=true;
        if (frame>=125 && frame<=127)
        {
            transform.Position={20,0,-12}; camera.Target={20,0,0};
            if (frame==126) { transform.Position={0,0,-12}; camera.Target={0,0,0}; }
            options.MeshletBounds=true; options.ClusterBounds=true; options.ConeAxes=true;
        }
        if (frame == 128)
        {
            GPUVisibility::GetSettings().FreezeCamera=false;
            options.MeshletBounds=false; options.ClusterBounds=false; options.ConeAxes=false;
        }
        if (frame == 130)
        {
            const auto instance=GpuScene::GetInstance(GpuScene::GetEntityInstance(fixtures[0]));
            auto* resource=ModelManager::GetStaticModel(model);
            Check(instance && resource,"material override fixture available");
            if (resource) Check(resource->ApplyMeshShadingOverridePartIds(GraphicsDevice::GetDevice(),{3}),"material override rebuilds Nanite");
        }
        if (frame == 133)
        {
            Clear();
            terrainSettings.Resolution=32;
            terrain=ProceduralScene::GenerateTerrain(terrainSettings);
            if (Registry::IsAlive(terrain)) fixtures.push_back(terrain);
        }
        if (frame>=134 && frame<=138 && Registry::IsAlive(terrain))
        {
            terrainSettings.Resolution=frame%2 ? 16 : 32;
            terrainSettings.Noise.Seed+=1;
            Check(ProceduralScene::GenerateTerrain(terrainSettings,terrain)==terrain,"repeat terrain resolution and bounds edit");
        }
        if (frame == 139 && Registry::IsAlive(terrain))
        {
            Entity clone = World::CreateEntity();
            clone.Add<TransformComponent>().Add<MeshComponent>().Add<MaterialComponent>()
                .Add<ProceduralTerrainComponent>();
            clone.Get<TransformComponent>() = Entity(terrain).Get<TransformComponent>();
            clone.Get<MeshComponent>() = Entity(terrain).Get<MeshComponent>();
            clone.Get<MaterialComponent>() = Entity(terrain).Get<MaterialComponent>();
            clone.Get<ProceduralTerrainComponent>() = Entity(terrain).Get<ProceduralTerrainComponent>();
            clone.Get<TransformComponent>().Position.x += 2;
            clone.Get<TransformComponent>().IsDirty = true;
            terrainClone = clone.GetID();
            fixtures.push_back(terrainClone);
            const auto& source = Entity(terrain).Get<MeshComponent>();
            sharedTerrainHash = source.GeometryHash;
            sharedTerrainAddress = source.VertexBufferView.BufferLocation;
        }
        if (frame == 140 && Registry::IsAlive(terrainClone))
        {
            auto settings = terrainSettings;
            settings.Resolution = 16;
            settings.Noise.Seed += 123;
            Check(ProceduralScene::GenerateTerrain(settings, terrainClone) == terrainClone,
                "edit cloned procedural terrain");
        }
        if (frame == 141 && Registry::IsAlive(terrain))
            World::DestroyEntity(Entity(terrain));
        if (frame == 144)
        {
            Clear();
            Grow(1, cube);
            auto& material = Entity(fixtures[0]).Get<MaterialComponent>();
            material.ShaderClassMode = MaterialMode::Auto;
            material.UseTexture = true;
            transform.Position = {0, 0, -8};
            camera.Target = {0, 0, 0};
        }
        if (frame >= 144)
        {
            transform.Position = {0, 0, -8};
            camera.Target = {0, 0, 0};
            GPUDrivenRenderer::SetEnabled(frame >= 146);
        }
        if (frame == 148)
        {
            auto* imported = ModelManager::GetStaticModel(cube);
            Check(imported && imported->ApplyMeshShadingOverridePartIds(GraphicsDevice::GetDevice(), {0}),
                "set Auto imported model to a forward-only material part");
            GpuSceneChanges::Notify(fixtures[0], GpuSceneDirtyFlags::Mesh | GpuSceneDirtyFlags::Material);
        }
        if (frame == 150)
        {
            Clear();
            terrainSettings.Resolution = 32;
            terrain = ProceduralScene::GenerateTerrain(terrainSettings);
            Check(Registry::IsAlive(terrain), "create procedural LOD fixture");
            if (Registry::IsAlive(terrain)) fixtures.push_back(terrain);
        }
        if (frame >= 150)
        {
            GPUVisibility::GetSettings().LOD = true;
            GPUVisibility::GetSettings().ForceLOD = int((min)(frame - 150, uint64_t(4)));
        }
        if (frame == 155)
        {
            terrainSettings.Noise.Seed += 7;
            Check(ProceduralScene::GenerateTerrain(terrainSettings, terrain) == terrain,
                  "edit terrain with GPU LOD enabled");
        }
        if (frame == 157 || frame == 158)
        {
            Entity e(terrain);
            if (frame == 157) e.Add<LODComponent>();
            e.Get<LODComponent>().UseLOD = frame == 158;
            GpuSceneChanges::Notify(terrain, GpuSceneDirtyFlags::Mesh);
        }
    }
}
void AfterFrame(uint64_t frame)
{
    if (frame >= 150 && frame <= 158 && Registry::IsAlive(terrain))
    {
        const auto& mesh = *Entity(terrain).Get<MeshComponent>().NaniteMesh;
        const auto slot = GpuScene::GetEntityInstance(terrain);
        vector<uint8_t> bytes;
        const bool read = slot != UINT32_MAX && RenderCapture::ReadBuffer(GPUVisibility::ResultBuffer(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, uint64_t(slot + 1) * 16, bytes);
        Visibility::VisibleInstanceData result{};
        if (read) memcpy(&result, bytes.data() + size_t(slot) * 16, sizeof(result));
        const uint32_t expected = frame == 157 ? 0 : uint32_t((min)(frame - 150, uint64_t(4)));
        Check(read && result.SelectedLOD == expected, "procedural forced GPU LOD selects requested level");
        const uint32_t triangles = expected ? uint32_t(mesh.CpuLodIndices[expected - 1].size() / 3) : mesh.IndexCount / 3;
        WriteGraphicsLog("Procedural LOD frame=%llu level=%u sourceTriangles=%u drawnTriangles=%u\n",
                         static_cast<unsigned long long>(frame), expected, mesh.IndexCount / 3, triangles);
        Check(Stats()[8] == triangles, "procedural GPU draw executes selected LOD triangle count");
        if (expected) Check(triangles > 0 && triangles < mesh.IndexCount / 3,
                            "procedural GPU LOD reduces drawn triangles");
    }
    if (frame==113 || frame==117 || frame==121) Capture(false);
    if (frame==115 || frame==119 || frame==123) Capture(true);
    if (frame==115 || frame==119)
    {
        Check(GPUDrivenRenderer::HandlesEntity(terrain),"procedural terrain uses GPU drawing");
        const auto stats=Stats();
        Check(stats[1]>0 && stats[8]==512,"procedural leaf clusters execute all source triangles");
    }
    if (frame==123)
    {
        const uint32_t slot=GpuScene::GetEntityInstance(fixtures[0]);
        vector<uint8_t> bytes;
        const bool read=RenderCapture::ReadBuffer(GPUVisibility::ResultBuffer(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,uint64_t(slot+1)*16,bytes);
        Visibility::VisibleInstanceData result{};
        if (read) memcpy(&result,bytes.data()+size_t(slot)*16,sizeof(result));
        Check(read && result.SelectedLOD==2,"LOD View uses actual forced GPU LOD");
    }
    if (frame==124) frozenCamera=GPUVisibility::CullingViewProjection();
    if (frame==126)
    {
        const auto camera=GPUVisibility::CullingViewProjection();
        Check(memcmp(&camera,&frozenCamera,sizeof(camera))==0 && !GPUVisibility::HistoryValid(),"frozen culling camera and conservative Hi-Z disable");
        Check(!MeshShaderRenderer::DebugRecords().empty(),"viewport culling records read back on demand");
        ImDrawList overlay(ImGui::GetDrawListSharedData());
        overlay._ResetForNewFrame();
        overlay.PushTexture(ImGui::GetIO().Fonts->TexRef);
        VirtualGeometryDebug::DrawViewport(&overlay,ImVec2(0,0),ImVec2(640,360),fixtures[0]);
        overlay.PopTexture();
        Check(overlay.VtxBuffer.Size>400,"Nanite viewport bounds generate overlay geometry");
    }
    if (frame==129)
        Check(MeshShaderRenderer::DebugRecords().empty(),"debug off clears sample and stops readback");
    if (frame==132)
    {
        const auto* resource=ModelManager::GetStaticModel(model);
        const auto& asset=resource->GetMeshData(0).VirtualGeometryLODs[0];
        Check(asset && asset->Error.empty() && asset->Hierarchy.SourceMaterialPartId==3,"material edits retain drawable VirtualGeometry");
    }
    if (frame==133)
    {
        const auto* instance=GpuScene::GetInstance(GpuScene::GetEntityInstance(terrain));
        repeatedEditMeshID=instance ? instance->MeshIndex : UINT32_MAX;
        repeatedEditArena=GPUDrivenRenderer::GeometryArenaBytes();
        repeatedEditMeshSlots=GpuScene::Statistics().Meshes;
    }
    if (frame>=134 && frame<=138)
    {
        const auto* instance=GpuScene::GetInstance(GpuScene::GetEntityInstance(terrain));
        Check(instance && instance->MeshIndex==repeatedEditMeshID && GpuScene::Statistics().Meshes==repeatedEditMeshSlots,
            "repeated terrain edits keep stable mesh ID and table size");
        Check(GPUDrivenRenderer::GeometryArenaBytes()==repeatedEditArena,"repeated terrain edits reuse reserved GPU geometry arena");
        const auto& mesh=Entity(terrain).Get<MeshComponent>();
        uint64_t expectedUpload = uint64_t(mesh.VertexCount) * 52;
        for (const auto& indices : mesh.NaniteMesh->CpuLodIndices) expectedUpload += indices.size() * sizeof(uint32_t);
        Check(GPUDrivenRenderer::GeometryUploadBytes() <= expectedUpload,
            "terrain edit uploads shared vertices and LOD indices without historical aliases");
    }
    if (frame == 139)
    {
        const auto* original = GpuScene::GetInstance(GpuScene::GetEntityInstance(terrain));
        const auto* clone = GpuScene::GetInstance(GpuScene::GetEntityInstance(terrainClone));
        Check(original && clone && original->MeshIndex == clone->MeshIndex,
            "cloned terrain shares one GPU mesh registration");
        if (original) sharedTerrainMesh = original->MeshIndex;
    }
    if (frame == 140)
    {
        const auto& original = Entity(terrain).Get<MeshComponent>();
        const auto& cloneMesh = Entity(terrainClone).Get<MeshComponent>();
        const auto* clone = GpuScene::GetInstance(GpuScene::GetEntityInstance(terrainClone));
        Check(original.GeometryHash == sharedTerrainHash &&
            original.VertexBufferView.BufferLocation == sharedTerrainAddress &&
            original.NaniteMesh != cloneMesh.NaniteMesh &&
            cloneMesh.VertexBufferView.BufferLocation != sharedTerrainAddress &&
            clone && clone->MeshIndex != sharedTerrainMesh,
            "editing a shared terrain separates source and GPU mesh without changing original");
        Check(GPUDrivenRenderer::HandlesEntity(terrain) && GPUDrivenRenderer::HandlesEntity(terrainClone),
            "original and edited clone both remain GPU drawable");
    }
    if (frame == 142)
    {
        Check(GPUDrivenRenderer::HandlesEntity(terrainClone) && Stats()[8] == 512,
            "edited terrain clone remains drawable after original deletion");
    }
    if (frame == 145) Capture(false);
    if (frame == 147)
    {
        Check(GPUDrivenRenderer::HandlesEntity(fixtures[0]),
            "default Auto imported model automatically uses GPU drawing");
        Check(!GpuScene::CpuOpaqueStaticEntities().empty(),
            "Auto imported model retains traditional outline pass");
        Capture(true);
        auto& material = Entity(fixtures[0]).Get<MaterialComponent>();
        const auto originalNormalMap = material.NormalMapID;
        material.NormalMapID = 0;
        Check(!GPUDrivenRenderer::SupportsEntity(fixtures[0]),
            "Auto normal-mapped models retain traditional rendering");
        material.NormalMapID = originalNormalMap;
    }
    if (frame == 149)
        Check(!GPUDrivenRenderer::SupportsEntity(fixtures[0]) && !GPUDrivenRenderer::HandlesEntity(fixtures[0]),
            "Auto forward-only material parts retain traditional rendering");
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
                    if (abs(motion) > 1e-5f)
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
        Check(read && samples > 100 && abs(observed - expected) < 0.00002,
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
    if (frame == 82 || frame == 85 || frame == 88 || frame == 91 || frame == 100)
        Capture(false);
    if (frame == 83 || frame == 86 || frame == 89 || frame == 90 || frame == 92 || frame == 108)
        // Frame 91 changes the camera; compare opaque fallback without its motion history.
        Capture(true, frame != 92);
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
            vector<uint8_t> bytes;
            bool read = RenderCapture::ReadBuffer(GPUVisibility::ResultBuffer(),
                                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                  uint64_t(slot + 1) * 16, bytes);
            Visibility::VisibleInstanceData selected{};
            if (read)
                memcpy(&selected, bytes.data() + size_t(slot) * 16, 16);
            Check(read && selected.SelectedLOD == (frame - 62) / 2, "correct GPU selected LOD");
            if (read && selected.SelectedMeshID < MeshletManager::Ranges().size())
                Check(stats[1] == MeshletManager::Ranges()[selected.SelectedMeshID].MeshletCount,
                      "correct selected meshlet range");
        }
        if (frame == 81)
            Check(stats[1] == 0, "missing meshlet asset draws with traditional fallback");
        if (frame == 83)
        {
            const auto *instance = GpuScene::GetInstance(GpuScene::GetEntityInstance(fixtures[0]));
            vector<uint8_t> bytes;
            bool read = instance && RenderCapture::ReadBuffer(
                                        MeshletManager::RangeBuffer().Resource(),
                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                        uint64_t(instance->MeshIndex + 1) * sizeof(Meshlets::Range), bytes);
            Meshlets::Range range{};
            if (read)
                memcpy(&range, bytes.data() + size_t(instance->MeshIndex) * sizeof(range),
                            sizeof(range));
            Check(read && range.MeshletCount == 0 && range.ClusterCount == 0 && stats[1] == 0,
                  "new legacy asset uploads zero GPU range and draws fallback");
        }
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
        if (frame <= 103)
        {
            traditionalFrame += RenderProfiler::GetLatest().GpuRenderMs;
            traditionalSubmission += GPUDrivenRenderer::SubmissionMilliseconds();
        }
        else
        {
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
    Check(frames >= 159, "complete meshlet sequence");
    WriteGraphicsLog("Meshlet performance samples traditional=%u mesh=%u opaqueTraditionalMs=%.5f "
                     "opaqueMeshMs=%.5f meshComputeMs=%.5f\n",
                     traditionalSamples, meshSamples,
                     traditionalSamples ? traditionalOpaque / traditionalSamples : 0,
                     meshSamples ? meshOpaque / meshSamples : 0, meshSamples ? meshCompute / meshSamples : 0);
    WriteGraphicsLog("Meshlet performance totalGPU traditionalMs=%.5f meshMs=%.5f CPU submission "
                     "traditionalMs=%.5f meshMs=%.5f\n",
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
