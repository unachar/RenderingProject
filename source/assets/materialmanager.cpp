#include "pch.h"
#include "materialmanager.h"
#include "editorvisibility.h"
#include "assetthumbnailmanager.h"
#include "texturemanager.h"
#include "noiseasset.h"
#include <cstring>
#include "modelmanager.h"
#include "world.h"
#include "materialsystem.h"
#include "graphicslog.h"
#include "gpuscene/gpuscenechanges.h"
#include <json/json.h>
#include <fstream>
#include <chrono>
#include "materialgraph.h"
#include "materialgraphcompiler.h"
#include "materialgraphlower.h"
#include "frameconstants.h"
#include "../audio/audiospectrum.h"

namespace
{
    unordered_map<AssetHandle, unique_ptr<MaterialRuntime>> runtimes;
    AssetRegistry registry;
    bool sceneHasAssets = false;
}

bool SceneHasMaterialAssets()
{
    return sceneHasAssets;
}

AssetRegistry& MaterialManager::Registry()
{
    return registry;
}

void MaterialManager::Init()
{
    Refresh();
}

void MaterialManager::Shutdown()
{
    runtimes.clear();
    sceneHasAssets = false;
}

void MaterialManager::PrepareStartupAssets()
{
    // Decode independent textures concurrently and submit their uploads as one batch.
    const auto handles = registry.HandlesOfType(AssetType::Material);
    {
        TextureManager::ScopedBatch batch;
        for (const auto handle : handles)
        {
            GetRuntime(handle);
        }
        UpdateGraphPrograms();
    }
    WriteGraphicsLog("Startup materials: prepared %zu materials and their texture dependencies\n", handles.size());
}

void MaterialManager::Refresh()
{
    string error;
    const bool refreshed = registry.Refresh(error);
    WriteGraphicsLog("Asset registry: root=%s, refreshed=%d\n", registry.Root().generic_string().c_str(), refreshed);
    if (!error.empty())
    {
        WriteGraphicsLog("Asset registry: %s\n", error.c_str());
    }
    for (auto& [handle, runtime] : runtimes)
    {
        const bool missing = registry.Resolve(handle).empty();
        if (missing != runtime->Missing)
        {
            runtime->Missing = missing;
            if (missing)
            {
                runtime->Error = "Missing material: " + FormatAssetHandle(handle);
                UpdateBinding(*runtime, MaterialAssetData{});
            }
            else
            {
                Revert(handle);
            }
        }
        // テクスチャの削除や移動では、保存済み GUID を変更せずにバインドを更新する。
        if (!missing)
        {
            UpdateBinding(*runtime, runtime->Editing);
        }
    }
}

MaterialRuntime& MaterialManager::GetRuntime(AssetHandle handle)
{
    const auto found = runtimes.find(handle);
    if (found != runtimes.end())
    {
        return *found->second;
    }
    auto runtime = make_unique<MaterialRuntime>();
    runtime->Handle = handle;
    const auto path = registry.Resolve(handle);
    if (handle != 0 && !path.empty() && registry.TypeOf(handle) == AssetType::Material)
    {
        MaterialSerializer::Load(path, runtime->Saved, runtime->Error);
    }
    else if (handle != 0)
    {
        runtime->Missing = true;
        runtime->Error = "Missing material: " + FormatAssetHandle(handle);
    }
    runtime->Editing = runtime->Saved;
    UpdateBinding(*runtime, runtime->Editing);
    auto& result = *runtime;
    runtimes.emplace(handle, move(runtime));
    return result;
}

const MaterialRuntime* MaterialManager::FindRuntime(AssetHandle handle)
{
    const auto found = runtimes.find(handle);
    return found == runtimes.end() ? nullptr : found->second.get();
}

MaterialComponent MaterialManager::BuildPreviewBinding(
    const MaterialAssetData& material, const array<int, static_cast<size_t>(MaterialTextureSlot::Count)>& descriptors)
{
    MaterialComponent binding;
    binding.ShaderClassMode = MaterialMode::Manual;
    binding.ShaderClass = ShaderClass::PBR;
    binding.BaseColor = XMFLOAT4(material.BaseColor.data());
    binding.Metallic = material.Metallic;
    binding.Roughness = material.Roughness;
    binding.ParallaxDepth = material.ParallaxDepth;
    binding.ParallaxSteps = material.ParallaxSteps;
    binding.NormalScale = material.NormalScale;
    binding.AOStrength = material.AOStrength;
    binding.EmissiveColor = XMFLOAT3(material.EmissiveColor.data());
    binding.EmissiveIntensity = material.EmissiveIntensity;
    // 旧アセットが Opaque を保存していても、光学的な透過があれば透明描画経路を選ぶ。
    // アルファ値は独立した被覆率として扱う。
    const bool opticalTransmission = material.Transmission > 0.0f;
    binding.AssetSurface = opticalTransmission ? MaterialSurface::Transparent : material.Surface;
    binding.IsTransparent = binding.AssetSurface == MaterialSurface::Transparent;
    binding.AssetCull = material.Cull;
    binding.AssetDepthWrite = binding.IsTransparent ? false : material.DepthWrite;
    binding.AlphaCutoff = material.AlphaCutoff;
    binding.ShadowMode = material.ShadowMode;
    binding.ShadowCutoff = material.ShadowCutoff;
    binding.IOR = material.IOR;
    binding.Transmission = material.Transmission;
    binding.TransmissionRoughness = material.TransmissionRoughness;
    const float dielectric = (material.IOR - 1.0f) / (material.IOR + 1.0f);
    binding.Fresnel = dielectric * dielectric;
    binding.Thickness = material.Thickness;
    binding.ShadingModel = material.ShadingModel;
    binding.ShadingStrength = material.ShadingStrength;
    binding.AbsorptionCoefficient = {material.AbsorptionCoefficient[0], material.AbsorptionCoefficient[1],
                                     material.AbsorptionCoefficient[2]};
    binding.DoubleSided = material.Cull == MaterialCull::None;
    binding.BaseSaturation = 1.0f;
    binding.TextureID = descriptors[0];
    binding.UseTexture = binding.TextureID >= 0;
    binding.NormalMapID = descriptors[1];
    binding.MetallicRoughnessID = descriptors[2];
    binding.AOTextureID = descriptors[3];
    binding.EmissiveTextureID = descriptors[4];
    binding.HeightTextureID = descriptors[5];
    return binding;
}

void MaterialManager::UpdateBinding(MaterialRuntime& runtime, const MaterialAssetData& material)
{
    array<int, static_cast<size_t>(MaterialTextureSlot::Count)> descriptors;
    descriptors.fill(-1);
    string sourceError;
    const auto noise = runtime.NoiseSources.Update(registry, material, sourceError);
    for (size_t slot = 0; slot < descriptors.size(); ++slot)
    {
        const auto path = registry.Resolve(material.Textures[slot]);
        if (!path.empty() && registry.TypeOf(material.Textures[slot]) == AssetType::Texture)
        {
            const auto previousHandle = runtime.BoundData.Textures[slot];
            const auto previousPath = runtime.TexturePaths[slot];
            if (previousHandle == material.Textures[slot] && previousPath == path &&
                runtime.TextureDescriptors[slot] >= 0)
            {
                descriptors[slot] = runtime.TextureDescriptors[slot];
            }
            else if (slot == static_cast<size_t>(MaterialTextureSlot::Normal))
            {
                descriptors[slot] = TextureManager::LoadNormalTexture(path.generic_string().c_str());
            }
            else
            {
                // シェーダーが参照するチャンネルに合わせて BC 形式を選ぶ。
                // MetallicRoughness は .rg（BC5）、AO と高さは .r（BC4）、ベースカラーとエミッシブは BC7。
                TextureUsage usage = TextureUsage::Color;
                if (slot == static_cast<size_t>(MaterialTextureSlot::MetallicRoughness))
                {
                    usage = TextureUsage::TwoChannel;
                }
                else if (slot == static_cast<size_t>(MaterialTextureSlot::AO) ||
                         slot == static_cast<size_t>(MaterialTextureSlot::Height))
                {
                    usage = TextureUsage::SingleChannel;
                }
                descriptors[slot] = TextureManager::LoadTexture(path, usage);
            }
            runtime.TexturePaths[slot] = path;
        }
        else
        {
            runtime.TexturePaths[slot].clear();
        }
    }
    auto binding = BuildPreviewBinding(material, descriptors);
    binding.RuntimeHandle = runtime.Handle;
    binding.GraphProgramID = runtime.GraphPublished ? runtime.GraphProgramSrv : -1;
    binding.Noise = noise;
    runtime.SourceError = sourceError;
    // リビジョン変更時は、共有マテリアルレコードとそのフレーム定数バッファだけを無効化する。
    if (runtime.Revision == 0 || runtime.Binding.BaseColor.x != binding.BaseColor.x || runtime.BoundData != material ||
        runtime.TextureDescriptors != descriptors ||
        std::memcmp(runtime.Binding.Noise.data(), noise.data(), sizeof(noise)) != 0)
    {
        if (runtime.Revision == 0 || runtime.BoundData.Cull != material.Cull ||
            runtime.BoundData.Surface != material.Surface || runtime.BoundData.DepthWrite != material.DepthWrite ||
            runtime.Binding.IsTransparent != binding.IsTransparent ||
            (material.Surface == MaterialSurface::Masked && (runtime.BoundData.AlphaCutoff != material.AlphaCutoff ||
                                                             runtime.BoundData.BaseColor[3] != material.BaseColor[3] ||
                                                             runtime.BoundData.Textures[0] != material.Textures[0] ||
                                                             runtime.TextureDescriptors[0] != descriptors[0])))
        {
            ++runtime.PipelineRevision;
        }
        runtime.Binding = move(binding);
        runtime.BoundData = material;
        runtime.TextureDescriptors = descriptors;
        ++runtime.Revision;
    }
}

// ---------------------------------------------------------------------------------------------
// マテリアルグラフの描画時評価
// ---------------------------------------------------------------------------------------------
namespace
{
    void DisableGraphProgram(MaterialRuntime& runtime)
    {
        runtime.GraphActive = false;
        runtime.GraphProgramHash = 0;
        runtime.GraphTexels.clear();
        runtime.GraphUsesView = false;
        runtime.GraphEmissive = false;
        if (runtime.GraphPublished)
        {
            runtime.GraphPublished = false;
            runtime.Binding.GraphProgramID = -1;
            ++runtime.Revision; // マテリアル定数（AssetTextures.w）を作り直す
        }
    }
}

namespace
{
    // Material Function（.matfunc）を、アセットのハンドルから読む。MaterialGraphLower が展開するときに呼ぶ
    void EnsureFunctionLoader()
    {
        static bool registered = false;
        if (registered)
        {
            return;
        }
        registered = true;
        MaterialFunctionLibrary::SetLoader([](AssetHandle handle) -> std::shared_ptr<const MaterialGraph> {
            if (MaterialManager::Registry().TypeOf(handle) != AssetType::MaterialFunction)
            {
                return nullptr;
            }
            const auto path = MaterialManager::Registry().Resolve(handle);
            auto graph = std::make_shared<MaterialGraph>();
            std::string error;
            if (path.empty() || !MaterialGraph::Load(path, *graph, error) || !graph->Settings.IsFunction)
            {
                return nullptr;
            }
            return graph;
        });
    }
}

void MaterialManager::SetGraphProgram(AssetHandle handle, const MaterialGraph* sourceGraph)
{
    if (handle == 0)
    {
        return;
    }
    EnsureFunctionLoader();
    auto& runtime = GetRuntime(handle);
    // マテリアルインスタンスの上書き（Texture Parameter 2D）を、保存しない実行時の値としてグラフへ渡す
    MaterialGraph instanceGraph;
    const MaterialGraph* graph = sourceGraph;
    if (sourceGraph)
    {
        instanceGraph = *sourceGraph;
        instanceGraph.InstanceTextures = runtime.Editing.TextureOverrides;
        graph = &instanceGraph;
    }
    runtime.GraphChecked = true;
    if (!graph)
    {
        runtime.GraphStatus.clear();
        DisableGraphProgram(runtime);
        return;
    }
    if (!graph->Settings.RuntimeEvaluation)
    {
        runtime.GraphStatus = "描画時評価はオフです（ベイクした値を使います）";
        DisableGraphProgram(runtime);
        return;
    }
    const auto program = MaterialGraphVm::Compile(*graph, &MaterialManager::ResolveGraphTexture);
    if (!program.Success)
    {
        runtime.GraphStatus = program.Error;
        DisableGraphProgram(runtime);
        return;
    }
    runtime.GraphStatus.clear();
    runtime.GraphInstructions = program.Instructions;
    runtime.GraphRegisters = program.Registers;
    runtime.GraphUsesSceneDepth = program.UsesSceneDepth;
    runtime.GraphUsesSceneColor = program.UsesSceneColor;
    runtime.GraphPerLightToon = program.PerLightToon;
    runtime.GraphUsesDynamics = program.UsesTime || !program.Dynamics.empty();
    runtime.GraphProgram = program;
    runtime.GraphDynamicState = {};
    // 発光の動的ベイク（GI / 反射）用：出力レジスタ [2].x = Emissive（-1 = 未接続）
    runtime.GraphUsesView = program.UsesView;
    runtime.GraphEmissive = program.Texels.size() > 2 && program.Texels[2][0] >= 0.0f;
    runtime.GraphTexels = program.Texels;
    if (runtime.GraphActive && runtime.GraphProgramHash == program.Hash)
    {
        return;
    }
    if (runtime.GraphProgramSrv < 0)
    {
        runtime.GraphProgramSrv =
            TextureManager::CreateDataTexture("MaterialGraphProgram", MaterialGraphVm::kMaxTexels);
        if (runtime.GraphProgramSrv < 0)
        {
            runtime.GraphStatus = "プログラム用のテクスチャを作れません（テクスチャ表が一杯です）";
            DisableGraphProgram(runtime);
            return;
        }
    }
    TextureManager::UpdateDataTexture(runtime.GraphProgramSrv, 0, program.Texels);
    runtime.GraphProgramHash = program.Hash;
    runtime.GraphHeader = program.Texels[0];
    runtime.GraphUsesTime = program.UsesTime;
    runtime.GraphActive = true;
}

float MaterialManager::GraphTime()
{
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
}

bool MaterialManager::GetDynamicEmissionSource(AssetHandle handle, DynamicEmissionSource& source)
{
    const auto* runtime = FindRuntime(handle);
    if (!runtime || !runtime->GraphActive || !runtime->GraphPublished || !runtime->GraphEmissive ||
        runtime->GraphTexels.empty() || !(runtime->GraphUsesView || runtime->GraphUsesTime))
    {
        return false;
    }
    source.Program = &runtime->GraphTexels;
    source.Hash = runtime->GraphProgramHash;
    source.UsesTime = runtime->GraphUsesTime;
    source.UsesView = runtime->GraphUsesView;
    return true;
}

void MaterialManager::UpdateGraphPrograms()
{
    const float time = GraphTime();
    // Material Function が保存された：保存済みグラフを読み直して、呼び出しているマテリアルのプログラムを作り直す
    static uint64_t functionRevision = MaterialFunctionLibrary::Revision();
    const uint64_t currentRevision = MaterialFunctionLibrary::Revision();
    if (currentRevision != functionRevision)
    {
        functionRevision = currentRevision;
        for (auto& [handle, runtimePointer] : runtimes)
        {
            runtimePointer->GraphChecked = false;
        }
    }
    for (auto& [handle, runtimePointer] : runtimes)
    {
        auto& runtime = *runtimePointer;
        if (!runtime.GraphChecked && !runtime.Missing && handle != 0)
        {
            // エディターで開いていないマテリアル：保存済みのグラフ（<マテリアル>.graph.json）を読む
            runtime.GraphChecked = true;
            auto path = registry.Resolve(handle);
            if (!path.empty())
            {
                path += ".graph.json";
                std::error_code error;
                if (std::filesystem::exists(path, error))
                {
                    MaterialGraph graph;
                    string loadError;
                    if (MaterialGraph::Load(path, graph, loadError))
                    {
                        SetGraphProgram(handle, &graph);
                    }
                    else
                    {
                        runtime.GraphStatus = loadError;
                    }
                }
            }
        }
        if (!runtime.GraphActive || runtime.GraphProgramSrv < 0)
        {
            continue;
        }
        if (!runtime.GraphPublished && TextureManager::IsDataTextureReady(runtime.GraphProgramSrv))
        {
            // プログラムの最初の書き込みが記録された後に、マテリアル定数へ反映する
            runtime.GraphPublished = true;
            runtime.Binding.GraphProgramID = runtime.GraphProgramSrv;
            ++runtime.Revision;
        }
        if (runtime.GraphUsesTime || !runtime.GraphProgram.Dynamics.empty())
        {
            // 時刻（ヘッダー）と動的テクセル（Beat Phase / Audio Spectrum / 音声クロック）を毎フレーム書く。
            // 音声クロックは XAudio2 の再生位置（AudioMaterial::Bridge）。音声が無いときは壁時計
            MaterialGraphVm::DynamicInputs inputs;
            const auto audio = AudioMaterial::Bridge::Query(static_cast<double>(time));
            inputs.AudioValid = audio.Valid;
            inputs.AudioTime = audio.Time;
            inputs.WallTime = static_cast<double>(time);
            inputs.Bands = audio.Valid ? audio.Bands.data() : nullptr;
            inputs.DeltaTime = std::clamp(time - runtime.GraphLastTime, 0.0f, 0.25f);
            runtime.GraphLastTime = time;
            if (runtime.GraphTexels.size() == runtime.GraphProgram.Texels.size())
            {
                const int count = MaterialGraphVm::UpdateDynamics(runtime.GraphProgram, runtime.GraphTexels, runtime.GraphDynamicState, inputs, time);
                if (count > 0)
                {
                    TextureManager::UpdateDataTexture(
                        runtime.GraphProgramSrv, 0,
                        std::vector<std::array<float, 4>>(runtime.GraphTexels.begin(), runtime.GraphTexels.begin() + count));
                }
            }
        }
    }
}

int MaterialManager::ResolveGraphTexture(AssetHandle texture, MaterialSamplerType sampler)
{
    const auto path = registry.Resolve(texture);
    if (path.empty() || registry.TypeOf(texture) != AssetType::Texture)
    {
        return -1;
    }
    // エンジンのベースカラーと同じく、格納値をそのまま読む（Normal は法線マップとして読み込む）
    const int srv = sampler == MaterialSamplerType::Normal
                        ? TextureManager::LoadNormalTexture(path.generic_string().c_str())
                        : TextureManager::LoadTexture(path, TextureUsage::Color);
    const int index = srv - static_cast<int>(FrameConstants::g_kTEXTURE_SRV_START_INDEX);
    return index >= 0 && index < 512 ? index : -1;
}

bool MaterialManager::IsGraphProgramActive(AssetHandle handle)
{
    const auto* runtime = FindRuntime(handle);
    return runtime && runtime->GraphActive;
}

AssetHandle MaterialManager::SlotHandle(const MaterialComponent& material, size_t slot)
{
    if (slot >= material.MaterialSlots.size())
    {
        return 0;
    }
    return material.MaterialSlots[slot];
}

const MaterialComponent& MaterialManager::Resolve(const MaterialComponent& legacy, size_t slot)
{
    const auto handle = SlotHandle(legacy, slot);
    if (handle == 0)
    {
        return legacy;
    }
    return GetRuntime(handle).Binding;
}

bool MaterialManager::SetEditing(AssetHandle handle, const MaterialAssetData& editing)
{
    auto& runtime = GetRuntime(handle);
    if (!MaterialSerializer::Validate(editing, runtime.Error) || runtime.Missing)
    {
        return false;
    }
    UpdateBinding(runtime, editing);
    runtime.Editing = editing;
    runtime.Dirty = runtime.Editing != runtime.Saved;
    AssetThumbnailManager::MarkDirty(handle);
    return true;
}

bool MaterialManager::Save(AssetHandle handle)
{
    auto& runtime = GetRuntime(handle);
    const auto path = registry.Resolve(handle);
    if (path.empty())
    {
        runtime.Error = "Cannot save a missing material";
        return false;
    }
    if (!MaterialSerializer::Save(path, runtime.Editing, runtime.Error))
    {
        return false;
    }
    runtime.Saved = runtime.Editing;
    runtime.Dirty = false;
    AssetThumbnailManager::MarkDirty(handle);
    return true;
}

bool MaterialManager::Revert(AssetHandle handle)
{
    auto& runtime = GetRuntime(handle);
    MaterialAssetData loaded;
    if (!MaterialSerializer::Load(registry.Resolve(handle), loaded, runtime.Error))
    {
        return false;
    }
    runtime.Saved = loaded;
    runtime.Missing = false;
    return SetEditing(handle, loaded);
}

bool MaterialManager::IsRenderable(EntityID entity)
{
    return ::Registry::IsAlive(entity) && ComponentManager::HasComponent<MaterialComponent>(entity) &&
           (ComponentManager::HasComponent<StaticModelComponent>(entity) ||
            ComponentManager::HasComponent<AnimationModelComponent>(entity) ||
            ComponentManager::HasComponent<MeshComponent>(entity) ||
            (ComponentManager::HasComponent<SpriteComponent>(entity) &&
             ComponentManager::GetComponentUnchecked<SpriteComponent>(entity).Is3D));
}

void MaterialManager::Discard(AssetHandle handle)
{
    auto& runtime = GetRuntime(handle);
    runtime.Editing = runtime.Saved;
    runtime.Dirty = false;
    if (!runtime.Missing)
    {
        UpdateBinding(runtime, runtime.Saved);
    }
}

bool MaterialManager::HasSurfaceForPass(EntityID entity, bool transparent)
{
    if (EditorVisibility::IsHidden(entity))
    {
        return false; // #5 アウトライナーで非表示
    }
    if (!ComponentManager::HasComponent<MaterialComponent>(entity))
    {
        return !transparent;
    }
    const auto& source = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
    const auto count = max<size_t>(1, SlotCount(entity));
    for (size_t slot = 0; slot < count; ++slot)
    {
        if (MaterialSystem::IsTransparentMaterial(Resolve(source, slot)) == transparent)
        {
            return true;
        }
    }
    return false;
}

size_t MaterialManager::SlotCount(EntityID entity)
{
    if (!IsRenderable(entity))
    {
        return 0;
    }
    if (ComponentManager::HasComponent<StaticModelComponent>(entity))
    {
        const auto* model =
            ModelManager::GetStaticModel(ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity).ModelId);
        if (model)
        {
            return model->GetMeshCount();
        }
    }
    if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
    {
        const auto* model = ModelManager::GetAnimModel(
            ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity).ModelId);
        if (model)
        {
            return model->GetMeshCount();
        }
    }
    return 1;
}

bool MaterialManager::Assign(EntityID entity, size_t slot, AssetHandle handle)
{
    if (!IsRenderable(entity) || slot >= SlotCount(entity) ||
        (handle != 0 && registry.TypeOf(handle) != AssetType::Material))
    {
        return false;
    }
    auto& component = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
    component.MaterialSlots.resize(SlotCount(entity));
    component.MaterialSlots[slot] = handle;
    if (all_of(component.MaterialSlots.begin(), component.MaterialSlots.end(), [](AssetHandle assigned) {
            return assigned == 0;
        }))
    {
        component.MaterialSlots.clear();
    }
    NotifyAssignmentsChanged();
    GpuSceneChanges::Notify(entity, GpuSceneDirtyFlags::Material);
    return true;
}

void MaterialManager::NotifyAssignmentsChanged()
{
    sceneHasAssets = false;
    for (EntityID entity : World::GetView<MaterialComponent>())
    {
        const auto& slots = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity).MaterialSlots;
        if (any_of(slots.begin(), slots.end(), [](AssetHandle handle) {
                return handle != 0;
            }))
        {
            sceneHasAssets = true;
            return;
        }
    }
}


