// Alicia PBR toon vertex shader.

#include "pbrToonCommon.slang"

[shader("vertex")]
PSInput3D main(VSInput3D input)
{
    PSInput3D output;

    float4 worldPos = mul(float4(input.Position, 1.0f), g_Model.World);
    float4 viewPos = mul(worldPos, g_Model.View);

    output.Position = mul(viewPos, g_Model.Projection);
    output.Normal = safeNormalize(mul(input.Normal, (float3x3)g_Model.World), float3(0.0f, 1.0f, 0.0f));
    output.TexCoord = input.TexCoord;
    output.Diffuse = input.Diffuse;
    output.WorldPos = worldPos.xyz;
    output.ViewPos = viewPos.xyz;
    output.CameraPos = g_Model.CameraPos;

    return output;
}
