#include "pch.h"
#include "simulationconfiguration.h"
#include "atmosphere.h"
#include "daynightcycle.h"
#include "raytracing.h"
#include "proceduralscene.h"
#include <sstream>
#include <stdexcept>
#include <cmath>

namespace
{
float ReadFloat(std::istream& stream, float minimum, float maximum)
{
    float value = 0.0f;
    if (!(stream >> value) || !std::isfinite(value))
    {
        throw std::invalid_argument("Invalid simulation setting");
    }
    return std::clamp(value, minimum, maximum);
}

void RequireValidRecord(std::istream& stream)
{
    if (stream.fail())
    {
        throw std::invalid_argument("Incomplete simulation setting");
    }
    std::string extra;
    if (stream >> extra)
    {
        throw std::invalid_argument("Unexpected simulation setting field");
    }
}
}

void SimulationConfiguration::Save(std::ostream& stream)
{
    const auto& cycle = DayNightCycle::GetSettings();
    const auto& rays = Raytracing::GetSettings();
    const auto& air = Atmosphere::GetParameters();
    stream << "procedural_live_update=" << ProceduralScene::GetLiveUpdate() << '\n';
    stream << "sun_cycle=" << cycle.TimeDriven << ' ' << cycle.Running << ' ' << cycle.PreviewInEditor << ' '
        << cycle.AutomaticLighting << ' ' << cycle.Hours << ' ' << cycle.Latitude << ' ' << cycle.DayOfYear << ' '
        << cycle.DayDurationSeconds << ' ' << cycle.OrbitRadius << ' ' << cycle.DayIntensity << ' '
        << cycle.SkyIntensity << ' ' << cycle.StarBrightness << '\n';
    stream << "raytracing=" << rays.Enabled << ' ' << rays.Shadows << ' ' << rays.AmbientOcclusion << ' '
        << rays.Samples << ' ' << rays.ShadowDistance << ' ' << rays.AmbientRadius << ' ' << rays.NormalBias << ' '
        << rays.SunAngularRadius << '\n';
    stream << "raytracing_denoise=" << rays.Denoise << '\n';
    stream << "raytracing_reflections=" << rays.Reflections << '\n';
    stream << "atmosphere=" << air.Enabled << ' ' << air.PhysicalSky << ' ' << air.RayleighStrength << ' '
        << air.MieStrength << ' ' << air.Density << ' ' << air.HeightFalloff << ' ' << air.Extinction << ' '
        << air.MieG << ' ' << air.DistanceScale << ' ' << air.LightShaftStrength << ' ' << air.LightShaftBlur << ' '
        << air.AmbientStrength << ' ' << air.RayleighColor.x << ' ' << air.RayleighColor.y << ' ' << air.RayleighColor.z << ' '
        << air.MieColor.x << ' ' << air.MieColor.y << ' ' << air.MieColor.z << '\n';
}

bool SimulationConfiguration::TryLoad(const std::string& key, const std::string& value)
{
    std::istringstream record(value);
    if (key == "procedural_live_update")
    {
        int enabled = -1;
        record >> enabled;
        RequireValidRecord(record);
        if (enabled != 0 && enabled != 1)
        {
            throw std::invalid_argument("Invalid procedural live update setting");
        }
        ProceduralScene::SetLiveUpdate(enabled != 0);
        return true;
    }
    if (key == "sun_cycle")
    {
        DayNightCycle::Settings cycle;
        record >> cycle.TimeDriven >> cycle.Running >> cycle.PreviewInEditor >> cycle.AutomaticLighting;
        cycle.Hours = ReadFloat(record, 0.0f, 24.0f);
        cycle.Latitude = ReadFloat(record, -89.0f, 89.0f);
        record >> cycle.DayOfYear;
        cycle.DayOfYear = std::clamp(cycle.DayOfYear, 1, 365);
        cycle.DayDurationSeconds = ReadFloat(record, 1.0f, 86400.0f);
        cycle.OrbitRadius = ReadFloat(record, 10.0f, 1000.0f);
        cycle.DayIntensity = ReadFloat(record, 0.0f, 20.0f);
        cycle.SkyIntensity = ReadFloat(record, 0.0f, 30.0f);
        cycle.StarBrightness = ReadFloat(record, 0.0f, 1.0f);
        RequireValidRecord(record);
        DayNightCycle::GetSettings() = cycle;
        return true;
    }
    if (key == "raytracing")
    {
        auto rays = Raytracing::GetSettings();
        record >> rays.Enabled >> rays.Shadows >> rays.AmbientOcclusion >> rays.Samples;
        rays.Samples = std::clamp(rays.Samples, 1, 8);
        rays.ShadowDistance = ReadFloat(record, 1.0f, 2000.0f);
        rays.AmbientRadius = ReadFloat(record, 0.05f, 10.0f);
        rays.NormalBias = ReadFloat(record, 0.001f, 0.1f);
        rays.SunAngularRadius = ReadFloat(record, 0.0f, 0.05f);
        RequireValidRecord(record);
        Raytracing::GetSettings() = rays;
        return true;
    }
    if (key == "raytracing_denoise")
    {
        int enabled = -1;
        record >> enabled;
        RequireValidRecord(record);
        if (enabled != 0 && enabled != 1) throw std::invalid_argument("Invalid NRD setting");
        Raytracing::GetSettings().Denoise = enabled != 0;
        return true;
    }
    if (key == "raytracing_reflections")
    {
        int enabled = -1;
        record >> enabled;
        RequireValidRecord(record);
        if (enabled != 0 && enabled != 1) throw std::invalid_argument("Invalid reflection setting");
        Raytracing::GetSettings().Reflections = enabled != 0;
        return true;
    }
    if (key == "atmosphere")
    {
        AtmosphereParameters air;
        record >> air.Enabled >> air.PhysicalSky;
        air.RayleighStrength = ReadFloat(record, 0.0f, 4.0f);
        air.MieStrength = ReadFloat(record, 0.0f, 2.0f);
        air.Density = ReadFloat(record, 0.0f, 3.0f);
        air.HeightFalloff = ReadFloat(record, 0.0f, 1.0f);
        air.Extinction = ReadFloat(record, 0.0f, 2.0f);
        air.MieG = ReadFloat(record, -0.95f, 0.95f);
        air.DistanceScale = ReadFloat(record, 0.001f, 0.25f);
        air.LightShaftStrength = ReadFloat(record, 0.0f, 4.0f);
        air.LightShaftBlur = ReadFloat(record, 0.0f, 1.0f);
        air.AmbientStrength = ReadFloat(record, 0.0f, 1.0f);
        air.RayleighColor.x = ReadFloat(record, 0.0f, 10.0f);
        air.RayleighColor.y = ReadFloat(record, 0.0f, 10.0f);
        air.RayleighColor.z = ReadFloat(record, 0.0f, 10.0f);
        air.MieColor.x = ReadFloat(record, 0.0f, 10.0f);
        air.MieColor.y = ReadFloat(record, 0.0f, 10.0f);
        air.MieColor.z = ReadFloat(record, 0.0f, 10.0f);
        RequireValidRecord(record);
        Atmosphere::GetMutableParameters() = air;
        return true;
    }
    return false;
}
