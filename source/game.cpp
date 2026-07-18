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

namespace
{
	bool IsPhysicsSmokeTestEnabled()
	{
		static const bool enabled = []()
			{
				char value[8]{};
				return GetEnvironmentVariableA(
					"DX12_PHYSICS_SMOKE_TEST",
					value,
					static_cast<DWORD>(size(value))) > 0 &&
					atoi(value) != 0;
			}();
		return enabled;
	}

	float MeasurePmxDynamicBoneScaleError()
	{
		const Entity karen = World::GetEntityByName("Karen");
		if (!karen.IsValid() || !karen.Has<AnimationModelComponent>())
		{
			return 0.0f;
		}
		const auto& animation = karen.Get<AnimationModelComponent>();
		AnimationModelResource* model = ModelManager::GetAnimModel(animation.ModelId);
		if (!model)
		{
			return 0.0f;
		}

		float maxError = 0.0f;
		unordered_set<string> measuredBones;
		for (const PmxRigidBodyData& body : model->GetPmxRigidBodies())
		{
			if (body.Operation == 0 || body.BoneName.empty() ||
				!measuredBones.insert(body.BoneName).second)
			{
				continue;
			}

			XMFLOAT4X4 currentStored{};
			XMFLOAT4X4 bindStored{};
			if (!model->GetBoneGlobalTransform(body.BoneName, currentStored) ||
				!model->GetBoneBindGlobalTransform(body.BoneName, bindStored))
			{
				continue;
			}

			XMVECTOR currentScale{};
			XMVECTOR currentRotation{};
			XMVECTOR currentTranslation{};
			XMVECTOR bindScale{};
			XMVECTOR bindRotation{};
			XMVECTOR bindTranslation{};
			if (!XMMatrixDecompose(
				&currentScale, &currentRotation, &currentTranslation,
				XMLoadFloat4x4(&currentStored)) ||
				!XMMatrixDecompose(
					&bindScale, &bindRotation, &bindTranslation,
					XMLoadFloat4x4(&bindStored)))
			{
				continue;
			}

			XMFLOAT3 current{};
			XMFLOAT3 bind{};
			XMStoreFloat3(&current, currentScale);
			XMStoreFloat3(&bind, bindScale);
			maxError = max(maxError, fabsf(current.x - bind.x));
			maxError = max(maxError, fabsf(current.y - bind.y));
			maxError = max(maxError, fabsf(current.z - bind.z));
		}
		return maxError;
	}

	void UpdatePhysicsSmokeTest()
	{
		if (!IsPhysicsSmokeTestEnabled())
		{
			return;
		}

		static int frame = 0;
		static size_t maxRigCounts[3]{};
		static size_t maxRigBodyCounts[3]{};
		static size_t maxRigJointCounts[3]{};
		static size_t maxEntityBodyCounts[3]{};
		static float maxBoneScaleErrors[3]{};
		static uint64_t previousPoseApplySerial = 0;
		static int zeroStepFrames = 0;
		static int zeroStepPoseApplyFailures = 0;
		++frame;
		if (frame == 1 || frame == 20 || frame == 40)
		{
			FILE* progress = nullptr;
			if (fopen_s(&progress, "Save/physics_smoke_progress.txt", "a") == 0 && progress)
			{
				fprintf(progress, "frame=%d phase=%s\n", frame,
					frame < 20 ? "Bullet" : (frame < 40 ? "Jolt" : "PhysX"));
				fclose(progress);
			}
		}
		PhysicsSystem* physicsSystem = SystemManager::GetSystem<PhysicsSystem>();
		const int phase = frame < 20 ? 0 : (frame < 40 ? 1 : 2);
		if (physicsSystem)
		{
			maxRigCounts[phase] = max(
				maxRigCounts[phase],
				physicsSystem->GetBoneRigCount());
			maxRigBodyCounts[phase] = max(
				maxRigBodyCounts[phase],
				physicsSystem->GetBoneBodyCount());
			maxRigJointCounts[phase] = max(
				maxRigJointCounts[phase],
				physicsSystem->GetBoneJointCount());
			maxEntityBodyCounts[phase] = max(
				maxEntityBodyCounts[phase],
				physicsSystem->GetEntityBodyCount());
			maxBoneScaleErrors[phase] = max(
				maxBoneScaleErrors[phase],
				MeasurePmxDynamicBoneScaleError());
			const uint64_t poseApplySerial = physicsSystem->GetPoseApplySerial();
			if (physicsSystem->GetBoneRigCount() > 0 &&
				physicsSystem->GetLastSubStepCount() == 0)
			{
				++zeroStepFrames;
				if (poseApplySerial <= previousPoseApplySerial)
				{
					++zeroStepPoseApplyFailures;
				}
			}
			previousPoseApplySerial = poseApplySerial;
		}

		if (frame == 10 || frame == 20 || frame == 40)
		{
			Entity karen = World::GetEntityByName("Karen");
			if (karen.IsValid() && karen.Has<TransformComponent>())
			{
				const float scale = frame == 10 ? 0.075f
					: (frame == 20 ? 0.30f : 0.15f);
				karen.Get<TransformComponent>().Scale = { scale, scale, scale };
				karen.Get<TransformComponent>().IsDirty = true;
			}
			if (frame == 20 || frame == 40)
			{
				const PhysicsEngine engine =
					frame == 20 ? PhysicsEngine::Jolt : PhysicsEngine::PhysX;
				ComponentManager::ForEachComponent<PhysicsComponent>(
					[engine](EntityID, PhysicsComponent& physics)
					{
						physics.UsePhysicsEngine = engine;
						++physics.SettingsRevision;
					});
			}
		}

		if (frame < 60)
		{
			return;
		}

		FILE* file = nullptr;
		if (fopen_s(&file, "Save/physics_smoke_test.txt", "w") == 0 && file)
		{
			fprintf(file, "bullet_rigs=%zu\n", maxRigCounts[0]);
			fprintf(file, "jolt_rigs=%zu\n", maxRigCounts[1]);
			fprintf(file, "physx_rigs=%zu\n", maxRigCounts[2]);
			fprintf(file, "bullet_bodies=%zu\n", maxRigBodyCounts[0]);
			fprintf(file, "jolt_bodies=%zu\n", maxRigBodyCounts[1]);
			fprintf(file, "physx_bodies=%zu\n", maxRigBodyCounts[2]);
			fprintf(file, "bullet_joints=%zu\n", maxRigJointCounts[0]);
			fprintf(file, "jolt_joints=%zu\n", maxRigJointCounts[1]);
			fprintf(file, "physx_joints=%zu\n", maxRigJointCounts[2]);
			fprintf(file, "bullet_entity_bodies=%zu\n", maxEntityBodyCounts[0]);
			fprintf(file, "jolt_entity_bodies=%zu\n", maxEntityBodyCounts[1]);
			fprintf(file, "physx_entity_bodies=%zu\n", maxEntityBodyCounts[2]);
			fprintf(file, "bullet_max_bone_scale_error=%.8f\n", maxBoneScaleErrors[0]);
			fprintf(file, "jolt_max_bone_scale_error=%.8f\n", maxBoneScaleErrors[1]);
			fprintf(file, "physx_max_bone_scale_error=%.8f\n", maxBoneScaleErrors[2]);
			fprintf(file, "zero_step_frames=%d\n", zeroStepFrames);
			fprintf(file, "zero_step_pose_apply_failures=%d\n", zeroStepPoseApplyFailures);
			if (physicsSystem)
			{
				fprintf(file, "bullet_available=%d\n",
					physicsSystem->IsBackendAvailable(PhysicsEngine::Bullet) ? 1 : 0);
				fprintf(file, "jolt_available=%d\n",
					physicsSystem->IsBackendAvailable(PhysicsEngine::Jolt) ? 1 : 0);
				fprintf(file, "physx_available=%d\n",
					physicsSystem->IsBackendAvailable(PhysicsEngine::PhysX) ? 1 : 0);
			}
			fclose(file);
		}
		ProjectManager::Stop();
		PostQuitMessage(0);
	}

	void CaptureAutomatedBenchmark()
	{
		static int sampleTarget = []()
			{
				char value[16]{};
				if (GetEnvironmentVariableA(
					"DX12_BENCHMARK_FRAMES",
					value,
					static_cast<DWORD>(size(value))) == 0)
				{
					return 0;
				}
				return max(atoi(value), 0);
			}();
		if (sampleTarget <= 0)
		{
			return;
		}

		constexpr int kWarmupFrames = 120;
		static int elapsedFrames = 0;
		static int capturedFrames = 0;
		static double elapsedSeconds = 0.0;
		static double cpuRenderMilliseconds = 0.0;
		static int cpuRenderSamples = 0;
		static double gpuMilliseconds = 0.0;
		static int gpuSamples = 0;
		static uint64_t lastProfilerSerial = 0;
		struct PassSamples
		{
			double CpuMilliseconds = 0.0;
			double GpuMilliseconds = 0.0;
			int Count = 0;
		};
		static unordered_map<string, PassSamples> passSamples;

		++elapsedFrames;
		if (elapsedFrames <= kWarmupFrames)
		{
			return;
		}

		elapsedSeconds += World::GetRawDeltaTime();
		++capturedFrames;
		if (RenderProfiler::HasLatest())
		{
			const RenderProfiler::FrameSnapshot& snapshot = RenderProfiler::GetLatest();
			if (snapshot.Serial != lastProfilerSerial)
			{
				lastProfilerSerial = snapshot.Serial;
				cpuRenderMilliseconds += snapshot.CpuRenderMs;
				++cpuRenderSamples;
				if (snapshot.GpuTimingValid)
				{
					gpuMilliseconds += snapshot.GpuRenderMs;
					++gpuSamples;
				}
				for (const RenderProfiler::PassTiming& pass : snapshot.Passes)
				{
					if (pass.Name == "Frame")
					{
						continue;
					}
					auto& sample = passSamples[pass.Name];
					sample.CpuMilliseconds += pass.CpuMs;
					sample.GpuMilliseconds += pass.GpuMs;
					++sample.Count;
				}
			}
		}

		if (capturedFrames < sampleTarget)
		{
			return;
		}

		FILE* file = nullptr;
		if (fopen_s(&file, "Save/render_benchmark.txt", "w") == 0 && file)
		{
			const double averageFrameMs =
				elapsedSeconds > 0.0 ? elapsedSeconds * 1000.0 / capturedFrames : 0.0;
			const double averageFps =
				elapsedSeconds > 0.0 ? capturedFrames / elapsedSeconds : 0.0;
			fprintf(file, "samples=%d\n", capturedFrames);
			fprintf(file, "resolution=%ux%u\n", RendererCore::GetWidth(), RendererCore::GetHeight());
			fprintf(file, "internal_resolution=%ux%u\n",
				RendererCore::GetSceneWidth(), RendererCore::GetSceneHeight());
			fprintf(file, "average_fps=%.3f\n", averageFps);
			fprintf(file, "average_frame_ms=%.3f\n", averageFrameMs);
			fprintf(file, "average_cpu_render_ms=%.3f\n",
				cpuRenderSamples > 0 ? cpuRenderMilliseconds / cpuRenderSamples : 0.0);
			fprintf(file, "average_gpu_ms=%.3f\n",
				gpuSamples > 0 ? gpuMilliseconds / gpuSamples : 0.0);
			for (const auto& [name, sample] : passSamples)
			{
				if (sample.Count > 0)
				{
					fprintf(file, "pass.%s.cpu_ms=%.3f\n",
						name.c_str(),
						sample.CpuMilliseconds / sample.Count);
					fprintf(file, "pass.%s.gpu_ms=%.3f\n",
						name.c_str(),
						sample.GpuMilliseconds / sample.Count);
				}
			}
			fclose(file);
		}

		sampleTarget = 0;
		PostQuitMessage(0);
	}
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
	if (IsPhysicsSmokeTestEnabled())
	{
		Entity cube = World::GetEntityByName("Cube");
		if (cube.IsValid() && !cube.Has<PhysicsComponent>())
		{
			cube.Add<PhysicsComponent>();
			auto& physics = cube.Get<PhysicsComponent>();
			physics.UsePhysics = true;
			physics.UsePhysicsEngine = PhysicsEngine::Bullet;
			physics.BodyType = PhysicsBodyType::Dynamic;
			physics.Shape = PhysicsShape::Box;
		}
		ProjectManager::Play();
	}
}

void Game::Uninit()
{
	// Model and system resources can still be referenced by the final submitted
	// frame. Drain the direct queue before any resource owner starts teardown.
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
	UpdatePhysicsSmokeTest();
}

void Game::Draw()
{
	RendererDraw::BeginDraw();

	RenderProfiler::BeginFrame(
		RendererCore::GetDevice(),
		RendererCore::GetCommandQueue(),
		RendererCore::GetCommandList(),
		RendererCore::GetFrameIndex());

	// The ImGui frame has already been opened by RendererDraw::BeginDraw().
	// Display the last fence-safe result while the current frame is measured.
	RenderProfiler::DrawImGuiWindow();

	SystemManager::RenderFlow();

	// Timestamp and pipeline-statistics queries must be resolved before
	// RendererDraw::EndDraw() closes and submits the command list.
	RenderProfiler::EndFrame(RendererCore::GetCommandList());
	RendererDraw::EndDraw();
	CaptureAutomatedBenchmark();
}
