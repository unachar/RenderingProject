// インポスターのベイク（エントリ：BakeVS / BakePS）。ImpostorRenderer が実行時にコンパイルする。
// 1フレーム = 正射影で境界球を撮影したタイル。出力：
//   SV_Target0 (R8G8B8A8_UNORM_SRGB) : アルベド RGB + 被覆率
//   SV_Target1 (R8G8B8A8_UNORM)      : ローカル法線（オクタヘドラル RG）+ 深度（B）+ マテリアルパーツ ID / 15（A）
#include "ImpostorMath.hlsli"

cbuffer ImpostorBakeConstants : register(b0)
{
    float4 BakeBounds; // xyz: ローカル中心, w: 半径
    float4 BakeFrame;  // xy: フレームセル, z: 1辺のフレーム数, w: 上半球のみ
    float4 BakeFlags;  // x: テクスチャを使う
};

Texture2D BakeTexture : register(t0);
SamplerState BakeSampler : register(s0);

struct BakeVSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD;
    float4 Diffuse : COLOR;
};

struct BakePSInput
{
    float4 Position : SV_POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD;
    float4 Diffuse : COLOR;
};

BakePSInput BakeVS(BakeVSInput input)
{
    float3 direction = ImpostorFrameDirection(BakeFrame.xy, BakeFrame.z, BakeFrame.w > 0.5f);
    float3 right;
    float3 up;
    ImpostorFrameBasis(direction, right, up);
    float radius = max(BakeBounds.w, 1e-4f);
    float3 p = input.Position - BakeBounds.xyz;

    BakePSInput output;
    // 深度 0 = カメラ側の境界球の接平面、1 = 奥側。
    output.Position = float4(dot(p, right) / radius, dot(p, up) / radius,
                             saturate((radius - dot(p, direction)) / (2.0f * radius)), 1.0f);
    output.Normal = input.Normal;
    output.TexCoord = input.TexCoord;
    output.Diffuse = input.Diffuse;
    return output;
}

struct BakeOutput
{
    float4 Albedo : SV_Target0;
    float4 NormalDepth : SV_Target1;
};

BakeOutput BakePS(BakePSInput input)
{
    // 通常の不透明パス（modelshaderPS_MRT / VisibilityPS）と同じ規則：頂点色 RGB × テクスチャ、alpha 0.01 で切り抜き。
    float4 color = float4(input.Diffuse.rgb, 1.0f);
    if (BakeFlags.x > 0.5f)
    {
        color *= BakeTexture.Sample(BakeSampler, input.TexCoord);
    }
    clip(color.a - 0.01f);

    float lengthSq = dot(input.Normal, input.Normal);
    float3 normal = lengthSq > 1e-12f ? input.Normal * rsqrt(lengthSq) : float3(0.0f, 1.0f, 0.0f);
    float part = clamp(round(max(input.Diffuse.a, 0.0f)), 0.0f, 15.0f);

    BakeOutput output;
    output.Albedo = float4(color.rgb, 1.0f);
    output.NormalDepth = float4(ImpostorOctEncode(normal) * 0.5f + 0.5f, input.Position.z, part / 15.0f);
    return output;
}
