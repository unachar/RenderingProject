#include "pch.h"
#include "postprocesssystem.h"
#include "componentmanager.h"
#include "systemmanager.h"
#include "rendererdraw.h"
#include "camera.h"
#include "world.h"

void PostProcessSystem::Draw(RenderPass renderPass, bool receivingPostProcessOnly)
{
	if (renderPass != RenderPass::PrimaryScene || receivingPostProcessOnly)
	{
		return;
	}

	auto cameraPostView = World::GetView<CameraComponent, PostProcessComponent>();
	if (cameraPostView.empty())
	{
		return;
	}

	EntityID cameraEntity = *cameraPostView.begin();
	auto& camera = ComponentManager::GetComponent<CameraComponent>(cameraEntity);
	auto& postProcess = ComponentManager::GetComponent<PostProcessComponent>(cameraEntity);

	PostProcessComponent effectivePostProcess = postProcess;
	if (!camera.EnablePostProcess)
	{
		effectivePostProcess.Type = PostProcessType::NONE;
	}
 
 	RendererDraw::ApplyPostProcess(effectivePostProcess);
 }
