#pragma once
#include "systembase.h"
#include "ecs.h"

struct MaterialComponent;

class MaterialSystem : public SystemBase
{
public:
    void Update() override;

    static bool IsReceivingPostProcess(EntityID entity);
    static bool IsTransparentMaterial(const MaterialComponent& material);
    static bool IsTransparent(EntityID entity);
    static bool SetTexture(EntityID entity, const char* texturePath);
	static bool SetPBRParameters(EntityID entity, float metallic, float roughness, float fresnel);
};


