ByteAddressBuffer CandidateCommands : register(t0);
RWByteAddressBuffer VisibleCommands : register(u0);
RWByteAddressBuffer VisibleCommandCount : register(u1);
Texture2D<float> PreviousHiZ : register(t1);
Texture2D<float> CurrentHiZ : register(t2);
SamplerState HiZSampler : register(s0);

cbuffer CullConstants : register(b0)
{
    row_major float4x4 World;
    row_major float4x4 ViewProjection;
    float4 LocalCenter;
    float4 LocalExtents;
    uint CommandStrideBytes;
    uint CandidateCount;
    uint EnableFrustumCulling;
	uint Phase;
	uint EnableOcclusion;
	uint HiZWidth;
	uint HiZHeight;
	uint HiZMipCount;
	uint PreviousHiZValid;
	uint CurrentHiZValid;
};

bool IsOccluded(Texture2D<float> hierarchy, float2 uvMinimum, float2 uvMaximum, float nearestDepth)
{
    float2 extentPixels = max((uvMaximum - uvMinimum) * float2(HiZWidth, HiZHeight), 1.0f);
    float mip = clamp(floor(log2(max(extentPixels.x, extentPixels.y))), 0.0f, (float)max(HiZMipCount, 1u) - 1.0f);
    float2 center = (uvMinimum + uvMaximum) * 0.5f;
    float maximumDepth = hierarchy.SampleLevel(HiZSampler, center, mip);
    maximumDepth = max(maximumDepth, hierarchy.SampleLevel(HiZSampler, uvMinimum, mip));
    maximumDepth = max(maximumDepth, hierarchy.SampleLevel(HiZSampler, float2(uvMaximum.x, uvMinimum.y), mip));
    maximumDepth = max(maximumDepth, hierarchy.SampleLevel(HiZSampler, float2(uvMinimum.x, uvMaximum.y), mip));
    maximumDepth = max(maximumDepth, hierarchy.SampleLevel(HiZSampler, uvMaximum, mip));
    return maximumDepth < nearestDepth - 0.0015f;
}

groupshared uint GroupVisible;
groupshared uint GroupDestinationBase;
groupshared uint GroupCommandCount;

uint IsObjectVisible()
{
    uint result = 1u;
    uint outsideMask = 0x3fu;
    float2 uvMinimum = 1.0f;
    float2 uvMaximum = 0.0f;
    float nearestDepth = 1.0f;
    uint crossesNearPlane = 0u;

    [unroll]
    for (uint cornerIndex = 0; cornerIndex < 8; ++cornerIndex)
    {
        float3 signValue = float3(
            (cornerIndex & 1) ? 1.0f : -1.0f,
            (cornerIndex & 2) ? 1.0f : -1.0f,
            (cornerIndex & 4) ? 1.0f : -1.0f);
        float3 localPosition = LocalCenter.xyz + LocalExtents.xyz * signValue;
        float4 worldPosition = mul(float4(localPosition, 1.0f), World);
        float4 clipPosition = mul(worldPosition, ViewProjection);

        uint cornerMask =
            (clipPosition.x < -clipPosition.w ? 0x01u : 0u) |
            (clipPosition.x > clipPosition.w ? 0x02u : 0u) |
            (clipPosition.y < -clipPosition.w ? 0x04u : 0u) |
            (clipPosition.y > clipPosition.w ? 0x08u : 0u) |
            (clipPosition.z < 0.0f ? 0x10u : 0u) |
            (clipPosition.z > clipPosition.w ? 0x20u : 0u);
        outsideMask &= cornerMask;
        crossesNearPlane |= (clipPosition.w <= 0.0f || clipPosition.z <= 0.0f) ? 1u : 0u;
        float inverseW = rcp(max(abs(clipPosition.w), 1.0e-5f));
        float3 ndc = clipPosition.xyz * inverseW;
        float2 uv = ndc.xy * float2(0.5f, -0.5f) + 0.5f;
        uvMinimum = min(uvMinimum, uv);
        uvMaximum = max(uvMaximum, uv);
        nearestDepth = min(nearestDepth, ndc.z);
    }

    if (EnableFrustumCulling != 0u && outsideMask != 0u)
        result = 0u;

    if (result != 0u && EnableOcclusion != 0u && Phase != 0u && crossesNearPlane == 0u)
    {
        uvMinimum = saturate(uvMinimum);
        uvMaximum = saturate(uvMaximum);
        nearestDepth = saturate(nearestDepth);
        if (Phase == 1u)
        {
            if (PreviousHiZValid != 0u && IsOccluded(PreviousHiZ, uvMinimum, uvMaximum, nearestDepth))
                result = 0u;
        }
        else if (Phase == 2u)
        {
            // Phase two is restricted to objects rejected by the previous frame.
            if (PreviousHiZValid == 0u ||
                !IsOccluded(PreviousHiZ, uvMinimum, uvMaximum, nearestDepth) ||
                (CurrentHiZValid != 0u && IsOccluded(CurrentHiZ, uvMinimum, uvMaximum, nearestDepth)))
                result = 0u;
        }
    }
    return result;
}

void CopyCommand(uint sourceBase, uint destinationBase)
{
    uint byteOffset = 0;
    for (; byteOffset + 16 <= CommandStrideBytes; byteOffset += 16)
    {
        VisibleCommands.Store4(destinationBase + byteOffset,
            CandidateCommands.Load4(sourceBase + byteOffset));
    }
    for (; byteOffset < CommandStrideBytes; byteOffset += 4)
    {
        VisibleCommands.Store(destinationBase + byteOffset,
            CandidateCommands.Load(sourceBase + byteOffset));
    }
}

[numthreads(64, 1, 1)]
void main(uint3 groupId : SV_GroupID,
          uint groupThreadIndex : SV_GroupIndex,
          uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint groupBegin = groupId.x * 64;

    if (groupThreadIndex == 0)
    {
        GroupVisible = IsObjectVisible();
        GroupDestinationBase = 0;
        GroupCommandCount = groupBegin < CandidateCount
            ? min(64u, CandidateCount - groupBegin)
            : 0u;

        if (GroupVisible != 0 && GroupCommandCount != 0)
        {
            VisibleCommandCount.InterlockedAdd(
                0, GroupCommandCount, GroupDestinationBase);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    const uint commandIndex = dispatchThreadId.x;
    if (GroupVisible == 0 || commandIndex >= CandidateCount)
        return;

    const uint visibleIndex = GroupDestinationBase + groupThreadIndex;
    CopyCommand(commandIndex * CommandStrideBytes,
                visibleIndex * CommandStrideBytes);
}
