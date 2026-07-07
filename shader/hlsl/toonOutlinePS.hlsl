#define SHADER_3D
#include "common.hlsl"

float4 main(PSInput3D input) : SV_Target
{
    return input.Diffuse;
}
