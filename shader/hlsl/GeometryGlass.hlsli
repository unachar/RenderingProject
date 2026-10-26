#ifndef GEOMETRY_GLASS_INCLUDED
#define GEOMETRY_GLASS_INCLUDED
#ifdef SHADER_RAYTRACING
struct GlassMeshData
{
    float4x4 World, NormalToWorld;
    uint4 Layout;
    float4 BaseColor, Emission, Optical, Absorption, OpticalExtra;
    uint4 Flags;
    ProceduralTexture::Parameters NoiseBase;
};

StructuredBuffer<GlassMeshData> GlassMeshes : register(t0, space12);
ByteAddressBuffer GlassVertices[16384] : register(t1, space12);
ByteAddressBuffer GlassIndices[16384] : register(t0, space13);
Texture2D<float4> GlassTextures[16384] : register(t0, space14);
#include "GlassHitLighting.hlsli"

uint GlassIndex(uint instance, uint index, GlassMeshData mesh)
{
    if (mesh.Layout.y == 0)
    {
        return index;
    }
    uint address = mesh.Layout.w + index * (mesh.Layout.y == 16 ? 2 : 4);
    uint word = GlassIndices[NonUniformResourceIndex(instance)].Load(address & ~3u);
    return mesh.Layout.y == 16 ? ((word >> ((address & 2u) * 8u)) & 65535u) : word;
}

struct GlassHit
{
    uint Instance;
    float Distance;
    float3 Position, Normal, Geometric;
    float2 UV;
    float3 Color;
};

bool TraceGlassBoundary(float3 origin, float3 direction, float coneAngle, float traveled, out GlassHit hit)
{
    hit = (GlassHit)0;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.00001f;
    ray.TMax = 2000.0f;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0x06, ray);
    while (query.Proceed())
    {
    }
    if (query.CommittedStatus() == COMMITTED_NOTHING)
    {
        return false;
    }
    uint instance = query.CommittedInstanceID();
    if (instance >= (uint)SceneRayFlags.w)
    {
        return false;
    }
    GlassMeshData mesh = GlassMeshes[instance];
    uint primitive = query.CommittedPrimitiveIndex();
    if (primitive >= mesh.Flags.z)
    {
        return false;
    }
    float2 bary = query.CommittedTriangleBarycentrics();
    float3 weights = float3(1 - bary.x - bary.y, bary);
    float3 positions[3];
    float2 triangleUV[3];
    float3 smoothNormal = 0;
    float2 uv = 0;
    float3 vertexColor = 0;
    [unroll] for (uint corner = 0; corner < 3; ++corner)
    {
        uint index = GlassIndex(instance, primitive * 3 + corner, mesh);
        uint offset = mesh.Layout.z + index * mesh.Layout.x;
        float3 p = asfloat(GlassVertices[NonUniformResourceIndex(instance)].Load3(offset));
        positions[corner] = mul(float4(p, 1), mesh.World).xyz;
        if (mesh.Layout.x >= 24)
        {
            smoothNormal +=
                asfloat(GlassVertices[NonUniformResourceIndex(instance)].Load3(offset + 12)) * weights[corner];
        }
        triangleUV[corner] =
            mesh.Layout.x >= 32 ? asfloat(GlassVertices[NonUniformResourceIndex(instance)].Load2(offset + 24)) : 0.0f;
        uv += triangleUV[corner] * weights[corner];
        if (mesh.Layout.x >= 44)
        {
            vertexColor +=
                asfloat(GlassVertices[NonUniformResourceIndex(instance)].Load3(offset + 32)) * weights[corner];
        }
    }
    float3 geometric =
        SafeNormalizeCommon(cross(positions[1] - positions[0], positions[2] - positions[0]), float3(0, 1, 0));
    smoothNormal = SafeNormalizeCommon(mul(float4(smoothNormal, 0), mesh.NormalToWorld).xyz, geometric);
    if (dot(geometric, smoothNormal) < 0)
    {
        geometric = -geometric;
    }
    hit.Instance = instance;
    hit.Distance = query.CommittedRayT();
    hit.Position = origin + direction * hit.Distance;
    hit.Normal = smoothNormal;
    hit.Geometric = geometric;
    hit.UV = uv;
    hit.Color = max(mesh.BaseColor.rgb, 0.0f);
    if (mesh.NoiseBase.Width != 0)
        hit.Color *= ProceduralTexture::Color(mesh.NoiseBase, ProceduralTexture::Scalar(mesh.NoiseBase, uv));
    // ラスターパスではアセットのマテリアルは白から始める。設定されたプリミティブやモデルの
    // 頂点 RGB を使うのは従来のマテリアルだけ。
    if ((mesh.Flags.w & 1u) == 0u && mesh.Layout.x >= 44)
    {
        hit.Color *= vertexColor;
    }
    if (mesh.Flags.y != 0)
    {
        uint width, height, levels;
        GlassTextures[NonUniformResourceIndex(instance)].GetDimensions(0, width, height, levels);
        float3 e1 = positions[1] - positions[0];
        float3 e2 = positions[2] - positions[0];
        float2 duv1 = triangleUV[1] - triangleUV[0];
        float2 duv2 = triangleUV[2] - triangleUV[0];
        float area = max(length(cross(e1, e2)), 1e-10f);
        float uvDensity = sqrt(abs(duv1.x * duv2.y - duv1.y * duv2.x) / area);
        float footprint = max(coneAngle * (traveled + hit.Distance), 1e-6f);
        float lod =
            clamp(log2(max(footprint * uvDensity * max(width, height), 1.0f)), 0.0f, (float)max(levels, 1u) - 1.0f);
        hit.Color *= GlassTextures[NonUniformResourceIndex(instance)].SampleLevel(g_SamplerState, uv, lod).rgb;
    }
    return all(isfinite(hit.Position)) && all(isfinite(hit.Normal)) && all(isfinite(hit.Color));
}

float3 GlassHitRadiance(GlassHit hit, float3 direction)
{
    GlassMeshData mesh = GlassMeshes[hit.Instance];
    float3 N = dot(hit.Normal, direction) < 0 ? hit.Normal : -hit.Normal;
    // 常に実際のヒットをシェーディングする。画面色の近道、可視性の
    // 切り替え、主マテリアルのグローバル値で床の色を変えない。
    float f0 = saturate(mesh.OpticalExtra.y);
    float3 direct =
        GlassEvaluateWorldDirect(hit.Position, N, -direction, hit.Color, mesh.Optical.w, mesh.Optical.z, f0, 1.0f);
    return direct + max(mesh.Emission.rgb * mesh.Emission.w, 0.0f);
}

float GlassRayEpsilon(float3 p)
{
    return max(0.000005f, max(abs(p.x), max(abs(p.y), abs(p.z))) * 0.00000025f);
}

// 外向きに閉じたメッシュが必要。透過経路は決定的に計算する。
// 環境照明や反射へのフォールバックは使わず、全反射は実際の形状をたどる。
bool IntegrateGeometryGlass(GlassHit hit, float3 origin, float3 direction, float coneAngle, out float3 radiance)
{
    radiance = 0;
    float traveled = 0;
    uint mediumID[8];
    float mediumIOR[8];
    float3 mediumSigma[8];
    [unroll] for (uint z = 0; z < 8; ++z)
    {
        mediumID[z] = 0;
        mediumIOR[z] = 1;
        mediumSigma[z] = 0;
    }
    uint count = 0;
    float3 throughput = 1;
    float pathRoughness = 0;
    [loop] for (uint boundary = 0; boundary < 24; ++boundary)
    {
        GlassMeshData mesh = GlassMeshes[hit.Instance];
        bool dielectric = mesh.Optical.y > 0.001f && mesh.Optical.w < 0.999f;
        bool entering = dot(direction, hit.Geometric) < 0;
        if (dielectric)
        {
            pathRoughness = max(pathRoughness, saturate(mesh.OpticalExtra.x));
        }
        if (boundary == 0 && dielectric && !entering)
        {
            mediumID[0] = mesh.Flags.x;
            mediumIOR[0] = max(mesh.Optical.x, 1.0f);
            mediumSigma[0] =
                max(mesh.Absorption.xyz, 0) - log(clamp(hit.Color, 1e-4f, 1)) / max(mesh.Absorption.w, 1e-4f);
            count = 1;
        }
        if (count > 0)
        {
            throughput *= exp(-min(mediumSigma[count - 1] * hit.Distance, 80.0f));
        }
        if (max(throughput.x, max(throughput.y, throughput.z)) < 0.0001f)
        {
            return true;
        }
        if (!dielectric)
        {
            radiance += throughput * GlassHitRadiance(hit, direction);
            return true;
        }
        float etaI = count > 0 ? mediumIOR[max((int)count - 1, 0)] : 1.0f;
        int found = -1;
        [unroll] for (uint k = 0; k < 8; ++k)
        {
            if (k < count && mediumID[k] == mesh.Flags.x)
            {
                found = (int)k;
            }
        }
        float etaT;
        if (entering)
        {
            etaT = max(mesh.Optical.x, 1.0f);
        }
        else if (found >= 0)
        {
            etaT = etaI;
            if ((uint)found == count - 1)
            {
                etaT = 1.0f;
                if (count > 1)
                {
                    etaT = mediumIOR[max((int)count - 2, 0)];
                }
            }
        }
        else
        {
            etaI = max(mesh.Optical.x, 1.0f);
            etaT = count > 0 ? mediumIOR[max((int)count - 1, 0)] : 1.0f;
        }
        float3 N = entering ? hit.Normal : -hit.Normal;
        float3 Ng = entering ? hit.Geometric : -hit.Geometric;
        if (dot(N, direction) >= -0.00001f)
        {
            N = Ng;
        }
        float3 transmitted = refract(direction, N, etaI / max(etaT, 1e-6f));
        if (dot(transmitted, transmitted) > 1e-8f && dot(transmitted, Ng) >= 0)
        {
            N = Ng;
            transmitted = refract(direction, N, etaI / max(etaT, 1e-6f));
        }
        bool tir = dot(transmitted, transmitted) < 1e-8f;
        float F = GlassFresnel(-dot(direction, N), etaI, etaT);
        if (entering || boundary == 0)
        {
            float transmission = saturate(mesh.Optical.y);
            float metallic = saturate(mesh.Optical.w);
            float f0 = pow((etaI - etaT) / max(etaI + etaT, 1e-6f), 2);
            float3 direct = GlassEvaluateWorldDirect(hit.Position, N, -direction, hit.Color, metallic, mesh.Optical.z,
                                                     f0, 1 - transmission);
            radiance += throughput * (direct + mesh.Emission.rgb * mesh.Emission.w);
        }
        if (tir)
        {
            float3 reflected = reflect(direction, N);
            if (dot(reflected, Ng) <= 0)
            {
                reflected = reflect(direction, Ng);
            }
            direction = normalize(reflected);
        }
        else
        {
            // 誘電体の境界では環境放射輝度を加えない。
            throughput *= 1 - F;
            if (entering)
            {
                if (count >= 8)
                {
                    return true;
                }
                mediumID[count] = mesh.Flags.x;
                mediumIOR[count] = etaT;
                mediumSigma[count] =
                    max(mesh.Absorption.xyz, 0) - log(clamp(hit.Color, 1e-4f, 1)) / max(mesh.Absorption.w, 1e-4f);
                ++count;
                throughput *= saturate(mesh.Optical.y) * (1 - saturate(mesh.Optical.w));
            }
            else if (found >= 0)
            {
                [unroll] for (uint m = 0; m < 7; ++m)
                {
                    if (m >= (uint)found && m + 1 < count)
                    {
                        mediumID[m] = mediumID[m + 1];
                        mediumIOR[m] = mediumIOR[m + 1];
                        mediumSigma[m] = mediumSigma[m + 1];
                    }
                }
                --count;
            }
            direction = normalize(transmitted);
        }
        traveled += hit.Distance;
        // 各界面を通じてテクスチャの投影範囲を連続させる。
        // 粗さはフィルタリングに影響し、スネルの法則による方向自体は変えない。
        coneAngle = max(coneAngle, pathRoughness * pathRoughness * 0.01f);
        float epsilon = GlassRayEpsilon(hit.Position);
        origin = hit.Position + Ng * (tir ? epsilon : -epsilon) + direction * (epsilon * 0.25f);
        if (count > 0)
        {
            throughput *= exp(-min(mediumSigma[count - 1] * epsilon, 80.0f));
        }
        if (!TraceGlassBoundary(origin, direction, coneAngle, traveled, hit))
        {
            if (count == 0)
            {
                radiance += throughput * SampleGlassVisibleBackground(origin, direction, pathRoughness);
            }
            return true;
        }
    }
    return true; // 界面数に上限を設け、減衰していない背景が漏れるのを防ぐ
}

// 固定のサブピクセルサンプルを使い、フレームごとのランダムなジッターや古い履歴は使わない。
// 低い内部解像度では 4 経路、大きな描画先では 1 経路を維持する。
bool TraceGeometryGlass(float3 rasterPosition, float3 viewDir, float3 rayDx, float3 rayDy,
                        float3 originDx, float3 originDy, out float3 radiance,
                        out bool ownsPixel)
{
    radiance = 0;
    ownsPixel = true;
    if (QualityFlags.w < 0.5f || SceneRayFlags.z < 0.5f || SceneRayFlags.w < 0.5f)
    {
        return false;
    }
    float3 direction = -viewDir;
    float3 origin = SurfaceCameraOriginCommon(rasterPosition, CameraPos);
    float rasterDistance = length(rasterPosition - origin);
    float coneAngle = max(max(length(rayDx), length(rayDy)),
                          max(length(originDx), length(originDy)) / max(rasterDistance, 0.001f));
    coneAngle = max(coneAngle, 1e-6f);
    GlassHit center;
    if (!TraceGlassBoundary(origin, direction, coneAngle, 0.0f, center))
    {
        return false;
    }
    GlassMeshData first = GlassMeshes[center.Instance];
    if (first.Optical.y <= 0.001f)
    {
        return false;
    }
    // 固定のワールド空間しきい値の代わりに、ピクセルの投影範囲の小さな割合を使い、
    // 曲面メッシュで RT とフォールバックの担当が交互に変わるのを防ぐ。
    float tolerance = max(0.0001f, rasterDistance * coneAngle * 0.05f);
    if (center.Distance < rasterDistance - tolerance)
    {
        ownsPixel = false;
        return true;
    }
    if (abs(center.Distance - rasterDistance) > tolerance)
    {
        return false;
    }
    float3 centerRadiance;
    IntegrateGeometryGlass(center, origin, direction, coneAngle, centerRadiance);
    radiance = centerRadiance;
    uint width, height;
    SceneColorTexture.GetDimensions(width, height);
    uint sampleCount = width < 960u ? 4u : 1u;
    const float2 offsets[3] = {float2(-0.28f, -0.16f), float2(0.28f, -0.16f), float2(0.0f, 0.32f)};
    [loop] for (uint sampleIndex = 1; sampleIndex < sampleCount; ++sampleIndex)
    {
        float2 offset = offsets[sampleIndex - 1];
        float3 sampleDirection = SafeNormalizeCommon(direction + rayDx * offset.x + rayDy * offset.y, direction);
        float3 sampleOrigin = origin + originDx * offset.x + originDy * offset.y;
        GlassHit sampleHit;
        float3 sampleRadiance = centerRadiance;
        if (TraceGlassBoundary(sampleOrigin, sampleDirection, coneAngle, 0.0f, sampleHit))
        {
            GlassMeshData sampleMesh = GlassMeshes[sampleHit.Instance];
            // 隣接する不透明な物体をガラスの輪郭へにじませない。
            if (sampleMesh.Optical.y > 0.001f && sampleMesh.Flags.x == first.Flags.x)
            {
                IntegrateGeometryGlass(sampleHit, sampleOrigin, sampleDirection, coneAngle, sampleRadiance);
            }
        }
        radiance += sampleRadiance;
    }
    radiance /= sampleCount;
    return true;
}
#endif
#endif
