#define main SelectLodBody
#include "GPULODSelectionCS.hlsl"
#undef main
struct LodInput { uint3 Grid : SV_DispatchGrid; };
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeIsProgramEntry]
[NodeMaxDispatchGrid(65535, 1, 1)]
[NumThreads(64, 1, 1)]
void SelectLod(DispatchNodeInputRecord<LodInput> input, uint3 thread : SV_DispatchThreadID)
{
    SelectLodBody(thread);
}
