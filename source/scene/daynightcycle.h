#pragma once

#include <DirectXMath.h>

class DayNightCycle
{
  public:
    struct Settings
    {
        bool TimeDriven = false;
        bool Running = false;
        bool PreviewInEditor = true;
        bool AutomaticLighting = true;
        float Hours = 12.0f;
        float Latitude = 35.0f;
        int DayOfYear = 81;
        float DayDurationSeconds = 120.0f;
        float OrbitRadius = 60.0f;
        float DayIntensity = 3.0f;
        float SkyIntensity = 8.0f;
        float StarBrightness = 0.12f;
    };

    static Settings& GetSettings();
    static void Update(float deltaTime);
    static DirectX::XMFLOAT4 GetSunDirection();
    static float GetDaylight();
    static void SetTime(float hours);
};
