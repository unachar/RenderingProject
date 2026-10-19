#define main VirtualGeometryMaterialMain
#include "GPUDrivenOpaquePS.hlsl"
#undef main

cbuffer NativeVisualization : register(b6)
{
    uint NativeLevelColors;
}

GBufferOutput main(PSInput3D input, bool frontFace : SV_IsFrontFace)
{
    GBufferOutput output = VirtualGeometryMaterialMain(input, frontFace);
    if (NativeLevelColors != 0)
    {
        // テクスチャやライティングと独立して、選択レベルの色を直接出力する。
        output.Color = float4(0, 0, 0, 1);
        output.EmissionAO = float4(input.Diffuse.rgb, 1);
    }
    return output;
}
