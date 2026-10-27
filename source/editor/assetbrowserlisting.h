#pragma once
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <future>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

struct AssetBrowserItem
{
    std::filesystem::path Path;
    std::string Name;
    std::string Key;
    bool IsDirectory = false;
    std::filesystem::file_time_type MetadataTime{};

    bool operator==(const AssetBrowserItem&) const = default;

    const std::filesystem::path& path() const
    {
        return Path;
    }
};

// Only the worker touches the filesystem. Published snapshots belong to the UI thread.
class AssetBrowserListing
{
  public:
    bool Update(const std::filesystem::path& directory, bool force, double now)
    {
        bool changed = false;
        if (directory != m_Directory)
        {
            m_Directory = directory;
            m_Items.clear();
            m_Error.clear();
            m_Dirty = true;
            changed = true;
        }
        m_Dirty = m_Dirty || force;
        if (m_Pending.valid() && m_Pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            auto result = m_Pending.get();
            if (result.Directory == m_Directory)
            {
                if (m_Items != result.Items || m_Error != result.Error)
                {
                    m_Items = std::move(result.Items);
                    changed = true;
                }
                m_Error = result.Error;
                m_NextScan = now + 0.5;
            }
        }
        if (!m_Pending.valid() && (m_Dirty || now >= m_NextScan))
        {
            m_Dirty = false;
            m_Pending = std::async(std::launch::async, [directory] {
                return Scan(directory);
            });
        }
        return changed;
    }

    const std::vector<AssetBrowserItem>& Items() const
    {
        return m_Items;
    }

    const std::error_code& Error() const
    {
        return m_Error;
    }

    bool Loading() const
    {
        return m_Dirty || m_Pending.valid();
    }

  private:
    struct Result
    {
        std::filesystem::path Directory;
        std::vector<AssetBrowserItem> Items;
        std::error_code Error;
    };

    static Result Scan(const std::filesystem::path& directory)
    {
        Result result;
        result.Directory = directory;
        try
        {
            std::unordered_map<std::string, std::filesystem::file_time_type> metadataTimes;
            std::filesystem::directory_iterator iterator(directory, result.Error);
            if (result.Error)
            {
                // Windows directory enumeration can report a different native code for a file.
                std::error_code statusError;
                const auto status = std::filesystem::status(directory, statusError);
                if (!statusError && std::filesystem::exists(status) && !std::filesystem::is_directory(status))
                {
                    result.Error = std::make_error_code(std::errc::not_a_directory);
                }
            }
            const std::filesystem::directory_iterator end;
            while (!result.Error && iterator != end)
            {
                const auto& entry = *iterator;
                std::error_code statusError;
                const bool isDirectory = entry.is_directory(statusError);
                const auto name = entry.path().filename().generic_string();
                auto extension = entry.path().extension().generic_string();
                std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char letter) {
                    return static_cast<char>(std::tolower(letter));
                });
                if (extension == ".meta")
                {
                    const auto source = entry.path().parent_path() / entry.path().stem();
                    std::error_code timeError;
                    metadataTimes[source.generic_string()] = entry.last_write_time(timeError);
                }
                bool hidden =
                    extension == ".meta" || extension == ".tmp" || extension == ".mtl" || extension == ".vgderived";
                if (isDirectory)
                {
                    for (const char* prefix : {"__material_validation_", "__temporal_lighting_", "__gi_validation_",
                                               "__thumbnail_validation_"})
                    {
                        hidden = hidden || name.starts_with(prefix);
                    }
                }
                if (!hidden && !statusError)
                {
                    result.Items.push_back({entry.path(), name, entry.path().generic_string(), isDirectory});
                }
                iterator.increment(result.Error);
            }
            for (auto& item : result.Items)
            {
                const auto metadata = metadataTimes.find(item.Key);
                if (metadata != metadataTimes.end())
                {
                    item.MetadataTime = metadata->second;
                }
            }
            std::sort(result.Items.begin(), result.Items.end(), [](const auto& first, const auto& second) {
                if (first.IsDirectory != second.IsDirectory)
                {
                    return first.IsDirectory > second.IsDirectory;
                }
                return first.Name < second.Name;
            });
        }
        catch (const std::filesystem::filesystem_error& error)
        {
            result.Error = error.code();
        }
        return result;
    }

    std::filesystem::path m_Directory;
    std::vector<AssetBrowserItem> m_Items;
    std::error_code m_Error;
    std::future<Result> m_Pending;
    double m_NextScan = 0.0;
    bool m_Dirty = false;
};
