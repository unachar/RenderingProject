#pragma once
#include "systembase.h"
#include "ecs.h"

struct MaterialComponent;
enum class MaterialShadowMode : uint32_t;

class MaterialSystem : public SystemBase
{
public:
    void Update() override;

    static bool IsReceivingPostProcess(EntityID entity);
    static bool IsTransparentMaterial(const MaterialComponent& material);
    static bool CastsShadow(const MaterialComponent& material);
    static MaterialShadowMode ResolveShadowMode(const MaterialComponent& material);
    static bool CastsShadowInPass(const MaterialComponent& material, bool transmission);
    static bool IsTransparent(EntityID entity);
    static bool SetTexture(EntityID entity, const char* texturePath);
	static bool SetPBRParameters(EntityID entity, float metallic, float roughness, float fresnel);
};


