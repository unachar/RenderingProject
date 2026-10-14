#include "pch.h"
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
std::filesystem::path fixtureRoot;
std::vector<std::filesystem::path> ownedFiles;
std::array<RenderCapture::TextureData, 6> reference;
RenderCapture::TextureData withNormal;
XMFLOAT2 inspectorDrop{};
XMFLOAT2 normalDrop{};
XMFLOAT2 revertButton{};
bool injectDrag = false;
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

void MakeNormalTexture(const std::filesystem::path& path)
{
    std::array<uint8_t, 18> header{};
    header[2] = 2;
    header[12] = 16;
    header[14] = 16;
    header[16] = 32;
    header[17] = 0x20;
    std::ofstream stream(path, std::ios::binary);
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

void MakeSurfaceTexture(const std::filesystem::path& path)
{
    std::array<uint8_t, 18> header{};
    header[2] = 2;
    header[12] = 4;
    header[14] = 4;
    header[16] = 32;
    header[17] = 0x20;
    std::ofstream stream(path, std::ios::binary);
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
}

bool MaterialValidation::Enabled()
{
    char value[8]{};
    return GetEnvironmentVariableA("DX12_MATERIAL_ASSET_TEST", value, sizeof(value)) > 0 && atoi(value) != 0;
}

void MaterialValidation::BeforeFrame(uint64_t frame)
{
    frames = frame + 1;
    if (frame == 0)
    {
        RenderProfiler::SetWindowVisible(false);
        ImGuiManager::m_ShowAdjustmentPanel = false;
        ImGuiManager::m_ProjectSettingsLoaded = true;
        std::vector<EntityID> remove;
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
        fixtureRoot = std::filesystem::path("asset") / ("__material_validation_" + std::to_string(GetCurrentProcessId()));
        std::filesystem::create_directories(fixtureRoot);
        std::filesystem::create_directories("build/material-tests");
        const auto normal = fixtureRoot / "Normal.tga";
        MakeNormalTexture(normal);
        ownedFiles.push_back(normal);
        ownedFiles.emplace_back(normal.wstring() + L".meta");
        std::string error;
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
        std::ofstream(meshPath) << "mtllib sections.mtl\nv -1 -1 0\nv 0 1 0\nv 1 -1 0\nv 2 -1 0\nv 3 1 0\nv 4 -1 0\nusemtl One\nf 1 2 3\nusemtl Two\nf 4 5 6\n";
        std::ofstream(mtlPath) << "newmtl One\nKd 1 1 1\nnewmtl Two\nKd 0.5 1 1\n";
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
        std::error_code ignored;
        std::filesystem::remove(MaterialManager::Registry().Resolve(handle), ignored);
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
        std::string error;
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
        std::ofstream("build/material-tests/bad-scene.json") <<
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
        std::error_code ignored;
        std::filesystem::remove(MaterialManager::Registry().Resolve(surfaceTextureHandle), ignored);
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
        std::vector<uint8_t> records;
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
                    Check(std::abs(RenderCapture::HalfToFloat(materialPixel[0]) - expectedMetallic) < 0.001f &&
                        std::abs(RenderCapture::HalfToFloat(materialPixel[1]) - expectedRoughness) < 0.001f,
                        "metallic R and roughness G channels modulate scalar parameters");
                    Check(std::abs(RenderCapture::HalfToFloat(emissionPixel[3]) - expectedAO) < 0.001f &&
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
        std::string error;
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
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    std::error_code ignored;
    std::filesystem::remove(fixtureRoot, ignored);
    WriteGraphicsLog("Material validation failures = %u\n", failures);
    return failures == 0;
}
