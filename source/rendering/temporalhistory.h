#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace TemporalHistory
{
enum Reason : uint32_t
{
    None = 0,
    FirstFrame = 1u << 0,
    FrameGap = 1u << 1,
    CameraChanged = 1u << 2,
    ResolutionChanged = 1u << 3,
    SceneChanged = 1u << 4,
    LightingChanged = 1u << 5,
    LodChanged = 1u << 6,
    ExposureChanged = 1u << 7,
    ConfigurationChanged = 1u << 8,
    SignalChanged = 1u << 9
};

struct State
{
    uint64_t Serial = 0;
    uint64_t CameraId = 0;
    uint64_t CameraCutGeneration = 0;
    uint64_t SceneGeneration = 0;
    uint64_t LightingGeneration = 0;
    uint64_t LodGeneration = 0;
    uint64_t ConfigurationKey = 0;
    uint32_t InputWidth = 0;
    uint32_t InputHeight = 0;
    uint32_t OutputWidth = 0;
    uint32_t OutputHeight = 0;
    double Exposure = 1.0;
    bool CameraCut = false;
};

struct Decision
{
    uint32_t Reasons = None;
    bool CanReuse = false;
};

// Uncompensated exposure or a topology change invalidates the whole signal.
// Ordinary camera/object movement is reprojected by the caller and does not
// increment a generation. Local rejection requires a separate per-pixel test.
inline Decision Evaluate(const State& previous, const State& current, bool valid)
{
    uint32_t reasons = valid ? None : FirstFrame;
    if (valid && (previous.Serial == (std::numeric_limits<uint64_t>::max)() ||
        current.Serial != previous.Serial + 1))
        reasons |= FrameGap;
    if (previous.CameraId != current.CameraId || current.CameraCut ||
        previous.CameraCutGeneration != current.CameraCutGeneration)
        reasons |= CameraChanged;
    if (current.InputWidth == 0 || current.InputHeight == 0 ||
        current.OutputWidth == 0 || current.OutputHeight == 0 ||
        previous.InputWidth != current.InputWidth || previous.InputHeight != current.InputHeight ||
        previous.OutputWidth != current.OutputWidth || previous.OutputHeight != current.OutputHeight)
        reasons |= ResolutionChanged;
    if (previous.SceneGeneration != current.SceneGeneration) reasons |= SceneChanged;
    if (previous.LightingGeneration != current.LightingGeneration) reasons |= LightingChanged;
    if (previous.LodGeneration != current.LodGeneration) reasons |= LodChanged;
    if (!std::isfinite(previous.Exposure) || !std::isfinite(current.Exposure) ||
        previous.Exposure <= 0.0 || current.Exposure <= 0.0 || previous.Exposure != current.Exposure)
        reasons |= ExposureChanged;
    if (previous.ConfigurationKey != current.ConfigurationKey) reasons |= ConfigurationChanged;
    return {reasons, reasons == None};
}
}
