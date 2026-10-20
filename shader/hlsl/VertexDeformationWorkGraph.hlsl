#define VERTEX_DEFORMATION_WORK_GRAPH
#include "VertexDeformationCS.hlsl"

struct VertexDeformationRecord
{
    uint3 DispatchGrid : SV_DispatchGrid;
};

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeIsProgramEntry]
[NodeMaxDispatchGrid(65535, 256, 1)]
[NumThreads(64, 1, 1)]
void VertexDeformationEntry(
    DispatchNodeInputRecord<VertexDeformationRecord> input,
    uint3 group : SV_GroupID,
    uint thread : SV_GroupIndex)
{
    DeformVertex(group, thread);
}
