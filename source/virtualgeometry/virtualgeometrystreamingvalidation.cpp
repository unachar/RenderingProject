#include "pch.h"
#include "virtualgeometrystreamingvalidation.h"
#include "virtualgeometrystreaming.h"
#include "virtualgeometrytraversal.h"
#include "virtualgeometrypagestress.h"
#include "virtualgeometryruntime.h"
#include "world.h"
#include "componentmanager.h"
#include "modelmanager.h"
#include "camera.h"
#include "gpuscene/gpuscene.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "visibility/gpuvisibility.h"
#include "meshshader/meshshaderrenderer.h"
#include "renderconfiguration.h"
#include "renderersettings.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "rendercapture.h"
#include "rendertargets.h"
#include "daynightcycle.h"
#include <filesystem>
#include <fstream>
#include <psapi.h>

namespace VirtualGeometryStreamingValidation
{
namespace
{
uint32_t failures = 0, assetCount = 0, budgetMiB = 0, comparisons = 0;
uint64_t sourceTriangles = 0, readyFrame = UINT64_MAX;
uint64_t timingSerial = UINT64_MAX, timingSamples = 0;
uint64_t maximumPendingGraphics = 0;
uint32_t routingSamples = 0, maximumSelected = 0, maximumLeaves = 0;
double cpuMilliseconds = 0, gpuMilliseconds = 0, maximumCpuMilliseconds = 0;
uint64_t steadySamples = 0;
double steadyCpuMilliseconds = 0, steadyGpuMilliseconds = 0;
std::vector<uint8_t> referenceCoverage;
std::filesystem::path directory;
std::shared_ptr<VirtualGeometryPageCache::PageManifest> admissionProbe;
void Check(bool condition, const char* message)
{
    if (!condition) ++failures;
    WriteGraphicsLog("Geometry streaming validation: %s %s\n",condition ? "PASS" : "FAIL",message);
}
std::vector<uint8_t> Coverage()
{
    RenderCapture::TextureData texture;
    if (!RenderCapture::ReadTexture(RenderTargets::GetGBufferResource(GBufferType::DEPTH),texture) ||
        texture.Format != DXGI_FORMAT_R32_FLOAT || !texture.Width || !texture.Height)
    {
        return {};
    }
    std::vector<uint8_t> result(size_t(texture.Width)*texture.Height);
    for (uint32_t y = 0; y < texture.Height; ++y)
    {
        const auto* depth = reinterpret_cast<const float*>(texture.Bytes.data()+size_t(y)*texture.RowPitch);
        for (uint32_t x = 0; x < texture.Width; ++x)
        {
            result[size_t(y)*texture.Width+x] = uint8_t(std::isfinite(depth[x]) && depth[x] > 0 && depth[x] < 1);
        }
    }
    return result;
}
void Setup()
{
    char path[4096]{};
    GetEnvironmentVariableA("DX12_VG_STRESS_DIRECTORY",path,sizeof(path));
    directory = path;
    char requestedText[32]{};
    GetEnvironmentVariableA("DX12_VG_STRESS_TRIANGLES",requestedText,sizeof(requestedText));
    uint64_t requested = std::stoull(requestedText);
    assetCount = uint32_t((requested+VirtualGeometryPageStress::SourceTrianglesPerAsset-1)/VirtualGeometryPageStress::SourceTrianglesPerAsset);
    budgetMiB = (assetCount+15)/16+4;
    VirtualGeometryStreaming::GetSettings().BudgetMiB = budgetMiB;
    std::vector<uint32_t> remove;
    for (uint32_t entity = 0; entity < Registry::GetEntities().size(); ++entity)
    {
        if (Registry::IsAlive(entity) && !ComponentManager::HasComponent<CameraComponent>(entity)) remove.push_back(entity);
    }
    for (uint32_t entity : remove) World::DestroyEntity(Entity(entity));
    for (uint32_t asset = 0; asset < assetCount; ++asset)
    {
        auto manifest = std::make_shared<VirtualGeometryPageCache::PageManifest>();
        std::string error;
        if (!VirtualGeometryPageCache::Open(directory/("asset-"+std::to_string(asset)+".vgp2"),*manifest,error))
        {
            throw std::runtime_error(error);
        }
        for (const auto& cluster : manifest->Metadata.Clusters)
        {
            if (cluster.Level == 0) sourceTriangles += cluster.TriangleCount;
        }
        if (asset == 0)
        {
            admissionProbe = std::make_shared<VirtualGeometryPageCache::PageManifest>(*manifest);
            std::fill(admissionProbe->PinnedPages.begin(),admissionProbe->PinnedPages.end(),uint8_t(1));
        }
        int modelId = ModelManager::LoadStaticModel(manifest->Path.string().c_str(),false);
        auto* model = ModelManager::GetStaticModel(modelId);
        if (!model || model->GetMeshCount() != 1) throw std::runtime_error("Streaming fallback mesh import failed");
        // Standard VG importer supplies pinned root fallback and the original Entity identity.
        Entity entity = World::CreateEntity();
        entity.Add<TransformComponent>().Add<StaticModelComponent>().Add<MaterialComponent>().Add<ShaderComponent>().Add<NameComponent>();
        entity.Get<NameComponent>().Name = "Streaming disk asset "+std::to_string(asset);
        entity.Get<StaticModelComponent>().ModelId = modelId;
        entity.Get<TransformComponent>().Position = {float(asset%16)*10,float(asset/16)*10,0};
        auto& material = entity.Get<MaterialComponent>();
        material.ShaderClassMode = MaterialMode::Manual;
        material.ShaderClass = ShaderClass::PBR;
        material.UseTexture = false;
    }
    Check(sourceTriangles >= requested,"scene contains requested source triangles without an instance multiplier");
    WriteGraphicsLog("Geometry streaming scene: source triangles=%llu assets=%u budget MiB=%u\n",sourceTriangles,assetCount,budgetMiB);
    World::SetVSyncEnabled(false);
}
}
bool Enabled()
{
    char value[8]{};
    return GetEnvironmentVariableA("DX12_VIRTUAL_GEOMETRY_STREAMING_TEST",value,sizeof(value)) && atoi(value) != 0;
}
void BeforeFrame(uint64_t frame)
{
    if (frame == 0) Setup();
    if (frame == 48)
    {
        uint32_t assetsBefore = VirtualGeometryResources::AssetCount();
        uint64_t clustersBefore = VirtualGeometryResources::RegisteredClusterCount();
        uint32_t pagesBefore = VirtualGeometryStreaming::PageCount();
        bool rejected = true;
        for (uint32_t attempt = 0; attempt < 32; ++attempt)
        {
            rejected &= VirtualGeometryResources::RegisterManifest(admissionProbe) == UINT32_MAX;
        }
        Check(rejected && assetsBefore == VirtualGeometryResources::AssetCount() &&
            clustersBefore == VirtualGeometryResources::RegisteredClusterCount() && pagesBefore == VirtualGeometryStreaming::PageCount(),
            "repeated pinned-budget rejection leaves asset, metadata and Page counts unchanged");
        admissionProbe.reset();
    }
    RenderConfiguration::SetRenderMode(RenderMode::DEFERRED);
    RendererSettings::SetMeshShadersEnabled(false);
    RendererSettings::SetComputeGBufferEnabled(false);
    RendererSettings::SetTwoPhaseOcclusionEnabled(false);
    RendererSettings::SetUpscaleMode(UpscaleMode::Bilateral);
    DayNightCycle::GetSettings().Running = false;
    bool traditional = frame%12 == 1;
    GPUDrivenRenderer::SetEnabled(!traditional);
    MeshShaderRenderer::GetSettings().Enabled = !traditional;
    auto& options = VirtualGeometryTraversal::GetSettings();
    options.Enabled = !traditional;
    options.DebugReadback = false;
    options.LevelColors = false;
    options.FrustumCulling = true;
    options.ConeCulling = false;
    options.OcclusionCulling = false;
    options.ForceHierarchyLevel = 0;
    options.MaxGroupStates = 4000000;
    options.MaxSelectedClusters = 4000000;
    GPUVisibility::GetSettings().LOD = false;
    GPUVisibility::GetSettings().Occlusion = false;
    if (frame == 60) VirtualGeometryStreaming::GetSettings().BudgetMiB = budgetMiB-1;
    if (frame == 120) VirtualGeometryStreaming::GetSettings().BudgetMiB = budgetMiB;
    uint32_t target = uint32_t((frame/12)%assetCount);
    if (frame%12 > 1) target = uint32_t((frame*17)%assetCount);
    float distance = frame/12%3 == 0 ? 5.0f : frame/12%3 == 1 ? 30.0f : 100.0f;
    auto cameraEntity = Camera::GetCameraEntity();
    if (Registry::IsAlive(cameraEntity))
    {
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity);
        auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(cameraEntity);
        float x = float(target%16)*10;
        float y = float(target/16)*10;
        transform.Position = {x,y,-distance};
        transform.IsDirty = true;
        camera.Target = {x,y,0};
    }
}
void AfterFrame(uint64_t frame)
{
    const auto& stats = VirtualGeometryStreaming::GetStatistics();
    uint64_t submitted = GraphicsDevice::GetPendingSubmissionFence()-1;
    uint64_t completed = GraphicsDevice::GetCompletedFence();
    if (submitted > completed) maximumPendingGraphics = std::max(maximumPendingGraphics,submitted-completed);
    if (RenderProfiler::HasLatest())
    {
        const auto& timing = RenderProfiler::GetLatest();
        if (timing.Serial != timingSerial)
        {
            timingSerial = timing.Serial;
            ++timingSamples;
            cpuMilliseconds += timing.CpuRenderMs;
            gpuMilliseconds += timing.GpuRenderMs;
            maximumCpuMilliseconds = std::max(maximumCpuMilliseconds,timing.CpuRenderMs);
            if (readyFrame != UINT64_MAX && frame > readyFrame+12)
            {
                ++steadySamples;
                steadyCpuMilliseconds += timing.CpuRenderMs;
                steadyGpuMilliseconds += timing.GpuRenderMs;
            }
        }
    }
    if (readyFrame == UINT64_MAX && stats.Pinned == assetCount && stats.Resident >= stats.Pinned)
    {
        bool ready = VirtualGeometryResources::AssetCount() == assetCount;
        for (uint32_t asset = 0; asset < VirtualGeometryResources::AssetCount(); ++asset) ready &= VirtualGeometryResources::Ready(asset);
        if (ready) readyFrame = frame;
    }
    if (frame%24 == 0)
    {
        WriteGraphicsLog("Geometry streaming frame=%llu resident=%llu requested=%llu loading=%llu evicted=%llu evictions=%llu bytes=%llu/%llu requests=%llu copy=%llu\n",
            frame,stats.Resident,stats.Requested,stats.Loading,stats.Evicted,stats.Evictions,stats.ReservedBytes,stats.BudgetBytes,stats.GpuRequests,stats.CompletedBytes);
#ifdef _DEBUG
        Check(RenderCapture::ReportGraphicsErrors() == 0,"D3D12 errors during streaming camera movement");
#endif
    }
    if (readyFrame == UINT64_MAX || frame < readyFrame+4) return;
    if (frame%12 == 0)
    {
        auto routing = MeshShaderRenderer::CaptureNativeRouting();
        Check(routing.Error.empty(),"unique native cut and exclusive native/legacy/fallback routing");
        if (!routing.Error.empty()) WriteGraphicsLog("Geometry routing capture: %s\n",routing.Error.c_str());
        maximumSelected = std::max(maximumSelected,routing.SelectedClusters);
        maximumLeaves = std::max(maximumLeaves,routing.LeafClusters);
        ++routingSamples;
        referenceCoverage = Coverage();
        Check(std::count(referenceCoverage.begin(),referenceCoverage.end(),uint8_t(1)) != 0,"native or parent fallback renders visible geometry");
    }
    if (frame%12 == 1 && !referenceCoverage.empty())
    {
        auto traditional = Coverage();
        Check(traditional == referenceCoverage,"Page loading/eviction preserves traditional raster coverage");
        ++comparisons;
        referenceCoverage.clear();
    }
}
bool Finish()
{
    Check(GraphicsDevice::WaitForGpuIdle(),"final graphics work completed");
    char gpuValidation[8]{};
    if (GetEnvironmentVariableA("DX12_GPU_BASED_VALIDATION",gpuValidation,sizeof(gpuValidation)) && atoi(gpuValidation))
    {
        Check(GraphicsDevice::GpuBasedValidationEnabled(),"requested GPU Based Validation actually enabled");
    }
    const auto& stats = VirtualGeometryStreaming::GetStatistics();
    Check(readyFrame != UINT64_MAX,"all source assets reached native root residency");
    Check(comparisons >= 4,"repeated raster comparisons after camera teleports");
    Check(routingSamples >= 4 && maximumSelected > 0,"repeated native cluster and routing inspection");
    Check(maximumPendingGraphics > 0,"ordinary stress frames submit graphics before prior work completes");
    Check(stats.IoErrors == 1,"single injected I/O failure recovered without additional read errors");
    Check(stats.GpuRequests > 0 && stats.CpuRequests > 0,"GPU requests reached the CPU loader");
    Check(stats.Evictions > 0,"working set exceeds pool budget and evicts old Pages");
    Check(stats.CompletedBytes > uint64_t(assetCount)*65536,"fine Pages completed Copy after pinned roots");
    Check(stats.PoolBytes <= stats.BudgetBytes && stats.ReservedBytes <= stats.BudgetBytes,"physical pool and reservations fit restored budget");
#ifdef _DEBUG
    Check(RenderCapture::ReportGraphicsErrors() == 0,"final D3D12 errors zero");
#endif
    WriteGraphicsLog("Geometry streaming final: source=%llu comparisons=%u GPU requests=%llu SSD bytes=%llu Copy bytes=%llu VRAM=%llu staging=%llu evictions=%llu I/O errors=%llu CPU ms=%.3f I/O ms=%.3f\n",
        sourceTriangles,comparisons,stats.GpuRequests,stats.DiskBytes,stats.CompletedBytes,stats.PoolBytes,stats.StagingBytes,stats.Evictions,stats.IoErrors,stats.CpuMilliseconds,stats.IoMilliseconds);
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    bool memoryValid = K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)) != FALSE;
    WriteGraphicsLog("Geometry streaming performance: samples=%llu average CPU render ms=%.3f average GPU render ms=%.3f maximum CPU render ms=%.3f RAM working set=%llu private bytes=%llu memory valid=%u\n",
        timingSamples,cpuMilliseconds/std::max(uint64_t(1),timingSamples),gpuMilliseconds/std::max(uint64_t(1),timingSamples),maximumCpuMilliseconds,
        uint64_t(memory.WorkingSetSize),uint64_t(memory.PrivateUsage),unsigned(memoryValid));
    WriteGraphicsLog("Geometry streaming steady performance: samples=%llu CPU render ms=%.3f GPU render ms=%.3f (after root warmup)\n",
        steadySamples,steadyCpuMilliseconds/std::max(uint64_t(1),steadySamples),steadyGpuMilliseconds/std::max(uint64_t(1),steadySamples));
    WriteGraphicsLog("Geometry streaming routing: samples=%u maximum selected clusters=%u maximum leaves=%u maximum pending graphics fence values=%llu synchronous counters every frame=0\n",
        routingSamples,maximumSelected,maximumLeaves,maximumPendingGraphics);
    WriteGraphicsLog("Geometry streaming validation failures = %u\n",failures);
    return failures == 0;
}
}
