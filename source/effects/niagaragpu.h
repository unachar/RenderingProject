#pragma once
#include "niagara.h"

namespace Niagara
{
    // Synchronous GPU reference backend: CPU schedules spawn/events, compute updates particles.
    // Resources remain retained if a bounded fence wait times out.
    bool UpdateGPU(const System& system, const Emitter& emitter, std::vector<Particle>& particles, float time,
                   float seconds, std::string& diagnostic);
    bool SupportsGPU(const Emitter& emitter, std::string& diagnostic);
    void ShutdownGPU();
}
