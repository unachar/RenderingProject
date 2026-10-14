#pragma once
#include <algorithm>
#include <cmath>

struct ThumbnailCameraFit
{
    float Distance = 0;
    float Near = 0;
    float Far = 0;
    bool Valid = false;
};

inline ThumbnailCameraFit FitThumbnailCamera(float radius)
{
    if (!std::isfinite(radius) || radius < 0) return {};
    const float safeRadius = std::max(radius, 0.0001f);
    const float distance = safeRadius / std::sin(3.14159265358979323846f / 8) * 1.15f;
    return {distance, std::max(safeRadius * 0.01f, 0.000001f), distance + safeRadius * 2.0f, true};
}
