#include "Common.hlsl"
#include "ColorManagement.hlsl"

RWTexture2D<float4> g_OutputTexture : register(u0);
Texture2D<float4> g_InputTexture : register(t0);

cbuffer ColorManagementConstants : register(b0)
{
    uint4 TextureSize;        // x=width, y=height, z=mipLevel(0), w=unused
    uint4 DisplayConfig;      // x=displayTransformType, y=srgb, z=exposure*1000, w=gamma*1000
    float4 ExposureGamma;     // x=exposure, y=gamma, z=unused, w=unused
    float3 Lut3DSize;         // x=lutSize, y=1/lutSize, z=unused
    float4 Lut3DScaleOffset;  // x=scale, y=offset, z=unused, w=unused
};

Texture3D<float4> g_Lut3D : register(t1);
Texture1D<float> g_Lut1D_R : register(t2);
Texture1D<float> g_Lut1D_G : register(t3);
Texture1D<float> g_Lut1D_B : register(t4);

SamplerState g_SamplerLinearClamp : register(s0);
SamplerState g_SamplerPointClamp : register(s1);

#define DISPLAY_TRANSFORM_NONE     0
#define DISPLAY_TRANSFORM_OCIO_LUT 1
#define DISPLAY_TRANSFORM_SRGB     2
#define DISPLAY_TRANSFORM_ACES     3
#define DISPLAY_TRANSFORM_CUSTOM   4

// Helper function for ACES RRT + ODT fit
float RRTAndODTFit(float v)
{
    float a = v * (v + 0.0245786f) - 0.000090537f;
    float b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return max(0.0f, a / b);
}

[numthreads(16, 16, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= TextureSize.x || dispatchId.y >= TextureSize.y)
    {
        return;
    }

    float4 color = g_InputTexture.Load(uint3(dispatchId.x, dispatchId.y, 0));

    uint transformType = DisplayConfig.x;
    bool srgb = DisplayConfig.y != 0;
    float exposure = DisplayConfig.z * 0.001f;
    float gamma = DisplayConfig.w * 0.001f;

    // Apply exposure
    if (exposure != 0.0f)
    {
        float expScale = exp2(exposure);
        color.rgb *= expScale;
    }

    float3 result = color.rgb;

    [branch]
    if (transformType == DISPLAY_TRANSFORM_OCIO_LUT)
    {
        // Apply 3D LUT from OCIO
        if (Lut3DSize.x > 0)
        {
            float3 lutCoord = result * Lut3DScaleOffset.x + Lut3DScaleOffset.y;
            lutCoord = clamp(lutCoord, 0.0f, 1.0f);
            result = g_Lut3D[lutCoord].rgb;
        }
        else if (Lut3DSize.x > 0)
        {
            // Fallback to 1D LUTs
            result.r = g_Lut1D_R[clamp(uint(result.r * (Lut3DSize.x - 1)), 0u, Lut3DSize.x - 1)];
            result.g = g_Lut1D_G[clamp(uint(result.g * (Lut3DSize.x - 1)), 0u, Lut3DSize.x - 1)];
            result.b = g_Lut1D_B[clamp(uint(result.b * (Lut3DSize.x - 1)), 0u, Lut3DSize.x - 1)];
        }
    }
    else if (transformType == DISPLAY_TRANSFORM_SRGB)
    {
        // sRGB OETF
        result.r = (result.r <= 0.0031308f) ? (12.92f * result.r) : (1.055f * pow(result.r, 1.0f / 2.4f) - 0.055f);
        result.g = (result.g <= 0.0031308f) ? (12.92f * result.g) : (1.055f * pow(result.g, 1.0f / 2.4f) - 0.055f);
        result.b = (result.b <= 0.0031308f) ? (12.92f * result.b) : (1.055f * pow(result.b, 1.0f / 2.4f) - 0.055f);
    }
    else if (transformType == DISPLAY_TRANSFORM_ACES)
    {
        // ACES Filmic tone mapping (simplified)
        float3 acesInputMat[3] = {
            float3(0.59719f, 0.35458f, 0.04823f),
            float3(0.07600f, 0.90834f, 0.01566f),
            float3(0.02840f, 0.13383f, 0.83777f)
        };

        float3 acesOutputMat[3] = {
            float3(1.60475f, -0.53108f, -0.07367f),
            float3(-0.10208f, 1.10813f, -0.00605f),
            float3(-0.00327f, -0.07276f, 1.07602f)
        };

        // Apply exposure
        float3 colorExp = result * exp2(ExposureGamma.x);

        // ACES input transform
        float3 acesColor;
        acesColor.r = dot(acesInputMat[0], colorExp);
        acesColor.g = dot(acesInputMat[1], colorExp);
        acesColor.b = dot(acesInputMat[2], colorExp);

        // RRT + ODT (simplified)
        acesColor.r = RRTAndODTFit(acesColor.r);
        acesColor.g = RRTAndODTFit(acesColor.g);
        acesColor.b = RRTAndODTFit(acesColor.b);

        // ACES output transform
        result.r = dot(acesOutputMat[0], acesColor);
        result.g = dot(acesOutputMat[1], acesColor);
        result.b = dot(acesOutputMat[2], acesColor);
    }

    // Apply gamma correction if not using sRGB/ACES
    if (transformType != DISPLAY_TRANSFORM_SRGB && transformType != DISPLAY_TRANSFORM_ACES && gamma > 0.0f)
    {
        float invGamma = 1.0f / gamma;
        result.r = pow(max(0.0f, result.r), invGamma);
        result.g = pow(max(0.0f, result.g), invGamma);
        result.b = pow(max(0.0f, result.b), invGamma);
    }

    // Clamp to valid range
    result = saturate(result);

    g_OutputTexture[dispatchId.xy] = float4(result, color.a);
}