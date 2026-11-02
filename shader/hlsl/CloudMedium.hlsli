#ifndef CLOUD_MEDIUM
#define CLOUD_MEDIUM
Texture3D<float> CloudBaseNoise : register(t31);
Texture3D<float> CloudDetailNoise : register(t32);
SamplerState CloudWrap : register(s3);

struct CloudLayerParameters
{
    float4 Layer;
    float4 Shape;
    float4 Wind;
};

// 明示的な float4/uint4 の組で CloudRenderer::Constants と 16 バイトのパッキングに合わせる。
cbuffer CloudParameters : register(b9)
{
    float4 CloudLayer;    // 下端、上端、被覆率、密度の倍率
    float4 CloudShape;    // 基本スケール、詳細スケール、浸食、消散
    float4 CloudLighting; // 位相関数の g、環境光の強度、パウダー効果、ジッター
    float4 CloudWind;     // ラップ済みの基本 UV オフセット（xy）、詳細 UV オフセット（zw）
    uint4 CloudQuality;   // 視線のステップ数、光のステップ数、デバッグモード、レイヤー数
    float4 CloudExtent;   // 全体の幅と高さ、雲の幅と高さ
    float4 CloudTrace;    // 最大距離、ノイズスライス、有効フラグ、影の有効フラグ
    CloudLayerParameters CloudAdditionalLayers[3];
};

CloudLayerParameters GetCloudLayer(uint index)
{
    CloudLayerParameters layer;
    if (index == 0)
    {
        layer.Layer = CloudLayer;
        layer.Shape = CloudShape;
        layer.Wind = CloudWind;
    }
    else
    {
        layer = CloudAdditionalLayers[index - 1];
    }
    return layer;
}

float2 CloudHeightBounds()
{
    [branch] if (CloudQuality.w <= 1)
    {
        return CloudLayer.xy;
    }
    float2 bounds = float2(20000, -10000);
    [loop] for (uint index = 0; index < CloudQuality.w; ++index)
    {
        float4 layer = GetCloudLayer(index).Layer;
        if (layer.z > 0 && layer.w > 0)
        {
            bounds.x = min(bounds.x, layer.x);
            bounds.y = max(bounds.y, layer.y);
        }
    }
    return bounds;
}

void SortCloudIntervalPair(inout float2 nearInterval, inout float2 farInterval)
{
    if (nearInterval.x > farInterval.x)
    {
        float2 previousNear = nearInterval;
        nearInterval = farInterval;
        farInterval = previousNear;
    }
}

// 占有区間をソートして結合し、空の隙間を飛ばして重複部分は一度だけ積分する。
uint CloudIntervals(float3 origin, float3 direction, float limit, out float2 intervals[4])
{
    uint count = 0;
    [unroll] for (uint clearIndex = 0; clearIndex < 4; ++clearIndex)
    {
        // 空の区間は、開始位置が limit より小さい全占有区間の後に並べる。
        intervals[clearIndex] = float2(limit, limit);
    }
    [loop] for (uint index = 0; index < CloudQuality.w; ++index)
    {
        float4 layer = GetCloudLayer(index).Layer;
        if (layer.z <= 0 || layer.w <= 0)
        {
            continue;
        }
        float start = 0;
        float end = limit;
        if (abs(direction.y) < 0.000001)
        {
            if (origin.y < layer.x || origin.y > layer.y)
            {
                continue;
            }
        }
        else
        {
            float2 slab = (layer.xy - origin.y) / direction.y;
            start = max(0, min(slab.x, slab.y));
            end = min(limit, max(slab.x, slab.y));
        }
        if (end > start)
        {
            intervals[index] = float2(start, end);
            ++count;
        }
    }
    [branch] if (CloudQuality.w <= 1)
    {
        return count;
    }
    // 固定の 4 入力ソートネットワークで、FXC の動的インデックス付き挿入ループを避ける。
    SortCloudIntervalPair(intervals[0], intervals[1]);
    SortCloudIntervalPair(intervals[2], intervals[3]);
    SortCloudIntervalPair(intervals[0], intervals[2]);
    SortCloudIntervalPair(intervals[1], intervals[3]);
    SortCloudIntervalPair(intervals[1], intervals[2]);
    uint mergedCount = 0;
    [loop] for (uint interval = 0; interval < count; ++interval)
    {
        float2 current = intervals[interval];
        if (mergedCount > 0)
        {
            if (current.x <= intervals[mergedCount - 1].y)
            {
                intervals[mergedCount - 1].y = max(intervals[mergedCount - 1].y, current.y);
                continue;
            }
        }
        intervals[mergedCount] = current;
        ++mergedCount;
    }
    return mergedCount;
}

// detail = false は基本形状だけを返す（詳細ノイズの 3D サンプルを省く）。
// 空白のスキップと、遠い光のサンプルで使う。浸食は輪郭を削るだけなので、基本形状は上限として安全。
float CloudLayerDensityLod(float3 worldPosition, CloudLayerParameters layer, uint index, bool detail)
{
    if (layer.Layer.z <= 0 || layer.Layer.w <= 0)
    {
        return 0;
    }
    float height = (worldPosition.y - layer.Layer.x) / max(layer.Layer.y - layer.Layer.x, 0.01);
    if (height <= 0 || height >= 1)
    {
        return 0;
    }
    float gradient = smoothstep(0, 0.15, height) * (1 - smoothstep(0.7, 1, height));
    // スロットごとの固定オフセットで、ノイズボリュームを増やさずレイヤー間の相関を除く。
    float3 noiseOffset = float3(0.173, 0.367, 0.619) * index;
    float3 baseUv = worldPosition * layer.Shape.x + noiseOffset;
    baseUv.xz -= layer.Wind.xy;
    float baseNoise = CloudBaseNoise.SampleLevel(CloudWrap, baseUv, 0);
    baseNoise = saturate((baseNoise - 0.2) / 0.6);
    float shape = saturate((baseNoise - (1 - layer.Layer.z)) / max(layer.Layer.z, 0.00001));
    if (shape * gradient < 0.00001)
    {
        return 0;
    }
    // 上部ほど丸く（積雲のカリフラワー状）、下部ほど平らになる高さ依存の形状
    float coverageShape = shape * gradient;
    if (!detail)
    {
        return saturate(coverageShape) * layer.Layer.w;
    }
    float3 detailUv = worldPosition * layer.Shape.y + noiseOffset;
    detailUv.xz -= layer.Wind.zw;
    float detailNoise = CloudDetailNoise.SampleLevel(CloudWrap, detailUv, 0);
    // 下側は細かく（ちぎれた雲底）、上側はふくらんだ（反転した）詳細で浸食する
    float detailModifier = lerp(detailNoise, 1 - detailNoise, saturate(height * 4.0));
    return saturate(coverageShape - detailModifier * layer.Shape.z * (1 - coverageShape)) * layer.Layer.w;
}

float CloudLayerDensity(float3 worldPosition, CloudLayerParameters layer, uint index)
{
    return CloudLayerDensityLod(worldPosition, layer, index, true);
}

float CloudDensity(float3 worldPosition)
{
    float density = CloudLayerDensity(worldPosition, GetCloudLayer(0), 0);
    [branch] if (CloudQuality.w <= 1)
    {
        return density;
    }
    [loop] for (uint index = 1; index < CloudQuality.w; ++index)
    {
        density += CloudLayerDensity(worldPosition, GetCloudLayer(index), index);
    }
    return density;
}

float CloudDensityLod(float3 worldPosition, bool detail)
{
    float density = CloudLayerDensityLod(worldPosition, GetCloudLayer(0), 0, detail);
    [branch] if (CloudQuality.w <= 1)
    {
        return density;
    }
    [loop] for (uint index = 1; index < CloudQuality.w; ++index)
    {
        density += CloudLayerDensityLod(worldPosition, GetCloudLayer(index), index, detail);
    }
    return density;
}

// 雲の内部の照明用の光マーチ。地表の影用（CloudLightTransmittance）とは別。
//  ・距離を指数的に伸ばす（近くは細かく、遠くは粗く）。全区間（数 km）を等分していた従来より、
//    自己遮蔽の細部が出て、サンプル数も少なくて済む
//  ・最初の 2 サンプル以外は基本形状のみ（詳細ノイズを省く）
//  ・最後に長いサンプルを 1 つ追加して、遠くの雲の塊による遮蔽も拾う
float CloudMarchOpticalDepth(float3 position, float3 towardLight, float maxDistance)
{
    const uint stepCount = clamp(CloudQuality.y, 2u, 16u);
    float opticalDepth = 0;
    float previous = 0;
    // 1 + r + r^2 + ... = maxDistance になる公比 r = 1.6 の等比数列
    const float ratio = 1.6;
    float first = maxDistance * (ratio - 1) / (pow(ratio, (float)stepCount) - 1);
    float segment = first;
    [loop] for (uint i = 0; i < stepCount; ++i)
    {
        float distance = previous + segment * 0.5;
        float density = CloudDensityLod(position + towardLight * distance, i < 2);
        opticalDepth += density * segment;
        previous += segment;
        segment *= ratio;
        if (opticalDepth * CloudShape.w > 9.21)
        {
            return opticalDepth;
        }
    }
    // 遠方の塊：光線方向に maxDistance の 3 倍先を 1 サンプル
    opticalDepth += CloudDensityLod(position + towardLight * (maxDistance * 2.5), false) * maxDistance * 2.0;
    return opticalDepth;
}

float CloudHG(float cosine)
{
    float g = CloudLighting.x;
    return (1 - g * g) / (12.566370614 * pow(max(1 + g * g - 2 * g * cosine, 0.001), 1.5));
}

float CloudHGg(float cosine, float g)
{
    return (1 - g * g) / (12.566370614 * pow(max(1 + g * g - 2 * g * cosine, 0.001), 1.5));
}

// 二重ローブ（前方の銀の縁 + 弱い後方散乱）。設定の g を前方ローブに使う
float CloudDualPhase(float cosine, float g)
{
    return lerp(CloudHGg(cosine, -0.25 * saturate(g / 0.85)), CloudHGg(cosine, g), 0.75);
}

float CloudLightTransmittance(float3 position, float3 towardSun)
{
    float2 intervals[4];
    uint count = CloudIntervals(position, towardSun, CloudTrace.x, intervals);
    float opticalDepth = 0;
    [loop] for (uint interval = 0; interval < count; ++interval)
    {
        float stepLength = (intervals[interval].y - intervals[interval].x) / max(CloudQuality.y, 1u);
        [loop] for (uint sample = 0; sample < CloudQuality.y; ++sample)
        {
            float distance = intervals[interval].x + (sample + 0.5) * stepLength;
            opticalDepth += CloudDensity(position + towardSun * distance) * stepLength;
            if (opticalDepth * CloudShape.w > 9.21)
            {
                return 0;
            }
        }
    }
    return exp(-opticalDepth * CloudShape.w);
}

// 太陽円盤の等面積サンプル 4 個を平均する（角半径は約 0.266 度）。
// 雲の内部と受光面で同じ入射光モデルを使う。
float CloudSolarTransmittance(float3 position, float3 towardSun)
{
    float3 tangent = normalize(cross(abs(towardSun.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0), towardSun));
    float3 bitangent = cross(towardSun, tangent);
    const float discOffset = 0.00465 * 0.5;
    float transmission = 0;
    [unroll] for (uint sample = 0; sample < 4; ++sample)
    {
        float2 offset = float2((sample & 1) ? 1 : -1, (sample & 2) ? 1 : -1) * discOffset;
        float3 direction = normalize(towardSun + tangent * offset.x + bitangent * offset.y);
        if (direction.y > 0)
        {
            transmission += CloudLightTransmittance(position, direction);
        }
    }
    return transmission * 0.25;
}

#ifndef CLOUD_MEDIUM_NO_SCENE_LIGHTS
// 空と同じ Sun エンティティについて、CPU が Index+1 を書き込む。
int CloudSunIndex()
{
    int index = (int)round(SkySunDirection.w) - 1;
    return index >= 0 && index < min((int)LightCount.x, 160) ? index : -1;
}

float CloudSunVisibility(float3 position)
{
    if (CloudTrace.z < 0.5 || CloudTrace.w < 0.5 || CloudSunIndex() < 0)
    {
        return 1;
    }
    float3 towardSun = SafeNormalizeCommon(SkySunDirection.xyz, float3(0, 1, 0));
    if (towardSun.y <= 0)
    {
        return 0; // 不透明な惑星が地平線より下からの太陽光を遮る。
    }
    return CloudSolarTransmittance(position, towardSun);
}
#endif

#endif
