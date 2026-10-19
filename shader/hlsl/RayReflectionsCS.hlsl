#define SHADER_POSTPROCESS
#define SHADER_3D
#define SHADER_RAYTRACING
#define SKY_COMPUTE
#include "common.hlsl"
// 大気 LUT の物理的な空（ルート引数 13、space6、クランプサンプラー s1）。
#define SKY_LUT 1
#define SKY_LUT_SCATTER_REGISTER t0, space6
#define SKY_LUT_TRANSMISSION_REGISTER t1, space6
#define SKY_LUT_SAMPLER_REGISTER s1
#include "SkyAtmosphere.hlsli"
#if USE_NRD
#include "../../External/NRD/sdk/Shaders/NRD.hlsli"
#endif

#include "RayTelemetry.hlsli"
Texture2D<float> ReflectionDepth : register(t1);
Texture2D<float4> ReflectionNormal : register(t2);
Texture2D<float4> ReflectionMaterial : register(t3);
Texture2D<float4> FilteredRadiance : register(t4);
ByteAddressBuffer SceneVertices[] : register(t0, space1);
ByteAddressBuffer SceneIndices[] : register(t0, space2);
Texture2D<float4> SceneTextures[] : register(t0, space3);
Texture2D<float4> EmissionTextures[] : register(t0, space4);
Texture2D<float4> HeightTextures[] : register(t0, space12); // POM の高さマップ（Surface.z != 0 のメッシュのみ）
StructuredBuffer<uint> EmissiveMeshes : register(t6);
SamplerState MaterialSampler : register(s0);
RWTexture2D<float4> NoisyRadiance : register(u0);
RWTexture2D<float4> ReflectionRadiance : register(u1);

struct ReflectionGeometry
{
    float4x4 World;
    float4x4 NormalToWorld;
    uint4 Layout;    // 頂点ストライド、インデックス形式（0/16/32）、頂点オフセット、インデックスオフセット
    float4 Material; // 粗さ、金属度、テクスチャの有効フラグ、POM の深さ（UV 単位）
    float4 BaseColor;
    float4 Emission;
    uint4 Surface; // 三角形数、発光テクスチャの有無、高さマップの有無、マテリアルアセット
};

StructuredBuffer<ReflectionGeometry> ReflectionMeshes : register(t0);

struct ReflectionLight
{
    float4 Direction, Color, PositionType, Extras;
    uint4 Flags;
};

StructuredBuffer<ReflectionLight> ReflectionLights : register(t5);

cbuffer ReflectionConstants : register(b6)
{
    float4x4 ReflectionInvViewProjection;
    float4x4 ReflectionView;
    float4x4 ReflectionViewProjection;
    float4 ReflectionCamera;
    uint4 ReflectionExtent;  // 幅、高さ、シーケンス、ライト数
    float4 GiParameters;     // 距離、強度、チェッカーボードの有効フラグ、発光メッシュ数
    float4 CacheParameters;  // 有効フラグ、基本セルサイズ（m）、容量、履歴の最大フレーム数
    float4 ReSTIRParameters; // 有効フラグ、時間方向の最大 M、空間サンプル数、空間半径（px）
    float4x4 AdaptivePreviousViewProjection;
    float4 AdaptiveSampling;
    uint4 ReSTIRState; // history valid, reservoir width (traced columns), spatial visibility, 反射レイの粗さ上限 × 1000
    float4 ReflectionStability;
};

#include "../../source/rendering/stablesampling.h"
#include "../../source/rendering/giresampling.h"
#include "LumenSurfaceCache.hlsli"

Texture2D<float4> AdaptiveRadiance : register(t0, space10);
Texture2D<float4> AdaptiveGuide : register(t1, space10);
RWTexture2D<float4> AdaptiveGuideOut : register(u0, space10);

// ---------------------------------------------------------------------------------------------
// ReSTIR GI（Ouyang ほか 2021）：二次反射に対する重要度の再サンプリング。
// 追跡した各ピクセルはリザーバーを格納する（サンプル点、法線、出射放射輝度、重み W、
// 信頼度 M、再利用の検証用の可視点の深度と法線）。
// 時間パス：前フレームの再投影済みリザーバー（履歴）を統合する。
// 空間パス：立体角変化のヤコビアンと、任意の可視性レイで近傍 k 個を統合し、
// デノイザー用の最終放射照度を書き込む。
// リザーバーは追跡解像度に置く（NRD 有効時はチェッカーボードの列を詰める）。
// ---------------------------------------------------------------------------------------------
uint ReflectionHash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

RWByteAddressBuffer ReservoirCurrent : register(u0, space8);
RWByteAddressBuffer ReservoirHistory : register(u1, space8);
Texture2D<float4> GiVelocity : register(t0, space8);

struct GiReservoir
{
    float3 Position;    // 二次ヒット（空のサンプルではレイ上の遠点）
    float3 Normal;      // 二次ヒットの法線（空では -ray）
    float3 Radiance;    // サンプルから可視点へ向かう出射放射輝度
    float W;            // 不偏な寄与の重み
    float M;            // 信頼度
    float VisibleDepth; // リザーバーを生成・保持するピクセルの線形ビュー深度
    float3 VisibleNormal;
};

// ベクトル条件式を使わず成分ごとの符号を求める（HLSL 2021 ではベクトル条件式が無効）。
float2 OctSign(float2 v)
{
    return step(0.0f, v) * 2.0f - 1.0f;
}

uint PackOctahedral16(float3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 0.000001f);
    float2 e = n.xy;
    if (n.z < 0.0f)
    {
        e = (1.0f - abs(n.yx)) * OctSign(n.xy);
    }
    uint2 q = (uint2)round(saturate(e * 0.5f + 0.5f) * 65535.0f);
    return q.x | (q.y << 16);
}

float3 UnpackOctahedral16(uint packed)
{
    float2 e = float2(packed & 0xffffu, packed >> 16) / 65535.0f * 2.0f - 1.0f;
    float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y));
    if (n.z < 0.0f)
    {
        n.xy = (1.0f - abs(n.yx)) * OctSign(n.xy);
    }
    return normalize(n);
}

uint PackOctahedral8(float3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 0.000001f);
    float2 e = n.xy;
    if (n.z < 0.0f)
    {
        e = (1.0f - abs(n.yx)) * OctSign(n.xy);
    }
    uint2 q = (uint2)round(saturate(e * 0.5f + 0.5f) * 255.0f);
    return q.x | (q.y << 8);
}

float3 UnpackOctahedral8(uint packed)
{
    float2 e = float2(packed & 0xffu, (packed >> 8) & 0xffu) / 255.0f * 2.0f - 1.0f;
    float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y));
    if (n.z < 0.0f)
    {
        n.xy = (1.0f - abs(n.yx)) * OctSign(n.xy);
    }
    return normalize(n);
}

void StoreReservoir(RWByteAddressBuffer buffer, uint index, GiReservoir r)
{
    uint4 a = uint4(asuint(r.Position), PackOctahedral16(r.Normal));
    uint4 b =
        uint4(f32tof16(r.Radiance.r) | (f32tof16(r.Radiance.g) << 16), f32tof16(r.Radiance.b) | (f32tof16(r.M) << 16),
              asuint(r.W), f32tof16(r.VisibleDepth) | (PackOctahedral8(r.VisibleNormal) << 16));
    buffer.Store4(index * 32u, a);
    buffer.Store4(index * 32u + 16u, b);
}

GiReservoir LoadReservoir(RWByteAddressBuffer buffer, uint index)
{
    uint4 a = buffer.Load4(index * 32u);
    uint4 b = buffer.Load4(index * 32u + 16u);
    GiReservoir r;
    r.Position = asfloat(a.xyz);
    r.Normal = UnpackOctahedral16(a.w);
    r.Radiance = float3(f16tof32(b.x), f16tof32(b.x >> 16), f16tof32(b.y));
    r.M = f16tof32(b.y >> 16);
    r.W = asfloat(b.z);
    r.VisibleDepth = f16tof32(b.w);
    r.VisibleNormal = UnpackOctahedral8(b.w >> 16);
    if (any(isnan(r.Radiance)) || isnan(r.W) || isinf(r.W))
    {
        r.W = 0.0f;
        r.M = 0.0f;
    }
    return r;
}

float ReSTIRImportance(float3 c)
{
    return GiRadianceImportance(c.r, c.g, c.b);
}

// 色成分を考慮した目標値で、有色の反射を再利用するときに赤や青の経路が不足するのを防ぐ。
float ReSTIRTarget(GiReservoir r, float3 position, float3 normal)
{
    float3 direction = r.Position - position;
    float distance = length(direction);
    if (distance < 0.0001f)
    {
        return 0.0f;
    }
    float cosine = saturate(dot(normal, direction / distance));
    return ReSTIRImportance(r.Radiance) * cosine;
}

float ReSTIRRandom(inout uint state)
{
    state = ReflectionHash(state + 0x9e3779b9u);
    return ((state >> 8) + 0.5f) / 16777216.0f;
}

// 再投影済みや近傍のリザーバーは、似た面でのみ再利用する。
bool ReSTIRSimilarSurface(float depthA, float3 normalA, float depthB, float3 normalB)
{
    return abs(depthA - depthB) < max(depthA, 0.01f) * 0.1f && dot(normalA, normalB) > 0.9f;
}

uint2 ReSTIRStorageToPixel(uint2 storage)
{
    uint2 pixel = storage;
    if (GiParameters.z > 0.5f)
    {
        pixel.x = 2 * storage.x + ((storage.y + ReflectionExtent.z) & 1u);
    }
    return pixel;
}

bool ReSTIRVisiblePoint(uint2 pixel, out float3 position, out float3 normal, out float viewDepth)
{
    position = 0;
    normal = float3(0, 1, 0);
    viewDepth = 0;
    if (any(pixel >= ReflectionExtent.xy))
    {
        return false;
    }
    float depth = ReflectionDepth.Load(int3(pixel, 0));
    if (depth >= 0.99999f)
    {
        return false;
    }
    float2 uv = (pixel + 0.5f) / ReflectionExtent.xy;
    float4 world = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1), ReflectionInvViewProjection);
    position = world.xyz / world.w;
    normal = normalize(ReflectionNormal.Load(int3(pixel, 0)).xyz * 2 - 1);
    viewDepth = abs(mul(float4(position, 1), ReflectionView).z);
    return true;
}

// ---------------------------------------------------------------------------------------------
// ワールド空間の放射照度キャッシュ（SHaRC 形式のハッシュグリッド）。
// 各 GI サンプルは、空・反射・発光体を含む放射照度の推定値（ヒット点で評価する
// 解析的なライトを除く）を、主表面のセルへ書き込む。
// GI レイがキャッシュ済みセルにヒットすると、1 回反射の放射輝度に albedo/pi * E_cache を加える。
// 間接光がフレームをまたいで伝播し、1 回の参照コストで多重反射を実現する。
// ---------------------------------------------------------------------------------------------
RWByteAddressBuffer CacheKeys : register(u0, space7);
RWByteAddressBuffer CacheAccumulation : register(u1, space7); // uint4：rgb の固定小数点値、サンプル数

struct CacheValue
{
    float4 Signal;
    float4 Distance;
};

RWStructuredBuffer<CacheValue> CacheResolved : register(u2, space7); // rgb = 放射照度、履歴フレーム数
RWByteAddressBuffer CacheAges : register(u3, space7);
static const float kCacheScale = 256.0f; // 蓄積用の固定小数点スケール
static const float kCacheMaxIrradiance = 256.0f;
static const uint kCacheProbes = 8;
static const uint kCacheMaxAge = 240; // サンプルがない状態で退去させるまでのフレーム数

uint CacheHash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

// カメラから 8 m を超えると、距離が倍になるごとにセルサイズも倍にする（画面上の投影範囲を一定に保つ）。
void CacheCell(float3 position, float3 normal, out uint key, out uint bucket)
{
    float distance = length(position - ReflectionCamera.xyz);
    uint level = (uint)clamp(floor(log2(max(distance, 1.0f) / 8.0f)) + 1.0f, 0.0f, 12.0f);
    float cellSize = CacheParameters.y * exp2((float)level);
    int3 cell = int3(floor(position / cellSize));
    float3 magnitude = abs(normal);
    uint axis = 2u;
    if (magnitude.x > magnitude.y)
    {
        if (magnitude.x > magnitude.z)
        {
            axis = 0u;
        }
    }
    else if (magnitude.y > magnitude.z)
    {
        axis = 1u;
    }
    uint orientation = axis * 2u + (normal[axis] < 0 ? 1u : 0u);
    uint hash = CacheHash(
        asuint(cell.x) + CacheHash(asuint(cell.y) + CacheHash(asuint(cell.z) + CacheHash(level | (orientation << 4)))));
    key = hash | 1u; // 0 は空きスロットを表す
    bucket = CacheHash(hash ^ 0x9e3779b9u) & ((uint)CacheParameters.z - 1u);
}

bool CacheLookupSignal(float3 position, float3 normal, out float3 irradiance, out float hitDistance)
{
    irradiance = 0;
    hitDistance = 0;
    if (CacheParameters.x < 0.5f)
    {
        return false;
    }
    uint key, bucket;
    CacheCell(position, normal, key, bucket);
    uint mask = (uint)CacheParameters.z - 1u;
    [loop] for (uint probe = 0; probe < kCacheProbes; ++probe)
    {
        uint slot = (bucket + probe) & mask;
        if (CacheKeys.Load(slot * 4) == key)
        {
            float4 resolved = CacheResolved[slot].Signal;
            hitDistance = CacheResolved[slot].Distance.x;
            irradiance = resolved.rgb;
            return resolved.w > 0 && CacheAges.Load(slot * 4) <= 8u && all(isfinite(resolved)) && isfinite(hitDistance);
        }
    }
    return false;
}

bool CacheLookup(float3 position, float3 normal, out float3 irradiance)
{
    float distance;
    return CacheLookupSignal(position, normal, irradiance, distance);
}

void CacheInsert(float3 position, float3 normal, float3 irradiance, float hitDistance)
{
    if (CacheParameters.x < 0.5f || any(isnan(irradiance)) || any(isinf(irradiance)))
    {
        return;
    }
    uint key, bucket;
    CacheCell(position, normal, key, bucket);
    uint mask = (uint)CacheParameters.z - 1u;
    int found = -1;
    [loop] for (uint probe = 0; probe < kCacheProbes && found < 0; ++probe)
    {
        uint slot = (bucket + probe) & mask;
        if (CacheKeys.Load(slot * 4) == key)
        {
            found = (int)slot;
        }
    }
    [loop] for (uint claim = 0; claim < kCacheProbes && found < 0; ++claim)
    {
        uint slot = (bucket + claim) & mask;
        uint previous;
        CacheKeys.InterlockedCompareExchange(slot * 4, 0u, key, previous);
        if (previous == 0u || previous == key)
        {
            found = (int)slot;
        }
    }
    if (found < 0)
    {
        return; // 近傍が満杯の場合、このサンプルはキャッシュしない
    }
    uint3 value = (uint3)(min(max(irradiance, 0), kCacheMaxIrradiance) * kCacheScale + 0.5f);
    uint address = (uint)found * 32;
    uint ignored;
    CacheAccumulation.InterlockedAdd(address + 0, value.x, ignored);
    CacheAccumulation.InterlockedAdd(address + 4, value.y, ignored);
    CacheAccumulation.InterlockedAdd(address + 8, value.z, ignored);
    CacheAccumulation.InterlockedAdd(address + 12, 1u, ignored);
    CacheAccumulation.InterlockedMax(address + 16, asuint(clamp(hitDistance, 0, 65504)), ignored);
}

// NVIDIA MathLib v11 と同様の有界 VNDF サンプリング。反射レイを面の上側に保つ。
// https://github.com/NVIDIA-RTX/MathLib/blob/aa432ad9a80691eb8c3ec4d576bbad5ac100fdc8/ml.hlsli
float3 SampleVisibleGgx(float3 view, float3 normal, float roughness, float2 random)
{
    float3 tangent = normalize(cross(abs(normal.y) < 0.95f ? float3(0, 1, 0) : float3(1, 0, 0), normal));
    float3 bitangent = cross(normal, tangent);
    float alpha = roughness * roughness;
    float3 localView = float3(dot(view, tangent), dot(view, bitangent), max(dot(view, normal), 0.0001f));
    float3 stretchedView = normalize(float3(localView.xy * alpha, localView.z));
    float boundScale = 1 + length(localView.xy);
    float boundSquared = boundScale * boundScale;
    float bound = (1 - alpha * alpha) * boundSquared / (boundSquared + alpha * alpha * localView.z * localView.z);
    float height = 1 - random.y * (1 + bound * stretchedView.z);
    float radius = sqrt(max(1 - height * height, 0));
    float angle = 6.2831853f * random.x;
    float3 hemisphereNormal = float3(radius * cos(angle), radius * sin(angle), height) + stretchedView;
    float3 localHalf = normalize(float3(hemisphereNormal.xy * alpha, max(hemisphereNormal.z, 0)));
    return normalize(tangent * localHalf.x + bitangent * localHalf.y + normal * localHalf.z);
}

uint LoadReflectionIndex(uint instance, uint index, ReflectionGeometry mesh)
{
    if (mesh.Layout.y == 0)
    {
        return index;
    }
    uint offset = mesh.Layout.w + index * (mesh.Layout.y / 8);
    uint packed = SceneIndices[NonUniformResourceIndex(instance)].Load(offset & ~3u);
    return mesh.Layout.y == 16 ? (packed >> ((offset & 2) * 8)) & 0xffff : packed;
}

float3 SampleEmission(uint instance, float2 uv)
{
    ReflectionGeometry mesh = ReflectionMeshes[instance];
    float3 emission = mesh.Emission.rgb * mesh.Emission.a;
    if (mesh.Surface.y != 0)
    {
        emission *= EmissionTextures[NonUniformResourceIndex(instance)].SampleLevel(MaterialSampler, uv, 0).rgb;
    }
    return emission;
}

float TraceBounceVisibility(float3 position, float3 normal, float3 direction, float distance)
{
    RayDesc ray;
    ray.Origin = position + normal * SceneRayParameters.y;
    ray.Direction = direction;
    ray.TMin = SceneRayParameters.y * 0.25f;
    ray.TMax = max(distance - SceneRayParameters.y, ray.TMin + 0.001f);
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    CountTelemetryRay(RAY_TELEMETRY_RADIANCE_SHADOW);
    query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0x01, ray);
    while (query.Proceed())
    {
    }
    return query.CommittedStatus() == COMMITTED_NOTHING ? 1 : 0;
}

float3 SampleVisibleReflectionSky(float3 position, float3 normal, float3 direction, float angularFootprint,
                                  bool includeDiscs)
{
    // 環境の参照はレイが何にもヒットしなかったことを表す。空を参照する前に、
    // 影の投影を無効に設定した物体も含め、全ての反射形状を検査する。
    RayDesc ray;
    ray.Origin = position + normal * SceneRayParameters.y;
    ray.Direction = direction;
    ray.TMin = SceneRayParameters.y * 0.25f;
    ray.TMax = SceneRayParameters.x;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    CountTelemetryRay(RAY_TELEMETRY_REFLECTION);
    query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0x03, ray);
    while (query.Proceed())
    {
    }
    if (query.CommittedStatus() != COMMITTED_NOTHING)
    {
        return 0;
    }
    return EvaluatePhysicalSkyFast(direction, angularFootprint, includeDiscs);
}

// 反射の二次ヒットでの POM。レイ上では画素微分が無いので、1 回の視差オフセット（Offset Limiting）と
// 高さマップの中心差分による法線に絞る。ラスター側と同じく白 = 上面、黒 = 最深部。
float ReflectionHeight(uint instance, float2 uv, float lod)
{
    return saturate(HeightTextures[NonUniformResourceIndex(instance)].SampleLevel(MaterialSampler, uv, lod).r);
}

void ApplyReflectionParallax(uint instance, ReflectionGeometry mesh, float3 p0, float3 p1, float3 p2, float2 t0,
                             float2 t1, float2 t2, float3 incoming, float hitDistance, inout float2 uv,
                             inout float3 normal)
{
    float depthScale = clamp(mesh.Material.w, 0.0f, 0.2f);
    if (mesh.Surface.z == 0u || depthScale <= 0.0f)
    {
        return;
    }
    float3 e1 = p1 - p0, e2 = p2 - p0;
    float2 d1 = t1 - t0, d2 = t2 - t0;
    float determinant = d1.x * d2.y - d1.y * d2.x;
    if (abs(determinant) < 1e-12f)
    {
        return;
    }
    float3 tangent = (e1 * d2.y - e2 * d1.y) / determinant;
    float3 uvBitangent = (e2 * d1.x - e1 * d2.x) / determinant;
    tangent -= normal * dot(normal, tangent);
    if (dot(tangent, tangent) < 1e-12f || dot(uvBitangent, uvBitangent) < 1e-12f)
    {
        return;
    }
    float worldPerUv = sqrt(length(tangent) * length(uvBitangent));
    tangent = normalize(tangent);
    float3 bitangent = normalize(cross(normal, tangent));
    if (dot(bitangent, uvBitangent) < 0.0f)
    {
        bitangent = -bitangent;
    }
    uint width, height, levels;
    HeightTextures[NonUniformResourceIndex(instance)].GetDimensions(0u, width, height, levels);
    // レイの到達距離から UV のフットプリントを見積もり、ミップを選ぶ（遠い反射ほど粗く・安定に）。
    float uvFootprint = max(hitDistance, 0.01f) * 0.002f / max(worldPerUv, 1e-4f);
    float lod = clamp(log2(max(uvFootprint * max(width, height), 1.0f)), 0.0f, (float)max(levels, 1u) - 1.0f);
    float3 view = -incoming;
    float facing = dot(normal, view);
    if (facing > 0.05f)
    {
        float3 viewTS = float3(dot(view, tangent), dot(view, bitangent), facing);
        float depth = 1.0f - ReflectionHeight(instance, uv, lod);
        // Offset Limiting：facing で割らず、寝た角度でも破綻しない。
        uv -= viewTS.xy * depth * depthScale * smoothstep(0.05f, 0.2f, facing);
    }
    float2 e = max(1.0f / float2(max(width, 1u), max(height, 1u)) * exp2(lod), 1e-5f);
    float left = ReflectionHeight(instance, uv - float2(e.x, 0), lod);
    float right = ReflectionHeight(instance, uv + float2(e.x, 0), lod);
    float down = ReflectionHeight(instance, uv - float2(0, e.y), lod);
    float up = ReflectionHeight(instance, uv + float2(0, e.y), lod);
    float2 slope = float2(right - left, up - down) / (2.0f * e) * depthScale;
    float3 tangentNormal = normalize(float3(-slope, 1.0f));
    float3 mapped = tangentNormal.x * tangent + tangentNormal.y * bitangent + tangentNormal.z * normal;
    normal = dot(mapped, mapped) > 1e-8f ? normalize(mapped) : normal;
}

float3 ShadeReflectionHit(uint instance, uint primitive, float2 barycentrics, float3 position, float3 incoming,
                          bool diffuseOnly, uint randomSeed, out float3 surfaceDiffuse, out float3 surfaceNormal)
{
    ReflectionGeometry mesh = ReflectionMeshes[instance];
    float3 weights = float3(1 - barycentrics.x - barycentrics.y, barycentrics);
    float3 normal = 0;
    float2 uv = 0;
    float3 albedo = 0;
    [unroll] for (uint corner = 0; corner < 3; ++corner)
    {
        uint vertex = LoadReflectionIndex(instance, primitive * 3 + corner, mesh);
        uint offset = mesh.Layout.z + vertex * mesh.Layout.x;
        normal += asfloat(SceneVertices[NonUniformResourceIndex(instance)].Load3(offset + 12)) * weights[corner];
        uv += asfloat(SceneVertices[NonUniformResourceIndex(instance)].Load2(offset + 24)) * weights[corner];
        albedo += asfloat(SceneVertices[NonUniformResourceIndex(instance)].Load3(offset + 32)) * weights[corner];
    }
    normal = normalize(mul(float4(normal, 0), mesh.NormalToWorld).xyz);
    if (dot(normal, incoming) > 0)
    {
        normal = -normal;
    }
    [branch] if (mesh.Surface.z != 0u)
    {
        float3 corners[3];
        float2 cornerUv[3];
        [unroll] for (uint c = 0; c < 3; ++c)
        {
            uint vertex = LoadReflectionIndex(instance, primitive * 3 + c, mesh);
            uint offset = mesh.Layout.z + vertex * mesh.Layout.x;
            corners[c] =
                mul(float4(asfloat(SceneVertices[NonUniformResourceIndex(instance)].Load3(offset)), 1), mesh.World).xyz;
            cornerUv[c] = asfloat(SceneVertices[NonUniformResourceIndex(instance)].Load2(offset + 24));
        }
        float hitDistance = length(position - ReflectionCamera.xyz);
        ApplyReflectionParallax(instance, mesh, corners[0], corners[1], corners[2], cornerUv[0], cornerUv[1],
                                cornerUv[2], incoming, hitDistance, uv, normal);
    }
    float3 lumenTotal, lumenIndirect, lumenAlbedo, lumenEmission;
    bool lumenCached =
        LumenLookupMaterial(instance, position, normal, lumenTotal, lumenIndirect, lumenAlbedo, lumenEmission);
    // modelshaderPS と合わせ、共有マテリアルアセットは頂点 RGB を無視する。
    if (lumenCached)
    {
        albedo = lumenAlbedo;
    }
    else
    {
        if ((mesh.Surface.w & 1u) != 0u)
        {
            albedo = 1.0f;
        }
        if (mesh.Material.z > 0.5f)
        {
            albedo *= SceneTextures[NonUniformResourceIndex(instance)].SampleLevel(MaterialSampler, uv, 0).rgb;
        }
        albedo *= mesh.BaseColor.rgb;
    }
    float metallic = saturate(mesh.Material.y);
    float roughness = clamp(mesh.Material.x, 0.02f, 1);
    float3 f0 = lerp(0.04f, max(albedo, 0), metallic);
    float3 view = -incoming;
    float nDotV = max(dot(normal, view), 0.0001f);
    float3 skyFresnel = f0 + (1 - f0) * pow(1 - saturate(nDotV), 5);
    float3 diffuse = max(albedo, 0) * (1 - metallic) / 3.14159265f;
    surfaceDiffuse = diffuse;
    surfaceNormal = normal;
    if (diffuseOnly && lumenCached)
    {
        return diffuse * lumenTotal;
    }
    float3 radiance = 0;
    if (!diffuseOnly)
    {
        // 空が見える場所だけで、低コストの二次環境近似を維持する。
        // 太陽と月の円盤は直接光に属し、拡散環境光の近似には含めない。
        radiance =
            (lumenCached ? diffuse * lumenIndirect
                         : diffuse * SampleVisibleReflectionSky(position, normal, normal, 0.01f, false) * 0.12f) +
            skyFresnel * SampleVisibleReflectionSky(position, normal, reflect(incoming, normal),
                                                    max(0.001f, roughness * 0.02f), true) +
            (lumenCached ? lumenEmission : SampleEmission(instance, uv));
    }
    int count = (int)ReflectionExtent.w;
    [branch] if (diffuseOnly)
    {
        // GI の各反射にシャドウレイを 1 本使い、遮蔽前の寄与に比例してライトを選ぶ
        // （1 パスの重み付きリザーバーサンプリング）。以前の一様選択では、
        // 弱いライトや範囲外のライトにレイの大半を費やし、0.5 rpp ではノイズになっていた。
        // 不偏な推定値：estimate = f_i * V_i / (w_i / W)。
        float random = ((ReflectionHash(randomSeed) >> 8) + 0.5f) / 16777216.0f;
        float totalWeight = 0;
        float selectedWeight = 0;
        float3 selectedDirection = 0;
        float3 selectedRadiance = 0;
        float selectedRange = 0;
        bool selectedShadow = false;
        // Lumen モードでは、ライトの部分集合を制限する（包含確率 M/N、N/M 倍で補正）。キャッシュの
        // ミスやプローブのヒットで O(ライト数) を払わない。既存の DXR GI は全ループを維持する。
        uint candidates = (uint)count;
        uint stride = 1;
        uint start = 0;
        float subsetScale = 1;
        if (LumenCounts.x != 0 && count > 32)
        {
            candidates = 32;
            stride = (uint)count / 32;
            start = ReflectionHash(randomSeed * 747796405u + 2891336453u) % (uint)count;
            subsetScale = count / 32.0f;
        }
        [loop] for (uint k = 0; k < candidates; ++k)
        {
            ReflectionLight light = ReflectionLights[(start + k * stride) % (uint)count];
            float3 direction;
            float attenuation, volume;
            ResolveSingleLightCommon(position, light.Direction, light.PositionType, light.Extras, direction,
                                     attenuation, volume);
            float3 incident = light.Color.rgb * max(light.Color.a, 0) * saturate(dot(normal, direction)) * attenuation;
            float weight = dot(incident, float3(0.2126f, 0.7152f, 0.0722f));
            if (weight <= 0.0000001f)
            {
                continue;
            }
            totalWeight += weight;
            float probability = weight / totalWeight;
            if (random < probability)
            {
                random /= probability;
                selectedWeight = weight;
                selectedDirection = direction;
                selectedRadiance = incident;
                selectedShadow = light.Flags.x != 0;
                selectedRange = light.PositionType.w < 0.5f
                                    ? SceneRayParameters.x
                                    : min(length(light.PositionType.xyz - position), SceneRayParameters.x);
            }
            else
            {
                random = (random - probability) / max(1 - probability, 0.000001f);
            }
        }
        if (selectedWeight > 0)
        {
            float shadow =
                selectedShadow ? TraceBounceVisibility(position, normal, selectedDirection, selectedRange) : 1;
            radiance += diffuse * selectedRadiance * (shadow * totalWeight / selectedWeight * subsetScale);
        }
        return radiance;
    }
    float shadowRandom = ((ReflectionHash(randomSeed ^ 0x5bd1e995u) >> 8) + 0.5f) / 16777216.0f;
    float shadowTotalWeight = 0;
    float shadowSelectedWeight = 0;
    float3 shadowSelectedContribution = 0;
    float3 shadowSelectedDirection = 0;
    float shadowSelectedRange = 0;
    [loop] for (int i = 0; i < count; ++i)
    {
        ReflectionLight light = ReflectionLights[i];
        float3 direction;
        float attenuation, volume;
        ResolveSingleLightCommon(position, light.Direction, light.PositionType, light.Extras, direction, attenuation,
                                 volume);
        float cosine = saturate(dot(normal, direction));
        if (cosine * attenuation < 0.00001f)
        {
            continue;
        }
        float3 halfVector = normalize(view + direction);
        float nDotH = saturate(dot(normal, halfVector));
        float vDotH = saturate(dot(view, halfVector));
        float alphaSquared = max(pow(roughness, 4), 0.000001f);
        float denominator = nDotH * nDotH * (alphaSquared - 1) + 1;
        float distribution = alphaSquared / max(3.14159265f * denominator * denominator, 0.000001f);
        float k = (roughness + 1) * (roughness + 1) / 8;
        float geometry = (nDotV / (nDotV * (1 - k) + k)) * (cosine / (cosine * (1 - k) + k));
        float3 fresnel = f0 + (1 - f0) * pow(1 - vDotH, 5);
        float3 specular = distribution * geometry * fresnel / max(4 * nDotV * cosine, 0.00001f);
        float3 response = (1 - fresnel) * diffuse + specular;
        float3 contribution = response * light.Color.rgb * max(light.Color.a, 0) * cosine * attenuation;
        // 結果は同じだが、ピクセルを変えないライトにはシャドウレイを発行しない。
        if (all(contribution < 0.000001f))
        {
            continue;
        }
        if (light.Flags.x == 0)
        {
            radiance += contribution; // 影を落とさないライトはそのまま足す（レイ不要）
            continue;
        }
        // 負荷削減：影を落とすライトは「寄与に比例して 1 灯選び、影のレイは 1 本だけ」（重み付きリザーバー、不偏）。
        // 以前はライトごとに影のレイを飛ばしていたので、ライトが多いと反射のコストがライト数に比例していた
        float weight = dot(contribution, float3(0.2126f, 0.7152f, 0.0722f));
        if (weight <= 0.0000001f)
        {
            continue;
        }
        shadowTotalWeight += weight;
        float probability = weight / shadowTotalWeight;
        if (shadowRandom < probability)
        {
            shadowRandom /= probability;
            shadowSelectedWeight = weight;
            shadowSelectedContribution = contribution;
            shadowSelectedDirection = direction;
            shadowSelectedRange = light.PositionType.w < 0.5f
                                      ? SceneRayParameters.x
                                      : min(length(light.PositionType.xyz - position), SceneRayParameters.x);
        }
        else
        {
            shadowRandom = (shadowRandom - probability) / max(1 - probability, 0.000001f);
        }
    }
    if (shadowSelectedWeight > 0)
    {
        float shadow = TraceSceneShadow(position, normal, shadowSelectedDirection, shadowSelectedRange);
        radiance += shadowSelectedContribution * (shadow * shadowTotalWeight / shadowSelectedWeight);
    }
    return radiance;
}

float3 ShadeReflectionHit(uint instance, uint primitive, float2 barycentrics, float3 position, float3 incoming,
                          bool diffuseOnly = false, uint randomSeed = 0)
{
    float3 surfaceDiffuse, surfaceNormal;
    return ShadeReflectionHit(instance, primitive, barycentrics, position, incoming, diffuseOnly, randomSeed,
                              surfaceDiffuse, surfaceNormal);
}

bool AdaptiveReflection(uint2 pixel, float depth, float3 position, float3 normal, float roughness, out float4 signal)
{
    signal = 0;
    if (AdaptiveSampling.x < .5 || AdaptiveSampling.y < .5 || roughness < AdaptiveSampling.z)
    {
        return false;
    }
    uint period = roughness > .55 ? 4u : 2u;
    if (((pixel.x + pixel.y * 2u + ReflectionExtent.z) % period) == 0)
    {
        return false;
    }
    float2 motion = GiVelocity.Load(int3(pixel, 0)).zw;
    if (!all(isfinite(motion)) || length(motion * ReflectionExtent.xy) > .5)
    {
        return false;
    }
    float tolerance = max(2e-5, abs(1 - depth) * .005);
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        int2 offset = int2(0, -1);
        if (i == 0)
        {
            offset = int2(1, 0);
        }
        else if (i == 1)
        {
            offset = int2(-1, 0);
        }
        else if (i == 2)
        {
            offset = int2(0, 1);
        }
        int2 p = clamp(int2(pixel) + offset, 0, int2(ReflectionExtent.xy) - 1);
        if (abs(ReflectionDepth.Load(int3(p, 0)) - depth) > tolerance)
        {
            return false;
        }
        if (dot(normalize(ReflectionNormal.Load(int3(p, 0)).xyz * 2 - 1), normal) < .995)
        {
            return false;
        }
    }
    float4 previous = mul(float4(position, 1), AdaptivePreviousViewProjection);
    if (previous.w <= 0)
    {
        return false;
    }
    float3 ndc = previous.xyz / previous.w;
    float2 uv = ndc.xy * float2(.5, -.5) + .5;
    if (any(uv <= 0) || any(uv >= 1) || ndc.z < 0 || ndc.z > 1)
    {
        return false;
    }
    uint2 old = min(uint2(uv * ReflectionExtent.xy), ReflectionExtent.xy - 1);
    float4 guide = AdaptiveGuide.Load(int3(old, 0));
    if (abs(guide.w - ndc.z) > tolerance || dot(guide.xyz, normal) < .995)
    {
        return false;
    }
    signal = AdaptiveRadiance.Load(int3(old, 0));
    return all(isfinite(signal)) && all(signal.rgb >= 0);
}

[numthreads(8, 8, 1)] void CaptureAdaptiveGuide(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= ReflectionExtent.xy))
    {
        return;
    }
    AdaptiveGuideOut[id.xy] =
        float4(normalize(ReflectionNormal.Load(int3(id.xy, 0)).xyz * 2 - 1), ReflectionDepth.Load(int3(id.xy, 0)));
}[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)
{
    // 負荷削減：NRD がある場合は半分のピクセル（チェッカーボード）だけトレースし、NRD が残りを復元する
    // （GI の TraceDiffuse と同じ詰め方：トレースしたピクセルを (x >> 1, y) に格納。NRD の CheckerboardMode::BLACK）
    uint2 storage = id.xy;
    uint2 pixel = id.xy;
    const bool checkerboard = (ReSTIRState.w & 0x10000u) != 0u;
    if (checkerboard)
    {
        pixel.x = 2 * pixel.x + ((pixel.y + ReflectionExtent.z) & 1u);
        if (storage.y >= ReflectionExtent.y || storage.x >= (ReflectionExtent.x + 1) / 2)
        {
            return;
        }
        if (pixel.x >= ReflectionExtent.x)
        {
            NoisyRadiance[storage] = 0;
            return;
        }
    }
    else if (any(pixel >= ReflectionExtent.xy))
    {
        return;
    }
    float depth = ReflectionDepth.Load(int3(pixel, 0));
    if (depth >= 0.99999f)
    {
        NoisyRadiance[storage] = 0;
        ReflectionRadiance[pixel] = 0;
        return;
    }
    float2 uv = (pixel + 0.5f) / ReflectionExtent.xy;
    float4 world = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1), ReflectionInvViewProjection);
    float3 position = world.xyz / world.w;
    float3 normal = normalize(ReflectionNormal.Load(int3(pixel, 0)).xyz * 2 - 1);
    float3 view = normalize(ReflectionCamera.xyz - position);
    float roughness = clamp(ReflectionMaterial.Load(int3(pixel, 0)).y, 0.02f, 1);
    float4 adaptiveSignal;
    if (AdaptiveReflection(pixel, depth, position, normal, roughness, adaptiveSignal))
    {
        ReflectionRadiance[pixel] = adaptiveSignal;
        float distance = abs(adaptiveSignal.w) > 0 ? abs(adaptiveSignal.w) : 65504;
#if USE_NRD
        float viewZ = abs(mul(float4(position, 1), ReflectionView).z);
        float normalized = REBLUR_FrontEnd_GetNormHitDist(distance, viewZ, float3(3, .1, 20), roughness);
        NoisyRadiance[storage] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(adaptiveSignal.rgb, normalized, true);
#else
        NoisyRadiance[storage] = float4(adaptiveSignal.rgb, distance);
#endif
        return;
    }

    uint seed = ReflectionHash(pixel.x + pixel.y * ReflectionExtent.x + ReflectionExtent.z * 104729u);
    float2 random = float2((seed & 0xffff) + 0.5f, (seed >> 16) + 0.5f) / 65536;
    // 粗い面ではヒットのシェーディングを省くが、空を使う前の可視性レイは必要。
    const float roughnessCutoff = (ReSTIRState.w & 0xffffu) / 1000.0f;
    if (roughnessCutoff < 0.999f && roughness > roughnessCutoff)
    {
        float3 skyRadiance =
            SampleVisibleReflectionSky(position, normal, reflect(-view, normal), max(0.001f, roughness * 0.02f), true);
        ReflectionRadiance[pixel] = float4(skyRadiance, 0);
#if USE_NRD
        float viewZCut = abs(mul(float4(position, 1), ReflectionView).z);
        NoisyRadiance[storage] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(
            skyRadiance, REBLUR_FrontEnd_GetNormHitDist(65504, viewZCut, float3(3, 0.1f, 20), roughness), true);
#else
        NoisyRadiance[storage] = float4(skyRadiance, 65504);
#endif
        return;
    }
    float alpha = roughness * roughness;
    float3 halfVector = SampleVisibleGgx(view, normal, roughness, random);
    float3 direction = reflect(-view, halfVector);
    RayDesc ray;
    ray.Origin = position + normal * SceneRayParameters.y;
    ray.Direction = direction;
    ray.TMin = SceneRayParameters.y * 0.25f;
    ray.TMax = SceneRayParameters.x;
    bool hit = false;
    float hitDistance = 65504;
    float3 radiance = 0;
    float nDotL = dot(normal, direction);
    if (nDotL > 0)
    {
        RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
        CountTelemetryRay(RAY_TELEMETRY_REFLECTION);
        query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0x03, ray);
        while (query.Proceed())
        {
        }
        hit = query.CommittedStatus() != COMMITTED_NOTHING;
        hitDistance = hit ? query.CommittedRayT() : 65504;
        radiance = hit ? ShadeReflectionHit(query.CommittedInstanceID(), query.CommittedPrimitiveIndex(),
                                            query.CommittedTriangleBarycentrics(), ray.Origin + direction * hitDistance,
                                            direction, false, seed * 7919u + 17u)
                       : EvaluatePhysicalSky(direction, max(0.001f, roughness * 0.02f));
        // サンプリング密度で除算した後の Smith のマスキングと有界ローブの確率。
        float masking = 2 * nDotL / (nDotL + sqrt(alpha * alpha + (1 - alpha * alpha) * nDotL * nDotL));
        float nDotV = max(dot(normal, view), 0.0001f);
        float projectedView = sqrt(max(1 - nDotV * nDotV, 0));
        float boundSquared = (1 + projectedView) * (1 + projectedView);
        float bound = (1 - alpha * alpha) * boundSquared / (boundSquared + alpha * alpha * nDotV * nDotV);
        float stretchedLength = sqrt(alpha * alpha * (1 - nDotV * nDotV) + nDotV * nDotV);
        radiance *= masking * (bound * nDotV + stretchedLength) / (nDotV + stretchedLength);
    }
    radiance = min(max(radiance, 0), 1000);
    float4 hitClip = mul(float4(ray.Origin + direction * min(hitDistance, ray.TMax), 1), ReflectionViewProjection);
    bool offscreen = hitClip.w <= 0 || any(abs(hitClip.xy) > hitClip.w) || hitClip.z < 0 || hitClip.z > hitClip.w;
    // 符号付き距離はキャプチャ検証に使い、RGB はシェーディング信号として扱う。
    float signedHitDistance = 0;
    if (hit)
    {
        signedHitDistance = hitDistance;
        if (offscreen)
        {
            signedHitDistance = -hitDistance;
        }
    }
    ReflectionRadiance[pixel] = float4(radiance, signedHitDistance);
#if USE_NRD
    float viewZ = abs(mul(float4(position, 1), ReflectionView).z);
    float normalizedDistance = REBLUR_FrontEnd_GetNormHitDist(hitDistance, viewZ, float3(3, 0.1f, 20), roughness);
    NoisyRadiance[storage] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, normalizedDistance, true);
#else
    NoisyRadiance[storage] = float4(radiance, hitDistance);
#endif
}

#if USE_NRD
[numthreads(8, 8, 1)] void Compose(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= ReflectionExtent.xy))
    {
        return;
    }
    float depth = ReflectionDepth.Load(int3(id.xy, 0));
    float2 uv = (id.xy + 0.5f) / ReflectionExtent.xy;
    float4 world = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1), ReflectionInvViewProjection);
    float viewZ = abs(mul(float4(world.xyz / world.w, 1), ReflectionView).z);
    if (depth < 0.99999f && viewZ < ReflectionCamera.w)
    {
        ReflectionRadiance[id.xy] =
            float4(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(FilteredRadiance.Load(int3(id.xy, 0))).rgb,
                   ReflectionRadiance[id.xy].w);
    }
}
#endif

// 1 つの発光メッシュから三角形を 1 つ一様にサンプリングする。面積と逆二乗減衰で、
// 設定された放射輝度を入射放射照度へ変換する。可視性レイで壁越しの光漏れを防ぐ。
float3 SampleEmissiveIrradiance(float3 position, float3 normal, uint seed, uint2 pixel, bool checkerboard)
{
    uint emitterCount = uint(GiParameters.w);
    if (emitterCount == 0)
    {
        return 0;
    }
    uint sequence = PixelSampleIndex(ReflectionExtent.z, checkerboard);
    SamplePair selection = SampleSequence(pixel.x, pixel.y, sequence, 1u);
    uint emitterIndex = ReflectionStability.x > .5 ? min(uint(selection.x * emitterCount), emitterCount - 1)
                                                   : ReflectionHash(seed) % emitterCount;
    uint instance = EmissiveMeshes[emitterIndex];
    ReflectionGeometry mesh = ReflectionMeshes[instance];
    uint triangleCount = mesh.Surface.x;
    if (triangleCount == 0)
    {
        return 0;
    }
    uint primitive = ReflectionStability.x > .5 ? min(uint(selection.y * triangleCount), triangleCount - 1)
                                                : ReflectionHash(seed + 1) % triangleCount;
    uint random = ReflectionHash(seed + 2);
    float2 sample = float2((random & 65535) + 0.5f, (random >> 16) + 0.5f) / 65536;
    if (ReflectionStability.x > .5)
    {
        SamplePair sequenceSample = SampleSequence(pixel.x, pixel.y, sequence, 2u);
        sample = float2(sequenceSample.x, sequenceSample.y);
    }
    float root = sqrt(sample.x);
    float3 weights = float3(1 - root, root * (1 - sample.y), root * sample.y);
    float3 vertices[3];
    float2 uv = 0;
    [unroll] for (uint corner = 0; corner < 3; ++corner)
    {
        uint index = LoadReflectionIndex(instance, primitive * 3 + corner, mesh);
        uint offset = mesh.Layout.z + index * mesh.Layout.x;
        vertices[corner] =
            mul(float4(asfloat(SceneVertices[NonUniformResourceIndex(instance)].Load3(offset)), 1), mesh.World).xyz;
        uv += asfloat(SceneVertices[NonUniformResourceIndex(instance)].Load2(offset + 24)) * weights[corner];
    }
    float3 areaVector = cross(vertices[1] - vertices[0], vertices[2] - vertices[0]);
    float twiceArea = length(areaVector);
    if (twiceArea < 0.000001f)
    {
        return 0;
    }
    float3 lightPosition = vertices[0] * weights.x + vertices[1] * weights.y + vertices[2] * weights.z;
    float3 offset = lightPosition - position;
    float distanceSquared = dot(offset, offset);
    float distance = sqrt(distanceSquared);
    if (distance < 2 * SceneRayParameters.y || distance > GiParameters.x)
    {
        return 0;
    }
    float3 direction = offset / distance;
    float cosine = saturate(dot(normal, direction));
    // 発光マテリアルは両面から放射し、既存の両面レイシーンと一致させる。
    float emitterCosine = abs(dot(areaVector / twiceArea, -direction));
    if (cosine * emitterCosine <= 0)
    {
        return 0;
    }
    RayDesc ray;
    ray.Origin = position + normal * SceneRayParameters.y;
    float3 biasedOffset = lightPosition - ray.Origin;
    ray.Direction = normalize(biasedOffset);
    ray.TMin = SceneRayParameters.y * 0.25f;
    ray.TMax = max(length(biasedOffset) - SceneRayParameters.y, ray.TMin);
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    CountTelemetryRay(RAY_TELEMETRY_RADIANCE_SHADOW);
    query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0x03, ray);
    while (query.Proceed())
    {
    }
    if (query.CommittedStatus() != COMMITTED_NOTHING)
    {
        return 0;
    }
    // 選んだ発光メッシュの立体角の推定値（三角形選択の面積 PDF）。
    // メッシュが半球（2 pi sr）を超えて覆うことはない。推定値を制限して、受光点のすぐ近くの
    // サンプルで発生する 1/d^2 の特異点を除く。これにより発光物体の周りで
    // フレームごとに現れる孤立した輝点ノイズを抑える。それ以外では不偏性を保つ。
    float solidAngle = emitterCosine * 0.5f * twiceArea * triangleCount / max(distanceSquared, 0.0001f);
    solidAngle = min(solidAngle, 6.2831853f);
    return SampleEmission(instance, uv) * (cosine * solidAngle * emitterCount);
}

[numthreads(8, 8, 1)] void TraceDiffuse(uint3 id : SV_DispatchThreadID) {
    // NRD のチェッカーボード入力は詰めた配置を使い、横並びの各組の追跡ピクセルを
    // (x >> 1, y)、つまりテクスチャの左半分へ格納する。全解像度の位置へ書くと、
    // NRD が画面を横に引き伸ばした像と過去フレームの古いテクセルを読み取り、
    // デノイズしたノイズに物体が焼き付いたように見えていた。
    uint2 storage = id.xy;
    uint2 pixel = id.xy;
    bool checkerboard = GiParameters.z > 0.5f;
    if (checkerboard)
    {
        // NRD と同じ偶奇：((x ^ y ^ frameIndex) & 1) == 0 の場合にデータを持つ（CheckerboardMode::BLACK）。
        pixel.x = 2 * pixel.x + ((pixel.y + ReflectionExtent.z) & 1u);
        if (storage.y >= ReflectionExtent.y || storage.x >= (ReflectionExtent.x + 1) / 2)
        {
            return;
        }
        if (pixel.x >= ReflectionExtent.x)
        {
            // 幅が奇数の場合も末尾の詰めたテクセルを定義する。NRD はそこをデータとして読まない。
            NoisyRadiance[storage] = 0;
            return;
        }
    }
    else if (any(pixel >= ReflectionExtent.xy))
    {
        return;
    }
    float depth = ReflectionDepth.Load(int3(pixel, 0));
    NoisyRadiance[storage] = 0;
    if (depth >= 0.99999f)
    {
        return;
    }
    float2 uv = (pixel + 0.5f) / ReflectionExtent.xy;
    float4 world = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1), ReflectionInvViewProjection);
    float3 position = world.xyz / world.w;
    float3 normal = normalize(ReflectionNormal.Load(int3(pixel, 0)).xyz * 2 - 1);
    float3 lumenIrradiance;
    float lumenDistance;
    if (LumenGatherIrradiance(position, normal, pixel, lumenIrradiance, lumenDistance))
    {
#if USE_NRD
        float viewZ = abs(mul(float4(position, 1), ReflectionView).z);
        float normalized = REBLUR_FrontEnd_GetNormHitDist(lumenDistance, viewZ, float3(3, 0.1f, 20), 1);
        NoisyRadiance[storage] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(lumenIrradiance, normalized, true);
#else
        NoisyRadiance[storage] = float4(lumenIrradiance, lumenDistance);
#endif
        return;
    }
    uint period = max(1u, (uint)AdaptiveSampling.w);
    if (period > 1 && ((ReflectionHash(pixel.x + pixel.y * ReflectionExtent.x) + ReflectionExtent.z) % period) != 0)
    {
        float2 motion = GiVelocity.Load(int3(pixel, 0)).zw;
        float3 cached;
        float cachedDistance;
        bool stable = all(isfinite(motion)) && length(motion * ReflectionExtent.xy) < .25;
        [unroll] for (uint i = 0; i < 4; ++i)
        {
            int2 offset = int2(0, -1);
            if (i == 0)
            {
                offset = int2(1, 0);
            }
            else if (i == 1)
            {
                offset = int2(-1, 0);
            }
            else if (i == 2)
            {
                offset = int2(0, 1);
            }
            int2 p = clamp(int2(pixel) + offset, 0, int2(ReflectionExtent.xy) - 1);
            stable = stable && abs(ReflectionDepth.Load(int3(p, 0)) - depth) < max(2e-5, abs(1 - depth) * .005) &&
                     dot(normalize(ReflectionNormal.Load(int3(p, 0)).xyz * 2 - 1), normal) > .995;
        }
        if (stable && CacheLookupSignal(position, normal, cached, cachedDistance))
        {
#if USE_NRD
            float viewZ = abs(mul(float4(position, 1), ReflectionView).z);
            float normalized = REBLUR_FrontEnd_GetNormHitDist(cachedDistance, viewZ, float3(3, .1, 20), 1);
            NoisyRadiance[storage] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(cached, normalized, true);
#else
            NoisyRadiance[storage] = float4(cached, cachedDistance);
#endif
            return;
        }
    }
    uint seed = ReflectionHash(pixel.x + pixel.y * ReflectionExtent.x + ReflectionExtent.z * 104729u);
    float2 random = float2((seed & 65535) + 0.5f, (seed >> 16) + 0.5f) / 65536;
    if (ReflectionStability.x > .5)
    {
        SamplePair sequenceSample =
            SampleSequence(pixel.x, pixel.y, PixelSampleIndex(ReflectionExtent.z, checkerboard), 0u);
        random = float2(sequenceSample.x, sequenceSample.y);
    }
    float radius = sqrt(random.x);
    float angle = 6.2831853f * random.y;
    float3 tangent = normalize(cross(abs(normal.y) < 0.95f ? float3(0, 1, 0) : float3(1, 0, 0), normal));
    float3 direction =
        tangent * (radius * cos(angle)) + cross(normal, tangent) * (radius * sin(angle)) + normal * sqrt(1 - random.x);
    RayDesc ray;
    ray.Origin = position + normal * SceneRayParameters.y;
    ray.Direction = direction;
    ray.TMin = SceneRayParameters.y * 0.25f;
    ray.TMax = GiParameters.x;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    CountTelemetryRay(RAY_TELEMETRY_GI);
    query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0x03, ray);
    while (query.Proceed())
    {
    }
    float hitDistance = ray.TMax;
    float3 incoming = 0;
    float3 secondaryNormal = -direction;
    if (query.CommittedStatus() != COMMITTED_NOTHING)
    {
        hitDistance = query.CommittedRayT();
        // 二重加算を避けるため、発光は面積サンプリングで別に推定する。
        float3 hitPosition = ray.Origin + direction * hitDistance;
        float3 hitDiffuse, hitNormal;
        incoming = ShadeReflectionHit(query.CommittedInstanceID(), query.CommittedPrimitiveIndex(),
                                      query.CommittedTriangleBarycentrics(), hitPosition, direction, true, seed,
                                      hitDiffuse, hitNormal);
        secondaryNormal = hitNormal;
        // 追加の反射：ヒット面でキャッシュした間接放射照度（radiance = albedo/pi * E）。
        float3 cachedIrradiance;
        if (CacheLookup(hitPosition, hitNormal, cachedIrradiance))
        {
            incoming += hitDiffuse * cachedIrradiance;
        }
    }
    else
    {
        // 太陽と月の円盤は除く。直接光ですでに扱っている（SkyAtmosphere.hlsli を参照）。
        incoming = EvaluatePhysicalSkyFast(direction, 0.01f, false);
    }
    float3 emissiveIrradiance = SampleEmissiveIrradiance(position, normal, seed + 17, pixel, checkerboard);
    float3 irradiance = incoming * 3.14159265f + emissiveIrradiance;
    irradiance = min(max(irradiance, 0), 65504);
    CacheInsert(position, normal, irradiance, hitDistance);
    if (ReSTIRParameters.x > 0.5f)
    {
        // 初期候補：元の PDF = cos / pi、目標値 = importance(L) * cos。
        GiReservoir reservoir;
        bool hit = query.CommittedStatus() != COMMITTED_NOTHING;
        reservoir.Position = hit ? ray.Origin + direction * hitDistance : position + direction * 10000.0f;
        reservoir.Normal = secondaryNormal; // 空間再利用のヤコビアンに必要
        reservoir.Radiance = min(max(incoming, 0.0f), 65504.0f);
        float target = ReSTIRImportance(reservoir.Radiance) * saturate(dot(normal, direction));
        reservoir.M = 1.0f;
        reservoir.W = target > 0.0f ? 3.14159265f / max(saturate(dot(normal, direction)), 0.0001f) : 0.0f;
        reservoir.VisibleDepth = abs(mul(float4(position, 1), ReflectionView).z);
        reservoir.VisibleNormal = normal;
        StoreReservoir(ReservoirCurrent, storage.y * ReSTIRState.y + storage.x, reservoir);
        // 発光体は別にサンプリングし、再サンプリングしない。空間パスのために保持する。
        NoisyRadiance[storage] = float4(min(emissiveIrradiance, 65504.0f), hitDistance);
        return;
    }
#if USE_NRD
    float viewZ = abs(mul(float4(position, 1), ReflectionView).z);
    float normalizedDistance = REBLUR_FrontEnd_GetNormHitDist(hitDistance, viewZ, float3(3, 0.1f, 20), 1);
    NoisyRadiance[storage] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(irradiance, normalizedDistance, true);
#else
    NoisyRadiance[storage] = float4(irradiance, hitDistance);
#endif
}

    [numthreads(8, 8, 1)] void FilterDiffuse(uint3 id : SV_DispatchThreadID)
{
    uint2 pixel = id.xy;
    if (any(pixel >= ReflectionExtent.xy))
    {
        return;
    }
    float centerDepth = ReflectionDepth.Load(int3(pixel, 0));
    if (centerDepth >= 0.99999f)
    {
        ReflectionRadiance[pixel] = 0;
        return;
    }
    float3 centerNormal = normalize(ReflectionNormal.Load(int3(pixel, 0)).xyz * 2 - 1);
    float3 sum = 0;
    float weightSum = 0;
    [unroll] for (int y = -2; y <= 2; ++y)
    {
        [unroll] for (int x = -2; x <= 2; ++x)
        {
            int2 samplePixel = clamp(int2(pixel) + int2(x, y), 0, int2(ReflectionExtent.xy) - 1);
            int2 storage = samplePixel;
            if (GiParameters.z > 0.5f)
            {
                if (((samplePixel.x + samplePixel.y + ReflectionExtent.z) & 1u) != 0)
                {
                    continue;
                }
                storage.x >>= 1; // 詰めたチェッカーボード配置。TraceDiffuse を参照
            }
            float depth = ReflectionDepth.Load(int3(samplePixel, 0));
            float3 normal = normalize(ReflectionNormal.Load(int3(samplePixel, 0)).xyz * 2 - 1);
            float weight = exp2(-abs(depth - centerDepth) * 10000 - float(x * x + y * y) * 0.25f) *
                           pow(saturate(dot(normal, centerNormal)), 32);
            float4 noisy = FilteredRadiance.Load(int3(storage, 0));
#if USE_NRD
            float3 sampleIrradiance = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(noisy).rgb;
#else
            float3 sampleIrradiance = noisy.rgb;
#endif
            sum += sampleIrradiance * weight;
            weightSum += weight;
        }
    }
    ReflectionRadiance[pixel] = float4(sum / max(weightSum, 0.00001f) * GiParameters.y, 1);
}

#if USE_NRD
[numthreads(8, 8, 1)] void ComposeDiffuse(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= ReflectionExtent.xy))
    {
        return;
    }
    float depth = ReflectionDepth.Load(int3(id.xy, 0));
    float3 irradiance = 0;
    if (depth < 0.99999f)
    {
        float3 normal = ReflectionNormal.Load(int3(id.xy, 0)).xyz * 2 - 1;
        float depthTolerance = max(0.00002f, (1 - depth) * 0.02f);
        float weightSum = 0;
        // NRD に正しく詰めたチェッカーボードを渡すため、軽い 3x3 のエッジを考慮した
        // 平滑化だけを残す（以前は回避策で 5x5）。幾何形状の重みで隣接する壁と
        // 輪郭を分け、物体をまたいだにじみを防ぐ。
        float3 centerIrradiance =
            REBLUR_BackEnd_UnpackRadianceAndNormHitDist(FilteredRadiance.Load(int3(id.xy, 0))).rgb;
        float centerLuma = dot(centerIrradiance, float3(0.2126f, 0.7152f, 0.0722f));
        [unroll] for (int y = -1; y <= 1; ++y)
        {
            [unroll] for (int x = -1; x <= 1; ++x)
            {
                int2 pixel = clamp(int2(id.xy) + int2(x, y), 0, int2(ReflectionExtent.xy) - 1);
                float sampleDepth = ReflectionDepth.Load(int3(pixel, 0));
                if (sampleDepth >= 0.99999f)
                {
                    continue;
                }
                float3 sampleNormal = ReflectionNormal.Load(int3(pixel, 0)).xyz * 2 - 1;
                float geometryWeight = saturate(1 - abs(sampleDepth - depth) / depthTolerance) *
                                       pow(saturate(dot(normal, sampleNormal)), 32);
                float spatialWeight = exp2(-float(x * x + y * y) * 0.35f);
                float3 sampleIrradiance =
                    REBLUR_BackEnd_UnpackRadianceAndNormHitDist(FilteredRadiance.Load(int3(pixel, 0))).rgb;
                float sampleLuma = dot(sampleIrradiance, float3(0.2126f, 0.7152f, 0.0722f));
                float lumaWeight = rcp(1.0f + abs(sampleLuma - centerLuma) / max(0.25f + centerLuma * 0.35f, 0.0001f));
                float weight = geometryWeight * spatialWeight * lumaWeight;
                irradiance += sampleIrradiance * weight;
                weightSum += weight;
            }
        }
        irradiance /= max(weightSum, 0.00001f);
    }
    ReflectionRadiance[id.xy] = float4(irradiance * GiParameters.y, 1);
}
#endif

// ---------------------------------------------------------------------------------------------
// 放射輝度キャッシュの保守（セルごとに 1 スレッド）。
// ---------------------------------------------------------------------------------------------
[numthreads(64, 1, 1)] void ResolveRadianceCache(uint3 id : SV_DispatchThreadID)
{
    uint slot = id.x;
    if (slot >= (uint)CacheParameters.z)
    {
        return;
    }
    uint key = CacheKeys.Load(slot * 4);
    if (key == 0u)
    {
        return;
    }
    uint4 accumulated = CacheAccumulation.Load4(slot * 32);
    if (accumulated.w > 0u)
    {
        float3 average = float3(accumulated.xyz) / (kCacheScale * accumulated.w);
        float4 resolved = CacheResolved[slot].Signal;
        float history = min(resolved.w + 1.0f, max(CacheParameters.w, 1.0f));
        resolved.rgb = lerp(resolved.rgb, average, 1.0f / history);
        resolved.w = history;
        CacheResolved[slot].Signal = resolved;
        CacheResolved[slot].Distance = float4(asfloat(CacheAccumulation.Load(slot * 32 + 16)), 0, 0, 0);
        CacheAccumulation.Store4(slot * 32, uint4(0, 0, 0, 0));
        CacheAccumulation.Store4(slot * 32 + 16, uint4(0, 0, 0, 0));
        CacheAges.Store(slot * 4, 0u);
        return;
    }
    uint age = CacheAges.Load(slot * 4) + 1u;
    if (age > kCacheMaxAge)
    {
        CacheKeys.Store(slot * 4, 0u);
        CacheResolved[slot] = (CacheValue)0;
        age = 0u;
    }
    CacheAges.Store(slot * 4, age);
}

[numthreads(64, 1, 1)] void ClearRadianceCache(uint3 id : SV_DispatchThreadID) {
    uint slot = id.x;
    if (slot >= (uint)CacheParameters.z)
    {
        return;
    }
    CacheKeys.Store(slot * 4, 0u);
    CacheAccumulation.Store4(slot * 32, uint4(0, 0, 0, 0));
    CacheAccumulation.Store4(slot * 32 + 16, uint4(0, 0, 0, 0));
    CacheResolved[slot] = (CacheValue)0;
    CacheAges.Store(slot * 4, 0u);
}

// ---------------------------------------------------------------------------------------------
// ReSTIR GI の各パス（追跡解像度でディスパッチする）。
// ---------------------------------------------------------------------------------------------
[numthreads(8, 8, 1)] void ReSTIRTemporal(uint3 id : SV_DispatchThreadID)
{
    uint2 storage = id.xy;
    if (storage.x >= ReSTIRState.y || storage.y >= ReflectionExtent.y)
    {
        return;
    }
    uint2 pixel = ReSTIRStorageToPixel(storage);
    float3 position, normal;
    float viewDepth;
    uint index = storage.y * ReSTIRState.y + storage.x;
    if (!ReSTIRVisiblePoint(pixel, position, normal, viewDepth))
    {
        return;
    }
    GiReservoir current = LoadReservoir(ReservoirCurrent, index);
    if (ReSTIRState.x == 0u)
    {
        return;
    }
    // GBuffer のモーション（現在 uv - 前 uv）で再投影する。
    float2 uv = (pixel + 0.5f) / ReflectionExtent.xy;
    float2 previousUv = uv - GiVelocity.Load(int3(pixel, 0)).zw;
    if (any(previousUv < 0.0f) || any(previousUv >= 1.0f))
    {
        return;
    }
    int2 previousPixel = int2(previousUv * ReflectionExtent.xy);
    uint2 previousStorage = uint2(GiParameters.z > 0.5f ? previousPixel.x >> 1 : previousPixel.x, previousPixel.y);
    previousStorage.x = min(previousStorage.x, ReSTIRState.y - 1);
    GiReservoir history = LoadReservoir(ReservoirHistory, previousStorage.y * ReSTIRState.y + previousStorage.x);
    if (history.M <= 0.0f || !ReSTIRSimilarSurface(viewDepth, normal, history.VisibleDepth, history.VisibleNormal))
    {
        return;
    }
    history.M = min(history.M, ReSTIRParameters.y);
    uint state = ReflectionHash(index * 7919u + ReflectionExtent.z * 104729u);
    // 統合式：w_i = p_hat_q(y_i) * W_i * M_i
    float targetCurrent = ReSTIRTarget(current, position, normal);
    float targetHistory = ReSTIRTarget(history, position, normal);
    float weightCurrent = targetCurrent * current.W * current.M;
    float weightHistory = targetHistory * history.W * history.M;
    float weightSum = weightCurrent + weightHistory;
    GiReservoir result = current;
    float selectedTarget = targetCurrent;
    if (weightSum > 0.0f && ReSTIRRandom(state) * weightSum < weightHistory)
    {
        result = history;
        selectedTarget = targetHistory;
    }
    result.M = current.M + history.M;
    result.W = selectedTarget > 0.0f ? weightSum / (result.M * selectedTarget) : 0.0f;
    result.VisibleDepth = viewDepth;
    result.VisibleNormal = normal;
    StoreReservoir(ReservoirCurrent, index, result);
}

// 近傍のサンプル（可視点 xr）を xq で再利用するヤコビアン（ReSTIR GI、式 11）。
float ReSTIRJacobian(GiReservoir sample, float3 neighbourPosition, float3 position)
{
    float3 toNeighbour = neighbourPosition - sample.Position;
    float3 toCenter = position - sample.Position;
    float distanceNeighbour = dot(toNeighbour, toNeighbour);
    float distanceCenter = dot(toCenter, toCenter);
    if (distanceCenter < 1e-8f || distanceNeighbour < 1e-8f)
    {
        return 0.0f;
    }
    float cosineNeighbour = abs(dot(sample.Normal, toNeighbour * rsqrt(distanceNeighbour)));
    float cosineCenter = abs(dot(sample.Normal, toCenter * rsqrt(distanceCenter)));
    float jacobian = (cosineCenter / max(cosineNeighbour, 1e-4f)) * (distanceNeighbour / distanceCenter);
    return (isnan(jacobian) || isinf(jacobian)) ? 0.0f : clamp(jacobian, 0.0f, 10.0f);
}

[numthreads(8, 8, 1)] void ReSTIRSpatial(uint3 id : SV_DispatchThreadID) {
    uint2 storage = id.xy;
    if (storage.x >= ReSTIRState.y || storage.y >= ReflectionExtent.y)
    {
        return;
    }
    uint2 pixel = ReSTIRStorageToPixel(storage);
    float3 position, normal;
    float viewDepth;
    if (!ReSTIRVisiblePoint(pixel, position, normal, viewDepth))
    {
        return;
    }
    uint index = storage.y * ReSTIRState.y + storage.x;
    GiReservoir center = LoadReservoir(ReservoirCurrent, index);
    uint state = ReflectionHash(index * 31337u + ReflectionExtent.z * 7919u + 17u);
    float centerTarget = ReSTIRTarget(center, position, normal);
    GiReservoir result = center;
    float selectedTarget = centerTarget;
    float weightSum = centerTarget * center.W * center.M;
    float totalM = center.M;
    uint samples = (uint)ReSTIRParameters.z;
    float radius = ReSTIRParameters.w;
    float columnScale = GiParameters.z > 0.5f ? 0.5f : 1.0f;
    [loop] for (uint sampleIndex = 0; sampleIndex < samples; ++sampleIndex)
    {
        float angle = ReSTIRRandom(state) * 6.2831853f;
        float distance = sqrt(ReSTIRRandom(state)) * radius;
        int2 offset = int2(round(float2(cos(angle) * distance * columnScale, sin(angle) * distance)));
        if (all(offset == 0))
        {
            continue;
        }
        int2 neighbourStorage = int2(storage) + offset;
        if (any(neighbourStorage < 0) || neighbourStorage.x >= (int)ReSTIRState.y ||
            neighbourStorage.y >= (int)ReflectionExtent.y)
        {
            continue;
        }
        uint neighbourIndex = neighbourStorage.y * ReSTIRState.y + neighbourStorage.x;
        GiReservoir neighbour = LoadReservoir(ReservoirCurrent, neighbourIndex);
        if (neighbour.M <= 0.0f ||
            !ReSTIRSimilarSurface(viewDepth, normal, neighbour.VisibleDepth, neighbour.VisibleNormal))
        {
            continue;
        }
        float3 neighbourPosition, neighbourNormal;
        float neighbourDepth;
        if (!ReSTIRVisiblePoint(ReSTIRStorageToPixel(uint2(neighbourStorage)), neighbourPosition, neighbourNormal,
                                neighbourDepth))
        {
            continue;
        }
        float jacobian = ReSTIRJacobian(neighbour, neighbourPosition, position);
        if (jacobian <= 0.0f)
        {
            continue;
        }
        GiReuseWeights reuse =
            GiSpatialReuseWeights(ReSTIRTarget(neighbour, position, normal), neighbour.W, neighbour.M, jacobian);
        float target = reuse.Target;
        float weight = reuse.Selection;
        weightSum += weight;
        totalM += neighbour.M;
        if (weight > 0.0f && ReSTIRRandom(state) * weightSum < weight)
        {
            result = neighbour;
            selectedTarget = target;
        }
    }
    float W = selectedTarget > 0.0f ? weightSum / (totalM * selectedTarget) : 0.0f;
    float3 direction = result.Position - position;
    float sampleDistance = length(direction);
    direction /= max(sampleDistance, 0.0001f);
    // このピクセルから再利用サンプルへの可視性（壁越しの光漏れを除く）。
    if (ReSTIRState.z != 0u && W > 0.0f && sampleDistance < 9000.0f)
    {
        RayDesc ray;
        ray.Origin = position + normal * SceneRayParameters.y;
        ray.Direction = direction;
        ray.TMin = SceneRayParameters.y * 0.25f;
        ray.TMax = max(sampleDistance - SceneRayParameters.y * 2.0f, ray.TMin + 0.001f);
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
        CountTelemetryRay(RAY_TELEMETRY_RESTIR_VISIBILITY);
        query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0x03, ray);
        while (query.Proceed())
        {
        }
        if (query.CommittedStatus() != COMMITTED_NOTHING)
        {
            W = 0.0f;
        }
    }
    float3 irradiance = result.Radiance * saturate(dot(normal, direction)) * W;
    float4 emissive = NoisyRadiance[storage]; // TraceDiffuse の（発光体の放射照度、主ヒット距離）
    irradiance = min(max(irradiance + emissive.rgb, 0.0f), 65504.0f);
    // コサイン BRDF の分布でリザーバーのガイドをフィルタリングする。RIS は明るい
    // サンプルを多く選ぶため、その距離をそのまま使うことはできない。
    // 除外したガイドは、新たな追跡なしで、ローブ内の新しい主レイへフォールバックする。
    float guideAcceptance = GiGuideAcceptance(saturate(dot(normal, direction)), W);
    uint guideState = ReflectionHash(index * 60013u + ReflectionExtent.z * 196613u);
    float hitDistance = ReSTIRRandom(guideState) < guideAcceptance ? min(sampleDistance, GiParameters.x)
                                                                   : clamp(emissive.w, 0.0f, GiParameters.x);
#if USE_NRD
    float normalizedDistance = REBLUR_FrontEnd_GetNormHitDist(hitDistance, viewDepth, float3(3, 0.1f, 20), 1);
    NoisyRadiance[storage] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(irradiance, normalizedDistance, true);
#else
    NoisyRadiance[storage] = float4(irradiance, hitDistance);
#endif
}

#include "LumenPasses.hlsli"

