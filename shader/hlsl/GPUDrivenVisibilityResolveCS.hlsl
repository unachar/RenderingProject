#define SHADER_GPU_DRIVEN
#define SHADER_VISIBILITY_RESOLVE
#define POM_LIGHT_CBV
#define main EvaluateMaterial
#include "DeferredPS.hlsl"
#undef main
ByteAddressBuffer Vertices : register(t33);
ByteAddressBuffer Indices : register(t34);
StructuredBuffer<uint4> Geometry : register(t35);
Texture2D<uint4> Visibility : register(t36);
Texture2D<float> HardwareDepth : register(t45);
RWTexture2D<float4> OutColor : register(u0);
RWTexture2D<float4> OutNormal : register(u1);
RWTexture2D<float> OutDepth : register(u2);
RWTexture2D<float4> OutMaterial : register(u3);
RWTexture2D<float4> OutShadow : register(u4);
RWTexture2D<float4> OutEmission : register(u5);

struct VertexData
{
    float3 P, N;
    float2 U;
    float4 C;
};

VertexData Vertex(uint index)
{
    uint a = index * 48u;
    VertexData v;
    v.P = asfloat(Vertices.Load3(a));
    v.N = asfloat(Vertices.Load3(a + 12));
    v.U = asfloat(Vertices.Load2(a + 24));
    v.C = asfloat(Vertices.Load4(a + 32));
    return v;
}

float Cross2(float2 a, float2 b)
{
    return a.x * b.y - a.y * b.x;
}

bool Reconstruct(uint4 id, float2 pixel, uint2 extent, out PSInput3D input)
{
    input = (PSInput3D)0;
    uint4 geometry = Geometry[id.y];
    if (geometry.w == 0 || id.z >= geometry.y / 3u)
    {
        return false;
    }
    uint3 index = Indices.Load3((geometry.x + id.z * 3u) * 4u) + geometry.zzz;
    VertexData a = Vertex(index.x), b = Vertex(index.y), c = Vertex(index.z);
    GPUInstanceData instance = gSceneInstances[id.x];
    float4 wa = mul(float4(a.P, 1), instance.World);
    float4 wb = mul(float4(b.P, 1), instance.World);
    float4 wc = mul(float4(c.P, 1), instance.World);
    float4 ca = mul(mul(wa, View), Projection);
    float4 cb = mul(mul(wb, View), Projection);
    float4 cc = mul(mul(wc, View), Projection);
    if (min(abs(ca.w), min(abs(cb.w), abs(cc.w))) < 1e-7)
    {
        return false;
    }
    float2 pa = ca.xy / ca.w;
    float2 pb = cb.xy / cb.w;
    float2 pc = cc.xy / cc.w;
    float2 p = pixel / float2(extent) * float2(2, -2) + float2(-1, 1);
    float determinant = Cross2(pb - pa, pc - pa);
    if (abs(determinant) < 1e-12)
    {
        return false;
    }
    float3 bary;
    bary.y = Cross2(p - pa, pc - pa) / determinant;
    bary.z = Cross2(pb - pa, p - pa) / determinant;
    bary.x = 1 - bary.y - bary.z;
    float depth = dot(bary, float3(ca.z / ca.w, cb.z / cb.w, cc.z / cc.w));
    float3 weights = bary / float3(ca.w, cb.w, cc.w);
    float sum = weights.x + weights.y + weights.z;
    if (abs(sum) < 1e-9)
    {
        return false;
    }
    weights /= sum;
    input.Position = float4(pixel, saturate(depth), 1);
    input.WorldPos = wa.xyz * weights.x + wb.xyz * weights.y + wc.xyz * weights.z;
    input.ViewPos = mul(float4(input.WorldPos, 1), View).xyz;
    input.Normal = normalize(mul(a.N * weights.x + b.N * weights.y + c.N * weights.z, (float3x3)instance.World));
    input.TexCoord = a.U * weights.x + b.U * weights.y + c.U * weights.z;
    input.Diffuse = a.C * weights.x + b.C * weights.y + c.C * weights.z;
    input.CameraPos = CameraPos;
    input.InstanceID = id.x;
    return all(isfinite(input.WorldPos)) && all(isfinite(input.TexCoord));
}

GBufferOutput Shade(PSInput3D input, uint4 id, uint2 extent)
{
    PSInput3D x, y;
    bool vx = Reconstruct(id, input.Position.xy + float2(1, 0), extent, x);
    bool vy = Reconstruct(id, input.Position.xy + float2(0, 1), extent, y);
    VisibilityUvDx = vx ? x.TexCoord - input.TexCoord : 0;
    VisibilityUvDy = vy ? y.TexCoord - input.TexCoord : 0;
    VisibilityWorldDx = vx ? x.WorldPos - input.WorldPos : 0;
    VisibilityWorldDy = vy ? y.WorldPos - input.WorldPos : 0;
    return EvaluateMaterial(input, (id.w & 1u) != 0);
}

void Store(uint2 p, GBufferOutput v, float depth)
{
    OutColor[p] = v.Color;
    OutNormal[p] = v.Normal;
    OutDepth[p] = depth;
    OutMaterial[p] = v.Material;
    OutShadow[p] = v.Shadow;
    OutEmission[p] = v.EmissionAO;
}

float2 UnpackUnorm16x2(uint value)
{
    return float2(value & 0xffffu, value >> 16u) / 65535.0f;
}

float3 DecodeOctahedron(float2 encoded)
{
    float2 f = encoded * 2.0f - 1.0f;
    float3 normal = float3(f, 1.0f - abs(f.x) - abs(f.y));
    if (normal.z < 0.0f)
    {
        float2 direction = float2(normal.x >= 0.0f ? 1.0f : -1.0f, normal.y >= 0.0f ? 1.0f : -1.0f);
        normal.xy = (1.0f - abs(normal.yx)) * direction;
    }
    return normalize(normal);
}

void ResolvePacked(uint2 pixel)
{

    OutEmission[pixel] = float4(0, 0, 0, 1);
    uint4 payload = Visibility.Load(int3(pixel, 0));
    if ((payload.w & 0x80000000u) == 0u)
    {
        OutColor[pixel] = float4(0.1f, 0.2f, 0.4f, 1.0f);
        OutNormal[pixel] = float4(0.5f, 1.0f, 0.5f, 1.0f);
        OutDepth[pixel] = 0.0f;
        OutMaterial[pixel] = float4(0.0f, 0.0f, 0.0f, -1.0f);
        OutShadow[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    float4 packedColor =
        float4(payload.x & 0xffu, (payload.x >> 8u) & 0xffu, (payload.x >> 16u) & 0xffu, (payload.x >> 24u) & 0xffu) /
        255.0f;
    float3 normal = DecodeOctahedron(UnpackUnorm16x2(payload.y));
    float depth = HardwareDepth.Load(int3(pixel, 0));
    float metallic = ((payload.z >> 16u) & 0xffu) / 255.0f;
    float roughness = ((payload.z >> 24u) & 0xffu) / 255.0f;
    float fresnel = (payload.w & 0xffu) / 255.0f;
    float materialPart = (float)((payload.w >> 8u) & 0xffu);
    float shadowThreshold = ((payload.w >> 16u) & 0xffu) / 255.0f;
    float shadowSoftness = ((payload.w >> 24u) & 0x7fu) / 127.0f;

    OutColor[pixel] = float4(packedColor.rgb, 1.0f);
    OutNormal[pixel] = float4(normal * 0.5f + 0.5f, 1.0f);
    OutDepth[pixel] = depth;
    OutMaterial[pixel] = float4(metallic, roughness, fresnel, materialPart);
    OutShadow[pixel] = float4(shadowThreshold, shadowSoftness, packedColor.a, 1.0f);
}

[numthreads(8, 8, 1)] void main(uint3 thread : SV_DispatchThreadID) {
    uint w, h;
    Visibility.GetDimensions(w, h);
    uint2 extent = uint2(w, h);
    uint2 base = thread.xy * 2;
    if (any(base >= extent))
    {
        return;
    }
    uint4 ids[4];
    PSInput3D inputs[4];
    bool valid[4];
    bool share = MaterialRate != 0;
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        uint2 p = base + uint2(i & 1u, i >> 1u);
        ids[i] = 0;
        inputs[i] = (PSInput3D)0;
        valid[i] = false;
        if (all(p < extent))
        {
            ids[i] = Visibility.Load(int3(p, 0));
            if ((ids[i].w & 0xc0000000u) == 0x40000000u)
            {
                valid[i] = Reconstruct(ids[i], float2(p) + .5, extent, inputs[i]);
            }
        }
        share = share && valid[i];
        if (i > 0)
        {
            share = share && all(ids[i] == ids[0]) && dot(inputs[i].Normal, inputs[0].Normal) > .9999 &&
                    all(abs(inputs[i].Diffuse - inputs[0].Diffuse) < .001) &&
                    abs(inputs[i].Position.z - inputs[0].Position.z) < 2e-5;
        }
    }
    if (share)
    {
        uint material = gSceneInstances[ids[0].x].MaterialIndex;
        uint4 metadata = gSceneMaterials.Load4(material * GPU_SCENE_MATERIAL_STRIDE + 32u);
        AssetMaterialParameters asset = LoadAssetMaterial(material);
        // POM はピクセルごとに UV・深度・影が変わるので、2x2 共有シェーディングから外す。
        share = asset.Noise[0].Width == 0u && asset.Noise[1].Width == 0u && asset.Noise[2].Width == 0u &&
                (metadata.z & 1u) == 0 && metadata.y == 0xffffffffu &&
                (asset.Surface.z < .5 || (all(asset.Textures.xyz >= 512u) &&
                                          (asset.Parallax.x <= 0.0f || asset.Parallax.z < 0.0f)));
        GPUInstanceData instance = gSceneInstances[ids[0].x];
        [unroll] for (uint row = 0; row < 4; ++row)
        {
            share = share && all(abs(instance.World[row] - instance.PreviousWorld[row]) < 1e-6);
        }
    }
    GBufferOutput sharedValue = (GBufferOutput)0;
    // One shading call site prevents the full procedural evaluator being inlined five times.
    // Shared quads evaluate their first pixel once; other quads evaluate every valid pixel.
    [loop] for (uint i = 0; i < 4; ++i)
    {
        uint2 p = base + uint2(i & 1u, i >> 1u);
        if (any(p >= extent))
        {
            continue;
        }
        if (valid[i])
        {
            if (!share || i == 0u)
            {
                sharedValue = Shade(inputs[i], ids[i], extent);
            }
            GBufferOutput value = sharedValue;
            // 非共有時は POM のピクセル深度オフセット込みの深度を書く。
            Store(p, value, share ? inputs[i].Position.z : value.Depth.x);
        }
        else
        {
            ResolvePacked(p);
        }
    }
}

