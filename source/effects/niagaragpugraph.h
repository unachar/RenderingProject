#pragma once
#include "niagara.h"

namespace Niagara
{
    // Emits a scoped block for the GPU simulation kernel. The caller supplies
    // Particle p, Time, Dt, t and StructuredBuffer<float4> NiagaraParameters at t0.
    // Parameter indices follow System.Parameters. SizeInitialSeed.w stores raw uint32 particle ID bits.
    bool GenerateGPUGraph(const Module& module, const System& system, std::string& hlsl, std::string& diagnostic);
}
