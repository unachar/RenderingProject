#include "pch.h"
#include "temporallightingvalidation.h"
#include "world.h"
#include "camera.h"
#include "sun.h"
#include "daynightcycle.h"
#include "primitivegeometry.h"
#include "materialmanager.h"
#include "renderconfiguration.h"
#include "renderersettings.h"
#include "performancegovernor.h"
#include "renderquality.h"
#include "advancedeffects.h"
#include "raytracing.h"
#include "rayreflections.h"
#include "rendertargets.h"
#include "rendercapture.h"
#include "renderprofiler.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "cloud/cloudrenderer.h"
#include "imguimanager.h"
#include "temporalupscaler.h"
#include <fstream>
#include <iomanip>

namespace
{
constexpr UINT Grid = 17, CaseCount = 4;
UINT FramesPerCase = 128, Warmup = 96;
constexpr float ReceiverHeight = -.95f;
UINT failures = 0;
uint64_t lastFrame = 0;
filesystem::path directory, materials;
ofstream hashes;

struct Moments
{
    double Sum = 0, Square = 0;
    UINT Count = 0;

    void Add(double value)
    {
        Sum += value;
        Square += value * value;
        ++Count;
    }
};

struct Measurement
{
    array<Moments, Grid * Grid> Gi, Shadow, Output, RawGi;
    Moments RawMean;
    UINT Frames = 0, InputWidth = 0, InputHeight = 0;
    double GpuMs = 0, CpuMs = 0;
    UINT TimingFrames = 0;
};

array<Measurement, CaseCount> measurements;
array<UINT, 448 * CaseCount + 16> serialToFrame{};
uint64_t firstSerial = 0, lastTimingSerial = 0;

void Check(bool valid, const char* message)
{
    if (!valid)
    {
        ++failures;
    }
    WriteGraphicsLog("Temporal lighting: %s %s\n", valid ? "PASS" : "FAIL", message);
}

bool Flag(const char* name)
{
    char value[16]{};
    return GetEnvironmentVariableA(name, value, sizeof(value)) && atoi(value) != 0;
}

AssetHandle Material(const char* name, XMFLOAT4 base, XMFLOAT3 emission)
{
    MaterialAssetData data;
    data.BaseColor = {base.x, base.y, base.z, base.w};
    data.Roughness = 1;
    data.EmissiveColor = {emission.x, emission.y, emission.z};
    data.EmissiveIntensity = 6;
    string error;
    const auto path = materials / name;
    Check(MaterialSerializer::Save(path, data, error), "create fixture material");
    return MaterialManager::Registry().Register(path, AssetType::Material, error);
}

void Cube(XMFLOAT3 position, XMFLOAT3 scale, AssetHandle material)
{
    auto entity = World::CreateEntity()
                      .Add<TransformComponent>()
                      .Add<MeshComponent>()
                      .Add<MaterialComponent>()
                      .Add<NameComponent>();
    entity.SetName("Temporal lighting fixture");
    auto& transform = entity.Get<TransformComponent>();
    transform.Position = position;
    transform.Scale = scale;
    transform.IsDirty = true;
    VertexResource mesh;
    mesh.entityid = entity.GetID();
    mesh.objectType = ObjectType::CUBE;
    mesh.color = Color::WHITE;
    PrimitiveGeometry::CreateObjectVertex(mesh);
    Check(MaterialManager::Assign(entity.GetID(), 0, material), "assign fixture material");
}

void Initialize()
{
    char run[96]{};
    GetEnvironmentVariableA("DX12_TEMPORAL_RUN", run, sizeof(run));
    directory = filesystem::path("build/temporal-lighting") / (run[0] ? run : "manual");
    filesystem::create_directories(directory);
    materials = filesystem::path("asset") / ("__temporal_lighting_" + to_string(GetCurrentProcessId()));
    filesystem::create_directories(materials);
    hashes.open(directory / "hashes.csv");
    hashes << "case,frame,gi,shadow,output,raw_gi\n";
    vector<EntityID> entities;
    for (EntityID entity : World::GetView<TransformComponent>())
    {
        if (!ComponentManager::HasComponent<CameraComponent>(entity))
        {
            entities.push_back(entity);
        }
    }
    for (auto entity : entities)
    {
        World::DestroyEntity(Entity(entity));
    }
    const auto white = Material("White.material", {.7f, .7f, .7f, 1}, {});
    const bool flow = Flag("DX12_TEMPORAL_FLOW");
    if (flow)
    {
        FramesPerCase = 448;
        Warmup = 384;
    }
    const auto red = Material("Red.material", {.6f, .1f, .1f, 1}, flow ? XMFLOAT3{} : XMFLOAT3{1, 0, 0});
    const auto green = Material("Green.material", {.1f, .6f, .1f, 1}, flow ? XMFLOAT3{} : XMFLOAT3{0, 1, 0});
    Cube({0, -1, 0}, {4, .1f, 4}, white);
    Cube({-.4f, -.1f, .3f}, {.15f, .8f, .15f}, white);
    Cube({.8f, -.35f, 1.1f}, {.12f, .55f, .12f}, white);
    Cube({flow ? -2.1f : -2.5f, .8f, flow ? 0.f : .8f}, {flow ? .2f : .05f, flow ? 3.6f : 1.6f, flow ? 4.4f : 2.f},
         red);
    Cube({flow ? 2.1f : 2.5f, .8f, flow ? 0.f : .8f}, {flow ? .2f : .05f, flow ? 3.6f : 1.6f, flow ? 4.4f : 2.f},
         green);
    const auto sun = Sun::CreateDefault();
    if (flow)
    {
        ComponentManager::GetComponentUnchecked<LightComponent>(sun).IsActive = false;
        // Analytic light on colored surfaces: GI must sample reflected radiance,
        // rather than the separately sampled emissive-light shortcut.
        auto lamp = World::CreateEntity().Add<TransformComponent>().Add<LightComponent>();
        lamp.Get<TransformComponent>().Position = {0, 1.2f, .8f};
        auto& light = lamp.Get<LightComponent>();
        light.Type = LightType::Point;
        light.Position = {0, 1.2f, .8f};
        light.Intensity = .5f;
        light.Range = 12;
        light.CastShadow = true;
        Cube({0, 2.5f, 0}, {6, .1f, 6}, white);
        Cube({0, .8f, -2.1f}, {4.4f, 3.6f, .2f}, white);
        Cube({0, .8f, 2.1f}, {4.4f, 3.6f, .2f}, white);
    }
    RenderConfiguration::Resize(1280, 720);
}

float Channel(const RenderCapture::TextureData& image, int x, int y, bool redOnly)
{
    const auto* row = image.Bytes.data() + UINT(y) * image.RowPitch;
    if (image.Format == DXGI_FORMAT_R16G16_FLOAT)
    {
        return RenderCapture::HalfToFloat(reinterpret_cast<const uint16_t*>(row)[UINT(x) * 2]);
    }
    if (image.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        const auto* pixel = reinterpret_cast<const uint16_t*>(row) + UINT(x) * 4;
        const float r = RenderCapture::HalfToFloat(pixel[0]);
        if (redOnly)
        {
            return r;
        }
        return .2126f * r + .7152f * RenderCapture::HalfToFloat(pixel[1]) +
               .0722f * RenderCapture::HalfToFloat(pixel[2]);
    }
    if (image.Format == DXGI_FORMAT_R8G8B8A8_UNORM || image.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
    {
        const auto* pixel = row + UINT(x) * 4;
        return (.2126f * pixel[0] + .7152f * pixel[1] + .0722f * pixel[2]) / 255;
    }
    Check(false, "supported readback format");
    return 0;
}

float Sample(const RenderCapture::TextureData& image, float u, float v, bool redOnly = false)
{
    const float px = u * image.Width - .5f, py = v * image.Height - .5f;
    const int x = int(floor(px)), y = int(floor(py));
    const float fx = px - x, fy = py - y;
    float sum = 0;
    for (int dy = 0; dy < 2; ++dy)
    {
        for (int dx = 0; dx < 2; ++dx)
        {
            sum += Channel(image, clamp(x + dx, 0, int(image.Width) - 1), clamp(y + dy, 0, int(image.Height) - 1),
                           redOnly) *
                   (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
        }
    }
    return sum;
}

uint64_t Hash(const RenderCapture::TextureData& image, UINT validWidth = 0)
{
    uint64_t hash = 14695981039346656037ull;
    const UINT bytesPerPixel = image.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
    for (UINT y = 0; y < image.Height; ++y)
    {
        for (UINT x = 0; x < (validWidth ? validWidth : image.Width) * bytesPerPixel; ++x)
        {
            hash ^= image.Bytes[y * image.RowPitch + x];
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

pair<double, double> Noise(const array<Moments, Grid * Grid>& values)
{
    double variance = 0, mean = 0;
    UINT pixels = 0;
    for (const auto& value : values)
    {
        if (value.Count < 24)
        {
            continue;
        }
        const double average = value.Sum / value.Count;
        variance += (std::max)(0., value.Square / value.Count - average * average);
        mean += average;
        ++pixels;
    }
    Check(pixels >= 100, "sufficient stable receiver samples");
    return {mean / (std::max)(pixels, 1u), sqrt(variance / (std::max)(pixels, 1u))};
}
} // namespace

bool TemporalLightingValidation::Enabled()
{
    char mode[16]{};
    return GetEnvironmentVariableA("DX12_GI_TEST", mode, sizeof(mode)) && atoi(mode) == 4;
}

void TemporalLightingValidation::BeforeFrame(uint64_t frame)
{
    lastFrame = frame;
    if (frame == 0)
    {
        Initialize();
    }
    if (frame >= FramesPerCase * CaseCount)
    {
        return;
    }
    const UINT phase = UINT(frame / FramesPerCase);
    PerformanceGovernor::GetOptions().Enabled = false;
    World::SetVSyncEnabled(false);
    World::SetFixedFrameRateEnabled(false);
    RenderConfiguration::SetResolutionScale(.29f);
    RendererSettings::SetUpscaleQuality(UpscaleQuality::Custom);
    RendererSettings::SetUpscaleMode(phase < 2 ? UpscaleMode::Tsr : UpscaleMode::Nis);
    RendererState::m_AntiAliasingMode = AntiAliasingMode::TAA;
    RendererSettings::SetGiMode(GlobalIlluminationMode::Realtime);
    RendererSettings::SetRealtimeGiIntensity(1);
    RendererSettings::SetGiRadianceCacheEnabled(Flag("DX12_TEMPORAL_CACHE"));
    if (Flag("DX12_TEMPORAL_FLOW"))
    {
        RendererSettings::SetGiRadianceCacheCellSize(1);
        RendererSettings::SetGiRadianceCacheHistory(4);
    }
    RendererSettings::SetMeshShadersEnabled(false);
    GPUDrivenRenderer::SetEnabled(false);
    RendererSettings::SetSsaoEnabled(false);
    RenderQuality::Get() = {};
    RenderQuality::Get().ReSTIRGI = Flag("DX12_TEMPORAL_RESTIR");
    AdvancedEffects::GetSettings() = {};
    CloudRenderer::GetSettings().Enabled = false;
    DayNightCycle::GetSettings().Running = false;
    DayNightCycle::SetTime(9);
    DayNightCycle::GetSettings().DayOfYear = 81;
    auto& rays = Raytracing::GetSettings();
    rays.Enabled = rays.Denoise = rays.Shadows = true;
    if (Flag("DX12_TEMPORAL_NO_NRD"))
    {
        rays.Denoise = false;
    }
    rays.RenderMode = Raytracing::Mode::Full;
    rays.Reflections = rays.AmbientOcclusion = false;
    rays.RadianceDenoiser = Raytracing::DenoiserType::Nrd;
    rays.StableNrdProfile = Flag("DX12_TEMPORAL_STABLE_NRD");
    rays.StableSampling = Flag("DX12_TEMPORAL_STABLE_SAMPLING");
    TemporalUpscaler::SetPlanarDepthEnabled(Flag("DX12_TEMPORAL_PLANAR_DEPTH"));
    if (frame % FramesPerCase == 0)
    {
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(Camera::GetCameraEntity());
        auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(Camera::GetCameraEntity());
        transform.Position = Flag("DX12_TEMPORAL_FLOW") ? XMFLOAT3{0, phase % 2 ? -.1f : 1.5f, -1.8f}
                                                        : XMFLOAT3{0, phase % 2 ? -.1f : 3.f, -7};
        transform.IsDirty = true;
        camera.Target = {0, ReceiverHeight, 0};
        camera.AllowUserControl = false;
        camera.LockOnTarget = g_kINVALID_ENTITY;
        RendererState::m_TaaFrameIndex = 0;
        Camera::NotifyCut();
    }
}

void TemporalLightingValidation::AfterFrame(uint64_t frame)
{
    if (frame >= FramesPerCase * CaseCount)
    {
        return;
    }
    const uint64_t serial = RenderProfiler::GetFrameSerial();
    if (!firstSerial)
    {
        firstSerial = serial;
    }
    if (serial >= firstSerial && serial - firstSerial < serialToFrame.size())
    {
        serialToFrame[serial - firstSerial] = UINT(frame);
    }
    const auto& timing = RenderProfiler::GetLatest();
    if (timing.Serial != lastTimingSerial && timing.Serial >= firstSerial &&
        timing.Serial - firstSerial < serialToFrame.size())
    {
        lastTimingSerial = timing.Serial;
        const UINT recordedFrame = serialToFrame[timing.Serial - firstSerial];
        if (recordedFrame % FramesPerCase >= Warmup && timing.GpuTimingValid)
        {
            auto& measured = measurements[recordedFrame / FramesPerCase];
            measured.GpuMs += timing.GpuRenderMs;
            measured.CpuMs += timing.CpuFrameMs;
            ++measured.TimingFrames;
        }
    }
    if (frame % FramesPerCase < Warmup)
    {
        return;
    }
    const UINT phase = UINT(frame / FramesPerCase);
    auto& measured = measurements[phase];
    RenderCapture::TextureData gi, shadow, output, depth, rawGi;
    if (!RenderCapture::ReadTexture(RayReflections::GetGiResource(), gi) ||
        !RenderCapture::ReadTexture(Raytracing::GetResultResource(), shadow) ||
        !RenderCapture::ReadTexture(RenderTargets::GetEditorSceneResource(), output) ||
        !RenderCapture::ReadTexture(RenderTargets::GetGBufferResource(GBufferType::DEPTH), depth) ||
        !RenderCapture::ReadTexture(RayReflections::GetGiNoisyResource(), rawGi))
    {
        Check(false, "read temporal signals");
        return;
    }

    measured.InputWidth = gi.Width;
    measured.InputHeight = gi.Height;
    ++measured.Frames;
    const bool checkerboard = Raytracing::GetSettings().Denoise && NrdDenoiser::IsCompiled();
    hashes << phase << ',' << frame % FramesPerCase << ',' << Hash(gi) << ',' << Hash(shadow) << ',' << Hash(output)
           << ',' << Hash(rawGi, checkerboard ? (rawGi.Width + 1) / 2 : rawGi.Width) << '\n';
    XMMATRIX view, projection;
    Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
    XMFLOAT4X4 jitteredProjection;
    XMStoreFloat4x4(&jitteredProjection, projection);
    const float jitterU = jitteredProjection._31 * .5f, jitterV = -jitteredProjection._32 * .5f;
    double rawMean = 0;
    UINT rawCount = 0;
    for (UINT z = 0; z < Grid; ++z)
    {
        for (UINT x = 0; x < Grid; ++x)
        {
            const auto world = XMVectorSet((float(x) - 8) * .2f, ReceiverHeight, (float(z) - 8) * .2f, 1);
            XMFLOAT3 pixel;
            XMStoreFloat3(&pixel, XMVector3Project(world, 0, 0, float(gi.Width), float(gi.Height), 0, 1, projection,
                                                   view, XMMatrixIdentity()));
            const float u = pixel.x / gi.Width, v = pixel.y / gi.Height;
            if (u < .01f || u > .99f || v < .01f || v > .99f)
            {
                continue;
            }
            const UINT px = UINT(pixel.x), py = UINT(pixel.y);
            const float actualDepth = reinterpret_cast<const float*>(depth.Bytes.data() + py * depth.RowPitch)[px];
            // Validate the receiver in world space: a grazing plane has a large depth
            // gradient within one input pixel, so projected point depth is unsuitable.
            const auto surface =
                XMVector3Unproject(XMVectorSet(float(px) + .5f, float(py) + .5f, actualDepth, 1), 0, 0, float(gi.Width),
                                   float(gi.Height), 0, 1, projection, view, XMMatrixIdentity());
            if (abs(XMVectorGetY(surface) - ReceiverHeight) > .015f)
            {
                continue;
            }
            const UINT index = z * Grid + x;
            measured.Gi[index].Add(Sample(gi, u, v, true));
            // REBLUR stores YCoCg: red = Y + Co - Cg. Without NRD SDK the signal is RGB.
            const UINT rawX = checkerboard ? px >> 1 : px;
            const auto* rawPixel =
                reinterpret_cast<const uint16_t*>(rawGi.Bytes.data() + py * rawGi.RowPitch) + rawX * 4;
            double rawRed = RenderCapture::HalfToFloat(rawPixel[0]);
            if (NrdDenoiser::IsCompiled())
            {
                rawRed += RenderCapture::HalfToFloat(rawPixel[1]) - RenderCapture::HalfToFloat(rawPixel[2]);
            }
            measured.RawGi[index].Add(rawRed);
            rawMean += rawRed;
            ++rawCount;
            measured.Shadow[index].Add(Sample(shadow, u, v));
            measured.Output[index].Add(Sample(output, phase < 2 ? u - jitterU : u, phase < 2 ? v - jitterV : v));
        }
    }
    if (rawCount)
    {
        measured.RawMean.Add(rawMean / rawCount);
    }
    if (frame % FramesPerCase == FramesPerCase - 1)
    {
        RenderCapture::SaveScenePng((directory / (to_string(phase) + ".png")).c_str());
    }
}

bool TemporalLightingValidation::Finish()
{
    ofstream report(directory / "summary.csv");
    report
        << "case,mode,angle,input_width,input_height,frames,gi_mean,gi_rms,shadow_mean,shadow_rms,output_mean,output_rms,gpu_ms,cpu_ms,timing_frames,raw_mean,raw_rms,raw_coherent_rms\n";
    report << setprecision(10);
    for (UINT phase = 0; phase < CaseCount; ++phase)
    {
        const auto& measured = measurements[phase];
        Check(measured.Frames == FramesPerCase - Warmup, "complete temporal sample window");
        const auto gi = Noise(measured.Gi), shadow = Noise(measured.Shadow), output = Noise(measured.Output);
        if (Flag("DX12_TEMPORAL_FLOW"))
        {
            Check(output.first > .005 && output.first < .5,
                  "flow receiver remains measurable without output saturation");
        }
        const auto raw = Noise(measured.RawGi);
        const double coherentMean = measured.RawMean.Sum / (std::max)(measured.RawMean.Count, 1u);
        const double coherentVariance =
            measured.RawMean.Square / (std::max)(measured.RawMean.Count, 1u) - coherentMean * coherentMean;
        report << phase << ',' << (phase < 2 ? "TSR" : "NIS_TAA") << ',' << (phase % 2 ? "grazing" : "normal") << ','
               << measured.InputWidth << ',' << measured.InputHeight << ',' << measured.Frames << ',' << gi.first << ','
               << gi.second << ',' << shadow.first << ',' << shadow.second << ',' << output.first << ','
               << output.second << ',' << measured.GpuMs / (std::max)(measured.TimingFrames, 1u) << ','
               << measured.CpuMs / (std::max)(measured.TimingFrames, 1u) << ',' << measured.TimingFrames << ','
               << raw.first << ',' << raw.second << ',' << sqrt((std::max)(0., coherentVariance)) << '\n';
    }
    Check(lastFrame >= FramesPerCase * CaseCount - 1, "complete all angles and upscalers");
    failures += RenderCapture::ReportGraphicsErrors();
    for (const char* name : {"White.material", "Red.material", "Green.material"})
    {
        error_code error;
        filesystem::remove(materials / name, error);
        filesystem::remove(materials / (string(name) + ".meta"), error);
    }
    error_code error;
    filesystem::remove(materials, error);
    WriteGraphicsLog("GI validation failures = %u\n", failures);
    return failures == 0;
}
