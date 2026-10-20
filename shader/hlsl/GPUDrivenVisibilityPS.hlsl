#define SHADER_3D
#define SHADER_GPU_DRIVEN
#include "common.hlsl"
#include "GPUDrivenOpaque.hlsli"
SamplerState g_SamplerState : register(s0);
#include "MaterialSurface.hlsli"

uint4 main(PSInput3D input, uint primitive : SV_PrimitiveID, bool front : SV_IsFrontFace) : SV_Target0
{
    uint material = gSceneInstances[input.InstanceID].MaterialIndex;
    uint4 metadata = gSceneMaterials.Load4(material * GPU_SCENE_MATERIAL_STRIDE + 32u);
    if ((metadata.z & 4u) == 0 && !front)
    {
        discard;
    }
    AssetMaterialParameters asset = LoadAssetMaterial(material);
    float4 color = asset.Surface.z > .5 ? float4(1, 1, 1, 1) : float4(input.Diffuse.rgb, 1);
    if ((metadata.z & 1u) != 0)
    {
        color *= LoadMaterialTexture(SceneTextureIndex(metadata.x)).Sample(g_SamplerState, input.TexCoord);
    }
    if (asset.Surface.z > .5)
    {
        color *= asset.BaseColor;
        if ((asset.Textures.w & 3u) == 1u)
        {
            clip(color.a - asset.Surface.w);
        }
        else if ((asset.Textures.w & 3u) == 0u)
        {
            color.a = 1;
        }
    }
    clip(color.a - .01);
    return uint4(input.InstanceID, gVisibilityDebug[input.InstanceID].y, primitive, 0x40000000u | (front ? 1u : 0u));
}
