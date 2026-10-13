#include "pch.h"
#include "gpuscenevalidation.h"
#include "gpuscene.h"
#include "world.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "primitivegeometry.h"
#include "rendercapture.h"
#include "camera.h"
#include "materialbindings.h"
#include "modelmanager.h"
#include "rendertargets.h"
#include "raytracing.h"
#include "renderprofiler.h"
#include "renderersettings.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "meshshader/meshshaderrenderer.h"
#include <filesystem>
#include <fstream>

namespace
{
uint32_t failures = 0;
std::vector<EntityID> entities;
MeshComponent sharedMesh;
GPUInstanceID removed = InvalidGPUInstanceID;
uint32_t sharedMaterial = UINT32_MAX;
uint32_t previousMesh = UINT32_MAX;
bool legacyCulling = false;
uint64_t currentFrame = 0;
uint32_t residentDrawCulls = 0, legacyDrawCulls = 0;
XMFLOAT4X4 comparisonProjection{};
std::vector<RenderCapture::TextureData> referenceGBuffer;
bool Enabled()
{
    char value[8]{};
    return GetEnvironmentVariableA("DX12_GPU_SCENE_STRESS_TEST", value, sizeof(value)) && atoi(value) != 0;
}
void Check(bool passed, const char *name)
{
    WriteGraphicsLog("GPU Scene validation: %s %s\n", passed ? "PASS" : "FAIL", name);
    if (!passed)
    {
        ++failures;
    }
}
EntityID CreateFixture(bool skipCulling = false)
{
    Entity entity = World::CreateEntity();
    entity.Add<TransformComponent>().Add<MeshComponent>().Add<MaterialComponent>();
    auto &transform = entity.Get<TransformComponent>();
    transform.Position = XMFLOAT3(100000, 0, 0);
    entity.Get<MeshComponent>() = sharedMesh;
    if (skipCulling)
    {
        entity.Add<InstancingComponent>();
        entity.Get<InstancingComponent>().EnableFrustumCulling = false;
    }
    // Reuse the renderer's default primitive shader and the shared geometry.
    // Opaque drawing remains on the existing CPU path during this phase.
    return entity.GetID();
}
bool CpuVisible(const GPUInstanceData &instance, const XMMATRIX &viewProjection)
{
    if (!(instance.Flags & 1u))
    {
        return false;
    }
    if (!(instance.Flags & 2u))
    {
        return true;
    }
    uint32_t outside = 63;
    for (uint32_t corner = 0; corner < 8; ++corner)
    {
        XMFLOAT4 position;
        const auto &c = instance.BoundsCenter;
        const auto &e = instance.BoundsExtents;
        XMStoreFloat4(&position, XMVector4Transform(XMVectorSet(c.x + ((corner & 1) ? e.x : -e.x),
                                                                c.y + ((corner & 2) ? e.y : -e.y),
                                                                c.z + ((corner & 4) ? e.z : -e.z), 1),
                                                    viewProjection));
        const uint32_t mask = (position.x < -position.w ? 1u : 0u) | (position.x > position.w ? 2u : 0u) |
                              (position.y < -position.w ? 4u : 0u) | (position.y > position.w ? 8u : 0u) |
                              (position.z < 0 ? 16u : 0u) | (position.z > position.w ? 32u : 0u);
        outside &= mask;
    }
    return !outside;
}
void Readback()
{
    std::vector<uint8_t> instanceBytes, visibilityBytes, meshBytes, materialBytes;
    const auto &stats = GpuScene::Statistics();
    const auto state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const bool read =
        RenderCapture::ReadBuffer(GpuScene::InstanceBuffer(), state,
                                  uint64_t(stats.InstanceSlots) * sizeof(GPUInstanceData), instanceBytes) &&
        RenderCapture::ReadBuffer(GpuScene::VisibilityBuffer(), state, uint64_t(stats.InstanceSlots) * 4,
                                  visibilityBytes) &&
        RenderCapture::ReadBuffer(GpuScene::MeshBuffer(), state, uint64_t(stats.Meshes) * sizeof(GPUMeshData),
                                  meshBytes) &&
        RenderCapture::ReadBuffer(GpuScene::MaterialBuffer(), state,
                                  uint64_t(stats.Materials) * sizeof(GPUMaterialData), materialBytes);
    Check(read, "GPU buffer readback");
    if (!read)
    {
        return;
    }
    XMMATRIX view, projection;
    Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
    bool matching = true, visibilityMatching = true, shared = true, meshReference = true;
    uint32_t visible = 0;
    uint32_t mesh = UINT32_MAX, material = UINT32_MAX;
    for (EntityID entity : entities)
    {
        const auto id = Entity(entity).GetGPUInstanceID();
        const auto *cached = GpuScene::GetInstance(id);
        if (!cached || id >= stats.InstanceSlots)
        {
            matching = false;
            continue;
        }
        const auto *gpu = reinterpret_cast<const GPUInstanceData *>(instanceBytes.data()) + id;
        matching &= std::memcmp(cached, gpu, sizeof(*gpu)) == 0;
        const uint32_t result = reinterpret_cast<const uint32_t *>(visibilityBytes.data())[id];
        visibilityMatching &= result == (CpuVisible(*cached, view * projection) ? 1u : 0u);
        visible += result;
        if (mesh == UINT32_MAX)
        {
            mesh = cached->MeshIndex;
            material = cached->MaterialIndex;
        }
        // Later material edits intentionally diverge one instance's shared reference.
        const auto &sourceMesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
        if (sourceMesh.VertexBuffer == sharedMesh.VertexBuffer)
        {
            shared &= cached->MeshIndex == mesh;
        }
        const auto *meshData = reinterpret_cast<const GPUMeshData *>(meshBytes.data()) + cached->MeshIndex;
        const uint64_t vertexAddress =
            uint64_t(meshData->VertexAddressLow) | (uint64_t(meshData->VertexAddressHigh) << 32);
        meshReference &= vertexAddress == sourceMesh.VertexBuffer->GetGPUVirtualAddress();
        const auto *gpuMaterial =
            reinterpret_cast<const GPUMaterialData *>(materialBytes.data()) + cached->MaterialIndex;
        matching &= gpuMaterial->ParameterVectorCount > 0 &&
                    gpuMaterial->ParameterVectorCount <= GpuSceneMaterialParameterVectors;
        const auto expectedMaterial =
            MaterialBindings::BuildGpuSceneMaterial(ComponentManager::GetComponentUnchecked<MaterialComponent>(entity));
        matching &= std::memcmp(gpuMaterial, &expectedMaterial, sizeof(expectedMaterial)) == 0;
    }
    Check(matching, "CPU/HLSL layout and resident instance/material bytes");
    Check(visibilityMatching && visible == 1, "10000 resident GPU frustum results match CPU reference");
    Check(shared && meshReference, "10000 instances share existing vertex buffer reference");
}
} // namespace

void GpuSceneValidation::BeforeFrame(uint64_t frame)
{
    if (!Enabled())
    {
        return;
    }
    currentFrame = frame;
    char cpuGeometry[8]{};
    if (GetEnvironmentVariableA("DX12_GPU_SCENE_CPU_GEOMETRY", cpuGeometry, sizeof(cpuGeometry)) &&
        atoi(cpuGeometry) != 0)
    {
        // Isolate scene bookkeeping from both GPU geometry drawing paths.
        GPUDrivenRenderer::SetEnabled(false);
        MeshShaderRenderer::GetSettings().Enabled = false;
    }
    legacyCulling = frame == 81;
    residentDrawCulls = legacyDrawCulls = 0;
    if (frame == 60)
    {
        Entity source = World::CreateEntity();
        source.Add<MeshComponent>();
        std::vector<Vertex> triangle(3);
        triangle[0].Pos = {-0.5f, 0, 0};
        triangle[1].Pos = {0.5f, 0, 0};
        triangle[2].Pos = {0, 1, 0};
        Check(PrimitiveGeometry::UploadVertices(source.GetID(), triangle), "create shared fixture geometry");
        sharedMesh = source.Get<MeshComponent>();
        World::DestroyEntity(source);
        entities.push_back(CreateFixture(true));
    }
    if (frame == 61 || frame == 62)
    {
        const size_t count = frame == 61 ? 1000 : 10000;
        while (entities.size() < count)
        {
            entities.push_back(CreateFixture());
        }
        WriteGraphicsLog("GPU Scene fixture creation finished: %zu entities\n", entities.size());
    }
    if (frame == 66)
    {
        auto &transform = Entity(entities[6789]).Get<TransformComponent>();
        transform.Position.x += 7;
        transform.IsDirty = true;
    }
    if (frame == 69)
    {
        removed = Entity(entities[6789]).GetGPUInstanceID();
        World::DestroyEntity(Entity(entities[6789]));
    }
    if (frame == 70)
    {
        entities[6789] = CreateFixture();
    }
    if (frame == 71)
    {
        sharedMaterial = GpuScene::GetInstance(Entity(entities[6789]).GetGPUInstanceID())->MaterialIndex;
        Entity(entities[6789]).Get<MaterialComponent>().Roughness = 0.81f;
    }
    if (frame == 73)
    {
        Entity entity(entities[6789]);
        entity.Add<AABBComponent>();
        entity.Get<AABBComponent>().Center = {0.5f, 0.2f, 0};
        entity.Get<AABBComponent>().Extents = {1, 2, 3};
        auto &transform = entity.Get<TransformComponent>();
        transform.Scale = {-2, 0.5f, 3};
        transform.Rotation = {0.2f, 0.5f, 0.1f};
        transform.IsDirty = true;
    }
    if (frame == 74)
    {
        previousMesh = GpuScene::GetInstance(Entity(entities[6791]).GetGPUInstanceID())->MeshIndex;
        std::vector<Vertex> triangle(3);
        triangle[0].Pos = {-1, 0, 0};
        triangle[1].Pos = {1, 0, 0};
        triangle[2].Pos = {0, 2, 0};
        Check(PrimitiveGeometry::UploadVertices(entities[6791], triangle), "replace one shared mesh instance geometry");
    }
    if (frame == 75)
    {
        std::filesystem::create_directories("build/gpuscene");
        std::ofstream mesh("build/gpuscene/culling-fixture.obj");
        mesh << "v -1 0 0\nv 0 2 0\nv 1 0 0\nvt 0 0\nvt 0.5 1\nvt 1 0\nvn 0 0 -1\nf 1/1/1 2/2/1 3/3/1\n";
        mesh.close();
        const int model = ModelManager::LoadStaticModel("build/gpuscene/culling-fixture.obj", false);
        Check(model >= 0, "load indexed model fixture for resident indirect culling");
        if (model >= 0)
        {
            for (int i = 0; i < 2; ++i)
            {
                Entity entity = World::CreateEntity();
                entity.Add<TransformComponent>().Add<StaticModelComponent>().Add<MaterialComponent>();
                entity.Get<StaticModelComponent>().ModelId = model;
                entity.Get<TransformComponent>().Position = {static_cast<float>(i * 3), 1, 0};
            }
        }
    }
    if (frame == 80 || frame == 81)
    {
        Raytracing::GetSettings().Enabled = false;
        RendererSettings::SetMeshShadersEnabled(false);
        RendererSettings::SetTwoPhaseOcclusionEnabled(false);
    }
}

void GpuSceneValidation::BeforeScene()
{
    if (!Enabled())
    {
        return;
    }
    const EntityID camera = Camera::GetCameraEntity();
    auto &data = ComponentManager::GetComponentUnchecked<CameraComponent>(camera);
    if (currentFrame == 80)
    {
        comparisonProjection = data.ProjectionMatrix;
    }
    if (currentFrame == 81)
    {
        data.ProjectionMatrix = comparisonProjection;
    }
}
bool GpuSceneValidation::UseLegacyCulling()
{
    return legacyCulling;
}
void GpuSceneValidation::RecordModelCull(bool resident)
{
    if (resident)
    {
        ++residentDrawCulls;
    }
    else
    {
        ++legacyDrawCulls;
    }
}

void GpuSceneValidation::AfterFrame(uint64_t frame)
{
    if (!Enabled() || frame < 60)
    {
        return;
    }
    const auto &stats = GpuScene::Statistics();
    if (frame <= 75)
    {
        WriteGraphicsLog("GPU Scene frame %llu: active=%u dirty=%u entities=%u instanceUpload=%llu totalUpload=%llu "
                         "capacity=%u cpu=%.3f ms\n",
                         frame, stats.ActiveInstances, stats.DirtyInstances, stats.EntitiesProcessed,
                         stats.InstanceUploadedBytes, stats.UploadedBytes, stats.InstanceCapacity,
                         stats.UpdateCpuMilliseconds);
    }
    if (frame == 60 || frame == 61 || frame == 62)
    {
        bool registered = true, shared = true;
        const auto *first = GpuScene::GetInstance(Entity(entities.front()).GetGPUInstanceID());
        for (EntityID entity : entities)
        {
            const auto *data = GpuScene::GetInstance(Entity(entity).GetGPUInstanceID());
            registered &= data && data->Entity == entity;
            shared &=
                first && data && first->MeshIndex == data->MeshIndex && first->MaterialIndex == data->MaterialIndex;
        }
        Check(registered && entities.size() == (frame == 60   ? 1
                                                : frame == 61 ? 1000
                                                              : 10000),
              "1/1000/10000 ECS instance registration");
        Check(shared, "mesh and material records are shared");
    }
    if (frame == 63 || frame == 66 || frame == 70 || frame == 71 || frame == 74)
    {
        Readback();
    }
    if (frame == 65 || frame == 68)
    {
        Check(stats.InstanceUploadedBytes == 0 && stats.DirtyInstances == 0,
              "unchanged scene performs zero instance upload");
    }
    if (frame == 66)
    {
        Check(stats.DirtyInstances == 1 && stats.InstanceUploadedBytes == sizeof(GPUInstanceData),
              "one of 10000 moves: exactly 224 bytes uploaded");
    }
    if (frame == 67)
    {
        Check(stats.DirtyInstances == 1 && stats.InstanceUploadedBytes == sizeof(GPUInstanceData),
              "one temporal history settle update");
    }
    if (frame == 69)
    {
        std::vector<uint8_t> bytes;
        Check(!GpuScene::GetInstance(removed), "destroyed instance is inactive");
        const bool read =
            RenderCapture::ReadBuffer(GpuScene::VisibilityBuffer(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                      uint64_t(stats.InstanceSlots) * 4, bytes);
        Check(read && reinterpret_cast<const uint32_t *>(bytes.data())[removed] == 0,
              "deleted slot is skipped by GPU culling");
    }
    if (frame == 70)
    {
        Check(Entity(entities[6789]).GetGPUInstanceID() == removed, "free list reuses removed GPU ID");
    }
    if (frame == 71)
    {
        const auto *changed = GpuScene::GetInstance(Entity(entities[6789]).GetGPUInstanceID());
        const auto *untouched = GpuScene::GetInstance(Entity(entities[6790]).GetGPUInstanceID());
        Check(changed && untouched && changed->MaterialIndex != sharedMaterial &&
                  untouched->MaterialIndex == sharedMaterial,
              "material edit preserves other instances' shared material");
    }
    if (frame == 73)
    {
        const EntityID entity = entities[6789];
        const auto *instance = GpuScene::GetInstance(Entity(entity).GetGPUInstanceID());
        const auto &local = ComponentManager::GetComponentUnchecked<AABBComponent>(entity);
        const auto &transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
        XMFLOAT3 minimum{FLT_MAX, FLT_MAX, FLT_MAX}, maximum{-FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (uint32_t corner = 0; corner < 8; ++corner)
        {
            XMFLOAT3 point;
            XMStoreFloat3(&point,
                          XMVector3TransformCoord(
                              XMVectorSet(local.Center.x + ((corner & 1) ? local.Extents.x : -local.Extents.x),
                                          local.Center.y + ((corner & 2) ? local.Extents.y : -local.Extents.y),
                                          local.Center.z + ((corner & 4) ? local.Extents.z : -local.Extents.z), 1),
                              XMLoadFloat4x4(&transform.WorldMatrix)));
            minimum = {(std::min)(minimum.x, point.x), (std::min)(minimum.y, point.y), (std::min)(minimum.z, point.z)};
            maximum = {(std::max)(maximum.x, point.x), (std::max)(maximum.y, point.y), (std::max)(maximum.z, point.z)};
        }
        Check(instance && std::abs(instance->BoundsCenter.x - (minimum.x + maximum.x) * 0.5f) < 0.02f &&
                  std::abs(instance->BoundsExtents.x - (maximum.x - minimum.x) * 0.5f) < 0.02f &&
                  std::abs(instance->BoundsExtents.y - (maximum.y - minimum.y) * 0.5f) < 0.002f &&
                  std::abs(instance->BoundsExtents.z - (maximum.z - minimum.z) * 0.5f) < 0.002f,
              "rotated negative scale and overridden bounds match transformed corners");
    }
    if (frame == 74)
    {
        const auto *changed = GpuScene::GetInstance(Entity(entities[6791]).GetGPUInstanceID());
        const auto *untouched = GpuScene::GetInstance(Entity(entities[6792]).GetGPUInstanceID());
        Check(changed && untouched && changed->MeshIndex != previousMesh && untouched->MeshIndex == previousMesh,
              "mesh replacement preserves shared geometry and stable instance IDs");
    }
    if (frame == 80 || frame == 81)
    {
        Check(frame == 80 ? residentDrawCulls > 0 : legacyDrawCulls > 0,
              "indexed model rendering executes resident/legacy GPU culling path");
        // Compare the opaque render targets before temporal DLSS/NRD can change
        // their histories. The camera projection is identical for these frames.
        bool matching = true;
        const GBufferType targets[] = {GBufferType::BASE_COLOR, GBufferType::NORMAL, GBufferType::MATERIAL};
        for (size_t i = 0; i < std::size(targets); ++i)
        {
            RenderCapture::TextureData capture;
            if (!RenderCapture::ReadTexture(RenderTargets::GetGBufferResource(targets[i]), capture))
            {
                matching = false;
                continue;
            }
            if (frame == 80)
            {
                referenceGBuffer.push_back(std::move(capture));
            }
            else if (i >= referenceGBuffer.size())
            {
                matching = false;
            }
            else
            {
                const auto &previous = referenceGBuffer[i];
                matching &= previous.Width == capture.Width && previous.Height == capture.Height &&
                            previous.Format == capture.Format;
                const uint32_t bytesPerPixel = capture.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
                if (matching)
                {
                    for (UINT row = 0; row < capture.Height; ++row)
                    {
                        matching &= std::memcmp(previous.Bytes.data() + row * previous.RowPitch,
                                                capture.Bytes.data() + row * capture.RowPitch,
                                                capture.Width * bytesPerPixel) == 0;
                    }
                }
            }
        }
        Check(matching, frame == 80 ? "capture resident rendering GBuffer"
                                    : "resident/legacy albedo, normal and material GBuffer match exactly");
        for (const auto &pass : RenderProfiler::GetLatest().Passes)
        {
            if (pass.Name == "GPU Scene" || pass.Name == "GPU Scene Culling" || pass.Name == "GPU Scene Update")
            {
                WriteGraphicsLog("GPU Scene profiler: %s GPU %.4f ms CPU %.4f ms\n", pass.Name.c_str(), pass.GpuMs,
                                 pass.CpuMs);
            }
        }
    }
}
uint32_t GpuSceneValidation::Failures()
{
    return failures;
}
