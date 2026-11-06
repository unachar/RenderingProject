#pragma once
#include "../ecs/ecs.h"
#include <filesystem>
#include <string>
#include <imgui.h>

namespace NiagaraScene
{
    EntityID Create(const std::filesystem::path& path, std::string& error);
    void Update();
    void Draw();
    void DrawBoundsGrid(ImDrawList* draw, ImVec2 origin, ImVec2 size, EntityID selected);
    void Shutdown();
    bool DrawInspector(EntityID entity);
}
