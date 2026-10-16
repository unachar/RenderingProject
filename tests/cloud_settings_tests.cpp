#include "cloudsettings.h"
#include <cassert>
#include <cmath>
#include <limits>

int main()
{
    CloudSettings layered;
    assert(layered.LayerCount == 1);
    assert(layered.Layer(0).LayerEnabled);
    assert(layered.AddLayer());
    layered.Layer(0).BottomHeight = 10;
    layered.Layer(0).TopHeight = 30;
    layered.Layer(1).BottomHeight = 40;
    layered.Layer(1).TopHeight = 50;
    layered.Layer(1).Density = 0.25f;
    layered.Validate();
    assert(layered.Layer(0).TopHeight == 30);
    assert(layered.Layer(1).BottomHeight == 40);
    assert(layered.Layer(1).Density == 0.25f);
    layered.Layer(1).TopHeight = 39;
    layered.Layer(1).BaseScale = std::numeric_limits<float>::quiet_NaN();
    layered.Validate();
    assert(layered.Layer(1).TopHeight > 40);
    assert(std::isfinite(layered.Layer(1).BaseScale));
    assert(layered.AddLayer());
    assert(layered.AddLayer());
    assert(!layered.AddLayer());
    layered.Layer(2).Density = 2.5f;
    assert(layered.RemoveLayer(1));
    assert(layered.Layer(1).Density == 2.5f);
    assert(layered.RemoveLayer(0));
    assert(layered.Density == 2.5f);
    assert(layered.RemoveLayer(0));
    assert(!layered.RemoveLayer(0));
    layered.LayerCount = 99;
    layered.Validate();
    assert(layered.LayerCount == CloudSettings::MaxLayers);
    layered.LayerCount = 0;
    layered.Validate();
    assert(layered.LayerCount == 1);

    CloudSettings settings;
    settings.Coverage = -1;
    settings.TopHeight = settings.BottomHeight;
    settings.PhaseG = 1;
    settings.ViewSteps = 0;
    settings.LightSteps = 999;
    settings.WindX = settings.WindZ = 0;
    settings.BaseScale = std::numeric_limits<float>::quiet_NaN();
    settings.Validate();
    assert(settings.Coverage == 0);
    assert(settings.TopHeight > settings.BottomHeight);
    assert(settings.PhaseG <= 0.9f);
    assert(settings.ViewSteps >= 1 && settings.LightSteps <= 16);
    assert(std::isfinite(settings.BaseScale) && settings.BaseScale > 0);
    assert(settings.WindX == 0 && settings.WindZ == 0);
    settings.ApplyQuality(CloudQuality::Low);
    const int low = settings.ViewSteps * settings.LightSteps;
    settings.ApplyQuality(CloudQuality::Medium);
    assert(settings.ViewSteps * settings.LightSteps > low);
    assert(settings.ResolutionScale == 0.5f);
    settings.ApplyQuality(CloudQuality::High);
    assert(settings.ViewSteps == 96 && settings.LightSteps == 8);
    settings.ApplyPreset(CloudPreset::Clear);
    assert(settings.Coverage == 0);
    settings.ApplyPreset(CloudPreset::Overcast);
    assert(settings.Coverage > 0.8f);
}
