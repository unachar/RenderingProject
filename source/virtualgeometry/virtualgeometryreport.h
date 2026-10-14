#pragma once
#include "virtualgeometryasset.h"
#include <filesystem>

class VirtualGeometryReport
{
public:
    static string Text(const VirtualGeometryAsset& asset);
    static bool Save(const filesystem::path& path, const VirtualGeometryAsset& asset, string& error);
};
