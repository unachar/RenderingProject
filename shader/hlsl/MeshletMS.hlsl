#define SHADER_3D
#define SHADER_GPU_DRIVEN
#include "common.hlsl"
#include "MeshletCommon.hlsli"
StructuredBuffer<GPUInstanceData> Instances:register(t30);
ByteAddressBuffer Materials:register(t31);
StructuredBuffer<MeshletRecord> Meshlets:register(t33);
StructuredBuffer<uint> Remap:register(t34);
StructuredBuffer<uint> Primitives:register(t35);
ByteAddressBuffer Vertices:register(t36);
StructuredBuffer<uint2> Visible:register(t37);
ByteAddressBuffer Statistics:register(t38);
cbuffer MeshDebug:register(b6) { uint MeshletColors; }
struct PrimitiveOutput { bool Cull:SV_CullPrimitive; };
#ifdef MESHLET_VELOCITY
struct MeshVertex { float4 Position:SV_POSITION; float4 CurrentClip:TEXCOORD0; float4 PreviousClip:TEXCOORD1; nointerpolation uint MaterialFlags:TEXCOORD2; };
#else
#define MeshVertex PSInput3D
#endif
[outputtopology("triangle")]
[numthreads(64,1,1)]
void main(uint3 group:SV_GroupID,uint lane:SV_GroupIndex,
    out vertices MeshVertex outputVertices[64],out indices uint3 outputTriangles[126],out primitives PrimitiveOutput primitiveOutput[126])
{
    uint count=Statistics.Load(4);
    uint index=group.x+group.y*min(count,65535u);
    MeshletRecord meshlet=(MeshletRecord)0;
    uint2 item=0;
    if(index<count) { item=Visible[index]; meshlet=Meshlets[item.y]; }
    SetMeshOutputCounts(meshlet.VertexCount,meshlet.PrimitiveCount);
    GPUInstanceData instance=Instances[item.x];
    for(uint i=lane;i<meshlet.VertexCount;i+=64)
    {
        uint address=Remap[meshlet.VertexOffset+i]*48;
        float3 position=asfloat(Vertices.Load3(address));
        float3 normal=asfloat(Vertices.Load3(address+12));
        MeshVertex vertex=(MeshVertex)0;
        float4 world=mul(float4(position,1),instance.World),view=mul(world,View);
        vertex.Position=mul(view,Projection);
#ifdef MESHLET_VELOCITY
        vertex.CurrentClip=vertex.Position;
        vertex.MaterialFlags=Materials.Load(instance.MaterialIndex*GPU_SCENE_MATERIAL_STRIDE+40);
        vertex.PreviousClip=mul(mul(float4(position,1),instance.PreviousWorld),PreviousViewProjection);
#else
        vertex.Normal=normalize(mul(normal,(float3x3)instance.World));
        vertex.TexCoord=asfloat(Vertices.Load2(address+24)); vertex.Diffuse=asfloat(Vertices.Load4(address+32));
        if(MeshletColors!=0) vertex.Diffuse.rgb=0.2+0.8*frac(float3(0.618,0.414,0.732)*(item.y+1));
        vertex.WorldPos=world.xyz; vertex.ViewPos=view.xyz; vertex.CameraPos=CameraPos; vertex.InstanceID=item.x;
#endif
        outputVertices[i]=vertex;
    }
    for(uint p=lane;p<meshlet.PrimitiveCount;p+=64)
    {
        uint packed=Primitives[meshlet.PrimitiveOffset+p];
        outputTriangles[p]=uint3(packed&255,(packed>>8)&255,(packed>>16)&255);
        float3 points[3];
        for(uint j=0;j<3;++j)
        {
            float3 position=asfloat(Vertices.Load3(Remap[meshlet.VertexOffset+((packed>>(j*8))&255)]*48));
            points[j]=mul(mul(float4(position,1),instance.World),View).xyz;
        }
        bool doubleSided=(Materials.Load(instance.MaterialIndex*GPU_SCENE_MATERIAL_STRIDE+40)&4u)!=0;
        primitiveOutput[p].Cull=!doubleSided && dot(cross(points[1]-points[0],points[2]-points[0]),points[0])>=0;
    }
}

