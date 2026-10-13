#define SHADER_3D
#include "common.hlsl"

#ifdef SHADER_GPU_DRIVEN
#include "GPUDrivenOpaque.hlsli"
#else
Texture2D g_Texture : register(t0);
#endif
SamplerState g_SamplerState : register(s0);

GBufferOutput main(PSInput3D In, bool frontFace : SV_IsFrontFace)
{
#ifdef SHADER_GPU_DRIVEN
    uint materialID = gSceneInstances[In.InstanceID].MaterialIndex;
    uint address = materialID * GPU_SCENE_MATERIAL_STRIDE;
    uint4 metadata = gSceneMaterials.Load4(address + 32u);
    if ((metadata.z & 4u) == 0 && !frontFace) discard;
    int UseTexture = (metadata.z & 1u) != 0;
    int FlipNormal = 0;
    int ShaderClass = metadata.w;
    int MaterialMode = gSceneMaterials.Load(address + 52u);
    float4 surface = asfloat(gSceneMaterials.Load4(address + 16u));
    float mMetallic = surface.x;
    float mRoughness = surface.y;
    float mFresnel = surface.z;
    float ShadowThreshold = LoadSceneMaterialVector(gSceneMaterials, materialID, 1u).w;
    float ShadowSoftness = LoadSceneMaterialVector(gSceneMaterials, materialID, 2u).x;
    float ShadowStrength = LoadSceneMaterialVector(gSceneMaterials, materialID, 2u).y;
#endif
    float materialPartId = (MaterialMode == 1) ? (float)ShaderClass : max(In.Diffuse.a, 0.0f);
    float4 baseColor = float4(In.Diffuse.rgb, 1.0f);
    if (UseTexture != 0)
    {
#ifdef SHADER_GPU_DRIVEN
        baseColor *= gSceneTextures[NonUniformResourceIndex(SceneTextureIndex(metadata.x))].Sample(g_SamplerState, In.TexCoord);
#else
        baseColor *= g_Texture.Sample(g_SamplerState, In.TexCoord);
#endif
    }

    clip(baseColor.a - 0.01f);

    float3 normal = In.Normal.xyz * ((FlipNormal != 0) ? -1.0f : 1.0f);

    GBufferOutput output;
    output.Color = baseColor;
    output.Normal = MakeGBufferNormal(normal);



    output.Depth = saturate(In.Position.z);

#ifdef SHADER_GPU_DRIVEN
    MaterialPartShaderParams partParams = LoadGpuDrivenPart(materialID, materialPartId);
#else
    MaterialPartShaderParams partParams = ResolveMaterialPartParams(materialPartId);
#endif
    bool usePartParams = MaterialMode != 1;
    output.Material.r = usePartParams ? partParams.Basic.x : mMetallic;
    output.Material.g = usePartParams ? partParams.Basic.y : mRoughness;
    output.Material.b = usePartParams ? partParams.Basic.z : mFresnel;
    output.Material.a = materialPartId;
    output.Shadow.r = usePartParams ? partParams.Shadow0.x : ShadowThreshold;
    output.Shadow.g = usePartParams ? partParams.Shadow0.y : ShadowSoftness;
    output.Shadow.b = usePartParams ? partParams.Shadow0.z : ShadowStrength;
    output.Shadow.a = 1.0f;
    output.Color.a = 1.0f;
#ifdef SHADER_GPU_DRIVEN
    if (VisibilityDebugMode != 0)
    {
        uint4 visibility = gVisibilityDebug[In.InstanceID];
        const float3 palette[8] = {float3(0.1, 1, 0.2), float3(0.1, 0.5, 1), float3(1, 0.8, 0.1), float3(1, 0.3, 0.1),
            float3(0.7, 0.2, 1), float3(0.2, 1, 1), float3(1, 0.2, 0.7), float3(0.8, 0.8, 0.8)};
        output.Color.rgb = VisibilityDebugMode == 2 ? palette[min(visibility.z, 7u)] : float3(0.1, 1, 0.2);
    }
#endif
    return output;
}
