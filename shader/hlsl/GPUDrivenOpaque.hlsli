#include "GpuSceneCommon.hlsli"
StructuredBuffer<GPUInstanceData> gSceneInstances : register(t30);
ByteAddressBuffer gSceneMaterials : register(t31);

cbuffer IndirectInstance : register(b4)
{
    uint GPUInstanceID;
}

#include "MaterialTextureHeap.hlsli"

cbuffer TextureLimits : register(b5)
{
    uint TextureBase;
    uint DefaultTexture;
    uint VisibilityDebugMode;
    uint MaterialRate;
}

StructuredBuffer<uint4> gVisibilityDebug : register(t32);

uint SceneTextureIndex(uint heapIndex)
{
    return heapIndex >= TextureBase && heapIndex - TextureBase < 512u ? heapIndex - TextureBase : DefaultTexture;
}

MaterialPartShaderParams LoadGpuDrivenPart(uint material, float partID)
{
    uint first = 11u + (uint)clamp((int)round(partID), 0, 14) * 9u;
    MaterialPartShaderParams part;
    part.Basic = LoadSceneMaterialVector(gSceneMaterials, material, first);
    part.Base = LoadSceneMaterialVector(gSceneMaterials, material, first + 1);
    part.Shadow0 = LoadSceneMaterialVector(gSceneMaterials, material, first + 2);
    part.Shadow1 = LoadSceneMaterialVector(gSceneMaterials, material, first + 3);
    part.Highlight = LoadSceneMaterialVector(gSceneMaterials, material, first + 4);
    part.RimStyle = LoadSceneMaterialVector(gSceneMaterials, material, first + 5);
    part.RimLight = LoadSceneMaterialVector(gSceneMaterials, material, first + 6);
    part.Skin0 = LoadSceneMaterialVector(gSceneMaterials, material, first + 7);
    part.Skin1 = LoadSceneMaterialVector(gSceneMaterials, material, first + 8);
    return part;
}
