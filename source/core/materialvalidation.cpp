#include "pch.h"
#include "transmittanceshadows.h"
#include "materialvalidation.h"
#include "materialmanager.h"
#include "materialbindings.h"
#include "materialpreview.h"
#include "psomanager.h"
#include "graphicslog.h"
#include "graphicsdevice.h"
#include "world.h"
#include "camera.h"
#include "primitivegeometry.h"
#include "modelmanager.h"
#include "imguimanager.h"
#include "renderconfiguration.h"
#include "renderersettings.h"
#include "rendercapture.h"
#include "renderprofiler.h"
#include "rendertargets.h"
#include "gpuscene/gpuscene.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "visibility/gpuvisibility.h"
#include "raytracing.h"
#include "daynightcycle.h"
#include "materialsystem.h"
#include "atmosphere.h"
#include "cloud/cloudrenderer.h"
#include "lightingresources.h"
#include <fstream>

namespace
{
uint32_t failures = 0;
uint64_t frames = 0;
EntityID first = g_kINVALID_ENTITY;
EntityID second = g_kINVALID_ENTITY;
EntityID multi = g_kINVALID_ENTITY;
AssetHandle handle = 0;
AssetHandle copyHandle = 0;
AssetHandle normalTextureHandle = 0;
AssetHandle surfaceTextureHandle = 0;
uint32_t gpuIndex = UINT32_MAX;
uint64_t constantEditPipelineCreations = 0;
filesystem::path fixtureRoot;
vector<filesystem::path> ownedFiles;
array<RenderCapture::TextureData, 6> reference;
RenderCapture::TextureData withNormal;
XMFLOAT2 inspectorDrop{};
XMFLOAT2 normalDrop{};
XMFLOAT2 revertButton{};
bool injectDrag = false;
EntityID transparencyFixture = g_kINVALID_ENTITY;
RenderCapture::TextureData opaqueLighting;
RenderCapture::TextureData invisibleLighting;
RenderCapture::TextureData shadowedReceiver;
XMFLOAT3 thinTransmission{};
EntityID secondTransmissionCaster = g_kINVALID_ENTITY;
EntityID transmissionReceiver = g_kINVALID_ENTITY;
XMFLOAT3 receiverBeforeCaster{}, receiverBehindCaster{};
void Check(bool passed, const char* message);

XMFLOAT3 MinimumTransmission(UINT slice)
{
    RenderCapture::TextureData texture;
    if (!RenderCapture::ReadTextureSlice(TransmittanceShadows::GetColorResource(), texture, slice))
    {
        Check(false, "read transparent shadow layer");
        return {1, 1, 1};
    }
    XMFLOAT3 minimum{1, 1, 1};
    std::vector<uint8_t> resident(texture.Width * texture.Height, 0);
    const float scale = static_cast<float>(texture.Width) /
        static_cast<float>(LightingResources::GetShadowMapResource()->GetDesc().Width);
    for (UINT pass = 0; pass < LightingResources::GetShadowLightCount(); ++pass)
    {
        UINT layer;
        D3D12_VIEWPORT viewport;
        D3D12_RECT rect;
        bool clearLayer;
        if (!LightingResources::GetShadowPassInfo(pass, layer, viewport, rect, clearLayer) || layer != slice / 4) continue;
        for (UINT y = static_cast<UINT>(rect.top * scale); y < static_cast<UINT>(rect.bottom * scale); ++y)
            for (UINT x = static_cast<UINT>(rect.left * scale); x < static_cast<UINT>(rect.right * scale); ++x)
                resident[y * texture.Width + x] = 1;
    }
    for (UINT y = 0; y < texture.Height; ++y)
    {
        const auto* pixels = reinterpret_cast<const uint16_t*>(texture.Bytes.data() + y * texture.RowPitch);
        for (UINT x = 0; x < texture.Width; ++x)
        {
            if (!resident[y * texture.Width + x]) continue;
            minimum.x = min(minimum.x, RenderCapture::HalfToFloat(pixels[x * 4]));
            minimum.y = min(minimum.y, RenderCapture::HalfToFloat(pixels[x * 4 + 1]));
            minimum.z = min(minimum.z, RenderCapture::HalfToFloat(pixels[x * 4 + 2]));
        }
    }
    return minimum;
}
void Check(bool passed, const char* message);

void CheckShadowDepth(bool expectedOcclusion)
{
    RenderCapture::TextureData shadow;
    bool captured = RenderCapture::ReadTexture(LightingResources::GetShadowMapResource(), shadow);
    Check(captured && LightingResources::GetShadowLightCount() > 0, "capture active material shadow map");
    if (!captured)
    {
        return;
    }
    bool occluded = false;
    for (UINT row = 0; row < shadow.Height && !occluded; ++row)
    {
        auto* depth = reinterpret_cast<const float*>(shadow.Bytes.data() + row * shadow.RowPitch);
        for (UINT column = 0; column < shadow.Width; ++column)
        {
            if (depth[column] < 0.9999f)
            {
                occluded = true;
                break;
            }
        }
    }
    Check(occluded == expectedOcclusion, expectedOcclusion ?
        "material shadow caster writes depth" : "disabled or clipped material writes no shadow depth");
}
AssetDragPayload uiPayload;

void Check(bool passed, const char* message)
{
    WriteGraphicsLog("Material validation: %s %s\n", passed ? "PASS" : "FAIL", message);
    if (!passed)
    {
        ++failures;
    }
}

EntityID MakeCube(float x)
{
    auto entity = World::CreateEntity().Add<TransformComponent>().Add<MeshComponent>().Add<MaterialComponent>().Add<NameComponent>().Add<LODComponent>();
    entity.SetName("Material fixture");
    auto& transform = entity.Get<TransformComponent>();
    transform.Position = {x, 0, 0};
    transform.Scale = {0.8f, 0.8f, 0.8f};
    transform.IsDirty = true;
    VertexResource geometry;
    geometry.entityid = entity.GetID();
    geometry.objectType = ObjectType::CUBE;
    geometry.color = Color::WHITE;
    PrimitiveGeometry::CreateObjectVertex(geometry);
    entity.Get<LODComponent>().UseLOD = true;
    return entity.GetID();
}

void MakeNormalTexture(const filesystem::path& path)
{
    array<uint8_t, 18> header{};
    header[2] = 2;
    header[12] = 16;
    header[14] = 16;
    header[16] = 32;
    header[17] = 0x20;
    ofstream stream(path, ios::binary);
    stream.write(reinterpret_cast<const char*>(header.data()), header.size());
    for (int y = 0; y < 16; ++y)
    {
        for (int x = 0; x < 16; ++x)
        {
            const uint8_t pixel[] = {220, static_cast<uint8_t>(80 + y * 6), static_cast<uint8_t>(70 + x * 7), 255};
            stream.write(reinterpret_cast<const char*>(pixel), 4);
        }
    }
}

void MakeSurfaceTexture(const filesystem::path& path)
{
    array<uint8_t, 18> header{};
    header[2] = 2;
    header[12] = 4;
    header[14] = 4;
    header[16] = 32;
    header[17] = 0x20;
    ofstream stream(path, ios::binary);
    stream.write(reinterpret_cast<const char*>(header.data()), header.size());
    const uint8_t pixel[] = {200, 128, 64, 255};
    for (int texel = 0; texel < 16; ++texel)
    {
        stream.write(reinterpret_cast<const char*>(pixel), sizeof(pixel));
    }
}

bool EqualPixels(const RenderCapture::TextureData& left, const RenderCapture::TextureData& right, UINT pixelBytes)
{
    if (left.Width != right.Width || left.Height != right.Height || left.Format != right.Format)
    {
        return false;
    }
    for (UINT row = 0; row < left.Height; ++row)
    {
        if (memcmp(left.Bytes.data() + row * left.RowPitch, right.Bytes.data() + row * right.RowPitch,
            left.Width * pixelBytes) != 0)
        {
            return false;
        }
    }
    return true;
}

void PrepareTransparencyFrame(uint64_t frame)
{
    if (frame == 24)
    {
        RenderConfiguration::SetHdr(true);
        vector<EntityID> remove;
        for (uint32_t entity = 0; entity < Registry::GetEntities().size(); ++entity)
        {
            if (Registry::IsAlive(entity) && !ComponentManager::HasComponent<CameraComponent>(entity))
            {
                remove.push_back(entity);
            }
        }
        for (EntityID entity : remove)
        {
            World::DestroyEntity(Entity(entity));
        }
        GPUDrivenRenderer::SetEnabled(false);
        RendererSettings::SetComputeGBufferEnabled(false);
        Atmosphere::GetMutableParameters().Enabled = false;
        Atmosphere::GetMutableParameters().PhysicalSky = false;
        CloudRenderer::GetSettings().Enabled = false;
        transparencyFixture = MakeCube(0);
        MaterialManager::Assign(transparencyFixture, 0, copyHandle);
        auto editing = MaterialManager::GetRuntime(copyHandle).Editing;
        editing.Surface = MaterialSurface::Opaque;
        editing.ShadowMode = MaterialShadowMode::None;
        editing.BaseColor = {0.4f, 0.6f, 0.8f, 1};
        editing.Metallic = 0.35f;
        editing.Roughness = 0.35f;
        editing.Textures.fill(0);
        editing.EmissiveIntensity = 0.0f;
        MaterialManager::SetEditing(copyHandle, editing);
        for (int index = 0; index < 3; ++index)
        {
            auto entity = World::CreateEntity().Add<TransformComponent>().Add<LightComponent>();
            auto& light = entity.Get<LightComponent>();
            light.Type = static_cast<LightType>(index);
            light.Direction = {-0.4f, 0.7f, -1.0f};
            light.Position = {static_cast<float>(index - 1) * 2, 2, -3};
            entity.Get<TransformComponent>().Position = light.Position;
            light.Range = 12.0f;
            light.Intensity = 2.0f;
            light.InnerAngle = 0.4f;
            light.OuterAngle = 0.8f;
            light.Color = {1, 0.9f, 0.8f, 1};
        }
    }
    if (frame == 26)
    {
        auto editing = MaterialManager::GetRuntime(copyHandle).Editing;
        editing.Surface = MaterialSurface::Transparent;
        MaterialManager::SetEditing(copyHandle, editing);
    }
    if (frame == 28)
    {
        auto editing = MaterialManager::GetRuntime(copyHandle).Editing;
        editing.BaseColor[3] = 0;
        editing.EmissiveIntensity = 10;
        editing.ShadowMode = MaterialShadowMode::Opaque;
        MaterialManager::SetEditing(copyHandle, editing);
        Check(!MaterialSystem::CastsShadow(MaterialManager::GetRuntime(copyHandle).Binding),
            "opacity zero suppresses opaque shadow casting");
    }
    if (frame == 30)
    {
        World::DestroyEntity(Entity(transparencyFixture));
    }
    if (frame == 32)
    {
        RendererSettings::SetShadowMapMethod(ShadowMapMethod::ShadowMap);
        transparencyFixture = MakeCube(0);
        MaterialManager::Assign(transparencyFixture, 0, copyHandle);
        auto editing = MaterialManager::GetRuntime(copyHandle).Editing;
        editing.BaseColor[3] = 0.5f;
        editing.EmissiveIntensity = 0;
        editing.ShadowMode = MaterialShadowMode::Opaque;
        MaterialManager::SetEditing(copyHandle, editing);
        for (EntityID entity : World::GetView<LightComponent>())
        {
            auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
            light.CastShadow = light.Type == LightType::Directional;
        }
    }
    if (frame == 34 || frame == 36 || frame == 38 || frame == 40)
    {
        auto editing = MaterialManager::GetRuntime(copyHandle).Editing;
        if (frame == 34)
        {
            editing.ShadowMode = MaterialShadowMode::None;
        }
        else if (frame == 40)
        {
            editing.ShadowMode = MaterialShadowMode::Opaque;
            editing.BaseColor[3] = 0;
        }
        else
        {
            editing.ShadowMode = MaterialShadowMode::AlphaTest;
            editing.ShadowCutoff = frame == 36 ? 0.25f : 0.75f;
        }
        MaterialManager::SetEditing(copyHandle, editing);
    }
    if (frame == 42)
    {
        auto editing = MaterialManager::GetRuntime(copyHandle).Editing;
        editing.BaseColor[3] = 0.5f;
        editing.ShadowMode = MaterialShadowMode::Opaque;
        MaterialManager::SetEditing(copyHandle, editing);
        auto receiver = MakeCube(0);
        transmissionReceiver = receiver;
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(receiver);
        transform.Position = {0, -1, 0};
        transform.Scale = {4, 0.1f, 4};
        transform.IsDirty = true;
        auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(receiver);
        material.ShaderClassMode = MaterialMode::Manual;
        material.ShaderClass = ShaderClass::PBR;
        material.IsTransparent = true;
        material.Alpha = 0.75f;
        material.ShadowMode = MaterialShadowMode::None;
    }
    if (frame == 44)
    {
        for (EntityID entity : World::GetView<LightComponent>())
        {
            ComponentManager::GetComponentUnchecked<LightComponent>(entity).CastShadow = false;
        }
    }
}

void PrepareTransmissionFrame(uint64_t frame)
{
    if (frame < 46 || (frame & 1) != 0) return;
    auto editing = MaterialManager::GetRuntime(copyHandle).Editing;
    if (frame == 46)
    {
        editing.Surface = MaterialSurface::Transparent;
        editing.ShadowMode = MaterialShadowMode::Auto;
        editing.BaseColor = {1, 1, 1, 0.5f};
        editing.Transmission = 0;
        editing.Metallic = 0;
        editing.Thickness = 0.5f;
        editing.AbsorptionCoefficient = {0, 0, 0};
        editing.IOR = 1.5f;
        for (EntityID entity : World::GetView<LightComponent>())
        {
            auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
            light.CastShadow = light.Type == LightType::Directional;
            light.Direction = {0, 1, 0};
        }
    }
    if (frame == 48) editing.Transmission = 1;
    if (frame == 50) editing.AbsorptionCoefficient = {2, 0.5f, 0};
    if (frame == 52) editing.Thickness = 1;
    if (frame == 54)
    {
        secondTransmissionCaster = MakeCube(0);
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(secondTransmissionCaster);
        transform.Position = {0, 1.5f, 0};
        transform.IsDirty = true;
        MaterialManager::Assign(secondTransmissionCaster, 0, copyHandle);
    }
    if (frame == 56) editing.BaseColor[3] = 0;
    if (frame == 58 || frame == 62)
    {
        if (frame == 58) World::DestroyEntity(Entity(secondTransmissionCaster));
        editing.BaseColor[3] = 1;
        editing.Transmission = 1;
        editing.ShadowMode = MaterialShadowMode::Transmittance;
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(transmissionReceiver);
        transform.Position.y = frame == 58 ? 0.9f : -1.0f;
        transform.IsDirty = true;
        auto& receiver = ComponentManager::GetComponentUnchecked<MaterialComponent>(transmissionReceiver);
        receiver.Alpha = 1;
        receiver.IsTransparent = false;
    }
    if (frame == 60 || frame == 64) editing.ShadowMode = MaterialShadowMode::None;
    if (frame == 66)
    {
        editing.ShadowMode = MaterialShadowMode::Transmittance;
        RendererSettings::SetShadowMapMethod(ShadowMapMethod::VirtualShadowMap);
    }
    if (frame == 68) editing.AbsorptionCoefficient = {0, 0, 0};
    MaterialManager::SetEditing(copyHandle, editing);
}

XMFLOAT3 ReadReceiverLight()
{
    RenderCapture::TextureData scene;
    if (!RenderCapture::ReadTexture(RenderTargets::GetSceneResource(), scene))
    {
        Check(false, "read receiver HDR radiance");
        return {};
    }
    XMMATRIX view, projection;
    Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
    auto position = ComponentManager::GetComponentUnchecked<TransformComponent>(transmissionReceiver).Position;
    position.y += 0.05f;
    XMFLOAT3 pixel;
    XMStoreFloat3(&pixel, XMVector3Project(XMLoadFloat3(&position), 0, 0,
        static_cast<float>(scene.Width), static_cast<float>(scene.Height), 0, 1, projection, view, XMMatrixIdentity()));
    if (pixel.x < 0 || pixel.x >= scene.Width || pixel.y < 0 || pixel.y >= scene.Height)
    {
        Check(false, "receiver sample is in view");
        return {};
    }
    const auto* rgb = reinterpret_cast<const uint16_t*>(scene.Bytes.data() + static_cast<UINT>(pixel.y) * scene.RowPitch + static_cast<UINT>(pixel.x) * 8);
    return {RenderCapture::HalfToFloat(rgb[0]), RenderCapture::HalfToFloat(rgb[1]), RenderCapture::HalfToFloat(rgb[2])};
}

void CheckTransparencyFrame(uint64_t frame)
{
    if (frame == 33 || frame == 35 || frame == 37 || frame == 39 || frame == 41)
    {
        CheckShadowDepth(frame == 33 || frame == 37);
    }
    if (frame == 43)
    {
        Check(RenderCapture::ReadTexture(RenderTargets::GetSceneResource(), shadowedReceiver),
            "capture transparent receiver with material shadow");
        RenderCapture::SaveTexturePng(RenderTargets::GetSceneResource(), L"build/material-tests/transparency-shadow.png");
    }
    if (frame == 45)
    {
        RenderCapture::TextureData unshadowed;
        Check(RenderCapture::ReadTexture(RenderTargets::GetSceneResource(), unshadowed) &&
            !EqualPixels(shadowedReceiver, unshadowed, 8), "Forward transparent receiver responds to shared shadow visibility");
    }
    if (frame == 25)
    {
        Check(RenderCapture::ReadTexture(RenderTargets::GetSceneResource(), opaqueLighting),
            "capture Deferred PBR reference");
        RenderCapture::SaveTexturePng(RenderTargets::GetSceneResource(),
            L"build/material-tests/transparency-deferred.png");
    }
    if (frame == 27)
    {
        RenderCapture::TextureData forward;
        bool captured = RenderCapture::ReadTexture(RenderTargets::GetSceneResource(), forward);
        Check(captured && forward.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
            forward.Width == opaqueLighting.Width && forward.Height == opaqueLighting.Height,
            "capture Forward opacity-one HDR result");
        if (captured && forward.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
            forward.Width == opaqueLighting.Width && forward.Height == opaqueLighting.Height)
        {
            double errorSum = 0;
            double referenceSum = 0;
            for (UINT row = 0; row < forward.Height; ++row)
            {
                for (UINT column = 0; column < forward.Width; ++column)
                {
                    auto* expected = reinterpret_cast<const uint16_t*>(opaqueLighting.Bytes.data() + row * opaqueLighting.RowPitch + column * 8);
                    auto* actual = reinterpret_cast<const uint16_t*>(forward.Bytes.data() + row * forward.RowPitch + column * 8);
                    for (UINT channel = 0; channel < 3; ++channel)
                    {
                        const float referenceValue = RenderCapture::HalfToFloat(expected[channel]);
                        errorSum += fabs(RenderCapture::HalfToFloat(actual[channel]) - referenceValue);
                        referenceSum += fabs(referenceValue);
                    }
                }
            }
            const double relativeError = errorSum / max(referenceSum, 0.000001);
            WriteGraphicsLog("Transparency relative HDR lighting error = %.6f\n", relativeError);
            Check(relativeError < 0.02, "Deferred / Forward HDR lighting agrees within two percent");
        }
        RenderCapture::SaveTexturePng(RenderTargets::GetSceneResource(),
            L"build/material-tests/transparency-forward.png");
    }
    if (frame == 29)
    {
        Check(RenderCapture::ReadTexture(RenderTargets::GetSceneResource(), invisibleLighting),
            "capture zero-opacity with emission");
    }
    if (frame == 31)
    {
        RenderCapture::TextureData absent;
        Check(RenderCapture::ReadTexture(RenderTargets::GetSceneResource(), absent) &&
            EqualPixels(invisibleLighting, absent, 8), "opacity zero equals no draw pixel-for-pixel");
        Check(!EqualPixels(opaqueLighting, absent, 8), "lighting comparison contains a visible material fixture");
    }
    if (frame == 47)
    {
        const auto t = MinimumTransmission(0);
        Check(fabs(t.x - 0.5f) < 0.002f && fabs(t.y - 0.5f) < 0.002f,
            "half coverage transmits half the source light");
        CheckShadowDepth(false);
        Check(!Raytracing::UsesRaytracedShadows(), "transparent casters use material-aware raster shadows");
    }
    if (frame == 49)
    {
        const auto t = MinimumTransmission(0);
        Check(fabs(t.x - 0.96153846f) < 0.002f, "dielectric slab includes Fresnel exit and internal reflections");
    }
    if (frame == 51)
    {
        thinTransmission = MinimumTransmission(0);
        Check(thinTransmission.x < thinTransmission.y && thinTransmission.y < thinTransmission.z,
            "RGB absorption produces colored transmissive shadows");
    }
    if (frame == 53)
    {
        const auto t = MinimumTransmission(0);
        Check(t.x < thinTransmission.x && t.y < thinTransmission.y && fabs(t.z - thinTransmission.z) < 0.002f,
            "increasing thickness absorbs more light without changing nonabsorbing wavelengths");
    }
    if (frame == 55)
    {
        const auto firstLayer = MinimumTransmission(0);
        const auto secondLayer = MinimumTransmission(1);
        const auto emptyLayer = MinimumTransmission(2);
        Check(firstLayer.x < 0.9f && secondLayer.x < 0.9f && emptyLayer.x == 1,
            "depth peeling captures two entries without double-counting exits");
        Check(RenderCapture::SaveScenePng(L"build/material-tests/transmission-overlap.png"), "capture colored transparent shadow scene");
    }
    if (frame == 57) Check(!TransmittanceShadows::HasCasters(), "zero opacity bypasses transmissive shadow rendering");
    if (frame == 59) receiverBeforeCaster = ReadReceiverLight();
    if (frame == 61)
    {
        const auto unshadowed = ReadReceiverLight();
        Check(receiverBeforeCaster.x > 0 && fabs(receiverBeforeCaster.x - unshadowed.x) < 0.002f &&
            fabs(receiverBeforeCaster.y - unshadowed.y) < 0.002f, "objects behind a receiver do not attenuate its light");
    }
    if (frame == 63) receiverBehindCaster = ReadReceiverLight();
    if (frame == 65)
    {
        const auto unshadowed = ReadReceiverLight();
        Check(unshadowed.x > receiverBehindCaster.x + 0.01f && unshadowed.y > receiverBehindCaster.y + 0.01f,
            "Deferred receiver radiance includes optical shadow transmission");
        Check(fabs(unshadowed.z - receiverBehindCaster.z) < max(0.01f, unshadowed.z * 0.15f),
            "nonabsorbing blue light remains transmitted apart from Fresnel reflection");
    }
    if (frame == 67) Check(MinimumTransmission(0).x < 0.9f, "virtual shadow pages contain RGB transmission");
    if (frame == 69) Check(MinimumTransmission(0).x > 0.9f, "optical parameter edits invalidate virtual shadow pages");
}
}

bool MaterialValidation::Enabled()
{
    char value[8]{};
    return GetEnvironmentVariableA("DX12_MATERIAL_ASSET_TEST", value, sizeof(value)) > 0 && atoi(value) != 0;
}

void MaterialValidation::BeforeFrame(uint64_t frame)
{
    frames = frame + 1;
    PrepareTransparencyFrame(frame);
    PrepareTransmissionFrame(frame);
    if (frame == 0)
    {
        RenderProfiler::SetWindowVisible(false);
        ImGuiManager::m_ShowAdjustmentPanel = false;
        ImGuiManager::m_ProjectSettingsLoaded = true;
        vector<EntityID> remove;
        for (uint32_t entity = 0; entity < Registry::GetEntities().size(); ++entity)
        {
            if (Registry::IsAlive(entity) && !ComponentManager::HasComponent<CameraComponent>(entity))
            {
                remove.push_back(entity);
            }
        }
        for (EntityID entity : remove)
        {
            World::DestroyEntity(Entity(entity));
        }
        fixtureRoot = filesystem::path("asset") / ("__material_validation_" + to_string(GetCurrentProcessId()));
        filesystem::create_directories(fixtureRoot);
        filesystem::create_directories("build/material-tests");
        const auto normal = fixtureRoot / "Normal.tga";
        MakeNormalTexture(normal);
        ownedFiles.push_back(normal);
        ownedFiles.emplace_back(normal.wstring() + L".meta");
        string error;
        const auto normalHandle = MaterialManager::Registry().Register(normal, AssetType::Texture, error);
        normalTextureHandle = normalHandle;
        Check(normalHandle != 0, "register texture GUID");
        const auto surface = fixtureRoot / "Surface.tga";
        MakeSurfaceTexture(surface);
        ownedFiles.push_back(surface);
        ownedFiles.emplace_back(surface.wstring() + L".meta");
        surfaceTextureHandle = MaterialManager::Registry().Register(surface, AssetType::Texture, error);
        MaterialAssetData authored;
        authored.BaseColor = {0.7f, 0.25f, 0.13f, 1};
        authored.Roughness = 0.7f;
        authored.Metallic = 0.6f;
        authored.AOStrength = 0.8f;
        authored.Textures = {surfaceTextureHandle, normalHandle, surfaceTextureHandle, surfaceTextureHandle, surfaceTextureHandle};
        authored.Textures[1] = normalHandle;
        authored.EmissiveColor = {0.2f, 0.1f, 0.02f};
        authored.EmissiveIntensity = 0.4f;
        const auto path = fixtureRoot / "Brick.material";
        Check(MaterialSerializer::Save(path, authored, error), "create material file");
        ownedFiles.push_back(path);
        ownedFiles.emplace_back(path.wstring() + L".meta");
        handle = MaterialManager::Registry().Register(path, AssetType::Material, error);
        ImGuiManager::OpenMaterialAsset(handle);
        first = MakeCube(-1.2f);
        second = MakeCube(1.2f);
        Check(MaterialManager::Assign(first, 0, handle) && MaterialManager::Assign(second, 0, handle), "assign shared material slots");
        const auto& left = MaterialManager::Resolve(ComponentManager::GetComponentUnchecked<MaterialComponent>(first));
        const auto& right = MaterialManager::Resolve(ComponentManager::GetComponentUnchecked<MaterialComponent>(second));
        Check(&left == &right && left.NormalMapID >= 0, "one shared runtime and resolved normal descriptor");
        Check(!MaterialManager::Assign(Camera::GetCameraEntity(), 0, handle), "reject camera material drop");
        Check(!MaterialManager::Assign(first, 1, handle), "reject out-of-range material slot");
        const auto copy = fixtureRoot / "Brick_Copy.material";
        copyHandle = MaterialManager::Registry().Duplicate(handle, copy, error);
        ownedFiles.push_back(copy);
        ownedFiles.emplace_back(copy.wstring() + L".meta");
        Check(copyHandle != 0 && copyHandle != handle, "duplicate material has independent identity");
        ImGuiManager::AssignMaterialAsset(first, 0, copyHandle);
        ImGuiManager::Undo();
        Check(MaterialManager::SlotHandle(ComponentManager::GetComponentUnchecked<MaterialComponent>(first), 0) == handle, "Undo restores assignment");
        ImGuiManager::Redo();
        Check(MaterialManager::SlotHandle(ComponentManager::GetComponentUnchecked<MaterialComponent>(first), 0) == copyHandle, "Redo reapplies assignment");
        MaterialManager::Assign(first, 0, handle);
        Check(MaterialManager::SaveSceneReferences("build/material-tests/scene.json", error), "scene references saved");
        MaterialManager::Assign(second, 0, copyHandle);
        Check(MaterialManager::LoadSceneReferences("build/material-tests/scene.json", error) &&
            MaterialManager::SlotHandle(ComponentManager::GetComponentUnchecked<MaterialComponent>(second), 0) == handle, "scene GUID reference roundtrip");
        const auto moved = fixtureRoot / "Wall.material";
        Check(MaterialManager::Registry().Move(handle, moved, error), "rename/move preserves material GUID");
        ownedFiles.push_back(moved);
        ownedFiles.emplace_back(moved.wstring() + L".meta");
        Check(MaterialManager::Resolve(ComponentManager::GetComponentUnchecked<MaterialComponent>(first)).RuntimeHandle == handle, "scene reference survives rename");
        const auto meshPath = fixtureRoot / "sections.obj";
        const auto mtlPath = fixtureRoot / "sections.mtl";
        ofstream(meshPath) << "mtllib sections.mtl\nv -1 -1 0\nv 0 1 0\nv 1 -1 0\nv 2 -1 0\nv 3 1 0\nv 4 -1 0\nusemtl One\nf 1 2 3\nusemtl Two\nf 4 5 6\n";
        ofstream(mtlPath) << "newmtl One\nKd 1 1 1\nnewmtl Two\nKd 0.5 1 1\n";
        ownedFiles.push_back(meshPath);
        ownedFiles.push_back(mtlPath);
        auto entity = World::CreateEntity().Add<TransformComponent>().Add<StaticModelComponent>().Add<MaterialComponent>();
        multi = entity.GetID();
        entity.Get<StaticModelComponent>().ModelId = ModelManager::LoadStaticModel(meshPath.generic_string().c_str(), false);
        entity.Get<TransformComponent>().Position = {0, 100, 0};
        Check(MaterialManager::SlotCount(multi) == 2, "multi-submesh exposes two slots");
        MaterialManager::Assign(multi, 0, handle);
        MaterialManager::Assign(multi, 1, copyHandle);
        Check(MaterialManager::SlotHandle(entity.Get<MaterialComponent>(), 0) == handle &&
            MaterialManager::SlotHandle(entity.Get<MaterialComponent>(), 1) == copyHandle, "slot assignment is isolated");
        auto baseline = World::CreateEntity().Add<TransformComponent>().Add<StaticModelComponent>();
        baseline.Get<StaticModelComponent>().ModelId = entity.Get<StaticModelComponent>().ModelId;
        baseline.Get<TransformComponent>().Position = {0, 200, 0};
        Check(MaterialManager::HasSurfaceForPass(baseline.GetID(), false), "model without MaterialComponent keeps its default material");
        MaterialManager::Assign(multi, 1, 0);
        entity.Get<MaterialComponent>().IsTransparent = true;
        entity.Get<MaterialComponent>().Alpha = 0.4f;
        Check(MaterialManager::HasSurfaceForPass(multi, false) && MaterialManager::HasSurfaceForPass(multi, true),
            "mixed opaque asset and transparent legacy slots route to both passes");
        Check(!GPUDrivenRenderer::SupportsEntity(multi), "mixed transparent model uses per-section CPU fallback");
        Raytracing::GetSettings().Enabled = false;
        DayNightCycle::GetSettings().Running = false;
        RenderConfiguration::SetRenderMode(RenderMode::DEFERRED);
        RendererSettings::SetComputeGBufferEnabled(false);
        RendererSettings::SetMeshShadersEnabled(false);
        RendererSettings::SetSsaoEnabled(false);
        RendererSettings::SetSsgiEnabled(false);
        GPUVisibility::GetSettings().Occlusion = false;
        GPUVisibility::GetSettings().LOD = false;
        GPUDrivenRenderer::SetEnabled(false);
        auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(Camera::GetCameraEntity());
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(Camera::GetCameraEntity());
        transform.Position = {0, 0, -7};
        transform.IsDirty = true;
        camera.Target = {0, 0, 0};
        camera.AllowUserControl = false;
        camera.LockOnTarget = g_kINVALID_ENTITY;
    }
    if (frame == 2)
    {
        auto editing = MaterialManager::GetRuntime(handle).Editing;
        const auto before = MaterialManager::GetRuntime(handle).Revision;
        editing.Roughness = 0.27f;
        Check(MaterialManager::SetEditing(handle, editing), "real-time parameter edit");
        Check(MaterialManager::GetRuntime(handle).Revision == before + 1 && MaterialManager::GetRuntime(handle).Dirty, "dirty state and runtime revision");
        Check(MaterialManager::Resolve(ComponentManager::GetComponentUnchecked<MaterialComponent>(second)).Roughness == 0.27f, "edit reflected in every shared entity");
        Check(MaterialManager::GetRuntime(copyHandle).Editing.Roughness == 0.7f, "duplicate remains independent");
        Check(MaterialManager::Save(handle) && !MaterialManager::GetRuntime(handle).Dirty, "Save clears dirty state");
        editing.Roughness = 0.9f;
        MaterialManager::SetEditing(handle, editing);
        Check(MaterialManager::Revert(handle) && MaterialManager::GetRuntime(handle).Editing.Roughness == 0.27f, "Revert restores disk state and shared runtime");
    }
    if (frame == 4)
    {
        MaterialPreview::SetMesh(MaterialPreview::Mesh::Cube);
        GPUDrivenRenderer::SetEnabled(true);
        GPUVisibility::GetSettings().LOD = true;
        GPUVisibility::GetSettings().ForceLOD = 0;
    }
    if (frame == 6)
    {
        MaterialPreview::SetMesh(MaterialPreview::Mesh::Plane);
        auto editing = MaterialManager::GetRuntime(handle).Editing;
        editing.NormalScale = 0;
        MaterialManager::SetEditing(handle, editing);
    }
    if (frame == 8)
    {
        error_code ignored;
        filesystem::remove(MaterialManager::Registry().Resolve(handle), ignored);
        MaterialManager::Refresh();
        const auto& fallback = MaterialManager::Resolve(ComponentManager::GetComponentUnchecked<MaterialComponent>(first));
        Check(fallback.Roughness == 0.5f && fallback.NormalMapID < 0, "missing material uses default runtime");
        MaterialManager::Discard(handle);
        Check(!MaterialManager::GetRuntime(handle).Dirty, "missing dirty material can be discarded");
    }
    if (frame == 9)
    {
        ImGuiManager::m_ShowMaterialEditorWindow = false;
        const auto identity = ComponentManager::GetComponentUnchecked<MaterialComponent>(first).MaterialSceneIdentity;
        World::DestroyEntity(Entity(first));
        auto unrelated = World::CreateEntity().Add<TransformComponent>().Add<MeshComponent>().Add<MaterialComponent>().Add<NameComponent>();
        unrelated.SetName("Unrelated fixture");
        unrelated.Get<TransformComponent>().Position = {0, 300, 0};
        VertexResource unrelatedMesh;
        unrelatedMesh.entityid = unrelated.GetID();
        unrelatedMesh.objectType = ObjectType::SPHERE;
        unrelatedMesh.color = Color::WHITE;
        PrimitiveGeometry::CreateObjectVertex(unrelatedMesh);
        string error;
        Check(MaterialManager::LoadSceneReferences("build/material-tests/scene.json", error), "restore scene references after entity destruction");
        bool restored = false;
        for (EntityID entity : World::GetView<MaterialComponent>())
        {
            const auto& component = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
            if (component.MaterialSceneIdentity == identity)
            {
                restored = MaterialManager::IsRenderable(entity) && MaterialManager::SlotHandle(component, 0) == handle;
            }
        }
        Check(restored, "saved primitive geometry and stable material reference recreate an entity");
        Check(unrelated.Get<NameComponent>().Name == "Unrelated fixture" &&
            unrelated.Get<MaterialComponent>().MaterialSlots.empty(), "scene restore never overwrites an unrelated reused entity ID");
        ofstream("build/material-tests/bad-scene.json") <<
            "{\"version\":1,\"entities\":[{\"entity\":0,\"materialSlots\":[\"bad-guid\"]}]}";
        Check(!MaterialManager::LoadSceneReferences("build/material-tests/bad-scene.json", error) &&
            MaterialManager::SlotHandle(ComponentManager::GetComponentUnchecked<MaterialComponent>(second), 0) == handle,
            "invalid scene GUID is rejected before altering assignments");
    }
    if (frame == 12)
    {
        ImGuiManager::m_SelectedEntity = second;
        MaterialManager::Assign(second, 0, 0);
    }
    if (frame == 15)
    {
        ImGuiManager::OpenMaterialAsset(copyHandle);
        auto editing = MaterialManager::GetRuntime(copyHandle).Editing;
        editing.Textures[1] = 0;
        MaterialManager::SetEditing(copyHandle, editing);
    }
    if (frame == 18)
    {
        error_code ignored;
        filesystem::remove(MaterialManager::Registry().Resolve(surfaceTextureHandle), ignored);
        MaterialManager::Refresh();
        const auto& binding = MaterialManager::GetRuntime(copyHandle).Binding;
        Check(!binding.UseTexture && binding.MetallicRoughnessID < 0 && binding.AOTextureID < 0 && binding.EmissiveTextureID < 0,
            "missing textures safely use all slot fallbacks");
        auto editing = MaterialManager::GetRuntime(copyHandle).Editing;
        editing.Roughness = 0.42f;
        MaterialManager::SetEditing(copyHandle, editing);
    }
    if (frame == 20)
    {
        auto editing = MaterialManager::GetRuntime(copyHandle).Editing;
        editing.Roughness = 0.82f;
        MaterialManager::SetEditing(copyHandle, editing);
    }
}

void MaterialValidation::AfterFrame(uint64_t frame)
{
    CheckTransparencyFrame(frame);
    const auto* firstInstance = GpuScene::GetInstance(GpuScene::GetEntityInstance(first));
    const auto* secondInstance = GpuScene::GetInstance(GpuScene::GetEntityInstance(second));
    if (frame == 1)
    {
        Check(firstInstance && secondInstance && firstInstance->MaterialIndex == secondInstance->MaterialIndex, "shared GPU material index");
        if (firstInstance)
        {
            gpuIndex = firstInstance->MaterialIndex;
        }
    }
    if (frame == 1)
    {
        constantEditPipelineCreations = PsoManager::GraphicsPipelineCreations();
    }
    if (frame == 3)
    {
        Check(firstInstance && firstInstance->MaterialIndex == gpuIndex, "constant edit updates GPU record in place");
        vector<uint8_t> records;
        Check(RenderCapture::ReadBuffer(GpuScene::MaterialBuffer(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            uint64_t(GpuScene::Statistics().Materials) * sizeof(GPUMaterialData), records), "read shared GPU material");
        if (gpuIndex != UINT32_MAX && records.size() >= (uint64_t(gpuIndex) + 1) * sizeof(GPUMaterialData))
        {
            GPUMaterialData material;
            memcpy(&material, records.data() + gpuIndex * sizeof(GPUMaterialData), sizeof(material));
            Check(material.Surface.y == 0.27f && material.NormalTextureIndex != UINT32_MAX, "GPU record contains updated roughness and normal");
        }
        for (uint32_t target = 0; target < reference.size(); ++target)
        {
            Check(RenderCapture::ReadTexture(RenderTargets::GetGBufferResource(static_cast<GBufferType>(target)), reference[target]), "capture CPU GBuffer");
        }
        Check(PsoManager::GraphicsPipelineCreations() == constantEditPipelineCreations,
            "constant edit does not create graphics pipelines");
        Check(GpuScene::Statistics().InstanceUploadedBytes == 0, "shared constant edit uploads no entity instances");
        withNormal = reference[1];
        bool verifiedSurfaceChannels = false;
        const auto& materialTexture = reference[3];
        const auto& emissionTexture = reference[5];
        for (UINT row = 0; row < materialTexture.Height && !verifiedSurfaceChannels; ++row)
        {
            for (UINT column = 0; column < materialTexture.Width; ++column)
            {
                const auto* materialPixel = reinterpret_cast<const uint16_t*>(materialTexture.Bytes.data() + row * materialTexture.RowPitch + column * 8);
                if (RenderCapture::HalfToFloat(materialPixel[3]) == static_cast<float>(ShaderClass::PBR))
                {
                    const auto* emissionPixel = reinterpret_cast<const uint16_t*>(emissionTexture.Bytes.data() + row * emissionTexture.RowPitch + column * 8);
                    const float expectedMetallic = 0.6f * (64.0f / 255.0f);
                    const float expectedRoughness = 0.27f * (128.0f / 255.0f);
                    const float expectedAO = 0.2f + 0.8f * (64.0f / 255.0f);
                    Check(abs(RenderCapture::HalfToFloat(materialPixel[0]) - expectedMetallic) < 0.001f &&
                        abs(RenderCapture::HalfToFloat(materialPixel[1]) - expectedRoughness) < 0.001f,
                        "metallic R and roughness G channels modulate scalar parameters");
                    Check(abs(RenderCapture::HalfToFloat(emissionPixel[3]) - expectedAO) < 0.001f &&
                        RenderCapture::HalfToFloat(emissionPixel[0]) > 0.015f, "AO strength and textured emission reach production GBuffer");
                    verifiedSurfaceChannels = true;
                    break;
                }
            }
        }
        Check(verifiedSurfaceChannels, "material texture validation covers rendered pixels");
        RenderCapture::SaveScenePng(L"build/material-tests/cpu.png");
        Check(MaterialPreview::GetTarget() && RenderCapture::SaveTexturePng(MaterialPreview::GetTarget(),
            L"build/material-tests/preview.png"), "production material preview renders");
    }
    if (frame == 5)
    {
        Check(GPUDrivenRenderer::HandlesEntity(first), "GPULOD owns material asset entity");
        for (uint32_t target = 0; target < reference.size(); ++target)
        {
            RenderCapture::TextureData gpu;
            Check(RenderCapture::ReadTexture(RenderTargets::GetGBufferResource(static_cast<GBufferType>(target)), gpu), "capture GPU GBuffer");
            UINT pixelBytes = 4;
            if (target == 3 || target == 5)
            {
                pixelBytes = 8;
            }
            Check(EqualPixels(reference[target], gpu, pixelBytes), "GPULOD ON/OFF material GBuffer parity");
        }
        RenderCapture::SaveScenePng(L"build/material-tests/gpulod.png");
        Check(RenderCapture::SaveTexturePng(MaterialPreview::GetTarget(), L"build/material-tests/preview-cube.png"), "preview Cube uses production material");
    }
    if (frame == 7)
    {
        Check(RenderCapture::SaveTexturePng(MaterialPreview::GetTarget(), L"build/material-tests/preview-plane.png"), "preview Plane uses production material");
        RenderCapture::TextureData flat;
        Check(RenderCapture::ReadTexture(RenderTargets::GetGBufferResource(GBufferType::NORMAL), flat) &&
            !EqualPixels(withNormal, flat, 4), "normal texture visibly changes GBuffer normals");
    }
    if (frame == 11)
    {
        Check(MaterialManager::SlotHandle(ComponentManager::GetComponentUnchecked<MaterialComponent>(second), 0) == copyHandle,
            "Viewport UI accepts material drag/drop on picked entity");
    }
    if (frame == 14)
    {
        Check(MaterialManager::SlotHandle(ComponentManager::GetComponentUnchecked<MaterialComponent>(second), 0) == copyHandle,
            "Inspector UI accepts material drag/drop on selected slot");
    }
    if (frame == 17)
    {
        Check(MaterialManager::GetRuntime(copyHandle).Editing.Textures[1] == normalTextureHandle,
            "Material Editor UI accepts texture drag/drop");
    }
    if (frame == 19)
    {
        MaterialAssetData saved;
        string error;
        Check(!MaterialManager::GetRuntime(copyHandle).Dirty &&
            MaterialSerializer::Load(MaterialManager::Registry().Resolve(copyHandle), saved, error) && saved.Roughness == 0.42f,
            "Ctrl+S saves the focused material editor to disk");
    }
    if (frame == 23)
    {
        Check(!MaterialManager::GetRuntime(copyHandle).Dirty && MaterialManager::GetRuntime(copyHandle).Editing.Roughness == 0.42f,
            "Revert toolbar discards unsaved changes");
    }
}

void MaterialValidation::CaptureDropTarget(const char* name, float left, float top, float right, float bottom)
{
    if (!Enabled())
    {
        return;
    }
    const XMFLOAT2 center{(left + right) * 0.5f, (top + bottom) * 0.5f};
    if (strcmp(name, "Inspector") == 0)
    {
        inspectorDrop = center;
    }
    else if (strcmp(name, "Normal") == 0)
    {
        normalDrop = center;
    }
    else if (strcmp(name, "Revert") == 0)
    {
        revertButton = center;
    }
}

void MaterialValidation::BeforeEditorFrame()
{
    if (!Enabled())
    {
        return;
    }
    injectDrag = false;
    const auto frame = frames - 1;
    if (frame == 18 || frame == 19)
    {
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, frame == 18);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_S, frame == 18);
        return;
    }
    if (frame == 21 || frame == 22)
    {
        ImGui::GetIO().AddMousePosEvent(revertButton.x, revertButton.y);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, frame == 21);
        return;
    }
    XMFLOAT2 mouse{};
    if (frame == 10 || frame == 11)
    {
        XMMATRIX view;
        XMMATRIX projection;
        Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
        const auto& position = ComponentManager::GetComponentUnchecked<TransformComponent>(second).Position;
        XMFLOAT3 projected;
        XMStoreFloat3(&projected, XMVector3Project(XMLoadFloat3(&position),
            ImGuiManager::m_SceneViewPos.x, ImGuiManager::m_SceneViewPos.y,
            ImGuiManager::m_SceneViewSize.x, ImGuiManager::m_SceneViewSize.y, 0, 1,
            projection, view, XMMatrixIdentity()));
        mouse = {projected.x, projected.y};
        uiPayload = {copyHandle, AssetType::Material};
    }
    else if (frame == 13 || frame == 14)
    {
        mouse = inspectorDrop;
        uiPayload = {copyHandle, AssetType::Material};
    }
    else if (frame == 16 || frame == 17)
    {
        mouse = normalDrop;
        uiPayload = {normalTextureHandle, AssetType::Texture};
    }
    else
    {
        return;
    }
    const bool held = frame == 10 || frame == 13 || frame == 16;
    auto& input = ImGui::GetIO();
    input.AddMousePosEvent(mouse.x, mouse.y);
    input.AddMouseButtonEvent(ImGuiMouseButton_Left, held);
    injectDrag = held;
    WriteGraphicsLog("Material UI trace: frame=%llu mouse=%.1f,%.1f held=%d type=%u\n",
        frame, mouse.x, mouse.y, held, static_cast<unsigned>(uiPayload.Type));
}

void MaterialValidation::InjectDragPayload()
{
    if (Enabled() && injectDrag && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern))
    {
        ImGui::SetDragDropPayload(AssetPayloadType, &uiPayload, sizeof(uiPayload));
        ImGui::TextUnformatted("Material validation drag source");
        ImGui::EndDragDropSource();
    }
}

bool MaterialValidation::Finish()
{
    GraphicsDevice::WaitForGpuIdle();
    Check(frames >= 24, "complete material validation sequence");
    failures += RenderCapture::ReportGraphicsErrors();
    for (const auto& path : ownedFiles)
    {
        error_code ignored;
        filesystem::remove(path, ignored);
    }
    error_code ignored;
    filesystem::remove(fixtureRoot, ignored);
    WriteGraphicsLog("Material validation failures = %u\n", failures);
    return failures == 0;
}
