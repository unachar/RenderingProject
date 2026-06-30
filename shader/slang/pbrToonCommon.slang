// Shared Alicia PBR toon shader declarations.

struct ConstantBuffer3D
{
    float4x4 World;
    float4x4 View;
    float4x4 Projection;
    int UseTexture;
    int FlipNormal;
    float2 Padding3D;
    float3 CameraPos;
};

struct VSInput3D
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD;
    float4 Diffuse  : COLOR;
};

struct PSInput3D
{
    float4 Position  : SV_POSITION;
    float3 Normal    : NORMAL;
    float2 TexCoord  : TEXCOORD;
    float4 Diffuse   : COLOR;
    float3 WorldPos  : TEXCOORD1;
    float3 ViewPos   : TEXCOORD2;
    float3 CameraPos : TEXCOORD3;
};

ConstantBuffer<ConstantBuffer3D> g_Model : register(b0);

float3 safeNormalize(float3 value, float3 fallback)
{
    float lenSq = dot(value, value);
    return (lenSq > 1.0e-5f) ? value * rsqrt(lenSq) : fallback;
}
