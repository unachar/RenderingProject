#pragma once
#include "virtualgeometryasset.h"

class VirtualGeometryValidator
{
  public:
    // Debug と Release の両方で明示的なエラーにする。不正データをログ出力だけで続行しない。
    static bool Validate(const VirtualGeometryAsset& asset, std::string& error);
};
