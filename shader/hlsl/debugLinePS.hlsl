#define SHADER_DEBUG_LINE
#include "common.hlsl"

float4 main(PSInputDebugLine input) : SV_TARGET
{

    return input.Color;
}
