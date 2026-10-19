#include "pch.h"
#include "envflags.h"
#include "camera.h"
#include "temporalhistorycontext.h"
#include <type_traits>
#include <map>
#include "materialmanager.h"
#include "raytracingscene.h"
#include "materialsystem.h"
#include "world.h"
#include "modelmanager.h"
#include "sun.h"
#include "vertexdeformation.h"
#include "texturemanager.h"

namespace
{
void SetMaterial(RaytracingGeometry& geometry, EntityID entity, int modelTexture = -1, size_t slot = 0)
{
    geometry.MediumID = static_cast<UINT>(entity);
    if (ComponentManager::HasComponent<MaterialComponent>(entity))
    {
        const auto& material =
            MaterialManager::Resolve(ComponentManager::GetComponentUnchecked<MaterialComponent>(entity), slot);
        if (material.RuntimeHandle != 0)
        {
            geometry.BaseColor = material.BaseColor;
            geometry.Emission = {material.EmissiveColor.x, material.EmissiveColor.y, material.EmissiveColor.z,
                                 material.EmissiveIntensity};
            geometry.EmissionTexture = TextureManager::GetTextureResource(material.EmissiveTextureID);
            if (geometry.EmissionTexture)
            {
                TextureManager::TouchTexture(material.EmissiveTextureID);
                geometry.EmissionResidentMip = TextureManager::GetTextureResidentMip(material.EmissiveTextureID);
            }
        }
        geometry.IOR = material.IOR;
        geometry.Transmission = MaterialSystem::IsTransparentMaterial(material) ? material.Transmission : 0.0f;
        geometry.Absorption = material.AbsorptionCoefficient;
        geometry.Thickness = material.Thickness;
        geometry.TransmissionRoughness = material.TransmissionRoughness;
        geometry.Fresnel = material.Fresnel;
        geometry.Emission = material.RuntimeHandle != 0 ? XMFLOAT4{material.EmissiveColor.x, material.EmissiveColor.y,
                                                                   material.EmissiveColor.z, material.EmissiveIntensity}
                                                        : XMFLOAT4{0, 0, 0, 0};
        // Raster ignores primitive vertex color for material assets and ignores
        // AssetBaseColor for legacy vertex-colored draws. Preserve that rule.
        geometry.UsesAssetMaterial = material.RuntimeHandle != 0;
        geometry.BaseColor = geometry.UsesAssetMaterial ? material.BaseColor : XMFLOAT4{1, 1, 1, 1};
        geometry.BaseColor.w *= material.Alpha;
        geometry.Roughness = material.Roughness;
        geometry.Metallic = material.Metallic;
        geometry.CastShadow = MaterialSystem::CastsShadow(material);
        if (material.UseTexture)
        {
            geometry.TextureIndex = material.TextureID;
            if (material.RuntimeHandle == 0 && modelTexture >= 0)
            {
                geometry.TextureIndex = modelTexture;
            }
        }
    }
    else
    {
        geometry.TextureIndex = modelTexture;
    }
    geometry.Texture = TextureManager::GetTextureResource(geometry.TextureIndex);
    if (geometry.Texture)
    {
        TextureManager::TouchTexture(geometry.TextureIndex);
        geometry.TextureResidentMip = TextureManager::GetTextureResidentMip(geometry.TextureIndex);
    }
}

bool IsRayGeometryVisible(const MaterialComponent& material)
{
    float opacity = material.Alpha;
    if (material.RuntimeHandle != 0)
    {
        opacity *= material.BaseColor.w;
    }
    return opacity > 0.0f && (!MaterialSystem::IsTransparentMaterial(material) ||
                              MaterialSystem::CastsShadow(material) || material.Transmission > 0.001f);
}

bool IsOpaqueCaster(EntityID entity)
{
    if (ComponentManager::HasComponent<MeshComponent>(entity) &&
        ComponentManager::GetComponentUnchecked<MeshComponent>(entity).IsEnvironment)
    {
        return false;
    }
    if (ComponentManager::HasComponent<SunComponent>(entity) ||
        ComponentManager::HasComponent<SpriteComponent>(entity) ||
        ComponentManager::HasComponent<CameraComponent>(entity))
    {
        return false;
    }
    if (ComponentManager::HasComponent<MaterialComponent>(entity))
    {
        const auto& source = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
        const size_t count = max(size_t(1), source.MaterialSlots.size());
        for (size_t slot = 0; slot < count; ++slot)
        {
            if (IsRayGeometryVisible(MaterialManager::Resolve(source, slot)))
            {
                return true;
            }
        }
        return false;
    }
    return true;
}

template <typename Model>
void AppendModel(vector<RaytracingGeometry>& geometry, EntityID entity, Model* model, bool deforming)
{
    if (!model || !IsOpaqueCaster(entity))
    {
        return;
    }
    const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
    for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
    {
        const auto& mesh = model->GetMeshData(meshIndex);
        if (ComponentManager::HasComponent<MaterialComponent>(entity) &&
            !IsRayGeometryVisible(MaterialManager::Resolve(
                ComponentManager::GetComponentUnchecked<MaterialComponent>(entity), meshIndex)))
        {
            continue;
        }
        if (!mesh.VertexBuffer || !mesh.IndexBuffer || mesh.IndexCount < 3)
        {
            continue;
        }
        RaytracingGeometry entry;
        entry.SurfaceOwner = (static_cast<uint64_t>(entity) << 32) | (uint64_t(meshIndex) + 1);
        entry.Vertices = mesh.VertexBuffer;
        entry.Indices = mesh.IndexBuffer;
        entry.VertexView = mesh.VertexBufferView;
        entry.IndexView = mesh.IndexBufferView;
        entry.IndexCount = mesh.IndexCount;
        entry.Deforming = deforming;
        entry.VertexState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        if (deforming)
        {
            // ModelSystem restores skinned output to UAV after each draw pass.
            entry.VertexState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        entry.IndexState = D3D12_RESOURCE_STATE_INDEX_BUFFER;
        D3D12_HEAP_PROPERTIES heapProperties{};
        if (SUCCEEDED(entry.Vertices->GetHeapProperties(&heapProperties, nullptr)) &&
            heapProperties.Type == D3D12_HEAP_TYPE_UPLOAD)
        {
            entry.VertexState = D3D12_RESOURCE_STATE_GENERIC_READ;
        }
        if (SUCCEEDED(entry.Indices->GetHeapProperties(&heapProperties, nullptr)) &&
            heapProperties.Type == D3D12_HEAP_TYPE_UPLOAD)
        {
            entry.IndexState = D3D12_RESOURCE_STATE_GENERIC_READ;
        }
        entry.World = transform.WorldMatrix;
        SetMaterial(entry, entity, mesh.TextureIndex, meshIndex);
        VertexDeformationView deformation;
        if (!deforming && VertexDeformation::TryResolveStaticMesh(entity, meshIndex, deformation))
        {
            entry.Vertices = deformation.CurrentResource;
            entry.VertexView = deformation.CurrentView;
            entry.VertexState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            entry.Deforming = true;
        }
        if constexpr (std::is_same_v<std::decay_t<decltype(mesh)>, StaticMeshData>)
        {
            static const bool rayLod = ReadEnvironmentFlagOnce("DX12_DXR_LOD"),
                              rayProxy = ReadEnvironmentFlagOnce("DX12_DXR_PROXY");
            const auto camera = Camera::GetCameraEntity();
            if (entry.Transmission <= 0.001f && !entry.Deforming && (rayLod || rayProxy) &&
                ComponentManager::HasComponent<TransformComponent>(camera))
            {
                const auto& eye = ComponentManager::GetComponentUnchecked<TransformComponent>(camera).Position;
                const XMMATRIX world = XMLoadFloat4x4(&entry.World);
                const float distance = XMVectorGetX(XMVector3Length(
                    XMVector3TransformCoord(XMLoadFloat3(&mesh.RayCenter), world) - XMLoadFloat3(&eye)));
                const float scale =
                    (std::max)({XMVectorGetX(XMVector3Length(world.r[0])), XMVectorGetX(XMVector3Length(world.r[1])),
                                XMVectorGetX(XMVector3Length(world.r[2]))});
                const bool important = entry.Roughness < .2f || entry.Emission.w > .001f ||
                                       mesh.RayRadius * scale / (std::max)(distance, 1.f) > .08f;
                UINT lod = rayLod ? (distance > 140 ? 3 : distance > 70 ? 2 : distance > 35 ? 1 : 0) : 0;
                if (important)
                {
                    lod = 0;
                }
                using Key = tuple<EntityID, UINT, UINT64>;
                static map<Key, UINT> previous;
                const Key key(entity, meshIndex, mesh.VertexBufferView.BufferLocation);
                const auto old = previous.find(key);
                if (!important && old != previous.end() && old->second <= 3 && old->second != lod)
                {
                    const UINT level = (std::max)(lod, old->second);
                    if (level)
                    {
                        const float boundary = 35.f * float(1u << (level - 1));
                        if (abs(distance - boundary) < boundary * .12f)
                        {
                            lod = old->second;
                        }
                    }
                }
                UINT selected = 0;
                if (rayProxy && !important && distance > 200 && mesh.RayProxyVertices && mesh.RayProxyIndices)
                {
                    entry.Vertices = mesh.RayProxyVertices;
                    entry.Indices = mesh.RayProxyIndices;
                    entry.VertexView = mesh.RayProxyVertexView;
                    entry.IndexView = mesh.RayProxyIndexView;
                    entry.VertexState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
                    entry.IndexState = D3D12_RESOURCE_STATE_INDEX_BUFFER;
                    entry.IndexCount = 36;
                    entry.Texture.Reset();
                    entry.TextureIndex = -1;
                    entry.EmissionTexture.Reset();
                    selected = 4;
                }
                else if (lod && mesh.RayLodBuffers[lod - 1])
                {
                    entry.Indices = mesh.RayLodBuffers[lod - 1];
                    entry.IndexView = mesh.RayLodViews[lod - 1];
                    entry.IndexCount = mesh.RayLodCounts[lod - 1];
                    entry.IndexState = D3D12_RESOURCE_STATE_INDEX_BUFFER;
                    selected = lod;
                }
                if (old != previous.end() && old->second != selected)
                {
                    TemporalHistory::Context::NotifyLodChanged();
                }
                if (previous.size() > 65536)
                {
                    previous.clear();
                }
                previous[key] = selected;
            }
        }
        geometry.push_back(move(entry));
    }
}
} // namespace

vector<RaytracingGeometry> GatherRaytracingGeometry()
{
    vector<RaytracingGeometry> geometry;
    geometry.reserve(World::GetView<MeshComponent>().size());
    for (EntityID entity : World::GetView<MeshComponent, TransformComponent>())
    {
        if (!IsOpaqueCaster(entity))
        {
            continue;
        }
        const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
        if (!mesh.VertexBuffer || mesh.VertexCount < 3)
        {
            continue;
        }
        RaytracingGeometry entry;
        entry.SurfaceOwner = (static_cast<uint64_t>(entity) << 32) | 0x80000000ull;
        entry.Vertices = mesh.VertexBuffer;
        entry.VertexView = mesh.VertexBufferView;
        entry.World = ComponentManager::GetComponentUnchecked<TransformComponent>(entity).WorldMatrix;
        SetMaterial(entry, entity);
        VertexDeformationView deformation;
        if (VertexDeformation::TryResolveMesh(entity, deformation))
        {
            entry.Vertices = deformation.CurrentResource;
            entry.VertexView = deformation.CurrentView;
            entry.VertexState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            entry.Deforming = true;
        }
        geometry.push_back(move(entry));
    }
    for (EntityID entity : World::GetView<StaticModelComponent, TransformComponent>())
    {
        const auto& component = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
        AppendModel(geometry, entity, ModelManager::GetStaticModel(component.ModelId), false);
    }
    for (EntityID entity : World::GetView<AnimationModelComponent, TransformComponent>())
    {
        const auto& component = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
        AppendModel(geometry, entity, ModelManager::GetAnimModel(component.ModelId), true);
    }
    return geometry;
}
