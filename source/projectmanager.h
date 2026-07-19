#pragma once

#include "ecs.h"
#include "componentmanager.h"
#include <vector>

enum class ProjectPlayState : int
{
	Edit = 0,
	Playing = 1,
	Paused = 2,
};



class ProjectManager final
{
	struct RuntimeSnapshot
	{
		EntityID Entity = g_kINVALID_ENTITY;
		bool HasTransform = false;
		TransformComponent Transform{};
		bool HasAnimation = false;
		AnimationModelComponent Animation{};
		bool HasCamera = false;
		CameraComponent Camera{};
		bool HasPostProcess = false;
		PostProcessComponent PostProcess{};
		bool HasTimeline = false;
		TimelineComponent Timeline{};
	};

	inline static ProjectPlayState s_State = ProjectPlayState::Edit;
	inline static std::vector<RuntimeSnapshot> s_RuntimeSnapshots{};
	inline static uint64_t s_PlaySession = 0;

	static void CaptureRuntimeState();
	static void RestoreRuntimeState();

public:
	static void Init();
	static void Uninit();

	static void Play();
	static void Stop();
	static void TogglePlay();
	static void TogglePause();

	static ProjectPlayState GetPlayState() { return s_State; }
	static bool IsPlaying() { return s_State != ProjectPlayState::Edit; }
	static bool IsPaused() { return s_State == ProjectPlayState::Paused; }
	static bool IsSimulationRunning() { return s_State == ProjectPlayState::Playing; }
	static uint64_t GetPlaySession() { return s_PlaySession; }
};
