struct InstanceInput
{
    row_major float4x4 World;
    float4 LocalCenter;
    float4 LocalExtents;
    float4 LodDistances;
};

struct InstanceTransform
{
    row_major float4x4 World;
};

StructuredBuffer<InstanceInput> CandidateInstances : register(t0);
RWStructuredBuffer<InstanceTransform> Lod0Instances : register(u0);
RWStructuredBuffer<InstanceTransform> Lod1Instances : register(u1);
RWStructuredBuffer<InstanceTransform> Lod2Instances : register(u2);
RWByteAddressBuffer LodCounts : register(u3);

cbuffer CullLodConstants : register(b0)
{
    float4 FrustumPlanes[6];
    float4 CameraPosition;
    uint CandidateCount;
    uint AvailableLodCount;
    uint EnableFrustumCulling;
    uint Padding0;
};

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    if (index >= CandidateCount)
        return;

    InstanceInput instance = CandidateInstances[index];
    if (EnableFrustumCulling != 0 && instance.LodDistances.z >= 0.5f)
    {
        float3 worldCenter = mul(float4(instance.LocalCenter.xyz, 1.0f), instance.World).xyz;
        float scaleX = length(instance.World[0].xyz);
        float scaleY = length(instance.World[1].xyz);
        float scaleZ = length(instance.World[2].xyz);
        float worldRadius = length(instance.LocalExtents.xyz) * max(scaleX, max(scaleY, scaleZ));
        [unroll]
        for (uint planeIndex = 0; planeIndex < 6; ++planeIndex)
        {
            float signedDistance = dot(float4(worldCenter, 1.0f), FrustumPlanes[planeIndex]);
            if (signedDistance < -worldRadius)
                return;
        }
    }

    float3 worldPosition = float3(instance.World[3][0], instance.World[3][1], instance.World[3][2]);
    float distanceToCamera = length(worldPosition - CameraPosition.xyz);
    uint lod = 0;
    if (AvailableLodCount > 2 && distanceToCamera >= instance.LodDistances.y)
        lod = 2;
    else if (AvailableLodCount > 1 && distanceToCamera >= instance.LodDistances.x)
        lod = 1;

    uint destination = 0;
    LodCounts.InterlockedAdd(lod * 4, 1, destination);
    InstanceTransform output;
    output.World = instance.World;
    if (lod == 0)
        Lod0Instances[destination] = output;
    else if (lod == 1)
        Lod1Instances[destination] = output;
    else
        Lod2Instances[destination] = output;
}
