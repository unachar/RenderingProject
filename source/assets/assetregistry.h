#pragma once
#include "materialasset.h"
#include <unordered_map>
#include <unordered_set>

class AssetRegistry
{
public:
    struct Entry
    {
        filesystem::path Path;
        AssetType Type = AssetType::Unknown;
    };
    explicit AssetRegistry(const filesystem::path& root = "asset");
    AssetHandle Register(const filesystem::path& path, AssetType type, string& error);
    filesystem::path Resolve(AssetHandle handle) const;
    AssetType TypeOf(AssetHandle handle) const;
    bool Refresh(string& error);
    bool Move(AssetHandle handle, const filesystem::path& destination, string& error);
    AssetHandle Duplicate(AssetHandle handle, const filesystem::path& destination, string& error);
    static AssetType Classify(const filesystem::path& path);
private:
    filesystem::path m_Root;
    unordered_map<AssetHandle, Entry> m_Entries;
    unordered_set<AssetHandle> m_Collisions;
    bool Contains(const filesystem::path& path) const;
};
