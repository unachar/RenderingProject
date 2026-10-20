#define main OcclusionBody
#include "GPUOcclusionCS.hlsl"
#undef main
struct ResetInput { uint3 Grid : SV_DispatchGrid; };
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeIsProgramEntry]
[NodeMaxDispatchGrid(1, 1, 1)]
[NumThreads(64, 1, 1)]
void ResetCounters(DispatchNodeInputRecord<ResetInput> input, uint3 thread : SV_DispatchThreadID)
{
    Reset(thread);
}
