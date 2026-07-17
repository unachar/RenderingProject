#define SHADER_3D
#include "common.hlsl"
#include "VisibilityCommon.hlsl"

uint4 main(PSInput3D input) : SV_Target0
{
    return PackVisibilityPayload(
        input.Diffuse.rgb,
        input.Normal,
        saturate(input.Position.z),
        0.0f,
        0.8f,
        0.02f,
        (float)ShaderClass,
        0.5f,
        0.05f,
        1.0f);
}
