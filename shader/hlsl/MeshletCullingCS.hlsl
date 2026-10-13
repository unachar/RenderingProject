#include "MeshletCullingCommon.hlsli"
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID)
{
    uint index=id.x;
    if(index>=min(Statistics.Load(0),Capacity)) return;
    uint4 item=Candidates[index]; if(item.z==2u) return;
    GPUInstanceData instance=Instances[item.x];
    MeshletBounds bounds=Bounds[item.y];
    float4 sphere=SphereWorld(bounds,instance);
    if(EnableFrustum!=0 && FrustumCull(sphere)) { Stat(16); item.z=3; Candidates[index]=item; return; }
    if(EnableCone!=0 && ConeCull(bounds,sphere,instance)) { Stat(20); item.z=4; Candidates[index]=item; return; }
    if(OcclusionCull(sphere,instance)) { Stat(24); item.z=5; Candidates[index]=item; return; }
    uint target; Statistics.InterlockedAdd(4,1,target);
    if(target<Capacity) Visible[target]=item.xy;
    item.z=1; Candidates[index]=item;
    MeshletRecord meshlet=Meshlets[item.y]; Stat(28,meshlet.VertexCount); Stat(32,meshlet.PrimitiveCount);
}
