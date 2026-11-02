// RTXDI DI reservoir core integrated with the engine's GBuffer and shadow pipeline.
// Basic MIS normalization; visibility is always traced again after spatial reuse.
#include "MegaLightsSampleCS.hlsl"
#include "Rtxdi/DI/Reservoir.hlsli"

// 96 bytes, including the source surface needed for MIS and disocclusion rejection.
struct RtxdiRecord
{
    uint4 sample;
    float4 positionM;
    float4 normal;
    float4 albedo;
    float4 material;
    uint4 identity;
};

RWStructuredBuffer<RtxdiRecord> InitialReservoirs : register(u9);
RWStructuredBuffer<RtxdiRecord> CurrentReservoirs : register(u10);
StructuredBuffer<RtxdiRecord> PreviousReservoirs : register(t16);
StructuredBuffer<uint> CurrentLightIndices : register(t17);

uint RtxdiLightSignature(MegaLightData light)
{
    uint4 hash = asuint(light.PositionType);
    hash = hash * 16777619u ^ asuint(light.DirectionRange);
    hash = hash * 16777619u ^ asuint(light.Color);
    hash = hash * 16777619u ^ asuint(light.Extra);
    hash = hash * 16777619u ^ asuint(light.Flags);
    hash = hash * 16777619u ^ asuint(light.IesProfile);
    return hash.x ^ hash.y ^ hash.z ^ hash.w;
}

RTXDI_DIReservoir LoadReservoir(RtxdiRecord record)
{
    RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();
    reservoir.lightData = record.sample.x;
    reservoir.uvData = record.sample.y;
    reservoir.weightSum = asfloat(record.sample.z);
    reservoir.targetPdf = asfloat(record.sample.w);
    reservoir.M = record.positionM.w;
    reservoir.age = record.identity.y;
    return reservoir;
}

RtxdiRecord StoreReservoir(RtxdiRecord surface, RTXDI_DIReservoir reservoir)
{
    surface.sample = uint4(reservoir.lightData, reservoir.uvData, asuint(reservoir.weightSum), asuint(reservoir.targetPdf));
    surface.positionM.w = reservoir.M;
    surface.identity = 0u;
    if (RTXDI_IsValidDIReservoir(reservoir))
    {
        surface.identity.x = asuint(Lights[RTXDI_GetDIReservoirLightIndex(reservoir)].Flags.z);
        surface.identity.y = reservoir.age;
        surface.identity.z = RtxdiLightSignature(Lights[RTXDI_GetDIReservoirLightIndex(reservoir)]);
    }
    return surface;
}

bool LoadSurface(uint2 samplePixel, out RtxdiRecord surface)
{
    surface = (RtxdiRecord)0;
    const uint2 pixel = MLSampleToScreen(samplePixel);
    const float depth = GBufferDepth.Load(int3(pixel, 0));
    const float4 material = GBufferMaterial.Load(int3(pixel, 0));
    if (MLCellIndexFromDepth(pixel, depth) == ML_INVALID_CELL || !MLIsPbrSurface(material.a) || MLCounts.x == 0u)
    {
        return false;
    }
    surface.positionM.xyz = MLWorldPositionFromDepth(pixel, depth);
    surface.normal.xyz = normalize(GBufferNormal.Load(int3(pixel, 0)).xyz * 2.0f - 1.0f);
    surface.albedo.rgb = GBufferBaseColor.Load(int3(pixel, 0)).rgb;
    surface.material = material;
    return true;
}

float TargetAtSurface(uint lightIndex, RtxdiRecord surface)
{
    // Full-support proposal avoids losing lights when a reservoir crosses a cluster boundary.
    // A positive floor also covers differences between the cheap target and the final LTC/BRDF.
    return max(MLTargetWeight(Lights[lightIndex], surface.positionM.xyz, surface.normal.xyz,
                             MLViewDirection(surface.positionM.xyz), surface.albedo.rgb, surface.material.rgb), 1.0e-8f);
}

bool CompatibleSurface(RtxdiRecord center, RtxdiRecord other)
{
    const float3 cameraPosition = mul(float4(0, 0, 0, 1), MLViewToWorld).xyz;
    const float tolerance = max(0.05f, length(center.positionM.xyz - cameraPosition) * 0.01f);
    return dot(center.normal.xyz, other.normal.xyz) > 0.9f &&
           length(center.positionM.xyz - other.positionM.xyz) < tolerance &&
           abs(center.material.g - other.material.g) < 0.15f &&
           abs(center.material.r - other.material.r) < 0.15f;
}

[numthreads(8, 8, 1)]
void RtxdiInitialTemporal(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint2 samplePixel = dispatchId.xy;
    if (any(samplePixel >= MLSampleGrid.xy))
    {
        return;
    }
    RtxdiRecord surface;
    const bool validSurface = LoadSurface(samplePixel, surface);
    const uint base = MLSampleBase(samplePixel);
    const uint2 pixel = MLSampleToScreen(samplePixel);
    uint2 cell = 0u;
    if (validSurface)
    {
        const uint cellIndex = MLCellIndexFromDepth(pixel, GBufferDepth.Load(int3(pixel, 0)));
        cell = Cells[cellIndex];
    }
    for (uint sampleIndex = 0u; sampleIndex < MLStochastic.y; ++sampleIndex)
    {
        RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();
        if (validSurface)
        {
            const uint seed = MLSeed(pixel, MLStochastic.z, 0x525458u + sampleIndex * 7793u);
            for (uint candidate = 0u; candidate < MLRtxdi.y; ++candidate)
            {
                const uint stream = seed + candidate * 0x9e3779b9u;
                // Mix clustered candidates with a full-scene proposal. The latter preserves
                // support when reusing reservoirs across cluster boundaries.
                const float globalMix = clamp(MLStochasticParams.x, 0.001f, 1.0f);
                uint lightIndex = min(uint(MLRandom(stream) * MLCounts.x), MLCounts.x - 1u);
                bool inCell = false;
                if (cell.y > 0u && MLRandom(stream ^ 0x4c4f4341u) >= globalMix)
                {
                    const uint localIndex = min(uint(MLRandom(stream ^ 0x494e4458u) * cell.y), cell.y - 1u);
                    lightIndex = CellIndices[cell.x + localIndex];
                    inCell = true;
                }
                else if (cell.y > 0u)
                {
                    for (uint localIndex = 0u; localIndex < cell.y; ++localIndex)
                    {
                        if (CellIndices[cell.x + localIndex] == lightIndex)
                        {
                            inCell = true;
                            break;
                        }
                    }
                }
                float proposalPdf = 1.0f / float(MLCounts.x);
                if (cell.y > 0u)
                {
                    proposalPdf *= globalMix;
                    if (inCell)
                    {
                        proposalPdf += (1.0f - globalMix) / float(cell.y);
                    }
                }
                const float2 uv = float2(MLRandom(stream ^ 0x165667b1u), MLRandom(stream ^ 0xd3a2646cu));
                RTXDI_StreamSample(reservoir, lightIndex, uv, MLRandom(stream ^ 0x27d4eb2fu),
                                   TargetAtSurface(lightIndex, surface), 1.0f / proposalPdf);
            }
            RTXDI_FinalizeResampling(reservoir, 1.0f, reservoir.M);
            const float2 previousUv = (float2(pixel) + 0.5f) / float2(MLScreen.xy) - GBufferVelocity.Load(int3(pixel, 0)).zw;
            if (MLRtxdi.x != 0u && all(previousUv >= 0.0f) && all(previousUv < 1.0f))
            {
                const uint2 previousPixel = min(uint2(previousUv * MLScreen.xy) / MLSampleScale(), MLSampleGrid.xy - 1u);
                const RtxdiRecord previous = PreviousReservoirs[MLSampleBase(previousPixel) + sampleIndex];
                RTXDI_DIReservoir history = LoadReservoir(previous);
                uint historyLight = 0xffffffffu;
                if (RTXDI_IsValidDIReservoir(history) && previous.identity.x < MLRtxdi.z)
                {
                    historyLight = CurrentLightIndices[previous.identity.x];
                }
                bool reusable = false;
                if (historyLight < MLCounts.x && history.age < 16u)
                {
                    reusable = previous.identity.z == RtxdiLightSignature(Lights[historyLight]) && CompatibleSurface(surface, previous);
                }
                if (reusable)
                {
                    history.lightData = historyLight | RTXDI_DIReservoir_LightValidBit;
                    history.M = min(history.M, float(MLRtxdi.y) * 4.0f);
                    history.age += 1u;
                    const RTXDI_DIReservoir initial = reservoir;
                    reservoir = RTXDI_EmptyDIReservoir();
                    RTXDI_CombineDIReservoirs(reservoir, initial, 0.5f, initial.targetPdf);
                    const bool selectedHistory = RTXDI_CombineDIReservoirs(reservoir, history, MLRandom(seed ^ 0xba55u), TargetAtSurface(historyLight, surface));
                    const uint selected = RTXDI_GetDIReservoirLightIndex(reservoir);
                    const float targetHere = TargetAtSurface(selected, surface);
                    const float targetPrevious = TargetAtSurface(selected, previous);
                    float numerator = targetHere;
                    if (selectedHistory)
                    {
                        numerator = targetPrevious;
                    }
                    const float denominator = initial.M * targetHere + history.M * targetPrevious;
                    RTXDI_FinalizeResampling(reservoir, numerator, denominator);
                }
            }
        }
        InitialReservoirs[base + sampleIndex] = StoreReservoir(surface, reservoir);
    }
}

[numthreads(8, 8, 1)]
void RtxdiSpatial(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint2 samplePixel = dispatchId.xy;
    if (any(samplePixel >= MLSampleGrid.xy))
    {
        return;
    }
    const uint base = MLSampleBase(samplePixel);
    const uint2 pixel = MLSampleToScreen(samplePixel);
    for (uint sampleIndex = 0u; sampleIndex < MLStochastic.y; ++sampleIndex)
    {
        const RtxdiRecord center = InitialReservoirs[base + sampleIndex];
        RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();
        RtxdiRecord sources[5];
        sources[0] = center;
        uint sourceCount = 1u;
        uint selectedSource = 0u;
        const uint seed = MLSeed(pixel, MLStochastic.z, 0x535041u + sampleIndex * 7793u);
        if (RTXDI_IsValidDIReservoir(LoadReservoir(center)))
        {
            const int2 offsets[4] = {int2(-2, 0), int2(2, 0), int2(0, -2), int2(0, 2)};
            for (uint neighbor = 0u; neighbor < 4u; ++neighbor)
            {
                const int2 neighborPixel = int2(samplePixel) + offsets[neighbor];
                if (any(neighborPixel < 0) || any(neighborPixel >= int2(MLSampleGrid.xy)))
                {
                    continue;
                }
                const RtxdiRecord candidate = InitialReservoirs[MLSampleBase(uint2(neighborPixel)) + sampleIndex];
                if (RTXDI_IsValidDIReservoir(LoadReservoir(candidate)) && CompatibleSurface(center, candidate))
                {
                    sources[sourceCount++] = candidate;
                }
            }
            for (uint source = 0u; source < sourceCount; ++source)
            {
                const RTXDI_DIReservoir candidate = LoadReservoir(sources[source]);
                if (RTXDI_CombineDIReservoirs(reservoir, candidate, MLRandom(seed + source * 0x9e3779b9u),
                                             TargetAtSurface(RTXDI_GetDIReservoirLightIndex(candidate), center)))
                {
                    selectedSource = source;
                }
            }
            const uint selected = RTXDI_GetDIReservoirLightIndex(reservoir);
            float denominator = 0.0f;
            float numerator = 0.0f;
            for (uint source = 0u; source < sourceCount; ++source)
            {
                const float targetAtSource = TargetAtSurface(selected, sources[source]);
                denominator += sources[source].positionM.w * targetAtSource;
                if (source == selectedSource)
                {
                    numerator = targetAtSource;
                }
            }
            RTXDI_FinalizeResampling(reservoir, numerator, denominator);
        }
        CurrentReservoirs[base + sampleIndex] = StoreReservoir(center, reservoir);
        uint4 result = 0u;
        if (RTXDI_IsValidDIReservoir(reservoir) && isfinite(reservoir.weightSum) && reservoir.weightSum > 0.0f)
        {
            const uint lightIndex = RTXDI_GetDIReservoirLightIndex(reservoir);
            result.x = lightIndex | ML_SAMPLE_VALID;
            if (MLLightCastsShadow(Lights[lightIndex]))
            {
                result.x |= ML_SAMPLE_TRACE;
            }
            else
            {
                result.x |= ML_SAMPLE_VISIBLE;
            }
            result.y = asuint(RTXDI_GetDIReservoirInvPdf(reservoir));
            result.z = MLPackUv(RTXDI_GetDIReservoirSampleUV(reservoir));
        }
        Samples[base + sampleIndex] = result;
        MLAppendTrace((result.x & ML_SAMPLE_TRACE) != 0u, MLPackTraceEntry(samplePixel, sampleIndex));
    }
}
