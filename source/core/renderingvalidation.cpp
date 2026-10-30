#include "pch.h"
#include "materialvalidation.h"
#include "thumbnailvalidation.h"
#include "cloud/cloudvalidation.h"
#include "renderingvalidation.h"
#include "meshlets/meshletvalidation.h"
#include "virtualgeometry/virtualgeometryvalidation.h"
#include "virtualgeometry/virtualgeometryproduction.h"
#include "virtualgeometry/virtualgeometrystreamingvalidation.h"
#include "gpudriven/gpudrivenvalidation.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "gpuscene/gpuscenevalidation.h"
#include "visibility/gpuvisibilityvalidation.h"
#include "rendercapture.h"
#include "referencecapture.h"
#include "referenceintegrator.h"
#include "rendertargets.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "renderconfiguration.h"
#include "raytracing.h"
#include "dlssupscaler.h"
#include "temporalupscaler.h"
#include "renderersettings.h"
#include "imguimanager.h"
#include "renderprofiler.h"
#include "proceduralscene.h"
#include "primitivegeometry.h"
#include "simulationconfiguration.h"
#include "daynightcycle.h"
#include "sun.h"
#include "projectmanager.h"
#include "world.h"
#include "vertexdeformation.h"
#include "vertexdeformationmath.h"
#include "nrddenoiser.h"
#include "meshshaderpipeline.h"
#include "rayreflections.h"
#include "lightingresources.h"
#include "camera.h"
#include "modelmanager.h"
#include "animator.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cmath>

namespace
{
    EntityID g_Terrain = g_kINVALID_ENTITY;
    EntityID g_Mover = g_kINVALID_ENTITY;
    UINT g_Failures = 0;
    float g_NoonSkyLuminance = 0.0f;
    uint64_t g_FrameCount = 0;
    bool g_GpuDrivenBeforeMeshFixture = true;
    UINT g_TsrJitterBeforeDraw = 0;
    double g_PausedTime = 0;
    uint64_t g_OriginalTerrainHash = 0;
    double g_PreviousVertexTime = 0;
    array<RenderCapture::TextureData, 4> g_PlayShadowReference;
    bool g_PlayCacheEnabled = true;

    struct ShadowBenchmarkSample
    {
        uint64_t Count = 0;
        uint64_t Vertices = 0;
        uint64_t Draws = 0;
        uint64_t MaxVertices = 0;
        double Gpu = 0;
        double Cpu = 0;
        double ShadowGpu = 0;
        double FrameMs = 0;
        vector<RenderProfiler::PassTiming> PassTotals;
        uint64_t GpuSamples = 0;
    };

    array<ShadowBenchmarkSample, 3> g_ShadowBenchmark{};
    uint64_t g_ShadowBenchmarkSerial = 0;
    uint32_t g_ProfilerFaultMask = 0;
    uint64_t g_ProfilerFaultSerial = 0;

    bool PlayRenderTestEnabled()
    {
        char value[8]{};
        GetEnvironmentVariableA("DX12_PLAY_RENDER_TEST", value, sizeof(value));
        return atoi(value) != 0;
    }

    bool ShadowBenchmarkEnabled()
    {
        char value[8]{};
        GetEnvironmentVariableA("DX12_PLAY_RENDER_BENCH", value, sizeof(value));
        return atoi(value) != 0;
    }

    void RecordShadowBenchmark()
    {
        const auto& snapshot = RenderProfiler::GetLatest();
        if (!snapshot.PipelineStatisticsValid || snapshot.Serial == g_ShadowBenchmarkSerial)
        {
            return;
        }
        g_ShadowBenchmarkSerial = snapshot.Serial;
        const uint64_t serial = snapshot.Serial;
        const int phase = serial >= 27 && serial <= 75
                              ? 1
                              : (serial >= 107 && serial <= 155 ? 2 : (serial >= 8 && serial <= 18 ? 0 : -1));
        if (phase < 0)
        {
            return;
        }
        auto& sample = g_ShadowBenchmark[phase];
        ++sample.Count;
        sample.Vertices += snapshot.Pipeline.IAVertices;
        sample.MaxVertices = max(sample.MaxVertices, snapshot.Pipeline.IAVertices);
        sample.Draws += snapshot.Calls.DrawCalls;
        sample.Gpu += snapshot.GpuRenderMs;
        sample.Cpu += snapshot.CpuRenderMs;
        sample.FrameMs += World::GetFrameTimeMs();
        if (snapshot.GpuTimingValid)
        {
            ++sample.GpuSamples;
            for (const auto& pass : snapshot.Passes)
            {
                if (pass.Name == "Shadow")
                {
                    sample.ShadowGpu += pass.GpuMs;
                }
                auto total = find_if(sample.PassTotals.begin(), sample.PassTotals.end(), [&](const auto& entry) {
                    return entry.Name == pass.Name;
                });
                if (total == sample.PassTotals.end())
                {
                    sample.PassTotals.push_back({pass.Name});
                    total = prev(sample.PassTotals.end());
                }
                total->CpuMs += pass.CpuMs;
                total->GpuMs += pass.GpuMs;
                total->DrawCalls += pass.DrawCalls;
            }
        }
    }

    void Check(bool passed, const char* name);

    const string& VmdCameraTestPath()
    {
        static const string path = [] {
            char value[2048]{};
            GetEnvironmentVariableA("DX12_VMD_CAMERA_TEST", value, sizeof(value));
            return string(value);
        }();
        return path;
    }

    struct VmdCameraTestState
    {
        EntityID Camera = g_kINVALID_ENTITY;
        EntityID OtherCamera = g_kINVALID_ENTITY;
        EntityID EditorCamera = g_kINVALID_ENTITY;
        TransformComponent OriginalTransform{};
        CameraComponent OriginalCamera{};
        XMFLOAT3 EditorPosition{};
        XMFLOAT3 PausedPosition{};
        float PausedTime = 0.0f;
        bool SawPerspective = false;
        bool SawOrthographic = false;
        bool Completed = false;
    };

    VmdCameraTestState g_VmdCameraTest;

    bool NearCameraValue(float actual, float expected)
    {
        return fabsf(actual - expected) < 0.001f;
    }

    bool SameCameraPosition(const XMFLOAT3& a, const XMFLOAT3& b)
    {
        return NearCameraValue(a.x, b.x) && NearCameraValue(a.y, b.y) && NearCameraValue(a.z, b.z);
    }

    void BeforeVmdCameraTestFrame(uint64_t frame)
    {
        auto& test = g_VmdCameraTest;
        if (frame == 1)
        {
            test.Camera = Camera::CreateGameCamera("VMD validation camera", {5, 4, -10}, {0, 3, 0}, XM_PIDIV4, true);
            test.OtherCamera = Camera::CreateGameCamera("VMD validation secondary", {5, 4, -10}, {0, 3, 0}, XM_PIDIV4, false);
            test.EditorCamera = Camera::GetEditorCameraEntity();
            test.EditorPosition = ComponentManager::GetComponentUnchecked<TransformComponent>(test.EditorCamera).Position;
            for (EntityID entity : {test.Camera, test.OtherCamera})
            {
                auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(entity);
                camera.Animation.Path = VmdCameraTestPath();
                camera.Animation.Scale = 0.15f;
                camera.Animation.Offset = {1, 0, 0};
            }
            test.OriginalCamera = ComponentManager::GetComponentUnchecked<CameraComponent>(test.Camera);
            test.OriginalTransform = ComponentManager::GetComponentUnchecked<TransformComponent>(test.Camera);
            ProjectManager::Play();
            const auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(test.Camera);
            const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(test.Camera);
            Check(Camera::GetCameraEntity() == test.Camera && camera.Animation.IsActive,
                  "VMD Play starts the selected main GameCamera");
            Check(!ComponentManager::GetComponentUnchecked<CameraComponent>(test.OtherCamera).Animation.IsActive,
                  "VMD Play leaves the secondary GameCamera idle");
            Check(SameCameraPosition(transform.Position, {1.0f, 0.3f, -1.05f}),
                  "VMD initial camera pose applies scale and world offset");
        }
        if (frame == 12)
        {
            test.PausedTime = ComponentManager::GetComponentUnchecked<CameraComponent>(test.Camera).Animation.CurrentTime;
            test.PausedPosition = ComponentManager::GetComponentUnchecked<TransformComponent>(test.Camera).Position;
            Check(test.PausedTime > 0.0f, "VMD camera advances in AnimationSystem");
            ProjectManager::TogglePause();
        }
        if (frame == 16)
        {
            ProjectManager::TogglePause();
        }
        if (frame == 20)
        {
            Check(ComponentManager::GetComponentUnchecked<CameraComponent>(test.Camera).Animation.CurrentTime > test.PausedTime,
                  "VMD camera resumes after project pause");
            ProjectManager::Stop();
            const auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(test.Camera);
            const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(test.Camera);
            Check(!camera.Animation.IsActive && SameCameraPosition(transform.Position, test.OriginalTransform.Position) &&
                      SameCameraPosition(camera.Target, test.OriginalCamera.Target) &&
                      SameCameraPosition(camera.Up, test.OriginalCamera.Up) &&
                      NearCameraValue(camera.Fov, test.OriginalCamera.Fov) &&
                      camera.IsOrthographic == test.OriginalCamera.IsOrthographic &&
                      NearCameraValue(camera.OrthographicHeight, test.OriginalCamera.OrthographicHeight),
                  "VMD Stop restores the complete editor camera pose");
            Check(SameCameraPosition(ComponentManager::GetComponentUnchecked<TransformComponent>(test.EditorCamera).Position,
                                     test.EditorPosition), "VMD playback preserves EditorCamera");
        }
        if (frame == 22)
        {
            Camera::SetMainGameCamera(test.OtherCamera);
            ProjectManager::Play();
            Check(ComponentManager::GetComponentUnchecked<CameraComponent>(test.OtherCamera).Animation.IsActive &&
                      !ComponentManager::GetComponentUnchecked<CameraComponent>(test.Camera).Animation.IsActive,
                  "VMD replay targets the main GameCamera selected for that play session");
        }
        if (frame == 25)
        {
            ProjectManager::Stop();
            test.Completed = true;
        }
    }

    void AfterVmdCameraTestFrame(uint64_t frame)
    {
        auto& test = g_VmdCameraTest;
        if (frame >= 1 && frame < 12)
        {
            const auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(test.Camera);
            const auto projection = XMLoadFloat4x4(&camera.ProjectionMatrix);
            const auto close = XMVector4Transform(XMVectorSet(0.25f, 0, 1, 1), projection);
            const auto distant = XMVector4Transform(XMVectorSet(0.25f, 0, 20, 1), projection);
            const float closeX = XMVectorGetX(close) / XMVectorGetW(close);
            const float distantX = XMVectorGetX(distant) / XMVectorGetW(distant);
            if (camera.IsOrthographic && !test.SawOrthographic)
            {
                Check(NearCameraValue(camera.ProjectionMatrix._44, 1.0f) && NearCameraValue(closeX, distantX),
                      "VMD orthographic render preserves size across camera depth");
                Check(RenderCapture::SaveScenePng(L"bin/validation/vmd-camera-orthographic.png"),
                      "capture VMD orthographic camera output");
                test.SawOrthographic = true;
            }
            if (!camera.IsOrthographic && !test.SawPerspective)
            {
                Check(NearCameraValue(camera.ProjectionMatrix._44, 0.0f) && fabsf(closeX - distantX) > 0.01f,
                      "VMD perspective render retains perspective division");
                test.SawPerspective = true;
            }
        }
        if (frame == 15)
        {
            const auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(test.Camera);
            const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(test.Camera);
            Check(NearCameraValue(camera.Animation.CurrentTime, test.PausedTime) &&
                      SameCameraPosition(transform.Position, test.PausedPosition),
                  "VMD project pause holds camera time and pose");
        }
    }

    bool SunLightingTestEnabled()
    {
        char value[8]{};
        return GetEnvironmentVariableA("DX12_SUN_LIGHTING_TEST", value, sizeof(value)) && atoi(value) != 0;
    }

    // Stage 2 H: one-shot reference path-tracer capture
    // (DX12_REFERENCE_CAPTURE_TEST=1). Writes reference_cornell.pfm /
    // .ppm next to the executable; size and spp can be overridden with
    // DX12_REFERENCE_CAPTURE_SIZE / DX12_REFERENCE_CAPTURE_SPP.
    bool ReferenceCaptureTestEnabled()
    {
        char value[8]{};
        return GetEnvironmentVariableA("DX12_REFERENCE_CAPTURE_TEST", value, sizeof(value)) && atoi(value) != 0;
    }

    bool g_ReferenceCaptureDone = false;

    void RunReferenceCaptureOnce()
    {
        if (g_ReferenceCaptureDone)
        {
            return;
        }
        g_ReferenceCaptureDone = true;
        ReferenceCapture::Settings settings{};
        settings.Width = 256;
        settings.Height = 256;
        settings.SamplesPerPixel = 256;
        settings.SamplesPerDispatch = 16;
        char value[16]{};
        if (GetEnvironmentVariableA("DX12_REFERENCE_CAPTURE_SIZE", value, sizeof(value)) && atoi(value) > 0)
        {
            settings.Width = settings.Height = static_cast<uint32_t>(atoi(value));
        }
        if (GetEnvironmentVariableA("DX12_REFERENCE_CAPTURE_SPP", value, sizeof(value)) && atoi(value) > 0)
        {
            settings.SamplesPerPixel = static_cast<uint32_t>(atoi(value));
        }
        char fixture[32]{};
        GetEnvironmentVariableA("DX12_REFERENCE_CAPTURE_FIXTURE", fixture, sizeof(fixture));
        const bool materials = strcmp(fixture, "materials") == 0;
        const ReferenceFixture::SceneData scene =
            materials ? ReferenceFixture::BuildMaterialTest() : ReferenceFixture::BuildCornellBox();
        const bool ok = ReferenceIntegrator::RunFixtureCaptureToFiles(
            GraphicsDevice::GetDevice(), GraphicsDevice::GetCommandQueue(), settings, scene,
            materials ? "reference_materials.pfm" : "reference_cornell.pfm",
            materials ? "reference_materials.ppm" : "reference_cornell.ppm");
        WriteGraphicsLog("Reference capture (Stage 2H, %s): %s (%ux%u, %u spp)\n", materials ? "materials" : "cornell",
                         ok ? "PASS" : "FAIL", settings.Width, settings.Height, settings.SamplesPerPixel);
        Check(ok, "reference capture produced PFM and PPM outputs");
    }

    EntityID g_TestSun = g_kINVALID_ENTITY;
    array<double, 5> g_SunRadiance{};
    array<array<double, 3>, 5> g_SunColors{};

    void BeforeSunLightingFrame(uint64_t frame)
    {
        g_FrameCount = frame + 1;
        const uint64_t phase = frame <= 10 ? frame : (frame <= 20 ? frame - 10 : 10);
        if (frame == 11)
        {
            auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(g_TestSun);
            transform.Position = {0, 1.568f, -10}; // 報告された朝の状態に合わせ、太陽高度を 8.9 度にする。
            transform.IsDirty = true;
            ComponentManager::GetComponentUnchecked<LightComponent>(g_TestSun).Color = {1, 0.35f, 0.08f, 1};
        }
        if (frame == 0)
        {
            vector<EntityID> removed;
            for (EntityID id : World::GetView<TransformComponent>())
            {
                if (ComponentManager::HasComponent<MeshComponent>(id) &&
                    ComponentManager::GetComponentUnchecked<MeshComponent>(id).IsEnvironment)
                {
                    Entity(id).SetName("Renamed Environment Regression Sky");
                    continue; // 環境の識別は編集可能な名前に依存させない。
                }
                if (!ComponentManager::HasComponent<CameraComponent>(id))
                {
                    removed.push_back(id);
                }
            }
            for (auto id : removed)
            {
                World::DestroyEntity(Entity(id));
            }
            Entity environment = World::CreateEntity()
                                     .Add<NameComponent>()
                                     .Add<MeshComponent>()
                                     .Add<StaticModelComponent>()
                                     .Add<TransformComponent>()
                                     .Add<MaterialComponent>();
            environment.SetName("Renamed Environment Regression Sky");
            environment.Get<MeshComponent>().IsEnvironment = true;
            environment.Get<TransformComponent>().Scale = {50, 50, 50};
            environment.Get<TransformComponent>().IsDirty = true;
            environment.Get<StaticModelComponent>().ModelId = ModelManager::LoadStaticModel("asset/model/sky/sky.obj");
            environment.Get<MaterialComponent>().ShaderClassMode = MaterialMode::Manual;
            environment.Get<MaterialComponent>().ShaderClass = ShaderClass::Unlit;
            g_TestSun = Sun::Create({0, 8, -8}, {0, 0, 0});
            Entity cube = World::CreateEntity()
                              .Add<NameComponent>()
                              .Add<MeshComponent>()
                              .Add<TransformComponent>()
                              .Add<MaterialComponent>();
            cube.SetName("Sun Lighting Regression Cube");
            auto& material = cube.Get<MaterialComponent>();
            material.ShaderClassMode = MaterialMode::Manual;
            material.ShaderClass = ShaderClass::PBR;
            material.Metallic = 0;
            material.Roughness = 0.8f;
            VertexResource resource;
            resource.entityid = cube.GetID();
            resource.objectType = ObjectType::CUBE;
            resource.color = Color::WHITE;
            PrimitiveGeometry::CreateObjectVertex(resource);
            auto id = Camera::GetCameraEntity();
            auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(id);
            transform.Position = {0, 3, -8};
            transform.IsDirty = true;
            auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(id);
            camera.Target = {0, 0, 0};
            camera.AllowUserControl = false;
            camera.LockOnTarget = g_kINVALID_ENTITY;
        }
        RenderConfiguration::SetHdr(true);
        RenderConfiguration::SetResolutionScale(1.0f);
        DayNightCycle::GetSettings().Running = false;
        DayNightCycle::GetSettings().TimeDriven = false;
        DayNightCycle::GetSettings().AutomaticLighting = false;
        RenderConfiguration::SetRenderMode(RenderMode::DEFERRED);
        RendererSettings::SetMeshShadersEnabled(false);
        RendererSettings::SetComputeGBufferEnabled(false);
        RendererSettings::SetSsaoEnabled(false);
        RendererSettings::SetSsgiEnabled(false);
        RendererSettings::SetUpscaleMode(UpscaleMode::Bilateral);
        auto& rays = Raytracing::GetSettings();
        rays.Enabled = true;
        rays.Reflections = true;
        rays.Denoise = false;
        rays.Shadows = true;
        rays.RenderMode = phase >= 7 && phase <= 8 ? Raytracing::Mode::Full : Raytracing::Mode::ReflectionsOnly;
        RendererSettings::SetShadowMapMethod(phase >= 5 && phase <= 6 ? ShadowMapMethod::VirtualShadowMap
                                                                      : ShadowMapMethod::ShadowMap);
        auto& sun = ComponentManager::GetComponentUnchecked<LightComponent>(g_TestSun);
        sun.IsActive = phase < 9;
        sun.CastShadow = phase >= 3;
        sun.AffectsVolumetrics = false;
    }

    void AfterSunLightingFrame(uint64_t frame)
    {
        if (frame < 2 || frame > 20 || frame % 2)
        {
            return;
        }
        const uint64_t phase = frame <= 10 ? frame : frame - 10;
        Check(phase == 8 ? Raytracing::UsesRaytracedShadows() : !Raytracing::UsesRaytracedShadows(),
              "sun regression uses the requested raster or raytraced shadow path");
        if (phase == 4 || phase == 6)
        {
            for (EntityID id : World::GetView<MeshComponent>())
            {
                if (ComponentManager::GetComponentUnchecked<MeshComponent>(id).IsEnvironment)
                {
                    Check(!LightingResources::ShouldDrawEntityInCurrentShadowPass(id),
                          "environment mesh is excluded from shadow culling and bounds");
                }
            }
        }
        RenderCapture::TextureData image, materials;
        bool read = RenderCapture::ReadTexture(RenderTargets::GetSceneResource(), image) &&
                    RenderCapture::ReadTexture(RenderTargets::GetGBufferResource(GBufferType::MATERIAL), materials);
        const bool compatible = read && image.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
                                materials.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && image.Width == materials.Width &&
                                image.Height == materials.Height;
        Check(compatible, "read sun regression HDR and PBR mask");
        if (!compatible)
        {
            return;
        }
        double sum = 0;
        array<double, 3> channels{};
        uint32_t pixels = 0;
        for (uint32_t y = 0; y < image.Height; ++y)
        {
            auto* color = reinterpret_cast<const uint16_t*>(image.Bytes.data() + y * image.RowPitch);
            auto* mask = reinterpret_cast<const uint16_t*>(materials.Bytes.data() + y * materials.RowPitch);
            for (uint32_t x = 0; x < image.Width; ++x)
            {
                if (abs(RenderCapture::HalfToFloat(mask[x * 4 + 3]) - 11.f) < 0.1f)
                {
                    sum += 0.2126 * RenderCapture::HalfToFloat(color[x * 4]) +
                           0.7152 * RenderCapture::HalfToFloat(color[x * 4 + 1]) +
                           0.0722 * RenderCapture::HalfToFloat(color[x * 4 + 2]);
                    for (uint32_t c = 0; c < 3; ++c)
                    {
                        channels[c] += RenderCapture::HalfToFloat(color[x * 4 + c]);
                    }
                    ++pixels;
                }
            }
        }
        Check(pixels > 100, "sun regression includes visible PBR pixels");
        g_SunRadiance[phase / 2 - 1] = pixels ? sum / pixels : 0;
        WriteGraphicsLog("Sun lighting regression frame=%llu radiance=%.6f pixels=%u\n", frame,
                         g_SunRadiance[phase / 2 - 1], pixels);
        for (uint32_t c = 0; c < 3; ++c)
        {
            g_SunColors[phase / 2 - 1][c] = pixels ? channels[c] / pixels : 0;
        }
        if (phase == 10)
        {
            double direct = g_SunRadiance[0] - g_SunRadiance[4];
            Check(direct > 0.05, "virtual sun lights unshadowed PBR surface");
            Check(g_SunRadiance[1] - g_SunRadiance[4] > direct * 0.5,
                  "reflection-only ShadowMap preserves sun on unoccluded PBR surface");
            Check(g_SunRadiance[2] - g_SunRadiance[4] > direct * 0.5,
                  "reflection-only Virtual ShadowMap preserves sun on unoccluded PBR surface");
            Check(g_SunRadiance[3] - g_SunRadiance[4] > direct * 0.5,
                  "full raytracing preserves sun on unoccluded PBR surface");
            if (frame == 20)
            {
                for (uint32_t path = 1; path <= 3; ++path)
                {
                    double red = g_SunColors[path][0] - g_SunColors[4][0];
                    double green = g_SunColors[path][1] - g_SunColors[4][1];
                    double blue = g_SunColors[path][2] - g_SunColors[4][2];
                    WriteGraphicsLog("Sun morning direct path=%u RGB=%.6f,%.6f,%.6f\n", path, red, green, blue);
                    Check(red > green * 1.4 && green > blue * 1.3,
                          "morning sun color reaches PBR in raster and raytraced shadow paths");
                }
            }
        }
    }

    void ValidateReflectionDenoising(const RenderCapture::TextureData& filtered)
    {
        if (!NrdDenoiser::IsCompiled())
        {
            return;
        }
        RenderCapture::TextureData noisy;
        const bool read = RenderCapture::ReadTexture(RayReflections::GetNoisyResource(), noisy);
        Check(read && noisy.Width == filtered.Width && noisy.Height == filtered.Height,
              "read matching raw NRD reflection input");
        if (!read || noisy.Width != filtered.Width || noisy.Height != filtered.Height)
        {
            return;
        }

        // NRD 入力の Y チャンネルは 0.25 R + 0.5 G + 0.25 B。同じ輝度を
        // 合成後の RGB 出力でも比較する。隣接差分で高周波の変化を測る。
        // 背景とヒット境界を除外し、輪郭が測定結果を支配しないようにする。
        // パック入力は両軸の追跡済み近傍を比較する。チェッカーボードの間隔は 2 ピクセル。
        double rawVariation = 0;
        double filteredVariation = 0;
        UINT pairs = 0;
        const auto layout = RayReflections::GetReflectionSamplingLayout();
        const UINT storageWidth = RenderMeasurement::TraceStorageWidth(filtered.Width, layout.Checkerboard);
        const UINT pixelStep = layout.Checkerboard ? 2u : 1u;
        for (UINT y = 0; y < filtered.Height; ++y)
        {
            const auto* raw = reinterpret_cast<const uint16_t*>(noisy.Bytes.data() + y * noisy.RowPitch);
            const auto* output = reinterpret_cast<const uint16_t*>(filtered.Bytes.data() + y * filtered.RowPitch);
            for (UINT storageX = 0; storageX < storageWidth; ++storageX)
            {
                const UINT x = RenderMeasurement::TracePixelX(storageX, y, layout.Sequence, layout.Checkerboard);
                if (x >= filtered.Width)
                {
                    continue;
                }
                const UINT current = x * 4;
                const UINT rawCurrent = storageX * 4;
                const auto accumulatePair = [&](const uint16_t* otherRaw, const uint16_t* otherOutput,
                                                UINT otherRawIndex, UINT otherOutputIndex) {
                    const float distance = RenderCapture::HalfToFloat(output[current + 3]);
                    const float otherDistance = RenderCapture::HalfToFloat(otherOutput[otherOutputIndex + 3]);
                    if (abs(distance) < 0.01f || abs(otherDistance) < 0.01f)
                    {
                        return;
                    }
                    const auto luminance = [](const uint16_t* color) {
                        return 0.25f * RenderCapture::HalfToFloat(color[0]) +
                               0.5f * RenderCapture::HalfToFloat(color[1]) +
                               0.25f * RenderCapture::HalfToFloat(color[2]);
                    };
                    rawVariation += abs(RenderCapture::HalfToFloat(raw[rawCurrent]) -
                                        RenderCapture::HalfToFloat(otherRaw[otherRawIndex]));
                    filteredVariation += abs(luminance(output + current) - luminance(otherOutput + otherOutputIndex));
                    ++pairs;
                };
                if (storageX > 0)
                {
                    accumulatePair(raw, output, rawCurrent - 4, (x - pixelStep) * 4);
                }
                if (y >= pixelStep)
                {
                    const auto* previousRaw =
                        reinterpret_cast<const uint16_t*>(noisy.Bytes.data() + (y - pixelStep) * noisy.RowPitch);
                    const auto* previousOutput =
                        reinterpret_cast<const uint16_t*>(filtered.Bytes.data() + (y - pixelStep) * filtered.RowPitch);
                    accumulatePair(previousRaw, previousOutput, rawCurrent, current);
                }
            }
        }
        WriteGraphicsLog("Validation NRD reflection: neighbor pairs=%u rawVariation=%.6f filteredVariation=%.6f\n",
                         pairs, rawVariation / max(pairs, 1u), filteredVariation / max(pairs, 1u));
        Check(pairs > 100 && rawVariation > 0 && filteredVariation < rawVariation,
              "NRD reduces reflection high-frequency variation in the test scene");
    }

    void ValidateProfilerIntervals()
    {
        const auto& snapshot = RenderProfiler::GetLatest();
        Check(snapshot.GpuTimingValid && !snapshot.GpuEvents.empty(), "profiler resolves GPU frame intervals");
        bool hierarchyValid = snapshot.DroppedGpuEvents == 0;
        bool selfTimeValid = true;
        for (const auto& event : snapshot.GpuEvents)
        {
            if (event.Interval.ParentId != RenderMeasurement::NoParent)
            {
                const auto parent =
                    find_if(snapshot.GpuEvents.begin(), snapshot.GpuEvents.end(), [&](const auto& candidate) {
                        return candidate.Interval.Id == event.Interval.ParentId;
                    });
                hierarchyValid &= parent != snapshot.GpuEvents.end() && parent->Depth + 1 == event.Depth;
            }
            selfTimeValid &= event.InclusiveMs.has_value() && event.ExclusiveMs.has_value();
            if (event.InclusiveMs && event.ExclusiveMs)
            {
                selfTimeValid &= *event.ExclusiveMs >= 0 && *event.ExclusiveMs <= *event.InclusiveMs + 1e-6;
            }
        }
        Check(hierarchyValid, "profiler records valid parent IDs without dropped GPU events");
        Check(selfTimeValid, "profiler self times exclude children without exceeding inclusive time");
    }

    void ValidateProfilerFaults()
    {
        const auto& snapshot = RenderProfiler::GetLatest();
        if (snapshot.InjectedFault == RenderProfiler::ValidationFault::None || snapshot.Serial == g_ProfilerFaultSerial)
        {
            return;
        }
        g_ProfilerFaultSerial = snapshot.Serial;
        g_ProfilerFaultMask |= 1u << (static_cast<uint32_t>(snapshot.InjectedFault) - 1u);
        Check(snapshot.GpuTimingValid, "profiler fault preserves independent Frame GPU timing");
        if (snapshot.InjectedFault == RenderProfiler::ValidationFault::ExhaustQueries)
        {
            Check(snapshot.DroppedGpuEvents > 0, "profiler reports timestamp exhaustion");
            const bool noInventedSelf =
                all_of(snapshot.GpuEvents.begin(), snapshot.GpuEvents.end(), [](const auto& event) {
                    return !event.ExclusiveMs;
                });
            Check(noInventedSelf, "timestamp exhaustion makes exclusive measurements unavailable");
        }
        else
        {
            const auto invalid = count_if(snapshot.GpuEvents.begin(), snapshot.GpuEvents.end(), [](const auto& event) {
                return !event.Interval.Valid;
            });
            Check(invalid >= 1, "profiler marks leaked or out-of-order scopes invalid");
            const auto frame = find_if(snapshot.GpuEvents.begin(), snapshot.GpuEvents.end(), [](const auto& event) {
                return event.Name == "Frame";
            });
            Check(frame != snapshot.GpuEvents.end() && !frame->ExclusiveMs,
                  "invalid direct child prevents an invented Frame self time");
        }
    }

    void ValidateRayTelemetry(bool expectReflectionRays)
    {
        if (!RayTelemetry::Requested())
        {
            return;
        }
        const bool idle = GraphicsDevice::WaitForGpuIdle();
        Check(idle, "ray telemetry readback fence completes");
        if (!idle)
        {
            return;
        }
        const auto counts = RayTelemetry::Data().ConsumeCurrent(RenderProfiler::GetFrameSerial());
        Check(counts.Valid, "ray telemetry readback has the current frame serial");
        Check(counts.ProviderKnown(RayTelemetry::Radiance) && counts.ProviderKnown(RayTelemetry::ShadowAo),
              "active radiance and shadow/AO shaders provide measured counters");
        if (expectReflectionRays && Raytracing::AreReflectionsActive())
        {
            Check(counts.Rays[RAY_TELEMETRY_REFLECTION] > 0, "reflection primary rays are counted at TraceRayInline");
        }
        if (Raytracing::UsesRaytracedAmbientOcclusion())
        {
            Check(counts.Rays[RAY_TELEMETRY_AO] > 0, "AO rays are counted by the actual ray-tracing shader");
        }
        if (!Raytracing::UsesRaytracedShadows() && !Raytracing::UsesRaytracedAmbientOcclusion())
        {
            Check(counts.Rays[RAY_TELEMETRY_SHADOW] == 0 && counts.Rays[RAY_TELEMETRY_AO] == 0,
                  "guides-only dispatch counts zero shadow/AO rays");
        }
        if (!Raytracing::IsAvailable())
        {
            Check(counts.Rays[RAY_TELEMETRY_REFLECTION] == 0 && counts.Rays[RAY_TELEMETRY_GI] == 0,
                  "DXR-disabled fallback measures zero primary radiance rays");
        }
        Check(!counts.AllSourcesCovered, "partial DXR telemetry does not claim a total");
        WriteGraphicsLog(
            "Ray telemetry serial=%llu reflection=%llu gi=%llu visibility=%llu restir=%llu shadow=%llu ao=%llu active=%u measured=%u\n",
            counts.Serial, counts.Rays[0], counts.Rays[1], counts.Rays[2], counts.Rays[3], counts.Rays[4],
            counts.Rays[5], counts.ActiveProviders, counts.MeasuredProviders);
    }

    void ValidateVertexDeformation()
    {
        VertexDeformationView view;
        const bool resolved = VertexDeformation::TryResolveMesh(g_Mover, view);
        Check(resolved && VertexDeformation::GetDispatchCount() > 0, "GPU procedural vertex dispatch");
        if (!resolved)
        {
            return;
        }
        const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(g_Mover);
        const auto& motion = ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(g_Mover);
        vector<uint8_t> current, previous;
        const bool read =
            RenderCapture::ReadBuffer(view.CurrentResource, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                                      view.CurrentView.SizeInBytes, current) &&
            RenderCapture::ReadBuffer(view.PreviousResource, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                                      view.PreviousView.SizeInBytes, previous);
        Check(read, "read current and previous GPU vertices");
        if (!read)
        {
            return;
        }
        void* mapped = nullptr;
        const CD3DX12_RANGE noRead(0, 0);
        if (FAILED(mesh.VertexBuffer->Map(0, &noRead, &mapped)))
        {
            Check(false, "map immutable procedural base vertices");
            return;
        }
        Procedural::VertexMotionSettings settings;
        settings.Method = motion.Method;
        settings.Amplitude = {motion.Amplitude.x, motion.Amplitude.y, motion.Amplitude.z};
        settings.Speed = motion.Speed;
        settings.SpatialFrequency = motion.SpatialFrequency;
        settings.Seed = motion.Seed;
        bool positions = true, normals = true, attributes = true, moved = false;
        const auto close = [](float a, float b, float tolerance) {
            return isfinite(a) && abs(a - b) < tolerance;
        };
        for (UINT index = 0; index < mesh.VertexCount; ++index)
        {
            const auto& base = static_cast<const Vertex*>(mapped)[index];
            const auto& now = reinterpret_cast<const Vertex*>(current.data())[index];
            const auto& old = reinterpret_cast<const Vertex*>(previous.data())[index];
            const Procedural::Vector3 basePosition{base.Pos.x, base.Pos.y, base.Pos.z};
            const auto expected = Procedural::DeformPosition(basePosition, settings, motion.Time);
            const auto expectedPrevious = Procedural::DeformPosition(basePosition, settings, g_PreviousVertexTime);
            const auto normal = Procedural::DeformNormal(basePosition, {base.Normal.x, base.Normal.y, base.Normal.z},
                                                         settings, motion.Time);
            positions &= close(now.Pos.x, expected.X, 0.002f) && close(now.Pos.y, expected.Y, 0.002f) &&
                         close(now.Pos.z, expected.Z, 0.002f) && close(old.Pos.x, expectedPrevious.X, 0.002f) &&
                         close(old.Pos.y, expectedPrevious.Y, 0.002f) && close(old.Pos.z, expectedPrevious.Z, 0.002f);
            normals &= close(now.Normal.x, normal.X, 0.02f) && close(now.Normal.y, normal.Y, 0.02f) &&
                       close(now.Normal.z, normal.Z, 0.02f);
            attributes &= memcmp(&now.Tex, &base.Tex, sizeof(base.Tex)) == 0 &&
                          memcmp(&now.Color, &base.Color, sizeof(base.Color)) == 0;
            moved |= abs(now.Pos.y - base.Pos.y) > 0.001f;
        }
        const CD3DX12_RANGE noWrite(0, 0);
        mesh.VertexBuffer->Unmap(0, &noWrite);
        Check(positions && moved, "GPU positions and motion history agree with CPU reference");
        Check(normals && attributes, "GPU deformed normals and unchanged UV/color");
    }

    void Check(bool passed, const char* name)
    {
        if (!passed)
        {
            ++g_Failures;
        }
        const char* result = "FAIL";
        if (passed)
        {
            result = "PASS";
        }
        WriteGraphicsLog("Validation: %s %s\n", result, name);
    }

    void CreateTestScene()
    {
        // 別の .cpp ファイルからの登録も含め、実際の起動シーンを検証する。
        const char* startupNames[] = {"Cube", "polygon3d", "XBot",           "field",
                                      "Moca", "Karen",     "Kacchatta_Hone", "Alicia"};
        for (const char* name : startupNames)
        {
            unsigned int count = 0;
            for (EntityID id : World::GetView<NameComponent>())
            {
                if (ComponentManager::GetComponent<NameComponent>(id).Name == name)
                {
                    ++count;
                }
            }
            const string checkName = string("auto-registered startup entity created once: ") + name;
            Check(count == 1, checkName.c_str());
        }

        bool startupSky = false;
        for (EntityID id : World::GetView<MeshComponent>())
        {
            startupSky |= ComponentManager::GetComponentUnchecked<MeshComponent>(id).IsEnvironment;
        }
        Check(!startupSky, "physical atmosphere startup does not create a legacy Sky object");

        RendererSettings::SetMeshShadersEnabled(true);
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 meshOptions{};
        if (SUCCEEDED(GraphicsDevice::GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &meshOptions,
                                                                       sizeof(meshOptions))) &&
            meshOptions.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED)
        {
            Check(MeshShaderPipeline::IsAvailable(), "mesh shader pipelines initialize on supported hardware");
        }
        const auto suns = World::GetView<SunComponent>();
        if (suns.begin() == suns.end())
        {
            Check(Sun::CreateDefault() != g_kINVALID_ENTITY, "create simulation Sun");
        }
        for (EntityID sun : World::GetView<SunComponent>())
        {
            Check(!ComponentManager::HasComponent<MeshComponent>(sun) &&
                      !ComponentManager::HasComponent<MaterialComponent>(sun),
                  "virtual Sun has no rendered mesh or material");
        }
        Procedural::TerrainSettings terrain;
        terrain.Resolution = 24;
        g_Terrain = ProceduralScene::GenerateTerrain(terrain);
        Check(g_Terrain != g_kINVALID_ENTITY, "generate terrain");
        Entity mover = World::CreateEntity()
                           .Add<NameComponent>()
                           .Add<MeshComponent>()
                           .Add<TransformComponent>()
                           .Add<MaterialComponent>()
                           .Add<ProceduralMotionComponent>();
        mover.SetName("Procedural Validation Cube");
        g_Mover = mover.GetID();
        auto& transform = mover.Get<TransformComponent>();
        transform.Position = {0.0f, 4.0f, 0.0f};
        mover.Get<MaterialComponent>().ShaderClassMode = MaterialMode::Manual;
        mover.Get<MaterialComponent>().ShaderClass = ShaderClass::PBR;
        mover.Get<ProceduralMotionComponent>().Origin = transform.Position;
        mover.Get<ProceduralMotionComponent>().Amplitude = {1.0f, 0.75f, 1.0f};
        VertexResource resource;
        resource.entityid = mover.GetID();
        resource.objectType = ObjectType::CUBE;
        resource.color = Color::WHITE;
        PrimitiveGeometry::CreateObjectVertex(resource);
        Check(ProceduralScene::SavePreset("bin/validation/terrain.proc", terrain), "save terrain preset");
        Procedural::TerrainSettings restored;
        Check(ProceduralScene::LoadPreset("bin/validation/terrain.proc", restored) &&
                  Procedural::BuildHeightMap(restored) == Procedural::BuildHeightMap(terrain),
              "load reproducible terrain preset");
        const auto before = restored;
        ofstream invalid("bin/validation/invalid.proc");
        invalid << "PROCEDURAL_TERRAIN 1\n99 0 nan 100 0 0 0\n-1 0 0 0\n";
        invalid.close();
        Check(!ProceduralScene::LoadPreset("bin/validation/invalid.proc", restored) &&
                  restored.Noise.Seed == before.Noise.Seed,
              "reject invalid preset without mutation");
        ostringstream configuration;
        SimulationConfiguration::Save(configuration);
        istringstream records(configuration.str());
        string line;
        while (getline(records, line))
        {
            const size_t delimiter = line.find('=');
            Check(SimulationConfiguration::TryLoad(line.substr(0, delimiter), line.substr(delimiter + 1)),
                  "simulation configuration roundtrip");
        }
        for (auto mode : {Raytracing::Mode::ReflectionsOnly, Raytracing::Mode::Full})
        {
            Raytracing::GetSettings().RenderMode = mode;
            ostringstream saved;
            SimulationConfiguration::Save(saved);
            Raytracing::GetSettings().RenderMode =
                mode == Raytracing::Mode::Full ? Raytracing::Mode::ReflectionsOnly : Raytracing::Mode::Full;
            istringstream reload(saved.str());
            while (getline(reload, line))
            {
                const size_t delimiter = line.find('=');
                SimulationConfiguration::TryLoad(line.substr(0, delimiter), line.substr(delimiter + 1));
            }
            Check(Raytracing::GetSettings().RenderMode == mode, "raytracing mode survives save/load");
        }
    }

    void ValidateRayTexture()
    {
        RenderCapture::TextureData data;
        if (!Raytracing::IsActive())
        {
            return;
        }
        Check(RenderCapture::ReadTexture(Raytracing::GetResultResource(), data), "read raytracing texture");
        UINT shadowed = 0;
        UINT occluded = 0;
        bool finite = true;
        for (UINT row = 0; row < data.Height; ++row)
        {
            const auto* values =
                reinterpret_cast<const uint16_t*>(data.Bytes.data() + static_cast<size_t>(row) * data.RowPitch);
            for (UINT column = 0; column < data.Width; ++column)
            {
                const float shadow = RenderCapture::HalfToFloat(values[column * 2]);
                const float ambient = RenderCapture::HalfToFloat(values[column * 2 + 1]);
                finite &= isfinite(shadow) && isfinite(ambient) && shadow >= 0.0f && shadow <= 1.0f &&
                          ambient >= 0.0f && ambient <= 1.0f;
                shadowed += shadow < 0.99f;
                occluded += ambient < 0.99f;
            }
        }
        Check(finite, "finite bounded ray visibility");
        Check(shadowed > 0 && occluded > 0, "rays intersect scene geometry");
        WriteGraphicsLog("Validation: ray hits shadow=%u ao=%u instances=%u\n", shadowed, occluded,
                         Raytracing::GetInstanceCount());
    }

    void ValidateSkyBrightness(bool night)
    {
        RenderCapture::TextureData texture;
        if (!RenderCapture::ReadTexture(RenderTargets::GetEditorSceneResource(), texture))
        {
            Check(false, "read atmospheric sky");
            return;
        }
        double total = 0;
        UINT samples = 0;
        for (UINT row = texture.Height / 32; row < texture.Height / 6; row += 4)
        {
            for (UINT column = 0; column < texture.Width; column += 4)
            {
                const BYTE* pixel = texture.Bytes.data() + static_cast<size_t>(row) * texture.RowPitch;
                float red;
                float green;
                float blue;
                if (texture.Format == DXGI_FORMAT_R8G8B8A8_UNORM)
                {
                    pixel += column * 4;
                    red = pixel[0] / 255.0f;
                    green = pixel[1] / 255.0f;
                    blue = pixel[2] / 255.0f;
                }
                else
                {
                    const auto* color = reinterpret_cast<const uint16_t*>(pixel) + column * 4;
                    red = RenderCapture::HalfToFloat(color[0]);
                    green = RenderCapture::HalfToFloat(color[1]);
                    blue = RenderCapture::HalfToFloat(color[2]);
                }
                total += red * 0.2126f + green * 0.7152f + blue * 0.0722f;
                ++samples;
            }
        }
        const float luminance = samples ? static_cast<float>(total / samples) : 0.0f;
        Check(isfinite(luminance) && luminance > 0, "finite visible atmospheric sky");
        if (night)
        {
            Check(g_NoonSkyLuminance > luminance * 1.5f, "night sky is darker than noon");
        }
        else
        {
            g_NoonSkyLuminance = luminance;
        }
        WriteGraphicsLog("Validation: %s sky luminance %.4f\n", night ? "night" : "noon", luminance);
    }
}

bool RenderingValidation::IsEnabled()
{
    char value[8]{};
    return GetEnvironmentVariableA("DX12_FEATURE_SMOKE_TEST", value, sizeof(value)) > 0 && atoi(value) != 0;
}

void RenderingValidation::BeforeFrame(uint64_t frame)
{
    if (!VmdCameraTestPath().empty())
    {
        BeforeVmdCameraTestFrame(frame);
        return;
    }
    if (VirtualGeometryProduction::ValidationEnabled())
    {
        VirtualGeometryProduction::BeforeFrame(frame);
        return;
    }
    if (PlayRenderTestEnabled())
    {
        if (ShadowBenchmarkEnabled())
        {
            if (frame == 1)
            {
                World::SetVSyncEnabled(false);
                World::SetFixedFrameRateEnabled(false);
                Check(Raytracing::GetSettings().RenderMode == Raytracing::Mode::ReflectionsOnly &&
                          RendererSettings::GetShadowMapMethod() == ShadowMapMethod::VirtualShadowMap,
                      "VSM benchmark keeps reflections-only raytracing throughout");
            }
            if (frame == 20)
            {
                SetEnvironmentVariableA("DX12_SHADOW_REFERENCE_DRAW", "1");
                ProjectManager::Play();
            }
            if (frame == 80)
            {
                ProjectManager::Stop();
                SetEnvironmentVariableA("DX12_SHADOW_REFERENCE_DRAW", "0");
            }
            if (frame == 100)
            {
                ProjectManager::Play();
            }
            if (frame == 160)
            {
                ProjectManager::Stop();
            }
            return;
        }
        if (frame == 20)
        {
            ProjectManager::Play();
        }
        if (frame == 46)
        {
            ProjectManager::TogglePause();
            g_PlayCacheEnabled = RendererSettings::GetCacheVirtualShadowPages();
            RendererSettings::SetCacheVirtualShadowPages(false);
            SetEnvironmentVariableA("DX12_SHADOW_REFERENCE_DRAW", "1");
        }
        if (frame == 49)
        {
            SetEnvironmentVariableA("DX12_SHADOW_REFERENCE_DRAW", nullptr);
        }
        if (frame == 52)
        {
            RendererSettings::SetCacheVirtualShadowPages(true);
        }
        if (frame >= 52 && frame <= 60)
        {
            // 一時停止中の姿勢を変えず、移動する影の投影元のキーだけを更新する。
            // 同じ参照画像を使って静止深度の再利用を検証する。
            for (auto entity : World::GetView<AnimationModelComponent>())
            {
                ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity).CurrentTime += 0.0001f;
            }
        }
        if (frame == 65)
        {
            ProjectManager::Stop();
            RendererSettings::SetCacheVirtualShadowPages(g_PlayCacheEnabled);
        }
        return;
    }
    if (ThumbnailValidation::Enabled())
    {
        ThumbnailValidation::BeforeFrame(frame);
        return;
    }
    if (MaterialValidation::Enabled())
    {
        MaterialValidation::BeforeFrame(frame);
        return;
    }
    if (CloudValidation::Enabled())
    {
        ImGuiManager::m_HdrEnabled = true;
        ImGuiManager::m_ToneMapEnabled = true;
        if (frame == 0)
        {
            BeforeSunLightingFrame(0);
        }
        CloudValidation::BeforeFrame(frame);
        return;
    }
    if (SunLightingTestEnabled())
    {
        ImGuiManager::m_HdrEnabled = true;
        ImGuiManager::m_ToneMapEnabled = false;
        BeforeSunLightingFrame(frame);
        return;
    }
    if (VirtualGeometryStreamingValidation::Enabled())
    {
        VirtualGeometryStreamingValidation::BeforeFrame(frame);
        return;
    }
    if (VirtualGeometryValidation::Enabled())
    {
        VirtualGeometryValidation::BeforeFrame(frame);
        return;
    }
    if (MeshletValidation::Enabled())
    {
        MeshletValidation::BeforeFrame(frame);
        return;
    }
    if (GPUVisibilityValidation::Enabled())
    {
        GPUVisibilityValidation::BeforeFrame(frame);
        return;
    }
    if (GPUDrivenValidation::Enabled())
    {
        GPUDrivenValidation::BeforeFrame(frame);
        return;
    }
    if (!IsEnabled())
    {
        return;
    }
    if (frame == 41)
    {
        Check(RenderProfiler::BeginGpuEvent("Outside Frame", GraphicsDevice::GetCommandList()) ==
                  RenderProfiler::kInvalidGpuToken,
              "profiler ignores events started outside a recording frame");
        RenderProfiler::SetValidationFault(RenderProfiler::ValidationFault::ExhaustQueries);
    }
    if (frame == 43)
    {
        RenderProfiler::SetValidationFault(RenderProfiler::ValidationFault::UnclosedScope);
    }
    if (frame == 45)
    {
        RenderProfiler::SetValidationFault(RenderProfiler::ValidationFault::OutOfOrderEnd);
    }
    if (frame == 42 || frame == 44 || frame == 46)
    {
        RenderProfiler::SetValidationFault(RenderProfiler::ValidationFault::None);
    }
    if (frame == 0)
    {
        char startup[8]{};
        if (GetEnvironmentVariableA("DX12_VALIDATION_DLSS_STARTUP", startup, sizeof(startup)) > 0 && atoi(startup) != 0)
        {
            RendererSettings::SetUpscaleMode(UpscaleMode::Dlss);
        }
        filesystem::create_directories("bin/validation");
        CreateTestScene();
    }
    g_FrameCount = frame + 1;
    const float hours[] = {6.0f, 12.0f, 18.0f, 0.0f};
    if (frame == 32)
    {
        GPUDrivenRenderer::SetEnabled(g_GpuDrivenBeforeMeshFixture);
        TemporalUpscaler::SetInputTaaEnabled(false);
        RendererSettings::SetUpscaleMode(UpscaleMode::Tsr);
        RenderConfiguration::SetResolutionScale(.2f);
    }
    if (frame == 36)
    {
        TemporalUpscaler::SetInputTaaEnabled(false);
        RendererSettings::SetUpscaleMode(UpscaleMode::Bilateral);
        RenderConfiguration::SetResolutionScale(1.f);
    }
    if (frame == 34)
    {
        TemporalUpscaler::SetInputTaaEnabled(true);
    }
    if (frame >= 33 && frame <= 35)
    {
        g_TsrJitterBeforeDraw = RendererState::m_TaaFrameIndex;
    }
    DayNightCycle::GetSettings().Running = false;
    DayNightCycle::GetSettings().DayOfYear = 81;
    DayNightCycle::SetTime(hours[(frame / 8) % 4]);
    Raytracing::GetSettings().Enabled = frame < 32 || frame >= 36;
    Raytracing::GetSettings().Reflections = (frame < 24 || frame >= 28) && frame != 18;
    Raytracing::GetSettings().Shadows = true;
    Raytracing::GetSettings().RenderMode =
        (frame == 12 || frame == 13) ? Raytracing::Mode::ReflectionsOnly : Raytracing::Mode::Full;
    if (frame == 12)
    {
        RendererSettings::SetShadowMapMethod(ShadowMapMethod::ShadowMap);
    }
    if (frame == 13)
    {
        RendererSettings::SetShadowMapMethod(ShadowMapMethod::VirtualShadowMap);
    }
    if (frame == 8)
    {
        ProjectManager::Play();
    }
    if (frame == 16)
    {
        g_PausedTime = ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(g_Mover).Time;
        ProjectManager::TogglePause();
    }
    if (frame == 18)
    {
        ProjectManager::TogglePause();
    }
    if (frame == 20)
    {
        ProjectManager::Stop();
    }
    if (frame == 24)
    {
        const auto before = ImGuiManager::CaptureEntity(g_Terrain);
        g_OriginalTerrainHash = ComponentManager::GetComponentUnchecked<MeshComponent>(g_Terrain).GeometryHash;
        auto settings = ComponentManager::GetComponentUnchecked<ProceduralTerrainComponent>(g_Terrain).Settings;
        settings.Noise.Method = Procedural::NoiseMethod::DomainWarp;
        settings.Noise.Seed = 123;
        Check(ProceduralScene::GenerateTerrain(settings, g_Terrain) == g_Terrain, "regenerate existing terrain");
        ImGuiManager::PushUndoSnapshot(before);
    }
    if (frame == 21)
    {
        RendererSettings::SetComputeGBufferEnabled(false);
        auto& motion = ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(g_Mover);
        motion.Target = Procedural::MotionTarget::MeshVertices;
        motion.PreviewInEditor = true;
        motion.Speed = 0.4f;
        motion.Time = 0.6;
        VertexDeformation::ResetHistory(g_Mover);
    }
    if (frame >= 22 && frame <= 38)
    {
        g_PreviousVertexTime = ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(g_Mover).Time;
    }
    if (frame == 39)
    {
        ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(g_Mover).Enabled = false;
    }
    if (frame == 30)
    {
        // このテストでは Mesh Shader のページオフセットを検証する。
        // GPU Driven のインデックス描画経路が先に地形を処理しないようにする。
        g_GpuDrivenBeforeMeshFixture = GPUDrivenRenderer::IsEnabled();
        GPUDrivenRenderer::SetEnabled(false);
        auto terrain = ComponentManager::GetComponentUnchecked<ProceduralTerrainComponent>(g_Terrain).Settings;
        terrain.Resolution = 256;
        Check(ProceduralScene::GenerateTerrain(terrain, g_Terrain) == g_Terrain,
              "generate large terrain for mesh shader validation");
        RendererSettings::SetComputeGBufferEnabled(true);
    }
    if (frame == 25)
    {
        ImGuiManager::Undo();
        Check(ComponentManager::GetComponentUnchecked<ProceduralTerrainComponent>(g_Terrain).Settings.Noise.Method ==
                      Procedural::NoiseMethod::Fractal &&
                  ComponentManager::GetComponentUnchecked<MeshComponent>(g_Terrain).GeometryHash ==
                      g_OriginalTerrainHash,
              "Undo restores terrain geometry and settings");
        ImGuiManager::Redo();
        Check(ComponentManager::GetComponentUnchecked<ProceduralTerrainComponent>(g_Terrain).Settings.Noise.Method ==
                  Procedural::NoiseMethod::DomainWarp,
              "Redo restores regenerated terrain");
    }
    if (frame == 28)
    {
        RenderConfiguration::Resize(960, 640);
    }
    if (frame == 40)
    {
        GraphicsDevice::WaitForGpuIdle();
        vector<EntityID> removed;
        for (EntityID entity : World::GetView<MeshComponent>())
        {
            if (!ComponentManager::HasComponent<SunComponent>(entity) &&
                !ComponentManager::HasComponent<CameraComponent>(entity))
            {
                removed.push_back(entity);
            }
        }
        for (EntityID entity : removed)
        {
            World::DestroyEntity(Entity(entity));
        }
    }
    if (frame == 44)
    {
        Procedural::TerrainSettings terrain;
        terrain.Resolution = 16;
        g_Terrain = ProceduralScene::GenerateTerrain(terrain);
        Check(g_Terrain != g_kINVALID_ENTITY, "restore geometry after empty scene");
    }
    if (frame == 48 || frame == 68)
    {
        RendererSettings::SetUpscaleMode(UpscaleMode::Dlss);
        RenderConfiguration::SetResolutionScale(0.5f);
    }
    if (frame == 56)
    {
        RenderConfiguration::Resize(1280, 720);
    }
    if (frame == 64)
    {
        RendererSettings::SetUpscaleMode(UpscaleMode::Bilateral);
    }
    if (frame == 68)
    {
        RenderConfiguration::SetResolutionScale(1.0f / 1.5f);
    }
    if (frame == 70)
    {
        RenderConfiguration::SetResolutionScale(1.0f / 1.7f);
    }
    if (frame == 72)
    {
        RenderConfiguration::SetResolutionScale(1.0f / 3.0f);
    }
    if (frame == 74)
    {
        RenderConfiguration::SetResolutionScale(1.0f / 1.3f);
    }
    if (frame == 82)
    {
        RenderConfiguration::SetResolutionScale(0.25f);
    }
    if (frame == 76)
    {
        ImGuiManager::m_HdrEnabled = true;
        ImGuiManager::m_ToneMapEnabled = false;
        RenderConfiguration::SetHdr(true);
    }
    if (frame == 79)
    {
        ImGuiManager::m_ToneMapEnabled = true;
    }
    GpuSceneValidation::BeforeFrame(frame);
}

void RenderingValidation::AfterFrame(uint64_t frame)
{
    if (ReferenceCaptureTestEnabled() && frame >= 5)
    {
        RunReferenceCaptureOnce();
    }
    if (!VmdCameraTestPath().empty())
    {
        AfterVmdCameraTestFrame(frame);
        return;
    }
    VirtualGeometryProduction::RecordBenchmark();
    if (VirtualGeometryProduction::ValidationEnabled())
    {
        VirtualGeometryProduction::AfterFrame(frame);
        return;
    }
    if (PlayRenderTestEnabled())
    {
        if (ShadowBenchmarkEnabled())
        {
            RecordShadowBenchmark();
            return;
        }
        if (frame == 18 || frame == 45 || frame == 70)
        {
            const auto& snapshot = RenderProfiler::GetLatest();
            WriteGraphicsLog(
                "Play render frame %llu: draws=%llu indexed=%llu dispatch=%llu indirect=%llu IA=%llu VS=%llu GPU=%.3f\n",
                frame, snapshot.Calls.DrawCalls, snapshot.Calls.IndexedDrawCalls, snapshot.Calls.DispatchCalls,
                snapshot.Calls.ExecuteIndirectCalls, snapshot.Pipeline.IAVertices, snapshot.Pipeline.VSInvocations,
                snapshot.GpuRenderMs);
            for (const auto& pass : snapshot.Passes)
            {
                WriteGraphicsLog("Play render pass %s: draws=%llu\n", pass.Name.c_str(), pass.DrawCalls);
            }
            if (frame == 45)
            {
                Check(!Raytracing::UsesRaytracedShadows() &&
                          RendererSettings::GetShadowMapMethod() == ShadowMapMethod::VirtualShadowMap,
                      "performance test keeps reflections-only raytracing and VSM shadows");
                Check(snapshot.Calls.DrawCalls < 64, "play avoids per-mesh per-page shadow draws");
                Check(snapshot.PipelineStatisticsValid && LightingResources::UseStaticShadowCache(0),
                      "VSM pipeline statistics and stationary-depth reuse are active");
            }
        }
        if (frame >= 57 && frame <= 61)
        {
            for (const auto& pass : RenderProfiler::GetLatest().Passes)
            {
                if (pass.Name == "Stationary Shadow Update")
                {
                    Check(pass.DrawCalls == 0, "moving-caster edits reuse stationary shadow depth");
                }
            }
        }
        if (frame == 48 || frame == 51 || frame == 61)
        {
            for (UINT layer = 0; layer < g_PlayShadowReference.size(); ++layer)
            {
                if (frame == 48)
                {
                    Check(RenderCapture::ReadTextureSlice(LightingResources::GetShadowMapResource(),
                                                          g_PlayShadowReference[layer], layer),
                          "capture reference shadow layer");
                    continue;
                }
                RenderCapture::TextureData optimized;
                Check(RenderCapture::ReadTextureSlice(LightingResources::GetShadowMapResource(), optimized, layer),
                      "capture optimized shadow layer");
                const auto& reference = g_PlayShadowReference[layer];
                uint64_t changed = 0;
                uint64_t populated = 0;
                float maximumError = 0;
                if (reference.Bytes.size() != optimized.Bytes.size() || reference.Bytes.empty())
                {
                    Check(false, "shadow comparison has equal nonempty dimensions");
                    continue;
                }
                for (UINT y = 0; y < reference.Height; ++y)
                {
                    for (UINT x = 0; x < reference.Width; ++x)
                    {
                        float a;
                        float b;
                        memcpy(&a, reference.Bytes.data() + y * reference.RowPitch + x * sizeof(float), sizeof(float));
                        memcpy(&b, optimized.Bytes.data() + y * optimized.RowPitch + x * sizeof(float), sizeof(float));
                        populated += a < 1.0f;
                        const float error = abs(a - b);
                        maximumError = max(maximumError, error);
                        changed += error > 0.00001f;
                    }
                }
                WriteGraphicsLog(
                    "Play shadow comparison frame=%llu layer=%u populated=%llu changed=%llu maxError=%.8f\n", frame,
                    layer, populated, changed, maximumError);
                Check(changed == 0, "merged and reference shadow depths match within 1e-5");
            }
        }
        return;
    }
    if (ThumbnailValidation::Enabled())
    {
        ThumbnailValidation::AfterFrame(frame);
        return;
    }
    if (MaterialValidation::Enabled())
    {
        MaterialValidation::AfterFrame(frame);
        return;
    }
    if (CloudValidation::Enabled())
    {
        CloudValidation::AfterFrame(frame);
        return;
    }
    if (SunLightingTestEnabled())
    {
        AfterSunLightingFrame(frame);
        return;
    }
    if (VirtualGeometryStreamingValidation::Enabled())
    {
        VirtualGeometryStreamingValidation::AfterFrame(frame);
        return;
    }
    if (VirtualGeometryValidation::Enabled())
    {
        VirtualGeometryValidation::AfterFrame(frame);
        return;
    }
    if (MeshletValidation::Enabled())
    {
        MeshletValidation::AfterFrame(frame);
        return;
    }
    if (GPUVisibilityValidation::Enabled())
    {
        GPUVisibilityValidation::AfterFrame(frame);
        return;
    }
    if (GPUDrivenValidation::Enabled())
    {
        GPUDrivenValidation::AfterFrame(frame);
        return;
    }
    GpuSceneValidation::AfterFrame(frame);
    if (!IsEnabled())
    {
        return;
    }
    ValidateProfilerFaults();
    if (frame == 5 || frame == 13)
    {
        ValidateRayTelemetry(frame == 13);
    }
    if (frame == 53)
    {
        Check(g_ProfilerFaultMask == 7, "profiler resolves all three injected GPU fault fixtures");
    }
    if (frame == 15)
    {
        Check(ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(g_Mover).Time > 0.0,
              "Play advances motion");
        ValidateRayTexture();
        if (Raytracing::IsAvailable())
        {
            Check(Raytracing::AreReflectionsActive(), "scene raytracing reflection dispatch");
            if (NrdDenoiser::IsCompiled())
            {
                Check(RayReflections::GetDenoiserDispatchCount() > 0,
                      "actual NVIDIA NRD REBLUR_SPECULAR dispatch sequence");
                Check(Raytracing::GetAmbientDenoiserDispatchCount() > 0,
                      "actual NVIDIA NRD REBLUR_DIFFUSE_OCCLUSION dispatch sequence");
            }
            RenderCapture::TextureData reflection;
            const bool read = RenderCapture::ReadTexture(RayReflections::GetResultResource(), reflection);
            Check(read, "read scene reflection radiance");
            if (read)
            {
                UINT hits = 0, offscreenHits = 0, bright = 0;
                bool finite = true;
                for (UINT y = 0; y < reflection.Height; ++y)
                {
                    const auto* pixels =
                        reinterpret_cast<const uint16_t*>(reflection.Bytes.data() + y * reflection.RowPitch);
                    for (UINT x = 0; x < reflection.Width; ++x)
                    {
                        const float distance = RenderCapture::HalfToFloat(pixels[x * 4 + 3]);
                        hits += abs(distance) > 0.01f;
                        offscreenHits += distance < -0.01f;
                        for (UINT c = 0; c < 3; ++c)
                        {
                            const float value = RenderCapture::HalfToFloat(pixels[x * 4 + c]);
                            finite &= isfinite(value) && value >= 0;
                            bright += value > 0.005f;
                        }
                    }
                }
                Check(finite && hits > 0 && bright > 0, "finite reflected radiance with actual scene intersections");
                Check(offscreenHits > 0, "reflection rays intersect geometry outside the camera image");
                WriteGraphicsLog("Validation reflection: hits=%u offscreenHits=%u brightChannels=%u extent=%ux%u\n",
                                 hits, offscreenHits, bright, reflection.Width, reflection.Height);
                ValidateReflectionDenoising(reflection);
            }
        }
        if (Raytracing::IsAvailable() && NrdDenoiser::IsCompiled())
        {
            Check(Raytracing::GetDenoiserDispatchCount() > 0, "actual NVIDIA NRD SIGMA dispatch sequence");
        }
        ValidateSkyBrightness(false);
    }
    if (frame == 31)
    {
        ValidateSkyBrightness(true);
        if (RenderProfiler::IsEnabled())
        {
            ValidateProfilerIntervals();
        }
    }
    if ((frame == 12 || frame == 13) && Raytracing::IsAvailable())
    {
        Check(Raytracing::AreReflectionsActive(), "reflection-only mode remains active");
        Check(!Raytracing::UsesRaytracedShadows() && !Raytracing::UsesRaytracedAmbientOcclusion(),
              "reflection-only mode uses raster shadows and disables ray AO");
        if (frame == 13 && NrdDenoiser::IsCompiled())
        {
            Check(!RayReflections::DidRestartHistory(), "reflection-only mode accumulates temporal history");
        }
        Check(LightingResources::GetShadowLightCount() > 0, "reflection-only mode has raster shadow passes");
        RenderCapture::TextureData shadowDepth;
        bool hasRasterShadow = RenderCapture::ReadTexture(RenderTargets::GetShadowDepthResource(), shadowDepth);
        UINT writtenDepth = 0;
        if (hasRasterShadow)
        {
            for (UINT y = 0; y < shadowDepth.Height; ++y)
            {
                const auto* values =
                    reinterpret_cast<const float*>(shadowDepth.Bytes.data() + y * shadowDepth.RowPitch);
                for (UINT x = 0; x < shadowDepth.Width; ++x)
                {
                    writtenDepth += isfinite(values[x]) && values[x] > 0.0f && values[x] < 1.0f;
                }
            }
        }
        Check(hasRasterShadow && writtenDepth > 0, frame == 12
                                                       ? "reflection-only ShadowMap writes geometry depth"
                                                       : "reflection-only Virtual ShadowMap writes geometry depth");
        RenderCapture::TextureData visibility;
        bool unshadowed = RenderCapture::ReadTexture(Raytracing::GetResultResource(), visibility);
        if (unshadowed)
        {
            for (UINT y = 0; y < visibility.Height; ++y)
            {
                const auto* pixels =
                    reinterpret_cast<const uint16_t*>(visibility.Bytes.data() + y * visibility.RowPitch);
                for (UINT x = 0; x < visibility.Width; ++x)
                {
                    unshadowed &= RenderCapture::HalfToFloat(pixels[x * 2]) >= 0.999f;
                }
            }
        }
        Check(unshadowed, "disabled ray shadows produce unshadowed visibility");
    }
    if (frame == 19 && Raytracing::IsAvailable() && NrdDenoiser::IsCompiled())
    {
        Check(RayReflections::DidRestartHistory(), "reflection re-enable restarts stale temporal history");
    }
    if (frame == 25 && Raytracing::IsAvailable())
    {
        Check(!Raytracing::AreReflectionsActive() && Raytracing::IsActive(),
              "reflection switch leaves raytracing shadows active");
    }
    if (frame == 23 || frame == 29 || frame == 37)
    {
        ValidateVertexDeformation();
    }
    if ((frame == 23 || frame == 31) && MeshShaderPipeline::IsSupported())
    {
        const auto& stats = MeshShaderPipeline::GetStatistics();
        Check(stats.Draws > 0 && stats.Meshlets > 0 && stats.MaxGroupsPerPage <= 65535,
              frame == 23 ? "mesh shader renders procedural vertices into MRT GBuffer"
                          : "mesh shader renders large procedural terrain into visibility GBuffer");
        WriteGraphicsLog("Validation mesh shaders: draws=%u meshlets=%u pages=%u maximumGroups=%u\n", stats.Draws,
                         stats.Meshlets, stats.Pages, stats.MaxGroupsPerPage);
        if (frame == 31)
        {
            Check(stats.Pages > stats.Draws, "GPU executes multiple mesh pages with a nonzero meshlet base");
        }
    }
    if (frame == 39)
    {
        VertexDeformationView view;
        Check(!VertexDeformation::TryResolveMesh(g_Mover, view), "disabled vertex motion restores base geometry");
    }
    if (frame == 17)
    {
        Check(ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(g_Mover).Time == g_PausedTime,
              "Pause freezes motion");
    }
    if (frame == 19)
    {
        Check(ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(g_Mover).Time > g_PausedTime,
              "resume advances motion");
    }
    if (frame == 20)
    {
        const auto& motion = ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(g_Mover);
        const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(g_Mover);
        Check(motion.Time == 0.0 && transform.Position.y == motion.Origin.y, "Stop restores motion origin and time");
    }
    if (frame == 29)
    {
        Check(GraphicsDevice::GetWidth() == 960 && GraphicsDevice::GetHeight() == 640, "resize render targets");
    }
    if (frame == 33)
    {
        Check(!Raytracing::IsActive(), "DXR off uses raster fallback");
        Check(TemporalUpscaler::HasValidHistory(), "Custom TSR renders with 20% input");
    }
    if (frame >= 33 && frame <= 35)
    {
        Check(RendererState::m_TaaFrameIndex == g_TsrJitterBeforeDraw + 1,
              frame == 33 ? "TSR jitter advances without TAA history"
                          : "input TAA plus TSR advances jitter exactly once");
        Check(TemporalUpscaler::HasValidHistory(), "TSR history survives input-TAA mode switch");
    }
    if (frame == 37 && Raytracing::IsAvailable())
    {
        Check(Raytracing::IsActive(), "DXR on rebuilds active frame");
    }
    if (frame == 41)
    {
        Check(!Raytracing::IsActive() && Raytracing::GetInstanceCount() == 0, "empty acceleration structure fallback");
    }
    if (frame == 47 && Raytracing::IsAvailable())
    {
        Check(Raytracing::IsActive(), "DXR resumes after geometry recreation");
    }
    if (frame == 5 || frame == 53 || frame == 61 || frame == 69 || frame == 71 || frame == 73 || frame == 75 ||
        frame == 78 || frame == 81 || frame == 83)
    {
        if (frame == 5 && RendererSettings::GetUpscaleMode() != UpscaleMode::Dlss)
        {
            return;
        }
        if (DlssUpscaler::IsAvailable())
        {
            Check(DlssUpscaler::IsActive(), "DLSS evaluates after resize/toggle/format change");
        }
        else
        {
            char disabled[8]{};
            const bool forcedDisabled =
                GetEnvironmentVariableA("DX12_DISABLE_DLSS", disabled, sizeof(disabled)) > 0 && atoi(disabled) != 0;
            Check(!DlssUpscaler::IsActive(), "unsupported DLSS uses raster upscale fallback");
            // 対応デバイスでは、評価中に DLSS が理由なく無効になってはならない。
            Check(forcedDisabled || string(DlssUpscaler::GetStatus()).find("failed") == string::npos,
                  "DLSS availability does not hide evaluation failure");
        }
        const auto& timing = RenderProfiler::GetLatest();
        for (const auto& pass : timing.Passes)
        {
            if (pass.Name == "DXR Shadows / AO" || pass.Name == "DLSS Super Resolution")
            {
                WriteGraphicsLog("Validation timing: %s GPU %.3f ms CPU %.3f ms\n", pass.Name.c_str(), pass.GpuMs,
                                 pass.CpuMs);
            }
        }
    }
    if (frame == 65)
    {
        Check(!DlssUpscaler::IsActive(), "DLSS off uses Bilateral");
    }
    if (frame == 55)
    {
        Check(RenderCapture::SaveScenePng(L"bin/validation/dlss.png"), "capture DLSS output");
    }
    if (frame < 32 && (frame % 8) == 7)
    {
        const wchar_t* paths[] = {L"bin/validation/morning.png", L"bin/validation/noon.png",
                                  L"bin/validation/evening.png", L"bin/validation/night.png"};
        Check(RenderCapture::SaveScenePng(paths[frame / 8]), "capture day period");
    }
}

bool RenderingValidation::Finish()
{
    if (!VmdCameraTestPath().empty())
    {
        Check(g_VmdCameraTest.Completed, "complete VMD GameCamera play/pause/stop sequence");
        Check(g_VmdCameraTest.SawOrthographic && g_VmdCameraTest.SawPerspective,
              "VMD runtime renders both projection modes in one clip");
        g_Failures += RenderCapture::ReportGraphicsErrors();
        WriteGraphicsLog("VMD camera failures = %u\n", g_Failures);
        return g_Failures == 0;
    }
    if (VirtualGeometryProduction::ValidationEnabled())
    {
        return VirtualGeometryProduction::Finish();
    }
    if (PlayRenderTestEnabled())
    {
        if (ShadowBenchmarkEnabled())
        {
            const char* names[] = {"edit", "reference-play", "optimized-play"};
            for (size_t i = 0; i < g_ShadowBenchmark.size(); ++i)
            {
                const auto& sample = g_ShadowBenchmark[i];
                Check(sample.Count >= (i == 0 ? 8u : 40u), "collect enough VSM benchmark samples");
                if (sample.Count)
                {
                    WriteGraphicsLog(
                        "VSM benchmark %s: samples=%llu IA=%.0f maxIA=%llu draws=%.1f GPU=%.3f CPU=%.3f shadowGPU=%.3f frameMs=%.3f FPS=%.1f\n",
                        names[i], sample.Count, double(sample.Vertices) / sample.Count, sample.MaxVertices,
                        double(sample.Draws) / sample.Count, sample.Gpu / sample.Count, sample.Cpu / sample.Count,
                        sample.ShadowGpu / sample.Count, sample.FrameMs / sample.Count,
                        sample.FrameMs > 0 ? 1000.0 * sample.Count / sample.FrameMs : 0);
                }
                Check(sample.GpuSamples == sample.Count, "benchmark has valid GPU timestamps for every sample");
                for (const auto& pass : sample.PassTotals)
                {
                    WriteGraphicsLog("Render benchmark %s pass [%s]: samples=%llu GPU=%.4f CPU=%.4f draws=%.2f\n",
                                     names[i], pass.Name.c_str(), sample.GpuSamples, pass.GpuMs / sample.GpuSamples,
                                     pass.CpuMs / sample.GpuSamples, double(pass.DrawCalls) / sample.GpuSamples);
                }
            }
            const auto& reference = g_ShadowBenchmark[1];
            const auto& optimized = g_ShadowBenchmark[2];
            Check(reference.Count && optimized.Count &&
                      double(optimized.Vertices) / optimized.Count <
                          double(reference.Vertices) / reference.Count * 0.75,
                  "VSM live play reduces average IA vertex work by at least 25 percent");
        }
        g_Failures += RenderCapture::ReportGraphicsErrors();
        WriteGraphicsLog("Play render failures = %u\n", g_Failures);
        return g_Failures == 0;
    }
    if (ThumbnailValidation::Enabled())
    {
        return ThumbnailValidation::Finish();
    }
    if (MaterialValidation::Enabled())
    {
        return MaterialValidation::Finish();
    }
    if (CloudValidation::Enabled())
    {
        return CloudValidation::Finish();
    }
    if (VirtualGeometryStreamingValidation::Enabled())
    {
        return VirtualGeometryStreamingValidation::Finish();
    }
    if (VirtualGeometryValidation::Enabled())
    {
        return VirtualGeometryValidation::Finish();
    }
    if (MeshletValidation::Enabled())
    {
        return MeshletValidation::Finish();
    }
    if (GPUVisibilityValidation::Enabled())
    {
        return GPUVisibilityValidation::Finish();
    }
    if (GPUDrivenValidation::Enabled())
    {
        return GPUDrivenValidation::Finish();
    }
    if (!IsEnabled())
    {
        return true;
    }
    GraphicsDevice::WaitForGpuIdle();
    Check(g_FrameCount >= 84, "complete feature smoke sequence");
    g_Failures += GpuSceneValidation::Failures();
    g_Failures += RenderCapture::ReportGraphicsErrors();
    WriteGraphicsLog("Validation: feature smoke failures = %u\n", g_Failures);
    return g_Failures == 0;
}
