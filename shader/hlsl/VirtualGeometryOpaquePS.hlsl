#define main VirtualGeometryMaterialMain
#include "GPUDrivenOpaquePS.hlsl"
#undef main
cbuffer NativeVisualization : register(b6) { uint NativeLevelColors; }
GBufferOutput main(PSInput3D input, bool frontFace : SV_IsFrontFace)
{
    GBufferOutput output = VirtualGeometryMaterialMain(input,frontFace);
    if (NativeLevelColors != 0)
    {
        // Emit selected-level color directly, independent of texture/lighting.
        output.Color = float4(0,0,0,1);
        output.EmissionAO = float4(input.Diffuse.rgb,1);
    }
    return output;
}
