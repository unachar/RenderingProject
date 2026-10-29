#pragma once
#include "models/modelresourcemap.h"
#include "imgui.h"
#include <limits>

// Call inside BeginTable/EndTable. Names with line breaks may have variable row
// heights, so preserve the full traversal rather than estimating their height.
template<class Visitor> void ForEachVisibleModelMesh(const ModelResourceMap* map, uint32_t meshCount, Visitor&& visit)
{
    if (!map || map->HasMultilineNames(ModelResourceMap::Meshes) ||
        map->HasMultilineNames(ModelResourceMap::Materials) || meshCount > static_cast<uint32_t>((std::numeric_limits<int>::max)()))
    {
        for (uint32_t i = 0; i < meshCount; ++i) visit(i);
        return;
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(meshCount));
    while (clipper.Step())
    {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) visit(static_cast<uint32_t>(i));
    }
}
