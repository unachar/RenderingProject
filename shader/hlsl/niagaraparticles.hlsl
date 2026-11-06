cbuffer Camera : register(b0)
{
    row_major float4x4 ViewProjection;
};

struct VertexInput
{
    float3 Position : POSITION;
    float4 Color : COLOR;
    float2 UV : TEXCOORD0;
    float Round : TEXCOORD1;
};

struct PixelInput
{
    float4 Position : SV_POSITION;
    float4 Color : COLOR;
    float2 UV : TEXCOORD0;
    float Round : TEXCOORD1;
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    output.Position = mul(float4(input.Position, 1), ViewProjection);
    output.Color = input.Color;
    output.UV = input.UV;
    output.Round = input.Round;
    return output;
}

float4 PSMain(PixelInput input) : SV_TARGET
{
    float alpha = input.Color.a;
    if (input.Round > 0.5)
    {
        float radius = length(input.UV * 2 - 1);
        alpha *= 1 - smoothstep(0.65, 1.0, radius);
    }
    clip(alpha - 0.001);
    return float4(input.Color.rgb, alpha);
}
