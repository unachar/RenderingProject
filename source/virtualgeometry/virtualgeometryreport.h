#pragma once
#include "virtualgeometryasset.h"
#include <filesystem>

class VirtualGeometryReport
{
public:
    static std::string Text(const VirtualGeometryAsset& asset);
    static bool Save(const std::filesystem::path& path, const VirtualGeometryAsset& asset, std::string& error);
};
