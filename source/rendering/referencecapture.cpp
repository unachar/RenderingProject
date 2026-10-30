#include "pch.h"
#include "referencecapture.h"

namespace ReferenceCapture
{
    namespace
    {
        Settings g_Settings{};
        State g_State = State::Idle;
        uint32_t g_AccumulatedSamples = 0;

        // splitmix32 finalizer: strong avalanche for seed spreading.
        uint32_t Mix(uint32_t value)
        {
            value ^= value >> 16;
            value *= 0x7feb352du;
            value ^= value >> 15;
            value *= 0x846ca68bu;
            value ^= value >> 16;
            return value;
        }
    } // namespace

    bool Begin(const Settings& settings)
    {
        if (g_State == State::Running)
        {
            return false;
        }
        if (settings.Width == 0 || settings.Height == 0 || settings.SamplesPerPixel == 0 ||
            settings.SamplesPerDispatch == 0)
        {
            return false;
        }
        g_Settings = settings;
        g_AccumulatedSamples = 0;
        g_State = State::Running;
        return true;
    }

    void Cancel()
    {
        if (g_State == State::Running)
        {
            g_State = State::Cancelled;
        }
    }

    bool RecordDispatchComplete()
    {
        if (g_State != State::Running)
        {
            return g_State == State::Complete;
        }
        g_AccumulatedSamples += g_Settings.SamplesPerDispatch;
        if (g_AccumulatedSamples >= g_Settings.SamplesPerPixel)
        {
            g_AccumulatedSamples = g_Settings.SamplesPerPixel;
            g_State = State::Complete;
            return true;
        }
        return false;
    }

    State GetState()
    {
        return g_State;
    }

    const Settings& GetSettings()
    {
        return g_Settings;
    }

    uint32_t GetAccumulatedSamples()
    {
        return g_AccumulatedSamples;
    }

    float Progress()
    {
        if (g_Settings.SamplesPerPixel == 0)
        {
            return 0.0f;
        }
        return static_cast<float>(g_AccumulatedSamples) / static_cast<float>(g_Settings.SamplesPerPixel);
    }

    uint32_t DeriveSeed(uint32_t pixelIndex, uint32_t sampleIndex)
    {
        uint32_t value = g_Settings.Seed + 0x9e3779b9u;
        value = Mix(value ^ pixelIndex);
        value = Mix(value ^ sampleIndex);
        return value;
    }
} // namespace ReferenceCapture
