#pragma once
#include "materialasset.h"
struct ID3D12Resource;

class MaterialPreview
{
public:
    enum class Mesh { Sphere, Cube, Plane };
    static void SetVisible(bool visible);
    static void DrawControls();
    static void DrawImage(AssetHandle material);
    static void Render();
    static void Shutdown();
    static ID3D12Resource* GetTarget();
    static void SetMesh(Mesh mesh);
    static void SetLightIntensity(float intensity);
};

