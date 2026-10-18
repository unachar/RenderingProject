#include "pch.h"

#include "game.h"
#include "renderconfiguration.h"
#include "world.h"
#include "Camera.h"
#include "input.h"
#include "graphicsdevice.h"
#include "shaderpipelines.h"
#include "renderprofiler.h"
#include <chrono>

#include "systemmanager.h"
#include "modelmanager.h"

#include "light.h"
#include "projectmanager.h"
#include "physicssystem.h"
#include "imguimanager.h"

#include "entitybase.h"
#include <unordered_set>
#include "renderframe.h"
#include "performancegovernor.h"

vector<unique_ptr<EntityBase>>& Game::GetEntities()
{
	static vector<unique_ptr<EntityBase>> entities;
	return entities;
}

vector<unique_ptr<EntityBase>>& Game::GetPendingEntities()
{
	static vector<unique_ptr<EntityBase>> entities;
	return entities;
}

bool Game::RegisterEntity(EntityBase* entity)
{
	if (!entity)
	{
		return false;
	}

	for (const auto& registered : GetEntities())
	{
		if (registered.get() == entity)
		{
			return true;
		}
	}

	for (auto& pending : GetPendingEntities())
	{
		if (pending.get() == entity)
		{
			GetEntities().emplace_back(move(pending));
			return true;
		}
	}

	return false;
}

void Game::Init()
{
	World::Init();
	ProjectManager::Init();
	Input::Init();
	SystemManager::Init();
}

void Game::Create()
{
	Camera::Create();
	Camera::CreateGameCamera();

	for (auto& entity : GetPendingEntities())
	{
		entity->Create();
	}
	GetPendingEntities().clear();
}

void Game::Uninit()
{
	GraphicsDevice::WaitForGpuIdle();
	if (ProjectManager::IsPlaying()) ProjectManager::Stop();
	ImGuiManager::SaveProjectSettings();
	SystemManager::Uninit();
	ModelManager::Uninit();
	ComponentManager::Uninit();
	ProjectManager::Uninit();

	Input::Uninit();
	GraphicsDevice::Uninit();
}

void Game::Run()
{
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
        const double milliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - frameBegin).count();
        RenderProfiler::RecordCpuFrameTime(RenderProfiler::GetFrameSerial(), milliseconds);
    }
}

void Game::Update()
{
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

void Game::Draw()
{
	RenderFrame::BeginDraw();

	RenderProfiler::DrawImGuiWindow();

	SystemManager::RenderFlow();

	RenderFrame::EndDraw();
}

