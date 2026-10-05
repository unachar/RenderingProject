"""Headless regression checks using the bundled Dear ImGui and actual editor methods.

Run from a Visual Studio x64 Developer Command Prompt:
    python tests/run_editor_ui_tests.py
No D3D device or application window is created. This does not verify GPU rendering.
"""
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def method(path, signature):
    text = (ROOT / path).read_text(encoding="utf-8-sig")
    start = text.index(signature)
    body = text.index("{", start)
    depth = 1
    end = body + 1
    while depth:
        if text[end] == "{":
            depth += 1
        elif text[end] == "}":
            depth -= 1
        end += 1
    return text[start:end]


def main():
    compiler = shutil.which("cl")
    if not compiler:
        raise SystemExit("Run this test in a Visual Studio x64 Developer Command Prompt (cl required).")
    output = ROOT / "bin" / "uiux-tests"
    output.mkdir(parents=True, exist_ok=True)
    source = output / "editor_ui_tests.cpp"
    source.write_text(r'''
#include <algorithm>
#include <cassert>
#include <cstdio>
#include "imgui.h"
#include "imgui_internal.h"
using std::max;
class ImGuiManager {
public:
    inline static bool m_ResetEditorLayout = false;
    static void DrawDockSpace();
    static void StyleModernSlim();
};
namespace EditorWidgets {
    void DrawSearchField(const char*, const char*, ImGuiTextFilter&);
}
''' + method("source/editor/imguimanager.cpp", "void ImGuiManager::DrawDockSpace()")
        + "\n" + method("source/editor/imguimanager.cpp", "void ImGuiManager::StyleModernSlim()")
        + "\nnamespace EditorWidgets {\n"
        + method("source/editor/editorwidgets.cpp", "void DrawSearchField(") + "\n}\n"
        + r'''
static void Frame() {
    ImGui::NewFrame();
    ImGuiManager::DrawDockSpace();
    const char* windows[] = {"ヒエラルキー", "シーンビュー", "インスペクター", "レンダーコントロール", "プロジェクト", "ログ"};
    for (const char* name : windows) {
        ImGui::Begin(name);
        ImGui::TextUnformatted("Content");
        ImGui::End();
    }
    ImGui::Render();
}
static ImVec2 SearchFrame(ImGuiTextFilter& filter) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(50, 50));
    ImGui::SetNextWindowSize(ImVec2(400, 150));
    ImGui::Begin("Search");
    EditorWidgets::DrawSearchField("TestSearch", "Search...", filter);
    const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    ImGui::End(); ImGui::Render();
    return ImVec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
}
int main() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1200, 800);
    io.DeltaTime = 1.0f / 60.0f;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGuiManager::StyleModernSlim();
    Frame(); Frame();
    auto* hierarchy = ImGui::FindWindowByName("ヒエラルキー");
    auto* scene = ImGui::FindWindowByName("シーンビュー");
    auto* inspector = ImGui::FindWindowByName("インスペクター");
    auto* assets = ImGui::FindWindowByName("プロジェクト");
    assert(hierarchy->DockId && inspector->DockId && assets->DockId && scene->DockId);
    assert(hierarchy->DockId != scene->DockId && inspector->DockId != scene->DockId);
    assert(assets->DockId != scene->DockId);
    assert(hierarchy->Pos.x < scene->Pos.x && inspector->Pos.x > scene->Pos.x);
    assert(assets->Pos.y > scene->Pos.y);
    assert(scene->DockNode->IsCentralNode());
    const ImGuiID root = ImGui::DockNodeGetRootNode(scene->DockNode)->ID;
    const ImGuiID custom = ImGui::DockBuilderSplitNode(scene->DockId, ImGuiDir_Up, 0.2f, nullptr, nullptr);
    ImGui::DockBuilderFinish(root);
    Frame();
    assert(ImGui::DockBuilderGetNode(custom)); // Existing user layout survives.
    ImGuiManager::m_ResetEditorLayout = true;
    Frame(); Frame();
    assert(!ImGuiManager::m_ResetEditorLayout && scene->DockNode->IsCentralNode());
    assert(scene->DockNode->ChildNodes[0] == nullptr); // Reset removes custom split.
    io.DisplaySize = ImVec2(800, 600);
    Frame();
    assert(scene->Size.x > 100 && scene->Size.y > 100);
    ImGuiTextFilter filter("Light,-Spot");
    SearchFrame(filter);
    const ImVec2 clearButton = SearchFrame(filter);
    assert(filter.PassFilter("Point Light") && !filter.PassFilter("Spot Light"));
    io.AddMousePosEvent(clearButton.x, clearButton.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    SearchFrame(filter);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    SearchFrame(filter);
    assert(!filter.IsActive() && filter.PassFilter("Spot Light"));
    ImGui::DestroyContext();
    puts("PASS: default docking, saved layout preservation, layout reset, small viewport, search widget and filters");
}
''', encoding="utf-8")
    imgui = ROOT / "External" / "ImGUI"
    command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/utf-8", "/MD", "/Od",
               "/I" + str(imgui), str(source)]
    command += [str(imgui / name) for name in
                ["imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp"]]
    command += ["/Fe:" + str(output / "editor_ui_tests.exe")]
    subprocess.run(command, cwd=output, check=True)
    subprocess.run([str(output / "editor_ui_tests.exe")], cwd=output, check=True)


if __name__ == "__main__":
    main()
