#include "pch.h"
#include "materialmanager.h"
#include "assetthumbnailmanager.h"
#include "texturemanager.h"
#include "modelmanager.h"
#include "world.h"
#include "materialsystem.h"
#include "gpuscene/gpuscenechanges.h"
#include <json/json.h>
#include <fstream>

namespace
{
    unordered_map<AssetHandle, unique_ptr<MaterialRuntime>> runtimes;
    AssetRegistry registry;
    bool sceneHasAssets = false;
}

bool SceneHasMaterialAssets()
{
    return sceneHasAssets;
}

AssetRegistry& MaterialManager::Registry()
{
    return registry;
}

void MaterialManager::Init()
{
    Refresh();
}

void MaterialManager::Shutdown()
{
    runtimes.clear();
    sceneHasAssets = false;
}

void MaterialManager::Refresh()
{
    string error;
    registry.Refresh(error);
    if (!error.empty())
    {
        Debug::Log("Asset registry: %s\n", error.c_str());
    }
    for (auto& [handle, runtime] : runtimes)
    {
        const bool missing = registry.Resolve(handle).empty();
        if (missing != runtime->Missing)
        {
            runtime->Missing = missing;
            if (missing)
            {
                runtime->Error = "Missing material: " + FormatAssetHandle(handle);
                UpdateBinding(*runtime, MaterialAssetData{});
            }
            else
            {
                Revert(handle);
            }
        }
        // テクスチャの削除や移動では、保存済み GUID を変更せずにバインドを更新する。
        if (!missing)
        {
            UpdateBinding(*runtime, runtime->Editing);
        }
    }
}

MaterialRuntime& MaterialManager::GetRuntime(AssetHandle handle)
{
    const auto found = runtimes.find(handle);
    if (found != runtimes.end())
    {
        return *found->second;
    }
    auto runtime = make_unique<MaterialRuntime>();
    runtime->Handle = handle;
    const auto path = registry.Resolve(handle);
    if (handle != 0 && !path.empty() && registry.TypeOf(handle) == AssetType::Material)
    {
        MaterialSerializer::Load(path, runtime->Saved, runtime->Error);
    }
    else if (handle != 0)
    {
        runtime->Missing = true;
        runtime->Error = "Missing material: " + FormatAssetHandle(handle);
    }
    runtime->Editing = runtime->Saved;
    UpdateBinding(*runtime, runtime->Editing);
    auto& result = *runtime;
    runtimes.emplace(handle, move(runtime));
    return result;
}

const MaterialRuntime* MaterialManager::FindRuntime(AssetHandle handle)
{
    const auto found = runtimes.find(handle);
    return found == runtimes.end() ? nullptr : found->second.get();
}

MaterialComponent MaterialManager::BuildPreviewBinding(const MaterialAssetData& material,
                                                       const array<int, 5>& descriptors)
{
    MaterialComponent binding;
    binding.ShaderClassMode = MaterialMode::Manual;
    binding.ShaderClass = ShaderClass::PBR;
    binding.BaseColor = XMFLOAT4(material.BaseColor.data());
    binding.Metallic = material.Metallic;
    binding.Roughness = material.Roughness;
    binding.NormalScale = material.NormalScale;
    binding.AOStrength = material.AOStrength;
    binding.EmissiveColor = XMFLOAT3(material.EmissiveColor.data());
    binding.EmissiveIntensity = material.EmissiveIntensity;
    // 旧アセットが Opaque を保存していても、光学的な透過があれば透明描画経路を選ぶ。
    // アルファ値は独立した被覆率として扱う。
    const bool opticalTransmission = material.Transmission > 0.0f;
    binding.AssetSurface = opticalTransmission ? MaterialSurface::Transparent : material.Surface;
    binding.IsTransparent = binding.AssetSurface == MaterialSurface::Transparent;
    binding.AssetCull = material.Cull;
    binding.AssetDepthWrite = binding.IsTransparent ? false : material.DepthWrite;
    binding.AlphaCutoff = material.AlphaCutoff;
    binding.ShadowMode = material.ShadowMode;
    binding.ShadowCutoff = material.ShadowCutoff;
    binding.IOR = material.IOR;
    binding.Transmission = material.Transmission;
    binding.TransmissionRoughness = material.TransmissionRoughness;
    const float dielectric = (material.IOR - 1.0f) / (material.IOR + 1.0f);
    binding.Fresnel = dielectric * dielectric;
    binding.Thickness = material.Thickness;
    binding.ShadingModel = material.ShadingModel;
    binding.ShadingStrength = material.ShadingStrength;
    binding.AbsorptionCoefficient = {material.AbsorptionCoefficient[0], material.AbsorptionCoefficient[1],
                                     material.AbsorptionCoefficient[2]};
    binding.DoubleSided = material.Cull == MaterialCull::None;
    binding.BaseSaturation = 1.0f;
    binding.TextureID = descriptors[0];
    binding.UseTexture = binding.TextureID >= 0;
    binding.NormalMapID = descriptors[1];
    binding.MetallicRoughnessID = descriptors[2];
    binding.AOTextureID = descriptors[3];
    binding.EmissiveTextureID = descriptors[4];
    return binding;
}

void MaterialManager::UpdateBinding(MaterialRuntime& runtime, const MaterialAssetData& material)
{
    array<int, 5> descriptors;
    descriptors.fill(-1);
    for (size_t slot = 0; slot < descriptors.size(); ++slot)
    {
        const auto path = registry.Resolve(material.Textures[slot]);
        if (!path.empty() && registry.TypeOf(material.Textures[slot]) == AssetType::Texture)
        {
            const auto previousHandle = runtime.Editing.Textures[slot];
            const auto previousPath = runtime.TexturePaths[slot];
            if (previousHandle == material.Textures[slot] && previousPath == path &&
                runtime.TextureDescriptors[slot] >= 0)
            {
                descriptors[slot] = runtime.TextureDescriptors[slot];
            }
            else if (slot == static_cast<size_t>(MaterialTextureSlot::Normal))
            {
                descriptors[slot] = TextureManager::LoadNormalTexture(path.generic_string().c_str());
            }
            else
            {
                descriptors[slot] = TextureManager::LoadTexture(path);
            }
            runtime.TexturePaths[slot] = path;
        }
        else
        {
            runtime.TexturePaths[slot].clear();
        }
    }
    auto binding = BuildPreviewBinding(material, descriptors);
    binding.RuntimeHandle = runtime.Handle;
    // リビジョン変更時は、共有マテリアルレコードとそのフレーム定数バッファだけを無効化する。
    if (runtime.Revision == 0 || runtime.Binding.BaseColor.x != binding.BaseColor.x || runtime.BoundData != material ||
        runtime.TextureDescriptors != descriptors)
    {
        if (runtime.Revision == 0 || runtime.BoundData.Cull != material.Cull ||
            runtime.BoundData.Surface != material.Surface || runtime.BoundData.DepthWrite != material.DepthWrite ||
            runtime.Binding.IsTransparent != binding.IsTransparent ||
            (material.Surface == MaterialSurface::Masked && (runtime.BoundData.AlphaCutoff != material.AlphaCutoff ||
                                                             runtime.BoundData.BaseColor[3] != material.BaseColor[3] ||
                                                             runtime.BoundData.Textures[0] != material.Textures[0] ||
                                                             runtime.TextureDescriptors[0] != descriptors[0])))
        {
            ++runtime.PipelineRevision;
        }
        runtime.Binding = move(binding);
        runtime.BoundData = material;
        runtime.TextureDescriptors = descriptors;
        ++runtime.Revision;
    }
}

AssetHandle MaterialManager::SlotHandle(const MaterialComponent& material, size_t slot)
{
    if (slot >= material.MaterialSlots.size())
    {
        return 0;
    }
    return material.MaterialSlots[slot];
}

const MaterialComponent& MaterialManager::Resolve(const MaterialComponent& legacy, size_t slot)
{
    const auto handle = SlotHandle(legacy, slot);
    if (handle == 0)
    {
        return legacy;
    }
    return GetRuntime(handle).Binding;
}

bool MaterialManager::SetEditing(AssetHandle handle, const MaterialAssetData& editing)
{
    auto& runtime = GetRuntime(handle);
    if (!MaterialSerializer::Validate(editing, runtime.Error) || runtime.Missing)
    {
        return false;
    }
    UpdateBinding(runtime, editing);
    runtime.Editing = editing;
    runtime.Dirty = runtime.Editing != runtime.Saved;
    AssetThumbnailManager::MarkDirty(handle);
    return true;
}

bool MaterialManager::Save(AssetHandle handle)
{
    auto& runtime = GetRuntime(handle);
    const auto path = registry.Resolve(handle);
    if (path.empty())
    {
        runtime.Error = "Cannot save a missing material";
        return false;
    }
    if (!MaterialSerializer::Save(path, runtime.Editing, runtime.Error))
    {
        return false;
    }
    runtime.Saved = runtime.Editing;
    runtime.Dirty = false;
    AssetThumbnailManager::MarkDirty(handle);
    return true;
}

bool MaterialManager::Revert(AssetHandle handle)
{
    auto& runtime = GetRuntime(handle);
    MaterialAssetData loaded;
    if (!MaterialSerializer::Load(registry.Resolve(handle), loaded, runtime.Error))
    {
        return false;
    }
    runtime.Saved = loaded;
    runtime.Missing = false;
    return SetEditing(handle, loaded);
}

bool MaterialManager::IsRenderable(EntityID entity)
{
    return ::Registry::IsAlive(entity) && ComponentManager::HasComponent<MaterialComponent>(entity) &&
           (ComponentManager::HasComponent<StaticModelComponent>(entity) ||
            ComponentManager::HasComponent<AnimationModelComponent>(entity) ||
            ComponentManager::HasComponent<MeshComponent>(entity) ||
            (ComponentManager::HasComponent<SpriteComponent>(entity) &&
             ComponentManager::GetComponentUnchecked<SpriteComponent>(entity).Is3D));
}

void MaterialManager::Discard(AssetHandle handle)
{
    auto& runtime = GetRuntime(handle);
    runtime.Editing = runtime.Saved;
    runtime.Dirty = false;
    if (!runtime.Missing)
    {
        UpdateBinding(runtime, runtime.Saved);
    }
}

bool MaterialManager::HasSurfaceForPass(EntityID entity, bool transparent)
{
    if (!ComponentManager::HasComponent<MaterialComponent>(entity))
    {
        return !transparent;
    }
    const auto& source = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
    const auto count = max<size_t>(1, SlotCount(entity));
    for (size_t slot = 0; slot < count; ++slot)
    {
        if (MaterialSystem::IsTransparentMaterial(Resolve(source, slot)) == transparent)
        {
            return true;
        }
    }
    return false;
}

size_t MaterialManager::SlotCount(EntityID entity)
{
    if (!IsRenderable(entity))
    {
        return 0;
    }
    if (ComponentManager::HasComponent<StaticModelComponent>(entity))
    {
        const auto* model =
            ModelManager::GetStaticModel(ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity).ModelId);
        if (model)
        {
            return model->GetMeshCount();
        }
    }
    if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
    {
        const auto* model = ModelManager::GetAnimModel(
            ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity).ModelId);
        if (model)
        {
            return model->GetMeshCount();
        }
    }
    return 1;
}

bool MaterialManager::Assign(EntityID entity, size_t slot, AssetHandle handle)
{
    if (!IsRenderable(entity) || slot >= SlotCount(entity) ||
        (handle != 0 && registry.TypeOf(handle) != AssetType::Material))
    {
        return false;
    }
    auto& component = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
    component.MaterialSlots.resize(SlotCount(entity));
    component.MaterialSlots[slot] = handle;
    if (all_of(component.MaterialSlots.begin(), component.MaterialSlots.end(), [](AssetHandle assigned) {
            return assigned == 0;
        }))
    {
        component.MaterialSlots.clear();
    }
    NotifyAssignmentsChanged();
    GpuSceneChanges::Notify(entity, GpuSceneDirtyFlags::Material);
    return true;
}

void MaterialManager::NotifyAssignmentsChanged()
{
    sceneHasAssets = false;
    for (EntityID entity : World::GetView<MaterialComponent>())
    {
        const auto& slots = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity).MaterialSlots;
        if (any_of(slots.begin(), slots.end(), [](AssetHandle handle) {
                return handle != 0;
            }))
        {
            sceneHasAssets = true;
            return;
        }
    }
}
