#include "pch.h"
#include "virtualgeometryvalidation.h"
#include "virtualgeometrytraversal.h"
#include "virtualgeometrystreaming.h"
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
#include "gpuscene/gpuscenechanges.h"
#include <filesystem>
#include <fstream>

namespace VirtualGeometryValidation
{
namespace
{
uint32_t failures = 0, fixture = UINT32_MAX, nearClusters = 0, farClusters = 0;
uint64_t warmupStart = UINT64_MAX;
bool completedChecks = false;
std::vector<bool> leafCoverage;
std::array<RenderCapture::TextureData, 4> nativeReference;
constexpr std::array<GBufferType, 4> comparisonTargets = {GBufferType::BASE_COLOR, GBufferType::NORMAL,
                                                          GBufferType::MATERIAL, GBufferType::EMISSION_AO};

void Check(bool condition, const char* label)
{
    WriteGraphicsLog("VirtualGeometry validation: %s %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition)
    {
        ++failures;
    }
}

void CheckGraphicsErrors()
{
#ifdef _DEBUG
    Check(RenderCapture::ReportGraphicsErrors() == 0, "D3D12 state validation");
#else
    WriteGraphicsLog("VirtualGeometry validation: SKIP D3D12 debug-layer checks in Release\n");
#endif
}

void WriteFixture()
{
    std::filesystem::create_directories("build/virtualgeometry-phase6");
    std::ofstream file("build/virtualgeometry-phase6/curved.obj");
    constexpr uint32_t size = 96;
    file << "vn 0 0 -1\n";
    for (uint32_t y = 0; y <= size; ++y)
    {
        for (uint32_t x = 0; x <= size; ++x)
        {
            float px = float(x) * 8 / size - 4, py = float(y) * 8 / size - 4;
            float z = 0.4f * sinf(px * 2.3f) * cosf(py * 1.7f);
            file << "v " << px << ' ' << py << ' ' << z << '\n';
        }
    }
    for (uint32_t y = 0; y < size; ++y)
    {
        for (uint32_t x = 0; x < size; ++x)
        {
            uint32_t a = y * (size + 1) + x + 1, b = a + 1, c = a + size + 1, d = c + 1;
            file << "f " << a << "//1 " << c << "//1 " << b << "//1\nf " << b << "//1 " << c << "//1 " << d << "//1\n";
        }
    }
}

bool Capture(const wchar_t* path)
{
    return RenderCapture::SaveTexturePng(RenderTargets::GetGBufferResource(GBufferType::EMISSION_AO), path);
}

std::vector<bool> Coverage()
{
    RenderCapture::TextureData texture;
    if (!RenderCapture::ReadTexture(RenderTargets::GetGBufferResource(GBufferType::EMISSION_AO), texture) ||
        texture.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        return {};
    }
    std::vector<bool> mask;
    mask.reserve(size_t(texture.Width) * texture.Height);
    for (uint32_t row = 0; row < texture.Height; ++row)
    {
        auto pixels = reinterpret_cast<const uint16_t*>(texture.Bytes.data() + size_t(row) * texture.RowPitch);
        for (uint32_t column = 0; column < texture.Width; ++column)
        {
            float color = RenderCapture::HalfToFloat(pixels[column * 4]) +
                          RenderCapture::HalfToFloat(pixels[column * 4 + 1]) +
                          RenderCapture::HalfToFloat(pixels[column * 4 + 2]);
            mask.push_back(color > 0.1f);
        }
    }
    return mask;
}

bool HasVisibleCenterDepth()
{
    RenderCapture::TextureData depth;
    if (!RenderCapture::ReadTexture(RenderTargets::GetGBufferResource(GBufferType::DEPTH), depth) ||
        depth.Format != DXGI_FORMAT_R32_FLOAT || !depth.Width || !depth.Height)
    {
        return false;
    }
    float centerDepth = 1;
    memcpy(&centerDepth,
           depth.Bytes.data() + size_t(depth.Height / 2) * depth.RowPitch + size_t(depth.Width / 2) * sizeof(float),
           sizeof(float));
    return std::isfinite(centerDepth) && centerDepth >= 0 && centerDepth < 1;
}

void CompareTraditionalRaster()
{
    uint64_t changed = 0, compared = 0;
    for (size_t target = 0; target < comparisonTargets.size(); ++target)
    {
        RenderCapture::TextureData traditional;
        const auto& native = nativeReference[target];
        if (!RenderCapture::ReadTexture(RenderTargets::GetGBufferResource(comparisonTargets[target]), traditional) ||
            traditional.Width != native.Width || traditional.Height != native.Height ||
            traditional.Format != native.Format || native.Bytes.empty())
        {
            Check(false, "native/traditional GBuffer readback dimensions");
            return;
        }
        const auto description = RenderTargets::GetGBufferResource(comparisonTargets[target])->GetDesc();
        UINT64 rowBytes = 0;
        GraphicsDevice::GetDevice()->GetCopyableFootprints(&description, 0, 1, 0, nullptr, nullptr, &rowBytes, nullptr);
        for (uint32_t row = 0; row < native.Height; ++row)
        {
            for (size_t byte = 0; byte < rowBytes; ++byte)
            {
                changed += abs(int(native.Bytes[size_t(row) * native.RowPitch + byte]) -
                               int(traditional.Bytes[size_t(row) * traditional.RowPitch + byte])) > 1;
                ++compared;
            }
        }
    }
    WriteGraphicsLog("VirtualGeometry native/traditional GBuffer changed bytes = %llu / %llu\n", changed, compared);
    Check(compared != 0 && changed <= compared / 10000,
          "native leaf material/normal GBuffer matches traditional raster");
}
} // namespace

bool Enabled()
{
    char value[8]{};
    return GetEnvironmentVariableA("DX12_VIRTUAL_GEOMETRY_STRESS_TEST", value, sizeof(value)) && atoi(value) != 0;
}

void BeforeFrame(uint64_t frame)
{
    if (frame == 0)
    {
        std::vector<uint32_t> remove;
        for (uint32_t entity = 0; entity < Registry::GetEntities().size(); ++entity)
        {
            if (Registry::IsAlive(entity) && !ComponentManager::HasComponent<CameraComponent>(entity))
            {
                remove.push_back(entity);
            }
        }
        for (uint32_t entity : remove)
        {
            World::DestroyEntity(Entity(entity));
        }
        WriteFixture();
        int model = ModelManager::LoadStaticModel("build/virtualgeometry-phase6/curved.obj", false);
        Check(model >= 0, "import curved hierarchy fixture");
        Entity entity = World::CreateEntity();
        entity.Add<TransformComponent>()
            .Add<StaticModelComponent>()
            .Add<MaterialComponent>()
            .Add<ShaderComponent>()
            .Add<NameComponent>();
        fixture = entity.GetID();
        entity.Get<NameComponent>().Name = "Native hierarchy acceptance fixture";
        entity.Get<StaticModelComponent>().ModelId = model;
        auto& material = entity.Get<MaterialComponent>();
        material.ShaderClassMode = MaterialMode::Manual;
        material.ShaderClass = ShaderClass::PBR;
        material.UseTexture = false;
        World::SetVSyncEnabled(false);
        auto* imported = ModelManager::GetStaticModel(model);
        Check(imported && imported->GetMeshCount() != 0, "native fixture imported mesh");
        if (imported && imported->GetMeshCount())
        {
            const auto& runtime = imported->GetMeshData(0).VirtualGeometryLODs[0];
            Check(runtime && runtime->Error.empty() && runtime->Hierarchy.Levels.size() > 1,
                  "fixture has drawable parent hierarchy");
        }
    }
    if (warmupStart == UINT64_MAX && frame > 0 && VirtualGeometryStreaming::PageCount() > 0 &&
        VirtualGeometryStreaming::GetStatistics().Resident == VirtualGeometryStreaming::PageCount())
    {
        warmupStart = frame;
        WriteGraphicsLog("VirtualGeometry streaming warmup completed at frame %llu\n", frame);
    }
    frame = warmupStart == UINT64_MAX ? 0 : frame - warmupStart;
    RenderConfiguration::SetRenderMode(RenderMode::DEFERRED);
    RendererSettings::SetMeshShadersEnabled(false);
    RendererSettings::SetComputeGBufferEnabled(false);
    RendererSettings::SetTwoPhaseOcclusionEnabled(false);
    RendererSettings::SetUpscaleMode(UpscaleMode::Bilateral);
    DayNightCycle::GetSettings().Running = false;
    GPUDrivenRenderer::SetEnabled(true);
    MeshShaderRenderer::GetSettings().Enabled = true;
    auto& options = VirtualGeometryTraversal::GetSettings();
    options.Enabled = frame < 20;
    options.DebugReadback = true;
    options.LevelColors = true;
    options.FrustumCulling = false;
    options.ConeCulling = false;
    options.OcclusionCulling = false;
    options.TargetErrorPixels = 1;
    options.ForceHierarchyLevel = frame < 4 ? 0 : frame < 8 ? 2 : -1;
    if (frame >= 24)
    {
        options.Enabled = frame < 28 || frame >= 32;
        options.LevelColors = false;
        options.ForceHierarchyLevel = 0;
        MeshShaderRenderer::GetSettings().Enabled = frame < 28 || frame >= 32;
        GPUDrivenRenderer::SetEnabled(frame < 28 || frame >= 32);
    }
    if (frame == 32)
    {
        // Manual Unlit is rendered by the traditional path, outside native eligibility.
        Entity(fixture).Get<MaterialComponent>().ShaderClass = ShaderClass::Unlit;
        GpuSceneChanges::Notify(fixture, GpuSceneDirtyFlags::Material);
    }
    GPUVisibility::GetSettings().LOD = true;
    if (frame >= 24)
    {
        GPUVisibility::GetSettings().LOD = false;
    }
    GPUVisibility::GetSettings().Occlusion = false;
    auto cameraEntity = Camera::GetCameraEntity();
    if (Registry::IsAlive(cameraEntity))
    {
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity);
        auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(cameraEntity);
        transform.Position = {0, 0, frame >= 12 && frame < 16 ? -100.0f : -5.0f};
        transform.IsDirty = true;
        camera.Target = {0, 0, 0};
    }
}

void AfterFrame(uint64_t frame)
{
    if (warmupStart == UINT64_MAX)
    {
        return;
    }
    frame -= warmupStart;
    if (frame == 3 || frame == 7 || frame == 11 || frame == 15)
    {
        Check(VirtualGeometryTraversal::Active(), "native traversal active in frame pipeline");
        const auto& sample = VirtualGeometryTraversal::DebugCounters();
        Check(sample[4] != 0 && sample[1] != 0 && sample[3] == 0,
              "complete native selection without capacity fallback");
        if (frame == 3)
        {
            Check(sample[8] == sample[1], "Force Level 0 selects leaves");
            Check(Capture(L"build/virtualgeometry-phase6/forced-leaf.png"), "capture selected leaf colors");
            leafCoverage = Coverage();
            Check(!leafCoverage.empty(), "read leaf raster coverage");
        }
        if (frame == 7)
        {
            uint32_t parents = 0;
            for (uint32_t level = 1; level < 32; ++level)
            {
                parents += sample[8 + level];
            }
            Check(parents != 0, "Force Level renders nonleaf clusters");
            Check(Capture(L"build/virtualgeometry-phase6/forced-parent.png"), "capture actual parent colors");
            Check(Coverage() == leafCoverage, "parent transition preserves full raster coverage without cracks");
        }
        if (frame == 11)
        {
            nearClusters = sample[1];
            uint32_t selectedLevels = 0;
            for (uint32_t level = 0; level < 32; ++level)
            {
                selectedLevels += sample[8 + level] != 0;
            }
            Check(selectedLevels > 1, "one mesh selects multiple hierarchy levels simultaneously");
            Check(Capture(L"build/virtualgeometry-phase6/near.png"), "capture automatic near hierarchy");
            Check(Coverage() == leafCoverage, "mixed-level selection preserves complete raster coverage");
        }
        if (frame == 15)
        {
            farClusters = sample[1];
            Check(farClusters < nearClusters, "camera distance coarsens cluster selection");
            Check(Capture(L"build/virtualgeometry-phase6/far.png"), "capture automatic far hierarchy");
        }
    }
    if (frame == 19)
    {
        Check(VirtualGeometryTraversal::DebugCounters()[1] == nearClusters,
              "camera return refines back to near detail");
    }
    if (frame == 23)
    {
        Check(MeshShaderRenderer::IsActive() && VirtualGeometryTraversal::DebugCounters()[4] == 0,
              "native disabled restores legacy discrete LOD path");
        CheckGraphicsErrors();
    }
    if (frame == 27)
    {
        Check(HasVisibleCenterDepth(), "native material reference contains visible geometry");
        Check(VirtualGeometryTraversal::DebugCounters()[1] != 0 &&
                  VirtualGeometryTraversal::DebugCounters()[8] == VirtualGeometryTraversal::DebugCounters()[1],
              "material reference uses native leaves");
        for (size_t target = 0; target < comparisonTargets.size(); ++target)
        {
            Check(RenderCapture::ReadTexture(RenderTargets::GetGBufferResource(comparisonTargets[target]),
                                             nativeReference[target]),
                  "capture native material reference");
        }
    }
    if (frame == 31)
    {
        Check(HasVisibleCenterDepth(), "traditional material comparison contains visible geometry");
        CompareTraditionalRaster();
    }
    if (frame == 35)
    {
        completedChecks = true;
        Check(!GPUDrivenRenderer::SupportsEntity(fixture) && !GPUDrivenRenderer::HandlesEntity(fixture),
              "unsupported Unlit material uses traditional renderer");
        Check(HasVisibleCenterDepth(), "unsupported material retains visible center depth");
        CheckGraphicsErrors();
    }
}

bool Finish()
{
    Check(completedChecks, "all 36 acceptance stages executed after streaming warmup");
    CheckGraphicsErrors();
    WriteGraphicsLog("VirtualGeometry validation failures = %u\n", failures);
    return failures == 0;
}
} // namespace VirtualGeometryValidation
