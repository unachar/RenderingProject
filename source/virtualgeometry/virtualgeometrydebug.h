#pragma once
#include <cstdint>
struct ImDrawList;
struct ImVec2;
namespace VirtualGeometryDebug
{
void DrawControls();
void DrawViewport(ImDrawList *draw, const ImVec2 &origin, const ImVec2 &size, uint32_t selectedEntity);
} // namespace VirtualGeometryDebug
