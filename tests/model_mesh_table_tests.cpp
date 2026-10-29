#include "editor/modelmeshtable.h"
#include <cassert>
#include <cstdio>
#include <vector>

int main()
{
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800,600);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ModelResourceMap map;
    map.Add(ModelResourceMap::Meshes, "Body", 0);
    for (int frame=0; frame<3; ++frame)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(ImVec2(400,300), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(0,0), ImGuiCond_Always);
        ImGui::Begin("Mesh rows");
        if (ImGui::BeginTable("large", 1, ImGuiTableFlags_ScrollY, ImVec2(300,100)))
        {
            std::vector<uint32_t> rows;
            ForEachVisibleModelMesh(&map, 10000, [&](uint32_t i) {
                rows.push_back(i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Row %u",i);
            });
            assert(!rows.empty() && rows.size() < 100);
            for(size_t i=1;i<rows.size();++i) assert(rows[i] > rows[i-1]);
            ImGui::EndTable();
        }
        ModelResourceMap multiline;
        multiline.Add(ModelResourceMap::Meshes, "Body\nDetail", 0);
        if (ImGui::BeginTable("multiline", 1, ImGuiTableFlags_ScrollY, ImVec2(300,100)))
        {
            unsigned visited=0;
            ForEachVisibleModelMesh(&multiline, 100, [&](uint32_t i) {
                assert(i==visited++);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(i%2 ? "Body" : "Body\nDetail");
            });
            assert(visited==100);
            ImGui::EndTable();
        }
        ImGui::End();
        ImGui::Render();
    }
    ImGui::DestroyContext();
    std::puts("Model mesh tables: PASS (10000 rows clipped; multiline fully preserved)");
}
