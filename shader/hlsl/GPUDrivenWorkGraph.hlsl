#define SHADER_3D
#define SHADER_GPU_DRIVEN
#include "common.hlsl"
#include "GPUDrivenOpaque.hlsli"

StructuredBuffer<uint4> Geometry : register(t33);
ByteAddressBuffer Vertices : register(t34);
ByteAddressBuffer Indices : register(t35);

struct FrameInput
{
    uint3 Grid : SV_DispatchGrid;
    uint InstanceSlots;
    uint MeshCount;
};

struct GeometryInput
{
    uint3 Grid : SV_DispatchGrid;
    uint Instance;
    uint Mesh;
};

// One record describes the whole view. Visible instances create their own raster work.
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeIsProgramEntry]
[NodeMaxDispatchGrid(65535, 1, 1)]
[NumThreads(64, 1, 1)]
void SelectInstances(DispatchNodeInputRecord<FrameInput> frame, uint3 thread : SV_DispatchThreadID,
                     [MaxRecords(1)] NodeOutput<GeometryInput> Raster)
{
    uint slot = thread.x;
    uint4 selected = 0;
    bool visible = slot < frame.Get().InstanceSlots;
    if (visible)
    {
        selected = gVisibilityDebug[slot];
        visible = (gSceneInstances[slot].Flags & 5u) == 5u && selected.w == 1u &&
                  selected.y < frame.Get().MeshCount;
    }
    uint groups = 0;
    if (visible)
    {
        uint4 mesh = Geometry[selected.y];
        visible = mesh.w != 0 && mesh.y >= 3;
        groups = (mesh.y + 62u) / 63u;
    }
    ThreadNodeOutputRecords<GeometryInput> output = Raster.GetThreadNodeOutputRecords(visible ? 1 : 0);
    if (visible)
    {
        output.Get().Grid = uint3(min(groups, 65535u), (groups + 65534u) / 65535u, 1);
        output.Get().Instance = slot;
        output.Get().Mesh = selected.y;
    }
    output.OutputComplete();
}

#ifdef WORK_GRAPH_VELOCITY
struct RasterVertex
{
    float4 Position : SV_POSITION;
    float4 CurrentClip : TEXCOORD0;
    float4 PreviousClip : TEXCOORD1;
};
#else
#define RasterVertex PSInput3D
#endif

struct RasterPrimitive
{
    bool Cull : SV_CullPrimitive;
    uint Primitive : SV_PrimitiveID;
};

[Shader("node")]
[NodeLaunch("mesh")]
[NodeMaxDispatchGrid(65535, 256, 1)]
[NumThreads(64, 1, 1)]
[OutputTopology("triangle")]
void Raster(DispatchNodeInputRecord<GeometryInput> input, uint3 group : SV_GroupID, uint lane : SV_GroupIndex,
            out vertices RasterVertex vertices[63], out indices uint3 triangles[21],
            out primitives RasterPrimitive primitives[21])
{
    uint4 mesh = Geometry[input.Get().Mesh];
    uint groupIndex = group.x + group.y * input.Get().Grid.x;
    uint firstIndex = groupIndex * 63u;
    uint remaining = firstIndex < mesh.y ? mesh.y - firstIndex : 0;
    uint triangleCount = min(remaining / 3u, 21u);
    SetMeshOutputCounts(triangleCount * 3u, triangleCount);
    GPUInstanceData instance = gSceneInstances[input.Get().Instance];
    if (lane < triangleCount * 3u)
    {
        uint vertexIndex = Indices.Load((mesh.x + firstIndex + lane) * 4u) + asint(mesh.z);
        uint address = vertexIndex * 48u;
        float3 local = asfloat(Vertices.Load3(address));
        float4 world = mul(float4(local, 1), instance.World);
        float4 view = mul(world, View);
        RasterVertex vertex = (RasterVertex)0;
        vertex.Position = mul(view, Projection);
#ifdef WORK_GRAPH_VELOCITY
        vertex.CurrentClip = vertex.Position;
        vertex.PreviousClip = mul(mul(float4(local, 1), instance.PreviousWorld), PreviousViewProjection);
#else
        vertex.Normal = normalize(mul(asfloat(Vertices.Load3(address + 12u)), (float3x3)instance.World));
        vertex.TexCoord = asfloat(Vertices.Load2(address + 24u));
        vertex.Diffuse = asfloat(Vertices.Load4(address + 32u));
        vertex.WorldPos = world.xyz;
        vertex.ViewPos = view.xyz;
        vertex.CameraPos = CameraPos;
        vertex.InstanceID = input.Get().Instance;
#endif
        vertices[lane] = vertex;
    }
    if (lane < triangleCount)
    {
        triangles[lane] = uint3(lane * 3u, lane * 3u + 1u, lane * 3u + 2u);
        primitives[lane].Primitive = firstIndex / 3u + lane;
        float3 points[3];
        for (uint corner = 0; corner < 3; ++corner)
        {
            uint vertexIndex = Indices.Load((mesh.x + firstIndex + lane * 3u + corner) * 4u) + asint(mesh.z);
            points[corner] = mul(mul(float4(asfloat(Vertices.Load3(vertexIndex * 48u)), 1), instance.World), View).xyz;
        }
        bool doubleSided = (gSceneMaterials.Load(instance.MaterialIndex * GPU_SCENE_MATERIAL_STRIDE + 40u) & 4u) != 0;
        primitives[lane].Cull = !doubleSided && dot(cross(points[1] - points[0], points[2] - points[0]), points[0]) >= 0;
    }
}
