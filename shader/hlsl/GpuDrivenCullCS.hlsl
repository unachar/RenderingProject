// GPU-generated indirect command compaction.
// Candidate and output buffers contain tightly packed D3D12 indirect records.
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

bool IsVisible()
{
    if (EnableFrustumCulling == 0)
    {
        return true;
    }

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
            (cornerIndex & 1) != 0 ? 1.0f : -1.0f,
            (cornerIndex & 2) != 0 ? 1.0f : -1.0f,
            (cornerIndex & 4) != 0 ? 1.0f : -1.0f);
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

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint commandIndex = dispatchThreadId.x;
    if (commandIndex >= CandidateCount || !IsVisible())
    {
        return;
    }

    uint visibleIndex = 0;
    VisibleCommandCount.InterlockedAdd(0, 1, visibleIndex);

    const uint sourceBase = commandIndex * CommandStrideBytes;
    const uint destinationBase = visibleIndex * CommandStrideBytes;
    for (uint byteOffset = 0; byteOffset < CommandStrideBytes; byteOffset += 4)
    {
        VisibleCommands.Store(destinationBase + byteOffset, CandidateCommands.Load(sourceBase + byteOffset));
    }
}
