#pragma once

#include "componentmanager.h"
#include "proceduralmath.h"

class ProceduralScene
{
  public:
    static EntityID GenerateTerrain(const Procedural::TerrainSettings& settings, EntityID target = g_kINVALID_ENTITY);
    static void UpdateMotion(float deltaTime);
    static void ResetMotion(EntityID entity);
    static bool GetLiveUpdate();
    static void SetLiveUpdate(bool enabled);
    static bool SavePreset(const char* path, const Procedural::TerrainSettings& settings);
    static bool LoadPreset(const char* path, Procedural::TerrainSettings& settings);
};
