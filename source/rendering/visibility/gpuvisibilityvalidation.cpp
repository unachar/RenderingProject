#include "pch.h"
#include "virtualgeometry/virtualgeometrytraversal.h"
#include "gpuvisibilityvalidation.h"
#include "camera.h"
#include "daynightcycle.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "gpudriven/indirectcommand.h"
#include "gpuscene/gpuscene.h"
#include "gpuscene/gpuscenechanges.h"
#include "gpuvisibility.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "modelmanager.h"
#include "occlusionculling.h"
#include "raytracing.h"
#include "rendercapture.h"
#include "renderconfiguration.h"
#include "renderersettings.h"
#include "renderprofiler.h"
#include "rendertargets.h"
#include "world.h"
#include <filesystem>
#include <fstream>

namespace
{
    vector<EntityID> fixtures;
    EntityID wall = UINT32_MAX;
    int meshID = -1, wallMeshID = -1;
    uint32_t failures = 0;
    uint64_t frameCount = 0;
    vector<RenderCapture::TextureData> reference;
    array<string, 8> meshPaths;
    string fixtureDirectory;
    ofstream measurements;
    vector<EntityID> buildings;

    void Check(bool success, const char* description)
    {
        WriteGraphicsLog("GPU Visibility validation: %s %s\n", success ? "PASS" : "FAIL", description);
        if (!success)
        {
            ++failures;
        }
    }

    void WriteGrid(const string& path, uint32_t subdivisions, uint32_t sections = 1)
    {
        ofstream file(path);
        if (sections > 1)
        {
            const auto materialPath = filesystem::path(path).replace_extension(".mtl");
            ofstream materials(materialPath);
            file << "mtllib " << materialPath.filename().string() << '\n';
            for (uint32_t section = 0; section < sections; ++section)
            {
                materials << "newmtl section" << section << "\nKd " << (0.2f + 0.05f * section) << " 1 1\n";
            }
        }
        const uint32_t side = subdivisions + 1;
        file << "vn 0 0 -1\n";
        for (uint32_t y = 0; y < side; ++y)
        {
            for (uint32_t x = 0; x < side; ++x)
            {
                const float fx = float(x) / float(subdivisions) * 2 - 1, fy = float(y) / float(subdivisions) * 2 - 1;
                file << "v " << fx << ' ' << fy << ' ' << 0.05f * sinf(fx * 3.14159265f) * sinf(fy * 3.14159265f)
                     << '\n';
            }
        }
        for (uint32_t section = 0; section < sections; ++section)
        {
            file << "g section" << section << "\nusemtl section" << section << '\n';
            for (uint32_t y = 0; y < subdivisions; ++y)
            {
                for (uint32_t x = 0; x < subdivisions; ++x)
                {
                    const uint32_t a = y * side + x + 1, b = a + side;
                    file << "f " << a << "//1 " << b << "//1 " << b + 1 << "//1\n";
                    file << "f " << a << "//1 " << b + 1 << "//1 " << a + 1 << "//1\n";
                }
            }
        }
    }

    EntityID Create(uint32_t index, bool isWall = false)
    {
        Entity entity = World::CreateEntity();
        if (!entity.IsValid())
        {
            Check(false, "fixture entity capacity");
            return UINT32_MAX;
        }
        entity.Add<TransformComponent>().Add<StaticModelComponent>().Add<MaterialComponent>().Add<LODComponent>();
        entity.Get<StaticModelComponent>().ModelId = isWall ? wallMeshID : meshID;
        auto& transform = entity.Get<TransformComponent>();
        transform.Position =
            isWall ? XMFLOAT3(0, 0, 0)
                   : XMFLOAT3(float(index % 100) * 0.08f - 3.96f, float((index / 100) % 100) * 0.08f - 3.96f, 5);
        transform.Scale = isWall ? XMFLOAT3(20, 20, 1) : XMFLOAT3(0.035f, 0.035f, 0.035f);
        auto& material = entity.Get<MaterialComponent>();
        material.ShaderClassMode = MaterialMode::Manual;
        material.ShaderClass = ShaderClass::PBR;
        material.UseTexture = false;
        material.IsTransparent = false;
        material.Alpha = 1;
        auto& lod = entity.Get<LODComponent>();
        lod.UseLOD = !isWall;
        lod.UseAssetSettings = false;
        lod.ScreenLOD.MeshPaths = meshPaths;
        return entity.GetID();
    }

    void Grow(uint32_t count)
    {
        while (fixtures.size() < count)
        {
            fixtures.push_back(Create(static_cast<uint32_t>(fixtures.size())));
        }
    }

    void Clear()
    {
        for (auto entity : fixtures)
        {
            if (Registry::IsAlive(entity))
            {
                World::DestroyEntity(Entity(entity));
            }
        }
        fixtures.clear();
        for (auto entity : buildings)
        {
            if (Registry::IsAlive(entity))
            {
                World::DestroyEntity(Entity(entity));
            }
        }
        buildings.clear();
        if (Registry::IsAlive(wall))
        {
            World::DestroyEntity(Entity(wall));
        }
        wall = UINT32_MAX;
    }

    void Capture(bool compare)
    {
        vector<RenderCapture::TextureData> image(2);
        bool read = RenderCapture::ReadTexture(RenderTargets::GetGBufferResource(GBufferType::BASE_COLOR), image[0]);
        read &= RenderCapture::ReadTexture(RenderTargets::GetGBufferResource(GBufferType::DEPTH), image[1]);
        Check(read, "capture opaque color/depth");
        if (!read)
        {
            return;
        }
        if (!compare)
        {
            reference = move(image);
            return;
        }
        uint64_t differences = 0, covered = 0;
        bool dimensions = reference.size() == image.size();
        for (size_t i = 0; dimensions && i < image.size(); ++i)
        {
            const auto& a = reference[i];
            const auto& b = image[i];
            dimensions &= a.Width == b.Width && a.Height == b.Height && a.Format == b.Format;
            if (!dimensions)
            {
                break;
            }
            for (uint32_t y = 0; y < a.Height; ++y)
            {
                for (uint32_t x = 0; x < a.Width; ++x)
                {
                    const auto* first = a.Bytes.data() + y * a.RowPitch + x * 4;
                    const auto* second = b.Bytes.data() + y * b.RowPitch + x * 4;
                    if (i == 1)
                    {
                        float da, db;
                        memcpy(&da, first, 4);
                        memcpy(&db, second, 4);
                        differences += abs(da - db) > 0.00001f;
                        covered += da > 0 && da <= 1.0f;
                    }
                    else
                    {
                        differences += memcmp(first, second, 4) != 0;
                    }
                }
            }
        }
        WriteGraphicsLog("GPU Visibility image comparison: differences=%llu covered=%llu\n", differences, covered);
        Check(dimensions && differences == 0 && covered > 0, "occlusion OFF/ON image equivalence");
    }

    Visibility::Statistics ReadStats()
    {
        vector<uint8_t> bytes;
        Visibility::Statistics stats;
        const bool read = RenderCapture::ReadBuffer(
            GPUVisibility::StatisticsBuffer(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, sizeof(stats), bytes);
        Check(read, "read validation-only visibility counters");
        if (read)
        {
            memcpy(&stats, bytes.data(), sizeof(stats));
        }
        Check(stats.FinalVisible + stats.Occluded == stats.FrustumVisible &&
                  stats.FrustumVisible + stats.FrustumCulled == stats.Total,
              "visibility counter conservation");
        return stats;
    }

    void ValidateSelectedCommands(uint32_t expectedLOD)
    {
        vector<uint8_t> counter, visible, commands, geometry;
        bool valid = RenderCapture::ReadBuffer(GPUDrivenRenderer::CountBuffer(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                                               4, counter);
        uint32_t count = 0;
        if (valid)
        {
            memcpy(&count, counter.data(), 4);
        }
        valid &= count > 0 && count <= GPUDrivenRenderer::CommandCapacity();
        if (valid)
        {
            valid &= RenderCapture::ReadBuffer(GPUDrivenRenderer::VisibleBuffer(),
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, uint64_t(count) * 16,
                                               visible);
            valid &=
                RenderCapture::ReadBuffer(GPUDrivenRenderer::CommandBuffer(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                                          uint64_t(count) * sizeof(GPUIndirectDrawCommand), commands);
            valid &= RenderCapture::ReadBuffer(
                GPUDrivenRenderer::GeometryBuffer(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                uint64_t(GpuScene::Statistics().Meshes) * sizeof(GPUDrivenGeometryData), geometry);
        }
        for (uint32_t i = 0; valid && i < count; ++i)
        {
            Visibility::VisibleInstanceData record{};
            GPUIndirectDrawCommand command{};
            GPUDrivenGeometryData mesh{};
            memcpy(&record, visible.data() + size_t(i) * 16, 16);
            memcpy(&command, commands.data() + size_t(i) * sizeof(command), sizeof(command));
            if (record.SelectedMeshID >= GpuScene::Statistics().Meshes || record.SelectedLOD != expectedLOD)
            {
                WriteGraphicsLog("GPU LOD mismatch: expected=%u selected=%u mesh=%u meshCount=%u\n", expectedLOD,
                                 record.SelectedLOD, record.SelectedMeshID, GpuScene::Statistics().Meshes);
                valid = false;
                break;
            }
            memcpy(&mesh, geometry.data() + size_t(record.SelectedMeshID) * sizeof(mesh), sizeof(mesh));
            valid &= mesh.Ready != 0 && command.InstanceID == record.InstanceID &&
                     command.Draw.IndexCountPerInstance == mesh.IndexCount &&
                     command.Draw.StartIndexLocation == mesh.StartIndex &&
                     command.Draw.BaseVertexLocation == mesh.BaseVertex && command.Draw.InstanceCount == 1 &&
                     command.Draw.StartInstanceLocation == 0;
        }
        Check(valid, "Indirect commands use exact selected LOD mesh geometry");
    }

    void SetHysteresisCamera(float screenSize)
    {
        const auto* mesh = ModelManager::GetStaticModel(meshID);
        const auto extents = mesh->GetAabbExtents();
        const float radius = sqrtf(extents.x * extents.x + extents.y * extents.y + extents.z * extents.z);
        const auto camera = Camera::GetCameraEntity();
        auto& data = ComponentManager::GetComponentUnchecked<CameraComponent>(camera);
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(camera);
        const float focal = data.ProjectionMatrix._22;
        transform.Position = {0, 0, -(radius + radius * focal / screenSize)};
        transform.IsDirty = true;
        data.Target = {0, 0, 0};
    }
} // 名前空間

bool GPUVisibilityValidation::Enabled()
{
    char value[8]{};
    return GetEnvironmentVariableA("DX12_GPU_VISIBILITY_STRESS_TEST", value, sizeof(value)) && atoi(value) != 0;
}

void GPUVisibilityValidation::BeforeFrame(uint64_t frame)
{
    // このテスト群はメッシュ全体の離散 LOD を検証する。ネイティブの連続階層選択は
    // 別途検証し、意図的にこの規則を迂回する。
    VirtualGeometryTraversal::GetSettings().Enabled = false;
    frameCount = frame + 1;
    if (!frame)
    {
        vector<EntityID> remove;
        for (uint32_t i = 0; i < Registry::GetEntities().size(); ++i)
        {
            if (Registry::IsAlive(i) && !ComponentManager::HasComponent<CameraComponent>(i))
            {
                remove.push_back(i);
            }
        }
        for (auto entity : remove)
        {
            World::DestroyEntity(Entity(entity));
        }
        filesystem::create_directories("build/visibility");
        // 実行ごとに新しい入力を用意し、過去の派生ジオメトリや通常シーンのキャッシュを混ぜない。
        fixtureDirectory = "build/visibility/fixtures-" + to_string(GetCurrentProcessId()) + "-" +
                           to_string(GetTickCount64());
        filesystem::create_directories(fixtureDirectory);
        for (uint32_t level = 0; level < 5; ++level)
        {
            const auto path = fixtureDirectory + "/grid-lod" + to_string(level) + ".obj";
            WriteGrid(path, 16u >> level);
            if (level)
            {
                meshPaths[level] = path;
            }
        }
        meshID = ModelManager::LoadStaticModel((fixtureDirectory + "/grid-lod0.obj").c_str(), false);
        WriteGrid(fixtureDirectory + "/wall.obj", 1);
        wallMeshID = ModelManager::LoadStaticModel((fixtureDirectory + "/wall.obj").c_str(), false);
        Check(meshID >= 0 && wallMeshID >= 0, "import five-level fixture and wall");
        for (uint32_t level = 0; level < 5; ++level)
        {
            const string path = fixtureDirectory + "/grid-lod" + to_string(level) + ".obj";
            const auto* fixture = ModelManager::GetStaticModel(ModelManager::LoadStaticModel(path.c_str(), false));
            if (fixture)
            {
                const auto center = fixture->GetAabbCenter();
                const auto extents = fixture->GetAabbExtents();
                WriteGraphicsLog("GPU LOD fixture: level=%u meshes=%u center=(%g,%g,%g) extents=(%g,%g,%g)\n",
                                 level, fixture->GetMeshCount(), center.x, center.y, center.z,
                                 extents.x, extents.y, extents.z);
            }
        }
        Grow(1000);
        World::SetVSyncEnabled(false);
        measurements.open("build/visibility/comparison.csv");
        measurements << "frame,mode,total,frustum,occluded,visible,triangles,index_vertices,frame_gpu_ms,vs_"
                        "invocations,ps_invocations,cull_gpu_ms,lod_gpu_ms,opaque_gpu_ms,hiz_gpu_ms\n";
    }
    RenderConfiguration::SetRenderMode(RenderMode::DEFERRED);
    RendererSettings::SetComputeGBufferEnabled(false);
    RendererSettings::SetMeshShadersEnabled(false);
    RendererSettings::SetTwoPhaseOcclusionEnabled(false);
    RendererSettings::SetUpscaleMode(UpscaleMode::Bilateral);
    Raytracing::GetSettings().Enabled = false;
    DayNightCycle::GetSettings().Running = false;
    GPUDrivenRenderer::SetEnabled(true);
    auto& settings = GPUVisibility::GetSettings();
    settings.Frustum = true;
    settings.Occlusion = frame == 0 || frame >= 10;
    settings.LOD = frame >= 52;
    settings.DebugReadback = true;
    settings.DebugMode = 0;
    settings.ForceLOD = -1;
    const auto cameraID = Camera::GetCameraEntity();
    auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(cameraID);
    auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(cameraID);
    transform.Position = {0, 0, -12};
    transform.IsDirty = true;
    camera.Target = {0, 0, 5};
    camera.AllowUserControl = false;
    camera.LockOnTarget = UINT32_MAX;
    if (frame == 20)
    {
        Grow(10000);
        wall = Create(0, true);
    }
    if (frame >= 20 && frame < 30)
    {
        settings.Occlusion = false;
    }
    if (frame >= 40 && frame < 48)
    {
        transform.Position.x = frame % 2 ? 25.0f : -25.0f;
        camera.Target = {0, 0, 0};
    }
    if (frame == 48)
    {
        transform.Position = {100, 50, -100};
        camera.Target = {0, 0, 0};
    }
    if (frame == 52 && Registry::IsAlive(wall))
    {
        World::DestroyEntity(Entity(wall));
        wall = UINT32_MAX;
    }
    if (frame >= 52 && frame < 72)
    {
        settings.ForceLOD = int((frame - 52) / 4);
    }
    if (frame == 72)
    {
        Clear();
        Grow(1);
        auto& object = Entity(fixtures[0]).Get<TransformComponent>();
        object.Position = {0, 0, 0};
        object.Scale = {1, 1, 1};
        object.IsDirty = true;
        GpuSceneChanges::Notify(fixtures[0]);
    }
    if (frame >= 72 && frame < 100)
    {
        settings.Occlusion = false;
        if (frame < 84)
        {
            settings.ForceLOD = 0;
        }
        float size = 0.34f;
        if (frame < 87)
        {
            size = 0.28f;
        }
        else if (frame < 91)
        {
            size = 0.26f;
        }
        else if (frame < 95)
        {
            size = 0.32f;
        }
        SetHysteresisCamera(size);
    }
    if (frame == 76)
    {
        Entity(fixtures[0]).Get<LODComponent>().ScreenLOD.Count = 8;
        GpuSceneChanges::Notify(fixtures[0]);
    }
    if (frame >= 76 && frame < 80)
    {
        settings.ForceLOD = 7; // LOD5～7 がないため、LOD4 を使う。
    }
    if (frame == 100 || frame == 125)
    {
        Clear();
        const uint32_t sections = frame == 100 ? 5 : 10;
        const auto path = fixtureDirectory + "/city-" + to_string(sections) + ".obj";
        WriteGrid(path, 2, sections);
        meshID = ModelManager::LoadStaticModel(path.c_str(), false);
        auto* city = ModelManager::GetStaticModel(meshID);
        Check(city && city->LoadAssimpModel(path.c_str(), GraphicsDevice::GetDevice(), false) &&
                  city->GetMeshCount() == sections,
              "import independent city mesh sections");
        // 複数区画のフィクスチャでも、既存 ECS の16384エンティティ制限を守る。
        meshPaths.fill("");
        Grow(10000);
        for (uint32_t building = 0; building < 12; ++building)
        {
            const auto id = Create(building, true);
            buildings.push_back(id);
            auto& object = Entity(id).Get<TransformComponent>();
            object.Position = {float(building % 4) * 1.8f - 2.7f, float(building / 4) * 2.4f - 2.4f, 0};
            object.Scale = {0.5f, 0.8f, 1};
            object.IsDirty = true;
            GpuSceneChanges::Notify(id);
        }
        for (auto entity : fixtures)
        {
            Entity(entity).Get<LODComponent>().ScreenLOD.Thresholds = {0.3f, 0.15f, 0.05f, 0.01f, 0, 0, 0, 0};
            GpuSceneChanges::Notify(entity);
        }
    }
    if (frame >= 100 && frame < 108)
    {
        settings.Occlusion = false;
        settings.LOD = false;
    }
    else if (frame >= 108 && frame < 116)
    {
        settings.Occlusion = true;
        settings.LOD = false;
    }
    else if (frame >= 116)
    {
        settings.Occlusion = true;
        settings.LOD = true;
    }
    if (frame == 126)
    {
        Check(GraphicsDevice::WaitForGpuIdle(), "wait for resize resource retirement");
        RenderTargets::ResizeScene(801, 451);
    }
    if (frame == 135 || frame == 136)
    {
        OcclusionCulling::SetDebugPreview(frame == 135 ? 3u : 0u, true);
    }
}

void GPUVisibilityValidation::AfterFrame(uint64_t frame)
{
    if (frame == 0 || !GPUDrivenRenderer::IsActive())
    {
        Check(GPUDrivenRenderer::IsActive(), "production GPU Driven path active");
    }
    if (frame == 0)
    {
        Check(!GPUVisibility::HistoryValid() && ReadStats().Tested == 0, "first frame does not sample previous Hi-Z");
    }
    if (frame == 126)
    {
        Check(!GPUVisibility::HistoryValid(), "resize invalidates previous Hi-Z");
    }
    if (frame == 6 || frame == 26)
    {
        Capture(false);
    }
    if (frame == 18 || frame == 38)
    {
        Capture(true);
    }
    if (frame == 38)
    {
        const auto stats = ReadStats();
        Check(stats.Total == 10001 && stats.Occluded >= 9500 && stats.FinalVisible < 100,
              "wall occludes most of 10000 objects");
    }
    if (frame >= 40 && frame <= 49)
    {
        const auto stats = ReadStats();
        Check(!GPUVisibility::HistoryValid() && stats.Tested == 0, "camera motion/cut never tests stale Hi-Z");
        if (frame == 48)
        {
            Check(GPUVisibility::CameraCut(), "teleport sets camera cut");
        }
    }
    if (frame >= 55 && frame < 72 && (frame - 55) % 4 == 0)
    {
        const auto stats = ReadStats();
        const auto level = uint32_t((frame - 52) / 4);
        Check(stats.FinalVisible > 0 && stats.LODCounts[level] == stats.FinalVisible,
              "five selected LOD meshes reach Indirect drawing");
        ValidateSelectedCommands(level);
    }
    if (frame == 79)
    {
        ValidateSelectedCommands(4);
    }
    if (frame == 86 || frame == 90 || frame == 94 || frame == 98)
    {
        const auto stats = ReadStats();
        const uint32_t expected = frame == 90 || frame == 94 ? 1 : 0;
        Check(stats.FinalVisible == 1 && stats.LODCounts[expected] == 1,
              "LOD hysteresis retains and crosses correct boundary");
    }
    if (frame == 106 || frame == 114 || frame == 122 || frame == 137)
    {
        const auto stats = ReadStats();
        Check(stats.Total >= (frame < 125 ? 50000u : 100000u), "large scene instance capacity");
        const auto& profiler = RenderProfiler::GetLatest();
        double cull = 0, lod = 0, opaque = 0, hiz = 0;
        for (const auto& pass : profiler.Passes)
        {
            if (pass.Name == "GPU Scene Culling" || pass.Name == "Occlusion Culling")
            {
                cull += pass.GpuMs;
            }
            if (pass.Name == "LOD Selection")
            {
                lod += pass.GpuMs;
            }
            if (pass.Name == "Opaque ExecuteIndirect")
            {
                opaque += pass.GpuMs;
            }
            if (pass.Name == "Hi-Z Build")
            {
                hiz += pass.GpuMs;
            }
        }
        const char* mode = "Frustum+Occlusion+LOD";
        if (frame == 106)
        {
            mode = "Frustum";
        }
        else if (frame == 114)
        {
            mode = "Frustum+Occlusion";
        }
        measurements << frame << ',' << mode << ',' << stats.Total << ',' << stats.FrustumVisible << ','
                     << stats.Occluded << ',' << stats.FinalVisible << ',' << stats.Triangles << ','
                     << stats.IndexVertices << ',' << profiler.GpuRenderMs << ',' << profiler.Pipeline.VSInvocations
                     << ',' << profiler.Pipeline.PSInvocations << ',' << cull << ',' << lod << ',' << opaque << ','
                     << hiz << '\n';
        WriteGraphicsLog("GPU Visibility stress: %s total=%u final=%u occluded=%u triangles=%u frame=%.4f culling=%.4f "
                         "opaque=%.4f ms\n",
                         mode, stats.Total, stats.FinalVisible, stats.Occluded, stats.Triangles, profiler.GpuRenderMs,
                         cull, opaque);
    }
    if (frame == 138)
    {
        vector<RenderCapture::TextureData> mips(OcclusionCulling::GetMipCount());
        bool valid = true;
        for (uint32_t level = 0; level < mips.size(); ++level)
        {
            valid &= RenderCapture::ReadTextureMip(OcclusionCulling::GetPreviousResource(), mips[level], level,
                                                   OcclusionCulling::ReadState());
        }
        for (uint32_t level = 1; valid && level < mips.size(); ++level)
        {
            const auto& source = mips[level - 1];
            const auto& destination = mips[level];
            for (uint32_t y = 0; y < destination.Height && valid; ++y)
            {
                for (uint32_t x = 0; x < destination.Width; ++x)
                {
                    const auto xr = Visibility::ReductionRange(x, source.Width, destination.Width);
                    const auto yr = Visibility::ReductionRange(y, source.Height, destination.Height);
                    float minimum = 1;
                    float actual = 0;
                    for (uint32_t sy = yr.first; sy < yr.second; ++sy)
                    {
                        for (uint32_t sx = xr.first; sx < xr.second; ++sx)
                        {
                            float depth;
                            memcpy(&depth, source.Bytes.data() + sy * source.RowPitch + sx * 4, 4);
                            minimum = (min)(minimum, depth);
                        }
                    }
                    memcpy(&actual, destination.Bytes.data() + y * destination.RowPitch + x * 4, 4);
                    if (actual != minimum)
                    {
                        valid = false;
                        break;
                    }
                }
            }
        }
        Check(valid && mips[0].Width == 801 && mips[0].Height == 451,
              "GPU odd-resolution pyramid matches CPU min reduction at every mip");
    }
}

bool GPUVisibilityValidation::Finish()
{
    GraphicsDevice::WaitForGpuIdle();
    Check(frameCount >= 140, "complete visibility stress sequence");
    failures += RenderCapture::ReportGraphicsErrors();
    measurements.close();
    WriteGraphicsLog("GPU Visibility validation failures = %u\n", failures);
    return failures == 0;
}
