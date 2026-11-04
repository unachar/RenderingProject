#ifndef MATERIAL_GRAPH_NODES_EXT_HLSLI
#define MATERIAL_GRAPH_NODES_EXT_HLSLI
// =====================================================================================================
// マテリアルグラフ 追加ノード 33 種の GPU 関数（MGX_*）
//  ・source/assets/materialgraphnodes.h（CPU の純粋関数）と式を 1 対 1 で揃えてある。直すときは両方を同じ順序で
//  ・MaterialGraphVM.hlsli（描画時評価の命令列）と、生成 HLSL（MaterialGraphCompiler::Generate）の両方から include する
//  ・何にも依存しない（テクスチャ / 定数バッファ / common.hlsl を使わない）ので、ToonLighting.hlsli からも使える
// =====================================================================================================
#define MGX_MAX_KEYS 16u

float MGX_Smooth(float e0, float e1, float x)
{
    float d = e1 - e0;
    float t = saturate(abs(d) < 1e-12f ? (x >= e1 ? 1.0f : 0.0f) : (x - e0) / d);
    return t * t * (3.0f - 2.0f * t);
}

float MGX_Frac(float v)
{
    return v - floor(v);
}

float3 MGX_Norm3(float3 v, float3 fallback)
{
    float l2 = dot(v, v);
    return l2 > 1e-12f ? v * rsqrt(l2) : fallback;
}

// ---------------------------------------------------------------------------- キー列（Multi Color Ramp / Curve / Curve Atlas）
// keys[i] = (x, y0, y1, y2)。昇順。count 個
float MGX_MonotoneSlope(float4 keys[MGX_MAX_KEYS], uint count, uint i, uint channel)
{
    if (count < 2u)
    {
        return 0.0f;
    }
    float d0 = 0.0f;
    float d1 = 0.0f;
    if (i > 0u)
    {
        float dx = keys[i].x - keys[i - 1u].x;
        d0 = abs(dx) < 1e-8f ? 0.0f : (keys[i][channel + 1u] - keys[i - 1u][channel + 1u]) / dx;
    }
    if (i + 1u < count)
    {
        float dx = keys[i + 1u].x - keys[i].x;
        d1 = abs(dx) < 1e-8f ? 0.0f : (keys[i + 1u][channel + 1u] - keys[i][channel + 1u]) / dx;
    }
    if (i == 0u)
    {
        return d1;
    }
    if (i + 1u == count)
    {
        return d0;
    }
    if (d0 * d1 <= 0.0f)
    {
        return 0.0f;
    }
    return 2.0f * d0 * d1 / (d0 + d1);
}

// kind 0 = カラーランプ（区間ごとに Linear / smoothstep / Constant）、kind 1 = カーブ（Linear / 単調 3 次 / Constant）
// mode 0 = Linear, 1 = Smooth, 2 = Constant。channels = 評価するチャンネル数（1〜3）
float3 MGX_EvalKeys(float4 keys[MGX_MAX_KEYS], uint count, uint kind, uint mode, uint channels, float x)
{
    if (count == 0u)
    {
        return 0.0f;
    }
    x = isfinite(x) ? x : 0.0f;
    if (count == 1u || x <= keys[0].x)
    {
        return keys[0].yzw;
    }
    if (x >= keys[count - 1u].x)
    {
        return keys[count - 1u].yzw;
    }
    uint i = 0u;
    [loop] while (i + 2u < count && x >= keys[i + 1u].x)
    {
        ++i;
    }
    float span = keys[i + 1u].x - keys[i].x;
    float t = span < 1e-8f ? 0.0f : (x - keys[i].x) / span;
    float3 a = keys[i].yzw;
    float3 b = keys[i + 1u].yzw;
    if (mode == 2u)
    {
        return a;
    }
    if (mode == 1u && kind == 0u)
    {
        t = t * t * (3.0f - 2.0f * t);
        return lerp(a, b, t);
    }
    if (mode == 1u)
    {
        float3 result = lerp(a, b, t);
        float t2 = t * t;
        float t3 = t2 * t;
        [unroll] for (uint c = 0u; c < 3u; ++c)
        {
            if (c >= channels)
            {
                break;
            }
            float m0 = MGX_MonotoneSlope(keys, count, i, c) * span;
            float m1 = MGX_MonotoneSlope(keys, count, i + 1u, c) * span;
            result[c] = (2 * t3 - 3 * t2 + 1) * a[c] + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * b[c] + (t3 - t2) * m1;
        }
        return result;
    }
    return lerp(a, b, t);
}

float MGX_ApplyLoop(float x, uint mode, float start, float end)
{
    float span = end - start;
    if (!(span > 1e-8f))
    {
        return start;
    }
    if (mode == 1u)
    {
        return start + MGX_Frac((x - start) / span) * span;
    }
    if (mode == 2u)
    {
        float p = fmod(abs(x - start) / span, 2.0f);
        return start + (p > 1.0f ? 2.0f - p : p) * span;
    }
    return clamp(x, start, end);
}

float MGX_RowWeight(float row, int index)
{
    return saturate(1.0f - abs(row - (float)index));
}

// ---------------------------------------------------------------------------- Toon
float MGX_ToonWrap(float toonClass)
{
    return toonClass >= 2 ? 0.38f : 0.12f;
}

// 段階ランプの帯の位置 q（0〜1）
float MGX_ToonBand(float nDotL, float wrap, float steps, float threshold, float smoothness)
{
    float c = saturate((nDotL + wrap) / (1.0f + wrap));
    int n = clamp((int)floor(steps + 0.5f), 1, 8);
    float w = max(smoothness, 0.001f);
    float b = 0.0f;
    [loop] for (int k = 1; k < 8; ++k)
    {
        if (k >= n)
        {
            break;
        }
        float edge = threshold + ((float)k - (float)n * 0.5f) / (float)n;
        b += MGX_Smooth(edge - w, edge + w, c);
    }
    return n > 1 ? b / (float)(n - 1) : 1.0f;
}

float3 MGX_ToonRampColor(float q, float3 c1, float3 c2, float3 c3)
{
    return q < 0.5f ? lerp(c1, c2, q * 2.0f) : lerp(c2, c3, (q - 0.5f) * 2.0f);
}

float MGX_ToonRim(float3 n, float3 v, float3 l, float width, float power, float directionalMask)
{
    float edge = 1.0f - saturate(dot(n, v));
    float rim = pow(MGX_Smooth(1.0f - clamp(width, 0.01f, 1.0f), 1.0f, edge), max(power, 0.01f));
    float facing = saturate(dot(n, l) * 0.5f + 0.5f);
    float back = MGX_Smooth(-0.4f, 0.2f, -dot(v, l)); // LiveToonBand(-dot(V, L), -0.1, 0.3)
    return rim * lerp(1.0f, facing * back, saturate(directionalMask));
}

float MGX_HairHighlight(float3 tangent, float3 n, float3 v, float3 l, float anisotropy, float roughness, float width, float shift)
{
    float3 h = MGX_Norm3(l + v, n);
    float3 t = MGX_Norm3(tangent - n * dot(n, tangent), float3(0, 1, 0));
    float3 ts = MGX_Norm3(t + n * shift, t);
    float tDotH = dot(ts, h);
    float sinTH = sqrt(saturate(1.0f - tDotH * tDotH));
    float e = lerp(256.0f, 8.0f, saturate(roughness));
    float kk = pow(sinTH, e) * MGX_Smooth(-1.0f, 0.0f, tDotH);
    float iso = pow(saturate(dot(n, h)), e * 0.5f);
    float spec = lerp(iso, kk, saturate(anisotropy));
    float w = clamp(width, 0.01f, 1.0f);
    return MGX_Smooth(1.0f - w, 1.0f - w * 0.5f, spec) * saturate(dot(n, l) + 0.2f);
}

float MGX_FaceSdfLit(float sdfA, float sdfB, float3 forwardDir, float3 rightDir, float3 lightDir, float threshold, float smoothness,
                     bool flip)
{
    float2 lf = lightDir.xz;
    float2 ff = forwardDir.xz;
    float2 rf = rightDir.xz;
    lf = dot(lf, lf) < 1e-12f ? float2(0, 1) : normalize(lf);
    ff = dot(ff, ff) < 1e-12f ? float2(0, 1) : normalize(ff);
    rf = dot(rf, rf) < 1e-12f ? float2(0, 1) : normalize(rf);
    float fDotL = dot(ff, lf);
    float rDotL = dot(rf, lf);
    bool rightSide = flip ? rDotL <= 0.0f : rDotL > 0.0f;
    float s = (rightSide ? sdfA : sdfB) + (threshold - 0.5f);
    float angle = 0.5f - 0.5f * fDotL;
    float w = max(smoothness, 0.001f);
    return MGX_Smooth(angle - w, angle + w, s);
}

float MGX_EyeHighlightShape(float qx, float qy, float size, float shape)
{
    float p = lerp(2.0f, 6.0f, saturate(shape));
    float d = pow(pow(abs(qx), p) + pow(abs(qy), p), 1.0f / p);
    float s = max(size, 1e-4f);
    return 1.0f - MGX_Smooth(s * 0.85f, s, d);
}

float MGX_EyeHighlight(float2 uv, float2 center, float3 lightDir, float3 viewDir, float3 tangent, float3 bitangent, float size,
                       float shape, float shift)
{
    float lx = dot(lightDir, tangent);
    float ly = dot(lightDir, bitangent);
    float vx = dot(viewDir, tangent);
    float vy = dot(viewDir, bitangent);
    float ox = (lx - vx * 0.5f) * shift;
    float oy = (ly - vy * 0.5f) * shift;
    return MGX_EyeHighlightShape(uv.x - center.x - ox, uv.y - center.y - oy, size, shape);
}

float MGX_Mekakure(float hair, float eye, float opacity, float fadeWidth, float dependence, float3 n, float3 v)
{
    hair = saturate(hair);
    eye = saturate(eye);
    opacity = saturate(opacity);
    float fade = MGX_Smooth(0.0f, max(fadeWidth, 1e-3f), saturate(dot(n, v)));
    float transmit = lerp(1.0f, hair, opacity);
    float effect = lerp(1.0f, fade, saturate(dependence)) * eye;
    return lerp(1.0f, transmit, effect);
}

// ---------------------------------------------------------------------------- PBR / 色
float3 MGX_HsvAdjust(float3 rgb, float hue, float saturation, float value)
{
    float mx = max(rgb.r, max(rgb.g, rgb.b));
    float mn = min(rgb.r, min(rgb.g, rgb.b));
    float d = mx - mn;
    float h = 0.0f;
    if (d > 1e-6f)
    {
        h = mx == rgb.r ? (rgb.g - rgb.b) / d : (mx == rgb.g ? 2.0f + (rgb.b - rgb.r) / d : 4.0f + (rgb.r - rgb.g) / d);
        h = MGX_Frac(h / 6.0f);
    }
    float s = mx > 1e-6f ? d / mx : 0.0f;
    float v = mx;
    h = MGX_Frac(h + hue);
    s = saturate(s * saturation);
    v = max(v * value, 0.0f);
    float3 k = float3(MGX_Frac(h + 3.0f / 3.0f), MGX_Frac(h + 2.0f / 3.0f), MGX_Frac(h + 1.0f / 3.0f)) * 6.0f;
    float3 p = saturate(abs(k - 3.0f) - 1.0f);
    return v * (1.0f + (p - 1.0f) * s);
}

float3 MGX_ThinFilm(float cosTheta, float thickness, float ior, float strength)
{
    float n = max(ior, 1.0f);
    float ci = saturate(cosTheta);
    float si2 = 1.0f - ci * ci;
    float ct = sqrt(max(1.0f - si2 / (n * n), 0.0f));
    float opd = 2.0f * n * max(thickness, 0.0f) * ct;
    float3 lambda = float3(650.0f, 532.0f, 450.0f);
    float3 phase = 6.28318531f * opd / lambda + 3.14159265f;
    return lerp(float3(1, 1, 1), 0.5f + 0.5f * cos(phase), saturate(strength));
}

float3 MGX_DecodeNormal(float4 c)
{
    float3 n = c.xyz * 2.0f - 1.0f;
    if (c.z <= 0.0f)
    {
        n.z = sqrt(saturate(1.0f - dot(n.xy, n.xy)));
    }
    return n;
}

// 3 面の接空間法線（YZ / XZ / XY）→ ワールド法線（whiteout）。weights は和が 1
float3 MGX_TriplanarNormalWorld(float3 nx, float3 ny, float3 nz, float3 nWorld, float3 weights)
{
    float sx = nWorld.x < 0.0f ? -1.0f : 1.0f;
    float sy = nWorld.y < 0.0f ? -1.0f : 1.0f;
    float sz = nWorld.z < 0.0f ? -1.0f : 1.0f;
    float3 tx = float3(nx.x * sx + nWorld.z, nx.y + nWorld.y, abs(nx.z) * nWorld.x);
    float3 ty = float3(ny.x * sy + nWorld.x, ny.y + nWorld.z, abs(ny.z) * nWorld.y);
    float3 tz = float3(nz.x * -sz + nWorld.x, nz.y + nWorld.y, abs(nz.z) * nWorld.z);
    float3 world = tx.zyx * weights.x + ty.xzy * weights.y + tz.xyz * weights.z;
    return MGX_Norm3(world, nWorld);
}

float2 MGX_Flipbook(float2 uv, float rows, float cols, float frameOffset, float fps, float time, uint loop)
{
    int r = clamp((int)floor(clamp(rows, 0.0f, 4096.0f) + 0.5f), 1, 4096); // CPU の Flipbook と同じ上限
    int c = clamp((int)floor(clamp(cols, 0.0f, 4096.0f) + 0.5f), 1, 4096);
    int frames = r * c;
    int raw = (int)clamp(floor(time * fps + frameOffset + 1e-4f), -2147483000.0f, 2147483000.0f);
    int frame = 0;
    if (frames > 1)
    {
        if (loop == 1u)
        {
            frame = raw % frames;
            frame = frame < 0 ? frame + frames : frame;
        }
        else if (loop == 2u)
        {
            int period = 2 * (frames - 1);
            int m = raw % period;
            m = m < 0 ? m + period : m;
            frame = m < frames ? m : period - m;
        }
        else
        {
            frame = clamp(raw, 0, frames - 1);
        }
    }
    return float2((uv.x + (float)(frame % c)) / (float)c, (uv.y + (float)(frame / c)) / (float)r);
}

float4 MGX_Beat(float time, float bpm, float offsetBeats, float subdivision)
{
    if (!(bpm > 0.0f) || !isfinite(time))
    {
        return 0.0f;
    }
    float sub = max(subdivision, 1e-3f);
    float beats = (time * bpm / 60.0f + offsetBeats) * sub;
    float whole = floor(beats);
    float f = beats - whole;
    float index = fmod(whole, 16777216.0f);
    index = index < 0.0f ? index + 16777216.0f : index;
    return float4(f, index, f * 60.0f / (bpm * sub), exp(-6.0f * f));
}

// 異方性の方向。戻り値 = (方向 xyz, 異方性)
float4 MGX_AnisotropyDirection(float3 tangent, float2 dir, float anisotropy, float rotation, float3 n)
{
    n = MGX_Norm3(n, float3(0, 0, 1));
    float3 tt = MGX_Norm3(tangent - n * dot(n, tangent), float3(0, 1, 0));
    float3 bb = cross(n, tt);
    float dl = length(dir);
    float dx = dl > 1e-6f ? dir.x / dl : 1.0f;
    float dy = dl > 1e-6f ? dir.y / dl : 0.0f;
    float a = rotation * 6.28318531f;
    float cs = cos(a);
    float sn = sin(a);
    float rx = dx * cs - dy * sn;
    float ry = dx * sn + dy * cs;
    float3 d = MGX_Norm3(tt * rx + bb * ry, tt);
    return float4(d, clamp(anisotropy, -1.0f, 1.0f));
}

float MGX_CurvatureMask(float curvature, float threshold, float softness, int mode)
{
    float c = mode == 0 ? curvature : (mode == 1 ? -curvature : abs(curvature));
    float s = max(softness, 1e-4f);
    return MGX_Smooth(threshold - s, threshold + s, c);
}

// 深度バッファの値 → ワールド単位の奥行き。reversedZ = 逆 Z（near が 1）。near / far は射影のもの
float MGX_LinearizeDepth(float rawDepth, float nearPlane, float farPlane, bool reversedZ)
{
    float z = reversedZ ? 1.0f - rawDepth : rawDepth;
    return nearPlane * farPlane / max(farPlane - z * (farPlane - nearPlane), 1e-6f);
}

// 奥行き（ワールド）→ 深度バッファの値（Scene Depth の「深度値」出力）
float MGX_DepthBufferValue(float eyeDepth, float nearPlane, float farPlane, bool reversedZ)
{
    float z = saturate(farPlane * (eyeDepth - nearPlane) / max(eyeDepth * (farPlane - nearPlane), 1e-6f));
    return reversedZ ? 1.0f - z : z;
}

// ---------------------------------------------------------------------------- 加算ライトでも使う Toon 係数（ToonNodeLighting.hlsli）
#endif
