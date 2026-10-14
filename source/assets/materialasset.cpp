#include "materialasset.h"
#include <json/json.h>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <windows.h>

std::string FormatAssetHandle(AssetHandle handle)
{
    std::ostringstream output;
    output << "guid:" << std::hex << std::setw(16) << std::setfill('0') << handle;
    return output.str();
}

AssetHandle ParseAssetHandle(const std::string& text)
{
    if (text.size() != 21 || text.substr(0, 5) != "guid:")
    {
        return 0;
    }
    AssetHandle handle = 0;
    const auto result = std::from_chars(text.data() + 5, text.data() + text.size(), handle, 16);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
    {
        return 0;
    }
    return handle;
}

bool WriteAssetFile(const std::filesystem::path& path, const std::string& contents, std::string& error)
{
    error.clear();
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        stream.flush();
        if (!stream)
        {
            error = "Failed to write " + path.generic_string();
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        error = "Failed to replace " + path.generic_string() + " (" + std::to_string(GetLastError()) + ")";
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

bool MaterialSerializer::Validate(const MaterialAssetData& material, std::string& error)
{
    error.clear();
    auto range = [&](float value, float minimum, float maximum, const char* property)
    {
        if (!std::isfinite(value) || value < minimum || value > maximum)
        {
            error = std::string("Invalid ") + property;
            return false;
        }
        return true;
    };
    for (float channel : material.BaseColor)
    {
        if (!range(channel, 0, 1, "baseColor"))
        {
            return false;
        }
    }
    for (float channel : material.EmissiveColor)
    {
        if (!range(channel, 0, 65504, "emissiveColor"))
        {
            return false;
        }
    }
    if (!range(material.Metallic, 0, 1, "metallic") ||
        !range(material.Roughness, 0, 1, "roughness") ||
        !range(material.NormalScale, 0, 8, "normalScale") ||
        !range(material.AOStrength, 0, 1, "aoStrength") ||
        !range(material.EmissiveIntensity, 0, 65504, "emissiveIntensity") ||
        !range(material.AlphaCutoff, 0, 1, "alphaCutoff"))
    {
        return false;
    }
    if (material.Surface > MaterialSurface::Transparent || material.Cull > MaterialCull::Front)
    {
        error = "Unsupported render state";
        return false;
    }
    return true;
}

namespace
{
constexpr const char* textureNames[] = {"baseColor", "normal", "metallicRoughness", "ao", "emissive"};
bool ReadFloat(const Json::Value& object, const char* name, float& value, std::string& error)
{
    if (!object.isMember(name))
    {
        return true;
    }
    if (!object[name].isNumeric())
    {
        error = std::string("Invalid ") + name + " property";
        return false;
    }
    value = object[name].asFloat();
    return true;
}
template<size_t Count>
bool ReadColor(const Json::Value& object, const char* name, std::array<float, Count>& color, std::string& error)
{
    if (!object.isMember(name))
    {
        return true;
    }
    const auto& array = object[name];
    if (!array.isArray() || array.size() != Count)
    {
        error = std::string("Invalid ") + name + " array";
        return false;
    }
    for (Json::ArrayIndex i = 0; i < Count; ++i)
    {
        if (!array[i].isNumeric())
        {
            error = std::string("Invalid ") + name + " channel";
            return false;
        }
        color[i] = array[i].asFloat();
    }
    return true;
}
}

bool MaterialSerializer::Load(const std::filesystem::path& path, MaterialAssetData& material, std::string& error)
{
    error.clear();
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        error = "Failed to open " + path.generic_string();
        return false;
    }
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
        error = "Unsupported material version (expected 1)";
        return false;
    }
    if (root.isMember("shader") && root["shader"] != "PBR")
    {
        error = "Only the PBR shader is supported";
        return false;
    }
    MaterialAssetData result;
    const auto& parameters = root["parameters"];
    const auto& renderState = root["renderState"];
    const auto& textures = root["textures"];
    for (const char* object : {"parameters", "renderState", "textures"})
    {
        if (root.isMember(object) && !root[object].isObject())
        {
            error = std::string("Invalid ") + object + " object";
            return false;
        }
    }
    if (!ReadColor(parameters, "baseColor", result.BaseColor, error) ||
        !ReadColor(parameters, "emissiveColor", result.EmissiveColor, error) ||
        !ReadFloat(parameters, "metallic", result.Metallic, error) ||
        !ReadFloat(parameters, "roughness", result.Roughness, error) ||
        !ReadFloat(parameters, "normalScale", result.NormalScale, error) ||
        !ReadFloat(parameters, "aoStrength", result.AOStrength, error) ||
        !ReadFloat(parameters, "emissiveIntensity", result.EmissiveIntensity, error) ||
        !ReadFloat(renderState, "alphaCutoff", result.AlphaCutoff, error))
    {
        return false;
    }
    if (root.isMember("surfaceType"))
    {
        const auto& surface = root["surfaceType"];
        if (surface == "Masked")
        {
            result.Surface = MaterialSurface::Masked;
        }
        else if (surface == "Transparent")
        {
            result.Surface = MaterialSurface::Transparent;
        }
        else if (surface != "Opaque")
        {
            error = "Unsupported surfaceType";
            return false;
        }
    }
    if (renderState.isMember("cullMode"))
    {
        const auto& cull = renderState["cullMode"];
        if (cull == "None")
        {
            result.Cull = MaterialCull::None;
        }
        else if (cull == "Front")
        {
            result.Cull = MaterialCull::Front;
        }
        else if (cull != "Back")
        {
            error = "Invalid cullMode";
            return false;
        }
    }
    if (renderState.isMember("depthWrite"))
    {
        if (!renderState["depthWrite"].isBool())
        {
            error = "Invalid depthWrite";
            return false;
        }
        result.DepthWrite = renderState["depthWrite"].asBool();
    }
    for (size_t slot = 0; slot < result.Textures.size(); ++slot)
    {
        const auto& reference = textures[textureNames[slot]];
        if (reference.isNull())
        {
            continue;
        }
        if (!reference.isString())
        {
            error = std::string("Invalid texture reference: ") + textureNames[slot];
            return false;
        }
        const auto text = reference.asString();
        result.Textures[slot] = ParseAssetHandle(text);
        if (result.Textures[slot] == 0 && text != FormatAssetHandle(0) && !text.empty())
        {
            error = std::string("Invalid texture GUID: ") + textureNames[slot];
            return false;
        }
    }
    if (!Validate(result, error))
    {
        return false;
    }
    material = result;
    return true;
}

bool MaterialSerializer::Save(const std::filesystem::path& path, const MaterialAssetData& material, std::string& error)
{
    if (!Validate(material, error))
    {
        return false;
    }
    Json::Value root(Json::objectValue);
    root["version"] = 1;
    root["shader"] = "PBR";
    root["surfaceType"] = "Opaque";
    if (material.Surface == MaterialSurface::Masked)
    {
        root["surfaceType"] = "Masked";
    }
    else if (material.Surface == MaterialSurface::Transparent)
    {
        root["surfaceType"] = "Transparent";
    }
    const char* cullNames[] = {"Back", "None", "Front"};
    auto& state = root["renderState"];
    state["cullMode"] = cullNames[static_cast<size_t>(material.Cull)];
    state["depthWrite"] = material.DepthWrite;
    state["alphaCutoff"] = material.AlphaCutoff;
    auto& parameters = root["parameters"];
    for (float value : material.BaseColor)
    {
        parameters["baseColor"].append(value);
    }
    for (float value : material.EmissiveColor)
    {
        parameters["emissiveColor"].append(value);
    }
    parameters["metallic"] = material.Metallic;
    parameters["roughness"] = material.Roughness;
    parameters["normalScale"] = material.NormalScale;
    parameters["aoStrength"] = material.AOStrength;
    parameters["emissiveIntensity"] = material.EmissiveIntensity;
    for (size_t slot = 0; slot < material.Textures.size(); ++slot)
    {
        root["textures"][textureNames[slot]] = FormatAssetHandle(material.Textures[slot]);
    }
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    return WriteAssetFile(path, Json::writeString(writer, root), error);
}
