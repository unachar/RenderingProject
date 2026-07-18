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

	const EntityID cameraEntity = Camera::GetCameraEntity();
	if (cameraEntity == g_kINVALID_ENTITY ||
		!ComponentManager::HasComponent<CameraComponent>(cameraEntity) ||
		!ComponentManager::HasComponent<PostProcessComponent>(cameraEntity))
	{
		return;
	}

	auto& camera = ComponentManager::GetComponent<CameraComponent>(cameraEntity);
	auto& postProcess = ComponentManager::GetComponent<PostProcessComponent>(cameraEntity);

	PostProcessComponent effectivePostProcess = postProcess;
	if (!camera.EnablePostProcess)
	{
		effectivePostProcess.Type = PostProcessType::NONE;
	}
 
 	RendererDraw::ApplyPostProcess(effectivePostProcess);
 }
