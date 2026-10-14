#include "GpuSceneCommon.hlsli"
#include "VirtualGeometryCommon.hlsli"
StructuredBuffer<GPUInstanceData> Instances : register(t0);
StructuredBuffer<uint4> Objects : register(t1);
StructuredBuffer<VirtualGeometryGPUAsset> Assets : register(t2);
StructuredBuffer<VirtualGeometryGPUCluster> Clusters : register(t3);
StructuredBuffer<VirtualGeometryGPUGroup> Groups : register(t4);
StructuredBuffer<uint> Hierarchy : register(t5);
StructuredBuffer<uint> Roots : register(t6);
ByteAddressBuffer ObjectCount : register(t7);
ByteAddressBuffer Materials : register(t8);
Texture2D<float> HiZ : register(t9);
globallycoherent RWStructuredBuffer<uint> GroupStates : register(u0);
RWStructuredBuffer<VisibleVirtualCluster> Visible : register(u1);
RWByteAddressBuffer Statistics : register(u2);
RWByteAddressBuffer Arguments : register(u3);
RWStructuredBuffer<uint> Success : register(u4);
RWStructuredBuffer<uint> TraversalQueue : register(u5);
cbuffer Parameters : register(b0)
{
    row_major float4x4 VP;
    row_major float4x4 PreviousVP;
    float4 Camera;
    uint Capacity, ObjectCapacity, AssetCount, MaterialCount;
    uint EnableFrustum, EnableCone, EnableOcclusion, Enabled;
    float Bias; float2 HiZSize; uint HiZMips;
    float ProjectionPixels, TargetPixels;
    int ForceLevel;
    uint StateCapacity;
};
groupshared uint StateBase, QueueBase, QueueCount, QueueCursor, OutputBase, OutputCount, Accepted;
groupshared uint CurrentClusters[64];
void Stat(uint offset, uint value = 1)
{
    uint unused;
    Statistics.InterlockedAdd(offset, value, unused);
}
float MaximumScale(GPUInstanceData instance)
{
    float3 a = instance.World[0].xyz, b = instance.World[1].xyz, c = instance.World[2].xyz;
    // Gershgorin upper bound on the Gram matrix's largest eigenvalue.
    float ab = abs(dot(a,b)), ac = abs(dot(a,c)), bc = abs(dot(b,c));
    return sqrt(max(dot(a,a)+ab+ac,max(dot(b,b)+ab+bc,dot(c,c)+ac+bc)));
}
float4 WorldSphere(float4 bounds, GPUInstanceData instance)
{
    return float4(mul(float4(bounds.xyz, 1), instance.World).xyz, bounds.w * MaximumScale(instance));
}
bool FrustumCull(float4 sphere)
{
    float4 c0 = float4(VP[0][0], VP[1][0], VP[2][0], VP[3][0]);
    float4 c1 = float4(VP[0][1], VP[1][1], VP[2][1], VP[3][1]);
    float4 c2 = float4(VP[0][2], VP[1][2], VP[2][2], VP[3][2]);
    float4 c3 = float4(VP[0][3], VP[1][3], VP[2][3], VP[3][3]);
    float4 planes[6] = {c3+c0, c3-c0, c3+c1, c3-c1, c2, c3-c2};
    for (uint plane = 0; plane < 6; ++plane)
    {
        if (dot(float4(sphere.xyz, 1), planes[plane]) < -sphere.w * length(planes[plane].xyz)) return true;
    }
    return false;
}
bool OcclusionCull(float4 sphere, GPUInstanceData instance)
{
    if (EnableOcclusion == 0) return false;
    for (uint row = 0; row < 4; ++row)
    {
        if (!all(instance.World[row] == instance.PreviousWorld[row])) return false;
    }
    float2 minimum = 1, maximum = 0;
    float nearest = 1;
    for (uint corner = 0; corner < 8; ++corner)
    {
        float3 sign = float3((corner&1) ? 1 : -1, (corner&2) ? 1 : -1, (corner&4) ? 1 : -1);
        float4 clip = mul(float4(sphere.xyz + sign*sphere.w, 1), PreviousVP);
        if (!all(isfinite(clip)) || clip.w <= 1e-5 || clip.z <= 0 || clip.z >= clip.w) return false;
        float3 ndc = clip.xyz / clip.w;
        float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
        minimum = min(minimum, uv);
        maximum = max(maximum, uv);
        nearest = min(nearest, ndc.z);
    }
    minimum -= 1 / HiZSize;
    maximum += 1 / HiZSize;
    if (any(minimum < 0) || any(maximum >= 1) || any(maximum < minimum)) return false;
    for (uint mip = 0; mip < HiZMips; ++mip)
    {
        uint width, height, mips;
        HiZ.GetDimensions(mip, width, height, mips);
        uint2 first = min((uint2)floor(minimum*float2(width,height)), uint2(width-1,height-1));
        uint2 last = min((uint2)floor(maximum*float2(width,height)), uint2(width-1,height-1));
        if (any(last-first > 1)) continue;
        float farthest = 0;
        for (uint y = first.y; y <= last.y; ++y)
        {
            for (uint x = first.x; x <= last.x; ++x)
            {
                float depth = HiZ.Load(int3(x,y,mip));
                if (!isfinite(depth)) return false;
                farthest = max(farthest,depth);
            }
        }
        return nearest > farthest + max(Bias,0);
    }
    return false;
}
bool ConeCull(VirtualGeometryGPUCluster cluster, GPUInstanceData instance)
{
    if (EnableCone == 0 || cluster.Cone.w >= 1) return false;
    float3 a = instance.World[0].xyz, b = instance.World[1].xyz, c = instance.World[2].xyz;
    float3 sizes = float3(length(a),length(b),length(c));
    if (min(sizes.x,min(sizes.y,sizes.z)) < 1e-6 || dot(cross(a,b),c) <= 0) return false;
    a /= sizes.x; b /= sizes.y; c /= sizes.z;
    if (max(sizes.x,max(sizes.y,sizes.z))-min(sizes.x,min(sizes.y,sizes.z)) > sizes.x*1e-4 ||
        abs(dot(a,b))+abs(dot(a,c))+abs(dot(b,c)) > 1e-4) return false;
    float3 apex = mul(float4(cluster.ConeApex.xyz,1),instance.World).xyz;
    float3 axis = normalize(mul(cluster.Cone.xyz,(float3x3)instance.World));
    float3 delta = apex-Camera.xyz;
    float distance = length(delta);
    if (distance <= 1e-5) return false;
    return dot(axis,delta/distance) >= cluster.Cone.w + 1e-5;
}
bool Reserve(uint counter, uint amount, uint limit, out uint base)
{
    uint first = Statistics.Load(counter), observed;
    [allow_uav_condition] while (true)
    {
        if (amount > limit || first > limit-amount) { base = 0; return false; }
        Statistics.InterlockedCompareExchange(counter,first,first+amount,observed);
        if (first == observed) { base = first; return true; }
        first = observed;
    }
}
[numthreads(64,1,1)]
void Reset(uint3 id : SV_DispatchThreadID)
{
    if (id.x < 64) Statistics.Store(id.x*4,0);
    if (id.x < ObjectCapacity) Success[id.x] = 0;
}
[numthreads(1,1,1)]
void BuildArguments()
{
    uint count = Statistics.Load(4);
    uint x = min(count,65535u);
    Arguments.Store3(0,uint3(x,count ? (count+x-1)/x : 1,1));
}
[numthreads(64,1,1)]
void Traverse(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint object = group.x + group.y*65535u;
    if (object >= min(ObjectCount.Load(0),ObjectCapacity) || Enabled == 0) return;
    uint4 selected = Objects[object];
    GPUInstanceData instance = Instances[selected.x];
    uint assetId = instance.VirtualGeometryAssetIndex;
    if (assetId >= AssetCount || (instance.Flags&12u) != 4u || instance.MaterialIndex >= MaterialCount) return;
    uint materialFlags = Materials.Load(instance.MaterialIndex*GPU_SCENE_MATERIAL_STRIDE+40);
    // Initial native path excludes alpha-test and two-sided raster classes.
    if ((materialFlags&6u) != 0) return;
    uint surfaceFlags = Materials.Load(instance.MaterialIndex*GPU_SCENE_MATERIAL_STRIDE+64+149*16+12);
    if ((surfaceFlags&3u) != 0 || ((surfaceFlags>>2)&3u) != 0) return;
    VirtualGeometryGPUAsset asset = Assets[assetId];
    if ((asset.Flags&1u) == 0) return;
    if (lane == 0)
    {
        Accepted = Reserve(0,asset.GroupCount,StateCapacity,StateBase) &&
            Reserve(8,asset.ClusterCount,StateCapacity,QueueBase);
        QueueCount = 0; QueueCursor = 0; OutputCount = 0;
        if (Accepted == 0) Stat(12);
    }
    GroupMemoryBarrierWithGroupSync();
    if (Accepted == 0) return;

    // Establish a group-consistent cut before traversal. Fine-group demands
    // propagate to every consuming ancestor, including split parent sets.
    for (uint index = lane; index < asset.GroupCount; index += 64)
    {
        VirtualGeometryGPUGroup node = Groups[asset.GroupOffset+index];
        float4 sphere = WorldSphere(node.Bounds,instance);
        float distance = max(length(sphere.xyz-Camera.xyz)-sphere.w,1e-6);
        float errorPixels = node.MaxError*MaximumScale(instance)*ProjectionPixels/distance;
        bool refine = ForceLevel >= 0 ? node.Level >= (uint)ForceLevel : errorPixels > TargetPixels;
        GroupStates[StateBase+index] = refine ? 1u : 0u;
    }
    AllMemoryBarrierWithGroupSync();
    uint hierarchyLevels = (asset.Flags>>8)&63u;
    for (uint level = 0; level < hierarchyLevels; ++level)
    {
        for (uint index = lane; index < asset.GroupCount; index += 64)
        {
            VirtualGeometryGPUGroup node = Groups[asset.GroupOffset+index];
            if (node.Level != level || GroupStates[StateBase+index] == 0) continue;
            for (uint parent = 0; parent < node.ParentCount; ++parent)
            {
                uint cluster = Hierarchy[asset.HierarchyIndexOffset+node.FirstParent+parent];
                uint consumer = Clusters[asset.ClusterOffset+cluster].GroupIndex;
                if (consumer != 0xffffffffu)
                {
                    uint unused;
                    InterlockedOr(GroupStates[StateBase+consumer],1,unused);
                }
            }
        }
        AllMemoryBarrierWithGroupSync();
    }
    // Bit 1 marks generating groups already expanded, so shared roots enqueue
    // each child set once. Each cluster can enter the persistent queue once.
    for (uint root = lane; root < asset.RootCount; root += 64)
    {
        uint offset;
        InterlockedAdd(QueueCount,1,offset);
        TraversalQueue[QueueBase+offset] = Roots[asset.RootOffset+root];
    }
    AllMemoryBarrierWithGroupSync();
    while (QueueCursor < QueueCount)
    {
        uint begin = QueueCursor;
        uint end = min(begin+64,QueueCount);
        if (begin+lane < end) CurrentClusters[lane] = TraversalQueue[QueueBase+begin+lane];
        // All waves must snapshot the same cursor before lane 0 advances it.
        GroupMemoryBarrierWithGroupSync();
        if (lane == 0) QueueCursor = end;
        GroupMemoryBarrierWithGroupSync();
        if (begin+lane < end)
        {
            uint index = CurrentClusters[lane];
            VirtualGeometryGPUCluster cluster = Clusters[asset.ClusterOffset+index];
            uint producer = cluster.GeneratingGroupIndex;
            bool expand = false;
            if (producer != 0xffffffffu)
            {
                expand = (GroupStates[StateBase+producer]&1u) != 0;
            }
            if (expand)
            {
                uint old;
                InterlockedOr(GroupStates[StateBase+producer],2u,old);
                if ((old&2u) == 0)
                {
                    VirtualGeometryGPUGroup node = Groups[asset.GroupOffset+producer];
                    float4 sphere = WorldSphere(node.Bounds,instance);
                    // Group bounds cover all descendants, allowing subtree rejection.
                    if ((EnableFrustum == 0 || !FrustumCull(sphere)) && !OcclusionCull(sphere,instance))
                    {
                        uint offset;
                        InterlockedAdd(QueueCount,node.ChildCount,offset);
                        for (uint child = 0; child < node.ChildCount; ++child)
                        {
                            TraversalQueue[QueueBase+offset+child] = Hierarchy[asset.HierarchyIndexOffset+node.FirstChild+child];
                        }
                    }
                    else Stat(20);
                }
            }
            else
            {
                float4 sphere = WorldSphere(cluster.Bounds,instance);
                if ((EnableFrustum == 0 || !FrustumCull(sphere)) && !OcclusionCull(sphere,instance) && !ConeCull(cluster,instance))
                {
                    // Compact in place behind the read cursor; queue entries are
                    // held in shared memory before any lane overwrites this prefix.
                    uint output;
                    InterlockedAdd(OutputCount,1,output);
                    TraversalQueue[QueueBase+output] = index;
                }
                else Stat(20);
            }
        }
        AllMemoryBarrierWithGroupSync();
    }
    if (lane == 0)
    {
        Accepted = Reserve(4,OutputCount,Capacity,OutputBase);
        Success[object] = Accepted;
        if (Accepted == 0) Stat(12);
        else Stat(16);
    }
    GroupMemoryBarrierWithGroupSync();
    if (Accepted == 0) return;
    for (uint output = lane; output < OutputCount; output += 64)
    {
        uint index = TraversalQueue[QueueBase+output];
        VisibleVirtualCluster record;
        record.InstanceIndex = selected.x;
        record.ClusterIndex = asset.ClusterOffset+index;
        record.MaterialIndex = instance.MaterialIndex;
        record.Flags = assetId;
        Visible[OutputBase+output] = record;
        VirtualGeometryGPUCluster cluster = Clusters[record.ClusterIndex];
        Stat(24,cluster.TriangleCount);
        Stat(32+cluster.Level*4);
    }
}
