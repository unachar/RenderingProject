#define main OcclusionBody
#include "GPUOcclusionCS.hlsl"
#undef main
struct OcclusionInput { uint3 Grid : SV_DispatchGrid; };
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeIsProgramEntry]
[NodeMaxDispatchGrid(65535, 1, 1)]
[NumThreads(64, 1, 1)]
void CullOcclusion(DispatchNodeInputRecord<OcclusionInput> input, uint3 thread : SV_DispatchThreadID)
{
    OcclusionBody(thread);
}
