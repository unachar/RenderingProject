// AOV entry point of the reference integrator (see
// rendering-technology-stage2h-design.md): compiles
// ReferencePathTraceCS.hlsl with the AOV main instead of the
// beauty main.
#define REFERENCE_AOV_ENTRY
#include "ReferencePathTraceCS.hlsl"
