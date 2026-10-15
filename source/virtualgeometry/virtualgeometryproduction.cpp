#include "pch.h"
#include "virtualgeometryproduction.h"
#include "virtualgeometrydebug.h"
#include "virtualgeometryruntime.h"
#include "virtualgeometryresources.h"
#include "virtualgeometrystreaming.h"
#include "virtualgeometrytraversal.h"
#include "virtualgeometrybinning.h"
#include "renderprofiler.h"
#include "rendercapture.h"
#include "rendertargets.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "renderconfiguration.h"
#include "renderersettings.h"
#include "gpuscene/gpuscene.h"
#include "gpuscene/gpuscenechanges.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "meshshader/meshshaderrenderer.h"
#include "visibility/gpuvisibility.h"
#include "lightingresources.h"
#include "raytracing.h"
#include "modelmanager.h"
#include "world.h"
#include "camera.h"
#include "daynightcycle.h"
#include "componentmanager.h"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <psapi.h>
#include <intrin.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cmath>
#pragma comment(lib, "psapi.lib")

namespace VirtualGeometryProduction
{
namespace
{
std::ofstream benchmark;
std::ofstream passBenchmark;
std::string lastError;
uint64_t lastSerial = 0, previousUpload = 0;
std::chrono::steady_clock::time_point previousTime;
std::filesystem::path outputDirectory;
std::string hardwareKey;
uint32_t failures = 0, fixture = UINT32_MAX;
uint64_t readyFrame = UINT64_MAX;
bool compared = false;
constexpr std::array<GBufferType, 7> targets = {GBufferType::BASE_COLOR, GBufferType::NORMAL,
    GBufferType::MATERIAL, GBufferType::DEPTH, GBufferType::VELOCITY, GBufferType::SHADOW, GBufferType::EMISSION_AO};
std::vector<RenderCapture::TextureData> reference;
void Check(bool condition, const char* label)
{
    WriteGraphicsLog("VG production: %s %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition)
    {
        ++failures;
    }
}
std::string Hardware(std::ofstream& report)
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    const auto luid = GraphicsDevice::GetDevice()->GetAdapterLuid();
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))))
    {
        return {};
    }
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc)))
    {
        return {};
    }
    LARGE_INTEGER driver{};
    adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
    std::ostringstream key;
    key << std::hex << desc.VendorId << '-' << desc.DeviceId << '-' << desc.SubSysId << '-'
        << desc.Revision << '-' << driver.QuadPart;
    char gpu[512]{};
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, gpu, sizeof(gpu), nullptr, nullptr);
    char cpu[49]{};
    int registers[4]{};
    __cpuid(registers, static_cast<int>(0x80000000u));
    if (static_cast<uint32_t>(registers[0]) >= 0x80000004u)
    {
        for (uint32_t index = 0; index < 3; ++index)
        {
            __cpuid(registers, static_cast<int>(0x80000002u+index));
            memcpy(cpu+index*16, registers, 16);
        }
    }
    report << "VirtualGeometry Final Benchmark\nGPU: " << gpu << "\nCPU: " << cpu
        << "\nHardware key: " << key.str() << "\nDedicated VRAM bytes: " << desc.DedicatedVideoMemory
        << "\nDriver identifier: " << driver.QuadPart
        << "\nGPU timestamps are blank when unavailable. Decode is included in mesh raster timing."
        << "\nPage counters are delayed fence-completed samples; upload bandwidth is wall-time averaged.\n";
    uint64_t cpuHash = 14695981039346656037ull;
    for (const unsigned char byte : cpu)
    {
        cpuHash = (cpuHash ^ byte)*1099511628211ull;
    }
    MEMORYSTATUSEX ram{};
    ram.dwLength = sizeof(ram);
    GlobalMemoryStatusEx(&ram);
    key << '-' << cpuHash << '-' << ram.ullTotalPhys;
    report << "RAM bytes: " << ram.ullTotalPhys << "\nComplete reference key: " << key.str() << '\n';
    return key.str();
}
bool Compare(const RenderCapture::TextureData& a, const RenderCapture::TextureData& b,
    double absoluteTolerance, double maximumChangedFraction)
{
    if (!a.Width || !a.Height || a.Width != b.Width || a.Height != b.Height || a.Format != b.Format)
    {
        return false;
    }
    uint32_t channels = 0, bytes = 0;
    bool half = false, normalized = false, packedNormal = false;
    switch (a.Format)
    {
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS:
        channels = 1; bytes = 4; break;
    case DXGI_FORMAT_R32G32_FLOAT: channels = 2; bytes = 4; break;
    case DXGI_FORMAT_R16G16_FLOAT: channels = 2; bytes = 2; half = true; break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: channels = 4; bytes = 2; half = true; break;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        channels = 4; bytes = 1; normalized = true; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        channels = 4; bytes = 1; packedNormal = true; break;
    default: return false;
    }
    const size_t rowBytes = size_t(a.Width)*channels*bytes;
    if (a.RowPitch < rowBytes || b.RowPitch < rowBytes ||
        uint64_t(a.RowPitch)*a.Height > a.Bytes.size() || uint64_t(b.RowPitch)*b.Height > b.Bytes.size())
    {
        return false;
    }
    uint64_t changed = 0, count = uint64_t(a.Width)*a.Height*channels;
    auto scalar = [half, normalized, packedNormal](const uint8_t* address, uint32_t component)
    {
        if (packedNormal)
        {
            uint32_t value;
            memcpy(&value,address,sizeof(value));
            const uint32_t mask = component == 3 ? 3u : 1023u;
            return double((value>>(component*10))&mask)/mask;
        }
        if (normalized)
        {
            return double(*address)/255;
        }
        if (half)
        {
            uint16_t value;
            memcpy(&value, address, sizeof(value));
            return double(RenderCapture::HalfToFloat(value));
        }
        float value;
        memcpy(&value, address, sizeof(value));
        return double(value);
    };
    for (uint32_t row = 0; row < a.Height; ++row)
    {
        for (size_t column = 0; column < size_t(a.Width)*channels; ++column)
        {
            const size_t offset = packedNormal ? (column/4)*4 : column*bytes;
            const double x = scalar(a.Bytes.data()+size_t(row)*a.RowPitch+offset,static_cast<uint32_t>(column%channels));
            const double y = scalar(b.Bytes.data()+size_t(row)*b.RowPitch+offset,static_cast<uint32_t>(column%channels));
            if (!std::isfinite(x) || !std::isfinite(y))
            {
                return false;
            }
            changed += std::abs(x-y) > absoluteTolerance;
        }
    }
    WriteGraphicsLog("VG reference comparison changed=%llu components=%llu tolerance=%g\n", changed, count, absoluteTolerance);
    return double(changed) <= double(count)*maximumChangedFraction;
}
bool HasVisibleMovingGeometry(const RenderCapture::TextureData& depth, const RenderCapture::TextureData& velocity)
{
    if (depth.Format != DXGI_FORMAT_R32_FLOAT || velocity.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        !depth.Width || depth.Width != velocity.Width || depth.Height != velocity.Height ||
        depth.RowPitch < uint64_t(depth.Width)*4 || velocity.RowPitch < uint64_t(velocity.Width)*8 ||
        uint64_t(depth.RowPitch)*depth.Height > depth.Bytes.size() ||
        uint64_t(velocity.RowPitch)*velocity.Height > velocity.Bytes.size())
    {
        return false;
    }
    uint64_t visible = 0, moving = 0;
    for (uint32_t y = 0; y < depth.Height; ++y)
    {
        for (uint32_t x = 0; x < depth.Width; ++x)
        {
            float z;
            memcpy(&z,depth.Bytes.data()+size_t(y)*depth.RowPitch+size_t(x)*4,4);
            if (!std::isfinite(z))
            {
                return false;
            }
            if (z <= 0 || z >= 1)
            {
                continue;
            }
            ++visible;
            uint16_t values[2];
            memcpy(values,velocity.Bytes.data()+size_t(y)*velocity.RowPitch+size_t(x)*8+4,4);
            const float vx = RenderCapture::HalfToFloat(values[0]), vy = RenderCapture::HalfToFloat(values[1]);
            if (!std::isfinite(vx) || !std::isfinite(vy))
            {
                return false;
            }
            moving += std::abs(vx) > 0.00001f || std::abs(vy) > 0.00001f;
        }
    }
    return visible > 64 && moving > 64;
}
std::vector<RenderCapture::TextureData> Capture()
{
    std::vector<RenderCapture::TextureData> result;
    for (const auto target : targets)
    {
        RenderCapture::TextureData texture;
        Check(RenderCapture::ReadTexture(RenderTargets::GetGBufferResource(target), texture), "GBuffer reference readback");
        result.push_back(std::move(texture));
    }
    if (auto* shadow = LightingResources::GetShadowMapResource())
    {
        std::vector<uint32_t> layers;
        for (uint32_t pass = 0; pass < LightingResources::GetShadowLightCount(); ++pass)
        {
            UINT layer = 0;
            D3D12_VIEWPORT viewport{};
            D3D12_RECT scissor{};
            bool clear = false;
            if (LightingResources::GetShadowPassInfo(pass,layer,viewport,scissor,clear) &&
                layer < shadow->GetDesc().DepthOrArraySize)
            {
                layers.push_back(layer);
            }
        }
        std::sort(layers.begin(),layers.end());
        layers.erase(std::unique(layers.begin(),layers.end()),layers.end());
        Check(!layers.empty(),"shadow attachments contain active light layers");
        for (const uint32_t slice : layers)
        {
            RenderCapture::TextureData texture;
            Check(RenderCapture::ReadTextureSlice(shadow, texture, slice), "light shadow slice readback");
            result.push_back(std::move(texture));
        }
    }
    return result;
}
void Setup()
{
    outputDirectory = "build/virtualgeometry-phase11";
    std::filesystem::create_directories(outputDirectory);
    std::vector<uint32_t> remove;
    for (auto entity : World::GetView<TransformComponent>())
    {
        if (!ComponentManager::HasComponent<CameraComponent>(entity))
        {
            remove.push_back(entity);
        }
    }
    for (const auto entity : remove)
    {
        World::DestroyEntity(Entity(entity));
    }
    const auto path = outputDirectory/"production-fixture.obj";
    {
        std::ofstream mesh(path);
        mesh << "# regression session " << GetTickCount64() << '\n';
        constexpr uint32_t size = 64;
        mesh << "vn 0 0 -1\n";
        for (uint32_t y = 0; y <= size; ++y)
        {
            for (uint32_t x = 0; x <= size; ++x)
            {
                const float px = float(x)/8-4, py = float(y)/8-4;
                mesh << "v " << px << ' ' << py << ' ' << 0.1f*std::sin(px)*std::cos(py) << '\n';
            }
        }
        for (uint32_t y = 0; y < size; ++y)
        {
            for (uint32_t x = 0; x < size; ++x)
            {
                const uint32_t a = y*(size+1)+x+1, c = a+size+1;
                mesh << "f " << a << "//1 " << c << "//1 " << a+1 << "//1\nf "
                    << a+1 << "//1 " << c << "//1 " << c+1 << "//1\n";
            }
        }
    }
    Entity entity = World::CreateEntity();
    entity.Add<TransformComponent>().Add<StaticModelComponent>().Add<MaterialComponent>().Add<NameComponent>().Add<ShaderComponent>();
    fixture = entity.GetID();
    entity.Get<NameComponent>().Name = "VG Production Regression";
    entity.Get<StaticModelComponent>().ModelId = ModelManager::LoadStaticModel(path.string().c_str(), false);
    auto& material = entity.Get<MaterialComponent>();
    material.ShaderClassMode = MaterialMode::Manual;
    material.ShaderClass = ShaderClass::PBR;
    material.UseTexture = false;
    for (auto type : {LightType::Directional, LightType::Spot, LightType::Point})
    {
        auto light = World::CreateEntity();
        light.Add<TransformComponent>().Add<LightComponent>();
        auto& component = light.Get<LightComponent>();
        component.Type = type;
        component.CastShadow = true;
        component.Position = {1, 3, -5};
        component.Direction = {0, -0.3f, 1};
        component.Range = 30;
        component.Intensity = 4;
        component.InnerAngle = 20;
        component.OuterAngle = 45;
        light.Get<TransformComponent>().Position = component.Position;
        light.Get<TransformComponent>().IsDirty = true;
    }
    auto cameraId = Camera::GetCameraEntity();
    auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(cameraId);
    transform.Position = {0,0,-12};
    transform.IsDirty = true;
    auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(cameraId);
    camera.Target = {0,0,0};
    camera.AllowUserControl = false;
    camera.LockOnTarget = UINT32_MAX;
    DayNightCycle::GetSettings().Running = false;
    DayNightCycle::GetSettings().TimeDriven = false;
    DayNightCycle::GetSettings().AutomaticLighting = false;
    RenderConfiguration::SetRenderMode(RenderMode::DEFERRED);
    RendererSettings::SetComputeGBufferEnabled(false);
    RendererSettings::SetMeshShadersEnabled(false);
    RendererSettings::SetUpscaleMode(UpscaleMode::Bilateral);
    RendererSettings::SetShadowMapMethod(ShadowMapMethod::ShadowMap);
    Raytracing::GetSettings().Enabled = false;
    World::SetVSyncEnabled(false);
    auto& traversal = VirtualGeometryTraversal::GetSettings();
    traversal.ForceHierarchyLevel = 0;
    traversal.View = VirtualGeometryTraversal::DebugView::None;
    traversal.LevelColors = false;
    traversal.OcclusionCulling = false;
    GPUVisibility::GetSettings().LOD = false;
    GPUVisibility::GetSettings().Occlusion = false;
    GPUDrivenRenderer::SetEnabled(true);
    MeshShaderRenderer::GetSettings().Enabled = true;
    Check(BeginBenchmark(outputDirectory, lastError),"benchmark output opened");
}
}
bool BeginBenchmark(const std::filesystem::path& directory, std::string& error)
{
    EndBenchmark();
    error.clear();
    try
    {
        std::filesystem::create_directories(directory);
        std::ofstream hardware(directory/"hardware.tmp");
        hardwareKey = Hardware(hardware);
        hardware.close();
        if (hardwareKey.empty())
        {
            throw std::runtime_error("Cannot identify reference GPU");
        }
        const auto hardwarePath = directory/("hardware-"+hardwareKey+".txt");
        std::filesystem::copy_file(directory/"hardware.tmp", hardwarePath, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::remove(directory/"hardware.tmp");
        const auto session = std::to_string(GetTickCount64());
        benchmark.open(directory/("VirtualGeometry-Final-Benchmark-"+hardwareKey+"-"+session+".csv"));
        passBenchmark.open(directory/("VirtualGeometry-GPU-Passes-"+hardwareKey+"-"+session+".csv"));
        passBenchmark << "frame,pass,cpu_ms,gpu_ms,draws\n";
        if (!benchmark || !passBenchmark)
        {
            throw std::runtime_error("Cannot write VirtualGeometry benchmark");
        }
        benchmark << "session,frame,mode,frame_ms,fps,cpu_ms,gpu_ms,process_ram_bytes,vg_pool_bytes,vg_budget_bytes,"
            "instances,clusters_registered,clusters_visible,triangles_visible,resident_pages,upload_bytes_per_second,draws,indirect_calls,"
            "source_triangles,instanced_source_triangles,local_vram_bytes,visibility_sample_serial\n";
        lastSerial = 0;
        previousUpload = GetVGStatistics().SubmittedBytes;
        previousTime = std::chrono::steady_clock::now();
        lastError.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        lastError = error;
        EndBenchmark();
        return false;
    }
}
void EndBenchmark()
{
    benchmark.close();
    benchmark.clear();
    passBenchmark.close();
    passBenchmark.clear();
}
bool Recording() { return benchmark.is_open(); }
const std::string& LastError() { return lastError; }
void RecordBenchmark()
{
    static bool checkedEnvironment = false;
    if (!checkedEnvironment)
    {
        checkedEnvironment = true;
        char flag[8]{};
        if (!Recording() && GetEnvironmentVariableA("DX12_VG_BENCHMARK", flag, sizeof(flag)) && atoi(flag) != 0)
        {
            std::string error;
            BeginBenchmark("build/virtualgeometry-phase11", error);
        }
    }
    if (!Recording() || !RenderProfiler::HasLatest())
    {
        return;
    }
    const auto& frame = RenderProfiler::GetLatest();
    if (frame.Serial == lastSerial)
    {
        return;
    }
    lastSerial = frame.Serial;
    const auto stats = GetVGStatistics();
    PROCESS_MEMORY_COUNTERS_EX process{};
    process.cb = sizeof(process);
    const bool haveMemory = GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process)) != FALSE;
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now-previousTime).count();
    const uint64_t upload = stats.SubmittedBytes >= previousUpload ? stats.SubmittedBytes-previousUpload : 0;
    previousTime = now;
    previousUpload = stats.SubmittedBytes;
    benchmark << std::setprecision(9) << hardwareKey << ',' << frame.Serial << ',' << frame.GeometryMode
        << ',' << World::GetFrameTimeMs() << ',' << (World::GetFrameTimeMs() > 0 ? 1000/World::GetFrameTimeMs() : 0)
        << ',' << frame.CpuRenderMs << ',';
    if (frame.GpuTimingValid)
    {
        benchmark << frame.GpuRenderMs;
    }
    benchmark << ',';
    if (haveMemory)
    {
        benchmark << process.WorkingSetSize;
    }
    benchmark << ',' << stats.ResidentAllocationBytes << ',' << stats.BudgetBytes << ',' << GpuScene::Statistics().ActiveInstances
        << ',' << stats.RegisteredClusters << ',';
    if (stats.HasVisibilitySample)
    {
        benchmark << stats.FirstVisibleClusters+stats.SecondVisibleClusters;
    }
    benchmark << ',';
    if (stats.HasVisibilitySample)
    {
        benchmark << VirtualGeometryTraversal::DebugCounters()[6];
    }
    benchmark << ',' << stats.ResidentPages << ',' << (elapsed > 0 ? upload/elapsed : 0)
        << ',' << frame.Calls.DrawCalls << ',' << frame.Calls.ExecuteIndirectCalls
        << ',' << stats.SourceTriangles << ',' << stats.InstancedSourceTriangles << ',';
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter3> adapter;
    DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->EnumAdapterByLuid(GraphicsDevice::GetDevice()->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) &&
        SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory)))
    {
        benchmark << memory.CurrentUsage;
    }
    benchmark << ',' << stats.VisibilitySampleSerial << '\n';
    for (const auto& pass : frame.Passes)
    {
        passBenchmark << frame.Serial << ',' << std::quoted(pass.Name) << ',' << pass.CpuMs << ',';
        if (frame.GpuTimingValid)
        {
            passBenchmark << pass.GpuMs;
        }
        passBenchmark << ',' << pass.DrawCalls << '\n';
    }
    if (frame.Serial%30 == 0)
    {
        benchmark.flush();
        passBenchmark.flush();
    }
    if (!benchmark || !passBenchmark)
    {
        lastError = "VirtualGeometry benchmark write failed";
        EndBenchmark();
    }
}
bool ValidationEnabled()
{
    char flag[8]{};
    return GetEnvironmentVariableA("DX12_VG_FINAL_VALIDATION", flag, sizeof(flag)) && atoi(flag) != 0;
}
void BeforeFrame(uint64_t frame)
{
    if (!ValidationEnabled())
    {
        return;
    }
    if (frame == 0)
    {
        Setup();
    }
    const auto& streaming = VirtualGeometryStreaming::GetStatistics();
    if (readyFrame == UINT64_MAX && streaming.Resident > 0 && streaming.Resident == VirtualGeometryStreaming::PageCount())
    {
        readyFrame = frame;
    }
    const uint64_t phase = readyFrame == UINT64_MAX ? 0 : frame-readyFrame;
    GetGeometryRenderingMode() = phase < 8 ? GeometryRenderingMode::VirtualGeometry : GeometryRenderingMode::Legacy;
    if (phase == 2 || phase == 10 || phase == 3 || phase == 11)
    {
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(fixture);
        transform.Position.x = phase%8 == 3 ? 0.25f : 0;
        transform.IsDirty = true;
        GpuSceneChanges::Notify(fixture, GpuSceneDirtyFlags::Transform);
    }
}
void AfterFrame(uint64_t frame)
{
    if (!ValidationEnabled())
    {
        return;
    }
    if (readyFrame == UINT64_MAX)
    {
        return;
    }
    const uint64_t phase = frame-readyFrame;
    if (phase == 3)
    {
        Check(VirtualGeometryTraversal::Active(), "production hierarchy render active");
        Check(VirtualGeometryBinning::Active(), "production material binning active");
        const auto native = MeshShaderRenderer::CaptureNativeRouting();
        Check(native.Error.empty() && native.SuccessfulInstances > 0, "native routing completed without fallback");
        for (const auto instance : GpuScene::GetEntityInstances(fixture))
        {
            const auto generation = GpuScene::InstanceHistoryGeneration(instance);
            Check(GpuScene::ResolveEntity(instance,generation) == fixture, "native Object ID resolves original Entity");
            Check(GpuScene::ResolveEntity(instance,generation+1) == UINT32_MAX, "stale picking sample rejected");
        }
        reference = Capture();
        Check(HasVisibleMovingGeometry(reference[3],reference[4]),"native reference contains visible moving geometry");
        const auto diagnostics = GetVGStatistics();
        Check(diagnostics.VisibleInstanceOverflow == 0 && diagnostics.InvalidIndirectCommands == 0 &&
            diagnostics.BinningRejectedClusters == 0,"no instance/indirect/bin output loss");
        Check(RenderCapture::SaveScenePng((outputDirectory/"virtualgeometry.png").c_str()),"save VG reference image");
    }
    if (phase == 11)
    {
        auto legacy = Capture();
        Check(HasVisibleMovingGeometry(legacy[3],legacy[4]),"legacy reference contains visible moving geometry");
        Check(reference.size() == legacy.size() && !reference.empty(), "same reference attachments");
        for (size_t index = 0; index < std::min(reference.size(), legacy.size()); ++index)
        {
            Check(Compare(reference[index], legacy[index], index == 3 ? 0.0001 : 0.003, 0.005),
                "VG/Legacy geometry material shadow depth velocity parity");
        }
        Check(RenderCapture::SaveScenePng((outputDirectory/"legacy.png").c_str()),"save legacy reference image");
        Check(LightingResources::GetShadowLightCount() > 0, "regular light shadow pipeline participates");
        std::array<bool,3> lightTypes{};
        for (uint32_t pass = 0; pass < LightingResources::GetShadowLightCount(); ++pass)
        {
            LightingResources::SetCurrentShadowPassIndex(pass);
            XMFLOAT4 positionType, direction;
            LightingResources::GetCurrentShadowLight(positionType,direction);
            const int type = static_cast<int>(positionType.w);
            if (type >= 0 && type < 3)
            {
                lightTypes[static_cast<size_t>(type)] = true;
            }
        }
        LightingResources::SetCurrentShadowPassIndex(0);
        Check(lightTypes[0] && lightTypes[1] && lightTypes[2],"directional / point / spot shadow passes present");
        Check(RenderCapture::ReportGraphicsErrors() == 0, "D3D12 validation errors");
        compared = true;
    }
    if (phase == 7)
    {
        StaticModelResource warm;
        Check(warm.LoadDerivedVirtualGeometry((outputDirectory/"production-fixture.obj").string().c_str(),
            GraphicsDevice::GetDevice(),false) && warm.UsesDerivedVirtualGeometry(), "source imports through persistent derived index");
    }
}
bool Finish()
{
    if (!ValidationEnabled())
    {
        return true;
    }
    Check(GraphicsDevice::WaitForGpuIdle(),"final GPU work completed");
    Check(GraphicsDevice::GpuBasedValidationEnabled(), "GPU Based Validation actually enabled");
    ComPtr<ID3D12InfoQueue> debugQueue;
    Check(SUCCEEDED(GraphicsDevice::GetDevice()->QueryInterface(IID_PPV_ARGS(&debugQueue))),
        "D3D12 debug layer available (absence cannot count as zero errors)");
    Check(compared, "reference sequence completed before frame limit");
    Check(RenderCapture::ReportGraphicsErrors() == 0, "final D3D12 validation errors");
    EndBenchmark();
    std::ofstream report(outputDirectory/"VirtualGeometry-Final-Validation.txt");
    report << "failures=" << failures << "\ncompared=" << compared << '\n';
    return failures == 0 && bool(report);
}
}
