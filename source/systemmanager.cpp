#include "pch.h"
#include "systemmanager.h"
#include "rendererdraw.h"
#include "renderercore.h"
#include "componentmanager.h"
#include "world.h"
#include "camera.h"

#include "inputsystem.h"
#include "transformsystem.h"
#include "movementsystem.h"
#include "timelinesystem.h"
#include "cameracontrolsystem.h"
#include "postprocessinputsystem.h"
#include "animationsystem.h"
#include "materialsystem.h"
#include "lightsystem.h"
#include "camerasystem.h"
#include "gridsystem.h"
#include "rendersystem.h"
#include "modelsystem.h"
#include "debugsystem.h"
#include "postprocesssystem.h"


bool SystemManager::Init()
{
	// unique_ptrで受け取る
	auto addSystem = [](unique_ptr<SystemBase> system)
		{
			SystemBase* rawPtr = system.get();
			m_Systems.push_back(move(system));
			m_SystemMap[type_index(typeid(*rawPtr))] = rawPtr;
		};


	addSystem(make_unique<InputSystem>());
	addSystem(make_unique<MovementSystem>());
	addSystem(make_unique<CameraControlSystem>());
	addSystem(make_unique<PostProcessInputSystem>());
	addSystem(make_unique<TimeLineSystem>());
	addSystem(make_unique<AnimationSystem>());
	addSystem(make_unique<MaterialSystem>());
	addSystem(make_unique<LightSystem>());
	addSystem(make_unique<CameraSystem>());
	addSystem(make_unique<TransformSystem>());

	addSystem(make_unique<GridSystem>());
	addSystem(make_unique<RenderSystem>());
	addSystem(make_unique<ModelSystem>());
	addSystem(make_unique<DebugSystem>());

	for (auto& system : m_Systems)
	{
		system->Init();
	}
	return true;
}

void SystemManager::Uninit()
{
	for (auto& system : m_Systems)
	{
		system->Uninit();
	}
	m_Systems.clear();
	m_SystemMap.clear();
}

void SystemManager::UpdateSystem()
{
	for (auto& system : m_Systems)
	{
		system->Update();
	}
}

void SystemManager::DrawSystem(RenderPass renderPass, bool receivingPostProcessOnly)
{
	for (auto& system : m_Systems)
	{
		system->Draw(renderPass, receivingPostProcessOnly);
	}
}

void SystemManager::RenderFlow()
{
	auto renderDeferred = []()
		{
			RendererDraw::BeginShadowPass();
			DrawSystem(RenderPass::ShadowMap, false);
			RendererDraw::EndShadowPass();

			RendererDraw::BeginScenePass();
			DrawSystem(RenderPass::PrimaryScene, false);
			RendererDraw::EndScenePass();

			PostProcessSystem postProcess;
			postProcess.Draw(RenderPass::PrimaryScene, false);

			RendererDraw::BeginEditorSceneOverlayPass();
			DrawSystem(RenderPass::OverlayScene, false);
			RendererDraw::EndEditorSceneOverlayPass();
		};

	renderDeferred();
}

