#pragma once

namespace fastgltf
{
    class Asset;
}

// Internal to FastGltfAdapter.dll; no SDK or STL objects cross the application boundary.
namespace GltfDraco
{
    bool Expand(fastgltf::Asset& asset);
}
