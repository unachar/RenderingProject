#pragma once
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <istream>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

// Dependencies validate a mesh preview without importing the model again.
struct ThumbnailManifest
{
    uint64_t BaseKey = 0;
    uint64_t Key = 0;
    std::vector<std::pair<std::filesystem::path, uint64_t>> Dependencies;
    std::filesystem::path SourcePath;

    void Write(std::ostream& stream) const
    {
        const auto source = SourcePath.generic_u8string();
        stream << "thumbnail-v3 " << BaseKey << ' ' << Key << ' ' << Dependencies.size() << ' '
               << std::quoted(std::string(source.begin(), source.end())) << '\n';
        for (const auto& [path, revision] : Dependencies)
        {
            const auto utf8 = path.generic_u8string();
            stream << std::quoted(std::string(utf8.begin(), utf8.end())) << ' ' << revision << '\n';
        }
    }

    bool Read(std::istream& stream)
    {
        Dependencies.clear();
        std::string version;
        size_t count = 0;
        std::string source;
        if (!(stream >> version >> BaseKey >> Key >> count >> std::quoted(source)) || version != "thumbnail-v3" ||
            count > 65536)
        {
            return false;
        }
        SourcePath = std::filesystem::path(std::u8string(source.begin(), source.end()));
        for (size_t index = 0; index < count; ++index)
        {
            std::string path;
            uint64_t revision = 0;
            if (!(stream >> std::quoted(path) >> revision))
            {
                Dependencies.clear();
                return false;
            }
            Dependencies.emplace_back(std::filesystem::path(std::u8string(path.begin(), path.end())), revision);
        }
        return true;
    }

    template <typename RevisionReader>
    bool Matches(uint64_t baseKey, const std::filesystem::path& source, RevisionReader revisionOf) const
    {
        if (BaseKey != baseKey || SourcePath != source)
        {
            return false;
        }
        for (const auto& [path, revision] : Dependencies)
        {
            if (revisionOf(path) != revision)
            {
                return false;
            }
        }
        return true;
    }
};
