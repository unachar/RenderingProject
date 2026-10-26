// =============================================================================================
// MegaLights M2 : 確率的ライトサンプリング
//   SampleLights  (ML3) : セルのライトリストを 1 回だけ走査し、N 個のライトを選ぶ
//                         p_i = (1 - α) w_i / Σw + α / M （α: 一様混合率, M: 候補数）
//   TraceShadows  (ML5) : 選んだサンプルのうち CastShadow のライトへ影レイ（RayQuery）
//   ErrorReduce         : 検証用。累積平均とリファレンスの二乗誤差をグループごとに合計
//   M5：タイル分類 + コンパクション + スクリーントレース
//   ClearTraceCounters  : カウンタを 0 に（0: タイル数, 4: トレース要求数, 8: ワールドトレース数, 12:
//   スクリーンで遮蔽が見つかった数） ClassifyTiles (ML2) : 8x8 タイルごとに「PBR
//   面でセルにライトがある」ピクセルがあるか → タイルリスト。空のタイルはサンプルを 0 に ArgsTiles/Trace/World :
//   カウンタ → 間接ディスパッチの引数 SampleLights  (ML3) :
//   タイルリストで間接ディスパッチ。影レイが必要なサンプルをトレースリストへ ScreenTrace   (ML4) :
//   深度バッファを短くマーチ。遮蔽が見つかれば確定、見つからなければワールドトレースリストへ WorldTrace    (ML5) :
//   ワールドトレースリストだけを RayQuery（ウェーブの全スレッドがレイを飛ばす） BuildHash     (ML7) :
//   M4。影レイが通ったライトを 8x8 タイルの 128bit ブルームフィルタへ。
//                         前フレームの分（リプロジェクトしたタイル）は確率的に減衰させて引き継ぐ
//   ガイド（M4）        : w_i ← w_i × g_i（g = 1：前フレームのハッシュに載っている, g = g_min：載っていない）
//                         g は選択確率 p に入るので推定量は不偏のまま。g_min > 0 なので遮られがちなライトも時々調べる
//   シェーディング（ML6）は DeferredLightingPS で行う（M1 と同じ BRDF・同じ材質の値を使うため）。
//   実行時に DXC（cs_6_5）でコンパイルする。.vcxproj への追加は不要。
// =============================================================================================
#define MEGALIGHTS_CB_REGISTER b0
#include "MegaLightsCommon.hlsli"

#define ML_MAX_SPP 4u

StructuredBuffer<MegaLightData> Lights : register(t1);
StructuredBuffer<uint2> Cells : register(t3);
StructuredBuffer<uint> CellIndices : register(t4);
RaytracingAccelerationStructure Scene : register(t5);
StructuredBuffer<float4> Reference : register(t7);

// GBuffer（ポストプロセスと同じ並び。ディスクリプタテーブル 1 つ）
Texture2D<float4> GBufferBaseColor : register(t8);
Texture2D<float4> GBufferNormal : register(t9);
Texture2D<float> GBufferDepth : register(t10);
Texture2D<float4> GBufferMaterial : register(t11);
Texture2D<float4> GBufferVelocity : register(t15); // zw = 現在の UV - 前フレームの UV
StructuredBuffer<uint4> PreviousHash : register(t6);

RWStructuredBuffer<uint4> Samples : register(u0);
RWStructuredBuffer<float4> Accumulation : register(u1);
RWStructuredBuffer<float2> Partials : register(u2);
RWStructuredBuffer<uint4> CurrentHash : register(u3);
// ---- M5 段階
RWStructuredBuffer<uint> TileList : register(u4);
RWStructuredBuffer<uint> TraceList : register(u5);
RWStructuredBuffer<uint> WorldList : register(u6);
RWByteAddressBuffer TraceCounters : register(u7);
RWByteAddressBuffer DispatchArgs : register(u8);

#define ML_COUNTER_TILES 0u
#define ML_COUNTER_TRACE 4u
#define ML_COUNTER_WORLD 8u
#define ML_COUNTER_SCREEN 12u

// ウェーブでまとめてリストへ追加（アトミックはウェーブに 1 回）
void MLAppendTrace(bool want, uint value)
{
    const uint count = WaveActiveCountBits(want);
    if (count == 0u)
    {
        return;
    }
    uint base = 0u;
    if (WaveIsFirstLane())
    {
        TraceCounters.InterlockedAdd(ML_COUNTER_TRACE, count, base);
    }
    base = WaveReadLaneFirst(base);
    if (want)
    {
        TraceList[base + WavePrefixCountBits(want)] = value;
    }
}

void MLAppendWorld(bool want, uint value)
{
    const uint count = WaveActiveCountBits(want);
    if (count == 0u)
    {
        return;
    }
    uint base = 0u;
    if (WaveIsFirstLane())
    {
        TraceCounters.InterlockedAdd(ML_COUNTER_WORLD, count, base);
    }
    base = WaveReadLaneFirst(base);
    if (want)
    {
        WorldList[base + WavePrefixCountBits(want)] = value;
    }
}

bool MLGuideActive()
{
    return (MLStochastic.x & ML_FLAG_GUIDE) != 0u;
}

// 前フレームのハッシュ（このピクセルを Velocity で前フレームへ戻したタイル）。使えなければ false
bool MLLoadPreviousHash(uint2 pixel, out uint4 hash)
{
    hash = uint4(0u, 0u, 0u, 0u);
    if (!MLGuideActive())
    {
        return false;
    }
    const float2 size = float2(MLScreen.xy);
    const float2 previousUv = (float2(pixel) + 0.5f) / size - GBufferVelocity.Load(int3(pixel, 0)).zw;
    if (any(previousUv < 0.0f) || any(previousUv >= 1.0f))
    {
        return false;
    }
    hash = PreviousHash[MLHashTileIndex(uint2(previousUv * size))];
    return true;
}

float MLLuminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

bool MLIsPbrSurface(float shaderClass)
{
    return abs(shaderClass - 11.0f) < 0.5f;
}

// 寄与が厳密に 0 になるライト（AccumulateSurfaceLight が必ず 0 を返す）。それ以外は確率を 0 にしない
//   ・強度 0
//   ・Point / Spot の範囲外（ResolveSingleLightCommon の減衰が 0）
//   ・Rect は「エリアライト（LTC）」設定がオフだと別の式で評価されるため、ここでは除外しない
bool MLLightIsZero(MegaLightData light, float3 worldPos)
{
    if (light.Color.a <= 0.000001f)
    {
        return true;
    }
    const int type = (int)round(light.PositionType.w);
    if (type == 1 || type == 2)
    {
        return length(light.PositionType.xyz - worldPos) >= max(light.DirectionRange.w, 0.01f);
    }
    return false;
}

// 影なしの寄与の安い見積もり（輝度）。外れても一様混合で確率 > 0 なので不偏性は保たれる
float MLTargetWeight(MegaLightData light, float3 worldPos, float3 N, float3 V, float3 albedo, float3 materialData)
{
    const float3 toLight = light.PositionType.xyz - worldPos;
    const float distance = max(length(toLight), 1.0e-4f);
    const float3 L = toLight / distance;
    const float rangeFade = saturate(1.0f - distance / max(light.DirectionRange.w, 0.01f));
    float geometry = rangeFade * rangeFade;
    const int type = (int)round(light.PositionType.w);
    if (type == 2)
    {
        const float cosAngle = dot(-L, normalize(light.DirectionRange.xyz));
        const float cosOuter = light.Extra.y;
        const float cosInner = max(light.Extra.x, cosOuter + 1.0e-3f);
        geometry *= smoothstep(cosOuter, cosInner, cosAngle);
    }
    else if (type == 4)
    {
        const float area = max(light.Extra.x * light.Extra.y, 1.0e-4f);
        const float cosEmit = saturate(dot(-L, normalize(light.DirectionRange.xyz)));
        geometry *= area * cosEmit / (distance * distance + area);
    }
    const float NdotL = saturate(dot(N, L));
    const float metallic = saturate(materialData.r);
    const float roughness = clamp(materialData.g, 0.04f, 1.0f);
    const float3 F0 = lerp(max(materialData.b, 0.04f).xxx, albedo, metallic);
    const float diffuse = MLLuminance(albedo) * (1.0f - metallic) / ML_PI;
    const float3 H = normalize(L + V);
    const float alpha = roughness * roughness;
    const float shininess = min(2.0f / max(alpha * alpha, 1.0e-4f) - 2.0f, 2048.0f);
    const float specular =
        min(MLLuminance(F0) * (shininess + 2.0f) / (2.0f * ML_PI) * pow(saturate(dot(N, H)), shininess), 64.0f);
    return MLLuminance(max(light.Color.rgb, 0.0f)) * max(light.Color.a, 0.0f) * geometry * NdotL * (diffuse + specular);
}

// --------------------------------------------------------------------------------------------
// ML3：ライトの選択
// --------------------------------------------------------------------------------------------
[numthreads(8, 8, 1)] void SampleLights(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID) {
    // M5：対象のタイルだけを間接ディスパッチ
    const uint tileIndex = MLLinearGroup(groupId);
    if (tileIndex >= TraceCounters.Load(ML_COUNTER_TILES))
    {
        return;
    }
    const uint packedTile = TileList[tileIndex];
    // タイルはサンプル空間（半解像度なら画面の 1/2）。GBuffer は代表ピクセルで読む
    const uint2 samplePixel = uint2(packedTile & 0xffffu, packedTile >> 16u) * ML_HASH_TILE + groupThreadId.xy;
    if (any(samplePixel >= MLSampleGrid.xy))
    {
        return;
    }
    const uint2 pixel = MLSampleToScreen(samplePixel);
    const uint sampleCount = min(MLStochastic.y, ML_MAX_SPP);
    const uint base = MLSampleBase(samplePixel);

    const float depth = GBufferDepth.Load(int3(pixel, 0));
    const uint cellIndex = MLCellIndexFromDepth(pixel, depth);
    const float4 material = GBufferMaterial.Load(int3(pixel, 0));
    uint2 cell = uint2(0u, 0u);
    if (cellIndex != ML_INVALID_CELL && MLIsPbrSurface(material.a))
    {
        cell = Cells[cellIndex];
    }
    if (cell.y == 0u)
    {
        for (uint clearIndex = 0u; clearIndex < sampleCount; ++clearIndex)
        {
            Samples[base + clearIndex] = uint4(0u, 0u, 0u, 0u);
        }
        return;
    }

    const float3 worldPos = MLWorldPositionFromDepth(pixel, depth);
    const float3 N = normalize(GBufferNormal.Load(int3(pixel, 0)).xyz * 2.0f - 1.0f);
    const float3 V = MLViewDirection(worldPos);
    const float3 albedo = GBufferBaseColor.Load(int3(pixel, 0)).rgb;
    const uint seed = MLSeed(pixel, MLStochastic.z, 0x4d4cu);
    // M4：ガイド。前フレームのタイルのハッシュが空（何も見えていない / 初回）ならガイドしない
    uint4 previousHash;
    const bool guided = MLLoadPreviousHash(pixel, previousHash) && MLHashOccupancy(previousHash) != 0u;
    const float guideMin = clamp(MLDepth2.w, 0.001f, 1.0f);

    float weightSum = 0.0f;
    uint candidates = 0u;
    uint weightedPick[ML_MAX_SPP];
    float weightedValue[ML_MAX_SPP];
    uint uniformPick[ML_MAX_SPP];
    float uniformValue[ML_MAX_SPP];
    [unroll] for (uint init = 0u; init < ML_MAX_SPP; ++init)
    {
        weightedPick[init] = 0xffffffffu;
        weightedValue[init] = 0.0f;
        uniformPick[init] = 0xffffffffu;
        uniformValue[init] = 0.0f;
    }

    // 1 回の走査で、重み比例のリザーバーと一様のリザーバーを N 個ずつ更新する
    [loop] for (uint i = 0u; i < cell.y; ++i)
    {
        const uint lightIndex = CellIndices[cell.x + i];
        const MegaLightData light = Lights[lightIndex];
        if (MLLightIsZero(light, worldPos))
        {
            continue;
        }
        float w = max(MLTargetWeight(light, worldPos, N, V, albedo, material.rgb), 0.0f);
        if (guided && !MLHashContains(previousHash, MLHashBits(light)))
        {
            w *= guideMin;
        }
        candidates += 1u;
        weightSum += w;
        [unroll] for (uint k = 0u; k < ML_MAX_SPP; ++k)
        {
            if (k >= sampleCount)
            {
                break;
            }
            const uint stream = seed + (k * 0x9e3779b9u) + i * 0x85ebca6bu;
            if (w > 0.0f && MLRandom(stream) * weightSum < w)
            {
                weightedPick[k] = lightIndex;
                weightedValue[k] = w;
            }
            if (MLRandom(stream ^ 0x68e31da4u) * (float)candidates < 1.0f)
            {
                uniformPick[k] = lightIndex;
                uniformValue[k] = w;
            }
        }
    }

    const float alpha = (weightSum > 0.0f) ? saturate(MLStochasticParams.x) : 1.0f;
    for (uint k = 0u; k < ML_MAX_SPP; ++k)
    {
        if (k >= sampleCount)
        {
            break;
        }
        uint4 result = uint4(0u, 0u, 0u, 0u);
        if (candidates > 0u)
        {
            const uint stream = seed + 0x27d4eb2fu * (k + 1u);
            const bool useUniform = weightedPick[k] == 0xffffffffu || MLRandom(stream) < alpha;
            const uint lightIndex = useUniform ? uniformPick[k] : weightedPick[k];
            const float w = useUniform ? uniformValue[k] : weightedValue[k];
            const float pdf = (weightSum > 0.0f ? (1.0f - alpha) * w / weightSum : 0.0f) + alpha / (float)candidates;
            if (lightIndex != 0xffffffffu && pdf > 0.0f)
            {
                const MegaLightData light = Lights[lightIndex];
                const bool castShadow = MLLightCastsShadow(light);
                const float2 uv = float2(MLRandom(stream ^ 0x165667b1u), MLRandom(stream ^ 0xd3a2646cu));
                result.x = (lightIndex & ML_SAMPLE_INDEX) | ML_SAMPLE_VALID |
                           (castShadow ? ML_SAMPLE_TRACE : ML_SAMPLE_VISIBLE);
                result.y = asuint(1.0f / pdf);
                result.z = MLPackUv(uv);
                result.w = 0u;
            }
        }
        Samples[base + k] = result;
        // 影レイが必要なサンプルだけをトレースリストへ（M5 のコンパクション）
        MLAppendTrace((result.x & ML_SAMPLE_TRACE) != 0u, MLPackTraceEntry(samplePixel, k));
    }
}

// --------------------------------------------------------------------------------------------
// ML5：影レイ（始点 = 面上の点 + 法線バイアス、終点 = ライト上の点）
// --------------------------------------------------------------------------------------------
bool MLTraceSegment(float3 position, float3 normal, float3 target, out float hitDistance)
{
    const float bias = MLStochasticParams.y;
    const float3 origin = position + normal * bias;
    const float3 segment = target - origin;
    const float distance = length(segment);
    hitDistance = distance;
    if (distance <= bias * 2.0f)
    {
        return true;
    }
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = segment / distance;
    ray.TMin = bias * 0.25f;
    ray.TMax = max(distance - bias, ray.TMin + 0.001f);
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(Scene, RAY_FLAG_NONE, 0x01, ray);
    while (query.Proceed())
    {
    }
    if (query.CommittedStatus() == COMMITTED_NOTHING)
    {
        return true;
    }
    hitDistance = query.CommittedRayT();
    return false;
}

// --------------------------------------------------------------------------------------------
// M5：スクリーントレース。面上の点からライト上の点へ向けて深度バッファを短くマーチする
//   遮蔽あり：その点の手前に、厚み以内の面がある（深度バッファの値が点より手前）
//   見つからない / 画面外：ワールドトレースへ（ワールドトレースは線分全体を調べる）
//   深度バッファにしかない細部（TLAS に入っていない変形・細かいジオメトリ）の接地影を拾える
// --------------------------------------------------------------------------------------------
bool MLScreenTraceOccluded(uint2 pixel, float3 worldPos, float3 normal, float3 target, uint seed, out float hitDistance)
{
    hitDistance = 0.0f;
    const float3 origin = worldPos + normal * MLStochasticParams.y;
    const float3 startView = mul(float4(origin, 1.0f), MLWorldToView).xyz;
    float3 endView = mul(float4(target, 1.0f), MLWorldToView).xyz;
    const float nearZ = MLDepth.z;
    if (startView.z <= nearZ)
    {
        return false;
    }
    // 終点がカメラの後ろなら手前で切る
    float segmentScale = 1.0f;
    if (endView.z < nearZ)
    {
        segmentScale = (startView.z - nearZ) / max(startView.z - endView.z, 1.0e-4f);
        endView = lerp(startView, endView, segmentScale);
    }
    const float2 startPixel = MLViewToPixel(startView);
    const float2 endPixel = MLViewToPixel(endView);
    const float pixelLength = length(endPixel - startPixel);
    const uint steps = clamp((uint)MLScreenTrace.x, 1u, 64u);
    const float maxPixels = max(MLScreenTrace.y, 1.0f);
    const float tEnd = min(1.0f, maxPixels / max(pixelLength, 1.0e-3f));
    const float thickness = max(MLScreenTrace.z, 0.001f);
    const float selfPixels = max(MLScreenTrace.w, 0.0f);
    const float jitter = MLRandom(seed);
    const float segmentLength = length(target - origin) * segmentScale;
    for (uint step = 0u; step < steps; ++step)
    {
        const float t = tEnd * ((float)step + jitter) / (float)steps;
        const float3 pointView = lerp(startView, endView, t);
        const float2 samplePixel = MLViewToPixel(pointView);
        if (any(samplePixel < 0.0f) || any(samplePixel >= float2(MLScreen.xy)))
        {
            return false; // 画面外
        }
        if (length(samplePixel - startPixel) < selfPixels)
        {
            continue; // 自分のピクセル付近は自己遮蔽になりやすい
        }
        const float sceneDepth = GBufferDepth.Load(int3(uint2(samplePixel), 0));
        if (!MLIsGeometryDepth(sceneDepth))
        {
            continue;
        }
        const float sceneZ = MLViewZFromDepth(sceneDepth);
        const float bias = 0.005f * pointView.z;
        if (sceneZ < pointView.z - bias && pointView.z - sceneZ < thickness)
        {
            hitDistance = t * segmentLength;
            return true;
        }
    }
    return false;
}

[numthreads(64, 1, 1)] void ScreenTrace(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex) {
    const uint index = MLLinearGroup(groupId) * 64u + groupIndex;
    if (index >= TraceCounters.Load(ML_COUNTER_TRACE))
    {
        return;
    }
    const uint entry = TraceList[index];
    uint2 samplePixel;
    uint k;
    MLUnpackTraceEntry(entry, samplePixel, k);
    const uint2 pixel = MLSampleToScreen(samplePixel);
    const uint sampleAddress = MLSampleBase(samplePixel) + k;
    bool resolved = false;
    if ((MLStochastic.x & ML_FLAG_SCREEN_TRACE) != 0u)
    {
        uint4 lightSample = Samples[sampleAddress];
        const float depth = GBufferDepth.Load(int3(pixel, 0));
        const float3 worldPos = MLWorldPositionFromDepth(pixel, depth);
        const float3 N = normalize(GBufferNormal.Load(int3(pixel, 0)).xyz * 2.0f - 1.0f);
        const MegaLightData light = Lights[lightSample.x & ML_SAMPLE_INDEX];
        const float3 target = MLLightSamplePoint(light, worldPos, MLUnpackUv(lightSample.z));
        float hitDistance;
        if (MLScreenTraceOccluded(pixel, worldPos, N, target, MLSeed(pixel, MLStochastic.z, 0x5354u + k), hitDistance))
        {
            lightSample.x = (lightSample.x & ~ML_SAMPLE_VISIBLE) | ML_SAMPLE_SCREEN;
            lightSample.w = asuint(hitDistance);
            Samples[sampleAddress] = lightSample;
            resolved = true;
        }
    }
    const uint hits = WaveActiveCountBits(resolved);
    if (hits != 0u && WaveIsFirstLane())
    {
        uint ignored;
        TraceCounters.InterlockedAdd(ML_COUNTER_SCREEN, hits, ignored);
    }
    MLAppendWorld(!resolved, entry);
}

    [numthreads(64, 1, 1)] void WorldTrace(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    const uint index = MLLinearGroup(groupId) * 64u + groupIndex;
    if (index >= TraceCounters.Load(ML_COUNTER_WORLD))
    {
        return;
    }
    uint2 samplePixel;
    uint k;
    MLUnpackTraceEntry(WorldList[index], samplePixel, k);
    const uint2 pixel = MLSampleToScreen(samplePixel);
    const uint sampleAddress = MLSampleBase(samplePixel) + k;
    uint4 lightSample = Samples[sampleAddress];
    const float depth = GBufferDepth.Load(int3(pixel, 0));
    const float3 worldPos = MLWorldPositionFromDepth(pixel, depth);
    const float3 N = normalize(GBufferNormal.Load(int3(pixel, 0)).xyz * 2.0f - 1.0f);
    const MegaLightData light = Lights[lightSample.x & ML_SAMPLE_INDEX];
    const float3 target = MLLightSamplePoint(light, worldPos, MLUnpackUv(lightSample.z));
    float hitDistance;
    if (MLTraceSegment(worldPos, N, target, hitDistance))
    {
        lightSample.x |= ML_SAMPLE_VISIBLE;
    }
    lightSample.w = asuint(hitDistance);
    Samples[sampleAddress] = lightSample;
}

// --------------------------------------------------------------------------------------------
// M5：タイル分類 / カウンタ / 間接ディスパッチの引数
// --------------------------------------------------------------------------------------------
[numthreads(1, 1, 1)] void ClearTraceCounters() {
    TraceCounters.Store4(0, uint4(0u, 0u, 0u, 0u));
}

groupshared uint gs_TileActive;

[numthreads(8, 8, 1)] void ClassifyTiles(uint3 groupId : SV_GroupID, uint3 dispatch : SV_DispatchThreadID,
                                         uint groupIndex : SV_GroupIndex) {
    if (groupIndex == 0u)
    {
        gs_TileActive = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    // サンプル空間の 8x8 タイル
    const uint2 samplePixel = dispatch.xy;
    const bool inScreen = all(samplePixel < MLSampleGrid.xy);
    const uint2 pixel = MLSampleToScreen(min(samplePixel, MLSampleGrid.xy - 1u));
    if (inScreen)
    {
        const float depth = GBufferDepth.Load(int3(pixel, 0));
        const uint cellIndex = MLCellIndexFromDepth(pixel, depth);
        if (cellIndex != ML_INVALID_CELL && MLIsPbrSurface(GBufferMaterial.Load(int3(pixel, 0)).a) &&
            Cells[cellIndex].y > 0u)
        {
            InterlockedOr(gs_TileActive, 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gs_TileActive == 0u)
    {
        // サンプリングしないタイル：前フレームのサンプルが残らないように 0 にする
        if (inScreen)
        {
            const uint base = MLSampleBase(samplePixel);
            const uint sampleCount = min(MLStochastic.y, ML_MAX_SPP);
            for (uint k = 0u; k < sampleCount; ++k)
            {
                Samples[base + k] = uint4(0u, 0u, 0u, 0u);
            }
        }
        return;
    }
    if (groupIndex == 0u)
    {
        uint slot;
        TraceCounters.InterlockedAdd(ML_COUNTER_TILES, 1u, slot);
        TileList[slot] = groupId.x | (groupId.y << 16u);
    }
}

void MLWriteDispatchArgs(uint offset, uint groups)
{
    const uint x = min(groups, 65535u);
    const uint y = max((groups + 65534u) / 65535u, 1u);
    DispatchArgs.Store3(offset, uint3(x, y, 1u));
}

[numthreads(1, 1, 1)] void ArgsTiles() {
    MLWriteDispatchArgs(0u, TraceCounters.Load(ML_COUNTER_TILES));
}

    [numthreads(1, 1, 1)] void ArgsTrace()
{
    MLWriteDispatchArgs(12u, (TraceCounters.Load(ML_COUNTER_TRACE) + 63u) / 64u);
}

[numthreads(1, 1, 1)] void ArgsWorld() {
    MLWriteDispatchArgs(24u, (TraceCounters.Load(ML_COUNTER_WORLD) + 63u) / 64u);
}

// --------------------------------------------------------------------------------------------
// 検証：累積平均とリファレンスの差（輝度）。Partials[g] = (Σ(acc - ref)², Σ ref²)
// --------------------------------------------------------------------------------------------
groupshared float2 gs_Error[256];

[numthreads(256, 1, 1)] void ErrorReduce(uint3 groupId : SV_GroupID, uint3 dispatch : SV_DispatchThreadID,
                                         uint groupIndex : SV_GroupIndex) {
    const uint pixelCount = MLScreen.x * MLScreen.y;
    float2 value = 0.0f;
    if (dispatch.x < pixelCount)
    {
        const float accumulated = MLLuminance(Accumulation[dispatch.x].rgb);
        const float reference = MLLuminance(Reference[dispatch.x].rgb);
        if (all(isfinite(float2(accumulated, reference))))
        {
            const float difference = accumulated - reference;
            value = float2(difference * difference, reference * reference);
        }
        else
        {
            value = float2(1.0e6f, 0.0f); // NaN / Inf は大きな誤差として必ず見えるようにする
        }
    }
    gs_Error[groupIndex] = value;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 128u; stride > 0u; stride >>= 1u)
    {
        if (groupIndex < stride)
        {
            gs_Error[groupIndex] += gs_Error[groupIndex + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (groupIndex == 0u)
    {
        Partials[groupId.x] = gs_Error[0];
    }
}

// --------------------------------------------------------------------------------------------
// ML7（M4）：可視ライトハッシュの構築。1 グループ = 1 タイル（8x8 ピクセル）
//   ・このフレームのサンプルのうち「見えた」もの（影レイが通った / 影を落とさないライト）を OR
//   ・前フレームの分は、タイル中心を Velocity で戻したタイルから読み、ビットごとに keep の確率で残す
//     （ずっと見えないライトは数フレームで消える。見え続けるライトは毎フレーム立て直される）
// --------------------------------------------------------------------------------------------
groupshared uint gs_Hash[4];

[numthreads(8, 8, 1)] void BuildHash(uint3 groupId : SV_GroupID, uint3 dispatch : SV_DispatchThreadID,
                                     uint groupIndex : SV_GroupIndex) {
    if (groupIndex < 4u)
    {
        gs_Hash[groupIndex] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint2 pixel = dispatch.xy;
    if (all(pixel < MLScreen.xy))
    {
        const uint sampleCount = min(MLStochastic.y, ML_MAX_SPP);
        const uint base = MLSampleBase(MLScreenToSample(pixel)); // 半解像度では 2x2 が同じサンプルを読む
        uint4 bits = uint4(0u, 0u, 0u, 0u);
        for (uint k = 0u; k < sampleCount; ++k)
        {
            const uint4 lightSample = Samples[base + k];
            if ((lightSample.x & ML_SAMPLE_VALID) == 0u || (lightSample.x & ML_SAMPLE_VISIBLE) == 0u)
            {
                continue;
            }
            bits |= MLHashBits(Lights[lightSample.x & ML_SAMPLE_INDEX]);
        }
        if (any(bits != 0u))
        {
            InterlockedOr(gs_Hash[0], bits.x);
            InterlockedOr(gs_Hash[1], bits.y);
            InterlockedOr(gs_Hash[2], bits.z);
            InterlockedOr(gs_Hash[3], bits.w);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (groupIndex == 0u)
    {
        uint4 hash = uint4(gs_Hash[0], gs_Hash[1], gs_Hash[2], gs_Hash[3]);
        const uint2 center = min(groupId.xy * ML_HASH_TILE + ML_HASH_TILE / 2u, MLScreen.xy - 1u);
        uint4 previous;
        if (MLLoadPreviousHash(center, previous))
        {
            // ビットごとに keep の確率で残す
            const float keep = (float)((MLGrid.w >> 8u) & 255u) / 255.0f;
            const uint seed = MLSeed(groupId.xy, MLStochastic.z, 0x48415348u);
            uint4 keepMask = uint4(0u, 0u, 0u, 0u);
            [unroll] for (uint word = 0u; word < 4u; ++word)
            {
                uint mask = 0u;
                uint remaining = previous[word];
                while (remaining != 0u)
                {
                    const uint bit = firstbitlow(remaining);
                    remaining &= remaining - 1u;
                    if (MLRandom(seed + word * 131u + bit * 7919u) < keep)
                    {
                        mask |= 1u << bit;
                    }
                }
                keepMask[word] = mask;
            }
            hash |= keepMask;
        }
        CurrentHash[groupId.y * ((MLScreen.x + ML_HASH_TILE - 1u) / ML_HASH_TILE) + groupId.x] = hash;
    }
}
