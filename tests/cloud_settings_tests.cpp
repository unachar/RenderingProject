#include "cloudsettings.h"
#include <cassert>
#include <cmath>
#include <limits>

int main()
{
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
