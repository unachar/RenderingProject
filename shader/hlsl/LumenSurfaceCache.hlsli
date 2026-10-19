#include "../../source/rendering/lumencachemath.h"
cbuffer LumenParameters : register(b7)
{
    uint4 LumenCounts;
    uint4 LumenGrid;
    float4 LumenSettings;
};
StructuredBuffer<uint> LumenMeshSlots : register(t0, space11);
StructuredBuffer<uint4> LumenWork : register(t1, space11);
RWTexture2D<float4> LumenPosition : register(u0, space11);
RWTexture2D<float4> LumenNormal : register(u1, space11);
RWTexture2D<float4> LumenDiffuse : register(u2, space11);
RWTexture2D<float4> LumenEmission : register(u3, space11);
RWTexture2D<float4> LumenTotalRead : register(u4, space11);
RWTexture2D<float4> LumenTotalWrite : register(u5, space11);
RWTexture2D<float4> LumenIndirectRead : register(u6, space11);
RWTexture2D<float4> LumenIndirectWrite : register(u7, space11);
struct LumenBounds { float4 Minimum; float4 Maximum; };
RWStructuredBuffer<LumenBounds> LumenMeshBounds : register(u8, space11);
RWStructuredBuffer<float4> LumenProbeSignal : register(u9, space11);
struct LumenProbe { float4 Position; float4 Normal; };
RWStructuredBuffer<LumenProbe> LumenProbeGuide : register(u10, space11);

bool LumenLookupMaterial(uint instance, float3 position, float3 normal, out float3 total, out float3 indirect,
                         out float3 albedo, out float3 emission)
{
    total = indirect = 0;
    albedo = emission = 0;
    if (LumenCounts.x == 0) return false;
    uint slot = LumenMeshSlots[instance];
    if (slot >= LumenGrid.w) return false;
    LumenBounds bounds = LumenMeshBounds[slot];
    float3 extent = max(bounds.Maximum.xyz - bounds.Minimum.xyz, 0.001f);
    float3 a = abs(normal);
    uint axis = a.x > a.y && a.x > a.z ? 0 : a.y > a.z ? 1 : 2;
    uint face = axis * 2 + (normal[axis] < 0 ? 1 : 0);
    uint u = (axis + 1) % 3, v = (axis + 2) % 3;
    float3 relative = position - bounds.Minimum.xyz;
    float2 uv = float2(relative[u] / extent[u], relative[v] / extent[v]);
    if (any(uv < 0) || any(uv > 1)) return false;
    uint2 texel = min(uint2(uv * 16), 15u) + uint2(face * 16, slot * 16);
    float4 surface = LumenPosition[texel];
    if (surface.w == 0) return false;
    float3 cachedNormal = LumenNormal[texel].xyz;
    float3 delta = surface.xyz - position;
    float plane = abs(dot(delta, normal));
    float tangent = length(delta - normal * dot(delta, normal));
    float texelSize = length(float2(extent[u], extent[v])) / 16;
    if (!AcceptSurfaceSample(plane, tangent, dot(normal, cachedNormal), texelSize)) return false;
    float4 lighting = LumenTotalRead[texel];
    if (lighting.w < 1 || !all(isfinite(lighting))) return false;
    total = max(lighting.rgb, 0);
    indirect = max(LumenIndirectRead[texel].rgb, 0);
    albedo = LumenDiffuse[texel].rgb;
    emission = LumenEmission[texel].rgb;
    return true;
}
bool LumenLookup(uint instance, float3 position, float3 normal, out float3 total, out float3 indirect)
{
    float3 albedo, emission;
    return LumenLookupMaterial(instance, position, normal, total, indirect, albedo, emission);
}
bool LumenGatherIrradiance(float3 position, float3 normal, uint2 pixel, out float3 irradiance, out float distance);
