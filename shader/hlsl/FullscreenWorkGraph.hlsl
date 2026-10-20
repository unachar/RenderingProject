struct FullscreenEntryRecord
{
    uint Reserved;
};

struct FullscreenMeshRecord
{
    uint3 DispatchGrid : SV_DispatchGrid;
};

struct FullscreenPixelInput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeIsProgramEntry]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
void FullscreenEntry(
    DispatchNodeInputRecord<FullscreenEntryRecord> input,
    [NodeID("FullscreenMesh")][MaxRecords(1)] NodeOutput<FullscreenMeshRecord> output)
{
    GroupNodeOutputRecords<FullscreenMeshRecord> records = output.GetGroupNodeOutputRecords(1);
    records.Get().DispatchGrid = uint3(1, 1, 1);
    records.OutputComplete();
}

[Shader("node")]
[NodeLaunch("mesh")]
[NodeID("FullscreenMesh")]
[NodeMaxDispatchGrid(1, 1, 1)]
[NodeMaxInputRecordsPerGraphEntryRecord(1, false)]
[OutputTopology("triangle")]
[NumThreads(3, 1, 1)]
void FullscreenMeshMain(
    uint vertexIndex : SV_GroupIndex,
    DispatchNodeInputRecord<FullscreenMeshRecord> input,
    out vertices FullscreenPixelInput vertices[3],
    out indices uint3 triangles[1])
{
    SetMeshOutputCounts(3, 1);
    // Match postProcessVS exactly, including the oversized triangle and UV interpolation.
    float2 textureCoordinate = float2((vertexIndex << 1) & 2, vertexIndex & 2);
    vertices[vertexIndex].TexCoord = textureCoordinate;
    vertices[vertexIndex].Position = float4(textureCoordinate * float2(2, -2) + float2(-1, 1), 0, 1);
    if (vertexIndex == 0)
    {
        triangles[0] = uint3(0, 1, 2);
    }
}
