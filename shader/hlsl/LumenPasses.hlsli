// Six axis-aligned cards per resident mesh, each with 16 x 16 texels.
groupshared float3 LumenMinimum[64];
groupshared float3 LumenMaximum[64];
[numthreads(64,1,1)]
void LumenBuildBounds(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint4 work = LumenWork[group.x];
    ReflectionGeometry mesh = ReflectionMeshes[work.x];
    float3 minimum = 1e30f, maximum = -1e30f;
    for (uint vertex = lane; vertex < work.z; vertex += 64)
    {
        uint offset = mesh.Layout.z + vertex * mesh.Layout.x;
        float3 position = asfloat(SceneVertices[NonUniformResourceIndex(work.x)].Load3(offset));
        position = mul(float4(position, 1), mesh.World).xyz;
        minimum = min(minimum, position);
        maximum = max(maximum, position);
    }
    LumenMinimum[lane] = minimum;
    LumenMaximum[lane] = maximum;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 32; stride > 0; stride >>= 1)
    {
        if (lane < stride)
        {
            LumenMinimum[lane] = min(LumenMinimum[lane], LumenMinimum[lane + stride]);
            LumenMaximum[lane] = max(LumenMaximum[lane], LumenMaximum[lane + stride]);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0)
    {
        LumenBounds bounds;
        bounds.Minimum = float4(LumenMinimum[0] - 0.002f, 0);
        bounds.Maximum = float4(LumenMaximum[0] + 0.002f, 0);
        LumenMeshBounds[work.y] = bounds;
    }
}

[numthreads(8,8,1)]
void LumenClear(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= 96 || id.y >= LumenGrid.w * 16) return;
    LumenTotalRead[id.xy] = 0;
    LumenTotalWrite[id.xy] = 0;
    LumenIndirectRead[id.xy] = 0;
    LumenIndirectWrite[id.xy] = 0;
}

[numthreads(8,8,1)]
void LumenCapture(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= 16) || id.z >= LumenCounts.y * 6) return;
    uint4 work = LumenWork[id.z / 6];
    uint face = id.z % 6, axis = face / 2, u = (axis + 1) % 3, v = (axis + 2) % 3;
    uint2 texel = id.xy + uint2(face * 16, work.y * 16);
    LumenPosition[texel] = 0;
    LumenNormal[texel] = 0;
    LumenDiffuse[texel] = 0;
    LumenEmission[texel] = 0;
    LumenTotalRead[texel] = LumenTotalWrite[texel] = 0;
    LumenIndirectRead[texel] = LumenIndirectWrite[texel] = 0;
    LumenBounds bounds = LumenMeshBounds[work.y];
    float3 extent = bounds.Maximum.xyz - bounds.Minimum.xyz;
    float3 origin = (bounds.Minimum.xyz + bounds.Maximum.xyz) * 0.5f;
    float2 uv = (id.xy + 0.5f) / 16;
    origin[u] = bounds.Minimum[u] + uv.x * extent[u];
    origin[v] = bounds.Minimum[v] + uv.y * extent[v];
    origin[axis] = (face & 1) == 0 ? bounds.Maximum[axis] + 0.01f : bounds.Minimum[axis] - 0.01f;
    float3 direction = 0;
    direction[axis] = (face & 1) == 0 ? -1 : 1;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0;
    ray.TMax = extent[axis] + 0.02f;
    // Ignore other instances during material capture, including meshes surrounding this one.
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> query;
    CountTelemetryRay(RAY_TELEMETRY_GI);
    query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0x03, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE && query.CandidateInstanceID() == work.x)
            query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() == COMMITTED_NOTHING) return;
    uint primitive = query.CommittedPrimitiveIndex();
    float2 bary = query.CommittedTriangleBarycentrics();
    ReflectionGeometry mesh = ReflectionMeshes[work.x];
    float3 weights = float3(1 - bary.x - bary.y, bary);
    float3 normal = 0, albedo = 0;
    float2 materialUv = 0;
    for (uint corner = 0; corner < 3; ++corner)
    {
        uint vertex = LoadReflectionIndex(work.x, primitive * 3 + corner, mesh);
        uint offset = mesh.Layout.z + vertex * mesh.Layout.x;
        normal += asfloat(SceneVertices[NonUniformResourceIndex(work.x)].Load3(offset + 12)) * weights[corner];
        materialUv += asfloat(SceneVertices[NonUniformResourceIndex(work.x)].Load2(offset + 24)) * weights[corner];
        albedo += asfloat(SceneVertices[NonUniformResourceIndex(work.x)].Load3(offset + 32)) * weights[corner];
    }
    normal = normalize(mul(float4(normal, 0), mesh.NormalToWorld).xyz);
    if (dot(normal, direction) > 0) normal = -normal;
    if ((mesh.Surface.w & 1) != 0) albedo = 1;
    if (mesh.Material.z > 0.5f)
        albedo *= SceneTextures[NonUniformResourceIndex(work.x)].SampleLevel(MaterialSampler, materialUv, 0).rgb;
    // Bound feedback energy for authored colors outside the physical reflectance range.
    float3 reflectance = saturate(albedo * mesh.BaseColor.rgb);
    LumenPosition[texel] = float4(origin + direction * query.CommittedRayT(), 1);
    LumenNormal[texel] = float4(normal, 1);
    LumenDiffuse[texel] = float4(reflectance, 1);
    LumenEmission[texel] = float4(SampleEmission(work.x, materialUv), 1);
}

float3 LumenCosineDirection(float3 normal, uint seed)
{
    float2 random = float2((seed & 65535) + 0.5f, (seed >> 16) + 0.5f) / 65536;
    float radius = sqrt(random.x), angle = 6.2831853f * random.y;
    float3 tangent = normalize(cross(abs(normal.y) < 0.95f ? float3(0,1,0) : float3(1,0,0), normal));
    return tangent * (radius * cos(angle)) + cross(normal, tangent) * (radius * sin(angle)) + normal * sqrt(1 - random.x);
}
// ---------------------------------------------------------------------------------------------
// World-space radiance cache (Lumen mode). Shares the GI hash grid storage but uses a salted key
// namespace, so it never mixes with the irradiance entries of the DXR GI fallback path.
// Stores outgoing diffuse radiance at ray hit points; a probe / surface-cache ray that hits a cached
// cell skips material evaluation, the light loop and the bounce shadow ray.
static const uint LUMEN_WORLD_SALT = 0xA511E9B3u;
static const uint LUMEN_WORLD_REFRESH = 8; // 1 in N cached hits still shades and refreshes the cell

void LumenWorldCell(float3 position, float3 normal, out uint key, out uint bucket)
{
    CacheCell(position, normal, key, bucket);
    key = CacheHash(key ^ LUMEN_WORLD_SALT) | 1u;
    bucket = CacheHash(key ^ 0x9e3779b9u) & ((uint)CacheParameters.z - 1u);
}

float LumenWorldCellSize(float3 position)
{
    float distance = length(position - ReflectionCamera.xyz);
    float level = clamp(floor(log2(max(distance, 1.0f) / 8.0f)) + 1.0f, 0.0f, 12.0f);
    return CacheParameters.y * exp2(level);
}

bool LumenWorldLookup(float3 position, float3 normal, out float3 radiance)
{
    radiance = 0;
    if (CacheParameters.x < 0.5f) return false;
    uint key, bucket;
    LumenWorldCell(position, normal, key, bucket);
    uint mask = (uint)CacheParameters.z - 1u;
    [loop] for (uint probe = 0; probe < kCacheProbes; ++probe)
    {
        uint slot = (bucket + probe) & mask;
        uint stored = CacheKeys.Load(slot * 4);
        if (stored == key)
        {
            float4 resolved = CacheResolved[slot].Signal;
            radiance = resolved.rgb;
            // Require a few accumulated frames so single noisy samples are never reused.
            return resolved.w >= 2 && CacheAges.Load(slot * 4) <= 8u && all(isfinite(resolved));
        }
        if (stored == 0) return false;
    }
    return false;
}

void LumenWorldInsert(float3 position, float3 normal, float3 radiance)
{
    if (CacheParameters.x < 0.5f || any(!isfinite(radiance))) return;
    uint key, bucket;
    LumenWorldCell(position, normal, key, bucket);
    uint mask = (uint)CacheParameters.z - 1u;
    int found = -1;
    [loop] for (uint probe = 0; probe < kCacheProbes && found < 0; ++probe)
    {
        uint slot = (bucket + probe) & mask;
        uint previous;
        CacheKeys.InterlockedCompareExchange(slot * 4, 0u, key, previous);
        if (previous == 0u || previous == key) found = (int)slot;
    }
    if (found < 0) return;
    uint address = (uint)found * 32;
    uint3 value = (uint3)(min(max(radiance, 0), kCacheMaxIrradiance) * kCacheScale + 0.5f);
    uint ignored;
    CacheAccumulation.InterlockedAdd(address + 0, value.x, ignored);
    CacheAccumulation.InterlockedAdd(address + 4, value.y, ignored);
    CacheAccumulation.InterlockedAdd(address + 8, value.z, ignored);
    CacheAccumulation.InterlockedAdd(address + 12, 1u, ignored);
}

// Geometric normal of the hit triangle (positions only), facing the ray origin.
float3 LumenHitNormal(uint instance, uint primitive, float3 direction)
{
    ReflectionGeometry mesh = ReflectionMeshes[instance];
    float3 corners[3];
    [unroll] for (uint corner = 0; corner < 3; ++corner)
    {
        uint vertex = LoadReflectionIndex(instance, primitive * 3 + corner, mesh);
        corners[corner] = asfloat(SceneVertices[NonUniformResourceIndex(instance)].Load3(mesh.Layout.z + vertex * mesh.Layout.x));
    }
    float3 normal = cross(corners[1] - corners[0], corners[2] - corners[0]);
    normal = mul(float4(normal, 0), mesh.NormalToWorld).xyz;
    normal *= rsqrt(max(dot(normal, normal), 1e-20f));
    return dot(normal, direction) > 0 ? -normal : normal;
}

float3 LumenIncoming(float3 position, float3 normal, uint seed, out float hitDistance)
{
    float3 direction = LumenCosineDirection(normal, seed);
    RayDesc ray;
    ray.Origin = position + normal * SceneRayParameters.y;
    ray.Direction = direction;
    ray.TMin = SceneRayParameters.y * 0.25f;
    ray.TMax = GiParameters.x;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    CountTelemetryRay(RAY_TELEMETRY_GI);
    query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0x03, ray);
    while (query.Proceed()) { }
    hitDistance = ray.TMax;
    if (query.CommittedStatus() == COMMITTED_NOTHING)
        return EvaluatePhysicalSkyFast(direction, 0.01f, false);
    hitDistance = query.CommittedRayT();
    uint instance = query.CommittedInstanceID(), primitive = query.CommittedPrimitiveIndex();
    float3 hitPosition = ray.Origin + direction * hitDistance;
    // Near hits keep full shading: a cell larger than the ray would blur contact lighting and leak.
    bool useWorld = CacheParameters.x > 0.5f && hitDistance > LumenWorldCellSize(hitPosition) * 2;
    float3 hitNormal = 0;
    if (useWorld)
    {
        hitNormal = LumenHitNormal(instance, primitive, direction);
        float3 cached;
        bool refresh = (ReflectionHash(seed ^ 0x27d4eb2du) % LUMEN_WORLD_REFRESH) == 0;
        if (!refresh && LumenWorldLookup(hitPosition, hitNormal, cached)) return cached;
    }
    float3 radiance = ShadeReflectionHit(instance, primitive, query.CommittedTriangleBarycentrics(), hitPosition,
                                         direction, true, seed);
    if (useWorld) LumenWorldInsert(hitPosition, hitNormal, radiance);
    return radiance;
}

// Stochastic light subset: each light enters the candidate set with probability M/N (random
// start, fixed stride), then one candidate is chosen by weighted reservoir sampling. Scaling by
// N/M keeps the estimator unbiased while bounding per-texel cost to 32 light evaluations
// instead of O(light count) (2000 lights in the night city scene).
static const uint LUMEN_LIGHT_CANDIDATES = 32;
float3 LumenDirect(float3 position, float3 normal, uint seed)
{
    uint lightCount = ReflectionExtent.w;
    if (lightCount == 0) return 0;
    uint candidates = min(lightCount, LUMEN_LIGHT_CANDIDATES);
    uint stride = max(lightCount / candidates, 1u);
    uint start = ReflectionHash(seed * 747796405u + 2891336453u) % lightCount;
    float3 incident = 0, selectedDirection = 0;
    float sum = 0, selectedWeight = 0, selectedRange = 0;
    bool selectedShadow = false;
    float random = ((ReflectionHash(seed) >> 8) + 0.5f) / 16777216.0f;
    for (uint k = 0; k < candidates; ++k)
    {
        uint i = (start + k * stride) % lightCount;
        ReflectionLight light = ReflectionLights[i];
        float3 direction;
        float attenuation, volume;
        ResolveSingleLightCommon(position, light.Direction, light.PositionType, light.Extras, direction, attenuation, volume);
        float3 contribution = light.Color.rgb * max(light.Color.a, 0) * saturate(dot(normal, direction)) * attenuation;
        float weight = dot(contribution, float3(0.2126f, 0.7152f, 0.0722f));
        if (weight <= 1e-7f) continue;
        sum += weight;
        float probability = weight / sum;
        if (random < probability)
        {
            random /= probability;
            incident = contribution;
            selectedWeight = weight;
            selectedDirection = direction;
            selectedRange = light.PositionType.w < 0.5f ? SceneRayParameters.x :
                min(length(light.PositionType.xyz - position), SceneRayParameters.x);
            selectedShadow = light.Flags.x != 0;
        }
        else random = (random - probability) / max(1 - probability, 1e-6f);
    }
    if (selectedWeight <= 0) return 0;
    float visibility = selectedShadow ? TraceBounceVisibility(position, normal, selectedDirection, selectedRange) : 1;
    return incident * (visibility * sum / selectedWeight * (float(lightCount) / float(candidates)));
}

[numthreads(8,8,1)]
void LumenUpdateLighting(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= 16) || id.z >= LumenCounts.z * 6) return;
    uint4 work = LumenWork[LumenCounts.y + id.z / 6];
    uint2 texel = id.xy + uint2((id.z % 6) * 16, work.y * 16);
    float4 surface = LumenPosition[texel];
    if (surface.w == 0) return;
    float3 normal = LumenNormal[texel].xyz;
    uint seed = ReflectionHash(texel.x + texel.y * 96 + ReflectionExtent.z * 104729u);
    float distance;
    float3 indirect = 0;
    if (LumenSettings.w > 0.5f)
    {
        indirect = LumenIncoming(surface.xyz, normal, seed, distance) * 3.14159265f +
            SampleEmissiveIrradiance(surface.xyz, normal, seed + 17, texel, false);
    }
    float3 total = indirect + LumenDirect(surface.xyz, normal, seed ^ 0x5bd1e995u);
    float4 previous = LumenTotalRead[texel];
    float count = min(previous.w + 1, LumenSettings.x);
    float3 oldIndirect = LumenIndirectRead[texel].rgb;
    // Independent read/write atlases prevent traversal order from changing the bounce solution.
    total = lerp(previous.rgb, min(max(total, 0), 4096), 1 / count);
    indirect = lerp(oldIndirect, min(max(indirect, 0), 4096), 1 / count);
    LumenTotalWrite[texel] = float4(total, count);
    LumenIndirectWrite[texel] = float4(indirect, count);
}

// 8 probes x 8 rays per 64-lane group. The former 8-lane groups left most of each wave idle.
// Probe record (LUMEN_PROBE_STRIDE float4, double buffered by LumenCounts.w bit 0):
//   [0] irradiance at the probe normal (rgb), hit distance
//   [1..3] per-channel irradiance gradient over normal tilt (xyz), [1].w = history length
#define LUMEN_PROBES_PER_GROUP 8
#define LUMEN_PROBE_STRIDE 4
uint LumenProbeCount() { return LumenGrid.x * LumenGrid.y; }
uint LumenProbeBase(bool previous) { return ((LumenCounts.w & 1u) ^ (previous ? 1u : 0u)) * LumenProbeCount(); }
groupshared float4 LumenProbeSamples[64];
groupshared float3 LumenProbeDirections[64];

// Reproject the probe's surface point into last frame's grid and validate against the stored guide.
bool LumenFindHistory(float3 position, float3 normal, out uint previousIndex)
{
    previousIndex = 0;
    if ((LumenCounts.w & 2u) == 0) return false;
    float4 clip = mul(float4(position, 1), AdaptivePreviousViewProjection);
    if (clip.w <= 1e-5f) return false;
    float2 ndc = clip.xy / clip.w;
    float2 pixel = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f) * ReflectionExtent.xy;
    if (any(pixel < 0) || any(pixel >= ReflectionExtent.xy)) return false;
    int2 probe = clamp(int2(floor(pixel / LumenGrid.z)), int2(0, 0), int2(LumenGrid.xy) - 1);
    previousIndex = probe.y * LumenGrid.x + probe.x;
    LumenProbe guide = LumenProbeGuide[LumenProbeBase(true) + previousIndex];
    if (guide.Position.w == 0 || dot(guide.Normal.xyz, normal) < 0.95f) return false;
    float3 delta = guide.Position.xyz - position;
    float tolerance = max(0.01f, guide.Normal.w * 0.01f);
    return abs(dot(delta, normal)) < tolerance && length(delta) < tolerance * 4 + LumenGrid.z * guide.Normal.w * 0.002f;
}

[numthreads(64,1,1)]
void LumenTraceProbes(uint3 group : SV_GroupID, uint threadIndex : SV_GroupIndex)
{
    uint lane = threadIndex & 7, local = threadIndex >> 3;
    uint index = group.x * LUMEN_PROBES_PER_GROUP + local;
    bool valid = index < LumenProbeCount();
    uint2 probe = valid ? uint2(index % LumenGrid.x, index / LumenGrid.x) : 0;
    uint2 pixel = min(probe * LumenGrid.z + LumenGrid.z / 2, ReflectionExtent.xy - 1);
    float depth = valid ? ReflectionDepth.Load(int3(pixel, 0)) : 1;
    float2 uv = (pixel + 0.5f) / ReflectionExtent.xy;
    float4 world = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1), ReflectionInvViewProjection);
    float3 normal = normalize(ReflectionNormal.Load(int3(pixel, 0)).xyz * 2 - 1);
    float3 position = world.xyz / world.w;
    bool sky = depth >= 0.99999f;
    uint current = LumenProbeBase(false);
    if (valid && lane == 0)
    {
        LumenProbe guide;
        guide.Position = float4(position, sky ? 0 : 1);
        guide.Normal = float4(normal, length(position - ReflectionCamera.xyz));
        LumenProbeGuide[current + index] = guide;
    }
    float4 signal = 0;
    float3 direction = normal;
    if (valid && !sky)
    {
        uint seed = ReflectionHash(index * 8 + lane + ReflectionExtent.z * 104729u);
        float distance;
        direction = LumenCosineDirection(normal, seed);
        float3 irradiance = LumenIncoming(position, normal, seed, distance) * 3.14159265f +
            SampleEmissiveIrradiance(position, normal, seed + 17, pixel + uint2(lane, 0), false);
        signal = float4(min(max(irradiance, 0), 65504), distance);
    }
    // Barrier stays in uniform control flow (all 64 lanes reach it).
    LumenProbeSamples[threadIndex] = signal;
    LumenProbeDirections[threadIndex] = direction;
    GroupMemoryBarrierWithGroupSync();
    if (!valid || lane != 0) return;
    uint record = (current + index) * LUMEN_PROBE_STRIDE;
    if (sky)
    {
        [unroll] for (uint i = 0; i < LUMEN_PROBE_STRIDE; ++i) LumenProbeSignal[record + i] = 0;
        return;
    }
    float4 average = 0;
    float3 gradient[3] = {float3(0, 0, 0), float3(0, 0, 0), float3(0, 0, 0)};
    [unroll] for (uint s = 0; s < 8; ++s) average += LumenProbeSamples[threadIndex + s] * 0.125f;
    [unroll] for (uint k = 0; k < 8; ++k)
    {
        // Mean-centred linear fit of irradiance vs. normal tilt (E[t t^T] = I/4 for cosine samples),
        // damped by 1/2 because 8 rays per frame give a noisy slope.
        float3 value = LumenProbeSamples[threadIndex + k].rgb - average.rgb;
        float3 d = LumenProbeDirections[threadIndex + k];
        float3 tangent = d - normal * dot(d, normal);
        gradient[0] += tangent * (value.r * 0.25f);
        gradient[1] += tangent * (value.g * 0.25f);
        gradient[2] += tangent * (value.b * 0.25f);
    }
    float history = 1;
    uint previous;
    if (LumenFindHistory(position, normal, previous))
    {
        uint old = (LumenProbeBase(true) + previous) * LUMEN_PROBE_STRIDE;
        float4 oldAverage = LumenProbeSignal[old];
        float oldCount = LumenProbeSignal[old + 1].w;
        float limit = min(LumenSettings.x, 8);
        history = min(oldCount + 1, limit);
        float alpha = 1 / history;
        // Clamp history luminance against the new sample to bound ghosting after lighting changes.
        float newLuma = dot(average.rgb, float3(0.2126f, 0.7152f, 0.0722f));
        float oldLuma = dot(oldAverage.rgb, float3(0.2126f, 0.7152f, 0.0722f));
        float clampScale = oldLuma > newLuma * 4 + 1e-3f ? (newLuma * 4 + 1e-3f) / oldLuma : 1;
        average = lerp(oldAverage * float4(clampScale.xxx, 1), average, alpha);
        [unroll] for (uint c = 0; c < 3; ++c)
            gradient[c] = lerp(LumenProbeSignal[old + 1 + c].xyz * clampScale, gradient[c], alpha);
    }
    LumenProbeSignal[record] = average;
    LumenProbeSignal[record + 1] = float4(gradient[0], history);
    LumenProbeSignal[record + 2] = float4(gradient[1], 0);
    LumenProbeSignal[record + 3] = float4(gradient[2], 0);
}

float3 LumenEvaluateProbe(uint record, float3 normal, float3 probeNormal)
{
    float3 tilt = normal - probeNormal;
    float3 base = LumenProbeSignal[record].rgb;
    float3 delta = float3(dot(LumenProbeSignal[record + 1].xyz, tilt), dot(LumenProbeSignal[record + 2].xyz, tilt),
                          dot(LumenProbeSignal[record + 3].xyz, tilt));
    return max(base + delta, base * 0.25f);
}

// Both endpoints are visible G-buffer samples, so the reconnection segment is tested against the
// depth buffer instead of a per-pixel TLAS ray. Costs 6 depth loads instead of up to 4 rays/pixel.
bool LumenProbeVisibleScreen(float3 position, float3 normal, float3 probePosition, float3 probeNormal)
{
    float3 origin = position + normal * SceneRayParameters.y;
    float3 target = probePosition + probeNormal * SceneRayParameters.y;
    [unroll] for (uint marchStep = 1; marchStep <= 6; ++marchStep)
    {
        float3 marchPoint = lerp(origin, target, marchStep / 7.0f);
        float4 clip = mul(float4(marchPoint, 1), ReflectionViewProjection);
        if (clip.w <= 1e-5f) return false;
        float3 ndc = clip.xyz / clip.w;
        float2 screen = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f) * ReflectionExtent.xy;
        if (any(screen < 0) || any(screen >= ReflectionExtent.xy)) return false;
        float scene = ReflectionDepth.Load(int3(screen, 0));
        if (scene < ndc.z - max(2e-5f, (1 - ndc.z) * 0.02f)) return false;
    }
    return true;
}

bool LumenProbeVisible(float3 position, float3 normal, float3 probePosition, float3 probeNormal)
{
    float3 origin = position + normal * SceneRayParameters.y;
    float3 delta = probePosition + probeNormal * SceneRayParameters.y - origin;
    float length = sqrt(dot(delta, delta));
    if (length <= SceneRayParameters.y * 2) return true;
    if (LumenSettings.z < 0.5f) return LumenProbeVisibleScreen(position, normal, probePosition, probeNormal);
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = delta / length;
    ray.TMin = SceneRayParameters.y * 0.25f;
    ray.TMax = length - SceneRayParameters.y * 0.5f;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    CountTelemetryRay(RAY_TELEMETRY_RADIANCE_SHADOW);
    query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0x03, ray);
    while (query.Proceed()) { }
    return query.CommittedStatus() == COMMITTED_NOTHING;
}

bool LumenGatherIrradiance(float3 position, float3 normal, uint2 pixel, out float3 irradiance, out float distance)
{
    irradiance = 0;
    distance = 0;
    if (LumenCounts.x == 0) return false;
    if (LumenSettings.y > 1.5f)
    {
        RayDesc ray;
        ray.Direction = normalize(position - ReflectionCamera.xyz);
        ray.Origin = ReflectionCamera.xyz;
        ray.TMin = 0;
        ray.TMax = length(position - ray.Origin) + 0.05f;
        RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
        query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0x03, ray);
        while (query.Proceed()) { }
        float3 total, indirect;
        bool covered = query.CommittedStatus() != COMMITTED_NOTHING &&
            LumenLookup(query.CommittedInstanceID(), position, normal, total, indirect);
        irradiance = covered ? float3(0, 1, 0) : float3(1, 0, 1);
        return true;
    }
    float2 gridPosition = (float2(pixel) + 0.5f - LumenGrid.z / 2) / LumenGrid.z;
    int2 base = int2(floor(gridPosition));
    int2 nearest = clamp(int2(floor(gridPosition + 0.5f)), int2(0, 0), int2(LumenGrid.xy) - 1);
    float totalWeight = 0;
    for (uint i = 0; i < 4; ++i)
    {
        int2 probe = base + int2(i & 1, i >> 1);
        if (any(probe < 0) || any(probe >= int2(LumenGrid.xy))) continue;
        // Coarse probe settings trade interpolation quality for one connection instead of four.
        if (LumenGrid.z >= 12 && any(probe != nearest)) continue;
        uint index = LumenProbeBase(false) + probe.y * LumenGrid.x + probe.x;
        LumenProbe guide = LumenProbeGuide[index];
        // Directional probes extrapolate across small normal changes, so curved surfaces no longer
        // fall back to the full DXR GI path. Weight still favours matching normals.
        float normalAgreement = dot(normal, guide.Normal.xyz);
        if (guide.Position.w == 0 || normalAgreement < 0.9f) continue;
        float3 delta = guide.Position.xyz - position;
        float separation = abs(dot(delta, normal));
        // Reject unrelated planes before paying for a short reconnection visibility ray.
        float planeTolerance = max(0.002f, length(delta) * 0.01f);
        if (separation > planeTolerance) continue;
        float2 weight2 = saturate(1 - abs(gridPosition - probe));
        float weight = weight2.x * weight2.y * saturate((normalAgreement - 0.9f) * 10);
        if (weight < 1e-5f) continue;
        if (!LumenProbeVisible(position, normal, guide.Position.xyz, guide.Normal.xyz)) continue;
        uint record = index * LUMEN_PROBE_STRIDE;
        irradiance += LumenEvaluateProbe(record, normal, guide.Normal.xyz) * weight;
        distance += LumenProbeSignal[record].w * weight;
        totalWeight += weight;
    }
    if (totalWeight < 0.05f)
    {
        if (LumenSettings.y < 0.5f) return false;
        irradiance = float3(1, 0, 1);
        return true;
    }
    irradiance /= totalWeight;
    distance /= totalWeight;
    if (LumenSettings.y > 0.5f) irradiance = float3(0, 1, 0);
    return all(isfinite(irradiance));
}

