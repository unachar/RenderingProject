#include "pch.h"
#include "modelmanager.h"
#include "graphicsdevice.h"
#include "texturemanager.h"
#include "materialmanager.h"
#include "world.h"
#include "componentmanager.h"
#include "gpuscene/gpuscenechanges.h"
#include "meshlets/meshletmanager.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "meshshader/meshshaderrenderer.h"
#include "renderconfiguration.h"
#include "renderersettings.h"
#include <chrono>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_set>

namespace
{
    struct SourceWatch
    {
        std::filesystem::file_time_type Source{}, Settings{}, Lod{};
        bool Convert = true;
    };

    std::unordered_map<int, SourceWatch> sourceWatches;

    struct RetiredModel
    {
        uint64_t Fence = 0;
        std::unique_ptr<StaticModelResource> Model;
    };

    std::vector<RetiredModel> retiredModels;

    std::filesystem::file_time_type Stamp(const std::filesystem::path& path)
    {
        std::error_code ec;
        return std::filesystem::last_write_time(path, ec);
    }

    SourceWatch Watch(const std::string& path, bool convert)
    {
        return {Stamp(std::filesystem::u8path(path)), Stamp(std::filesystem::u8path(path + ".vgsettings")),
                Stamp(std::filesystem::u8path(path + ".lod")), convert};
    }
}

vector<unique_ptr<AnimationModelResource>> ModelManager::m_AnimModels;
vector<unique_ptr<StaticModelResource>> ModelManager::m_StaticModels;
unordered_map<string, int> ModelManager::m_AnimModelCache;
unordered_map<string, int> ModelManager::m_StaticModelCache;

string GetLowerExtension(const char* fileName)
{
    if (!fileName)
    {
        return {};
    }
    string extension = filesystem::path(fileName).extension().string();
    transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character) {
        return static_cast<char>(tolower(character));
    });
    return extension;
}

string MakeModelCacheKey(const char* fileName, bool isConvert)
{
    const char* sourcePath = fileName;
    if (!sourcePath)
    {
        sourcePath = "";
    }
    const char* conversionSuffix = "|raw";
    if (isConvert)
    {
        conversionSuffix = "|convert";
    }
    std::error_code pathError;
    const auto canonicalPath = std::filesystem::weakly_canonical(std::filesystem::u8path(sourcePath), pathError);
    std::string identity = sourcePath;
    if (!pathError)
    {
        const auto encodedPath = canonicalPath.generic_u8string();
        identity.assign(reinterpret_cast<const char*>(encodedPath.data()), encodedPath.size());
    }
    std::transform(identity.begin(), identity.end(), identity.begin(), [](unsigned char letter) {
        return static_cast<char>(std::tolower(letter));
    });
    return identity + conversionSuffix;
}

int ModelManager::LoadAnimModel(const char* fileName, bool isConvert)
{
    string key = MakeModelCacheKey(fileName, isConvert);
    auto it = m_AnimModelCache.find(key);
    if (it != m_AnimModelCache.end())
    {
        return it->second;
    }
    int id = LoadAnimModelDetailed(fileName, isConvert).ModelId;

    if (id >= 0)
    {
        m_AnimModelCache[key] = id;
    }
    return id;
}

int ModelManager::LoadStaticModel(const char* fileName, bool isConvert)
{
    string key = MakeModelCacheKey(fileName, isConvert);
    auto it = m_StaticModelCache.find(key);
    if (it != m_StaticModelCache.end())
    {
        return it->second;
    }
    int id = LoadStaticModelDetailed(fileName, isConvert).ModelId;
    if (id >= 0)
    {
        m_StaticModelCache[key] = id;
    }
    return id;
}

ModelManager::LoadModelResult ModelManager::LoadAnimModelDetailed(const char* fileName, bool isConvert)
{
    LoadModelResult result{};
    auto start = chrono::high_resolution_clock::now();

    TextureManager::BeginTextureLoading();
    auto model = make_unique<AnimationModelResource>();
    if (!model->Load(fileName, GraphicsDevice::GetDevice(), isConvert))
    {
        TextureManager::EndTextureLoading();
        auto end = chrono::high_resolution_clock::now();
        result.ElapsedMs = chrono::duration<double, milli>(end - start).count();
        result.Error = "Animation model load failed";
        Debug::Log("Anim model load failed: %s (%.2f ms)\n", fileName, result.ElapsedMs);
        return result;
    }

    result.ModelId = (int)m_AnimModels.size();
    m_AnimModels.push_back(move(model));
    TextureManager::EndTextureLoading();

    auto end = chrono::high_resolution_clock::now();
    result.ElapsedMs = chrono::duration<double, milli>(end - start).count();
    result.Succeeded = true;
    return result;
}

ModelManager::LoadModelResult ModelManager::LoadStaticModelDetailed(const char* fileName, bool isConvert)
{
    LoadModelResult result{};
    auto start = chrono::high_resolution_clock::now();

    TextureManager::BeginTextureLoading();
    auto model = make_unique<StaticModelResource>();
    const string ext = GetLowerExtension(fileName);
    bool loaded = false;
    auto derived = make_unique<StaticModelResource>();
    if (derived->LoadDerivedVirtualGeometry(fileName, GraphicsDevice::GetDevice(), isConvert))
    {
        model = std::move(derived);
        loaded = true;
    }
    else
    {
        if (ext != ".vgmesh" && ext != ".vgp2")
        {
            model->CaptureSourceIdentity(std::filesystem::u8path(fileName));
        }
    }
    if (!loaded)
    {
        if (ext == ".obj")
        {
            loaded = model->LoadObj(fileName, GraphicsDevice::GetDevice());
        }
        else if (ext == ".vgmesh" || ext == ".vgp2")
        {
            loaded = model->LoadVirtualGeometry(fileName, GraphicsDevice::GetDevice());
        }
        else if (ext == ".fbx" || ext == ".vrm" || ext == ".pmx" || ext == ".gltf" || ext == ".glb")
        {
            loaded = model->LoadAssimpModel(fileName, GraphicsDevice::GetDevice(), isConvert);
        }
        else
        {
            Debug::Log("ERROR: Unsupported static model extension: %s\n", fileName);
        }
    }

    if (!loaded)
    {
        TextureManager::EndTextureLoading();
        auto end = chrono::high_resolution_clock::now();
        result.ElapsedMs = chrono::duration<double, milli>(end - start).count();
        result.Error = "Static model load failed";
        Debug::Log("Static model load failed: %s (%.2f ms)\n", fileName, result.ElapsedMs);
        return result;
    }

    result.ModelId = (int)m_StaticModels.size();
    model->LoadLODMetadata(fileName);
    std::string registryError;
    MaterialManager::Registry().Register(std::filesystem::u8path(fileName), AssetType::Mesh, registryError);
    m_StaticModels.push_back(move(model));
    sourceWatches[result.ModelId] = Watch(fileName, isConvert);
    TextureManager::EndTextureLoading();

    auto end = chrono::high_resolution_clock::now();
    result.ElapsedMs = chrono::duration<double, milli>(end - start).count();
    result.Succeeded = true;
    return result;
}

bool ModelManager::LoadAnimation(int modelId, const char* fileName, const char* name)
{
    if (modelId >= 0 && modelId < (int)m_AnimModels.size())
    {
        return m_AnimModels[modelId]->LoadAnimation(fileName, name);
    }
    return false;
}

AnimationModelResource* ModelManager::GetAnimModel(int modelId)
{
    if (modelId >= 0 && modelId < (int)m_AnimModels.size())
    {
        return m_AnimModels[modelId].get();
    }
    return nullptr;
}

StaticModelResource* ModelManager::GetStaticModel(int modelId)
{
    if (modelId >= 0 && modelId < (int)m_StaticModels.size())
    {
        return m_StaticModels[modelId].get();
    }
    return nullptr;
}

void ModelManager::Uninit()
{
    for (auto& model : m_AnimModels)
    {
        model->Uninit();
    }
    m_AnimModels.clear();
    m_AnimModelCache.clear();

    for (auto& model : m_StaticModels)
    {
        model->Uninit();
    }
    m_StaticModels.clear();
    m_StaticModelCache.clear();
    sourceWatches.clear();
    retiredModels.clear();
}

bool ModelManager::SaveVirtualGeometrySettings(const std::filesystem::path& path, const VGImportSettings& settings,
                                               std::string& error)
{
    if (!VirtualGeometryImport::Save(path, settings, error))
    {
        return false;
    }
    for (auto& model : m_StaticModels)
    {
        std::error_code ec;
        if (std::filesystem::equivalent(path, std::filesystem::u8path(model->GetSourcePath()), ec) && !ec)
        {
            model->RebuildVirtualGeometry(settings);
        }
    }
    for (auto entity : World::GetView<StaticModelComponent>())
    {
        GpuSceneChanges::Notify(entity, GpuSceneDirtyFlags::Mesh);
    }
    return true;
}

void ModelManager::UpdateVirtualGeometry()
{
    const auto completed = GraphicsDevice::GetCompletedFence();
    std::erase_if(retiredModels, [completed](const auto& item) {
        return item.Fence <= completed;
    });
    static auto nextScan = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() >= nextScan)
    {
        nextScan = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        std::vector<int> reloads;
        for (const auto& [id, previous] : sourceWatches)
        {
            bool requiresOriginal = GetGeometryRenderingMode() == GeometryRenderingMode::Legacy ||
                                    !MeshShaderRenderer::Supported() ||
                                    RenderConfiguration::GetRenderMode() != RenderMode::DEFERRED ||
                                    RendererSettings::GetComputeGBufferEnabled();
            if (m_StaticModels.at(id)->UsesDerivedVirtualGeometry() && !requiresOriginal)
            {
                for (auto entity : World::GetView<StaticModelComponent>())
                {
                    if (ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity).ModelId == id &&
                        !GPUDrivenRenderer::SupportsEntity(entity))
                    {
                        requiresOriginal = true;
                        break;
                    }
                }
            }
            const auto current = Watch(m_StaticModels.at(id)->GetSourcePath(), previous.Convert);
            if (current.Source != previous.Source || current.Settings != previous.Settings ||
                current.Lod != previous.Lod || m_StaticModels.at(id)->SourceDependenciesChanged() ||
                (requiresOriginal && m_StaticModels.at(id)->UsesDerivedVirtualGeometry()))
            {
                reloads.push_back(id);
            }
        }
        for (const int id : reloads)
        {
            const auto path = m_StaticModels.at(id)->GetSourcePath();
            const bool convert = sourceWatches.at(id).Convert;
            const auto stamp = Watch(path, convert);
            if (stamp.Source == sourceWatches.at(id).Source && !m_StaticModels.at(id)->SourceDependenciesChanged() &&
                !m_StaticModels.at(id)->UsesDerivedVirtualGeometry())
            {
                m_StaticModels.at(id)->LoadLODMetadata(path.c_str());
                m_StaticModels.at(id)->RebuildVirtualGeometry(m_StaticModels.at(id)->GetVirtualGeometrySettings());
                sourceWatches[id] = stamp;
            }
            else
            {
                // 互換用のフォールバックでは、派生キャッシュによる高速インポートを迂回する。
                const auto previousMode = GetGeometryRenderingMode();

                struct RestoreMode
                {
                    GeometryRenderingMode Value;

                    ~RestoreMode()
                    {
                        GetGeometryRenderingMode() = Value;
                    }
                } restore{previousMode};

                GetGeometryRenderingMode() = GeometryRenderingMode::Legacy;
                const auto replacement = LoadStaticModelDetailed(path.c_str(), convert);
                GetGeometryRenderingMode() = previousMode;
                if (!replacement.Succeeded)
                {
                    continue; // 書き込み途中のソースで、最後の完全なメッシュを置き換えない。
                }
                retiredModels.push_back({GraphicsDevice::GetPendingSubmissionFence(), std::move(m_StaticModels[id])});
                m_StaticModels[id] = std::move(m_StaticModels.back());
                m_StaticModels.pop_back();
                sourceWatches.erase(replacement.ModelId);
                sourceWatches[id] = stamp;
            }
            for (auto entity : World::GetView<StaticModelComponent>())
            {
                if (ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity).ModelId == id)
                {
                    GpuSceneChanges::Notify(entity, GpuSceneDirtyFlags::All);
                }
            }
        }
    }
    std::unordered_set<int> changed;
    for (size_t index = 0; index < m_StaticModels.size(); ++index)
    {
        if (m_StaticModels[index]->PollVirtualGeometry())
        {
            changed.insert(static_cast<int>(index));
        }
        if (const auto watch = sourceWatches.find(static_cast<int>(index)); watch != sourceWatches.end())
        {
            m_StaticModels[index]->PublishDerivedVirtualGeometry(watch->second.Convert);
        }
    }
    for (auto entity : World::GetView<StaticModelComponent>())
    {
        const auto& component = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
        if (changed.contains(component.ModelId))
        {
            GpuSceneChanges::Notify(entity, GpuSceneDirtyFlags::Mesh);
        }
    }
    for (auto entity : World::GetView<MeshComponent>())
    {
        auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity).NaniteMesh;
        if (mesh && MeshletManager::PollImportedMesh(*mesh))
        {
            GpuSceneChanges::Notify(entity, GpuSceneDirtyFlags::Mesh);
        }
    }
}

StaticModelResource* ModelManager::FindLoadedStaticModel(const std::filesystem::path& path)
{
    for (const auto& model : m_StaticModels)
    {
        std::error_code ec;
        if (std::filesystem::equivalent(path, std::filesystem::u8path(model->GetSourcePath()), ec) && !ec)
        {
            return model.get();
        }
    }
    return nullptr;
}
