// SHaRC backend for the renderer's material-independent diffuse irradiance cache.
// Store E / pi, then restore E at lookup. Direct surface lighting remains in the
// existing hit shader, so cached direct light is never added a second time.
#define SHARC_UPDATE 1
#define SHARC_QUERY 1
#define SHARC_ENABLE_CACHE_RESAMPLING 0
#define HASH_GRID_ENABLE_64_BIT_ATOMICS 0
#include "../../External/RTXGI/SHaRC/include/SharcCommon.h"

RWStructuredBuffer<uint64_t> SharcKeys : register(u0, space7);
RWStructuredBuffer<SharcAccumulationData> SharcAccumulation : register(u1, space7);
RWStructuredBuffer<SharcPackedData> SharcResolved : register(u2, space7);
RWStructuredBuffer<uint> SharcLocks : register(u3, space7);

SharcParameters GetSharcParameters()
{
    SharcParameters parameters;
    parameters.hashGridParameters.cameraPosition = ReflectionCamera.xyz;
    parameters.hashGridParameters.logarithmBase = SHARC_GRID_LOGARITHM_BASE;
    // Match the existing cache's minimum cell size and distance growth at 8 m.
    parameters.hashGridParameters.sceneScale = 4.0f / max(CacheParameters.y, 0.05f);
    parameters.hashGridParameters.levelBias = -1.0f;
    parameters.hashGridData.capacity = (uint)CacheParameters.z;
    parameters.hashGridData.hashEntriesBuffer = SharcKeys;
    parameters.hashGridData.lockBuffer = SharcLocks;
    parameters.accumulationBuffer = SharcAccumulation;
    parameters.resolvedBuffer = SharcResolved;
    parameters.radianceScale = 256.0f;
    return parameters;
}

bool CacheLookupSignal(float3 position, float3 normal, out float3 irradiance, out float hitDistance)
{
    // SHaRC has no hit-distance channel. Never use it to skip a primary GI ray
    // feeding NRD, which needs the actual secondary hit distance.
    irradiance = 0;
    hitDistance = 0;
    return false;
}

bool CacheLookup(float3 position, float3 normal, out float3 irradiance)
{
    irradiance = 0;
    if (CacheParameters.x < 0.5f)
    {
        return false;
    }
    SharcHitData hit = (SharcHitData)0;
    hit.positionWorld = position;
    hit.normalWorld = normal;
    float3 radiance;
    bool found = SharcGetCachedRadiance(GetSharcParameters(), hit, radiance, false);
    irradiance = radiance * 3.14159265f;
    return found && all(isfinite(irradiance));
}

void CacheInsert(float3 position, float3 normal, float3 irradiance, float hitDistance)
{
    if (CacheParameters.x < 0.5f || !all(isfinite(irradiance)))
    {
        return;
    }
    // The SDK recommends sparse updates. Hash world positions and frame index
    // to distribute 1/16 of the existing samples across both space and time.
    // Queries and the actual GI rays still run at the full traced resolution.
    uint sampleSeed = ReflectionHash(asuint(position.x) ^ ReflectionHash(asuint(position.y)) ^
                                     ReflectionHash(asuint(position.z)) ^ (ReflectionExtent.z * 0x9e3779b9u));
    if ((sampleSeed & 15u) != 0u)
    {
        return;
    }
    SharcState state;
    SharcInit(state);
    SharcHitData hit = (SharcHitData)0;
    hit.positionWorld = position;
    hit.normalWorld = normal;
    // The existing trace already evaluated this irradiance sample. Fuse the
    // SDK update into that pass; Resolve runs after all updates complete.
    SharcUpdateHit(GetSharcParameters(), state, hit, clamp(irradiance, 0, 256) / 3.14159265f, 0.5f);
}

[numthreads(64, 1, 1)]
void ResolveRadianceCache(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)CacheParameters.z)
    {
        return;
    }
    SharcResolveParameters resolve;
    resolve.cameraPositionPrev = SharcPreviousCamera.xyz;
    resolve.accumulationFrameNum = (uint)max(CacheParameters.w, 1);
    resolve.responsiveFrameNum = 1;
    // Sparse cells may receive only one update in sixteen frames. Keep them
    // long enough to avoid expiring visible cells between accepted samples.
    resolve.staleFrameNumMax = 64;
    resolve.frameIndex = ReflectionExtent.z;
    SharcResolveEntry(id.x, GetSharcParameters(), resolve);
}

[numthreads(64, 1, 1)]
void ClearRadianceCache(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)CacheParameters.z)
    {
        return;
    }
    SharcKeys[id.x] = 0;
    SharcAccumulation[id.x] = (SharcAccumulationData)0;
    SharcResolved[id.x] = (SharcPackedData)0;
    SharcLocks[id.x] = 0;
}
