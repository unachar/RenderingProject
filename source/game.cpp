#include "pch.h"

#include "game.h"
#include "world.h"
#include "camera.h"
#include "input.h"
#include "rendererdraw.h"
#include "renderercore.h"
#include "renderershader.h"

#include "systemmanager.h"
#include "modelmanager.h"

#include "polygon3d.h"
#include "xbot.h"
#include "field.h"
#include "moca.h"
#include "karen.h"
#include "kacchatta_hone.h"
#include "alicia.h"
#include "sky.h"
#include "cube.h"
#include "light.h"
#include "projectmanager.h"
#include "physicssystem.h"

#include "entitybase.h"
#include <unordered_set>

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

	AddEntity<Cube>();
	AddEntity<Polygon3D>();
	AddEntity<XBot>();
	AddEntity<Field>();
	AddEntity<Moca>();
	AddEntity<Karen>();
	AddEntity<KacchattaHone>();
	AddEntity<Alicia>();
	AddEntity<Sky>();

	for (auto& entitys : entityBase)
	{
		entitys->Create();
	}
}

void Game::Uninit()
{
	RendererCore::WaitForGpuIdle();
	SystemManager::Uninit();
	ModelManager::Uninit();
	ComponentManager::Uninit();
	ProjectManager::Uninit();

	Input::Uninit();
	RendererCore::Uninit();
}

void Game::Run()
{
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
	RendererDraw::BeginDraw();

	RenderProfiler::BeginFrame(
		RendererCore::GetDevice(),
		RendererCore::GetCommandQueue(),
		RendererCore::GetCommandList(),
		RendererCore::GetFrameIndex());

	RenderProfiler::DrawImGuiWindow();

	SystemManager::RenderFlow();

	RenderProfiler::EndFrame(RendererCore::GetCommandList());
	RendererDraw::EndDraw();
}
