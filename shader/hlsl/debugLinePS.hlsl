#define SHADER_DEBUG_LINE
#include "common.hlsl"

float4 main(PSInputDebugLine input) : SV_TARGET
{
    // 受け取った頂点カラーをそのまま出力
    return input.Color;
}
