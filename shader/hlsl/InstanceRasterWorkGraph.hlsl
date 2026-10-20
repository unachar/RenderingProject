#define main InstanceVertexMain
#ifdef INSTANCE_SHADOW_GRAPH
#include "ShadowMapInstancedVS.hlsl"
struct InstanceMeshVertex
{
    float4 Position : SV_POSITION;
};
#else
#include "modelshaderInstancedVS.hlsl"
#define InstanceMeshVertex PSInput3D
#endif
#undef main

ByteAddressBuffer InstanceVertices : register(t20);
ByteAddressBuffer InstanceIndices : register(t21);
cbuffer InstanceGeometry : register(b4)
{
    uint VertexStride;
    uint IndexStride;
    uint IsIndexed;
    uint GeometryPadding;
};

struct InstanceRasterRecord
{
    uint DrawCount;
    uint InstanceCount;
    uint StartIndex;
    uint BaseVertex;
    uint FirstInstance;
};

struct InstanceMeshRecord
{
    uint3 DispatchGrid : SV_DispatchGrid;
    uint FirstGroup;
    uint GroupsPerInstance;
    uint TriangleCount;
    uint TotalGroups;
};

struct InstancePrimitive
{
    uint PrimitiveID : SV_PrimitiveID;
};

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeIsProgramEntry]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
void InstanceRasterEntry(
    DispatchNodeInputRecord<InstanceRasterRecord> input,
    [NodeID("InstanceMesh")][MaxRecords(1024)] NodeOutput<InstanceMeshRecord> output)
{
    uint triangleCount = input.Get().DrawCount / 3;
    uint groupsPerInstance = (triangleCount + 20) / 21;
    uint totalGroups = groupsPerInstance * input.Get().InstanceCount;
    const uint groupsPerRecord = 65535 * 64;
    uint recordCount = (totalGroups + groupsPerRecord - 1) / groupsPerRecord;
    GroupNodeOutputRecords<InstanceMeshRecord> records = output.GetGroupNodeOutputRecords(recordCount);
    for (uint recordIndex = 0; recordIndex < recordCount; ++recordIndex)
    {
        uint firstGroup = recordIndex * groupsPerRecord;
        uint groupCount = min(totalGroups - firstGroup, groupsPerRecord);
        uint gridWidth = min(groupCount, 65535u);
        records[recordIndex].DispatchGrid = uint3(gridWidth, (groupCount + gridWidth - 1) / gridWidth, 1);
        records[recordIndex].FirstGroup = firstGroup;
        records[recordIndex].GroupsPerInstance = groupsPerInstance;
        records[recordIndex].TriangleCount = triangleCount;
        records[recordIndex].TotalGroups = totalGroups;
    }
    records.OutputComplete();
}

uint LoadInstanceVertexIndex(uint element)
{
    if (IsIndexed == 0)
    {
        return element;
    }
    if (IndexStride == 4)
    {
        return InstanceIndices.Load(element * 4);
    }
    uint byteOffset = element * 2;
    return (InstanceIndices.Load(byteOffset & ~3u) >> ((byteOffset & 2u) * 8)) & 65535u;
}

[Shader("node")]
[NodeLaunch("mesh")]
[NodeID("InstanceMesh")]
[NodeMaxDispatchGrid(65535, 64, 1)]
[NodeMaxInputRecordsPerGraphEntryRecord(1024, false)]
[OutputTopology("triangle")]
[NumThreads(64, 1, 1)]
void InstanceMeshMain(
    uint threadIndex : SV_GroupIndex,
    uint3 groupId : SV_GroupID,
    DispatchNodeInputRecord<InstanceMeshRecord> input,
    out vertices InstanceMeshVertex vertices[63],
    out indices uint3 triangles[21],
    out primitives InstancePrimitive primitiveData[21])
{
    InstanceMeshRecord record = input.Get();
    uint groupIndex = record.FirstGroup + groupId.y * record.DispatchGrid.x + groupId.x;
    uint triangleCount = 0;
    uint instanceIndex = 0;
    uint firstTriangle = 0;
    if (groupIndex < record.TotalGroups)
    {
        instanceIndex = groupIndex / record.GroupsPerInstance;
        firstTriangle = (groupIndex % record.GroupsPerInstance) * 21;
        triangleCount = min(record.TriangleCount - firstTriangle, 21u);
    }
    SetMeshOutputCounts(triangleCount * 3, triangleCount);
    if (threadIndex < triangleCount * 3)
    {
        uint vertexIndex = LoadInstanceVertexIndex(firstTriangle * 3 + threadIndex);
        uint address = vertexIndex * VertexStride;
        VSInput3D vertex;
        vertex.Position = asfloat(InstanceVertices.Load3(address));
        vertex.Normal = asfloat(InstanceVertices.Load3(address + 12));
        vertex.TexCoord = asfloat(InstanceVertices.Load2(address + 24));
        vertex.Diffuse = asfloat(InstanceVertices.Load4(address + 32));
#ifdef INSTANCE_SHADOW_GRAPH
        vertices[threadIndex].Position = InstanceVertexMain(vertex, instanceIndex);
#else
        vertices[threadIndex] = InstanceVertexMain(vertex, instanceIndex);
#endif
    }
    if (threadIndex < triangleCount)
    {
        triangles[threadIndex] = uint3(threadIndex * 3, threadIndex * 3 + 1, threadIndex * 3 + 2);
        primitiveData[threadIndex].PrimitiveID = firstTriangle + threadIndex;
    }
}
