#ifndef LIVE_TOON_LIGHTING_HLSLI
#define LIVE_TOON_LIGHTING_HLSLI

// Shared by Forward and Deferred. No material/global buffer reads here: Deferred
// must not borrow another entity's material controls from the full-screen pass.
bool IsLiveToonEyeClass(float materialClass)
{
    return materialClass >= 14.5f && materialClass < 17.5f;
}

// Preserve the dedicated eye overrides even on a model using manual Toon.
float ResolveLiveToonMaterialClass(float vertexClass, float manualClass)
{
    float part = vertexClass >= 100.0f ? vertexClass - 100.0f : vertexClass;
    return abs(manualClass - 4.0f) < 0.5f && IsLiveToonEyeClass(part) ? vertexClass : manualClass;
}

bool IsLiveToonClass(float materialClass)
{
    return (materialClass >= 0.5f && materialClass < 4.5f) || IsLiveToonEyeClass(materialClass);
}

float4 PackLiveToonPomShadow(float4 authoredShadow, float visibility)
{
    // SHADOW is R8G8B8A8_UNORM. Toon does not use the PBR shading-model alpha;
    // zero alpha naturally means no POM occlusion for other GBuffer writers.
    return float4(authoredShadow.x, authoredShadow.y, authoredShadow.z, 1.0f - saturate(visibility));
}

float LiveToonPomVisibility(float encodedOcclusion)
{
    return 1.0f - saturate(encodedOcclusion);
}

float LiveToonBand(float value, float threshold, float softness)
{
    float width = max(softness, 0.001f);
    return smoothstep(threshold - width, threshold + width, value);
}

float3 LiveToonNormalize(float3 value, float3 fallback)
{
    float lengthSquared = dot(value, value);
    return lengthSquared > 0.000001f ? value * rsqrt(lengthSquared) : fallback;
}

// Radiance already contains intensity, distance falloff and shadow visibility.
// Accumulate this for each light, never with an averaged direction: opposing
// blue/red spots need independent cel bands, highlights and directional rims.
float3 EvaluateLiveToonLight(float3 albedo, float3 normal, float3 viewDir, float3 lightDir,
                             float3 radiance, float materialClass, float roughness,
                             float shadowThreshold, float shadowSoftness, float shadowStrength)
{
    bool skin = abs(materialClass - 3.0f) < 0.5f;
    bool hair = abs(materialClass - 1.0f) < 0.5f;
    float nDotL = dot(normal, lightDir);
    float wrap = skin ? 0.38f : 0.12f;
    float diffuseCoordinate = saturate((nDotL + wrap) / (1.0f + wrap));
    float softness = max(shadowSoftness, skin ? 0.075f : 0.015f);
    float mid = LiveToonBand(diffuseCoordinate, saturate(shadowThreshold), softness);
    float lit = LiveToonBand(diffuseCoordinate, min(shadowThreshold + 0.24f, 0.95f), softness);
    float shadeFloor = skin ? 0.55f : 0.28f;
    float shade = lerp(1.0f, lerp(shadeFloor, 0.76f, mid) + 0.24f * lit, saturate(shadowStrength));

    // Skin retains a neutral component under saturated stage lights; cloth/hair
    // retain the full stage tint. This is a material profile, not an RGB skin mask.
    float luminance = max(dot(radiance, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
    float3 skinRadiance = lerp(float3(luminance, luminance, luminance), radiance, 0.55f);
    float3 diffuseRadiance = skin ? skinRadiance : radiance;

    float3 halfDir = LiveToonNormalize(lightDir + viewDir, normal);
    float specular = pow(saturate(dot(normal, halfDir)), lerp(96.0f, 20.0f, saturate(roughness)));
    specular = LiveToonBand(specular, hair ? 0.45f : 0.65f, 0.04f) * saturate(nDotL);
    float specularStrength = skin ? 0.025f : (hair ? 0.20f : 0.12f);

    float edge = 1.0f - saturate(dot(normal, viewDir));
    float rim = edge * edge * edge * edge;
    rim *= LiveToonBand(-dot(viewDir, lightDir), 0.1f, 0.3f);
    rim *= saturate(nDotL + 0.3f);
    float rimStrength = skin ? 0.16f : 0.42f;

    // Every term is linear in albedo, and zero for zero radiance. Keep HDR values
    // for exposure/bloom; final display mapping belongs to the post-process pass.
    return albedo * (diffuseRadiance * shade + radiance * (specular * specularStrength + rim * rimStrength));
}

float3 EvaluateLiveToonAmbient(float3 albedo, float3 environmentIrradiance, float materialClass)
{
    float fill = abs(materialClass - 3.0f) < 0.5f ? 0.20f : 0.12f;
    return albedo * (float3(fill, fill, fill) + environmentIrradiance * 0.25f);
}

// ---------------------------------------------------------------------------------------------
// ライブトゥーン拡張データ（学マス方式：シェードカラー / キャラ別陰影ランプ / Def の学マス配置 /
// リムマスク / 質感ランプ行 / メタリック / スペキュラマスク）。
//
// ジオメトリ段（DeferredPS / modelshaderPS）で作り、
//   ・Forward：そのまま g_LiveToonExtra に入れてライティングへ
//   ・Deferred：GBuffer の Material.r / Material.b（R16G16B16A16_FLOAT）に半精度のビット列として格納
// トゥーンの画素では Material.r（メタリック）と Material.b（フレネル）をライティングが使わないので、
// 他のパス（RT 反射・NRD・MegaLights・大気）は .g / .a しか読まず、影響しない。
//
// 半精度のビット列は NaN / Inf（指数 31）と非正規化数（指数 0）を避けて詰めるので、
// float32 → float16 の書き込み・読み戻しで値が変わらない。
//   Material.r : bit15 = 肌, bit14-10 = シェード R（1..30）, bit9-5 = G, bit4-0 = B
//   Material.b : bit15 = シェードカラーあり, bit14 = 1（印。|値| >= 2 になる）,
//                bit13-11 = キャラ陰影ランプのスロット（0..6）, bit10-8 = 質感ランプ行（0 = 自動）,
//                bit7-5 = リムマスク, bit4-2 = メタリック, bit1-0 = スペキュラマスク
// ---------------------------------------------------------------------------------------------
struct LiveToonExtra
{
    bool Valid;
    bool HasShade;
    bool Skin;
    float3 ShadeColor;
    uint RampSlot;       // 0 = シーン共通の陰影ランプ、1..6 = キャラ別
    uint TextureRampRow; // 0 = マテリアルクラスから自動、1..7 = 指定行
    float RimMask;
    float Metallic;
    float SpecularMask;
};

LiveToonExtra LiveToonExtraDefault()
{
    LiveToonExtra e;
    e.Valid = false;
    e.HasShade = false;
    e.Skin = false;
    e.ShadeColor = float3(0.0f, 0.0f, 0.0f);
    e.RampSlot = 0u;
    e.TextureRampRow = 0u;
    e.RimMask = 1.0f;
    e.Metallic = 0.0f;
    e.SpecularMask = 1.0f;
    return e;
}

// ライティング側が参照する現在の画素の拡張データ（g_LiveToonClass と同じ使い方）。
// ゼロ初期化（Valid = false）。拡張データの各値は Valid のときだけ参照する。
static LiveToonExtra g_LiveToonExtra = (LiveToonExtra)0;
// 現在評価中のライトのスペキュラ倍率（LightComponent::SpecularScale）。
static float g_LightSpecularScale = 1.0f;

bool IsLiveToonExtraPacked(float materialB)
{
    return abs(materialB) >= 2.0f;
}

void PackLiveToonExtra(LiveToonExtra e, out float materialR, out float materialB)
{
    uint3 c = (uint3)round(saturate(e.ShadeColor) * float3(29.0f, 31.0f, 31.0f));
    c.x += 1u;
    uint rBits = (e.Skin ? 0x8000u : 0u) | (c.x << 10) | (c.y << 5) | c.z;
    uint slot = min(e.RampSlot, 6u);
    uint row = min(e.TextureRampRow, 7u);
    uint rim = (uint)round(saturate(e.RimMask) * 7.0f);
    uint metal = (uint)round(saturate(e.Metallic) * 7.0f);
    uint spec = (uint)round(saturate(e.SpecularMask) * 3.0f);
    uint bBits = (e.HasShade ? 0x8000u : 0u) | 0x4000u | (slot << 11) | (row << 8) | (rim << 5) | (metal << 2) | spec;
    materialR = f16tof32(rBits);
    materialB = f16tof32(bBits);
}

LiveToonExtra UnpackLiveToonExtra(float materialR, float materialB)
{
    LiveToonExtra e = LiveToonExtraDefault();
    uint rBits = f32tof16(materialR) & 0xffffu;
    uint bBits = f32tof16(materialB) & 0xffffu;
    e.Valid = true;
    e.Skin = (rBits & 0x8000u) != 0u;
    e.ShadeColor = float3((float)(((rBits >> 10) & 31u)) - 1.0f, (float)((rBits >> 5) & 31u), (float)(rBits & 31u)) /
                   float3(29.0f, 31.0f, 31.0f);
    e.ShadeColor = saturate(e.ShadeColor);
    e.HasShade = (bBits & 0x8000u) != 0u;
    e.RampSlot = (bBits >> 11) & 7u;
    e.TextureRampRow = (bBits >> 8) & 7u;
    e.RimMask = (float)((bBits >> 5) & 7u) / 7.0f;
    e.Metallic = (float)((bBits >> 2) & 7u) / 7.0f;
    e.SpecularMask = (float)(bBits & 3u) / 3.0f;
    return e;
}

// 肌判定：シェードカラーのアルファ（学マス方式）があればそれを、無ければマテリアルクラスを使う。
float LiveToonRimMask()
{
    return g_LiveToonExtra.Valid ? g_LiveToonExtra.RimMask : 1.0f;
}

float LiveToonSpecularMask()
{
    return g_LiveToonExtra.Valid ? g_LiveToonExtra.SpecularMask : 1.0f;
}

// スペキュラ色：メタリックほどアルベド色（従来はアルベドと白の中間）。
float3 LiveToonSpecularTint(float3 albedo, float whiteBlend)
{
    float3 dielectric = lerp(albedo, float3(1.0f, 1.0f, 1.0f), whiteBlend);
    return g_LiveToonExtra.Valid ? lerp(dielectric, albedo, g_LiveToonExtra.Metallic) : dielectric;
}

bool LiveToonIsSkin(float materialClass)
{
    return g_LiveToonExtra.Valid ? g_LiveToonExtra.Skin : abs(materialClass - 3.0f) < 0.5f;
}

// ライトの照射チャンネルとスペキュラ倍率（LightIesProfiles[i].w）。0 = 従来（両方・1.0）。
//   整数部 = チャンネル（bit0 = 背景, bit1 = キャラ）、小数部 / 0.99 = スペキュラ倍率
uint LightChannelMaskCommon(float packed)
{
    return packed < 0.5f ? 3u : (uint)floor(packed);
}

float LightSpecularScaleCommon(float packed)
{
    return packed < 0.5f ? 1.0f : saturate(frac(packed) / 0.99f);
}
#endif


