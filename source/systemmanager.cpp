#include "pch.h"
#include "systemmanager.h"
#include "rendererdraw.h"
#include "renderercore.h"
#include "rendererresource.h"
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
#include "physicssystem.h"
#include "projectmanager.h"
#include "materialsystem.h"
#include "lightsystem.h"
#include "camerasystem.h"
#include "gridsystem.h"
#include "rendersystem.h"
#include "modelsystem.h"
#include "instancingsystem.h"
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
	// Animation evaluates the authored pose first. Physics then updates dynamic
	// bodies/bones before the final entity transform and render systems run.
	addSystem(make_unique<PhysicsSystem>());
	addSystem(make_unique<MaterialSystem>());
	addSystem(make_unique<LightSystem>());
	addSystem(make_unique<CameraSystem>());
	addSystem(make_unique<TransformSystem>());

	addSystem(make_unique<GridSystem>());
	addSystem(make_unique<RenderSystem>());
	addSystem(make_unique<ModelSystem>());
	addSystem(make_unique<InstancingSystem>());
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
	// Snapshot transforms before any movement, timeline, animation or camera
	// system mutates the current frame. Newly-created entities are initialized
	// after TransformSystem has produced their first valid world matrix.
	ComponentManager::ForEachComponent<TransformComponent>([](EntityID, TransformComponent& transform)
		{
		if (transform.HasPreviousWorld)
		{
			transform.PreviousWorldMatrix = transform.WorldMatrix;
		}
		});

	for (auto& system : m_Systems)
	{
		const type_index systemType(typeid(*system));
		const bool playOnlySystem =
			systemType == type_index(typeid(InputSystem)) ||
			systemType == type_index(typeid(MovementSystem)) ||
			systemType == type_index(typeid(CameraControlSystem)) ||
			systemType == type_index(typeid(PostProcessInputSystem)) ||
			systemType == type_index(typeid(TimeLineSystem)) ||
			systemType == type_index(typeid(AnimationSystem));
		if (playOnlySystem && !ProjectManager::IsSimulationRunning())
		{
			continue;
		}
		system->Update();
	}

	ComponentManager::ForEachComponent<TransformComponent>([](EntityID, TransformComponent& transform)
		{
		if (!transform.HasPreviousWorld)
		{
			transform.PreviousWorldMatrix = transform.WorldMatrix;
			transform.HasPreviousWorld = true;
		}
		});
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
			ID3D12GraphicsCommandList* commandList = RendererCore::GetCommandList();

			{
				RenderProfiler::ScopedEvent profile("Shadow", commandList);
				const UINT shadowLightCount = RendererResource::GetShadowLightCount();
				for (UINT shadowIndex = 0; shadowIndex < shadowLightCount; ++shadowIndex)
				{
					if (!RendererResource::ShouldRenderShadowPass(shadowIndex))
					{
						continue;
					}
					if (RendererDraw::BeginShadowPass(shadowIndex))
					{
						DrawSystem(RenderPass::ShadowMap, false);
						RendererDraw::EndShadowPass();
					}
				}
				RendererDraw::EndShadowPassBatch();
				if (shadowLightCount > 0)
				{
					RendererResource::SetCurrentShadowPassIndex(0);
					RendererResource::UpdateShadowConstantBuffer();
				}
			}

			{
				RenderProfiler::ScopedEvent profile("GBuffer / Opaque", commandList);
				RendererDraw::BeginScenePass();
				DrawSystem(RenderPass::PrimaryScene, false);
				if (RendererDraw::BuildOcclusionHierarchyAndBeginPhaseTwo())
				{
					DrawSystem(RenderPass::OcclusionPhase2, false);
				}
				RendererDraw::EndScenePass();
			}

			{
				RenderProfiler::ScopedEvent profile("Velocity", commandList);
				RendererDraw::RenderVelocityBuffer();
				DrawSystem(RenderPass::Velocity, false);
				RendererDraw::EndVelocityBuffer();
			}

			{
				RenderProfiler::ScopedEvent profile("Deferred Lighting / PostProcess", commandList);
				PostProcessSystem postProcess;
				postProcess.Draw(RenderPass::PrimaryScene, false);
			}

			{
				RenderProfiler::ScopedEvent profile("Transparent / Overlay", commandList);
				RendererDraw::PrepareTransparentSceneCopy();
				RendererDraw::BeginEditorSceneOverlayPass();
				DrawSystem(RenderPass::OverlayScene, false);
				RendererDraw::EndEditorSceneOverlayPass();
			}

			{
				RenderProfiler::ScopedEvent profile("AntiAliasing", commandList);
				RendererDraw::ApplyAntiAliasing();
			}
		};

	renderDeferred();
}
