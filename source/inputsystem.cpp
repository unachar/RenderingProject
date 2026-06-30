#include "pch.h"
#include "inputsystem.h"
#include "input.h"
#include "componentmanager.h"
#include "camera.h"
#include "world.h"

void InputSystem::Update()
{
	auto inputEntities = World::GetView<InputComponent>();
	for (EntityID e : inputEntities)
	{
		auto& input = ComponentManager::GetComponentUnchecked<InputComponent>(e);

		input.MoveLeft = false;
		input.MoveRight = false;
		input.MoveForward = false;
		input.MoveBackward = false;
		input.MoveUp = false;
		input.MoveDown = false;
		input.RotateLeft = false;
		input.RotateRight = false;
		input.PostProcessNone = false;
		input.PostProcessBlur = false;
		input.PostProcessSepia = false;
		input.PostProcessGrayscale = false;
		input.PostProcessInvert = false;
		input.IntensityUp = false;
		input.IntensityDown = false;
		input.RotateUp = false;
		input.RotateDown = false;

		if (!input.IsActive)
		{
			continue;
		}

		const bool isCamera = ComponentManager::HasComponent<CameraComponent>(e);
		const bool allowTransformInput = !isCamera || Input::IsKeyHeld(VK_RBUTTON);

		if (allowTransformInput)
		{
			if (Input::IsKeyHeld('A')) input.MoveLeft = true;
			if (Input::IsKeyHeld('D')) input.MoveRight = true;
			if (Input::IsKeyHeld('W')) input.MoveForward = true;
			if (Input::IsKeyHeld('S')) input.MoveBackward = true;
			if (Input::IsKeyHeld('Q')) input.MoveDown = true;
			if (Input::IsKeyHeld('E')) input.MoveUp = true;
			if (Input::IsKeyHeld('R')) input.RotateUp = true;
			if (Input::IsKeyHeld('F')) input.RotateDown = true;
			if (Input::IsKeyHeld('C')) input.RotateLeft = true;
			if (Input::IsKeyHeld('V')) input.RotateRight = true;
		}

		if (Input::IsKeyPress('1')) input.PostProcessNone = true;
		if (Input::IsKeyPress('2')) input.PostProcessBlur = true;
		if (Input::IsKeyPress('3')) input.PostProcessSepia = true;
		if (Input::IsKeyPress('4')) input.PostProcessGrayscale = true;
		if (Input::IsKeyPress('5')) input.PostProcessInvert = true;

		if (Input::IsKeyHeld(VK_UP)) input.IntensityUp = true;
		if (Input::IsKeyHeld(VK_DOWN)) input.IntensityDown = true;

	}
}

