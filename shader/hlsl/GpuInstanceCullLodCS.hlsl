// インスタンス描画（InstancingSystem）の GPU カリング + LOD 選択 + インポスター置き換え。
// CPU は候補を並べるだけで、視錐台・LOD・インポスター切替の判定はすべてここで行う。
struct InstanceInput
{
    row_major float4x4 World;
    float4 LocalCenter;
    float4 LocalExtents;
    float4 LodDistances; // x: LOD1, y: LOD2, z: 視錐台カリング有効
    float4 Impostor;     // xyz: モデルのローカル中心, w: 切替距離（0 = インポスターなし）
    uint4 ImpostorInfo;  // x: グループ, y: 出力先頭, z: グループ容量, w: 有効
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
RWStructuredBuffer<InstanceTransform> ImpostorInstances : register(u4);
RWByteAddressBuffer ImpostorArgs : register(u5);

cbuffer CullLodConstants : register(b0)
{
    float4 FrustumPlanes[6];
    float4 CameraPosition;
    uint CandidateCount;
    uint AvailableLodCount;
    uint EnableFrustumCulling;
    uint MinimumLod;
    uint OutputBase;
    uint CountOffset; // バイト
    uint EnableImpostor;
    uint Padding0;
};

void CullInstance(uint3 dispatchThreadId)
{
    const uint index = dispatchThreadId.x;
    if (index >= CandidateCount)
    {
        return;
    }

    InstanceInput instance = CandidateInstances[index];
    if (EnableFrustumCulling != 0 && instance.LodDistances.z >= 0.5f)
    {
        float3 worldCenter = mul(float4(instance.LocalCenter.xyz, 1.0f), instance.World).xyz;
        float scaleX = length(instance.World[0].xyz);
        float scaleY = length(instance.World[1].xyz);
        float scaleZ = length(instance.World[2].xyz);
        float worldRadius = length(instance.LocalExtents.xyz) * max(scaleX, max(scaleY, scaleZ));
        [unroll] for (uint planeIndex = 0; planeIndex < 6; ++planeIndex)
        {
            float signedDistance = dot(float4(worldCenter, 1.0f), FrustumPlanes[planeIndex]);
            if (signedDistance < -worldRadius)
            {
                return;
            }
        }
    }

    // インポスター置き換え（メイン視点のみ。影パスでは EnableImpostor = 0 で常に実メッシュ）。
    if (EnableImpostor != 0 && instance.ImpostorInfo.w != 0 && instance.Impostor.w > 0.0f)
    {
        float3 toCamera = mul(float4(instance.Impostor.xyz, 1.0f), instance.World).xyz - CameraPosition.xyz;
        if (dot(toCamera, toCamera) >= instance.Impostor.w * instance.Impostor.w)
        {
            uint slot = 0;
            ImpostorArgs.InterlockedAdd(instance.ImpostorInfo.x * 16u + 4u, 1u, slot);
            if (slot < instance.ImpostorInfo.z)
            {
                InstanceTransform impostor;
                impostor.World = instance.World;
                ImpostorInstances[instance.ImpostorInfo.y + slot] = impostor;
            }
            return;
        }
    }

    float3 worldPosition = float3(instance.World[3][0], instance.World[3][1], instance.World[3][2]);
    float distanceToCamera = length(worldPosition - CameraPosition.xyz);
    uint lod = 0;
    if (AvailableLodCount > 2 && distanceToCamera >= instance.LodDistances.y)
    {
        lod = 2;
    }
    else if (AvailableLodCount > 1 && distanceToCamera >= instance.LodDistances.x)
    {
        lod = 1;
    }
    lod = min(max(lod, MinimumLod), max(AvailableLodCount, 1u) - 1u);

    uint destination = 0;
    LodCounts.InterlockedAdd(CountOffset + lod * 4, 1, destination);
    InstanceTransform output;
    output.World = instance.World;
    if (lod == 0)
    {
        Lod0Instances[OutputBase + destination] = output;
    }
    else if (lod == 1)
    {
        Lod1Instances[OutputBase + destination] = output;
    }
    else
    {
        Lod2Instances[OutputBase + destination] = output;
    }
}

#ifndef INSTANCE_CULL_WORK_GRAPH
[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    CullInstance(dispatchThreadId);
}
#endif
