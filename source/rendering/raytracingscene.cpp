#include "pch.h"
#include "raytracingscene.h"
#include "materialsystem.h"
#include "world.h"
#include "modelmanager.h"
#include "sun.h"
#include "vertexdeformation.h"
#include "texturemanager.h"

namespace
{
void SetMaterial(RaytracingGeometry& geometry, EntityID entity, int modelTexture = -1)
{
    if (ComponentManager::HasComponent<MaterialComponent>(entity))
    {
        const auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
        geometry.Roughness = material.Roughness;
        geometry.Metallic = material.Metallic;
        if (material.UseTexture) geometry.TextureIndex = modelTexture >= 0 ? modelTexture : material.TextureID;
    }
    else geometry.TextureIndex = modelTexture;
    geometry.Texture = TextureManager::GetTextureResource(geometry.TextureIndex);
    if (geometry.Texture)
    {
        TextureManager::TouchTexture(geometry.TextureIndex);
        geometry.TextureResidentMip = TextureManager::GetTextureResidentMip(geometry.TextureIndex);
    }
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
        const auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
        return !MaterialSystem::IsTransparentMaterial(material);
    }
    return true;
}

template<typename Model>
void AppendModel(std::vector<RaytracingGeometry>& geometry, EntityID entity, Model* model, bool deforming)
{
    if (!model || !IsOpaqueCaster(entity))
    {
        return;
    }
    const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
    for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
    {
        const auto& mesh = model->GetMeshData(meshIndex);
        if (!mesh.VertexBuffer || !mesh.IndexBuffer || mesh.IndexCount < 3)
        {
            continue;
        }
        RaytracingGeometry entry;
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
        SetMaterial(entry, entity, mesh.TextureIndex);
        VertexDeformationView deformation;
        if (!deforming && VertexDeformation::TryResolveStaticMesh(entity, meshIndex, deformation))
        {
            entry.Vertices = deformation.CurrentResource;
            entry.VertexView = deformation.CurrentView;
            entry.VertexState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            entry.Deforming = true;
        }
        geometry.push_back(std::move(entry));
    }
}
}

std::vector<RaytracingGeometry> GatherRaytracingGeometry()
{
    std::vector<RaytracingGeometry> geometry;
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
        geometry.push_back(std::move(entry));
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
