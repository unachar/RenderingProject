ByteAddressBuffer CandidateCommands : register(t0);
RWByteAddressBuffer VisibleCommands : register(u0);
RWByteAddressBuffer VisibleCommandCount : register(u1);

cbuffer CullConstants : register(b0)
{
    row_major float4x4 World;
    row_major float4x4 ViewProjection;
    float4 LocalCenter;
    float4 LocalExtents;
    uint CommandStrideBytes;
    uint CandidateCount;
    uint EnableFrustumCulling;
    uint Padding0;
};

groupshared uint GroupVisible;
groupshared uint GroupDestinationBase;
groupshared uint GroupCommandCount;

bool IsObjectVisible()
{
    if (EnableFrustumCulling == 0)
        return true;

    bool outsideLeft = true;
    bool outsideRight = true;
    bool outsideBottom = true;
    bool outsideTop = true;
    bool outsideNear = true;
    bool outsideFar = true;

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

        outsideLeft &= clipPosition.x < -clipPosition.w;
        outsideRight &= clipPosition.x > clipPosition.w;
        outsideBottom &= clipPosition.y < -clipPosition.w;
        outsideTop &= clipPosition.y > clipPosition.w;
        outsideNear &= clipPosition.z < 0.0f;
        outsideFar &= clipPosition.z > clipPosition.w;
    }

    return !(outsideLeft || outsideRight || outsideBottom || outsideTop || outsideNear || outsideFar);
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
        GroupVisible = IsObjectVisible() ? 1u : 0u;
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
