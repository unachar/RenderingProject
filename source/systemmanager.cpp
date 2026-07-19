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
#include "rendergraph.h"


bool SystemManager::Init()
{

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
	ID3D12GraphicsCommandList* commandList = RendererCore::GetCommandList();
	static RenderGraph graph;
	static bool graphReady = false;

	if (!graphReady)
	{
		if (graph.GetPassCount() == 0)
		{
			RenderGraph::ImportedResource shadowDepthResource{};
			shadowDepthResource.Name = "Shadow Depth";
			shadowDepthResource.Resource = RendererDraw::GetShadowDepthResource();
			shadowDepthResource.InitialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			shadowDepthResource.FinalState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			shadowDepthResource.HasFinalState = true;
			const auto shadowDepth = graph.ImportResource(shadowDepthResource);

			const auto geometry = graph.CreateLogicalResource("Geometry Buffers");
			const auto sceneDepth = graph.CreateLogicalResource("Scene Depth");
			const auto velocity = graph.CreateLogicalResource("Velocity");
			const auto sceneColor = graph.CreateLogicalResource("Scene Color");
			const auto editorScene = graph.CreateLogicalResource("Editor Scene");

			graph.AddPass(
				"Shadow",
				[shadowDepth](RenderGraph::PassBuilder& builder)
				{
					builder.Write(shadowDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
				},
				[](ID3D12GraphicsCommandList* passCommandList)
				{
					RenderProfiler::ScopedEvent profile("Shadow", passCommandList);
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
				});

			graph.AddPass(
				"GBuffer / Opaque",
				[shadowDepth, geometry, sceneDepth](RenderGraph::PassBuilder& builder)
				{
					builder.Read(shadowDepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
						.Write(geometry)
						.Write(sceneDepth);
				},
				[](ID3D12GraphicsCommandList* passCommandList)
				{
					RenderProfiler::ScopedEvent profile("GBuffer / Opaque", passCommandList);
					RendererDraw::BeginScenePass();
					DrawSystem(RenderPass::PrimaryScene, false);
					if (RendererDraw::BuildOcclusionHierarchyAndBeginPhaseTwo())
					{
						DrawSystem(RenderPass::OcclusionPhase2, false);
					}
					RendererDraw::EndScenePass();
				});

			graph.AddPass(
				"Velocity",
				[sceneDepth, velocity](RenderGraph::PassBuilder& builder)
				{
					builder.Read(sceneDepth).Write(velocity);
				},
				[](ID3D12GraphicsCommandList* passCommandList)
				{
					RenderProfiler::ScopedEvent profile("Velocity", passCommandList);
					RendererDraw::RenderVelocityBuffer();
					DrawSystem(RenderPass::Velocity, false);
					RendererDraw::EndVelocityBuffer();
				});

			graph.AddPass(
				"Deferred Lighting / PostProcess",
				[shadowDepth, geometry, sceneDepth, velocity, sceneColor, editorScene](
					RenderGraph::PassBuilder& builder)
				{
					builder.Read(shadowDepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
						.Read(geometry)
						.Read(sceneDepth)
						.Read(velocity)
						.Write(sceneColor)
						.Write(editorScene);
				},
				[](ID3D12GraphicsCommandList* passCommandList)
				{
					RenderProfiler::ScopedEvent profile("Deferred Lighting / PostProcess", passCommandList);
					PostProcessSystem postProcess;
					postProcess.Draw(RenderPass::PrimaryScene, false);
				});

			graph.AddPass(
				"Transparent / Overlay",
				[sceneColor, editorScene](RenderGraph::PassBuilder& builder)
				{
					builder.Read(sceneColor).ReadWrite(editorScene);
				},
				[](ID3D12GraphicsCommandList* passCommandList)
				{
					RenderProfiler::ScopedEvent profile("Transparent / Overlay", passCommandList);
					RendererDraw::PrepareTransparentSceneCopy();
					RendererDraw::BeginEditorSceneOverlayPass();
					DrawSystem(RenderPass::OverlayScene, false);
					RendererDraw::EndEditorSceneOverlayPass();
				});

			graph.AddPass(
				"AntiAliasing",
				[sceneDepth, velocity, editorScene](RenderGraph::PassBuilder& builder)
				{
					builder.Read(sceneDepth).Read(velocity).ReadWrite(editorScene);
				},
				[](ID3D12GraphicsCommandList* passCommandList)
				{
					RenderProfiler::ScopedEvent profile("AntiAliasing", passCommandList);
					RendererDraw::ApplyAntiAliasing();
				});
		}

		graphReady = graph.Compile();
		if (!graphReady)
		{
			Debug::Log("ERROR: Frame RenderGraph compile failed: %s\n", graph.GetLastError().c_str());
			return;
		}
	}

	if (!graph.Execute(commandList))
	{
		Debug::Log("ERROR: Frame RenderGraph execution failed: %s\n", graph.GetLastError().c_str());
	}
}
