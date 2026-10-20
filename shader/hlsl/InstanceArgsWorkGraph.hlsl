#define INSTANCE_ARGS_WORK_GRAPH
#include "GpuInstanceArgsCS.hlsl"

struct InstanceArgumentRecord
{
    uint Reserved;
};

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeIsProgramEntry]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
void InstanceArgsEntry(DispatchNodeInputRecord<InstanceArgumentRecord> input)
{
    BuildInstanceArguments();
}