// ColorManagement.hlsl - Shared constants and structures for color management shaders
#pragma once

// Display transform types (must match CPU-side ColorManagement::DisplayConfig)
#define DISPLAY_TRANSFORM_NONE     0
#define DISPLAY_TRANSFORM_OCIO_LUT 1
#define DISPLAY_TRANSFORM_SRGB     2
#define DISPLAY_TRANSFORM_ACES     3
#define DISPLAY_TRANSFORM_CUSTOM   4

// ColorManagementConstants buffer layout (register b0)
// uint4 TextureSize;        // x=width, y=height, z=mipLevel(0), w=unused
// uint4 DisplayConfig;      // x=displayTransformType, y=srgb, z=exposure*1000, w=gamma*1000
// float4 ExposureGamma;     // x=exposure, y=gamma, z=unused, w=unused
// float3 Lut3DSize;         // x=lutSize, y=1/lutSize, z=unused
// float4 Lut3DScaleOffset;  // x=scale, y=offset, z=unused, w=unused

// Resource bindings:
// Texture2D<float4> g_InputTexture : register(t0);
// Texture3D<float4> g_Lut3D : register(t1);
// Texture1D<float> g_Lut1D_R : register(t2);
// Texture1D<float> g_Lut1D_G : register(t3);
// Texture1D<float> g_Lut1D_B : register(t4);
// RWTexture2D<float4> g_OutputTexture : register(u0);