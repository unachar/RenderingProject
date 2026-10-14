#pragma once
#include "assetregistry.h"
#include "componentmanager.h"
#include <functional>

struct MaterialRuntime
{
    AssetHandle Handle = 0;
    MaterialAssetData Saved;
    MaterialAssetData Editing;
    MaterialAssetData BoundData;
    array<filesystem::path, 5> TexturePaths;
    array<int, 5> TextureDescriptors{-1, -1, -1, -1, -1};
    MaterialComponent Binding;
    uint64_t Revision = 0;
    uint64_t PipelineRevision = 0;
    bool Dirty = false;
    bool Missing = false;
    string Error;
    // Kept alive until shutdown after the graphics fences have completed.
    ComPtr<ID3D12Resource> Constants;
    uint8_t* MappedConstants = nullptr;
    array<uint64_t, 3> UploadedRevision{};
};

class MaterialManager
{
public:
    static AssetRegistry& Registry();
    static void Init();
    static void Shutdown();
    static void Refresh();
    static MaterialRuntime& GetRuntime(AssetHandle handle);
    static const MaterialRuntime* FindRuntime(AssetHandle handle);
    static MaterialComponent BuildPreviewBinding(const MaterialAssetData& material, const array<int, 5>& descriptors);
    static const MaterialComponent& Resolve(const MaterialComponent& legacy, size_t slot = 0);
    static AssetHandle SlotHandle(const MaterialComponent& material, size_t slot);
    static bool SetEditing(AssetHandle handle, const MaterialAssetData& editing);
    static bool Save(AssetHandle handle);
    static bool Revert(AssetHandle handle);
    static void Discard(AssetHandle handle);
    static bool HasSurfaceForPass(EntityID entity, bool transparent);
    static size_t SlotCount(EntityID entity);
    static bool IsRenderable(EntityID entity);
    static bool Assign(EntityID entity, size_t slot, AssetHandle handle);
    static void NotifyAssignmentsChanged();
    static bool SaveSceneReferences(const filesystem::path& path, string& error);
    static bool LoadSceneReferences(const filesystem::path& path, string& error);
private:
    static void UpdateBinding(MaterialRuntime& runtime, const MaterialAssetData& material);
};
