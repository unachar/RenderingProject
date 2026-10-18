#pragma once
#include <algorithm>
#include <cmath>

namespace VirtualGeometryTemporal
{
inline bool Refine(double errorPixels, float targetPixels, float fraction, bool previousRefine, bool historyValid)
{
    float target = std::isfinite(targetPixels) && targetPixels > 0 ? targetPixels : 1.0f;
    float width = std::isfinite(fraction) ? std::clamp(fraction, 0.0f, 0.5f) : 0.1f;
    float threshold = target;
    if (historyValid)
    {
        threshold *= previousRefine ? 1.0f - width : 1.0f + width;
    }
    return errorPixels > threshold;
}
} // namespace VirtualGeometryTemporal
