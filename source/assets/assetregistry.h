#pragma once
#include "materialasset.h"
#include <unordered_map>
#include <unordered_set>

class AssetRegistry
{
public:
    struct Entry
    {
        std::filesystem::path Path;
        AssetType Type = AssetType::Unknown;
    };
    explicit AssetRegistry(const std::filesystem::path& root = "asset");
    AssetHandle Register(const std::filesystem::path& path, AssetType type, std::string& error);
    std::filesystem::path Resolve(AssetHandle handle) const;
    AssetType TypeOf(AssetHandle handle) const;
    bool Refresh(std::string& error);
    bool Move(AssetHandle handle, const std::filesystem::path& destination, std::string& error);
    AssetHandle Duplicate(AssetHandle handle, const std::filesystem::path& destination, std::string& error);
    static AssetType Classify(const std::filesystem::path& path);
private:
    std::filesystem::path m_Root;
    std::unordered_map<AssetHandle, Entry> m_Entries;
    std::unordered_set<AssetHandle> m_Collisions;
    bool Contains(const std::filesystem::path& path) const;
};

