#include "pch.h"
#include "materialmanager.h"
#include "materialsystem.h"
#include "componentmanager.h"
#include "modelmanager.h"
#include "texturemanager.h"
#include "world.h"

bool MaterialSystem::IsTransparentMaterial(const MaterialComponent& material)
{
    if (material.IsTransparent || material.Alpha < 0.999f)
    {
        return true;
    }

    if (material.ShaderClassMode != MaterialMode::Manual)
    {
        return false;
    }

    return material.ShaderClass == ShaderClass::Transparent || material.ShaderClass == ShaderClass::BTDF ||
           material.ShaderClass == ShaderClass::BSDF;
}

bool MaterialSystem::IsTransparent(EntityID entity)
{
    if (entity == g_kINVALID_ENTITY || !Registry::IsAlive(entity) ||
        !ComponentManager::HasComponent<MaterialComponent>(entity))
    {
        return false;
    }
    return IsTransparentMaterial(
        MaterialManager::Resolve(ComponentManager::GetComponentUnchecked<MaterialComponent>(entity)));
}

bool MaterialSystem::CastsShadow(const MaterialComponent& material)
{
    float opacity = material.Alpha;
    if (material.RuntimeHandle != 0)
    {
        opacity *= material.BaseColor.w;
    }
    return opacity > 0.0f && material.ShadowMode != MaterialShadowMode::None &&
           !(material.ShaderClassMode == MaterialMode::Manual && material.ShaderClass == ShaderClass::Shadow);
}

MaterialShadowMode MaterialSystem::ResolveShadowMode(const MaterialComponent& material)
{
    if (material.ShadowMode != MaterialShadowMode::Auto)
    {
        return material.ShadowMode;
    }
    if (material.AssetSurface == MaterialSurface::Masked)
    {
        return MaterialShadowMode::AlphaTest;
    }
    return IsTransparentMaterial(material) ? MaterialShadowMode::Transmittance : MaterialShadowMode::Opaque;
}

bool MaterialSystem::CastsShadowInPass(const MaterialComponent& material, bool transmission)
{
    return CastsShadow(material) && (ResolveShadowMode(material) == MaterialShadowMode::Transmittance) == transmission;
}

bool MaterialSystem::IsReceivingPostProcess(EntityID entity)
{
    if (ComponentManager::HasComponent<SpriteComponent>(entity))
    {
        auto& sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(entity);
        if (!sprite.Is3D && !sprite.UsePostProcess)
        {
            return false;
        }
    }

    if (ComponentManager::HasComponent<MaterialComponent>(entity))
    {
        return ComponentManager::GetComponentUnchecked<MaterialComponent>(entity).ReceivingPostProcess;
    }

    return true;
}

bool MaterialSystem::SetTexture(EntityID entity, const char* texturePath)
{
    if (entity == g_kINVALID_ENTITY || !Registry::IsAlive(entity))
    {
        Debug::Log("ERROR: SetTexture failed. Invalid or dead entity (id=%u)\n", entity);
        return false;
    }

    if (!ComponentManager::HasComponent(entity, ComponentType::MATERIAL))
    {
        ComponentManager::AddComponent(entity, ComponentType::MATERIAL);
    }

    GpuSceneChanges::Notify(entity, GpuSceneDirtyFlags::Material);
    auto& mat = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);

    if (!texturePath || texturePath[0] == '\0')
    {
        mat.TexturePath.clear();
        mat.TextureID = TextureManager::GetDefaultTextureIndex();
        mat.UseTexture = false;
        return true;
    }

    mat.TexturePath = texturePath;
    mat.TextureID = TextureManager::LoadTexture(texturePath);
    if (mat.TextureID < 0)
    {
        Debug::Log("ERROR: SetTexture failed to load: %s (entity=%u)\n", texturePath, entity);
        mat.TextureID = TextureManager::GetErrorTextureIndex();
    }
    mat.UseTexture = true;
    return true;
}

bool MaterialSystem::SetPBRParameters(EntityID entity, float metallic, float roughness, float fresnel)
{
    if (entity == g_kINVALID_ENTITY || !Registry::IsAlive(entity))
    {
        Debug::Log("ERROR: SetPBRParameters failed. Invalid or dead entity (id=%u)\n", entity);
        return false;
    }

    if (!ComponentManager::HasComponent(entity, ComponentType::MATERIAL))
    {
        ComponentManager::AddComponent(entity, ComponentType::MATERIAL);
    }

    GpuSceneChanges::Notify(entity, GpuSceneDirtyFlags::Material);
    auto& mat = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
    mat.Metallic = clamp(metallic, 0.0f, 1.0f);
    mat.Roughness = clamp(roughness, 0.0f, 1.0f);
    mat.Fresnel = clamp(fresnel, 0.0f, 1.0f);
    return true;
}

void MaterialSystem::Update()
{
    auto resolveStaticModelTexture = [&](EntityID i) {
        auto& mat = ComponentManager::GetComponentUnchecked<MaterialComponent>(i);
        if (mat.TextureID >= 0)
        {
            return;
        }

        GpuSceneChanges::Notify(i, GpuSceneDirtyFlags::Material);
        StaticModelResource* model =
            ModelManager::GetStaticModel(ComponentManager::GetComponentUnchecked<StaticModelComponent>(i).ModelId);
        if (model)
        {
            if (!model->GetMaterialPath().empty())
            {
                mat.TexturePath = model->GetMaterialPath();
                mat.TextureID = TextureManager::LoadTexture(mat.TexturePath.c_str());
                if (mat.TextureID < 0)
                {
                    mat.TextureID = TextureManager::GetErrorTextureIndex();
                }
            }
            else
            {
                mat.TextureID = TextureManager::GetDefaultTextureIndex();
            }
        }

        if (mat.TextureID < 0)
        {
            mat.TextureID = TextureManager::GetErrorTextureIndex();
        }
    };

    auto resolveAnimModelTexture = [&](EntityID i) {
        auto& mat = ComponentManager::GetComponentUnchecked<MaterialComponent>(i);
        if (mat.TextureID >= 0)
        {
            return;
        }

        GpuSceneChanges::Notify(i, GpuSceneDirtyFlags::Material);
        AnimationModelResource* model =
            ModelManager::GetAnimModel(ComponentManager::GetComponentUnchecked<AnimationModelComponent>(i).ModelId);
        if (model && model->GetMeshCount() > 0)
        {
            mat.TextureID = model->GetMeshData(0).TextureIndex;
            if (mat.TextureID < 0)
            {
                mat.TextureID = TextureManager::GetErrorTextureIndex();
            }
        }

        if (mat.TextureID < 0)
        {
            mat.TextureID = TextureManager::GetErrorTextureIndex();
        }
    };

    auto staticMaterialEntities = World::GetView<MaterialComponent, StaticModelComponent>();
    for (EntityID i : staticMaterialEntities)
    {
        resolveStaticModelTexture(i);
    }

    auto animMaterialEntities = World::GetView<MaterialComponent, AnimationModelComponent>();
    for (EntityID i : animMaterialEntities)
    {
        resolveAnimModelTexture(i);
    }

    auto materialEntities = World::GetView<MaterialComponent>();
    for (EntityID i : materialEntities)
    {
        auto& mat = ComponentManager::GetComponentUnchecked<MaterialComponent>(i);
        if (mat.TextureID < 0)
        {
            GpuSceneChanges::Notify(i, GpuSceneDirtyFlags::Material);
            mat.TextureID = TextureManager::GetErrorTextureIndex();
        }
    }
}
