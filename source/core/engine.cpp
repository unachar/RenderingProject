#include "pch.h"

#include "engine.h"
#include "camera.h"
#include "componentmanager.h"
#include "graphicsdevice.h"
#include "imguimanager.h"
#include "input.h"
#include "modelmanager.h"
#include "performancegovernor.h"
#include "projectmanager.h"
#include "renderconfiguration.h"
#include "renderframe.h"
#include "renderprofiler.h"
#include "systemmanager.h"
#include "world.h"
#include "../../External/tracy/public/tracy/Tracy.hpp"

#include <chrono>

void Engine::Init()
{
    World::Init();
    ProjectManager::Init();
    Input::Init();
    SystemManager::Init();
    Camera::Create();
}

void Engine::Uninit()
{
    GraphicsDevice::WaitForGpuIdle();
    if (ProjectManager::IsPlaying())
    {
        ProjectManager::Stop();
    }
    ImGuiManager::SaveProjectSettings();
    SystemManager::Uninit();
    ModelManager::Uninit();
    ComponentManager::Uninit();
    ProjectManager::Uninit();

    Input::Uninit();
    GraphicsDevice::Uninit();
}

void Engine::Run()
{
    ZoneScopedN("Engine frame");
    const uint64_t previousSerial = RenderProfiler::GetFrameSerial();
    const auto frameBegin = std::chrono::steady_clock::now();
    RenderConfiguration::ApplyPendingRenderMode();
    RenderConfiguration::ApplyPendingHdr();
    PerformanceGovernor::Update();
    RenderConfiguration::ApplyPendingResolutionScale();
    Update();
    Draw();
    if (RenderProfiler::GetFrameSerial() != previousSerial)
    {
        const double milliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameBegin).count();
        RenderProfiler::RecordCpuFrameTime(RenderProfiler::GetFrameSerial(), milliseconds);
    }
    FrameMark;
}

void Engine::Update()
{
    ZoneScopedN("Engine update");
    World::Update();
    Input::Update();

    // Esc：終了（未保存のシーンがあれば確認）。文字入力中やポップアップ表示中は終了しない
    if (Input::IsKeyPress(VK_ESCAPE) && !ImGui::GetIO().WantTextInput &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
    {
        ImGuiManager::RequestQuit();
    }

    SystemManager::UpdateSystem();
}

void Engine::Draw()
{
    ZoneScopedN("Engine draw submission");
    RenderFrame::BeginDraw();

    RenderProfiler::DrawImGuiWindow();

    SystemManager::RenderFlow();

    RenderFrame::EndDraw();
}
