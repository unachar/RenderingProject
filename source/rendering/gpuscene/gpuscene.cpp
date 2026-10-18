#include "pch.h"
#include "cpuculling.h"
#include "gpuscene.h"
#include "gpuscenechanges.h"
#include "gpusceneslots.h"
#include "gpuscenebuffer.h"
#include "gpusceneculling.h"
#include "gpuscenevalidation.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "gpudriven/gpudrivenvalidation.h"
#include "componentmanager.h"
#include "world.h"
#include "modelmanager.h"
#include "materialbindings.h"
#include "materialmanager.h"
#include "texturemanager.h"
#include "graphicsdevice.h"
#include "rendergraph.h"
#include "renderprofiler.h"
#include "visibility/gpuvisibility.h"
#include "virtualgeometry/virtualgeometryresources.h"
#include <unordered_set>

namespace
{
struct SceneState
{
    GpuSceneSlots Instances;
    vector<GPUMeshData> Meshes;
    vector<GPUMaterialData> Materials;
    vector<uint32_t> NewMeshes, NewMaterials;
    vector<ComPtr<ID3D12Resource>> GeometryOwners;
    unordered_map<uint32_t, ComPtr<ID3D12Resource>> PrimitiveOwners;
    unordered_map<uint32_t, array<uint32_t, StaticMeshData::LodCount - 1>> PrimitiveLODs;
    array<vector<ComPtr<ID3D12Resource>>, GpuSceneBuffer::FrameCount> RetiredPrimitiveOwners;
    vector<uint32_t> FreePrimitiveMeshes;
    unordered_map<uint64_t, vector<uint32_t>> MeshLookup, MaterialLookup;

    struct AssetMaterialRecord
    {
        uint32_t Index = 0;
        uint64_t Revision = 0;
        uint64_t PipelineRevision = 0;
        unordered_set<uint32_t> Users;
    };

    unordered_map<AssetHandle, AssetMaterialRecord> AssetMaterials;
    unordered_map<uint32_t, vector<AssetHandle>> EntityAssets;
    unordered_map<uint32_t, vector<GPUInstanceID>> Entities;
    vector<uint32_t> CpuOpaqueStatic;
    unordered_map<uint32_t, size_t> CpuOpaquePositions;
    GpuSceneBuffer InstanceBuffer, MeshBuffer, MaterialBuffer;
    GpuSceneCulling Culling;
    GpuSceneStatistics Stats;
};

SceneState& Scene()
{
    static SceneState state;
    return state;
}

uint64_t Hash(const void* data, size_t size)
{
    uint64_t hash = 1469598103934665603ull;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
    {
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
    return hash;
}

template <typename T>
uint32_t RegisterShared(const T& data, vector<T>& records, unordered_map<uint64_t, vector<uint32_t>>& lookup,
                        vector<uint32_t>& changes)
{
    auto& matches = lookup[Hash(&data, sizeof(data))];
    for (uint32_t id : matches)
    {
        if (memcmp(&records[id], &data, sizeof(data)) == 0)
        {
            return id;
        }
    }
    const uint32_t id = static_cast<uint32_t>(records.size());
    records.push_back(data);
    matches.push_back(id);
    changes.push_back(id);
    return id;
}

void SetCpuOpaqueEntity(uint32_t entity, bool include)
{
    auto& scene = Scene();
    const auto position = scene.CpuOpaquePositions.find(entity);
    if (include && position == scene.CpuOpaquePositions.end())
    {
        scene.CpuOpaquePositions[entity] = scene.CpuOpaqueStatic.size();
        scene.CpuOpaqueStatic.push_back(entity);
    }
    else if (!include && position != scene.CpuOpaquePositions.end())
    {
        const size_t index = position->second;
        const uint32_t moved = scene.CpuOpaqueStatic.back();
        scene.CpuOpaqueStatic[index] = moved;
        scene.CpuOpaquePositions[moved] = index;
        scene.CpuOpaqueStatic.pop_back();
        scene.CpuOpaquePositions.erase(entity);
    }
}

void RemoveEntity(uint32_t entity)
{
    GPUDrivenRenderer::TrackEntityTextures(entity, {});
    SetCpuOpaqueEntity(entity, false);
    auto& scene = Scene();
    const auto assets = scene.EntityAssets.find(entity);
    if (assets != scene.EntityAssets.end())
    {
        for (const auto handle : assets->second)
        {
            if (auto material = scene.AssetMaterials.find(handle); material != scene.AssetMaterials.end())
            {
                material->second.Users.erase(entity);
            }
        }
        scene.EntityAssets.erase(assets);
    }

    const auto found = scene.Entities.find(entity);
    if (found == scene.Entities.end())
    {
        return;
    }
    for (GPUInstanceID id : found->second)
    {
        scene.Instances.Remove(id);
    }
    scene.Entities.erase(found);
}

uint32_t RegisterMesh(ID3D12Resource* vertices, ID3D12Resource* indices, uint32_t vertexCount, uint32_t indexCount,
                      const XMFLOAT3& center, const XMFLOAT3& extents, bool retain = true)
{
    auto& scene = Scene();
    GPUMeshData mesh{};
    const uint64_t vertexAddress = vertices ? vertices->GetGPUVirtualAddress() : 0;
    const uint64_t indexAddress = indices ? indices->GetGPUVirtualAddress() : 0;
    mesh.VertexAddressLow = static_cast<uint32_t>(vertexAddress);
    mesh.VertexAddressHigh = static_cast<uint32_t>(vertexAddress >> 32);
    mesh.IndexAddressLow = static_cast<uint32_t>(indexAddress);
    mesh.IndexAddressHigh = static_cast<uint32_t>(indexAddress >> 32);
    mesh.VertexCount = vertexCount;
    mesh.IndexCount = indexCount;
    mesh.BoundsCenter = XMFLOAT4(center.x, center.y, center.z, 1);
    mesh.BoundsExtents = XMFLOAT4(extents.x, extents.y, extents.z, 0);
    const size_t oldCount = scene.Meshes.size();
    const uint32_t id = RegisterShared(mesh, scene.Meshes, scene.MeshLookup, scene.NewMeshes);
    if (retain && scene.Meshes.size() != oldCount)
    {
        if (vertices)
        {
            scene.GeometryOwners.emplace_back(vertices);
        }
        if (indices)
        {
            scene.GeometryOwners.emplace_back(indices);
        }
    }
    return id;
}

void RemoveMeshLookup(uint32_t id)
{
    auto& scene = Scene();
    const auto key = Hash(&scene.Meshes.at(id), sizeof(GPUMeshData));
    auto found = scene.MeshLookup.find(key);
    if (found == scene.MeshLookup.end())
    {
        return;
    }
    erase(found->second, id);
    if (found->second.empty())
    {
        scene.MeshLookup.erase(found);
    }
}

uint32_t RegisterPrimitiveMesh(uint32_t entity, ID3D12Resource* vertices, uint32_t vertexCount, const XMFLOAT3& center,
                               const XMFLOAT3& extents)
{
    auto& scene = Scene();
    uint32_t reusable = UINT32_MAX;
    if (const auto previous = scene.Entities.find(entity);
        previous != scene.Entities.end() && !previous->second.empty())
    {
        const auto old = scene.Instances.Data()[previous->second.front()].MeshIndex;
        uint32_t references = 0;
        if (scene.PrimitiveOwners.contains(old))
        {
            for (const auto& instance : scene.Instances.Data())
            {
                if ((instance.Flags & 1u) && instance.MeshIndex == old)
                {
                    ++references;
                }
            }
        }
        if (references == 1)
        {
            reusable = old;
        }
    }
    GPUMeshData record{};
    const auto address = vertices->GetGPUVirtualAddress();
    record.VertexAddressLow = uint32_t(address);
    record.VertexAddressHigh = uint32_t(address >> 32);
    record.VertexCount = vertexCount;
    record.BoundsCenter = XMFLOAT4(center.x, center.y, center.z, 1);
    record.BoundsExtents = XMFLOAT4(extents.x, extents.y, extents.z, 0);
    const auto key = Hash(&record, sizeof(record));
    if (const auto existing = scene.MeshLookup.find(key); existing != scene.MeshLookup.end())
    {
        for (auto id : existing->second)
        {
            if (memcmp(&scene.Meshes[id], &record, sizeof(record)) == 0)
            {
                return id;
            }
        }
    }
    if (reusable == UINT32_MAX && !scene.FreePrimitiveMeshes.empty())
    {
        reusable = scene.FreePrimitiveMeshes.back();
        scene.FreePrimitiveMeshes.pop_back();
    }
    if (reusable != UINT32_MAX)
    {
        RemoveMeshLookup(reusable);
        if (auto old = scene.PrimitiveOwners.find(reusable); old != scene.PrimitiveOwners.end())
        {
            scene.RetiredPrimitiveOwners[GraphicsDevice::GetFrameIndex()].push_back(move(old->second));
        }
        scene.Meshes[reusable] = record;
        scene.MeshLookup[key].push_back(reusable);
        scene.NewMeshes.push_back(reusable);
    }
    else
    {
        reusable = RegisterMesh(vertices, nullptr, vertexCount, 0, center, extents, false);
    }
    scene.PrimitiveOwners[reusable] = vertices;
    return reusable;
}

void RetireUnusedPrimitiveMeshes()
{
    auto& scene = Scene();
    unordered_set<uint32_t> used;
    for (const auto& instance : scene.Instances.Data())
    {
        if (instance.Flags & 1u)
        {
            used.insert(instance.MeshIndex);
        }
    }
    for (auto it = scene.PrimitiveLODs.begin(); it != scene.PrimitiveLODs.end();)
    {
        if (used.contains(it->first))
        {
            for (auto id : it->second)
            {
                if (id != UINT32_MAX)
                {
                    used.insert(id);
                }
            }
            ++it;
        }
        else
        {
            it = scene.PrimitiveLODs.erase(it);
        }
    }
    for (auto it = scene.PrimitiveOwners.begin(); it != scene.PrimitiveOwners.end();)
    {
        if (used.contains(it->first))
        {
            ++it;
            continue;
        }
        const auto id = it->first;
        scene.RetiredPrimitiveOwners[GraphicsDevice::GetFrameIndex()].push_back(move(it->second));
        RemoveMeshLookup(id);
        scene.Meshes[id] = {};
        scene.NewMeshes.push_back(id);
        scene.FreePrimitiveMeshes.push_back(id);
        GPUDrivenRenderer::ReleasePrimitiveGeometry(id);
        it = scene.PrimitiveOwners.erase(it);
    }
}

void UpdateEntity(uint32_t entity, GpuSceneDirtyFlags dirty)
{
    auto& scene = Scene();
    if (!Registry::IsAlive(entity) || !ComponentManager::HasComponent<TransformComponent>(entity))
    {
        RemoveEntity(entity);
        return;
    }
    const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
    for (const auto handle : scene.EntityAssets[entity])
    {
        if (auto material = scene.AssetMaterials.find(handle); material != scene.AssetMaterials.end())
        {
            material->second.Users.erase(entity);
        }
    }
    scene.EntityAssets[entity].clear();
    const MaterialComponent defaultMaterial{};
    const auto& material = ComponentManager::HasComponent<MaterialComponent>(entity)
                               ? ComponentManager::GetComponentUnchecked<MaterialComponent>(entity)
                               : defaultMaterial;
    auto& ids = scene.Entities[entity];
    size_t section = 0;
    auto append = [&](ID3D12Resource* vertices, ID3D12Resource* indices, uint32_t vertexCount, uint32_t indexCount,
                      XMFLOAT3 center, XMFLOAT3 extents, int texture, bool primitive = false) {
        if (!vertices || !vertexCount)
        {
            return;
        }
        const XMFLOAT3 meshCenter = center;
        const XMFLOAT3 meshExtents = extents;
        if (ComponentManager::HasComponent<AABBComponent>(entity))
        {
            const auto& bounds = ComponentManager::GetComponentUnchecked<AABBComponent>(entity);
            center = bounds.Center;
            extents = bounds.Extents;
        }
        GPUInstanceData instance{};
        instance.Entity = entity;
        instance.Flags = 3;
        if (section < ids.size())
        {
            instance.Flags |= scene.Instances.Data()[ids[section]].Flags & 4u;
        }
        instance.World = transform.WorldMatrix;
        instance.PreviousWorld = section < ids.size() ? scene.Instances.Data()[ids[section]].World : instance.World;
        // Material-only edits must preserve the temporal state of a moving instance.
        if (section < ids.size() &&
            memcmp(&instance.World, &scene.Instances.Data()[ids[section]].World, sizeof(instance.World)) == 0)
        {
            instance.PreviousWorld = scene.Instances.Data()[ids[section]].PreviousWorld;
        }
        instance.LocalCenter = XMFLOAT4(meshCenter.x, meshCenter.y, meshCenter.z, 1);
        instance.LocalExtents = XMFLOAT4(meshExtents.x, meshExtents.y, meshExtents.z, 0);
        const XMMATRIX world = XMLoadFloat4x4(&instance.World);
        XMFLOAT3 worldCenter;
        XMStoreFloat3(&worldCenter, XMVector3TransformCoord(XMLoadFloat3(&center), world));
        const auto& matrix = instance.World;
        instance.BoundsCenter = XMFLOAT4(worldCenter.x, worldCenter.y, worldCenter.z, 1);
        instance.BoundsExtents =
            XMFLOAT4(abs(matrix._11) * extents.x + abs(matrix._21) * extents.y + abs(matrix._31) * extents.z,
                     abs(matrix._12) * extents.x + abs(matrix._22) * extents.y + abs(matrix._32) * extents.z,
                     abs(matrix._13) * extents.x + abs(matrix._23) * extents.y + abs(matrix._33) * extents.z, 0);
        if (ComponentManager::HasComponent<InstancingComponent>(entity) &&
            !ComponentManager::GetComponentUnchecked<InstancingComponent>(entity).EnableFrustumCulling)
        {
            instance.Flags &= ~2u;
        }
        if (ComponentManager::HasComponent<LODComponent>(entity))
        {
            const auto& lod = ComponentManager::GetComponentUnchecked<LODComponent>(entity);
            instance.LodDistances = XMFLOAT4(lod.Lod1Distance, lod.Lod2Distance, lod.UseLOD ? 1.0f : 0.0f, 0);
        }
        const bool existing = section < ids.size();
        const uint32_t flags = static_cast<uint32_t>(dirty);
        if (existing && !(flags & static_cast<uint32_t>(GpuSceneDirtyFlags::Mesh | GpuSceneDirtyFlags::Bounds)))
        {
            instance.MeshIndex = scene.Instances.Data()[ids[section]].MeshIndex;
        }
        else
        {
            instance.MeshIndex =
                primitive ? RegisterPrimitiveMesh(entity, vertices, vertexCount, meshCenter, meshExtents)
                          : RegisterMesh(vertices, indices, vertexCount, indexCount, meshCenter, meshExtents);
        }
        if (existing && !(flags & static_cast<uint32_t>(GpuSceneDirtyFlags::Material | GpuSceneDirtyFlags::Mesh)))
        {
            instance.MaterialIndex = scene.Instances.Data()[ids[section]].MaterialIndex;
        }
        else
        {
            const auto& resolved = MaterialManager::Resolve(material, section);
            auto gpuMaterial = MaterialBindings::BuildGpuSceneMaterial(resolved);
            if (resolved.RuntimeHandle == 0 && material.UseTexture && texture >= 0)
            {
                gpuMaterial.AlbedoTextureIndex = static_cast<uint32_t>(texture);
            }
            if (resolved.RuntimeHandle != 0)
            {
                const auto handle = resolved.RuntimeHandle;
                auto found = scene.AssetMaterials.find(handle);
                if (found == scene.AssetMaterials.end())
                {
                    const auto index = static_cast<uint32_t>(scene.Materials.size());
                    scene.Materials.push_back(gpuMaterial);
                    scene.NewMaterials.push_back(index);
                    const auto& runtime = MaterialManager::GetRuntime(handle);
                    scene.AssetMaterials.emplace(
                        handle, SceneState::AssetMaterialRecord{index, runtime.Revision, runtime.PipelineRevision, {}});
                    instance.MaterialIndex = index;
                }
                else
                {
                    instance.MaterialIndex = found->second.Index;
                }
            }
            else
            {
                instance.MaterialIndex =
                    RegisterShared(gpuMaterial, scene.Materials, scene.MaterialLookup, scene.NewMaterials);
            }
        }
        const auto assetHandle = MaterialManager::SlotHandle(material, section);
        if (assetHandle != 0)
        {
            // SlotHandle can outlive a failed/removed runtime material while a scene is
            // being rebuilt.  Do not use at(): a stale handle must not terminate loading.
            if (auto asset = scene.AssetMaterials.find(assetHandle); asset != scene.AssetMaterials.end())
            {
                asset->second.Users.insert(entity);
                scene.EntityAssets[entity].push_back(assetHandle);
            }
        }
        if (section < ids.size())
        {
            scene.Instances.Update(ids[section], instance);
        }
        else
        {
            ids.push_back(scene.Instances.Register(instance));
        }
        ++section;
    };
    if (ComponentManager::HasComponent<StaticModelComponent>(entity))
    {
        auto* model =
            ModelManager::GetStaticModel(ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity).ModelId);
        if (model)
        {
            for (UINT i = 0; i < model->GetMeshCount(); ++i)
            {
                const auto& mesh = model->GetMeshData(i);
                const size_t before = section;
                append(mesh.VertexBuffer.Get(), mesh.IndexBuffer.Get(), mesh.VertexCount, mesh.IndexCount,
                       model->GetAabbCenter(), model->GetAabbExtents(), mesh.TextureIndex);
                if (section > before)
                {
                    const auto slot = ids[section - 1];
                    auto instance = scene.Instances.Data()[slot];
                    GPUDrivenRenderer::RegisterGeometry(instance.MeshIndex, mesh);
                    if (mesh.VirtualGeometrySettings.Enabled && mesh.VirtualGeometryLODs[0])
                    {
                        instance.VirtualGeometryAssetIndex =
                            VirtualGeometryResources::Register(mesh.VirtualGeometryLODs[0]);
                    }
                    // 自動 LOD：LOD コンポーネントが無いモデルも、インポート時に生成した LOD を画面サイズで切り替える
                    const bool hasLodComponent = ComponentManager::HasComponent<LODComponent>(entity);
                    if (hasLodComponent || RenderCulling::GetSettings().AutoLod)
                    {
                        const LODComponent lod = hasLodComponent
                                                     ? ComponentManager::GetComponentUnchecked<LODComponent>(entity)
                                                     : LODComponent{true};
                        if (lod.UseLOD)
                        {
                            array<uint32_t, Visibility::MaxLODs> lodMeshes;
                            lodMeshes.fill(Visibility::InvalidID);
                            lodMeshes[0] = instance.MeshIndex;
                            const auto& metadata = lod.UseAssetSettings ? model->GetLODMetadata() : lod.ScreenLOD;
                            for (uint32_t level = 1; level < metadata.Count && level < Visibility::MaxLODs; ++level)
                            {
                                if (!metadata.MeshPaths[level].empty())
                                {
                                    auto* alternative = ModelManager::GetStaticModel(
                                        ModelManager::LoadStaticModel(metadata.MeshPaths[level].c_str(), false));
                                    if (!alternative || alternative->GetMeshCount() != model->GetMeshCount())
                                    {
                                        continue;
                                    }
                                    // Original bounds must cover every selectable LOD.
                                    const auto c = model->GetAabbCenter(), e = model->GetAabbExtents();
                                    const auto ac = alternative->GetAabbCenter(), ae = alternative->GetAabbExtents();
                                    if (abs(ac.x - c.x) + ae.x > e.x + 0.0001f ||
                                        abs(ac.y - c.y) + ae.y > e.y + 0.0001f ||
                                        abs(ac.z - c.z) + ae.z > e.z + 0.0001f)
                                    {
                                        continue;
                                    }
                                    const auto& alternativeMesh = alternative->GetMeshData(i);
                                    const auto meshID = RegisterMesh(
                                        alternativeMesh.VertexBuffer.Get(), alternativeMesh.IndexBuffer.Get(),
                                        alternativeMesh.VertexCount, alternativeMesh.IndexCount, ac, ae);
                                    GPUDrivenRenderer::RegisterGeometry(meshID, alternativeMesh);
                                    if (GPUDrivenRenderer::GeometryReady(meshID))
                                    {
                                        lodMeshes[level] = meshID;
                                    }
                                    continue;
                                }
                                if (level >= StaticMeshData::LodCount)
                                {
                                    continue;
                                }
                                const auto& indices = mesh.CpuLodIndices[level - 1];
                                if (indices.empty())
                                {
                                    continue;
                                }
                                const auto meshID =
                                    RegisterMesh(mesh.VertexBuffer.Get(), mesh.LodIndexBuffers[level - 1].Get(),
                                                 mesh.VertexCount, static_cast<uint32_t>(indices.size()),
                                                 model->GetAabbCenter(), model->GetAabbExtents());
                                GPUDrivenRenderer::RegisterLODGeometry(meshID, instance.MeshIndex, indices,
                                                                       &mesh.MeshletLODs[level]);
                                if (GPUDrivenRenderer::GeometryReady(meshID))
                                {
                                    lodMeshes[level] = meshID;
                                }
                            }
                            instance.LODGroupIndex = GPUVisibility::RegisterLODGroup(lodMeshes, metadata);
                        }
                    }
                    if (ComponentManager::HasComponent<ProceduralMotionComponent>(entity) &&
                        ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(entity).Enabled)
                    {
                        instance.Flags |= 8u;
                    }
                    scene.Instances.Update(slot, instance);
                }
            }
        }
    }
    else if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
    {
        auto* model = ModelManager::GetAnimModel(
            ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity).ModelId);
        if (model)
        {
            for (UINT i = 0; i < model->GetMeshCount(); ++i)
            {
                const auto& mesh = model->GetMeshData(i);
                append(mesh.VertexBuffer.Get(), mesh.IndexBuffer.Get(), mesh.VertexCount, mesh.IndexCount,
                       model->GetAabbCenter(), model->GetAabbExtents(), mesh.TextureIndex);
            }
        }
    }
    else if (ComponentManager::HasComponent<MeshComponent>(entity))
    {
        const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
        append(mesh.VertexBuffer.Get(), nullptr, mesh.VertexCount, 0, mesh.LocalBoundsCenter, mesh.LocalBoundsExtents,
               -1, mesh.NaniteMesh != nullptr);
        if (mesh.NaniteMesh && !ids.empty())
        {
            const auto slot = ids.back();
            auto instance = scene.Instances.Data()[slot];
            const auto& source = *mesh.NaniteMesh;
            GPUDrivenRenderer::RegisterPrimitiveGeometry(instance.MeshIndex, source);
            if (source.VirtualGeometrySettings.Enabled && source.VirtualGeometryLODs[0])
            {
                instance.VirtualGeometryAssetIndex = VirtualGeometryResources::Register(source.VirtualGeometryLODs[0]);
            }
            const bool hasSettings = ComponentManager::HasComponent<LODComponent>(entity);
            const auto settings =
                hasSettings ? ComponentManager::GetComponentUnchecked<LODComponent>(entity) : LODComponent{};
            // Generated geometry has no imported asset settings. Enable its generated
            // levels by default, while honoring an explicitly attached LOD component.
            if (!hasSettings || settings.UseLOD)
            {
                auto [entry, inserted] = scene.PrimitiveLODs.try_emplace(instance.MeshIndex);
                if (inserted)
                {
                    entry->second.fill(UINT32_MAX);
                }
                array<uint32_t, Visibility::MaxLODs> lodMeshes;
                lodMeshes.fill(UINT32_MAX);
                lodMeshes[0] = instance.MeshIndex;
                const auto metadata = settings.UseAssetSettings ? Visibility::LODMetadata{} : settings.ScreenLOD;
                for (uint32_t level = 1; level < StaticMeshData::LodCount; ++level)
                {
                    const auto& indices = source.CpuLodIndices[level - 1];
                    auto& id = entry->second[level - 1];
                    if (indices.empty())
                    {
                        if (id != UINT32_MAX)
                        {
                            GPUDrivenRenderer::ReleasePrimitiveGeometry(id);
                        }
                        continue;
                    }
                    if (id == UINT32_MAX)
                    {
                        if (!scene.FreePrimitiveMeshes.empty())
                        {
                            id = scene.FreePrimitiveMeshes.back();
                            scene.FreePrimitiveMeshes.pop_back();
                        }
                        else
                        {
                            id = static_cast<uint32_t>(scene.Meshes.size());
                            scene.Meshes.emplace_back();
                        }
                    }
                    auto record = scene.Meshes[instance.MeshIndex];
                    record.IndexCount = static_cast<uint32_t>(indices.size());
                    if (memcmp(&scene.Meshes[id], &record, sizeof(record)) != 0)
                    {
                        scene.Meshes[id] = record;
                        scene.NewMeshes.push_back(id);
                    }
                    scene.PrimitiveOwners[id] = mesh.VertexBuffer;
                    GPUDrivenRenderer::RegisterPrimitiveGeometry(id, source, level, instance.MeshIndex);
                    if (GPUDrivenRenderer::GeometryReady(id))
                    {
                        lodMeshes[level] = id;
                    }
                }
                instance.LODGroupIndex = GPUVisibility::RegisterLODGroup(lodMeshes, metadata);
            }
            scene.Instances.Update(slot, instance);
        }
    }
    else if (ComponentManager::HasComponent<SpriteComponent>(entity))
    {
        const auto& sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(entity);
        if (sprite.Is3D)
        {
            append(sprite.VertexBuffer.Get(), nullptr, sprite.VertexCount, 0, sprite.LocalBoundsCenter,
                   sprite.LocalBoundsExtents, -1);
        }
    }
    while (ids.size() > section)
    {
        scene.Instances.Remove(ids.back());
        ids.pop_back();
    }
    if (ids.empty())
    {
        scene.Entities.erase(entity);
        SetCpuOpaqueEntity(entity, ComponentManager::HasComponent<StaticModelComponent>(entity));
        GPUDrivenRenderer::TrackEntityTextures(entity, {});
    }
    else
    {
        bool supported = GPUDrivenRenderer::SupportsEntity(entity);
        for (auto id : ids)
        {
            supported &= GPUDrivenRenderer::GeometryReady(scene.Instances.Data()[id].MeshIndex);
        }
        for (auto id : ids)
        {
            auto instance = scene.Instances.Data()[id];
            instance.Flags = supported ? (instance.Flags | 4u) : (instance.Flags & ~4u);
            instance.Flags &= ~16u;
            const auto& gpuMaterial = scene.Materials.at(instance.MaterialIndex);
            uint32_t surfaceFlags = 0;
            memcpy(&surfaceFlags, &gpuMaterial.ShadingParameters[149].w, sizeof(surfaceFlags));
            if (supported && instance.VirtualGeometryAssetIndex != UINT32_MAX && (instance.Flags & 8u) == 0 &&
                (gpuMaterial.Flags & 6u) == 0 && (surfaceFlags & 15u) == 0)
            {
                instance.Flags |= 16u;
            }
            scene.Instances.Update(id, instance);
        }
        // Auto models still use the CPU outline pass after their base geometry moves to Nanite.
        SetCpuOpaqueEntity(entity, ComponentManager::HasComponent<StaticModelComponent>(entity) &&
                                       (!supported || material.ShaderClassMode == MaterialMode::Auto));
        vector<uint32_t> textures;
        if (supported)
        {
            for (auto id : ids)
            {
                const auto& material = scene.Materials[scene.Instances.Data()[id].MaterialIndex];
                if ((material.Flags & 1u) && material.AlbedoTextureIndex != UINT32_MAX)
                {
                    textures.push_back(material.AlbedoTextureIndex);
                }
                if (material.NormalTextureIndex != UINT32_MAX)
                {
                    textures.push_back(material.NormalTextureIndex);
                }
            }
        }
        GPUDrivenRenderer::TrackEntityTextures(entity, textures);
    }
}
} // namespace

void GpuScene::Execute(ID3D12GraphicsCommandList* commands)
{
    ModelManager::UpdateVirtualGeometry();
    GPUDrivenValidation::Trace("GPU Scene begin");
    GpuSceneValidation::BeforeScene();
    const auto start = chrono::steady_clock::now();
    auto& scene = Scene();
    const uint32_t frame = GraphicsDevice::GetFrameIndex();
    scene.RetiredPrimitiveOwners[frame].clear(); // This frame slot's fence completed before Execute.
    auto* device = GraphicsDevice::GetDevice();
    scene.Instances.SettlePreviousTransforms();
    for (auto& [handle, record] : scene.AssetMaterials)
    {
        const auto& runtime = MaterialManager::GetRuntime(handle);
        if (record.PipelineRevision != runtime.PipelineRevision)
        {
            for (const auto entity : record.Users)
            {
                GpuSceneChanges::Notify(entity, GpuSceneDirtyFlags::Material);
            }
            record.PipelineRevision = runtime.PipelineRevision;
        }
    }
    auto pending = GpuSceneChanges::Take();
    bool occluderChanged = pending.ResetScene || !pending.Destroyed.empty();
    for (const auto& [entity, dirty] : pending.Changed)
    {
        if (scene.Entities.contains(entity) || ComponentManager::HasComponent<StaticModelComponent>(entity))
        {
            occluderChanged = true;
        }
    }
    if (occluderChanged)
    {
        GPUVisibility::Invalidate("Scene/occluder changed");
    }
    GPUVisibility::PrepareCamera();
    if (pending.ResetScene)
    {
        for (auto& [entity, ids] : scene.Entities)
        {
            GPUDrivenRenderer::TrackEntityTextures(entity, {});
            for (auto id : ids)
            {
                scene.Instances.Remove(id);
            }
        }
        scene.Entities.clear();
        scene.EntityAssets.clear();
        for (auto& [handle, record] : scene.AssetMaterials)
        {
            record.Users.clear();
        }
        scene.CpuOpaqueStatic.clear();
        scene.CpuOpaquePositions.clear();
    }
    for (uint32_t entity : pending.Destroyed)
    {
        RemoveEntity(entity);
    }
    auto changed = move(pending.Changed);
    for (const auto& [entity, flags] : changed)
    {
        UpdateEntity(entity, flags);
    }
    const bool geometryChanged = any_of(changed.begin(), changed.end(), [](const auto& change) {
        return (static_cast<uint32_t>(change.second) & static_cast<uint32_t>(GpuSceneDirtyFlags::Mesh)) != 0;
    });
    if (geometryChanged || pending.ResetScene || !pending.Destroyed.empty())
    {
        RetireUnusedPrimitiveMeshes();
    }
    scene.Stats.EntitiesProcessed = static_cast<uint32_t>(changed.size());
    scene.Stats.DirtyInstances = static_cast<uint32_t>(scene.Instances.Updates().size());
    scene.Stats.InstanceSlots = static_cast<uint32_t>(scene.Instances.Data().size());
    scene.Stats.ActiveInstances = scene.Instances.ActiveCount();
    scene.Stats.Meshes = static_cast<uint32_t>(scene.Meshes.size());
    for (auto& [handle, record] : scene.AssetMaterials)
    {
        const auto& runtime = MaterialManager::GetRuntime(handle);
        for (const auto descriptor : runtime.TextureDescriptors)
        {
            if (descriptor >= 0)
            {
                TextureManager::TouchTexture(descriptor);
            }
        }
        if (runtime.Revision != record.Revision)
        {
            scene.Materials[record.Index] = MaterialBindings::BuildGpuSceneMaterial(runtime.Binding);
            scene.NewMaterials.push_back(record.Index);
            record.Revision = runtime.Revision;
        }
    }
    scene.Stats.Materials = static_cast<uint32_t>(scene.Materials.size());
    scene.InstanceBuffer.BeginFrame(frame);
    scene.MeshBuffer.BeginFrame(frame);
    scene.MaterialBuffer.BeginFrame(frame);
    scene.InstanceBuffer.Ensure(device, scene.Stats.InstanceSlots, sizeof(GPUInstanceData), frame,
                                L"GPU Scene Instances");
    scene.MeshBuffer.Ensure(device, scene.Stats.Meshes, sizeof(GPUMeshData), frame, L"GPU Scene Meshes");
    scene.MaterialBuffer.Ensure(device, scene.Stats.Materials, sizeof(GPUMaterialData), frame, L"GPU Scene Materials");
    scene.Culling.Prepare(device, scene.Stats.InstanceSlots, frame);
    GPUDrivenValidation::Trace("GPU Scene culling pipeline ready");
    const uint64_t instanceBytes = scene.InstanceBuffer.Stage(device, frame, scene.Instances.Data().data(),
                                                              scene.Stats.InstanceSlots, scene.Instances.Updates());
    const uint64_t meshBytes =
        scene.MeshBuffer.Stage(device, frame, scene.Meshes.data(), scene.Stats.Meshes, scene.NewMeshes);
    const uint64_t materialBytes =
        scene.MaterialBuffer.Stage(device, frame, scene.Materials.data(), scene.Stats.Materials, scene.NewMaterials);
    scene.Stats.InstanceUploadedBytes = instanceBytes;
    scene.Stats.UploadedBytes = instanceBytes + meshBytes + materialBytes;
    RenderGraph graph;
    auto import = [&](const char* name, GpuSceneBuffer& buffer) {
        RenderGraph::ImportedResource resource{};
        resource.Name = name;
        resource.Resource = buffer.Resource();
        resource.InitialState = buffer.State;
        resource.FinalState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        resource.HasFinalState = true;
        return graph.ImportResource(resource);
    };
    const auto instances = import("GPU Scene Instances", scene.InstanceBuffer);
    const auto meshes = import("GPU Scene Meshes", scene.MeshBuffer);
    const auto materials = import("GPU Scene Materials", scene.MaterialBuffer);
    const auto visibility = import("GPU Scene Visibility", scene.Culling.Visibility);
    if (scene.Stats.UploadedBytes)
    {
        graph.AddPass(
            "GPU Scene Update",
            [&](RenderGraph::PassBuilder& builder) {
                if (instanceBytes)
                {
                    builder.Write(instances, D3D12_RESOURCE_STATE_COPY_DEST);
                }
                if (meshBytes)
                {
                    builder.Write(meshes, D3D12_RESOURCE_STATE_COPY_DEST);
                }
                if (materialBytes)
                {
                    builder.Write(materials, D3D12_RESOURCE_STATE_COPY_DEST);
                }
            },
            [&](ID3D12GraphicsCommandList* list) {
                RenderProfiler::ScopedEvent profile("GPU Scene Update", list);
                scene.InstanceBuffer.Copy(list, frame);
                scene.MeshBuffer.Copy(list, frame);
                scene.MaterialBuffer.Copy(list, frame);
            });
    }
    graph.AddPass(
        "GPU Scene Culling",
        [&](RenderGraph::PassBuilder& builder) {
            builder.Read(instances, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                .Read(meshes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                .Read(materials, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                .Write(visibility, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        },
        [&](ID3D12GraphicsCommandList* list) {
            scene.Culling.ExecuteCulling(list, scene.InstanceBuffer.Resource(), scene.MeshBuffer.Resource(),
                                         scene.MaterialBuffer.Resource(), scene.Stats.InstanceSlots, scene.Stats.Meshes,
                                         scene.Stats.Materials);
        });
    GPUDrivenRenderer::Prepare(graph, instances, visibility, scene.Stats.InstanceSlots, scene.InstanceBuffer.Capacity(),
                               scene.Stats.Meshes);
    GPUDrivenValidation::Trace("indirect pipeline ready");
    if (!graph.Compile() || !graph.Execute(commands))
    {
        throw runtime_error(graph.GetLastError());
    }
    scene.InstanceBuffer.State = scene.MeshBuffer.State = scene.MaterialBuffer.State =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    scene.Culling.Visibility.State = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    scene.Instances.ClearUpdates();
    scene.NewMeshes.clear();
    scene.NewMaterials.clear();
    scene.Stats.InstanceCapacity = scene.InstanceBuffer.Capacity();
    scene.Stats.BufferMemoryBytes = scene.InstanceBuffer.MemoryBytes() + scene.MeshBuffer.MemoryBytes() +
                                    scene.MaterialBuffer.MemoryBytes() + scene.Culling.Visibility.MemoryBytes();
    scene.Stats.UpdateCpuMilliseconds = chrono::duration<double, milli>(chrono::steady_clock::now() - start).count();
}

void GpuScene::Shutdown()
{
    GPUDrivenRenderer::Shutdown();
    Scene() = {};
}

uint32_t Entity::GetGPUInstanceID() const
{
    return IsValid() ? GpuScene::GetEntityInstance(m_Id) : InvalidGPUInstanceID;
}

const vector<GPUInstanceID>& GpuScene::GetEntityInstances(uint32_t entity)
{
    static const vector<GPUInstanceID> empty;
    const auto found = Scene().Entities.find(entity);
    return found == Scene().Entities.end() ? empty : found->second;
}

const vector<uint32_t>& GpuScene::CpuOpaqueStaticEntities()
{
    return Scene().CpuOpaqueStatic;
}

GPUInstanceID GpuScene::GetEntityInstance(uint32_t entity)
{
    const auto& ids = GetEntityInstances(entity);
    return ids.empty() ? InvalidGPUInstanceID : ids.front();
}

ID3D12Resource* GpuScene::InstanceBuffer()
{
    return Scene().InstanceBuffer.Resource();
}

ID3D12Resource* GpuScene::MeshBuffer()
{
    return Scene().MeshBuffer.Resource();
}

ID3D12Resource* GpuScene::MaterialBuffer()
{
    return Scene().MaterialBuffer.Resource();
}

ID3D12Resource* GpuScene::VisibilityBuffer()
{
    return Scene().Culling.Visibility.Resource();
}

const GpuSceneStatistics& GpuScene::Statistics()
{
    return Scene().Stats;
}

const GPUInstanceData* GpuScene::GetInstance(GPUInstanceID id)
{
    return Scene().Instances.IsActive(id) ? &Scene().Instances.Data()[id] : nullptr;
}

uint64_t GpuScene::InstanceHistoryGeneration(GPUInstanceID id)
{
    return Scene().Instances.Generation(id);
}

uint32_t GpuScene::ResolveEntity(GPUInstanceID id, uint64_t generation)
{
    const auto* instance = GetInstance(id);
    if (!instance || InstanceHistoryGeneration(id) != generation || !Registry::IsAlive(instance->Entity))
    {
        return UINT32_MAX;
    }
    const auto& destroyed = GpuSceneChanges::Pending().Destroyed;
    if (std::find(destroyed.begin(), destroyed.end(), instance->Entity) != destroyed.end())
    {
        return UINT32_MAX;
    }
    return instance->Entity;
}

void GpuScene::DrawDebug()
{
    const auto& stats = Statistics();
    ImGui::Text("Instances: %u / Active: %u / Capacity: %u", stats.InstanceSlots, stats.ActiveInstances,
                stats.InstanceCapacity);
    ImGui::Text("Meshes: %u / Materials: %u / Dirty: %u", stats.Meshes, stats.Materials, stats.DirtyInstances);
    ImGui::Text("Changed entities: %u / Upload: %llu bytes", stats.EntitiesProcessed, stats.UploadedBytes);
    ImGui::Text("Instance upload: %llu bytes / Memory: %.2f MiB", stats.InstanceUploadedBytes,
                stats.BufferMemoryBytes / 1048576.0);
    ImGui::Text("CPU update: %.3f ms", stats.UpdateCpuMilliseconds);
    for (const auto& pass : RenderProfiler::GetLatest().Passes)
    {
        if (pass.Name == "GPU Scene Update")
        {
            ImGui::Text("GPU update: %.3f ms", pass.GpuMs);
        }
    }
    if (ImGui::TreeNode("Instance / Entity / Mesh / Material"))
    {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(stats.InstanceSlots));
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                if (const auto* data = GetInstance(static_cast<GPUInstanceID>(i)))
                {
                    ImGui::Text("%u / %u / %u / %u", i, data->Entity, data->MeshIndex, data->MaterialIndex);
                }
            }
        }
        ImGui::TreePop();
    }
}
