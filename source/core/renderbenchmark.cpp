#include "pch.h"
#include "renderbenchmark.h"
#include "rendermeasurementwindow.h"
#include "renderprofiler.h"
#include "graphicsdevice.h"
#include "renderconfiguration.h"
#include "performancegovernor.h"
#include "imguimanager.h"
#include "projectmanager.h"
#include "componentmanager.h"
#include "world.h"
#include "camera.h"
#include "daynightcycle.h"
#include "raytracing.h"
#include "nrddenoiser.h"
#include "graphicslog.h"
#include "rendercapture.h"
#include "megalights.h"
#include "renderquality.h"
#include "renderersettings.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <unordered_map>
#include <cctype>

namespace
{
    struct Metadata
    {
        uint64_t SceneSignature = 0;
        uint64_t LocalUsage = 0;
        uint64_t LocalBudget = 0;
        bool MemoryValid = false;
        int DirectLighting = 0;
        UINT LocalLights = 0;
        bool RtxdiHistory = false;
        double RtxdiBufferMB = 0.0;
        int GiResampling = 0;
        bool GiActive = false;
        UINT LightSamples = 0;
        UINT LightSampleScale = 1;
        UINT InputWidth = 0, InputHeight = 0, OutputWidth = 0, OutputHeight = 0;
    };

    std::unique_ptr<RenderMeasurement::Window> g_Window;
    std::unordered_map<uint64_t, Metadata> g_Metadata;
    std::filesystem::path g_Directory;
    std::ofstream g_Frames, g_Passes;
    ComPtr<IDXGIAdapter3> g_Adapter;
    uint32_t g_Warmup = 120;
    uint32_t g_Measured = 600;
    uint64_t g_FirstSerial = 0;
    uint64_t g_LastRecorded = 0;
    XMFLOAT3 g_CameraOrigin{}, g_CameraTarget{};
    bool g_Failed = false;
    bool g_SettingsCaptured = false;
    bool g_NativeResolution = false;
    bool g_LowResolution250p = false;
    std::string g_LightingWorkload = "saved";
    bool g_DynamicLights = false;
    struct MovingLight
    {
        EntityID Entity;
        XMFLOAT3 Position;
        float Intensity;
    };
    std::vector<MovingLight> g_MovingLights;
    bool g_MovingLightsCaptured = false;

    void ApplyLightingWorkload(uint64_t frame)
    {
        if (g_LightingWorkload != "saved")
        {
            auto& rays = Raytracing::GetSettings();
            rays.Enabled = true;
            rays.RenderMode = Raytracing::Mode::Full;
            rays.Shadows = true;
            rays.Reflections = g_LightingWorkload != "direct";
            rays.AmbientOcclusion = g_LightingWorkload != "direct";
            MegaLights::GetSettings().Enabled = true;
            RenderQuality::Get().ReSTIRGI = g_LightingWorkload == "restir";
            if (g_LightingWorkload == "direct")
            {
                RendererSettings::SetGiMode(GlobalIlluminationMode::Off);
            }
            else
            {
                RendererSettings::SetGiMode(GlobalIlluminationMode::Realtime);
                RendererSettings::SetGiRadianceCacheEnabled(false);
            }
        }
        if (!g_DynamicLights || frame == 0)
        {
            return;
        }
        if (!g_MovingLightsCaptured)
        {
            for (EntityID entity : World::GetView<LightComponent, TransformComponent>())
            {
                const auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
                if (!light.IsActive || (light.Type != LightType::Point && light.Type != LightType::Spot && light.Type != LightType::Rect))
                {
                    continue;
                }
                const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
                g_MovingLights.push_back({entity, transform.Position, light.Intensity});
            }
            g_MovingLightsCaptured = true;
            if (g_MovingLights.empty())
            {
                throw std::runtime_error("Dynamic-light workload has no active local lights");
            }
        }
        const float time = static_cast<float>(frame) / 60.0f;
        for (const auto& moving : g_MovingLights)
        {
            auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(moving.Entity);
            auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(moving.Entity);
            const float phase = static_cast<float>(moving.Entity % 29u) * 0.21666156f;
            transform.Position = moving.Position;
            transform.Position.x += 1.5f * sinf(time * 1.2f + phase);
            transform.Position.z += 1.5f * cosf(time * 1.2f + phase);
            transform.IsDirty = true;
            light.Intensity = moving.Intensity * (0.65f + 0.35f * sinf(time * 2.3f + phase));
        }
    }

    float BenchmarkScale()
    {
        return g_LowResolution250p ? 0.35f : (g_NativeResolution ? 1.0f : 0.3f);
    }
    UINT BenchmarkWidth()
    {
        return g_LowResolution250p ? 448u : (g_NativeResolution ? 1280u : 384u);
    }
    UINT BenchmarkHeight()
    {
        return g_LowResolution250p ? 252u : (g_NativeResolution ? 720u : 216u);
    }

    uint32_t ReadCount(const char* name, uint32_t fallback, uint32_t minimum)
    {
        char value[32]{};
        if (!GetEnvironmentVariableA(name, value, sizeof(value)))
        {
            return fallback;
        }
        char* end = nullptr;
        const unsigned long parsed = strtoul(value, &end, 10);
        if (!end || *end || parsed < minimum || parsed > 100000)
        {
            throw std::runtime_error(std::string("Invalid benchmark count: ") + name);
        }
        return static_cast<uint32_t>(parsed);
    }

    void Hash(uint64_t& hash, const void* data, size_t bytes)
    {
        const auto* input = static_cast<const uint8_t*>(data);
        for (size_t index = 0; index < bytes; ++index)
        {
            hash ^= input[index];
            hash *= 1099511628211ull;
        }
    }

    uint64_t SceneSignature()
    {
        uint64_t hash = 14695981039346656037ull;
        for (EntityID entity : World::GetView<TransformComponent>())
        {
            const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
            Hash(hash, &entity, sizeof(entity));
            Hash(hash, &transform.Position, sizeof(transform.Position));
            Hash(hash, &transform.Rotation, sizeof(transform.Rotation));
            Hash(hash, &transform.Scale, sizeof(transform.Scale));
        }
        if (g_DynamicLights)
        {
            for (const auto& moving : g_MovingLights)
            {
                const auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(moving.Entity);
                Hash(hash, &light.Intensity, sizeof(light.Intensity));
            }
        }
        return hash;
    }

    void Nullable(std::ostream& stream, const std::optional<double>& value)
    {
        if (value && std::isfinite(*value))
        {
            stream << *value;
        }
        else
        {
            stream << "null";
        }
    }

    void CsvName(std::ostream& stream, const std::string& name)
    {
        stream << '"';
        for (char c : name)
        {
            if (c == '"')
            {
                stream << '"';
            }
            stream << c;
        }
        stream << '"';
    }

    void Initialize()
    {
        char workload[32]{};
        GetEnvironmentVariableA("DX12_BENCHMARK_LIGHTING_WORKLOAD", workload, sizeof(workload));
        if (workload[0])
        {
            g_LightingWorkload = workload;
        }
        if (g_LightingWorkload != "saved" && g_LightingWorkload != "direct" &&
            g_LightingWorkload != "raytracing" && g_LightingWorkload != "restir")
        {
            throw std::runtime_error("Unknown lighting benchmark workload");
        }
        char dynamicLights[8]{};
        g_DynamicLights = GetEnvironmentVariableA("DX12_BENCHMARK_DYNAMIC_LIGHTS", dynamicLights, sizeof(dynamicLights)) && atoi(dynamicLights) != 0;
        ApplyLightingWorkload(0);
        char native[8]{};
        g_NativeResolution = GetEnvironmentVariableA("DX12_RENDER_BENCHMARK_NATIVE", native, sizeof(native)) > 0 &&
                             atoi(native) != 0;
        char lowResolution[8]{};
        g_LowResolution250p = GetEnvironmentVariableA("DX12_RENDER_BENCHMARK_250P", lowResolution,
                                                     sizeof(lowResolution)) > 0 && atoi(lowResolution) != 0;
        g_Warmup = ReadCount("DX12_RENDER_BENCHMARK_WARMUP", 120, 2);
        g_Measured = ReadCount("DX12_RENDER_BENCHMARK_SAMPLES", 600, 2);
        char label[128]{};
        GetEnvironmentVariableA("DX12_RENDER_BENCHMARK_RUN", label, sizeof(label));
        std::string name = label[0] ? label : "run-" + std::to_string(GetCurrentProcessId());
        if (!std::all_of(name.begin(), name.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '-' || c == '_';
            }))
        {
            throw std::runtime_error("Benchmark run ID must be a simple directory name");
        }
        g_Directory = std::filesystem::path("bin/validation/render-benchmark") / name;
        if (std::filesystem::exists(g_Directory))
        {
            throw std::runtime_error("Benchmark run directory already exists");
        }
        std::filesystem::create_directories(g_Directory);
        g_Frames.open(g_Directory / "frames.csv");
        g_Passes.open(g_Directory / "passes.csv");
        g_Frames << std::setprecision(17);
        g_Passes << std::setprecision(17);
        g_Frames
            << "Serial,FrameIndex,FrameMs,CpuFrameMs,CpuRenderMs,GpuMs,GpuValid,SceneSignature,InputWidth,InputHeight,OutputWidth,OutputHeight,ProcessLocalUsageBytes,LocalBudgetBytes,ManagedAllocationBytes,RayCount,HistoryRejectionRate,ReflectionRays,GiRays,RadianceShadowRays,ReSTIRVisibilityRays,ShadowRays,AoRays,RayTelemetryValid,RayActiveProviders,RayMeasuredProviders,DirectLightingMode,LocalLights,RtxdiHistoryValid,RtxdiBufferMB,GiResamplingMode,GiActive,LightSamplesPerPixel,LightSampleScale\n";
        g_Passes << "Serial,Name,CpuInclusiveMs,GpuInclusiveMs,GpuExclusiveMs,Events,ValidGpuEvents,DroppedGpuEvents\n";
        if (!g_Frames || !g_Passes)
        {
            throw std::runtime_error("Cannot open benchmark output files");
        }
        ComPtr<IDXGIFactory4> factory;
        if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        {
            factory->EnumAdapterByLuid(GraphicsDevice::GetDevice()->GetAdapterLuid(), IID_PPV_ARGS(&g_Adapter));
        }
        std::ofstream metadata(g_Directory / "metadata.txt");
        metadata << "scenario=startup-camera-path-v1\nfixed_step_seconds=0.016666666666666666\nwarmup=" << g_Warmup
                 << "\nsamples=" << g_Measured << "\ndxr=" << Raytracing::IsAvailable()
                 << "\nnrd=" << NrdDenoiser::IsCompiled() << '\n';
        metadata
            << "requested_input_width=" << BenchmarkWidth() << "\nrequested_input_height=" << BenchmarkHeight()
            << "\nrequested_output_width=1280\nrequested_output_height=720\n";
        metadata << "gpu_scope=direct-frame-commands\n";
        metadata << "direct_lighting=" << static_cast<int>(MegaLights::GetRequestedMode()) << '\n';
        metadata << "gi_resampling=" << RenderQuality::GetGiResamplingMode()
                 << "\nlighting_workload=" << g_LightingWorkload << "\ndynamic_lights=" << g_DynamicLights << '\n';
        metadata << "ray_telemetry_requested=" << RayTelemetry::Requested()
                 << "\nray_telemetry_scope=radiance-and-shadow-ao\n";
        metadata << "final_capture=png\n";
        const auto& capabilities = GraphicsDevice::GetFeatureCapabilities();
        metadata << "hardware_mesh_shaders=" << capabilities.MeshShader
                 << "\nhardware_dxr11=" << capabilities.Raytracing
                 << "\nhardware_sampler_feedback=" << capabilities.SamplerFeedback
                 << "\nhardware_reserved_texture_tiles=" << capabilities.ReservedResources << '\n';
#ifdef _DEBUG
        metadata << "configuration=Debug\n";
#else
        metadata << "configuration=Release\n";
#endif
        const LUID luid = GraphicsDevice::GetDevice()->GetAdapterLuid();
        metadata << "adapter_luid_high=" << luid.HighPart << "\nadapter_luid_low=" << luid.LowPart << '\n';
        if (g_Adapter)
        {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(g_Adapter->GetDesc1(&desc)))
            {
                metadata << "vendor_id=" << desc.VendorId << "\ndevice_id=" << desc.DeviceId
                         << "\ndedicated_video_bytes=" << desc.DedicatedVideoMemory << '\n';
            }
        }
        World::SetVSyncEnabled(false);
        World::SetFixedFrameRateEnabled(false);
        PerformanceGovernor::GetOptions().Enabled = false;
        if (ProjectManager::IsPlaying())
        {
            ProjectManager::Stop();
        }
        auto& cycle = DayNightCycle::GetSettings();
        cycle.Running = false;
        cycle.PreviewInEditor = false;
        RenderConfiguration::Resize(1280, 720);
        RenderConfiguration::SetResolutionScale(BenchmarkScale());
        const auto camera = Camera::GetCameraEntity();
        g_CameraOrigin = ComponentManager::GetComponentUnchecked<TransformComponent>(camera).Position;
        g_CameraTarget = ComponentManager::GetComponentUnchecked<CameraComponent>(camera).Target;
        g_FirstSerial = RenderProfiler::GetFrameSerial() + 1;
        g_Window = std::make_unique<RenderMeasurement::Window>(g_FirstSerial, g_Warmup, g_Measured);
    }

    void Record(const RenderProfiler::FrameSnapshot& snapshot)
    {
        if (!g_Window || snapshot.Serial == g_LastRecorded)
        {
            return;
        }
        g_LastRecorded = snapshot.Serial;
        const RenderMeasurement::Sample sample{snapshot.Serial, snapshot.FrameIntervalMs, snapshot.CpuFrameMs,
                                               snapshot.GpuRenderMs, snapshot.GpuTimingValid};
        if (!g_Window->Record(sample))
        {
            return;
        }
        const auto found = g_Metadata.find(snapshot.Serial);
        if (found == g_Metadata.end())
        {
            g_Failed = true;
            return;
        }
        const auto& metadata = found->second;
        g_Frames << sample.Serial << ',' << sample.Serial - g_FirstSerial << ',' << sample.FrameMs << ','
                 << sample.CpuMs << ',' << snapshot.CpuRenderMs << ',';
        Nullable(g_Frames, sample.GpuValid ? std::optional<double>(sample.GpuMs) : std::nullopt);
        g_Frames << ',' << sample.GpuValid << ',' << metadata.SceneSignature << ',' << metadata.InputWidth << ','
                 << metadata.InputHeight << ',' << metadata.OutputWidth << ',' << metadata.OutputHeight << ',';
        if (metadata.MemoryValid)
        {
            g_Frames << metadata.LocalUsage << ',' << metadata.LocalBudget;
        }
        else
        {
            g_Frames << "null,null";
        }
        // 他の DXR ソースと NRD 内部の棄却情報はまだ取得できない。
        g_Frames << ",null,null,null";
        for (UINT channel = RAY_TELEMETRY_REFLECTION; channel <= RAY_TELEMETRY_AO; ++channel)
        {
            const uint32_t provider =
                channel <= RAY_TELEMETRY_RESTIR_VISIBILITY ? RayTelemetry::Radiance : RayTelemetry::ShadowAo;
            g_Frames << ',';
            if (snapshot.Rays.ProviderKnown(provider))
            {
                g_Frames << snapshot.Rays.Rays[channel];
            }
            else
            {
                g_Frames << "null";
            }
        }
        g_Frames << ',' << snapshot.Rays.Valid << ',' << snapshot.Rays.ActiveProviders << ','
                 << snapshot.Rays.MeasuredProviders << ',' << metadata.DirectLighting << ',' << metadata.LocalLights
                 << ',' << metadata.RtxdiHistory << ',' << metadata.RtxdiBufferMB << ',' << metadata.GiResampling
                 << ',' << metadata.GiActive << ',' << metadata.LightSamples << ',' << metadata.LightSampleScale << '\n';
        for (const auto& pass : snapshot.Passes)
        {
            g_Passes << sample.Serial << ',';
            CsvName(g_Passes, pass.Name);
            g_Passes << ',' << pass.CpuMs << ',';
            Nullable(g_Passes,
                     RenderMeasurement::IsGpuTimingComplete(pass.GpuEvents, pass.GpuSamples, snapshot.DroppedGpuEvents)
                         ? std::optional<double>(pass.GpuMs)
                         : std::nullopt);
            g_Passes << ',';
            Nullable(g_Passes, RenderMeasurement::IsGpuTimingComplete(pass.GpuEvents, pass.GpuExclusiveSamples,
                                                                      snapshot.DroppedGpuEvents)
                                   ? std::optional<double>(pass.GpuExclusiveMs)
                                   : std::nullopt);
            g_Passes << ',' << pass.GpuEvents << ',' << pass.GpuSamples << ',' << snapshot.DroppedGpuEvents << '\n';
        }
    }
}

bool RenderBenchmark::IsEnabled()
{
    static const bool enabled = [] {
        char value[8]{};
        return GetEnvironmentVariableA("DX12_RENDER_BENCHMARK", value, sizeof(value)) > 0 && atoi(value) != 0;
    }();
    return enabled;
}

void RenderBenchmark::BeforeFrame(uint64_t frame)
{
    if (!IsEnabled() || g_Failed)
    {
        return;
    }
    try
    {
        if (!g_Window)
        {
            Initialize();
        }
        // エディターは最初の UI フレームで保存設定を読み込む。
        // その後にベンチマーク条件を再適用し、測定条件のずれを検出したら拒否する。
        World::SetVSyncEnabled(false);
        World::SetFixedFrameRateEnabled(false);
        PerformanceGovernor::GetOptions().Enabled = false;
        RenderProfiler::SetEnabled(true);
        RenderConfiguration::SetResolutionScale(BenchmarkScale());
        auto& cycle = DayNightCycle::GetSettings();
        cycle.Running = false;
        cycle.PreviewInEditor = false;
        if (frame == 1)
        {
            if (ProjectManager::IsPlaying())
            {
                ProjectManager::Stop();
            }
            g_CameraOrigin =
                ComponentManager::GetComponentUnchecked<TransformComponent>(Camera::GetCameraEntity()).Position;
            g_CameraTarget = ComponentManager::GetComponentUnchecked<CameraComponent>(Camera::GetCameraEntity()).Target;
            RenderConfiguration::Resize(1280, 720);
        }
        ApplyLightingWorkload(frame);
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(Camera::GetCameraEntity());
        auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(Camera::GetCameraEntity());
        transform.Position = g_CameraOrigin;
        transform.Position.x += 2.0f * std::sin(static_cast<float>(frame) / 120.0f);
        transform.IsDirty = true;
        camera.Target = g_CameraTarget;
        camera.AllowUserControl = false;
        camera.LockOnTarget = g_kINVALID_ENTITY;
        if (frame + 1 == g_Warmup)
        {
            std::ofstream settings(g_Directory / "settings.cfg", std::ios::binary);
            settings << ImGuiManager::SerializeProjectSettings();
            g_SettingsCaptured = static_cast<bool>(settings);
        }
    }
    catch (const std::exception& error)
    {
        g_Failed = true;
        WriteGraphicsLog("Render benchmark: %s\n", error.what());
    }
}

void RenderBenchmark::AfterFrame()
{
    if (!IsEnabled() || g_Failed || !g_Window)
    {
        return;
    }
    Metadata metadata;
    const auto& lighting = MegaLights::GetStats();
    const auto requestedLighting = MegaLights::GetRequestedMode();
    metadata.DirectLighting = static_cast<int>(lighting.ActiveMode);
    if (lighting.RtxdiActive)
    {
        metadata.DirectLighting = static_cast<int>(MegaLights::Mode::Rtxdi);
    }
    metadata.LocalLights = lighting.SubmittedLights;
    metadata.RtxdiHistory = lighting.RtxdiHistoryValid;
    metadata.RtxdiBufferMB = lighting.RtxdiBufferMB;
    metadata.GiResampling = RenderQuality::GetGiResamplingMode();
    metadata.GiActive = Raytracing::IsGiActive();
    metadata.LightSamples = lighting.SamplesPerPixel;
    metadata.LightSampleScale = lighting.SampleScale;
    if (RenderProfiler::GetFrameSerial() >= g_FirstSerial + g_Warmup &&
        MegaLights::GetSettings().Enabled &&
        ((requestedLighting == MegaLights::Mode::Rtxdi && !lighting.RtxdiActive) ||
         (requestedLighting == MegaLights::Mode::Stochastic && lighting.ActiveMode != MegaLights::Mode::Stochastic)))
    {
        g_Failed = true;
        WriteGraphicsLog("Render benchmark: requested direct lighting unavailable: %s\n",
                         lighting.FallbackReason ? lighting.FallbackReason : "path did not execute");
        return;
    }
    metadata.SceneSignature = SceneSignature();
    metadata.InputWidth = GraphicsDevice::GetSceneWidth();
    metadata.InputHeight = GraphicsDevice::GetSceneHeight();
    metadata.OutputWidth = GraphicsDevice::GetWidth();
    metadata.OutputHeight = GraphicsDevice::GetHeight();
    if (RenderProfiler::GetFrameSerial() >= g_FirstSerial + g_Warmup &&
        (metadata.InputWidth != BenchmarkWidth() || metadata.InputHeight != BenchmarkHeight() || metadata.OutputWidth != 1280 ||
         metadata.OutputHeight != 720))
    {
        g_Failed = true;
        WriteGraphicsLog("Render benchmark: resolution drift, input=%ux%u output=%ux%u\n", metadata.InputWidth,
                         metadata.InputHeight, metadata.OutputWidth, metadata.OutputHeight);
    }
    if (g_Adapter)
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        metadata.MemoryValid = SUCCEEDED(g_Adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info));
        if (metadata.MemoryValid)
        {
            metadata.LocalUsage = info.CurrentUsage;
            metadata.LocalBudget = info.Budget;
        }
    }
    g_Metadata[RenderProfiler::GetFrameSerial()] = metadata;
    if (RenderProfiler::HasLatest())
    {
        Record(RenderProfiler::GetLatest());
    }
    if (RenderProfiler::GetFrameSerial() == g_FirstSerial + g_Warmup + g_Measured - 1)
    {
        const auto capture = g_Directory / "final.png";
        if (!RenderCapture::SaveScenePng(capture.c_str()))
        {
            g_Failed = true;
            WriteGraphicsLog("Render benchmark: final scene capture failed\n");
        }
    }
}

bool RenderBenchmark::Finish()
{
    if (!IsEnabled())
    {
        return true;
    }
    if (!g_Window || !GraphicsDevice::WaitForGpuIdle())
    {
        return false;
    }
    RenderProfiler::DrainCompletedFrames(Record);
    const auto summary = RenderMeasurement::Summarize(g_Window->Samples());
    const bool complete = !g_Failed && g_SettingsCaptured && g_Window->Complete() &&
                          summary.FrameSamples == g_Measured && summary.CpuSamples == g_Measured &&
                          summary.GpuSamples == g_Measured;
    std::ofstream output(g_Directory / "summary.json");
    output << std::setprecision(17) << "{\"complete\":" << (complete ? "true" : "false")
           << ",\"frames\":" << summary.FrameSamples << ",\"gpuSamples\":" << summary.GpuSamples << ",\"frameMeanMs\":";
    Nullable(output, summary.FrameMeanMs);
    output << ",\"cpuFrameMeanMs\":";
    Nullable(output, summary.CpuMeanMs);
    output << ",\"gpuMeanMs\":";
    Nullable(output, summary.GpuMeanMs);
    output << ",\"lowOnePercentFps\":";
    Nullable(output, summary.LowOnePercentFps);
    output << "}\n";
    g_Frames.close();
    g_Passes.close();
    output.close();
    const bool written = g_Frames.good() && g_Passes.good() && output.good();
    WriteGraphicsLog("Render benchmark: complete=%u frames=%zu GPU=%zu path=%s\n", complete && written,
                     summary.FrameSamples, summary.GpuSamples, g_Directory.string().c_str());
    return complete && written;
}

bool RenderBenchmark::ShouldStop(uint64_t completedFrames)
{
    return IsEnabled() && (g_Failed || completedFrames >= static_cast<uint64_t>(g_Warmup) + g_Measured);
}
