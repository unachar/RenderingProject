#include "pch.h"
#include "daynightcycle.h"
#include "solarcycle.h"
#include "sun.h"
#include "world.h"
#include "projectmanager.h"

namespace
{
    DayNightCycle::Settings g_Settings;
}

DayNightCycle::Settings& DayNightCycle::GetSettings()
{
    return g_Settings;
}

void DayNightCycle::SetTime(float hours)
{
    g_Settings.Hours = SolarCycle::WrapHours(hours);
    g_Settings.TimeDriven = true;
    Update(0.0f);
}

void DayNightCycle::Update(float deltaTime)
{
    if (g_Settings.Running && g_Settings.TimeDriven && !ProjectManager::IsPaused() &&
        (ProjectManager::IsSimulationRunning() || g_Settings.PreviewInEditor))
    {
        const double hoursPerSecond = 24.0 / (max)(1.0f, g_Settings.DayDurationSeconds);
        g_Settings.Hours = SolarCycle::WrapHours(g_Settings.Hours + deltaTime * hoursPerSecond);
    }
    bool firstSun = true;
    for (EntityID entity : World::GetView<SunComponent, TransformComponent, LightComponent>())
    {
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
        const auto& sun = ComponentManager::GetComponentUnchecked<SunComponent>(entity);
        auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
        if (g_Settings.TimeDriven && firstSun)
        {
            const auto direction = SolarCycle::Calculate(g_Settings.Hours, g_Settings.Latitude, g_Settings.DayOfYear);
            const float radius = clamp(g_Settings.OrbitRadius, 10.0f, 1000.0f);
            const XMFLOAT3 position = {sun.Target.x + direction.East * radius, sun.Target.y + direction.Up * radius,
                                       sun.Target.z + direction.North * radius};
            if (position.x != transform.Position.x || position.y != transform.Position.y ||
                position.z != transform.Position.z)
            {
                transform.Position = position;
                transform.IsDirty = true;
            }
        }
        Sun::Sync(entity);
        if (g_Settings.AutomaticLighting && sun.SyncDirectionalLight)
        {
            const float daylight = SolarCycle::Daylight(light.Direction.y);
            const float neutral = SolarCycle::SmoothRange(0.0f, 0.65f, light.Direction.y);
            light.Color = {1.0f, 0.38f + neutral * 0.57f, 0.12f + neutral * 0.73f, 1.0f};
            light.Intensity =
                (max)(0.0f, g_Settings.DayIntensity) * daylight * pow((max)(0.05f, light.Direction.y), 0.35f);
        }
        firstSun = false;
    }
}

XMFLOAT4 DayNightCycle::GetSunDirection()
{
    for (EntityID entity : World::GetView<SunComponent, LightComponent>())
    {
        const auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
        return {light.Direction.x, light.Direction.y, light.Direction.z, 1.0f};
    }
    return {0.0f, 1.0f, 0.0f, 1.0f};
}

float DayNightCycle::GetDaylight()
{
    return SolarCycle::Daylight(GetSunDirection().y);
}
