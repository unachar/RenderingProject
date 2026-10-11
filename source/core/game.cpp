#include "pch.h"

#include "game.h"
#include "renderconfiguration.h"
#include "world.h"
#include "Camera.h"
#include "input.h"
#include "graphicsdevice.h"
#include "shaderpipelines.h"

#include "systemmanager.h"
#include "modelmanager.h"

#include "light.h"
#include "projectmanager.h"
#include "physicssystem.h"

#include "entitybase.h"
#include <unordered_set>
#include "renderframe.h"

std::vector<std::unique_ptr<EntityBase>>& Game::GetEntities()
{
	static std::vector<std::unique_ptr<EntityBase>> entities;
	return entities;
}

std::vector<std::unique_ptr<EntityBase>>& Game::GetPendingEntities()
{
	static std::vector<std::unique_ptr<EntityBase>> entities;
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
			GetEntities().emplace_back(std::move(pending));
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
	SystemManager::Uninit();
	ModelManager::Uninit();
	ComponentManager::Uninit();
	ProjectManager::Uninit();

	Input::Uninit();
	GraphicsDevice::Uninit();
}

void Game::Run()
{
	RenderConfiguration::ApplyPendingRenderMode();
	RenderConfiguration::ApplyPendingHdr();
	RenderConfiguration::ApplyPendingResolutionScale();
	Update();
	Draw();
}

void Game::Update()
{
	World::Update();
	Input::Update();

	if (Input::IsKeyPress(VK_ESCAPE))
	{
		PostQuitMessage(0);
	}

	SystemManager::UpdateSystem();
}

void Game::Draw()
{
	RenderFrame::BeginDraw();

	RenderProfiler::BeginFrame(
		GraphicsDevice::GetDevice(),
		GraphicsDevice::GetCommandQueue(),
		GraphicsDevice::GetCommandList(),
		GraphicsDevice::GetFrameIndex());

	RenderProfiler::DrawImGuiWindow();

	SystemManager::RenderFlow();

	RenderProfiler::EndFrame(GraphicsDevice::GetCommandList());
	RenderFrame::EndDraw();
}
