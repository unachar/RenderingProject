#include "noiseasset.h"
#include "materialasset.h"
#include <json/json.h>
#include <cmath>
#include <fstream>
#include <limits>
#include <type_traits>

namespace
{
    template <class T> bool Read(const Json::Value& object, const char* key, T& out, std::string& error)
    {
        if (!object.isMember(key))
        {
            return true;
        }
        const auto& value = object[key];
        bool valid = false;
        if constexpr (std::is_enum_v<T>)
        {
            valid = value.isInt();
            if (valid)
            {
                out = static_cast<T>(value.asInt());
            }
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
            valid = value.isBool();
            if (valid)
            {
                out = value.asBool();
            }
        }
        else if constexpr (std::is_same_v<T, uint32_t>)
        {
            valid = value.isUInt();
            if (valid)
            {
                out = value.asUInt();
            }
        }
        else if constexpr (std::is_integral_v<T>)
        {
            valid = value.isInt();
            if (valid)
            {
                out = value.asInt();
            }
        }
        else
        {
            valid = value.isNumeric() && std::isfinite(value.asDouble()) &&
                    std::abs(value.asDouble()) <= std::numeric_limits<float>::max();
            if (valid)
            {
                out = value.asFloat();
            }
        }
        if (!valid)
        {
            error = std::string("Invalid .noise property: ") + key;
        }
        return valid;
    }

    bool ReadColor(const Json::Value& root, const char* key, std::array<float, 3>& color, std::string& error)
    {
        if (!root.isMember(key))
        {
            return true;
        }
        const auto& value = root[key];
        if (!value.isArray() || value.size() != 3)
        {
            error = std::string("Invalid .noise color: ") + key;
            return false;
        }
        for (Json::ArrayIndex i = 0; i < 3; ++i)
        {
            if (!value[i].isNumeric() || !std::isfinite(value[i].asDouble()) || value[i].asDouble() < 0 ||
                value[i].asDouble() > 1)
            {
                error = std::string("Invalid .noise color channel: ") + key;
                return false;
            }
            color[i] = value[i].asFloat();
        }
        return true;
    }
}

bool NoiseSerializer::Validate(const ProcStudio::TextureParams& p, std::string& error)
{
    using namespace ProcStudio;
    error.clear();
    const auto range = [&](double value, double low, double high, const char* name) {
        if (!std::isfinite(value) || value < low || value > high)
        {
            error = std::string("Invalid .noise parameter: ") + name;
            return false;
        }
        return true;
    };
    if (p.Kind == TextureKind::Mandelbrot || p.Kind == TextureKind::Julia)
    {
        error = "Mandelbrot / Julia require CPU image export; .noise supports GPU procedural patterns.";
        return false;
    }
    if (!range(static_cast<int>(p.Kind), 0, static_cast<int>(TextureKind::Count) - 1, "Kind"))
    {
        return false;
    }
    if (!range(static_cast<int>(p.Output), 0, static_cast<int>(TextureOutput::Count) - 1, "Output"))
    {
        return false;
    }
    if (!range(static_cast<int>(p.Pattern), 0, static_cast<int>(GeometricPattern::Count) - 1, "Pattern"))
    {
        return false;
    }
    if (!range(static_cast<int>(p.Noise.BasisType), 0, static_cast<int>(Basis::Count) - 1, "Noise.BasisType"))
    {
        return false;
    }
    if (!range(static_cast<int>(p.Noise.Fractal), 0, static_cast<int>(FractalType::Count) - 1, "Noise.Fractal"))
    {
        return false;
    }
    if (!range(p.Noise.Frequency, 0.001, 64, "Noise.Frequency"))
    {
        return false;
    }
    if (!range(p.Noise.Octaves, 1, 10, "Noise.Octaves"))
    {
        return false;
    }
    if (!range(p.Noise.Lacunarity, 1, 4, "Noise.Lacunarity"))
    {
        return false;
    }
    if (!range(p.Noise.Gain, 0, 1, "Noise.Gain"))
    {
        return false;
    }
    if (!range(p.Noise.WarpStrength, 0, 4, "Noise.WarpStrength"))
    {
        return false;
    }
    if (!range(p.Noise.RidgeOffset, 0, 2, "Noise.RidgeOffset"))
    {
        return false;
    }
    if (!range(p.Noise.Direction, -10000, 10000, "Noise.Direction"))
    {
        return false;
    }
    if (!range(p.Noise.Phase, -10000, 10000, "Noise.Phase"))
    {
        return false;
    }
    if (!range(p.Noise.Anisotropy, 1, 8, "Noise.Anisotropy"))
    {
        return false;
    }
    if (!range(p.Noise.KernelFrequency, 0.01, 8, "Noise.KernelFrequency"))
    {
        return false;
    }
    if (!range(p.Contrast, 0, 10, "Contrast"))
    {
        return false;
    }
    if (!range(p.Bias, -1, 1, "Bias"))
    {
        return false;
    }
    if (!range(p.Turbulence, 0, 20, "Turbulence"))
    {
        return false;
    }
    if (!range(p.RingCount, 1, 64, "RingCount"))
    {
        return false;
    }
    if (!range(p.BricksX, 1, 128, "BricksX"))
    {
        return false;
    }
    if (!range(p.BricksY, 1, 128, "BricksY"))
    {
        return false;
    }
    if (!range(p.RepeatX, 1, 128, "RepeatX"))
    {
        return false;
    }
    if (!range(p.RepeatY, 1, 128, "RepeatY"))
    {
        return false;
    }
    if (!range(p.Mortar, 0, 0.5, "Mortar"))
    {
        return false;
    }
    if (!range(p.PatternSize, 0.01, 1, "PatternSize"))
    {
        return false;
    }
    if (!range(p.PatternSoftness, 1e-05, 1, "PatternSoftness"))
    {
        return false;
    }
    if (!range(p.Time, -1000000.0, 1000000.0, "Time"))
    {
        return false;
    }
    if (!range(p.Choppiness, 0, 1, "Choppiness"))
    {
        return false;
    }
    if (!range(p.Coverage, 0, 1, "Coverage"))
    {
        return false;
    }
    if (!range(p.Softness, 1e-05, 1, "Softness"))
    {
        return false;
    }
    if (!range(p.NormalStrength, 0, 64, "NormalStrength"))
    {
        return false;
    }
    if (!range(p.CenterX, -10000000000.0, 10000000000.0, "CenterX"))
    {
        return false;
    }
    if (!range(p.CenterY, -10000000000.0, 10000000000.0, "CenterY"))
    {
        return false;
    }
    if (!range(p.Zoom, 1e-06, 10000000000.0, "Zoom"))
    {
        return false;
    }
    if (!range(p.JuliaX, -100, 100, "JuliaX"))
    {
        return false;
    }
    if (!range(p.JuliaY, -100, 100, "JuliaY"))
    {
        return false;
    }
    if (!range(p.Iterations, 1, 10000, "Iterations"))
    {
        return false;
    }
    for (const auto& color : {p.ColorA, p.ColorB, p.ColorC})
    {
        for (float channel : color)
        {
            if (!range(channel, 0, 1, "color"))
            {
                return false;
            }
        }
    }
    return true;
}

std::string NoiseSerializer::Encode(const ProcStudio::TextureParams& p)
{
    Json::Value root(Json::objectValue);
    root["version"] = 1;
    root["kind"] = static_cast<int>(p.Kind);
    root["output"] = static_cast<int>(p.Output);
    root["tileable"] = p.Tileable;
    root["contrast"] = p.Contrast;
    root["bias"] = p.Bias;
    root["turbulence"] = p.Turbulence;
    root["ringCount"] = p.RingCount;
    root["mortar"] = p.Mortar;
    root["patternSize"] = p.PatternSize;
    root["patternSoftness"] = p.PatternSoftness;
    root["time"] = p.Time;
    root["choppiness"] = p.Choppiness;
    root["coverage"] = p.Coverage;
    root["softness"] = p.Softness;
    root["centerX"] = p.CenterX;
    root["centerY"] = p.CenterY;
    root["zoom"] = p.Zoom;
    root["juliaX"] = p.JuliaX;
    root["juliaY"] = p.JuliaY;
    root["normalStrength"] = p.NormalStrength;
    root["bricksX"] = p.BricksX;
    root["bricksY"] = p.BricksY;
    root["repeatX"] = p.RepeatX;
    root["repeatY"] = p.RepeatY;
    root["iterations"] = p.Iterations;
    root["pattern"] = static_cast<int>(p.Pattern);
    root["noise"]["basis"] = static_cast<int>(p.Noise.BasisType);
    root["noise"]["fractal"] = static_cast<int>(p.Noise.Fractal);
    root["noise"]["seed"] = p.Noise.Seed;
    root["noise"]["octaves"] = p.Noise.Octaves;
    root["noise"]["frequency"] = p.Noise.Frequency;
    root["noise"]["lacunarity"] = p.Noise.Lacunarity;
    root["noise"]["gain"] = p.Noise.Gain;
    root["noise"]["warpStrength"] = p.Noise.WarpStrength;
    root["noise"]["ridgeOffset"] = p.Noise.RidgeOffset;
    root["noise"]["direction"] = p.Noise.Direction;
    root["noise"]["anisotropy"] = p.Noise.Anisotropy;
    root["noise"]["kernelFrequency"] = p.Noise.KernelFrequency;
    root["noise"]["phase"] = p.Noise.Phase;
    for (float channel : p.ColorA)
    {
        root["colorA"].append(channel);
    }
    for (float channel : p.ColorB)
    {
        root["colorB"].append(channel);
    }
    for (float channel : p.ColorC)
    {
        root["colorC"].append(channel);
    }
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    writer["precision"] = 9;
    return Json::writeString(writer, root) + "\n";
}

bool NoiseSerializer::Load(const std::filesystem::path& path, ProcStudio::TextureParams& params, std::string& error)
{
    error.clear();
    std::error_code sizeError;
    const auto bytes = std::filesystem::file_size(path, sizeError);
    if (sizeError || bytes > 65536)
    {
        error = "Missing or oversized .noise definition: " + path.generic_string();
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    Json::Value root;
    Json::CharReaderBuilder reader;
    reader["rejectDupKeys"] = true;
    reader["failIfExtra"] = true;
    if (!Json::parseFromStream(reader, stream, &root, &error))
    {
        return false;
    }
    if (!root.isObject() || !root["version"].isInt() || root["version"].asInt() != 1)
    {
        error = "Unsupported .noise version (expected 1)";
        return false;
    }
    ProcStudio::TextureParams loaded;
    if (!Read(root, "kind", loaded.Kind, error))
    {
        return false;
    }
    if (!Read(root, "output", loaded.Output, error))
    {
        return false;
    }
    if (!Read(root, "tileable", loaded.Tileable, error))
    {
        return false;
    }
    if (!Read(root, "contrast", loaded.Contrast, error))
    {
        return false;
    }
    if (!Read(root, "bias", loaded.Bias, error))
    {
        return false;
    }
    if (!Read(root, "turbulence", loaded.Turbulence, error))
    {
        return false;
    }
    if (!Read(root, "ringCount", loaded.RingCount, error))
    {
        return false;
    }
    if (!Read(root, "mortar", loaded.Mortar, error))
    {
        return false;
    }
    if (!Read(root, "patternSize", loaded.PatternSize, error))
    {
        return false;
    }
    if (!Read(root, "patternSoftness", loaded.PatternSoftness, error))
    {
        return false;
    }
    if (!Read(root, "time", loaded.Time, error))
    {
        return false;
    }
    if (!Read(root, "choppiness", loaded.Choppiness, error))
    {
        return false;
    }
    if (!Read(root, "coverage", loaded.Coverage, error))
    {
        return false;
    }
    if (!Read(root, "softness", loaded.Softness, error))
    {
        return false;
    }
    if (!Read(root, "centerX", loaded.CenterX, error))
    {
        return false;
    }
    if (!Read(root, "centerY", loaded.CenterY, error))
    {
        return false;
    }
    if (!Read(root, "zoom", loaded.Zoom, error))
    {
        return false;
    }
    if (!Read(root, "juliaX", loaded.JuliaX, error))
    {
        return false;
    }
    if (!Read(root, "juliaY", loaded.JuliaY, error))
    {
        return false;
    }
    if (!Read(root, "normalStrength", loaded.NormalStrength, error))
    {
        return false;
    }
    if (!Read(root, "bricksX", loaded.BricksX, error))
    {
        return false;
    }
    if (!Read(root, "bricksY", loaded.BricksY, error))
    {
        return false;
    }
    if (!Read(root, "repeatX", loaded.RepeatX, error))
    {
        return false;
    }
    if (!Read(root, "repeatY", loaded.RepeatY, error))
    {
        return false;
    }
    if (!Read(root, "iterations", loaded.Iterations, error))
    {
        return false;
    }
    if (!Read(root, "pattern", loaded.Pattern, error))
    {
        return false;
    }
    if (root.isMember("noise") && !root["noise"].isObject())
    {
        error = "Invalid .noise settings object";
        return false;
    }
    const auto& noise = root["noise"];
    if (!Read(noise, "basis", loaded.Noise.BasisType, error))
    {
        return false;
    }
    if (!Read(noise, "fractal", loaded.Noise.Fractal, error))
    {
        return false;
    }
    if (!Read(noise, "seed", loaded.Noise.Seed, error))
    {
        return false;
    }
    if (!Read(noise, "octaves", loaded.Noise.Octaves, error))
    {
        return false;
    }
    if (!Read(noise, "frequency", loaded.Noise.Frequency, error))
    {
        return false;
    }
    if (!Read(noise, "lacunarity", loaded.Noise.Lacunarity, error))
    {
        return false;
    }
    if (!Read(noise, "gain", loaded.Noise.Gain, error))
    {
        return false;
    }
    if (!Read(noise, "warpStrength", loaded.Noise.WarpStrength, error))
    {
        return false;
    }
    if (!Read(noise, "ridgeOffset", loaded.Noise.RidgeOffset, error))
    {
        return false;
    }
    if (!Read(noise, "direction", loaded.Noise.Direction, error))
    {
        return false;
    }
    if (!Read(noise, "anisotropy", loaded.Noise.Anisotropy, error))
    {
        return false;
    }
    if (!Read(noise, "kernelFrequency", loaded.Noise.KernelFrequency, error))
    {
        return false;
    }
    if (!Read(noise, "phase", loaded.Noise.Phase, error))
    {
        return false;
    }
    if (!ReadColor(root, "colorA", loaded.ColorA, error))
    {
        return false;
    }
    if (!ReadColor(root, "colorB", loaded.ColorB, error))
    {
        return false;
    }
    if (!ReadColor(root, "colorC", loaded.ColorC, error))
    {
        return false;
    }
    if (!Validate(loaded, error))
    {
        return false;
    }
    params = loaded;
    return true;
}

bool NoiseSerializer::Save(const std::filesystem::path& path, const ProcStudio::TextureParams& params,
                           std::string& error)
{
    return Validate(params, error) && WriteAssetFile(path, Encode(params), error);
}

std::array<ProcStudio::TextureGpuParams, 3> MaterialNoiseSources::Update(const AssetRegistry& registry,
                                                                         const MaterialAssetData& material,
                                                                         std::string& error)
{
    error.clear();
    std::array<ProcStudio::TextureGpuParams, 3> result{};
    constexpr size_t slots[] = {0, 1, 5};
    for (size_t channel = 0; channel < 3; ++channel)
    {
        auto& cached = m_Sources[channel];
        const auto handle = material.Textures[slots[channel]];
        if (handle && registry.TypeOf(handle) == AssetType::Unknown)
        {
            cached = {};
            error += "Missing material source: " + FormatAssetHandle(handle) + "\n";
            continue;
        }
        if (!handle || registry.TypeOf(handle) != AssetType::Noise)
        {
            cached = {};
            continue;
        }
        const auto path = registry.Resolve(handle);
        std::error_code timeError;
        const auto time = std::filesystem::last_write_time(path, timeError);
        if (timeError || path.empty())
        {
            cached = {};
            error += "Missing .noise definition: " + FormatAssetHandle(handle) + "\n";
            continue;
        }
        if (cached.Handle == handle && cached.Path == path && cached.Time == time && cached.Parameters.Width)
        {
            result[channel] = cached.Parameters;
            continue;
        }
        ProcStudio::TextureParams definition;
        std::string loadError;
        if (!NoiseSerializer::Load(path, definition, loadError))
        {
            cached = {};
            error += path.filename().generic_string() + ": " + loadError + "\n";
            continue;
        }
        auto parameters = ProcStudio::MakeTextureGpuParams(definition);
        parameters.Width = 1; // Material enable flag; never an image resolution.
        cached = {handle, path, time, parameters};
        result[channel] = parameters;
    }
    for (size_t slot : {size_t{2}, size_t{3}, size_t{4}})
    {
        if (registry.TypeOf(material.Textures[slot]) == AssetType::Noise)
        {
            error += ".noise is supported only in Base Color, Normal and Height (POM).\n";
        }
    }
    return result;
}
