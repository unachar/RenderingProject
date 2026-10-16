#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

enum class CloudQuality { Low, Medium, High };
enum class CloudPreset { Clear, Fair, Cloudy, Overcast, Storm };
enum class CloudDebug { Normal, Density, Transmittance, Lighting, Depth, Steps, BaseNoise, DetailNoise };

// The inherited first layer preserves the existing settings and validation API.
struct CloudLayerSettings
{
    bool LayerEnabled = true;
    float BottomHeight = 20.0f;
    float TopHeight = 40.0f;
    float Coverage = 0.55f;
    float Density = 1.0f;
    float BaseScale = 0.006f;
    float DetailScale = 0.04f;
    float DetailStrength = 0.18f;

    void ValidateLayer()
    {
        auto bounded = [](float value, float fallback, float low, float high)
        {
            return std::clamp(std::isfinite(value) ? value : fallback, low, high);
        };
        BottomHeight = bounded(BottomHeight, 20, -10000, 10000);
        TopHeight = bounded(TopHeight, 40, BottomHeight + 0.01f, 20000);
        Coverage = bounded(Coverage, 0.55f, 0, 1);
        Density = bounded(Density, 1, 0, 10);
        BaseScale = bounded(BaseScale, 0.006f, 0.00001f, 1);
        DetailScale = bounded(DetailScale, 0.04f, 0.00001f, 4);
        DetailStrength = bounded(DetailStrength, 0.18f, 0, 1);
    }
};

struct CloudSettings : CloudLayerSettings
{
    static constexpr int MaxLayers = 4;
    int LayerCount = 1;
    CloudLayerSettings AdditionalLayers[MaxLayers - 1];
    bool Enabled = false;
    bool CastShadows = true;
    float Extinction = 0.12f;
    float PhaseG = 0.65f;
    float AmbientStrength = 0.5f;
    float PowderStrength = 0.0f;
    float JitterStrength = 1.0f;
    float WindX = 1.0f;
    float WindZ = 0.25f;
    float WindSpeed = 1.0f;
    float MaxTraceDistance = 2000.0f;
    float ResolutionScale = 0.5f;
    float NoiseSlice = 0.5f;
    int ViewSteps = 64;
    int LightSteps = 6;
    uint32_t Seed = 1337;
    CloudDebug Debug = CloudDebug::Normal;

    CloudLayerSettings& Layer(int index)
    {
        if (index == 0)
        {
            return *this;
        }
        return AdditionalLayers[index - 1];
    }

    bool AddLayer()
    {
        if (LayerCount >= MaxLayers)
        {
            return false;
        }
        const float previousTop = Layer(LayerCount - 1).TopHeight;
        auto& layer = Layer(LayerCount);
        layer = CloudLayerSettings{};
        layer.BottomHeight = previousTop + 10.0f;
        layer.TopHeight = layer.BottomHeight + 10.0f;
        ++LayerCount;
        Validate();
        return true;
    }

    bool RemoveLayer(int index)
    {
        if (LayerCount <= 1 || index < 0 || index >= LayerCount)
        {
            return false;
        }
        for (int next = index + 1; next < LayerCount; ++next)
        {
            Layer(next - 1) = Layer(next);
        }
        --LayerCount;
        Layer(LayerCount) = CloudLayerSettings{};
        return true;
    }

    void ApplyQuality(CloudQuality quality)
    {
        ViewSteps = quality == CloudQuality::Low ? 40 : quality == CloudQuality::Medium ? 64 : 96;
        LightSteps = quality == CloudQuality::Low ? 4 : quality == CloudQuality::Medium ? 6 : 8;
        ResolutionScale = 0.5f;
    }

    void ApplyPreset(CloudPreset preset)
    {
        constexpr float coverage[] = { 0.0f, 0.35f, 0.6f, 0.95f, 1.0f };
        constexpr float density[] = { 1.0f, 0.7f, 1.0f, 1.4f, 2.0f };
        const int index = std::clamp(static_cast<int>(preset), 0, 4);
        Coverage = coverage[index];
        Density = density[index];
    }

    void Validate()
    {
        auto bounded = [](float value, float fallback, float low, float high)
        {
            return std::clamp(std::isfinite(value) ? value : fallback, low, high);
        };
        LayerCount = std::clamp(LayerCount, 1, MaxLayers);
        ValidateLayer();
        for (auto& layer : AdditionalLayers)
        {
            layer.ValidateLayer();
        }
        Extinction = bounded(Extinction, 0.12f, 0, 10);
        PhaseG = bounded(PhaseG, 0.65f, -0.9f, 0.9f);
        AmbientStrength = bounded(AmbientStrength, 0.5f, 0, 10);
        PowderStrength = bounded(PowderStrength, 0, 0, 2);
        JitterStrength = bounded(JitterStrength, 1, 0, 1);
        WindX = bounded(WindX, 1, -1, 1);
        WindZ = bounded(WindZ, 0.25f, -1, 1);
        WindSpeed = bounded(WindSpeed, 1, 0, 1000);
        MaxTraceDistance = bounded(MaxTraceDistance, 2000, 0.1f, 100000);
        ResolutionScale = bounded(ResolutionScale, 0.5f, 0.25f, 1);
        NoiseSlice = bounded(NoiseSlice, 0.5f, 0, 1);
        ViewSteps = std::clamp(ViewSteps, 1, 192);
        LightSteps = std::clamp(LightSteps, 1, 16);
        Debug = static_cast<CloudDebug>(std::clamp(static_cast<int>(Debug), 0, 7));
    }
};

