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

#include "entitybase.h"

namespace
{
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
}

void Game::Uninit()
{
	// Model and system resources can still be referenced by the final submitted
	// frame. Drain the direct queue before any resource owner starts teardown.
	RendererCore::WaitForGpuIdle();
	SystemManager::Uninit();
	ModelManager::Uninit();
	ComponentManager::Uninit();

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
