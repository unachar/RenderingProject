#define SHADER_3D
#define SHADER_GPU_DRIVEN
#include "../shader/hlsl/common.hlsl"

float4 main(PSInput3D input) : SV_Target0
{
    return float4(input.Diffuse.rgb, 1);
}
