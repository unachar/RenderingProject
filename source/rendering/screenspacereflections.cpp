#include "pch.h"
#include "screenspacereflections.h"

namespace ScreenSpaceReflections
{
    Source SelectSource(bool dxrReflectionActive, bool ssrRequested, bool ssrResultReady)
    {
        if (dxrReflectionActive)
        {
            return Source::Raytraced;
        }
        if (ssrRequested && ssrResultReady)
        {
            return Source::ScreenSpace;
        }
        return Source::Environment;
    }
} // namespace ScreenSpaceReflections
