#define INSTANCE_CULL_WORK_GRAPH
#include "GpuInstanceCullLodCS.hlsl"

struct InstanceCullRecord
{
    uint3 DispatchGrid : SV_DispatchGrid;
};

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeIsProgramEntry]
[NodeMaxDispatchGrid(512, 1, 1)]
[NumThreads(64, 1, 1)]
void InstanceCullEntry(DispatchNodeInputRecord<InstanceCullRecord> input,
                       uint3 dispatchThreadId : SV_DispatchThreadID)
{
    CullInstance(dispatchThreadId);
}