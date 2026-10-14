#pragma once
#include "virtualgeometryasset.h"

class VirtualGeometryValidator
{
public:
    // Explicit error in Debug AND Release. Never logs-and-continues on invalid data.
    static bool Validate(const VirtualGeometryAsset& asset, std::string& error);
};
