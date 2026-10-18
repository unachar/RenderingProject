#include "pch.h"
#include "visibility/gpuvisibility.h"
#include "visibility/gpuvisibilityvalidation.h"
#include "gpudrivenvalidation.h"
#include "gpudrivenrenderer.h"
#include "indirectcommand.h"
#include "gpuscene/gpuscene.h"
#include "gpuscene/gpuscenechanges.h"
#include "world.h"
#include "camera.h"
#include "modelmanager.h"
#include "renderconfiguration.h"
#include "renderersettings.h"
#include "rendercapture.h"
#include "rendertargets.h"
#include "renderprofiler.h"
#include "graphicslog.h"
#include "graphicsdevice.h"
#include "raytracing.h"
#include "texturemanager.h"
#include "daynightcycle.h"
#include <fstream>
#include <filesystem>
#include <unordered_set>

namespace
{
vector<EntityID> fixtures;
int modelID = -1;
uint32_t failures = 0;
uint64_t frames = 0;
vector<RenderCapture::TextureData> reference;
vector<double> cpuTimes, gpuTimes;

bool GpuValidation()
{
    char value[8]{};
    return GetEnvironmentVariableA("DX12_GPU_BASED_VALIDATION", value, sizeof(value)) && atoi(value) != 0;
}

void Check(bool result, const char* name)
{
    WriteGraphicsLog("GPU Driven validation: %s %s\n", result ? "PASS" : "FAIL", name);
    if (!result)
    {
        ++failures;
    }
}

EntityID Create(size_t index)
{
    Entity entity = World::CreateEntity();
    if (!entity.IsValid())
    {
        Check(false, "fixture capacity");
        return g_kINVALID_ENTITY;
    }
    entity.Add<TransformComponent>().Add<StaticModelComponent>().Add<MaterialComponent>();
    entity.Add<ShaderComponent>().Add<NameComponent>().Add<ProceduralMotionComponent>();
    entity.Get<NameComponent>().Name = "GPU fixture";
    entity.Get<ProceduralMotionComponent>().Enabled = false;
    entity.Get<StaticModelComponent>().ModelId = modelID;
    auto& transform = entity.Get<TransformComponent>();
    transform.Position = {float(index % 100) * 0.08f - 4, float((index / 100) % 100) * 0.08f - 4,
                          float(index / 10000) * 0.2f};
    transform.Scale = {0.035f, 0.035f, 0.035f};
    auto& material = entity.Get<MaterialComponent>();
    material.ShaderClassMode = MaterialMode::Manual;
    material.ShaderClass = ShaderClass::PBR;
    material.UseTexture = true;
    material.TextureID = index % 2 ? TextureManager::GetDefaultTextureIndex() : TextureManager::GetErrorTextureIndex();
    material.Metallic = float(index % 5) * 0.2f;
    material.Roughness = 0.2f + float(index % 4) * 0.15f;
    return entity.GetID();
}

void Grow(size_t count)
{
    while (fixtures.size() < count)
    {
        fixtures.push_back(Create(fixtures.size()));
    }
}

void Capture(bool compare)
{
    vector<RenderCapture::TextureData> images(5);
    bool read = true;
    for (uint32_t i = 0; i < 5; ++i)
    {
        read &= RenderCapture::ReadTexture(RenderTargets::GetGBufferResource(static_cast<GBufferType>(i)), images[i]);
    }
    Check(read, "capture GBuffer");
    if (!read)
    {
        return;
    }
    if (!compare)
    {
        reference = move(images);
        return;
    }
    bool match = reference.size() == images.size();
    uint64_t compared = 0, different = 0, covered = 0;
    for (size_t i = 0; i < images.size() && match; ++i)
    {
        const auto& a = reference[i];
        const auto& b = images[i];
        match &= a.Width == b.Width && a.Height == b.Height && a.Format == b.Format;
        if (!match)
        {
            break;
        }
        const uint32_t pixelBytes = i == 3 ? 8 : 4;
        for (uint32_t y = 0; y < a.Height; ++y)
        {
            for (uint32_t x = 0; x < a.Width; ++x)
            {
                const auto* pa = a.Bytes.data() + y * a.RowPitch + x * pixelBytes;
                const auto* pb = b.Bytes.data() + y * b.RowPitch + x * pixelBytes;
                if (i == 2)
                {
                    float da, db;
                    memcpy(&da, pa, 4);
                    memcpy(&db, pb, 4);
                    different += abs(da - db) > 0.00001f;
                    covered += da < 0.999f && da > 0.0f;
                }
                else
                {
                    different += memcmp(pa, pb, pixelBytes) != 0;
                }
                ++compared;
            }
        }
    }
    WriteGraphicsLog("GPU Driven GBuffer comparison: different=%llu pixels=%llu covered=%llu\n", different, compared,
                     covered);
    Check(match && different == 0 && covered > 0, "CPU/GPU transform/material/texture/normal GBuffer match");
}

void ValidateCommands()
{
    const auto& stats = GpuScene::Statistics();
    vector<uint8_t> counts, visibility, commands, geometry;
    bool read =
        RenderCapture::ReadBuffer(GPUDrivenRenderer::CountBuffer(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, 4, counts);
    read &= RenderCapture::ReadBuffer(GpuScene::VisibilityBuffer(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                      uint64_t(stats.InstanceSlots) * 4, visibility);
    read &=
        RenderCapture::ReadBuffer(GPUDrivenRenderer::GeometryBuffer(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                  uint64_t(stats.Meshes) * sizeof(GPUDrivenGeometryData), geometry);
    Check(read, "read debug count/visibility/geometry");
    if (!read)
    {
        return;
    }
    uint32_t count = 0;
    memcpy(&count, counts.data(), 4);
    unordered_set<uint32_t> expected;
    const auto* flags = reinterpret_cast<const uint32_t*>(visibility.data());
    for (EntityID entity : fixtures)
    {
        for (auto id : GpuScene::GetEntityInstances(entity))
        {
            const auto* instance = GpuScene::GetInstance(id);
            if (instance && (instance->Flags & 5u) == 5u && flags[id])
            {
                expected.insert(id);
            }
        }
    }
    Check(count == expected.size() && count <= GPUDrivenRenderer::CommandCapacity(),
          "GPU count equals compact visible instances without overflow");
    if (count > GPUDrivenRenderer::CommandCapacity())
    {
        return;
    }
    if (count)
    {
        read = RenderCapture::ReadBuffer(GPUDrivenRenderer::CommandBuffer(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                                         uint64_t(count) * sizeof(GPUIndirectDrawCommand), commands);
    }
    bool valid = read;
    const auto* meshes = reinterpret_cast<const GPUDrivenGeometryData*>(geometry.data());
    for (uint32_t i = 0; i < count && valid; ++i)
    {
        GPUIndirectDrawCommand command{};
        memcpy(&command, commands.data() + size_t(i) * sizeof(command), sizeof(command));
        const auto* instance = GpuScene::GetInstance(command.InstanceID);
        valid &= instance && expected.erase(command.InstanceID) == 1;
        if (!valid)
        {
            break;
        }
        const auto& mesh = meshes[instance->MeshIndex];
        valid &= command.Draw.IndexCountPerInstance == mesh.IndexCount &&
                 command.Draw.StartIndexLocation == mesh.StartIndex &&
                 command.Draw.BaseVertexLocation == mesh.BaseVertex && command.Draw.InstanceCount == 1 &&
                 command.Draw.StartInstanceLocation == 0;
    }
    Check(valid && expected.empty(), "GPU commands have unique live Instance IDs and exact mesh arguments");
    Check(GPUDrivenRenderer::ExecuteCalls() == 1, "Opaque submission uses one ExecuteIndirect");
    WriteGraphicsLog("GPU Driven frame: fixtures=%zu visible=%u commands=%u capacity=%u submission=%.4f ms\n",
                     fixtures.size(), count, count, GPUDrivenRenderer::CommandCapacity(),
                     GPUDrivenRenderer::SubmissionMilliseconds());
}
} // namespace

bool GPUDrivenValidation::Enabled()
{
    char value[8]{};
    return GetEnvironmentVariableA("DX12_GPU_DRIVEN_STRESS_TEST", value, sizeof(value)) && atoi(value) != 0;
}

void GPUDrivenValidation::Trace(const char* stage)
{
    if (frames == 1 && IsGpuValidationRun())
    {
        WriteGraphicsLog("GPU Driven GBV stage: %s\n", stage);
    }
}

bool GPUDrivenValidation::IsGpuValidationRun()
{
    static const bool enabled = (Enabled() || GPUVisibilityValidation::Enabled()) && GpuValidation();
    return enabled;
}

void GPUDrivenValidation::BeforeFrame(uint64_t frame)
{
    GPUVisibility::GetSettings().Occlusion = false;
    GPUVisibility::GetSettings().LOD = false;
    frames = frame + 1;
    if (frame == 0)
    {
        vector<EntityID> remove;
        for (uint32_t i = 0; i < Registry::GetEntities().size(); ++i)
        {
            if (Registry::IsAlive(i) && !ComponentManager::HasComponent<CameraComponent>(i))
            {
                remove.push_back(i);
            }
        }
        for (auto id : remove)
        {
            World::DestroyEntity(Entity(id));
        }
        filesystem::create_directories("build/gpudriven");
        ofstream obj("build/gpudriven/fixture.obj");
        obj << "v -1 -1 0\nv 0 1 0\nv 1 -1 0\nvt 0 0\nvt 0.5 1\nvt 1 0\nvn 0 0 -1\nf 1/1/1 2/2/1 3/3/1\n";
        obj.close();
        modelID = ModelManager::LoadStaticModel("build/gpudriven/fixture.obj", false);
        Check(modelID >= 0, "load indexed static fixture");
        Trace("create fixtures begin");
        Grow(100);
        Trace("create fixtures end");
        World::SetVSyncEnabled(false);
    }
    RenderConfiguration::SetRenderMode(RenderMode::DEFERRED);
    Trace("set deferred mode end");
    RendererSettings::SetComputeGBufferEnabled(false);
    RendererSettings::SetMeshShadersEnabled(false);
    RendererSettings::SetTwoPhaseOcclusionEnabled(false);
    RendererSettings::SetUpscaleMode(UpscaleMode::Bilateral);
    Trace("set raster GBuffer mode end");
    Raytracing::GetSettings().Enabled = false;
    DayNightCycle::GetSettings().Running = false;
    auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(Camera::GetCameraEntity());
    auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(Camera::GetCameraEntity());
    transform.Position = {0, 0, -12};
    transform.IsDirty = true;
    camera.Target = {0, 0, 0};
    camera.AllowUserControl = false;
    camera.LockOnTarget = g_kINVALID_ENTITY;
    if (frame >= 32 && frame < 48)
    {
        transform.Position.x = (frame % 2) ? 20.0f : -2.0f;
        camera.Target = {transform.Position.x, 0, 0};
    }
    GPUDrivenRenderer::SetEnabled(GpuValidation() ||
                                  !(frame < 2 || (frame >= 10 && frame < 12) || (frame >= 16 && frame < 24) ||
                                    frame == 55 || frame == 57 || frame == 61 || frame == 74 || frame == 75));
    if (frame == 4)
    {
        Grow(1000);
    }
    if (frame == 8)
    {
        Grow(10000);
    }
    if (frame >= 48 && frame < 54)
    {
        if (frame % 2 == 0)
        {
            for (size_t i = 0; i < 500; ++i)
            {
                World::DestroyEntity(Entity(fixtures.back()));
                fixtures.pop_back();
            }
        }
        else
        {
            Grow(10000);
        }
    }
    if (frame == 58 && !fixtures.empty())
    {
        Entity(fixtures[0]).Get<ProceduralMotionComponent>().Enabled = false;
        auto& moving = Entity(fixtures[0]).Get<TransformComponent>();
        moving.Position.x += 0.15f;
        moving.Scale.x *= -1;
        moving.IsDirty = true;
        Entity(fixtures[0]).Get<MaterialComponent>().Roughness = 0.83f;
    }
    if (frame == 64)
    {
        for (auto entity : fixtures)
        {
            World::DestroyEntity(Entity(entity));
        }
        fixtures.clear();
    }
    if (frame == 65)
    {
        Grow(1000);
    }
    if (!GpuValidation())
    {
        if (frame == 54)
        {
            Entity(fixtures[0]).Get<ShaderComponent>().PsPath = "shader/hlsl/build/modelshaderPS_MRT.cso";
        }
        if (frame == 56)
        {
            Entity(fixtures[0]).Get<ShaderComponent>().PsPath.clear();
            auto& motion = Entity(fixtures[0]).Get<ProceduralMotionComponent>();
            motion.Target = Procedural::MotionTarget::MeshVertices;
            motion.Speed = 0;
            motion.Time = 0;
            motion.Enabled = true;
        }
        if (frame == 60)
        {
            Entity(fixtures[0]).Get<NameComponent>().Name = "Sky";
        }
        if (frame == 62)
        {
            Entity(fixtures[0]).Get<NameComponent>().Name = "GPU fixture";
        }
    }
    if (frame == 66 && !GpuValidation())
    {
        Entity(fixtures[0]).Get<ShaderComponent>().VsPath = "shader/hlsl/build/colorshader3dVS.cso";
    }
    if (frame == 67)
    {
        Entity(fixtures[0]).Get<ShaderComponent>().VsPath.clear();
    }
    if (frame == 68 && !GpuValidation())
    {
        auto& motion = Entity(fixtures[0]).Get<ProceduralMotionComponent>();
        motion.Target = Procedural::MotionTarget::MeshVertices;
        motion.Enabled = true;
    }
    if (frame == 69)
    {
        Entity(fixtures[0]).Get<ProceduralMotionComponent>().Enabled = false;
    }
    if (frame == 70 && !GpuValidation())
    {
        Entity(fixtures[0]).Get<NameComponent>().Name = "Sky";
    }
    if (frame == 71)
    {
        Entity(fixtures[0]).Get<NameComponent>().Name = "GPU fixture";
    }
    if (frame == 72)
    {
        // 10,000 entities with five material sections exercise 50,000 static mesh
        // instances without changing the existing 16,384-entity ECS contract.
        Grow(10000);
        ofstream obj("build/gpudriven/fixture-sections.obj");
        ofstream mtl("build/gpudriven/fixture-sections.mtl");
        obj << "mtllib fixture-sections.mtl\n";
        for (uint32_t i = 0; i < 5; ++i)
        {
            mtl << "newmtl Section" << i << "\nKd " << (0.2f + i * 0.15f) << " 1 1\n";
            // Keep sections and adjacent entities disjoint. Coplanar overlapping
            // materials otherwise make an unsorted opaque draw order observable.
            const float x = -0.7f + float(i) * 0.35f;
            obj << "v " << x - 0.12f << " -0.8 0\nv " << x << " 0.8 0\nv " << x + 0.12f << " -0.8 0\n";
            obj << "usemtl Section" << i << "\nf " << i * 3 + 1 << ' ' << i * 3 + 2 << ' ' << i * 3 + 3 << '\n';
        }
        obj.close();
        mtl.close();
        auto* model = ModelManager::GetStaticModel(modelID);
        Check(model &&
                  model->LoadAssimpModel("build/gpudriven/fixture-sections.obj", GraphicsDevice::GetDevice(), false) &&
                  model->GetMeshCount() == 5,
              "reload fixture with five indexed static mesh sections");
        for (auto entity : fixtures)
        {
            GpuSceneChanges::Notify(entity, GpuSceneDirtyFlags::All);
        }
    }
    Trace("BeforeFrame end");
}

void GPUDrivenValidation::AfterFrame(uint64_t frame)
{
    if (!GpuValidation() && (frame == 1 || frame == 11 || frame == 54 || frame == 56 || frame == 60 || frame == 75))
    {
        Capture(false);
    }
    if (!GpuValidation() && (frame == 2 || frame == 12 || frame == 55 || frame == 57 || frame == 61 || frame == 76))
    {
        Capture(true);
    }
    if (frame == 2 || frame == 5 || frame == 9 || (frame >= 32 && frame < 54) || frame == 64 || frame == 65 ||
        frame == 73 || frame == 83)
    {
        ValidateCommands();
    }
    const auto& snapshot = RenderProfiler::GetLatest();
    for (const auto& pass : snapshot.Passes)
    {
        if (pass.Name == "GBuffer / Opaque")
        {
            // GetLatest has frame-slot latency; keep only settled samples within each window.
            if (frame >= 20 && frame < 24)
            {
                cpuTimes.push_back(pass.CpuMs);
            }
            if (frame >= 28 && frame < 32)
            {
                gpuTimes.push_back(pass.CpuMs);
            }
        }
    }
    if (frame == 31 && !GpuValidation())
    {
        auto average = [](const vector<double>& values) {
            double sum = 0;
            for (double value : values)
            {
                sum += value;
            }
            return values.empty() ? 0 : sum / values.size();
        };
        const double cpu = average(cpuTimes), gpu = average(gpuTimes);
        WriteGraphicsLog("GPU Driven CPU submission comparison (10000): CPU=%.4f ms GPU=%.4f ms\n", cpu, gpu);
        Check(cpu > 0 && gpu > 0 && gpu < cpu, "10000 GPU opaque submission costs less than CPU fallback");
    }
    if (frame == 83)
    {
        for (const auto& pass : snapshot.Passes)
        {
            if (pass.Name == "Generate Indirect Commands" || pass.Name == "Opaque ExecuteIndirect" ||
                pass.Name == "GPU Scene Culling")
            {
                WriteGraphicsLog("GPU Driven profiler: %s GPU %.4f ms CPU %.4f ms\n", pass.Name.c_str(), pass.GpuMs,
                                 pass.CpuMs);
            }
        }
    }
    if (frame == 58 && !fixtures.empty())
    {
        const auto* instance = GpuScene::GetInstance(Entity(fixtures[0]).GetGPUInstanceID());
        Check(instance && memcmp(&instance->World, &instance->PreviousWorld, sizeof(instance->World)) != 0,
              "GPU driven instance preserves previous transform during motion");
    }
    if (frame == 59 && !fixtures.empty())
    {
        const auto* instance = GpuScene::GetInstance(Entity(fixtures[0]).GetGPUInstanceID());
        Check(instance && memcmp(&instance->World, &instance->PreviousWorld, sizeof(instance->World)) == 0,
              "GPU driven previous transform settles on stationary frame");
    }
    if (frame >= 66 && frame <= 71 && !GpuValidation())
    {
        const bool expectedGpu = frame % 2 != 0;
        const auto& fallback = GpuScene::CpuOpaqueStaticEntities();
        const bool found = find(fallback.begin(), fallback.end(), fixtures[0]) != fallback.end();
        Check(GPUDrivenRenderer::HandlesEntity(fixtures[0]) == expectedGpu && found != expectedGpu,
              "component API edits transfer ownership between GPU and CPU fallback");
    }
    if (frame == 73)
    {
        Check(GpuScene::Statistics().ActiveInstances == 50000 && GPUDrivenRenderer::CommandCapacity() >= 50000,
              "50000 static mesh instances fit grown command capacity");
    }
}

bool GPUDrivenValidation::Finish()
{
    GraphicsDevice::WaitForGpuIdle();
    Check(frames >= 84, "complete GPU driven stress sequence");
    failures += RenderCapture::ReportGraphicsErrors();
    WriteGraphicsLog("GPU Driven validation failures = %u\n", failures);
    return failures == 0;
}
