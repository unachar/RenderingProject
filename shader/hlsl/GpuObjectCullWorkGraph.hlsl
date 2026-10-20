#define main CullObjectBody
#include "GpuObjectCullCS.hlsl"
#undef main

struct ObjectCullInput
{
    uint3 DispatchGrid : SV_DispatchGrid;
};

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeIsProgramEntry]
[NodeMaxDispatchGrid(65535, 1, 1)]
[NumThreads(64, 1, 1)]
void CullObjects(DispatchNodeInputRecord<ObjectCullInput> input, uint3 thread : SV_DispatchThreadID)
{
    CullObjectBody(thread);
}
