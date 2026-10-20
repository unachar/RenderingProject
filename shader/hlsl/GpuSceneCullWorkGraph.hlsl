#define main CullSceneBody
#include "GpuSceneCullCS.hlsl"
#undef main

struct SceneCullInput
{
    uint3 DispatchGrid : SV_DispatchGrid;
};

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeIsProgramEntry]
[NodeMaxDispatchGrid(65535, 1, 1)]
[NumThreads(64, 1, 1)]
void CullScene(DispatchNodeInputRecord<SceneCullInput> input, uint3 thread : SV_DispatchThreadID)
{
    CullSceneBody(thread);
}
