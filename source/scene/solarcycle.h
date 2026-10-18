#pragma once

#include <algorithm>
#include <cmath>

namespace SolarCycle
{
constexpr float Pi = 3.14159265358979323846f;

struct Position
{
    float East = 0.0f;
    float Up = 1.0f;
    float North = 0.0f;
};

inline float WrapHours(double hours)
{
    if (!isfinite(hours))
    {
        return 12.0f;
    }
    double wrapped = fmod(hours, 24.0);
    if (wrapped < 0.0)
    {
        wrapped += 24.0;
    }
    return static_cast<float>(wrapped);
}

// Local solar time; longitude, time zones and the equation of time are excluded.
inline Position Calculate(float hours, float latitudeDegrees, int dayOfYear)
{
    const float latitude = std::clamp(latitudeDegrees, -89.0f, 89.0f) * Pi / 180.0f;
    const float season = static_cast<float>(std::clamp(dayOfYear, 1, 365) - 81) / 365.0f;
    const float declination = 23.44f * Pi / 180.0f * sin(2.0f * Pi * season);
    const float hourAngle = (WrapHours(hours) - 12.0f) * Pi / 12.0f;
    Position result;
    result.East = -cos(declination) * sin(hourAngle);
    result.Up = sin(latitude) * sin(declination) + cos(latitude) * cos(declination) * cos(hourAngle);
    result.North = cos(latitude) * sin(declination) - sin(latitude) * cos(declination) * cos(hourAngle);
    return result;
}

inline float SmoothRange(float lower, float upper, float value)
{
    const float weight = std::clamp((value - lower) / (upper - lower), 0.0f, 1.0f);
    return weight * weight * (3.0f - 2.0f * weight);
}

inline float Daylight(float sunHeight)
{
    return SmoothRange(-0.08f, 0.16f, sunHeight);
}
} // namespace SolarCycle
