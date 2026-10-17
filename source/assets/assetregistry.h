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
    // ルート（asset フォルダ）の絶対パス。作業フォルダが変わったら取り直す。
    const std::filesystem::path& Root() const;
private:
    // グローバル変数として WinMain より前に構築されるため、構築時点の作業フォルダで
    // ルートを固定すると、main.cpp の SetCurrentDirectoryW 後に別フォルダを指してしまう。
    // 指定値だけ保持し、絶対パスは使用時に解決する。
    std::filesystem::path m_RootSpec;
    mutable std::filesystem::path m_Root;
    mutable std::filesystem::path m_RootBase;
    std::unordered_map<AssetHandle, Entry> m_Entries;
    std::unordered_set<AssetHandle> m_Collisions;
    bool Contains(const std::filesystem::path& path) const;
};


