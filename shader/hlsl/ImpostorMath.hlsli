#ifndef IMPOSTOR_MATH_HLSLI
#define IMPOSTOR_MATH_HLSLI

// オクタヘドラル・インポスターの共通数学（ベイクと実行時で同じ関数を使い、フレームの向きを完全に一致させる）。
// 座標はすべてモデルのローカル空間。Y を上とする。

float ImpostorSignNotZero(float value)
{
    return value >= 0.0f ? 1.0f : -1.0f;
}

// 全球オクタヘドラル：[-1,1]^2 -> 単位ベクトル
float3 ImpostorOctDecode(float2 f)
{
    float3 n = float3(f.x, 1.0f - abs(f.x) - abs(f.y), f.y);
    float t = saturate(-n.y);
    n.x += (n.x >= 0.0f) ? -t : t;
    n.z += (n.z >= 0.0f) ? -t : t;
    return normalize(n);
}

float2 ImpostorOctEncode(float3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6f);
    float2 p = n.xz;
    if (n.y < 0.0f)
    {
        p = (1.0f - abs(p.yx)) * float2(ImpostorSignNotZero(p.x), ImpostorSignNotZero(p.y));
    }
    return p;
}

// 上半球オクタヘドラル（地面に立つ物用。真下からの視点を持たない分、同じ枚数で解像度が上がる）
float3 ImpostorHemiOctDecode(float2 f)
{
    float x = (f.x + f.y) * 0.5f;
    float z = (f.x - f.y) * 0.5f;
    return normalize(float3(x, max(1.0f - abs(x) - abs(z), 0.0f), z));
}

float2 ImpostorHemiOctEncode(float3 n)
{
    n.y = max(n.y, 0.0f);
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6f);
    return float2(n.x + n.z, n.x - n.z);
}

// フレーム (i, j) の撮影方向（中心 -> カメラ）。グリッドの端点にもフレームを置く（N-1 分割）。
float3 ImpostorFrameDirection(float2 cell, float framesPerAxis, bool hemisphere)
{
    float2 f = cell / max(framesPerAxis - 1.0f, 1.0f) * 2.0f - 1.0f;
    return hemisphere ? ImpostorHemiOctDecode(f) : ImpostorOctDecode(f);
}

void ImpostorFrameBasis(float3 direction, out float3 right, out float3 up)
{
    float3 upHint = abs(direction.y) > 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
    right = normalize(cross(upHint, direction));
    up = cross(direction, right);
}

// ローカル点 -> フレーム内 UV（[0,1]、v は下向き）
float2 ImpostorProject(float3 p, float3 center, float radius, float3 right, float3 up)
{
    float3 q = p - center;
    return float2(dot(q, right), -dot(q, up)) / radius * 0.5f + 0.5f;
}

// 視線方向（ローカル、中心 -> カメラ）から、ブレンドする3フレームと重みを選ぶ。
void ImpostorSelectFrames(float3 viewDirection, float framesPerAxis, bool hemisphere, out float2 cell0,
                          out float2 cell1, out float2 cell2, out float3 weights)
{
    float3 v = viewDirection;
    if (hemisphere)
    {
        v.y = max(v.y, 0.0f);
    }
    float lengthSq = dot(v, v);
    v = lengthSq > 1e-10f ? v * rsqrt(lengthSq) : float3(0.0f, 1.0f, 0.0f);
    float2 encoded = hemisphere ? ImpostorHemiOctEncode(v) : ImpostorOctEncode(v);
    float maxCell = max(framesPerAxis - 1.0f, 1.0f);
    float2 grid = saturate(encoded * 0.5f + 0.5f) * maxCell;
    float2 baseCell = clamp(floor(grid), 0.0f, max(framesPerAxis - 2.0f, 0.0f));
    float2 f = saturate(grid - baseCell);
    if (f.x + f.y <= 1.0f)
    {
        cell0 = baseCell;
        cell1 = baseCell + float2(1.0f, 0.0f);
        cell2 = baseCell + float2(0.0f, 1.0f);
        weights = float3(1.0f - f.x - f.y, f.x, f.y);
    }
    else
    {
        cell0 = baseCell + float2(1.0f, 1.0f);
        cell1 = baseCell + float2(0.0f, 1.0f);
        cell2 = baseCell + float2(1.0f, 0.0f);
        weights = float3(f.x + f.y - 1.0f, 1.0f - f.x, 1.0f - f.y);
    }
}

float3 ImpostorLinearToSrgb(float3 c)
{
    c = saturate(c);
    float3 low = c * 12.92f;
    float3 high = 1.055f * pow(c, 1.0f / 2.4f) - 0.055f;
    return float3(c.r <= 0.0031308f ? low.r : high.r, c.g <= 0.0031308f ? low.g : high.g,
                  c.b <= 0.0031308f ? low.b : high.b);
}

#endif
