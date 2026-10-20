#pragma once

#include <cmath>
#include <cstdint>

// ハードウェア対応の有無だけを示す。GPU コマンドを記録する前に各機能でシェーダーの可用性と
// 実行時リソースの準備完了を確認する必要がある。
namespace RenderFeature
{
    struct Capabilities
    {
        bool MeshShader = false;
        bool Raytracing = false;
        bool SamplerFeedback = false;
        bool ReservedResources = false;
        bool BindlessResources = false;
        // SM6.2 のネイティブ 16bit 演算（-enable-16bit-types）。Requirements には含めない（各シェーダーが自動で退避）。
        bool Native16BitShaderOps = false;
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
        // ゼロは設定上の上限なしを表すが、確保失敗を無視する意味ではない。
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
        if (!std::isfinite(usage.GpuTimeMs) || usage.GpuTimeMs < 0.0 || !std::isfinite(budget.GpuTimeMs) ||
            budget.GpuTimeMs < 0.0)
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
} // 名前空間 RenderFeature
