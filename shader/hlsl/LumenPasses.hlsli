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
    return ShadeReflectionHit(query.CommittedInstanceID(), query.CommittedPrimitiveIndex(),
        query.CommittedTriangleBarycentrics(), ray.Origin + direction * hitDistance, direction, true, seed);
}

float3 LumenDirect(float3 position, float3 normal, uint seed)
{
    float3 incident = 0, selectedDirection = 0;
    float sum = 0, selectedWeight = 0, selectedRange = 0;
    bool selectedShadow = false;
    float random = ((ReflectionHash(seed) >> 8) + 0.5f) / 16777216.0f;
    for (uint i = 0; i < ReflectionExtent.w; ++i)
    {
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
    return incident * (visibility * sum / selectedWeight);
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

groupshared float4 LumenProbeSamples[8];
[numthreads(8,1,1)]
void LumenTraceProbes(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint2 probe = group.xy;
    if (any(probe >= LumenGrid.xy)) return;
    uint index = probe.y * LumenGrid.x + probe.x;
    uint2 pixel = min(probe * LumenGrid.z + LumenGrid.z / 2, ReflectionExtent.xy - 1);
    float depth = ReflectionDepth.Load(int3(pixel, 0));
    float2 uv = (pixel + 0.5f) / ReflectionExtent.xy;
    float4 world = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1), ReflectionInvViewProjection);
    float3 normal = normalize(ReflectionNormal.Load(int3(pixel, 0)).xyz * 2 - 1);
    float3 position = world.xyz / world.w;
    if (lane == 0)
    {
        LumenProbe guide;
        guide.Position = float4(position, depth < 0.99999f ? 1 : 0);
        guide.Normal = float4(normal, length(position - ReflectionCamera.xyz));
        LumenProbeGuide[index] = guide;
    }
    if (depth >= 0.99999f)
    {
        LumenProbeSignal[index * 9 + lane] = 0;
        if (lane == 0) LumenProbeSignal[index * 9 + 8] = 0;
        return;
    }
    uint seed = ReflectionHash(index * 8 + lane + ReflectionExtent.z * 104729u);
    float distance;
    float3 irradiance = LumenIncoming(position, normal, seed, distance) * 3.14159265f +
        SampleEmissiveIrradiance(position, normal, seed + 17, pixel + uint2(lane, 0), false);
    float4 signal = float4(min(max(irradiance, 0), 65504), distance);
    LumenProbeSignal[index * 9 + lane] = signal;
    LumenProbeSamples[lane] = signal;
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0)
    {
        float4 average = 0;
        for (uint sample = 0; sample < 8; ++sample) average += LumenProbeSamples[sample] * 0.125f;
        LumenProbeSignal[index * 9 + 8] = average;
    }
}

bool LumenProbeVisible(float3 position, float3 normal, float3 probePosition, float3 probeNormal)
{
    float3 origin = position + normal * SceneRayParameters.y;
    float3 delta = probePosition + probeNormal * SceneRayParameters.y - origin;
    float length = sqrt(dot(delta, delta));
    if (length <= SceneRayParameters.y * 2) return true;
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
        uint index = probe.y * LumenGrid.x + probe.x;
        LumenProbe guide = LumenProbeGuide[index];
        if (guide.Position.w == 0 || dot(normal, guide.Normal.xyz) < 0.98f) continue;
        float3 delta = guide.Position.xyz - position;
        float separation = abs(dot(delta, normal));
        // Reject unrelated planes before paying for a short reconnection visibility ray.
        float planeTolerance = max(0.002f, length(delta) * 0.01f);
        if (separation > planeTolerance) continue;
        float2 weight2 = saturate(1 - abs(gridPosition - probe));
        float weight = weight2.x * weight2.y;
        if (weight < 1e-5f) continue;
        if (!LumenProbeVisible(position, normal, guide.Position.xyz, guide.Normal.xyz)) continue;
        float4 average = LumenProbeSignal[index * 9 + 8];
        irradiance += average.rgb * weight;
        distance += average.w * weight;
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
