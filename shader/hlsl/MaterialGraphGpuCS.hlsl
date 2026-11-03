// =====================================================================================================
// マテリアルグラフの GPU 評価（エディター用）
//  ・ノードプレビュー（平面 / 球）と Material Output のプレビュー（簡易 PBR）
//  ・テクスチャへのベイク（Base Color / Normal / Metallic Roughness / AO / Emissive / Height）
//  ・Convert To Texture / Convert To Solid の「作成」（ノード単体を UV 0〜1 で焼く）
//  ・発光の動的ベイク（モード 5）：カメラ距離 / 時刻で変わる Emissive を、オブジェクトごとに毎フレーム焼く。
//    GI / 反射 / Lumen のサーフェスキャッシュが読む発光テクスチャ（RayReflectionsCS の SampleEmission）になる
// エンジンの描画と同じ命令列（MaterialGraphVM.hlsli）を使うので、プレビュー・ベイク・シーンの結果が一致する。
// 実行時に DXC でコンパイルする（MaterialGraphGpu / materialgraphshader.cpp）。vcxproj への登録は不要
// =====================================================================================================

// ルート定数（materialgraphshader.cpp の GpuConstants と同じ並び）
cbuffer GraphGpuConstants : register(b0)
{
    uint g_Mode;        // 0 ノード（平面）, 1 ノード（球）, 2 Material Output（球）, 3 ベイク, 4 ノードを焼く,
                        // 5 発光の動的ベイク
    uint g_ProgramRow;  // プログラム表の行
    uint g_CellX;       // 出力先の左上（プレビューの表）
    uint g_CellY;
    uint g_Resolution;  // 出力の一辺
    float g_Time;
    uint g_Flags;       // 0〜2：bit0 色（sRGB 表示）, bit1 法線（*0.5+0.5）, bit4-6 成分数
                        // 3：bit0-1 アルファ（0 Base Color / 1 Opacity / 2 Opacity Mask）, bit2 旧グラフの色の法線
                        // 4：0 Color / 1 Normal / 2 Linear
    uint g_BakeMask;    // 3：焼くスロット（bit = MaterialTextureSlot）
    float g_Clip;       // 2：Opacity Mask のクリップ値
    uint g_OutputPins;  // 2：bit0 Normal, bit1 Opacity Mask, bit2 Transmission, bit3 Opacity が繋がっている
    float g_Yaw;
    float g_Pitch;
    float3 g_ViewCamera;   // 5：実カメラのワールド座標（Camera Position ノード）
    float g_ViewDistance;  // 5：カメラからオブジェクトまでの距離（Camera Distance / Distance Fade）
    float g_ViewDepth;     // 5：オブジェクトのビュー深度（Pixel Depth）
    float3 g_ViewPadding;
};

Texture2D<float4> GraphPrograms : register(t0);
RWTexture2D<float4> PreviewOutput : register(u0);       // 0〜2（RGBA8）
RWTexture2DArray<float4> BakeOutput : register(u1);     // 3〜4（RGBA32F、スライス = スロット）
SamplerState g_GraphSampler : register(s0);

// ---- MaterialGraphVM.hlsli が使う共通関数 / 定数（common.hlsl を使わない最小構成）
// プレビュー / ベイクのカメラ：正投影で -Z から +Z を見る（視線 = (0,0,-1)、奥行き = z + 3）
static const float4x4 View = float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 3, 1);
static const float4x4 Projection = float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);

float3 SafeNormalizeCommon(float3 value, float3 fallback)
{
    float lengthSquared = dot(value, value);
    return lengthSquared > 1e-12f ? value * rsqrt(lengthSquared) : fallback;
}

float3 SurfaceViewDirectionCommon(float3 worldPosition, float3 cameraPosition)
{
    return float3(0, 0, -1);
}

// The editor compute root signature binds a texture table in space1. It does
// not provide the per-material constants used by the scene's bindless path.
#ifdef RP_BINDLESS
#undef RP_BINDLESS
#endif
#define RP_BINDLESS 0
#include "MaterialTextureHeap.hlsli"
#include "noise.hlsl"

// モード 5 だけ、視点に関わる入力を実カメラとオブジェクトの関係に置き換える（0〜4 は従来どおり）
// （MaterialGraphVmInput はこの後の include で定義されるので、成分で受け取る）
float StandaloneCameraDistance(float3 worldPos, float3 cameraPos)
{
    return g_Mode == 5u ? g_ViewDistance : length(worldPos - cameraPos);
}

float StandalonePixelDepth(float3 worldPos)
{
    return g_Mode == 5u ? g_ViewDepth : mul(float4(worldPos, 1), View).z;
}

#define MG_VM_CAMERA_DISTANCE(input) StandaloneCameraDistance((input).WorldPos, (input).CameraPos)
#define MG_VM_PIXEL_DEPTH(input) StandalonePixelDepth((input).WorldPos)
#define MG_VM_STANDALONE
#define MG_VM_PROGRAM_TEXTURE GraphPrograms
#define MG_VM_ROW ((int)g_ProgramRow)
#define MG_VM_TIME_OVERRIDE g_Time
#include "MaterialGraphVM.hlsli"

static const float kPi = 3.14159265f;

float3 LinearToSrgb(float3 v)
{
    v = saturate(v);
    return lerp(v * 12.92f, 1.055f * pow(v, 1.0f / 2.4f) - 0.055f, step(0.0031308f, v));
}

struct PreviewPoint
{
    MaterialGraphVmInput Input;
    float3 Tangent;
    float3 Bitangent;
    bool Valid;
};

MaterialGraphVmInput MakeInput(float3 position, float3 normal, float2 uv, float2 pixel)
{
    MaterialGraphVmInput input;
    input.WorldPos = position;
    input.Normal = normal;
    input.CameraPos = position + float3(0, 0, -3);
    input.UV = uv;
    input.VertexColor = float4(1, 1, 1, 1);
    // プレビュー / ベイクにはオブジェクトが無い：原点 0・スケール 1・範囲 -1〜1・乱数 0.5（CPU 評価と同じ）
    input.ObjectToWorld = float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    input.BoundsCenter = float3(0, 0, 0);
    input.BoundsExtents = float3(1, 1, 1);
    input.InstanceRandom = 0.5f;
    input.PixelPos = pixel;
    return input;
}

// CPU の PlaneContext と同じ
PreviewPoint PlanePoint(uint2 p, uint resolution)
{
    PreviewPoint result;
    float2 uv = (float2(p) + 0.5f) / resolution;
    result.Input = MakeInput(float3(uv.x * 2 - 1, 1 - uv.y * 2, 0), float3(0, 0, -1), uv, float2(p));
    result.Tangent = float3(1, 0, 0);
    result.Bitangent = float3(0, -1, 0);
    result.Valid = true;
    return result;
}

// CPU の SphereContext と同じ（カメラは -Z 側から +Z を見る）
PreviewPoint SpherePoint(uint2 p, uint resolution)
{
    PreviewPoint result = (PreviewPoint)0;
    float px = (p.x + 0.5f) / resolution * 2 - 1;
    float py = 1 - (p.y + 0.5f) / resolution * 2;
    float r2 = px * px + py * py;
    result.Valid = r2 <= 1.0f;
    float3 n = float3(px, py, -sqrt(saturate(1 - r2)));
    float cp = cos(-g_Pitch), sp = sin(-g_Pitch);
    float oy = n.y * cp - n.z * sp;
    float oz1 = n.y * sp + n.z * cp;
    float cy = cos(-g_Yaw), sy = sin(-g_Yaw);
    float ox = n.x * cy + oz1 * sy;
    float oz = -n.x * sy + oz1 * cy;
    float2 uv = float2(atan2(ox, -oz) / (2 * kPi) + 0.5f, acos(clamp(oy, -1.0f, 1.0f)) / kPi);
    result.Input = MakeInput(n, n, uv, float2(p));
    float3 t = float3(-n.z, 0, n.x);
    float tl = length(t.xz);
    t = tl > 1e-4f ? t / tl : float3(1, 0, 0);
    result.Tangent = t;
    result.Bitangent = cross(n, t);
    return result;
}

// ---- ノードプレビュー（CPU の PackValue と同じ表示）
float4 EncodeNode(float4 v)
{
    uint dims = (g_Flags >> 4u) & 7u;
    if ((g_Flags & 2u) != 0u)
    {
        v.xyz = v.xyz * 0.5f + 0.5f;
    }
    float3 c = dims <= 1u ? v.xxx : (dims == 2u ? float3(v.xy, 0) : v.xyz);
    c = (g_Flags & 1u) != 0u ? LinearToSrgb(c) : saturate(c);
    return float4(c, 1);
}

// ---- Material Output のプレビュー（CPU の ShadeOutput と同じ簡易 PBR。POM は省略）
float4 ShadeOutput(MaterialGraphVmOutput graph, PreviewPoint pt, uint2 p)
{
    float3 n = pt.Input.Normal;
    if ((g_OutputPins & 1u) != 0u)
    {
        float3 tn = graph.Normal;
        tn.xy *= max(graph.NormalIntensity, 0.0f);
        float3 world = pt.Tangent * tn.x + pt.Bitangent * tn.y + n * tn.z;
        n = SafeNormalizeCommon(world, n);
    }
    float3 baseColor = saturate(graph.BaseColor.rgb);
    float metallic = saturate(graph.Metallic);
    float roughness = clamp(graph.Roughness, 0.04f, 1.0f);
    float opacity = saturate(graph.Opacity);
    float ao = saturate(graph.AO);
    if ((g_OutputPins & 2u) != 0u && graph.OpacityMask < g_Clip)
    {
        return float4(0, 0, 0, 0);
    }
    if ((g_OutputPins & 4u) != 0u)
    {
        opacity *= 1 - 0.7f * saturate(graph.Transmission);
    }
    float3 v = float3(0, 0, -1);
    float3 l = normalize(float3(-0.45f, 0.65f, -0.6f));
    float3 h = normalize(l + v);
    float nDotL = saturate(dot(n, l));
    float nDotV = saturate(dot(n, v)) + 1e-4f;
    float nDotH = saturate(dot(n, h));
    float vDotH = saturate(dot(v, h));
    float a = roughness * roughness;
    float a2 = a * a;
    float d = a2 / (kPi * pow(nDotH * nDotH * (a2 - 1) + 1, 2) + 1e-6f);
    float k = (roughness + 1) * (roughness + 1) / 8;
    float g = (nDotL / (nDotL * (1 - k) + k)) * (nDotV / (nDotV * (1 - k) + k));
    float fresnelWeight = pow(1 - vDotH, 5);
    float ambientWeight = pow(1 - nDotV, 5);
    float sky = n.y * 0.5f + 0.5f;
    float3 f0 = lerp(0.04f.xxx, baseColor, metallic);
    float3 f = f0 + (1 - f0) * fresnelWeight;
    float3 specular = d * f * g / (4 * nDotL * nDotV + 1e-4f);
    float3 diffuse = baseColor * (1 - metallic) / kPi;
    float3 ambientColor = float3(0.22f, 0.22f, 0.30f) * sky + 0.06f * (1 - sky);
    float3 ambient = ambientColor * ao * (baseColor * (1 - metallic) + (f0 + (1 - f0) * ambientWeight) * (1 - roughness * 0.7f));
    float3 color = (diffuse + specular) * nDotL * 3 + ambient + max(graph.Emissive, 0.0f);
    color = color / (1 + color);
    if (opacity < 0.999f)
    {
        uint cell = max(g_Resolution / 8u, 1u);
        float background = (((p.x / cell) + (p.y / cell)) & 1u) != 0u ? 0.18f : 0.06f;
        color = color * opacity + background * (1 - opacity);
    }
    return float4(LinearToSrgb(color), 1);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_Resolution || id.y >= g_Resolution)
    {
        return;
    }
    uint2 p = id.xy;
    if (g_Mode <= 2u)
    {
        PreviewPoint pt;
        if (g_Mode == 0u)
        {
            pt = PlanePoint(p, g_Resolution);
        }
        else
        {
            pt = SpherePoint(p, g_Resolution);
        }
        float4 color = float4(0, 0, 0, 0);
        if (pt.Valid)
        {
            MaterialGraphVmOutput graph = EvaluateMaterialGraphVm(1u, pt.Input, g_GraphSampler);
            color = g_Mode == 2u ? ShadeOutput(graph, pt, p) : EncodeNode(graph.BaseColor);
        }
        PreviewOutput[uint2(g_CellX, g_CellY) + p] = color;
        return;
    }

    // ---- ベイク：CPU の MaterialGraphBake::Evaluate と同じ（UV 0〜1、正面から見た値）
    PreviewPoint pt = PlanePoint(p, g_Resolution);
    if (g_Mode == 5u)
    {
        // 発光の動的ベイク：模様（UV / 位置）は通常のベイクと同じ点で評価し、
        // カメラ位置・距離・深度・時刻だけを実際の値にする。正規化せず HDR のまま書く（RGBA16F）
        pt.Input.CameraPos = g_ViewCamera;
        MaterialGraphVmOutput dynamicGraph = EvaluateMaterialGraphVm(1u, pt.Input, g_GraphSampler);
        float3 e = dynamicGraph.Emissive;
        e = float3(isfinite(e.x) ? max(e.x, 0) : 0, isfinite(e.y) ? max(e.y, 0) : 0, isfinite(e.z) ? max(e.z, 0) : 0);
        BakeOutput[uint3(p, 0)] = float4(min(e, 65504.0f), 1);
        return;
    }
    MaterialGraphVmOutput graph = EvaluateMaterialGraphVm(1u, pt.Input, g_GraphSampler);
    if (g_Mode == 4u)
    {
        float4 value = graph.BaseColor; // Expand 済みではない：成分数で揃える
        uint dims = (g_BakeMask >> 4u) & 7u;
        value = dims <= 1u ? float4(value.xxx, 1) : float4(value.x, value.y, dims > 2u ? value.z : 0.0f, dims > 3u ? value.w : 1.0f);
        if ((g_Flags & 3u) == 1u)
        {
            value = float4(value.xyz * 0.5f + 0.5f, 1);
        }
        BakeOutput[uint3(p, 0)] = saturate(value);
        return;
    }
    // 0 Base Color
    if ((g_BakeMask & 1u) != 0u)
    {
        float4 color = graph.BaseColor;
        float alpha = saturate(color.a);
        uint alphaSource = g_Flags & 3u;
        if (alphaSource == 1u)
        {
            alpha *= saturate(graph.Opacity);
        }
        else if (alphaSource == 2u)
        {
            alpha = saturate(graph.OpacityMask);
        }
        BakeOutput[uint3(p, 0)] = float4(saturate(color.rgb), alpha);
    }
    // 1 Normal
    if ((g_BakeMask & 2u) != 0u)
    {
        float3 n = graph.Normal;
        if ((g_Flags & 4u) != 0u)
        {
            n = LinearToSrgb(n) * 2 - 1; // 旧グラフの Color サンプラーの法線マップ
        }
        float lengthValue = length(n);
        n = lengthValue > 1e-6f && isfinite(lengthValue) ? n / lengthValue : float3(0, 0, 1);
        BakeOutput[uint3(p, 1)] = float4(n * 0.5f + 0.5f, 1);
    }
    // 2 Metallic Roughness
    if ((g_BakeMask & 4u) != 0u)
    {
        BakeOutput[uint3(p, 2)] = float4(saturate(graph.Metallic), saturate(graph.Roughness), 0, 1);
    }
    // 3 AO
    if ((g_BakeMask & 8u) != 0u)
    {
        float ao = saturate(graph.AO);
        BakeOutput[uint3(p, 3)] = float4(ao, ao, ao, 1);
    }
    // 4 Emissive（最大値での正規化は CPU 側で行う）
    if ((g_BakeMask & 16u) != 0u)
    {
        float3 e = graph.Emissive;
        e = float3(isfinite(e.x) ? max(e.x, 0) : 0, isfinite(e.y) ? max(e.y, 0) : 0, isfinite(e.z) ? max(e.z, 0) : 0);
        BakeOutput[uint3(p, 4)] = float4(e, 1);
    }
    // 5 Height
    if ((g_BakeMask & 32u) != 0u)
    {
        float h = saturate(graph.Height);
        BakeOutput[uint3(p, 5)] = float4(h, h, h, 1);
    }
}

