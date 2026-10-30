#define SHADER_3D
#define SHADER_GPU_DRIVEN
#include "common.hlsl"
#include "GpuSceneCommon.hlsli"
#include "GPUDrivenOpaque.hlsli"
#include "MaterialSurface.hlsli"

// Alpha-tested shadow depth for the VG dedicated shadow path. Mirrors the
// legacy ShadowMapPS semantics (shadow mode + cutoff from the packed PBR
// parameter vector 8: .y = ResolveShadowMode, .z = ShadowCutoff) using the
// bindless material data the mesh shader already runs with.
SamplerState g_SamplerState : register(s0);

struct ShadowPSInput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
    float Alpha : TEXCOORD1;
    nointerpolation uint MaterialIndex : TEXCOORD2;
};

void main(ShadowPSInput input)
{
    uint4 metadata = gSceneMaterials.Load4(input.MaterialIndex * GPU_SCENE_MATERIAL_STRIDE + 32u);
    AssetMaterialParameters material = LoadAssetMaterial(input.MaterialIndex);
    float opacity = saturate(input.Alpha);
    if (material.Surface.z > 0.5f)
    {
        opacity *= material.BaseColor.a;
    }
    else
    {
        // Runtime materials fold material alpha into the packed base color.
        opacity *= asfloat(gSceneMaterials.Load4(input.MaterialIndex * GPU_SCENE_MATERIAL_STRIDE)).a;
    }
    clip(opacity - (1.0f / 255.0f));
    float4 pbrPadding = LoadSceneMaterialVector(gSceneMaterials, input.MaterialIndex, 8u);
    int shadowMode = (int)round(pbrPadding.y);
    clip((float)shadowMode - 0.5f);
    clip(2.5f - (float)shadowMode);
    if (shadowMode == 2)
    {
        if ((metadata.z & 1u) != 0)
        {
            opacity *= LoadMaterialTexture(SceneTextureIndex(metadata.x)).Sample(g_SamplerState, input.TexCoord).a;
        }
        clip(opacity - max(pbrPadding.z, 1.0f / 255.0f));
    }
}
