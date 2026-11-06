#pragma once

#include <filesystem>
#include <cstdint>
#include <string>

namespace CachePaths
{
    inline std::filesystem::path Directory(const char* category = "")
    {
        return std::filesystem::path("Data/Cache") / category;
    }

    inline std::filesystem::path SourceEntry(const char* category, const std::filesystem::path& source,
                                             const char* extension)
    {
        const auto identity = std::filesystem::weakly_canonical(source).generic_u8string();
        uint64_t hash = 14695981039346656037ull;
        for (const auto byte : identity)
        {
            hash = (hash ^ static_cast<uint8_t>(byte)) * 1099511628211ull;
        }
        return Directory(category) / (std::to_string(hash) + extension);
    }

    // Keep existing entries in the destination. Failed moves can be retried next launch.
    inline void MigrateDirectory(const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        std::error_code error;
        if (!std::filesystem::is_directory(source, error))
        {
            return;
        }
        std::filesystem::create_directories(destination.parent_path(), error);
        std::filesystem::rename(source, destination, error);
        if (!error)
        {
            return;
        }
        error.clear();
        std::filesystem::create_directories(destination, error);
        std::filesystem::directory_iterator entries(source, error);
        const std::filesystem::directory_iterator end;
        while (!error && entries != end)
        {
            const auto entry = *entries;
            const auto target = destination / entry.path().filename();
            if (entry.is_directory(error))
            {
                MigrateDirectory(entry.path(), target);
            }
            else if (!error && !std::filesystem::exists(target, error))
            {
                std::filesystem::rename(entry.path(), target, error);
            }
            if (error)
            {
                return;
            }
            entries.increment(error);
        }
        if (!error)
        {
            std::filesystem::remove(source, error); // Only removes an empty directory.
        }
    }

    inline void Initialize()
    {
        MigrateDirectory("EditorCache", Directory());
        MigrateDirectory("bin/cache/pso", Directory("Pso"));
        MigrateDirectory("asset/virtualgeometry-cache", Directory("VirtualGeometry"));
        MigrateDirectory("cache", Directory("Diagnostics"));
    }
}
