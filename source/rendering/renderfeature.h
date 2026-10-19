#pragma once

#include <cmath>
#include <cstdint>

// Hardware support only. Shader availability and runtime resource readiness must
// still be checked by each feature before recording GPU commands.
namespace RenderFeature
{
struct Capabilities
{
    bool MeshShader = false;
    bool Raytracing = false;
    bool SamplerFeedback = false;
    bool ReservedResources = false;
};

struct Requirements
{
    bool MeshShader = false;
    bool Raytracing = false;
    bool SamplerFeedback = false;
    bool ReservedResources = false;
};

struct Budget
{
    // Zero means no configured limit; it does not bypass allocation failures.
    uint64_t GpuMemoryBytes = 0;
    uint64_t UploadBytesPerFrame = 0;
    double GpuTimeMs = 0.0;
};

struct Usage
{
    uint64_t GpuMemoryBytes = 0;
    uint64_t UploadBytes = 0;
    double GpuTimeMs = 0.0;
};

enum class Status
{
    Disabled,
    Ready,
    UnsupportedHardware,
    BudgetExceeded,
    InvalidMeasurement
};

inline Status Evaluate(bool enabled, const Capabilities& caps, const Requirements& requirements,
                       const Budget& budget, const Usage& usage)
{
    if (!enabled)
    {
        return Status::Disabled;
    }
    if ((requirements.MeshShader && !caps.MeshShader) || (requirements.Raytracing && !caps.Raytracing) ||
        (requirements.SamplerFeedback && !caps.SamplerFeedback) ||
        (requirements.ReservedResources && !caps.ReservedResources))
    {
        return Status::UnsupportedHardware;
    }
    if (!std::isfinite(usage.GpuTimeMs) || usage.GpuTimeMs < 0.0 ||
        !std::isfinite(budget.GpuTimeMs) || budget.GpuTimeMs < 0.0)
    {
        return Status::InvalidMeasurement;
    }
    if ((budget.GpuMemoryBytes != 0 && usage.GpuMemoryBytes > budget.GpuMemoryBytes) ||
        (budget.UploadBytesPerFrame != 0 && usage.UploadBytes > budget.UploadBytesPerFrame) ||
        (budget.GpuTimeMs > 0.0 && usage.GpuTimeMs > budget.GpuTimeMs))
    {
        return Status::BudgetExceeded;
    }
    return Status::Ready;
}
} // namespace RenderFeature
