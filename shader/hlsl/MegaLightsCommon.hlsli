#ifndef MEGALIGHTS_COMMON_HLSLI
#define MEGALIGHTS_COMMON_HLSLI
// =============================================================================================
// MegaLights 共通定義
//   M1 : クラスター（フロクセル）ライトグリッド
//   M2 : 確率的ライトサンプリング（ライト上の点の取り方・乱数・サンプルの詰め方）
//        半解像度：サンプル空間（MLSampleGrid）は画面の 1/2。各サンプルは 2x2 の代表ピクセル（毎フレーム回転）の
//        GBuffer で作る
//   M7 : 半透明（Forward / OIT）からのグリッド参照、テクスチャ付き矩形ライト
//   ・CPU 側の定義は source/rendering/megalights.h（レイアウトを必ず一致させる）
//   ・include する側で MEGALIGHTS_CB_REGISTER を定義してから include する
// =============================================================================================

#define ML_TILE_SIZE 16u
#define ML_MAX_SLICES 32u
#define ML_COARSE_TILES 4u
#define ML_COARSE_SLICES 4u
#define ML_INVALID_CELL 0xffffffffu

// MLStochastic.x のビット
#define ML_FLAG_STOCHASTIC 1u         // M2 のサンプルがこのフレームに作られている
#define ML_FLAG_REFERENCE 2u          // リファレンス（全ライト × 面上の複数点の影レイ）
#define ML_FLAG_ACCUMULATE 4u         // 静止時の累積平均
#define ML_FLAG_HAS_REF 8u            // リファレンス画像を保存済み
#define ML_FLAG_DENOISED 16u          // M3：ディファードは ML6 の代わりにデノイズ済みの結果を使う
#define ML_FLAG_GUIDE 32u             // M4：前フレームの可視ライトハッシュでサンプリングを導く
#define ML_GRID_FORWARD (1u << 17)    // MLGrid.w：Forward / 半透明もグリッドで評価する（M7）
#define ML_GRID_VOLUMETRIC (1u << 16) // MLGrid.w：フォグ用に視錐台全体（ジオメトリの手前のスライスも）を構築
#define ML_FLAG_SCREEN_TRACE 64u      // M5：ワールドトレースの前にスクリーントレース（深度バッファ）で遮蔽を探す

// サンプル（uint4）x のビット
#define ML_SAMPLE_VALID 0x00010000u
#define ML_SAMPLE_TRACE 0x00020000u   // 影レイが必要（CastShadow）
#define ML_SAMPLE_VISIBLE 0x00040000u // 影レイの結果：見える（TRACE でなければ常に見える扱い）
#define ML_SAMPLE_SCREEN 0x00080000u  // M5：スクリーントレースで遮蔽が見つかった（ワールドトレースしていない）
#define ML_SAMPLE_INDEX 0x0000ffffu

#define ML_PI 3.14159265359f

// 80 byte。xyzw の意味は LightParams の配列（LightPositionTypes など）と同じ。
struct MegaLightData
{
    float4 PositionType;   // xyz: 位置, w: LightType（1 Point / 2 Spot / 4 Rect）
    float4 DirectionRange; // xyz: 方向, w: 範囲
    float4 Color;          // rgb: 色, a: 強度（ディファード強度係数込み）
    float4 Extra;          // Spot: cos(内角), cos(外角), 密度, 形状 / Rect: 幅, 高さ, 密度, 0
    float4 Flags; // x: bit0 影を落とす / bit1 Forward に影響（M7）, y: ソース半径, z: Entity ID（asuint。M4）, w:
                  // 強度（M6 フォグ）
    // Rect の Extra.w：テクスチャ番号 + 1（0 = なし。M7）
};

#ifndef MEGALIGHTS_CB_REGISTER
#error "MEGALIGHTS_CB_REGISTER must be defined before including MegaLightsCommon.hlsli"
#endif

cbuffer MegaLightGridConstants : register(MEGALIGHTS_CB_REGISTER)
{
    float4x4 MLWorldToView; // mul(float4(world, 1), MLWorldToView)
    float4 MLProjection;    // _11, _22, _31, _32（ジッター込み）
    float4 MLDepth;         // _33, _43, near, スライス係数（slices / log(sliceFar / near)）
    float4 MLDepth2;        // 遠平面の NDC 深度, 実際の遠平面距離, ヒートマップ上限, 0
    uint4 MLGrid;           // タイル数 X, タイル数 Y, スライス数, 有効（このフレームに構築済み）
    uint4 MLCounts;         // ライト数, デバッグ表示, 細グリッドの容量, 粗グリッドの容量
    uint4 MLScreen;         // 幅, 高さ, 粗セル数 X, 粗セル数 Y
    // ---- M2 段階
    uint4 MLStochastic;        // フラグ, サンプル数 N, フレーム番号, 累積フレーム数
    float4 MLStochasticParams; // 一様混合率 α, 影レイの法線バイアス, 1/p の表示上限, リファレンスの影レイ数
    float4x4 MLViewToWorld;    // mul(float4(view, 1), MLViewToWorld)
    // ---- M5 段階
    float4 MLScreenTrace; // 最大ステップ数, 最大距離（ピクセル）, 厚み（m）, 自己遮蔽を避ける距離（ピクセル）
    uint4 MLSampleGrid;   // サンプル空間の幅, 高さ, 縮小率（1 / 2）, 代表ピクセルの回転（0..3）
};

bool MLLightCastsShadow(MegaLightData light)
{
    return (((uint)round(light.Flags.x)) & 1u) != 0u;
}

bool MLLightAffectsForward(MegaLightData light)
{
    return (((uint)round(light.Flags.x)) & 2u) != 0u;
}

// ---- 半解像度サンプリング
uint MLSampleScale()
{
    return max(MLSampleGrid.z, 1u);
}

// サンプル空間のピクセル → そのサンプルを作る代表ピクセル（画面）
uint2 MLSampleToScreen(uint2 samplePixel)
{
    const uint scale = MLSampleScale();
    if (scale == 1u)
    {
        return samplePixel;
    }
    const uint2 offsets[4] = {uint2(0u, 0u), uint2(1u, 1u), uint2(1u, 0u), uint2(0u, 1u)};
    return min(samplePixel * scale + offsets[MLSampleGrid.w & 3u], MLScreen.xy - 1u);
}

// 画面のピクセル → それを含むサンプル空間のピクセル
uint2 MLScreenToSample(uint2 pixel)
{
    return min(pixel / MLSampleScale(), MLSampleGrid.xy - 1u);
}

bool MLGridActive()
{
    return (MLGrid.w & 1u) != 0u;
}

bool MLForwardGridActive()
{
    return MLGridActive() && (MLGrid.w & ML_GRID_FORWARD) != 0u;
}

bool MLStochasticActive()
{
    return MLGridActive() && (MLStochastic.x & ML_FLAG_STOCHASTIC) != 0u && MLStochastic.y != 0u;
}

bool MLDenoisedActive()
{
    return MLStochasticActive() && (MLStochastic.x & ML_FLAG_DENOISED) != 0u;
}

bool MLReferenceActive()
{
    return MLGridActive() && (MLStochastic.x & ML_FLAG_REFERENCE) != 0u;
}

// NDC 深度 → ビュー空間 Z（透視投影。逆 Z でも同じ式）
float MLViewZFromDepth(float depth)
{
    return MLDepth.y / (depth - MLDepth.x);
}

bool MLIsGeometryDepth(float depth)
{
    return abs(depth - MLDepth2.x) > 1.0e-7f;
}

uint MLSliceFromViewZ(float viewZ)
{
    float nearZ = MLDepth.z;
    float slice = floor(log(max(viewZ, nearZ) / nearZ) * MLDepth.w);
    return min((uint)max(slice, 0.0f), MLGrid.z - 1u);
}

// スライス s の手前側の距離。最後のスライスの奥は実際の遠平面まで含める。
float MLSliceNearZ(uint slice)
{
    if (slice >= MLGrid.z)
    {
        return MLDepth2.y;
    }
    return MLDepth.z * exp((float)slice / MLDepth.w);
}

uint MLCellIndex(uint2 tile, uint slice)
{
    return (slice * MLGrid.y + tile.y) * MLGrid.x + tile.x;
}

// ピクセル座標（GBuffer 解像度）と GBuffer の深度からセルを求める。
// 構築側（MegaLightsGridCS の TileSliceMask）・M2 のサンプリング・ディファードで同じ入力・同じ式を使うこと。
uint MLCellIndexFromDepth(uint2 pixel, float depth)
{
    if (!MLGridActive() || !MLIsGeometryDepth(depth))
    {
        return ML_INVALID_CELL;
    }
    uint2 tile = min(pixel / ML_TILE_SIZE, MLGrid.xy - 1u);
    return MLCellIndex(tile, MLSliceFromViewZ(MLViewZFromDepth(depth)));
}

// ビュー Z から直接セルを求める（Forward / 半透明。GBuffer の深度を使わない）
uint MLCellIndexFromWorld(float3 worldPos)
{
    if (!MLGridActive())
    {
        return ML_INVALID_CELL;
    }
    const float3 view = mul(float4(worldPos, 1.0f), MLWorldToView).xyz;
    if (view.z <= MLDepth.z)
    {
        return ML_INVALID_CELL;
    }
    const float invZ = 1.0f / view.z;
    const float2 ndc =
        float2(MLProjection.x * view.x * invZ + MLProjection.z, MLProjection.y * view.y * invZ + MLProjection.w);
    const float2 pixel = float2((ndc.x * 0.5f + 0.5f) * MLScreen.x, (0.5f - ndc.y * 0.5f) * MLScreen.y);
    if (any(pixel < 0.0f) || any(pixel >= float2(MLScreen.xy)))
    {
        return ML_INVALID_CELL;
    }
    const uint2 tile = min(uint2(pixel) / ML_TILE_SIZE, MLGrid.xy - 1u);
    return MLCellIndex(tile, MLSliceFromViewZ(view.z));
}

// GBuffer の深度からワールド座標（ピクセル中心）
float3 MLWorldPositionFromDepth(uint2 pixel, float depth)
{
    float viewZ = MLViewZFromDepth(depth);
    float2 ndc = float2((pixel.x + 0.5f) / MLScreen.x * 2.0f - 1.0f, 1.0f - (pixel.y + 0.5f) / MLScreen.y * 2.0f);
    float3 view = float3((ndc.x - MLProjection.z) / MLProjection.x * viewZ,
                         (ndc.y - MLProjection.w) / MLProjection.y * viewZ, viewZ);
    return mul(float4(view, 1.0f), MLViewToWorld).xyz;
}

// ビュー空間の点 → ピクセル座標（MLWorldPositionFromDepth の逆）
float2 MLViewToPixel(float3 view)
{
    const float invZ = 1.0f / max(view.z, 1.0e-4f);
    const float2 ndc =
        float2(MLProjection.x * view.x * invZ + MLProjection.z, MLProjection.y * view.y * invZ + MLProjection.w);
    return float2((ndc.x * 0.5f + 0.5f) * MLScreen.x, (0.5f - ndc.y * 0.5f) * MLScreen.y);
}

// 2D ディスパッチ（x 最大 65535 グループ）の通し番号。間接ディスパッチの引数と対応
uint MLLinearGroup(uint3 groupId)
{
    return groupId.y * 65535u + groupId.x;
}

// トレースリストの要素：x 14bit | y 14bit | サンプル番号 2bit
uint MLPackTraceEntry(uint2 pixel, uint sampleIndex)
{
    return pixel.x | (pixel.y << 14u) | (sampleIndex << 28u);
}

void MLUnpackTraceEntry(uint entry, out uint2 pixel, out uint sampleIndex)
{
    pixel = uint2(entry & 0x3fffu, (entry >> 14u) & 0x3fffu);
    sampleIndex = entry >> 28u;
}

float3 MLCameraPosition()
{
    return mul(float4(0.0f, 0.0f, 0.0f, 1.0f), MLViewToWorld).xyz;
}

// サンプル空間のピクセルのサンプル先頭（N 個連続）
uint MLSampleBase(uint2 samplePixel)
{
    return (samplePixel.y * MLSampleGrid.x + samplePixel.x) * MLStochastic.y;
}

// タイル範囲 [tileMin, tileMax] × スライス範囲 [sliceMin, sliceMax] を囲むビュー空間 AABB
void MLCellBounds(uint2 tileMin, uint2 tileMax, uint sliceMin, uint sliceMax, out float3 boundsMin,
                  out float3 boundsMax)
{
    float z0 = MLSliceNearZ(sliceMin);
    float z1 = MLSliceNearZ(sliceMax + 1u);
    float2 screen = float2(MLScreen.xy);
    float2 pixel0 = float2(tileMin * ML_TILE_SIZE);
    float2 pixel1 = min(float2((tileMax + 1u) * ML_TILE_SIZE), screen);
    float ndcX0 = pixel0.x / screen.x * 2.0f - 1.0f;
    float ndcX1 = pixel1.x / screen.x * 2.0f - 1.0f;
    float ndcY0 = 1.0f - pixel1.y / screen.y * 2.0f;
    float ndcY1 = 1.0f - pixel0.y / screen.y * 2.0f;
    float2 x0 = (ndcX0 - MLProjection.z) / MLProjection.x * float2(z0, z1);
    float2 x1 = (ndcX1 - MLProjection.z) / MLProjection.x * float2(z0, z1);
    float2 y0 = (ndcY0 - MLProjection.w) / MLProjection.y * float2(z0, z1);
    float2 y1 = (ndcY1 - MLProjection.w) / MLProjection.y * float2(z0, z1);
    boundsMin = float3(min(min(x0.x, x0.y), min(x1.x, x1.y)), min(min(y0.x, y0.y), min(y1.x, y1.y)), z0);
    boundsMax = float3(max(max(x0.x, x0.y), max(x1.x, x1.y)), max(max(y0.x, y0.y), max(y1.x, y1.y)), z1);
}

float4 MLLightViewSphere(MegaLightData light)
{
    float3 center = mul(float4(light.PositionType.xyz, 1.0f), MLWorldToView).xyz;
    return float4(center, max(light.DirectionRange.w, 0.01f));
}

bool MLSphereIntersectsAabb(float4 sphere, float3 boundsMin, float3 boundsMax)
{
    float3 distance = max(max(boundsMin - sphere.xyz, 0.0f), sphere.xyz - boundsMax);
    return dot(distance, distance) <= sphere.w * sphere.w;
}

float3 MLHeatmap(float value)
{
    float t = saturate(value);
    float3 cold = float3(0.05f, 0.10f, 0.45f);
    float3 mid = float3(0.10f, 0.85f, 0.35f);
    float3 warm = float3(1.00f, 0.85f, 0.10f);
    float3 hot = float3(1.00f, 0.15f, 0.10f);
    if (t < 0.33f)
    {
        return lerp(cold, mid, t / 0.33f);
    }
    if (t < 0.66f)
    {
        return lerp(mid, warm, (t - 0.33f) / 0.33f);
    }
    return lerp(warm, hot, (t - 0.66f) / 0.34f);
}

// =============================================================================================
// M2：乱数とライト上の点
// =============================================================================================
uint MLHash(uint value)
{
    // PCG（O'Neill）の出力関数
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float MLRandom(uint seed)
{
    return (MLHash(seed) >> 8) * (1.0f / 16777216.0f); // [0, 1)
}

// ピクセル・フレーム・用途ごとに独立な種
uint MLSeed(uint2 pixel, uint frame, uint stream)
{
    return MLHash(pixel.x * 1973u + MLHash(pixel.y * 9277u + MLHash(frame * 26699u + stream)));
}

// 矩形ライトの基底（SurfaceLighting.hlsli の RectLightBasis と同じ。LTC と幅・高さの向きを揃える）
void MLRectBasis(float3 emitNormal, out float3 right, out float3 up)
{
    float3 axis = abs(emitNormal.y) > 0.99f ? float3(1.0f, 0.0f, 0.0f) : float3(0.0f, 1.0f, 0.0f);
    right = normalize(cross(axis, emitNormal));
    up = cross(emitNormal, right);
}

// ライト上の点 y（uv ∈ [0,1)²）
//   Rect : 面積一様
//   Point / Spot（半径 > 0）: シェーディング点から見た球の立体角一様
//   Point / Spot（半径 0）  : 位置そのもの
float3 MLLightSamplePoint(MegaLightData light, float3 worldPos, float2 uv)
{
    const float3 center = light.PositionType.xyz;
    const int type = (int)round(light.PositionType.w);
    if (type == 4)
    {
        float3 emitNormal = normalize(light.DirectionRange.xyz);
        float3 right, up;
        MLRectBasis(emitNormal, right, up);
        return center + right * ((uv.x - 0.5f) * light.Extra.x) + up * ((uv.y - 0.5f) * light.Extra.y);
    }
    const float radius = max(light.Flags.y, 0.0f);
    float3 toCenter = center - worldPos;
    const float distance = length(toCenter);
    if (radius <= 1.0e-4f || distance <= radius * 1.001f)
    {
        return center;
    }
    toCenter /= distance;
    const float sinMax2 = saturate((radius * radius) / (distance * distance));
    const float cosMax = sqrt(1.0f - sinMax2);
    const float cosTheta = 1.0f - uv.x * (1.0f - cosMax);
    const float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    const float phi = 2.0f * ML_PI * uv.y;
    float3 tangent = abs(toCenter.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    tangent = normalize(cross(tangent, toCenter));
    const float3 bitangent = cross(toCenter, tangent);
    const float3 direction = toCenter * cosTheta + (tangent * cos(phi) + bitangent * sin(phi)) * sinTheta;
    // 球との交点（手前側）
    const float b = distance * cosTheta;
    const float t = b - sqrt(max(radius * radius - distance * distance * sinTheta * sinTheta, 0.0f));
    return worldPos + direction * max(t, 0.0f);
}

// ---- M4：可視ライトハッシュ（8x8 タイルごとの 128bit ブルームフィルタ。ライトごとに 2bit）
#define ML_HASH_TILE 8u

uint MLHashTileIndex(uint2 pixel)
{
    const uint tilesX = (MLScreen.x + ML_HASH_TILE - 1u) / ML_HASH_TILE;
    return (pixel.y / ML_HASH_TILE) * tilesX + pixel.x / ML_HASH_TILE;
}

uint4 MLHashBits(MegaLightData light)
{
    const uint key = MLHash(asuint(light.Flags.z) * 0x9e3779b9u + 0x7f4a7c15u);
    const uint bit0 = key & 127u;
    const uint bit1 = (key >> 7u) & 127u;
    uint4 bits = uint4(0u, 0u, 0u, 0u);
    bits[bit0 >> 5u] |= 1u << (bit0 & 31u);
    bits[bit1 >> 5u] |= 1u << (bit1 & 31u);
    return bits;
}

bool MLHashContains(uint4 hash, uint4 bits)
{
    return all((hash & bits) == bits);
}

uint MLHashOccupancy(uint4 hash)
{
    return countbits(hash.x) + countbits(hash.y) + countbits(hash.z) + countbits(hash.w);
}

float2 MLUnpackUv(uint packed)
{
    return f16tof32(uint2(packed & 0xffffu, packed >> 16u));
}

uint MLPackUv(float2 uv)
{
    uint2 h = f32tof16(saturate(uv) * 0.9995f);
    return h.x | (h.y << 16u);
}

#endif
