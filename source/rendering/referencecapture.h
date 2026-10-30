#pragma once
#include <cstdint>

struct ID3D12Device;
struct ID3D12Resource;

// Stage 2 H: bookkeeping for offline reference path-trace capture
// jobs (see docs/rendering-technology-stage2h-design.md). This module
// owns the job state machine, sample accounting and the deterministic
// seed derivation the integrator shader shares; the GPU integrator
// itself lands in a later step.
namespace ReferenceCapture
{
    struct Settings
    {
        uint32_t Width = 512;
        uint32_t Height = 512;
        uint32_t SamplesPerPixel = 1024;
        uint32_t MaxDepth = 8;
        uint32_t Seed = 1234;
        // Samples accumulated per integrator dispatch (progressive
        // refinement granularity and cancellation responsiveness).
        uint32_t SamplesPerDispatch = 4;
    };

    enum class State
    {
        Idle,
        Running,
        Complete,
        Cancelled,
    };

    // Starts a job; returns false when a job is already running or
    // the settings are invalid (zero size / zero samples).
    bool Begin(const Settings& settings);
    void Cancel();
    // The integrator calls this after each dispatch; returns true
    // when the job has accumulated all requested samples.
    bool RecordDispatchComplete();

    State GetState();
    const Settings& GetSettings();
    uint32_t GetAccumulatedSamples();
    float Progress(); // 0..1

    // Deterministic per-sample seed: a pure function of the job seed,
    // the pixel index and the sample index, shared verbatim with the
    // integrator shader so captures are reproducible across runs and
    // machines.
    uint32_t DeriveSeed(uint32_t pixelIndex, uint32_t sampleIndex);
} // namespace ReferenceCapture
