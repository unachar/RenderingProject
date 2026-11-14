#include "noise.hlsl"

// =============================================================================================
// BloomCS.hlsl - 物理ベースのブルーム（二重フィルターのミップチェーン）、レンズフレア、画面空間の
// 雨と雪。AdvancedEffects 内でトーンマッピング前の線形 HDR シーンへ適用する。
// BloomDownsample：13 タップのフィルター（Jimenez 2014、CoD:AW）。レベル 0 ではソフトなしきい値と
// Karis 平均を加えて輝点ノイズを抑える。
// BloomUpsample：3x3 テント、up[i] = down[i] + tent(up[i+1]) * radius
// BloomCombine：エネルギー保存の合成 + レンズフレア（Chapman のゴーストとハロー）+ 天候。
// =============================================================================================
cbuffer BloomConstants : register(b0)
{
    uint2 DestinationSize;
    uint2 SourceSize;
    float Threshold;     // 0 = しきい値なし
    float Radius;        // アップサンプリングの散布量
    float Intensity;     // 最終的な合成量
    float Normalization; // 1 / (1 + r + r^2 + ...)
    float FlareIntensity;
    float FlareHalo;
    float RainIntensity;
    float SnowIntensity;
    float Time;
    float WindX;
    uint FirstLevel;
    uint Flags; // bit0 = ブルーム、bit1 = フレア、bit2 = 雨、bit3 = 雪、bit4 = 太陽のレンズフレア、bit5 = ディフュージョン
    float DiffusionAmount;
    float DiffusionRadius; // 出力解像度のピクセル
    float2 DiffusionPad;
};

// 太陽のレンズフレア（AdvancedEffects::SunFlareConstants と同じ並び）
cbuffer SunFlareConstants : register(b1)
{
    float2 SunUv;      // 太陽の画面 UV（画面外のこともある）
    float SunVisible;  // 画面内 / 高度によるフェード
    float SunAspect;   // 幅 / 高さ
    float3 SunColor;   // 太陽光の色 × 強度（線形 HDR）
    float SunFlareIntensity;
    float SunGlare;
    float SunStarburst;
    float SunGhosts;
    float SunHalo;
    float SunAnamorphic;
    float SunBlades;
    float SunRotation;
    float SunUnused;
};

Texture2D<float4> Source0 : register(t0);
Texture2D<float4> Source1 : register(t1);
Texture2D<float4> Source2 : register(t2);
Texture2D<float> SceneDepth : register(t3);
RWTexture2D<float4> Destination : register(u0);
SamplerState LinearClamp : register(s0);

#include "Half16.hlsli"

static const float3 kLuma = float3(0.2126f, 0.7152f, 0.0722f);

// ダウン/アップサンプルのフィルター演算は 16bit（対応 GPU）で行う。加算は重み付きで逐次行い、
// 中間値が入力の最大値を超えないようにして FP16 のオーバーフローを避ける。
// 明るい画素の重みは FP16 の非正規化数域（< 6.1e-5）に入るため、重みと正規化は float で行う。
float KarisWeight(hfloat3 color)
{
    return 1.0f / (1.0f + dot((float3)color, kLuma));
}

hfloat3 SoftThreshold(hfloat3 color)
{
    if (Threshold <= 0.0f)
    {
        return color;
    }
    // しきい値の計算はスカラー 1 回なので float で行い、係数だけ 16bit に戻す。
    float brightness = max(color.r, max(color.g, color.b));
    float knee = Threshold * 0.5f;
    float soft = clamp(brightness - Threshold + knee, 0.0f, 2.0f * knee);
    soft = soft * soft / (4.0f * knee + 0.00001f);
    float contribution = max(soft, brightness - Threshold) / max(brightness, 0.00001f);
    return color * (hfloat)contribution;
}

hfloat3 SampleSource0(float2 uv)
{
    return ToHalf3(Source0.SampleLevel(LinearClamp, uv, 0).rgb);
}

hfloat3 SampleSource1(float2 uv)
{
    return ToHalf3(Source1.SampleLevel(LinearClamp, uv, 0).rgb);
}

// 4 値の平均（逐次加算でオーバーフローしない）
hfloat3 Average4(hfloat3 a, hfloat3 b, hfloat3 c, hfloat3 d)
{
    const hfloat quarter = (hfloat)0.25f;
    return a * quarter + b * quarter + c * quarter + d * quarter;
}

[numthreads(8, 8, 1)] void BloomDownsample(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= DestinationSize))
    {
        return;
    }
    float2 uv = (id.xy + 0.5f) / DestinationSize;
    float2 t = 1.0f / SourceSize;
    hfloat3 a = SampleSource0(uv + t * float2(-2, -2));
    hfloat3 b = SampleSource0(uv + t * float2(0, -2));
    hfloat3 c = SampleSource0(uv + t * float2(2, -2));
    hfloat3 d = SampleSource0(uv + t * float2(-2, 0));
    hfloat3 e = SampleSource0(uv);
    hfloat3 f = SampleSource0(uv + t * float2(2, 0));
    hfloat3 g = SampleSource0(uv + t * float2(-2, 2));
    hfloat3 h = SampleSource0(uv + t * float2(0, 2));
    hfloat3 i = SampleSource0(uv + t * float2(2, 2));
    hfloat3 j = SampleSource0(uv + t * float2(-1, -1));
    hfloat3 k = SampleSource0(uv + t * float2(1, -1));
    hfloat3 l = SampleSource0(uv + t * float2(-1, 1));
    hfloat3 m = SampleSource0(uv + t * float2(1, 1));
    hfloat3 result;
    if (FirstLevel != 0)
    {
        // 2x2 グループごとの Karis 平均で、チェーン全体から単独ピクセルの輝点ノイズを除く。
        hfloat3 g0 = Average4(j, k, l, m);
        hfloat3 g1 = Average4(a, b, d, e);
        hfloat3 g2 = Average4(b, c, e, f);
        hfloat3 g3 = Average4(d, e, g, h);
        hfloat3 g4 = Average4(e, f, h, i);
        float w0 = KarisWeight(g0) * 0.5f;
        float w1 = KarisWeight(g1) * 0.125f;
        float w2 = KarisWeight(g2) * 0.125f;
        float w3 = KarisWeight(g3) * 0.125f;
        float w4 = KarisWeight(g4) * 0.125f;
        float3 weighted = (float3)g0 * w0 + (float3)g1 * w1 + (float3)g2 * w2 + (float3)g3 * w3 + (float3)g4 * w4;
        result = ToHalf3(weighted / max(w0 + w1 + w2 + w3 + w4, 0.0001f));
        result = SoftThreshold(result);
    }
    else
    {
        // 重みの総和は 1。逐次加算で中間値が最大値を超えない。
        const hfloat corner = (hfloat)0.03125f, edge = (hfloat)0.0625f, inner = (hfloat)0.125f;
        result = e * inner;
        result += a * corner + c * corner + g * corner + i * corner;
        result += b * edge + d * edge + f * edge + h * edge;
        result += j * inner + k * inner + l * inner + m * inner;
    }
    Destination[id.xy] = float4(max((float3)result, 0.0f), 1.0f);
}

    [numthreads(8, 8, 1)] void BloomUpsample(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= DestinationSize))
    {
        return;
    }
    float2 uv = (id.xy + 0.5f) / DestinationSize;
    float2 t = 1.0f / SourceSize; // 下位の粗いレベルのサイズ
    // 3x3 テント（重み 4/16, 2/16, 1/16）。重みを先に掛けて加算する。
    const hfloat centerWeight = (hfloat)0.25f, edgeWeight = (hfloat)0.125f, cornerWeight = (hfloat)0.0625f;
    hfloat3 tent = SampleSource1(uv) * centerWeight;
    tent += SampleSource1(uv + t * float2(-1, 0)) * edgeWeight + SampleSource1(uv + t * float2(1, 0)) * edgeWeight +
            SampleSource1(uv + t * float2(0, -1)) * edgeWeight + SampleSource1(uv + t * float2(0, 1)) * edgeWeight;
    tent += SampleSource1(uv + t * float2(-1, -1)) * cornerWeight +
            SampleSource1(uv + t * float2(1, -1)) * cornerWeight +
            SampleSource1(uv + t * float2(-1, 1)) * cornerWeight + SampleSource1(uv + t * float2(1, 1)) * cornerWeight;
    // 最後の合成は float（current + tent * Radius は FP16 の範囲を超え得る）。
    float3 current = Source0.SampleLevel(LinearClamp, uv, 0).rgb;
    Destination[id.xy] = float4(current + (float3)tent * Radius, 1.0f);
}



float3 LensFlare(float2 uv)
{
    float2 flipped = 1.0f - uv;
    float2 ghostVector = (0.5f - flipped) * 0.37f;
    float3 result = 0.0f;
    [unroll] for (int ghost = 0; ghost < 5; ++ghost)
    {
        float2 offset = frac(flipped + ghostVector * ghost);
        float weight = pow(saturate(1.0f - length(0.5f - offset) / 0.7071f), 8.0f);
        // 色収差：各チャンネルをゴーストの軸に沿って少しずらす。
        float2 shift = normalize(ghostVector + 0.00001f) * 0.006f;
        float3 sample =
            float3(Source2.SampleLevel(LinearClamp, offset + shift, 0).r, Source2.SampleLevel(LinearClamp, offset, 0).g,
                   Source2.SampleLevel(LinearClamp, offset - shift, 0).b);
        result += max(sample - 1.0f, 0.0f) * weight * (ghost == 0 ? 0.5f : 1.0f);
    }
    float2 haloVector = normalize(ghostVector + 0.00001f) * 0.42f;
    float2 haloUv = frac(flipped + haloVector);
    float haloWeight = pow(saturate(1.0f - length(0.5f - haloUv) / 0.7071f), 5.0f);
    result += max(Source2.SampleLevel(LinearClamp, haloUv, 0).rgb - 1.0f, 0.0f) * haloWeight * FlareHalo * 4.0f;
    // コーティングされた光学系に似た分光的な色調（中心は暖色、端は寒色）。
    float radial = length(uv - 0.5f) * 1.4f;
    return result * lerp(float3(1.0f, 0.85f, 0.7f), float3(0.6f, 0.75f, 1.0f), saturate(radial));
}

// 視差の異なる 3 層の落下する筋。シーンが照らされている場所ほど明るい。
float RainLayer(float2 uv, float scale, float speed, float slant, float seed)
{
    float2 p = float2(uv.x * scale + uv.y * slant * scale, uv.y * scale * 0.12f - Time * speed);
    float2 cell = floor(p);
    float2 local = frac(p);
    float random = Hash21(cell + seed);
    if (random > 0.35f)
    {
        return 0.0f;
    }
    float x = abs(local.x - frac(0.2f + random * 1.6f));
    float streak = saturate(1.0f - x * scale * 0.9f);
    float y = frac(local.y + random * 7.0f);
    float lengthMask = smoothstep(0.0f, 0.15f, y) * smoothstep(0.75f, 0.35f, y);
    return streak * streak * lengthMask;
}

float SnowLayer(float2 uv, float scale, float speed, float seed, float aspect)
{
    float2 p = float2(uv.x * aspect, uv.y) * scale;
    p.y -= Time * speed;
    p.x += sin(Time * 0.7f + p.y * 0.8f + seed) * 0.35f + Time * WindX * 0.5f;
    float2 cell = floor(p);
    float2 local = frac(p) - 0.5f;
    float random = Hash21(cell + seed);
    if (random > 0.45f)
    {
        return 0.0f;
    }
    float2 center = (float2(Hash21(cell + 3.1f + seed), Hash21(cell + 7.7f + seed)) - 0.5f) * 0.6f;
    float radius = 0.06f + random * 0.08f;
    return smoothstep(radius, radius * 0.4f, length(local - center));
}

// =============================================================================================
// 太陽のレンズフレア（大気シミュレーションの太陽から生成。参照：実写の逆光）
//  ・グレア     : 太陽の周りの明るいにじみ（ベーリンググレア）
//  ・光芒       : 絞り羽根の回折による放射状の光条（羽根の枚数 × 2 本）と細かい筋
//  ・ゴースト   : 太陽 → 画面中心 → 反対側の軸上に並ぶ、絞りの形（多角形）の反射像。縁は色収差で虹色
//  ・ハロー     : 太陽を囲む淡い虹色のリング
//  ・ストリーク : アナモルフィックレンズ風の横長の光（任意）
//  遮蔽は 8x8 グループ単位で、太陽の円盤の周りの深度 64 点を調べて空の割合を求める（建物・木の裏で消える）
// =============================================================================================
groupshared float gs_SunOcclusion[64];

float SunFlareHash(float x)
{
    return frac(sin(x * 127.1f) * 43758.5453f);
}

// 正 N 角形（中心からの距離を多角形の内接円で正規化）。N が大きいほど円に近い
float PolygonDistance(float2 p, float blades, float rotation)
{
    const float sector = 6.28318530718f / blades;
    float angle = atan2(p.y, p.x) + rotation;
    float local = fmod(angle + 31.4159265f * blades, sector) - sector * 0.5f; // 正の剰余
    return length(p) * cos(local) / cos(sector * 0.5f);
}

// 1 つのゴースト。R/G/B で半径を少し変えて縁に色収差を出す
float3 SunGhost(float2 p, float2 center, float radius, float3 tint, float ringWeight)
{
    float3 result;
    [unroll] for (int channel = 0; channel < 3; ++channel)
    {
        const float r = radius * (1.0f + (channel - 1) * 0.035f);
        const float d = PolygonDistance(p - center, SunBlades, SunRotation * 2.0f) / r;
        const float body = smoothstep(1.0f, 0.82f, d); // 内側は塗り、縁はやわらかく
        const float ring = exp(-pow((d - 0.94f) * 14.0f, 2.0f));
        result[channel] = body * (0.55f + 0.45f * d) + ring * ringWeight;
    }
    return result * tint;
}

float SunOcclusionTap(uint index)
{
    // 太陽の円盤（視直径 約 0.5 度）より少し広い範囲を、黄金角のらせんで 64 点
    const float t = (index + 0.5f) / 64.0f;
    const float angle = index * 2.39996323f;
    const float radius = sqrt(t) * 0.012f;
    float2 uv = SunUv + float2(cos(angle) / max(SunAspect, 0.01f), sin(angle)) * radius;
    if (any(uv < 0.0f) || any(uv > 1.0f))
    {
        return 1.0f; // 画面外の部分は遮蔽物が分からないので見えているとみなす
    }
    // 逆 Z：空（何も描かれていない）は深度 0
    return SceneDepth.SampleLevel(LinearClamp, uv, 0) <= 0.0f ? 1.0f : 0.0f;
}

float3 SunLensFlare(float2 uv, float visibility)
{
    // アスペクト補正した画面中心基準の座標
    const float2 aspect = float2(SunAspect, 1.0f);
    const float2 p = (uv - 0.5f) * aspect;
    const float2 sun = (SunUv - 0.5f) * aspect;
    const float2 toSun = p - sun;
    const float distance = length(toSun);
    const float3 sunLight = SunColor * (SunFlareIntensity * visibility);
    float3 result = 0.0f;

    // ---- グレア：芯の明るい点 + 広いにじみ（距離の逆二乗で減衰）
    const float core = 1.0f / (1.0f + pow(distance * 90.0f, 2.0f));
    const float veil = 1.0f / (1.0f + pow(distance * 9.0f, 2.0f));
    result += (core * 2.0f + veil * 0.12f) * SunGlare;

    // ---- 光芒：羽根の数 × 2 本の主光条 + ハッシュで揺らした細い筋
    {
        const float angle = atan2(toSun.y, toSun.x) - SunRotation;
        const float rays = pow(abs(cos(angle * SunBlades * 0.5f)), 220.0f);
        const float fine = SunFlareHash(floor((angle + 3.14159265f) * 60.0f));
        const float streaks = pow(abs(cos(angle * 37.0f + fine * 6.28f)), 60.0f) * fine;
        const float falloff = 1.0f / (1.0f + distance * 7.0f) * exp(-distance * 2.2f);
        result += (rays * 0.9f + streaks * 0.35f) * falloff * 0.6f * SunStarburst;
    }

    // ---- ゴースト：太陽と画面中心を結ぶ軸上（反対側へ伸びる）
    if (SunGhosts > 0.0f)
    {
        // 太陽が画面中心に近いほどゴーストが重なって明るい
        const float centered = saturate(1.0f - length(sun) * 0.9f);
        // 位置（軸上の比率：1 = 太陽、0 = 中心、負 = 反対側）、半径、色
        static const float3 kGhosts[8] = {float3(0.62f, 0.016f, 0.0f), float3(0.35f, 0.035f, 1.0f),
                                          float3(0.12f, 0.010f, 2.0f), float3(-0.18f, 0.060f, 3.0f),
                                          float3(-0.42f, 0.022f, 4.0f), float3(-0.70f, 0.105f, 5.0f),
                                          float3(-1.05f, 0.040f, 6.0f), float3(-1.40f, 0.150f, 7.0f)};
        static const float3 kTints[8] = {float3(1.0f, 0.65f, 0.30f), float3(0.35f, 1.0f, 0.55f),
                                         float3(1.0f, 0.85f, 0.45f), float3(0.55f, 0.35f, 1.0f),
                                         float3(0.40f, 0.95f, 0.65f), float3(0.70f, 0.40f, 0.95f),
                                         float3(1.0f, 0.55f, 0.35f), float3(0.45f, 0.65f, 1.0f)};
        float3 ghosts = 0.0f;
        [unroll] for (int ghost = 0; ghost < 8; ++ghost)
        {
            const float2 center = sun * kGhosts[ghost].x;
            const float radius = kGhosts[ghost].y * (1.0f + length(sun) * 0.6f);
            // 小さいゴーストは明るく、大きいものは淡く（同じ光量が広がる）
            const float brightness = 0.02f / (radius + 0.02f);
            ghosts += SunGhost(p, center, radius, kTints[ghost], 0.6f) * brightness;
        }
        result += ghosts * 0.04f * (0.35f + 0.65f * centered) * SunGhosts;
    }

    // ---- ハロー：太陽を囲む虹色のリング（波長ごとに半径を変える）
    if (SunHalo > 0.0f)
    {
        float3 halo;
        [unroll] for (int channel = 0; channel < 3; ++channel)
        {
            const float radius = 0.30f + channel * 0.012f;
            halo[channel] = exp(-pow((distance - radius) * 38.0f, 2.0f));
        }
        result += halo * 0.012f * SunHalo;
    }

    // ---- アナモルフィック・ストリーク
    if (SunAnamorphic > 0.0f)
    {
        const float streak = exp(-abs(toSun.y) * 260.0f) * exp(-abs(toSun.x) * 1.6f);
        result += streak * float3(0.55f, 0.75f, 1.0f) * 0.08f * SunAnamorphic;
    }
    return result * sunLight;
}

[numthreads(8, 8, 1)] void BloomCombine(uint3 id : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
    // 太陽の遮蔽（グループ全体で 64 点を分担）。早期 return より前に行う（バリアを全スレッドで通過させる）
    float sunVisibility = 0.0f;
    if ((Flags & 16u) != 0)
    {
        gs_SunOcclusion[groupIndex] = SunOcclusionTap(groupIndex);
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint stride = 32; stride > 0; stride >>= 1)
        {
            if (groupIndex < stride)
            {
                gs_SunOcclusion[groupIndex] += gs_SunOcclusion[groupIndex + stride];
            }
            GroupMemoryBarrierWithGroupSync();
        }
        // 一部が隠れると急に暗くなる（実際のフレアも絞りに入る光量にほぼ比例）
        sunVisibility = (gs_SunOcclusion[0] / 64.0f) * SunVisible;
    }
    if (any(id.xy >= DestinationSize))
    {
        return;
    }
    float2 uv = (id.xy + 0.5f) / DestinationSize;
    float3 color = Source0[id.xy].rgb;
    if ((Flags & 32u) != 0)
    {
        // ディフュージョン（ソフトフォーカス）：黄金角スパイラル 16 タップでにじませ、「比較（明）」で重ねる。
        // 暗部は持ち上げず、明るい部分の周りだけがふわっと広がる（肌・衣装をやわらかく見せる撮影用フィルター風）。
        const float2 texel = 1.0f / (float2)DestinationSize;
        float weightSum = 1.0f / (1.0f + dot(color, kLuma));
        float3 blurred = color * weightSum;
        [unroll] for (uint tap = 1u; tap < 16u; ++tap)
        {
            float radius = sqrt((float)tap / 15.0f) * DiffusionRadius;
            float angle = (float)tap * 2.39996323f;
            float2 offset = float2(cos(angle), sin(angle)) * radius * texel;
            float3 sampleColor = Source0.SampleLevel(LinearClamp, uv + offset, 0).rgb;
            // 輝点 1 つで全体が白むのを抑える（Karis 平均と同じ考え方）
            float weight = 1.0f / (1.0f + dot(sampleColor, kLuma));
            blurred += sampleColor * weight;
            weightSum += weight;
        }
        blurred /= weightSum;
        color = lerp(color, max(color, blurred), DiffusionAmount);
    }
    float3 bloom = 0.0f;
    if ((Flags & 1u) != 0)
    {
        bloom = Source1.SampleLevel(LinearClamp, uv, 0).rgb * Normalization;
        // エネルギー保存：ブルームは光を追加せず、再分配する。
        color = lerp(color, bloom, Intensity);
    }
    if ((Flags & 2u) != 0)
    {
        color += LensFlare(uv) * FlareIntensity * 0.05f;
    }
    if ((Flags & 16u) != 0 && sunVisibility > 0.0001f)
    {
        color += SunLensFlare(uv, sunVisibility);
    }
    float ambient = dot(bloom, kLuma) + 0.02f;
    if ((Flags & 4u) != 0)
    {
        float aspect = DestinationSize.x / (float)DestinationSize.y;
        float2 rainUv = float2(uv.x * aspect, uv.y);
        float rain = RainLayer(rainUv, 70.0f, 2.6f, WindX * 0.25f, 1.0f) * 0.5f +
                     RainLayer(rainUv, 45.0f, 2.0f, WindX * 0.22f, 2.0f) * 0.35f +
                     RainLayer(rainUv, 28.0f, 1.5f, WindX * 0.2f, 3.0f) * 0.25f;
        color += rain * RainIntensity * (0.03f + ambient * 0.6f);
    }
    if ((Flags & 8u) != 0)
    {
        float aspect = DestinationSize.x / (float)DestinationSize.y;
        float snow = SnowLayer(uv, 26.0f, 0.10f, 1.0f, aspect) * 0.6f + SnowLayer(uv, 14.0f, 0.06f, 5.0f, aspect) +
                     SnowLayer(uv, 8.0f, 0.04f, 9.0f, aspect) * 1.2f;
        color = lerp(color, max(color, (0.35f + ambient * 2.0f).xxx), saturate(snow * SnowIntensity));
    }
    Destination[id.xy] = float4(color, 1.0f);
}


