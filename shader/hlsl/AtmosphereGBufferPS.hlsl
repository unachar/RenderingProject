#define SHADER_POSTPROCESS
#define SHADER_3D
#include "common.hlsl"

// GBuffer inputs.  The range rooted at t0 supplies t0-t5; environment and
// shadow maps are supplied by the existing t6/t7 root tables.
Texture2D<float4> BaseColorTexture : register(t0);
Texture2D<float4> NormalTexture : register(t1);
Texture2D<float4> PositionTexture : register(t2);
Texture2D<float> DepthTexture : register(t3);
Texture2D<float4> MaterialTexture : register(t4);
Texture2D<float4> ShadowGBufferTexture : register(t5);
Texture2D<float4> EnvironmentTexture : register(t6);
Texture2DArray<float> ShadowMapTexture : register(t7);

SamplerState TextureSampler : register(s0);
SamplerState ShadowSampler : register(s1);

float4 main(PSInputPostProcess input) : SV_Target
{
    float4 position = PositionTexture.SampleLevel(TextureSampler, input.TexCoord, 0);
    float4 material = MaterialTexture.SampleLevel(TextureSampler, input.TexCoord, 0);
    bool background = material.a < -0.5f;

    // The dedicated buffer has no geometry dependency for empty pixels.  Sky
    // pixels use a camera ray, while geometry terminates the integration at
    // its actual world-space position.
    float3 viewRay = ReconstructPostProcessViewRayCommon(input.TexCoord);
    float3 rayEnd = (background || position.w <= 0.0001f)
        ? PPCameraPos.xyz + viewRay * 80.0f
        : position.xyz;
    float3 scatter = RayMarchAtmosphereViewCommon(
        rayEnd,
        ShadowMapTexture,
        ShadowSampler,
        LightViewProjection,
        ShadowMapParams);
    return float4(scatter, 1.0f);
}
